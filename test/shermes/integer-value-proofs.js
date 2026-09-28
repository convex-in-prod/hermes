/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -fstatic-math-builtins -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fstatic-math-builtins -Xsmall-c -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O0 -exec %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fstatic-math-builtins -emit-c -o %t.c %s
// RUN: %FileCheck --check-prefix=C --input-file=%t.c %s

function known(a, b) {
  "noinline";
  var x = Math.imul(+a, +b);
  return (((x << 5) | (x >>> 27)) ^ (x >> 7)) >>> 0;
}
// C-LABEL: static SHLegacyValue _{{[0-9]+}}_known(SHRuntime *shr) {
// C: _sh_ljs_imul_number(
// C-NOT: _sh_to_uint32_double
// C-NOT: _sh_to_int32_double
// C-LABEL: static SHLegacyValue _{{[0-9]+}}_bounded(SHRuntime *shr) {

function bounded(x, y, n) {
  "noinline";
  x = x | 0;
  y = y >>> 0;
  while (n-- > 0) {
    x = Math.imul(((x << 13) | (x >>> 19)) ^ y, 0x85ebca6b);
    y = (y + (x >>> 0)) >>> 0;
  }
  return [x, x >>> 0, y, y | 0];
}
// C: _sh_ljs_imul_uint32(
// C: (uint32_t)(int32_t)_sh_ljs_get_double(
// C-LABEL: static SHLegacyValue _{{[0-9]+}}_mixed(SHRuntime *shr) {

function mixed(a, b, flag) {
  "noinline";
  var x = flag ? a | 0 : b >>> 0;
  return Math.imul(x, 5) >>> 0;
}
// C: _sh_ljs_imul_uint32((uint32_t)(int64_t)_sh_ljs_get_double(

function remainder(x, y) {
  "noinline";
  var r = +x % +y;
  return [r, r | 0, r >>> 0, Math.imul(r, 5)];
}
function stringify(values) {
  return values.map(x => Object.is(x, -0) ? '-0' : String(x)).join(' ');
}
print(stringify(remainder(3.9, 2)), stringify(remainder(-4, 2)));
print(stringify(remainder(Infinity, 2)), stringify(remainder(4294967295, 1e20)));

var values = [-Infinity, -1e100, -4294967297, -2147483649, -2147483648,
              -3.9, -0, 0, 0.9, 1, 2147483647, 2147483648, 4294967295,
              4294967296, 1e100, Infinity, NaN];
var digest = 0, comparisons = 0;
for (var a of values) {
  for (var b of values) {
    var results = [known(a, b), ...bounded(a, b, 4),
                   mixed(a, b, true), mixed(a, b, false)];
    for (var result of results) {
      digest = (Math.imul(digest, 31) + result) | 0;
      ++comparisons;
    }
  }
}
print(comparisons, digest);

var order = '';
function operand(label, value) {
  return {valueOf() { order += label; return value; }};
}
print(known(operand('a', -3.9), operand('b', 2.8)), order);
print(mixed(-1, 0xffffffff, true), mixed(-1, 0xffffffff, false));
function bigObjects() {
  "noinline";
  var left = {valueOf() { return 6n; }};
  var right = {valueOf() { return 3n; }};
  return [(left & right) | 1n, (left ^ right) & 7n, (left | right) ^ 1n];
}
print(bigObjects().map(String).join(' '));
function mixedBigInt() {
  "noinline";
  var left = {valueOf() { return 6n; }};
  var right = {valueOf() { return 3n; }};
  return (left & right) | 0;
}
try { mixedBigInt(); } catch (e) { print(e.name); }

// CHECK: 1.9 1 1 5 -0 0 0 0
// CHECK-NEXT: NaN 0 0 0 4294967295 -1 4294967295 -5
// CHECK-NEXT: 2023 118213184
// CHECK-NEXT: 160 ab
// CHECK-NEXT: 4294967291 4294967291
// CHECK-NEXT: 3 5 6
// CHECK-NEXT: TypeError
