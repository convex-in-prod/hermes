/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fno-inline -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -Xsmall-c -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O0 -exec %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O4 -verify-ir %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fno-inline -dump-ir -Xdump-functions=make %s | %FileCheck --check-prefix=IR %s
// RUN: %shermes -O -fno-inline -dump-ir -Xdump-functions=read %s | %FileCheck --check-prefix=READ %s
// RUN: %shermes -O -fno-inline -dump-ir -Xdump-functions=conditionalWrite %s | %FileCheck --check-prefix=WRITE %s
// RUN: %shermes -O -fno-inline -dump-ir -Xdump-functions=overwrite %s | %FileCheck --check-prefix=OVERWRITE %s

(function () {
  'use strict';
  let trace = '';
  let reenter = false;
  let nested;
  function check(value) {
    if (!value) throw new Error('record control-flow contract');
  }
  function effect(value, fail) {
    'noinline';
    trace += value;
    if (reenter) {
      reenter = false;
      nested = make(false, {count: 20}, false);
    }
    gc();
    if (fail) throw new Error('initializer');
    return value;
  }
  function capture(record) {
    'noinline';
    return function read() {
      'noinline';
      gc();
      return record.child.count + record.value;
    };
  }
  function make(select, child, fail) {
    'noinline';
    return capture({child, value: select ? effect(7, fail) : effect(13, fail)});
  }
  const child = {count: 2};
  const first = make(true, child, false);
  const second = make(false, child, false);
  check(trace === '713' && first() === 9 && second() === 15);
  child.count = 5;
  check(first() === 12 && second() === 18);
  reenter = true;
  const outer = make(true, child, false);
  check(outer() === 12 && nested() === 33);
  try {
    make(false, child, true);
    check(false);
  } catch (error) {
    check(error.message === 'initializer');
  }

  function captureOverwritten(record) {
    'noinline';
    return function () { gc(); return record.child.count + record.value; };
  }
  // General property writes are not the proven slot initialization admitted by
  // this pass. Keep this callee separate from the positive construction cases.
  function overwrite(select, child) {
    'noinline';
    const record = {child, value: effect(1, false)};
    record.value = select ? effect(2, false) : effect(3, false);
    record.value = effect(5, false);
    return captureOverwritten(record);
  }
  trace = '';
  const overwritten = overwrite(true, child);
  check(overwritten() === 10 && trace === '125');
  trace = '';
  check(overwrite(false, child)() === 10 && trace === '135');

  function readConditional(record) {
    'noinline';
    return function () { gc(); return record.value; };
  }
  function conditionalWrite(select) {
    'noinline';
    const record = {value: 0};
    if (select) record.value = 42;
    return readConditional(record);
  }
  check(conditionalWrite(false)() === 0 && conditionalWrite(true)() === 42);

  let saved;
  function saveAndThrow(record) {
    'noinline';
    saved = function () { gc(); return record.value; };
    throw new Error('callee');
  }
  function caught(select) {
    'noinline';
    const record = {value: select ? effect(1, false) : effect(2, false)};
    try {
      return saveAndThrow(record);
    } catch (error) {
      check(error.message === 'callee');
      record.value = 31;
      return saved;
    }
  }
  check(caught(false)() === 31 && caught(true)() === 31);
  print('record control flow passed');
})();
// CHECK: record control flow passed

// IR-LABEL: function make(
// IR-NOT: AllocObjectLiteralInst
// IR: function_end

// READ-LABEL: function read(
// READ-NOT: "value": string
// READ-NOT: "child": string
// READ: function_end

// WRITE-LABEL: function conditionalWrite(
// WRITE: AllocObjectLiteralInst
// WRITE: function_end

// OVERWRITE-LABEL: function overwrite(
// OVERWRITE: AllocObjectLiteralInst
// OVERWRITE: function_end
