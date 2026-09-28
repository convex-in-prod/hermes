/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -fno-inline -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fno-inline -Xsmall-c -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O0 -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fno-inline -emit-c -o %t.c %s
// RUN: %FileCheck --check-prefix=C --input-file=%t.c %s

(function () {
  'use strict';
  function make(value) {
    function read(add) { gc(); return value + add; }
    return function invokeKnown(add) { return read(add); };
  }
  var first = make(11), second = make(23);
  print(first(2), second(3), first(4));

  function choose(flag) {
    var reader = flag ? function () { return 17; } : undefined;
    return reader();
  }
  print(choose(true));
  try { choose(false); } catch (e) { print(e.name); }

  function Value(value) { this.value = value; }
  function construct(value) { return new Value(value); }
  print(construct(29).value);
  var arrow = () => 31;
  try { new arrow(); } catch (e) { print(e.name); }
  class Item { constructor(value) { this.value = value; } }
  print(new Item(37).value);
  try { Item(41); } catch (e) { print(e.name); }

  function nestedThrow() { gc(); throw 'marker'; }
  function catches() { try { return nestedThrow(); } catch (e) { return e; } }
  print(catches());
  function recursive(n) { return n === 0 ? 1 : n * recursive(n - 1); }
  print(recursive(6));
  function* sequence(value) { yield value; yield value + 1; }
  print(Array.from(sequence(43)).join(' '));
})();

// CHECK: 13 26 15
// CHECK-NEXT: 17
// CHECK-NEXT: TypeError
// CHECK-NEXT: 29
// CHECK-NEXT: TypeError
// CHECK-NEXT: 37
// CHECK-NEXT: TypeError
// CHECK-NEXT: marker
// CHECK-NEXT: 720
// CHECK-NEXT: 43 44
// C-LABEL: static SHLegacyValue _{{[0-9]+}}_invokeKnown(SHRuntime *shr) {
// C-NOT: _sh_ljs_call(
// C: = _{{[0-9]+}}_read(shr);
// C-NOT: _sh_ljs_call(
// C: _sh_leave(shr, &locals.head, frame);
