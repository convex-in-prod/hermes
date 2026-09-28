/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -Xnative-promise -Xhermes-internal-test-methods %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O -Xnative-promise -Xhermes-internal-test-methods %s | %FileCheck --match-full-lines %s

function check(value, message) {
  if (!value) throw new Error(message);
}
function drain() {
  if (typeof gc === 'function') gc();
  HermesInternal.drainJobs();
}
function outcome(promise) {
  var result = {state: 'pending'};
  promise.then(function (value) {
    result.state = 'fulfilled';
    result.value = value;
  }, function (error) {
    result.state = 'rejected';
    result.value = error;
  });
  return result;
}
function fulfilled(result, expected) {
  drain();
  check(result.state === 'fulfilled', 'expected fulfillment');
  check(JSON.stringify(result.value) === JSON.stringify(expected), 'wrong values');
}

fulfilled(outcome(Promise.all([])), []);
fulfilled(outcome(Promise.all([Promise.resolve(1), Promise.resolve(2)])), [1, 2]);
fulfilled(outcome(Promise.all([Promise.resolve(3), 4])), [3, 4]);
fulfilled(outcome(Promise.all([, Promise.resolve(5)])), [undefined, 5]);
var primitiveValues = [undefined, null, true, false, 0, -0, NaN,
  Infinity, -Infinity, '', 'text', Symbol('marker'), 123n];
var primitiveResult = outcome(Promise.all(primitiveValues));
drain();
check(primitiveResult.state === 'fulfilled', 'primitive aggregation failed');
for (var i = 0; i < primitiveValues.length; ++i) {
  check(Object.is(primitiveResult.value[i], i === 5 ? 0 : primitiveValues[i]),
        'primitive resolution changed');
}
var negativeZero = new Promise(function (resolve) { resolve(-0); });
var zeroResult = outcome(Promise.all([negativeZero, -0]));
drain();
check(Object.is(zeroResult.value[0], -0) && Object.is(zeroResult.value[1], 0),
      'Promise and primitive zero signs were conflated');
fulfilled(outcome(Promise.all([{then: function (resolve) { resolve(22); }}])), [22]);

var resolveFirst, resolveSecond, rejectThird;
var first = new Promise(function (r) { resolveFirst = r; });
var second = new Promise(function (r) { resolveSecond = r; });
var third = new Promise(function (r, j) { rejectThird = j; });
var mixed = outcome(Promise.all([first, Promise.resolve(6), second, first]));
resolveSecond(7);
drain();
check(mixed.state === 'pending', 'completed before all inputs');
resolveFirst(8);
fulfilled(mixed, [8, 6, 7, 8]);
var rejected = outcome(Promise.all([Promise.resolve(9), third]));
rejectThird('reason');
drain();
check(rejected.state === 'rejected' && rejected.value === 'reason', 'lost rejection');

var resolveSource, resolveAdopter;
var source = new Promise(function (r) { resolveSource = r; });
var adopter = new Promise(function (r) { resolveAdopter = r; });
resolveAdopter(source);
var adopted = outcome(Promise.all([adopter]));
resolveSource(10);
fulfilled(adopted, [10]);

// Already-fulfilled inputs retain the existing synchronous shortcut. Adopted
// promises still schedule a job, even when their target is already fulfilled.
var syncOrder = [];
Promise.all([Promise.resolve(11)]).then(function () { syncOrder.push('all'); });
Promise.resolve().then(function () { syncOrder.push('other'); });
drain();
check(syncOrder.join(',') === 'all,other', 'fulfilled timing changed');

var originalResolve = Promise.resolve;
var resolveReads = 0;
Object.defineProperty(Promise, 'resolve', {configurable: true, get: function () {
  ++resolveReads;
  return originalResolve;
}});
var getterResult = outcome(Promise.all([first, second]));
Object.defineProperty(Promise, 'resolve', {value: originalResolve, writable: true, configurable: true});
fulfilled(getterResult, [8, 7]);
check(resolveReads === 1, 'resolve getter was duplicated');

