/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "JSLibInternal.h"

#include "hermes/Platform/Intl/PlatformIntl.h"

#ifdef HERMES_ENABLE_INTL

#include "hermes/VM/ArrayLike.h"
#include "hermes/VM/JSLib/DateUtil.h"
#include "hermes/VM/PrimitiveBox.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/StackFrame-inline.h"
#include "hermes/VM/StringView.h"
#include <cmath>

namespace hermes {
// TODO T65916424: Move this out of the hermes::vm namespace.
namespace vm {

// This implementation tries to avoid errors in platform code causing
// crashes or throwing JS exceptions.  However, it can still result in
// non-compliant behavior.  For example, resolvedOptions methods are
// required to have a particular set of properties, but if the
// platform result doesn't include them, then they will simply not be
// present.  This could manifest as a JS error later if one of the
// properties is referenced.

namespace {

CallResult<std::u16string> stringFromJS(
    Runtime &runtime,
    PseudoHandle<> value) {
  struct : public Locals {
    PinnedValue<> valueHandle;
    PinnedValue<StringPrimitive> strHandle;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.valueHandle = std::move(value);

  CallResult<PseudoHandle<StringPrimitive>> strRes =
      toString_RJS(runtime, lv.valueHandle);
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.strHandle = std::move(*strRes);
  auto view = vm::StringPrimitive::createStringView(runtime, lv.strHandle);
  return std::u16string(view.begin(), view.end());
}

#ifdef HERMES_USE_STATIC_INTL_PROVIDER
CallResult<platform_intl::NumberFormatValue> toIntlNumberValue(
    Runtime &runtime, Handle<> input) {
  using platform_intl::NumberFormatValue;
  struct : public Locals {
    PinnedValue<> value;
    PinnedValue<StringPrimitive> string;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  auto primitive = toPrimitive_RJS(runtime, input, PreferredType::NUMBER);
  if (primitive == ExecutionStatus::EXCEPTION) return primitive.getStatus();
  lv.value = *primitive;
  if (lv.value->isString()) {
    // Number conversion validates StringNumericLiteral and its overflow/underflow
    // limits. Preserve its exact finite decimal value instead of the rounded
    // double; non-decimal integer literals take the existing BigInt parser.
    auto number = toNumber_RJS(runtime, lv.value);
    if (number == ExecutionStatus::EXCEPTION) return number.getStatus();
    if (!std::isfinite(number->getNumber()) || number->getNumber() == 0)
      return NumberFormatValue(number->getNumber());
    lv.string = lv.value->getString();
    auto view = StringPrimitive::createStringView(runtime, lv.string);
    auto begin = view.begin(), end = view.end();
    while (begin != end && (isWhiteSpaceChar(*begin) || isLineTerminatorChar(*begin))) ++begin;
    while (begin != end && (isWhiteSpaceChar(*(end - 1)) || isLineTerminatorChar(*(end - 1)))) --end;
    std::string decimal;
    for (auto it = begin; it != end; ++it) decimal.push_back(static_cast<char>(*it));
    if (decimal.size() > 2 && decimal[0] == '0' &&
        (decimal[1] == 'x' || decimal[1] == 'X' || decimal[1] == 'b' ||
         decimal[1] == 'B' || decimal[1] == 'o' || decimal[1] == 'O')) {
      auto bigint = stringToBigInt(runtime, lv.value);
      if (bigint == ExecutionStatus::EXCEPTION) return bigint.getStatus();
      if (!bigint->isBigInt())
        return runtime.raiseError("Validated integer literal could not be parsed");
      lv.value = *bigint;
    } else {
      return NumberFormatValue(std::move(decimal));
    }
  }
  if (lv.value->isBigInt()) {
    auto string = toString_RJS(runtime, lv.value);
    if (string == ExecutionStatus::EXCEPTION) return string.getStatus();
    lv.string = std::move(*string);
    auto view = StringPrimitive::createStringView(runtime, lv.string);
    std::string decimal(view.begin(), view.end());
    return NumberFormatValue(std::move(decimal));
  }
  auto number = toNumber_RJS(runtime, lv.value);
  if (number == ExecutionStatus::EXCEPTION) return number.getStatus();
  return NumberFormatValue(number->getNumber());
}
#endif

CallResult<HermesValue> localesToJS(
    Runtime &runtime,
    std::vector<std::u16string> &&result) {
  struct : public Locals {
    PinnedValue<JSArray> array;
    PinnedValue<> name;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  auto arrayRes = JSArray::create(runtime, result.size(), result.size());
  if (LLVM_UNLIKELY(arrayRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.array = std::move(*arrayRes);
  uint64_t index = 0;
  GCScopeMarkerRAII marker{runtime};
  for (auto &locale : result) {
    marker.flush();
    CallResult<HermesValue> nameRes =
        StringPrimitive::createEfficient(runtime, std::move(locale));
    if (LLVM_UNLIKELY(nameRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.name = nameRes.getValue();
    if (LLVM_UNLIKELY(
            JSArray::setElementAt(lv.array, runtime, index++, lv.name) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
  }
  return lv.array.getHermesValue();
}

CallResult<HermesValue> optionsToJS(
    Runtime &runtime,
    platform_intl::Options result) {
  struct : public Locals {
    PinnedValue<JSObject> obj;
    PinnedValue<> key;
    PinnedValue<> value;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  CallResult<PseudoHandle<JSObject>> objRes = JSObject::create(runtime);
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.obj = std::move(*objRes);
  GCScopeMarkerRAII marker{runtime};
#ifdef HERMES_USE_STATIC_INTL_PROVIDER
  // Object insertion order is observable through Object.keys and serialization.
  // Each service uses the subsequence belonging to its resolved-options record.
  static constexpr const char16_t *keys[] = {
      u"locale", u"usage", u"sensitivity", u"ignorePunctuation", u"collation",
      u"numeric", u"caseFirst", u"calendar", u"numberingSystem", u"timeZone",
      u"hourCycle", u"hour12", u"weekday", u"era", u"year", u"month", u"day",
      u"dayPeriod", u"hour", u"minute", u"second", u"fractionalSecondDigits",
      u"timeZoneName", u"dateStyle", u"timeStyle", u"style", u"currency",
      u"currencyDisplay", u"currencySign", u"unit", u"unitDisplay",
      u"minimumIntegerDigits", u"minimumFractionDigits", u"maximumFractionDigits",
      u"minimumSignificantDigits", u"maximumSignificantDigits", u"useGrouping",
      u"notation", u"compactDisplay", u"signDisplay", u"roundingIncrement",
      u"roundingMode", u"roundingPriority", u"trailingZeroDisplay"};
  size_t emitted = 0;
  for (const auto *key : keys) {
    auto found = result.find(key);
    if (found == result.end()) continue;
    auto &kv = *found;
    ++emitted;
#else
  for (auto &kv : result) {
#endif
    marker.flush();
    CallResult<HermesValue> keyRes = StringPrimitive::createEfficient(
        runtime, createUTF16Ref(kv.first.c_str()));
    if (LLVM_UNLIKELY(keyRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.key = keyRes.getValue();
    if (kv.second.isBool()) {
      lv.value = HermesValue::encodeBoolValue(kv.second.getBool());
    } else if (kv.second.isNumber()) {
      lv.value = HermesValue::encodeTrustedNumberValue(kv.second.getNumber());
    } else {
      assert(kv.second.isString() && "Option is neither bool nor string");
      CallResult<HermesValue> strRes = StringPrimitive::createEfficient(
          runtime, std::move(kv.second.getString()));
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.value = strRes.getValue();
    }
    auto putRes = JSObject::putComputed_RJS(lv.obj, runtime, lv.key, lv.value);
    if (LLVM_UNLIKELY(putRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    assert(*putRes && "put returned false on a plain object");
  }
#ifdef HERMES_USE_STATIC_INTL_PROVIDER
  if (emitted != result.size())
    return runtime.raiseError("Intl resolved options contain an unknown field");
#endif
  return lv.obj.getHermesValue();
}

ExecutionStatus partToJS(
    Runtime &runtime,
    std::unordered_map<std::u16string, std::u16string> &&result,
    MutableHandle<JSObject> out) {
  struct : public Locals {
    PinnedValue<JSObject> obj;
    PinnedValue<> key;
    PinnedValue<> value;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  CallResult<PseudoHandle<JSObject>> objRes = JSObject::create(runtime);
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.obj = std::move(*objRes);
  GCScopeMarkerRAII marker{runtime};
#ifdef HERMES_USE_STATIC_INTL_PROVIDER
  if (!result.count(u"type") || !result.count(u"value") ||
      result.size() != (result.count(u"source") ? 3 : 2))
    return runtime.raiseError("Intl part has an invalid shape");
  for (const auto *key : {u"type", u"value", u"source"}) {
    const auto found = result.find(key);
    if (found == result.end()) continue;
    auto &kv = *found;
#else
  for (auto &kv : result) {
#endif
    marker.flush();

    CallResult<HermesValue> keyRes = StringPrimitive::createEfficient(
        runtime, createUTF16Ref(kv.first.c_str()));
    if (LLVM_UNLIKELY(keyRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.key = keyRes.getValue();
    CallResult<HermesValue> valueRes =
        StringPrimitive::createEfficient(runtime, std::move(kv.second));
    if (LLVM_UNLIKELY(valueRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.value = valueRes.getValue();
    auto putRes = JSObject::putComputed_RJS(lv.obj, runtime, lv.key, lv.value);
    if (LLVM_UNLIKELY(putRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    assert(*putRes && "put returned false on a plain object");
  }

  out.castAndSetHermesValue<JSObject>(lv.obj.getHermesValue());
  return ExecutionStatus::RETURNED;
}

CallResult<HermesValue> partsToJS(
    Runtime &runtime,
    std::vector<std::unordered_map<std::u16string, std::u16string>> &&result) {
  struct : public Locals {
    PinnedValue<JSArray> array;
    PinnedValue<JSObject> partObj;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  auto arrayRes = JSArray::create(runtime, result.size(), result.size());
  if (LLVM_UNLIKELY(arrayRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.array = std::move(*arrayRes);
  uint64_t index = 0;
  GCScopeMarkerRAII marker{runtime};
  for (auto &part : result) {
    marker.flush();
    if (LLVM_UNLIKELY(
            partToJS(
                runtime,
                std::move(part),
                MutableHandle<JSObject>{lv.partObj}) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (LLVM_UNLIKELY(
            JSArray::setElementAt(lv.array, runtime, index++, lv.partObj) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
  }
  return lv.array.getHermesValue();
}

CallResult<std::vector<std::u16string>> normalizeLocales(
    Runtime &runtime,
    Handle<> locales) {
#ifdef HERMES_USE_STATIC_INTL_PROVIDER
  if (locales->isUndefined()) return std::vector<std::u16string>{};
  if (locales->isString()) {
    auto view = StringPrimitive::createStringView(runtime, Handle<StringPrimitive>::vmcast(locales));
    return platform_intl::getCanonicalLocales(runtime, {std::u16string(view.begin(), view.end())});
  }
  struct : Locals {
    PinnedValue<JSObject> object;
    PinnedValue<> index;
    PinnedValue<> value;
    PinnedValue<StringPrimitive> string;
  } lv;
  LocalsRAII roots(runtime, &lv);
  auto object = toObject(runtime, locales);
  if (object == ExecutionStatus::EXCEPTION) return object.getStatus();
  lv.object.castAndSetHermesValue<JSObject>(*object);
  auto length = getArrayLikeLength_RJS(lv.object, runtime);
  if (length == ExecutionStatus::EXCEPTION) return length.getStatus();
  std::vector<std::u16string> result;
  GCScopeMarkerRAII marker(runtime);
  for (uint64_t index = 0; index < *length; ++index) {
    marker.flush();
    lv.index = HermesValue::encodeTrustedNumberValue(index);
    auto exists = JSObject::hasComputed(lv.object, runtime, lv.index);
    if (exists == ExecutionStatus::EXCEPTION) return exists.getStatus();
    if (!*exists) continue;
    auto value = JSObject::getComputed_RJS(lv.object, runtime, lv.index);
    if (value == ExecutionStatus::EXCEPTION) return value.getStatus();
    lv.value = std::move(*value);
    if (!lv.value->isString() && !lv.value->isObject())
      return runtime.raiseTypeError("Locale must be a string or object");
    auto string = toString_RJS(runtime, lv.value);
    if (string == ExecutionStatus::EXCEPTION) return string.getStatus();
    lv.string = std::move(*string);
    const auto view = StringPrimitive::createStringView(runtime, lv.string);
    // Canonicalize this element before reading the next one: validation and
    // duplicate removal are observable when the input contains getters/proxies.
    auto canonical = platform_intl::getCanonicalLocales(runtime,
        {std::u16string(view.begin(), view.end())});
    if (canonical == ExecutionStatus::EXCEPTION) return canonical.getStatus();
    assert(canonical->size() == 1);
    if (std::find(result.begin(), result.end(), canonical->front()) == result.end())
      result.push_back(std::move(canonical->front()));
  }
  return result;
#else
  std::vector<std::u16string> ret;

  if (locales->isUndefined()) {
    return ret;
  }

  if (locales->isString()) {
    auto view = StringPrimitive::createStringView(
        runtime, Handle<StringPrimitive>::vmcast(locales));
    ret.emplace_back(view.begin(), view.end());
    return ret;
  }

  struct : public Locals {
    PinnedValue<JSObject> localeObj;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  CallResult<HermesValue> objRes = toObject(runtime, locales);
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.localeObj.castAndSetHermesValue<JSObject>(objRes.getValue());

  CallResult<uint64_t> lengthRes =
      getArrayLikeLength_RJS(lv.localeObj, runtime);
  if (LLVM_UNLIKELY(lengthRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  bool isProxy = lv.localeObj->isProxyObject();
  if (LLVM_UNLIKELY(
          createListFromArrayLike_RJS(
              lv.localeObj,
              runtime,
              *lengthRes,
              [&ret, isProxy](
                  Runtime &runtime, uint64_t index, PseudoHandle<> value) {
                // When the locales list is a proxy object, gaps are allowed.
                if (isProxy && value->isUndefined())
                  return ExecutionStatus::RETURNED;

                if (!value->isString() && !value->isObject()) {
                  return runtime.raiseTypeError("Incorrect object type");
                }
                CallResult<std::u16string> strRes =
                    stringFromJS(runtime, std::move(value));
                if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
                  return ExecutionStatus::EXCEPTION;
                }
                ret.emplace_back(*strRes);
                return ExecutionStatus::RETURNED;
              }) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return std::move(ret);
#endif
}

constexpr int kDateRequired = 1 << 0;
constexpr int kTimeRequired = 1 << 1;
constexpr int kDateDefault = 1 << 2;
constexpr int kTimeDefault = 1 << 3;

// To normalize options, we need to know the valid options names, and
// the expected type of each.
struct OptionData {
  std::u16string_view name;
  platform_intl::Option::Kind kind;
  int flags;
};

constexpr OptionData kSupportedLocalesOfOptions[] = {
    {u"localeMatcher", platform_intl::Option::Kind::String, 0}};

constexpr OptionData kCollatorOptions[] = {
    {u"usage", platform_intl::Option::Kind::String, 0},
    {u"localeMatcher", platform_intl::Option::Kind::String, 0},
    {u"numeric", platform_intl::Option::Kind::Bool, 0},
    {u"caseFirst", platform_intl::Option::Kind::String, 0},
    {u"sensitivity", platform_intl::Option::Kind::String, 0},
    {u"ignorePunctuation", platform_intl::Option::Kind::Bool, 0},
    {u"collation", platform_intl::Option::Kind::String, 0},
};

constexpr OptionData kDTFOptions[] = {
    {u"localeMatcher", platform_intl::Option::Kind::String, 0},
    {u"calendar", platform_intl::Option::Kind::String, 0},
    {u"numberingSystem", platform_intl::Option::Kind::String, 0},
    {u"hour12", platform_intl::Option::Kind::Bool, 0},
    {u"hourCycle", platform_intl::Option::Kind::String, 0},
    {u"timeZone", platform_intl::Option::Kind::String, 0},
    {u"formatMatcher", platform_intl::Option::Kind::String, 0},
    {u"weekday", platform_intl::Option::Kind::String, kDateRequired},
    {u"era", platform_intl::Option::Kind::String, 0},
    {u"year",
     platform_intl::Option::Kind::String,
     kDateRequired | kDateDefault},
    {u"month",
     platform_intl::Option::Kind::String,
     kDateRequired | kDateDefault},
#ifdef HERMES_USE_STATIC_INTL_PROVIDER
    {u"dayPeriod", platform_intl::Option::Kind::String, kTimeRequired},
#else
    {u"dayPeriod", platform_intl::Option::Kind::String, 0},
#endif
    {u"day", platform_intl::Option::Kind::String, kDateRequired | kDateDefault},
    {u"hour",
     platform_intl::Option::Kind::String,
     kTimeRequired | kTimeDefault},
    {u"minute",
     platform_intl::Option::Kind::String,
     kTimeRequired | kTimeDefault},
    {u"second",
     platform_intl::Option::Kind::String,
     kTimeRequired | kTimeDefault},
    {u"timeZoneName", platform_intl::Option::Kind::String, 0},
    {u"dateStyle", platform_intl::Option::Kind::String, 0},
    {u"timeStyle", platform_intl::Option::Kind::String, 0},
#ifdef HERMES_USE_STATIC_INTL_PROVIDER
    {u"fractionalSecondDigits", platform_intl::Option::Kind::Number, kTimeRequired},
#else
    {u"fractionalSecondDigits", platform_intl::Option::Kind::Number, 0},
#endif
};

constexpr OptionData kNumberFormatOptions[] = {
    {u"localeMatcher", platform_intl::Option::Kind::String, 0},
    {u"numberingSystem", platform_intl::Option::Kind::String, 0},
    {u"style", platform_intl::Option::Kind::String, 0},
    {u"currency", platform_intl::Option::Kind::String, 0},
    {u"currencyDisplay", platform_intl::Option::Kind::String, 0},
    {u"currencySign", platform_intl::Option::Kind::String, 0},
    {u"unit", platform_intl::Option::Kind::String, 0},
    {u"unitDisplay", platform_intl::Option::Kind::String, 0},
    {u"notation", platform_intl::Option::Kind::String, 0},
    {u"minimumIntegerDigits", platform_intl::Option::Kind::Number, 0},
    {u"minimumFractionDigits", platform_intl::Option::Kind::Number, 0},
    {u"maximumFractionDigits", platform_intl::Option::Kind::Number, 0},
    {u"minimumSignificantDigits", platform_intl::Option::Kind::Number, 0},
    {u"maximumSignificantDigits", platform_intl::Option::Kind::Number, 0},
    {u"compactDisplay", platform_intl::Option::Kind::String, 0},
#ifdef HERMES_USE_STATIC_INTL_PROVIDER
    {u"useGrouping", platform_intl::Option::Kind::BoolOrString, 0},
    {u"roundingIncrement", platform_intl::Option::Kind::Number, 0},
    {u"roundingMode", platform_intl::Option::Kind::String, 0},
    {u"roundingPriority", platform_intl::Option::Kind::String, 0},
    {u"trailingZeroDisplay", platform_intl::Option::Kind::String, 0},
#else
    {u"useGrouping", platform_intl::Option::Kind::Bool, 0},
#endif
    {u"signDisplay", platform_intl::Option::Kind::String, 0},
};

CallResult<platform_intl::Options> normalizeOptions(
    Runtime &runtime,
    Handle<> options,
    llvh::ArrayRef<OptionData> optionData) {
  platform_intl::Options ret;

  if (options->isNull())
    return runtime.raiseTypeError("Options object can't be null !");

  auto optionsObj = Handle<JSObject>::dyn_vmcast(options);
  if (!optionsObj) {
    return ret;
  }

  struct : public Locals {
    PinnedValue<> name;
    PinnedValue<> value;
    PinnedValue<StringPrimitive> strValue;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  GCScopeMarkerRAII marker{runtime};
  for (const OptionData &pod : optionData) {
    marker.flush();
    CallResult<HermesValue> nameRes = StringPrimitive::createEfficient(
        runtime, UTF16Ref{pod.name.data(), pod.name.size()});
    if (LLVM_UNLIKELY(nameRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.name = nameRes.getValue();

    CallResult<PseudoHandle<>> valRes =
        JSObject::getComputed_RJS(optionsObj, runtime, lv.name);
    if (LLVM_UNLIKELY(valRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    if ((*valRes)->isUndefined()) {
      continue;
    }

    if (pod.kind == platform_intl::Option::Kind::Bool ||
        (pod.kind == platform_intl::Option::Kind::BoolOrString &&
         ((*valRes)->isBool() || !toBoolean(valRes->getHermesValue())))) {
      ret.emplace(
          pod.name, platform_intl::Option(toBoolean(valRes->getHermesValue())));
    } else if (pod.kind == platform_intl::Option::Kind::Number) {
      lv.value = std::move(*valRes);
      CallResult<HermesValue> numRes = toNumber_RJS(runtime, lv.value);
      if (LLVM_UNLIKELY(numRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      ret.emplace(pod.name, platform_intl::Option(numRes->getNumber()));
    } else {
      assert(
          (pod.kind == platform_intl::Option::Kind::String ||
           pod.kind == platform_intl::Option::Kind::BoolOrString) &&
          "Unknown option kind");
      lv.value = std::move(*valRes);
      CallResult<PseudoHandle<StringPrimitive>> strRes =
          toString_RJS(runtime, lv.value);
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.strValue = std::move(*strRes);
      auto view = StringPrimitive::createStringView(runtime, lv.strValue);
      ret.emplace(
          pod.name,
          platform_intl::Option(std::u16string(view.begin(), view.end())));
    }
  }

  return std::move(ret);
}

#ifdef HERMES_USE_STATIC_INTL_PROVIDER
// Deferred digit options must remain rooted until the platform initializer
// decides which precision family is used and performs the required conversions.
class JSIntlOptionReader final : public platform_intl::OptionReader {
 public:
  explicit JSIntlOptionReader(Runtime &runtime)
      : scope_(runtime), options_(runtime) {}

  ExecutionStatus initialize(Runtime &runtime, Handle<> input) {
    if (input->isUndefined()) return ExecutionStatus::RETURNED;
    auto boxed = toObject(runtime, input);
    if (boxed == ExecutionStatus::EXCEPTION) return boxed.getStatus();
    options_.castAndSetHermesValue<JSObject>(*boxed);
    return ExecutionStatus::RETURNED;
  }

  CallResult<std::optional<platform_intl::Option>> read(
      Runtime &runtime, const std::u16string &name,
      platform_intl::Option::Kind kind) override {
    if (!options_) return std::optional<platform_intl::Option>{};
    struct : Locals {
      PinnedValue<> key;
      PinnedValue<> value;
      PinnedValue<StringPrimitive> string;
    } lv;
    LocalsRAII roots(runtime, &lv);
    auto key = StringPrimitive::createEfficient(runtime, createUTF16Ref(name.c_str()));
    if (key == ExecutionStatus::EXCEPTION) return key.getStatus();
    lv.key = *key;
    auto value = JSObject::getComputed_RJS(options_, runtime, lv.key);
    if (value == ExecutionStatus::EXCEPTION) return value.getStatus();
    if ((*value)->isUndefined()) return std::optional<platform_intl::Option>{};
    lv.value = std::move(*value);
    using Option = platform_intl::Option;
    if (kind == Option::Kind::DeferredNumber)
      return std::optional<Option>(Option(runtime.makeHandle(lv.value.getHermesValue())));
    if (kind == Option::Kind::Bool ||
        (kind == Option::Kind::BoolOrString &&
         (lv.value->isBool() || !toBoolean(lv.value.getHermesValue()))))
      return std::optional<Option>(Option(toBoolean(lv.value.getHermesValue())));
    if (kind == Option::Kind::Number) {
      auto number = toNumber_RJS(runtime, lv.value);
      if (number == ExecutionStatus::EXCEPTION) return number.getStatus();
      return std::optional<Option>(Option(number->getNumber()));
    }
    assert(kind == Option::Kind::String || kind == Option::Kind::BoolOrString);
    auto string = toString_RJS(runtime, lv.value);
    if (string == ExecutionStatus::EXCEPTION) return string.getStatus();
    lv.string = std::move(*string);
    const auto view = StringPrimitive::createStringView(runtime, lv.string);
    return std::optional<Option>(Option(std::u16string(view.begin(), view.end())));
  }
 private:
  GCScope scope_;
  MutableHandle<JSObject> options_;
};
#endif

template <typename T>
ExecutionStatus checkOptions(Runtime &runtime, const platform_intl::Options &) {
  return ExecutionStatus::RETURNED;
}

template <>
ExecutionStatus checkOptions<platform_intl::DateTimeFormat>(
    Runtime &runtime,
    const platform_intl::Options &options) {
  // ECMA 402 2023 11.1.2
  const bool hasExplicitFormatComponents = options.count(u"weekday") > 0 ||
      options.count(u"era") > 0 || options.count(u"year") > 0 ||
      options.count(u"month") > 0 || options.count(u"day") > 0 ||
      options.count(u"dayPeriod") > 0 || options.count(u"hour") > 0 ||
      options.count(u"minute") > 0 || options.count(u"second") > 0 ||
      options.count(u"fractionalSecondDigits") > 0 ||
      options.count(u"timeZoneName") > 0;
  const bool hasStyle =
      options.count(u"dateStyle") > 0 || options.count(u"timeStyle") > 0;

  // 42. If dateStyle is not undefined or timeStyle is not undefined, then
  //    a. If hasExplicitFormatComponents is true, then
  //        i. Throw a TypeError exception.
  if (hasStyle && hasExplicitFormatComponents) {
    return runtime.raiseTypeError(
        "{data/time}Style and explicit format components shouldn't be used together");
  }

  return ExecutionStatus::RETURNED;
}

template <>
ExecutionStatus checkOptions<platform_intl::NumberFormat>(
    Runtime &runtime,
    const platform_intl::Options &options) {
  // As best as I can tell, there are only two places (in
  // SetNumberFormatUnitOptions) where option value handling can throw
  // a TypeError, and there is no other place where the platform code
  // needs a TypeError.  So, rather than have to deal with multiple
  // Java error classes, we just handle those two cases right here.

  auto styleIt = options.find(u"style");
  if (styleIt == options.end()) {
    return ExecutionStatus::RETURNED;
  }
  if (styleIt->second.isString() &&
      styleIt->second.getString() == u"currency" &&
      options.count(u"currency") == 0) {
    return runtime.raiseTypeError(
        "Style 'currency' requires option 'currency'");
  }
  if (styleIt->second.isString() && styleIt->second.getString() == u"unit" &&
      options.count(u"unit") == 0) {
    return runtime.raiseTypeError("Style 'unit' requires option 'unit'");
  }
  return ExecutionStatus::RETURNED;
}

void defineIntlConstructor(
    Runtime &runtime,
    SymbolID name,
    NativeFunctionPtr function,
    Handle<JSObject> prototype,
    unsigned paramCount,
    MutableHandle<NativeConstructor> out) {
  out = NativeConstructor::create(
      runtime, runtime.functionPrototype, nullptr, function, paramCount);
  runtime.ignoreAllocationFailure(Callable::defineNameLengthAndPrototype(
      out, runtime, name, paramCount, prototype, Callable::WritablePrototype::No));
}

// T here is one of the platform_intl types.  This exists to avoid
// duplicating code, since all the ctors are basically the same.
template <typename T>
CallResult<HermesValue> intlServiceConstructor(
    Runtime &runtime,
    NativeArgs args,
    llvh::ArrayRef<OptionData> optionData,
    Handle<NativeConstructor> serviceConstructor,
    Handle<JSObject> servicePrototype,
    unsigned int additionalSlots) {
  struct : public Locals {
    PinnedValue<JSObject> selfParent;
    PinnedValue<DecoratedObject> self;
    PinnedValue<HermesValue> typeVal;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  if (LLVM_LIKELY(
          !args.isConstructorCall() ||
          (args.getNewTarget().getRaw() ==
           serviceConstructor.getHermesValue().getRaw()))) {
    lv.selfParent = servicePrototype;
  } else {
    CallResult<PseudoHandle<JSObject>> thisParentRes =
        NativeConstructor::parentForNewThis_RJS(
            runtime,
            Handle<Callable>::vmcast(&args.getNewTarget()),
            servicePrototype);
    if (LLVM_UNLIKELY(thisParentRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.selfParent = std::move(*thisParentRes);
  }
  CallResult<std::vector<std::u16string>> localesRes =
      normalizeLocales(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(localesRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  auto createNative = [&]() -> CallResult<std::unique_ptr<T>> {
#ifdef HERMES_USE_STATIC_INTL_PROVIDER
    if constexpr (std::is_same<T, platform_intl::DateTimeFormat>::value ||
                  std::is_same<T, platform_intl::NumberFormat>::value ||
                  std::is_same<T, platform_intl::Collator>::value) {
      JSIntlOptionReader reader(runtime);
      if (reader.initialize(runtime, args.getArgHandle(1)) == ExecutionStatus::EXCEPTION)
        return ExecutionStatus::EXCEPTION;
      return T::create(runtime, *localesRes, reader);
    }
#endif
  CallResult<platform_intl::Options> optionsRes =
      normalizeOptions(runtime, args.getArgHandle(1), optionData);
  if (LLVM_UNLIKELY(optionsRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // Service specific checks.
  if (LLVM_UNLIKELY(
          checkOptions<T>(runtime, *optionsRes) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return T::create(runtime, *localesRes, *optionsRes);
  };
  auto nativeRes = createNative();
  if (LLVM_UNLIKELY(nativeRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  std::unique_ptr<T> native = std::move(*nativeRes);

  lv.self = DecoratedObject::create(
      runtime, lv.selfParent, std::move(native), additionalSlots);

  lv.typeVal =
      HermesValue::encodeTrustedNumberValue((uint32_t)T::getNativeType());
  auto res = JSObject::defineNewOwnProperty(
      lv.self,
      runtime,
      Predefined::getSymbolID(Predefined::InternalPropertyIntlNativeType),
      PropertyFlags::defaultNewNamedPropertyFlags(),
      lv.typeVal);
  (void)res;
  assert(res != ExecutionStatus::EXCEPTION && "Setting type cannot fail.");
#ifdef HERMES_USE_STATIC_INTL_PROVIDER
  if constexpr (std::is_same<T, platform_intl::DateTimeFormat>::value ||
                std::is_same<T, platform_intl::NumberFormat>::value) {
    if (!args.isConstructorCall()) {
      auto instance = ordinaryHasInstance(runtime, serviceConstructor, args.getThisHandle());
      if (instance == ExecutionStatus::EXCEPTION) return instance.getStatus();
      if (*instance) {
        auto flags = DefinePropertyFlags::getDefaultNewPropertyFlags();
        flags.writable = flags.enumerable = flags.configurable = 0;
        auto defined = JSObject::defineOwnProperty(
            Handle<JSObject>::vmcast(args.getThisHandle()), runtime,
            runtime.intlLegacyConstructedSymbol.getHermesValue().getSymbol(),
            flags, lv.self, PropOpFlags().plusThrowOnError());
        if (defined == ExecutionStatus::EXCEPTION) return defined.getStatus();
        assert(*defined && "ThrowOnError must reject failed legacy construction");
        return args.getThisArg();
      }
    }
  }
#endif
  return lv.self.getHermesValue();
}

// T is a class from platform_intl which has an appropriate
// supportedLocalesOf method.
template <typename T>
CallResult<HermesValue> intlServiceSupportedLocalesOf(
    Runtime &runtime,
    NativeArgs args) {
  CallResult<std::vector<std::u16string>> localesRes =
      normalizeLocales(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(localesRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

#ifdef HERMES_USE_STATIC_INTL_PROVIDER
  JSIntlOptionReader reader(runtime);
  if (reader.initialize(runtime, args.getArgHandle(1)) == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;
  platform_intl::Options options;
  auto matcher = reader.read(runtime, u"localeMatcher", platform_intl::Option::Kind::String);
  if (matcher == ExecutionStatus::EXCEPTION) return matcher.getStatus();
  if (*matcher) options.emplace(u"localeMatcher", std::move(**matcher));
  CallResult<platform_intl::Options> optionsRes(std::move(options));
#else
  CallResult<platform_intl::Options> optionsRes = normalizeOptions(
      runtime, args.getArgHandle(1), kSupportedLocalesOfOptions);
  if (LLVM_UNLIKELY(optionsRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

#endif
  auto cr = T::supportedLocalesOf(runtime, *localesRes, *optionsRes);
  if (cr == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  return localesToJS(runtime, std::move(cr.getValue()));
}

template <typename T>
bool hasIntlDecoration(Runtime &runtime, Handle<DecoratedObject> handle) {
  if (!handle) return false;
  NamedPropertyDescriptor desc;
  if (!JSObject::getOwnNamedDescriptor(
          handle, runtime,
          Predefined::getSymbolID(Predefined::InternalPropertyIntlNativeType), desc))
    return false;
  const auto value = JSObject::getNamedSlotValueUnsafe(*handle, runtime, desc);
  return value.getNumber(runtime) == static_cast<uint32_t>(T::getNativeType());
}

template <typename T>
CallResult<T *> verifyDecoration(
    Runtime &runtime, Handle<DecoratedObject> handle, const char *what) {
  if (!hasIntlDecoration<T>(runtime, handle))
    return runtime.raiseTypeError(TwineChar16(what) + " called with incompatible 'this'");
  return static_cast<T *>(handle->getDecoration());
}

#ifdef HERMES_USE_STATIC_INTL_PROVIDER
// Only format and resolvedOptions unwrap legacy receivers. Other methods must
// require native slots directly, even when a valid fallback symbol is present.
template <typename T>
CallResult<PseudoHandle<DecoratedObject>> unwrapIntlReceiver(
    Runtime &runtime, Handle<> receiver,
    Handle<NativeConstructor> constructor, const char *what) {
  auto native = Handle<DecoratedObject>::dyn_vmcast(receiver);
  if (hasIntlDecoration<T>(runtime, native)) return createPseudoHandle(native.get());
  if (receiver->isObject()) {
    auto instance = ordinaryHasInstance(runtime, constructor, receiver);
    if (instance == ExecutionStatus::EXCEPTION) return instance.getStatus();
    if (*instance) {
      auto fallback = JSObject::getNamed_RJS(
          Handle<JSObject>::vmcast(receiver), runtime,
          runtime.intlLegacyConstructedSymbol.getHermesValue().getSymbol());
      if (fallback == ExecutionStatus::EXCEPTION) return fallback.getStatus();
      auto rooted = runtime.makeHandle(std::move(*fallback));
      native = Handle<DecoratedObject>::dyn_vmcast(rooted);
      if (hasIntlDecoration<T>(runtime, native)) return createPseudoHandle(native.get());
    }
  }
  return runtime.raiseTypeError(TwineChar16(what) + " called with incompatible 'this'");
}
#endif

} // namespace

namespace { // Collator impl stuff.

// Collator internal slots.
enum class CollatorSlotIndexes { boundCompare, COUNT };

PseudoHandle<NativeFunction> getBoundCompare(
    DecoratedObject *collator,
    Runtime &runtime) {
  return createPseudoHandle(
      dyn_vmcast<NativeFunction>(
          DecoratedObject::getAdditionalSlotValue(
              collator,
              runtime,
              static_cast<unsigned int>(CollatorSlotIndexes::boundCompare))
              .unboxToHV(runtime)));
}

void setBoundCompare(
    DecoratedObject *collator,
    Runtime &runtime,
    PseudoHandle<NativeFunction> value) {
  DecoratedObject::setAdditionalSlotValue(
      collator,
      runtime,
      static_cast<unsigned int>(CollatorSlotIndexes::boundCompare),
      SmallHermesValue::encodeObjectValue(value.get(), runtime));
}

// Collator compare internal slots
enum class CollatorCompareSlotIndexes { collator, COUNT };

PseudoHandle<DecoratedObject> getCollator(
    PseudoHandle<NativeFunction> compare,
    Runtime &runtime) {
  return createPseudoHandle(
      vmcast<DecoratedObject>(
          NativeFunction::getAdditionalSlotValue(
              compare.get(),
              runtime,
              static_cast<unsigned int>(CollatorCompareSlotIndexes::collator))
              .getObject(runtime)));
}

void setCollator(
    PseudoHandle<NativeFunction> compare,
    Runtime &runtime,
    PseudoHandle<DecoratedObject> value) {
  NativeFunction::setAdditionalSlotValue(
      compare.get(),
      runtime,
      static_cast<unsigned int>(CollatorCompareSlotIndexes::collator),
      SmallHermesValue::encodeObjectValue(value.get(), runtime));
}

void defineIntlCollator(Runtime &runtime, Handle<JSObject> intl) {
  // Create %CollatorPrototype% intrinsic.  Properties will be added later.
  runtime.intlCollatorPrototype = JSObject::create(runtime);
  Handle<JSObject> prototype{runtime.intlCollatorPrototype};

  struct : public Locals {
    PinnedValue<NativeConstructor> constructor;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // Create %Collator% intrinsic.
  defineIntlConstructor(
      runtime,
      Predefined::getSymbolID(Predefined::Collator),
      intlCollatorConstructor,
      prototype,
      0,
      lv.constructor);
  runtime.intlCollator.castAndSetHermesValue<NativeConstructor>(
      lv.constructor.getHermesValue());

  {
    DefinePropertyFlags dpf{};
    dpf.setValue = 1;

    defineProperty(
        runtime,
        lv.constructor,
        Predefined::getSymbolID(Predefined::prototype),
        prototype,
        dpf);
  }

  defineMethod(
      runtime,
      lv.constructor,
      Predefined::getSymbolID(Predefined::supportedLocalesOf),
      nullptr,
      intlCollatorSupportedLocalesOf,
      1);

  // Add properties to prototype.

  defineProperty(
      runtime,
      prototype,
      Predefined::getSymbolID(Predefined::constructor),
      lv.constructor);

  {
    auto dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
    dpf.writable = 0;
    dpf.enumerable = 0;

    defineProperty(
        runtime,
        prototype,
        Predefined::getSymbolID(Predefined::SymbolToStringTag),
        runtime.getPredefinedStringHandle(Predefined::IntlCollator),
        dpf);
  }

  defineAccessor(
      runtime,
      prototype,
      Predefined::getSymbolID(Predefined::compare),
      nullptr,
      intlCollatorPrototypeCompareGetter,
      nullptr,
      false,
      true);

  defineMethod(
      runtime,
      prototype,
      Predefined::getSymbolID(Predefined::resolvedOptions),
      nullptr,
      intlCollatorPrototypeResolvedOptions,
      0);

  // Add Collator to Intl

  defineProperty(
      runtime,
      intl,
      Predefined::getSymbolID(Predefined::Collator),
      lv.constructor);
}

} // namespace

CallResult<HermesValue> intlCollatorConstructor(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return intlServiceConstructor<platform_intl::Collator>(
      runtime,
      args,
      kCollatorOptions,
      Handle<NativeConstructor>::vmcast(&runtime.intlCollator),
      Handle<JSObject>::vmcast(&runtime.intlCollatorPrototype),
      static_cast<unsigned int>(CollatorSlotIndexes::COUNT));
}

CallResult<HermesValue> intlCollatorSupportedLocalesOf(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return intlServiceSupportedLocalesOf<platform_intl::Collator>(runtime, args);
}

CallResult<HermesValue> intlCollatorCompare(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto *nf = vmcast<NativeFunction>(
      runtime.getCurrentFrame()->getCalleeClosureUnsafe());
  PseudoHandle<DecoratedObject> collatorHandle =
      getCollator(createPseudoHandle(nf), runtime);

  // Since collatorHandle came out of an internal slot, it's an
  // assertable failure if it has the wrong type.
  platform_intl::Collator *collator =
      static_cast<platform_intl::Collator *>(collatorHandle->getDecoration());
  assert(collator && "Intl.Collator platform part is nullptr");

  CallResult<std::u16string> xRes =
      stringFromJS(runtime, createPseudoHandle(args.getArg(0)));
  if (LLVM_UNLIKELY(xRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  CallResult<std::u16string> yRes =
      stringFromJS(runtime, createPseudoHandle(args.getArg(1)));
  if (LLVM_UNLIKELY(yRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return HermesValue::encodeTrustedNumberValue(collator->compare(*xRes, *yRes));
}

CallResult<HermesValue> intlCollatorPrototypeCompareGetter(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  Handle<DecoratedObject> collatorHandle = args.dyncastThis<DecoratedObject>();

  CallResult<platform_intl::Collator *> collatorRes =
      verifyDecoration<platform_intl::Collator>(
          runtime, collatorHandle, "Intl.Collator.prototype.compare getter");
  if (LLVM_UNLIKELY(collatorRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  PseudoHandle<NativeFunction> boundCompare =
      getBoundCompare(*collatorHandle, runtime);
  if (boundCompare) {
    return boundCompare.getHermesValue();
  }

  Handle<NativeFunction> compare = NativeFunction::create(
      runtime,
      runtime.functionPrototype,
      Runtime::makeNullHandle<Environment>(),
      nullptr,
      intlCollatorCompare,
      Predefined::getSymbolID(Predefined::emptyString),
      2,
      Runtime::makeNullHandle<JSObject>(),
      static_cast<unsigned int>(CollatorCompareSlotIndexes::COUNT));
  setCollator(compare, runtime, collatorHandle);

  setBoundCompare(*collatorHandle, runtime, compare);

  return compare.getHermesValue();
}

CallResult<HermesValue> intlCollatorPrototypeResolvedOptions(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  Handle<DecoratedObject> collatorHandle = args.dyncastThis<DecoratedObject>();

  CallResult<platform_intl::Collator *> collatorRes =
      verifyDecoration<platform_intl::Collator>(
          runtime, collatorHandle, "Intl.Collator.prototype.resolvedOptions");
  if (LLVM_UNLIKELY(collatorRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return optionsToJS(runtime, (*collatorRes)->resolvedOptions());
}

namespace { // DateTimeFormat impl stuff.

// DateTimeFormat internal slots.
enum class DTFSlotIndexes { boundFormat, COUNT };

PseudoHandle<NativeFunction> getDTFBoundFormat(
    PseudoHandle<DecoratedObject> dtf,
    Runtime &runtime) {
  return createPseudoHandle(
      dyn_vmcast<NativeFunction>(
          DecoratedObject::getAdditionalSlotValue(
              dtf.get(),
              runtime,
              static_cast<unsigned int>(DTFSlotIndexes::boundFormat))
              .unboxToHV(runtime)));
}

void setDTFBoundFormat(
    PseudoHandle<DecoratedObject> dtf,
    Runtime &runtime,
    PseudoHandle<NativeFunction> value) {
  DecoratedObject::setAdditionalSlotValue(
      dtf.get(),
      runtime,
      static_cast<unsigned int>(DTFSlotIndexes::boundFormat),
      SmallHermesValue::encodeObjectValue(value.get(), runtime));
}

// DateTimeFormat format internal slots
enum class DTFFormatSlotIndexes { dateTimeFormat, COUNT };

PseudoHandle<DecoratedObject> getDateTimeFormat(
    PseudoHandle<NativeFunction> format,
    Runtime &runtime) {
  return createPseudoHandle(
      vmcast<DecoratedObject>(
          NativeFunction::getAdditionalSlotValue(
              format.get(),
              runtime,
              static_cast<unsigned int>(DTFFormatSlotIndexes::dateTimeFormat))
              .getObject(runtime)));
}

void setDateTimeFormat(
    PseudoHandle<NativeFunction> format,
    Runtime &runtime,
    PseudoHandle<DecoratedObject> value) {
  NativeFunction::setAdditionalSlotValue(
      format.get(),
      runtime,
      static_cast<unsigned int>(DTFFormatSlotIndexes::dateTimeFormat),
      SmallHermesValue::encodeObjectValue(value.get(), runtime));
}

void defineIntlDateTimeFormat(Runtime &runtime, Handle<JSObject> intl) {
  // Create %DateTimeFormatPrototype% intrinsic.  Properties will be added
  // later.
  runtime.intlDateTimeFormatPrototype = JSObject::create(runtime);
  Handle<JSObject> prototype{runtime.intlDateTimeFormatPrototype};

  struct : public Locals {
    PinnedValue<NativeConstructor> constructor;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // Create %DateTimeFormat% intrinsic.
  defineIntlConstructor(
      runtime,
      Predefined::getSymbolID(Predefined::DateTimeFormat),
      intlDateTimeFormatConstructor,
      prototype,
      0,
      lv.constructor);
  runtime.intlDateTimeFormat.castAndSetHermesValue<NativeConstructor>(
      lv.constructor.getHermesValue());

  {
    DefinePropertyFlags dpf{};
    dpf.setValue = 1;

    defineProperty(
        runtime,
        lv.constructor,
        Predefined::getSymbolID(Predefined::prototype),
        prototype,
        dpf);
  }

  defineMethod(
      runtime,
      lv.constructor,
      Predefined::getSymbolID(Predefined::supportedLocalesOf),
      nullptr,
      intlDateTimeFormatSupportedLocalesOf,
      1);

  // Add properties to prototype.

  defineProperty(
      runtime,
      prototype,
      Predefined::getSymbolID(Predefined::constructor),
      lv.constructor);

  {
    auto dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
    dpf.writable = 0;
    dpf.enumerable = 0;

    defineProperty(
        runtime,
        prototype,
        Predefined::getSymbolID(Predefined::SymbolToStringTag),
        runtime.getPredefinedStringHandle(Predefined::IntlDateTimeFormat),
        dpf);
  }

  defineAccessor(
      runtime,
      prototype,
      Predefined::getSymbolID(Predefined::format),
      nullptr,
      intlDateTimeFormatPrototypeFormatGetter,
      nullptr,
      false,
      true);

  defineMethod(
      runtime,
      prototype,
      Predefined::getSymbolID(Predefined::formatToParts),
      nullptr,
      intlDateTimeFormatPrototypeFormatToParts,
      1);
#ifdef HERMES_USE_STATIC_INTL_PROVIDER
  defineMethod(runtime, prototype, Predefined::getSymbolID(Predefined::formatRange),
      nullptr, intlDateTimeFormatPrototypeFormatRange, 2);
  defineMethod(runtime, prototype, Predefined::getSymbolID(Predefined::formatRangeToParts),
      reinterpret_cast<void *>(1), intlDateTimeFormatPrototypeFormatRange, 2);
#endif

  defineMethod(
      runtime,
      prototype,
      Predefined::getSymbolID(Predefined::resolvedOptions),
      nullptr,
      intlDateTimeFormatPrototypeResolvedOptions,
      0);

  // Add DateTimeFormat to Intl

  defineProperty(
      runtime,
      intl,
      Predefined::getSymbolID(Predefined::DateTimeFormat),
      lv.constructor);
}

} // namespace

CallResult<HermesValue> intlDateTimeFormatConstructor(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return intlServiceConstructor<platform_intl::DateTimeFormat>(
      runtime,
      args,
      kDTFOptions,
      Handle<NativeConstructor>::vmcast(&runtime.intlDateTimeFormat),
      Handle<JSObject>::vmcast(&runtime.intlDateTimeFormatPrototype),
      static_cast<unsigned int>(DTFSlotIndexes::COUNT));
}

CallResult<HermesValue> intlDateTimeFormatSupportedLocalesOf(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return intlServiceSupportedLocalesOf<platform_intl::DateTimeFormat>(
      runtime, args);
}

namespace {

CallResult<double> dateNowValue(Runtime &runtime, NativeArgs args) {
  // ECMA 402 13.1.5:
  // 1 and 2 are implicit in the callers.
  // 3. If date is not provided or is undefined, then
  if (args.getArg(0).isUndefined()) {
    // a. Let x be Call(%Date_now%, undefined).
    // Reusing NativeArgs is kind of a hack, but dateNow doesn't use
    // it (or the context arg), so this is ok.
    CallResult<HermesValue> dateRes = dateNow(nullptr, runtime);
    if (LLVM_UNLIKELY(dateRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    return dateRes->getNumber();
  }

  // 4.a. Let x be ? ToNumber(date).
  CallResult<HermesValue> xRes = toNumber_RJS(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(xRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // ECMA 402 13.1.6, shared by the callers to this function:
  // 1. Let x be TimeClip(x).
  double x = timeClip(xRes->getNumber());
  // 2. If x is NaN, throw a RangeError exception.
  if (std::isnan(x)) {
    return runtime.raiseRangeError("Invalid time value");
  }

  return x;
}

} // namespace

CallResult<HermesValue> intlDateTimeFormatFormat(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto *nf = vmcast<NativeFunction>(
      runtime.getCurrentFrame()->getCalleeClosureUnsafe());
  PseudoHandle<DecoratedObject> dateTimeFormatHandle =
      getDateTimeFormat(createPseudoHandle(nf), runtime);

  // Since dateTimeFormatHandle came out of an internal slot, it's an
  // assertable failure if it has the wrong type.
  platform_intl::DateTimeFormat *dateTimeFormat =
      static_cast<platform_intl::DateTimeFormat *>(
          dateTimeFormatHandle->getDecoration());
  assert(dateTimeFormat && "Intl.DateTimeFormat platform part is nullptr");

  CallResult<double> dateRes = dateNowValue(runtime, args);
  if (LLVM_UNLIKELY(dateRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
#ifdef HERMES_USE_STATIC_INTL_PROVIDER
  auto result = dateTimeFormat->format(runtime, *dateRes);
  if (result == ExecutionStatus::EXCEPTION) return result.getStatus();
  return StringPrimitive::createEfficient(runtime, std::move(*result));
#else
  return StringPrimitive::createEfficient(
      runtime, dateTimeFormat->format(*dateRes));
#endif
}

CallResult<HermesValue> intlDateTimeFormatPrototypeFormatGetter(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
#ifdef HERMES_USE_STATIC_INTL_PROVIDER
  GCScope scope(runtime);
  auto unwrapped = unwrapIntlReceiver<platform_intl::DateTimeFormat>(
      runtime, args.getThisHandle(), Handle<NativeConstructor>(runtime.intlDateTimeFormat),
      "Intl.DateTimeFormat.prototype.format getter");
  if (unwrapped == ExecutionStatus::EXCEPTION) return unwrapped.getStatus();
  auto dateTimeFormatHandle = runtime.makeHandle(std::move(*unwrapped));
#else
  Handle<DecoratedObject> dateTimeFormatHandle =
      args.dyncastThis<DecoratedObject>();
#endif

  CallResult<platform_intl::DateTimeFormat *> dateTimeFormatRes =
      verifyDecoration<platform_intl::DateTimeFormat>(
          runtime,
          dateTimeFormatHandle,
          "Intl.DateTimeFormat.prototype.format getter");
  if (LLVM_UNLIKELY(dateTimeFormatRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  PseudoHandle<NativeFunction> boundFormat =
      getDTFBoundFormat(dateTimeFormatHandle, runtime);
  if (boundFormat) {
    return boundFormat.getHermesValue();
  }

  Handle<NativeFunction> format = NativeFunction::create(
      runtime,
      runtime.functionPrototype,
      Runtime::makeNullHandle<Environment>(),
      nullptr,
      intlDateTimeFormatFormat,
      Predefined::getSymbolID(Predefined::emptyString),
      1,
      Runtime::makeNullHandle<JSObject>(),
      static_cast<unsigned int>(DTFFormatSlotIndexes::COUNT));
  setDateTimeFormat(format, runtime, dateTimeFormatHandle);

  setDTFBoundFormat(dateTimeFormatHandle, runtime, format);

  return format.getHermesValue();
}

CallResult<HermesValue> intlDateTimeFormatPrototypeFormatToParts(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  Handle<DecoratedObject> dateTimeFormatHandle =
      args.dyncastThis<DecoratedObject>();

  CallResult<platform_intl::DateTimeFormat *> dateTimeFormatRes =
      verifyDecoration<platform_intl::DateTimeFormat>(
          runtime,
          dateTimeFormatHandle,
          "Intl.DateTimeFormat.prototype.formatToParts");
  if (LLVM_UNLIKELY(dateTimeFormatRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  CallResult<double> dateRes = dateNowValue(runtime, args);
  if (LLVM_UNLIKELY(dateRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
#ifdef HERMES_USE_STATIC_INTL_PROVIDER
  auto result = (*dateTimeFormatRes)->formatToParts(runtime, *dateRes);
  if (result == ExecutionStatus::EXCEPTION) return result.getStatus();
  return partsToJS(runtime, std::move(*result));
#else
  return partsToJS(runtime, (*dateTimeFormatRes)->formatToParts(*dateRes));
#endif
}

#ifdef HERMES_USE_STATIC_INTL_PROVIDER
CallResult<HermesValue> intlDateTimeFormatPrototypeFormatRange(void *context, Runtime &runtime) {
  const auto args = runtime.getCurrentFrame().getNativeArgs();
  auto formatter = verifyDecoration<platform_intl::DateTimeFormat>(runtime,
      args.dyncastThis<DecoratedObject>(), "Intl.DateTimeFormat range method");
  if (formatter == ExecutionStatus::EXCEPTION) return formatter.getStatus();
  if (args.getArg(0).isUndefined() || args.getArg(1).isUndefined())
    return runtime.raiseTypeError("Intl date range requires both endpoints");
  auto start = toNumber_RJS(runtime, args.getArgHandle(0));
  if (start == ExecutionStatus::EXCEPTION) return start.getStatus();
  const double x = start->getNumber();
  auto end = toNumber_RJS(runtime, args.getArgHandle(1));
  if (end == ExecutionStatus::EXCEPTION) return end.getStatus();
  // Both observable conversions precede TimeClip and invalid-date rejection.
  const double clippedStart = timeClip(x), clippedEnd = timeClip(end->getNumber());
  if (std::isnan(clippedStart) || std::isnan(clippedEnd))
    return runtime.raiseRangeError("Invalid date range endpoint");
  if (context) {
    auto result = (*formatter)->formatRangeToParts(runtime, x, end->getNumber());
    if (result == ExecutionStatus::EXCEPTION) return result.getStatus();
    return partsToJS(runtime, std::move(*result));
  }
  auto result = (*formatter)->formatRange(runtime, x, end->getNumber());
  if (result == ExecutionStatus::EXCEPTION) return result.getStatus();
  return StringPrimitive::createEfficient(runtime, std::move(*result));
}
#endif

CallResult<HermesValue> intlDateTimeFormatPrototypeResolvedOptions(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
#ifdef HERMES_USE_STATIC_INTL_PROVIDER
  GCScope scope(runtime);
  auto unwrapped = unwrapIntlReceiver<platform_intl::DateTimeFormat>(
      runtime, args.getThisHandle(), Handle<NativeConstructor>(runtime.intlDateTimeFormat),
      "Intl.DateTimeFormat.prototype.resolvedOptions");
  if (unwrapped == ExecutionStatus::EXCEPTION) return unwrapped.getStatus();
  auto dateTimeFormatHandle = runtime.makeHandle(std::move(*unwrapped));
#else
  Handle<DecoratedObject> dateTimeFormatHandle =
      args.dyncastThis<DecoratedObject>();
#endif

  CallResult<platform_intl::DateTimeFormat *> dateTimeFormatRes =
      verifyDecoration<platform_intl::DateTimeFormat>(
          runtime,
          dateTimeFormatHandle,
          "Intl.DateTimeFormat.prototype.resolvedOptions");
  if (LLVM_UNLIKELY(dateTimeFormatRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return optionsToJS(runtime, (*dateTimeFormatRes)->resolvedOptions());
}

namespace { // NumberFormat impl stuff.

// NumberFormat internal slots.
enum class NFSlotIndexes { boundFormat, COUNT };

PseudoHandle<NativeFunction> getNFBoundFormat(
    PseudoHandle<DecoratedObject> nf,
    Runtime &runtime) {
  return createPseudoHandle(
      dyn_vmcast<NativeFunction>(
          DecoratedObject::getAdditionalSlotValue(
              nf.get(),
              runtime,
              static_cast<unsigned int>(NFSlotIndexes::boundFormat))
              .unboxToHV(runtime)));
}

void setNFBoundFormat(
    PseudoHandle<DecoratedObject> nf,
    Runtime &runtime,
    PseudoHandle<NativeFunction> value) {
  DecoratedObject::setAdditionalSlotValue(
      nf.get(),
      runtime,
      static_cast<unsigned int>(NFSlotIndexes::boundFormat),
      SmallHermesValue::encodeObjectValue(value.get(), runtime));
}

// NumberFormat format internal slots
enum class NFFormatSlotIndexes { numberFormat, COUNT };

PseudoHandle<DecoratedObject> getNumberFormat(
    PseudoHandle<NativeFunction> format,
    Runtime &runtime) {
  return createPseudoHandle(
      vmcast<DecoratedObject>(
          NativeFunction::getAdditionalSlotValue(
              format.get(),
              runtime,
              static_cast<unsigned int>(NFFormatSlotIndexes::numberFormat))
              .getObject(runtime)));
}

void setNumberFormat(
    PseudoHandle<NativeFunction> format,
    Runtime &runtime,
    PseudoHandle<DecoratedObject> value) {
  NativeFunction::setAdditionalSlotValue(
      format.get(),
      runtime,
      static_cast<unsigned int>(NFFormatSlotIndexes::numberFormat),
      SmallHermesValue::encodeObjectValue(value.get(), runtime));
}

void defineIntlNumberFormat(Runtime &runtime, Handle<JSObject> intl) {
  // Create %NumberFormatPrototype% intrinsic.  Properties will be added later.
  runtime.intlNumberFormatPrototype = JSObject::create(runtime);
  Handle<JSObject> prototype{runtime.intlNumberFormatPrototype};

  struct : public Locals {
    PinnedValue<NativeConstructor> constructor;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // Create %NumberFormat% intrinsic.
  defineIntlConstructor(
      runtime,
      Predefined::getSymbolID(Predefined::NumberFormat),
      intlNumberFormatConstructor,
      prototype,
      0,
      lv.constructor);
  runtime.intlNumberFormat.castAndSetHermesValue<NativeConstructor>(
      lv.constructor.getHermesValue());

  {
    DefinePropertyFlags dpf{};
    dpf.setValue = 1;

    defineProperty(
        runtime,
        lv.constructor,
        Predefined::getSymbolID(Predefined::prototype),
        prototype,
        dpf);
  }

  defineMethod(
      runtime,
      lv.constructor,
      Predefined::getSymbolID(Predefined::supportedLocalesOf),
      nullptr,
      intlNumberFormatSupportedLocalesOf,
      1);

  // Add properties to prototype.

  defineProperty(
      runtime,
      prototype,
      Predefined::getSymbolID(Predefined::constructor),
      lv.constructor);

  {
    auto dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
    dpf.writable = 0;
    dpf.enumerable = 0;

    defineProperty(
        runtime,
        prototype,
        Predefined::getSymbolID(Predefined::SymbolToStringTag),
        runtime.getPredefinedStringHandle(Predefined::IntlNumberFormat),
        dpf);
  }

  defineAccessor(
      runtime,
      prototype,
      Predefined::getSymbolID(Predefined::format),
      nullptr,
      intlNumberFormatPrototypeFormatGetter,
      nullptr,
      false,
      true);

#ifndef __APPLE__
  defineMethod(
      runtime,
      prototype,
      Predefined::getSymbolID(Predefined::formatToParts),
      nullptr,
      intlNumberFormatPrototypeFormatToParts,
      1);
#endif
#ifdef HERMES_USE_STATIC_INTL_PROVIDER
  defineMethod(runtime, prototype, Predefined::getSymbolID(Predefined::formatRange),
      nullptr, intlNumberFormatPrototypeFormatRange, 2);
  defineMethod(runtime, prototype, Predefined::getSymbolID(Predefined::formatRangeToParts),
      reinterpret_cast<void *>(1), intlNumberFormatPrototypeFormatRange, 2);
#endif

  defineMethod(
      runtime,
      prototype,
      Predefined::getSymbolID(Predefined::resolvedOptions),
      nullptr,
      intlNumberFormatPrototypeResolvedOptions,
      0);

  // Add NumberFormat to Intl

  defineProperty(
      runtime,
      intl,
      Predefined::getSymbolID(Predefined::NumberFormat),
      lv.constructor);
}

} // namespace

CallResult<HermesValue> intlNumberFormatConstructor(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return intlServiceConstructor<platform_intl::NumberFormat>(
      runtime,
      args,
      kNumberFormatOptions,
      Handle<NativeConstructor>::vmcast(&runtime.intlNumberFormat),
      Handle<JSObject>::vmcast(&runtime.intlNumberFormatPrototype),
      static_cast<unsigned int>(NFSlotIndexes::COUNT));
}

CallResult<HermesValue> intlNumberFormatSupportedLocalesOf(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return intlServiceSupportedLocalesOf<platform_intl::NumberFormat>(
      runtime, args);
}

CallResult<HermesValue> intlNumberFormatFormat(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto *nf = vmcast<NativeFunction>(
      runtime.getCurrentFrame()->getCalleeClosureUnsafe());
  PseudoHandle<DecoratedObject> numberFormatHandle =
      getNumberFormat(createPseudoHandle(nf), runtime);

  // Since numberFormatHandle came out of an internal slot, it's an
  // assertable failure if it has the wrong type.
  platform_intl::NumberFormat *numberFormat =
      static_cast<platform_intl::NumberFormat *>(
          numberFormatHandle->getDecoration());
  assert(numberFormat && "Intl.NumberFormat platform part is nullptr");

#ifdef HERMES_USE_STATIC_INTL_PROVIDER
  auto xRes = toIntlNumberValue(runtime, args.getArgHandle(0));
#else
  CallResult<HermesValue> xRes = toNumber_RJS(runtime, args.getArgHandle(0));
#endif
  if (LLVM_UNLIKELY(xRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
#ifdef HERMES_USE_STATIC_INTL_PROVIDER
  auto formatted = numberFormat->format(runtime, *xRes);
  if (formatted == ExecutionStatus::EXCEPTION) return formatted.getStatus();
  return StringPrimitive::createEfficient(runtime, std::move(*formatted));
#else
  return StringPrimitive::createEfficient(
      runtime, numberFormat->format(xRes->getNumber()));
#endif
}

CallResult<HermesValue> intlNumberFormatPrototypeFormatGetter(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
#ifdef HERMES_USE_STATIC_INTL_PROVIDER
  GCScope scope(runtime);
  auto unwrapped = unwrapIntlReceiver<platform_intl::NumberFormat>(
      runtime, args.getThisHandle(), Handle<NativeConstructor>(runtime.intlNumberFormat),
      "Intl.NumberFormat.prototype.format getter");
  if (unwrapped == ExecutionStatus::EXCEPTION) return unwrapped.getStatus();
  auto numberFormatHandle = runtime.makeHandle(std::move(*unwrapped));
#else
  Handle<DecoratedObject> numberFormatHandle =
      args.dyncastThis<DecoratedObject>();
#endif

  CallResult<platform_intl::NumberFormat *> numberFormatRes =
      verifyDecoration<platform_intl::NumberFormat>(
          runtime,
          numberFormatHandle,
          "Intl.NumberFormat.prototype.format getter");
  if (LLVM_UNLIKELY(numberFormatRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  PseudoHandle<NativeFunction> boundFormat =
      getNFBoundFormat(numberFormatHandle, runtime);
  if (boundFormat) {
    return boundFormat.getHermesValue();
  }

  Handle<NativeFunction> format = NativeFunction::create(
      runtime,
      runtime.functionPrototype,
      Runtime::makeNullHandle<Environment>(),
      nullptr,
      intlNumberFormatFormat,
      Predefined::getSymbolID(Predefined::emptyString),
      1,
      Runtime::makeNullHandle<JSObject>(),
      static_cast<unsigned int>(NFFormatSlotIndexes::COUNT));
  setNumberFormat(format, runtime, numberFormatHandle);

  setNFBoundFormat(numberFormatHandle, runtime, format);

  return format.getHermesValue();
}

CallResult<HermesValue> intlNumberFormatPrototypeFormatToParts(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  Handle<DecoratedObject> numberFormatHandle =
      args.dyncastThis<DecoratedObject>();

  CallResult<platform_intl::NumberFormat *> numberFormatRes =
      verifyDecoration<platform_intl::NumberFormat>(
          runtime,
          numberFormatHandle,
          "Intl.NumberFormat.prototype.formatToParts");
  if (LLVM_UNLIKELY(numberFormatRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

#ifdef HERMES_USE_STATIC_INTL_PROVIDER
  auto xRes = toIntlNumberValue(runtime, args.getArgHandle(0));
#else
  CallResult<HermesValue> xRes = toNumber_RJS(runtime, args.getArgHandle(0));
#endif
  if (LLVM_UNLIKELY(xRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
#ifdef HERMES_USE_STATIC_INTL_PROVIDER
  auto formatted = (*numberFormatRes)->formatToParts(runtime, *xRes);
  if (formatted == ExecutionStatus::EXCEPTION) return formatted.getStatus();
  return partsToJS(runtime, std::move(*formatted));
#else
  return partsToJS(
      runtime, (*numberFormatRes)->formatToParts(xRes->getNumber()));
#endif
}

#ifdef HERMES_USE_STATIC_INTL_PROVIDER
CallResult<HermesValue> intlNumberFormatPrototypeFormatRange(void *context, Runtime &runtime) {
  const auto args = runtime.getCurrentFrame().getNativeArgs();
  // Range methods require native slots, even for legacy-chained receivers.
  auto formatter = verifyDecoration<platform_intl::NumberFormat>(runtime,
      args.dyncastThis<DecoratedObject>(), "Intl.NumberFormat range method");
  if (formatter == ExecutionStatus::EXCEPTION) return formatter.getStatus();
  if (args.getArg(0).isUndefined() || args.getArg(1).isUndefined())
    return runtime.raiseTypeError("Intl number range requires both endpoints");
  auto start = toIntlNumberValue(runtime, args.getArgHandle(0));
  if (start == ExecutionStatus::EXCEPTION) return start.getStatus();
  auto end = toIntlNumberValue(runtime, args.getArgHandle(1));
  if (end == ExecutionStatus::EXCEPTION) return end.getStatus();
  // Both coercions must finish before checking NaN; the second can run JS.
  for (const auto *value : {&*start, &*end})
    if (const auto *binary = std::get_if<double>(value); binary && std::isnan(*binary))
      return runtime.raiseRangeError("Intl number range endpoint is NaN");
  if (context) {
    auto result = (*formatter)->formatRangeToParts(runtime, *start, *end);
    if (result == ExecutionStatus::EXCEPTION) return result.getStatus();
    return partsToJS(runtime, std::move(*result));
  }
  auto result = (*formatter)->formatRange(runtime, *start, *end);
  if (result == ExecutionStatus::EXCEPTION) return result.getStatus();
  return StringPrimitive::createEfficient(runtime, std::move(*result));
}
#endif

CallResult<HermesValue> intlNumberFormatPrototypeResolvedOptions(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
#ifdef HERMES_USE_STATIC_INTL_PROVIDER
  GCScope scope(runtime);
  auto unwrapped = unwrapIntlReceiver<platform_intl::NumberFormat>(
      runtime, args.getThisHandle(), Handle<NativeConstructor>(runtime.intlNumberFormat),
      "Intl.NumberFormat.prototype.resolvedOptions");
  if (unwrapped == ExecutionStatus::EXCEPTION) return unwrapped.getStatus();
  auto numberFormatHandle = runtime.makeHandle(std::move(*unwrapped));
#else
  Handle<DecoratedObject> numberFormatHandle =
      args.dyncastThis<DecoratedObject>();
#endif

  CallResult<platform_intl::NumberFormat *> numberFormatRes =
      verifyDecoration<platform_intl::NumberFormat>(
          runtime,
          numberFormatHandle,
          "Intl.NumberFormat.prototype.resolvedOptions");
  if (LLVM_UNLIKELY(numberFormatRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return optionsToJS(runtime, (*numberFormatRes)->resolvedOptions());
}

// ECMA 402 supersedes some definitionss in ECMA 262

namespace {

constexpr int kDTODate = 1 << 0;
constexpr int kDTOTime = 1 << 1;

ExecutionStatus toDateTimeOptions(
    Runtime &runtime,
    platform_intl::Options &options,
    int dtoFlags) {
  // The behavior of format with respect to default options is to
  // check if any of a set of date and time keys are present in
  // options.  If none are, then a default set of date keys is used.
  //
  // So, as long as this code sets any of the date or time keys, then
  // the default options behavior of format will not apply.

  bool needDefaults = true;
  for (const OptionData &pod : kDTFOptions) {
    if (dtoFlags & kDTODate && pod.flags & kDateRequired &&
        options.find(std::u16string(pod.name)) != options.end()) {
      needDefaults = false;
      break;
    }
    if (needDefaults) {
      if (dtoFlags & kDTOTime && pod.flags & kTimeRequired &&
          options.find(std::u16string(pod.name)) != options.end()) {
        needDefaults = false;
        break;
      }
    }
  }

  const bool hasStyle =
      options.count(u"dateStyle") > 0 || options.count(u"timeStyle") > 0;
  if (hasStyle)
    needDefaults = false;

  if (!(dtoFlags & kDTOTime) && options.count(u"timeStyle") > 0)
    return runtime.raiseTypeError("Invalid timeStyle option");

  if (!(dtoFlags & kDTODate) && options.count(u"dateStyle") > 0)
    return runtime.raiseTypeError("Invalid dateStyle option");

  if (needDefaults) {
    for (const OptionData &pod : kDTFOptions) {
      if ((dtoFlags & kDTODate && pod.flags & kDateDefault) ||
          (dtoFlags & kDTOTime && pod.flags & kTimeDefault)) {
        options.emplace(pod.name, std::u16string(u"numeric"));
      }
    }
  }

  return ExecutionStatus::RETURNED;
}

CallResult<HermesValue> intlDatePrototypeToSomeLocaleString(
    Runtime &runtime,
    const NativeArgs &args,
    JSDate *date,
    int dtoFlags) {
  double x = date->getPrimitiveValue();
  std::u16string str;
  if (std::isnan(x)) {
    str = u"Invalid Date";
  } else {
    CallResult<std::vector<std::u16string>> localesRes =
        normalizeLocales(runtime, args.getArgHandle(0));
    if (LLVM_UNLIKELY(localesRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

#ifdef HERMES_USE_STATIC_INTL_PROVIDER
    JSIntlOptionReader reader(runtime);
    if (reader.initialize(runtime, args.getArgHandle(1)) == ExecutionStatus::EXCEPTION)
      return ExecutionStatus::EXCEPTION;
    const auto required = dtoFlags == kDTODate ? u"date" : dtoFlags == kDTOTime ? u"time" : u"any";
    const auto defaults = dtoFlags == kDTODate ? u"date" : dtoFlags == kDTOTime ? u"time" : u"all";
    auto dtfRes = platform_intl::DateTimeFormat::create(runtime, *localesRes, reader, required, defaults);
#else
    CallResult<platform_intl::Options> optionsRes =
        normalizeOptions(runtime, args.getArgHandle(1), kDTFOptions);
    if (LLVM_UNLIKELY(optionsRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    if (LLVM_UNLIKELY(
            toDateTimeOptions(runtime, *optionsRes, dtoFlags) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    CallResult<std::unique_ptr<platform_intl::DateTimeFormat>> dtfRes =
        platform_intl::DateTimeFormat::create(
            runtime, *localesRes, *optionsRes);
#endif
    if (LLVM_UNLIKELY(dtfRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    // Naively, the spec requires TimeClip to be called here, but
    // since in this code path, x comes from a Date slot which has
    // already been clipped, there's no reason to do it again.
#ifdef HERMES_USE_STATIC_INTL_PROVIDER
    auto formatted = (*dtfRes)->format(runtime, x);
    if (formatted == ExecutionStatus::EXCEPTION) return formatted.getStatus();
    str = std::move(*formatted);
#else
    str = (*dtfRes)->format(x);
#endif
  }

  return StringPrimitive::createEfficient(runtime, std::move(str));
}

} // namespace

CallResult<HermesValue> intlDatePrototypeToLocaleDateString(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  JSDate *date = dyn_vmcast<JSDate>(args.getThisArg());
  if (!date) {
    return runtime.raiseTypeError(
        "Date.prototype.toLocaleString() called on non-Date object");
  }
  return intlDatePrototypeToSomeLocaleString(runtime, args, date, kDTODate);
}

CallResult<HermesValue> intlDatePrototypeToLocaleString(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  JSDate *date = dyn_vmcast<JSDate>(args.getThisArg());
  if (!date) {
    return runtime.raiseTypeError(
        "Date.prototype.toLocaleString() called on non-Date object");
  }
  return intlDatePrototypeToSomeLocaleString(
      runtime, args, date, kDTODate | kDTOTime);
}

CallResult<HermesValue> intlDatePrototypeToLocaleTimeString(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  JSDate *date = dyn_vmcast<JSDate>(args.getThisArg());
  if (!date) {
    return runtime.raiseTypeError(
        "Date.prototype.toLocaleString() called on non-Date object");
  }
  return intlDatePrototypeToSomeLocaleString(runtime, args, date, kDTOTime);
}

#ifdef HERMES_USE_STATIC_INTL_PROVIDER
namespace {
CallResult<HermesValue> formatNumericLocaleString(
    Runtime &runtime, const NativeArgs &args,
    const platform_intl::NumberFormatValue &value) {
  auto locales = normalizeLocales(runtime, args.getArgHandle(0));
  if (locales == ExecutionStatus::EXCEPTION) return locales.getStatus();
  JSIntlOptionReader reader(runtime);
  if (reader.initialize(runtime, args.getArgHandle(1)) == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;
  auto formatter = platform_intl::NumberFormat::create(runtime, *locales, reader);
  if (formatter == ExecutionStatus::EXCEPTION) return formatter.getStatus();
  auto formatted = (*formatter)->format(runtime, value);
  if (formatted == ExecutionStatus::EXCEPTION) return formatted.getStatus();
  return StringPrimitive::createEfficient(runtime, std::move(*formatted));
}
} // namespace

CallResult<HermesValue> intlBigIntPrototypeToLocaleString(Runtime &runtime) {
  const auto args = runtime.getCurrentFrame().getNativeArgs();
  auto bigint = thisBigIntValue(runtime, args.getThisHandle());
  if (bigint == ExecutionStatus::EXCEPTION) return bigint.getStatus();
  struct : Locals { PinnedValue<> value; } lv;
  LocalsRAII roots(runtime, &lv);
  lv.value = *bigint;
  auto value = toIntlNumberValue(runtime, lv.value);
  if (value == ExecutionStatus::EXCEPTION) return value.getStatus();
  return formatNumericLocaleString(runtime, args, *value);
}
#endif

CallResult<HermesValue> intlNumberPrototypeToLocaleString(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  double x;
  if (args.getThisArg().isNumber()) {
    x = args.getThisArg().getNumber();
  } else {
    auto numPtr = Handle<JSNumber>::dyn_vmcast(args.getThisHandle());
    if (LLVM_UNLIKELY(!numPtr)) {
      return runtime.raiseTypeError(
          "Number.prototype.toLocaleString() can only be used on numbers");
    }
    x = numPtr->getPrimitiveNumber();
  }

#ifdef HERMES_USE_STATIC_INTL_PROVIDER
  return formatNumericLocaleString(runtime, args, x);
#else
  CallResult<std::vector<std::u16string>> localesRes =
      normalizeLocales(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(localesRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  CallResult<platform_intl::Options> optionsRes =
      normalizeOptions(runtime, args.getArgHandle(1), kNumberFormatOptions);
  if (LLVM_UNLIKELY(optionsRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  CallResult<std::unique_ptr<platform_intl::NumberFormat>> nfRes =
      platform_intl::NumberFormat::create(runtime, *localesRes, *optionsRes);
  if (LLVM_UNLIKELY(nfRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return StringPrimitive::createEfficient(runtime, (*nfRes)->format(x));
#endif
}

CallResult<HermesValue> intlStringPrototypeLocaleCompare(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  if (args.getThisArg().isUndefined() || args.getThisArg().isNull()) {
    return runtime.raiseTypeError(
        "String.prototype.localeCompare called on null or undefined");
  }
  CallResult<std::u16string> thisRes =
      stringFromJS(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(thisRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  CallResult<std::u16string> thatRes =
      stringFromJS(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(thatRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  CallResult<std::vector<std::u16string>> localesRes =
      normalizeLocales(runtime, args.getArgHandle(1));
  if (LLVM_UNLIKELY(localesRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
#ifdef HERMES_USE_STATIC_INTL_PROVIDER
  JSIntlOptionReader reader(runtime);
  if (reader.initialize(runtime, args.getArgHandle(2)) == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;
  auto collatorRes = platform_intl::Collator::create(runtime, *localesRes, reader);
#else
  CallResult<platform_intl::Options> optionsRes =
      normalizeOptions(runtime, args.getArgHandle(2), kCollatorOptions);
  if (LLVM_UNLIKELY(optionsRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  CallResult<std::unique_ptr<platform_intl::Collator>> collatorRes =
      platform_intl::Collator::create(runtime, *localesRes, *optionsRes);
#endif
  if (LLVM_UNLIKELY(collatorRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return HermesValue::encodeTrustedNumberValue(
      (*collatorRes)->compare(*thisRes, *thatRes));
}

CallResult<HermesValue> intlStringPrototypeToLocaleLowerCase(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  if (args.getThisArg().isUndefined() || args.getThisArg().isNull()) {
    return runtime.raiseTypeError(
        "String.prototype.localeCompare called on null or undefined");
  }
  CallResult<std::u16string> thisRes =
      stringFromJS(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(thisRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  CallResult<std::vector<std::u16string>> localesRes =
      normalizeLocales(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(localesRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  CallResult<std::u16string> lowerRes =
      platform_intl::toLocaleLowerCase(runtime, *localesRes, *thisRes);
  if (LLVM_UNLIKELY(lowerRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return StringPrimitive::createEfficient(runtime, std::move(*lowerRes));
}

CallResult<HermesValue> intlStringPrototypeToLocaleUpperCase(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  if (args.getThisArg().isUndefined() || args.getThisArg().isNull()) {
    return runtime.raiseTypeError(
        "String.prototype.localeCompare called on null or undefined");
  }
  CallResult<std::u16string> thisRes =
      stringFromJS(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(thisRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  CallResult<std::vector<std::u16string>> localesRes =
      normalizeLocales(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(localesRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  CallResult<std::u16string> upperRes =
      platform_intl::toLocaleUpperCase(runtime, *localesRes, *thisRes);
  if (LLVM_UNLIKELY(upperRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return StringPrimitive::createEfficient(runtime, std::move(*upperRes));
}

} // namespace vm

namespace intl {

namespace { // Intl impl stuff.

vm::CallResult<vm::HermesValue> intlGetCanonicalLocales(
    void *,
    vm::Runtime &runtime) {
  vm::NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  vm::CallResult<std::vector<std::u16string>> localesRes =
      vm::normalizeLocales(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(localesRes == vm::ExecutionStatus::EXCEPTION)) {
    return vm::ExecutionStatus::EXCEPTION;
  }

  auto cr = platform_intl::getCanonicalLocales(runtime, *localesRes);
  if (cr == vm::ExecutionStatus::EXCEPTION) {
    return vm::ExecutionStatus::EXCEPTION;
  }
  return vm::localesToJS(runtime, std::move(cr.getValue()));
}

} // namespace

vm::HermesValue createIntlObject(vm::Runtime &runtime) {
  struct : public vm::Locals {
    vm::PinnedValue<vm::JSObject> intl;
#ifdef HERMES_USE_STATIC_INTL_PROVIDER
    vm::PinnedValue<vm::StringPrimitive> legacyDescription;
#endif
  } lv;
  vm::LocalsRAII lraii(runtime, &lv);

  lv.intl = vm::JSObject::create(runtime);
  defineMethod(
      runtime,
      lv.intl,
      vm::Predefined::getSymbolID(vm::Predefined::getCanonicalLocales),
      nullptr,
      intlGetCanonicalLocales,
      1);

  {
    auto dpf = vm::DefinePropertyFlags::getDefaultNewPropertyFlags();
    dpf.writable = 0;
    dpf.enumerable = 0;

    defineProperty(
        runtime,
        lv.intl,
        vm::Predefined::getSymbolID(vm::Predefined::SymbolToStringTag),
        runtime.getPredefinedStringHandle(vm::Predefined::Intl),
        dpf);
  }

#ifdef HERMES_USE_STATIC_INTL_PROVIDER
  lv.legacyDescription.castAndSetHermesValue<vm::StringPrimitive>(
      runtime.ignoreAllocationFailure(vm::StringPrimitive::create(
          runtime, llvh::createASCIIRef("IntlLegacyConstructedSymbol"))));
  const auto legacySymbol = runtime.ignoreAllocationFailure(
      runtime.getIdentifierTable().createNotUniquedSymbol(runtime, lv.legacyDescription));
  runtime.intlLegacyConstructedSymbol = vm::HermesValue::encodeSymbolValue(legacySymbol);
#endif
  vm::defineIntlCollator(runtime, lv.intl);
  vm::defineIntlDateTimeFormat(runtime, lv.intl);
  vm::defineIntlNumberFormat(runtime, lv.intl);
  return lv.intl.getHermesValue();
}

} // namespace intl
} // namespace hermes

#endif
