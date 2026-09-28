/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "JSLibInternal.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSPromise.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/SmallHermesValue-inline.h"
#include "hermes/VM/StackFrame-inline.h"

namespace hermes {
namespace vm {

namespace {
CallResult<HermesValue> nativePromiseAllReactionJob(void *, Runtime &);

enum ConfigSlot { Constructor, Prototype, HandleHook, RejectHook, Noop, ThenKey, ResolveNonExact, AllResolve, AllThen, AllDefine, ResolveKey, ConfigCount };
enum ReactionSlot { Fulfill, Reject, Target, ErrorTarget, Next, Argument, Rejected, Busy, ReactionCount };
enum AllSlot { AllResult, AllValues, AllRemaining, AllResolved, AllExposed, AllCount };
enum class Operation : uintptr_t {
  Pending, Fulfilled, Resolve, Reject, Handle, CreateHandler, IsPromise,
  IsRejected, IsHandled, SetHooks, TryFulfilled, ConfigureAsync, FinishAsync,
};

enum class PlainProperty { Missing, Data, Unsupported };

// A failed guard must not perform an observable read before the JS fallback.
PlainProperty plainProperty(
    Runtime &runtime,
    Handle<JSObject> initial,
    SymbolID key,
    PinnedHermesValue &value) {
  auto object = runtime.makeMutableHandle(*initial);
  for (unsigned depth = 0; object && depth < 16; ++depth) {
    if (object->isProxyObject() || object->isHostObject() || object->isLazy() ||
        object->getClass(runtime)->isTyped())
      return PlainProperty::Unsupported;
    NamedPropertyDescriptor descriptor;
    if (JSObject::getOwnNamedDescriptor(object, runtime, key, descriptor)) {
      if (descriptor.flags.accessor)
        return PlainProperty::Unsupported;
      value = JSObject::getNamedSlotValueUnsafe(*object, runtime, descriptor)
                  .unboxToHV(runtime);
      return PlainProperty::Data;
    }
    object = object->getParent(runtime);
  }
  return object ? PlainProperty::Unsupported : PlainProperty::Missing;
}

bool plainPropertyEquals(
    Runtime &runtime,
    Handle<JSObject> object,
    SymbolID key,
    Handle<> expected) {
  PinnedHermesValue value;
  return plainProperty(runtime, object, key, value) == PlainProperty::Data &&
      value.getRaw() == expected->getRaw();
}

bool plainNativeMethod(
    Runtime &runtime,
    Handle<JSObject> object,
    Predefined::Str key,
    NativeFunctionPtr expected) {
  PinnedHermesValue value;
  if (plainProperty(runtime, object, Predefined::getSymbolID(key), value) !=
      PlainProperty::Data)
    return false;
  auto *function = dyn_vmcast<NativeFunction>(value);
  return function && function->getFunctionPtr() == expected &&
      function->getContext() == nullptr;
}

bool canDefineAllElement(Runtime &runtime) {
  if (!plainPropertyEquals(runtime, runtime.getGlobal(),
          Predefined::getSymbolID(Predefined::Object), runtime.objectConstructor) ||
      !plainNativeMethod(runtime, runtime.objectConstructor,
          Predefined::defineProperty, objectDefineProperty))
    return false;
  PinnedHermesValue ignored;
  // The original descriptor literal inherits these two fields. Even an
  // inherited undefined getter field makes a data descriptor invalid.
  return plainProperty(runtime, runtime.objectPrototype,
             Predefined::getSymbolID(Predefined::get), ignored) ==
          PlainProperty::Missing &&
      plainProperty(runtime, runtime.objectPrototype,
          Predefined::getSymbolID(Predefined::set), ignored) ==
          PlainProperty::Missing;
}

Handle<Environment> currentConfig(Runtime &runtime) {
  return runtime.makeHandle(
      runtime.getCurrentFrame()->getCalleeClosureUnsafe()->getEnvironment(runtime));
}

HermesValue readSlot(NativeFunction *function, Runtime &runtime, unsigned slot) {
  return NativeFunction::getAdditionalSlotValue(function, runtime, slot)
      .unboxToHV(runtime);
}

void writeSlot(
    Handle<NativeFunction> function,
    Runtime &runtime,
    unsigned slot,
    HermesValue value) {
  auto encoded = SmallHermesValue::encodeHermesValue(value, runtime);
  NativeFunction::setAdditionalSlotValue(*function, runtime, slot, encoded);
}

HermesValue readReactionSlot(
    NativeFunction *function,
    Runtime &runtime,
    unsigned slot) {
  return function->getEnvironment(runtime)->slot(slot);
}

void writeReactionSlot(
    Handle<NativeFunction> function,
    Runtime &runtime,
    unsigned slot,
    HermesValue value) {
  function->getEnvironment(runtime)->slot(slot).set(value, runtime.getHeap());
}

Handle<NativeFunction> makeFunction(
    Runtime &runtime,
    Handle<Environment> config,
    NativeFunctionPtr body,
    unsigned length,
    unsigned slots,
    void *context = nullptr) {
  return NativeFunction::create(
      runtime, runtime.functionPrototype, config, context, body,
      Predefined::getSymbolID(Predefined::emptyString), length,
      Runtime::makeNullHandle<JSObject>(), slots);
}
} // namespace

struct NativePromiseCore {
  using State = JSPromise::State;

  static ExecutionStatus settleAll(
      Runtime &runtime,
      Handle<Environment> aggregate,
      bool rejected,
      Handle<> value) {
    if (aggregate->slot(AllResolved).getBool())
      return ExecutionStatus::RETURNED;
    // Resolution may run a thenable while the result is still Pending.
    // Preserve the capability's AlreadyResolved token independently of state.
    aggregate->slot(AllResolved).set(
        HermesValue::encodeBoolValue(true), runtime.getHeap());
    if (!rejected)
      aggregate->slot(AllExposed).set(
          HermesValue::encodeBoolValue(true), runtime.getHeap());
    auto config = runtime.makeHandle(aggregate->getParentEnvironment(runtime));
    auto result = runtime.makeHandle<JSPromise>(aggregate->slot(AllResult));
    return rejected ? reject(runtime, config, result, value)
                    : resolve(runtime, config, result, value);
  }

