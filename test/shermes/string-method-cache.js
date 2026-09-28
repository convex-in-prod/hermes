// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// RUN: %shermes -exec -O %s | %FileCheck %s
// RUN: %shermes -exec -O0 %s | %FileCheck %s

function method(value) { return value.probe(); }
function check(actual, expected) {
  if (actual !== expected) throw new Error('String method cache changed behavior');
}
String.prototype.probe = function() { 'use strict'; return this; };
for (var i = 0; i < 100; ++i) check(method('first'), 'first');
String.prototype.probe = function() { return 'replaced'; };
check(method('first'), 'replaced');
Object.defineProperty(String.prototype, 'probe', {
  configurable: true,
  get: function() {
    'use strict';
    check(typeof this, 'string');
    return function() { 'use strict'; return this; };
  },
});
check(method('getter'), 'getter');
delete String.prototype.probe;
Object.prototype.probe = function() { return 'inherited'; };
check(method('first'), 'inherited');
String.prototype.probe = function() { return 'own'; };
check(method('first'), 'own');
check(method({ probe: function() { return 'object'; } }), 'object');
check(method('first'), 'own');
delete String.prototype.probe;
delete Object.prototype.probe;
print('string method cache passed');
// CHECK: string method cache passed
