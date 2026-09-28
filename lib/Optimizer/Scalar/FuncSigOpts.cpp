/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#define DEBUG_TYPE "funcsigopts"

#include "hermes/Optimizer/Scalar/FuncSigOpts.h"
#include "hermes/Optimizer/Scalar/Utils.h"

#include "hermes/IR/Analysis.h"
#include "hermes/IR/CFG.h"
#include "hermes/IR/IRBuilder.h"
#include "hermes/IR/Instrs.h"
#include "hermes/Support/Statistic.h"

#include "llvh/ADT/DenseMap.h"
#include "llvh/ADT/DenseSet.h"
#include "llvh/ADT/STLExtras.h"
#include "llvh/Support/Debug.h"

#include <iterator>

STATISTIC(NumParamOpt, "Number of parameters optimized");
STATISTIC(NumArgsOpt, "Number of arguments optimized");
STATISTIC(NumRecordParams, "Number of record parameters promoted to fields");

using namespace hermes;
using llvh::dbgs;

/// Prove that a captured parameter is initialized before its environment can
/// reach a closure or a read. New field slots will be initialized at this store,
/// so they retain the original environment lifetime and GC ownership.
static bool isInitialRecordCapture(StoreFrameInst *store, Function *F) {
  auto *scope = llvh::dyn_cast<CreateScopeInst>(store->getScope());
  if (!scope || store->getParent() != &F->front() ||
      scope->getParent() != store->getParent() ||
      scope->getVariableScope() != store->getVariable()->getParent())
    return false;
  for (auto *user : store->getVariable()->getUsers()) {
    if (user != store && !llvh::isa<LoadFrameInst>(user))
      return false;
  }
  for (auto it = std::next(scope->getIterator());
       it != scope->getParent()->end(); ++it) {
    if (&*it == store)
      return true;
    for (unsigned i = 0; i < it->getNumOperands(); ++i) {
      if (it->getOperand(i) != scope)
        continue;
      // Other parameter/lexical initializations do not publish the scope.
      auto *priorStore = llvh::dyn_cast<StoreFrameInst>(&*it);
      if (!priorStore || priorStore->getScope() != scope)
        return false;
    }
  }
  return false;
}

/// A missing own property cannot invoke inherited accessors when its prototype
/// is null, or when the embedding guarantees it stays absent on the intrinsic
/// Object.prototype. Existing standard keys may still change descriptors.
static bool isProvedAbsent(
    AllocObjectLiteralInst *alloc,
    LiteralString *key) {
  auto *parent = alloc->getParentObject();
  if (llvh::isa<LiteralNull>(parent))
    return true;
  if (!llvh::isa<EmptySentinel>(parent) ||
      !alloc->getModule()->getContext().getOptimizationSettings()
           .staticObjectPrototype)
    return false;
  return !isStandardObjectPrototypeKey(key->getValue());
}

