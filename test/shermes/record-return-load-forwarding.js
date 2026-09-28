/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -Xcustom-opt=simplestackpromotion,simplemem2reg,frameloadstoreopts,functionanalysis,objectmergenewstores,record-return-load-forwarding,typeinference,instsimplify,dce -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -Xsmall-c -Xcustom-opt=simplestackpromotion,simplemem2reg,frameloadstoreopts,functionanalysis,objectmergenewstores,record-return-load-forwarding,typeinference,instsimplify,dce -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O0 -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -Xcustom-opt=simplestackpromotion,simplemem2reg,frameloadstoreopts,functionanalysis,objectmergenewstores,record-return-load-forwarding,typeinference,instsimplify,dce -verify-ir -dump-ir -Xdump-functions=consume %s | %FileCheck --check-prefix=IR %s

(function () {
  'use strict';
  let published;
  let trace = '';
  function check(value) {
    if (!value) throw new Error('returned record contract');
  }
  function produce(optional, child) {
    'noinline';
    trace += 'p';
    gc();
    if (optional) return {base: child, optional: undefined, tail: child};
    return {base: child, tail: child};
  }
  function forward(optional, child) {
    'noinline';
    try {
      return produce(optional, child);
    } finally {
      trace += 'f';
    }
  }
  function consume(optional, child) {
    'noinline';
    const result = forward(optional, child);
    const first = result.base;
    const last = result.tail;
    published = result;
    return [first, last];
  }
  function mutate(record) {
    'noinline';
    record.base = 91;
    delete record.tail;
    gc();
  }
  function afterEscape(child) {
    'noinline';
    const result = produce(false, child);
    mutate(result);
    return [result.base, result.tail];
  }
  function alreadyPublished(child) {
    'noinline';
    const result = {base: child};
    published = result;
    return result;
  }
  function changePublished() {
    'noinline';
    published.base = 73;
  }
  const child = {count: 1};
  for (const optional of [false, true]) {
    const result = consume(optional, child);
    check(result[0] === child && result[1] === child);
    check(Object.prototype.hasOwnProperty.call(published, 'optional') === optional);
    check(Object.keys(published).join(',') ===
      (optional ? 'base,optional,tail' : 'base,tail'));
    child.count++;
    check(published.base.count === child.count);
  }
  check(trace === 'pfpf');
  const changed = afterEscape(child);
  check(changed[0] === 91 && changed[1] === undefined);
  const escaped = alreadyPublished(child);
  changePublished();
  check(escaped.base === 73);

  let getterCalls = 0;
  Object.defineProperty(Object.prototype, 'optional', {
    configurable: true,
    get() { getterCalls++; this.base = 63; return 47; },
  });
  const absent = produce(false, child);
  check(absent.optional === 47 && getterCalls === 1);
  check(absent.base === 63);
  const present = produce(true, child);
  check(present.optional === undefined && getterCalls === 1);
  delete Object.prototype.optional;
  print('returned record semantics passed');
})();

// CHECK: returned record semantics passed
// IR-LABEL: function consume(
// IR-NOT: LoadPropertyInst
// IR: ReturnInst
