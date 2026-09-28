/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LoweringPasses.h"
#include "hermes/AST/NativeContext.h"
#include "hermes/BCGen/CommonPeepholeLowering.h"
#include "hermes/IR/IRBuilder.h"

namespace hermes::sh {

namespace {

class PeepholeLowering {
  Function *const F_;
  IRBuilder builder_{F_};
  IRBuilder::InstructionDestroyer destroyer_{};
  bool optimize_;

 public:
  explicit PeepholeLowering(Function *F, bool optimize)
      : F_(F), optimize_(optimize) {}

  bool run() {
    bool changed = false;
    for (auto &BB : *F_) {
      for (auto &I : BB) {
        if (Value *replaceVal = peep(&I)) {
          if (replaceVal != &I)
            I.replaceAllUsesWith(replaceVal);
          changed = true;
        }
      }
    }
    return changed;
  }

 private:
  bool reuseAsyncGeneratorResults(CreateFunctionInst *generator) {
    if (!F_->getContext().getOptimizationSettings().asyncGeneratorResults ||
        !generator->hasOneUser() ||
        !generator->getFunctionCode()->hasOneUser())
      return false;

    CreateGeneratorInst *create = nullptr;
    for (auto &BB : *generator->getFunctionCode()) {
      for (auto &I : BB) {
        // A loose arguments object exposes the factory through .callee even
        // when its only explicit IR user is spawnAsync. Direct eval can also
        // observe frame state that is not represented by ordinary SSA users.
        if (llvh::isa<CreateArgumentsLooseInst>(&I) ||
            llvh::isa<DirectEvalInst>(&I))
          return false;
        if (auto *candidate = llvh::dyn_cast<CreateGeneratorInst>(&I)) {
          if (create)
            return false;
          create = candidate;
        }
      }
    }
    if (!create || !create->hasOneUser() ||
        !llvh::isa<ReturnInst>(*create->users_begin()) ||
        !create->getFunctionCode()->hasOneUser())
      return false;
    auto *scope = llvh::dyn_cast<BaseScopeInst>(create->getScope());
    if (!scope)
      return false;
    auto *inner = create->getFunctionCode();
    if (inner->getJSDynamicParams().size() != 3)
      return false;
    llvh::SmallVector<AllocObjectLiteralInst *, 8> results;
    for (auto &BB : *inner) {
      for (auto &I : BB) {
        if (llvh::isa<CreateArgumentsLooseInst>(&I) ||
            llvh::isa<DirectEvalInst>(&I))
          return false;
      }
      auto *ret = llvh::dyn_cast<ReturnInst>(BB.getTerminator());
      if (!ret)
        continue;
      auto *object = llvh::dyn_cast<AllocObjectLiteralInst>(ret->getValue());
      if (!object || !object->hasOneUser() ||
          object->getKeyValuePairCount() != 2 ||
          !llvh::isa<EmptySentinel>(object->getParentObject()))
        return false;
      auto *valueName = llvh::dyn_cast<LiteralString>(object->getKey(0));
      auto *doneName = llvh::dyn_cast<LiteralString>(object->getKey(1));
      if (!valueName || valueName->getValue().str() != "value" ||
          !doneName || doneName->getValue().str() != "done" ||
          !llvh::isa<LiteralBool>(object->getValue(1)))
        return false;
      results.push_back(object);
    }
    if (results.empty())
      return false;

    // Change producer and consumer together, after inlining. The state-machine
    // closure and this record never reach user code or generator prototypes.
    // User arguments were already lifted into the outer generator's scope.
    auto *param = builder_.createJSDynamicParam(
        inner, builder_.createIdentifier("async_result"));
    param->setType(Type::createObject());
    builder_.setInsertionPoint(&inner->front().front());
    auto *record = builder_.createLoadParamInst(param);
    for (auto *object : results) {
      builder_.setInsertionPoint(object);
      builder_.setLocation(object->getLocation());
      for (unsigned i = 0; i < 2; ++i) {
        // The matching startup call supplies a compiler-owned literal object
        // with value/done in slots 0/1. The boolean slot never holds pointers.
        builder_.createPrStoreInst(
            object->getValue(i), record, i, object->getKey(i), i == 1);
      }
      object->replaceAllUsesWith(record);
      destroyer_.add(object);
    }
    builder_.setInsertionPoint(create);
    auto *closure = builder_.createCreateFunctionInst(scope, inner);
    create->replaceAllUsesWith(closure);
    destroyer_.add(create);
    return true;
  }

