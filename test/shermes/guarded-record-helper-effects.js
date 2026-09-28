/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -fguarded-record-loads -fstatic-object-prototype -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fguarded-record-loads -fstatic-object-prototype -Xsmall-c -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O0 -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fguarded-record-loads -fstatic-object-prototype -dump-ir -Xdump-functions=acrossHelper %s | %FileCheck --check-prefix=IR %s
// RUN: %shermes -O -fguarded-record-loads -fstatic-object-prototype -dump-ir -Xdump-functions=acrossNested %s | %FileCheck --check-prefix=NESTED %s

(function () {
'use strict';
Object.preventExtensions(Object.prototype);
function assert(value) {
  if (!value) throw new Error('guarded helper effects');
}
function leaf(record) {
  'noinline';
  return record.value === 1;
}
function relay(record) {
  'noinline';
  return leaf(record);
}
function acrossHelper(record, other) {
  'noinline';
  var before = record.value;
  relay(other);
  return record.value === before && record.value === before &&
      record.value === before && record.value === before;
}
globalThis.testAcrossHelper = acrossHelper;
assert(acrossHelper({value: 1}, {value: 1}));
assert(acrossHelper({}, Object.create(null)));
var receiver = {value: 1};
assert(acrossHelper(receiver, receiver));
var reads = 0;
assert(!acrossHelper(receiver, {get value() {
  ++reads;
  receiver.value = 2;
  gc();
  return 1;
}}));
assert(reads === 1);
receiver.value = 1;
var proxy = new Proxy({value: 1}, {get(target, key) {
  ++reads;
  receiver.value = 2;
  gc();
  return target[key];
}});
assert(!acrossHelper(receiver, proxy));
assert(reads === 2);
receiver.value = 1;
var inherited = Object.create({get value() { receiver.value = 2; return 1; }});
assert(!acrossHelper(receiver, inherited));
receiver.value = 1;
var caught = false;
try {
  acrossHelper(receiver, {get value() { receiver.value = 2; throw 17; }});
} catch (error) {
  caught = error === 17;
}
assert(caught && receiver.value === 2);

function childRead(other) {
  'noinline';
  return other.child.value;
}
function acrossChild(record, other) {
  'noinline';
  var before = record.value;
  childRead(other);
  return record.value === before && record.value === before &&
      record.value === before && record.value === before;
}
globalThis.testAcrossChild = acrossChild;
receiver.value = 1;
assert(!acrossChild(receiver, {child: {get value() { receiver.value = 2; return 1; }}}));

function nestedLeaf(record) {
  'noinline';
  if (record.fail) {
    target.a = 99;
    throw 31;
  }
  return record.inner.value;
}
function nestedRelay(record, enabled) {
  'noinline';
  return enabled ? nestedLeaf(record.child) : 0;
}
function acrossNested(record, other, enabled) {
  'noinline';
  var a = record.a, b = record.b, c = record.c;
  var d = record.d, e = record.e, f = record.f;
  nestedRelay(other, enabled);
  return record.a === a && record.b === b && record.c === c &&
      record.d === d && record.e === e && record.f === f &&
      record.a === a && record.b === b && record.c === c &&
      record.d === d && record.e === e && record.f === f;
}
globalThis.testAcrossNested = acrossNested;
var target = {a: 1, b: 2, c: 3, d: 4, e: 5, f: 6};
assert(acrossNested(target, {child: {inner: {value: 1}}}, true));
assert(acrossNested(target, {}, false));
assert(acrossNested(target, {child: null}, false));
reads = 0;
var nestedGetter = {get child() { ++reads; target.a = 10; gc(); return {}; }};
assert(acrossNested(target, nestedGetter, false) && reads === 0);
var childGetter = {child: {get inner() {
  ++reads; target.a = 11; gc(); return {value: 1};
}}};
assert(!acrossNested(target, childGetter, true) && reads === 1);
var grandchildGetter = {child: {inner: {get value() {
  ++reads; target.a = 12; gc(); return 1;
}}}};
assert(!acrossNested(target, grandchildGetter, true) && reads === 2);
var nestedProxy = {child: new Proxy({inner: {value: 1}}, {get(object, key) {
  ++reads; target.a = 13; gc(); return object[key];
}})};
assert(!acrossNested(target, nestedProxy, true) && reads === 4);
var throwingChild = {child: {get inner() {
  ++reads; target.a = 14; throw 23;
}}};
caught = false;
try { acrossNested(target, throwingChild, true); }
catch (error) { caught = error === 23; }
assert(caught && target.a === 14 && reads === 5);
assert(acrossNested(target, throwingChild, false) && reads === 5);
var prototypeChild = {child: Object.create({get inner() {
  ++reads; target.a = 15; return {value: 1};
}})};
assert(!acrossNested(target, prototypeChild, true) && reads === 6);
var nullChild = Object.create(null);
nullChild.inner = {value: 1};
assert(acrossNested(target, {child: nullChild}, true));
var self = {a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, value: 1};
self.child = self;
self.inner = self;
assert(acrossNested(self, self, true));
caught = false;
try { acrossNested(target, {child: {fail: true, inner: {value: 1}}}, true); }
catch (error) { caught = error === 31; }
assert(caught && target.a === 99);

function mutating(other, target) {
  'noinline';
  target.value = 2;
  return other.value;
}
function mutatingRelay(other, target) {
  'noinline';
  return mutating(other, target);
}
function acrossMutation(record, other) {
  'noinline';
  var before = record.value;
  mutatingRelay(other, record);
  return record.value === before && record.value === before &&
      record.value === before && record.value === before;
}
globalThis.testAcrossMutation = acrossMutation;
assert(!acrossMutation({value: 1}, {value: 1}));

function coerces(other) {
  'noinline';
  return other + 1;
}
function acrossCoercion(record, other) {
  'noinline';
  var before = record.value;
  coerces(other);
  return record.value === before && record.value === before &&
      record.value === before && record.value === before;
}
globalThis.testAcrossCoercion = acrossCoercion;
receiver.value = 1;
assert(!acrossCoercion(receiver, {valueOf() { receiver.value = 2; return 1; }}));

var recursiveTarget;
function recursive(other, depth) {
  'noinline';
  if (depth) return recursive(other, 0);
  recursiveTarget.value = 2;
  return other.value;
}
function acrossRecursive(record, other) {
  'noinline';
  var before = record.value;
  recursive(other, 1);
  return record.value === before && record.value === before &&
      record.value === before && record.value === before;
}
globalThis.testAcrossRecursive = acrossRecursive;
recursiveTarget = {value: 1};
assert(!acrossRecursive(recursiveTarget, {value: 1}));

print('guarded helper effects passed');
})();
// CHECK: guarded helper effects passed
// IR-LABEL: function acrossHelper(
// IR: NativeCallInst
// IR: NativeCallInst
// IR: CallInst
// IR: PhiInst
// NESTED-LABEL: function acrossNested(
// NESTED-DAG: %[[CHILD:[0-9]+]] = LoadPropertyInst {{.*}} "child"
// NESTED-DAG: NativeCallInst {{.*}} %[[CHILD]]: any
// NESTED-DAG: %[[INNER:[0-9]+]] = LoadPropertyInst {{.*}} "inner"
// NESTED-DAG: NativeCallInst {{.*}} %[[INNER]]: any
// NESTED-DAG: CallInst {{.*}} %nestedRelay(): functionCode
// NESTED: PhiInst
