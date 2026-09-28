/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -Xsmall-c -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fno-inline -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -dump-ir -Xdump-functions=withCatch %s | %FileCheck --check-prefix=IR %s

(function () {
  'use strict';
  function assert(value) {
    if (!value) throw new Error('inline object reader contract');
  }
  function read(value) {
    var sum = value.left + value.right;
    return sum > 0 ? sum : 0;
  }
  function withCatch(left, right) {
    'noinline';
    try { return read({left: left | 0, right: right | 0}); }
    catch (e) { return -1; }
  }
  assert(withCatch(3, 5) === 8);
  assert(withCatch(-3, -5) === 0);
  assert(withCatch({valueOf() { throw 1; }}, 5) === -1);

  var marker = {value: 19};
  var finallyCount = 0;
  function readOrThrow(value) {
    var sum = value.left + value.right;
    gc();
    if (value.fail) throw value.error;
    return sum;
  }
  function withFinally(fail, error) {
    'noinline';
    var retained = {value: 23};
    try {
      return readOrThrow({left: 3, right: 5, fail: fail, error: error});
    } catch (e) {
      gc();
      assert(retained.value === 23);
      return e;
    } finally {
      gc();
      assert(retained.value === 23);
      ++finallyCount;
    }
  }
  assert(withFinally(false, marker) === 8);
  assert(withFinally(true, marker) === marker);
  assert(withFinally(true, undefined) === undefined);
  assert(withFinally(true, 0) === 0);
  assert(finallyCount === 4);

  // Object promotion must still reject a value exposed to an unknown callee.
  function escapingReader(value, consumer) {
    consumer(value);
    return value.left + value.right;
  }
  function escapingTry(consumer) {
    'noinline';
    try { return escapingReader({left: 3, right: 5}, consumer); }
    catch (e) { return e; }
  }
  var escaped;
  assert(escapingTry(function (value) { escaped = value; gc(); value.left = 9; }) === 14);
  assert(escaped.left === 9 && escaped.right === 5);
  assert(escapingTry(function () { throw marker; }) === marker);
  print('inline object readers in try passed');
})();

// CHECK: inline object readers in try passed
// IR-LABEL: function withCatch(
// IR-NOT: AllocObject
// IR-NOT: LoadPropertyInst
// IR-NOT: CallInst
// IR: FAddInst
// IR-NOT: AllocObject
// IR-NOT: LoadPropertyInst
// IR-NOT: CallInst
// IR: function_end
