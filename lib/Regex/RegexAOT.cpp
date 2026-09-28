/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/Regex/RegexAOT.h"
#include "hermes/Platform/Unicode/CodePointSet.h"
#include "hermes/Regex/RegexSerialization.h"
#include "hermes/Regex/RegexTraits.h"
#include "llvh/ADT/Twine.h"
#include "llvh/Support/raw_ostream.h"

#include <algorithm>
#include <array>

using namespace hermes;
using namespace hermes::regex;

namespace {
// These bounds cover analysis, emitted control flow and fixed stack scratch.
// No repetition is unrolled and no subject-dependent state is retained.
constexpr size_t kMaxBytecode = 2048;
constexpr size_t kMaxOperations = 64;
constexpr size_t kMaxRanges = 64;
constexpr size_t kMaxLoops = 8;
constexpr size_t kMaxAlternations = 8;
constexpr size_t kMaxSource = 24 * 1024;

struct AOTOperation {
  Opcode opcode;
  std::string predicate;
  uint32_t min = 1;
  uint32_t max = 1;
  uint32_t capture = 0;
  // Bytecode offset during decoding, operation index after target validation.
  uint32_t target = 0;
  bool greedy = true;
  bool invert = false;
};

std::string rangePredicate(uint32_t first, uint32_t last) {
  if (first == last)
    return "c == " + std::to_string(first) + "u";
  return "(c >= " + std::to_string(first) + "u && c <= " +
      std::to_string(last) + "u)";
}

const std::string &classPredicate(CharacterClass::Type type) {
  // Derive these once from the same tables as the interpreter. In particular,
  // JS whitespace must not drift when the platform Unicode support changes.
  static const auto predicates = [] {
    std::array<std::string, 3> result;
    const CharacterClass::Type types[] = {
        CharacterClass::Digits, CharacterClass::Spaces, CharacterClass::Words};
    for (size_t i = 0; i < result.size(); ++i) {
      result[i] = "(0";
      for (uint32_t c = 0; c < 65536;) {
        if (!UTF16RegexTraits::characterHasType(c, types[i])) {
          ++c;
          continue;
        }
        const uint32_t first = c++;
        while (c < 65536 && UTF16RegexTraits::characterHasType(c, types[i]))
          ++c;
        result[i] += " || " + rangePredicate(first, c - 1);
      }
      result[i] += ')';
    }
    return result;
  }();
  switch (type) {
    case CharacterClass::Digits:
      return predicates[0];
    case CharacterClass::Spaces:
      return predicates[1];
    case CharacterClass::Words:
      return predicates[2];
  }
  llvm_unreachable("Unknown character class");
}

// Substitute an expression for the predicate's code-unit variable. Generated
// expressions are numeric only; source pattern text never becomes C source.
std::string at(std::string predicate, llvh::StringRef value) {
  for (size_t i = 0; i < predicate.size(); ++i) {
    if (predicate[i] == 'c') {
      predicate.replace(i, 1, value.data(), value.size());
      i += value.size() - 1;
    }
  }
  return predicate;
}

// Decode precisely one width-one instruction. Case-insensitive literals use
// the same compile-time case-equivalence expansion as character classes.
// BracketICase ranges have already been expanded by the regexp compiler.
bool readTerm(
    const Insn *base,
    std::string &predicate,
    size_t &width,
    size_t &rangeCount) {
  uint32_t literal = 0;
  bool icase = false;
  switch (base->opcode) {
    case Opcode::MatchChar8:
      literal = static_cast<uint8_t>(llvh::cast<MatchChar8Insn>(base)->c);
      width = sizeof(MatchChar8Insn);
      break;
    case Opcode::MatchChar16:
      literal = llvh::cast<MatchChar16Insn>(base)->c;
      width = sizeof(MatchChar16Insn);
      break;
    case Opcode::MatchCharICase8:
      literal = static_cast<uint8_t>(llvh::cast<MatchCharICase8Insn>(base)->c);
      width = sizeof(MatchCharICase8Insn);
      icase = true;
      break;
    case Opcode::MatchCharICase16:
      literal = llvh::cast<MatchCharICase16Insn>(base)->c;
      width = sizeof(MatchCharICase16Insn);
      icase = true;
      break;
    case Opcode::MatchAny:
      predicate = "1";
      width = sizeof(MatchAnyInsn);
      return true;
    case Opcode::MatchAnyButNewline:
      predicate = "(c != 10u && c != 13u && c != 8232u && c != 8233u)";
      width = sizeof(MatchAnyButNewlineInsn);
      return true;
    case Opcode::Bracket:
    case Opcode::BracketICase: {
      const BracketInsn *term = base->opcode == Opcode::Bracket
          ? llvh::cast<BracketInsn>(base)
          : llvh::cast<BracketICaseInsn>(base);
      rangeCount += term->rangeCount;
      if (rangeCount > kMaxRanges)
        return false;
      // Non-Unicode bracket ranges already contain their case equivalents.
      // They need no runtime canonicalization, even for UTF-16 subjects.
      // Unicode-mode classes are rejected by the analyzer.
      width = term->totalWidth();
      predicate = "(0";
      for (auto type : {CharacterClass::Digits,
                        CharacterClass::Spaces,
                        CharacterClass::Words}) {
        if (term->positiveCharClasses & type)
          predicate += " || " + classPredicate(type);
        if (term->negativeCharClasses & type)
          predicate += " || !" + classPredicate(type);
      }
      const auto *ranges = reinterpret_cast<const BracketRange32 *>(term + 1);
      for (uint32_t i = 0; i < term->rangeCount; ++i)
        predicate += " || " + rangePredicate(ranges[i].start, ranges[i].end);
      predicate += ')';
      if (term->negate)
        predicate = "!" + predicate;
      return true;
    }
    default:
      return false;
  }
  predicate = rangePredicate(literal, literal);
  if (icase) {
    CodePointSet singleton;
    singleton.add(literal);
    const auto equivalents = makeCanonicallyEquivalent(singleton, false);
    predicate = "(0";
    for (const auto &range : equivalents.ranges())
      predicate += " || " + rangePredicate(range.first, range.end() - 1);
    predicate += ')';
  }
  return true;
}

void emitBody(
    llvh::raw_ostream &os,
    llvh::ArrayRef<AOTOperation> ops,
    bool anchored,
    llvh::StringRef suffix) {
  const std::string word = classPredicate(CharacterClass::Words);
  const std::string newline = "(c == 10u || c == 13u || c == 8232u || c == 8233u)";
  // A single character predicate is a linear scan with no backtracking. Avoid
  // checking it twice per candidate or restarting a long scan at the work cap.
  // Widen loads before predicates so 8-bit input can use the same class ranges.
  if (ops.size() == 1 && !ops.front().predicate.empty() &&
      ops.front().opcode != Opcode::Width1Loop) {
    os << "    for (uint32_t p = start; p < length; ++p) {\n"
          "      uint32_t c = s[p];\n"
          "      if (" << ops.front().predicate << ") {\n"
          "        ranges[0] = p; ranges[1] = p + 1;\n"
          "        return HermesRegexAOTMatch;\n"
          "      }\n"
          "      if (flags & " << constants::matchOnlyAtStart << "u) break;\n"
          "    }\n"
          "    return HermesRegexAOTNoMatch;\n";
    return;
  }
  auto label = [&](llvh::StringRef stem, size_t i) {
    return (stem + suffix + "_" + std::to_string(i)).str();
  };
  const std::string next = ("next" + suffix).str();
  const std::string backtrack = ("backtrack" + suffix).str();
  std::string firstSet = "(0";
  std::array<bool, kMaxOperations + 1> firstReachable{};
  firstReachable[0] = true;
  std::array<uint64_t, kMaxOperations + 1> minWidths{};
  // Forward targets allow bounded passes over the graph. Candidate pruning
  // must include every branch, including empty paths to a shared suffix.
  for (size_t i = ops.size(); i-- > 0;) {
    const auto &op = ops[i];
    if (op.opcode == Opcode::Jump32)
      minWidths[i] = minWidths[op.target];
    else if (op.opcode == Opcode::Alternation)
      minWidths[i] = std::min(minWidths[i + 1], minWidths[op.target]);
    else
      minWidths[i] = minWidths[i + 1] + (op.predicate.empty() ? 0 : op.min);
  }
  const AOTOperation *leadingBoundary = nullptr;
  bool sawTerm = false;
  bool fixedWidth = true;
  bool endAnchored = false;
  size_t loops = 0, alternatives = 0;
  const uint64_t minWidth = minWidths[0];
  for (size_t i = 0; i < ops.size(); ++i) {
    const auto &op = ops[i];
    if (!op.predicate.empty()) {
      if (firstReachable[i])
        firstSet += " || (" + op.predicate + ')';
      sawTerm = true;
      fixedWidth &= op.min == op.max;
    } else if (!sawTerm && op.opcode == Opcode::WordBoundary) {
      leadingBoundary = &op;
    }
    if (op.opcode == Opcode::Alternation || op.opcode == Opcode::Jump32) {
      firstReachable[op.target] |= firstReachable[i];
      // No assertion after a split is necessarily common to every path.
      sawTerm = true;
      fixedWidth = false;
      alternatives += op.opcode == Opcode::Alternation;
    }
    if (op.opcode != Opcode::Jump32 &&
        (op.predicate.empty() || op.min == 0))
      firstReachable[i + 1] |= firstReachable[i];
    if (op.opcode == Opcode::Width1Loop)
      ++loops;
    if (op.opcode == Opcode::RightAnchor)
      endAnchored = true;
  }
  firstSet += ')';
  os << "    uint32_t p, candidate = start, c;\n";
  for (size_t i = 0; i < loops; ++i)
    os << "    uint32_t lo" << i << ", hi" << i << ", pos" << i << ";\n";
  if (alternatives) {
    // Forward-only branches visit each site at most once on any active path.
    // A loop has one resumable choice, regardless of its repetition count.
    os << "    uint32_t choices[" << loops + alternatives << "], depth;\n";
    for (size_t i = 0; i < alternatives; ++i)
      os << "    uint32_t alternative" << i << ";\n";
  }
  if (minWidth > UINT32_MAX) {
    os << "    return HermesRegexAOTNoMatch;\n";
    return;
  }
  os << "    if (" << minWidth
     << "u > length - start) return HermesRegexAOTNoMatch;\n";
  // A fixed-width match with an ordinary end anchor has only one possible
  // start. Preserve the shared fixed-width matcher's shortcut; sticky and
  // start-anchored searches must still use their original start.
  if (fixedWidth && endAnchored && !anchored) {
    os << "    if ((flags & " << constants::matchOnlyAtStart
       << "u) && start != length - " << minWidth
       << "u) return HermesRegexAOTNoMatch;\n"
       << "    candidate = length - " << minWidth << "u;\n";
  }
  os << "    for (;;) {\n"
     << "      if (candidate > length - " << minWidth
     << "u) return HermesRegexAOTNoMatch;\n";
  // Reject impossible starts before initializing captures or repetition state.
  // Leading word-boundary checks also skip the interiors of word runs.
  if (!firstReachable[ops.size()]) {
    os << "      c = s[candidate];\n"
       << "      if (!(" << firstSet << ")) goto " << next << ";\n";
  }
  if (leadingBoundary) {
    os << "      if (((candidate != 0 && " << at(word, "s[candidate - 1]")
       << ") != (candidate != length && " << at(word, "s[candidate]")
       << ")) == " << (leadingBoundary->invert ? 1 : 0) << ") goto " << next
       << ";\n";
  }
  // First-character/boundary rejection is a single forward scan. Charge the
  // budget only when entering the matcher, where repeated work can backtrack;
  // otherwise a long irrelevant prefix makes the interpreter scan it again.
  os << "      if (!work--) return HermesRegexAOTFallback;\n"
        "      p = candidate;\n";
  if (alternatives)
    os << "      depth = 0;\n";
  std::string fail = alternatives ? backtrack : next;
  std::vector<std::string> choices;
  loops = 0;
  size_t alternative = 0;
  for (size_t index = 0; index < ops.size(); ++index) {
    const auto &op = ops[index];
    if (alternatives)
      os << "    " << label("op", index) << ":\n";
    if (op.opcode == Opcode::Width1Loop) {
      const auto i = loops++;
      os << "      if (" << op.min << "u > length - p) goto " << fail << ";\n"
         << "      lo" << i << " = p + " << op.min << "u;\n"
         << "      hi" << i << " = p;\n"
         << "      c = length - p < " << op.max << "u ? length : p + "
         << op.max << "u;\n"
         << "      while (hi" << i << " < c && ("
         << at(op.predicate, "s[hi" + std::to_string(i) + "]")
         << ")) {\n"
         << "        if (!work--) return HermesRegexAOTFallback;\n"
         << "        ++hi" << i << ";\n      }\n"
         << "      if (hi" << i << " < lo" << i << ") goto " << fail << ";\n"
         << "      pos" << i << " = " << (op.greedy ? "hi" : "lo") << i
         << ";\n"
         << "      goto " << label("resume", i) << ";\n"
         << "    " << label("backtrack", i) << ":\n"
         << "      if (!work--) return HermesRegexAOTFallback;\n"
         << "      if (pos" << i << " == " << (op.greedy ? "lo" : "hi")
         << i << ") goto " << fail << ";\n"
         << "      " << (op.greedy ? "--pos" : "++pos") << i << ";\n"
         << "    " << label("resume", i) << ":\n";
      if (alternatives) {
        os << "      choices[depth++] = " << choices.size() << "u;\n";
        choices.push_back(label("backtrack", i));
      } else {
        fail = label("backtrack", i);
      }
      os << "      p = pos" << i << ";\n";
      continue;
    }
    if (!op.predicate.empty()) {
      os << "      if (p == length || !(" << at(op.predicate, "s[p]")
         << ")) goto " << fail << ";\n      ++p;\n";
      continue;
    }
    switch (op.opcode) {
      case Opcode::Alternation: {
        const auto i = alternative++;
        os << "      alternative" << i << " = p;\n"
           << "      choices[depth++] = " << choices.size() << "u;\n"
           << "      goto " << label("op", index + 1) << ";\n"
           << "    " << label("alternative", i) << ":\n"
           << "      p = alternative" << i << ";\n"
           << "      goto " << label("op", op.target) << ";\n";
        choices.push_back(label("alternative", i));
        break;
      }
      case Opcode::Jump32:
        os << "      goto " << label("op", op.target) << ";\n";
        break;
      case Opcode::BeginMarkedSubexpression:
      case Opcode::EndMarkedSubexpression:
        os << "      ranges[" << (op.capture + 1) * 2 +
                (op.opcode == Opcode::EndMarkedSubexpression)
           << "] = p;\n";
        break;
      case Opcode::LeftAnchor:
        os << "      if (p != 0) goto " << fail << ";\n";
        break;
      case Opcode::LeftAnchorMultiline:
        os << "      if (p != 0 && !(" << at(newline, "s[p - 1]")
           << ")) goto " << fail << ";\n";
        break;
      case Opcode::RightAnchor:
      case Opcode::RightAnchorMultiline:
        os << "      if (!((p == length && !(flags & "
           << constants::matchNotEndOfLine << "u))";
        if (op.opcode == Opcode::RightAnchorMultiline)
          os << " || (p != length && " << at(newline, "s[p]") << ')';
        os << ")) goto " << fail << ";\n";
        break;
      case Opcode::WordBoundary:
        os << "      if (((p != 0 && " << at(word, "s[p - 1]")
           << ") != (p != length && " << at(word, "s[p]") << ")) == "
           << (op.invert ? 1 : 0) << ") goto " << fail << ";\n";
        break;
      default:
        llvm_unreachable("Unsupported operation in compiled matcher");
    }
  }
  // Every capture in a straight-line program is visited on a successful
  // path. Resuming a loop rewrites all subsequent captures; no capture can be
  // inside a loop, alternative or assertion requiring restoration.
  if (alternatives)
    os << "    " << label("op", ops.size()) << ":\n";
  os << "      ranges[0] = candidate; ranges[1] = p;\n"
     << "      return HermesRegexAOTMatch;\n";
  if (alternatives) {
    os << "    " << backtrack << ":\n"
       << "      if (!work--) return HermesRegexAOTFallback;\n"
       << "      if (depth) switch (choices[--depth]) {\n";
    for (size_t i = 0; i < choices.size(); ++i)
      os << "        case " << i << ": goto " << choices[i] << ";\n";
    os << "      }\n";
  }
  os << "    " << next << ":\n"
     << "      if (candidate == length";
  if (anchored)
    os << " || 1";
  else
    os << " || (flags & " << constants::matchOnlyAtStart << "u)";
  os << ") return HermesRegexAOTNoMatch;\n"
     << "      ++candidate;\n    }\n";
}
} // namespace

