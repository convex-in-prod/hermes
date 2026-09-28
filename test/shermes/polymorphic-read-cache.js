// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// RUN: %shermes -O -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -Xsmall-c -exec %s | %FileCheck --match-full-lines %s

function read(object) {
  'noinline';
  return object.value;
}
function check(actual, expected) {
  if (actual !== expected) throw new Error('named read contract');
}
var first = {value: 1};
var second = {before: 2, value: 3};
for (var i = 0; i < 20; ++i) {
  check(read(first), 1);
  check(read(second), 3);
  gc();
}
first.value = 4;
check(read(first), 4);
var getterCalls = 0;
Object.defineProperty(first, 'value', {configurable: true, get() {
  ++getterCalls;
  gc();
  return 5;
}});
check(read(first), 5);
check(getterCalls, 1);
check(read(second), 3);
delete second.value;
Object.setPrototypeOf(second, {value: 6});
check(read(second), 6);
Object.setPrototypeOf(second, {before: 7, value: 8});
check(read(second), 8);
second.value = 9;
check(read(second), 9);
var proxyCalls = 0;
check(read(new Proxy(second, {get(target, key, receiver) {
  ++proxyCalls;
  return receiver === this ? 0 : 10;
}})), 10);
check(proxyCalls, 1);
String.prototype.value = 11;
check(read('text'), 11);
String.prototype.value = 12;
check(read('text'), 12);
delete String.prototype.value;
check(read('text'), undefined);
for (var i = 0; i < 200; ++i) second['field' + i] = i;
delete second.field100;
delete second.value;
check(read(second), 8);
second.value = 13;
check(read(second), 13);
var threw = false;
try { read(null); } catch (e) { threw = e instanceof TypeError; }
check(threw, true);
print('named reads passed');
// CHECK: named reads passed