  static ExecutionStatus collectAll(
      Runtime &runtime,
      Handle<Environment> aggregate,
      Handle<> index,
      Handle<> value,
      bool directDefinition) {
    auto values = runtime.makeHandle<JSObject>(aggregate->slot(AllValues));
    if (directDefinition || canDefineAllElement(runtime)) {
      if (!aggregate->slot(AllExposed).getBool()) {
        // Until an observable callback receives it, this is a fresh array
        // with only our default indexed properties. Keep its visible length
        // current in case a later override exposes a partially filled result.
        auto array = Handle<JSArray>::vmcast(values);
        uint32_t offset = index->getNumber();
        if (JSArray::setElementAt(array, runtime, offset, value) == ExecutionStatus::EXCEPTION)
          return ExecutionStatus::EXCEPTION;
        if (offset >= JSArray::getLength(*array, runtime)) {
          auto length = SmallHermesValue::encodeNumberValue(offset + 1, runtime);
          JSArray::putLengthUnsafe(*array, runtime, length);
        }
      } else if (JSObject::defineOwnComputed(values, runtime, index,
              DefinePropertyFlags::getDefaultNewPropertyFlags(), value,
              PropOpFlags().plusThrowOnError()) == ExecutionStatus::EXCEPTION) {
        // A previous override may have exposed, frozen or redefined the array.
        return ExecutionStatus::EXCEPTION;
      }
    } else {
      aggregate->slot(AllExposed).set(
          HermesValue::encodeBoolValue(true), runtime.getHeap());
      auto config = runtime.makeHandle(aggregate->getParentEnvironment(runtime));
      auto callback = runtime.makeHandle<Callable>(config->slot(AllDefine));
      if (Callable::executeCall3(callback, runtime, Runtime::getUndefinedValue(),
              values.getHermesValue(), *index, *value) == ExecutionStatus::EXCEPTION)
        return ExecutionStatus::EXCEPTION;
    }
    auto remaining = aggregate->slot(AllRemaining).getNumber() - 1;
    aggregate->slot(AllRemaining).set(
        HermesValue::encodeTrustedNumberValue(remaining), runtime.getHeap());
    return remaining == 0 ? settleAll(runtime, aggregate, false, values)
                          : ExecutionStatus::RETURNED;
  }

  static CallResult<HermesValue> allReaction(
      Runtime &runtime,
      Handle<Environment> environment) {
    if (!environment->slot(Busy).getBool() ||
        !environment->slot(Rejected).isBool())
      return runtime.raiseTypeError("Promise aggregation has no queued input");
    auto aggregate = runtime.makeHandle(environment->getParentEnvironment(runtime));
    auto config = runtime.makeHandle(aggregate->getParentEnvironment(runtime));
    auto value = runtime.makeHandle(environment->slot(Argument));
    auto index = runtime.makeHandle(environment->slot(Fulfill));
    bool rejected = environment->slot(Rejected).getBool();
    environment->slot(Argument).set(HermesValue::encodeUndefinedValue(), runtime.getHeap());
    environment->slot(Busy).set(HermesValue::encodeBoolValue(false), runtime.getHeap());
    auto status = rejected ? settleAll(runtime, aggregate, true, value)
                           : collectAll(runtime, aggregate, index, value, false);
    if (status == ExecutionStatus::EXCEPTION) {
      if (isUncatchableError(runtime.getThrownValue()))
        return ExecutionStatus::EXCEPTION;
      // The JS .then result is otherwise unreachable and resolves undefined.
      // Materialize its rejection only on a throw, including when tracking was
      // enabled after registration. Never redirect that error to Promise.all.
      auto error = runtime.makeHandle(runtime.getThrownValue());
      runtime.clearThrownValue();
      auto target = pending(runtime, config);
      if (reject(runtime, config, target, error) == ExecutionStatus::EXCEPTION)
        return ExecutionStatus::EXCEPTION;
    }
    return HermesValue::encodeUndefinedValue();
  }

