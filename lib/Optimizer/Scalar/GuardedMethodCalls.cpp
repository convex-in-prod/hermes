/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/IR/IRBuilder.h"
#include "hermes/Optimizer/PassManager/Pass.h"
#include "hermes/Optimizer/Scalar/Utils.h"

#include "llvh/ADT/DenseMap.h"

namespace hermes {
namespace {

struct Method {
  CreateFunctionInst *closure;
  CreateScopeInst *scope;
  Value *owner;
  LiteralString *key;
  Variable *capture = nullptr;
};

// Scope creation in the entry block must precede a call in that same block.
// Requiring entry scopes also excludes per-iteration/block environments whose
// creation does not dominate every potential call in the function.
bool precedesCall(Instruction *scope, CallInst *call) {
  if (scope->getParent() != &scope->getFunction()->front())
    return false;
  if (scope->getParent() != call->getParent())
    return true;
  for (auto &inst : *scope->getParent()) {
    if (&inst == scope)
      return true;
    if (&inst == call)
      return false;
  }
  llvm_unreachable("Call must occur in its block");
}

Instruction *accessibleScope(Method &method, CallInst *call) {
  if (method.scope->getFunction() == call->getFunction())
    return precedesCall(method.scope, call) ? method.scope : nullptr;
  for (auto *user : call->getFunction()->getParentScopeParam()->getUsers()) {
    auto *parent = llvh::dyn_cast<GetParentScopeInst>(user);
    if (!parent || !precedesCall(parent, call))
      continue;
    for (auto *scope = parent->getVariableScope(); scope;
         scope = scope->getParentScope())
      if (scope == method.scope->getVariableScope())
        return parent;
  }
  return nullptr;
}

bool run(Module *M) {
  if (!M->getContext().getOptimizationSettings().guardedMethodCalls)
    return false;

  llvh::SmallVector<Method, 32> methods;
  llvh::DenseMap<Function *, Value *> owners;
  llvh::DenseMap<LiteralString *, llvh::SmallVector<unsigned, 2>> byKey;
  for (auto &F : *M) {
    for (auto &BB : F) {
      for (auto &inst : BB) {
        auto *store = llvh::dyn_cast<DefineOwnPropertyInst>(&inst);
        if (!store)
          continue;
        auto *closure = llvh::dyn_cast<CreateFunctionInst>(store->getStoredValue());
        auto *key = llvh::dyn_cast<LiteralString>(store->getProperty());
        if (!closure || !key ||
            closure->getFunctionCode()->getDefinitionKind() !=
                Function::DefinitionKind::ES6Method)
          continue;
        auto *scope = llvh::dyn_cast<CreateScopeInst>(closure->getScope());
        if (!scope || scope->getParent() != &F.front())
          continue;
        auto *target = closure->getFunctionCode();
        owners[target] = store->getObject();
        unsigned size = 0;
        for (auto &block : *target)
          size += block.size();
        if (size > 96 || target->size() > 24)
          continue;
        byKey[key].push_back(methods.size());
        methods.push_back({closure, scope, store->getObject(), key});
      }
    }
  }

  struct Site {
    CallInst *call;
    unsigned method;
    Instruction *scope;
  };
  llvh::SmallVector<Site, 64> sites;
  for (auto &F : *M) {
    unsigned functionSites = 0;
    for (auto &BB : F) {
      for (auto &inst : BB) {
        auto *call = llvh::dyn_cast<CallInst>(&inst);
        if (!call || !call->getNewTarget()->getType().isUndefinedType() ||
            !llvh::isa<EmptySentinel>(call->getTarget()) ||
            functionSites >= 16 || sites.size() >= 512)
          continue;
        auto *load = llvh::dyn_cast<LoadPropertyInst>(call->getCallee());
        auto *key = load ? llvh::dyn_cast<LiteralString>(load->getProperty()) : nullptr;
        if (!key || !byKey.count(key))
          continue;
        llvh::SmallVector<Site, 4> eligible;
        for (auto index : byKey[key]) {
          auto &method = methods[index];
          if (method.closure->getFunctionCode() == &F)
            continue;
          if (auto *scope = accessibleScope(method, call))
            eligible.push_back({call, index, scope});
        }
        // A member's calls on `this` preferentially use its own class's method.
        // Identity is still checked: borrowed methods, shadowing and prototype
        // replacement must use the original indirect call when they differ.
        auto *receiver = llvh::dyn_cast<LoadParamInst>(load->getObject());
        if (receiver && receiver->getParam() == F.getJSDynamicParam(0) &&
            owners.count(&F)) {
          llvh::SmallVector<Site, 4> sameOwner;
          for (auto &site : eligible)
            if (methods[site.method].owner == owners[&F])
              sameOwner.push_back(site);
          if (!sameOwner.empty())
            eligible = std::move(sameOwner);
        }
        // Do not build a chain of speculative guards for ambiguous names.
        if (eligible.size() != 1)
          continue;
        sites.push_back(eligible.front());
        ++functionSites;
      }
    }
  }

  IRBuilder builder{M};
  for (auto &site : sites) {
    auto &method = methods[site.method];
    if (!method.capture) {
      method.capture = builder.createVariable(
          method.scope->getVariableScope(), method.key->getValue(),
          Type::createAnyType(), true);
      builder.setLocation(method.closure->getLocation());
      builder.setInsertionPointAfter(method.scope);
      builder.createStoreFrameInst(
          method.scope, builder.getLiteralUndefined(), method.capture);
      builder.setInsertionPointAfter(method.closure);
      builder.createStoreFrameInst(method.scope, method.closure, method.capture);
    }

    auto *call = site.call;
    auto *F = call->getFunction();
    auto *before = call->getParent();
    auto *join = splitBasicBlock(before, call->getIterator());
    auto *identity = builder.createBasicBlock(F);
    auto *fast = builder.createBasicBlock(F);
    auto *slow = builder.createBasicBlock(F);
    builder.setInsertionBlock(before);
    builder.setLocation(call->getLocation());
    auto *scope = site.scope;
    auto *sourceScope = llvh::cast<BaseScopeInst>(scope)->getVariableScope();
    if (sourceScope != method.capture->getParent())
      scope = builder.createResolveScopeInst(
          method.capture->getParent(), sourceScope, scope);
    auto *expected = builder.createLoadFrameInst(scope, method.capture);
    auto *initialized = builder.createBinaryOperatorInst(
        expected, builder.getLiteralUndefined(),
        ValueKind::BinaryStrictlyNotEqualInstKind);
    builder.createCondBranchInst(initialized, identity, slow);
    builder.setInsertionBlock(identity);
    auto *matches = builder.createBinaryOperatorInst(
        call->getCallee(), expected, ValueKind::BinaryStrictlyEqualInstKind);
    builder.createCondBranchInst(matches, fast, slow);

    builder.setInsertionBlock(fast);
    llvh::SmallVector<Value *, 8> args;
    for (unsigned i = 1; i < call->getNumArguments(); ++i)
      args.push_back(call->getArgument(i));
    auto *direct = builder.createCallInst(
        expected, builder.getLiteralUndefined(), call->getThis(), args);
    direct->setTarget(method.closure->getFunctionCode());
    // This private slot holds only undefined or this method's closure. The
    // initialization guard therefore proves closure identity for direct-call
    // emission as well as inlining. Derive the environment from that closure,
    // never from another factory invocation or from the receiver's class.
    direct->setCalleeIsAlwaysClosure(builder.getLiteralBool(true));
    builder.createBranchInst(join);

    builder.setInsertionBlock(slow);
    auto *branch = builder.createBranchInst(join);
    call->moveBefore(branch);
    builder.setInsertionPoint(&join->front());
    auto *result = builder.createPhiInst();
    call->replaceAllUsesWith(result);
    result->addEntry(direct, fast);
    result->addEntry(call, slow);
  }
  return !sites.empty();
}

} // namespace

Pass *createGuardedMethodCalls() {
  class ThisPass : public ModulePass {
   public:
    ThisPass() : ModulePass("GuardedMethodCalls") {}
    bool runOnModule(Module *M) override {
      return run(M);
    }
  };
  return new ThisPass();
}

} // namespace hermes
