/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -fnative-integer-registers -fstatic-math-builtins -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fnative-integer-registers -fstatic-math-builtins -Xsmall-c -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fstatic-math-builtins -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fnative-integer-registers -fstatic-math-builtins -emit-c -o - %s | %FileCheck --check-prefix=C %s

'use strict';
function assert(value) {
  if (!value) throw new Error('native integer storage semantics');
}
function mix(value, rounds) {
  'noinline';
  var word = value >>> 0;
  for (var i = 0; i < rounds; ++i) {
    word = Math.imul(word, 0xcc9e2d51);
    word = ((word << 15) | (word >>> 17)) >>> 0;
    word = Math.imul(word, 0x1b873593);
    word = (word ^ (word >>> 16)) >>> 0;
  }
  return word;
}
globalThis.testMix = mix;
function signed(value, shift) {
  'noinline';
  var a = value | 0;
  var b = shift | 0;
  var c = (a << b) ^ (a >> b);
  return c | 0;
}
globalThis.testSigned = signed;
function swap(left, right, rounds) {
  'noinline';
  left = left | 0;
  right = right | 0;
  for (var i = 0; i < rounds; ++i) {
    var old = left;
    left = right;
    right = old;
  }
  return [left, right];
}
globalThis.testSwap = swap;
function negativeZero(choose) {
  'noinline';
  var value = choose ? -0 : 0;
  return 1 / value;
}
globalThis.testNegativeZero = negativeZero;
function dynamicMultiply(left, right) {
  'noinline';
  return Math.imul(left, right) >>> 0;
}
globalThis.testDynamicMultiply = dynamicMultiply;
function captured(value) {
  'noinline';
  var word = value >>> 0;
  return function () { gc(); return word; };
}
globalThis.testCaptured = captured;
function mixedPhi(value, choose) {
  'noinline';
  var word = value | 0;
  if (choose) word = -0;
  return word;
}
globalThis.testMixedPhi = mixedPhi;
assert(negativeZero(true) === -Infinity);
assert(negativeZero(false) === Infinity);
assert(Object.is(mixedPhi(3, true), -0));
assert(mixedPhi(3, false) === 3);
assert(swap(-2147483648, 2147483647, 3).join(',') === '2147483647,-2147483648');

// Compute the reference with BigInt so the test does not duplicate the native
// Number/int32 lowering being exercised, including the wrapping multiply.
var mask = (1n << 32n) - 1n;
function reference(value, rounds) {
  var word = BigInt(value >>> 0);
  for (var i = 0; i < rounds; ++i) {
    word = (word * 3432918353n) & mask;
    word = ((word << 15n) | (word >> 17n)) & mask;
    word = (word * 461845907n) & mask;
    word = (word ^ (word >> 16n)) & mask;
  }
  return Number(word);
}
var inputs = [0, -0, 1, -1, 2147483647, -2147483648, 2147483648,
  4294967295, 4294967296, -4294967297, 9007199254740991,
  1.75, -1.75, NaN, Infinity, -Infinity];
for (var i = 0; i < inputs.length; ++i) {
  for (var rounds = 0; rounds < 9; ++rounds)
    assert(mix(inputs[i], rounds) === reference(inputs[i], rounds));
  for (var shift = -1; shift < 34; ++shift) {
    var value = BigInt(inputs[i] | 0);
    var bits = BigInt(shift & 31);
    var expected = BigInt.asIntN(32, (value << bits) ^ (value >> bits));
    assert(signed(inputs[i], shift) === Number(expected));
  }
}
var effects = 0;
assert(mix({valueOf: function () { ++effects; gc(); return -1; }}, 5) === reference(-1, 5));
assert(effects === 1);
var order = '';
assert(dynamicMultiply(
  {valueOf: function () { order += 'l'; gc(); return 4294967295; }},
  {valueOf: function () { order += 'r'; gc(); return 3; }}
) === 4294967293);
assert(order === 'lr');
assert(captured(-1)() === 4294967295);
var sentinel = {};
var caught;
try { mix({valueOf: function () { throw sentinel; }}, 2); }
catch (error) { caught = error; }
assert(caught === sentinel);
caught = undefined;
try { mix(1n, 2); } catch (error) { caught = error; }
assert(caught instanceof TypeError);
print('native integer registers passed');
// CHECK: native integer registers passed
// C-LABEL: static SHLegacyValue _{{[0-9]+}}_mix(SHRuntime *shr) {
// C: {{u?int(32|64)_t}} ni{{[0-9]+}} = 0;
// C-NOT: frame[{{[0-9]+}}] =
// C: uint32_t bits =
// C-NOT: frame[{{[0-9]+}}] =
// C-LABEL: static SHLegacyValue _{{[0-9]+}}_signed(SHRuntime *shr) {
