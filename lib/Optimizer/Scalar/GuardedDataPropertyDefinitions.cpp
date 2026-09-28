/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/AST/NativeContext.h"
#include "hermes/IR/Analysis.h"
#include "hermes/IR/CFG.h"
#include "hermes/IR/IRBuilder.h"
#include "hermes/Optimizer/PassManager/Pass.h"
#include "hermes/Optimizer/Scalar/Utils.h"
#include "hermes/Support/Statistic.h"

#define DEBUG_TYPE "guarded-data-property-definitions"

STATISTIC(NumGuardedDefinitions, "Data-property construction sequences fused");

namespace hermes {
namespace {

struct Definition {
  BinaryOperatorInst *has;
  LoadFrameInst *callee;
  StorePropertyStrictInst *store;
  BasicBlock *join;
};

/// Recognize a complete, unused-result definition, not a named helper:
/// if (key in receiver) define(receiver, key, {value, ...true flags});
/// else receiver[key] = value;
/// Strict assignment is required: adding to a nonextensible object must throw
/// on either path. The runtime guard proves that `in` has no observable effects.
static OptValue<Definition> findDefinition(
    BinaryOperatorInst *has,
    const DominanceInfo &dominance) {
  if (has->getKind() != ValueKind::BinaryInInstKind || !has->hasOneUser() ||
      !llvh::isa<LiteralString>(has->getLeftHandSide()))
    return llvh::None;
  auto *branch = llvh::dyn_cast<CondBranchInst>(*has->getUsers().begin());
  if (!branch || branch->getParent() != has->getParent() ||
      branch->getCondition() != has ||
      std::next(has->getIterator()) != branch->getIterator())
    return llvh::None;
  auto *present = branch->getTrueDest();
  auto *absent = branch->getFalseDest();
  if (present == absent || pred_count(present) != 1 || pred_count(absent) != 1 ||
      present->size() > 5 || absent->size() != 2)
    return llvh::None;
  auto *presentEnd = llvh::dyn_cast<BranchInst>(present->getTerminator());
  auto *absentEnd = llvh::dyn_cast<BranchInst>(absent->getTerminator());
  auto *store = llvh::dyn_cast<StorePropertyStrictInst>(&absent->front());
  if (!presentEnd || !absentEnd || !store ||
      presentEnd->getBranchDest() != absentEnd->getBranchDest() ||
      store->getObject() != has->getRightHandSide() ||
      store->getProperty() != has->getLeftHandSide())
    return llvh::None;
  auto *join = presentEnd->getBranchDest();
  // A result phi may distinguish the call's receiver from the assigned value.
  // Do not invent that result or bypass other edge-dependent values.
  if (llvh::isa<PhiInst>(&join->front()))
    return llvh::None;

  LoadFrameInst *callee = nullptr;
  AllocObjectLiteralInst *descriptor = nullptr;
  CallInst *call = nullptr;
  PrStoreInst *valueStore = nullptr;
  for (auto &inst : *present) {
    if (auto *load = llvh::dyn_cast<LoadFrameInst>(&inst)) {
      if (callee || descriptor || call)
        return llvh::None;
      callee = load;
    } else if (auto *alloc = llvh::dyn_cast<AllocObjectLiteralInst>(&inst)) {
      if (descriptor || call)
        return llvh::None;
      descriptor = alloc;
    } else if (auto *slot = llvh::dyn_cast<PrStoreInst>(&inst)) {
      if (!descriptor || valueStore || call || slot->getObject() != descriptor)
        return llvh::None;
      valueStore = slot;
    } else if (auto *invoke = llvh::dyn_cast<CallInst>(&inst)) {
      if (!descriptor || call)
        return llvh::None;
      call = invoke;
    } else if (&inst != presentEnd) {
      return llvh::None;
    }
  }
  if (!callee || !descriptor || !call || call->hasUsers() ||
      call->getNumArguments() != 4 || call->getCallee() != callee ||
      !call->getNewTarget()->getType().isUndefinedType() ||
      call->getArgument(1) != store->getObject() ||
      call->getArgument(2) != store->getProperty() ||
      call->getArgument(3) != descriptor ||
      descriptor->getKeyValuePairCount() != 4 ||
      !llvh::isa<EmptySentinel>(descriptor->getParentObject()))
    return llvh::None;
  if (valueStore &&
      (valueStore->getPropIndex() >= 4 ||
       valueStore->getPropName() != descriptor->getKey(valueStore->getPropIndex())))
    return llvh::None;
  auto *scope = llvh::dyn_cast<Instruction>(callee->getScope());
  if (!scope || !dominance.properlyDominates(scope, has))
    return llvh::None;
  if (auto *value = llvh::dyn_cast<Instruction>(store->getStoredValue()))
    if (!dominance.properlyDominates(value, has))
      return llvh::None;
  unsigned flags = 0;
  Value *storedValue = nullptr;
  for (unsigned i = 0; i < 4; ++i) {
    auto *key = llvh::dyn_cast<LiteralString>(descriptor->getKey(i));
    if (!key)
      return llvh::None;
    const auto name = key->getValue().str();
    unsigned bit;
    if (name == "value") {
      bit = 8;
      storedValue = descriptor->getValue(i);
      if (valueStore && valueStore->getPropIndex() == i)
        storedValue = valueStore->getStoredValue();
    } else {
      bit = name == "enumerable" ? 1 : name == "configurable" ? 2
          : name == "writable" ? 4 : 0;
      auto *value = llvh::dyn_cast<LiteralBool>(descriptor->getValue(i));
      if (!bit || !value || !value->getValue() ||
          (valueStore && valueStore->getPropIndex() == i))
        return llvh::None;
    }
    if (flags & bit)
      return llvh::None;
    flags |= bit;
  }
  if (flags != 15 || storedValue != store->getStoredValue())
    return llvh::None;
  for (auto *user : descriptor->getUsers())
    if (user != call && user != valueStore)
      return llvh::None;
  return Definition{has, callee, store, join};
}

static bool run(Function *function) {
  const auto &settings = function->getContext().getOptimizationSettings();
  // The descriptor's absent `get`/`set` fields must stay absent on its prototype.
  if (!settings.staticHermesInlining ||
      !settings.guardedDataPropertyDefinitions ||
      !settings.staticObjectPrototype || functionHasTryCatch(function))
    return false;
  DominanceInfo dominance(function);
  llvh::SmallVector<Definition, 8> definitions;
  for (auto &block : *function)
    for (auto &inst : block)
      if (auto *has = llvh::dyn_cast<BinaryOperatorInst>(&inst)) {
        auto definition = findDefinition(has, dominance);
        if (definition && definitions.size() < 16)
          definitions.push_back(*definition);
      }
  if (definitions.empty())
    return false;
  IRBuilder builder(function);
  auto &context = function->getContext();
  auto &native = context.getNativeContext();
  auto *signature = native.getSignature(
      NativeCType::c_bool,
      {NativeCType::ptr, NativeCType::c_hermes_value, NativeCType::c_hermes_value});
  auto *probe = native.getExtern(
      context.getStringTable().getString("_sh_ljs_can_define_data_property"),
      signature, {}, true, nullptr);
  for (auto definition : definitions) {
    auto *before = definition.has->getParent();
    auto *slow = splitBasicBlock(before, definition.has->getIterator());
    auto *fast = builder.createBasicBlock(function);
    builder.setInsertionBlock(before);
    builder.setLocation(definition.has->getLocation());
    // A speculative frame read has no effects. Keep the original read in the
    // fallback: an observable `in` trap may replace the captured function.
    auto *callee = builder.createLoadFrameInst(
        definition.callee->getScope(), definition.callee->getLoadVariable());
    auto *guard = builder.createNativeCallInst(
        Type::createNumber(), builder.getLiteralNativeExtern(probe), signature,
        {builder.createGetNativeRuntimeInst(), callee,
         definition.store->getObject()});
    builder.createCondBranchInst(guard, fast, slow);
    builder.setInsertionBlock(fast);
    builder.createDefineOwnPropertyInst(
        definition.store->getStoredValue(), definition.store->getObject(),
        definition.store->getProperty(), IRBuilder::PropEnumerable::Yes);
    builder.createBranchInst(definition.join);
    ++NumGuardedDefinitions;
  }
  return true;
}

} // namespace

Pass *createGuardedDataPropertyDefinitions() {
  class ThisPass final : public FunctionPass {
   public:
    ThisPass() : FunctionPass("GuardedDataPropertyDefinitions") {}
    bool runOnFunction(Function *function) override {
      return run(function);
    }
  };
  return new ThisPass();
}

} // namespace hermes
