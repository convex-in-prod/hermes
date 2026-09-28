/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_BCGEN_LITERALBUFFERBUILDER_H
#define HERMES_BCGEN_LITERALBUFFERBUILDER_H

#include "hermes/BCGen/HBC/BCProvider.h"
#include "hermes/BCGen/SerializedLiteralGenerator.h"
#include "hermes/BCGen/ShapeTableEntry.h"
#include "hermes/Support/StringTableEntry.h"
#include "llvh/ADT/DenseMap.h"

namespace hermes {

class Instruction;
class Module;
class Function;

namespace LiteralBufferBuilder {

/// Offset information stored per object/array literal instruction.
struct LiteralOffset {
  /// Index into the shape table. Unused for array instructions.
  uint32_t shapeTableIdx;
  /// Byte offset into the value buffer.
  uint32_t valueBufferOffset;
};

using LiteralOffsetMapTy = llvh::DenseMap<const Instruction *, LiteralOffset>;

/// Validated allocation hints for serialized literals. Current literals are
/// serialized normally and matched by exact bytes before retaining an offset.
/// An empty instance requests cold layout capture; nullptr leaves it disabled.
struct RetainedBuffers {
  std::vector<unsigned char> values;
  std::vector<unsigned char> keys;
  std::vector<StringTableEntry> valueEntries;
  std::vector<StringTableEntry> keyEntries;
  std::vector<ShapeTableEntry> shapes;
  /// ValueKind of each shape's allocation; typed shapes cannot share caches
  /// with ordinary or typed non-enumerable object shapes.
  std::vector<uint8_t> shapeKinds;
  size_t liveValueEntries{0};
  size_t liveKeyEntries{0};
  size_t liveShapes{0};
  size_t liveValueBytes{0};
  size_t liveKeyBytes{0};
};

/// The LiteralBufferBuilder will build this struct as its output.
struct Result {
  std::vector<unsigned char> literalValBuffer;
  std::vector<unsigned char> keyBuffer;
  /// Contains the unique'd shapes of all object literals.
  std::vector<ShapeTableEntry> shapeTable;
  LiteralOffsetMapTy offsetMap;
};

/// Collect the literals, optionally deduplicate them, and generate the literal
/// buffers.
Result generate(
    Module *m,
    const std::function<bool(Function *)> &shouldVisitFunction,
    const SerializedLiteralGenerator::StringLookupFn &getIdentifier,
    const SerializedLiteralGenerator::StringLookupFn &getString,
    bool optimize,
    hbc::BCProviderBase *bcProvider = nullptr,
    RetainedBuffers *retainedBuffers = nullptr);
} // namespace LiteralBufferBuilder
} // namespace hermes

#endif // HERMES_BCGEN_LITERALBUFFERBUILDER_H