  static CallResult<HermesValue> tryAll(
      Runtime &runtime,
      Handle<Environment> config,
      Handle<> receiver,
      Handle<> input) {
    auto array = Handle<JSArray>::dyn_vmcast(input);
    if (!array || JSArray::getLength(*array, runtime) > 4096 ||
        receiver->getRaw() != config->slot(Constructor).getRaw() ||
        !config->slot(HandleHook).isNull() || !config->slot(RejectHook).isNull())
      return HermesValue::encodeUndefinedValue();
    auto constructor = Handle<JSObject>::vmcast(receiver);
    auto prototype = runtime.makeHandle<JSObject>(config->slot(Prototype));
    auto resolveMethod = runtime.makeHandle<JSObject>(config->slot(AllResolve));
    auto thenMethod = runtime.makeHandle(config->slot(AllThen));

    uint32_t length = JSArray::getLength(*array, runtime);
    {
      bool checkedConstructor = false;
      bool checkedThen = false;
      GCScopeMarkerRAII marker(runtime);
      for (uint32_t i = 0; i < length; ++i) {
        // Indexed storage excludes holes, accessors and named index overrides.
        auto item = array->at(runtime, i).unboxToHV(runtime);
        if (item.isEmpty())
          return HermesValue::encodeUndefinedValue();
        if (!item.isObject())
          continue;
        auto promise = Handle<JSPromise>::dyn_vmcast(runtime.makeHandle(item));
        if (!promise)
          return HermesValue::encodeUndefinedValue();
        if (promise->getParent(runtime) == *prototype &&
            promise->getClass(runtime)->getNumProperties() == 0) {
          // No JS runs in this guarded region. Empty native instances share
          // the same prototype proofs until registration has finished.
          if (!checkedConstructor) {
            if (!plainPropertyEquals(runtime, prototype,
                    Predefined::getSymbolID(Predefined::constructor), constructor))
              return HermesValue::encodeUndefinedValue();
            checkedConstructor = true;
          }
          if (promise->state_ != State::Fulfilled && !checkedThen) {
            if (!plainPropertyEquals(runtime, prototype,
                    config->slot(ThenKey).getSymbol(), thenMethod))
              return HermesValue::encodeUndefinedValue();
            checkedThen = true;
          }
        } else if (!plainPropertyEquals(runtime, promise,
                Predefined::getSymbolID(Predefined::constructor), constructor) ||
            (promise->state_ != State::Fulfilled &&
             !plainPropertyEquals(runtime, promise, config->slot(ThenKey).getSymbol(), thenMethod)))
          return HermesValue::encodeUndefinedValue();
        marker.flush();
      }
    }

    if (!plainPropertyEquals(runtime, constructor,
            config->slot(ResolveKey).getSymbol(), resolveMethod) ||
        !plainNativeMethod(runtime, resolveMethod, Predefined::call, functionPrototypeCall) ||
        !plainPropertyEquals(runtime, array,
            Predefined::getSymbolID(Predefined::SymbolIterator), runtime.arrayPrototypeValues) ||
        !plainNativeMethod(runtime, runtime.arrayIteratorPrototype,
            Predefined::next, arrayIteratorPrototypeNext) ||
        !canDefineAllElement(runtime))
      return HermesValue::encodeUndefinedValue();

    auto result = pending(runtime, config);
    auto valuesResult = JSArray::create(runtime, length, 0);
    if (valuesResult == ExecutionStatus::EXCEPTION)
      return ExecutionStatus::EXCEPTION;
    auto values = runtime.makeHandle(std::move(*valuesResult));
    auto aggregate = runtime.makeHandle<Environment>(
        Environment::create(runtime, config, AllCount));
    aggregate->slot(AllResult).set(result.getHermesValue(), runtime.getHeap());
    aggregate->slot(AllValues).set(values.getHermesValue(), runtime.getHeap());
    aggregate->slot(AllRemaining).set(
        HermesValue::encodeTrustedNumberValue(length + 1), runtime.getHeap());
    aggregate->slot(AllResolved).set(HermesValue::encodeBoolValue(false), runtime.getHeap());
    aggregate->slot(AllExposed).set(HermesValue::encodeBoolValue(false), runtime.getHeap());
    GCScopeMarkerRAII marker(runtime);
    for (uint32_t i = 0; i < length; ++i) {
      auto item = runtime.makeHandle(array->at(runtime, i).unboxToHV(runtime));
      auto promise = Handle<JSPromise>::dyn_vmcast(item);
      auto index = runtime.makeHandle(HermesValue::encodeTrustedNumberValue(i));
      if (!promise || promise->state_ == State::Fulfilled) {
        auto value = runtime.makeMutableHandle(
            promise ? HermesValue(promise->result_) : *item);
        // The existing Promise.resolve primitive cache maps both zero signs
        // to +0. A value already stored in a Promise must keep its own sign.
        if (!promise && value->isNumber() && value->getNumber() == 0)
          value = HermesValue::encodeTrustedNumberValue(0);
        // Entry guards prove that these writes cannot call JS. Retain the
        // existing production-mode synchronous fulfilled shortcut.
        if (collectAll(runtime, aggregate, index, value, true) == ExecutionStatus::EXCEPTION)
          return ExecutionStatus::EXCEPTION;
      } else {
        auto environment = runtime.makeHandle<Environment>(
            Environment::create(runtime, aggregate, ReactionCount));
        auto reaction = makeFunction(runtime, environment, nativePromiseAllReactionJob,
            0, 0);
        writeReactionSlot(reaction, runtime, Fulfill, *index);
        writeReactionSlot(reaction, runtime, Next, HermesValue::encodeNullValue());
        writeReactionSlot(reaction, runtime, Busy, HermesValue::encodeBoolValue(false));
        if (handle(runtime, config, promise, reaction) == ExecutionStatus::EXCEPTION)
          return ExecutionStatus::EXCEPTION;
      }
      marker.flush();
    }
    auto remaining = aggregate->slot(AllRemaining).getNumber() - 1;
    aggregate->slot(AllRemaining).set(
        HermesValue::encodeTrustedNumberValue(remaining), runtime.getHeap());
    if (remaining == 0 &&
        settleAll(runtime, aggregate, false, values) == ExecutionStatus::EXCEPTION) {
      if (isUncatchableError(runtime.getThrownValue()))
        return ExecutionStatus::EXCEPTION;
      // The outer JS catch calls the capability reject, whose token has
      // already been consumed by resolve, even if thenable assimilation threw.
      runtime.clearThrownValue();
    }
    return result.getHermesValue();
  }

  static ExecutionStatus rejectThrown(
      Runtime &runtime,
      Handle<Environment> config,
      Handle<JSPromise> promise) {
    if (isUncatchableError(runtime.getThrownValue()))
      return ExecutionStatus::EXCEPTION;
    auto error = runtime.makeHandle(runtime.getThrownValue());
    runtime.clearThrownValue();
    return reject(runtime, config, promise, error);
  }

  static Handle<JSPromise> pending(Runtime &runtime, Handle<Environment> config) {
    auto prototype = runtime.makeHandle<JSObject>(config->slot(Prototype));
    return runtime.makeHandle(JSPromise::create(runtime, prototype));
  }

  static Handle<NativeFunction> reaction(
      Runtime &runtime,
      Handle<Environment> config,
      Handle<> fulfilled,
      Handle<> rejected,
      Handle<> target,
      Handle<> errorTarget) {
    // The general NativeFunction slot reserve is intentionally small. Keep
    // this larger traced payload in its environment, with the core as parent.
    auto environment = runtime.makeHandle<Environment>(
        Environment::create(runtime, config, ReactionCount));
    auto result = makeFunction(
        runtime, environment, nativePromiseReactionJob, 0, 0);
    writeReactionSlot(result, runtime, Fulfill,
              vmisa<Callable>(*fulfilled) ? *fulfilled : HermesValue::encodeNullValue());
    writeReactionSlot(result, runtime, Reject,
              vmisa<Callable>(*rejected) ? *rejected : HermesValue::encodeNullValue());
    writeReactionSlot(result, runtime, Target, *target);
    writeReactionSlot(result, runtime, ErrorTarget, *errorTarget);
    writeReactionSlot(result, runtime, Next, HermesValue::encodeNullValue());
    writeReactionSlot(result, runtime, Busy, HermesValue::encodeBoolValue(false));
    return result;
  }