/// Replace a private record argument with its independent fields. Every caller
/// must construct an unaliased own-data-property record; the callee only reads
/// its fields. Even calls made by the callee cannot change the record;
/// passing the existing field values does not move getters or other effects.
static bool promoteRecordParameters(Function *F) {
  auto callsites = getKnownCallsites(F);
  const unsigned originalParams = F->getJSDynamicParams().size();
  if (callsites.empty() || callsites.size() > 64 || originalParams > 32)
    return false;
  for (auto *call : callsites)
    if (!llvh::isa<CallInst>(call) ||
        !call->getNewTarget()->getType().isUndefinedType())
      return false;

  bool changed = false;
  unsigned promotedFields = 0;
  for (unsigned paramIdx = 1; paramIdx < originalParams; ++paramIdx) {
    auto *param = F->getJSDynamicParam(paramIdx);
    llvh::SmallVector<LoadPropertyInst *, 8> reads;
    llvh::SmallVector<LiteralString *, 8> keys;
    llvh::SmallVector<Instruction *, 8> recordLoads;
    llvh::SmallVector<StoreFrameInst *, 2> captures;
    bool eligible = true;
    for (auto *user : param->getUsers()) {
      auto *load = llvh::dyn_cast<LoadParamInst>(user);
      if (!load) {
        eligible = false;
        break;
      }
      recordLoads.push_back(load);
    }
    for (unsigned i = 0; eligible && i < recordLoads.size(); ++i) {
      auto *load = recordLoads[i];
      for (auto *use : load->getUsers()) {
        if (auto *store = llvh::dyn_cast<StoreFrameInst>(use)) {
          // Only the original parameter may initialize a local frame slot.
          // Reassigned, transitive and loop-created environments stay generic.
          if (!llvh::isa<LoadParamInst>(load) || store->getValue() != load ||
              captures.size() >= 4 || !isInitialRecordCapture(store, F)) {
            eligible = false;
            break;
          }
          captures.push_back(store);
          for (auto *user : store->getVariable()->getUsers())
            if (auto *frameLoad = llvh::dyn_cast<LoadFrameInst>(user))
              recordLoads.push_back(frameLoad);
          if (recordLoads.size() > 64) {
            eligible = false;
            break;
          }
          continue;
        }
        auto *read = llvh::dyn_cast<LoadPropertyInst>(use);
        auto *key = read
            ? llvh::dyn_cast<LiteralString>(read->getProperty())
            : nullptr;
        if (!key || read->getObject() != load) {
          eligible = false;
          break;
        }
        reads.push_back(read);
        if (reads.size() > 256) {
          eligible = false;
          break;
        }
        if (llvh::find(keys, key) == keys.end())
          keys.push_back(key);
      }
      if (!eligible)
        break;
    }
    if (!eligible || keys.empty() || keys.size() + promotedFields > 8)
      continue;

    llvh::SmallVector<llvh::SmallVector<Value *, 8>, 4> fieldsByCall;
    for (auto *call : callsites) {
      if (paramIdx >= call->getNumArguments()) {
        eligible = false;
        break;
      }
      auto *alloc = llvh::dyn_cast<AllocObjectLiteralInst>(
          call->getArgument(paramIdx));
      if (!alloc || alloc->getKeyValuePairCount() > 16) {
        eligible = false;
        break;
      }
      const bool sameBlock = alloc->getParent() == call->getParent();
      if (!sameBlock) {
        // Across blocks, admit a normal tail return outside any handler. The
        // callee may retain a field-reading closure: its original wrapper must
        // never be changed or passed again after this invocation, including on
        // an exceptional path. Same-block initialization proves this separately
        // by requiring all stores between the allocation and call.
        auto next = std::next(call->getIterator());
        auto *ret = next == call->getParent()->end()
            ? nullptr
            : llvh::dyn_cast<ReturnInst>(&*next);
        if (!ret || ret->getValue() != call ||
            call->getFunction()->size() > 512) {
          eligible = false;
          break;
        }
        auto enclosing = findEnclosingTrysPerBlock(call->getFunction());
        if (enclosing && enclosing->lookup(call->getParent())) {
          eligible = false;
          break;
        }
      }
      for (unsigned i = 0; i < call->getNumArguments(); ++i)
        if (i != paramIdx && call->getArgument(i) == alloc)
          eligible = false;

      llvh::SmallDenseSet<PrStoreInst *, 8> stores;
      for (auto *user : alloc->getUsers()) {
        if (user == call)
          continue;
        auto *store = llvh::dyn_cast<PrStoreInst>(user);
        if (!store || store->getObject() != alloc ||
            store->getStoredValue() == alloc ||
            (sameBlock && store->getParent() != alloc->getParent()) ||
            store->getPropIndex() >= alloc->getKeyValuePairCount()) {
          eligible = false;
          break;
        }
        stores.insert(store);
      }
      if (!eligible)
        break;

      llvh::SmallVector<Value *, 8> values;
      for (unsigned i = 0; i < alloc->getKeyValuePairCount(); ++i)
        values.push_back(alloc->getValue(i));
      if (sameBlock) {
        // Reject stores after the call, including loop-carried mutations.
        auto it = std::next(alloc->getIterator());
        for (; it != alloc->getParent()->end() && &*it != call; ++it) {
          if (auto *store = llvh::dyn_cast<PrStoreInst>(&*it)) {
            if (stores.erase(store))
              values[store->getPropIndex()] = store->getStoredValue();
          }
        }
        if (it == alloc->getParent()->end() || !stores.empty()) {
          eligible = false;
          break;
        }
      } else {
        DominanceInfo dominance(call->getFunction());
        llvh::SmallVector<PrStoreInst *, 8> finalStores(values.size(), nullptr);
        for (auto *store : stores) {
          if (!dominance.properlyDominates(alloc, store) ||
              !dominance.properlyDominates(store, call)) {
            eligible = false;
            break;
          }
          auto *&last = finalStores[store->getPropIndex()];
          if (!last || dominance.properlyDominates(last, store))
            last = store;
          else if (!dominance.properlyDominates(store, last)) {
            eligible = false;
            break;
          }
        }
        if (!eligible)
          break;
        for (unsigned i = 0; i < finalStores.size(); ++i)
          if (finalStores[i])
            values[i] = finalStores[i]->getStoredValue();
      }
      llvh::SmallVector<Value *, 8> fields;
      for (auto *key : keys) {
        unsigned i = 0;
        for (; i < alloc->getKeyValuePairCount(); ++i)
          if (alloc->getKey(i) == key)
            break;
        if (i == alloc->getKeyValuePairCount()) {
          if (!isProvedAbsent(alloc, key)) {
            eligible = false;
            break;
          }
          fields.push_back(F->getParent()->getLiteralUndefined());
        } else {
          fields.push_back(values[i]);
        }
      }
      if (!eligible)
        break;
      fieldsByCall.push_back(std::move(fields));
    }
    if (!eligible)
      continue;

    IRBuilder builder(F);
    const unsigned firstField = F->getJSDynamicParams().size();
    llvh::SmallDenseMap<LiteralString *, JSDynamicParam *, 8> fieldParams;
    for (auto *key : keys)
      fieldParams[key] = builder.createJSDynamicParam(F, key->getValue());
    llvh::DenseMap<Variable *, llvh::SmallDenseMap<LiteralString *, Variable *, 8>>
        capturedFields;
    for (auto *capture : captures) {
      auto *variable = capture->getVariable();
      auto &fields = capturedFields[variable];
      // Allocate only fields read through this captured variable. Direct reads
      // still consume parameters without an extra environment load.
      for (auto *read : reads) {
        auto *frameLoad = llvh::dyn_cast<LoadFrameInst>(read->getObject());
        if (!frameLoad || frameLoad->getLoadVariable() != variable)
          continue;
        auto *key = llvh::cast<LiteralString>(read->getProperty());
        if (fields.count(key))
          continue;
        fields[key] = builder.createVariable(
            variable->getParent(), key->getValue(), Type::createAnyType(), true);
        builder.setInsertionPoint(capture);
        builder.setLocation(capture->getLocation());
        auto *value = builder.createLoadParamInst(fieldParams[key]);
        builder.createStoreFrameInst(capture->getScope(), value, fields[key]);
      }
    }
    for (auto *read : reads) {
      builder.setInsertionPoint(read);
      builder.setLocation(read->getLocation());
      auto *key = llvh::cast<LiteralString>(read->getProperty());
      if (auto *load = llvh::dyn_cast<LoadFrameInst>(read->getObject())) {
        read->replaceAllUsesWith(builder.createLoadFrameInst(
            load->getScope(), capturedFields[load->getLoadVariable()][key]));
      } else {
        read->replaceAllUsesWith(builder.createLoadParamInst(fieldParams[key]));
      }
      read->eraseFromParent();
    }
    for (auto *capture : captures)
      capture->eraseFromParent();
    for (auto *load : recordLoads) {
      assert(!load->hasUsers() && "record parameter still escapes");
      load->eraseFromParent();
    }
    for (unsigned i = 0; i < callsites.size(); ++i) {
      auto *call = callsites[i];
      llvh::SmallVector<Value *, 16> operands;
      for (unsigned j = 0; j < call->getNumOperands(); ++j)
        operands.push_back(static_cast<Instruction *>(call)->getOperand(j));
      // Extra user arguments are not visible (arguments/rest were rejected),
      // but their computations remain. The new fields must occupy formal slots.
      operands.resize(
          call->getThisIdx() + firstField, builder.getLiteralUndefined());
      operands.append(fieldsByCall[i].begin(), fieldsByCall[i].end());
      operands[call->getThisIdx() + paramIdx] =
          builder.getLiteralPositiveZero();
      builder.setInsertionPoint(call);
      auto *replacement =
          llvh::cast<CallInst>(builder.cloneInst(call, operands));
      call->replaceAllUsesWith(replacement);
      call->eraseFromParent();
      callsites[i] = replacement;
    }
    promotedFields += keys.size();
    ++NumRecordParams;
    changed = true;
  }
  return changed;
}

