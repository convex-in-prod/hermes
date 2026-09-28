/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -O %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -exec %s | %FileCheck --match-full-lines %s

function assert(value) {
  if (!value) throw new Error('RegExp string operation mismatch');
}
var builtinExec = RegExp.prototype.exec;
function replacementSnapshot(re, input) {
  /sentinel(x)/.test('sentinelx');
  var value = input.replace(re, '');
  return JSON.stringify([value, re.lastIndex, RegExp.lastMatch,
    RegExp.$1, RegExp.$2, RegExp.leftContext, RegExp.rightContext]);
}
function compare(re, input) {
  var generic = new RegExp(re.source, re.flags);
  // Force the observable object-result path; dynamic regex compilation alone
  // does not disable the builtin empty-replacement optimization.
  generic.exec = function(text) { return builtinExec.call(this, text); };
  assert(replacementSnapshot(re, input) === replacementSnapshot(generic, input));
}

var cases = [
  /[^a-z\u0100-\u017f0-9]/gi, /[\u03a3-\u03c9]/gi,
  /[^\w\u0100-\u017f]/gi, /[\W\u0100-\u017f]/gi,
  /([a-z]+)(\d*)/g, /(?<word>[a-z]+)/dg, /a*/g,
  /()/gu, /./gu, /./g, /a/gy, /a*/gy, /z/g, /a/
];
var inputs = ['', 'abc 123! a', '\u0100\u0101\u017f\u212a',
  '\u03a3\u03c2\u03c3\u03c9', '\ud800a\udc00\ud83d\ude00', 'bbb'];
for (var i = 0; i < cases.length; i++)
  for (var j = 0; j < inputs.length; j++) compare(cases[i], inputs[j]);

// Exercise the full code-unit domain, including case equivalents outside the
// written range and lone surrogates. Compare generated and interpreted exec.
var units = '';
for (var i = 0; i < 65536; i++) units += String.fromCharCode(i);
for (var i = 0; i < 4; i++) compare(cases[i], units);

var log = [];
var accessor = /a/g;
Object.defineProperty(accessor, 'exec', { get: function() {
  log.push(this.lastIndex);
  return builtinExec;
}});
assert('aba'.replace(accessor, '') === 'b');
assert(JSON.stringify(log) === '[0,1,3]');
var overridden = /a/g;
overridden.exec = function() { log.push('custom'); return null; };
assert('aba'.replace(overridden, '') === 'aba');
assert(log[log.length - 1] === 'custom');

var alteredFlags = /a/;
Object.defineProperty(alteredFlags, 'global', {value: true});
alteredFlags.exec = function() { return null; };
assert('a'.replace(alteredFlags, '') === 'a');
var nonGlobal = /a/g;
Object.defineProperty(nonGlobal, 'global', {value: false});
assert('aba'.replace(nonGlobal, '') === 'ba' && nonGlobal.lastIndex === 1);

var readonly = /a/g;
Object.defineProperty(readonly, 'lastIndex', {writable: false});
var threw = false;
try { 'a'.replace(readonly, ''); } catch (e) { threw = e instanceof TypeError; }
assert(threw);

// Coercion may install an accessor or change exec before its first lookup.
var coerced = /a/g;
assert('a'.replace(coerced, {toString: function() {
  coerced.exec = function() { return null; };
  return '';
}}) === 'a');
var test = /[a-z]/;
assert(test.test({toString: function() {
  test.exec = function() { return null; };
  return 'a';
}}) === false);
var getterTest = /[a-z]/;
var gets = 0;
Object.defineProperty(getterTest, 'exec', {get: function() {
  gets++;
  return builtinExec;
}});
assert(getterTest.test('a') && gets === 1);

// A sibling can move the property map away from a flags-only hidden-class
// transition. Its property count does not identify the changed exec slot.
var builtinTest = RegExp.prototype.test;
var builtinReplace = RegExp.prototype[Symbol.replace];
for (var accessorExec of [false, true]) {
  var execCalls = 0;
  var execGets = 0;
  function customExec() { execCalls++; return null; }
  function prototypeWithExec() {
    var proto = Object.create(RegExp.prototype);
    if (accessorExec) {
      Object.defineProperty(proto, 'exec', {
        configurable: true, enumerable: true,
        get: function() { execGets++; return customExec; }
      });
    } else {
      proto.exec = customExec;
    }
    proto.decoy = builtinExec;
    Object.defineProperty(proto, 'exec', {enumerable: false});
    return proto;
  }
  for (var replacement of [false, true]) {
    var guarded = /a/g;
    // Keep the replace prelude's public property reads on the receiver, so they
    // do not materialize the prototype's missing property map before the guard.
    Object.defineProperty(guarded, 'global', {value: true});
    Object.defineProperty(guarded, 'unicode', {value: false});
    Object.setPrototypeOf(guarded, prototypeWithExec());
    var sibling = prototypeWithExec();
    sibling['extra' + accessorExec + replacement] = 1;
    if (replacement)
      assert(builtinReplace.call(guarded, 'a', '') === 'a');
    else
      assert(!builtinTest.call(guarded, 'a'));
  }
  assert(execCalls === 2 && execGets === (accessorExec ? 2 : 0));
}
print('regexp string operations passed');
// CHECK: regexp string operations passed
