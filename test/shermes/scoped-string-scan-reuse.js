/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -fscoped-normalization-reuse -fstatic-object-prototype -verify-ir -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O0 -exec %s | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fscoped-normalization-reuse -fstatic-object-prototype -dump-ir -Xdump-functions=scanOwned,scanImpure %s | %FileCheck --check-prefix=IR %s

'use strict';
Object.preventExtensions(Object.prototype);
function scanOwned(text) {
  'noinline';
  var result = [];
  var start = 0;
  for (var i = 0; i <= text.length; ++i) {
    if (i === text.length || text[i] === ':') {
      var field = text.slice(start, i).trim();
      if (field.length === 0) throw new Error('empty field');
      if (field === '*') result.push({options: [field]});
      else result.push(field);
      start = i + 1;
    }
  }
  return result;
}
function scanNegative(text) {
  'noinline';
  var result = [];
  for (var i = -1; i < 1; ++i) {
    var value = text[i];
    result.push(value === undefined ? text.slice(0, 1).trim() : value);
  }
  return result;
}
var scanEffects = 0;
function scanImpure(text) {
  'noinline';
  var result = [];
  for (var i = 0; i < text.length; ++i) {
    ++scanEffects;
    result.push(text.slice(i, i + 1).trim());
  }
  return result;
}
print(JSON.stringify(scanOwned(' a : b ')), JSON.stringify(scanNegative('a')));
// CHECK: ["a","b"] ["a","a"]
print(JSON.stringify(scanOwned('a:*')), JSON.stringify(scanImpure('ab')), scanEffects);
// CHECK: ["a",{"options":["*"]}] ["a","b"] 2

// Called by the embedding test inside a reuse scope, after it has verified
// actual cache hits, distinct result identities and moving-GC survival.
globalThis.runScanMutationTests = function () {
  function require(condition) {
    if (!condition) throw new Error('String scan observation changed');
  }
  var first = scanOwned('a:b');
  first[0] = 'changed';
  first.extra = true;
  require(JSON.stringify(scanOwned('a:b')) === '["a","b"]');

  var slice = String.prototype.slice;
  var trim = String.prototype.trim;
  var push = Array.prototype.push;
  var calls = 0;
  String.prototype.slice = function (start, end) {
    ++calls;
    return slice.call(this, start, end);
  };
  require(JSON.stringify(scanOwned('a:b')) === '["a","b"]');
  require(JSON.stringify(scanOwned('a:b')) === '["a","b"]');
  require(calls === 4);
  String.prototype.slice = slice;

  calls = 0;
  Object.defineProperty(String.prototype, 'trim', {
    configurable: true,
    get: function () { ++calls; return trim; }
  });
  scanOwned('a:b');
  scanOwned('a:b');
  require(calls === 4);
  Object.defineProperty(String.prototype, 'trim', {
    configurable: true, writable: true, value: trim
  });
  calls = 0;
  Array.prototype.push = function (value) {
    ++calls;
    return push.call(this, value);
  };
  scanOwned('a:b');
  scanOwned('a:b');
  require(calls === 4);
  Array.prototype.push = push;

  calls = 0;
  Object.defineProperty(Array.prototype, '0', {
    configurable: true,
    set: function (value) {
      ++calls;
      Object.defineProperty(this, '0', {
        configurable: true, writable: true, enumerable: true, value: value
      });
    }
  });
  require(JSON.stringify(scanOwned('a:b')) === '["a","b"]');
  require(JSON.stringify(scanOwned('a:b')) === '["a","b"]');
  require(calls === 2);
  delete Array.prototype[0];
  calls = 0;
  Object.defineProperty(String.prototype, '-1', {
    configurable: true,
    get: function () { ++calls; return 'inherited'; }
  });
  require(JSON.stringify(scanNegative('a')) === '["inherited","a"]');
  require(JSON.stringify(scanNegative('a')) === '["inherited","a"]');
  require(calls === 2);
  delete String.prototype['-1'];

  require(JSON.stringify(scanOwned(new String('a:b'))) === '["a","b"]');
  calls = 0;
  var input = {0: 'a', 1: ':', 2: 'b',
    get length() { ++calls; return 3; },
    slice: function (start, end) { return slice.call('a:b', start, end); }
  };
  require(JSON.stringify(scanOwned(input)) === '["a","b"]');
  require(calls > 0);
  var threw = 0;
  for (var i = 0; i < 2; ++i) {
    try { scanOwned('a:'); } catch (error) {
      require(error.message === 'empty field');
      ++threw;
    }
  }
  require(threw === 2);
  return true;
};

// IR-LABEL: function scanOwned(
// IR: _sh_ljs_has_scan_reuse_builtins
// IR: _sh_ljs_scan_reuse_lookup
// IR: _sh_ljs_scan_reuse_store
// IR: function_end
// IR-LABEL: function scanImpure(
// IR-NOT: _sh_ljs_has_scan_reuse_builtins
// IR: function_end
