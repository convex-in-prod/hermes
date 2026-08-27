/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: (! %shermes -Werror -typed -dump-sema %s 2>&1 ) | %FileCheck --match-full-lines %s

let key: any = null;

class CheckedComputedMethodBody {
  [key](): number {
    return 'not a number';
  }
}

// CHECK: {{.*}}computed-class-method-body-error.js:14:5: error: ft: return value incompatible with return type: cannot return string as number
// CHECK: Emitted 1 errors. exiting.