var callCount = 0;
originalResolve.call = function (receiver, value) {
  ++callCount;
  return Reflect.apply(originalResolve, receiver, [value]);
};
var customCall = outcome(Promise.all([first, second]));
delete originalResolve.call;
fulfilled(customCall, [8, 7]);
check(callCount === 2, 'resolve.call override skipped');

var constructorReads = 0;
var constructorGetter = Promise.resolve(12);
Object.defineProperty(constructorGetter, 'constructor', {get: function () {
  ++constructorReads;
  return Promise;
}});
fulfilled(outcome(Promise.all([constructorGetter])), [12]);
check(constructorReads === 1, 'constructor getter was duplicated');

var prototypeInputs = [Promise.resolve(23), Promise.resolve(24)];
var prototypeConstructor = Object.getOwnPropertyDescriptor(Promise.prototype, 'constructor');
var prototypeReads = 0;
Object.defineProperty(Promise.prototype, 'constructor', {configurable: true, get: function () {
  ++prototypeReads;
  return Promise;
}});
var prototypeResult = Promise.all(prototypeInputs);
Object.defineProperty(Promise.prototype, 'constructor', prototypeConstructor);
fulfilled(outcome(prototypeResult), [23, 24]);
check(prototypeReads === 2, 'prototype constructor getter was skipped or duplicated');

var resolvePrototypeA, resolvePrototypeB;
var prototypeA = new Promise(function (r) { resolvePrototypeA = r; });
var prototypeB = new Promise(function (r) { resolvePrototypeB = r; });
var prototypeThen = Object.getOwnPropertyDescriptor(Promise.prototype, 'then');
var thenReads = 0;
Object.defineProperty(Promise.prototype, 'then', {configurable: true, get: function () {
  ++thenReads;
  return prototypeThen.value;
}});
var prototypePending = Promise.all([prototypeA, prototypeB]);
Object.defineProperty(Promise.prototype, 'then', prototypeThen);
var prototypePendingResult = outcome(prototypePending);
resolvePrototypeA(25);
resolvePrototypeB(26);
fulfilled(prototypePendingResult, [25, 26]);
check(thenReads === 2, 'prototype then getter was skipped or duplicated');

var resolveThenInput;
var thenInput = new Promise(function (r) { resolveThenInput = r; });
var thenCalls = 0;
thenInput.then = function (resolve, reject) {
  ++thenCalls;
  resolve(13);
  resolve(99);
};
fulfilled(outcome(Promise.all([thenInput])), [13]);
check(thenCalls === 1, 'custom then was skipped');

var iteratorCalls = 0;
var iterable = [first, second];
iterable[Symbol.iterator] = function* () {
  ++iteratorCalls;
  yield second;
  yield first;
};
fulfilled(outcome(Promise.all(iterable)), [7, 8]);
check(iteratorCalls === 1, 'custom iterator was duplicated');

var accessors = [first];
var elementReads = 0;
Object.defineProperty(accessors, 0, {get: function () {
  ++elementReads;
  return second;
}});
fulfilled(outcome(Promise.all(accessors)), [7]);
check(elementReads === 1, 'element getter was duplicated');

class OtherPromise extends Promise {}
var subclass = OtherPromise.all([first]);
check(subclass instanceof OtherPromise, 'subclass result lost');
fulfilled(outcome(subclass), [8]);

var originalDefine = Object.defineProperty;
var resolveLate;
var late = new Promise(function (r) { resolveLate = r; });
var lateResult = outcome(Promise.all([late]));
var definitions = 0;
Object.defineProperty = function (object, key, descriptor) {
  ++definitions;
  check(descriptor.value === 14 && descriptor.writable &&
        descriptor.enumerable && descriptor.configurable, 'descriptor changed');
  return originalDefine(object, key, descriptor);
};
resolveLate(14);
drain();
Object.defineProperty = originalDefine;
fulfilled(lateResult, [14]);
check(definitions === 1, 'late defineProperty override skipped');

