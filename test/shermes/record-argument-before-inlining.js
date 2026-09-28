/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O0 -exec %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O4 -verify-ir %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -dump-ir -Xdump-functions=protectedCaller %s | %FileCheck --check-prefix=CALLER %s
// RUN: %shermes -O -dump-ir -Xdump-functions=capturedRead %s | %FileCheck --check-prefix=CAPTURE %s

// Inlining into one caller duplicates the captured variable's initialization.
// Promote before that happens, so the remaining call in a try region can pass
// fields and the shared closure body can read the captured field directly.
(function () {
  'use strict';
  function effect(value) { 'noinline'; gc(); return value; }
  function captureBeforeInlining(record, value) {
    'inline';
    var read = function capturedRead() { return record.value; };
    value = effect(value) + effect(value) + effect(value) + effect(value) +
        effect(value) + effect(value) + effect(value) + effect(value) +
        effect(value) + effect(value) + effect(value) + effect(value);
    return {read, value};
  }
  function ordinaryCaller(value) { 'noinline'; return captureBeforeInlining({value},value); }
  function protectedCaller(value) {
    'noinline';
    try { return captureBeforeInlining({value},value); } catch (error) { throw error; }
  }
  print(ordinaryCaller(7).read(),protectedCaller(11).read());
})();
// CHECK: 7 11

// CALLER-LABEL: function protectedCaller(
// CALLER-NOT: AllocObjectLiteralInst
// CALLER: function_end

// CAPTURE-LABEL: function capturedRead(
// CAPTURE-NOT: LoadPropertyInst
// CAPTURE: function_end
