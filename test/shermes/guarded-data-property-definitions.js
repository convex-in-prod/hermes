/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -fstatic-object-prototype -fguarded-data-property-definitions -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -Xsmall-c -fstatic-object-prototype -fguarded-data-property-definitions -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fstatic-object-prototype -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fstatic-object-prototype -fguarded-data-property-definitions -verify-ir -dump-ir -Xdump-functions=install %s | %FileCheck --check-prefix=IR %s
// RUN: %shermes -O -fstatic-object-prototype -fguarded-data-property-definitions -verify-ir -dump-ir -Xdump-functions=result %s | %FileCheck --check-prefix=RESULT %s
// RUN: %shermes -O -fstatic-object-prototype -fguarded-data-property-definitions -verify-ir -dump-ir -Xdump-functions=partial %s | %FileCheck --check-prefix=PARTIAL %s

function looseWriterFactory(define) {
  return function loose(target, value) {
    'noinline';
    if ('field' in target) define(target, 'field', {
      value, enumerable: true, writable: true, configurable: true,
    });
    else target.field = value;
    return target;
  };
}

(function () {
  'use strict';
  const original = Object.defineProperty;
  let define = original;
  let trace = '';
  function check(value) {
    if (!value) throw new Error('data property contract');
  }
  function install(target, value) {
    'noinline';
    if ('field' in target) {
      define(target, 'field', {
        value, enumerable: true, writable: true, configurable: true,
      });
    } else {
      target.field = value;
    }
    return target;
  }
  function descriptor(target, value) {
    const d = Object.getOwnPropertyDescriptor(target, 'field');
    check(d.value === value && d.enumerable && d.configurable && d.writable);
  }
  const child = {id: 23};
  for (const target of [{}, Object.create(null), {field: 1}]) {
    check(install(target, child) === target);
    gc();
    descriptor(target, child);
    install(target, undefined);
    descriptor(target, undefined);
  }

  const prototype = {};
  original(prototype, 'field', {
    get() { throw new Error('inherited getter must not run'); },
    set() { throw new Error('inherited setter must not run'); },
  });
  const inherited = Object.create(prototype);
  install(inherited, child);
  descriptor(inherited, child);
  const ownAccessor = {};
  original(ownAccessor, 'field', {
    get() { throw new Error('own getter must not run'); }, configurable: true,
  });
  install(ownAccessor, child);
  descriptor(ownAccessor, child);

  for (const target of [Object.preventExtensions({}), Object.freeze({field: 2}),
                        null, undefined, 1, 'text']) {
    let failed = false;
    try { install(target, child); } catch (error) { failed = error instanceof TypeError; }
    check(failed);
  }
  const nonextensible = Object.preventExtensions({field: 1});
  install(nonextensible, child);
  descriptor(nonextensible, child);
  // Prime the same empty shape and parent immediately before checking the
  // object's independent extensibility flag on the cached-addition path.
  install({}, child);
  const emptySealed = Object.preventExtensions({});
  let sealedFailure = false;
  try { install(emptySealed, child); } catch (error) {
    sealedFailure = error instanceof TypeError;
  }
  check(sealedFailure && !Object.hasOwn(emptySealed, 'field'));

  const proxy = new Proxy({}, {
    has(target, key) { trace += 'h'; return Reflect.has(target, key); },
    set(target, key, value) { trace += 's'; target[key] = value; return true; },
    defineProperty(target, key, d) {
      trace += 'd'; return Reflect.defineProperty(target, key, d);
    },
  });
  install(proxy, 1);
  install(proxy, 2);
  check(trace === 'hshd' && proxy.field === 2);
  trace = '';
  const inheritedProxy = Object.create(new Proxy({}, {
    has() { trace += 'h'; return true; },
  }));
  install(inheritedProxy, 3);
  check(trace === 'h' && inheritedProxy.field === 3);

  const replacement = function (target, key, d) {
    trace += 'r'; gc(); return original(target, key, d);
  };
  define = replacement;
  trace = '';
  install({field: 0}, 4);
  install({}, 5);
  check(trace === 'r');
  define = original;
  trace = '';
  const reentrant = new Proxy({field: 0}, {
    has() { trace += 'h'; define = replacement; gc(); return true; },
  });
  install(reentrant, child);
  check(trace === 'hr' && reentrant.field === child);
  define = original;

  const revoked = Proxy.revocable({}, {});
  revoked.revoke();
  let revokedFailure = false;
  try { install(revoked.proxy, 0); } catch (error) {
    revokedFailure = error instanceof TypeError;
  }
  check(revokedFailure);
  let deep = {};
  for (let i = 0; i < 24; ++i) deep = Object.create(deep);
  install(deep, child);
  descriptor(deep, child);
  const array = [];
  install(array, child);
  descriptor(array, child);
  const typed = new Uint8Array(4);
  install(typed, child);
  descriptor(typed, child);

  function result(target, value) {
    'noinline';
    return 'field' in target ? define(target, 'field', {
      value, enumerable: true, writable: true, configurable: true,
    }) : target.field = value;
  }
  const existing = {field: 1};
  check(result(existing, child) === existing);
  check(result({}, child) === child);

  function partial(target, value) {
    'noinline';
    if ('field' in target) define(target, 'field', {value, writable: true});
    else target.field = value;
  }
  const hidden = {};
  original(hidden, 'field', {value: 0, configurable: true});
  partial(hidden, child);
  check(!Object.getOwnPropertyDescriptor(hidden, 'field').enumerable);

  const sealedEmpty = Object.preventExtensions({});
  check(looseWriterFactory(original)(sealedEmpty, child) === sealedEmpty);
  check(!Object.prototype.hasOwnProperty.call(sealedEmpty, 'field'));
  // Stress objects and values across explicit moving collections. No raw
  // pointer may survive from the probe into an allocating definition.
  let checksum = 0;
  for (let i = 0; i < 400; ++i) {
    const target = {};
    const value = {i};
    install(target, value);
    if ((i & 15) === 0) gc();
    checksum += target.field.i;
  }
  check(checksum === 79800);
  // Added slots cross the direct/indirect boundary and then grow the backing
  // storage. Both the cache and its values must survive moving collections.
  for (const width of [5, 6, 7, 12, 16]) {
    for (let i = 0; i < 24; ++i) {
      const target = {};
      for (let j = 0; j < width; ++j) target['key' + j] = j;
      const value = {i};
      install(target, value);
      if ((i & 3) === 0) gc();
      check(target.field === value && Object.keys(target).length === width + 1);
      for (let j = 0; j < width; ++j) check(target['key' + j] === j);
    }
  }
  print('guarded data properties passed');
})();

// CHECK: guarded data properties passed
// IR-LABEL: function install(
// IR: _sh_ljs_can_define_data_property
// IR: DefineOwnPropertyInst
// RESULT-LABEL: function result(
// RESULT-NOT: _sh_ljs_can_define_data_property
// RESULT: function_end
// PARTIAL-LABEL: function partial(
// PARTIAL-NOT: _sh_ljs_can_define_data_property
// PARTIAL: function_end
