/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -exec %s -Wx,-Xhermes-internal-test-methods | %FileCheck --match-full-lines %s
// RUN: %shermes -O -Xsmall-c -exec %s -Wx,-Xhermes-internal-test-methods | %FileCheck --match-full-lines %s
// RUN: %shermes -O -emit-c -o %t.c %s
// RUN: %FileCheck --check-prefix=C --input-file=%t.c %s

function read(object, key) {
  "noinline";
  return object[key];
}
function assert(value) {
  if (!value) throw new Error('computed load contract');
}
var key = Symbol('key');
var object = {alpha: 1, beta: 2, gamma: 3, delta: 4, [key]: 5};
var keys = Object.keys(object).concat(key);
for (var round = 0; round < 8; ++round) {
  for (var i = 0; i < keys.length; ++i) assert(read(object, keys[i]) === i + 1);
  gc();
}
object.alpha = 10;
assert(read(object, 'alpha') === 10);
var getterCalls = 0;
Object.defineProperty(object, 'alpha', {get() { ++getterCalls; return 11; }});
assert(read(object, 'alpha') === 11 && getterCalls === 1);
delete object.beta;
Object.setPrototypeOf(object, {beta: 12});
assert(read(object, 'beta') === 12);
object.beta = 13;
assert(read(object, 'beta') === 13);
var proxyCalls = 0;
var proxy = new Proxy(object, {get(target, key) { ++proxyCalls; return key; }});
assert(read(proxy, 'alpha') === 'alpha' && proxyCalls === 1);
var coercions = 0;
assert(read(object, {[Symbol.toPrimitive]() {
  ++coercions;
  gc();
  object.beta = 14;
  return 'beta';
}}) === 14 && coercions === 1);
assert(read('abc', 1) === 'b');
assert(read({0: 9}, -0) === 9);
for (var j = 0; j < 200; ++j) object['field' + j] = j;
delete object.field100;
object.field100 = 1000;
assert(read(object, 'field100') === 1000);
var failed = false;
try { read(null, {toString() { throw new Error('must not coerce'); }}); }
catch (e) { failed = e instanceof TypeError; }
assert(failed);
print('computed properties passed');
// CHECK: computed properties passed

// The same keys occupy different slots on these two receiver shapes.
var left = {first: 1, second: 2, [key]: 3};
var right = {[key]: 4, second: 5, first: 6};
for (var round = 0; round < 20; ++round) {
  assert(read(left, 'first') === 1 && read(right, 'first') === 6);
  assert(read(left, key) === 3 && read(right, key) === 4);
  gc();
}
var ordinary = [10, undefined, 30, 40];
for (var round = 0; round < 4; ++round) {
  assert(read(ordinary, 0) === 10 && read(ordinary, 1) === undefined);
  ordinary.shift();
  ordinary.unshift(10);
  gc();
}
delete ordinary[1];
var parent = Object.create(Array.prototype);
Object.defineProperty(parent, '1', {get() { return this[0] + 1; }});
Object.setPrototypeOf(ordinary, parent);
assert(read(ordinary, 1) === 11);
Object.defineProperty(ordinary, '2', {get() { gc(); return 32; }});
assert(read(ordinary, 2) === 32);
ordinary.length = 1;
assert(read(ordinary, 2) === undefined && read(ordinary, 1) === 11);
ordinary['4294967295'] = 99;
ordinary['-1'] = 98;
assert(read(ordinary, 4294967295) === 99 && read(ordinary, -1) === 98);
assert(read(ordinary, -0) === 10 && read(ordinary, 0.5) === undefined);
assert(read(Object.freeze([42]), 0) === 42);
assert(read(new Proxy([1], {get() { return 43; }}), 0) === 43);
assert(read([null, {answer: 44}], 1).answer === 44);
print('polymorphic and ordinary elements passed');
// CHECK-NEXT: polymorphic and ordinary elements passed

var constructors = [Uint8Array, Int8Array, Uint8ClampedArray, Uint16Array,
  Int16Array, Uint32Array, Int32Array, Float32Array, Float64Array];
for (var ctor of constructors) {
  var array = new ctor([0, 12, 23, 34]).subarray(1);
  for (var round = 0; round < 3; ++round) {
    assert(read(array, 0) === 12 && read(array, 1) === 23);
    assert(read(array, -0) === 12 && read(array, '1') === 23);
    assert(read(array, -1) === undefined && read(array, 0.5) === undefined);
    assert(read(array, array.length) === undefined);
    assert(read(array, NaN) === undefined && read(array, Infinity) === undefined);
    assert(read(array, 4294967295) === undefined);
    gc();
  }
  HermesInternal.detachArrayBuffer(array.buffer);
  assert(read(array, 0) === undefined);
}
assert(read(new Int8Array([-128]), 0) === -128);
assert(read(new Int16Array([-32768]), 0) === -32768);
assert(read(new Uint32Array([4294967295]), 0) === 4294967295);
assert(read(new Int32Array([-2147483648]), 0) === -2147483648);
var floating = new Float64Array([NaN, -0, Infinity]);
assert(Number.isNaN(read(floating, 0)) && Object.is(read(floating, 1), -0));
assert(read(floating, 2) === Infinity);
// NaN payloads must not become tagged pointers when read as doubles.
var words = new Uint32Array(floating.buffer);
words[0] = 1; words[1] = 0xffff0000;
assert(Number.isNaN(read(floating, 0)));
assert(read(new BigInt64Array([-1n]), 0) === -1n);
assert(read(new BigUint64Array([18446744073709551615n]), 0) === 18446744073709551615n);
print('typed elements passed');
// CHECK-NEXT: typed elements passed

function numeric(a, b) {
  "noinline";
  a = +a; b = +b;
  return [a | b, a & b, a ^ b, a << b, a >> b, a >>> b];
}
function generic(a, b) {
  "noinline";
  return [a | b, a & b, a ^ b, a << b, a >> b, a >>> b];
}
for (var a of [-0, -1, 2147483648, 4294967295, 4294967297, 1e30, NaN, Infinity, -3.7]) {
  for (var b of [-1, 0, 31, 32, 33, 3.4, Infinity]) {
    assert(JSON.stringify(numeric(a, b)) === JSON.stringify(generic(a, b)));
  }
}
failed = false;
try { generic(3n, 1n); } catch (e) { failed = e instanceof TypeError; }
assert(failed);
print('numeric operations passed');
// CHECK-NEXT: numeric operations passed

// C: _sh_ljs_get_by_val_cached_rjs_inline
// C: _sh_to_uint32_double(_sh_ljs_get_double