/// Optimize the calls to F, and the use of literals in F.
/// \returns true if some code changed.
static bool performFSO(Function *F, std::vector<Function *> &worklist) {
  LLVM_DEBUG(dbgs() << "-- Inspecting " << F->getInternalNameStr() << "\n");

  if (capturesArgumentVector(F))
    return false;

  // Generators and async functions should be treated as using all their
  // parameters. CreateGenerator is considered a user of all the
  // arguments because it stores them. The function that is called and the
  // function that actually uses the parameters are different.
  if (llvh::isa<GeneratorFunction>(F) || llvh::isa<AsyncFunction>(F))
    return false;

  IRBuilder builder(F);

  if (!F->allCallsitesKnown())
    return false;

  bool changed = promoteRecordParameters(F);
  auto callsites = getKnownCallsites(F);
  LLVM_DEBUG(dbgs() << "-- Has " << callsites.size() << " call sites\n");

  if (callsites.empty()) {
    // If we don't have any callsites, don't change any arguments.
    // This is necessary for us to ensure that we're not replacing arguments
    // with default values despite no callsites actually passing them in,
    // because for typed functions, the new value might not be compatible
    // with the typed instructions which use the parameter as an operand.
    // The function should be deleted in the future anyway.
    return false;
  }

  unsigned numFormalParam = F->getJSDynamicParams().size();

  // This vector saves the union of all callees for each parameter.
  // Null is used to mark non-literal or diverged parameters. False means
  // that the value has not been set before.
  Literal *undef = builder.getLiteralUndefined();
  llvh::SmallVector<std::pair<Literal *, bool>, 8> args(
      numFormalParam, {undef, false});
  // A list of unused arguments passed by the caller.
  struct UnusedArg {
    BaseCallInst *call;
    unsigned idx;
    /// null if idx doesn't represent a formal arg.
    JSDynamicParam *param;
  };
  llvh::SmallVector<UnusedArg, 8> unusedParams;

  // For each call site:
  for (BaseCallInst *caller : callsites) {
    for (uint32_t i = 0; i < numFormalParam; i++) {
      // Get the arg that matches the i'th parameter. Unpassed parameters are
      // converted into undefs.
      Value *arg = undef;
      if (i < caller->getNumArguments())
        arg = caller->getArgument(i);

      auto *L = llvh::dyn_cast<Literal>(arg);
      if (L) {
        LLVM_DEBUG(
            dbgs() << "-- Found literal for argument " << i << ": "
                   << L->getKindStr() << ".\n");
      }

      // Check if this is the first time we initialize the argument.
      if (!args[i].second) {
        args[i] = {L, true};
        continue;
      }

      // If the i'th argument from different call sites pass different values
      // then mark this argument as invalid.
      Literal *prev = args[i].first;
      if (prev != L) {
        LLVM_DEBUG(
            dbgs() << "-- Found disagreement for argument " << i << "\n");
        LLVM_DEBUG(
            dbgs() << "-- Previous value is "
                   << (prev ? prev->getKindStr() : "nullptr") << ".\n");
        args[i] = {nullptr, true};
      }
    }

    for (unsigned i = 0, e = caller->getNumArguments(); i < e; i++) {
      // Remember which arguments are unused by the callee (parameters with no
      // users and undeclared parameters).
      if (i >= numFormalParam || !F->getJSDynamicParam(i)->hasUsers()) {
        // Defer setting the type of the JSDynamicParam until we actually set
        // the arg to undefined, to keep all the changes in the same place.
        unusedParams.push_back(
            {caller,
             i,
             i < numFormalParam ? F->getJSDynamicParam(i) : nullptr});
      }
    }
  }

  LLVM_DEBUG(dbgs() << "-- Found " << unusedParams.size() << " unused args.\n");

  {
    IRBuilder::InstructionDestroyer destroyer{};
    for (uint32_t paramIdx = 0, e = F->getJSDynamicParams().size();
         paramIdx < e;
         ++paramIdx) {
      JSDynamicParam *P = F->getJSDynamicParam(paramIdx);
      LLVM_DEBUG(
          dbgs() << "-- Inspecting param " << P->getName().str() << ".\n");

      if (Literal *L = args[paramIdx].first) {
        LLVM_DEBUG(
            dbgs() << "-- Found literal " << L->getKindStr() << " for param "
                   << P->getName().str() << ".\n");

        for (auto *I : P->getUsers()) {
          I->replaceAllUsesWith(L);
          destroyer.add(I);
          changed = true;
        }

        NumParamOpt++;
      }
    }
  }

  llvh::DenseSet<Function *> toRedo;

  // Replace all unused arguments with 0 (which is cheap to create).
  Literal *zero = builder.getLiteralPositiveZero();
  for (auto &arg : unusedParams) {
    Value *prevArg = arg.call->getArgument(arg.idx);
    if (!llvh::isa<Literal>(prevArg))
      toRedo.insert(arg.call->getParent()->getParent());

    arg.call->setArgument(zero, arg.idx);
    if (arg.param)
      arg.param->setType(Type::createUint32());
    NumArgsOpt++;
  }

  for (auto *fRedo : toRedo) {
    worklist.push_back(fRedo);
  }

  return changed;
}

