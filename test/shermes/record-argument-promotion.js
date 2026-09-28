/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -Xsmall-c -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fno-inline -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O0 -exec %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -dump-ir -Xdump-functions=recordCaller %s | %FileCheck --check-prefix=IR %s
// RUN: %shermes -O -fno-inline -dump-ir -Xdump-functions=capturedCaller %s | %FileCheck --check-prefix=CAPTURE %s
// RUN: %shermes -O -fno-inline -dump-ir -Xdump-functions=capturedRead %s | %FileCheck --check-prefix=CAPTURE-READ %s

(function () {
  'use strict';
  function assert(value) {
    if (!value) throw new Error('record argument promotion contract');
  }
  var trace = '';
  function effect(value) {
    'noinline';
    trace += value;
    return value;
  }
  function consume(record, offset) {
    'noinline';
    gc();
    return record.left + record.right + record.left + offset;
  }
  function recordCaller(value) {
    'noinline';
    return consume({left: value | 0, right: effect(3)}, 4, effect(5));
  }
  function secondCaller(value) {
    'noinline';
    return consume({right: value | 0, left: 7}, 1);
  }
  assert(recordCaller(2) === 11);
  assert(trace === '35');
  assert(secondCaller(6) === 21);
  assert(recordCaller(-2) === 3);
  assert(trace === '3535');

  // Each field is evaluated at its original position, including unused fields.
  function readResult(record) {
    'noinline';
    return record.result;
  }
  function ordered() {
    'noinline';
    return readResult({unused: effect(1), result: effect(2), extra: effect(3)});
  }
  trace = '';
  assert(ordered() === 2 && trace === '123');
  var sentinel = {};
  try {
    recordCaller({valueOf() { throw sentinel; }});
    assert(false);
  } catch (error) {
    assert(error === sentinel && trace === '123');
  }

  // Mutating a referenced field value does not change the wrapper's own slot.
  function nested(record) {
    'noinline';
    record.child.count++;
    gc();
    return record.child;
  }
  var child = {count: 2};
  assert(nested({child}) === child && child.count === 3);

  // Captured values retain the invocation's environment after it returns.
  // Mutations to child objects must remain visible through the rooted value.
  function captured(record, callback) {
    'noinline';
    callback();
    return function capturedRead() {
      'noinline';
      gc();
      return record.child.count + record.amount + record.amount;
    };
  }
  function capturedCaller(child, amount, callback) {
    'noinline';
    return captured({child, amount}, callback);
  }
  var first = capturedCaller(child, 5, function () { child.count++; });
  var second = capturedCaller({count: 20}, 2, function () {});
  gc();
  assert(first() === 14 && second() === 24);
  child.count = 7;
  assert(first() === 17 && second() === 24);
  function deepCapture(record) {
    'noinline';
    return function () { return function () { gc(); return record.field; }; };
  }
  assert(deepCapture({field: child})()() === child);

  // Reassignment and escape from nested code prevent field snapshots.
  function replacedCapture(record) {
    'noinline';
    var get = function () { return record.field; };
    record = {field: 19};
    return get;
  }
  assert(replacedCapture({field: 3})() === 19);
  function escapingCapture(record) {
    'noinline';
    return [function () { return record; }, function () { return record.field; }];
  }
  var escaped = escapingCapture({field: 4});
  escaped[0]().field = 12;
  assert(escaped[1]() === 12);
  function mutateCapture(record) {
    'noinline';
    return [function () { record.field++; }, function () { return record.field; }];
  }
  var mutated = mutateCapture({field: 3});
  mutated[0]();
  assert(mutated[1]() === 4);

  // A missing-field getter can expose and mutate the otherwise fresh wrapper.
  Object.defineProperty(Object.prototype, 'optionalRecordField', {
    configurable: true,
    get: function () { this.field = 31; return 1; },
  });
  function optionalCapture(record) {
    'noinline';
    return function () {
      var before = record.field;
      var optional = record.optionalRecordField;
      return before + optional + record.field;
    };
  }
  assert(optionalCapture({field: 2})() === 34);
  assert(optionalCapture({field: 2, optionalRecordField: 5})() === 9);
  delete Object.prototype.optionalRecordField;

  // The root object is observable or mutable in the following cases.
  function escape(record) {
    'noinline';
    return record;
  }
  var saved = escape({field: 10});
  assert(saved.field === 10);
  function write(record) {
    'noinline';
    record.field++;
    return record.field;
  }
  assert(write({field: 3}) === 4);
  function aliased(record, callback) {
    'noinline';
    callback();
    return record.field;
  }
  var shared = {field: 4};
  assert(aliased(shared, function () { shared.field = 9; }) === 9);
  function afterCall(value) {
    'noinline';
    var record = {field: value};
    var before = readField(record);
    record.field = value + 1;
    return before + record.field;
  }
  function readField(record) {
    'noinline';
    return record.field;
  }
  assert(afterCall(6) === 13);

  // Absent fields and accessors must retain dynamic lookup and read counts.
  var getterReads = 0;
  function dynamic(record) {
    'noinline';
    return record.field + record.field;
  }
  assert(dynamic({get field() { return ++getterReads; }}) === 3);
  assert(getterReads === 2);
  var parent = {get inherited() { ++getterReads; return 7; }};
  function inherited(record) {
    'noinline';
    return record.inherited;
  }
  assert(inherited({__proto__: parent}) === 7 && getterReads === 3);
  function mixed(record) {
    'noinline';
    return record.present;
  }
  assert(mixed({present: 5}) === 5 && mixed({absent: 9}) === undefined);

  // Signature changes cannot affect argument-vector or function observations.
  function argumentVector(record) {
    'noinline';
    return arguments.length + arguments[0].field + record.field;
  }
  assert(argumentVector({field: 5}, 1, 2) === 13);
  function rest(record, ...tail) {
    'noinline';
    return record.field + tail.length;
  }
  assert(rest({field: 5}, 1, 2) === 7);
  function exportedReader(record) {
    'noinline';
    return record.field;
  }
  var invoke = {reader: exportedReader};
  assert(invoke.reader({field: 8}) === 8 && invoke.reader.length === 1);
  print('record argument promotion passed');
})();

// CHECK: record argument promotion passed
// IR-LABEL: function recordCaller(
// IR-NOT: AllocObjectLiteralInst
// IR-NOT: PrStoreInst
// IR: CallInst
// IR-NOT: AllocObjectLiteralInst
// IR-NOT: PrStoreInst
// IR: function_end
// CAPTURE-LABEL: function capturedCaller(
// CAPTURE-NOT: AllocObjectLiteralInst
// CAPTURE-NOT: PrStoreInst
// CAPTURE: CallInst
// CAPTURE-NOT: AllocObjectLiteralInst
// CAPTURE-NOT: PrStoreInst
// CAPTURE: function_end
// CAPTURE-READ-LABEL: function capturedRead(
// CAPTURE-READ-NOT: LoadPropertyInst {{.*}} "child"
// CAPTURE-READ-NOT: LoadPropertyInst {{.*}} "amount"
// CAPTURE-READ: function_end
