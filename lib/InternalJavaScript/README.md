# Internal Bytecode

## Promise

The Promise Internal Bytecode is generated on the fly by running:

```sh
$ utils/gen-promise-internal-bc.sh
```

The optional native Promise core uses GC-traced internal state and native
resolving functions and reactions. Enable it with
`RuntimeConfig::Builder().withEnableNativePromise(true)` or `-Xnative-promise`.
It is disabled by default and does not enable `HermesInternal` test methods.
Combinators and subclass capability construction continue to use internal
JavaScript, and reactions use the runtime's existing microtask queue.

The bootstrap factory on `HermesInternal` is captured and deleted during
internal initialization, before application code runs. Its returned operations
must remain private to the Promise and async implementations. Activation also
works when `EnableHermesInternal` is false.

With `-fasync-generator-results`, the compiler supplies a private state machine
and a completion record with `value` and `done` in slots 0 and 1. The native
Promise core can drive that state machine directly, retaining its state in one
reusable reaction instead of two JavaScript callbacks. The record layout and
non-escape proof in `PeepholeLowering` are part of this internal contract.
Ordinary generator execution keeps the JavaScript driver. Both paths observe
Promise constructor access and use the same primitive cache and job queue.

## Iterator helpers

The Iterator Helper polyfill is adapted from https://github.com/es-shims/iterator-helpers/ at commit fbd6ffee76f28c0aa1f99180d6971caa6a36f39c. The `Iterator.concat` polyfill was later adapted at commit `b790e7409435f3bd3f43edbf9dc1d962818d8f46`. The required dependencies are taken and flattened in `05-IteratorHelperDependencies.js` and the actual Iterator implementation are followed after the file. The dependencies may be simplified from the original source. Assertions from the polyfill, which threw `TypeError`s, are removed.

Note the original polyfill's dependencies included many polyfills for features that are supported by Hermes. In those cases, they are may be removed and use Hermes implementation directly.
Examples (non-exhausive):
* `Error`s are supported so its polyfill implementation is removed.
* Checks for `Symbol`s support are removed since Hermes support `Symbol`s.
* Hermes implements `Function.prototype.bind`, so the polyfill for this is removed.

The implementation for `Iterator.prototype.constructor` and `Iterator.prototype [ %Symbol.toStringTag% ]` differs from the original polyfill in order to adhere to spec compliance. The changes uses the polyfill for the abstract operation [SetterThatIgnoresPrototypeProperties](https://github.com/es-shims/es-abstract/blob/ba4a616318c6a9854d17f771dcae718d90b8939e/2025/SetterThatIgnoresPrototypeProperties.js) to help with the implementation.

At the time of adaptation, the original polyfill is based off the proposal at https://tc39.es/proposal-iterator-helpers/. This differs slightly from the spec, and the polyfill reflects some of these changes:
1. The original polyfill uses `this instanceof methodName` to guard against constructor calls. This triggers `[[GetPrototypeOf]]` on the receiver, which is observable via Proxy and violates the spec's expected side-effect ordering. Replaced with `new.target !== undefined`, which is not observable and correctly detects constructor calls.
2. An `IteratorRecord` with undefined `[[NextMethod]]` is created prior to argument checking. The original polyfill checks the argument first, then obtains the `IteratorRecord` directly via `GetIteratorDirect`.
3. The `ToPrimive` Abstract Operation in the polyfill is not spec compliant. It should check `%Symbol.toPrimitive%`, which has been added in our adaption of the polyfill.
4. The `SLOT.assert` function from the `internal-slot` package calls `channel.assert`, which uses `objectInspect` to build its error message. `objectInspect` calls `Object.prototype.toString.call(obj)`, which accesses `obj[Symbol.toStringTag]` — an observable side effect on Proxy objects. Removed the `channel.assert` call since the subsequent `SLOT.has` check is sufficient and does not trigger any observable property accesses.
