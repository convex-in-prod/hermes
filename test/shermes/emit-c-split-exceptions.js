/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// REQUIRES: linux
// RUN: %shermes -O -exec %s | %FileCheck %s
// RUN: rm -rf %t.dir && mkdir %t.dir
// RUN: cd %t.dir && %shermes -O -verify-ir -emit-c -Xemit-c-bundle -Xsplit-c-exceptions -o unit.c.json %s
// RUN: cd %t.dir && env python3 -c "import pathlib; source=''.join(p.read_text() for p in pathlib.Path('.').glob('*functions*.c')); assert '_exception_dispatch(' in source and '_exception_body(' in source and 'switch (resume)' in source and 'L_catch:' not in source"
// RUN: cd %t.dir && for source in sh_this_unit_*.c; do %c_compiler -O2 -DNDEBUG -I%static_h_config -I%S/../../include -c "$source" -o "${source%.c}.o" || exit; done
// RUN: cd %t.dir && %c_compiler sh_this_unit_*.o -L%hermesvm_lib_dir -L%shermes_console_lib_dir -Wl,-rpath,%hermesvm_lib_dir -Wl,-rpath,%shermes_console_lib_dir -lshermes_console -lhermesvm -lm -o executable
// RUN: cd %t.dir && ./executable | %FileCheck %s

(function () {
  'use strict';
  function check(value) {
    if (!value) throw new Error('exception dispatch contract');
  }
  const retained = [];
  let trace = '';
  function fail(value) {
    'noinline';
    gc();
    throw value;
  }
  function nested(depth, mode) {
    'noinline';
    const value = {depth, count: 0};
    retained.push(function () { return value; });
    try {
      try {
        if (depth > 0) {
          const child = nested(depth - 1, mode);
          check(child.depth === depth - 1 && child.count === 111);
        }
        value.count = 1;
        gc();
        if (mode !== 0) fail(value);
        return value;
      } catch (error) {
        check(error === value && error.count === 1);
        trace += 'c';
        if (mode === 2) fail(error);
        return value;
      } finally {
        value.count += 10;
        trace += 'i';
        gc();
      }
    } catch (error) {
      check(error === value && error.count === 11);
      trace += 'o';
      return value;
    } finally {
      value.count += 100;
      trace += 'f';
      gc();
    }
  }
  for (let mode = 0; mode < 3; ++mode) {
    trace = '';
    const result = nested(2, mode);
    check(result.depth === 2 && result.count === 111);
    const step = mode === 0 ? 'if' : mode === 1 ? 'cif' : 'ciof';
    check(trace === step + step + step);
  }
  gc();
  for (const read of retained) check(read().count === 111);

  const marker = {};
  function outside() {
    'noinline';
    try { gc(); } catch (error) { check(false); }
    fail(marker);
  }
  try {
    outside();
    check(false);
  } catch (error) {
    check(error === marker);
  }

  const finalValue = {};
  function override() {
    'noinline';
    try { fail(marker); } finally { gc(); return finalValue; }
  }
  check(override() === finalValue);

  function throwingFinally() {
    'noinline';
    try { return marker; } finally { fail(finalValue); }
  }
  try {
    throwingFinally();
    check(false);
  } catch (error) {
    check(error === finalValue);
  }
  function nativeCallback() {
    'noinline';
    try {
      return [marker].map(function (value) { gc(); throw value; });
    } catch (error) {
      return error;
    }
  }
  check(nativeCallback() === marker);

  let closed = 0;
  function* values() {
    try {
      yield marker;
      fail(finalValue);
    } catch (error) {
      gc();
      yield error;
    } finally {
      ++closed;
      gc();
    }
  }
  const first = values();
  check(first.next().value === marker);
  check(first.next().value === finalValue);
  check(first.next().done && closed === 1);
  const second = values();
  check(second.next().value === marker);
  check(second.throw(finalValue).value === finalValue);
  check(second.return(marker).value === marker && closed === 2);
  print('split exception dispatch passed');
})();
// CHECK: split exception dispatch passed
