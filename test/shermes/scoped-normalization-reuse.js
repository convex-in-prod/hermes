/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -fscoped-normalization-reuse -fstatic-object-prototype -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fscoped-normalization-reuse -fstatic-object-prototype -Xsmall-c -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O0 -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fscoped-normalization-reuse -fstatic-object-prototype -dump-ir -Xdump-functions=normalize,impure %s | %FileCheck --check-prefix=IR %s

(function () {
'use strict';
Object.preventExtensions(Object.prototype);
var separator = ':';
function validate(value) {
  if (value.length === 0) throw new Error('empty');
  if (value.trim() !== value) throw new Error('untrimmed');
  if (value.includes(separator)) throw new Error('separator');
  return value;
}
function normalize(record) {
  'noinline';
  return [validate(record.left), validate(record.right)].join(separator);
}
function repeat(record, change) {
  'noinline';
  var output = '';
  for (var i = 0; i < 3; ++i) {
    change(i, record);
    output += normalize(record) + '|';
  }
  return output;
}
function unchanged() {}
print(repeat({left: 'a', right: 'b'}, unchanged));
// CHECK: a:b|a:b|a:b|
print(repeat({left: 'a', right: 'b'}, function (i, record) {
  if (i === 1) record.left = 'c';
}));
// CHECK: a:b|c:b|c:b|
print(repeat({left: 'a', right: 'b'}, function (i) {
  separator = i === 1 ? '/' : ':';
}));
// CHECK: a:b|a/b|a:b|
print(repeat({left: 'a', right: 'b'}, function (i) {
  separator = i === 1 ? undefined : ':';
}));
// CHECK: a:b|a,b|a:b|
print(repeat({left: 'a', right: 'b'}, function () { gc(); }));
// CHECK: a:b|a:b|a:b|

var reads = '';
var getters = {get left() { reads += 'l'; return 'a'; },
               get right() { reads += 'r'; return 'b'; }};
print(repeat(getters, unchanged), reads);
// CHECK: a:b|a:b|a:b| lrlrlr
reads = '';
var proxy = new Proxy({left: 'a', right: 'b'}, {
  get: function (object, key) { reads += key === 'left' ? 'l' : 'r'; return object[key]; }
});
print(repeat(proxy, unchanged), reads);
// CHECK: a:b|a:b|a:b| lrlrlr
var changedReads = 0;
print(repeat({left: 'a', right: 'b'}, function (i, record) {
  if (i === 1) Object.defineProperty(record, 'right', {
    get: function () { ++changedReads; return 'c'; }
  });
}), changedReads);
// CHECK: a:b|a:c|a:c| 2

var originalTrim = String.prototype.trim;
var originalIncludes = String.prototype.includes;
var originalJoin = Array.prototype.join;
var methodReads = 0;
Object.defineProperty(String.prototype, 'trim', {
  configurable: true,
  get: function () { ++methodReads; return originalTrim; }
});
print(repeat({left: 'a', right: 'b'}, unchanged), methodReads);
// CHECK: a:b|a:b|a:b| 6
Object.defineProperty(String.prototype, 'trim', {
  configurable: true, writable: true, value: originalTrim
});
String.prototype.includes = String.prototype.startsWith;
print(repeat({left: 'a:b', right: 'c'}, unchanged));
// CHECK: a:b:c|a:b:c|a:b:c|
String.prototype.includes = originalIncludes;
var joinCalls = 0;
Array.prototype.join = function () { return 'custom' + (++joinCalls); };
print(repeat({left: 'a', right: 'b'}, unchanged));
// CHECK: custom1|custom2|custom3|
Array.prototype.join = originalJoin;

var rightReads = 0;
var invalid = {left: '', get right() { ++rightReads; return 'b'; }};
for (var attempt = 0; attempt < 2; ++attempt) {
  try { repeat(invalid, unchanged); } catch (error) { print(error.message, rightReads); }
}
// CHECK: empty 0
// CHECK: empty 0
try {
  repeat({left: 'a', right: 'b'}, function (i, record) {
    if (i === 1) record.left = '';
  });
} catch (error) { print(error.message); }
// CHECK: empty
print(repeat({left: 'a', right: 'b'}, function (i, record) {
  if (i === 1) {
    String.prototype.trim = function () { record.right = 'd'; return String(this); };
  } else if (i === 2) String.prototype.trim = originalTrim;
}));
// CHECK: a:b|a:d|a:d|
print(repeat({left: 'a', right: 'b'}, unchanged));
// CHECK: a:b|a:b|a:b|
var effects = 0;
function impure(record) {
  'noinline';
  ++effects;
  return [validate(record.left), validate(record.right)].join(separator);
}
function repeatImpure(record) {
  'noinline';
  var result = '';
  for (var i = 0; i < 3; ++i) result += impure(record) + '|';
  return result;
}
print(repeatImpure({left: 'a', right: 'b'}), effects);
// CHECK: a:b|a:b|a:b| 3
globalThis.scopeProbeFirst = function (record) { return normalize(record); };
globalThis.scopeProbeSecond = function (record) { return normalize(record); };
globalThis.scopeJoinFactory = function (joiner) {
  return function normalizeWithJoiner(record) {
    'noinline';
    return [record.left.trim(), record.right.trim()].join(joiner);
  };
};
})();

var coercible = {
  trim: function () { return this; },
  toString: function () { return 'coerced'; }
};
print(scopeJoinFactory(':')({left: coercible, right: 'b'}));
// CHECK: coerced:b
print(scopeJoinFactory(undefined)({left: 'a', right: 'b'}));
// CHECK: a,b
var separatorConversions = 0;
print(scopeJoinFactory({toString: function () {
  ++separatorConversions;
  return '/';
}})({left: 'a', right: 'b'}), separatorConversions);
// CHECK: a/b 1
print(scopeJoinFactory('🌍')({left: ' α ', right: ' β '}));
// CHECK: α🌍β

var intrinsicJoin = Array.prototype.join;
var joinGets = 0;
Object.defineProperty(Array.prototype, 'join', {
  configurable: true,
  get: function () {
    ++joinGets;
    this[0] = 'changed';
    this.length = 1;
    gc();
    return intrinsicJoin;
  }
});
print(scopeJoinFactory(':')({left: 'a', right: 'b'}), joinGets);
// CHECK: changed 1
Object.defineProperty(Array.prototype, 'join', {
  configurable: true, writable: true, value: intrinsicJoin
});

// IR-LABEL: function normalize(
// IR: _sh_ljs_has_normalization_builtins
// IR: _sh_ljs_has_array_join
// IR: StringConcatInst
// IR: _sh_ljs_is_plain_record
// IR-LABEL: function impure(
// IR-NOT: _sh_ljs_has_normalization_builtins
// IR: function_end
