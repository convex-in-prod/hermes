// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// RUN: %shermes -exec -O %s | %FileCheck %s
// RUN: %shermes -exec -O0 %s | %FileCheck %s

var cases = [
  ['\u0418\u0412\u0410\u041d', '\u0438\u0432\u0430\u043d', '\u0418\u0412\u0410\u041d'],
  ['Stra\u00dfe', 'stra\u00dfe', 'STRASSE'],
  ['\u0130', 'i\u0307', '\u0130'],
  ['\ufb03', '\ufb03', 'FFI'],
  ['\u039f\u03a3', '\u03bf\u03c2', '\u039f\u03a3'],
  ['\u03a3', '\u03c3', '\u03a3'],
  ['A\u0301\u03a3\u0301', 'a\u0301\u03c2\u0301', 'A\u0301\u03a3\u0301'],
  ['A\u03a3\u0301B', 'a\u03c3\u0301b', 'A\u03a3\u0301B'],
  ['A \u03a3', 'a \u03c3', 'A \u03a3'],
  ["A'.:^`\u03a3", "a'.:^`\u03c2", "A'.:^`\u03a3"],
  ["A\u03a3'.:^`B", "a\u03c3'.:^`b", "A\u03a3'.:^`B"],
  ['\u0345\u03a3', '\u0345\u03c3', '\u0399\u03a3'],
  ['A\u03a3\u0345', 'a\u03c2\u0345', 'A\u03a3\u0399'],
  ['\uD801\uDC00\uD801\uDC28', '\uD801\uDC28\uD801\uDC28', '\uD801\uDC00\uD801\uDC00'],
  ['\uD800a\uDC00', '\uD800a\uDC00', '\uD800A\uDC00'],
  ['', '', ''],
  ['\u00df'.repeat(256), '\u00df'.repeat(256), 'S'.repeat(512)],
];
for (var i = 0; i < cases.length; ++i) {
  if (cases[i][0].toLowerCase() !== cases[i][1] ||
      cases[i][0].toUpperCase() !== cases[i][2]) {
    throw new Error('Case conversion failed at case ' + i);
  }
}
print('case mappings passed');
// CHECK: case mappings passed
