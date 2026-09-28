/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_BCGEN_SH_CBUNDLELAYOUT_H
#define HERMES_BCGEN_SH_CBUNDLELAYOUT_H

#include "hermes/BCGen/LiteralBufferBuilder.h"
#include "hermes/IR/IR.h"
#include "llvh/Support/JSON.h"

#include <array>

namespace hermes::sh {

/// Retained allocation hints, never authority to reuse generated code. Every
/// function and metadata entry is regenerated from the current lowered IR.
class CBundleFunctionLayout {
 public:
  enum CacheKind { Write, Read, PrivateName, ComputedRead, CacheKindCount };

  struct ShardGroup {
    uint32_t index;
    bool retained;
    std::vector<Function *> functions;
  };

 private:
  struct Range {
    uint32_t start;
    uint32_t count;
  };
  struct Slot {
    std::string key;
    Function *function{nullptr};
    uint32_t anchorWeight{1};
    std::array<std::vector<Range>, CacheKindCount> caches;
    std::array<size_t, CacheKindCount> usedRanges{};
  };
  std::vector<Slot> slots_;
  llvh::DenseMap<Function *, uint32_t> functionSlots_;
  std::array<uint32_t, CacheKindCount> cacheSizes_{};
  using ScopeSlots = std::vector<std::vector<uint64_t>>;
  std::vector<ScopeSlots> retainedScopes_;
  std::vector<ScopeSlots> currentScopes_;
  llvh::DenseMap<Variable *, uint32_t> variableSlots_;
  llvh::DenseMap<VariableScope *, uint32_t> scopeSizes_;
  std::vector<std::vector<uint32_t>> retainedShards_;
  std::vector<std::vector<uint32_t>> currentShards_;
  LiteralBufferBuilder::RetainedBuffers literalBuffers_;

 public:
  /// Validate a seed before any code is emitted, including disjoint cache
  /// ranges across all live and retired function slots.
  bool read(const llvh::json::Array &seed);

  bool readScopes(const llvh::json::Array &seed);

  bool readShards(const llvh::json::Array &seed);

  bool readLiterals(const llvh::json::Value &seed);

  LiteralBufferBuilder::RetainedBuffers &literalBuffers() {
    return literalBuffers_;
  }

  /// Assign slots before register allocation mutates the lowered functions.
  void assign(llvh::ArrayRef<Function *> functions);

  /// Match environments by their unchanged consumers, even when the function
  /// creating the environment changes. This does not alter IR/debug indices.
  void assignScopes(Module *module);

  /// Preserve cold packing and the relative order of surviving functions.
  /// New shapes join a nearby surviving group to retain native inlining
  /// opportunities; current body sizes still determine splits within a group.
  std::vector<ShardGroup> groupFunctions(
      llvh::ArrayRef<Function *> functions);

  void recordShard(uint32_t group, llvh::ArrayRef<Function *> functions);

  uint32_t getVariableSlot(Variable *variable) const {
    return variableSlots_.find(variable)->second;
  }

  uint32_t scopeSize(VariableScope *scope) const {
    return scopeSizes_.find(scope)->second;
  }

  uint32_t getSlot(Function *function) const {
    return functionSlots_.find(function)->second;
  }

  std::vector<Function *> functionsBySlot() const;

  /// Allocate one site, keeping a contiguous range for polymorphic reads.
  uint32_t allocateCache(Function *function, CacheKind kind, uint32_t count);

  uint32_t cacheSize(CacheKind kind) const {
    return cacheSizes_[kind];
  }

  /// Excess retired state resets the next build's hints, never current C.
  llvh::json::Array generate() const;

  llvh::json::Array generateScopes() const;

  llvh::json::Array generateShards() const;

  llvh::json::Value generateLiterals() const;
};

} // namespace hermes::sh

#endif