  static ExecutionStatus handle(
      Runtime &runtime,
      Handle<Environment> config,
      Handle<JSPromise> initial,
      Handle<NativeFunction> reaction) {
    auto promise = runtime.makeMutableHandle(*initial);
    while (promise->state_ == State::Adopted)
      promise = vmcast<JSPromise>(promise->result_);
    if (readReactionSlot(*reaction, runtime, Busy).getBool())
      return runtime.raiseTypeError("Promise reaction is already attached");
    auto hook = runtime.makeHandle(config->slot(HandleHook));
    if (auto callable = Handle<Callable>::dyn_vmcast(hook)) {
      if (Callable::executeCall1(callable, runtime, Runtime::getUndefinedValue(),
                                promise.getHermesValue()) == ExecutionStatus::EXCEPTION)
        return ExecutionStatus::EXCEPTION;
    }
    // A hook can settle the Promise or reenter attachment. Test both states
    // again before changing the queue links.
    if (readReactionSlot(*reaction, runtime, Busy).getBool())
      return runtime.raiseTypeError("Promise reaction was attached by a hook");
    while (promise->state_ == State::Adopted)
      promise = vmcast<JSPromise>(promise->result_);
    promise->handled_ = true;
    writeReactionSlot(reaction, runtime, Busy, HermesValue::encodeBoolValue(true));
    if (promise->state_ == State::Pending) {
      if (auto *last = promise->last_.get(runtime)) {
        auto lastHandle = runtime.makeHandle(last);
        writeReactionSlot(lastHandle, runtime, Next, reaction.getHermesValue());
      } else {
        promise->first_.set(runtime, *reaction, runtime.getHeap());
      }
      promise->last_.set(runtime, *reaction, runtime.getHeap());
    } else {
      writeReactionSlot(reaction, runtime, Argument, promise->result_);
      writeReactionSlot(reaction, runtime, Rejected,
                HermesValue::encodeBoolValue(promise->state_ == State::Rejected));
      runtime.enqueueJob(*reaction);
    }
    return ExecutionStatus::RETURNED;
  }

  static ExecutionStatus finale(
      Runtime &runtime,
      Handle<Environment> config,
      Handle<JSPromise> promise) {
    auto current = runtime.makeMutableHandle(promise->first_.get(runtime));
    promise->first_.set(runtime, nullptr, runtime.getHeap());
    promise->last_.set(runtime, nullptr, runtime.getHeap());
    GCScopeMarkerRAII marker(runtime);
    while (current) {
      auto nextValue = readReactionSlot(*current, runtime, Next);
      auto next = runtime.makeHandle<NativeFunction>(nextValue.isNull()
          ? nullptr : vmcast<NativeFunction>(nextValue));
      writeReactionSlot(current, runtime, Next, HermesValue::encodeNullValue());
      writeReactionSlot(current, runtime, Busy, HermesValue::encodeBoolValue(false));
      if (handle(runtime, config, promise, current) == ExecutionStatus::EXCEPTION)
        return ExecutionStatus::EXCEPTION;
      current = *next;
      marker.flush();
    }
    return ExecutionStatus::RETURNED;
  }

  static ExecutionStatus reject(
      Runtime &runtime,
      Handle<Environment> config,
      Handle<JSPromise> promise,
      Handle<> value) {
    if (promise->state_ != State::Pending)
      return ExecutionStatus::RETURNED;
    promise->result_.set(*value, runtime.getHeap());
    promise->state_ = State::Rejected;
    auto hook = runtime.makeHandle(config->slot(RejectHook));
    if (auto callable = Handle<Callable>::dyn_vmcast(hook)) {
      if (Callable::executeCall2(callable, runtime, Runtime::getUndefinedValue(),
                                promise.getHermesValue(), *value) == ExecutionStatus::EXCEPTION)
        return ExecutionStatus::EXCEPTION;
    }
    return finale(runtime, config, promise);
  }

  static ExecutionStatus doResolve(
      Runtime &runtime,
      Handle<Environment> config,
      Handle<Callable> callback,
      Handle<> receiver,
      Handle<JSPromise> promise) {
    // Both functions share the resolve function's Promise slot. Clearing it
    // before calling user code implements AlreadyResolved, including throws.
    auto resolveFunction = makeFunction(
        runtime, config, nativePromiseResolveFunction, 1, 1);
    writeSlot(resolveFunction, runtime, 0, promise.getHermesValue());
    auto rejectFunction = makeFunction(
        runtime, config, nativePromiseResolveFunction, 1, 1,
        reinterpret_cast<void *>(uintptr_t(1)));
    writeSlot(rejectFunction, runtime, 0, resolveFunction.getHermesValue());
    auto result = Callable::executeCall2(
        callback, runtime, receiver,
        resolveFunction.getHermesValue(), rejectFunction.getHermesValue());
    if (result == ExecutionStatus::EXCEPTION) {
      if (isUncatchableError(runtime.getThrownValue()))
        return ExecutionStatus::EXCEPTION;
      auto pendingValue = readSlot(*resolveFunction, runtime, 0);
      if (pendingValue.isUndefined()) {
        runtime.clearThrownValue();
        return ExecutionStatus::RETURNED;
      }
      writeSlot(resolveFunction, runtime, 0, HermesValue::encodeUndefinedValue());
      return rejectThrown(runtime, config, promise);
    }
    return ExecutionStatus::RETURNED;
  }

