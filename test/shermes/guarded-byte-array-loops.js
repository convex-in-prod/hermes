/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %shermes -O -fstatic-math-builtins -fguarded-byte-array-loops -verify-ir -exec %s -Wx,-Xhermes-internal-test-methods | %FileCheck --match-full-lines %s
// RUN: %shermes -O -Xsmall-c -fstatic-math-builtins -fguarded-byte-array-loops -verify-ir -exec %s -Wx,-Xhermes-internal-test-methods | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fstatic-math-builtins -exec %s -Wx,-Xhermes-internal-test-methods | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fstatic-math-builtins -fstatic-object-prototype -fguarded-byte-array-loops -fnative-integer-registers -verify-ir -exec %s -Wx,-Xhermes-internal-test-methods | %FileCheck --match-full-lines %s
// RUN: %shermes -O -fstatic-math-builtins -fguarded-byte-array-loops -verify-ir -dump-ir -Xdump-functions=checksum %s | %FileCheck --check-prefix=FAST %s
// RUN: %shermes -O -fstatic-math-builtins -fguarded-byte-array-loops -verify-ir -dump-ir -Xdump-functions=withCall,withStore,withProperty,withCoercion,withHandler %s | %FileCheck --check-prefix=SLOW %s
// RUN: %shermes -O -fstatic-math-builtins -dump-ir -Xdump-functions=checksum %s | %FileCheck --check-prefix=OFF %s

function check(condition) {
  if (!condition) throw new Error('byte loop contract');
}

function checksum(bytes, start, limit) {
  'noinline';
  start = +start;
  limit = +limit;
  var result = 0;
  for (var i = start; i < limit; i += 4) {
    var word = bytes[i] | (bytes[i + 1] << 8) |
        (bytes[i + 2] << 16) | (bytes[i + 3] << 24);
    result = Math.imul(result ^ word, 16777619) >>> 0;
  }
  return [result, i];
}

function readWord(bytes, i) {
  'noinline';
  return bytes[i] | (bytes[i + 1] << 8) |
      (bytes[i + 2] << 16) | (bytes[i + 3] << 24);
}

function reference(bytes, start, limit) {
  'noinline';
  var i = +start, result = 0;
  limit = +limit;
  while (i < limit) {
    result = Math.imul(result ^ readWord(bytes, i), 16777619) >>> 0;
    i += 4;
  }
  return [result, i];
}

function compare(bytes, start, limit) {
  const actual = checksum(bytes, start, limit);
  const expected = reference(bytes, start, limit);
  check(Object.is(actual[0], expected[0]) && Object.is(actual[1], expected[1]));
}

for (const Type of [Uint8Array, Uint8ClampedArray, Int8Array, Uint16Array,
                   Int32Array, Float32Array, Float64Array, Array]) {
  for (let length = 0; length < 39; ++length) {
    const bytes = new Type(length);
    for (let i = 0; i < length; ++i) bytes[i] = i * 97 - 23;
    for (const start of [0, 1, 2, 7, -1, 0.5, -0, NaN]) {
      for (const limit of [0, length & ~3, length, length + 1, length + 0.5, NaN])
        compare(bytes, start, limit);
    }
    compare(bytes, 4294967294, 4294967295);
    if (Type !== Array) compare(bytes.subarray(1), 0, Math.max(length - 1, 0));
  }
}
compare(null, 0, 0);
compare(undefined, Infinity, Infinity);
compare(new Uint8Array(8), {valueOf() { return 0; }}, 8);
print('numeric bounds and layouts');
// CHECK: numeric bounds and layouts

