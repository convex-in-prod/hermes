/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -Xhermes-internal-test-methods -O %s | %FileCheck --match-full-lines %s
// RUN: %shermes -exec %s -Wx,-Xhermes-internal-test-methods | %FileCheck --match-full-lines %s

var source = {a: 1, b: {nested: 2}, c: "text"};
var copy = {...source};
print(copy.a, copy.b.nested, copy.c, copy !== source);
// CHECK: 1 2 text true
copy.a = 17;
print(source.a, copy.a);
// CHECK-NEXT: 1 17

// A getter must still run with the original receiver and order, including GC.
var getterReads = 0;
var getterSource = {};
Object.defineProperty(getterSource, "a", {
  enumerable: true,
  get: function () { ++getterReads; gc(); return 9; }
});
var getterCopy = {...getterSource};
print(getterReads, getterCopy.a);
// CHECK-NEXT: 1 9

// Symbols, non-enumerable fields and indexed properties retain the generic path.
var symbol = Symbol("s");
var symbolSource = {a: 3};
symbolSource[symbol] = 4;
var symbolCopy = {...symbolSource};
print(symbolCopy.a, symbolCopy[symbol], Object.getOwnPropertySymbols(symbolCopy).length);
// CHECK-NEXT: 3 4 1

var hiddenSource = {a: 5};
Object.defineProperty(hiddenSource, "hidden", {value: 6, enumerable: false});
var hiddenCopy = {...hiddenSource};
print(hiddenCopy.a, hiddenCopy.hidden === undefined);
// CHECK-NEXT: 5 true

var indexedSource = {a: 7};
indexedSource["0"] = 8;
var indexedCopy = {...indexedSource};
print(indexedCopy["0"], indexedCopy.a, Object.keys(indexedCopy).join(","));
// CHECK-NEXT: 8 7 0,a

var prototype = {inherited: 10};
var customSource = Object.create(prototype);
customSource.a = 11;
var customSourceCopy = {...customSource};
var customTarget = {__proto__: prototype, ...{a: 12}};
print(customSourceCopy.a, customSourceCopy.inherited === undefined,
    customTarget.a, customTarget.inherited);
// CHECK-NEXT: 11 true 12 10
var nullSource = Object.create(null);
nullSource.a = 18;
var customBothTarget = Object.create(prototype);
HermesInternal.copyDataProperties(customBothTarget, customSource);
var nullSourceCopy = {...nullSource};
print(customBothTarget.a, customBothTarget.inherited,
    nullSourceCopy.a, Object.getPrototypeOf(nullSourceCopy) === Object.prototype);
// CHECK-NEXT: 11 10 18 true

// Adopting the source's property layout must not change the target's prototype.
var sourcePrototype = {fromSource: 21};
var targetPrototype = {fromTarget: 22};
var mismatchedSource = Object.create(sourcePrototype);
mismatchedSource.a = 23;
var mismatchedTarget = Object.create(targetPrototype);
HermesInternal.copyDataProperties(mismatchedTarget, mismatchedSource);
print(mismatchedTarget.a, mismatchedTarget.fromSource === undefined,
    mismatchedTarget.fromTarget, Object.getPrototypeOf(mismatchedTarget) === targetPrototype);
// CHECK-NEXT: 23 true 22 true

var existingTarget = {a: 0};
HermesInternal.copyDataProperties(existingTarget, {a: 13, b: 14});
print(existingTarget.a, existingTarget.b);
// CHECK-NEXT: 13 14

// Medium records use indirect named storage on the source and target. They
// still satisfy the same data-only contract and must retain independent slots.
var mediumSource = {
  a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8,
  i: 9, j: 10, k: 11, l: 12
};
var mediumCopy = {...mediumSource};
mediumCopy.a = 41;
mediumCopy.l = 52;
gc();
print(mediumSource.a, mediumSource.l, mediumCopy.a, mediumCopy.l,
    Object.keys(mediumCopy).length);
// CHECK-NEXT: 1 12 41 52 12

var mediumSourcePrototype = {inheritedSource: 61};
var mediumTargetPrototype = {inheritedTarget: 62};
var mediumCustomSource = Object.create(mediumSourcePrototype);
for (var mediumKey of Object.keys(mediumSource))
  mediumCustomSource[mediumKey] = mediumSource[mediumKey];
var mediumCustomTarget = Object.create(mediumTargetPrototype);
HermesInternal.copyDataProperties(mediumCustomTarget, mediumCustomSource);
gc();
print(mediumCustomTarget.a, mediumCustomTarget.l,
    mediumCustomTarget.inheritedSource === undefined,
    mediumCustomTarget.inheritedTarget, Object.keys(mediumCustomTarget).length);
// CHECK-NEXT: 1 12 true 62 12

var callableTarget = function () {};
HermesInternal.copyDataProperties(callableTarget, {a: 19});
print(callableTarget.a, typeof callableTarget);
// CHECK-NEXT: 19 function

var gcSource = {a: {nested: 15}, b: 16};
var gcCopy = {...gcSource};
gc();
print(gcCopy.a.nested, gcCopy.b);
// CHECK-NEXT: 15 16