  static ExecutionStatus resolve(
      Runtime &runtime,
      Handle<Environment> config,
      Handle<JSPromise> promise,
      Handle<> value) {
    if (promise->state_ != State::Pending)
      return ExecutionStatus::RETURNED;
    if (value->getRaw() == promise.getHermesValue().getRaw()) {
      (void)runtime.raiseTypeError("A promise cannot be resolved with itself");
      return rejectThrown(runtime, config, promise);
    }
    if (auto object = Handle<JSObject>::dyn_vmcast(value)) {
      auto thenKey = config->slot(ThenKey).getSymbol();
      auto thenResult = JSObject::getNamed_RJS(object, runtime, thenKey);
      if (thenResult == ExecutionStatus::EXCEPTION)
        return rejectThrown(runtime, config, promise);
      auto then = runtime.makeHandle(std::move(*thenResult));
      if (auto other = Handle<JSPromise>::dyn_vmcast(value)) {
        auto selfThenResult = JSObject::getNamed_RJS(promise, runtime, thenKey);
        if (selfThenResult == ExecutionStatus::EXCEPTION)
          return rejectThrown(runtime, config, promise);
        if ((*selfThenResult)->getRaw() == then->getRaw()) {
          auto ctorResult = JSObject::getNamed_RJS(
              other, runtime, Predefined::getSymbolID(Predefined::constructor));
          if (ctorResult == ExecutionStatus::EXCEPTION)
            return rejectThrown(runtime, config, promise);
          if ((*ctorResult)->getRaw() == config->slot(Constructor).getRaw()) {
            auto target = *other;
            while (target != *promise && target->state_ == State::Adopted)
              target = vmcast<JSPromise>(target->result_);
            // Mutually resolving pending promises remain pending. Never create
            // a native adoption cycle that would make attachment loop forever.
            if (target == *promise)
              return ExecutionStatus::RETURNED;
            promise->result_.set(*value, runtime.getHeap());
            promise->state_ = State::Adopted;
            return finale(runtime, config, promise);
          }
        }
      }
      if (auto callable = Handle<Callable>::dyn_vmcast(then)) {
        // Keep the current internal thenable-binding behavior during this
        // representation experiment, including observable bind failures.
        auto bindResult = JSObject::getNamed_RJS(
            callable, runtime, Predefined::getSymbolID(Predefined::bind));
        if (bindResult == ExecutionStatus::EXCEPTION)
          return rejectThrown(runtime, config, promise);
        auto bindValue = runtime.makeHandle(std::move(*bindResult));
        auto bind = Handle<Callable>::dyn_vmcast(bindValue);
        if (!bind) {
          (void)runtime.raiseTypeError("Thenable bind must be callable");
          return rejectThrown(runtime, config, promise);
        }
        auto boundResult = Callable::executeCall1(bind, runtime, then, *value);
        if (boundResult == ExecutionStatus::EXCEPTION)
          return rejectThrown(runtime, config, promise);
        auto boundValue = runtime.makeHandle(std::move(*boundResult));
        auto bound = Handle<Callable>::dyn_vmcast(boundValue);
        if (!bound) {
          (void)runtime.raiseTypeError("Bound thenable must be callable");
          return rejectThrown(runtime, config, promise);
        }
        return doResolve(runtime, config, bound, Runtime::getUndefinedValue(), promise);
      }
    }
    promise->result_.set(*value, runtime.getHeap());
    promise->state_ = State::Fulfilled;
    return finale(runtime, config, promise);
  }

