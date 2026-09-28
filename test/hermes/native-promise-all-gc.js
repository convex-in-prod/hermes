/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O -Xnative-promise -Xhermes-internal-test-methods -gc-init-heap=4M -gc-max-heap=32M %s | %FileCheck --match-full-lines %s
// REQUIRES: gc_hades

function check(value, message) {
  if (!value) throw new Error(message);
}
function collect() {
  HermesInternal.drainJobs();
  gc();
}
function sample() {
  collect();
  var stats = HermesInternal.getInstrumentedStats();
  return {liveBytes: stats.js_allocatedBytes, heapBytes: stats.js_heapSize,
    totalAllocatedBytes: stats.js_totalAllocatedBytes, collections: stats.js_numGCs};
}
function aggregate(size, rejectFirst) {
  var inputs = [], resolvers = [], values = [];
  for (var i = 0; i < size; ++i) {
    var value = {index: i, payload: 'value-' + i};
    values.push(value);
    if (i % 2 === 0) inputs.push(Promise.resolve(value));
    else inputs.push(new Promise(function (resolve, reject) {
      resolvers.push({resolve: resolve, reject: reject, index: i});
    }));
  }
  var completed = false;
  var result = Promise.all(inputs);
  result.then(function (items) {
    check(!rejectFirst, 'unexpected fulfillment');
    check(items.length === size, 'wrong result length');
    for (var i = 0; i < size; ++i) check(items[i] === values[i], 'lost result identity');
    completed = true;
  }, function (error) {
    check(rejectFirst && error === 'expected', 'wrong rejection');
    completed = true;
  });
  inputs = null;
  result = null;
  gc();
  if (rejectFirst) {
    resolvers[0].reject('expected');
    collect();
    check(completed, 'aggregate did not reject');
  }
  for (var i = 0; i < resolvers.length; ++i) {
    if (!rejectFirst || i !== 0) resolvers[i].resolve(values[resolvers[i].index]);
    if (i % 128 === 0) gc();
  }
  collect();
  check(completed, 'aggregate did not complete');
}
// Exercise both sides of the bounded native path and collections while the
// aggregate is reachable only through pending reactions and the job queue.
for (var size of [0, 1, 4096, 4097]) aggregate(size, false);
aggregate(4096, true);
var measurements = [];
for (var epoch = 0; epoch < 6; ++epoch) {
  for (var iteration = 0; iteration < 16; ++iteration) {
    aggregate(128, iteration % 2 === 0);
  }
  measurements.push(sample());
}
var minimum = measurements[0].liveBytes;
var maximum = minimum;
for (var measurement of measurements) {
  minimum = Math.min(minimum, measurement.liveBytes);
  maximum = Math.max(maximum, measurement.liveBytes);
}
// Allow the small measurement records and runtime housekeeping, but reject
// retaining even one complete result per iteration after all reactions drain.
check(maximum - minimum < 65536, 'completed aggregates retain their values');
print('native Promise.all GC passed');
// CHECK: native Promise.all GC passed
