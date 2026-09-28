/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/AST/NativeContext.h"
#include "hermes/IR/IRBuilder.h"
#include "hermes/Optimizer/PassManager/Pass.h"
#include "hermes/Optimizer/Scalar/Utils.h"
#include "hermes/Support/Statistic.h"

#include "llvh/ADT/DenseMap.h"
#include "llvh/ADT/SmallPtrSet.h"

#include <algorithm>

#define DEBUG_TYPE "guarded-record-load-dedup"

STATISTIC(NumPlainRecordGuards, "Plain record receiver guards emitted");
STATISTIC(NumGuardedRecordReads, "Repeated record reads guarded for reuse");
STATISTIC(NumReadOnlyCalls, "Known helper calls crossed by guarded record regions");

namespace hermes {
namespace {

struct RecordAccess {
  Value *root;
  llvh::SmallVector<LiteralString *, 4> fields{};

  bool operator==(const RecordAccess &other) const {
    return root == other.root && fields == other.fields;
  }
};

struct ReadOnlyCallProof {
  bool valid = false;
  llvh::SmallVector<RecordAccess, 8> records{};
};

/// A known body may read its parameters without writing through aliases. A
/// plain-record probe on each relevant actual argument makes those reads
/// non-observable. This is not a purity annotation: the call still executes,
/// and failed probes invalidate every earlier cached value before it runs.
static bool collectReadOnlyCallProof(
    CallInst *call,
    const llvh::DenseMap<Value *, RecordAccess> *arguments,
    ReadOnlyCallProof &proof,
    llvh::SmallPtrSetImpl<Function *> &active,
    unsigned &remaining) {
  auto *callee = llvh::dyn_cast<Function>(call->getTarget());
  if (!callee || active.size() >= 4 || active.count(callee) ||
      !call->getCalleeIsAlwaysClosure()->getValue() ||
      !call->getNewTarget()->getType().isUndefinedType() ||
      functionHasTryCatch(callee))
    return false;

  active.insert(callee);
  // Only normal returns can reach the caller's later cached reads. Both the
  // caller and every summarized helper exclude handlers, so effects confined
  // to a throwing path cannot resume this region with stale values.
  llvh::SmallPtrSet<BasicBlock *, 32> returning;
  llvh::SmallVector<BasicBlock *, 32> pending;
  for (auto &block : *callee) {
    if (!remaining)
      return false;
    --remaining;
    if (llvh::isa<ReturnInst>(block.getTerminator()))
      pending.push_back(&block);
  }
  while (!pending.empty()) {
    auto *block = pending.pop_back_val();
    if (!returning.insert(block).second)
      continue;
    for (auto *pred : predecessors(block)) {
      if (!remaining)
        return false;
      --remaining;
      pending.push_back(pred);
    }
  }
  llvh::DenseMap<Value *, RecordAccess> values;
  for (auto &block : *callee) {
    if (!returning.count(&block))
      continue;
    for (auto &inst : block) {
      if (!remaining)
        return false;
      --remaining;
      if (auto *load = llvh::dyn_cast<LoadParamInst>(&inst)) {
        const auto &params = callee->getJSDynamicParams();
        auto found = std::find(params.begin(), params.end(), load->getParam());
        if (found == params.end())
          return false;
        const unsigned index = found - params.begin();
        if (index < call->getNumArguments()) {
          auto *argument = call->getArgument(index);
          if (arguments) {
            auto origin = arguments->find(argument);
            if (origin != arguments->end())
              values[load] = origin->second;
          } else {
            values[load] = {argument};
          }
        }
        continue;
      }
      if (auto *load = llvh::dyn_cast<LoadPropertyInst>(&inst)) {
        auto *key = llvh::dyn_cast<LiteralString>(load->getProperty());
        auto receiver = values.find(load->getObject());
        if (!key || isStandardObjectPrototypeKey(key->getValue()) ||
            receiver == values.end())
          return false;
        const auto &actual = receiver->second;
        if (std::find(proof.records.begin(), proof.records.end(), actual) ==
            proof.records.end()) {
          if (proof.records.size() == 8)
            return false;
          proof.records.push_back(actual);
        }
        if (actual.fields.size() == 4)
          return false;
        auto child = actual;
        child.fields.push_back(key);
        values[load] = std::move(child);
        continue;
      }
      if (auto *nested = llvh::dyn_cast<CallInst>(&inst)) {
        if (!collectReadOnlyCallProof(
                nested, &values, proof, active, remaining))
          return false;
        continue;
      }
      if (inst.getSideEffect().hasSideEffect())
        return false;
    }
  }
  active.erase(callee);
  return true;
}

bool run(Function *F) {
  const auto &settings = F->getContext().getOptimizationSettings();
  if (!settings.staticHermesInlining || !settings.guardedRecordLoads ||
      !settings.staticObjectPrototype || functionHasTryCatch(F))
    return false;

  constexpr unsigned absent = UINT32_MAX;
  using Key = std::pair<Value *, LiteralString *>;
  struct Step {
    LoadPropertyInst *load = nullptr;
    Instruction *numericEffect = nullptr;
    CallInst *readOnlyCall = nullptr;
    llvh::SmallVector<RecordAccess, 8> callRecords{};
    BasicBlock *join = nullptr;
    llvh::SmallVector<std::pair<TerminatorInst *, unsigned>, 4> incoming{};
    unsigned previousRead = absent;
    unsigned previousGuard = absent;
    bool guardReceiver = false;
  };
  struct State {
    llvh::DenseMap<Key, unsigned> available{};
    llvh::DenseMap<Value *, Value *> aliases{};
    llvh::SmallPtrSet<Value *, 8> guarded{};
    unsigned previousGuard = absent;
  };

  llvh::SmallVector<LoadPropertyInst *, 64> loads;
  llvh::DenseMap<Key, unsigned> counts;
  for (auto &block : *F) {
    for (auto &inst : block) {
      auto *load = llvh::dyn_cast<LoadPropertyInst>(&inst);
      auto *key = load
          ? llvh::dyn_cast<LiteralString>(load->getProperty())
          : nullptr;
      if (key && !isStandardObjectPrototypeKey(key->getValue())) {
        loads.push_back(load);
        ++counts[{load->getObject(), key}];
      }
    }
  }

  // Select starts with actual reuse, including later parts of large functions.
  // Every candidate walk and the emitted region have independent size bounds.
  llvh::SmallVector<LoadPropertyInst *, 64> roots;
  for (auto *load : loads)
    if (counts[{load->getObject(),
                llvh::cast<LiteralString>(load->getProperty())}] > 1)
      roots.push_back(load);
  std::stable_sort(roots.begin(), roots.end(), [&](auto *left, auto *right) {
    return counts[{left->getObject(),
                   llvh::cast<LiteralString>(left->getProperty())}] >
        counts[{right->getObject(),
                llvh::cast<LiteralString>(right->getProperty())}];
  });
  if (roots.size() > 64)
    roots.resize(64);

  llvh::SmallVector<Step, 32> selected;
  unsigned bestSavings = 0;
  // Cache each call-site analysis across candidate region walks, and bound the
  // total transitive body inspection independently of the region-size limits.
  llvh::DenseMap<CallInst *, ReadOnlyCallProof> callProofs;
  unsigned remainingCallInstructions = 4096;
  for (auto *root : roots) {
    llvh::SmallVector<Step, 32> steps;
    llvh::SmallPtrSet<BasicBlock *, 16> admitted;
    llvh::SmallVector<BasicBlock *, 16> pending{root->getParent()};
    llvh::DenseMap<BasicBlock *, State> outgoing;
    admitted.insert(root->getParent());
    unsigned instructions = 0;
    unsigned reads = 0;
    unsigned repeats = 0;
    unsigned guards = 0;
    unsigned childLoads = 0;
    unsigned numericGuards = 0;
    while (!pending.empty()) {
      // A join is usable only after every incoming path has completed without
      // an unsupported effect. Backedges and paths outside the region therefore
      // cannot import stale values into this bounded forward traversal.
      auto ready = std::find_if(pending.begin(), pending.end(), [&](auto *block) {
        if (block == root->getParent())
          return true;
        for (auto *pred : predecessors(block))
          if (!outgoing.count(pred))
            return false;
        return true;
      });
      if (ready == pending.end())
        break;
      auto *block = *ready;
      pending.erase(ready);
      State state;
      const bool entry = block == root->getParent();
      if (!entry) {
        Step join;
        join.join = block;
        llvh::SmallPtrSet<BasicBlock *, 16> seen;
        for (auto *pred : predecessors(block)) {
          if (!seen.insert(pred).second)
            continue;
          const auto &incoming = outgoing.find(pred)->second;
          join.incoming.push_back({pred->getTerminator(), incoming.previousGuard});
          if (seen.size() == 1) {
            state = incoming;
            continue;
          }
          for (auto it = state.available.begin(); it != state.available.end();) {
            auto current = it++;
            auto other = incoming.available.find(current->first);
            if (other == incoming.available.end() ||
                other->second != current->second)
              state.available.erase(current);
          }
          for (auto it = state.aliases.begin(); it != state.aliases.end();) {
            auto current = it++;
            auto other = incoming.aliases.find(current->first);
            if (other == incoming.aliases.end() || other->second != current->second)
              state.aliases.erase(current);
          }
          for (auto it = state.guarded.begin(); it != state.guarded.end();) {
            auto *value = *it++;
            if (!incoming.guarded.count(value))
              state.guarded.erase(value);
          }
        }
        if (std::any_of(join.incoming.begin(), join.incoming.end(), [&](auto edge) {
              return edge.second != state.previousGuard;
            })) {
          state.previousGuard = steps.size();
          steps.push_back(std::move(join));
        }
      }
      bool finished = true;
      auto begin = entry ? root->getIterator() : block->begin();
      for (auto it = begin; it != block->end(); ++it) {
        if (++instructions > 256 || reads == 32) {
          finished = false;
          break;
        }
        if (auto *load = llvh::dyn_cast<LoadPropertyInst>(&*it)) {
          auto *key = llvh::dyn_cast<LiteralString>(load->getProperty());
          if (!key || isStandardObjectPrototypeKey(key->getValue())) {
            finished = false;
            break;
          }
          auto *object = load->getObject();
          if (auto alias = state.aliases.find(object);
              alias != state.aliases.end())
            object = alias->second;
          auto found = state.available.find({object, key});
          const unsigned previous = found == state.available.end()
              ? absent
              : found->second;
          const bool guard = !state.guarded.count(object);
          if (guard && guards == 8) {
            finished = false;
            break;
          }
          const unsigned index = steps.size();
          Step read;
          read.load = load;
          read.previousRead = previous;
          read.previousGuard = state.previousGuard;
          read.guardReceiver = guard;
          steps.push_back(std::move(read));
          ++reads;
          if (guard) {
            state.guarded.insert(object);
            state.previousGuard = index;
            ++guards;
          }
          if (previous == absent) {
            state.available[{object, key}] = index;
          } else {
            state.aliases[load] = steps[previous].load;
            ++repeats;
          }
        } else if (it->getSideEffect().hasSideEffect()) {
          if (auto *call = llvh::dyn_cast<CallInst>(&*it)) {
            auto found = callProofs.find(call);
            if (found == callProofs.end()) {
              ReadOnlyCallProof proof;
              llvh::SmallPtrSet<Function *, 4> active;
              proof.valid = collectReadOnlyCallProof(
                  call, nullptr, proof, active, remainingCallInstructions);
              found = callProofs.insert({call, std::move(proof)}).first;
            }
            if (found->second.valid) {
              Step effect;
              effect.readOnlyCall = call;
              effect.previousGuard = state.previousGuard;
              unsigned neededLoads = 0;
              for (auto record : found->second.records) {
                if (auto alias = state.aliases.find(record.root);
                    alias != state.aliases.end())
                  record.root = alias->second;
                if (!record.fields.empty() || !state.guarded.count(record.root)) {
                  neededLoads += !record.fields.empty();
                  effect.callRecords.push_back(record);
                }
              }
              if (guards + effect.callRecords.size() <= 8) {
                for (const auto &record : effect.callRecords)
                  if (record.fields.empty())
                    state.guarded.insert(record.root);
                guards += effect.callRecords.size();
                childLoads += neededLoads;
                state.previousGuard = steps.size();
                steps.push_back(std::move(effect));
                continue;
              }
            }
          }
          llvh::SmallVector<Value *, 4> operands;
          bool numeric = false;
          if (auto *binary = llvh::dyn_cast<BinaryOperatorInst>(&*it)) {
            auto kind = binary->getKind();
            if (kind != ValueKind::BinaryInInstKind &&
                kind != ValueKind::BinaryInstanceOfInstKind &&
                kind != ValueKind::BinaryPrivateInInstKind) {
              auto *left = binary->getLeftHandSide();
              auto *right = binary->getRightHandSide();
              // Equality with null/undefined never converts the other operand.
              if ((kind == ValueKind::BinaryEqualInstKind ||
                   kind == ValueKind::BinaryNotEqualInstKind) &&
                  (llvh::isa<LiteralNull>(left) ||
                   llvh::isa<LiteralUndefined>(left) ||
                   llvh::isa<LiteralNull>(right) ||
                   llvh::isa<LiteralUndefined>(right)))
                continue;
              numeric = true;
              operands = {left, right};
            }
          } else if (auto *call = llvh::dyn_cast<CallBuiltinInst>(&*it)) {
            switch (call->getBuiltinIndex()) {
              case BuiltinMethod::Math_abs:
              case BuiltinMethod::Math_ceil:
              case BuiltinMethod::Math_floor:
              case BuiltinMethod::Math_round:
              case BuiltinMethod::Math_trunc:
              case BuiltinMethod::Math_min:
              case BuiltinMethod::Math_max:
                numeric = true;
                for (unsigned i = 1; i < call->getNumArguments(); ++i)
                  operands.push_back(call->getArgument(i));
                break;
              default:
                break;
            }
          }
          if (numeric) {
            unsigned needed = 0;
            for (auto *operand : operands)
              needed += !operand->getType().isNumberType();
            if (!needed)
              continue;
            if (numericGuards + needed <= 16) {
              // The original operation still executes once in place. If a
              // coercion can run user code, disable later reuse before it runs.
              Step effect;
              effect.numericEffect = &*it;
              effect.previousGuard = state.previousGuard;
              state.previousGuard = steps.size();
              steps.push_back(std::move(effect));
              numericGuards += needed;
              continue;
            }
          }
          // A layout proves reads cannot run JS, not that values survive writes
          // or unsupported calls. Even an unrelated-looking alias can write it.
          finished = false;
          break;
        }
      }
      if (!finished)
        continue;
      outgoing[block] = std::move(state);
      for (auto *successor : successors(block))
        if (admitted.size() < 16 && admitted.insert(successor).second)
          pending.push_back(successor);
    }
    if (repeats >= 2 && repeats > guards + childLoads &&
        repeats - guards - childLoads > bestSavings) {
      bestSavings = repeats - guards - childLoads;
      selected = std::move(steps);
    }
  }
  if (selected.empty())
    return false;

  IRBuilder builder(F);
  auto &context = F->getContext();
  auto &native = context.getNativeContext();
  auto *signature = native.getSignature(
      NativeCType::c_bool, {NativeCType::ptr, NativeCType::c_hermes_value});
  auto *probe = native.getExtern(
      context.getStringTable().getString("_sh_ljs_is_plain_record"),
      signature,
      {},
      true,
      nullptr);
  llvh::SmallVector<Value *, 32> values;
  llvh::SmallVector<Value *, 32> validAfter;
  llvh::DenseMap<Value *, Value *> rewrittenReads;
  for (const auto &read : selected) {
    if (read.join) {
      builder.setInsertionPoint(&read.join->front());
      auto *valid = builder.createPhiInst();
      for (auto edge : read.incoming)
        valid->addEntry(
            edge.second == absent ? builder.getLiteralNumber(1)
                                  : validAfter[edge.second],
            edge.first->getParent());
      values.push_back(nullptr);
      validAfter.push_back(valid);
      continue;
    }
    if (read.readOnlyCall) {
      builder.setInsertionPoint(read.readOnlyCall);
      builder.setLocation(read.readOnlyCall->getLocation());
      Value *valid = read.previousGuard == absent
          ? builder.getLiteralNumber(1)
          : validAfter[read.previousGuard];
      llvh::SmallVector<std::pair<RecordAccess, Value *>, 8> children;
      for (const auto &access : read.callRecords) {
        Value *record = access.root;
        // A preceding read may now live only in its fallback block. Use its
        // merged result, which still dominates the original helper call.
        if (auto rewritten = rewrittenReads.find(record);
            rewritten != rewrittenReads.end())
          record = rewritten->second;
        if (access.fields.empty()) {
          auto *plain = builder.createNativeCallInst(
              Type::createNumber(),
              builder.getLiteralNativeExtern(probe),
              signature,
              {builder.createGetNativeRuntimeInst(), record});
          valid = builder.createBinaryOperatorInst(
              valid, plain, ValueKind::BinaryAndInstKind);
        } else {
          auto prefix = access;
          auto *key = prefix.fields.pop_back_val();
          if (!prefix.fields.empty()) {
            auto parent = std::find_if(children.begin(), children.end(),
                [&](const auto &entry) { return entry.first == prefix; });
            assert(parent != children.end() && "Missing guarded path prefix");
            record = parent->second;
          }
          // Never hoist an unproved getter or descend through null/undefined.
          // Every prefix was probed before this child. If any probe failed,
          // skip speculative reads and let the original helper run once.
          auto *call = read.readOnlyCall;
          auto *before = call->getParent();
          auto *continuation = splitBasicBlock(before, call->getIterator());
          auto *check = builder.createBasicBlock(F);
          builder.setInsertionBlock(before);
          builder.createCondBranchInst(valid, check, continuation);
          builder.setInsertionBlock(check);
          auto *child = builder.createLoadPropertyInst(record, key);
          auto *plain = builder.createNativeCallInst(
              Type::createNumber(),
              builder.getLiteralNativeExtern(probe),
              signature,
              {builder.createGetNativeRuntimeInst(), child});
          builder.createBranchInst(continuation);
          builder.setInsertionPoint(&continuation->front());
          auto *checked = builder.createPhiInst();
          checked->addEntry(builder.getLiteralNumber(0), before);
          checked->addEntry(plain, check);
          auto *value = builder.createPhiInst();
          value->addEntry(builder.getLiteralUndefined(), before);
          value->addEntry(child, check);
          children.push_back({access, value});
          valid = checked;
          builder.setInsertionPoint(call);
        }
        ++NumPlainRecordGuards;
      }
      values.push_back(nullptr);
      validAfter.push_back(valid);
      ++NumReadOnlyCalls;
      continue;
    }
    if (read.numericEffect) {
      auto *inst = read.numericEffect;
      builder.setInsertionPoint(inst);
      builder.setLocation(inst->getLocation());
      Value *valid = read.previousGuard == absent
          ? builder.getLiteralNumber(1)
          : validAfter[read.previousGuard];
      llvh::SmallVector<Value *, 4> operands;
      if (auto *binary = llvh::dyn_cast<BinaryOperatorInst>(inst)) {
        operands = {binary->getLeftHandSide(), binary->getRightHandSide()};
      } else {
        auto *call = llvh::cast<CallBuiltinInst>(inst);
        for (unsigned i = 1; i < call->getNumArguments(); ++i)
          operands.push_back(call->getArgument(i));
      }
      for (auto *operand : operands) {
        if (operand->getType().isNumberType())
          continue;
        auto *number = builder.createTypeOfIsInst(
            operand,
            builder.getLiteralTypeOfIsTypes(TypeOfIsTypes{}.withNumber(true)));
        valid = builder.createBinaryOperatorInst(
            valid, number, ValueKind::BinaryAndInstKind);
      }
      values.push_back(nullptr);
      validAfter.push_back(valid);
      continue;
    }
    auto *load = read.load;
    builder.setLocation(load->getLocation());
    builder.setInsertionPoint(load);
    Value *valid = read.previousGuard == absent
        ? builder.getLiteralNumber(1)
        : validAfter[read.previousGuard];
    if (read.guardReceiver) {
      auto *plain = builder.createNativeCallInst(
          Type::createNumber(),
          builder.getLiteralNativeExtern(probe),
          signature,
          {builder.createGetNativeRuntimeInst(), load->getObject()});
      valid = read.previousGuard == absent
          ? static_cast<Value *>(plain)
          : builder.createBinaryOperatorInst(
                valid, plain, ValueKind::BinaryAndInstKind);
      ++NumPlainRecordGuards;
    }
    if (read.previousRead == absent) {
      values.push_back(load);
    } else {
      auto *original = load->getParent();
      auto *continuation = splitBasicBlock(original, load->getIterator());
      auto *slow = builder.createBasicBlock(F);
      builder.setInsertionBlock(original);
      builder.createCondBranchInst(valid, continuation, slow);
      builder.setInsertionBlock(slow);
      auto *branch = builder.createBranchInst(continuation);
      load->moveBefore(branch);
      builder.setInsertionPoint(&continuation->front());
      auto *result = builder.createPhiInst();
      load->replaceAllUsesWith(result);
      result->addEntry(values[read.previousRead], original);
      result->addEntry(load, slow);
      rewrittenReads[load] = result;
      values.push_back(result);
      ++NumGuardedRecordReads;
    }
    // Once any receiver could execute user code, every earlier value in this
    // region becomes unavailable. The generic reads never replay a prefix.
    validAfter.push_back(valid);
  }
  return true;
}

} // namespace

Pass *createGuardedRecordLoadDedup() {
  class ThisPass : public FunctionPass {
   public:
    ThisPass() : FunctionPass("GuardedRecordLoadDedup") {}
    bool runOnFunction(Function *F) override {
      return run(F);
    }
  };
  return new ThisPass();
}

} // namespace hermes
