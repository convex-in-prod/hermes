/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -fasync-generator-results -exec -Wx,-Xnative-promise,-Xhermes-internal-test-methods %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fasync-generator-results -Xsmall-c -exec -Wx,-Xnative-promise,-Xhermes-internal-test-methods %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fasync-generator-results -exec -Wx,-Xhermes-internal-test-methods %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fasync-generator-results -emit-c -o %t.c %s
// RUN: %FileCheck --check-prefix=LOWER %s < %t.c
// LOWER: _sh_ljs_spawn_async_direct_from_frame(

function check(value, message) {
  if (!value) throw new Error(message);
}
function collect() {
  if (typeof gc === 'function') gc();
}

var getterCalls = 0;
var bad = Promise.resolve(0);
var reason = {kind: 'constructor'};
Object.defineProperty(bad, 'constructor', {get: function () {
  ++getterCalls;
  collect();
  throw reason;
}});
var recovered = 0;
async function failures() {
  for (var i = 0; i < 1000; ++i) {
    try {
      await bad;
      throw new Error('normalization unexpectedly succeeded');
    } catch (error) {
      check(error === reason, 'normalization error changed');
      ++recovered;
    }
  }
  return await 23;
}
var a = failures();
check(getterCalls === 1000 && recovered === 1000,
      'normalization failures did not resume synchronously');

async function primitives() {
  var values = [undefined, null, true, false, 0, -0, '', 17, 'text', NaN];
  for (var i = 0; i < values.length; ++i) {
    var value = await values[i];
    collect();
    check(Object.is(value, values[i]), 'await changed a primitive');
  }
  return 29;
}

var constructorReads = 0;
var exact = Promise.resolve({value: 31});
Object.defineProperty(exact, 'constructor', {get: function () {
  ++constructorReads;
  HermesInternal.drainJobs();
  collect();
  return Promise;
}});
async function nested() {
  var value = await exact;
  HermesInternal.drainJobs();
  collect();
  return value.value;
}

var finallyCalls = 0;
async function cleanup() {
  try {
    await Promise.reject(reason);
  } catch (error) {
    check(error === reason, 'await rejection changed');
    return 37;
  } finally {
    ++finallyCalls;
    await undefined;
    ++finallyCalls;
  }
}
var parameterError = {kind: 'parameter'};
async function parameters({value}) {
  return value;
}
var input = {};
Object.defineProperty(input, 'value', {get: function () { throw parameterError; }});
var b = parameters(input).then(function () {
  throw new Error('parameter error was lost');
}, function (error) {
  check(error === parameterError, 'parameter error escaped or changed');
  return 41;
});

var finished = false;
Promise.all([a, primitives(), nested(), cleanup(), b]).then(function (values) {
  check(values.join(',') === '23,29,31,37,41', 'async completion changed');
  check(constructorReads === 1, 'await repeated the constructor getter');
  check(finallyCalls === 2, 'finally did not finish');
  finished = true;
});
HermesInternal.drainJobs();
check(finished, 'async work did not settle');
print('native async driver semantics passed');
// CHECK: native async driver semantics passed
