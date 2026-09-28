/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -finline-record-returns -verify-ir -exec %s | %FileCheck %s
// RUN: %shermes -O -verify-ir -exec %s | %FileCheck %s
// RUN: %shermes -O -finline-record-returns -fno-inline -verify-ir -exec %s | %FileCheck %s
// RUN: %shermes -O -finline-record-returns -Xsmall-c -verify-ir -exec %s | %FileCheck %s
// RUN: %shermes -O -finline-record-returns -verify-ir -dump-ir -Xdump-functions=read %s | %FileCheck --check-prefix=IR %s
// RUN: %shermes -O -finline-record-returns -verify-ir -dump-ir -Xdump-functions=plainRead %s | %FileCheck --check-prefix=NOHANDLER %s

(function () {
  'use strict';
  let effects = [];

  function produce(items, mode) {
    let total = 0;
    let count = 0;
    let last = null;
    try {
      for (const item of items) {
        total += +item;
        last = {count: ++count};
        if (mode === 'break') break;
        if (mode === 'throw' || mode === 'rethrow') throw new Error('body');
      }
    } catch (error) {
      effects.push('inner:' + error.message);
      if (mode === 'rethrow') throw error;
      total = -count;
    } finally {
      effects.push('finally');
      if (mode === 'finally') throw new Error('cleanup');
      gc();
    }
    return {total, count, last};
  }

  function read(items, mode) {
    'noinline';
    try {
      const result = produce(items, mode);
      return result.total * 100 + result.count * 10 +
        (result.last === null ? 0 : result.last.count);
    } catch (error) {
      effects.push('outer:' + error.message);
      return 900;
    }
  }

  function escape(items) {
    'noinline';
    return produce(items, 'normal');
  }

  function plainRead(items) {
    'noinline';
    const result = produce(items, 'normal');
    return result.total + result.count;
  }

  function readInherited(items) {
    'noinline';
    return produce(items, 'normal').inherited;
  }

  function iterable(closeThrows) {
    return {
      [Symbol.iterator]() {
        let value = 0;
        return {
          next() { return {done: value === 3, value: ++value}; },
          return() {
            effects.push('close');
            if (closeThrows) throw new Error('close');
            return {done: true};
          }
        };
      }
    };
  }

  function check(actual, expected) {
    if (actual !== expected) throw new Error('unexpected result');
  }

  for (const [mode, expected, trace] of [
    ['normal', 633, 'finally'],
    ['break', 111, 'close,finally'],
    ['throw', -89, 'close,inner:body,finally'],
    ['rethrow', 900, 'close,inner:body,finally,outer:body'],
    ['finally', 900, 'finally,outer:cleanup']
  ]) {
    effects = [];
    check(read(iterable(false), mode), expected);
    check(effects.join(','), trace);
  }
  effects = [];
  check(read(iterable(true), 'break'), -89);
  check(effects.join(','), 'close,inner:close,finally');
  effects = [];
  check(read(iterable(true), 'throw'), -89);
  check(effects.join(','), 'close,inner:body,finally');
  effects = [];
  check(read([{valueOf() { throw new Error('coercion'); }}], 'normal'), 0);
  check(effects.join(','), 'inner:coercion,finally');
  const first = escape([1]);
  const second = escape([1]);
  check(first === second, false);
  check(first.last === second.last, false);
  check(plainRead([1, 2]), 5);
  Object.defineProperty(Object.prototype, 'inherited', {
    configurable: true,
    get() { return this.count + 40; }
  });
  check(readInherited([1, 2]), 42);
  delete Object.prototype.inherited;
  print('record return inlining passed');
})();

// CHECK: record return inlining passed
// IR-LABEL: function read(
// IR-NOT: %produce(): functionCode
// IR-NOT: AllocObjectLiteralInst {{.*}}"total": string
// IR-NOT: LoadPropertyInst {{.*}}"total": string
// IR: function_end
// NOHANDLER-LABEL: function plainRead(
// NOHANDLER-NOT: TryStartInst
// NOHANDLER: CallInst {{.*}}%produce(): functionCode
// NOHANDLER-NOT: TryStartInst
// NOHANDLER: function_end