  static ExecutionStatus resumeAsync(
      Runtime &runtime,
      Handle<Environment> config,
      Handle<Callable> generator,
      Handle<JSPromise> result,
      Handle<JSObject> record,
      Handle<NativeFunction> initialReaction,
      bool isThrow,
      Handle<> initialValue) {
    auto value = runtime.makeMutableHandle(*initialValue);
    auto reaction = runtime.makeMutableHandle(*initialReaction);
    while (true) {
      GCScopeMarkerRAII marker(runtime);
      auto step = Callable::executeCall3(
          generator, runtime, Runtime::getUndefinedValue(),
          HermesValue::encodeTrustedNumberValue(isThrow ? 1 : 0), *value,
          record.getHermesValue());
      if (step == ExecutionStatus::EXCEPTION)
        return rejectThrown(runtime, config, result);
      // PeepholeLowering supplies this private record with value/done in
      // slots 0/1, and proves both the record and state machine cannot escape.
      assert(step->get().getRaw() == record.getHermesValue().getRaw());
      auto done = JSObject::getNamedSlotValueUnsafe(*record, runtime, 1)
                      .unboxToHV(runtime);
      assert(done.isBool());
      value = JSObject::getNamedSlotValueUnsafe(*record, runtime, 0)
                  .unboxToHV(runtime);
      JSObject::setNamedSlotValueUnsafe(
          *record, runtime, 0, SmallHermesValue::encodeUndefinedValue());
      if (done.getBool())
        return resolve(runtime, config, result, value);

      auto promise = runtime.makeMutableHandle<JSPromise>(nullptr);
      auto normalize = [&]() -> ExecutionStatus {
        if (auto exact = Handle<JSPromise>::dyn_vmcast(value)) {
          auto constructor = JSObject::getNamed_RJS(
              exact, runtime, Predefined::getSymbolID(Predefined::constructor));
          if (constructor == ExecutionStatus::EXCEPTION)
            return ExecutionStatus::EXCEPTION;
          if ((*constructor)->getRaw() == config->slot(Constructor).getRaw()) {
            promise = *exact;
            return ExecutionStatus::RETURNED;
          }
        }
        // Retain the existing primitive cache and thenable resolution path.
        auto nonExact = runtime.makeHandle<Callable>(config->slot(ResolveNonExact));
        auto wrapped = Callable::executeCall1(
            nonExact, runtime, Runtime::getUndefinedValue(), *value);
        if (wrapped == ExecutionStatus::EXCEPTION)
          return ExecutionStatus::EXCEPTION;
        promise = vmcast<JSPromise>(wrapped->get());
        return ExecutionStatus::RETURNED;
      };
      if (normalize() == ExecutionStatus::EXCEPTION) {
        if (isUncatchableError(runtime.getThrownValue()))
          return ExecutionStatus::EXCEPTION;
        // PromiseResolve failures occur at the await expression. Feed them
        // back without recursion when the catch reaches another failing await.
        value = runtime.getThrownValue();
        runtime.clearThrownValue();
        isThrow = true;
        continue;
      }
      if (!reaction) {
        auto environment = runtime.makeHandle<Environment>(
            Environment::create(runtime, config, ReactionCount));
        reaction = *makeFunction(
            runtime, environment, nativePromiseReactionJob, 0, 0,
            reinterpret_cast<void *>(uintptr_t(1)));
        // The native async job shares the queue fields, but owns the state
        // machine and private record instead of two JavaScript callbacks.
        writeReactionSlot(reaction, runtime, Fulfill, generator.getHermesValue());
        writeReactionSlot(reaction, runtime, Reject, record.getHermesValue());
        writeReactionSlot(reaction, runtime, Target, HermesValue::encodeNullValue());
        writeReactionSlot(reaction, runtime, ErrorTarget, result.getHermesValue());
        writeReactionSlot(reaction, runtime, Next, HermesValue::encodeNullValue());
        writeReactionSlot(reaction, runtime, Busy, HermesValue::encodeBoolValue(false));
      }
      if (handle(runtime, config, promise, reaction) != ExecutionStatus::EXCEPTION)
        return ExecutionStatus::RETURNED;
      if (isUncatchableError(runtime.getThrownValue()))
        return ExecutionStatus::EXCEPTION;
      value = runtime.getThrownValue();
      runtime.clearThrownValue();
      // Attachment failure leaves no continuation. Match the JS driver's
      // return/finally behavior, including a finally that replaces the error.
      auto closed = Callable::executeCall3(
          generator, runtime, Runtime::getUndefinedValue(),
          HermesValue::encodeTrustedNumberValue(2), HermesValue::encodeUndefinedValue(),
          record.getHermesValue());
      if (closed == ExecutionStatus::EXCEPTION)
        return rejectThrown(runtime, config, result);
      JSObject::setNamedSlotValueUnsafe(
          *record, runtime, 0, SmallHermesValue::encodeUndefinedValue());
      return reject(runtime, config, result, value);
    }
  }
};

namespace {
// Aggregation uses separate native entries to keep its guards out of ordinary
// Promise and async dispatch.
CallResult<HermesValue> nativePromiseAllReactionJob(void *, Runtime &runtime) {
  GCScope scope(runtime);
  return NativePromiseCore::allReaction(runtime, currentConfig(runtime));
}

CallResult<HermesValue> nativePromiseConfigureAll(void *, Runtime &runtime) {
  GCScope scope(runtime);
  auto args = runtime.getCurrentFrame().getNativeArgs();
  auto config = currentConfig(runtime);
  if (!config->slot(AllResolve).isUndefined() ||
      !args.dyncastArg<Callable>(0) || !args.dyncastArg<Callable>(1) ||
      !args.dyncastArg<Callable>(2))
    return runtime.raiseTypeError("Promise aggregation must be initialized once");
  config->slot(AllResolve).set(args.getArg(0), runtime.getHeap());
  config->slot(AllThen).set(args.getArg(1), runtime.getHeap());
  config->slot(AllDefine).set(args.getArg(2), runtime.getHeap());
  return HermesValue::encodeUndefinedValue();
}

CallResult<HermesValue> nativePromiseTryAll(void *, Runtime &runtime) {
  GCScope scope(runtime);
  auto args = runtime.getCurrentFrame().getNativeArgs();
  return NativePromiseCore::tryAll(
      runtime, currentConfig(runtime), args.getArgHandle(0), args.getArgHandle(1));
}
} // namespace

CallResult<HermesValue> nativePromiseConstructor(void *, Runtime &runtime) {
  GCScope scope(runtime);
  auto args = runtime.getCurrentFrame().getNativeArgs();
  if (!args.isConstructorCall())
    return runtime.raiseTypeError("Promise constructor requires new");
  auto executor = args.dyncastArg<Callable>(0);
  if (!executor)
    return runtime.raiseTypeError("Promise executor must be callable");
  auto config = currentConfig(runtime);
  auto prototype = runtime.makeHandle<JSObject>(config->slot(Prototype));
  auto parent = NativeConstructor::parentForNewThis_RJS(
      runtime, Handle<Callable>::vmcast(args.getNewTargetHandle()), prototype);
  if (parent == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;
  auto parentHandle = runtime.makeHandle(std::move(*parent));
  auto promise = runtime.makeHandle(JSPromise::create(runtime, parentHandle));
  if (executor.getHermesValue().getRaw() != config->slot(Noop).getRaw() &&
      NativePromiseCore::doResolve(runtime, config, executor,
          Runtime::getUndefinedValue(), promise) == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;
  return promise.getHermesValue();
}

CallResult<HermesValue> nativePromiseResolveFunction(void *context, Runtime &runtime) {
  GCScope scope(runtime);
  auto args = runtime.getCurrentFrame().getNativeArgs();
  auto function = runtime.makeHandle(vmcast<NativeFunction>(
      runtime.getCurrentFrame()->getCalleeClosureUnsafe()));
  auto config = currentConfig(runtime);
  auto token = runtime.makeHandle<NativeFunction>(context
      ? readSlot(*function, runtime, 0) : function.getHermesValue());
  auto value = readSlot(*token, runtime, 0);
  if (value.isUndefined())
    return HermesValue::encodeUndefinedValue();
  auto promise = runtime.makeHandle<JSPromise>(value);
  writeSlot(token, runtime, 0, HermesValue::encodeUndefinedValue());
  auto status = context
      ? NativePromiseCore::reject(runtime, config, promise, args.getArgHandle(0))
      : NativePromiseCore::resolve(runtime, config, promise, args.getArgHandle(0));
  if (status == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;
  return HermesValue::encodeUndefinedValue();
}

CallResult<HermesValue> nativePromiseReactionJob(void *context, Runtime &runtime) {
  GCScope scope(runtime);
  auto config = runtime.makeHandle(currentConfig(runtime)->getParentEnvironment(runtime));
  auto reaction = runtime.makeHandle(vmcast<NativeFunction>(
      runtime.getCurrentFrame()->getCalleeClosureUnsafe()));
  if (!readReactionSlot(*reaction, runtime, Busy).getBool() ||
      !readReactionSlot(*reaction, runtime, Rejected).isBool())
    return runtime.raiseTypeError("Promise reaction has no queued input");
  auto rejected = readReactionSlot(*reaction, runtime, Rejected).getBool();
  auto argument = runtime.makeHandle(readReactionSlot(*reaction, runtime, Argument));
  auto target = runtime.makeHandle(readReactionSlot(*reaction, runtime, Target));
  auto errorTarget = runtime.makeHandle(readReactionSlot(*reaction, runtime, ErrorTarget));
  auto callback = runtime.makeHandle(readReactionSlot(*reaction, runtime, rejected ? Reject : Fulfill));
  // The async callback may reuse this reaction before returning. Snapshot the
  // current job's input and clear its queue state before invoking it.
  writeReactionSlot(reaction, runtime, Argument, HermesValue::encodeUndefinedValue());
  writeReactionSlot(reaction, runtime, Busy, HermesValue::encodeBoolValue(false));
  if (context) {
    auto generator = runtime.makeHandle<Callable>(
        readReactionSlot(*reaction, runtime, Fulfill));
    auto record = runtime.makeHandle<JSObject>(
        readReactionSlot(*reaction, runtime, Reject));
    auto promise = Handle<JSPromise>::vmcast(errorTarget);
    if (NativePromiseCore::resumeAsync(
            runtime, config, generator, promise, record, reaction, rejected,
            argument) == ExecutionStatus::EXCEPTION &&
        NativePromiseCore::rejectThrown(runtime, config, promise) ==
            ExecutionStatus::EXCEPTION)
      return ExecutionStatus::EXCEPTION;
    return HermesValue::encodeUndefinedValue();
  }
  if (auto callable = Handle<Callable>::dyn_vmcast(callback)) {
    auto result = Callable::executeCall1(
        callable, runtime, Runtime::getUndefinedValue(), *argument);
    if (result == ExecutionStatus::EXCEPTION) {
      auto destination = target->isNull() ? errorTarget : target;
      if (NativePromiseCore::rejectThrown(runtime, config,
              Handle<JSPromise>::vmcast(destination)) == ExecutionStatus::EXCEPTION)
        return ExecutionStatus::EXCEPTION;
    } else if (!target->isNull()) {
      auto value = runtime.makeHandle(std::move(*result));
      if (NativePromiseCore::resolve(runtime, config,
              Handle<JSPromise>::vmcast(target), value) == ExecutionStatus::EXCEPTION)
        return ExecutionStatus::EXCEPTION;
    }
  } else if (!target->isNull()) {
    auto promise = Handle<JSPromise>::vmcast(target);
    auto status = rejected
        ? NativePromiseCore::reject(runtime, config, promise, argument)
        : NativePromiseCore::resolve(runtime, config, promise, argument);
    if (status == ExecutionStatus::EXCEPTION)
      return ExecutionStatus::EXCEPTION;
  }
  return HermesValue::encodeUndefinedValue();
}

CallResult<HermesValue> nativePromiseOperation(void *context, Runtime &runtime) {
  GCScope scope(runtime);
  auto args = runtime.getCurrentFrame().getNativeArgs();
  auto config = currentConfig(runtime);
  auto op = static_cast<Operation>(reinterpret_cast<uintptr_t>(context));
  if (op == Operation::ConfigureAsync) {
    if (!args.dyncastArg<Callable>(0) || !config->slot(ResolveNonExact).isUndefined())
      return runtime.raiseTypeError("Async Promise resolver must be initialized once");
    config->slot(ResolveNonExact).set(args.getArg(0), runtime.getHeap());
    return HermesValue::encodeUndefinedValue();
  }
  if (op == Operation::FinishAsync) {
    auto promise = args.dyncastArg<JSPromise>(0);
    if (!promise || !args.getArg(2).isBool())
      return runtime.raiseTypeError("Invalid private async completion");
    ExecutionStatus status;
    if (args.getArg(2).getBool()) {
      status = NativePromiseCore::reject(runtime, config, promise, args.getArgHandle(1));
    } else {
      auto generator = args.dyncastArg<Callable>(1);
      auto record = args.dyncastArg<JSObject>(3);
      if (!generator || !record)
        return runtime.raiseTypeError("Invalid private async state machine");
      status = NativePromiseCore::resumeAsync(
          runtime, config, generator, promise, record,
          Runtime::makeNullHandle<NativeFunction>(), false,
          Runtime::getUndefinedValue());
      if (status == ExecutionStatus::EXCEPTION)
        status = NativePromiseCore::rejectThrown(runtime, config, promise);
    }
    if (status == ExecutionStatus::EXCEPTION)
      return ExecutionStatus::EXCEPTION;
    return promise.getHermesValue();
  }
  if (op == Operation::IsPromise)
    return HermesValue::encodeBoolValue(bool(args.dyncastArg<JSPromise>(0)));
  if (op == Operation::Pending || op == Operation::Fulfilled) {
    auto promise = NativePromiseCore::pending(runtime, config);
    if (op == Operation::Fulfilled) {
      promise->result_.set(args.getArg(0), runtime.getHeap());
      promise->state_ = NativePromiseCore::State::Fulfilled;
    }
    return promise.getHermesValue();
  }
  if (op == Operation::SetHooks) {
    for (unsigned i = 0; i != 2; ++i) {
      auto value = args.getArg(i);
      if (!value.isNull() && !vmisa<Callable>(value))
        return runtime.raiseTypeError("Promise tracking hook must be callable or null");
    }
    config->slot(HandleHook).set(args.getArg(0), runtime.getHeap());
    config->slot(RejectHook).set(args.getArg(1), runtime.getHeap());
    return HermesValue::encodeUndefinedValue();
  }
  if (op == Operation::CreateHandler) {
    if ((!args.getArg(2).isNull() && !args.dyncastArg<JSPromise>(2)) ||
        (!args.getArg(3).isNull() && !args.dyncastArg<JSPromise>(3)) ||
        (args.getArg(2).isNull() && args.getArg(3).isNull()))
      return runtime.raiseTypeError("Promise reaction requires a result or error Promise");
    return NativePromiseCore::reaction(runtime, config, args.getArgHandle(0),
        args.getArgHandle(1), args.getArgHandle(2), args.getArgHandle(3)).getHermesValue();
  }
  auto promise = args.dyncastArg<JSPromise>(0);
  if (op == Operation::TryFulfilled) {
    if (!promise || promise->state_ != NativePromiseCore::State::Fulfilled)
      return HermesValue::encodeBoolValue(false);
    auto callback = args.dyncastArg<Callable>(1);
    if (!callback)
      return runtime.raiseTypeError("Promise callback must be callable");
    auto result = Callable::executeCall1(callback, runtime,
        Runtime::getUndefinedValue(), promise->result_);
    if (result == ExecutionStatus::EXCEPTION)
      return ExecutionStatus::EXCEPTION;
    return HermesValue::encodeBoolValue(true);
  }
  if (!promise)
    return runtime.raiseTypeError("Native Promise operation requires a Promise");
  if (op == Operation::IsRejected)
    return HermesValue::encodeBoolValue(promise->state_ == NativePromiseCore::State::Rejected);
  if (op == Operation::IsHandled)
    return HermesValue::encodeBoolValue(promise->handled_);
  ExecutionStatus status;
  if (op == Operation::Resolve || op == Operation::Reject) {
    status = op == Operation::Resolve
        ? NativePromiseCore::resolve(runtime, config, promise, args.getArgHandle(1))
        : NativePromiseCore::reject(runtime, config, promise, args.getArgHandle(1));
  } else {
    assert(op == Operation::Handle);
    auto reaction = args.dyncastArg<NativeFunction>(1);
    if (!reaction || reaction->getFunctionPtr() != nativePromiseReactionJob)
      return runtime.raiseTypeError("Native Promise reaction required");
    status = NativePromiseCore::handle(runtime, config, promise, reaction);
  }
  if (status == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;
  return HermesValue::encodeUndefinedValue();
}

CallResult<HermesValue> hermesInternalCreateNativePromiseCore(void *, Runtime &runtime) {
  GCScope scope(runtime);
  auto args = runtime.getCurrentFrame().getNativeArgs();
  auto noop = args.dyncastArg<Callable>(0);
  if (!noop)
    return runtime.raiseTypeError("Promise core requires its pending executor");
  auto config = runtime.makeHandle<Environment>(Environment::create(
      runtime, Runtime::makeNullHandle<Environment>(), ConfigCount));
  auto prototype = runtime.makeHandle(JSObject::create(runtime));
  auto constructor = runtime.makeHandle(NativeConstructor::create(
      runtime, runtime.functionPrototype, config, nullptr, nativePromiseConstructor));
  auto name = runtime.getIdentifierTable().getSymbolHandle(runtime, createASCIIRef("Promise"));
  if (name == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;
  if (Callable::defineNameLengthAndPrototype(constructor, runtime, **name, 1,
          prototype, Callable::WritablePrototype::No) == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;
  config->slot(Constructor).set(constructor.getHermesValue(), runtime.getHeap());
  config->slot(Prototype).set(prototype.getHermesValue(), runtime.getHeap());
  config->slot(HandleHook).set(HermesValue::encodeNullValue(), runtime.getHeap());
  config->slot(RejectHook).set(HermesValue::encodeNullValue(), runtime.getHeap());
  config->slot(Noop).set(noop.getHermesValue(), runtime.getHeap());
  config->slot(ResolveNonExact).set(HermesValue::encodeUndefinedValue(), runtime.getHeap());
  config->slot(AllResolve).set(HermesValue::encodeUndefinedValue(), runtime.getHeap());
  auto thenKey = runtime.getIdentifierTable().getSymbolHandle(runtime, createASCIIRef("then"));
  if (thenKey == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;
  config->slot(ThenKey).set(HermesValue::encodeSymbolValue(**thenKey), runtime.getHeap());
  auto resolveKey = runtime.getIdentifierTable().getSymbolHandle(runtime, createASCIIRef("resolve"));
  if (resolveKey == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;
  config->slot(ResolveKey).set(HermesValue::encodeSymbolValue(**resolveKey), runtime.getHeap());
  auto result = runtime.makeHandle(JSObject::create(runtime));
  if (JSObject::defineNewOwnProperty(result, runtime, **name,
          PropertyFlags::defaultNewNamedPropertyFlags(), constructor) == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;
  struct Entry {
    const char *name;
    Operation operation;
    unsigned length;
  };
  const Entry entries[] = {
    {"pending", Operation::Pending, 0}, {"fulfilled", Operation::Fulfilled, 1},
    {"resolve", Operation::Resolve, 2}, {"reject", Operation::Reject, 2},
    {"handle", Operation::Handle, 2}, {"createHandler", Operation::CreateHandler, 4},
    {"isPromise", Operation::IsPromise, 1}, {"isRejected", Operation::IsRejected, 1},
    {"isHandled", Operation::IsHandled, 1}, {"setHooks", Operation::SetHooks, 2},
    {"tryFulfilled", Operation::TryFulfilled, 2},
    {"configureAsync", Operation::ConfigureAsync, 1},
    {"finishAsync", Operation::FinishAsync, 4},
  };
  for (const auto &entry : entries) {
    // Installed functions are rooted by result; do not retain each entry's
    // temporary handles for the rest of bootstrap.
    GCScopeMarkerRAII marker(runtime);
    auto key = runtime.getIdentifierTable().getSymbolHandle(runtime, createASCIIRef(entry.name));
    if (key == ExecutionStatus::EXCEPTION)
      return ExecutionStatus::EXCEPTION;
    auto function = makeFunction(runtime, config, nativePromiseOperation, entry.length, 0,
        reinterpret_cast<void *>(static_cast<uintptr_t>(entry.operation)));
    if (JSObject::defineNewOwnProperty(result, runtime, **key,
            PropertyFlags::defaultNewNamedPropertyFlags(), function) == ExecutionStatus::EXCEPTION)
      return ExecutionStatus::EXCEPTION;
  }
  // Keep the new bootstrap hooks positional and capture them in JS closures.
  // Named methods would add identifiers to every runtime and require a
  // property lookup at each aggregation call.
  auto hooksResult = JSArray::create(runtime, 3, 3);
  if (hooksResult == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;
  auto hooks = runtime.makeHandle(std::move(*hooksResult));
  auto configureAll = makeFunction(runtime, config, nativePromiseConfigureAll, 3, 0);
  auto tryAll = makeFunction(runtime, config, nativePromiseTryAll, 2, 0);
  JSArray::setElementAt(hooks, runtime, 0, result);
  JSArray::setElementAt(hooks, runtime, 1, configureAll);
  JSArray::setElementAt(hooks, runtime, 2, tryAll);
  return hooks.getHermesValue();
}

} // namespace vm
} // namespace hermes
