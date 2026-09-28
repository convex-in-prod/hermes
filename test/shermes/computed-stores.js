/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -Xsmall-c -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -emit-c -o %t.c %s
// RUN: %FileCheck --check-prefix=C --input-file=%t.c %s

function store(object, key, value) {
  "noinline";
  "use strict";
  object[key] = value;
}
function loose(object, key, value) {
  "noinline";
  object[key] = value;
}
function assert(value) {
  if (!value) throw new Error('computed store contract');
}
function throwsTypeError(body) {
  var failed = false;
  try { body(); } catch (e) { failed = e instanceof TypeError; }
  assert(failed);
}
var symbol = Symbol('field');
var keys = ['alpha', 'beta', 'gamma', 'delta', symbol];
for (var round = 0; round < 8; ++round) {
  var object = {};
  for (var i = 0; i < keys.length; ++i) store(object, keys[i], {value: i});
  gc();
  for (var i = 0; i < keys.length; ++i) assert(object[keys[i]].value === i);
  for (var i = keys.length - 1; i >= 0; --i) store(object, keys[i], i + 10);
  assert(object.alpha === 10 && object[symbol] === 14);
  assert(Object.keys(object).join(',') === 'alpha,beta,gamma,delta');
}

// A cached addition must observe later changes on the same parent object.
var parent = {};
var receiver;
var calls = 0;
for (var round = 0; round < 3; ++round) store(Object.create(parent), 'alpha', 1);
Object.defineProperty(parent, 'alpha', {
  configurable: true,
  set(value) { receiver = this; ++calls; assert(value === 7); }
});
var child = Object.create(parent);
store(child, 'alpha', 7);
assert(receiver === child && calls === 1 && !Object.hasOwn(child, 'alpha'));
Object.defineProperty(parent, 'alpha', {value: 8, writable: false});
throwsTypeError(() => store(Object.create(parent), 'alpha', 9));
var ignored = Object.create(parent);
loose(ignored, 'alpha', 9);
assert(ignored.alpha === 8 && !Object.hasOwn(ignored, 'alpha'));

var sealed = Object.preventExtensions({alpha: 1});
store(sealed, 'alpha', 2);
assert(sealed.alpha === 2);
throwsTypeError(() => store(sealed, 'beta', 3));
Object.freeze(sealed);
throwsTypeError(() => store(sealed, 'alpha', 3));
loose(sealed, 'alpha', 3);
assert(sealed.alpha === 2);
for (var round = 0; round < 3; ++round) store({}, 'delta', 1);
throwsTypeError(() => store(Object.preventExtensions({}), 'delta', 2));
function namedStore(object) {
  "noinline";
  "use strict";
  object.delta = 1;
}
for (var round = 0; round < 3; ++round) namedStore({});
throwsTypeError(() => namedStore(Object.preventExtensions({})));
var changed = {alpha: 1};
store(changed, 'alpha', 2);
Object.defineProperty(changed, 'alpha', {set(value) { this.beta = value; }});
store(changed, 'alpha', 12);
assert(changed.beta === 12);
delete changed.alpha;
store(changed, 'alpha', 13);
assert(changed.alpha === 13);

var protoObject = {};
var newParent = {inherited: 14};
store(protoObject, '__proto__', newParent);
assert(Object.getPrototypeOf(protoObject) === newParent);
var nullParent = Object.create(null);
store(nullParent, '__proto__', newParent);
assert(nullParent.__proto__ === newParent && Object.getPrototypeOf(nullParent) === null);

// Recursive setters revisit this store site with many keys, collect, and throw.
var recursiveKeys = [];
for (var i = 0; i < 80; ++i) recursiveKeys.push(Symbol('recursive'));
var recursive = {};
Object.defineProperty(recursive, 'alpha', {set(value) {
  // Symbols enter the key cache immediately; freshly concatenated strings may
  // still be uninterned and would only exercise the generic fallback here.
  for (var i = 0; i < recursiveKeys.length; ++i) store(value, recursiveKeys[i], i);
  gc();
  if (value.fail) throw 17;
}});
var nested = {};
store(recursive, 'alpha', nested);
assert(nested[recursiveKeys[0]] === 0 && nested[recursiveKeys[79]] === 79);
var caught = false;
try { store(recursive, 'alpha', {fail: true}); } catch (e) { caught = e === 17; }
assert(caught);
store(nested, 'alpha', 18);
assert(nested.alpha === 18);

var coercions = 0;
store(nested, {[Symbol.toPrimitive]() {
  ++coercions;
  gc();
  return 'beta';
}}, 19);
assert(coercions === 1 && nested.beta === 19);
throwsTypeError(() => store(null, {toString() { ++coercions; return 'x'; }}, 0));
throwsTypeError(() => loose(undefined, {toString() { ++coercions; return 'x'; }}, 0));
assert(coercions === 1);
var proxyCalls = 0;
var proxy = new Proxy({}, {set(target, key, value, receiver) {
  ++proxyCalls;
  assert(receiver === proxy && key === 'alpha' && value === 20);
  return true;
}});
store(proxy, 'alpha', 20);
assert(proxyCalls === 1);
var proxyParent = new Proxy({}, {set(target, key, value, receiver) {
  ++proxyCalls;
  receiver.seen = value;
  return true;
}});
// Avoid invoking the proxy setter recursively for its evidence field.
Object.defineProperty(proxyParent, 'seen', {value: 0, writable: true});
var proxyChild = {seen: 0};
Object.setPrototypeOf(proxyChild, proxyParent);
store(proxyChild, 'alpha', 21);
assert(proxyChild.seen === 21 && proxyCalls === 2);

var array = [];
store(array, '0', 22);
store(array, 1, 23);
assert(array.length === 2 && array[0] === 22 && array[1] === 23);
store(array, 'length', 0);
assert(array.length === 0 && array[0] === undefined);
var typed = new Uint8Array(2);
store(typed, '0', 255);
store(typed, 1, 11);
assert(typed[0] === 255 && typed[1] === 11);
var indexed = {};
for (var key of ['2', '1', '4294967295', '01']) store(indexed, key, key);
assert(Object.keys(indexed).join(',') === '1,2,4294967295,01');
throwsTypeError(() => store('abc', 'length', 0));
loose('abc', 'length', 0);

// Exercise dictionary shapes at a site with a populated key cache.
for (var round = 0; round < 4; ++round) {
  var many = {};
  for (var i = 0; i < 200; ++i) store(many, 'field' + i, {value: i});
  delete many.field80;
  store(many, 'field80', {value: 800});
  assert(many.field80.value === 800 && many.field199.value === 199);
  gc();
}

// Give temporary keys their own site so permanent keys from earlier cases do
// not fill the cache and keep this test on the uncached fallback throughout.
function weakStore(object, key, value) {
  "noinline";
  "use strict";
  object[key] = value;
}
for (var round = 0; round < 4; ++round) {
  for (var i = 0; i < 40; ++i) {
    var temporary = {};
    var temporaryKey = Symbol('temporary');
    weakStore(temporary, temporaryKey, {value: i});
    assert(temporary[temporaryKey].value === i);
    weakStore(temporary, temporaryKey, i + 1);
    assert(temporary[temporaryKey] === i + 1);
  }
  gc();
}
print('computed stores passed');
// CHECK: computed stores passed
// C: _sh_ljs_put_by_val_cached_strict_rjs
// C: _sh_ljs_put_by_val_cached_loose_rjs
