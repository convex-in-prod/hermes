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
// RUN: cd %t.bundle && %shermes -O -emit-c -Xemit-c-bundle -Xemit-c-shard-size=65536 -exported-unit=regex_aot -o unit.c.json %s
// RUN: cat %t.bundle/sh_regex_aot_metadata.c | %FileCheck --check-prefix=C %s

// C: int32_t sh_regex_
// C: uint32_t work = 262144u;
// C: backtrack8_
// C: HermesRegexAOTFallback
// C: uint32_t choices[
// C: alternative16_

// Leave room for the parity driver's control-flow wrapper: outlining moves
// instruction runs into helpers but keeps the block labels and branches.

function assert(value) {
  if (!value) throw new Error('Compiled regexp mismatch');
}
function seedMatchState() {
  /(seed)(ed)/.exec('!seeded?');
}
function matchState() {
  return [RegExp.input, RegExp.lastMatch, RegExp.lastParen,
    RegExp.leftContext, RegExp.rightContext, RegExp.$1, RegExp.$2, RegExp.$3];
}
function snapshot(re, input, start) {
  seedMatchState();
  re.lastIndex = start;
  var m = re.exec(input);
  return JSON.stringify([m, m && m.index, m && m.input, m && m.groups,
    m && m.indices, re.lastIndex, matchState()]);
}
function testSnapshot(re, input, start) {
  seedMatchState();
  re.lastIndex = start;
  var matched = re.test(input);
  return JSON.stringify([matched, re.lastIndex, matchState()]);
}
// The constructor consumes a runtime property, so it uses the interpreter.
// Literal matches must agree on priority, captures and state, not only success.
var patterns = [
  /a[0-9]x/, /^a[0-9]x$/, /([a-z]+)([0-9]*)/d,
  /\b([a-z]+)\s*[:=]\s*(\d+)\b/i,
  /(a*)(a+)b/d, /(a*?)(a+?)b/d, /(a{1,3})(a{0,2})b/d,
  /(a{1,3}?)(a{0,2}?)b/d, /((a*)(b*))c/d,
  // Backtracking an earlier loop must rewrite nested and empty later captures.
  /(a*)(()(a*))(a+)b/d, /((a*)(a*))ab/d,
  /(a{0})(a{0,4294967295})/d, /a{4294967295}a/,
  // End-anchored fixed widths have one candidate; sticky still fixes the start.
  /([a-z])$/d, /(a{2})(b{0})$/dgy, /()$/dgy, /$(a)/d,
  /(?m:^)(a)$/dg, /a$/m,
  /a?b?c?/, /()/d, /a*/g, /a*?/y, /(a*)(b*)/dgy,
  /a+b/y, /a+b/g, /(?<key>[a-z]+)=(?<value>[0-9]+)/dg, /^([a-z]+)$/m, /(^a*)/m, /(a*$)/m,
  /\ba+\b/, /\Ba+\B/, /[\d\S][^\w]+/,
  /[\D\s]+/, /[\s]+/, /[^]+/, /[]*/, /(.+)(.*)/s,
  /\u0100+(\ud800*)/d, /[\u0100-\uffff]+/,
  /(?i:a+)(?-i:B+)/, /(?s:.+)(?-s:.)/,
  // Non-Unicode folded literals, literal runs and width-one repetitions use
  // the same equivalence classes for ASCII and UTF-16 subjects.
  /abc/i, /\u00e9cho/i, /\u0101+end/i, /\ud800+end/i,
  /[a-z]+\u00e9[\s\S]*?fin/i,
  /k/i, /s/i, /\u212a/i, /\u017f/i, /\u03c3/i,
  /\u00b5/i, /\u0131/i, /\u00df/i,
  // Ordered alternatives must retry after their shared continuation fails.
  /(?:a|ab)c/d, /(?:ab|a)b/d, /(?:ab|a)/dg, /(?:a|ab)/dy,
  /(?:|ab)c/d, /(?:ab|)c/d, /(?:|ab)/dgy, /(?:ab|)/dgy,
  /(?:a*|ab)c/d, /(?:a*?|ab)c/d, /(?:a{1,3}?|ab)b/d,
  /a*(?:ab|a)b/d, /a*?(?:ab|a)b/d,
  /(?:ab|a)b*(?:bc|b)c/d, /(?:ab|a)b*?(?:bc|b)c/d,
  /(?:(?:ab|a)c|a)b/d, /(?:a$|ab)c/, /(?:^a|b)c/,
  /(?:\ba|b)c/, /(?:a|b\B)c/, /(?:^a|b$)/m,
  /^(?:ab|a)b$/dgy, /(?:\u00e9cho|\u00e9clair)\s*[^]*?fin/ig,
  /(?:\u00e9cho\s*|\u00e9cho[^]*?)fin/iy,
  /(?:\ud800a|\ud800)end/ig,
  /(?:ab|a)(?:ab|a)(?:ab|a)(?:ab|a)(?:ab|a)(?:ab|a)(?:ab|a)(?:ab|a)c/,
  // Unsupported control flow and Unicode stay on the interpreter.
  /(ab|cd)+/, /(a)?b/d, /(?=(a))a/, /(a)\1/,
  /(?<=a)b/, /./u, /\u212a/iu, /\u017f/iu, /k/iu, /s/iu,
  /(?:a|ab)+c/, /(?:a|ab)*?c/
];
var inputs = ['', 'a', 'b', 'ab', 'aaab', 'aaac', 'xaa', 'x\na', 'a1x', 'za7xz',
  'abc: 123', 'ABC = 42', 'no delimiter', ' a: 1! ',
  '\naaa\r\nbbb\u2028a\u2029', 'a\n', 'aaaBBB',
  '\u0100\u0100\ud800\ud800', '\ud800a\udc00', '\u00e9a',
  '\u212ak', '\u017fs', '\u00a0\ufeff\u1680\u2000\u202f\u3000',
  '\u00e9ABC: 42', 'abc', 'abb', 'abbbbc', 'abab', 'acb',
  'ABC', '\u00e9ABC', '\u00c9CHO', '\u00c9CLAIR \nFIN',
  '\u00e9cho FIN', '\u00c9CHO missing suffix',
  '\u0100\u0101END', '\ud800END', '\ud800aEND',
  'kKsS', '\u212a\u017f', '\u03a3\u03c2\u03c3',
  '\u00b5\u039c\u03bc', '\u0131I', '\u00dfSS',
  'aaaaaaaac', 'ababababababababc', 'abababababababab!'];