bool FuncSigOpts::runOnModule(Module *M) {
  bool changed = false;

  std::vector<Function *> worklist;

  for (auto &F : *M) {
    if (F.isGlobalScope() || M->findCJSModule(&F)) {
      // If the function is a top-level CommonJS module or a global function,
      // skip it.
      continue;
    }

    worklist.push_back(&F);
  }

  // Process all functions.
  while (worklist.size()) {
    Function *F = worklist.back();
    worklist.pop_back();
    changed |= performFSO(F, worklist);
  }

  return changed;
}

Pass *hermes::createFuncSigOpts() {
  return new FuncSigOpts();
}

Pass *hermes::createRecordArgumentPromotion() {
  class RecordArguments final : public ModulePass {
   public:
    RecordArguments() : ModulePass("RecordArgumentPromotion") {}
    bool runOnModule(Module *M) override {
      bool changed = false;
      for (auto &F : *M) {
        if (F.isGlobalScope() || M->findCJSModule(&F) ||
            !F.allCallsitesKnown() || llvh::isa<GeneratorFunction>(&F) ||
            llvh::isa<AsyncFunction>(&F) || capturesArgumentVector(&F))
          continue;
        changed |= promoteRecordParameters(&F);
      }
      return changed;
    }
  };
  return new RecordArguments();
}

#undef DEBUG_TYPE
