/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: (! %shermes -Werror -ferror-limit=0 -typed -dump-sema %s 2>&1 ) | %FileCheck --match-full-lines %s

let key: any = null;

class ComputedAccessor {
  get [key](): number {
    return 1;
  }
}

class FinalComputedMethod {
  @Hermes.final
  [key](): void {}
}

class OverloadedComputedMethod {
  @Hermes.final
  @Hermes.overload
  [key](value: number): void {}
}

class GenericComputedMethod {
  [key]<T>(value: T): T {
    return value;
  }
}

// CHECK: {{.*}}computed-class-method-error.js:13:3: error: ft: computed class accessors are unsupported
// CHECK: {{.*}}computed-class-method-error.js:19:3: error: ft: decorators on computed class methods are unsupported
// CHECK: {{.*}}computed-class-method-error.js:24:3: error: ft: decorators on computed class methods are unsupported
// CHECK: {{.*}}computed-class-method-error.js:30:8: error: ft: generic computed class methods are unsupported
// CHECK: Emitted 4 errors. exiting.
