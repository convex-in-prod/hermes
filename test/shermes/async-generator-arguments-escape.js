/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -fasync-generator-results -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fasync-frame-arguments -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fasync-generator-results -Xsmall-c -exec %s | %FileCheck --match-full-lines %s

var asyncResultPassed = false;
async function exposesCallee() {
  return arguments.callee;
}
async function strictCallee() {
  'use strict';
  return arguments.callee;
}

Promise.all([
  exposesCallee().then(function (callee) {
    // The existing lowering may expose its generator factory here. Regardless
    // of that limitation, calling it must not expose a private state machine
    // that expects a compiler-owned completion record.
    if (typeof callee() !== 'object')
      throw new Error('private async state machine escaped through arguments');
  }),
  strictCallee().then(function () {
    throw new Error('strict arguments unexpectedly exposed a callee');
  }, function (error) {
    if (!(error instanceof TypeError)) throw error;
  })
]).then(function () {
  asyncResultPassed = true;
  print('async arguments escape controls passed');
});
// CHECK: async arguments escape controls passed
