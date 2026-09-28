/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/IR/IRBuilder.h"
#include "hermes/IR/Instrs.h"
#include "hermes/Optimizer/PassManager/Pass.h"
#include "hermes/Optimizer/Scalar/RecordUseAnalysis.h"
#include "hermes/Support/Statistic.h"

STATISTIC(NumReturnedFieldValues, "Returned record reads replaced with values");
STATISTIC(NumReturnedFieldSlots, "Returned record reads replaced with slots");

namespace hermes {

Pass *createRecordReturnLoadForwarding() {
  class Forward final : public ModulePass {
   public:
    Forward() : ModulePass("RecordReturnLoadForwarding") {}
    bool runOnModule(Module *module) override {
      struct Read {
        LoadPropertyInst *load;
        CallInst *call;
        ReturnedRecordField field;
      };
      RecordReturnAnalysis analysis;
      llvh::SmallVector<Read, 32> reads;
      // Collect all proofs before changing IR: an edit in a producer must not
      // invalidate cached summaries while another caller is being examined.
      for (auto &function : *module) {
        for (auto &block : function) {
          for (auto &inst : block) {
            auto *call = llvh::dyn_cast<CallInst>(&inst);
            if (!call || !call->getCalleeIsAlwaysClosure()->getValue() ||
                !call->getNewTarget()->getType().isUndefinedType())
              continue;
            auto *target = llvh::dyn_cast<Function>(call->getTarget());
            if (!target)
              continue;
            const auto &summary = analysis.analyze(target);
            if (summary.status != ReturnedRecord::Status::Complete)
              continue;
            unsigned scanned = 0;
            for (auto it = std::next(call->getIterator()); it != block.end(); ++it) {
              if (++scanned > 512)
                break;
              bool usesRecord = false;
              for (unsigned i = 0; i < it->getNumOperands(); ++i)
                usesRecord |= it->getOperand(i) == call;
              if (!usesRecord)
                continue;
              auto *load = llvh::dyn_cast<LoadPropertyInst>(&*it);
              auto *key = load
                  ? llvh::dyn_cast<LiteralString>(load->getProperty())
                  : nullptr;
              const ReturnedRecordField *field = nullptr;
              for (const auto &candidate : summary.fields)
                if (candidate.key == key && candidate.definitelyOwn)
                  field = &candidate;
              // Before any publication, mutation, alias creation or possible
              // getter, unrelated calls cannot reach this fresh record. Child
              // values retain their identities; nothing assumes they are pure.
              if (!load || load->getObject() != call || !field)
                break;
              if (field->value || field->ordinal.hasValue())
                reads.push_back({load, call, *field});
            }
          }
        }
      }

      IRBuilder builder(module);
      IRBuilder::InstructionDestroyer destroyer;
      for (const auto &read : reads) {
        Value *replacement = read.field.value;
        if (auto *parameter = llvh::dyn_cast_or_null<JSDynamicParam>(replacement)) {
          unsigned index = parameter->getIndexInParamList();
          replacement = index < read.call->getNumArguments()
              ? read.call->getArgument(index)
              : builder.getLiteralUndefined();
        }
        if (replacement) {
          ++NumReturnedFieldValues;
        } else {
          builder.setInsertionPoint(read.load);
          builder.setLocation(read.load->getLocation());
          replacement = builder.createPrLoadInst(
              read.call, read.field.ordinal.getValue(), read.field.key,
              read.field.type);
          ++NumReturnedFieldSlots;
        }
        read.load->replaceAllUsesWith(replacement);
        destroyer.add(read.load);
      }
      return !reads.empty();
    }
  };
  return new Forward();
}

} // namespace hermes
