/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -Werror -typed -exec -O0 %s | %FileCheck --match-full-lines %s
// RUN: %shermes -Werror -typed -exec -O %s | %FileCheck --match-full-lines %s

'use strict';

print(new (class {
  value: number = 1;
})().value);
// CHECK: 1

var Assigned = class {
  value: number = 2;
};
print(new Assigned().value);
// CHECK-NEXT: 2

var Original = class {
  value: number = 3;
};
var Alias = Original;
print(new Alias().value);
// CHECK-NEXT: 3

var CommitTsPlaceholder = class CommitTsPlaceholder {
  [Symbol.toPrimitive](hint: string): string {
    return hint;
  }
};
var ExportedCommitTsPlaceholder = CommitTsPlaceholder;
var commitTsPlaceholder = new ExportedCommitTsPlaceholder();
print(typeof commitTsPlaceholder);
// CHECK-NEXT: object
print(commitTsPlaceholder instanceof CommitTsPlaceholder);
// CHECK-NEXT: true

var OuterName = class InnerName {
  innerConstructor(): any {
    return InnerName;
  }
};
var namedInstance = new OuterName();
print(namedInstance.innerConstructor() === OuterName);
// CHECK-NEXT: true
