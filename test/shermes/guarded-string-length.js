/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -Xsmall-c -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O0 -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -dump-lir -Xdump-functions=scan %s | %FileCheck --check-prefix=LIR %s

function assert(value) {
  if (!value) throw new Error('guarded string length semantics');
}
function scan(source, limit, effect) {
  'noinline';
  var count = 0;
  for (var i = 0; i < limit; ++i) {
    effect(i);
    if (i >= source.length) break;
    if (source[i] === 'x') ++count;
  }
  return count;
}
function collect() { gc(); }

assert(scan('x\ud800x\udc00', 10, collect) === 2);
assert(scan('', 10, collect) === 0);
assert(scan(new String('xxx'), 10, collect) === 3);
assert(scan(null, 0, collect) === 0);
assert(scan(undefined, 0, collect) === 0);

var trace = '';
var reads = 0;
var dynamic = {
  get length() { trace += 'l'; gc(); return ++reads === 1 ? 2 : 1; },
  get 0() { trace += 'c'; gc(); return 'x'; },
};
assert(scan(dynamic, 5, function () { trace += 'e'; }) === 1);
assert(reads === 2 && trace === 'elcel');

var values = ['x', 'x', 'x'];
assert(scan(values, 5, function (i) {
  if (i === 1) values.length = 1;
  gc();
}) === 1);
values = ['x'];
assert(scan(values, 3, function () { values.push('x'); gc(); }) === 3);

var accesses = [];
var proxy = new Proxy({0: 'x', 1: 'y', length: 2}, {
  get(target, key) { accesses.push(key); gc(); return target[key]; },
});
assert(scan(proxy, 5, collect) === 1);
assert(accesses.join(',') === 'length,0,length,1,length');
var revoked = Proxy.revocable({}, {});
revoked.revoke();
assert(scan(revoked.proxy, 0, collect) === 0);

var sentinel = {};
var observed;
trace = '';
try {
  scan({get length() { trace += 'l'; throw sentinel; }}, 1,
       function () { trace += 'e'; });
} catch (error) { observed = error; }
assert(observed === sentinel && trace === 'el');
observed = undefined;
try {
  scan(null, 1, function () { throw sentinel; });
} catch (error) { observed = error; }
assert(observed === sentinel);

function reassigned(source) {
  'noinline';
  var change = function (value) { source = value; };
  var count = 0;
  for (var i = 0; i < 3; ++i) {
    if (i === 1) change({length: 1});
    if (i >= source.length) break;
    if (source[i] === 'x') ++count;
  }
  return count;
}
assert(reassigned('xxx') === 1);

function nested(source, repeats) {
  'noinline';
  var count = 0;
  for (var round = 0; round < repeats; ++round)
    for (var i = 0; i < source.length; ++i) {
      if (source[i] === 'x') ++count;
      gc();
    }
  return count;
}
assert(nested('x\ud800x', 3) === 6);
assert(nested(null, 0) === 0);

function changingInnerReceiver(sources) {
  'noinline';
  var count = 0;
  for (var round = 0; round < sources.length; ++round) {
    var source = sources[round];
    for (var i = 0; i < source.length; ++i) {
      if (source[i] === 'x') ++count;
      gc();
    }
  }
  return count;
}
assert(changingInnerReceiver(['xx', 'x', '', new String('xx'), ['x', 'y']]) === 6);
print('guarded string length passed');
// CHECK: guarded string length passed

// The guard and cached length precede the callback. Its generic fallback still
// reads at the original position, as the changing-getter case above requires.
// LIR-LABEL: function scan(
// LIR: %[[SOURCE:[0-9]+]] = LoadParamInst (:any) %source: any
// LIR: %[[STRING:[0-9]+]] = TypeOfIsInst (:boolean) %[[SOURCE]]: any, typeOfIs(String)
// LIR: CondBranchInst %[[STRING]]: boolean, %[[FAST:BB[0-9]+]], %{{BB[0-9]+}}
// LIR: CallInst
// LIR-NEXT: CondBranchInst %[[STRING]]: boolean,
// LIR: %[[FAST]]:
// LIR-NEXT: %{{[0-9]+}} = LoadPropertyInst (:number) %[[SOURCE]]: any, "length": string
// LIR: function_end
