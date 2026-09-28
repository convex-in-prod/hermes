/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LoweringPasses.h"
#include "hermes/IR/Analysis.h"
#include "hermes/IR/IRBuilder.h"
#include "hermes/Optimizer/Scalar/Utils.h"
#include "hermes/Regex/RegexAOT.h"
#include "hermes/Regex/RegexBytecode.h"
#include "hermes/Regex/RegexSerialization.h"
#include "llvh/ADT/STLExtras.h"

namespace hermes::sh {
namespace {

LoadPropertyInst *method(CallInst *call, llvh::StringRef name) {
  if (!call || !llvh::isa<LiteralUndefined>(call->getNewTarget()))
    return nullptr;
  auto *load = llvh::dyn_cast<LoadPropertyInst>(call->getCallee());
  auto *key = load ? llvh::dyn_cast<LiteralString>(load->getProperty()) : nullptr;
  return key && key->getValue().str() == name &&
          load->getObject() == call->getThis() && load->hasOneUser()
      ? load : nullptr;
}

bool onlyUsers(Value *value, llvh::ArrayRef<Instruction *> allowed) {
  for (auto *use : value->getUsers())
    if (!llvh::is_contained(allowed, use))
      return false;
  return true;
}

// Every skipped instruction must belong to the recognized expression. This
// deliberately excludes intervening coercions, getters and calls: resolving
// test before argument effects and exec after them is observable JavaScript.
bool exactRegion(llvh::ArrayRef<Instruction *> instructions) {
  for (size_t i = 1; i < instructions.size(); ++i)
    if (instructions[i - 1]->getNextNode() != instructions[i])
      return false;
  return true;
}

bool availableBefore(Value *value, Instruction *before) {
  auto *inst = llvh::dyn_cast<Instruction>(value);
  return !inst || DominanceInfo(before->getFunction())
                     .properlyDominates(inst, before);
}

bool supportsCharacterOperation(CreateRegExpInst *regexp) {
  if (regexp->getFlags()->getValue().str().contains('y'))
    return false;
  auto &compiled = regexp->getContext().getCompiledRegExp(
      regexp->getPattern()->getValue().getUnderlyingPointer(),
      regexp->getFlags()->getValue().getUnderlyingPointer());
  auto simple = compiled.compileSimpleRegex();
  if (!simple)
    return false;
  const auto *header =
      reinterpret_cast<const regex::SimpleRegexHeader *>(simple->data());
  return header->termCount == 1 && !header->anchoredAtStart &&
      !header->anchoredAtEnd;
}

void replaceRegion(
    Function *function, CreateRegExpInst *regexp,
    llvh::ArrayRef<Instruction *> region, unsigned operation, Value *input,
    Value *index) {
  IRBuilder builder(function);
  auto *first = region.front();
  auto *last = region.back();
  builder.setLocation(first->getLocation());
  auto *before = first->getParent();
  auto *slow = splitBasicBlock(before, first->getIterator());
  auto *join = splitBasicBlock(slow, last->getNextNode()->getIterator());
  builder.setInsertionBlock(before);
  auto *fast = builder.createLIRTryFastRegExpInst(
      regexp->getPattern(), regexp->getFlags(), operation, input, index);
  auto *declined = builder.createTypeOfIsInst(
      fast, builder.getLiteralTypeOfIsTypes(
                TypeOfIsTypes{}.withUndefined(true)));
  builder.createCondBranchInst(declined, slow, join);
  builder.setInsertionBlock(slow);
  builder.createBranchInst(join);
  builder.setInsertionPoint(&join->front());
  auto *result = builder.createPhiInst();
  last->replaceAllUsesWith(result);
  result->addEntry(last, slow);
  result->addEntry(fast, before);
}

bool lowerTest(Function *function, CallInst *call) {
  auto *load = method(call, "test");
  auto *regexp = llvh::dyn_cast<CreateRegExpInst>(call->getThis());
  if (!load || !regexp || call->getNumArguments() != 2 ||
      !onlyUsers(regexp, {load, call}) || !supportsCharacterOperation(regexp))
    return false;
  IRBuilder builder(function);
  auto *input = call->getArgument(1);
  auto *index = llvh::dyn_cast<LoadPropertyInst>(input);
  if (index && index->hasOneUser()) {
    llvh::SmallVector<Instruction *, 4> region;
    // Both source orders occur after inlining. The runtime admits only own
    // in-bounds string indices, so the fast path cannot invoke a prototype.
    if (index->getNextNode() == regexp)
      region = {index, regexp, load, call};
    else
      region = {regexp, load, index, call};
    if (exactRegion(region) && availableBefore(index->getObject(), region[0]) &&
        availableBefore(index->getProperty(), region[0])) {
      replaceRegion(function, regexp, region, HermesRegexTestIndexed,
                    index->getObject(), index->getProperty());
      return true;
    }
  }
  if (!exactRegion({regexp, load, call}) || !availableBefore(input, regexp))
    return false;
  replaceRegion(function, regexp, {regexp, load, call}, HermesRegexTest,
                input, builder.getLiteralUndefined());
  return true;
}

bool lowerReplace(Function *function, CallInst *call) {
  auto *load = method(call, "replace");
  if (!load || call->getNumArguments() != 3)
    return false;
  auto *regexp = llvh::dyn_cast<CreateRegExpInst>(call->getArgument(1));
  auto *replacement = llvh::dyn_cast<LiteralString>(call->getArgument(2));
  if (!regexp || !regexp->hasOneUser() || !replacement ||
      !replacement->getValue().str().empty() ||
      !regexp->getFlags()->getValue().str().contains('g') ||
      regexp->getFlags()->getValue().str().contains('y') ||
      !supportsCharacterOperation(regexp))
    return false;

  Value *input = call->getThis();
  llvh::SmallVector<Instruction *, 6> region{load, regexp, call};
  unsigned operation = HermesRegexReplaceEmpty;
  auto *lower = llvh::dyn_cast<CallInst>(input);
  auto *lowerMethod = method(lower, "toLowerCase");
  if (lowerMethod && lower->getNumArguments() == 1 &&
      onlyUsers(lower, {load, call}) &&
      exactRegion({lowerMethod, lower, load, regexp, call})) {
    region.insert(region.begin(), {lowerMethod, lower});
    input = lower->getThis();
    operation = HermesRegexLowerReplaceEmpty;
  }
  if (!exactRegion(region) || !availableBefore(input, region.front()))
    return false;

  // An equality consumer needs neither the filtered string nor an early-exit
  // scan: the final successful match still determines the legacy RegExp state.
  auto *comparison = llvh::dyn_cast<BinaryOperatorInst>(call->getNextNode());
  if (comparison && call->hasOneUser() &&
      comparison->getKind() == ValueKind::BinaryStrictlyEqualInstKind &&
      ((comparison->getLeftHandSide() == call &&
        comparison->getRightHandSide() == input) ||
       (comparison->getRightHandSide() == call &&
        comparison->getLeftHandSide() == input))) {
    region.push_back(comparison);
    operation = operation == HermesRegexLowerReplaceEmpty
        ? HermesRegexLowerReplaceEmptyEqual : HermesRegexReplaceEmptyEqual;
  }
  IRBuilder builder(function);
  replaceRegion(function, regexp, region, operation, input,
                builder.getLiteralUndefined());
  return true;
}

class RegExpLowering : public FunctionPass {
 public:
  RegExpLowering() : FunctionPass("RegExpLowering") {}
  bool runOnFunction(Function *function) override {
    llvh::SmallVector<CallInst *, 16> calls;
    for (auto &block : *function)
      for (auto &inst : block)
        if (auto *call = llvh::dyn_cast<CallInst>(&inst))
          calls.push_back(call);
    bool changed = false;
    for (auto *call : calls)
      changed |= lowerTest(function, call) || lowerReplace(function, call);
    return changed;
  }
};
} // namespace

Pass *createRegExpLowering() {
  return new RegExpLowering();
}
} // namespace hermes::sh
