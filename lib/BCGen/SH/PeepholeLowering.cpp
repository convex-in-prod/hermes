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
    bool changed = false;
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
