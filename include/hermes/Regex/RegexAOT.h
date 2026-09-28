/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_REGEX_REGEXAOT_H
#define HERMES_REGEX_REGEXAOT_H

#include <stdint.h>

#define HERMES_REGEX_AOT_MAX_CAPTURES 8

// Matchers are shared between translation units of one linked SH module. They
// must not become exports merely because an embedding exports its runtime API.
#if defined(__GNUC__) && !defined(_WIN32)
#define HERMES_REGEX_AOT_HIDDEN __attribute__((visibility("hidden")))
#else
#define HERMES_REGEX_AOT_HIDDEN
#endif

/// Internal generated-C/runtime contract, not an embedding API. Input is
/// borrowed ASCII or UTF-16, with code-unit offsets. No allocation, GC or VM
/// reentry is permitted. On success, ranges contains start/end pairs for the
/// whole match followed by every capture. Other outcomes leave no usable ranges.
/// A fallback restarts the original interpreter search, including its resource
/// checks; it must never be interpreted as no match.
/// The typed executor adapter normalizes matchInputAllAscii to describe storage:
/// set means 8-bit ASCII, clear means UTF-16, even if its contents are ASCII.
enum HermesRegexAOTResult {
  HermesRegexAOTFallback = -1,
  HermesRegexAOTNoMatch = 0,
  HermesRegexAOTMatch = 1,
};

typedef int32_t (*HermesRegexAOTMatcher)(
    const void *input,
    uint32_t length,
    uint32_t start,
    uint32_t matchFlags,
    uint32_t *ranges);

/// Guarded whole-operation lowering. A declined operation returns undefined
/// without changing observable state; generated code executes the original IR.
enum HermesRegexOperation {
  HermesRegexTest,
  HermesRegexTestIndexed,
  HermesRegexReplaceEmpty,
  HermesRegexLowerReplaceEmpty,
  HermesRegexReplaceEmptyEqual,
  HermesRegexLowerReplaceEmptyEqual,
};

#endif
