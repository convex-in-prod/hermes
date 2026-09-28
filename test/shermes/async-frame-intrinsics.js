/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -fasync-frame-arguments -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fasync-frame-arguments -Xsmall-c -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fasync-frame-arguments -exec -Wx,-Xes6-proxy=false %s | %FileCheck --match-full-lines %s

var asyncFrameResult = false;
var entered = 0;
var intercepted = 0;
var generatorPrototype = Object.getPrototypeOf(function* () {});
var functionApply = Object.getOwnPropertyDescriptor(Function.prototype, 'apply');

function unexpectedApply() {
  ++intercepted;
  throw new Error('unexpected async apply lookup');
}

async function positional(left, right) {
  ++entered;
  await 0;
  return this.base + left + right + arguments.length;
}

async function rejects(value = (() => { throw new Error('parameter'); })()) {
  throw new Error('unreachable body');
}

var receiver = {base: 10, invoke: positional};
var results = [];
// Compiler-created generator startup must not consult application properties.
// Reflect is deliberately unnecessary, including when ES6Proxy is disabled.
for (var mode = 0; mode < 2; ++mode) {
  var target = mode === 0 ? Function.prototype : generatorPrototype;
  Object.defineProperty(target, 'apply', {get: unexpectedApply, configurable: true});
  try {
    results.push(receiver.invoke(1, 2));
    results.push(rejects().then(
      function () { throw new Error('unexpected fulfillment'); },
      function (error) { return error.message; }
    ));
  } finally {
    if (mode === 0) Object.defineProperty(target, 'apply', functionApply);
    else delete target.apply;
  }
}
if (entered !== 2 || intercepted !== 0)
  throw new Error('async startup consulted application properties');
print('synchronous startup passed');
// CHECK: synchronous startup passed

if (typeof gc === 'function') gc();
Promise.all(results).then(function (values) {
  if (values.join(',') !== '15,parameter,15,parameter')
    throw new Error('async argument or rejection changed');
  asyncFrameResult = true;
  print('async intrinsic semantics passed');
});
// CHECK-NEXT: async intrinsic semantics passed
