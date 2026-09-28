/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_SCOPEDSTRINGREUSE_H
#define HERMES_VM_SCOPEDSTRINGREUSE_H

#include "hermes/VM/Runtime.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/StringPrimitive.h"

#include <array>
#include <string>

namespace hermes {
namespace vm {

/// An embedding-owned invocation scope. Fixed-size keys/results are ordinary
/// VM locals, including suspended outer scopes during reentrant execution.
class ScopedStringReuse final {
 public:
  static constexpr unsigned maxInputs = 12;
  static constexpr unsigned capacity = 16;
  static constexpr uint64_t maxEntryStorageBytes = 64 * 1024;
  struct Statistics {
    uint64_t hits{0};
    uint64_t misses{0};
    uint64_t stores{0};
  };

 private:
  static constexpr unsigned stride = maxInputs + 1;
  static constexpr unsigned utf8Capacity = 4;
  static constexpr uint64_t maxUTF8InputStorageBytes = 16 * 1024;
  Runtime &runtime_;
  ScopedStringReuse *previous_;
  struct Roots : Locals {
    PinnedHermesValue values[capacity * stride];
    PinnedHermesValue scanPrototypeShapes[3];
    PinnedHermesValue utf8Inputs[utf8Capacity];
  } roots_;
  LocalsRAII rootGuard_;
  std::array<unsigned, capacity> counts_{};
  std::array<const void *, capacity> functions_{};
  unsigned next_{0};
  Statistics statistics_{};
  std::array<std::string, utf8Capacity> utf8Results_;
  unsigned utf8Next_{0};
  unsigned utf8Misses_{0};
  struct HelperState {
    const void *function{nullptr};
    unsigned misses{0};
  };
  std::array<HelperState, capacity> helpers_{};

  HelperState *helperState(const void *function) {
    for (auto &helper : helpers_) {
      if (!helper.function)
        helper.function = function;
      if (helper.function == function)
        return &helper;
    }
    return nullptr;
  }

 public:
  explicit ScopedStringReuse(Runtime &runtime)
      : runtime_(runtime), previous_(runtime.activeStringReuse),
        rootGuard_(runtime, &roots_) {
    runtime.activeStringReuse = this;
  }
  ~ScopedStringReuse() {
    assert(runtime_.activeStringReuse == this && "String reuse scopes must be nested");
    runtime_.activeStringReuse = previous_;
  }
  ScopedStringReuse(const ScopedStringReuse &) = delete;
  ScopedStringReuse &operator=(const ScopedStringReuse &) = delete;

  Statistics statistics() const { return statistics_; }

  static bool consumeStringStorage(StringPrimitive *input, uint64_t &remaining) {
    // A buffered prefix can keep a concatenation buffer alive while subsequent
    // concatenations grow it. Its storage is not bounded at admission time.
    if (isBufferedStringPrimitive(input))
      return false;
    uint64_t bytes = uint64_t(input->getStringLength()) * sizeof(char16_t);
    if (input->isExternal()) {
      // Moved native strings may own much more capacity than their text needs.
      const auto allocated = input->getVT()->getMallocSize(input);
      if (allocated > bytes)
        bytes = allocated;
    }
    if (bytes > remaining)
      return false;
    remaining -= bytes;
    return true;
  }

  bool allowsUTF8Reuse(StringPrimitive *input) const {
    // ASCII conversion is already one copy. Bound retained UTF-16 keys and
    // native output bytes; unique-input workloads stop probing after two fills.
    const auto length = input->getStringLength();
    uint64_t remaining = maxUTF8InputStorageBytes;
    return !input->isASCII() && length >= 64 &&
        utf8Misses_ < 2 * utf8Capacity && consumeStringStorage(input, remaining);
  }

  const std::string *lookupUTF8(StringPrimitive *input) {
    assert(allowsUTF8Reuse(input));
    auto value = HermesValue::encodeStringValue(input);
    for (unsigned slot = 0; slot < utf8Capacity; ++slot) {
      // The keys are rooted, so identity remains valid across moving GC.
      // Equal but separately allocated strings may miss without scanning text.
      if (roots_.utf8Inputs[slot].getRaw() == value.getRaw()) {
        utf8Misses_ = 0;
        return &utf8Results_[slot];
      }
    }
    ++utf8Misses_;
    return nullptr;
  }

