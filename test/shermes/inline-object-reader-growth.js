/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Repeated callsites in one try region must not expand without a caller budget.
// RUN: python3 -c "from pathlib import Path; calls=''.join('sum += read({left: input | 0, right: %d});\n' % i for i in range(80)); Path(r'%t.js').write_text('(function () { \\\"use strict\\\"; function read(value) { var sum = value.left + value.right; return sum > 0 ? sum : 0; } function withManyReaders(input) { \\\"noinline\\\"; var sum = 0; try {\n' + calls + '\n} catch (e) { return -1; } return sum; } print(withManyReaders(1)); print(withManyReaders({valueOf() {throw 1;}})); })();')"
// RUN: %shermes -O -exec %t.js | %FileCheck --match-full-lines %s
// RUN: %shermes -O -dump-ir -Xdump-functions=withManyReaders %t.js | %FileCheck --check-prefix=IR %s

// CHECK: 3240
// CHECK-NEXT: -1
// IR-LABEL: function withManyReaders(
// IR: CallInst {{.*}} %read(): functionCode
