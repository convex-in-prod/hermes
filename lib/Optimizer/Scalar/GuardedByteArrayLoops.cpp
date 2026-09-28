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

#include "llvh/ADT/DenseMap.h"
#include "llvh/ADT/SmallPtrSet.h"

#include <cmath>

STATISTIC(NumByteArrayLoops, "Numeric loops specialized for guarded byte arrays");

namespace hermes {
namespace {

struct ByteLoop {
  BasicBlock *body;
  BasicBlock *preheader;
  BasicBlock *exit;
  PhiInst *index;
  Value *start;
  Value *limit;
  Value *receiver{nullptr};
  unsigned step;
  unsigned maxOffset{0};
};

static bool availableBefore(Value *value, Instruction *before,
                            const DominanceInfo &dominance) {
  auto *inst = llvh::dyn_cast<Instruction>(value);
  return !inst || dominance.properlyDominates(inst, before);
}

static OptValue<unsigned> boundedInteger(Value *value) {
  auto *literal = llvh::dyn_cast<LiteralNumber>(value);
  if (!literal)
    return llvh::None;
  double number = literal->getValue();
  if (!(number >= 0 && number <= 256) || std::floor(number) != number)
    return llvh::None;
  return static_cast<unsigned>(number);
}

static OptValue<ByteLoop> findLoop(BasicBlock *body,
                                  const DominanceInfo &dominance) {
  if (body->size() > 128 || pred_count(body) != 2)
    return llvh::None;
  auto *back = llvh::dyn_cast<CondBranchInst>(body->getTerminator());
  if (!back || back->getTrueDest() != body || back->getFalseDest() == body)
    return llvh::None;
  BasicBlock *preheader = nullptr;
  for (auto *pred : predecessors(body))
    if (pred != body)
      preheader = pred;
  if (!preheader)
    return llvh::None;
  auto *entry = llvh::dyn_cast<CondBranchInst>(preheader->getTerminator());
  if (!entry || entry->getTrueDest() != body ||
      entry->getFalseDest() != back->getFalseDest())
    return llvh::None;
  auto *before = llvh::dyn_cast<FCompareInst>(entry->getCondition());
  auto *after = llvh::dyn_cast<FCompareInst>(back->getCondition());
  if (!before || !after ||
      before->getKind() != ValueKind::FLessThanInstKind ||
      after->getKind() != ValueKind::FLessThanInstKind ||
      after->getParent() != body ||
      before->getRight() != after->getRight())
    return llvh::None;
  auto *increment = llvh::dyn_cast<FBinaryMathInst>(after->getLeft());
  if (!increment || increment->getKind() != ValueKind::FAddInstKind ||
      increment->getParent() != body)
    return llvh::None;
  auto *index = llvh::dyn_cast<PhiInst>(increment->getLeft());
  auto step = boundedInteger(increment->getRight());
  if (!index || index->getParent() != body || !step || !*step ||
      index->getNumEntries() != 2)
    return llvh::None;
  for (unsigned i = 0; i < 2; ++i) {
    auto incoming = index->getEntry(i);
    if (incoming.second == body ? incoming.first != increment
        : incoming.second != preheader || incoming.first != before->getLeft())
      return llvh::None;
  }
  ByteLoop loop{body, preheader, back->getFalseDest(), index,
                before->getLeft(), before->getRight(), nullptr, *step};
  if (!loop.start->getType().isNumberType() ||
      !loop.limit->getType().isNumberType() ||
      !availableBefore(loop.start, entry, dominance) ||
      !availableBefore(loop.limit, entry, dominance))
    return llvh::None;

  // Phi assumptions are inductive: each initial value and each backedge value
  // must be numeric using only the admitted reads and effect-free operations.
  llvh::SmallPtrSet<Value *, 32> numbers;
  auto numeric = [&](Value *value) {
    auto *inst = llvh::dyn_cast<Instruction>(value);
    return inst && inst->getParent() == body ? numbers.count(value) != 0
        : value->getType().isNumberType() &&
            availableBefore(value, entry, dominance);
  };
  for (auto &inst : *body) {
    auto *phi = llvh::dyn_cast<PhiInst>(&inst);
    if (!phi)
      break;
    if (phi->getNumEntries() != 2 || !phi->getType().isNumberType())
      return llvh::None;
    bool initial = false, backedge = false;
    for (unsigned i = 0; i < 2; ++i) {
      auto incoming = phi->getEntry(i);
      if (incoming.second == preheader) {
        if (!incoming.first->getType().isNumberType() ||
            !availableBefore(incoming.first, entry, dominance))
          return llvh::None;
        initial = true;
      } else if (incoming.second == body) {
        backedge = true;
      }
    }
    if (!initial || !backedge)
      return llvh::None;
    numbers.insert(phi);
  }

  for (auto &inst : *body) {
    // Every escaping value needs an edge-specific merge for the cloned loop.
    for (auto *user : inst.getUsers()) {
      if (user->getParent() == body)
        continue;
      auto *phi = llvh::dyn_cast<PhiInst>(user);
      if (!phi || phi->getParent() != loop.exit)
        return llvh::None;
      for (unsigned i = 0; i < phi->getNumEntries(); ++i) {
        auto incoming = phi->getEntry(i);
        if (incoming.first == &inst && incoming.second != body)
          return llvh::None;
      }
    }
    if (llvh::isa<PhiInst>(&inst) || &inst == back)
      continue;
    if (&inst == after) {
      if (!numeric(after->getLeft()) || !numeric(after->getRight()))
        return llvh::None;
      continue;
    }
    if (auto *load = llvh::dyn_cast<LoadPropertyInst>(&inst)) {
      if (!availableBefore(load->getObject(), entry, dominance) ||
          (loop.receiver && loop.receiver != load->getObject()))
        return llvh::None;
      unsigned offset = 0;
      if (load->getProperty() != index) {
        auto *add = llvh::dyn_cast<FBinaryMathInst>(load->getProperty());
        if (!add || add->getKind() != ValueKind::FAddInstKind ||
            add->getParent() != body || add->getLeft() != index)
          return llvh::None;
        auto literal = boundedInteger(add->getRight());
        if (!literal)
          return llvh::None;
        offset = *literal;
      }
      loop.receiver = load->getObject();
      loop.maxOffset = std::max(loop.maxOffset, offset);
    } else if (auto *call = llvh::dyn_cast<CallBuiltinInst>(&inst)) {
      if (call->getBuiltinIndex() != BuiltinMethod::Math_imul ||
          call->getNumArguments() != 3 ||
          !numeric(call->getArgument(1)) || !numeric(call->getArgument(2)))
        return llvh::None;
    } else {
      // Output Number alone is insufficient: bitwise operations and AsUint32
      // can invoke user coercions when any input is not proven numeric.
      switch (inst.getKind()) {
        case ValueKind::FAddInstKind:
        case ValueKind::FSubtractInstKind:
        case ValueKind::FMultiplyInstKind:
        case ValueKind::FDivideInstKind:
        case ValueKind::FModuloInstKind:
        case ValueKind::FNegateKind:
        case ValueKind::AsUint32InstKind:
        case ValueKind::AsInt32InstKind:
        case ValueKind::BinaryAndInstKind:
        case ValueKind::BinaryOrInstKind:
        case ValueKind::BinaryXorInstKind:
        case ValueKind::BinaryLeftShiftInstKind:
        case ValueKind::BinaryRightShiftInstKind:
        case ValueKind::BinaryUnsignedRightShiftInstKind:
          break;
        default:
          return llvh::None;
      }
      for (unsigned i = 0; i < inst.getNumOperands(); ++i)
        if (!numeric(inst.getOperand(i)))
          return llvh::None;
    }
    numbers.insert(&inst);
  }
  if (!loop.receiver)
    return llvh::None;
  for (auto &inst : *body) {
    auto *phi = llvh::dyn_cast<PhiInst>(&inst);
    if (!phi)
      break;
    for (unsigned i = 0; i < 2; ++i)
      if (!numeric(phi->getEntry(i).first))
        return llvh::None;
  }
  return loop;
}

static void specialize(Function *function, const ByteLoop &loop) {
  IRBuilder builder(function);
  auto &context = function->getContext();
  auto &native = context.getNativeContext();
  auto *probeSignature = native.getSignature(
      NativeCType::c_bool,
      {NativeCType::ptr, NativeCType::c_hermes_value, NativeCType::c_double,
       NativeCType::c_double, NativeCType::c_uint, NativeCType::c_uint});
  auto *probe = native.getExtern(
      context.getStringTable().getString("_sh_can_read_byte_array_loop"),
      probeSignature, {}, true, nullptr);
  auto *readSignature = native.getSignature(
      NativeCType::c_uchar,
      {NativeCType::ptr, NativeCType::c_hermes_value, NativeCType::c_double});
  auto *read = native.getExtern(
      context.getStringTable().getString("_sh_read_byte_array_element"),
      readSignature, {}, true, nullptr);
  auto *guard = builder.createBasicBlock(function);
  auto *fast = builder.createBasicBlock(function);
  builder.setInsertionBlock(guard);
  builder.setLocation(loop.body->getTerminator()->getLocation());
  auto *runtime = builder.createGetNativeRuntimeInst();
  auto *condition = builder.createNativeCallInst(
      Type::createNumber(), builder.getLiteralNativeExtern(probe), probeSignature,
      {runtime, loop.receiver, loop.start, loop.limit,
       builder.getLiteralNumber(loop.step),
       builder.getLiteralNumber(loop.maxOffset)});
  builder.createCondBranchInst(condition, fast, loop.body);

  llvh::DenseMap<Value *, Value *> copies;
  copies[loop.body] = fast;
  copies[loop.preheader] = guard;
  auto mapped = [&](Value *value) -> Value * {
    auto found = copies.find(value);
    return found == copies.end() ? value : found->second;
  };
  builder.setInsertionBlock(fast);
  // Create every phi before translating backedges or mutually dependent phis.
  for (auto &inst : *loop.body) {
    auto *phi = llvh::dyn_cast<PhiInst>(&inst);
    if (!phi)
      break;
    auto *copy = builder.createPhiInst();
    copy->setType(phi->getType());
    copies[phi] = copy;
  }
  for (auto &inst : *loop.body) {
    if (llvh::isa<PhiInst>(&inst))
      continue;
    builder.setLocation(inst.getLocation());
    if (auto *load = llvh::dyn_cast<LoadPropertyInst>(&inst)) {
      copies[load] = builder.createNativeCallInst(
          Type::createNumber(), builder.getLiteralNativeExtern(read),
          readSignature, {runtime, loop.receiver, mapped(load->getProperty())});
    } else {
      llvh::SmallVector<Value *, 8> operands;
      for (unsigned i = 0; i < inst.getNumOperands(); ++i)
        operands.push_back(mapped(inst.getOperand(i)));
      copies[&inst] = builder.cloneInst(&inst, operands);
    }
  }
  for (auto &inst : *loop.body) {
    auto *phi = llvh::dyn_cast<PhiInst>(&inst);
    if (!phi)
      break;
    auto *copy = llvh::cast<PhiInst>(copies[phi]);
    for (unsigned i = 0; i < 2; ++i) {
      auto incoming = phi->getEntry(i);
      copy->addEntry(mapped(incoming.first),
                     llvh::cast<BasicBlock>(mapped(incoming.second)));
      if (incoming.second == loop.preheader)
        phi->updateEntry(i, incoming.first, guard);
    }
  }
  // Include invariant exit values too: all exit phis must cover the new edge.
  for (auto &inst : *loop.exit) {
    auto *phi = llvh::dyn_cast<PhiInst>(&inst);
    if (!phi)
      break;
    unsigned count = phi->getNumEntries();
    for (unsigned i = 0; i < count; ++i) {
      auto incoming = phi->getEntry(i);
      if (incoming.second == loop.body)
        phi->addEntry(mapped(incoming.first), fast);
    }
  }
  loop.preheader->getTerminator()->setOperand(guard, CondBranchInst::TrueBlockIdx);
  ++NumByteArrayLoops;
}

static bool run(Function *function) {
  const auto &settings = function->getContext().getOptimizationSettings();
  if (!settings.staticHermesInlining || !settings.guardedByteArrayLoops ||
      functionHasTryCatch(function))
    return false;
  unsigned count = 0;
  for (auto &block : *function) {
    count += block.size();
    if (count > 4096)
      return false;
  }
  DominanceInfo dominance(function);
  llvh::SmallVector<ByteLoop, 4> loops;
  for (auto &block : *function) {
    auto loop = findLoop(&block, dominance);
    if (loop)
      loops.push_back(*loop);
    if (loops.size() == 4)
      break;
  }
  for (const auto &loop : loops)
    specialize(function, loop);
  return !loops.empty();
}

} // namespace

Pass *createGuardedByteArrayLoops() {
  class ThisPass final : public FunctionPass {
   public:
    ThisPass() : FunctionPass("GuardedByteArrayLoops") {}
    bool runOnFunction(Function *function) override {
      return run(function);
    }
  };
  return new ThisPass();
}

} // namespace hermes
