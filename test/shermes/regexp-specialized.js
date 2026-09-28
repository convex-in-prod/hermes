/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -emit-c -o %t.c %s
// RUN: %FileCheck --check-prefix=C --input-file=%t.c %s
// RUN: rm -rf %t.bundle && mkdir %t.bundle
// RUN: cd %t.bundle && %shermes -O -emit-c -Xemit-c-bundle -Xemit-c-shard-size=4096 -exported-unit=regex_test -o unit.c.json %s
// RUN: cat %t.bundle/sh_regex_test_functions_*.c | %FileCheck --check-prefix=C %s

// Check calls, not declarations: the runtime checks below must exercise both
// specialized literals and interpreter fallback in emitted C and bundle shards.
// C-DAG: = _sh_ljs_create_regexp_specialized(shr,
// C-DAG: = _sh_ljs_create_regexp_precompiled(shr,

function assert(value) {
  if (!value) throw new Error('RegExp regression');
}
function throwsTypeError(fn) {
  var threw = false;
  try { fn(); } catch (e) { threw = e instanceof TypeError; }
  assert(threw);
}
var test = RegExp.prototype.test;
var exec = RegExp.prototype.exec;
var trace = '';
var re = /a/g;
Object.defineProperty(re, 'exec', {configurable: true, get() {
  trace += 'e';
  /(nested)/.test('nested');
  gc();
  Object.setPrototypeOf(re, null);
  return exec;
}});
assert(test.call(re, {toString() {
  trace += 's';
  gc();
  return 'a';
}}));
assert(trace === 'se' && re.lastIndex === 1 && RegExp.lastMatch === 'a');
assert(RegExp.$1 === '');

// Coercion can replace exec before it is resolved. Custom results must still
// be objects or null, and test must not inspect properties of a returned object.
re = /a/;
assert(!test.call(re, {toString() {
  re.exec = function(s) { assert(this === re && s === 'a'); return null; };
  return 'a';
}}));
var ordinary = {exec(s) { assert(this === ordinary && s === '7'); return {}; }};
assert(test.call(ordinary, 7));
ordinary.exec = function() { return new Proxy({}, {get() { throw 1; }}); };
assert(test.call(ordinary, 'a'));
for (var value of [undefined, false, 1, 'a', Symbol('result')]) {
  ordinary.exec = function() { return value; };
  throwsTypeError(() => test.call(ordinary, 'a'));
}
var bound = /b/g;
re.exec = exec.bind(bound);
assert(test.call(re, 'b') && bound.lastIndex === 1);
var calls = 0;
re.exec = new Proxy(exec, {apply(target, receiver, args) {
  ++calls;
  assert(receiver === re);
  return Reflect.apply(target, receiver, args);
}});
assert(re.test('a') && calls === 1);
re.exec = 0;
assert(re.test('a'));
throwsTypeError(() => test.call({exec: 0}, 'a'));
throwsTypeError(() => test.call(new Proxy(/a/, {}), 'a'));
throwsTypeError(() => test.call(null, {toString() { throw 1; }}));
throwsTypeError(() => /a/.test(Symbol('input')));
assert(/7/.test(7n));
var saved = /a/;
saved.exec = exec;
RegExp.prototype.exec = function() { throw 1; };
assert(saved.test('a'));
RegExp.prototype.exec = exec;

re = /[a-z]/gy;
re.lastIndex = {valueOf() { gc(); return 1; }};
assert(re.test('!a') && re.lastIndex === 2);
assert(!re.test('!a') && re.lastIndex === 0);
for (var index of [-1, NaN, Infinity, 1e100]) {
  re.lastIndex = index;
  assert(re.test('a') === (index === -1 || Number.isNaN(index)));
}
re.lastIndex = 1n;
throwsTypeError(() => re.test('a'));
var plain = /a/;
plain.lastIndex = {valueOf() { throw new TypeError(); }};
throwsTypeError(() => plain.test('a'));
var fixed = /(?<letter>a)(b)?/dg;
Object.defineProperty(fixed, 'lastIndex', {writable: false});
throwsTypeError(() => fixed.test('xa!'));
assert(RegExp.input === 'xa!' && RegExp.$1 === 'a' && RegExp.$2 === '');
assert(RegExp.leftContext === 'x' && RegExp.rightContext === '!');
throwsTypeError(() => fixed.test('x'));
assert(RegExp.input === 'xa!');
var capture = /(?=(a))(a)(b)?/d;
assert(capture.test('a') && RegExp.$1 === 'a' && RegExp.$3 === '');
var result = capture.exec('a');
assert(result.length === 4 && result[3] === undefined);
assert(result.indices[2][0] === 0 && result.indices[2][1] === 1);
assert(result.indices[3] === undefined && result.groups === undefined);

function literal() { 'noinline'; return /a[0-9]./dgy; }
var first = literal(), second = literal();
assert(first !== second && first.test('a1!') && second.lastIndex === 0);
result = second.exec('a1!');
assert(result.index === 0 && result.indices[0][1] === 3);
assert(!/^a.$/.test('a\n') && /^a.$/s.test('a\n'));
assert(/(?s:a.b)/.test('a\nb') && !/(?-s:a.b)/s.test('a\nb'));
assert(/^a/m.test('x\na') && /(?i:a)/.test('A'));
assert(/[\d\S][^\w]/.test('a!') && !/[]/.test('a'));
assert(/\u00e9/.test('\u00e9') && /\ud800/.test('\ud800\udc00'));
assert(/\s/.test('\uFEFF') && !/./.test('\u2028'));
var unicode = /./uy;
unicode.lastIndex = 1;
assert(unicode.test('\ud800\udc00') && unicode.lastIndex === 2);
var empty = /(?:)/g;
assert(empty.test('') && empty.lastIndex === 0);
assert('xa1!'.match(/a[0-9]./)[0] === 'a1!');
assert('xa1!'.search(/a[0-9]./) === 1);
assert('xa1!'.replace(/a[0-9]./, '$&?') === 'xa1!?');
assert('xay'.split(/a/).join(',') === 'x,y');

// MatchAll uses the same-flags native copy route. Drop both an owned source
// and a borrowed literal source before collecting and consuming the clones.
function iterators() {
  var owned = new RegExp('(?<letter>a)', 'dg');
  var borrowed = /a/dg;
  return [owned[Symbol.matchAll]('xa'), borrowed[Symbol.matchAll]('xa')];
}
var iter = iterators();
gc();
result = iter[0].next().value;
assert(result.groups.letter === 'a' && result.indices.groups.letter[0] === 1);
assert(iter[1].next().value.indices[0][0] === 1);
assert(iter[0].next().done && iter[1].next().done);
first = second = re = saved = capture = fixed = bound = null;
iter = null;
/z/.test('z');
gc();
print('RegExp specialized paths passed');
// CHECK: RegExp specialized paths passed
