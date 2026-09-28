/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -farray-callbacks -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -farray-callbacks -Xsmall-c -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O0 -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -farray-callbacks -dump-ir -Xdump-functions=contains %s | %FileCheck --check-prefix=IR %s

'use strict';
function assert(value) {
  if (!value) throw new Error('array callback semantics');
}
var intrinsicSome = Array.prototype.some;
function contains(values, wanted) {
  'noinline';
  return values.some(value => value === wanted);
}
function scan(values, effect, context) {
  'noinline';
  return values.some(function (value, index, array) {
    assert(arguments.length === 3);
    assert(arguments[0] === value && arguments[1] === index);
    assert(arguments[2] === array);
    return effect(this, value, index, array);
  }, context);
}
assert(!contains([], undefined));
assert(!contains(new Array(3), undefined));
assert(contains([undefined], undefined));
assert(!contains([NaN], NaN));
assert(contains([-0], 0));
var identity = {};
assert(contains([identity], identity));
assert(!contains([{}], {}));

var trace = [];
var values = [1, 2, 3];
assert(!scan(values, function (context, value, index, array) {
  assert(context === identity && array === values);
  trace.push(index + ':' + value);
  if (index === 0) {
    array[1] = 20;
    delete array[2];
    array.push(4);
  }
  gc();
  return false;
}, identity));
assert(trace.join(',') === '0:1,1:20');
trace = [];
values = [1, 2, 3];
assert(!scan(values, function (context, value, index, array) {
  assert(context === 7);
  trace.push(value);
  array.length = 0;
  gc();
  return false;
}, 7));
assert(trace.join(',') === '1');

var inherited = Object.create(Array.prototype);
Object.defineProperty(inherited, '1', {
  get: function () { trace.push('inherited'); gc(); return 9; },
});
values = [1, , 3];
Object.setPrototypeOf(values, inherited);
trace = [];
assert(contains(values, 9));
assert(trace.join(',') === 'inherited');

values = [1, 2, 3];
Object.defineProperty(values, '0', {
  get: function () { values[1] = 8; gc(); return 1; },
});
assert(contains(values, 8));

values = [1, 2];
Object.defineProperty(values, '1', {
  get: function () { throw new Error('read after early exit'); },
});
assert(contains(values, 1));

trace = [];
values = [,];
Object.setPrototypeOf(values, new Proxy(Array.prototype, {
  has: function (target, key) { trace.push('has:' + key); return key === '0'; },
  get: function (target, key, receiver) {
    if (key === '0') { trace.push('get:0'); return 4; }
    return Reflect.get(target, key, receiver);
  },
}));
assert(contains(values, 4));
assert(trace.join(',') === 'has:0,get:0');

trace = [];
var arrayLike = {
  0: 'a', 1: 'b', some: intrinsicSome,
  get length() {
    trace.push('length');
    return { valueOf: function () { trace.push('coerce'); return 1.9; } };
  },
};
assert(!contains(arrayLike, 'b'));
assert(trace.join(',') === 'length,coerce');
trace = [];
var proxy = new Proxy([1, 2], {
  get: function (target, key) { trace.push('get:' + key); return target[key]; },
  has: function (target, key) { trace.push('has:' + key); return key in target; },
});
assert(contains(proxy, 2));
assert(trace.join(',') === 'get:some,get:length,has:0,get:0,has:1,get:1');

var sentinel = {};
var caught;
values = [1];
Object.defineProperty(values, '0', {get: function () { throw sentinel; }});
try { contains(values, 1); } catch (error) { caught = error; }
assert(caught === sentinel);
caught = undefined;
try {
  scan([1], function () { throw sentinel; }, undefined);
} catch (error) { caught = error; }
assert(caught === sentinel);
var revoked = Proxy.revocable([], {});
revoked.revoke();
caught = undefined;
try { contains(revoked.proxy, 0); } catch (error) { caught = error; }
assert(caught instanceof TypeError);

trace = [];
values = [1];
Object.defineProperty(values, 'some', {
  get: function () { trace.push('method'); return intrinsicSome; },
});
function argument() { trace.push('argument'); values.push(2); return 2; }
assert(contains(values, argument()));
// The caller's argument precedes contains' method lookup.
assert(trace.join(',') === 'argument,method');
function inlineOrder(values) {
  'noinline';
  return values.some(value => value === 2, argument());
}
trace = [];
assert(inlineOrder(values));
assert(trace.join(',') === 'method,argument');

values = [3];
values.some = function (callback) {
  assert(this === values);
  return callback(3, 0, this) ? 'overridden' : 'unexpected';
};
assert(contains(values, 3) === 'overridden');
Array.prototype.some = function (callback) { return 'replacement'; };
assert(contains([1], 1) === 'replacement');
Array.prototype.some = intrinsicSome;

function lexicalThis(values) {
  'noinline';
  return values.some(value => this === value);
}
assert(lexicalThis.call(identity, [identity]));
assert(scan([1], function (context) { return context === undefined; }, undefined));
assert(scan([1], function (context) { return context === null; }, null));

print('array callback lowering passed');
// CHECK: array callback lowering passed

// The body of this callback becomes an ordinary IR strict comparison.
// IR-LABEL: function contains(
// IR: BranchIfBuiltinInst [HermesBuiltin.arrayPrototypeSome]
// IR: CallBuiltinInst{{.*}}[HermesBuiltin.arrayIterationLength]
// IR: BinaryInInst
// IR: BinaryStrictlyEqualInst
// IR: function_end
