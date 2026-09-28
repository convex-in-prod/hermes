/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -Xnative-promise -Xhermes-internal-test-methods %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O -Xnative-promise -Xhermes-internal-test-methods %s | %FileCheck --match-full-lines %s

function check(condition, message) {
  if (!condition) throw new Error(message);
}
function collect() {
  if (typeof gc === 'function') gc();
}

var order = [];
var resolveFirst, resolveNested;
var first = new Promise(function (resolve) { resolveFirst = resolve; });
var nested = new Promise(function (resolve) { resolveNested = resolve; });
check(Object.keys(first).length === 0, 'native Promise core is not active');
var result = first.then(function (value) {
  check(value.marker === 17, 'queued input was not retained');
  order.push('first');
  resolveNested({marker: 23});
  collect();
  HermesInternal.drainJobs();
  check(value.marker === 17, 'active input was not retained');
  order.push('return');
  return {marker: 31};
});
first.then(function () { order.push('second'); });
nested.then(function (value) {
  collect();
  check(value.marker === 23, 'nested input was not retained');
  order.push('nested');
});
result.then(function (value) {
  check(value.marker === 31, 'reaction result changed');
  order.push('result');
});
resolveFirst({marker: 17});
collect();
HermesInternal.drainJobs();
check(order.join(',') === 'first,second,nested,return,result',
      'recursive drain changed FIFO');

var caught;
var continued = false;
HermesInternal.enqueueJob(function () { throw new Error('ordinary job'); });
Promise.resolve(41).then(function (value) {
  collect();
  check(value === 41, 'continuing reaction input changed');
  continued = true;
});
try { HermesInternal.drainJobs(); } catch (error) { caught = error; }
check(caught && caught.message === 'ordinary job', 'job exception did not escape');
check(!continued, 'drain did not stop at the job exception');
HermesInternal.drainJobs();
check(continued, 'remaining reaction was lost');

var settled = false;
async function nestedAwait() {
  for (var i = 0; i < 40; ++i) {
    var value = await Promise.resolve(i);
    HermesInternal.drainJobs();
    collect();
    check(value === i, 'reused async reaction lost its input');
  }
  return 42;
}
nestedAwait().then(function (value) {
  check(value === 42, 'reentrant async result changed');
  settled = true;
});
HermesInternal.drainJobs();
check(settled, 'reentrant async continuation did not settle');
print('native Promise reentrancy passed');
// CHECK: native Promise reentrancy passed
