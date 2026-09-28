/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -typed -exec %s | %FileCheck --match-full-lines %s

const malloc = $SHBuiltin.extern_c({include: "stdlib.h"},
    function malloc(size: c_size_t): c_ptr { throw 0; });
const free = $SHBuiltin.extern_c({include: "stdlib.h"},
    function free(pointer: c_ptr): void {});
const readByte = $SHBuiltin.extern_c({declared: true},
    function _sh_ptr_read_uchar(pointer: c_ptr, offset: c_int): c_uchar { throw 0; });
const writeUtf8 = $SHBuiltin.extern_c({declared: true, hv: true},
    function _sh_string_write_utf8(
        runtime: c_ptr,
        value: string,
        destination: c_ptr,
        capacity: c_size_t): c_ptrdiff_t { throw 0; });

const pointer = malloc(16);
const bytes = [65, 195, 169, 240, 159, 152, 128, 239, 191, 189];
const written = writeUtf8($SHBuiltin.c_native_runtime(), "Aé😀\ud800", pointer, 16);
if (written !== bytes.length) throw new Error("UTF-8 length differs");
for (let index = 0; index < bytes.length; index += 1) {
    if (readByte(pointer, index) !== bytes[index]) throw new Error("UTF-8 byte differs");
}
print("encoded");
// CHECK: encoded

if (writeUtf8($SHBuiltin.c_native_runtime(), "é", pointer, 1) !== -1) {
    throw new Error("short destination accepted");
}
print("bounded");
// CHECK: bounded
free(pointer);
