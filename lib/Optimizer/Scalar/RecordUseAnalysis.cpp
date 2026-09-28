/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/Optimizer/Scalar/RecordUseAnalysis.h"

#include "hermes/IR/CFG.h"
#include "hermes/IR/Instrs.h"
#include "hermes/Optimizer/PassManager/Pass.h"
#include "hermes/Optimizer/Scalar/Utils.h"
#include "llvh/ADT/SmallPtrSet.h"
#include "llvh/ADT/SmallSet.h"
#include "llvh/Support/raw_ostream.h"

#include <tuple>

namespace hermes {

const char *recordUseKindName(RecordUseKind kind) {
  switch (kind) {
    case RecordUseKind::Read:
      return "read";
    case RecordUseKind::Write:
      return "write";
    case RecordUseKind::Define:
      return "define";
    case RecordUseKind::Forward:
      return "forward";
    case RecordUseKind::Return:
      return "return";
    case RecordUseKind::Capture:
      return "capture";
    case RecordUseKind::StoredAsChild:
      return "stored-as-child";
    case RecordUseKind::Observe:
      return "observe";
    case RecordUseKind::UnknownCall:
      return "unknown-call";
    case RecordUseKind::Unsupported:
      return "unsupported";
    case RecordUseKind::Recursive:
      return "recursive";
    case RecordUseKind::Limit:
      return "limit";
  }
  llvm_unreachable("invalid record use kind");
}

RecordUseAnalysis::RecordUseAnalysis() : RecordUseAnalysis(Limits{}) {}
RecordUseAnalysis::RecordUseAnalysis(Limits limits) : limits_(limits) {
  assert(limits.values && limits.edges && limits.uses && limits.callDepth);
}

RecordUses RecordUseAnalysis::analyze(Instruction *root) {
  return analyzeValues(root->getFunction(), {root});
}

const RecordUses &RecordUseAnalysis::analyzeParameter(
    JSDynamicParam *parameter) {
  auto &entry = parameters_[parameter];
  if (entry)
    return entry->result;
  entry = std::make_unique<Entry>();
  // Keep the entry stable across recursive map insertions. A pending summary
  // contains no optimistic facts that a recursive consumer could mistake for
  // a completed no-escape proof.
  Entry *current = entry.get();
  current->pending = true;
  current->result.complete = false;
  auto *owner = parameter->getParent();
  auto [it, inserted] = argumentVectors_.try_emplace(owner, false);
  if (inserted)
    it->second = capturesArgumentVector(owner);
  llvh::SmallVector<Instruction *, 4> roots;
  for (auto *user : parameter->getUsers()) {
    if (roots.size() >= limits_.values) {
      current->result.uses.push_back({RecordUseKind::Limit, user});
      current->pending = false;
      return current->result;
    }
    roots.push_back(user);
  }
  if (it->second || llvh::isa<GeneratorFunction>(owner) ||
      llvh::isa<AsyncFunction>(owner)) {
    for (auto *root : roots)
      current->result.uses.push_back({RecordUseKind::Unsupported, root});
  } else {
    ++callDepth_;
    current->result = analyzeValues(owner, roots);
    --callDepth_;
  }
  current->pending = false;
  return current->result;
}

ReturnedRecordConstruction RecordUseAnalysis::describeReturnedConstruction(
    AllocObjectLiteralInst *allocation) {
  ReturnedRecordConstruction result;
  llvh::DenseMap<LiteralString *, unsigned> indexes;
  auto addField = [&](Literal *property) -> RecordFieldConstruction * {
    auto *key = llvh::dyn_cast<LiteralString>(property);
    if (!key || toArrayIndex(key->getValue().str()))
      return nullptr;
    auto found = indexes.find(key);
    if (found != indexes.end())
      return &result.fields[found->second];
    if (result.fields.size() >= limits_.values)
      return nullptr;
    indexes[key] = result.fields.size();
    result.fields.push_back({key, llvh::None, nullptr, {}, false});
    return &result.fields.back();
  };
  for (unsigned i = 0; i < allocation->getKeyValuePairCount(); ++i) {
    auto *field = addField(allocation->getKey(i));
    if (!field || field->initialOrdinal.hasValue()) {
      result.boundary = allocation;
      return result;
    }
    field->initialOrdinal = i;
    field->initialValue = allocation->getValue(i);
    field->definitelyOwnOnReturn = true;
  }
  unsigned edges = 0;
  for (auto *use : allocation->getUsers()) {
    if (++edges > limits_.edges) {
      result.boundary = use;
      return result;
    }
    if (auto *ret = llvh::dyn_cast<ReturnInst>(use)) {
      result.returns.push_back(ret);
      continue;
    }
    if (auto *store = llvh::dyn_cast<PrStoreInst>(use)) {
      if (store->getObject() == allocation &&
          store->getStoredValue() != allocation &&
          store->getPropIndex() < allocation->getKeyValuePairCount() &&
          store->getPropName() == allocation->getKey(store->getPropIndex())) {
        result.fields[store->getPropIndex()].definitions.push_back(use);
        continue;
      }
    }
    if (auto *define = llvh::dyn_cast<DefineOwnPropertyInst>(use)) {
      if (define->getObject() == allocation &&
          define->getStoredValue() != allocation && define->getIsEnumerable()) {
        if (auto *key = llvh::dyn_cast<LiteralString>(define->getProperty())) {
          if (auto *field = addField(key)) {
            field->definitions.push_back(use);
            continue;
          }
        }
      }
    }
    result.boundary = use;
    return result;
  }
  if (result.returns.empty()) {
    result.boundary = allocation;
    return result;
  }
  DominanceInfo dominance(allocation->getFunction());
  for (auto &field : result.fields) {
    if (field.initialOrdinal.hasValue())
      continue;
    // A definition on each incoming branch could also prove presence. This
    // first summary deliberately requires one dominating definition per return
    // and leaves such joins unproved rather than confusing absence with undef.
    field.definitelyOwnOnReturn = true;
    for (auto *ret : result.returns) {
      bool defined = false;
      for (auto *definition : field.definitions)
        defined |= dominance.properlyDominates(definition, ret);
      field.definitelyOwnOnReturn &= defined;
    }
  }
  return result;
}

RecordUses RecordUseAnalysis::analyzeValues(
    Function *owner,
    llvh::ArrayRef<Instruction *> roots) {
  RecordUses result;
  llvh::SmallVector<Instruction *, 16> worklist(roots.begin(), roots.end());
  llvh::SmallPtrSet<Instruction *, 32> visited;
  llvh::SmallSet<
      std::tuple<Instruction *, RecordUseKind, Literal *, unsigned>, 32> recorded;
  unsigned edges = 0;

  auto add = [&](RecordUseKind kind, Instruction *site,
                 Literal *property = nullptr, unsigned argument = 0) {
    if (!recorded.insert({site, kind, property, argument}).second)
      return;
    if (result.uses.size() >= limits_.uses) {
      result.complete = false;
      result.uses.back() = {RecordUseKind::Limit, site};
      return;
    }
    result.uses.push_back({kind, site, property, argument});
  };
  auto fail = [&](RecordUseKind kind, Instruction *site) {
    add(kind, site);
    result.complete = false;
  };

  while (!worklist.empty()) {
    auto *value = worklist.pop_back_val();
    if (!visited.insert(value).second)
      continue;
    if (visited.size() > limits_.values || result.uses.size() >= limits_.uses) {
      fail(RecordUseKind::Limit, value);
      break;
    }
    for (auto *user : value->getUsers()) {
      if (++edges > limits_.edges) {
        fail(RecordUseKind::Limit, user);
        return result;
      }
      if (llvh::isa<PhiInst>(user) || llvh::isa<MovInst>(user) ||
          llvh::isa<UnionNarrowTrustedInst>(user) ||
          llvh::isa<ThrowIfInst>(user)) {
        worklist.push_back(user);
        continue;
      }
      if (auto *read = llvh::dyn_cast<LoadPropertyInst>(user)) {
        if (read->getObject() == value)
          add(RecordUseKind::Read, user,
              llvh::dyn_cast<Literal>(read->getProperty()));
        if (read->getProperty() == value)
          fail(RecordUseKind::Unsupported, user);
        continue;
      }
      if (auto *read = llvh::dyn_cast<PrLoadInst>(user)) {
        add(RecordUseKind::Read, user, read->getPropName());
        continue;
      }
      if (auto *store = llvh::dyn_cast<PrStoreInst>(user)) {
        if (store->getObject() == value)
          add(RecordUseKind::Write, user, store->getPropName());
        if (store->getStoredValue() == value)
          add(RecordUseKind::StoredAsChild, user, store->getPropName());
        continue;
      }
      if (auto *store = llvh::dyn_cast<StorePropertyInst>(user)) {
        auto *key = llvh::dyn_cast<Literal>(store->getProperty());
        if (store->getObject() == value)
          add(RecordUseKind::Write, user, key);
        if (store->getStoredValue() == value)
          add(RecordUseKind::StoredAsChild, user, key);
        if (store->getProperty() == value)
          fail(RecordUseKind::Unsupported, user);
        continue;
      }
      if (auto *store = llvh::dyn_cast<DefineOwnPropertyInst>(user)) {
        auto *key = llvh::dyn_cast<Literal>(store->getProperty());
        if (store->getObject() == value)
          add(RecordUseKind::Define, user, key);
        if (store->getStoredValue() == value)
          add(RecordUseKind::StoredAsChild, user, key);
        if (store->getProperty() == value)
          fail(RecordUseKind::Unsupported, user);
        continue;
      }
      if (auto *alloc = llvh::dyn_cast<AllocObjectLiteralInst>(user)) {
        if (alloc->getParentObject() == value)
          add(RecordUseKind::Observe, user);
        for (unsigned i = 0; i < alloc->getKeyValuePairCount(); ++i)
          if (alloc->getValue(i) == value)
            add(RecordUseKind::StoredAsChild, user, alloc->getKey(i));
        continue;
      }
      if (auto *capture = llvh::dyn_cast<StoreFrameInst>(user)) {
        add(RecordUseKind::Capture, user);
        auto *variable = capture->getVariable();
        if (capture->getValue() != value ||
            isStoreOnceVariable(variable) != value) {
          fail(RecordUseKind::Unsupported, user);
          continue;
        }
        for (auto *use : variable->getUsers()) {
          if (++edges > limits_.edges) {
            fail(RecordUseKind::Limit, use);
            return result;
          }
          if (auto *load = llvh::dyn_cast<LoadFrameInst>(use))
            worklist.push_back(load);
        }
        continue;
      }
      if (auto *store = llvh::dyn_cast<StoreStackInst>(user)) {
        auto *slot = llvh::dyn_cast<AllocStackInst>(store->getPtr());
        if (!slot || store->getValue() != value ||
            isStoreOnceStackLocation(slot) != value) {
          fail(RecordUseKind::Unsupported, user);
          continue;
        }
        for (auto *use : slot->getUsers()) {
          if (++edges > limits_.edges) {
            fail(RecordUseKind::Limit, use);
            return result;
          }
          if (auto *load = llvh::dyn_cast<LoadStackInst>(use))
            worklist.push_back(load);
        }
        continue;
      }
      if (llvh::isa<ReturnInst>(user)) {
        if (user->getFunction() == owner) {
          result.returnsDirectAlias = true;
          add(RecordUseKind::Return, user);
        } else {
          // Returning a captured value from a nested closure does not mean
          // the enclosing call returns it. Its later callers are unknown here.
          add(RecordUseKind::Observe, user);
        }
        continue;
      }
      if (auto *call = llvh::dyn_cast<BaseCallInst>(user)) {
        auto *target = llvh::dyn_cast<Function>(call->getTarget());
        if (!llvh::isa<CallInst>(call) || !target ||
            !call->getCalleeIsAlwaysClosure()->getValue() ||
            !call->getNewTarget()->getType().isUndefinedType() ||
            call->getCallee() == value || call->getNewTarget() == value) {
          fail(RecordUseKind::UnknownCall, user);
          continue;
        }
        for (unsigned i = 0; i < call->getNumArguments(); ++i) {
          if (call->getArgument(i) != value)
            continue;
          add(RecordUseKind::Forward, user, nullptr, i);
          if (i >= target->getJSDynamicParams().size()) {
            fail(RecordUseKind::Unsupported, user);
            continue;
          }
          if (callDepth_ >= limits_.callDepth) {
            fail(RecordUseKind::Limit, user);
            continue;
          }
          auto *parameter = target->getJSDynamicParam(i);
          auto existing = parameters_.find(parameter);
          if (existing != parameters_.end() && existing->second->pending) {
            fail(RecordUseKind::Recursive, user);
            continue;
          }
          const auto &callee = analyzeParameter(parameter);
          result.complete &= callee.complete;
          if (!callee.complete && callee.uses.empty())
            fail(RecordUseKind::Unsupported, user);
          for (const auto &use : callee.uses)
            if (use.kind != RecordUseKind::Return)
              add(use.kind, use.site, use.property, use.argument);
          if (callee.returnsDirectAlias)
            worklist.push_back(call);
        }
        continue;
      }
      if (user->getKind() == ValueKind::BinaryStrictlyEqualInstKind ||
          user->getKind() == ValueKind::BinaryStrictlyNotEqualInstKind) {
        add(RecordUseKind::Observe, user);
        continue;
      }
      fail(RecordUseKind::Unsupported, user);
    }
  }
  return result;
}

/// Explicit diagnostic pass; no production pipeline or IR is changed.
Pass *createRecordUseAnalysis() {
  class Report final : public ModulePass {
   public:
    Report() : ModulePass("RecordUseAnalysis") {}
    bool runOnModule(Module *M) override {
      RecordUseAnalysis analysis;
      RecordReturnAnalysis returned;
      llvh::DenseMap<Instruction *, unsigned> ids;
      for (auto &function : *M) {
        unsigned id = 0;
        for (auto &block : function)
          for (auto &inst : block)
            ids[&inst] = id++;
      }
      for (auto &function : *M) {
        const auto &summary = returned.analyze(&function);
        auto &out = llvh::errs();
        out << "return-record " << function.getInternalNameStr()
            << " status=" << static_cast<unsigned>(summary.status);
        if (summary.boundary)
          out << " boundary=" << summary.boundary->getKindStr()
              << " @" << ids[summary.boundary];
        out << "\n";
        for (const auto &field : summary.fields) {
          out << "  field " << field.key->getValue().str()
              << " own=" << field.definitelyOwn;
          if (field.ordinal.hasValue())
            out << " ordinal=" << field.ordinal.getValue();
          if (auto *param = llvh::dyn_cast_or_null<JSDynamicParam>(field.value))
            out << " parameter=" << param->getIndexInParamList();
          else if (field.value)
            out << " value=" << field.value->getKindStr();
          out << "\n";
        }
        for (auto &block : function)
          for (auto &inst : block) {
            if (!llvh::isa<AllocObjectLiteralInst>(&inst) &&
                !llvh::isa<CallInst>(&inst))
              continue;
            if (!M->getTypeContext().canBeObject(inst.getType()))
              continue;
            auto summary = analysis.analyze(&inst);
            auto &out = llvh::errs();
            out << "record " << function.getInternalNameStr() << " @"
                << ids[&inst] << " " << inst.getKindStr()
                << " complete=" << summary.complete
                << " returns-direct-alias=" << summary.returnsDirectAlias << "\n";
            if (auto *allocation = llvh::dyn_cast<AllocObjectLiteralInst>(&inst)) {
              if (summary.returnsDirectAlias) {
                auto construction = analysis.describeReturnedConstruction(allocation);
                out << "  construction local=" << !construction.boundary << "\n";
                for (const auto &field : construction.fields) {
                  out << "    field " << field.key->getValue().str()
                      << " own-on-return=" << field.definitelyOwnOnReturn;
                  if (field.initialOrdinal.hasValue())
                    out << " initial-ordinal=" << field.initialOrdinal.getValue();
                  out << " definitions=" << field.definitions.size() << "\n";
                }
              }
            }
            for (const auto &use : summary.uses) {
              out << "  " << recordUseKindName(use.kind) << " "
                  << use.site->getFunction()->getInternalNameStr() << " @"
                  << ids[use.site] << " " << use.site->getKindStr();
              if (auto *key = llvh::dyn_cast_or_null<LiteralString>(use.property))
                out << " key=" << key->getValue().str();
              if (use.kind == RecordUseKind::Forward) {
                auto *call = llvh::cast<BaseCallInst>(use.site);
                out << " target="
                    << llvh::cast<Function>(call->getTarget())->getInternalNameStr()
                    << " argument=" << use.argument;
              }
              out << "\n";
            }
          }
      }
      return false;
    }
  };
  return new Report();
}

} // namespace hermes
