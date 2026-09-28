/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -Xsmall-c -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O0 -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -emit-c -o %t.c %s
// RUN: %FileCheck --check-prefix=C --input-file=%t.c %s

function equal(source, index) {
  "noinline";
  var text = '' + source;
  return text[+index] === 'x';
}
function different(source, index) {
  "noinline";
  var text = '' + source;
  return 'x' !== text[+index];
}
function surrogate(source, index) {
  "noinline";
  var text = '' + source;
  return text[+index] === '\uD800';
}
function nul(source, index) {
  "noinline";
  var text = '' + source;
  return text[+index] === '\0';
}
print(equal('xy', 0), equal('xy', -0), equal('xy', 1), different('xy', 0));
// CHECK: true true false false
print(equal('x', 1), equal('', 0), equal('x', -1), equal('x', 0.5),
      equal('x', NaN), equal('x', Infinity), equal('x', 4294967296));
// CHECK-NEXT: false false false false false false false
print(surrogate('\uD800\uDC00', 0), surrogate('\uD800\uDC00', 1),
      surrogate('\uD800', 0), equal('\uD800x', 1), nul('a\0b', 1));
// CHECK-NEXT: true false true true true

var reads = 0;
for (var key of ['1', '-1', '0.5', 'NaN', 'Infinity', '4294967296']) {
  Object.defineProperty(String.prototype, key, {
    configurable: true,
    get() { ++reads; gc(); return 'x'; },
  });
}
print(equal('a', 1), equal('a', -1), equal('a', 0.5), equal('a', NaN),
      equal('a', Infinity), equal('a', 4294967296), reads);
// CHECK-NEXT: true true true true true true 6
print(equal('ab', 1), reads);
// CHECK-NEXT: false 6
for (var key of ['1', '-1', '0.5', 'NaN', 'Infinity', '4294967296'])
  delete String.prototype[key];

Object.defineProperty(String.prototype, '3', {
  configurable: true,
  get() { gc(); throw 23; },
});
try { equal('a', 3); } catch (e) { print(e); }
// CHECK-NEXT: 23
delete String.prototype[3];
gc();
print(equal('x', 0), different('x', 4));
// CHECK-NEXT: true true

String.prototype[2] = new String('x');
print(equal('a', 2), different('a', 2));
// CHECK-NEXT: false true
delete String.prototype[2];

var order = '';
print(equal({toString() { order += 's'; return 'x'; }},
            {valueOf() { order += 'i'; gc(); return 0; }}), order);
// CHECK-NEXT: true si

function arbitrary(source, key) {
  "noinline";
  return source[key] === 'x';
}
print(arbitrary(['x'], 0), arbitrary({get a() { gc(); return 'x'; }}, 'a'));
// CHECK-NEXT: true true
try { arbitrary(null, 0); } catch (e) { print(e.name); }
// CHECK-NEXT: TypeError

function dynamic(source, index) {
  "noinline";
  return source[+index] === 'x';
}
print(dynamic('x', 0), dynamic(['x'], 0), dynamic(new String('x'), 0),
      dynamic(new Uint8Array([120]), 0));
// CHECK-NEXT: true true true false
Array.prototype[0] = 'x';
print(dynamic(new Array(1), 0));
// CHECK-NEXT: true
delete Array.prototype[0];
order = '';
print(dynamic(new Proxy({}, {get(target, key) { order += key; gc(); return 'x'; }}),
              {valueOf() { order += 'i'; return 0; }}), order);
// CHECK-NEXT: true i0
try { dynamic(null, 0); } catch (e) { print(e.name); }
// CHECK-NEXT: TypeError
var finalized = 0;
function caught(source, index) {
  "noinline";
  try { return source[+index] !== 'x'; }
  catch (e) { return e; }
  finally { ++finalized; }
}
print(caught('x', 0), caught({get 0() { gc(); throw 31; }}, 0), finalized);
// CHECK-NEXT: false 31 2
var matches = 0;
for (var i = 0; i < 100; ++i) {
  var source = i % 2 ? 'x' : ['x'];
  if (source[i % 1] === 'x') ++matches;
}
print(matches);
// CHECK-NEXT: 100

// C-DAG: _sh_ljs_string_index_compare(
// C-DAG: _sh_ljs_get_indexed_char_code(


function sharedCharacter(source, key) {
  "noinline";
  var c = source[key];
  return (c === 'x' ? 1 : 0) | (c === 'y' ? 2 : 0) |
         (c === '\uD800' ? 4 : 0) | (c !== 'x' ? 8 : 0) |
         (c === '\0' ? 16 : 0) | (c === '\uFFFF' ? 32 : 0);
}
print(sharedCharacter('xy', 0), sharedCharacter('xy', 1),
      sharedCharacter('\uD800\uDC00', 0), sharedCharacter('\uD800\uDC00', 1),
      sharedCharacter('\0', 0), sharedCharacter('\uFFFF', 0));
// CHECK-NEXT: 1 10 12 8 24 40
print(sharedCharacter('', 0), sharedCharacter('x', -1),
      sharedCharacter('x', 0.5), sharedCharacter('x', NaN),
      sharedCharacter('x', Infinity), sharedCharacter('x', 4294967296));
// CHECK-NEXT: 8 8 8 8 8 8
print(sharedCharacter(['x'], 0), sharedCharacter(['xx'], 0),
      sharedCharacter([new String('x')], 0), sharedCharacter([120], 0),
      sharedCharacter([undefined], 0), sharedCharacter([Symbol('x')], 0));
// CHECK-NEXT: 1 8 8 8 8 8

reads = 0;
var keyed = new Proxy({}, {
  get(target, key) { ++reads; gc(); return reads === 1 ? 'x' : 'y'; },
});
var key = {toString() { order += 'k'; gc(); return '0'; }};
order = '';
print(sharedCharacter(keyed, key), reads, order);
// CHECK-NEXT: 1 1 k
reads = 0;
Object.defineProperty(String.prototype, '-1', {
  configurable: true,
  get() { ++reads; gc(); return 'y'; },
});
print(sharedCharacter('x', -1), reads);
// CHECK-NEXT: 10 1
delete String.prototype['-1'];
var symbol = Symbol('key');
print(sharedCharacter({[symbol]: 'x'}, symbol), sharedCharacter({'2': 'y'}, 2n));
// CHECK-NEXT: 1 10

function readThenMutate(source, key, mutate) {
  "noinline";
  var c = source[key];
  mutate();
  if (c === 'x') return 1;
  return c === 'y' ? 2 : 3;
}
var observed = {0: 'x'};
print(readThenMutate(observed, 0, () => { observed[0] = 'y'; gc(); }), observed[0]);
// CHECK-NEXT: 1 y
order = '';
try {
  readThenMutate({get 0() { order += 'r'; gc(); throw 41; }}, 0,
                () => { order += 'm'; });
} catch (e) { print(e, order); }
// CHECK-NEXT: 41 r
try { sharedCharacter(null, 0); } catch (e) { print(e.name); }
// CHECK-NEXT: TypeError