let trace = '';
const sparse = [7, , 9, 11, , 13, 15, 17];
const parent = Object.create(Array.prototype);
Object.defineProperty(parent, '1', {get() { trace += 'a'; return 19; }});
Object.defineProperty(parent, '4', {get() { trace += 'b'; return 23; }});
Object.setPrototypeOf(sparse, parent);
const sparseResult = checksum(sparse, 0, 8);
check(trace === 'ab');
trace = '';
check(sparseResult[0] === reference(sparse, 0, 8)[0] && trace === 'ab');
const proxy = new Proxy(new Uint8Array([1, 2, 3, 4]), {
  get(target, key) { trace += key; return target[key]; },
});
trace = '';
checksum(proxy, 0, 4);
check(trace === '0123');
const spoof = new Uint8Array([1, 2, 3]);
Object.defineProperty(spoof, 'length', {value: 8});
compare(spoof, 0, spoof.length);
const detached = new Uint8Array([1, 2, 3, 4]);
HermesInternal.detachArrayBuffer(detached.buffer);
compare(detached, 0, 4);
const revoked = Proxy.revocable({}, {});
revoked.revoke();
let failed = false;
try { checksum(revoked.proxy, 0, 4); } catch (error) {
  failed = error instanceof TypeError;
}
check(failed);
print('observable fallback');
// CHECK-NEXT: observable fallback

function stride(bytes, start, limit) {
  'noinline';
  start = +start;
  limit = +limit;
  let sum = 0;
  for (let i = start; i < limit; i += 3) sum = (sum + (bytes[i + 2] | 0)) | 0;
  return sum;
}
const sequence = new Uint8Array([0, 1, 2, 3, 4, 5, 6, 7, 8, 9]);
check(stride(sequence, 0, 7) === 15);
check(stride(sequence, 1, 8) === 18);
check(stride(sequence, 0, 8) === 15);
check(stride(sequence, 0, 10) === 15);

function withCall(bytes, limit, visit) {
  'noinline';
  limit = +limit;
  let sum = 0;
  for (let i = 0; i < limit; ++i) {
    visit(i);
    sum ^= bytes[i];
  }
  return sum;
}
function withStore(bytes, limit) {
  'noinline';
  limit = +limit;
  let sum = 0;
  for (let i = 0; i < limit; ++i) {
    bytes[i] = i;
    sum ^= bytes[i];
  }
  return sum;
}
function withProperty(bytes, limit, record) {
  'noinline';
  limit = +limit;
  let sum = 0;
  for (let i = 0; i < limit; ++i) sum ^= bytes[i] ^ record.value;
  return sum;
}
function withCoercion(bytes, limit, value) {
  'noinline';
  limit = +limit;
  let sum = 0;
  for (let i = 0; i < limit; ++i) sum ^= bytes[i] ^ value;
  return sum;
}
function withHandler(bytes, limit) {
  'noinline';
  limit = +limit;
  let sum = 0;
  try {
    for (let i = 0; i < limit; ++i) sum ^= bytes[i];
  } catch (error) { return -1; }
  return sum;
}
const detachable = new Uint8Array([11, 13, 17, 19]);
check(withCall(detachable, 4, i => {
  if (i === 1) HermesInternal.detachArrayBuffer(detachable.buffer);
}) === 11);
check(withStore(new Uint8Array(4), 4) === 0);
trace = '';
withProperty(sequence, 4, {get value() { trace += 'p'; return 3; }});
check(trace === 'pppp');
trace = '';
withCoercion(sequence, 4, {valueOf() { trace += 'c'; return 5; }});
check(trace === 'cccc');
check(withHandler(revoked.proxy, 4) === -1);
print('effect exclusions');
// CHECK-NEXT: effect exclusions

// Collections between calls move receivers while their buffers remain live.
// The specialized reads must decode the receiver of the current invocation.
for (let i = 0; i < 80; ++i) {
  const bytes = new Uint8Array([i, 2, 3, 4, 5, 6, 7, 8]).subarray(1);
  gc();
  compare(bytes, 0, 4);
  compare(bytes, 0, 7);
}
print('moving receivers');
// CHECK-NEXT: moving receivers

// FAST-LABEL: function checksum(
// FAST: _sh_can_read_byte_array_loop
// FAST: _sh_read_byte_array_element
// FAST: _sh_read_byte_array_element
// FAST: _sh_read_byte_array_element
// FAST: _sh_read_byte_array_element
// FAST: function_end
// SLOW-NOT: _sh_can_read_byte_array_loop
// SLOW-NOT: _sh_read_byte_array_element
// OFF-NOT: _sh_can_read_byte_array_loop
// OFF-NOT: _sh_read_byte_array_element
