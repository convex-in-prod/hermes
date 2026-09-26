/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -target=HBC -O %s | %FileCheck --match-full-lines %s
// RUN: %shermes -exec %s | %FileCheck --match-full-lines %s

print(JSON.stringify([1, 2, 3].map(function (value) { return value * 2; })));
// CHECK: [2,4,6]

var updated = [1, 2, 3];
print(JSON.stringify(updated.map(function (value, index) {
  if (index === 0) updated[1] = 7;
  return value * 2;
})));
// CHECK-NEXT: [2,14,6]

var inherited = [1, 2, 3];
print(JSON.stringify(inherited.map(function (value, index) {
  if (index === 0) {
    delete inherited[1];
    Array.prototype[1] = 9;
  }
  return value * 2;
})));
delete Array.prototype[1];
// CHECK-NEXT: [2,18,6]

var truncated = [1, 2, 3];
print(JSON.stringify(truncated.map(function (value, index) {
  if (index === 0) truncated.length = 1;
  return value * 2;
})));
// CHECK-NEXT: [2,null,null]
