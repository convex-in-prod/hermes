/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/IR/Analysis.h"
#include "hermes/IR/CFG.h"
#include "hermes/IR/IRBuilder.h"
#include "hermes/IR/Instrs.h"
#include "hermes/Optimizer/PassManager/Pass.h"
#include "hermes/Optimizer/Scalar/RecordUseAnalysis.h"
#include "hermes/Optimizer/Scalar/Utils.h"
#include "hermes/Support/Statistic.h"

#include "llvh/ADT/STLExtras.h"

STATISTIC(NumLateRecords, "Returned records materialized after initialization");
STATISTIC(NumLateDefinitions, "Generic own definitions replaced with slots");

namespace hermes {
namespace {

using Shape = llvh::SmallVector<unsigned, 8>;
using Shapes = llvh::SmallVector<Shape, 4>;

constexpr unsigned kMaxFields = 12;
constexpr unsigned kMaxShapes = 4;
constexpr unsigned kMaxBlocks = 256;
constexpr unsigned kMaxInstructions = 4096;
constexpr unsigned kMaxMaterializations = 4;

struct ShapeState {
  bool beforeAllocation = false;
  Shapes shapes;
};

static unsigned shapeMask(const Shape &shape) {
  unsigned mask = 0;
  for (auto field : shape)
    mask |= 1u << field;
  return mask;
}

/// Enumerate insertion order before mutating IR. Exceptional edges and cycles
/// require a richer state analysis; leave those functions unchanged here.
static bool findReturnShapes(
    AllocObjectLiteralInst *allocation,
    const llvh::DenseMap<Instruction *, unsigned> &definitions,
    llvh::DenseMap<ReturnInst *, Shapes> &returnShapes) {
  auto *function = allocation->getFunction();
  if (functionHasTryCatch(function))
    return false;
  auto reachable = postOrderAnalysis(function);
  if (reachable.size() > kMaxBlocks)
    return false;
  llvh::DenseMap<BasicBlock *, unsigned> indegrees;
  unsigned instructions = 0;
  for (auto *block : reachable) {
    instructions += block->getInstList().size();
    for (auto *successor : successors(block))
      ++indegrees[successor];
  }
  if (instructions > kMaxInstructions)
    return false;
  llvh::SmallVector<BasicBlock *, 32> ready;
  for (auto *block : reachable)
    if (!indegrees.lookup(block))
      ready.push_back(block);
  llvh::SmallVector<BasicBlock *, 32> order;
  while (!ready.empty()) {
    auto *block = ready.pop_back_val();
    order.push_back(block);
    for (auto *successor : successors(block))
      if (!--indegrees[successor])
        ready.push_back(successor);
  }
  if (order.size() != reachable.size())
    return false;

  Shape initial;
  for (unsigned i = 0; i < allocation->getKeyValuePairCount(); ++i)
    initial.push_back(i);
  llvh::DenseMap<BasicBlock *, ShapeState> incoming;
  incoming[&function->front()].beforeAllocation = true;
  unsigned materializations = 0;
  for (auto *block : order) {
    auto state = incoming[block];
    for (auto &inst : *block) {
      if (&inst == allocation) {
        state.beforeAllocation = false;
        state.shapes = {initial};
      }
      auto definition = definitions.find(&inst);
      if (definition != definitions.end()) {
        if (state.beforeAllocation || state.shapes.empty())
          return false;
        Shapes updated;
        for (auto shape : state.shapes) {
          if (!llvh::is_contained(shape, definition->second))
            shape.push_back(definition->second);
          if (!llvh::is_contained(updated, shape))
            updated.push_back(std::move(shape));
        }
        state.shapes = std::move(updated);
      }
      if (auto *ret = llvh::dyn_cast<ReturnInst>(&inst)) {
        if (ret->getValue() != allocation)
          continue;
        if (state.beforeAllocation || state.shapes.empty())
          return false;
        // A presence mask cannot distinguish two insertion orders for the
        // same keys. Such joins need an explicit ordered-shape discriminator.
        for (unsigned i = 0; i < state.shapes.size(); ++i)
          for (unsigned j = 0; j < i; ++j)
            if (shapeMask(state.shapes[i]) == shapeMask(state.shapes[j]))
              return false;
        materializations += state.shapes.size();
        if (materializations > kMaxMaterializations)
          return false;
        returnShapes[ret] = state.shapes;
      }
    }
    for (auto *successor : successors(block)) {
      auto &next = incoming[successor];
      next.beforeAllocation |= state.beforeAllocation;
      for (const auto &shape : state.shapes) {
        if (!llvh::is_contained(next.shapes, shape))
          next.shapes.push_back(shape);
        if (next.shapes.size() > kMaxShapes)
          return false;
      }
    }
  }
  return !returnShapes.empty();
}

static bool materializeAtReturns(AllocObjectLiteralInst *allocation) {
  auto *parent = allocation->getParentObject();
  if (!llvh::isa<EmptySentinel>(parent) && !llvh::isa<LiteralNull>(parent))
    return false;
  unsigned genericDefinitions = 0;
  for (auto *user : allocation->getUsers())
    genericDefinitions += llvh::isa<DefineOwnPropertyInst>(user);
  // Repaying the discriminator and additional live fields needs more than a
  // single generic definition. Straight-line literals are already merged.
  if (genericDefinitions < 2)
    return false;
  RecordUseAnalysis analysis;
  auto construction = analysis.describeReturnedConstruction(allocation);
  if (construction.boundary || construction.fields.size() > kMaxFields)
    return false;
  llvh::DenseMap<Instruction *, unsigned> definitions;
  for (unsigned i = 0; i < construction.fields.size(); ++i)
    for (auto *definition : construction.fields[i].definitions)
      definitions[definition] = i;
  llvh::DenseMap<ReturnInst *, Shapes> returnShapes;
  if (!findReturnShapes(allocation, definitions, returnShapes) ||
      returnShapes.size() != construction.returns.size())
    return false;

  auto *function = allocation->getFunction();
  IRBuilder builder(function);
  builder.setInsertionPoint(allocation);
  builder.setLocation(allocation->getLocation());
  llvh::SmallVector<AllocStackInst *, 8> fields;
  for (const auto &field : construction.fields) {
    auto *slot = builder.createAllocStackInst(
        field.key->getValue(), Type::createAnyType());
    fields.push_back(slot);
    // Traced storage always contains a valid value before calls or GC. Shape
    // state, not this internal undefined initializer, controls field presence.
    builder.createStoreStackInst(
        field.initialValue ? field.initialValue : builder.getLiteralUndefined(),
        slot);
  }
  unsigned presentInAnyShape = 0;
  unsigned presentInEveryShape = (1u << construction.fields.size()) - 1;
  bool needsMask = false;
  for (const auto &entry : returnShapes) {
    if (entry.second.size() == 1)
      continue;
    needsMask = true;
    for (const auto &shape : entry.second) {
      presentInAnyShape |= shapeMask(shape);
      presentInEveryShape &= shapeMask(shape);
    }
  }
  // Only presence differences distinguish the final layouts. Updating bits
  // for unconditional fields would extend the discriminator's dependency chain
  // across their initializer calls without contributing to shape selection.
  unsigned discriminatingFields = presentInAnyShape & ~presentInEveryShape;
  AllocStackInst *mask = nullptr;
  if (needsMask) {
    assert(discriminatingFields && "distinct shapes have identical presence");
    mask = builder.createAllocStackInst("[record presence]", Type::createNumber());
    builder.createStoreStackInst(builder.getLiteralNumber(0), mask);
  }

  for (const auto &entry : definitions) {
    auto *definition = entry.first;
    unsigned field = entry.second;
    Value *value = llvh::isa<PrStoreInst>(definition)
        ? llvh::cast<PrStoreInst>(definition)->getStoredValue()
        : llvh::cast<DefineOwnPropertyInst>(definition)->getStoredValue();
    builder.setInsertionPoint(definition);
    builder.setLocation(definition->getLocation());
    builder.createStoreStackInst(value, fields[field]);
    if (mask && (discriminatingFields & (1u << field))) {
      auto *updated = builder.createBinaryOperatorInst(
          builder.createLoadStackInst(mask),
          builder.getLiteralNumber(1u << field),
          ValueKind::BinaryOrInstKind);
      builder.createStoreStackInst(updated, mask);
    }
    definition->eraseFromParent();
  }

  auto materialize = [&](const Shape &shape) {
    AllocObjectLiteralInst::ObjectPropertyMap properties;
    for (auto field : shape)
      properties.push_back(
          {construction.fields[field].key, builder.getLiteralNull()});
    auto *record = builder.createAllocObjectLiteralInst(properties, parent);
    for (unsigned slot = 0; slot < shape.size(); ++slot) {
      unsigned field = shape[slot];
      builder.createPrStoreInst(
          builder.createLoadStackInst(fields[field]),
          record,
          slot,
          construction.fields[field].key,
          false);
    }
    return record;
  };
  for (auto *ret : construction.returns) {
    const auto &shapes = returnShapes.at(ret);
    builder.setInsertionPoint(ret);
    builder.setLocation(ret->getLocation());
    if (shapes.size() == 1) {
      ret->setOperand(materialize(shapes.front()), 0);
      continue;
    }
    auto *currentMask = builder.createLoadStackInst(mask);
    auto *block = ret->getParent();
    ret->eraseFromParent();
    builder.setInsertionBlock(block);
    for (unsigned i = 0; i + 1 < shapes.size(); ++i) {
      auto *selected = builder.createBasicBlock(function);
      auto *next = builder.createBasicBlock(function);
      auto *matches = builder.createBinaryOperatorInst(
          currentMask,
          builder.getLiteralNumber(shapeMask(shapes[i]) & discriminatingFields),
          ValueKind::BinaryStrictlyEqualInstKind);
      builder.createCondBranchInst(matches, selected, next);
      builder.setInsertionBlock(selected);
      builder.createReturnInst(materialize(shapes[i]));
      builder.setInsertionBlock(next);
    }
    // Every possible incoming ordered shape was enumerated before rewriting.
    builder.createReturnInst(materialize(shapes.back()));
  }
  assert(!allocation->hasUsers() && "materialized record still has an alias");
  allocation->eraseFromParent();
  ++NumLateRecords;
  NumLateDefinitions += genericDefinitions;
  return true;
}

} // namespace

Pass *createObjectLateMaterialization() {
  class LateMaterialization final : public FunctionPass {
   public:
    LateMaterialization() : FunctionPass("ObjectLateMaterialization") {}
    bool runOnFunction(Function *function) override {
      llvh::SmallVector<AllocObjectLiteralInst *, 8> candidates;
      for (auto &block : *function)
        for (auto &inst : block)
          if (auto *allocation = llvh::dyn_cast<AllocObjectLiteralInst>(&inst))
            candidates.push_back(allocation);
      unsigned changed = 0;
      for (auto *allocation : candidates) {
        changed += materializeAtReturns(allocation);
        if (changed == 4)
          break;
      }
      return changed != 0;
    }
  };
  return new LateMaterialization();
}

} // namespace hermes
