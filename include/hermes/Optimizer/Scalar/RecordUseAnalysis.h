/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_OPTIMIZER_SCALAR_RECORD_USE_ANALYSIS_H
#define HERMES_OPTIMIZER_SCALAR_RECORD_USE_ANALYSIS_H

#include "hermes/IR/IR.h"
#include "llvh/ADT/DenseMap.h"
#include "llvh/ADT/SmallVector.h"

#include <memory>

namespace hermes {

class AllocObjectLiteralInst;
class ReturnInst;

/// A use of a possible record alias. These are obligations for a representation
/// transformation, not a proof of immutability or of an own data property.
enum class RecordUseKind {
  Read,
  Write,
  Define,
  Forward,
  Return,
  Capture,
  StoredAsChild,
  Observe,
  UnknownCall,
  Unsupported,
  Recursive,
  Limit,
};

struct RecordUse {
  RecordUseKind kind;
  Instruction *site;
  /// Null for computed keys and uses which do not name a property.
  Literal *property = nullptr;
  /// The formal argument index, including this, for Forward uses.
  unsigned argument = 0;
};

struct RecordUses {
  llvh::SmallVector<RecordUse, 8> uses;
  /// Some normal return directly forwards a possible input alias. Field reads
  /// are terminal obligations, not identity edges: a false result does not
  /// rule out aliases through child back-references or accessor side effects.
  bool returnsDirectAlias = false;
  /// False on unsupported operations, recursion or budget exhaustion. Complete
  /// means all direct uses were classified, not that all JavaScript aliases or
  /// effects are known. Each read/write still needs a layout and effect proof.
  bool complete = true;
};

struct RecordFieldConstruction {
  LiteralString *key;
  /// Initial literal order only; conditional extensions have no fixed ordinal.
  OptValue<unsigned> initialOrdinal;
  Value *initialValue;
  llvh::SmallVector<Instruction *, 2> definitions;
  /// False means unproved presence, not a proof of absence.
  bool definitelyOwnOnReturn = false;
};

struct ReturnedRecordConstruction {
  llvh::SmallVector<RecordFieldConstruction, 8> fields;
  llvh::SmallVector<ReturnInst *, 2> returns;
  /// Null only when every use is an admitted own-data definition or return.
  /// Even then this describes construction, not the lifetime after return.
  Instruction *boundary = nullptr;
};

struct ReturnedRecordField {
  LiteralString *key;
  bool definitelyOwn;
  /// Absent when returned alternatives place the field at different slots.
  OptValue<unsigned> ordinal;
  Type type;
  /// A literal or a parameter of the summarized function, never its local SSA.
  /// Null means the field value is not representable in the caller.
  Value *value;
};

struct ReturnedRecord {
  enum class Status { Complete, Unsupported, Recursive, Limit, NoReturn };
  Status status = Status::Unsupported;
  llvh::SmallVector<ReturnedRecordField, 8> fields;
  Instruction *boundary = nullptr;
};

/// Summaries describe a fresh, unpublished ordinary object at normal return.
/// They do not prove that its fields remain unchanged after the caller receives
/// it. As with use summaries, all cached facts require immutable IR.
class RecordReturnAnalysis {
 public:
  const ReturnedRecord &analyze(Function *function);

 private:
  struct Entry {
    ReturnedRecord result;
  };
  llvh::DenseMap<Function *, std::unique_ptr<Entry>> functions_;
  unsigned depth_ = 0;
  ReturnedRecord compute(Function *function);
};

/// Demand-driven, context-independent parameter summaries on immutable IR.
/// Known calls instantiate returned aliases at that call's result, never at
/// other callers of the function. Phi inputs are possible aliases; joined
/// values therefore need a separate origin/layout proof before optimization.
/// Construct a new analysis after any IR mutation; pointers and summaries are
/// deliberately not valid across transformations.
class RecordUseAnalysis {
 public:
  struct Limits {
    unsigned values = 512;
    unsigned edges = 4096;
    unsigned uses = 256;
    unsigned callDepth = 16;
  };

  RecordUseAnalysis();
  explicit RecordUseAnalysis(Limits limits);

  RecordUses analyze(Instruction *root);
  const RecordUses &analyzeParameter(JSDynamicParam *parameter);
  ReturnedRecordConstruction describeReturnedConstruction(
      AllocObjectLiteralInst *allocation);

 private:
  struct Entry {
    RecordUses result;
    bool pending = false;
  };

  Limits limits_;
  unsigned callDepth_ = 0;
  llvh::DenseMap<JSDynamicParam *, std::unique_ptr<Entry>> parameters_;
  llvh::DenseMap<Function *, bool> argumentVectors_;

  RecordUses analyzeValues(
      Function *owner,
      llvh::ArrayRef<Instruction *> roots);
};

const char *recordUseKindName(RecordUseKind kind);

} // namespace hermes
#endif
