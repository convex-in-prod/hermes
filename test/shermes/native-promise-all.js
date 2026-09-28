/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -exec -Wx,-Xnative-promise,-Xhermes-internal-test-methods %S/../hermes/native-promise-all.js | %FileCheck --match-full-lines %S/../hermes/native-promise-all.js
// RUN: %shermes -O -Xsmall-c -exec -Wx,-Xnative-promise,-Xhermes-internal-test-methods %S/../hermes/native-promise-all.js | %FileCheck --match-full-lines %S/../hermes/native-promise-all.js
