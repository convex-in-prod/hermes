/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -fstatic-object-prototype -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fstatic-object-prototype -Xsmall-c -exec %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O -fstatic-object-prototype %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fno-inline -fstatic-object-prototype -dump-ir -Xdump-functions=absentCaller %s | %FileCheck --check-prefix=STATIC %s
// RUN: %shermes -O -fno-inline -dump-ir -Xdump-functions=absentCaller %s | %FileCheck --check-prefix=DYNAMIC %s
// RUN: %shermes -O -fno-inline -dump-ir -Xdump-functions=nullCaller %s | %FileCheck --check-prefix=NULL %s

(function () {
  'use strict';
  function assert(value) {
    if (!value) throw new Error('static prototype record contract');
  }
  // Existing standard keys may change descriptors. The flag proves absence
  // only for nonstandard keys on the intrinsic, nonextensible prototype.
  Object.defineProperty(Object.prototype, 'toString', {
    configurable: true,
    get: function () { this.value = 100; return 5; },
  });
  Object.preventExtensions(Object.prototype);

  function consume(record, callback) {
    'noinline';
    callback();
    return function () {
      gc();
      return record.value + (record.optional === undefined ? 0 : record.optional);
    };
  }
  function absentCaller(value, callback) {
    'noinline';
    return consume({value}, callback);
  }
  function presentCaller(value, callback) {
    'noinline';
    return consume({value, optional: 7}, callback);
  }
  function attemptMutation() {
    assert(!Reflect.defineProperty(Object.prototype, 'optional', {
      get: function () { this.value = 200; return 9; },
    }));
  }
  var absent = absentCaller(3, attemptMutation);
  var present = presentCaller(4, attemptMutation);
  assert(absent() === 3 && present() === 11);
  gc();
  assert(absent() === 3 && present() === 11);

  function nullConsume(record) {
    'noinline';
    return record.value + (record.toString === undefined ? 1 : 0);
  }
  function nullCaller(value) {
    'noinline';
    return nullConsume({__proto__: null, value});
  }
  assert(nullCaller(6) === 7);

  function standard(record) {
    'noinline';
    return record.value + record.toString + record.value;
  }
  assert(standard({value: 2}) === 107);
  function custom(record) {
    'noinline';
    return record.value + record.optional + record.value;
  }
  var parent = {get optional() { this.value = 20; return 4; }};
  assert(custom({__proto__: parent, value: 1}) === 25);
  print('static prototype record passed');
})();

// CHECK: static prototype record passed
// STATIC-LABEL: function absentCaller(
// STATIC-NOT: AllocObjectLiteralInst
// STATIC-NOT: PrStoreInst
// STATIC: function_end
// DYNAMIC-LABEL: function absentCaller(
// DYNAMIC: AllocObjectLiteralInst
// DYNAMIC: function_end
// NULL-LABEL: function nullCaller(
// NULL-NOT: AllocObjectLiteralInst
// NULL-NOT: PrStoreInst
// NULL: function_end