var resolveMiddle;
var middle = new Promise(function (r) { resolveMiddle = r; });
var partial = outcome(Promise.all([Promise.resolve(27), middle, Promise.resolve(29)]));
var partialObserved = false;
Object.defineProperty = function (object, key, descriptor) {
  check(object.length === 3 && Object.keys(object).join(',') === '0,2',
        'private result length or holes changed');
  var firstDescriptor = Object.getOwnPropertyDescriptor(object, '0');
  check(firstDescriptor.value === 27 && firstDescriptor.writable &&
        firstDescriptor.enumerable && firstDescriptor.configurable,
        'private result descriptor changed');
  partialObserved = true;
  return originalDefine(object, key, descriptor);
};
resolveMiddle(28);
drain();
Object.defineProperty = originalDefine;
fulfilled(partial, [27, 28, 29]);
check(partialObserved, 'partial result was not exposed');

// Rejection does not suppress later resolve-element callbacks.
var rejectEarly, resolveAfter;
var early = new Promise(function (r, j) { rejectEarly = j; });
var after = new Promise(function (r) { resolveAfter = r; });
var earlyResult = outcome(Promise.all([early, after]));
rejectEarly('early');
drain();
definitions = 0;
Object.defineProperty = function (object, key, descriptor) {
  ++definitions;
  return originalDefine(object, key, descriptor);
};
resolveAfter(15);
drain();
Object.defineProperty = originalDefine;
check(earlyResult.state === 'rejected' && earlyResult.value === 'early', 'rejection changed');
check(definitions === 1, 'write after rejection was skipped');

// A late descriptor-prototype getter must run. Its exception rejects the
// discarded .then result, leaving the aggregate pending.
var resolveDescriptor;
var descriptorInput = new Promise(function (r) { resolveDescriptor = r; });
var descriptorResult = outcome(Promise.all([descriptorInput]));
var inheritedReads = 0;
originalDefine(Object.prototype, 'get', {configurable: true, get: function () {
  ++inheritedReads;
  throw new Error('descriptor getter');
}});
resolveDescriptor(16);
drain();
delete Object.prototype.get;
check(inheritedReads === 1 && descriptorResult.state === 'pending', 'descriptor prototype bypassed');

// Exposing and freezing the result during one callback must affect the next.
var resolveA, resolveB;
var a = new Promise(function (r) { resolveA = r; });
var b = new Promise(function (r) { resolveB = r; });
var frozenResult = outcome(Promise.all([a, b]));
Object.defineProperty = function (object, key, descriptor) {
  originalDefine(object, key, descriptor);
  Object.freeze(object);
};
resolveA(17);
drain();
Object.defineProperty = originalDefine;
resolveB(18);
drain();
check(frozenResult.state === 'pending', 'frozen result array was overwritten');

// Recursive job draining must retain aggregate state and each active value.
var resolveOuter, resolveInner;
var outer = new Promise(function (r) { resolveOuter = r; });
var inner = new Promise(function (r) { resolveInner = r; });
var recursive = outcome(Promise.all([outer, inner]));
Object.defineProperty = function (object, key, descriptor) {
  if (key === 0) {
    resolveInner(20);
    drain();
  }
  return originalDefine(object, key, descriptor);
};
resolveOuter(19);
drain();
Object.defineProperty = originalDefine;
fulfilled(recursive, [19, 20]);

// A tracker installed after registration observes the otherwise discarded
// reaction result when the resolve-element callback throws.
var resolveTracked;
var trackedInput = new Promise(function (r) { resolveTracked = r; });
var trackedPromise = Promise.all([trackedInput]);
var trackedResult = outcome(trackedPromise);
var timers = [];
globalThis.setTimeout = function (callback) {
  timers.push(callback);
  return timers.length;
};
globalThis.clearTimeout = function (id) { timers[id - 1] = null; };
var unhandled = [];
HermesInternal.enablePromiseRejectionTracker({
  allRejections: true,
  onUnhandled: function (id, error, promise) {
    unhandled.push({error: error, promise: promise});
  },
});
var trackedError = new Error('late definition');
Object.defineProperty = function () { throw trackedError; };
resolveTracked(21);
drain();
Object.defineProperty = originalDefine;
for (var timer of timers) if (timer !== null) timer();
check(trackedResult.state === 'pending', 'callback error rejected aggregate');
check(unhandled.length === 1 && unhandled[0].error === trackedError &&
      unhandled[0].promise !== trackedPromise, 'discarded rejection not tracked');
print('native Promise.all passed');
// CHECK: native Promise.all passed
