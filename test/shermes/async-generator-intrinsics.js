/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -fasync-generator-results -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fasync-generator-results -Xsmall-c -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fasync-generator-results -exec -Wx,-Xes6-proxy=false %s | %FileCheck --match-full-lines %s

var asyncResultPassed = false;
var intercepted = 0;
var prototype = Object.getPrototypeOf(function* () {}.prototype);
var saved = ['next', 'throw', 'return'].map(function (key) {
  return Object.getOwnPropertyDescriptor(prototype, key);
});
var entered = 0;
function unexpected() {
  ++intercepted;
  throw new Error('private generator escaped');
}
async function run(value) {
  ++entered;
  await 0;
  try {
    await Promise.reject(value);
  } catch (error) {
    return {value: error};
  } finally {
    await 1;
  }
}

var first = run(11);
// Mutate after suspension as well as before starting the second invocation.
['next', 'throw', 'return'].forEach(function (key) {
  Object.defineProperty(prototype, key, {get: unexpected, configurable: true});
});
var second = run(22);
if (entered !== 2 || intercepted !== 0)
  throw new Error('private async startup consulted generator prototypes');
print('private async startup passed');
// CHECK: private async startup passed

Promise.all([first, second]).then(function (values) {
  ['next', 'throw', 'return'].forEach(function (key, index) {
    Object.defineProperty(prototype, key, saved[index]);
  });
  if (values[0].value !== 11 || values[1].value !== 22 || intercepted !== 0)
    throw new Error('private async resume consulted generator prototypes');
  asyncResultPassed = true;
  print('private async intrinsic semantics passed');
});
// CHECK-NEXT: private async intrinsic semantics passed
