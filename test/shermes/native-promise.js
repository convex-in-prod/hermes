/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Exercise the shared semantic fixtures with compiled callers and explicit
// activation, including runtimes with HermesInternal disabled.
// RUN: %shermes -O -exec -Wx,-Xnative-promise %S/../hermes/native-promise-core.js | %FileCheck --match-full-lines %S/../hermes/native-promise-core.js
// RUN: %shermes -O -Xsmall-c -exec -Wx,-Xnative-promise,-enable-hermes-internal=false %S/../hermes/native-promise-core.js | %FileCheck --match-full-lines %S/../hermes/native-promise-core.js
// RUN: %shermes -O -fasync-frame-arguments -exec -Wx,-Xnative-promise,-Xhermes-internal-test-methods %S/../hermes/native-promise-reentrancy.js | %FileCheck --match-full-lines %S/../hermes/native-promise-reentrancy.js
// RUN: %shermes -O -fasync-generator-results -exec -Wx,-Xnative-promise,-Xhermes-internal-test-methods %S/../hermes/native-promise-reentrancy.js | %FileCheck --match-full-lines %S/../hermes/native-promise-reentrancy.js
// RUN: %shermes -O -fasync-generator-results -exec -Wx,-Xnative-promise %S/async-generator-results.js | %FileCheck --match-full-lines %S/async-generator-results.js
// RUN: %shermes -O -fasync-generator-results -Xsmall-c -exec -Wx,-Xnative-promise %S/async-generator-intrinsics.js | %FileCheck --match-full-lines %S/async-generator-intrinsics.js
// RUN: %shermes -O -fasync-generator-results -exec -Wx,-Xnative-promise %S/../hermes/async-function-reaction-failure.js | %FileCheck --match-full-lines %S/../hermes/async-function-reaction-failure.js
// RUN: %shermes -O -fasync-generator-results -exec -Wx,-Xnative-promise %S/../hermes/async-function-thenable-bind.js | %FileCheck --match-full-lines %S/../hermes/async-function-thenable-bind.js
// RUN: %shermes -O -fasync-generator-results -exec -Wx,-Xnative-promise %S/../hermes/async-function-await-rejection.js | %FileCheck --match-full-lines %S/../hermes/async-function-await-rejection.js
