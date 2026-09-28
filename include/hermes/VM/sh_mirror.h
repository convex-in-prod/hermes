/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_SH_MIRROR_H
#define HERMES_SH_MIRROR_H

#include "hermes/VM/sh_legacy_value.h"
#include "hermes/VM/sh_runtime.h"
#include "hermes/VMLayouts/PropertyCache.h"

#ifdef HERMESVM_COMPRESSED_POINTERS
typedef uint32_t SHCompressedPointerRawType;
#else
typedef uintptr_t SHCompressedPointerRawType;
#endif

typedef uint32_t SHWeakRootSymbolID;

typedef struct SHNativeFuncInfo SHNativeFuncInfo;
typedef struct SHUnit SHUnit;

/// Struct mirroring the layout of PropertyCacheEntry. This allows us to expose
/// the offsets of certain fields without needing to make the actual C++ version
/// available here.

typedef struct SHWritePropertyCacheEntry {
  SHCompressedPointerRawType clazz;
  uint32_t slotAndAddCacheIndex;
} SHWritePropertyCacheEntry;

typedef struct SHReadPropertyCacheEntry {
  SHCompressedPointerRawType clazz;
  SHCompressedPointerRawType negMatchClazz;
  uint16_t _slot16;
  uint8_t numChanges;
} SHReadPropertyCacheEntry;

typedef struct SHPrivateNameCacheEntry {
  SHCompressedPointerRawType clazz;
  SHWeakRootSymbolID nameVal;
  uint32_t slot;
} SHPrivateNameCacheEntry;

typedef struct SHComputedReadCacheEntry {
  SHCompressedPointerRawType clazz;
  SHWeakRootSymbolID key;
  uint32_t slot;
} SHComputedReadCacheEntry;

/// Struct mirroring the layout of GCCell.
typedef struct SHGCCell {
  SHCompressedPointerRawType kindAndSize;
#ifndef NDEBUG
  uint16_t magic;
  uint32_t debugAllocationId;
#endif
} SHGCCell;

enum SHCellKind {
#define CELL_KIND(name) SH_##name##Kind,
#include "hermes/VM/CellKinds.def"
};

// KindAndSize uses the low 32 bits for size on uncompressed 64-bit hosts,
// and the low 24 bits on 32-bit or compressed-pointer hosts.
#define SH_CELL_KIND_SHIFT (sizeof(SHCompressedPointerRawType) > 4 ? 32 : 24)

/// Struct mirroring the layout of JSObject (without the direct props).
typedef struct SHJSObject {
  SHGCCell base;
  SHObjectFlags flags;
  SHCompressedPointerRawType parent;
  SHCompressedPointerRawType clazz;
  SHCompressedPointerRawType propStorage;
} SHJSObject;

typedef struct SHJSTypedArray {
  SHJSObject base;
  SHCompressedPointer buffer;
  uint32_t length;
  uint32_t offset;
} SHJSTypedArray;

typedef struct SHArrayImpl {
  SHJSObject base;
  uint32_t beginIndex;
  uint32_t elemCount;
  SHCompressedPointer indexedStorage;
} SHArrayImpl;

typedef struct SHJSArrayBuffer {
  SHJSObject base;
  uint8_t *data;
  uint32_t size;
  bool external;
  bool attached;
} SHJSArrayBuffer;

#ifdef HERMESVM_BOXED_DOUBLES
typedef SHCompressedPointerRawType SHGCSmallHermesValue;
#else
typedef SHLegacyValue SHGCSmallHermesValue;
#endif

typedef struct SHJSObjectAndDirectProps {
  SHJSObject base;
  SHGCSmallHermesValue directProps[HERMESVM_DIRECT_PROPERTY_SLOTS];
} SHJSObjectAndDirectProps;

/// Struct mirroring the layout of Callable.
typedef struct SHCallable {
  SHJSObject base;
  SHCompressedPointer environment;
} SHCallable;

/// A pointer to native function.
typedef SHLegacyValue (*NativeJSFunctionPtr)(SHRuntime *shr);

/// Struct mirroring the layout of NativeJSFunction.
typedef struct SHNativeJSFunction {
  SHCallable base;
  NativeJSFunctionPtr functionPtr;
  SHNativeFuncInfo *funcInfo;
  SHUnit *unit;
} SHNativeJSFunction;

/// Struct mirroring the layout of Environment.
typedef struct SHEnvironment {
  SHGCCell base;
  SHCompressedPointer parentEnvironment;
  uint32_t size;

  SHLegacyValue slots[0];
} SHEnvironment;

/// Struct mirroring the layout of FastArray.
typedef struct SHFastArray {
  SHJSObject base;
  SHCompressedPointer indexedStorage;
  SHGCSmallHermesValue length;
} SHFastArray;

/// Struct mirroring the layout of ArrayStorageSmall.
typedef struct SHArrayStorageSmall {
  SHGCCell base;

  uint32_t size;
  SHGCSmallHermesValue storage[0];
} SHArrayStorageSmall;

/// Struct mirroring the layout of ArrayStorage.
typedef struct SHArrayStorage {
  SHGCCell base;

  uint32_t size;
  SHLegacyValue storage[0];
} SHArrayStorage;

/// Struct mirroring the layout of BoxedDouble.
typedef struct SHBoxedDouble {
  SHGCCell base;
  double value_;
} SHBoxedDouble;

#endif