llvh::Optional<std::string> CompiledRegExp::compileAOTRegex(
    llvh::StringRef name) const {
  auto bytes = getBytecode();
  const auto *header =
      reinterpret_cast<const RegexBytecodeHeader *>(bytes.data());
  const auto flags = SyntaxFlags::fromByte(header->syntaxFlags);
  if (flags.unicode || bytes.size() > kMaxBytecode ||
      header->markedCount > HERMES_REGEX_AOT_MAX_CAPTURES ||
      header->loopCount > kMaxLoops)
    return llvh::None;

  size_t ranges = 0, loops = 0, alternatives = 0, offset = sizeof(*header);
  // Only decoded operation boundaries may be branch destinations. In
  // particular, a run's payload and a width-one loop's body are not entries.
  std::array<size_t, kMaxBytecode> operationAt;
  operationAt.fill(kMaxOperations + 1);
  std::vector<AOTOperation> ops;
  uint32_t begun = 0, ended = 0;
  while (offset < bytes.size()) {
    const auto *base = reinterpret_cast<const Insn *>(bytes.data() + offset);
    // Goal validates completion but does not add an operation. A program that
    // exactly fills the operation budget must still be allowed to terminate.
    if (ops.size() == kMaxOperations && base->opcode != Opcode::Goal)
      return llvh::None;
    operationAt[offset - sizeof(*header)] = ops.size();
    AOTOperation op{base->opcode, {}};
    size_t width = 0;
    if (readTerm(base, op.predicate, width, ranges)) {
      offset += width;
      ops.push_back(std::move(op));
      continue;
    }
    switch (base->opcode) {
      case Opcode::Alternation:
      case Opcode::Jump32:
        // Captures require restoration across alternatives; leave those
        // programs on the interpreter. Only forward control flow is emitted.
        if (header->markedCount)
          return llvh::None;
        if (base->opcode == Opcode::Alternation) {
          if (++alternatives > kMaxAlternations)
            return llvh::None;
          op.target = llvh::cast<AlternationInsn>(base)->secondaryBranch;
          width = sizeof(AlternationInsn);
        } else {
          op.target = llvh::cast<Jump32Insn>(base)->target;
          width = sizeof(Jump32Insn);
        }
        if (op.target <= offset - sizeof(*header) ||
            op.target >= bytes.size() - sizeof(*header))
          return llvh::None;
        offset += width;
        ops.push_back(std::move(op));
        break;
      case Opcode::Goal:
        if (offset + sizeof(GoalInsn) != bytes.size() ||
            begun != (1u << header->markedCount) - 1 || ended != begun)
          return llvh::None;
        offset = bytes.size();
        break;
      case Opcode::MatchNChar8:
      case Opcode::MatchNCharICase8: {
        // The two instruction layouts have identical count and payload fields.
        const bool icase = base->opcode == Opcode::MatchNCharICase8;
        const uint32_t count = icase
            ? llvh::cast<MatchNCharICase8Insn>(base)->charCount
            : llvh::cast<MatchNChar8Insn>(base)->charCount;
        const size_t prefix =
            icase ? sizeof(MatchNCharICase8Insn) : sizeof(MatchNChar8Insn);
        const auto *chars = reinterpret_cast<const uint8_t *>(base) + prefix;
        for (uint32_t i = 0; i < count; ++i) {
          if (ops.size() == kMaxOperations)
            return llvh::None;
          MatchCharICase8Insn folded{};
          folded.opcode = Opcode::MatchCharICase8;
          folded.c = chars[i];
          AOTOperation term{
              Opcode::MatchChar8, rangePredicate(chars[i], chars[i])};
          if (icase && !readTerm(&folded, term.predicate, width, ranges))
            return llvh::None;
          ops.push_back(std::move(term));
        }
        offset += prefix + count;
        break;
      }
      case Opcode::Width1Loop: {
        if (++loops > kMaxLoops)
          return llvh::None;
        const auto *loop = llvh::cast<Width1LoopInsn>(base);
        if (!readTerm(
                reinterpret_cast<const Insn *>(loop + 1),
                op.predicate,
                width,
                ranges) ||
            offset + sizeof(*loop) + width !=
                sizeof(*header) + loop->notTakenTarget)
          return llvh::None;
        op.min = loop->min;
        op.max = loop->max;
        op.greedy = loop->greedy;
        offset += sizeof(*loop) + width;
        ops.push_back(std::move(op));
        break;
      }
      case Opcode::BeginMarkedSubexpression:
      case Opcode::EndMarkedSubexpression: {
        const bool begin = base->opcode == Opcode::BeginMarkedSubexpression;
        op.capture = begin
            ? llvh::cast<BeginMarkedSubexpressionInsn>(base)->mexp
            : llvh::cast<EndMarkedSubexpressionInsn>(base)->mexp;
        if (op.capture >= header->markedCount)
          return llvh::None;
        const uint32_t bit = 1u << op.capture;
        if (begin ? (begun & bit) : (!(begun & bit) || (ended & bit)))
          return llvh::None;
        (begin ? begun : ended) |= bit;
        offset += begin ? sizeof(BeginMarkedSubexpressionInsn)
                        : sizeof(EndMarkedSubexpressionInsn);
        ops.push_back(std::move(op));
        break;
      }
      case Opcode::WordBoundary:
      case Opcode::WordBoundaryICase:
        // Non-Unicode folding cannot turn a non-ASCII code unit into ASCII.
        op.invert = base->opcode == Opcode::WordBoundary
            ? llvh::cast<WordBoundaryInsn>(base)->invert
            : llvh::cast<WordBoundaryICaseInsn>(base)->invert;
        op.opcode = Opcode::WordBoundary;
        offset += base->opcode == Opcode::WordBoundary
            ? sizeof(WordBoundaryInsn)
            : sizeof(WordBoundaryICaseInsn);
        ops.push_back(std::move(op));
        break;
      case Opcode::LeftAnchor:
      case Opcode::LeftAnchorMultiline:
      case Opcode::RightAnchor:
      case Opcode::RightAnchorMultiline:
        offset += sizeof(Insn);
        ops.push_back(std::move(op));
        break;
      default:
        return llvh::None;
    }
  }
  for (size_t i = 0; i < ops.size(); ++i) {
    auto &op = ops[i];
    if (op.opcode != Opcode::Alternation && op.opcode != Opcode::Jump32)
      continue;
    const auto target = operationAt[op.target];
    if (!alternatives || target > ops.size() || target <= i)
      return llvh::None;
    op.target = target;
  }
  std::string source;
  llvh::raw_string_ostream os(source);
  os << "HERMES_REGEX_AOT_HIDDEN int32_t " << name
     << "(const void *input, uint32_t length, uint32_t start, "
        "uint32_t flags, uint32_t *ranges) {\n"
        "  uint32_t work = 262144u;\n";
  const bool anchored = header->constraints & MatchConstraintAnchoredAtStart;
  if (anchored)
    os << "  if (start != 0) return HermesRegexAOTNoMatch;\n";
  os << "  if (flags & " << constants::matchInputAllAscii << "u) {\n"
        "    const uint8_t *s = (const uint8_t *)input;\n";
  emitBody(os, ops, anchored, "8");
  os << "  } else {\n    const uint16_t *s = (const uint16_t *)input;\n";
  emitBody(os, ops, anchored, "16");
  os << "  }\n}\n";
  os.flush();
  if (source.size() > kMaxSource)
    return llvh::None;
  return source;
}
