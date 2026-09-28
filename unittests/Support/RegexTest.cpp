/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/Regex/Regex.h"
#include "hermes/Regex/Executor.h"
#include "hermes/Regex/RegexSerialization.h"
#include "hermes/Regex/RegexTraits.h"

#include <string>
#include <utility>
#include "gtest/gtest.h"

using namespace hermes::regex;

namespace {

using cregex = Regex<UTF16RegexTraits>;
using cmatch = std::vector<CapturedRange>;

bool search(
    const char16_t *start,
    const char16_t *end,
    cmatch &m,
    const cregex &cr,
    constants::MatchFlagType flags = constants::matchDefault) {
  return searchWithBytecode(cr.compile(), start, 0, end - start, &m, flags) ==
      MatchRuntimeResult::Match;
}

bool search(
    const char16_t *str,
    cmatch &m,
    const cregex &cr,
    constants::MatchFlagType flags = constants::matchDefault) {
  return search(
      str, str + std::char_traits<char16_t>::length(str), m, cr, flags);
}

bool search(
    const std::u16string &text,
    cmatch &m,
    const cregex &cr,
    constants::MatchFlagType flags = constants::matchDefault) {
  return search(text.c_str(), m, cr, flags);
}

// Flatten a list of captured ranges into a string representation like:
// "(0-5) (1-3)"
std::string flatten(const std::vector<CapturedRange> &ranges) {
  std::string result;
  for (const auto &range : ranges) {
    if (!result.empty())
      result += " ";
    if (!range.matched()) {
      result += "nm";
    } else {
      result += '(';
      result += std::to_string(range.start);
      result += '-';
      result += std::to_string(range.end);
      result += ')';
    }
  }
  return result;
}

TEST(Regex, Short) {
  std::u16string pattern = u"a([0-9]+)b";
  cregex reg(pattern.c_str());
  std::vector<CapturedRange> matchRanges;

  const std::u16string text1 = u"a0b";
  EXPECT_TRUE(search(text1, matchRanges, reg, constants::matchDefault));
  EXPECT_EQ("(0-3) (1-2)", flatten(matchRanges));

  const std::u16string text2 = u"a1234b";
  EXPECT_TRUE(search(text2, matchRanges, reg, constants::matchDefault));
  EXPECT_EQ("(0-6) (1-5)", flatten(matchRanges));
}

TEST(Regex, AOTEligibility) {
  for (const auto &pattern : {
           "", "(a*)", "(a*?)(a{2,4})", "^([a-z]+)$", "(?m:^a+$)",
           "\\b([a-z]+)\\s*[:=]\\s*(\\d+)\\b", "(?i:a+)(?-i:B)",
           "(?<name>[^,]+),", "[\\d\\S]+", "\\u0100+(\\ud800*)",
           "(?:a|ab)c", "(?:ab|a)b", "(?:|ab)c", "(?:ab|)c",
           "(?:a*|ab)c", "(?:a*?|ab)c",
           "(?i:\\u00e9(?:cho|clair)\\s*.*?END)"}) {
    auto compiled = hermes::CompiledRegExp::tryCompile(pattern, "");
    ASSERT_TRUE(compiled);
    EXPECT_TRUE(compiled->compileAOTRegex("matcher")) << pattern;
  }
  for (const auto &pattern : {
           "(ab|cd)", "(?=a)a", "(?<=a)b", "(a)\\1", "(a)+",
           "(a)?", "(a)(?:b|c)", "(?:a|b)+", "(?:a|ab)*?c", "(?:ab|cd)+",
           "(a)(b)(c)(d)(e)(f)(g)(h)(i)"}) {
    auto compiled = hermes::CompiledRegExp::tryCompile(pattern, "");
    ASSERT_TRUE(compiled);
    EXPECT_FALSE(compiled->compileAOTRegex("matcher")) << pattern;
  }
  auto unicode = hermes::CompiledRegExp::tryCompile("a+", "u");
  ASSERT_TRUE(unicode);
  EXPECT_FALSE(unicode->compileAOTRegex("matcher"));
  auto large = hermes::CompiledRegExp::tryCompile(std::string(2048, 'a'), "");
  ASSERT_TRUE(large);
  EXPECT_FALSE(large->compileAOTRegex("matcher"));
  for (size_t count : {63u, 64u, 65u}) {
    auto compiled =
        hermes::CompiledRegExp::tryCompile(std::string(count, 'a'), "");
    ASSERT_TRUE(compiled);
    EXPECT_EQ(count <= 64, compiled->compileAOTRegex("matcher").hasValue())
        << count;
  }
  for (size_t count : {8u, 9u}) {
    std::string pattern;
    for (size_t i = 0; i < count; ++i)
      pattern += "(?:ab|a)";
    auto compiled = hermes::CompiledRegExp::tryCompile(pattern, "");
    ASSERT_TRUE(compiled);
    EXPECT_EQ(count == 8, compiled->compileAOTRegex("matcher").hasValue());
  }
}

TEST(Regex, AOTBranchTargets) {
  // Literal runs require at least three characters; a two-character branch
  // emits separate instructions, so its second character is a legal target.
  for (const auto &pattern : {"abc|def", "a*|b*"}) {
    auto compiled = hermes::CompiledRegExp::tryCompile(pattern, "");
    ASSERT_TRUE(compiled);
    ASSERT_TRUE(compiled->compileAOTRegex("matcher"));
    // Corrupt only targets in an otherwise producer-validated program. AOT
    // must not turn instruction payloads or loop bodies into entry points.
    auto bytes = compiled->getBytecode();
    auto *code = const_cast<uint8_t *>(bytes.data()) +
        sizeof(RegexBytecodeHeader);
    auto *alt = reinterpret_cast<AlternationInsn *>(code);
    ASSERT_EQ(Opcode::Alternation, alt->opcode);
    const uint32_t secondary = alt->secondaryBranch;
    auto *jump = reinterpret_cast<Jump32Insn *>(
        code + secondary - sizeof(Jump32Insn));
    ASSERT_EQ(Opcode::Jump32, jump->opcode);
    const uint32_t continuation = jump->target;
    const uint32_t prefix = pattern[1] == '*' ? sizeof(Width1LoopInsn)
                                            : sizeof(MatchNChar8Insn);
    const uint32_t primaryPayload = sizeof(AlternationInsn) + prefix;
    const uint32_t secondaryPayload = secondary + prefix;
    const uint32_t pastGoal = bytes.size() - sizeof(RegexBytecodeHeader);
    // The secondary payload is ahead of Jump32, so its rejection also checks
    // operation boundaries rather than just the backward-branch guard.
    for (uint32_t target :
         {0u, 1u, primaryPayload, secondaryPayload, pastGoal, UINT32_MAX}) {
      alt->secondaryBranch = target;
      EXPECT_FALSE(compiled->compileAOTRegex("matcher")) << target;
      alt->secondaryBranch = secondary;
      jump->target = target;
      EXPECT_FALSE(compiled->compileAOTRegex("matcher")) << target;
      jump->target = continuation;
    }
    EXPECT_TRUE(compiled->compileAOTRegex("matcher"));
  }
}

TEST(Regex, AOTFallbackRetainsCaptures) {
  auto compiled = hermes::CompiledRegExp::tryCompile("(a*)(a+)b", "");
  ASSERT_TRUE(compiled);
  auto decline = +[](const void *, uint32_t, uint32_t, uint32_t, uint32_t *ranges)
      -> int32_t {
    // A budget decline can follow partial capture writes. None may escape.
    ranges[0] = ranges[1] = ranges[2] = ranges[3] = kNotMatched;
    return HermesRegexAOTFallback;
  };
  const std::string text = "xaaab";
  cmatch actual;
  auto sink = [&](CapturedRange full, llvh::ArrayRef<CapturedRange> captures) {
    actual.push_back(full);
    actual.insert(actual.end(), captures.begin(), captures.end());
  };
  auto guard =
#ifdef HERMES_CHECK_NATIVE_STACK
      hermes::StackOverflowGuard::nativeStackGuard(512 * 1024);
#else
      hermes::StackOverflowGuard::depthCounterGuard(1);
#endif
  EXPECT_EQ(
      MatchRuntimeResult::Match,
      searchWithAOTRegex(
          decline,
          compiled->getBytecode(),
          text.data(),
          2,
          text.size(),
          sink,
          constants::matchInputAllAscii | constants::matchOnlyAtStart,
          guard));
  EXPECT_EQ("(2-5) (2-3) (3-4)", flatten(actual));
}

TEST(Regex, AOTInputRepresentation) {
  auto compiled = hermes::CompiledRegExp::tryCompile("ab", "");
  ASSERT_TRUE(compiled);
  auto asciiMatcher = +[](const void *, uint32_t, uint32_t, uint32_t flags,
                          uint32_t *) -> int32_t {
    EXPECT_NE(0u, flags & constants::matchInputAllAscii);
    EXPECT_NE(0u, flags & constants::matchOnlyAtStart);
    return HermesRegexAOTNoMatch;
  };
  auto utf16Matcher = +[](const void *, uint32_t, uint32_t, uint32_t flags,
                          uint32_t *) -> int32_t {
    EXPECT_EQ(0u, flags & constants::matchInputAllAscii);
    EXPECT_NE(0u, flags & constants::matchOnlyAtStart);
    return HermesRegexAOTNoMatch;
  };
  auto guard =
#ifdef HERMES_CHECK_NATIVE_STACK
      hermes::StackOverflowGuard::nativeStackGuard(512 * 1024);
#else
      hermes::StackOverflowGuard::depthCounterGuard(1);
#endif
  // The hint is optional for ASCII storage and valid for ASCII text stored
  // as UTF-16. Neither form may change the generated matcher's pointer width.
  EXPECT_EQ(
      MatchRuntimeResult::NoMatch,
      searchWithAOTRegex(
          asciiMatcher,
          compiled->getBytecode(),
          "ab",
          0,
          2,
          {},
          constants::matchOnlyAtStart,
          guard));
  EXPECT_EQ(
      MatchRuntimeResult::NoMatch,
      searchWithAOTRegex(
          utf16Matcher,
          compiled->getBytecode(),
          u"ab",
          0,
          2,
          {},
          constants::matchInputAllAscii | constants::matchOnlyAtStart,
          guard));
}

TEST(Regex, SimpleDescriptorMatchesInterpreter) {
  const std::pair<const char *, const char *> patterns[] = {
      {"abc", ""},
      {"^a[0-9]", "gdy"},
      {"[a-z]$", ""},
      {"^a.$", ""},
      {"[\\d\\S][^\\w]", ""},
      {"[\\D\\s]", ""},
      {"[^]", ""},
      {"[]", ""},
      {".", ""},
      {".", "s"},
      {"\\u00e9", ""},
      {"[\\u0100-\\uffff]", ""},
      {"\\ud800", ""},
      {"(?s:.)", ""},
      {"(?-s:.)", "s"},
      {"(?-m:^a$)", "m"}};
  const std::string ascii = "abc xa1\n\t!";
  std::u16string utf16 = u"abc xa1\n\t!\u00e9\u2028\uFEFF";
  utf16.push_back(0xd800);
  utf16.push_back(0xdc00);
  utf16.push_back(0xd800);
  for (auto pattern : patterns) {
    SCOPED_TRACE(pattern.first);
    auto compiled =
        hermes::CompiledRegExp::tryCompile(pattern.first, pattern.second);
    ASSERT_TRUE(compiled);
    auto descriptor = compiled->compileSimpleRegex();
    ASSERT_TRUE(descriptor);
    auto compare = [&](const auto &text, constants::MatchFlagType inputFlags) {
      for (uint32_t start = 0; start <= text.size(); ++start) {
        for (auto flags : {constants::matchDefault,
                           constants::matchOnlyAtStart,
                           constants::matchNotEndOfLine,
                           constants::matchNotBeginningOfLine}) {
          cmatch expected, actual;
          auto sink = [&](
                          CapturedRange full,
                          llvh::ArrayRef<CapturedRange> captures) {
            EXPECT_TRUE(captures.empty());
            actual.push_back(full);
          };
          auto result = searchWithBytecode(
              compiled->getBytecode(),
              text.data(),
              start,
              text.size(),
              &expected,
              inputFlags | flags);
          auto guard =
#ifdef HERMES_CHECK_NATIVE_STACK
              hermes::StackOverflowGuard::nativeStackGuard(512 * 1024);
#else
              hermes::StackOverflowGuard::depthCounterGuard(128);
#endif
          EXPECT_EQ(
              result,
              searchWithSimpleRegex(
                  descriptor->data(),
                  text.data(),
                  start,
                  text.size(),
                  sink,
                  inputFlags | flags,
                  guard));
          EXPECT_EQ(flatten(expected), flatten(actual));
        }
      }
    };
    compare(ascii, constants::matchInputAllAscii);
    compare(utf16, constants::matchDefault);
  }
}

TEST(Regex, SimpleDescriptorEligibility) {
  for (const char *pattern : {"",
                             "(a)",
                             "(?=a)a",
                             "a|b",
                             "a*",
                             "a{2}",
                             "(?i:a)",
                             "(?m:^a)",
                             "abcdefghijklmnopq"}) {
    auto compiled = hermes::CompiledRegExp::tryCompile(pattern, "");
    ASSERT_TRUE(compiled);
    EXPECT_FALSE(compiled->compileSimpleRegex()) << pattern;
  }
  for (const char *flags : {"i", "u"}) {
    auto compiled = hermes::CompiledRegExp::tryCompile("a", flags);
    ASSERT_TRUE(compiled);
    EXPECT_FALSE(compiled->compileSimpleRegex());
  }
  auto maxTerms = hermes::CompiledRegExp::tryCompile("abcdefghijklmnop", "");
  ASSERT_TRUE(maxTerms);
  EXPECT_TRUE(maxTerms->compileSimpleRegex());
  std::string ranges = "[";
  for (unsigned i = 0; i < 33; ++i) {
    // Separated singleton ranges cannot coalesce during bytecode generation.
    ranges += "\\u01";
    ranges += "01234"[i / 8];
    ranges += "02468ace"[i % 8];
    if (i == 31 || i == 32) {
      auto compiled = hermes::CompiledRegExp::tryCompile(ranges + "]", "");
      ASSERT_TRUE(compiled);
      EXPECT_EQ(i == 31, compiled->compileSimpleRegex().hasValue());
    }
  }
  auto large = hermes::CompiledRegExp::tryCompile(std::string(1024, 'a'), "");
  ASSERT_TRUE(large);
  EXPECT_FALSE(large->compileSimpleRegex());
}

#ifndef HERMES_CHECK_NATIVE_STACK
TEST(Regex, SimpleDescriptorStackGuardOrdering) {
  for (const char *pattern : {"a", "a$", "^a", "\\u0100"}) {
    auto compiled = hermes::CompiledRegExp::tryCompile(pattern, "");
    ASSERT_TRUE(compiled);
    auto descriptor = compiled->compileSimpleRegex();
    ASSERT_TRUE(descriptor);
    for (std::string text : {"", "ab"}) {
      for (uint32_t start = 0; start <= text.size(); ++start) {
        auto guard = hermes::StackOverflowGuard::depthCounterGuard(0);
        auto flags =
            constants::matchInputAllAscii | constants::matchNotEndOfLine;
        EXPECT_EQ(
            searchWithBytecode(
                compiled->getBytecode(),
                text.data(),
                start,
                text.size(),
                nullptr,
                flags,
                guard),
            searchWithSimpleRegex(
                descriptor->data(),
                text.data(),
                start,
                text.size(),
                {},
                flags,
                guard));
      }
    }
  }
}
#endif

TEST(Regex, CaptureSinkAndVectorFailure) {
  auto bytecode = cregex(u"(?=(a))(a)(b)?").compile();
  cmatch output;
  unsigned calls = 0;
  auto sink = [&](CapturedRange full, llvh::ArrayRef<CapturedRange> captures) {
    ++calls;
    output.push_back(full);
    output.insert(output.end(), captures.begin(), captures.end());
  };
  auto guard =
#ifdef HERMES_CHECK_NATIVE_STACK
      hermes::StackOverflowGuard::nativeStackGuard(512 * 1024);
#else
      hermes::StackOverflowGuard::depthCounterGuard(128);
#endif
  EXPECT_EQ(
      MatchRuntimeResult::Match,
      searchWithBytecode(
          bytecode, u"a", 0, 1, sink, constants::matchDefault, guard));
  EXPECT_EQ("(0-1) (0-1) (0-1) nm", flatten(output));
  EXPECT_EQ(
      MatchRuntimeResult::NoMatch,
      searchWithBytecode(
          bytecode, u"x", 0, 1, sink, constants::matchDefault, guard));
  EXPECT_EQ(1u, calls);
  EXPECT_EQ(
      MatchRuntimeResult::NoMatch,
      searchWithBytecode(
          bytecode, u"x", 0, 1, &output, constants::matchDefault, guard));
  EXPECT_EQ("(0-1) (0-1) (0-1) nm", flatten(output));
  EXPECT_EQ(
      MatchRuntimeResult::Match,
      searchWithBytecode(
          bytecode, u"a", 0, 1, nullptr, constants::matchDefault, guard));
}

MatchRuntimeResult searchResult(
    const char16_t *text,
    const char16_t *pattern,
    const char16_t *flags = u"") {
  cmatch m;
  return searchWithBytecode(
      cregex(pattern, flags).compile(),
      text,
      0,
      std::char_traits<char16_t>::length(text),
      &m,
      constants::matchDefault);
}

TEST(Regex, ExecFail) {
  EXPECT_EQ(
      searchResult(u"", u"(){999999999}"), MatchRuntimeResult::StackOverflow);
  EXPECT_EQ(
      searchResult(u"", u"(?=^){999999999}"),
      MatchRuntimeResult::StackOverflow);
}

/* The following tests are adapted from re.alg.search/ecma.pass.cpp in libc++ */
TEST(Regex, FromLibCXX) {
  {
    cmatch m;
    const char16_t s[] = u"a";
    EXPECT_TRUE(search(s, m, cregex(u"a")));
    EXPECT_EQ("(0-1)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"ab";
    EXPECT_TRUE(search(s, m, cregex(u"ab")));
    EXPECT_EQ("(0-2)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"ab";
    EXPECT_FALSE(search(s, m, cregex(u"ba")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"aab";
    EXPECT_TRUE(search(s, m, cregex(u"ab")));
    EXPECT_EQ("(1-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abcd";
    EXPECT_TRUE(search(s, m, cregex(u"bc")));
    EXPECT_EQ("(1-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abbc";
    EXPECT_TRUE(search(s, m, cregex(u"ab*c")));
    EXPECT_EQ("(0-4)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"ababc";
    EXPECT_TRUE(search(s, m, cregex(u"(ab)*c")));
    EXPECT_EQ("(0-5) (2-4)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abcdefghijk";
    EXPECT_TRUE(search(s, m, cregex(u"cd((e)fg)hi")));
    EXPECT_EQ("(2-9) (4-7) (4-5)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abc";
    EXPECT_TRUE(search(s, m, cregex(u"^abc")));
    EXPECT_EQ("(0-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abcd";
    EXPECT_TRUE(search(s, m, cregex(u"^abc")));
    EXPECT_EQ("(0-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"aabc";
    EXPECT_FALSE(search(s, m, cregex(u"^abc")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abc";
    EXPECT_TRUE(search(s, m, cregex(u"abc$")));
    EXPECT_EQ("(0-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"efabc";
    EXPECT_TRUE(search(s, m, cregex(u"abc$")));
    EXPECT_EQ("(2-5)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"efabcg";
    EXPECT_FALSE(search(s, m, cregex(u"abc$")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abc";
    EXPECT_TRUE(search(s, m, cregex(u"a.c")));
    EXPECT_EQ("(0-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"acc";
    EXPECT_TRUE(search(s, m, cregex(u"a.c")));
    EXPECT_EQ("(0-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"acc";
    EXPECT_TRUE(search(s, m, cregex(u"a.c")));
    EXPECT_EQ("(0-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abcdef";
    EXPECT_TRUE(search(s, m, cregex(u"(.*).*")));
    EXPECT_EQ("(0-6) (0-6)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"bc";
    EXPECT_TRUE(search(s, m, cregex(u"(a*)*")));
    EXPECT_EQ("(0-0) nm", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abbc";
    EXPECT_FALSE(search(s, m, cregex(u"ab{3,5}c")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abbbc";
    EXPECT_TRUE(search(s, m, cregex(u"ab{3,5}c")));
    EXPECT_EQ(m.size(), 1u);
    EXPECT_EQ("(0-5)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abbbbc";
    EXPECT_TRUE(search(s, m, cregex(u"ab{3,5}c")));
    EXPECT_EQ("(0-6)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abbbbbc";
    EXPECT_TRUE(search(s, m, cregex(u"ab{3,5}c")));
    EXPECT_EQ("(0-7)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"adefc";
    EXPECT_FALSE(search(s, m, cregex(u"ab{3,5}c")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abbbbbbc";
    EXPECT_FALSE(search(s, m, cregex(u"ab{3,5}c")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"adec";
    EXPECT_FALSE(search(s, m, cregex(u"a.{3,5}c")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"adefc";
    EXPECT_TRUE(search(s, m, cregex(u"a.{3,5}c")));
    EXPECT_EQ("(0-5)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"adefgc";
    EXPECT_TRUE(search(s, m, cregex(u"a.{3,5}c")));
    EXPECT_EQ("(0-6)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"adefghc";
    EXPECT_TRUE(search(s, m, cregex(u"a.{3,5}c")));
    EXPECT_EQ("(0-7)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"adefghic";
    EXPECT_FALSE(search(s, m, cregex(u"a.{3,5}c")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"tournament";
    EXPECT_TRUE(search(s, m, cregex(u"tour|to|tournament")));
    EXPECT_EQ("(0-4)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"tournamenttotour";
    EXPECT_TRUE(search(s, m, cregex(u"(tour|to|tournament)+")));
    EXPECT_EQ("(0-4) (0-4)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"ttotour";
    EXPECT_TRUE(search(s, m, cregex(u"(tour|to|t)+")));
    EXPECT_EQ("(0-7) (3-7)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"-ab,ab-";
    EXPECT_FALSE(search(s, m, cregex(u"-(.*),\1-")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"-ab,ab-";
    EXPECT_TRUE(search(s, m, cregex(u"-.*,.*-")));
    EXPECT_EQ("(0-7)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"a";
    EXPECT_TRUE(search(s, m, cregex(u"^[a]$")));
    EXPECT_EQ("(0-1)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"a";
    EXPECT_TRUE(search(s, m, cregex(u"^[ab]$")));
    EXPECT_EQ("(0-1)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"c";
    EXPECT_TRUE(search(s, m, cregex(u"^[a-f]$")));
    EXPECT_EQ("(0-1)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"g";
    EXPECT_FALSE(search(s, m, cregex(u"^[a-f]$")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"Iraqi";
    EXPECT_TRUE(search(s, m, cregex(u"q[^u]")));
    EXPECT_EQ("(3-5)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"Iraq";
    EXPECT_FALSE(search(s, m, cregex(u"q[^u]")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"01a45cef9";
    EXPECT_TRUE(search(s, m, cregex(u"[ace1-9]*")));
    EXPECT_EQ("(0-0)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"01a45cef9";
    EXPECT_TRUE(search(s, m, cregex(u"[ace1-9]+")));
    EXPECT_EQ("(1-7)", flatten(m));
  }
  {
    const char16_t r[] = u"^[-+]?[0-9]+[CF]$";
    std::ptrdiff_t sr = std::char_traits<char16_t>::length(r);
    using FI = const char16_t *;
    using BI = const char16_t *;
    cregex regex(llvh::ArrayRef<char16_t>(FI(r), FI(r + sr)));
    cmatch m;
    const char16_t s[] = u"-40C";
    std::ptrdiff_t ss = std::char_traits<char16_t>::length(s);
    EXPECT_TRUE(search(BI(s), BI(s + ss), m, regex));
    EXPECT_EQ("(0-4)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"Jeff Jeffs u";
    EXPECT_TRUE(search(s, m, cregex(u"Jeff(?=s\\b)")));
    EXPECT_EQ("(5-9)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"Jeffs Jeff";
    EXPECT_TRUE(search(s, m, cregex(u"Jeff(?!s\\b)")));
    EXPECT_EQ("(6-10)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"5%k";
    EXPECT_TRUE(search(s, m, cregex(u"\\d[\\W]k")));
    EXPECT_EQ("(0-3)", flatten(m));
  }

  {
    cmatch m;
    const char16_t s[] = u"a";
    EXPECT_TRUE(search(s, m, cregex(u"a")));
    EXPECT_EQ("(0-1)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"ab";
    EXPECT_TRUE(search(s, m, cregex(u"ab")));
    EXPECT_EQ("(0-2)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"ab";
    EXPECT_FALSE(search(s, m, cregex(u"ba")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"aab";
    EXPECT_TRUE(search(s, m, cregex(u"ab")));
    EXPECT_EQ("(1-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abcd";
    EXPECT_TRUE(search(s, m, cregex(u"bc")));
    EXPECT_EQ("(1-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abbc";
    EXPECT_TRUE(search(s, m, cregex(u"ab*c")));
    EXPECT_EQ("(0-4)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"ababc";
    EXPECT_TRUE(search(s, m, cregex(u"(ab)*c")));
    EXPECT_EQ("(0-5) (2-4)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abcdefghijk";
    EXPECT_TRUE(search(s, m, cregex(u"cd((e)fg)hi")));
    EXPECT_EQ("(2-9) (4-7) (4-5)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abc";
    EXPECT_TRUE(search(s, m, cregex(u"^abc")));
    EXPECT_EQ("(0-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abcd";
    EXPECT_TRUE(search(s, m, cregex(u"^abc")));
    EXPECT_EQ("(0-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"aabc";
    EXPECT_FALSE(search(s, m, cregex(u"^abc")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abc";
    EXPECT_TRUE(search(s, m, cregex(u"abc$")));
    EXPECT_EQ("(0-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"efabc";
    EXPECT_TRUE(search(s, m, cregex(u"abc$")));
    EXPECT_EQ("(2-5)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"efabcg";
    EXPECT_FALSE(search(s, m, cregex(u"abc$")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abc";
    EXPECT_TRUE(search(s, m, cregex(u"a.c")));
    EXPECT_EQ("(0-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"acc";
    EXPECT_TRUE(search(s, m, cregex(u"a.c")));
    EXPECT_EQ("(0-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"acc";
    EXPECT_TRUE(search(s, m, cregex(u"a.c")));
    EXPECT_EQ("(0-3)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abcdef";
    EXPECT_TRUE(search(s, m, cregex(u"(.*).*")));
    EXPECT_EQ("(0-6) (0-6)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"bc";
    EXPECT_TRUE(search(s, m, cregex(u"(a*)*")));
    EXPECT_EQ("(0-0) nm", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abbc";
    EXPECT_FALSE(search(s, m, cregex(u"ab{3,5}c")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abbbc";
    EXPECT_TRUE(search(s, m, cregex(u"ab{3,5}c")));
    EXPECT_EQ("(0-5)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abbbbc";
    EXPECT_TRUE(search(s, m, cregex(u"ab{3,5}c")));
    EXPECT_EQ("(0-6)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abbbbbc";
    EXPECT_TRUE(search(s, m, cregex(u"ab{3,5}c")));
    EXPECT_EQ("(0-7)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"adefc";
    EXPECT_FALSE(search(s, m, cregex(u"ab{3,5}c")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"abbbbbbc";
    EXPECT_FALSE(search(s, m, cregex(u"ab{3,5}c")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"adec";
    EXPECT_FALSE(search(s, m, cregex(u"a.{3,5}c")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"adefc";
    EXPECT_TRUE(search(s, m, cregex(u"a.{3,5}c")));
    EXPECT_EQ("(0-5)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"adefgc";
    EXPECT_TRUE(search(s, m, cregex(u"a.{3,5}c")));
    EXPECT_EQ("(0-6)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"adefghc";
    EXPECT_TRUE(search(s, m, cregex(u"a.{3,5}c")));
    EXPECT_EQ("(0-7)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"adefghic";
    EXPECT_FALSE(search(s, m, cregex(u"a.{3,5}c")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"tournament";
    EXPECT_TRUE(search(s, m, cregex(u"tour|to|tournament")));
    EXPECT_EQ("(0-4)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"tournamenttotour";
    EXPECT_TRUE(search(s, m, cregex(u"(tour|to|tournament)+")));
    EXPECT_EQ("(0-4) (0-4)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"ttotour";
    EXPECT_TRUE(search(s, m, cregex(u"(tour|to|t)+")));
    EXPECT_EQ("(0-7) (3-7)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"-ab,ab-";
    EXPECT_FALSE(search(s, m, cregex(u"-(.*),\1-")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"-ab,ab-";
    EXPECT_TRUE(search(s, m, cregex(u"-.*,.*-")));
    EXPECT_EQ("(0-7)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"a";
    EXPECT_TRUE(search(s, m, cregex(u"^[a]$")));
    EXPECT_EQ("(0-1)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"a";
    EXPECT_TRUE(search(s, m, cregex(u"^[ab]$")));
    EXPECT_EQ("(0-1)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"c";
    EXPECT_TRUE(search(s, m, cregex(u"^[a-f]$")));
    EXPECT_EQ("(0-1)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"g";
    EXPECT_FALSE(search(s, m, cregex(u"^[a-f]$")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"Iraqi";
    EXPECT_TRUE(search(s, m, cregex(u"q[^u]")));
    EXPECT_EQ("(3-5)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"Iraq";
    EXPECT_FALSE(search(s, m, cregex(u"q[^u]")));
    EXPECT_EQ("", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"01a45cef9";
    EXPECT_TRUE(search(s, m, cregex(u"[ace1-9]*")));
    EXPECT_EQ("(0-0)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"01a45cef9";
    EXPECT_TRUE(search(s, m, cregex(u"[ace1-9]+")));
    EXPECT_EQ("(1-7)", flatten(m));
  }
  {
    const char16_t r[] = u"^[-+]?[0-9]+[CF]$";
    std::ptrdiff_t sr = std::char_traits<char16_t>::length(r);
    using FI = const char16_t *;
    using BI = const char16_t *;
    cregex regex(llvh::ArrayRef<char16_t>(FI(r), FI(r + sr)));
    cmatch m;
    const char16_t s[] = u"-40C";
    std::ptrdiff_t ss = std::char_traits<char16_t>::length(s);
    EXPECT_TRUE(search(BI(s), BI(s + ss), m, regex));
    EXPECT_EQ("(0-4)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"Jeff Jeffs u";
    EXPECT_TRUE(search(s, m, cregex(u"Jeff(?=s\\b)")));
    EXPECT_EQ("(5-9)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"Jeffs Jeff";
    EXPECT_TRUE(search(s, m, cregex(u"Jeff(?!s\\b)")));
    EXPECT_EQ("(6-10)", flatten(m));
  }
  {
    cmatch m;
    const char16_t s[] = u"5%k";
    EXPECT_TRUE(search(s, m, cregex(u"\\d[\\W]k")));
    EXPECT_EQ("(0-3)", flatten(m));
  }
}

static constants::ErrorType error_for(
    const char16_t *pattern,
    const char16_t *flags = u"") {
  return cregex(pattern, flags).getError();
};

TEST(Regex, Invalid) {
  using namespace constants;
  EXPECT_EQ(error_for(u""), ErrorType::None);
  EXPECT_EQ(error_for(u"abc\\"), ErrorType::EscapeIncomplete);
  EXPECT_EQ(error_for(u"abc\\5"), ErrorType::None);
  EXPECT_EQ(error_for(u"abc\\5def"), ErrorType::None);
  EXPECT_EQ(error_for(u"abc\\99999999999999999999999999def"), ErrorType::None);
  EXPECT_EQ(error_for(u"abc[def"), ErrorType::UnbalancedBracket);
  EXPECT_EQ(error_for(u"abc(def"), ErrorType::UnbalancedParenthesis);
  EXPECT_EQ(error_for(u"abc{3"), ErrorType::None);
  EXPECT_EQ(error_for(u"abc{3,e}"), ErrorType::None);
  EXPECT_EQ(error_for(u"{3,,5}"), ErrorType::None);
  EXPECT_EQ(error_for(u"{3,5}"), ErrorType::InvalidRepeat);
  EXPECT_EQ(error_for(u"abc{10,3}"), ErrorType::BraceRange);
  EXPECT_EQ(error_for(u"abc[b-a]"), ErrorType::CharacterRange);
  EXPECT_EQ(error_for(u"(*)"), ErrorType::InvalidRepeat);
  EXPECT_EQ(error_for(u"(+)"), ErrorType::InvalidRepeat);
  EXPECT_EQ(error_for(u"({1})"), ErrorType::InvalidRepeat);
  EXPECT_EQ(error_for(u"(?)"), ErrorType::InvalidFlags);
  EXPECT_EQ(error_for(u"abc", u"a"), ErrorType::InvalidFlags);
  EXPECT_EQ(error_for(u"abc", u"gg"), ErrorType::InvalidFlags);
  EXPECT_EQ(error_for(u"(?=a){1}", u"u"), ErrorType::InvalidRepeat);
  EXPECT_EQ(error_for(u"(?<=a){1}", u"u"), ErrorType::InvalidRepeat);
  EXPECT_EQ(error_for(u"(?=a){1}"), ErrorType::None);
  EXPECT_EQ(error_for(u"(?!a){1}"), ErrorType::None);
  EXPECT_EQ(error_for(u"(?<=a){1}"), ErrorType::InvalidRepeat);
}

TEST(Regex, InvalidFromLibCXX) {
  using namespace constants;
  const char16_t *pat1 = u"a(b)c\\1234";
  EXPECT_EQ(
      cregex(llvh::ArrayRef<char16_t>(pat1, pat1 + 7)).getError(),
      ErrorType::None);

  EXPECT_EQ(error_for(u"[\\a]"), ErrorType::None);
  EXPECT_EQ(error_for(u"\\a"), ErrorType::None);
  EXPECT_EQ(error_for(u"\\"), ErrorType::EscapeIncomplete);
  EXPECT_EQ(error_for(u"[\\e]"), ErrorType::None);
  EXPECT_EQ(error_for(u"\\e"), ErrorType::None);
  EXPECT_EQ(error_for(u"[\\c:]"), ErrorType::None);
  EXPECT_EQ(error_for(u"\\c:"), ErrorType::None);
  EXPECT_EQ(error_for(u"\\c"), ErrorType::None);
  EXPECT_EQ(error_for(u"[\\cA]"), ErrorType::None);
  EXPECT_EQ(error_for(u"[\\CA]"), ErrorType::None);
  EXPECT_EQ(error_for(u"\\cA"), ErrorType::None);

  EXPECT_EQ(error_for(u"?a"), ErrorType::InvalidRepeat);
  EXPECT_EQ(error_for(u"*a"), ErrorType::InvalidRepeat);
  EXPECT_EQ(error_for(u"+a"), ErrorType::InvalidRepeat);
  EXPECT_EQ(error_for(u"{a"), ErrorType::None);

  EXPECT_EQ(error_for(u"?(a+)"), ErrorType::InvalidRepeat);
  EXPECT_EQ(error_for(u"*(a+)"), ErrorType::InvalidRepeat);
  EXPECT_EQ(error_for(u"+(a+)"), ErrorType::InvalidRepeat);
  EXPECT_EQ(error_for(u"{(a+)"), ErrorType::None);
}

TEST(RegExp, RangeChecks) {
  using namespace constants;
  EXPECT_EQ(error_for(u"[a-b]"), ErrorType::None);
  EXPECT_EQ(error_for(u"[b-a]"), ErrorType::CharacterRange);
  EXPECT_EQ(error_for(u"[\\uD800-\\uDFFF]"), ErrorType::None);
  EXPECT_EQ(error_for(u"[\\uDFFF-\\uD800]"), ErrorType::CharacterRange);
  // In the following we are required to interpret - as a ClassAtom and not
  // part of a range.
  EXPECT_EQ(error_for(u"[a\\s-b]"), ErrorType::None);
  EXPECT_EQ(error_for(u"[x-\\S]"), ErrorType::None);
  EXPECT_EQ(error_for(u"[\\d-\\S]"), ErrorType::None);
}

static bool regexIsAnchored(
    const char16_t *pattern,
    const char16_t *flags = u"") {
  return cregex(pattern, flags).matchConstraints() &
      MatchConstraintAnchoredAtStart;
}

static bool regexIsNonASCII(
    const char16_t *pattern,
    const char16_t *flags = u"") {
  return cregex(pattern, flags).matchConstraints() & MatchConstraintNonASCII;
}

TEST(Regex, LineStartAnchoring) {
  EXPECT_TRUE(regexIsAnchored(u"^abc"));
  EXPECT_TRUE(regexIsAnchored(u"abc^"));
  EXPECT_TRUE(regexIsAnchored(u"abc^"));
  EXPECT_TRUE(regexIsAnchored(u"abc^|^def"));
  EXPECT_TRUE(regexIsAnchored(u"abc^|^def"));
  EXPECT_TRUE(regexIsAnchored(u"^foo|^bar|^baz"));
  EXPECT_TRUE(regexIsAnchored(u"(^bar)"));
  EXPECT_TRUE(regexIsAnchored(u"(?=^bar)\\w+"));
  EXPECT_FALSE(regexIsAnchored(u"(?!^bar)\\w+"));
  EXPECT_FALSE(regexIsAnchored(u"(?=^bar)|\\w+"));
  EXPECT_FALSE(regexIsAnchored(u"abc"));
  EXPECT_FALSE(regexIsAnchored(u"abc|^def"));
  EXPECT_FALSE(regexIsAnchored(u"abc^|^def|\\w"));
  EXPECT_FALSE(regexIsAnchored(u"abc^", u"m"));
  EXPECT_FALSE(regexIsAnchored(u"abc", u"m"));
}

TEST(Regex, NonASCII) {
  EXPECT_TRUE(regexIsNonASCII(u"ab\xFF"));
  EXPECT_TRUE(regexIsNonASCII(u"ab(?:\xFF)"));
  EXPECT_TRUE(regexIsNonASCII(u"[\xFE]"));
  EXPECT_FALSE(regexIsNonASCII(u"[0\xFE]"));
  EXPECT_FALSE(regexIsNonASCII(u"[^\xFE]"));
  EXPECT_FALSE(regexIsNonASCII(u"[\\w\xFE]"));
  EXPECT_FALSE(regexIsNonASCII(u"[\x7F\xFE]"));
  EXPECT_FALSE(regexIsNonASCII(u"abFF"));
  EXPECT_FALSE(regexIsNonASCII(u"^abFF"));
  EXPECT_FALSE(regexIsNonASCII(u"ab(\xFF|c)"));

  // This regex will take a very long time unless we exploit the non-ASCII
  // optimization.

  const std::u16string ascii = u"This is a pretty long all-ASCII string";
  cmatch matchRanges;
  EXPECT_FALSE(search(
      ascii,
      matchRanges,
      cregex(u"(((.*)*)*)*\xFF"),
      constants::matchInputAllAscii));
  EXPECT_FALSE(search(
      ascii,
      matchRanges,
      cregex(u"(?!(((.*)*)*)*\xFF)abc"),
      constants::matchInputAllAscii));
}

} // end anonymous namespace
