/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -Xnative-promise %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O -Xnative-promise %s | %FileCheck --match-full-lines %s
// RUN: %hermes -Xnative-promise -enable-hermes-internal=false %s | %FileCheck --match-full-lines %s

function check(condition, message) {
  if (!condition) throw new Error(message);
}
function collect() {
  if (typeof gc === 'function') gc();
}
var checks = [];
function expectValue(promise, value) {
  checks.push(promise.then(function (actual) {
    check(actual === value, 'unexpected fulfilled value');
  }));
}
function expectError(promise, message) {
  checks.push(promise.then(function () {
    throw new Error('unexpected fulfillment');
  }, function (error) {
    check(error.message === message, 'unexpected rejection');
  }));
}

var resolveFrozen;
var frozen = new Promise(function (resolve) { resolveFrozen = resolve; });
check(Object.keys(frozen).length === 0, 'state leaked into own properties');
expectValue(frozen, 42);
Object.freeze(frozen);
collect();
resolveFrozen(42);

var forgedRejected = false;
try {
  Promise.prototype.then.call(Object.create(Promise.prototype), function () {});
} catch (error) {
  forgedRejected = error instanceof TypeError;
}
check(forgedRejected, 'prototype-only receiver was accepted');

var resolveChanged;
var changed = new Promise(function (resolve) { resolveChanged = resolve; });
Object.setPrototypeOf(changed, null);
expectValue(Promise.prototype.then.call(changed, function (value) {
  collect();
  return value + 1;
}), 44);
resolveChanged(43);

var retainedResolve, retainedReject;
var retained = new Promise(function (resolve, reject) {
  retainedResolve = resolve;
  retainedReject = reject;
});
check(retainedResolve.length === 1 && retainedReject.length === 1,
      'resolver arity changed');
var resolverConstructed = false;
try { new retainedResolve(); } catch (error) {
  resolverConstructed = error instanceof TypeError;
}
check(resolverConstructed, 'resolver became constructable');
expectValue(retained, 'first');
collect();
retainedResolve('first');
retainedReject(new Error('second'));
retainedResolve('third');

expectValue(new Promise(function (resolve) {
  resolve('settled');
  throw new Error('ignored after settlement');
}), 'settled');
expectError(new Promise(function () { throw new Error('executor'); }), 'executor');

var payload = { marker: 17 };
var thenGets = 0;
var thenable = Object.defineProperty({}, 'then', { get: function () {
  thenGets++;
  collect();
  return function (resolve, reject) {
    check(this === thenable, 'thenable receiver changed');
    resolve(payload);
    reject(new Error('late rejection'));
    throw new Error('late throw');
  };
}});
expectValue(new Promise(function (resolve) { resolve(thenable); }), payload);
check(thenGets === 1, 'then getter repeated');
expectError(new Promise(function (resolve) {
  resolve(Object.defineProperty({}, 'then', { get: function () {
    collect();
    throw new Error('then getter');
  }}));
}), 'then getter');
expectError(Promise.resolve(1).then(function () {
  collect();
  throw new Error('reaction');
}), 'reaction');

var resolveSelf;
var self = new Promise(function (resolve) { resolveSelf = resolve; });
checks.push(self.then(function () { throw new Error('self resolution fulfilled'); },
  function (error) { check(error instanceof TypeError, 'self rejection type'); }));
resolveSelf(self);

var resolveA, resolveB;
var a = new Promise(function (resolve) { resolveA = resolve; });
var b = new Promise(function (resolve) { resolveB = resolve; });
var cycleSettled = false;
a.then(function () { cycleSettled = true; }, function () { cycleSettled = true; });
resolveA(b);
resolveB(a);

class SubPromise extends Promise {}
var sub = new SubPromise(function (resolve) { resolve(8); });
var subResult = sub.then(function (value) { return value + 1; });
check(subResult instanceof SubPromise, 'subclass result lost its prototype');
expectValue(subResult, 9);

var constructorReads = 0;
var exact = Promise.resolve(7);
Object.defineProperty(exact, 'constructor', { get: function () {
  constructorReads++;
  collect();
  return Promise;
}});
check(Promise.resolve(exact) === exact && constructorReads === 1,
      'exact Promise constructor check changed');

var order = [];
var resolveOrder;
var ordered = new Promise(function (resolve) { resolveOrder = resolve; });
checks.push(ordered.then(function () { order.push(1); collect(); }));
checks.push(ordered.then(function () { order.push(2); }));
checks.push(ordered.then(function () { order.push(3); }));
resolveOrder(undefined);

async function repeatedAwait() {
  var value = 0;
  for (var i = 0; i < 30; ++i) {
    value += await Promise.resolve(i);
    if (i % 5 === 0) collect();
  }
  try { await Promise.reject(new Error('await rejection')); }
  catch (error) { check(error.message === 'await rejection', 'await error changed'); }
  return value;
}
expectValue(repeatedAwait(), 435);

Promise.all(checks).then(function () {
  check(order.join(',') === '1,2,3', 'reaction FIFO changed');
  check(!cycleSettled, 'mutual adoption unexpectedly settled');
  print('native Promise core passed');
}, function (error) {
  print('native Promise core failed:', error.message);
});
// CHECK: native Promise core passed
