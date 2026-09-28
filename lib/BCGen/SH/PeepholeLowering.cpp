/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LoweringPasses.h"
#include "hermes/AST/NativeContext.h"
#include "hermes/BCGen/CommonPeepholeLowering.h"
#include "hermes/IR/Analysis.h"
#include "hermes/IR/CFG.h"
#include "hermes/IR/IRBuilder.h"
#include "hermes/Optimizer/Scalar/Utils.h"
#include "hermes/Support/UTF8.h"

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
    bool changed = optimize_ && hoistGuardedStringLength();
    llvh::SmallVector<BinaryOperatorInst *, 16> comparisons;
    for (auto &BB : *F_) {
      for (auto &I : BB) {
        if (optimize_ &&
            (I.getKind() == ValueKind::BinaryStrictlyEqualInstKind ||
             I.getKind() == ValueKind::BinaryStrictlyNotEqualInstKind))
          comparisons.push_back(llvh::cast<BinaryOperatorInst>(&I));
        if (Value *replaceVal = peep(&I)) {
          if (replaceVal != &I)
            I.replaceAllUsesWith(replaceVal);
          changed = true;
        }
      }
    }
    // Guard insertion splits blocks. Visit only the original comparisons so
    // the retained fallback cannot be specialized repeatedly.
    for (auto *comparison : comparisons) {
      if (lowerStringTrimComparison(comparison)) {
        changed = true;
        continue;
      }
      auto *load = llvh::dyn_cast<LoadPropertyInst>(comparison->getLeftHandSide());
      if (!load)
        load = llvh::dyn_cast<LoadPropertyInst>(comparison->getRightHandSide());
      if (load && lowerSharedStringIndexComparisons(load)) {
        changed = true;
        continue;
      }
      if (auto *result = lowerStringIndexComparison(comparison)) {
        if (result != comparison)
          comparison->replaceAllUsesWith(result);
        changed = true;
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

  bool lowerStringTrimComparison(BinaryOperatorInst *comparison) {
    if (!F_->getContext().getOptimizationSettings().stringTrimComparisons)
      return false;
    auto *call = llvh::dyn_cast<CallInst>(comparison->getLeftHandSide());
    Value *other = comparison->getRightHandSide();
    if (!call) {
      call = llvh::dyn_cast<CallInst>(comparison->getRightHandSide());
      other = comparison->getLeftHandSide();
    }
    if (!call || !call->hasOneUser() || call->getNextNode() != comparison ||
        call->getThis() != other ||
        !call->getNewTarget()->getType().isUndefinedType())
      return false;
    auto *load = llvh::dyn_cast<LoadPropertyInst>(call->getCallee());
    auto *key = load ? llvh::dyn_cast<LiteralString>(load->getProperty()) : nullptr;
    if (!key || key->getValue().str() != "trim")
      return false;

    builder_.setLocation(call->getLocation());
    auto *before = call->getParent();
    auto *join = splitBasicBlock(before, call->getIterator());
    auto *slow = builder_.createBasicBlock(F_);
    builder_.setInsertionBlock(before);
    auto &context = F_->getContext();
    auto &native = context.getNativeContext();
    auto *signature = native.getSignature(
        NativeCType::i32,
        {NativeCType::c_hermes_value, NativeCType::c_hermes_value});
    auto *probe = native.getExtern(
        context.getStringTable().getString("_sh_ljs_try_string_trim_equal"),
        signature, {}, true, nullptr);
    // Preserve the actual method lookup, including a getter's effects. Guard
    // that value rather than inspecting the current String.prototype method.
    auto *status = builder_.createNativeCallInst(
        Type::createNumber(), builder_.getLiteralNativeExtern(probe), signature,
        {call->getCallee(), call->getThis()});
    auto *supported = builder_.createBinaryOperatorInst(
        status, builder_.getLiteralPositiveZero(),
        ValueKind::BinaryGreaterThanOrEqualInstKind);
    auto *equal = builder_.createBinaryOperatorInst(
        status,
        builder_.getLiteralNumber(
            comparison->getKind() == ValueKind::BinaryStrictlyEqualInstKind),
        ValueKind::BinaryStrictlyEqualInstKind);
    builder_.createCondBranchInst(supported, join, slow);
    builder_.setInsertionBlock(slow);
    auto *continuation = builder_.createBranchInst(join);
    call->moveBefore(continuation);
    comparison->moveBefore(continuation);
    builder_.setInsertionPoint(&join->front());
    auto *result = builder_.createPhiInst();
    comparison->replaceAllUsesWith(result);
    result->addEntry(equal, before);
    result->addEntry(comparison, slow);
    return true;
  }

  /// Cache an immutable primitive's length outside a character-scanning loop.
  /// The receiver must already be available at the preheader. Non-string
  /// receivers keep their original property reads, including effects and throws.
  bool hoistGuardedStringLength() {
    if (functionHasTryCatch(F_))
      return false;
    llvh::SmallVector<LoadPropertyInst *, 8> candidates;
    for (auto &BB : *F_) {
      for (auto &I : BB) {
        auto *load = llvh::dyn_cast<LoadPropertyInst>(&I);
        auto *key = load
            ? llvh::dyn_cast<LiteralString>(load->getProperty())
            : nullptr;
        if (key && key->getValue().str() == "length" &&
            F_->getParent()->getTypeContext().canBeString(
                load->getObject()->getType()))
          candidates.push_back(load);
      }
    }
    if (candidates.empty())
      return false;

    DominanceInfo dominance(F_);
    LoopAnalysis loops(F_, dominance);
    for (auto *candidate : candidates) {
      auto *receiver = llvh::dyn_cast<Instruction>(candidate->getObject());
      auto *preheader = loops.getLoopPreheader(candidate->getParent());
      if (!receiver || !preheader ||
          !dominance.properlyDominates(receiver, preheader->getTerminator()))
        continue;

      // Do not add a string path to every array loop. A character comparison
      // supplies a bounded profitability hint, not the semantic proof.
      bool comparesCharacters = false;
      for (auto *use : receiver->getUsers()) {
        auto *indexed = llvh::dyn_cast<LoadPropertyInst>(use);
        if (!indexed || indexed->getObject() != receiver ||
            llvh::isa<LiteralString>(indexed->getProperty()))
          continue;
        for (auto *user : indexed->getUsers()) {
          if (user->getKind() != ValueKind::BinaryStrictlyEqualInstKind &&
              user->getKind() != ValueKind::BinaryStrictlyNotEqualInstKind)
            continue;
          auto *comparison = llvh::cast<BinaryOperatorInst>(user);
          auto *literal = llvh::dyn_cast<LiteralString>(
              comparison->getLeftHandSide() == indexed
                  ? comparison->getRightHandSide()
                  : comparison->getLeftHandSide());
          if (literal && literal->getValue().str().size() == 1)
            comparesCharacters = true;
        }
      }
      if (!comparesCharacters)
        continue;
      while (auto *outer = loops.getLoopPreheader(preheader)) {
        if (!dominance.properlyDominates(receiver, outer->getTerminator()))
          break;
        preheader = outer;
      }

      llvh::SmallVector<LoadPropertyInst *, 8> reads;
      for (auto *load : candidates) {
        if (load->getObject() == receiver && dominance.properlyDominates(
                preheader->getTerminator(), load))
          reads.push_back(load);
      }
      if (reads.empty() || reads.size() > 8)
        continue;

      builder_.setLocation(candidate->getLocation());
      auto *join = splitBasicBlock(
          preheader, preheader->getTerminator()->getIterator());
      auto *stringPath = builder_.createBasicBlock(F_);
      builder_.setInsertionBlock(preheader);
      auto *isString = builder_.createTypeOfIsInst(
          receiver,
          builder_.getLiteralTypeOfIsTypes(TypeOfIsTypes{}.withString(true)));
      builder_.createCondBranchInst(isString, stringPath, join);
      builder_.setInsertionBlock(stringPath);
      // String length is an own, nonconfigurable value and cannot execute user
      // code. Cache a number, not a pointer into movable character storage.
      auto *length = builder_.createLoadPropertyInst(
          receiver, candidate->getProperty());
      length->setType(Type::createNumber());
      builder_.createBranchInst(join);
      builder_.setInsertionPoint(&join->front());
      auto *cached = builder_.createPhiInst();
      cached->addEntry(length, stringPath);
      cached->addEntry(builder_.getLiteralPositiveZero(), preheader);

      for (auto *load : reads) {
        auto *original = load->getParent();
        auto *continuation = splitBasicBlock(original, load->getIterator());
        auto *slow = builder_.createBasicBlock(F_);
        builder_.setInsertionBlock(original);
        builder_.createCondBranchInst(isString, continuation, slow);
        builder_.setInsertionBlock(slow);
        auto *branch = builder_.createBranchInst(continuation);
        load->moveBefore(branch);
        builder_.setInsertionPoint(&continuation->front());
        auto *result = builder_.createPhiInst();
        load->replaceAllUsesWith(result);
        result->addEntry(cached, original);
        result->addEntry(load, slow);
      }
      // Mutations invalidate the loop/dominance analyses. One receiver per
      // function bounds growth and avoids repeatedly rebuilding those analyses.
      return true;
    }
    return false;
  }

  bool lowerSharedStringIndexComparisons(LoadPropertyInst *load) {
    if (!F_->getParent()->getTypeContext().canBeString(load->getObject()->getType()))
      return false;
    if (load->hasOneUser() && load->getNextNode() == load->getUsers().front() &&
        load->getProperty()->getType().isNumberType())
      return false;

    llvh::SmallVector<std::pair<BinaryOperatorInst *, char16_t>, 4> uses;
    for (auto *user : load->getUsers()) {
      if (user->getKind() != ValueKind::BinaryStrictlyEqualInstKind &&
          user->getKind() != ValueKind::BinaryStrictlyNotEqualInstKind)
        return false;
      auto *comparison = llvh::cast<BinaryOperatorInst>(user);
      auto *literal = llvh::dyn_cast<LiteralString>(
          comparison->getLeftHandSide() == load
              ? comparison->getRightHandSide()
              : comparison->getLeftHandSide());
      if (!literal)
        return false;
      auto text = literal->getValue().str();
      if (text.size() > 3)
        return false;
      std::u16string units;
      convertUTF8WithSurrogatesToUTF16(
          std::back_inserter(units), text.data(), text.data() + text.size());
      if (units.size() != 1)
        return false;
      uses.emplace_back(comparison, units[0]);
    }
    if (uses.empty())
      return false;

    // Read exactly once at the original position, before any intervening
    // effects. A non-character result becomes -1, which cannot equal a UTF-16
    // code unit. Comparisons can then share a number across blocks and calls.
    builder_.setInsertionPoint(load);
    builder_.setLocation(load->getLocation());
    auto &context = F_->getContext();
    auto &native = context.getNativeContext();
    auto *signature = native.getSignature(
        NativeCType::c_hermes_value,
        {NativeCType::ptr, NativeCType::c_hermes_value,
         NativeCType::c_hermes_value});
    auto *callee = native.getExtern(
        context.getStringTable().getString("_sh_ljs_get_indexed_char_code"),
        signature, {}, /* declared */ true, /* include */ nullptr);
    auto *code = builder_.createNativeCallInst(
        Type::createNumber(), builder_.getLiteralNativeExtern(callee), signature,
        {builder_.createGetNativeRuntimeInst(), load->getObject(),
         load->getProperty()});
    for (auto [comparison, character] : uses) {
      comparison->setOperand(code, BinaryOperatorInst::LeftHandSideIdx);
      comparison->setOperand(
          builder_.getLiteralNumber(character), BinaryOperatorInst::RightHandSideIdx);
    }
    destroyer_.add(load);
    return true;
  }

  Value *lowerStringIndexComparison(BinaryOperatorInst *comparison) {
    auto *load = llvh::dyn_cast<LoadPropertyInst>(comparison->getLeftHandSide());
    auto *literal =
        llvh::dyn_cast<LiteralString>(comparison->getRightHandSide());
    if (!load || !literal) {
      load = llvh::dyn_cast<LoadPropertyInst>(comparison->getRightHandSide());
      literal = llvh::dyn_cast<LiteralString>(comparison->getLeftHandSide());
    }
    // Fuse before register allocation, and never move a potentially throwing
    // property read across another operation. Other receivers keep their own
    // indexed fast paths rather than paying for a string-specific helper.
    if (!load || !literal || !load->hasOneUser() ||
        load->getNextNode() != comparison ||
        !F_->getParent()->getTypeContext().canBeString(load->getObject()->getType()) ||
        !load->getProperty()->getType().isNumberType())
      return nullptr;

    auto text = literal->getValue().str();
    if (text.size() > 3)
      return nullptr;
    std::u16string units;
    convertUTF8WithSurrogatesToUTF16(
        std::back_inserter(units), text.data(), text.data() + text.size());
    if (units.size() != 1)
      return nullptr;

    builder_.setInsertionPoint(comparison);
    builder_.setLocation(load->getLocation());
    BasicBlock *continuation = nullptr;
    BasicBlock *slow = nullptr;
    BasicBlock *fast = nullptr;
    if (!load->getObject()->getType().isStringType()) {
      auto *original = load->getParent();
      continuation = splitBasicBlock(original, load->getIterator());
      slow = builder_.createBasicBlock(F_);
      fast = builder_.createBasicBlock(F_);
      builder_.setInsertionBlock(original);
      auto *isString = builder_.createTypeOfIsInst(
          load->getObject(),
          builder_.getLiteralTypeOfIsTypes(TypeOfIsTypes{}.withString(true)));
      builder_.createCondBranchInst(isString, fast, slow);
      builder_.setInsertionBlock(fast);
    }
    auto &context = F_->getContext();
    auto &native = context.getNativeContext();
    auto *signature = native.getSignature(
        NativeCType::c_hermes_value,
        {NativeCType::ptr, NativeCType::c_hermes_value, NativeCType::f64,
         NativeCType::u16, NativeCType::u8});
    auto *callee = native.getExtern(
        context.getStringTable().getString("_sh_ljs_string_index_compare"),
        signature,
        {},
        /* declared */ true,
        /* include */ nullptr);
    auto *result = builder_.createNativeCallInst(
        Type::createBoolean(),
        builder_.getLiteralNativeExtern(callee),
        signature,
        {builder_.createGetNativeRuntimeInst(), load->getObject(),
         load->getProperty(), builder_.getLiteralNumber(units[0]),
         builder_.getLiteralNumber(
             comparison->getKind() == ValueKind::BinaryStrictlyNotEqualInstKind)});
    if (continuation) {
      builder_.createBranchInst(continuation);
      builder_.setInsertionBlock(slow);
      auto *branch = builder_.createBranchInst(continuation);
      load->moveBefore(branch);
      comparison->moveBefore(branch);
      builder_.setInsertionPoint(&*continuation->begin());
      auto *phi = builder_.createPhiInst();
      comparison->replaceAllUsesWith(phi);
      phi->addEntry(result, fast);
      phi->addEntry(comparison, slow);
      return comparison;
    }
    destroyer_.add(comparison);
    destroyer_.add(load);
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
