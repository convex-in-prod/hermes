/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -exec -Wx,-Xnative-promise,-Xhermes-internal-test-methods,-gc-init-heap=4M,-gc-max-heap=32M %S/../hermes/native-promise-all-gc.js | %FileCheck --match-full-lines %S/../hermes/native-promise-all-gc.js
// REQUIRES: gc_hades