for (var i = 0; i < patterns.length; ++i) {
  var re = patterns[i];
  var dynamic = new RegExp(re.source, re.flags);
  for (var j = 0; j < inputs.length; ++j) {
    var input = inputs[j];
    for (var start = 0; start <= input.length + 1; ++start) {
      assert(snapshot(re, input, start) === snapshot(dynamic, input, start));
      assert(testSnapshot(re, input, start) ===
        testSnapshot(dynamic, input, start));
    }
  }
}
assert(!/k/i.test('\u212a') && !/s/i.test('\u017f'));
assert(/k/iu.test('\u212a') && /s/iu.test('\u017f'));
assert(/\u03c3/i.test('\u03c2') && /\u00b5/i.test('\u039c'));
assert(!/i/i.test('\u0131') && !/ss/i.test('\u00df'));
// Ambiguous loops exceed the compiled work budget and restart the interpreter.
var expensive = /^(a*)(a*)(a*)b$/d;
var text = new Array(141).join('a');
assert(snapshot(expensive, text, 0) ===
  snapshot(new RegExp(expensive.source, expensive.flags), text, 0));
// The branch stack also restarts safely after excessive continuation retries.
var expensiveAlternative = /^(?:a*a*a*b|a*a*a*c)$/d;
assert(snapshot(expensiveAlternative, text, 0) ===
  snapshot(new RegExp(expensiveAlternative.source, expensiveAlternative.flags),
    text, 0));
// Consuming a long capture exceeds the work budget even though the match
// succeeds. An irrelevant prefix is pruned without spending that budget.
var long = 'a' + new Array(270001).join('1') + 'b';
var longPattern = /a(\d+)b/d;
assert(snapshot(longPattern, long, 0) ===
  snapshot(new RegExp(longPattern.source, longPattern.flags), long, 0));
var longAlternative = /(?:\u00e9\d+|e\d+)b/id;
var longUtf16 = '\u00c9' + new Array(270001).join('1') + 'b';
assert(snapshot(longAlternative, longUtf16, 0) ===
  snapshot(new RegExp(longAlternative.source, longAlternative.flags),
    longUtf16, 0));
// The result-free builtin still publishes generated captures and legacy state.
var tested = /(?<left>a*)(a+)b/dg;
assert(tested.test('xaaab') && tested.lastIndex === 5);
assert(RegExp.$1 === 'aa' && RegExp.$2 === 'a' && RegExp.lastMatch === 'aaab');
assert(!tested.test('xaaab') && tested.lastIndex === 0 && RegExp.$1 === 'aa');
// Retained literals survive GC; copies have independent lastIndex and bytecode.
var retained = /([a-z]+)=(\d+)/dg;
var copy = new RegExp(retained);
gc();
assert(snapshot(retained, 'key=12', 0) === snapshot(copy, 'key=12', 0));
assert(retained !== copy);
print('compiled regexp parity');
// CHECK: compiled regexp parity
