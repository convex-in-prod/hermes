/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -fstatic-math-builtins -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fstatic-math-builtins -emit-c -o %t.c %s
// RUN: %FileCheck --check-prefix=C --input-file=%t.c %s

function literal() {
  "noinline";
  return /(?<word>a+)\k<word>/gy;
}
var first = literal(), second = literal();
print(first !== second, first.exec('aaaa').groups.word, first.lastIndex, second.lastIndex);
// CHECK: true aa 4 0
print(second.test('baaaa'), second.lastIndex);
// CHECK-NEXT: false 0
print(/\u{1F600}/u.test('\uD83D\uDE00'), /a/gi.flags);
// CHECK-NEXT: true gi
function numeric(a, b) {
  "noinline";
  return Math.imul(+a, +b);
}
print(numeric(-1, 5), numeric(0x80000000, -1), numeric(0xffffffff, 0xffffffff));
// CHECK-NEXT: -5 -2147483648 1
print(numeric(Infinity, 3), numeric(NaN, 4), numeric(-3.9, 2.8), numeric(4294967297, 3));
// CHECK-NEXT: 0 0 -6 3
var order = '';
function generic(a, b) {
  "noinline";
  return Math.imul(a, b);
}
print(generic({valueOf() { order += 'a'; return 3; }}, {valueOf() {order += 'b'; return 4;}}), order);
// CHECK-NEXT: 12 ab
Math.random = () => 0.25;
Date.now = () => 123;
print(Math.random(), Date.now());
// CHECK-NEXT: 0.25 123
// C: _sh_ljs_create_regexp_precompiled
// C: _sh_ljs_imul_number
