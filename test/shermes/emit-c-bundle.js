/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// REQUIRES: linux
// RUN: rm -rf %t.dir && mkdir %t.dir
// RUN: cd %t.dir && %shermes -O -emit-c -Xemit-c-bundle -Xemit-c-shard-size=1 -exported-unit=bundle_test -o unit.c.json %s
// RUN: cd %t.dir && %shermes -O -emit-c -Xemit-c-bundle -Xemit-c-shard-size=1 -exported-unit=bundle_other -o other.c.json %s
// RUN: %FileCheck --input-file=%t.dir/unit.c.json %s
// RUN: cd %t.dir && %python -c "import hashlib,json,pathlib; manifest=json.load(open('unit.c.json')); members=[manifest['header'],*manifest['translationUnits']]; assert all(hashlib.sha256(pathlib.Path(member['path']).read_bytes()).hexdigest() == member['sha256'] and pathlib.Path(member['path']).stat().st_size == member['size'] for member in members)"
// RUN: cd %t.dir && %python -c "import pathlib; source=pathlib.Path('sh_bundle_test_metadata.c').read_text(); assert 'SH_STATIC_ABI_DESCRIPTOR_INITIALIZER' in source and '&s_static_abi_descriptor, sizeof(s_static_abi_descriptor)' in source and source.index('_sh_check_abi') < source.index('calloc')"
// RUN: cd %t.dir && for source in sh_bundle_*.c; do %c_compiler -I%static_h_config -I%S/../../include -c "$source" -o "${source%.c}.o" || exit; done
// RUN: cd %t.dir && %c_compiler -r sh_bundle_test_*.o sh_bundle_other_*.o -o unit.o
// RUN: cd %t.dir && ! nm -u unit.o | grep 'sh_bundle_\(test\|other\)'

// CHECK: {"header":{"path":"sh_bundle_test_internal.h","role":"header","sha256":"{{[0-9a-f][0-9a-f]+}}","size":{{[1-9][0-9]*}}},"kind":"static-hermes-c-bundle-v1","schemaVersion":1,"translationUnits":[{"path":"sh_bundle_test_metadata.c","role":"metadata","sha256":"{{[0-9a-f][0-9a-f]+}}","size":{{[1-9][0-9]*}}},{"firstFunctionId":0,"functionCount":1,"lastFunctionId":0,"oversize":true,"oversizeReason":"no-outlineable-run","path":"sh_bundle_test_functions_00000.c"
// CHECK-SAME: {"firstFunctionId":1,"functionCount":1,"lastFunctionId":1,"oversize":true,"oversizeReason":"no-outlineable-run","path":"sh_bundle_test_functions_00001.c"
// CHECK-SAME: {"firstFunctionId":2,"functionCount":1,"lastFunctionId":2,"oversize":true,"oversizeReason":"no-outlineable-run","path":"sh_bundle_test_functions_00002.c"

function child() {
  "noinline";
  return 1;
}

function parent() {
  "noinline";
  return child();
}

print(parent());
