/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -fguarded-record-loads -fstatic-object-prototype -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fguarded-record-loads -fstatic-object-prototype -Xsmall-c -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fguarded-record-loads -fstatic-object-prototype -fstatic-math-builtins -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fguarded-record-loads -fstatic-object-prototype -dump-ir -Xdump-functions=repeated,nested %s | %FileCheck --check-prefix=IR %s

'use strict';
Object.preventExtensions(Object.prototype);
function assert(value) {
  if (!value) throw new Error('guarded record region semantics');
}
function repeated(record) {
  'noinline';
  return record.x === record.y && record.x === record.z &&
      record.x === record.w && record.x === record.y;
}
function nested(record) {
  'noinline';
  return record.child.x === 1 && record.child.y === 2 &&
      record.child.x === 1 && record.child.y === 2;
}
globalThis.testRepeated = repeated;
globalThis.testNested = nested;
assert(repeated({x: 1, y: 1, z: 1, w: 1}));
assert(!repeated({x: 1, y: 1, z: 2, w: 1}));
assert(repeated({}));
assert(repeated(Object.create(null)));
assert(nested({child: {x: 1, y: 2}}));

var reads = 0;
assert(nested({get child() { ++reads; gc(); return {x: 1, y: 2}; }}));
assert(reads === 4);
reads = 0;
assert(repeated({
  get x() { ++reads; gc(); return 1; }, y: 1, z: 1, w: 1,
}));
assert(reads === 4);
reads = 0;
var inherited = Object.create({get x() { ++reads; return 1; }});
inherited.y = inherited.z = inherited.w = 1;
assert(repeated(inherited));
assert(reads === 4);

var trace = [];
var proxy = new Proxy({x: 1, y: 1, z: 1, w: 1}, {
  get(target, key) { trace.push(key); gc(); return target[key]; },
});
assert(repeated(proxy));
assert(trace.join(',') === 'x,y,x,z,x,w,x,y');
var dictionary = {x: 1, y: 1, z: 1, w: 1};
for (var i = 0; i < 80; ++i) dictionary['key' + i] = i;
assert(repeated(dictionary));

function interleaved(left, right) {
  'noinline';
  return left.x === 1 && right.x === 1 && left.x === 1 && left.x === 1;
}
globalThis.testInterleaved = interleaved;
var left = {x: 1};
assert(interleaved(left, {x: 1}));
assert(!interleaved(left, {get x() { left.x = 2; gc(); return 1; }}));
left.x = 1;
assert(!interleaved(left, {get x() {
  Object.defineProperty(left, 'x', {get: function () { return 2; }});
  return 1;
}}));

function effects(record, effect) {
  'noinline';
  var first = record.x === 1 && record.x === 1 && record.x === 1;
  effect(record);
  return first && record.x === 2 && record.x === 2 && record.x === 2;
}
assert(effects({x: 1}, function (record) { record.x = 2; gc(); }));
function writes(record, alias) {
  'noinline';
  var first = record.x;
  alias.x = 2;
  return first === 1 && record.x === 2 && record.x === 2 && record.x === 2;
}
left = {x: 1};
assert(writes(left, left));

function joined(left, right, skip) {
  'noinline';
  var first = left.x;
  var condition = skip || right.x === 1;
  return condition && left.x === first && left.x === first && left.x === first;
}
globalThis.testJoined = joined;
assert(joined({x: 1}, {x: 1}, false));
assert(joined({x: 1}, null, true));
left = {x: 1};
assert(!joined(left, {get x() { left.x = 2; gc(); return 1; }}, false));

function coercion(record, value) {
  'noinline';
  var first = record.x === 1 && record.x === 1 && record.x === 1;
  var number = value * 2;
  return first && number === 4 && record.x === 2 && record.x === 2 && record.x === 2;
}
globalThis.testCoercion = coercion;
left = {x: 1};
assert(coercion(left, {valueOf: function () { left.x = 2; return 2; }}));

function numeric(record, value) {
  'noinline';
  var first = record.x === 1 && record.x === 1 && record.x === 1;
  var rounded = Math.round(value * 2);
  return first && rounded === 4 && record.x === 1 && record.x === 1 && record.x === 1;
}
globalThis.testNumeric = numeric;
assert(numeric({x: 1}, 2));
left = {x: 1};
assert(!numeric(left, {valueOf: function () { left.x = 2; gc(); return 2; }}));
function mathCoercion(record, value) {
  'noinline';
  var first = record.x === 1 && record.x === 1 && record.x === 1;
  var rounded = Math.round(value);
  return first && rounded === 2 && record.x === 1 && record.x === 1 && record.x === 1;
}
globalThis.testMathCoercion = mathCoercion;
assert(mathCoercion({x: 1}, 2));
left = {x: 1};
assert(!mathCoercion(left, {valueOf: function () { left.x = 2; return 2; }}));

var oldConstructor = Object.getOwnPropertyDescriptor(Object.prototype, 'constructor');
reads = 0;
Object.defineProperty(Object.prototype, 'constructor', {
  configurable: true,
  get: function () { ++reads; return 1; },
});
function standardKeys(record) {
  'noinline';
  return record.constructor === 1 && record.constructor === 1 &&
      record.constructor === 1;
}
assert(standardKeys({}));
assert(reads === 3);
Object.defineProperty(Object.prototype, 'constructor', oldConstructor);

var sentinel = {};
var caught;
try {
  repeated({get x() { throw sentinel; }});
} catch (error) { caught = error; }
assert(caught === sentinel);
caught = undefined;
try { repeated(null); } catch (error) { caught = error; }
assert(caught instanceof TypeError);
var revoked = Proxy.revocable({}, {});
revoked.revoke();
caught = undefined;
try { repeated(revoked.proxy); } catch (error) { caught = error; }
assert(caught instanceof TypeError);

print('guarded record regions passed');
// CHECK: guarded record regions passed

// One receiver guard covers all four named fields; nested receivers need their
// own proof. Generic getter paths retain the original read sequence.
// IR-LABEL: function repeated(
// IR: NativeCallInst {{.*}}extern_c(bool _sh_ljs_is_plain_record
// IR: CondBranchInst
// IR: PhiInst
// IR: function_end
// IR-LABEL: function nested(
// IR: NativeCallInst {{.*}}extern_c(bool _sh_ljs_is_plain_record
// IR: NativeCallInst {{.*}}extern_c(bool _sh_ljs_is_plain_record
// IR: function_end
