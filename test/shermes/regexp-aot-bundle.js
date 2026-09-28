/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// REQUIRES: linux, shermes
// RUN: rm -rf %t.bundle && mkdir %t.bundle
// RUN: cd %t.bundle && %shermes -O -emit-c -Xemit-c-bundle -Xemit-c-shard-size=65536 -o unit.c.json %S/regexp-aot.js
// RUN: cd %t.bundle && for source in sh_this_unit_*.c; do %c_compiler -O2 -DNDEBUG -I%static_h_config -I%S/../../include -c "$source" -o "${source%.c}.o" || exit; done
// RUN: cd %t.bundle && %c_compiler sh_this_unit_*.o -L%hermesvm_lib_dir -L%shermes_console_lib_dir -Wl,-rpath,%hermesvm_lib_dir -Wl,-rpath,%shermes_console_lib_dir -lshermes_console -lhermesvm -lm -o regexp-aot
// RUN: cd %t.bundle && ./regexp-aot | %FileCheck --match-full-lines %S/regexp-aot.js

// Run the existing parity cases across separately compiled translation units.
// Matcher definitions live in metadata while literal references can occur in
// function shards and outlined helpers; emitted-text checks cannot verify this
// linkage or the runtime function pointers.
