/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -fstring-trim-comparisons -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fstring-trim-comparisons -Xsmall-c -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O0 -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fstring-trim-comparisons -dump-lir -Xdump-functions=equal %s | %FileCheck --check-prefix=LIR %s

function assert(value) {
  if (!value) throw new Error('trim comparison semantics');
}
function equal(value) {
  'noinline';
  return value.trim() === value;
}
function different(value) {
  'noinline';
  return value !== value.trim();
}
function withArgument(value, effect) {
  'noinline';
  return value.trim(effect()) === value;
}
function localCatch(value, expected) {
  'noinline';
  try {
    return value.trim() === value;
  } catch (error) {
    return error === expected;
  }
}
var originalTrim = String.prototype.trim;
var spaces = '\u0009\u000a\u000b\u000c\u000d\u0020\u00a0\u1680' +
    '\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200a' +
    '\u2028\u2029\u202f\u205f\u3000\ufeff';
for (var i = 0; i < spaces.length; ++i) {
  var space = spaces[i];
  assert(!equal(space) && different(space));
  assert(!equal(space + 'x') && !equal('x' + space));
  assert(equal('x' + space + 'x') && !different('x' + space + 'x'));
}
var unchanged = ['', 'abc', '\ud800', '\udc00', '\ud83d\ude00',
    '\u0085', '\u180e', '\u200b', '\u200c'];
for (var i = 0; i < unchanged.length; ++i)
  assert(equal(unchanged[i]) && !different(unchanged[i]));

var effects = 0;
assert(withArgument('x', function () { ++effects; gc(); }));
assert(effects === 1);
String.prototype.trim = function () { ++effects; gc(); return String(this); };
assert(equal(' x ') && effects === 2);
Object.defineProperty(String.prototype, 'trim', {configurable: true, get() {
  ++effects;
  gc();
  return originalTrim;
}});
assert(equal('x') && effects === 3);
assert(!equal(' x ') && effects === 4);
Object.defineProperty(String.prototype, 'trim', {
  value: originalTrim, configurable: true, writable: true,
});

// A wrapper or custom method can observe the call even when its result compares
// equal to its receiver. The primitive guard must leave these calls in place.
var custom = {trim() { ++effects; gc(); return this; }};
assert(equal(custom) && effects === 5);
var wrapper = new String('x');
wrapper.toString = function () { ++effects; gc(); return 'x'; };
assert(!equal(wrapper) && effects === 6);
var proxy = new Proxy(custom, {get(target, key, receiver) {
  ++effects;
  return Reflect.get(target, key, receiver);
}});
assert(equal(proxy) && effects === 8);
var sentinel = {};
var observed;
try { equal({trim() { throw sentinel; }}); } catch (error) { observed = error; }
assert(observed === sentinel);
observed = undefined;
try { equal({get trim() { throw sentinel; }}); } catch (error) { observed = error; }
assert(observed === sentinel);
observed = undefined;
try { equal(null); } catch (error) { observed = error; }
assert(observed instanceof TypeError);
assert(localCatch('x', sentinel));
assert(localCatch({trim() { throw sentinel; }}, sentinel));
observed = undefined;
try { equal({trim: 0}); } catch (error) { observed = error; }
assert(observed instanceof TypeError);

// Compare against the receiver captured before argument evaluation changes
// prototype state. The original looked-up function is still the actual callee.
assert(withArgument('x', function () {
  String.prototype.trim = function () { throw sentinel; };
  gc();
}));
String.prototype.trim = originalTrim;
print('trim comparisons passed');
// CHECK: trim comparisons passed
// LIR-LABEL: function equal(
// LIR: NativeCallInst
// LIR: _sh_ljs_try_string_trim_equal
