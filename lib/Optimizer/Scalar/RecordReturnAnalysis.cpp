/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/Optimizer/Scalar/RecordUseAnalysis.h"

#include "hermes/IR/CFG.h"
#include "hermes/IR/Instrs.h"
#include "hermes/Optimizer/Scalar/Utils.h"
#include "llvh/ADT/SmallPtrSet.h"

namespace hermes {
namespace {

constexpr unsigned kMaxValues = 512;
constexpr unsigned kMaxEdges = 4096;
constexpr unsigned kMaxFields = 64;
constexpr unsigned kMaxDepth = 16;

Value *exportedValue(Value *value) {
  if (llvh::isa<Literal>(value))
    return value;
  auto *load = llvh::dyn_cast<LoadParamInst>(value);
  return load ? llvh::dyn_cast<JSDynamicParam>(load->getParam()) : nullptr;
}

ReturnedRecordField *findField(ReturnedRecord &record, LiteralString *key) {
  for (auto &field : record.fields)
    if (field.key == key)
      return &field;
  return nullptr;
}

} // namespace

const ReturnedRecord &RecordReturnAnalysis::analyze(Function *function) {
  auto &entry = functions_[function];
  if (entry)
    return entry->result;
  entry = std::make_unique<Entry>();
  auto *current = entry.get();
  current->result.status = ReturnedRecord::Status::Recursive;
  if (depth_ == kMaxDepth) {
    current->result.status = ReturnedRecord::Status::Limit;
  } else {
    ++depth_;
    current->result = compute(function);
    --depth_;
  }
  return current->result;
}

ReturnedRecord RecordReturnAnalysis::compute(Function *function) {
  using Status = ReturnedRecord::Status;
  ReturnedRecord result;
  auto fail = [&](Status status, Instruction *boundary) {
    result.status = status;
    result.boundary = boundary;
    result.fields.clear();
    return result;
  };
  if (llvh::isa<GeneratorFunction>(function) ||
      llvh::isa<AsyncFunction>(function) || capturesArgumentVector(function))
    return result;

  llvh::SmallVector<ReturnInst *, 4> returns;
  llvh::SmallVector<Value *, 16> worklist;
  unsigned edges = 0;
  for (auto &block : *function) {
    for (auto &inst : block) {
      if (++edges > kMaxEdges)
        return fail(Status::Limit, &inst);
      if (auto *ret = llvh::dyn_cast<ReturnInst>(&inst)) {
        returns.push_back(ret);
        worklist.push_back(ret->getValue());
      }
    }
  }
  if (returns.empty())
    return fail(Status::NoReturn, nullptr);

  llvh::SmallPtrSet<Instruction *, 32> aliases;
  llvh::SmallVector<Instruction *, 8> origins;
  while (!worklist.empty()) {
    auto *value = llvh::dyn_cast<Instruction>(worklist.pop_back_val());
    if (!value)
      return fail(Status::Unsupported, returns.front());
    if (!aliases.insert(value).second)
      continue;
    if (aliases.size() > kMaxValues)
      return fail(Status::Limit, value);
    if (auto *phi = llvh::dyn_cast<PhiInst>(value)) {
      for (unsigned i = 0; i < phi->getNumEntries(); ++i) {
        if (++edges > kMaxEdges)
          return fail(Status::Limit, value);
        worklist.push_back(phi->getEntry(i).first);
      }
    } else if (llvh::isa<MovInst>(value) ||
               llvh::isa<UnionNarrowTrustedInst>(value)) {
      worklist.push_back(value->getOperand(0));
    } else if (llvh::isa<AllocObjectLiteralInst>(value) ||
               llvh::isa<CallInst>(value)) {
      origins.push_back(value);
    } else {
      return fail(Status::Unsupported, value);
    }
  }
  if (origins.empty())
    return fail(Status::Unsupported, returns.front());

  // No origin or joined alias may be published, captured, modified through an
  // alias, or observed by a getter before return. Unknown calls on unrelated
  // values are harmless to an object that has never been published.
  for (auto *alias : aliases) {
    for (auto *user : alias->getUsers()) {
      if (++edges > kMaxEdges)
        return fail(Status::Limit, user);
      if (llvh::isa<ReturnInst>(user) ||
          (aliases.count(user) &&
           (llvh::isa<PhiInst>(user) || llvh::isa<MovInst>(user) ||
            llvh::isa<UnionNarrowTrustedInst>(user))))
        continue;
      auto *allocation = llvh::dyn_cast<AllocObjectLiteralInst>(alias);
      if (auto *store = llvh::dyn_cast<PrStoreInst>(user)) {
        if (allocation && store->getObject() == allocation &&
            !aliases.count(llvh::dyn_cast<Instruction>(store->getStoredValue())) &&
            store->getPropIndex() < allocation->getKeyValuePairCount() &&
            store->getPropName() == allocation->getKey(store->getPropIndex()))
          continue;
      }
      return fail(Status::Unsupported, user);
    }
  }

  DominanceInfo dominance(function);
  bool first = true;
  for (auto *origin : origins) {
    ReturnedRecord part;
    part.status = Status::Complete;
    if (auto *allocation = llvh::dyn_cast<AllocObjectLiteralInst>(origin)) {
      auto *parent = allocation->getParentObject();
      if (!llvh::isa<EmptySentinel>(parent) && !llvh::isa<LiteralNull>(parent))
        return fail(Status::Unsupported, allocation);
      if (allocation->getKeyValuePairCount() > kMaxFields)
        return fail(Status::Limit, allocation);
      for (unsigned i = 0; i < allocation->getKeyValuePairCount(); ++i) {
        auto *key = llvh::dyn_cast<LiteralString>(allocation->getKey(i));
        if (!key || toArrayIndex(key->getValue().str()) || findField(part, key))
          return fail(Status::Unsupported, allocation);
        Value *value = allocation->getValue(i);
        Type type = value->getType();
        PrStoreInst *last = nullptr;
        bool ordered = true;
        for (auto *user : allocation->getUsers()) {
          if (++edges > kMaxEdges)
            return fail(Status::Limit, user);
          auto *store = llvh::dyn_cast<PrStoreInst>(user);
          if (!store || store->getPropIndex() != i)
            continue;
          type = function->getParent()->getTypeContext().unionTy(
              type, store->getStoredValue()->getType());
          if (!last || dominance.properlyDominates(last, store))
            last = store;
          else if (!dominance.properlyDominates(store, last))
            ordered = false;
        }
        if (last) {
          // A store need only reach this origin's incoming phi edge, not the
          // other branch's allocation. All later alias writes were rejected.
          for (auto *user : allocation->getUsers()) {
            if (++edges > kMaxEdges)
              return fail(Status::Limit, user);
            if (llvh::isa<PrStoreInst>(user))
              continue;
            if (auto *phi = llvh::dyn_cast<PhiInst>(user)) {
              for (unsigned j = 0; j < phi->getNumEntries(); ++j) {
                if (++edges > kMaxEdges)
                  return fail(Status::Limit, phi);
                auto entry = phi->getEntry(j);
                if (entry.first == allocation)
                  ordered &= dominance.properlyDominates(
                      last, entry.second->getTerminator());
              }
            } else {
              ordered &= dominance.properlyDominates(last, user);
            }
          }
          if (ordered) {
            value = last->getStoredValue();
            type = value->getType();
          } else {
            value = nullptr;
          }
        }
        part.fields.push_back(
            {key, true, i, type, value ? exportedValue(value) : nullptr});
      }
    } else {
      auto *call = llvh::cast<CallInst>(origin);
      auto *target = llvh::dyn_cast<Function>(call->getTarget());
      if (!target || !call->getCalleeIsAlwaysClosure()->getValue() ||
          !call->getNewTarget()->getType().isUndefinedType())
        return fail(Status::Unsupported, call);
      part = analyze(target);
      if (part.status != Status::Complete)
        return fail(part.status, call);
      for (auto &field : part.fields) {
        if (auto *param = llvh::dyn_cast_or_null<JSDynamicParam>(field.value)) {
          unsigned index = param->getIndexInParamList();
          // Missing arguments are left unknown rather than constructing IR
          // values inside an analysis.
          field.value = index < call->getNumArguments()
              ? exportedValue(call->getArgument(index))
              : nullptr;
        }
      }
    }

    if (first) {
      result.fields = std::move(part.fields);
      first = false;
      continue;
    }
    for (auto &field : result.fields) {
      auto *other = findField(part, field.key);
      field.definitelyOwn &= other && other->definitelyOwn;
      if (!other) {
        field.ordinal = llvh::None;
        field.value = nullptr;
        continue;
      }
      if (field.ordinal != other->ordinal)
        field.ordinal = llvh::None;
      if (field.value != other->value)
        field.value = nullptr;
      field.type = function->getParent()->getTypeContext().unionTy(
          field.type, other->type);
    }
    for (auto field : part.fields) {
      if (findField(result, field.key))
        continue;
      if (result.fields.size() == kMaxFields)
        return fail(Status::Limit, origin);
      field.definitelyOwn = false;
      field.ordinal = llvh::None;
      field.value = nullptr;
      result.fields.push_back(field);
    }
  }
  result.status = Status::Complete;
  return result;
}

} // namespace hermes
