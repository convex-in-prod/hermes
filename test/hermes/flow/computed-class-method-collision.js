/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -Werror -typed -O0 %s | %FileCheck --match-full-lines %s
// RUN: %shermes -Werror -typed -exec -O0 %s | %FileCheck --match-full-lines %s

'use strict';

let methodKey: any = 'method';

class StaticCollision {
  static method(): string {
    return 'named static';
  }

  static [methodKey](): string {
    return 'computed static';
  }
}

print(StaticCollision.method());
// CHECK: computed static
print(((StaticCollision: any).method)());
// CHECK-NEXT: computed static

class StaticNamedLast {
  static [methodKey](): string {
    return 'computed static';
  }

  static method(): string {
    return 'named static';
  }
}

print(StaticNamedLast.method());
// CHECK-NEXT: named static
print(((StaticNamedLast: any).method)());
// CHECK-NEXT: named static

class ParentMethod {
  method(): string {
    return 'parent';
  }
}

class InheritedCollision extends ParentMethod {
  [methodKey](): string {
    return 'computed instance';
  }
}

let inherited: InheritedCollision = new InheritedCollision();
print(inherited.method());
// CHECK-NEXT: computed instance
print(((inherited: any).method)());
// CHECK-NEXT: computed instance

class InstanceNamedLast {
  [methodKey](): string {
    return 'computed instance';
  }

  method(): string {
    return 'named instance';
  }
}

let namedLast: InstanceNamedLast = new InstanceNamedLast();
print(namedLast.method());
// CHECK-NEXT: named instance
print(((namedLast: any).method)());
// CHECK-NEXT: named instance
