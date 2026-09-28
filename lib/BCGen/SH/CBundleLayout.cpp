/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CBundleLayout.h"

#include "hermes/IR/Instrs.h"
#include "llvh/ADT/BitVector.h"
#include "llvh/ADT/StringMap.h"
#include "llvh/ADT/StringSet.h"
#include "llvh/Support/SHA1.h"

#include <set>
#include <tuple>

namespace hermes::sh {
namespace {

constexpr size_t kRetainedFunctionSlots = 64 * 1024;
constexpr uint32_t kRetainedCacheSlots = 1024 * 1024;
constexpr uint32_t kRetainedEnvironmentSlots = 256 * 1024;
constexpr size_t kVariableAnchors = 4;
constexpr size_t kRetainedShardGroups = 65533;
constexpr size_t kRetainedLiteralBytes = 8 * 1024 * 1024;
constexpr size_t kRetainedLiteralEntries = 256 * 1024;
constexpr size_t kRetainedLiteralShapes = 64 * 1024;

/// Function-local shape, without source coordinates, identifier spellings, or
/// module ordinals. This deliberately is not a semantic program hash: changed
/// callees, captures, attributes, and even hash collisions still regenerate C.
/// Occurrence suffixes keep duplicate shapes injective within each compilation.
std::string functionLayoutKey(Function *function) {
  llvh::SHA1 hash;
  auto number = [&](uint64_t value) {
    uint8_t bytes[8];
    for (unsigned i = 0; i < 8; ++i)
      bytes[i] = value >> (8 * i);
    hash.update(bytes);
  };
  auto string = [&](llvh::StringRef value) {
    number(value.size());
    hash.update(value);
  };
  llvh::DenseMap<Value *, uint32_t> localValues;
  for (auto &block : *function) {
    localValues.try_emplace(&block, localValues.size());
    for (auto &instruction : block)
      localValues.try_emplace(&instruction, localValues.size());
  }
  number(static_cast<unsigned>(function->getKind()));
  number(function->getExpectedParamCountIncludingThis());
  for (auto &block : *function) {
    number(block.size());
    for (auto &instruction : block) {
      number(static_cast<unsigned>(instruction.getKind()));
      number(instruction.getNumOperands());
      for (unsigned i = 0; i < instruction.getNumOperands(); ++i) {
        auto *value = instruction.getOperand(i);
        if (!value) {
          number(UINT64_MAX);
          continue;
        }
        number(static_cast<unsigned>(value->getKind()));
        if (auto *literal = llvh::dyn_cast<LiteralNumber>(value)) {
          number(llvh::DoubleToBits(literal->getValue()));
        } else if (auto *literal = llvh::dyn_cast<LiteralString>(value)) {
          string(literal->getValue().str());
        } else if (auto *literal = llvh::dyn_cast<LiteralBigInt>(value)) {
          string(literal->getValue()->str());
        } else if (auto *literal = llvh::dyn_cast<LiteralBool>(value)) {
          number(literal->getValue());
        } else if (auto *param = llvh::dyn_cast<JSDynamicParam>(value)) {
          number(param->getIndexInParamList());
        } else {
          // Number external references by first local use, preserving their
          // equality relationships without importing a changing parent layout.
          auto entry = localValues.try_emplace(value, localValues.size());
          number(entry.first->second);
        }
      }
    }
  }
  std::string key;
  const char *hex = "0123456789abcdef";
  for (unsigned char byte : hash.final()) {
    key += hex[byte >> 4];
    key += hex[byte & 15];
  }
  return key;
}

bool validFunctionKey(llvh::StringRef key) {
  if (key.size() < 42 || key.size() > 51 || key[40] != ':')
    return false;
  for (char c : key.take_front(40))
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return false;
  auto occurrence = key.drop_front(41);
  if (occurrence.size() > 1 && occurrence.front() == '0')
    return false;
  for (char c : occurrence)
    if (c < '0' || c > '9')
      return false;
  return true;
}

} // namespace

bool CBundleFunctionLayout::readLiterals(const llvh::json::Value &seed) {
  if (seed.getAsNull())
    return true;
  const auto *object = seed.getAsObject();
  if (!object || object->size() != 5)
    return false;
  auto decode = [&](llvh::StringRef bytesKey,
                    llvh::StringRef entriesKey,
                    std::vector<unsigned char> &bytes,
                    std::vector<StringTableEntry> &entries) {
    const auto text = object->getString(bytesKey);
    const auto *rawEntries = object->getArray(entriesKey);
    if (!text || text->size() % 2 ||
        text->size() / 2 > kRetainedLiteralBytes ||
        !rawEntries || rawEntries->size() > kRetainedLiteralEntries)
      return false;
    unsigned byte = 0;
    for (size_t i = 0; i < text->size(); ++i) {
      char c = (*text)[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
        return false;
      byte = byte * 16 + (c <= '9' ? c - '0' : c - 'a' + 10);
      if (i % 2) {
        bytes.push_back(byte);
        byte = 0;
      }
    }
    llvh::StringSet<> seen;
    for (const auto &rawEntry : *rawEntries) {
      const auto *entry = rawEntry.getAsArray();
      if (!entry || entry->size() != 2)
        return false;
      auto offset = (*entry)[0].getAsInteger();
      auto length = (*entry)[1].getAsInteger();
      if (!offset || !length || *offset < 0 || *length < 0 ||
          static_cast<uint64_t>(*offset) > bytes.size() ||
          static_cast<uint64_t>(*length) > bytes.size() - *offset)
        return false;
      llvh::StringRef segment{
          bytes.empty()
              ? ""
              : reinterpret_cast<const char *>(bytes.data()) + *offset,
          static_cast<size_t>(*length)};
      // The storage table and the uniqued serialized set must have matching
      // indices. Overlap is valid compression, but repeated exact entries are not.
      if (!seen.insert(segment).second)
        return false;
      entries.emplace_back(*offset, *length, false);
    }
    return true;
  };
  if (!decode(
          "values",
          "valueEntries",
          literalBuffers_.values,
          literalBuffers_.valueEntries) ||
      !decode(
          "keys",
          "keyEntries",
          literalBuffers_.keys,
          literalBuffers_.keyEntries))
    return false;
  const auto *shapes = object->getArray("shapes");
  if (!shapes || shapes->size() > kRetainedLiteralShapes)
    return false;
  llvh::DenseSet<uint32_t> keyOffsets;
  for (const auto &entry : literalBuffers_.keyEntries)
    keyOffsets.insert(entry.getOffset());
  std::set<std::tuple<uint32_t, uint32_t, uint8_t>> seenShapes;
  for (const auto &rawShape : *shapes) {
    const auto *shape = rawShape.getAsArray();
    if (!shape || shape->size() != 3)
      return false;
    auto offset = (*shape)[0].getAsInteger();
    auto count = (*shape)[1].getAsInteger();
    auto kind = (*shape)[2].getAsInteger();
    if (!offset || !count || !kind || *offset < 0 || *offset > UINT32_MAX ||
        *count < 0 || *count > UINT32_MAX || !keyOffsets.count(*offset) ||
        (*kind !=
             static_cast<uint8_t>(ValueKind::LIRAllocObjectFromBufferInstKind) &&
         *kind != static_cast<uint8_t>(
                      ValueKind::LIRAllocTypedObjectFromBufferInstKind) &&
         *kind != static_cast<uint8_t>(
                      ValueKind::LIRAllocTypedNonEnumObjectFromBufferInstKind)) ||
        !seenShapes.emplace(*offset, *count, *kind).second)
      return false;
    literalBuffers_.shapes.push_back(
        {static_cast<uint32_t>(*offset), static_cast<uint32_t>(*count)});
    literalBuffers_.shapeKinds.push_back(*kind);
  }
  return true;
}

llvh::json::Value CBundleFunctionLayout::generateLiterals() const {
  const auto &buffers = literalBuffers_;
  auto excess = [](size_t total, size_t live, size_t minimum) {
    return total - live > std::max(minimum, live / 4);
  };
  if (buffers.values.size() > kRetainedLiteralBytes ||
      buffers.keys.size() > kRetainedLiteralBytes ||
      buffers.valueEntries.size() > kRetainedLiteralEntries ||
      buffers.keyEntries.size() > kRetainedLiteralEntries ||
      buffers.shapes.size() > kRetainedLiteralShapes ||
      excess(buffers.valueEntries.size(), buffers.liveValueEntries, 1024) ||
      excess(buffers.keyEntries.size(), buffers.liveKeyEntries, 1024) ||
      excess(buffers.shapes.size(), buffers.liveShapes, 1024) ||
      excess(buffers.values.size(), buffers.liveValueBytes, 64 * 1024) ||
      excess(buffers.keys.size(), buffers.liveKeyBytes, 64 * 1024))
    return nullptr;
  auto encode = [](llvh::ArrayRef<unsigned char> bytes) {
    std::string result;
    result.reserve(bytes.size() * 2);
    const char *hex = "0123456789abcdef";
    for (unsigned char byte : bytes) {
      result += hex[byte >> 4];
      result += hex[byte & 15];
    }
    return result;
  };
  auto entries = [](llvh::ArrayRef<StringTableEntry> entries) {
    llvh::json::Array result;
    for (const auto &entry : entries)
      result.push_back(llvh::json::Array{entry.getOffset(), entry.getLength()});
    return result;
  };
  llvh::json::Array shapes;
  for (size_t i = 0; i < buffers.shapes.size(); ++i) {
    const auto &shape = buffers.shapes[i];
    shapes.push_back(llvh::json::Array{
        shape.keyBufferOffset, shape.numProps, buffers.shapeKinds[i]});
  }
  return llvh::json::Object{
      {"values", encode(buffers.values)},
      {"valueEntries", entries(buffers.valueEntries)},
      {"keys", encode(buffers.keys)},
      {"keyEntries", entries(buffers.keyEntries)},
      {"shapes", std::move(shapes)},
  };
}

bool CBundleFunctionLayout::read(const llvh::json::Array &seed) {
  if (seed.size() > kRetainedFunctionSlots)
    return false;
  llvh::StringSet<> keys;
  std::array<llvh::BitVector, CacheKindCount> occupied;
  for (auto &bits : occupied)
    bits.resize(kRetainedCacheSlots);
  uint64_t totalSlots = 0;
  for (const auto &value : seed) {
    const auto *entry = value.getAsArray();
    if (!entry || entry->size() != 1 + CacheKindCount)
      return false;
    auto key = (*entry)[0].getAsString();
    if (!key || (slots_.empty() ? *key != "global" : !validFunctionKey(*key)) ||
        !keys.insert(*key).second)
      return false;
    Slot slot;
    slot.key = key->str();
    for (unsigned kind = 0; kind < CacheKindCount; ++kind) {
      const auto *ranges = (*entry)[kind + 1].getAsArray();
      if (!ranges)
        return false;
      for (const auto &rangeValue : *ranges) {
        const auto *range = rangeValue.getAsArray();
        if (!range || range->size() != 2)
          return false;
        auto start = (*range)[0].getAsInteger();
        auto count = (*range)[1].getAsInteger();
        if (!start || !count || *start < 0 || *count <= 0 ||
            *start >= kRetainedCacheSlots ||
            *count > kRetainedCacheSlots - *start ||
            (kind != Read && *count != 1))
          return false;
        totalSlots += *count;
        if (totalSlots > kRetainedCacheSlots)
          return false;
        const uint32_t end = *start + *count;
        for (uint32_t index = *start; index < end; ++index) {
          if (occupied[kind].test(index))
            return false;
          occupied[kind].set(index);
        }
        cacheSizes_[kind] = std::max(cacheSizes_[kind], end);
        slot.caches[kind].push_back(
            {static_cast<uint32_t>(*start), static_cast<uint32_t>(*count)});
      }
    }
    slots_.push_back(std::move(slot));
  }
  // Gaps can inflate a unit even with very few retained sites. Bound the total
  // allocated extent, including holes, rather than just the seed's records.
  uint64_t extent = 0;
  for (auto size : cacheSizes_)
    extent += size;
  return extent <= kRetainedCacheSlots;
}

bool CBundleFunctionLayout::readScopes(const llvh::json::Array &seed) {
  if (seed.size() > kRetainedEnvironmentSlots ||
      (slots_.empty() && !seed.empty()))
    return false;
  llvh::DenseSet<uint64_t> seen;
  uint64_t totalSlots = 0;
  for (const auto &value : seed) {
    const auto *scope = value.getAsArray();
    if (!scope || scope->empty())
      return false;
    totalSlots += scope->size();
    if (totalSlots > kRetainedEnvironmentSlots)
      return false;
    ScopeSlots entries;
    for (const auto &entryValue : *scope) {
      const auto *anchors = entryValue.getAsArray();
      if (!anchors || anchors->size() > kVariableAnchors)
        return false;
      std::vector<uint64_t> entry;
      for (const auto &anchorValue : *anchors) {
        auto anchor = anchorValue.getAsInteger();
        if (!anchor || *anchor < 0 ||
            (static_cast<uint64_t>(*anchor) >> 32) >= slots_.size() ||
            (!entry.empty() && static_cast<uint64_t>(*anchor) <= entry.back()) ||
            !seen.insert(*anchor).second)
          return false;
        entry.push_back(*anchor);
      }
      entries.push_back(std::move(entry));
    }
    retainedScopes_.push_back(std::move(entries));
  }
  return true;
}

void CBundleFunctionLayout::assign(llvh::ArrayRef<Function *> functions) {
  llvh::StringMap<uint32_t> byKey;
  for (uint32_t index = 0; index < slots_.size(); ++index)
    byKey[slots_[index].key] = index;
  llvh::DenseMap<Function *, std::string> shapes;
  llvh::DenseMap<Function *, size_t> sizes;
  llvh::StringMap<uint32_t> shapeCounts;
  for (auto *function : functions) {
    auto shape = functionLayoutKey(function);
    ++shapeCounts[shape];
    shapes[function] = std::move(shape);
    size_t size = 0;
    for (auto &block : *function)
      size += block.size();
    sizes[function] = size;
  }
  llvh::StringMap<uint32_t> occurrences;
  for (auto *function : functions) {
    std::string key = "global";
    if (function != functions.front()) {
      auto shape = shapes.find(function)->second;
      if (shapeCounts[shape] > 1) {
        // Unrelated parents often create identical small closures. Distinguish
        // them by property publication, or by a small creating parent's shape.
        // Never import arbitrary callers or large initializer shapes: one edit
        // there would otherwise change thousands of unrelated closure slots.
        std::vector<std::string> callers;
        std::vector<std::string> publications;
        for (auto *user : function->getUsers()) {
          if (!llvh::isa<BaseCreateLexicalChildInst>(user))
            continue;
          auto *caller = user->getFunction();
          if (caller != functions.front() && sizes.find(caller)->second <= 256)
            callers.push_back("parent:" + shapes.find(caller)->second);
          for (auto *consumer : user->getUsers()) {
            Value *property = nullptr;
            if (auto *store = llvh::dyn_cast<BaseStorePropertyInst>(consumer)) {
              if (store->getStoredValue() == user)
                property = store->getProperty();
            } else if (auto *define = llvh::dyn_cast<DefineOwnPropertyInst>(consumer)) {
              if (define->getStoredValue() == user)
                property = define->getProperty();
            }
            if (auto *name = llvh::dyn_cast_or_null<LiteralString>(property))
              publications.push_back("property:" + name->getValue().str().str());
          }
        }
        if (!publications.empty())
          callers = std::move(publications);
        std::sort(callers.begin(), callers.end());
        callers.erase(std::unique(callers.begin(), callers.end()), callers.end());
        llvh::SHA1 hash;
        hash.update(shape);
        for (const auto &caller : callers) {
          hash.update(std::to_string(caller.size()));
          hash.update(":");
          hash.update(caller);
        }
        shape.clear();
        const char *hex = "0123456789abcdef";
        for (unsigned char byte : hash.final()) {
          shape += hex[byte >> 4];
          shape += hex[byte & 15];
        }
      }
      key = shape + ":" + std::to_string(occurrences[shape]++);
    }
    auto entry = byKey.try_emplace(key, slots_.size());
    if (entry.second) {
      Slot slot;
      slot.key = std::move(key);
      slots_.push_back(std::move(slot));
    }
    auto index = entry.first->second;
    assert(!slots_[index].function && "function slots must be injective");
    slots_[index].function = function;
    functionSlots_[function] = index;
  }
  // An occurrence-matched getter can refer to another binding after deletion.
  // One uniquely matched consumer must outweigh every ambiguous hint for a
  // variable. The global initializer's local positions are also weak hints.
  for (auto &slot : slots_) {
    if (slot.function && slot.key != "global" &&
        occurrences[llvh::StringRef(slot.key).take_front(40)] == 1)
      slot.anchorWeight = kVariableAnchors + 1;
  }
}

bool CBundleFunctionLayout::readShards(const llvh::json::Array &seed) {
  if (seed.size() > kRetainedShardGroups ||
      (slots_.empty() && !seed.empty()))
    return false;
  llvh::BitVector seen(slots_.size());
  for (const auto &value : seed) {
    const auto *members = value.getAsArray();
    if (!members || members->size() > slots_.size())
      return false;
    std::vector<uint32_t> group;
    for (const auto &member : *members) {
      auto slot = member.getAsInteger();
      if (!slot || *slot < 0 || static_cast<uint64_t>(*slot) >= slots_.size() ||
          seen.test(*slot))
        return false;
      seen.set(*slot);
      group.push_back(*slot);
    }
    retainedShards_.push_back(std::move(group));
  }
  return true;
}

std::vector<CBundleFunctionLayout::ShardGroup>
CBundleFunctionLayout::groupFunctions(llvh::ArrayRef<Function *> functions) {
  std::vector<ShardGroup> groups;
  llvh::DenseMap<Function *, uint32_t> membership;
  for (uint32_t index = 0; index < retainedShards_.size(); ++index) {
    groups.push_back({index, true, {}});
    for (uint32_t slot : retainedShards_[index]) {
      if (auto *function = slots_[slot].function) {
        groups.back().functions.push_back(function);
        membership[function] = index;
      }
    }
  }
  currentShards_.resize(groups.size());
  if (membership.empty()) {
    groups.push_back({
        static_cast<uint32_t>(groups.size()),
        false,
        std::vector<Function *>(functions.begin(), functions.end()),
    });
    return groups;
  }

  // Function shape is not stable through a body edit. Keep new shapes near
  // their original neighbours instead of moving every edited function into a
  // separate translation unit. This is packing policy, not semantic identity.
  std::vector<size_t> nextMatch(functions.size(), functions.size());
  size_t next = functions.size();
  for (size_t i = functions.size(); i-- > 0;) {
    if (membership.count(functions[i]))
      next = i;
    nextMatch[i] = next;
  }
  size_t previous = functions.size();
  for (size_t i = 0; i < functions.size(); ++i) {
    if (membership.count(functions[i])) {
      previous = i;
      continue;
    }
    next = nextMatch[i];
    const size_t neighbour = previous == functions.size() ? next
        : next == functions.size()                       ? previous
        : i - previous < next - i                        ? previous
                                                         : next;
    groups[membership.find(functions[neighbour])->second].functions.push_back(
        functions[i]);
  }
  return groups;
}

void CBundleFunctionLayout::recordShard(
    uint32_t group,
    llvh::ArrayRef<Function *> functions) {
  if (currentShards_.size() <= group)
    currentShards_.resize(group + 1);
  auto &members = currentShards_[group];
  for (auto *function : functions) {
    const auto slot = getSlot(function);
    // Outlined fragments are consecutive pieces of the same function.
    if (members.empty() || members.back() != slot)
      members.push_back(slot);
  }
}

llvh::json::Array CBundleFunctionLayout::generateShards() const {
  llvh::json::Array result;
  size_t empty = 0;
  for (const auto &group : currentShards_)
    empty += group.empty();
  if (currentShards_.size() > kRetainedShardGroups ||
      empty > std::max<size_t>(1024, (currentShards_.size() - empty) / 4))
    return result;
  for (const auto &group : currentShards_) {
    llvh::json::Array members;
    for (auto slot : group)
      members.push_back(slot);
    result.push_back(std::move(members));
  }
  return result;
}

void CBundleFunctionLayout::assignScopes(Module *module) {
  // Each function numbers its distinct captured variables by first use.
  // Stable consumers supply identity when a shared initializer is edited or
  // a minifier changes every binding name. Keep several anchors per variable
  // so losing one consumer need not renumber the remaining environment.
  // Prefer unambiguous consumers, then consumers of few variables. Thousands
  // of identical getters can otherwise outweigh a stable, unique consumer.
  using Candidate = std::tuple<bool, uint32_t, uint64_t>;
  llvh::DenseMap<Variable *, std::vector<Candidate>> candidates;
  for (auto *function : functionsBySlot()) {
    if (!function)
      continue;
    llvh::DenseMap<Variable *, uint32_t> localVariables;
    for (auto &block : *function) {
      for (auto &instruction : block) {
        for (unsigned i = 0; i < instruction.getNumOperands(); ++i) {
          auto *variable = llvh::dyn_cast_or_null<Variable>(
              instruction.getOperand(i));
          if (!variable)
            continue;
          localVariables.try_emplace(variable, localVariables.size());
        }
      }
    }
    for (auto variable : localVariables) {
      uint64_t anchor = (uint64_t(getSlot(function)) << 32) | variable.second;
      Candidate candidate{
          slots_[getSlot(function)].anchorWeight == 1,
          localVariables.size(),
          anchor};
      auto &references = candidates[variable.first];
      auto position =
          std::lower_bound(references.begin(), references.end(), candidate);
      if (position != references.end() || references.size() < kVariableAnchors) {
        references.insert(position, candidate);
        if (references.size() > kVariableAnchors)
          references.pop_back();
      }
    }
  }
  llvh::DenseMap<Variable *, std::vector<uint64_t>> anchors;
  for (const auto &entry : candidates) {
    auto &references = anchors[entry.first];
    for (auto candidate : entry.second)
      references.push_back(std::get<2>(candidate));
    std::sort(references.begin(), references.end());
  }

  using Location = std::pair<uint32_t, uint32_t>;
  llvh::DenseMap<uint64_t, Location> previousAnchors;
  for (uint32_t scope = 0; scope < retainedScopes_.size(); ++scope)
    for (uint32_t slot = 0; slot < retainedScopes_[scope].size(); ++slot)
      for (auto anchor : retainedScopes_[scope][slot])
        previousAnchors[anchor] = {scope, slot};

  std::vector<VariableScope *> scopes;
  for (auto &scope : module->getVariableScopes()) {
    scopeSizes_[&scope] = scope.getVariables().size();
    if (!scope.getVariables().empty())
      scopes.push_back(&scope);
  }
  struct Match {
    uint32_t votes;
    uint32_t current;
    uint32_t previous;
  };
  auto strongestFirst = [](const Match &a, const Match &b) {
    if (a.votes != b.votes)
      return a.votes > b.votes;
    if (a.current != b.current)
      return a.current < b.current;
    return a.previous < b.previous;
  };
  std::vector<Match> matches;
  for (uint32_t scope = 0; scope < scopes.size(); ++scope) {
    llvh::DenseMap<uint32_t, uint32_t> votes;
    for (auto *variable : scopes[scope]->getVariables()) {
      for (auto anchor : anchors[variable]) {
        auto previous = previousAnchors.find(anchor);
        if (previous != previousAnchors.end())
          votes[previous->second.first] += slots_[anchor >> 32].anchorWeight;
      }
    }
    for (auto vote : votes)
      matches.push_back({vote.second, scope, vote.first});
  }
  std::sort(matches.begin(), matches.end(), strongestFirst);
  std::vector<uint32_t> previousScope(scopes.size(), UINT32_MAX);
  llvh::BitVector usedScopes(retainedScopes_.size());
  for (auto match : matches) {
    if (previousScope[match.current] == UINT32_MAX &&
        !usedScopes.test(match.previous)) {
      previousScope[match.current] = match.previous;
      usedScopes.set(match.previous);
    }
  }

  for (uint32_t scope = 0; scope < scopes.size(); ++scope) {
    auto variables = scopes[scope]->getVariables();
    auto previous = previousScope[scope];
    const auto oldSize = previous == UINT32_MAX
        ? 0
        : retainedScopes_[previous].size();
    const auto capacity = std::max(oldSize, variables.size());
    std::vector<uint32_t> assignment(variables.size(), UINT32_MAX);
    llvh::BitVector usedSlots(capacity);
    matches.clear();
    for (uint32_t variable = 0; variable < variables.size(); ++variable) {
      llvh::DenseMap<uint32_t, uint32_t> votes;
      for (auto anchor : anchors[variables[variable]]) {
        auto old = previousAnchors.find(anchor);
        if (old != previousAnchors.end() && old->second.first == previous)
          votes[old->second.second] += slots_[anchor >> 32].anchorWeight;
      }
      for (auto vote : votes)
        matches.push_back({vote.second, variable, vote.first});
    }
    std::sort(matches.begin(), matches.end(), strongestFirst);
    for (auto match : matches) {
      if (assignment[match.current] == UINT32_MAX &&
          !usedSlots.test(match.previous)) {
        assignment[match.current] = match.previous;
        usedSlots.set(match.previous);
      }
    }
    // All surviving variables have claimed their slots before holes can be
    // assigned to new variables. Never alias two live variables, even when
    // approximate anchors matched different bindings in the previous source.
    uint32_t nextFree = 0;
    for (uint32_t variable = 0; variable < variables.size(); ++variable) {
      if (assignment[variable] == UINT32_MAX) {
        while (usedSlots.test(nextFree))
          ++nextFree;
        assignment[variable] = nextFree;
        usedSlots.set(nextFree);
      }
    }
    // Keep exactly the live environment size: extra slots would add allocation
    // and GC work. Fill deletion holes with variables beyond the new end,
    // changing only those variables' offsets, before any body is emitted.
    nextFree = 0;
    for (auto &slot : assignment) {
      if (slot < variables.size())
        continue;
      while (usedSlots.test(nextFree))
        ++nextFree;
      assert(nextFree < variables.size() && "a live variable needs a free slot");
      usedSlots.reset(slot);
      slot = nextFree;
      usedSlots.set(nextFree);
    }
    ScopeSlots emitted(variables.size());
    for (uint32_t variable = 0; variable < variables.size(); ++variable) {
      variableSlots_[variables[variable]] = assignment[variable];
      emitted[assignment[variable]] = std::move(anchors[variables[variable]]);
    }
    currentScopes_.push_back(std::move(emitted));
  }
}

llvh::json::Array CBundleFunctionLayout::generateScopes() const {
  uint64_t count = 0;
  for (const auto &scope : currentScopes_)
    count += scope.size();
  llvh::json::Array result;
  if (count > kRetainedEnvironmentSlots || slots_.size() > kRetainedFunctionSlots)
    return result;
  for (const auto &scope : currentScopes_) {
    llvh::json::Array entries;
    for (const auto &slot : scope) {
      llvh::json::Array anchors;
      for (auto anchor : slot)
        anchors.push_back(static_cast<int64_t>(anchor));
      entries.push_back(std::move(anchors));
    }
    result.push_back(std::move(entries));
  }
  return result;
}

std::vector<Function *> CBundleFunctionLayout::functionsBySlot() const {
  std::vector<Function *> result;
  result.reserve(slots_.size());
  for (const auto &slot : slots_)
    result.push_back(slot.function);
  return result;
}

uint32_t CBundleFunctionLayout::allocateCache(
    Function *function,
    CacheKind kind,
    uint32_t count) {
  auto &slot = slots_[getSlot(function)];
  auto &ranges = slot.caches[kind];
  const auto site = slot.usedRanges[kind]++;
  if (site < ranges.size() && ranges[site].count >= count)
    return ranges[site].start;
  if (count > UINT32_MAX - cacheSizes_[kind])
    hermes_fatal("Static Hermes property cache exceeds index capacity");
  Range range{cacheSizes_[kind], count};
  cacheSizes_[kind] += count;
  if (site < ranges.size())
    ranges[site] = range;
  else
    ranges.push_back(range);
  return range.start;
}

llvh::json::Array CBundleFunctionLayout::generate() const {
  size_t liveFunctions = 0;
  uint64_t liveCacheSlots = 0, cacheSlots = 0;
  for (const auto &slot : slots_) {
    if (slot.function)
      ++liveFunctions;
    for (unsigned kind = 0; kind < CacheKindCount; ++kind)
      for (size_t site = 0; site < slot.usedRanges[kind]; ++site)
        liveCacheSlots += slot.caches[kind][site].count;
  }
  for (auto size : cacheSizes_)
    cacheSlots += size;
  llvh::json::Array result;
  if (slots_.size() > kRetainedFunctionSlots ||
      cacheSlots > kRetainedCacheSlots ||
      slots_.size() - liveFunctions >
          std::max<size_t>(1024, liveFunctions / 4) ||
      cacheSlots - liveCacheSlots >
          std::max<uint64_t>(1024, liveCacheSlots / 4))
    return result;
  for (const auto &slot : slots_) {
    llvh::json::Array entry{slot.key};
    for (const auto &ranges : slot.caches) {
      llvh::json::Array encoded;
      for (const auto &range : ranges)
        encoded.push_back(llvh::json::Array{range.start, range.count});
      entry.push_back(std::move(encoded));
    }
    result.push_back(std::move(entry));
  }
  return result;
}

} // namespace hermes::sh
