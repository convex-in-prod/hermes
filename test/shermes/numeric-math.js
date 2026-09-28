/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -fstatic-math-builtins -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fstatic-math-builtins -Xsmall-c -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fstatic-math-builtins -emit-c -o %t.c %s
// RUN: %FileCheck --check-prefix=C --input-file=%t.c %s

function numeric(x) {
  "noinline";
  x = +x;
  return [Math.abs(x), Math.ceil(x), Math.floor(x), Math.trunc(x),
          Math.round(x), Math.sqrt(x)];
}
function bounds(x, y) {
  "noinline";
  return [Math.min(+x, +y), Math.max(+x, +y)];
}
function show(x) {
  if (x === 0 && 1 / x < 0) return '-0';
  return '' + x;
}
print(numeric(-0.5).map(show).join(' '));
// CHECK: 0.5 -0 -1 -0 -0 NaN
print(numeric(-0).map(show).join(' '));
// CHECK-NEXT: 0 -0 -0 -0 -0 -0
print(numeric(0.49999999999999994).map(show).join(' '));
// CHECK-NEXT: 0.49999999999999994 1 0 0 0 0.7071067811865475
print(numeric(4503599627370497)[4], numeric(-4503599627370497)[4]);
// CHECK-NEXT: 4503599627370497 -4503599627370497
print(bounds(0, -0).map(show).join(' '), bounds(-0, 0).map(show).join(' '));
// CHECK-NEXT: -0 0 -0 0
print(bounds(NaN, 3).map(show).join(' '), bounds(3, NaN).map(show).join(' '));
// CHECK-NEXT: NaN NaN NaN NaN

var methods = ['abs', 'ceil', 'floor', 'trunc', 'round', 'sqrt'];
var values = [-Infinity, -4503599627370497, -2.5, -1.5, -0.5000000000000001,
              -0.5, -0.49999999999999994, -Number.MIN_VALUE, -0, 0,
              Number.MIN_VALUE, 0.49999999999999994, 0.5, 1.5,
              4503599627370497, Infinity, NaN];
var comparisons = 0;
for (var x of values) {
  var actual = numeric(x);
  for (var i = 0; i < methods.length; ++i) {
    if (!Object.is(actual[i], Math[methods[i]](x))) throw new Error('unary');
    ++comparisons;
  }
  for (var y of values) {
    actual = bounds(x, y);
    for (var i = 0; i < 2; ++i) {
      if (!Object.is(actual[i], Math[i ? 'max' : 'min'](x, y)))
        throw new Error('bounds');
      ++comparisons;
    }
  }
}
print(comparisons);
// CHECK-NEXT: 680

var order = '';
function operand(label, value) {
  return {valueOf() { order += label; return value; }};
}
function generic(x, y) {
  "noinline";
  return Math.min(x, y);
}
print(generic(operand('a', NaN), operand('b', 3)), order);
// CHECK-NEXT: NaN ab
order = '';
try {
  generic(operand('a', NaN), {valueOf() { order += 'b'; throw 17; }});
} catch (e) {
  print(e, order);
}
// CHECK-NEXT: 17 ab
order = '';
print(bounds(operand('a', -2), operand('b', 3)).join(' '), order);
// CHECK-NEXT: -2 3 abab
print(Math.min(), Math.max(), Math.round(), Math.min(3), Math.max(3, 7, 5));
// CHECK-NEXT: Infinity -Infinity NaN 3 7
order = '';
print(Math.round(+operand('a', 2.5), +operand('b', 9)), order);
// CHECK-NEXT: 3 ab

// C-DAG: _sh_ljs_double(fabs(_sh_ljs_get_double(
// C-DAG: _sh_ljs_double(ceil(_sh_ljs_get_double(
// C-DAG: _sh_ljs_double(floor(_sh_ljs_get_double(
// C-DAG: _sh_ljs_double(trunc(_sh_ljs_get_double(
// C-DAG: _sh_ljs_double(hermesMathRound(_sh_ljs_get_double(
// C-DAG: _sh_ljs_double(sqrt(_sh_ljs_get_double(
// C-DAG: _sh_ljs_double(hermesMathMin(_sh_ljs_get_double(
// C-DAG: _sh_ljs_double(hermesMathMax(_sh_ljs_get_double(
