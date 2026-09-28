/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -fasync-frame-arguments -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O0 -fasync-frame-arguments -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fasync-frame-arguments -Xsmall-c -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fasync-frame-arguments -exec -Wx,-Xes6-proxy=false %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fasync-frame-arguments -emit-c -o %t.c %s
// RUN: %FileCheck --check-prefix=LOWER %s < %t.c
// LOWER: _sh_ljs_spawn_async_from_frame(

var asyncFrameResult = false;
var synchronous = [];
var completions = [];
function check(condition) {
  if (!condition) throw new Error('async frame contract failed');
}

async function positional(left, right) {
  'use strict';
  synchronous.push('positional');
  var oldRight = right;
  await 0;
  right = 20;
  return [this.base, left.value, oldRight, right, arguments.length, arguments[1]];
}

async function defaults({value}, amount = 4, ...rest) {
  synchronous.push('defaults');
  await Promise.resolve(0);
  return [value, amount, rest.join(':')];
}

async function nested(value) {
  if (value === 0) return 1;
  return (await nested(value - 1)) + 1;
}

async function throwsDefault(value = (() => { throw new Error('parameter'); })()) {
  throw new Error('unreachable body');
}

async function catches() {
  try {
    await Promise.reject('rejection');
  } catch (error) {
    return error;
  } finally {
    synchronous.push('finally');
  }
}

function makeArrow(value) {
  return async (increment) => {
    await 0;
    return this.base + value + arguments[0] + increment;
  };
}

async function capturedArguments(first) {
  // The outer capture intentionally retains its arguments object. Eliminating
  // the startup use must not replace this closure's distinct user-visible one.
  var read = () => arguments[0];
  await 0;
  return [first, read()];
}

class Parent {
  value() { return this.base; }
}
class Child extends Parent {
  async value(extra) {
    await 0;
    return super.value() + extra;
  }
}

var receiver = {base: 10, positional: positional, make: makeArrow};
var record = {value: 3};
completions.push(receiver.positional(record, 7));
record.value = 5;
completions.push(defaults({value: 2}, undefined, 6, 8));
completions.push(nested(4));
var rejection = throwsDefault();
check(rejection instanceof Promise);
completions.push(rejection.then(
  function () { throw new Error('unexpected fulfillment'); },
  function (error) { return error.message; }
));
completions.push(catches());
completions.push(receiver.make(3)(4));
completions.push(capturedArguments(9));
var child = new Child();
child.base = 12;
completions.push(child.value(5));
completions.push((async function () { 'use strict'; return this; })());
check(synchronous.join(',') === 'positional,defaults');
print('synchronous startup passed');
// CHECK: synchronous startup passed

Promise.all(completions).then(function (values) {
  check(JSON.stringify(values) ===
    '[[10,5,7,20,2,7],[2,4,"6:8"],5,"parameter","rejection",20,[9,9],17,null]');
  check(synchronous.join(',') === 'positional,defaults,finally');
  asyncFrameResult = true;
  print('async frame semantics passed');
});
// CHECK-NEXT: async frame semantics passed
