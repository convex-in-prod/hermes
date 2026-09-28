/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -Xsmall-c -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fno-inline -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fno-inline -dump-ir -Xdump-functions=invokeKnown %s | %FileCheck --check-prefix=TARGET %s
// RUN: %shermes -O -fno-inline -dump-ir -Xdump-functions=aliased %s | %FileCheck --check-prefix=LOOSE %s

(function () {
  'use strict';
  function assert(value) {
    if (!value) throw new Error('callback target contract');
  }
  function factory(captured) {
    'noinline';
    return function callback(input) {
      gc();
      return captured.value + input;
    };
  }
  function invokeKnown(fn, input) {
    'noinline';
    return fn(input);
  }
  var first = factory({value: 10});
  var second = factory({value: 20});
  assert(first !== second);
  assert(invokeKnown(first, 1) === 11);
  assert(invokeKnown(second, 2) === 22);
  function invokePhi(flag) {
    'noinline';
    var fn = flag ? factory({value: 30}) : factory({value: 40});
    return fn(3);
  }
  assert(invokePhi(true) === 33 && invokePhi(false) === 43);

  function invokeMixed(fn) {
    'noinline';
    return fn(4);
  }
  assert(invokeMixed(first) === 14);
  assert(invokeMixed(function other(n) { return n * 2; }) === 8);
  function invokeMissing(fn) {
    'noinline';
    return fn(5);
  }
  assert(invokeMissing(first) === 15);
  var caught = false;
  try { invokeMissing(); } catch (e) { caught = e instanceof TypeError; }
  assert(caught);

  globalThis.externalCallback = function externalCallback(fn) {
    return fn(6);
  };
  assert(globalThis.externalCallback(first) === 16);
  assert(globalThis.externalCallback(function (n) { return n * 3; }) === 18);

  function maybeFactory(flag) {
    'noinline';
    if (flag) return factory({value: 50});
  }
  assert(maybeFactory(true)(7) === 57);
  caught = false;
  try { maybeFactory(false)(7); } catch (e) { caught = e instanceof TypeError; }
  assert(caught);

  function recursiveFactory(n) {
    'noinline';
    return n ? recursiveFactory(n - 1) : factory({value: 60});
  }
  assert(recursiveFactory(3)(8) === 68);
  function constructorFactory() {
    'noinline';
    this.value = 70;
    return 0;
  }
  caught = false;
  try { (new constructorFactory())(); } catch (e) { caught = e instanceof TypeError; }
  assert(caught);

  var marker = {};
  function throwingFactory() {
    'noinline';
    return function throwingCallback() { gc(); throw marker; };
  }
  function invokeThrowing(fn) {
    'noinline';
    try { fn(); } catch (e) { return e; }
  }
  assert(invokeThrowing(throwingFactory()) === marker);
  print('known callback targets passed');
})();

// SH deliberately uses unmapped arguments even in loose mode (SpecIncompat.md).
// The shared target analysis still restricts parameter proofs to strict functions.
(function () {
  function aliased(fn) {
    'noinline';
    arguments[0] = function () { return 2; };
    return fn();
  }
  if (aliased(function () { return 1; }) !== 1)
    throw new Error('aliased callback contract');
  print('aliased callbacks passed');
})();

// CHECK: known callback targets passed
// CHECK-NEXT: aliased callbacks passed
// TARGET-LABEL: function invokeKnown(
// TARGET: CallInst {{.*}} %callback(): functionCode, true: boolean, empty: any
// LOOSE-LABEL: function aliased(
// LOOSE: CallInst {{.*}} empty: any, false: boolean, empty: any
