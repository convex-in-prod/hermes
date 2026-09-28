/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -Xsmall-c -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O0 -exec %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -dump-ir -Xdump-functions=optional %s | %FileCheck --check-prefix=IR %s
// RUN: %shermes -O -dump-ir -Xdump-functions=inLoop %s | %FileCheck --check-prefix=LOOP %s

function optional(flag, value) {
  'noinline';
  return {base: 1, ...(flag ? {item: value} : {}), tail: 2};
}
for (var flag of [false, true]) {
  var result = optional(flag, 7);
  print(Object.keys(result).join(' '), JSON.stringify(result));
}
var order = '';
function observe(label, value) { order += label; gc(); return value; }
function sequence(flag) {
  'noinline';
  return {a: observe('a', 1),
          ...(flag ? {b: observe('b', 2), a: observe('c', 3)} : {}),
          c: observe('d', 4)};
}
print(JSON.stringify(sequence(false)), order);
order = '';
print(JSON.stringify(sequence(true)), order);
function alternatives(flag) {
  'noinline';
  return {start: 0, ...(flag ? {x: 1, y: 2} : {y: 3, x: 4}), end: 5};
}
print(JSON.stringify(alternatives(false)), JSON.stringify(alternatives(true)));
function keys(flag) {
  'noinline';
  return {b: 2, ...(flag ? {2: 'two', 1: 'one', ['__proto__']: 'own'} : {}), a: 1};
}
var keyed = keys(true);
print(Object.keys(keyed).join(' '), keyed.__proto__, Object.getPrototypeOf(keyed) === Object.prototype);

// The source is observable here; mutation, accessors, symbols and proxy traps
// must remain part of the ordinary CopyDataProperties operation.
function exposed(flag, change) {
  'noinline';
  var source = flag ? {x: 1} : {};
  change(source);
  return {start: 0, ...source};
}
print(JSON.stringify(exposed(true, value => { value.x = 9; value.y = 3; })));
var reads = 0;
print(JSON.stringify(exposed(false, value => Object.defineProperty(value, 'x', {
  enumerable: true, get() { gc(); ++reads; return 11; }
}))), reads);
var symbol = Symbol('key');
var symbols = exposed(false, value => { value[symbol] = 13; });
print(symbols[symbol], Object.getOwnPropertySymbols(symbols).length);
var traps = '';
var proxy = new Proxy({x: 17}, {
  ownKeys(target) { traps += 'k'; return Reflect.ownKeys(target); },
  getOwnPropertyDescriptor(target, key) { traps += 'd'; return Reflect.getOwnPropertyDescriptor(target, key); },
  get(target, key) { traps += 'g'; return target[key]; }
});
function external(flag, source) { 'noinline'; return {...(flag ? source : {})}; }
print(JSON.stringify(external(true, proxy)), traps);

var marker = {};
function caught(flag) {
  'noinline';
  try { return {a: 1, ...(flag ? {x: observe('t', 1), y: (() => { throw marker; })()} : {}), b: 2}; }
  catch (error) { gc(); return error === marker; }
}
print(JSON.stringify(caught(false)), caught(true));
function many() {
  'noinline';
  var results = [];
  for (var i = 0; i < 4; ++i) results.push(optional(i & 1, i));
  return results;
}
print(JSON.stringify(many()));

var closed = 0;
function* values() { try { yield 1; yield 2; yield 3; } finally { ++closed; } }
function inLoop(flag) {
  'noinline';
  var items = [];
  for (var value of values()) {
    var item = {base: value, ...(flag ? {item: observe('l', value)} : {})};
    items.push(item);
    if (value === 2) throw items;
  }
}
try { inLoop(true); } catch (items) { print(JSON.stringify(items), closed); }

// CHECK: base tail {"base":1,"tail":2}
// CHECK-NEXT: base item tail {"base":1,"item":7,"tail":2}
// CHECK-NEXT: {"a":1,"c":4} ad
// CHECK-NEXT: {"a":3,"b":2,"c":4} abcd
// CHECK-NEXT: {"start":0,"y":3,"x":4,"end":5} {"start":0,"x":1,"y":2,"end":5}
// CHECK-NEXT: 1 2 b __proto__ a own true
// CHECK-NEXT: {"start":0,"x":9,"y":3}
// CHECK-NEXT: {"start":0,"x":11} 1
// CHECK-NEXT: 13 1
// CHECK-NEXT: {"x":17} kdg
// CHECK-NEXT: {"a":1,"b":2} true
// CHECK-NEXT: [{"base":1,"tail":2},{"base":1,"item":1,"tail":2},{"base":1,"tail":2},{"base":1,"item":3,"tail":2}]
// CHECK-NEXT: [{"base":1,"item":1},{"base":2,"item":2}] 1
// IR-LABEL: function optional(
// IR: AllocObjectLiteralInst
// IR-NOT: AllocObjectLiteralInst
// IR-NOT: copyDataProperties
// IR: function_end
// LOOP-LABEL: function inLoop(
// LOOP: TryStartInst
// LOOP-NOT: copyDataProperties
// LOOP: AllocObjectLiteralInst
// LOOP-NOT: copyDataProperties
// LOOP-NOT: AllocObjectLiteralInst
// LOOP: function_end
