/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O -Xhermes-internal-test-methods -gc-sanitize-handles=1 %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O0 -Xhermes-internal-test-methods -gc-sanitize-handles=1 %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -exec %s -Wx,-Xhermes-internal-test-methods,-gc-sanitize-handles=1 | %FileCheck --match-full-lines %s
// RUN: %shermes -O0 -exec %s -Wx,-Xhermes-internal-test-methods,-gc-sanitize-handles=1 | %FileCheck --match-full-lines %s

// Under MallocGC handle sanitization, allocating or growing the target's
// indirect storage moves the source, its hidden class, and all object values.
// Collection after the copy alone would not exercise stale pointers in its loop.
for (var width = 6; width <= 17; ++width) {
  var source = {};
  for (var field = 0; field < width; ++field)
    source['field' + field] = {value: field};

  var target = {};
  HermesInternal.copyDataProperties(target, source);
  if (target === source ||
      Object.keys(target).join(',') !== Object.keys(source).join(','))
    throw new Error('copy identity or property order changed');

  for (var field = 0; field < width; ++field) {
    var key = 'field' + field;
    if (target[key] !== source[key] || target[key].value !== field)
      throw new Error('copied object value changed');
    target[key] = null;
    if (source[key] === null)
      throw new Error('copy aliases source property storage');
  }
  print('copied', width);
}
// CHECK: copied 6
// CHECK-NEXT: copied 7
// CHECK-NEXT: copied 8
// CHECK-NEXT: copied 9
// CHECK-NEXT: copied 10
// CHECK-NEXT: copied 11
// CHECK-NEXT: copied 12
// CHECK-NEXT: copied 13
// CHECK-NEXT: copied 14
// CHECK-NEXT: copied 15
// CHECK-NEXT: copied 16
// CHECK-NEXT: copied 17
