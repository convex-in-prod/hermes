/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -exec %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -emit-c -o %t.c %s
// RUN: %FileCheck --check-prefix=C --input-file=%t.c %s
// Require every operation kind; parity alone would also pass if lowering
// silently stopped recognizing one of these expression shapes.
// C-DAG: = _sh_ljs_try_fast_regexp(shr, regexp_term, 0,
// C-DAG: = _sh_ljs_try_fast_regexp(shr, regexp_term, 1,
// C-DAG: = _sh_ljs_try_fast_regexp(shr, regexp_term, 2,
// C-DAG: = _sh_ljs_try_fast_regexp(shr, regexp_term, 3,
// C-DAG: = _sh_ljs_try_fast_regexp(shr, regexp_term, 4,
// C-DAG: = _sh_ljs_try_fast_regexp(shr, regexp_term, 5,

function assert(value) {
  if (!value) throw new Error('RegExp fusion mismatch');
}
function state(value) {
  return JSON.stringify([value, RegExp.input, RegExp.lastMatch, RegExp.$1,
    RegExp.$2, RegExp.lastParen, RegExp.leftContext, RegExp.rightContext]);
}
function seed() { new RegExp('(seed)(!)').exec('a seed! z'); }
function test(value) { return /[a-z\u0100-\u017f0-9]/i.test(value); }
function indexed(value, index) { return /[a-z\u0100-\u017f0-9]/i.test(value[index]); }
function indexedBefore(value, index) {
  var character = value[index];
  return /[a-z\u0100-\u017f0-9]/i.test(character);
}
function filter(value) { return value.replace(/[^a-z\u0100-\u017f0-9]/gi, ''); }
function lowerFilter(value) {
  return value.toLowerCase().replace(/[^a-z\u0100-\u017f0-9]/gi, '');
}
function equal(value) { return value === value.replace(/[^a-z\u0100-\u017f0-9]/gi, ''); }
function lowerEqual(value) {
  return value === value.toLowerCase().replace(/[^a-z\u0100-\u017f0-9]/gi, '');
}
var classSource = /[a-z\u0100-\u017f0-9]/.source;
var filterSource = /[^a-z\u0100-\u017f0-9]/.source;
function genericTest(value) { return new RegExp(classSource, 'i').test(value); }
function genericIndexed(value, index) { return genericTest(value[index]); }
function genericFilter(value) { return value.replace(new RegExp(filterSource, 'gi'), ''); }
function genericLowerFilter(value) { return genericFilter(value.toLowerCase()); }
function genericEqual(value) { return value === genericFilter(value); }
function genericLowerEqual(value) { return value === genericLowerFilter(value); }
function compare(fast, slow, input, index) {
  seed();
  var expected = state(slow(input, index));
  seed();
  assert(state(fast(input, index)) === expected);
}
var inputs = ['', 'az19', 'a!b?c!', 'ABC!', '\u0101 \u0105',
  '\u0100\u0130', '\u039f\u03a3', '\u03bf\u03c3',
  '\ud801\udc00', '\ud800a\udc00', '\u0000!', 'a!B', 'a!\u0130'];
for (var input of inputs) {
  compare(test, genericTest, input);
  compare(filter, genericFilter, input);
  compare(lowerFilter, genericLowerFilter, input);
  compare(equal, genericEqual, input);
  compare(lowerEqual, genericLowerEqual, input);
  for (var index of [-1, -0, 0, 1, 2, 30, 0.5, NaN, Infinity, '1', 1n]) {
    compare(indexed, genericIndexed, input, index);
    compare(indexedBefore, genericIndexed, input, index);
  }
}

// Single-unit lazy input survives collection, does not keep the original
// string as its legacy input, and is replaced by the next ordinary match.
assert(indexed('x\u0101z', 1));
gc();
assert(RegExp.input === '\u0101' && RegExp.lastMatch === '\u0101');
assert(RegExp.$1 === '' && RegExp.leftContext === '' && RegExp.rightContext === '');
new RegExp('(next)').test('a next z');
assert(RegExp.input === 'a next z' && RegExp.$1 === 'next');

