/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/Optimizer/Scalar/RecordUseAnalysis.h"
#include "hermes/IR/IRBuilder.h"
#include "hermes/IR/Instrs.h"
#include "hermes/Optimizer/PassManager/Pass.h"
#include "gtest/gtest.h"

#include <algorithm>

using namespace hermes;

namespace {
class RecordUseAnalysisTest : public ::testing::Test {
 protected:
  std::shared_ptr<Context> context = std::make_shared<Context>();
  Module module{context};
  IRBuilder builder{&module};
  RecordUseAnalysis analysis;

  Function *function(const char *name) {
    auto *fn = builder.createFunction(
        name, Function::DefinitionKind::ES5Function, true);
    builder.createJSThisParam(fn);
    builder.createJSDynamicParam(fn, "record");
    builder.setInsertionBlock(builder.createBasicBlock(fn));
    return fn;
  }

  CallInst *call(Function *fn, Value *arg) {
    return builder.createCallInst(
        builder.getLiteralUndefined(), fn, true, builder.getEmptySentinel(),
        builder.getLiteralUndefined(), builder.getLiteralUndefined(), {arg});
  }

  static unsigned count(const RecordUses &uses, RecordUseKind kind) {
    return std::count_if(uses.uses.begin(), uses.uses.end(),
                         [kind](const auto &use) { return use.kind == kind; });
  }
};

TEST_F(RecordUseAnalysisTest, ReturnedAliasBelongsToThisCall) {
  auto *identity = function("identity");
  auto *arg = builder.createLoadParamInst(identity->getJSDynamicParam(1));
  builder.createReturnInst(arg);
  function("caller");
  auto *first = builder.createAllocObjectLiteralInst();
  auto *second = builder.createAllocObjectLiteralInst();
  auto *returned = call(identity, first);
  builder.createLoadPropertyInst(returned, builder.getLiteralString("value"));
  auto *other = call(identity, second);
  builder.createReturnInst(other);

  const auto uses = analysis.analyze(first);
  EXPECT_TRUE(uses.complete);
  EXPECT_FALSE(uses.returnsDirectAlias);
  EXPECT_EQ(1u, count(uses, RecordUseKind::Read));
  EXPECT_EQ(0u, count(uses, RecordUseKind::Return));
  const auto otherUses = analysis.analyze(second);
  EXPECT_TRUE(otherUses.returnsDirectAlias);
  EXPECT_EQ(0u, count(otherUses, RecordUseKind::Read));
}

TEST_F(RecordUseAnalysisTest, ForwardedContainmentIsNotAReadOnlyUse) {
  auto *pack = function("pack");
  auto *arg = builder.createLoadParamInst(pack->getJSDynamicParam(1));
  auto *container = builder.createAllocObjectLiteralInst();
  auto *key = builder.getLiteralString("child");
  builder.createDefineOwnPropertyInst(
      arg, container, key, IRBuilder::PropEnumerable::Yes);
  builder.createReturnInst(container);
  function("caller");
  auto *record = builder.createAllocObjectLiteralInst();
  auto *result = call(pack, record);
  builder.createReturnInst(result);

  const auto uses = analysis.analyze(record);
  EXPECT_TRUE(uses.complete);
  EXPECT_FALSE(uses.returnsDirectAlias);
  EXPECT_EQ(1u, count(uses, RecordUseKind::StoredAsChild));
  for (const auto &use : uses.uses)
    if (use.kind == RecordUseKind::StoredAsChild) {
      EXPECT_EQ(pack, use.site->getFunction());
      EXPECT_EQ(key, use.property);
    }
}

TEST_F(RecordUseAnalysisTest, RecursionDoesNotProduceOptimisticSummary) {
  auto *recursive = function("recursive");
  auto *arg = builder.createLoadParamInst(recursive->getJSDynamicParam(1));
  builder.createReturnInst(call(recursive, arg));
  const auto &uses = analysis.analyzeParameter(recursive->getJSDynamicParam(1));
  EXPECT_FALSE(uses.complete);
  EXPECT_EQ(1u, count(uses, RecordUseKind::Recursive));
  EXPECT_FALSE(uses.returnsDirectAlias);
}

TEST_F(RecordUseAnalysisTest, JoinRetainsPossibleAliasAndMutation) {
  auto *fn = function("join");
  auto *record = builder.createAllocObjectLiteralInst();
  auto *other = builder.createAllocObjectLiteralInst();
  auto *left = builder.createBasicBlock(fn);
  auto *right = builder.createBasicBlock(fn);
  auto *join = builder.createBasicBlock(fn);
  builder.createCondBranchInst(builder.getLiteralBool(true), left, right);
  builder.setInsertionBlock(left);
  builder.createBranchInst(join);
  builder.setInsertionBlock(right);
  builder.createBranchInst(join);
  builder.setInsertionBlock(join);
  auto *phi = builder.createPhiInst();
  phi->addEntry(record, left);
  phi->addEntry(other, right);
  builder.createStorePropertyLooseInst(
      builder.getLiteralNumber(3), phi, builder.getLiteralString("value"));
  builder.createReturnInst(phi);
  const auto uses = analysis.analyze(record);
  EXPECT_TRUE(uses.complete);
  EXPECT_TRUE(uses.returnsDirectAlias);
  EXPECT_EQ(1u, count(uses, RecordUseKind::Write));
}

TEST_F(RecordUseAnalysisTest, UnknownCallRequiresAnExplicitBoundary) {
  function("caller");
  auto *record = builder.createAllocObjectLiteralInst();
  builder.createCallInst(
      builder.getLiteralUndefined(), builder.getLiteralUndefined(),
      builder.getLiteralUndefined(), {record});
  builder.createReturnInst(builder.getLiteralUndefined());
  const auto uses = analysis.analyze(record);
  EXPECT_FALSE(uses.complete);
  EXPECT_EQ(1u, count(uses, RecordUseKind::UnknownCall));
}

TEST_F(RecordUseAnalysisTest, ImplicitArgumentsAreNotUnusedParameters) {
  auto *fn = function("indirect");
  auto *arguments = builder.createCreateArgumentsStrictInst();
  builder.createReturnInst(arguments);
  function("caller");
  auto *record = builder.createAllocObjectLiteralInst();
  builder.createReturnInst(call(fn, record));
  const auto uses = analysis.analyze(record);
  EXPECT_FALSE(uses.complete);
  EXPECT_EQ(1u, count(uses, RecordUseKind::Unsupported));
}

TEST_F(RecordUseAnalysisTest, CapturedReturnIsNotTheEnclosingReturn) {
  auto *outer = function("outer");
  auto *vs = builder.createVariableScope(nullptr);
  auto *scope = builder.createCreateScopeInst(vs, builder.getEmptySentinel());
  auto *variable = builder.createVariable(vs, "saved", Type::createAnyType(), false);
  auto *record = builder.createAllocObjectLiteralInst();
  builder.createStoreFrameInst(scope, record, variable);
  auto *nested = function("nested");
  auto *parent = builder.createGetParentScopeInst(vs, nested->getParentScopeParam());
  builder.createReturnInst(builder.createLoadFrameInst(parent, variable));
  builder.setInsertionBlock(&outer->front());
  builder.createReturnInst(builder.createCreateFunctionInst(scope, nested));

  const auto uses = analysis.analyze(record);
  EXPECT_TRUE(uses.complete);
  EXPECT_FALSE(uses.returnsDirectAlias);
  EXPECT_EQ(1u, count(uses, RecordUseKind::Capture));
  EXPECT_EQ(1u, count(uses, RecordUseKind::Observe));
}

TEST_F(RecordUseAnalysisTest, ReassignedCaptureIsIncomplete) {
  function("caller");
  auto *vs = builder.createVariableScope(nullptr);
  auto *scope = builder.createCreateScopeInst(vs, builder.getEmptySentinel());
  auto *variable = builder.createVariable(vs, "saved", Type::createAnyType(), false);
  auto *record = builder.createAllocObjectLiteralInst();
  builder.createStoreFrameInst(scope, record, variable);
  builder.createStoreFrameInst(scope, builder.getLiteralUndefined(), variable);
  builder.createReturnInst(builder.createLoadFrameInst(scope, variable));
  const auto uses = analysis.analyze(record);
  EXPECT_FALSE(uses.complete);
  EXPECT_EQ(1u, count(uses, RecordUseKind::Unsupported));
}

TEST_F(RecordUseAnalysisTest, BudgetExhaustionIsNotASafetyProof) {
  function("caller");
  auto *record = builder.createAllocObjectLiteralInst();
  for (unsigned i = 0; i < 4; ++i)
    builder.createLoadPropertyInst(record, builder.getLiteralString("value"));
  builder.createReturnInst(builder.getLiteralUndefined());
  RecordUseAnalysis bounded({2, 2, 2, 2});
  const auto uses = bounded.analyze(record);
  EXPECT_FALSE(uses.complete);
  EXPECT_EQ(1u, count(uses, RecordUseKind::Limit));
}

TEST_F(RecordUseAnalysisTest, ConditionalConstructionPreservesPresence) {
  auto *fn = function("producer");
  auto *baseKey = builder.getLiteralString("base");
  auto *optionalKey = builder.getLiteralString("optional");
  auto *lastKey = builder.getLiteralString("last");
  auto *record = builder.createAllocObjectLiteralInst(
      {{baseKey, builder.getLiteralUndefined()}});
  auto *branch = builder.createBasicBlock(fn);
  auto *end = builder.createBasicBlock(fn);
  auto *condition = builder.createLoadParamInst(fn->getJSDynamicParam(1));
  builder.createCondBranchInst(condition, branch, end);
  builder.setInsertionBlock(branch);
  builder.createDefineOwnPropertyInst(
      builder.getLiteralUndefined(), record, optionalKey,
      IRBuilder::PropEnumerable::Yes);
  builder.createBranchInst(end);
  builder.setInsertionBlock(end);
  builder.createDefineOwnPropertyInst(
      builder.getLiteralUndefined(), record, lastKey,
      IRBuilder::PropEnumerable::Yes);
  builder.createReturnInst(record);

  auto construction = analysis.describeReturnedConstruction(record);
  EXPECT_EQ(nullptr, construction.boundary);
  ASSERT_EQ(3u, construction.fields.size());
  for (const auto &field : construction.fields) {
    EXPECT_EQ(field.key != optionalKey, field.definitelyOwnOnReturn);
    EXPECT_EQ(field.key == baseKey, field.initialOrdinal.hasValue());
  }
}

TEST_F(RecordUseAnalysisTest, ContainmentRequiresASeparateLayoutProof) {
  function("producer");
  auto *record = builder.createAllocObjectLiteralInst();
  auto *container = builder.createAllocObjectLiteralInst(
      {{builder.getLiteralString("one"), record},
       {builder.getLiteralString("two"), record}});
  builder.createReturnInst(record);
  const auto uses = analysis.analyze(record);
  EXPECT_EQ(2u, count(uses, RecordUseKind::StoredAsChild));
  const auto construction = analysis.describeReturnedConstruction(record);
  EXPECT_EQ(container, construction.boundary);
}

TEST_F(RecordUseAnalysisTest, ReturnLayoutsKeepPresenceSeparateFromSlot) {
  auto *fn = function("layouts");
  auto *arg = builder.createLoadParamInst(fn->getJSDynamicParam(1));
  auto *left = builder.createBasicBlock(fn);
  auto *right = builder.createBasicBlock(fn);
  auto *base = builder.getLiteralString("base");
  auto *optional = builder.getLiteralString("optional");
  auto *tail = builder.getLiteralString("tail");
  builder.createCondBranchInst(arg, left, right);
  builder.setInsertionBlock(left);
  auto *first = builder.createAllocObjectLiteralInst(
      {{base, arg}, {optional, builder.getLiteralUndefined()}, {tail, arg}});
  builder.createReturnInst(first);
  builder.setInsertionBlock(right);
  builder.createReturnInst(builder.createAllocObjectLiteralInst(
      {{base, arg}, {tail, arg}}));

  RecordReturnAnalysis returned;
  const auto &summary = returned.analyze(fn);
  ASSERT_EQ(ReturnedRecord::Status::Complete, summary.status);
  ASSERT_EQ(3u, summary.fields.size());
  for (const auto &field : summary.fields) {
    EXPECT_EQ(field.key != optional, field.definitelyOwn);
    EXPECT_EQ(field.key == base, field.ordinal.hasValue());
    if (field.key != optional) {
      EXPECT_EQ(fn->getJSDynamicParam(1), field.value);
    }
  }
}

TEST_F(RecordUseAnalysisTest, ReturnForwardingSubstitutesEachCallArgument) {
  auto *producer = function("producer");
  auto *arg = builder.createLoadParamInst(producer->getJSDynamicParam(1));
  builder.createReturnInst(builder.createAllocObjectLiteralInst(
      {{builder.getLiteralString("value"), arg}}));
  auto *forward = function("forward");
  auto *other = builder.createJSDynamicParam(forward, "other");
  auto *value = builder.createLoadParamInst(other);
  builder.createReturnInst(call(producer, value));
  auto *constant = function("constant");
  builder.createReturnInst(call(producer, builder.getLiteralNumber(17)));

  RecordReturnAnalysis returned;
  const auto &forwarded = returned.analyze(forward);
  ASSERT_EQ(ReturnedRecord::Status::Complete, forwarded.status);
  ASSERT_EQ(1u, forwarded.fields.size());
  EXPECT_EQ(other, forwarded.fields.front().value);
  const auto &fixed = returned.analyze(constant);
  ASSERT_EQ(ReturnedRecord::Status::Complete, fixed.status);
  EXPECT_EQ(builder.getLiteralNumber(17), fixed.fields.front().value);
}

TEST_F(RecordUseAnalysisTest, ReturnedPhiJoinsLiteralFields) {
  auto *fn = function("joined");
  auto *left = builder.createBasicBlock(fn);
  auto *right = builder.createBasicBlock(fn);
  auto *join = builder.createBasicBlock(fn);
  builder.createCondBranchInst(builder.getLiteralBool(true), left, right);
  auto *key = builder.getLiteralString("value");
  builder.setInsertionBlock(left);
  auto *a = builder.createAllocObjectLiteralInst({{key, builder.getLiteralNumber(1)}});
  builder.createBranchInst(join);
  builder.setInsertionBlock(right);
  auto *b = builder.createAllocObjectLiteralInst({{key, builder.getLiteralNumber(2)}});
  builder.createBranchInst(join);
  builder.setInsertionBlock(join);
  auto *phi = builder.createPhiInst();
  phi->addEntry(a, left);
  phi->addEntry(b, right);
  builder.createReturnInst(phi);
  RecordReturnAnalysis returned;
  const auto &summary = returned.analyze(fn);
  ASSERT_EQ(ReturnedRecord::Status::Complete, summary.status);
  ASSERT_EQ(1u, summary.fields.size());
  EXPECT_TRUE(summary.fields.front().definitelyOwn);
  EXPECT_EQ(0u, summary.fields.front().ordinal.getValue());
  EXPECT_TRUE(summary.fields.front().type.isNumberType());
  EXPECT_EQ(nullptr, summary.fields.front().value);
}

TEST_F(RecordUseAnalysisTest, BranchStoresProveTheValueAtTheirPhiEdges) {
  auto *fn = function("joinedStores");
  auto *arg = builder.createLoadParamInst(fn->getJSDynamicParam(1));
  auto *left = builder.createBasicBlock(fn);
  auto *right = builder.createBasicBlock(fn);
  auto *join = builder.createBasicBlock(fn);
  builder.createCondBranchInst(arg, left, right);
  auto *key = builder.getLiteralString("value");
  builder.setInsertionBlock(left);
  auto *a = builder.createAllocObjectLiteralInst({{key, builder.getLiteralNull()}});
  builder.createPrStoreInst(arg, a, 0, key, false);
  builder.createBranchInst(join);
  builder.setInsertionBlock(right);
  auto *b = builder.createAllocObjectLiteralInst({{key, builder.getLiteralNull()}});
  builder.createPrStoreInst(arg, b, 0, key, false);
  builder.createBranchInst(join);
  builder.setInsertionBlock(join);
  auto *phi = builder.createPhiInst();
  phi->addEntry(a, left);
  phi->addEntry(b, right);
  builder.createReturnInst(phi);
  RecordReturnAnalysis returned;
  const auto &summary = returned.analyze(fn);
  ASSERT_EQ(ReturnedRecord::Status::Complete, summary.status);
  ASSERT_EQ(1u, summary.fields.size());
  EXPECT_EQ(fn->getJSDynamicParam(1), summary.fields.front().value);
}

TEST_F(RecordUseAnalysisTest, ReturnedStoreMustReachEveryReturnOfItsOrigin) {
  auto *fn = function("conditional");
  auto *key = builder.getLiteralString("value");
  auto *record = builder.createAllocObjectLiteralInst(
      {{key, builder.getLiteralNumber(1)}});
  auto *write = builder.createBasicBlock(fn);
  auto *end = builder.createBasicBlock(fn);
  builder.createCondBranchInst(builder.getLiteralBool(true), write, end);
  builder.setInsertionBlock(write);
  builder.createPrStoreInst(builder.getLiteralNumber(2), record, 0, key, false);
  builder.createBranchInst(end);
  builder.setInsertionBlock(end);
  builder.createReturnInst(record);
  RecordReturnAnalysis returned;
  const auto &summary = returned.analyze(fn);
  ASSERT_EQ(ReturnedRecord::Status::Complete, summary.status);
  EXPECT_EQ(nullptr, summary.fields.front().value);
  EXPECT_TRUE(summary.fields.front().type.isNumberType());
}

TEST_F(RecordUseAnalysisTest, PublishedAndNestedReturnedAliasesAreRejected) {
  auto *fn = function("published");
  auto *record = builder.createAllocObjectLiteralInst();
  auto *publish = builder.createCallInst(
      builder.getLiteralUndefined(), builder.getLiteralUndefined(),
      builder.getLiteralUndefined(), {record});
  builder.createReturnInst(record);
  auto *nested = function("nested");
  auto *child = builder.createAllocObjectLiteralInst();
  auto *parent = builder.createAllocObjectLiteralInst(
      {{builder.getLiteralString("child"), child}});
  auto *left = builder.createBasicBlock(nested);
  auto *right = builder.createBasicBlock(nested);
  builder.createCondBranchInst(builder.getLiteralBool(true), left, right);
  builder.setInsertionBlock(left);
  builder.createReturnInst(child);
  builder.setInsertionBlock(right);
  builder.createReturnInst(parent);
  RecordReturnAnalysis returned;
  EXPECT_EQ(ReturnedRecord::Status::Unsupported, returned.analyze(fn).status);
  EXPECT_EQ(publish, returned.analyze(fn).boundary);
  EXPECT_EQ(ReturnedRecord::Status::Unsupported, returned.analyze(nested).status);
}

TEST_F(RecordUseAnalysisTest, ReturnedRecursionCannotProveFreshness) {
  auto *fn = function("recursive");
  builder.createReturnInst(call(fn, builder.getLiteralUndefined()));
  RecordReturnAnalysis returned;
  EXPECT_EQ(ReturnedRecord::Status::Recursive, returned.analyze(fn).status);
}

TEST_F(RecordUseAnalysisTest, ReturnedKnownTargetStillRequiresClosureProof) {
  auto *producer = function("producer");
  builder.createReturnInst(builder.createAllocObjectLiteralInst());
  auto *forward = function("maybeCallable");
  auto *result = call(producer, builder.getLiteralUndefined());
  result->setCalleeIsAlwaysClosure(builder.getLiteralBool(false));
  builder.createReturnInst(result);
  RecordReturnAnalysis returned;
  EXPECT_EQ(ReturnedRecord::Status::Unsupported, returned.analyze(forward).status);
}

TEST_F(RecordUseAnalysisTest, ForwardingCannotPublishItsReturnedArgument) {
  auto *producer = function("producer");
  builder.createReturnInst(builder.createAllocObjectLiteralInst());
  auto *identity = function("identity");
  builder.createReturnInst(builder.createLoadParamInst(identity->getJSDynamicParam(1)));
  auto *fn = function("forward");
  auto *a = call(producer, builder.getLiteralUndefined());
  auto *b = call(identity, a);
  auto *left = builder.createBasicBlock(fn);
  auto *right = builder.createBasicBlock(fn);
  builder.createCondBranchInst(builder.getLiteralBool(true), left, right);
  builder.setInsertionBlock(left);
  builder.createReturnInst(a);
  builder.setInsertionBlock(right);
  builder.createReturnInst(b);
  RecordReturnAnalysis returned;
  EXPECT_EQ(ReturnedRecord::Status::Unsupported, returned.analyze(fn).status);
}

TEST_F(RecordUseAnalysisTest, CallDepthLimitCannotProveFreshness) {
  auto *fn = function("leaf");
  builder.createReturnInst(builder.createAllocObjectLiteralInst());
  for (unsigned i = 0; i < 20; ++i) {
    auto *outer = function("forward");
    builder.createReturnInst(call(fn, builder.getLiteralUndefined()));
    fn = outer;
  }
  RecordReturnAnalysis returned;
  EXPECT_EQ(ReturnedRecord::Status::Limit, returned.analyze(fn).status);
}

TEST_F(RecordUseAnalysisTest, ReturnedSlotsPreserveVaryingValues) {
  auto *producer = function("producer");
  auto *left = builder.createBasicBlock(producer);
  auto *right = builder.createBasicBlock(producer);
  builder.createCondBranchInst(builder.getLiteralBool(true), left, right);
  auto *key = builder.getLiteralString("value");
  builder.setInsertionBlock(left);
  builder.createReturnInst(builder.createAllocObjectLiteralInst(
      {{key, builder.getLiteralNumber(2)}}));
  builder.setInsertionBlock(right);
  builder.createReturnInst(builder.createAllocObjectLiteralInst(
      {{key, builder.getLiteralNumber(3)}}));
  function("consumer");
  auto *record = call(producer, builder.getLiteralUndefined());
  auto *read = builder.createLoadPropertyInst(record, key);
  auto *ret = builder.createReturnInst(read);
  std::unique_ptr<Pass> pass(createRecordReturnLoadForwarding());
  EXPECT_TRUE(llvh::cast<ModulePass>(pass.get())->runOnModule(&module));
  auto *slot = llvh::dyn_cast<PrLoadInst>(ret->getValue());
  ASSERT_NE(nullptr, slot);
  EXPECT_TRUE(slot->getType().isNumberType());
  EXPECT_EQ(0u, slot->getPropIndex());
}

TEST_F(RecordUseAnalysisTest, ReturnReadForwardingStopsAtMutation) {
  auto *producer = function("producer");
  auto *arg = builder.createLoadParamInst(producer->getJSDynamicParam(1));
  auto *key = builder.getLiteralString("value");
  builder.createReturnInst(builder.createAllocObjectLiteralInst({{key, arg}}));
  function("consumer");
  auto *value = builder.getLiteralNumber(42);
  auto *record = call(producer, value);
  auto *early = builder.createLoadPropertyInst(record, key);
  auto *sink = builder.createAllocObjectLiteralInst(
      {{builder.getLiteralString("saved"), early}});
  builder.createStorePropertyStrictInst(builder.getLiteralNumber(7), record, key);
  auto *late = builder.createLoadPropertyInst(record, key);
  builder.createReturnInst(builder.createAllocObjectLiteralInst(
      {{builder.getLiteralString("before"), sink}, {key, late}}));
  std::unique_ptr<Pass> pass(createRecordReturnLoadForwarding());
  EXPECT_TRUE(llvh::cast<ModulePass>(pass.get())->runOnModule(&module));
  EXPECT_EQ(value, sink->getValue(0));
  EXPECT_EQ(record, late->getObject());
}
} // namespace