  Value *lowerAsyncFrameArguments(CallInst *call) {
    if (!optimize_ ||
        !F_->getContext().getOptimizationSettings().asyncFrameArguments ||
        !llvh::isa<AsyncFunction>(F_) || call->getNumArguments() != 4 ||
        !llvh::isa<LiteralUndefined>(call->getNewTarget()))
      return nullptr;
    auto *callee = llvh::dyn_cast<GetBuiltinClosureInst>(call->getCallee());
    auto *args = llvh::dyn_cast<CreateArgumentsInst>(call->getArgument(3));
    auto *generator =
        llvh::dyn_cast<CreateFunctionInst>(call->getArgument(1));
    if (!callee ||
        callee->getBuiltinIndex() != BuiltinMethod::HermesBuiltin_spawnAsync ||
        !args || !args->hasOneUser() || !generator ||
        !llvh::isa<GeneratorFunction>(generator->getFunctionCode()))
      return nullptr;

    // This lowering runs after all inlining, in the original async wrapper.
    // The native entry reads that frame, so it cannot be moved into a caller.
    bool reuseResults = reuseAsyncGeneratorResults(generator);
    builder_.setInsertionPoint(call);
    builder_.setLocation(call->getLocation());
    auto &context = F_->getContext();
    auto &native = context.getNativeContext();
    llvh::SmallVector<NativeCType, 4> parameterTypes{
        NativeCType::ptr, NativeCType::c_hermes_value,
        NativeCType::c_hermes_value};
    llvh::SmallVector<Value *, 4> arguments{
        builder_.createGetNativeRuntimeInst(), generator, call->getArgument(2)};
    if (reuseResults) {
      AllocObjectLiteralInst::ObjectPropertyMap properties{
          {builder_.getLiteralString("value"), builder_.getLiteralUndefined()},
          {builder_.getLiteralString("done"), builder_.getLiteralBool(false)}};
      // LowerAllocObjectLiteral preserves these two non-index keys in order,
      // starting with the empty null-prototype class. Keep ordinary writable
      // fields so the JS driver can clear the payload before suspension.
      auto *record = builder_.createAllocObjectLiteralInst(
          properties, builder_.getLiteralNull());
      parameterTypes.push_back(NativeCType::c_hermes_value);
      arguments.push_back(record);
    }
    auto *signature = native.getSignature(
        NativeCType::c_hermes_value, parameterTypes);
    auto *entry = native.getExtern(
        context.getStringTable().getString(
            reuseResults ? "_sh_ljs_spawn_async_direct_from_frame"
                         : "_sh_ljs_spawn_async_from_frame"),
        signature, {}, /* declared */ true, /* include */ nullptr);
    auto *result = builder_.createNativeCallInst(
        call->getType(), builder_.getLiteralNativeExtern(entry), signature,
        arguments);
    destroyer_.add(call);
    destroyer_.add(args);
    return result;
  }

  /// Perform peephole optimization on the specified instruction. Replacement
  /// instructions need to be created and inserted in the correct position by
  /// using the builder. Instructions for deletion should be inserted in the
  /// destroyer in the correct order (users first). If a change is made, a
  /// non-null value must be returned; if it is different from \p I, it will be
  /// used to replace all uses of \p I.
  Value *peep(Instruction *I) {
    switch (I->getKind()) {
      case ValueKind::CoerceThisNSInstKind: {
        // This transformation is purely an optimization to collapse a sequence
        // of LoadParam + CoerceThisNS into a LoadThisNS, so skip it if
        // optimizations are disabled.
        if (!optimize_)
          return nullptr;
        return lowerCoerceThisNSInst(
            llvh::cast<CoerceThisNSInst>(I), builder_, destroyer_);
      }
      case ValueKind::BinaryExponentiationInstKind: {
        return lowerBinaryExponentiationInst(
            llvh::cast<BinaryOperatorInst>(I), builder_, destroyer_);
      }
      case ValueKind::CallInstKind: {
        if (auto *result = lowerAsyncFrameArguments(llvh::cast<CallInst>(I)))
          return result;
        return stripEnvFromCall(llvh::cast<CallInst>(I), builder_);
      }
      case ValueKind::DebuggerInstKind:
      case ValueKind::EvalCompilationDataInstKind:
        // Instructions aren't supported when compiling to the native
        // backend, so delete it if it was generated so that the lowering
        // doesn't have to deal with it.
        destroyer_.add(I);
        return nullptr;
      default:
        return nullptr;
    }
  }
};

} // namespace

Pass *createPeepholeLowering(bool optimize) {
  class ThisPass : public FunctionPass {
    bool optimize_;

   public:
    explicit ThisPass(bool optimize)
        : FunctionPass("PeepholeLowering"), optimize_(optimize) {}
    bool runOnFunction(Function *F) override {
      return PeepholeLowering(F, optimize_).run();
    }
  };
  return new ThisPass(optimize);
}

} // namespace hermes::sh
