/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -fguarded-method-calls -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fguarded-method-calls -fno-inline -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fguarded-method-calls -Xsmall-c -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fguarded-method-calls -dump-ir -Xdump-functions=invoke %s | %FileCheck --check-prefix=IR %s
// RUN: %shermes -O -fguarded-method-calls -fno-inline -dump-ir -Xdump-functions=invoke %s | %FileCheck --check-prefix=DIRECT %s
// RUN: %shermes -O -fguarded-method-calls -dump-ir -Xdump-functions=route %s | %FileCheck --check-prefix=OWNER %s

(function () {
  'use strict';
  function assert(value) {
    if (!value) throw new Error('guarded method contract');
  }
  var trace = '';
  var sentinel = {};
  class Builder {
    constructor(value) { this.value = value; }
    consume() { this.value = this.value + 1; }
    append(x) { this.consume(); return this.value + x; }
  }
  function invoke(receiver, x) {
    'noinline';
    return receiver.append(x);
  }
  const original = Builder.prototype.append;
  assert(invoke(new Builder(4), 2) === 7);
  const borrowed = {value: 8, consume: Builder.prototype.consume, append: original};
  assert(invoke(borrowed, 3) === 12);
  borrowed.consume = function () { this.value = 20; };
  assert(invoke(borrowed, 3) === 23);

  class Child extends Builder {
    consume() { this.value = 30; }
  }
  assert(invoke(new Child(1), 2) === 32);
  Builder.prototype.append = function (x) { return this.value - x; };
  assert(invoke(new Builder(8), 3) === 5);
  Builder.prototype.append = original;
  const own = new Builder(7);
  own.append = function (x) { return this.value * x; };
  assert(invoke(own, 3) === 21);
  own.append = original.bind(new Builder(40));
  assert(invoke(own, 2) === 43);

  const getter = new Builder(10);
  Object.defineProperty(getter, 'append', {get() {
    trace += 'g';
    gc();
    return original;
  }});
  assert(invoke(getter, 2) === 13 && trace === 'g');
  const proxy = new Proxy(new Builder(50), {get(target, key, receiver) {
    trace += key + ';';
    return Reflect.get(target, key, receiver);
  }});
  trace = '';
  assert(invoke(proxy, 2) === 53);
  assert(trace === 'append;consume;value;value;');

  function invokeEffect(receiver, effect) {
    'noinline';
    return receiver.append(effect());
  }
  trace = '';
  assert(invokeEffect(getter, function () {
    trace += 'a';
    Builder.prototype.append = function () { throw sentinel; };
    gc();
    return 4;
  }) === 16);
  assert(trace === 'ga');
  Builder.prototype.append = original;
  const throwing = {get append() { trace += 't'; throw sentinel; }};
  trace = '';
  try { invokeEffect(throwing, function () { trace += 'a'; }); assert(false); }
  catch (error) { assert(error === sentinel && trace === 't'); }
  try { invokeEffect(getter, function () { throw sentinel; }); assert(false); }
  catch (error) { assert(error === sentinel && trace === 'tg'); }
  try { invoke({append: undefined}, 1); assert(false); }
  catch (error) { assert(error instanceof TypeError); }
  const revocable = Proxy.revocable(original, {});
  revocable.revoke();
  try { invoke({append: revocable.proxy}, 1); assert(false); }
  catch (error) { assert(error instanceof TypeError); }

  class Branches {
    constructor() { this.calls = 0; }
    consume() { this.calls++; }
    route(a, b, c, d) {
      'noinline';
      const kind = a ? 'one' : b ? 'two' : c ? 'three' : d ? 'four' : 'five';
      this.consume();
      return kind;
    }
  }
  const branches = new Branches();
  assert(branches.route(false, false, false, true) === 'four');
  assert(branches.calls === 1);

  function factory(offset) {
    'noinline';
    function callCaptured(receiver, x) {
      'noinline';
      return receiver.addCaptured(x);
    }
    // Calls before class initialization still fail when both lookup and the
    // private capture produce undefined.
    try { callCaptured({addCaptured: undefined}, 2); assert(false); }
    catch (error) { assert(error instanceof TypeError); }
    class Captured {
      addCaptured(x) { gc(); return offset + this.value + x; }
    }
    const object = new Captured();
    object.value = 3;
    return {object, call: callCaptured};
  }
  const first = factory(10), second = factory(100);
  assert(first.call(first.object, 2) === 15);
  assert(second.call(second.object, 2) === 105);
  assert(first.call(second.object, 2) === 105);
  assert(second.call(first.object, 2) === 15);
  gc();
  assert(first.call(first.object, 4) === 17);
})();

print('guarded methods passed');
// CHECK: guarded methods passed

// The original lookup survives, followed by identity discrimination and the
// inlined property work on the matching branch.
// IR-LABEL: function invoke
// IR: LoadPropertyInst {{.*}} "append"
// IR: BinaryStrictlyEqualInst
// IR: LoadPropertyInst {{.*}} "consume"

// A non-inlined fast path must also keep the proved target and closure fact.
// DIRECT-LABEL: function invoke
// DIRECT: CallInst {{.*}} %append(): functionCode, true: boolean

// Branches in a caller must not hide its owner when selecting a small callee.
// OWNER-LABEL: method route
// OWNER: LoadPropertyInst {{.*}} "consume"
// OWNER: BinaryStrictlyEqualInst
// OWNER: LoadPropertyInst {{.*}} "calls"
