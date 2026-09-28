/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -farray-callbacks -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -farray-callbacks -Xsmall-c -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -exec %s | %FileCheck --match-full-lines %s

function assert(value) {
  if (!value) throw new Error('array callback exception and receiver semantics');
}
var trace = '';
var sentinel = {};
function guarded(values) {
  'noinline';
  try {
    trace += 'enter;';
    return values.some(function (value) {
      trace += 'callback;';
      gc();
      if (value) throw sentinel;
      return false;
    });
  } catch (error) {
    trace += 'catch;';
    return error;
  } finally {
    trace += 'finally;';
  }
}
assert(guarded([1]) === sentinel);
assert(trace === 'enter;callback;catch;finally;');
trace = '';
assert(guarded([0]) === false);
assert(trace === 'enter;callback;finally;');
trace = '';
assert(guarded([]) === false);
assert(trace === 'enter;finally;');
trace = '';
assert(guarded({some: function () { throw sentinel; }}) === sentinel);
assert(trace === 'enter;catch;finally;');

function sloppyReceiver(values, context, expected) {
  'noinline';
  return values.some(function (value, index, array) {
    assert(array === values && index === 0 && value === 1);
    assert(arguments.length === 3);
    gc();
    return this === expected;
  }, context);
}
assert(sloppyReceiver([1], undefined, globalThis));
assert(sloppyReceiver([1], null, globalThis));
assert(sloppyReceiver([1], sentinel, sentinel));

function boxedReceiver(values) {
  'noinline';
  return values.some(function () {
    return typeof this === 'object' && this.valueOf() === 7;
  }, 7);
}
assert(boxedReceiver([1]));

function callbackIdentity(values) {
  'noinline';
  return values.some(function (value) {
    var callback = arguments.callee;
    gc();
    return arguments[0] === value && callback === arguments.callee;
  });
}
assert(callbackIdentity([1]));
print('array callback exception and receiver semantics passed');
// CHECK: array callback exception and receiver semantics passed
