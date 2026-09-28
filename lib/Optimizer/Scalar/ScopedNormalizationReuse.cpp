/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/AST/NativeContext.h"
#include "hermes/IR/Analysis.h"
#include "hermes/IR/IRBuilder.h"
#include "hermes/Optimizer/PassManager/Pass.h"
#include "hermes/Optimizer/Scalar/Utils.h"
#include "hermes/Support/Statistic.h"

#include "llvh/ADT/DenseMap.h"
#include "llvh/ADT/SmallPtrSet.h"
#include "llvh/ADT/STLExtras.h"

#include <functional>

#define DEBUG_TYPE "scoped-normalization-reuse"

STATISTIC(NumNormalizationSites, "String helpers using invocation-owned reuse");
STATISTIC(NumFreshArrayJoinFastPaths, "Fresh string-array joins with concat fast paths");
STATISTIC(NumStringScanSites, "Primitive string scans with fresh-result reuse");

namespace hermes {
namespace {

enum class Kind { Unknown, String, Number, Boolean, Record, Array, Method };

struct Summary {
  llvh::SmallVector<LiteralString *, 8> fields{};
  llvh::SmallVector<Variable *, 4> captures{};
  llvh::SmallVector<CallInst *, 4> joins{};
};

/// Successful paths may inspect one primitive string and build local values.
/// A monotone union analysis handles numeric induction through loop phis; the
/// final validation rejects unresolved operands and effects rather than using
/// the function's inferred output type as an effect proof.
static bool summarizeStringScan(Function *F) {
  if (F->getJSDynamicParams().size() != 2 || functionHasTryCatch(F))
    return false;
  constexpr unsigned String = 1, Number = 2, Boolean = 4, Undefined = 8, Null = 16;
  constexpr unsigned Array = 32, Record = 64, Method = 128;
  constexpr unsigned Primitive = String | Number | Boolean | Undefined | Null;
  constexpr unsigned Payload = Primitive | Array | Record;
  llvh::SmallPtrSet<BasicBlock *, 32> returning;
  llvh::SmallVector<BasicBlock *, 32> pending;
  unsigned count = 0;
  for (auto &block : *F) {
    count += block.size();
    if (count > 512)
      return false;
    if (llvh::isa<ReturnInst>(block.getTerminator()))
      pending.push_back(&block);
  }
  while (!pending.empty()) {
    auto *block = pending.pop_back_val();
    if (returning.insert(block).second)
      for (auto *pred : predecessors(block))
        pending.push_back(pred);
  }
  DominanceInfo dominance(F);
  LoopAnalysis loops(F, dominance);
  bool hasLoop = false;
  for (auto *block : returning)
    hasLoop |= loops.isBlockInLoop(block);
  if (!hasLoop)
    return false;

  llvh::DenseMap<Value *, unsigned> kinds;
  auto kind = [&](Value *value) -> unsigned {
    if (llvh::isa<LiteralString>(value)) return String;
    if (llvh::isa<LiteralNumber>(value)) return Number;
    if (llvh::isa<LiteralBool>(value)) return Boolean;
    if (llvh::isa<LiteralUndefined>(value)) return Undefined;
    if (llvh::isa<LiteralNull>(value)) return Null;
    return kinds.lookup(value);
  };
  auto within = [&](Value *value, unsigned allowed) {
    auto bits = kind(value);
    return bits != 0 && !(bits & ~allowed);
  };
  auto infer = [&](Instruction *inst) -> unsigned {
    if (auto *param = llvh::dyn_cast<LoadParamInst>(inst))
      return param->getParam() == F->getJSDynamicParam(1) ? String : 0;
    if (auto *phi = llvh::dyn_cast<PhiInst>(inst)) {
      unsigned result = 0;
      for (unsigned i = 0; i < phi->getNumEntries(); ++i)
        result |= kind(phi->getEntry(i).first);
      return result;
    }
    if (auto *array = llvh::dyn_cast<AllocArrayInst>(inst)) {
      for (unsigned i = 0; i < array->getElementCount(); ++i)
        if (!within(array->getArrayElement(i), Payload))
          return 0;
      return Array;
    }
    if (auto *record = llvh::dyn_cast<AllocObjectLiteralInst>(inst)) {
      if (!llvh::isa<EmptySentinel>(record->getParentObject()) &&
          !llvh::isa<LiteralNull>(record->getParentObject()))
        return 0;
      for (unsigned i = 0; i < record->getKeyValuePairCount(); ++i)
        if (!within(record->getValue(i), Payload))
          return 0;
      return Record;
    }
    if (auto *load = llvh::dyn_cast<LoadPropertyInst>(inst)) {
      auto receiver = kind(load->getObject());
      if (auto *key = llvh::dyn_cast<LiteralString>(load->getProperty())) {
        auto name = key->getValue().str();
        if ((receiver == String || receiver == Array) && name == "length")
          return Number;
        if ((receiver == String && (name == "slice" || name == "trim")) ||
            (receiver == Array && name == "push"))
          return Method;
      }
      if (receiver == String && kind(load->getProperty()) == Number)
        return String | Undefined;
      return 0;
    }
    if (auto *call = llvh::dyn_cast<CallInst>(inst)) {
      auto *load = llvh::dyn_cast<LoadPropertyInst>(call->getCallee());
      if (!load || kind(load) != Method || call->getThis() != load->getObject() ||
          !call->getNewTarget()->getType().isUndefinedType())
        return 0;
      auto name = llvh::cast<LiteralString>(load->getProperty())->getValue().str();
      if (name == "trim")
        return call->getNumArguments() == 1 ? String : 0;
      if (name == "slice") {
        if (call->getNumArguments() < 2 || call->getNumArguments() > 3)
          return 0;
        for (unsigned i = 1; i < call->getNumArguments(); ++i)
          if (!within(call->getArgument(i), Number | Undefined))
            return 0;
        return String;
      }
      if (name == "push" && call->getNumArguments() == 2 &&
          within(call->getArgument(1), Payload))
        return Number;
      return 0;
    }
    switch (inst->getKind()) {
      case ValueKind::BinaryStrictlyEqualInstKind:
      case ValueKind::BinaryStrictlyNotEqualInstKind:
        return within(inst->getOperand(0), Primitive) &&
                within(inst->getOperand(1), Primitive) ? Boolean : 0;
      case ValueKind::UnaryBangInstKind:
        return within(inst->getOperand(0), Payload) ? Boolean : 0;
      case ValueKind::BinaryLessThanInstKind:
      case ValueKind::BinaryLessThanOrEqualInstKind:
      case ValueKind::BinaryGreaterThanInstKind:
      case ValueKind::BinaryGreaterThanOrEqualInstKind:
      case ValueKind::FLessThanInstKind:
      case ValueKind::FLessThanOrEqualInstKind:
      case ValueKind::FGreaterThanInstKind:
      case ValueKind::FGreaterThanOrEqualInstKind:
        return kind(inst->getOperand(0)) == Number &&
                kind(inst->getOperand(1)) == Number ? Boolean : 0;
      case ValueKind::BinaryAddInstKind:
      case ValueKind::BinarySubtractInstKind:
      case ValueKind::FAddInstKind:
      case ValueKind::FSubtractInstKind:
        return kind(inst->getOperand(0)) == Number &&
                kind(inst->getOperand(1)) == Number ? Number : 0;
      case ValueKind::UnaryIncInstKind:
      case ValueKind::UnaryDecInstKind:
      case ValueKind::FNegateKind:
        return kind(inst->getOperand(0)) == Number ? Number : 0;
      default:
        return 0;
    }
  };
  bool stable = false;
  for (unsigned round = 0; round < 32; ++round) {
    bool changed = false;
    for (auto &block : *F) {
      if (!returning.count(&block)) continue;
      for (auto &inst : block) {
        auto next = kinds.lookup(&inst) | infer(&inst);
        if (next != kinds.lookup(&inst)) {
          kinds[&inst] = next;
          changed = true;
        }
      }
    }
    if (!changed) { stable = true; break; }
  }
  if (!stable)
    return false;
  unsigned operations = 0;
  for (auto &block : *F) {
    if (!returning.count(&block)) continue;
    for (auto &inst : block) {
      if (auto *ret = llvh::dyn_cast<ReturnInst>(&inst)) {
        if (!within(ret->getValue(), String | Array)) return false;
      } else if (auto *branch = llvh::dyn_cast<CondBranchInst>(&inst)) {
        if (!within(branch->getCondition(), Payload)) return false;
      } else if (llvh::isa<BranchInst>(&inst) ||
                 llvh::isa<GetParentScopeInst>(&inst)) {
        // Hoisting may leave environment handles used only by throwing paths.
        // They stay unclassified: reads through them are still rejected here.
        continue;
      } else if (auto *store = llvh::dyn_cast<DefineOwnInDenseArrayInst>(&inst)) {
        if (kind(store->getArray()) != Array ||
            !within(store->getStoredValue(), Payload)) return false;
      } else if (auto *store = llvh::dyn_cast<PrStoreInst>(&inst)) {
        // Literal construction reserves slots before evaluating nested values.
        // A direct slot write into a proven local record has no setter lookup.
        if (kind(store->getObject()) != Record ||
            !within(store->getStoredValue(), Payload)) return false;
      } else {
        if (!kind(&inst) || infer(&inst) != kind(&inst)) return false;
        if (auto *phi = llvh::dyn_cast<PhiInst>(&inst))
          for (unsigned i = 0; i < phi->getNumEntries(); ++i)
            if (!kind(phi->getEntry(i).first)) return false;
      }
      operations += llvh::isa<CallInst>(&inst);
    }
  }
  return operations >= 2;
}

static void rewriteStringScan(Function *F) {
  F->setNoInline();
  IRBuilder builder(F);
  auto &context = F->getContext();
  auto &native = context.getNativeContext();
  auto invoke = [&](llvh::StringRef name, NativeCType resultType, Type irType,
                    llvh::ArrayRef<Value *> arguments,
                    llvh::ArrayRef<NativeCType> types) {
    auto *signature = native.getSignature(resultType, types);
    auto *external = native.getExtern(
        context.getStringTable().getString(name), signature, {}, true, nullptr);
    return builder.createNativeCallInst(
        irType, builder.getLiteralNativeExtern(external), signature, arguments);
  };
  llvh::SmallVector<ReturnInst *, 8> returns;
  for (auto &block : *F)
    if (auto *ret = llvh::dyn_cast<ReturnInst>(block.getTerminator()))
      returns.push_back(ret);
  auto *entry = &F->front();
  auto *body = splitBasicBlock(entry, entry->begin());
  auto *guard = builder.createBasicBlock(F);
  auto *lookup = builder.createBasicBlock(F);
  auto *hit = builder.createBasicBlock(F);
  auto *miss = builder.createBasicBlock(F);
  builder.setInsertionBlock(entry);
  auto *ready = builder.createAllocStackInst("scan.reuse", Type::createBoolean());
  builder.createStoreStackInst(builder.getLiteralBool(false), ready);
  auto *input = builder.createLoadParamInst(F->getJSDynamicParam(1));
  auto *runtime = builder.createGetNativeRuntimeInst();
  builder.createCondBranchInst(builder.createTypeOfIsInst(input,
      builder.getLiteralTypeOfIsTypes(TypeOfIsTypes{}.withString(true))), guard, body);
  builder.setInsertionBlock(guard);
  auto *safe = invoke("_sh_ljs_has_scan_reuse_builtins", NativeCType::c_bool,
      Type::createNumber(), {runtime}, {NativeCType::ptr});
  builder.createCondBranchInst(safe, lookup, body);
  builder.setInsertionBlock(lookup);
  auto *cached = invoke("_sh_ljs_scan_reuse_lookup", NativeCType::c_hermes_value,
      Type::createAnyType(), {runtime, input},
      {NativeCType::ptr, NativeCType::c_hermes_value});
  auto *found = builder.createTypeOfIsInst(cached,
      builder.getLiteralTypeOfIsTypes(TypeOfIsTypes{}.withString(true).withObject(true)));
  builder.createCondBranchInst(found, hit, miss);
  builder.setInsertionBlock(hit);
  builder.createReturnInst(cached);
  builder.setInsertionBlock(miss);
  builder.createStoreStackInst(builder.getLiteralBool(true), ready);
  builder.createBranchInst(body);
  for (auto *ret : returns) {
    auto *before = ret->getParent();
    auto *after = splitBasicBlock(before, ret->getIterator());
    auto *publish = builder.createBasicBlock(F);
    builder.setInsertionBlock(before);
    builder.createCondBranchInst(builder.createLoadStackInst(ready), publish, after);
    builder.setInsertionBlock(publish);
    invoke("_sh_ljs_scan_reuse_store", NativeCType::c_void, Type::createUndefined(),
        {runtime, input, ret->getValue()},
        {NativeCType::ptr, NativeCType::c_hermes_value, NativeCType::c_hermes_value});
    builder.createBranchInst(after);
  }
  ++NumStringScanSites;
}

/// Prove successful executions deterministic under primitive field/capture and
/// intrinsic-method guards. Throw-only paths remain untouched and failures are
/// never cached. The input record's identity is not a result dependency.
static bool summarize(Function *F, Summary &summary) {
  if (F->getJSDynamicParams().size() != 2 || functionHasTryCatch(F))
    return false;
  llvh::SmallPtrSet<BasicBlock *, 32> returning;
  llvh::SmallVector<BasicBlock *, 32> pending;
  unsigned instructions = 0;
  for (auto &block : *F) {
    instructions += block.size();
    if (instructions > 384)
      return false;
    if (llvh::isa<ReturnInst>(block.getTerminator()))
      pending.push_back(&block);
  }
  while (!pending.empty()) {
    auto *block = pending.pop_back_val();
    if (returning.insert(block).second)
      for (auto *pred : predecessors(block))
        pending.push_back(pred);
  }
  if (returning.empty())
    return false;
  DominanceInfo dominance(F);
  LoopAnalysis loops(F, dominance);
  for (auto *block : returning)
    if (loops.isBlockInLoop(block))
      return false;

  llvh::DenseMap<Value *, Kind> kinds;
  unsigned operations = 0;
  std::function<Kind(Value *)> classify = [&](Value *value) -> Kind {
    if (llvh::isa<LiteralString>(value))
      return Kind::String;
    if (llvh::isa<LiteralNumber>(value))
      return Kind::Number;
    if (llvh::isa<LiteralBool>(value))
      return Kind::Boolean;
    auto existing = kinds.find(value);
    if (existing != kinds.end())
      return existing->second;
    kinds[value] = Kind::Unknown;
    const Kind result = [&]() -> Kind {
      if (auto *param = llvh::dyn_cast<LoadParamInst>(value))
        return param->getParam() == F->getJSDynamicParam(1) ? Kind::Record
                                                         : Kind::Unknown;
      if (auto *load = llvh::dyn_cast<LoadFrameInst>(value)) {
        auto *parent = llvh::dyn_cast<GetParentScopeInst>(load->getScope());
        auto *variable = load->getLoadVariable();
        if (!parent || parent->getVariableScope() != variable->getParent() ||
            (!summary.captures.empty() &&
             summary.captures.front()->getParent() != variable->getParent()))
          return Kind::Unknown;
        if (llvh::find(summary.captures, variable) == summary.captures.end())
          summary.captures.push_back(variable);
        return Kind::String;
      }
      if (auto *load = llvh::dyn_cast<LoadPropertyInst>(value)) {
        auto *key = llvh::dyn_cast<LiteralString>(load->getProperty());
        if (!key)
          return Kind::Unknown;
        auto receiver = classify(load->getObject());
        const auto name = key->getValue().str();
        if (receiver == Kind::Record &&
            !name.empty() && !(name.front() >= '0' && name.front() <= '9') &&
            !isStandardObjectPrototypeKey(key->getValue())) {
          if (llvh::find(summary.fields, key) == summary.fields.end())
            summary.fields.push_back(key);
          return Kind::String;
        }
        if (receiver == Kind::String && name == "length")
          return Kind::Number;
        if ((receiver == Kind::String &&
             (name == "trim" || name == "includes")) ||
            (receiver == Kind::Array && name == "join"))
          return Kind::Method;
        return Kind::Unknown;
      }
      if (auto *array = llvh::dyn_cast<AllocArrayInst>(value)) {
        if (array->getElementCount() > 8)
          return Kind::Unknown;
        for (unsigned i = 0; i < array->getElementCount(); ++i)
          if (classify(array->getArrayElement(i)) != Kind::String)
            return Kind::Unknown;
        return Kind::Array;
      }
      if (auto *call = llvh::dyn_cast<CallInst>(value)) {
        auto *load = llvh::dyn_cast<LoadPropertyInst>(call->getCallee());
        if (!load || classify(load) != Kind::Method ||
            call->getThis() != load->getObject() ||
            !call->getNewTarget()->getType().isUndefinedType())
          return Kind::Unknown;
        auto name = llvh::cast<LiteralString>(load->getProperty())->getValue().str();
        if (call->getNumArguments() != (name == "trim" ? 1 : 2) ||
            (name != "trim" && classify(call->getArgument(1)) != Kind::String))
          return Kind::Unknown;
        if (name == "join") {
          auto *array = llvh::cast<AllocArrayInst>(load->getObject());
          // Every element must be an own string at join; holes could read an
          // inherited getter. Reject escapes, duplicate stores and late stores.
          unsigned mask = (1u << array->getElementCount()) - 1;
          for (auto *user : array->getUsers()) {
            if (user == load || user == call)
              continue;
            auto *store = llvh::dyn_cast<DefineOwnInDenseArrayInst>(user);
            if (!store || store->getArray() != array ||
                !store->getArrayIndex()->isUInt32Representible() ||
                store->getArrayIndex()->asUInt32() >= 8 ||
                classify(store->getStoredValue()) != Kind::String ||
                !dominance.properlyDominates(store, call))
              return Kind::Unknown;
            unsigned bit = 1u << store->getArrayIndex()->asUInt32();
            if (mask & bit)
              return Kind::Unknown;
            mask |= bit;
          }
          if (!mask || (mask & (mask + 1)))
            return Kind::Unknown;
          summary.joins.push_back(call);
        }
        ++operations;
        return name == "includes" ? Kind::Boolean : Kind::String;
      }
      if (auto *binary = llvh::dyn_cast<BinaryOperatorInst>(value)) {
        auto left = classify(binary->getLeftHandSide());
        auto right = classify(binary->getRightHandSide());
        auto primitive = [](Kind kind) {
          return kind == Kind::String || kind == Kind::Number ||
              kind == Kind::Boolean;
        };
        if (primitive(left) && primitive(right) &&
            (binary->getKind() == ValueKind::BinaryStrictlyEqualInstKind ||
             binary->getKind() == ValueKind::BinaryStrictlyNotEqualInstKind))
          return Kind::Boolean;
      }
      return Kind::Unknown;
    }();
    kinds[value] = result;
    return result;
  };
  for (auto &block : *F) {
    if (!returning.count(&block))
      continue;
    for (auto &inst : block) {
      if (auto *ret = llvh::dyn_cast<ReturnInst>(&inst)) {
        if (classify(ret->getValue()) != Kind::String)
          return false;
      } else if (auto *branch = llvh::dyn_cast<CondBranchInst>(&inst)) {
        auto kind = classify(branch->getCondition());
        if (kind != Kind::Boolean && kind != Kind::String && kind != Kind::Number)
          return false;
      } else if (auto *store = llvh::dyn_cast<DefineOwnInDenseArrayInst>(&inst)) {
        if (classify(store->getArray()) != Kind::Array ||
            classify(store->getStoredValue()) != Kind::String)
          return false;
      } else if (!llvh::isa<BranchInst>(&inst) &&
                 !llvh::isa<GetParentScopeInst>(&inst) &&
                 classify(&inst) == Kind::Unknown) {
        return false;
      }
    }
  }
  return operations >= 2 && !summary.fields.empty() && summary.fields.size() <= 8 &&
      summary.captures.size() <= 4;
}

static void rewrite(Function *F, const Summary &summary) {
  // Native function metadata identifies this proof in the active scope. A later
  // inlining pass must not merge independent proofs into one caller identity.
  F->setNoInline();
  IRBuilder builder(F);

  // The array cannot escape except through its method lookup. Guard that lookup
  // before it runs: a getter may mutate the array and still return the intrinsic.
  // Keep allocation and lookup for the fallback and check actual string values
  // at the call, since the summary's kinds are conditional reuse proofs.
  for (auto *call : summary.joins) {
    auto *load = llvh::dyn_cast<BaseLoadPropertyInst>(call->getCallee());
    auto *array = load ? llvh::dyn_cast<AllocArrayInst>(load->getObject())
                       : nullptr;
    if (!load || !array || call->getNumArguments() != 2 ||
        !call->getNewTarget()->getType().isUndefinedType())
      continue;

    const unsigned size = array->getSizeHint()->asUInt32();
    if (size == 0 || size > 8)
      continue;
    llvh::SmallVector<Value *, 8> elements(size, nullptr);
    bool complete = true;
    if (array->getElementCount() == size) {
      for (unsigned i = 0; i < size; ++i)
        elements[i] = array->getArrayElement(i);
    }
    for (auto *user : array->getUsers()) {
      auto *store = llvh::dyn_cast<DefineOwnInDenseArrayInst>(user);
      if (!store)
        continue;
      const unsigned index = store->getArrayIndex()->asUInt32();
      if (index >= size || elements[index] != nullptr) {
        complete = false;
        break;
      }
      elements[index] = store->getStoredValue();
    }
    for (auto *element : elements)
      if (!element) {
        complete = false;
        break;
      }
    if (!complete)
      continue;

    builder.setLocation(load->getLocation());
    auto &context = F->getContext();
    auto &native = context.getNativeContext();
    auto *signature = native.getSignature(NativeCType::c_bool, {NativeCType::ptr});
    auto *external = native.getExtern(
        context.getStringTable().getString("_sh_ljs_has_array_join"),
        signature, {}, true, nullptr);
    llvh::SmallVector<Value *, 16> operands;
    operands.reserve(size * 2 - 1);
    for (unsigned i = 0; i < size; ++i) {
      if (i)
        operands.push_back(call->getArgument(1));
      operands.push_back(elements[i]);
    }

    auto *before = call->getParent();
    assert(load->getParent() == before && "join lookup must share its call block");
    auto *afterLoad = splitBasicBlock(before, load->getIterator());
    auto *safePath = builder.createBasicBlock(F);
    auto *unsafePath = builder.createBasicBlock(F);
    builder.setInsertionBlock(before);
    auto *safeLookup = builder.createNativeCallInst(
        Type::createNumber(), builder.getLiteralNativeExtern(external),
        signature, {builder.createGetNativeRuntimeInst()});
    builder.createCondBranchInst(safeLookup, safePath, unsafePath);
    builder.setInsertionBlock(safePath);
    builder.createBranchInst(afterLoad);
    builder.setInsertionBlock(unsafePath);
    builder.createBranchInst(afterLoad);

    builder.setInsertionPoint(&afterLoad->front());
    auto *safe = builder.createPhiInst();
    safe->addEntry(builder.getLiteralBool(true), safePath);
    safe->addEntry(builder.getLiteralBool(false), unsafePath);

    builder.setLocation(call->getLocation());
    auto *continuation = splitBasicBlock(afterLoad, call->getIterator());
    auto *fast = builder.createBasicBlock(F);
    auto *slow = builder.createBasicBlock(F);

    builder.setInsertionBlock(afterLoad);
    // StringConcatInst lowers to a helper that accepts StringPrimitive
    // handles directly. Array.prototype.join, however, coerces every element
    // and the separator. Keep the generic call for nullish, numeric, symbol,
    // and object values; the builtin identity guard alone is not a string
    // coercion proof.
    Value *allStrings = safe;
    for (auto *element : elements)
      allStrings = builder.createBinaryOperatorInst(
          allStrings,
          builder.createTypeOfIsInst(
              element,
              builder.getLiteralTypeOfIsTypes(TypeOfIsTypes{}.withString(true))),
          ValueKind::BinaryAndInstKind);
    allStrings = builder.createBinaryOperatorInst(
        allStrings,
        builder.createTypeOfIsInst(
            call->getArgument(1),
            builder.getLiteralTypeOfIsTypes(TypeOfIsTypes{}.withString(true))),
        ValueKind::BinaryAndInstKind);
    builder.createCondBranchInst(allStrings, fast, slow);

    builder.setInsertionBlock(fast);
    llvh::SmallVector<Value *, 16> stringOperands;
    stringOperands.reserve(operands.size());
    for (auto *operand : operands)
      stringOperands.push_back(builder.createUnionNarrowTrustedInst(
          operand, Type::createString()));
    auto *fastResult = builder.createStringConcatInst(stringOperands);
    builder.createBranchInst(continuation);

    builder.setInsertionBlock(slow);
    auto *slowBranch = builder.createBranchInst(continuation);
    call->moveBefore(slowBranch);

    builder.setInsertionPoint(&continuation->front());
    auto *result = builder.createPhiInst();
    call->replaceAllUsesWith(result);
    result->addEntry(fastResult, fast);
    result->addEntry(call, slow);
    ++NumFreshArrayJoinFastPaths;
  }

  llvh::SmallVector<ReturnInst *, 8> returns;
  for (auto &block : *F)
    if (auto *ret = llvh::dyn_cast<ReturnInst>(block.getTerminator()))
      returns.push_back(ret);
  auto *entry = &F->front();
  auto *body = splitBasicBlock(entry, entry->begin());
  auto *recordBlock = builder.createBasicBlock(F);
  auto *fieldsBlock = builder.createBasicBlock(F);
  auto *lookupBlock = builder.createBasicBlock(F);
  auto *hit = builder.createBasicBlock(F);
  auto *miss = builder.createBasicBlock(F);
  builder.setInsertionBlock(entry);
  auto *ready = builder.createAllocStackInst(
      "normalization.ready", Type::createBoolean());
  builder.createStoreStackInst(builder.getLiteralBool(false), ready);
  llvh::SmallVector<AllocStackInst *, 12> slots;
  for (unsigned i = 0; i < summary.fields.size() + summary.captures.size(); ++i) {
    auto *slot = builder.createAllocStackInst(
        "normalization.input", Type::createAnyType());
    builder.createStoreStackInst(builder.getLiteralUndefined(), slot);
    slots.push_back(slot);
  }
  auto &context = F->getContext();
  auto &native = context.getNativeContext();
  auto invoke = [&](llvh::StringRef name, NativeCType resultType, Type irType,
                    llvh::ArrayRef<Value *> arguments,
                    llvh::ArrayRef<NativeCType> types) {
    auto *signature = native.getSignature(resultType, types);
    auto *external = native.getExtern(
        context.getStringTable().getString(name), signature, {}, true, nullptr);
    return builder.createNativeCallInst(
        irType, builder.getLiteralNativeExtern(external), signature, arguments);
  };
  auto *runtime = builder.createGetNativeRuntimeInst();
  auto *record = builder.createLoadParamInst(F->getJSDynamicParam(1));
  auto *builtins = invoke("_sh_ljs_has_normalization_builtins", NativeCType::c_bool,
      Type::createNumber(), {runtime}, {NativeCType::ptr});
  builder.createCondBranchInst(builtins, recordBlock, body);

  builder.setInsertionBlock(recordBlock);
  auto *plain = invoke("_sh_ljs_is_plain_record", NativeCType::c_bool,
      Type::createNumber(), {runtime, record},
      {NativeCType::ptr, NativeCType::c_hermes_value});
  builder.createCondBranchInst(plain, fieldsBlock, body);

  builder.setInsertionBlock(fieldsBlock);
  llvh::SmallVector<Value *, 12> values;
  for (auto *key : summary.fields)
    values.push_back(builder.createLoadPropertyInst(record, key));
  if (!summary.captures.empty()) {
    auto *parent = builder.createGetParentScopeInst(
        summary.captures.front()->getParent(), F->getParentScopeParam());
    for (auto *variable : summary.captures) {
      assert(variable->getParent() == summary.captures.front()->getParent());
      values.push_back(builder.createLoadFrameInst(parent, variable));
    }
  }
  Value *valid = builder.getLiteralBool(true);
  for (auto *value : values)
    valid = builder.createBinaryOperatorInst(valid, builder.createTypeOfIsInst(
        value, builder.getLiteralTypeOfIsTypes(TypeOfIsTypes{}.withString(true))),
        ValueKind::BinaryAndInstKind);
  builder.createCondBranchInst(valid, lookupBlock, body);

  builder.setInsertionBlock(lookupBlock);
  llvh::SmallVector<Value *, 16> lookupArgs{runtime,
      builder.getLiteralNumber(values.size())};
  lookupArgs.append(values.begin(), values.end());
  llvh::SmallVector<NativeCType, 16> lookupTypes{
      NativeCType::ptr, NativeCType::u32};
  lookupTypes.append(values.size(), NativeCType::c_hermes_value);
  auto *cached = invoke("_sh_ljs_string_reuse_lookup", NativeCType::c_hermes_value,
      Type::createAnyType(), lookupArgs, lookupTypes);
  auto *found = builder.createTypeOfIsInst(cached,
      builder.getLiteralTypeOfIsTypes(TypeOfIsTypes{}.withString(true)));
  builder.createCondBranchInst(found, hit, miss);
  builder.setInsertionBlock(hit);
  builder.createReturnInst(cached);
  builder.setInsertionBlock(miss);
  for (unsigned i = 0; i < values.size(); ++i)
    builder.createStoreStackInst(values[i], slots[i]);
  builder.createStoreStackInst(builder.getLiteralBool(true), ready);
  builder.createBranchInst(body);

  for (auto *ret : returns) {
    auto *original = ret->getParent();
    auto *continuation = splitBasicBlock(original, ret->getIterator());
    auto *publish = builder.createBasicBlock(F);
    builder.setInsertionBlock(original);
    builder.createCondBranchInst(builder.createLoadStackInst(ready), publish, continuation);
    builder.setInsertionBlock(publish);
    llvh::SmallVector<Value *, 16> arguments{runtime, ret->getValue(),
        builder.getLiteralNumber(slots.size())};
    llvh::SmallVector<NativeCType, 16> types{
        NativeCType::ptr, NativeCType::c_hermes_value, NativeCType::u32};
    for (auto *slot : slots) {
      arguments.push_back(builder.createLoadStackInst(slot));
      types.push_back(NativeCType::c_hermes_value);
    }
    // Publishing occurs only on an ordinary successful return. Throwing paths
    // retain their original effects and leave the invocation scope unchanged.
    invoke("_sh_ljs_string_reuse_store", NativeCType::c_void,
        Type::createUndefined(), arguments, types);
    builder.createBranchInst(continuation);
  }
  ++NumNormalizationSites;
}

static bool run(Module *M) {
  const auto &settings = M->getContext().getOptimizationSettings();
  if (!settings.staticHermesInlining || !settings.staticObjectPrototype ||
      !settings.scopedNormalizationReuse)
    return false;
  bool changed = false;
  unsigned selected = 0;
  for (auto &function : *M) {
    if (selected == 16)
      break;
    Summary summary;
    if (summarize(&function, summary)) {
      rewrite(&function, summary);
      ++selected;
      changed = true;
    } else if (summarizeStringScan(&function)) {
      rewriteStringScan(&function);
      ++selected;
      changed = true;
    }
  }
  return changed;
}

} // namespace

Pass *createScopedNormalizationReuse() {
  class ThisPass : public ModulePass {
   public:
    ThisPass() : ModulePass("ScopedNormalizationReuse") {}
    bool runOnModule(Module *M) override { return run(M); }
  };
  return new ThisPass();
}

} // namespace hermes
