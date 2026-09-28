/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_REGEX_EXECUTOR_H
#define HERMES_REGEX_EXECUTOR_H

#include "hermes/Regex/RegexAOT.h"
#include "hermes/Regex/RegexBytecode.h"
#include "hermes/Regex/RegexTypes.h"
#include "hermes/Support/StackOverflowGuard.h"
#include "llvh/ADT/STLExtras.h"

// This file contains the machinery for executing a regexp compiled to bytecode.

namespace hermes {
namespace regex {

/// The result of trying to find a match.
enum class MatchRuntimeResult {
  /// Match found.
  Match,

  /// No match found.
  NoMatch,

  /// Stack overflow during match attempt.
  StackOverflow,
};

/// A constant used inside a capture group to indicate that the capture group
/// did not match.
constexpr uint32_t kNotMatched = UINT32_MAX;

/// The maximum number of times we will backtrack.
constexpr uint32_t kBacktrackLimit = 1u << 30;

/// A CapturedRange represents a range of the input string captured by a capture
/// group. A CaptureGroup may also not have matched, in which case its start is
/// set to kNotMatched. Note that an unmatched capture group is different than a
/// capture group that matched an empty string.
struct CapturedRange {
  /// Index of the first captured character, or kNotMatched if not matched.
  uint32_t start;

  /// One past the index of the last captured character.
  uint32_t end;

  /// \return whether this range was a successful match.
  bool matched() const {
    return start != kNotMatched;
  }
};

/// Called synchronously on success with the full match and capture groups.
/// The borrowed capture array is valid only for the duration of the call.
using CaptureRangeSink =
    llvh::function_ref<void(CapturedRange, llvh::ArrayRef<CapturedRange>)>;

/// Classify one code unit using an unanchored, one-term simple descriptor.
/// The caller proves this descriptor shape; no state, allocation or JS calls.
bool matchSimpleRegexCharacter(const uint8_t *descriptor, char16_t character);

/// Invoke generated control flow with the ordinary constraint/stack checks.
/// A declined representation or exhausted work budget restarts bytecode search.
MatchRuntimeResult searchWithAOTRegex(
    HermesRegexAOTMatcher matcher,
    llvh::ArrayRef<uint8_t> bytecode,
    const char *first,
    uint32_t start,
    uint32_t length,
    CaptureRangeSink captures,
    constants::MatchFlagType matchFlags,
    StackOverflowGuard guard);

MatchRuntimeResult searchWithAOTRegex(
    HermesRegexAOTMatcher matcher,
    llvh::ArrayRef<uint8_t> bytecode,
    const char16_t *first,
    uint32_t start,
    uint32_t length,
    CaptureRangeSink captures,
    constants::MatchFlagType matchFlags,
    StackOverflowGuard guard);

/// Search without allocating an intermediate output vector. The sink must not
/// reenter the VM or trigger GC while the executor holds input pointers.
MatchRuntimeResult searchWithBytecode(
    llvh::ArrayRef<uint8_t> bytecode,
    const char16_t *first,
    uint32_t start,
    uint32_t length,
    CaptureRangeSink captures,
    constants::MatchFlagType matchFlags,
    StackOverflowGuard guard);

MatchRuntimeResult searchWithBytecode(
    llvh::ArrayRef<uint8_t> bytecode,
    const char *first,
    uint32_t start,
    uint32_t length,
    CaptureRangeSink captures,
    constants::MatchFlagType matchFlags,
    StackOverflowGuard guard);

/// Execute a persistent descriptor produced by compileSimpleRegex. This path
/// accepts only fixed-width, capture-free, non-Unicode code-unit patterns.
/// Case-insensitive classes use their already-expanded bytecode ranges;
/// folded literal instructions are not supported.
/// As with searchWithBytecode, start must not exceed length. The sink has the
/// same synchronous, non-reentrant lifetime contract.
MatchRuntimeResult searchWithSimpleRegex(
    const uint8_t *descriptor,
    const char16_t *first,
    uint32_t start,
    uint32_t length,
    CaptureRangeSink captures,
    constants::MatchFlagType matchFlags,
    StackOverflowGuard guard);

MatchRuntimeResult searchWithSimpleRegex(
    const uint8_t *descriptor,
    const char *first,
    uint32_t start,
    uint32_t length,
    CaptureRangeSink captures,
    constants::MatchFlagType matchFlags,
    StackOverflowGuard guard);

/// Given a string \p first with length \p length, look for regex matches
/// starting at offset \p start. We must have 0 <= start <= length.
/// Search using the compiled regex represented by \p bytecode with the flags \p
/// matchFlags. If the search succeeds, populate \p captures with the capture
/// groups.
/// \param guard is used to implement stack overflow prevention.
/// \return true if some portion of the string matched the regex
/// represented by the bytecode, false otherwise. This is the char16_t overload.
MatchRuntimeResult searchWithBytecode(
    llvh::ArrayRef<uint8_t> bytecode,
    const char16_t *first,
    uint32_t start,
    uint32_t length,
    std::vector<CapturedRange> *captures,
    constants::MatchFlagType matchFlags,
    StackOverflowGuard guard =
#ifdef HERMES_CHECK_NATIVE_STACK
        StackOverflowGuard::nativeStackGuard(
            512 * 1024) // this is a conservative gap that should work in
                        // sanitizer builds
#else
        StackOverflowGuard::depthCounterGuard(128)
#endif
);

/// This is the ASCII overload.
MatchRuntimeResult searchWithBytecode(
    llvh::ArrayRef<uint8_t> bytecode,
    const char *first,
    uint32_t start,
    uint32_t length,
    std::vector<CapturedRange> *captures,
    constants::MatchFlagType matchFlags,
    StackOverflowGuard guard =
#ifdef HERMES_CHECK_NATIVE_STACK
        StackOverflowGuard::nativeStackGuard(
            512 * 1024) // this is a conservative gap that should work in
                        // sanitizer builds
#else
        StackOverflowGuard::depthCounterGuard(128)
#endif
);

} // namespace regex
} // namespace hermes

#endif