  void storeUTF8(StringPrimitive *input, const std::string &result) {
    assert(!input->isASCII() && input->getStringLength() <= 8192);
    assert(result.size() <= 3 * input->getStringLength());
    uint64_t remaining = maxUTF8InputStorageBytes;
    if (!consumeStringStorage(input, remaining))
      return;
    const auto slot = utf8Next_++ % utf8Capacity;
    utf8Results_[slot] = result;
    roots_.utf8Inputs[slot] = HermesValue::encodeStringValue(input);
  }

  bool hasScanNumericKeyProof(llvh::ArrayRef<Handle<JSObject>> prototypes) const {
    assert(prototypes.size() == 3);
    for (unsigned i = 0; i < prototypes.size(); ++i) {
      auto *clazz = prototypes[i]->getClass(runtime_);
      if (clazz->isDictionary() ||
          roots_.scanPrototypeShapes[i].getRaw() !=
              HermesValue::encodeObjectValue(clazz).getRaw())
        return false;
    }
    return true;
  }

  // Only non-dictionary shapes with no numeric names may be remembered. Method
  // values, prototype links and indexed storage still need per-call guards.
  void rememberScanNumericKeyProof(llvh::ArrayRef<Handle<JSObject>> prototypes) {
    assert(prototypes.size() == 3);
    for (unsigned i = 0; i < prototypes.size(); ++i) {
      auto *clazz = prototypes[i]->getClass(runtime_);
      assert(!clazz->isDictionary());
      roots_.scanPrototypeShapes[i] = HermesValue::encodeObjectValue(clazz);
    }
  }

  bool allowsReuse(const void *function) {
    auto *helper = helperState(function);
    // Allow two complete cache fills before deciding that this helper's inputs
    // do not repeat. The decision lasts only until the invocation scope ends.
    return helper && helper->misses < 2 * capacity;
  }

  HermesValue lookup(const void *function, llvh::ArrayRef<PinnedHermesValue> inputs) {
    assert(!inputs.empty() && inputs.size() <= maxInputs);
    auto *helper = helperState(function);
    assert(helper && helper->misses < 2 * capacity);
    for (unsigned slot = 0; slot < capacity; ++slot) {
      auto *values = &roots_.values[slot * stride];
      if (counts_[slot] != inputs.size() || functions_[slot] != function)
        continue;
      bool same = true;
      for (unsigned i = 0; i < inputs.size(); ++i) {
        assert(inputs[i].isString());
        if (!inputs[i].getString()->equals(values[i].getString())) {
          same = false;
          break;
        }
      }
      if (same) {
        helper->misses = 0;
        ++statistics_.hits;
        return values[stride - 1];
      }
    }
    ++helper->misses;
    ++statistics_.misses;
    return HermesValue::encodeUndefinedValue();
  }

  void store(const void *function, llvh::ArrayRef<PinnedHermesValue> inputs,
             HermesValue result) {
    assert(!inputs.empty() && inputs.size() <= maxInputs);
    // Array results are private, already validated snapshots. They must be
    // copied again before returning to JS; no caller may mutate a cache root.
    uint64_t remaining = maxEntryStorageBytes;
    if (result.isString()) {
      if (!consumeStringStorage(result.getString(), remaining))
        return;
    } else {
      auto *array = vmcast<JSArray>(result);
      const auto length = JSArray::getLength(array, runtime_);
      const uint64_t elementBytes = uint64_t(length) * sizeof(SmallHermesValue);
      if (elementBytes > remaining)
        return;
      remaining -= elementBytes;
      for (uint32_t i = 0; i < length; ++i) {
        auto value = array->at(runtime_, i);
        assert(value.isString());
        if (!consumeStringStorage(value.getString(runtime_), remaining))
          return;
      }
    }
    for (auto input : inputs) {
      assert(input.isString());
      if (!consumeStringStorage(input.getString(), remaining))
        return;
    }
    auto slot = next_++ % capacity;
    auto *values = &roots_.values[slot * stride];
    // Clear unused roots as well as the key count when replacing an entry.
    for (unsigned i = 0; i < stride; ++i)
      values[i] = HermesValue::encodeUndefinedValue();
    functions_[slot] = function;
    for (unsigned i = 0; i < inputs.size(); ++i) {
      assert(inputs[i].isString());
      values[i] = inputs[i];
    }
    values[stride - 1] = result;
    counts_[slot] = inputs.size();
    ++statistics_.stores;
  }
};

} // namespace vm
} // namespace hermes

#endif
