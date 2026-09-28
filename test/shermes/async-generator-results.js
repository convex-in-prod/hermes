/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -fasync-generator-results -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O0 -fasync-generator-results -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fasync-generator-results -Xsmall-c -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fasync-generator-results -emit-c -o %t.c %s
// RUN: %FileCheck --check-prefix=LOWER %s < %t.c
// LOWER: _sh_ljs_spawn_async_direct_from_frame(

var asyncResultPassed = false;
function check(condition) {
  if (!condition) throw new Error('async completion contract failed');
}

function* visibleGenerator() {
  yield {value: 1};
  yield {value: 2};
  return {value: 3};
}
var visible = visibleGenerator();
var first = visible.next();
var second = visible.next();
first.done = 'retained';
var third = visible.next();
check(first !== second && second !== third);
check(first.done === 'retained' && first.value.value === 1);
check(second.done === false && second.value.value === 2);
check(third.done === true && third.value.value === 3);

var entered = 0;
async function repeated(seed) {
  ++entered;
  var output = [];
  for (var i = 0; i < 12; ++i) {
    var value = await {value: seed + i};
    output.push(value);
    check(value.value === seed + i);
  }
  return output;
}

async function nested(depth) {
  if (depth === 0) return {value: 1};
  var result = await nested(depth - 1);
  await 0;
  return {value: result.value + 1};
}

async function recovers() {
  var p = Promise.resolve(0);
  Object.defineProperty(p, 'constructor', {
    get: function () { throw 'constructor failure'; }
  });
  try {
    await p;
    throw 'unreachable';
  } catch (error) {
    check(error === 'constructor failure');
    return await {value: 'recovered'};
  } finally {
    await 1;
  }
}

async function lexical(first, ...rest) {
  var read = () => [this.value, arguments.length, arguments[0], rest.length];
  await 0;
  return read();
}

var pending = [repeated(10), repeated(30), nested(5), recovers(),
  lexical.call({value: 7}, 4, 5, 6)];
check(entered === 2);
print('synchronous and public generator controls passed');
// CHECK: synchronous and public generator controls passed

Promise.all(pending).then(function (values) {
  for (var i = 0; i < 12; ++i) {
    check(values[0][i].value === 10 + i);
    check(values[1][i].value === 30 + i);
    if (i) check(values[0][i] !== values[0][i - 1]);
  }
  check(values[2].value === 6 && values[3].value === 'recovered');
  check(values[4].join(',') === '7,3,4,2');
  asyncResultPassed = true;
  print('private async completion semantics passed');
});
// CHECK-NEXT: private async completion semantics passed