// Failed operations retain prior captures, including a failed indexed test.
seed();
var previous = state(false);
assert(!indexed('!', 0));
assert(state(false) === previous);
assert(filter('abc') === 'abc');
assert(state(false) === previous);

// Out-of-range access must still consult prototypes and then ToString.
Object.defineProperty(String.prototype, '-1', {
  configurable: true, get() { return 'a'; }
});
compare(indexed, genericIndexed, 'z', -1);
delete String.prototype['-1'];
compare(test, genericTest, {toString() { return 'a'; }});
compare(indexed, genericIndexed, {0: 'a'}, 0);

var builtinTest = RegExp.prototype.test;
var builtinExec = RegExp.prototype.exec;
RegExp.prototype.test = function(value) { return ['custom test', this.source, value]; };
compare(test, genericTest, 'a');
compare(indexed, genericIndexed, 'z', 0);
RegExp.prototype.test = builtinTest;
RegExp.prototype.exec = function(value) { return null; };
compare(test, genericTest, 'a');
compare(filter, genericFilter, 'a!');
RegExp.prototype.exec = builtinExec;

// Lookup order matters even when a getter returns the original builtin.
var trace = '';
Object.defineProperty(RegExp.prototype, 'test', {
  configurable: true,
  get() { trace += 't'; return builtinTest; }
});
function effectful() {
  return /[a-z]/.test({toString() { trace += 's'; return 'a'; }});
}
assert(effectful() && trace === 'ts');
Object.defineProperty(RegExp.prototype, 'test', {
  configurable: true, writable: true, value: builtinTest
});

var globalDescriptor = Object.getOwnPropertyDescriptor(RegExp.prototype, 'global');
Object.defineProperty(RegExp.prototype, 'global', {
  configurable: true, get() { return false; }
});
compare(filter, genericFilter, 'a!b?');
Object.defineProperty(RegExp.prototype, 'global', globalDescriptor);
var unicodeDescriptor = Object.getOwnPropertyDescriptor(RegExp.prototype, 'unicode');
var reads = 0;
Object.defineProperty(RegExp.prototype, 'unicode', {
  configurable: true, get() { ++reads; return false; }
});
filter('a!');
assert(reads === 1);
Object.defineProperty(RegExp.prototype, 'unicode', unicodeDescriptor);

var builtinReplace = String.prototype.replace;
Object.defineProperty(String.prototype, 'replace', {
  configurable: true, get() { new RegExp('(getter)').test('getter'); return builtinReplace; }
});
compare(filter, genericFilter, 'abc');
Object.defineProperty(String.prototype, 'replace', {
  configurable: true, writable: true, value: builtinReplace
});
var builtinLower = String.prototype.toLowerCase;
String.prototype.toLowerCase = function() { return 'changed!'; };
compare(lowerFilter, genericLowerFilter, 'abc');
compare(lowerEqual, genericLowerEqual, 'abc');
String.prototype.toLowerCase = builtinLower;
var symbolReplace = RegExp.prototype[Symbol.replace];
RegExp.prototype[Symbol.replace] = function(value) { return 'symbol replacement'; };
compare(filter, genericFilter, 'abc');
RegExp.prototype[Symbol.replace] = symbolReplace;

// Sticky matching, captures, anchors and Unicode classes stay on the original
// path. In particular, the class scanner must not search beyond sticky start.
function sticky(value) { return /[a-z]/y.test(value); }
assert(!sticky('!a') && sticky('a!'));
function captured(value) { return /([a-z])/.test(value); }
assert(captured('!a') && RegExp.$1 === 'a');
function unicode(value) { return /\w/iu.test(value); }
assert(unicode('\u212a'));

print('RegExp fusion passed');
// CHECK: RegExp fusion passed
