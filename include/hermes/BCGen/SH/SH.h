/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_BCGEN_SH_SH_H
#define HERMES_BCGEN_SH_SH_H

#include "hermes/IR/IR.h"
#include "hermes/Utils/Options.h"
#include "llvh/ADT/STLExtras.h"

#include <string>
#include <vector>

namespace hermes {
namespace sh {

/// Given a module \p M and an ostream \p OS, compiles the module into C, and
/// outputs the code to the ostream.
void generateSH(
    Module *M,
    llvh::raw_ostream &OS,
    const BytecodeGenerationOptions &options);

enum class SHCBundleFileRole {
  Header,
  Metadata,
  Function,
};

enum class SHCBundleCOptimizationLevel {
  Default,
  O0,
};

/// Why a one-function translation unit exceeds targetBytes. An oversized
/// function without one of these reasons is a bundle-generation invariant
/// failure.
enum class SHCBundleOversizeReason {
  None,
  /// The oversized fragment has one generated instruction.
  SingleInstruction,
  /// No same-block non-terminator instruction run meets the outlining policy.
  NoOutlineableRun,
};

struct SHCBundleFile {
  std::string path;
  SHCBundleFileRole role;
  uint32_t firstFunctionId{0};
  uint32_t lastFunctionId{0};
  uint32_t functionCount{0};
  uint64_t targetBytes{0};
  bool oversize{false};
  SHCBundleOversizeReason oversizeReason{SHCBundleOversizeReason::None};
  /// Fragment fields are zero for ordinary function shards. For an outlined
  /// function, index 0 is the wrapper and later indexes are helpers in
  /// deterministic wrapper call-site emission order. Every fragment has the
  /// same positive count.
  uint32_t functionFragmentIndex{0};
  uint32_t functionFragmentCount{0};
  /// Default leaves optimization policy to the bundle consumer. O0 is an
  /// explicit per-translation-unit requirement.
  SHCBundleCOptimizationLevel cOptimizationLevel{
      SHCBundleCOptimizationLevel::Default};
};

/// Callback used by generateSHBundle() to write one file atomically. The
/// callback must call \p emit with a writable stream and return whether the
/// file was successfully published.
using SHCBundleWriteFile = llvh::function_ref<bool(
    llvh::StringRef path,
    llvh::function_ref<void(llvh::raw_ostream &)> emit)>;

/// Compile \p M into one metadata translation unit and multiple function
/// translation units. The returned file list is in dependency order: header,
/// metadata, then function shards.
bool generateSHBundle(
    Module *M,
    SHCBundleWriteFile writeFile,
    std::vector<SHCBundleFile> &files,
    const BytecodeGenerationOptions &options);

} // namespace sh
} // namespace hermes

#endif
