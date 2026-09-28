/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fno-inline -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -Xsmall-c -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O0 -exec %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O4 -verify-ir %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -verify-ir -dump-ir -Xdump-functions=produce %s | %FileCheck --check-prefix=IR %s
// RUN: %shermes -O -verify-ir -dump-ir -Xdump-functions=oppositeOrder %s | %FileCheck --check-prefix=ORDER %s
// RUN: %shermes -O -verify-ir -dump-ir -Xdump-functions=multipleReturns %s | %FileCheck --check-prefix=MULTI %s
// RUN: %shermes -O -verify-ir -dump-ir -Xdump-functions=tooManyShapes %s | %FileCheck --check-prefix=LIMIT %s

(function () {
  'use strict';
  let trace = [];
  let reenter = false;
  let nested;
  function check(condition) {
    if (!condition) throw new Error('record construction mismatch');
  }
  function effect(name, value, fail) {
    'noinline';
    trace.push(name);
    try {
      if (reenter && name === 'tail') {
        reenter = false;
        nested = produce(false, {nested: true}, false);
      }
      if (fail) throw new Error('initializer');
      gc();
      return value;
    } finally {
      trace.push(name + ':finally');
    }
  }

  function produce(include, child, fail) {
    'noinline';
    return {
      first: effect('first', child, false),
      second: effect('second', child, false),
      ...(include ? {optional: effect('optional', undefined, false)} : {}),
      tail: effect('tail', child, fail),
      fifth: effect('fifth', child, false),
      sixth: effect('sixth', 6, false),
      last: effect('last', child, false),
    };
  }
  const child = {value: 17};
  for (const include of [false, true]) {
    trace = [];
    const record = produce(include, child, false);
    check(Object.keys(record).join(',') ===
      (include ? 'first,second,optional,tail,fifth,sixth,last'
               : 'first,second,tail,fifth,sixth,last'));
    check(Object.prototype.hasOwnProperty.call(record, 'optional') === include);
    check(record.first === child && record.second === child &&
          record.tail === child && record.last === child);
    const names = include ? ['first', 'second', 'optional', 'tail', 'fifth', 'sixth', 'last']
                          : ['first', 'second', 'tail', 'fifth', 'sixth', 'last'];
    check(trace.join(',') === names.map(n => n + ',' + n + ':finally').join(','));
    check(Object.keys(Object.getOwnPropertyDescriptors(record)).join(',') === Object.keys(record).join(','));
    for (const name of Object.keys(record)) {
      const descriptor = Object.getOwnPropertyDescriptor(record, name);
      check(descriptor.writable && descriptor.enumerable && descriptor.configurable);
    }
    record.first = {changed: true};
    check(record.last === child);
  }
  trace = [];
  try {
    produce(true, child, true);
    throw new Error('missing initializer failure');
  } catch (error) {
    check(error.message === 'initializer');
  }
  check(trace.join(',') === 'first,first:finally,second,second:finally,optional,optional:finally,tail,tail:finally');
  reenter = true;
  const outer = produce(true, child, false);
  check(outer.first === child && outer.last === child);
  check(nested.first === nested.last && nested.first !== child);
  check(!Object.prototype.hasOwnProperty.call(nested, 'optional'));

  function overwrite(include) {
    'noinline';
    return {
      first: effect('overwritten', 1, false),
      ...(include ? {first: effect('replacement', 2, false)} : {}),
      tail: effect('tail', 3, false),
      last: effect('last', 4, false),
    };
  }
  for (const include of [false, true]) {
    trace = [];
    const result = overwrite(include);
    check(result.first === (include ? 2 : 1));
    check(Object.keys(result).join(',') === 'first,tail,last');
    check(trace[0] === 'overwritten');
    check(trace.includes('replacement') === include);
  }

  // Identical sets of keys can have different observable insertion order.
  function oppositeOrder(reverse) {
    'noinline';
    return {
      ...(reverse ? {b: effect('b', 2, false), a: effect('a', 1, false)}
                  : {a: effect('a', 1, false), b: effect('b', 2, false)}),
      tail: effect('tail', 3, false),
      last: effect('last', 4, false),
    };
  }
  check(Object.keys(oppositeOrder(false)).join(',') === 'a,b,tail,last');
  check(Object.keys(oppositeOrder(true)).join(',') === 'b,a,tail,last');

  function multipleReturns(early, include) {
    'noinline';
    const record = {
      first: effect('first', child, false),
      ...(include ? {optional: undefined} : {}),
      tail: effect('tail', child, false),
      last: effect('last', child, false),
    };
    if (early) return record;
    effect('before return', child, false);
    return record;
  }
  for (const early of [false, true]) {
    for (const include of [false, true]) {
      trace = [];
      const result = multipleReturns(early, include);
      check(Object.keys(result).join(',') ===
        (include ? 'first,optional,tail,last' : 'first,tail,last'));
      check(result.first === child && result.last === child);
      check(trace.includes('before return') === !early);
    }
  }

  function tooManyShapes(a, b, c) {
    'noinline';
    return {first: child, ...(a ? {a: 1} : {}),
            ...(b ? {b: 2} : {}), ...(c ? {c: 3} : {}),
            tail: effect('tail', child, false), last: effect('last', child, false)};
  }
  for (let mask = 0; mask < 8; ++mask) {
    const result = tooManyShapes(mask & 1, mask & 2, mask & 4);
    const expected = ['first'];
    for (let bit = 0; bit < 3; ++bit)
      if (mask & (1 << bit)) expected.push(['a', 'b', 'c'][bit]);
    expected.push('tail', 'last');
    check(Object.keys(result).join(',') === expected.join(','));
  }

  function nullParent(include) {
    'noinline';
    return {__proto__: null, first: child,
            ...(include ? {optional: undefined} : {}),
            tail: effect('tail', child, false), last: effect('last', child, false)};
  }
  for (const include of [false, true]) {
    const result = nullParent(include);
    check(Object.getPrototypeOf(result) === null);
    check(result.first === result.last);
    check(Object.prototype.hasOwnProperty.call(result, 'optional') === include);
  }

  const prototype = {inherited: true};
  function customParent(include) {
    'noinline';
    return {__proto__: prototype, first: child,
            ...(include ? {optional: undefined} : {}),
            tail: effect('tail', child, false), last: effect('last', child, false)};
  }
  check(Object.getPrototypeOf(customParent(true)) === prototype);

  let getterCalls = 0;
  function accessor(include) {
    'noinline';
    return {get first() { ++getterCalls; return child; },
            ...(include ? {optional: undefined} : {}),
            tail: effect('tail', child, false), last: effect('last', child, false)};
  }
  const withGetter = accessor(true);
  check(getterCalls === 0);
  check(withGetter.first === child && getterCalls === 1);

  Object.defineProperty(Object.prototype, 'optional', {
    configurable: true, get() { return 99; }
  });
  try {
    check(produce(false, child, false).optional === 99);
    check(produce(true, child, false).optional === undefined);
  } finally {
    delete Object.prototype.optional;
  }
  print('late object construction passed');
})();
// CHECK: late object construction passed

// IR-LABEL: function produce(
// IR-NOT: DefineOwnPropertyInst
// IR-NOT: BinaryOrInst
// IR: CondBranchInst
// IR-NOT: DefineOwnPropertyInst
// IR-NOT: BinaryOrInst
// IR: AllocObjectLiteralInst
// IR-NOT: DefineOwnPropertyInst
// IR: ReturnInst
// IR-NOT: DefineOwnPropertyInst
// IR: AllocObjectLiteralInst
// IR-NOT: DefineOwnPropertyInst
// IR: ReturnInst
// IR-NOT: DefineOwnPropertyInst
// IR: function_end

// ORDER-LABEL: function oppositeOrder(
// ORDER: DefineOwnPropertyInst
// ORDER: function_end

// MULTI-LABEL: function multipleReturns(
// MULTI-NOT: DefineOwnPropertyInst
// MULTI: function_end

// LIMIT-LABEL: function tooManyShapes(
// LIMIT: DefineOwnPropertyInst
// LIMIT: function_end
