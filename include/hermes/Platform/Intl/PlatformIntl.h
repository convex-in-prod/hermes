/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_PLATFORMINTL_PLATFORMINTL_H
#define HERMES_PLATFORMINTL_PLATFORMINTL_H

#ifdef HERMES_ENABLE_INTL
#include "hermes/VM/CallResult.h"
#include "hermes/VM/DecoratedObject.h"
#include "hermes/VM/Handle.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSObject.h"
#include "hermes/VM/StringPrimitive.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <variant>

namespace hermes {
namespace platform_intl {

class Option {
 public:
  enum Kind { Bool, Number, String, BoolOrString
#ifdef HERMES_USE_STATIC_INTL_PROVIDER
    , DeferredNumber
#endif
  };

  Option(bool b) : kind_(Kind::Bool), num_(b ? 1.0 : 0.0) {}
  Option(double n) : kind_(Kind::Number), num_(n) {}
  Option(std::u16string s) : kind_(Kind::String), str_(std::move(s)) {}
#ifdef HERMES_USE_STATIC_INTL_PROVIDER
  explicit Option(vm::Handle<> raw) : kind_(Kind::DeferredNumber), raw_(raw) {}
  bool isDeferredNumber() const { return kind_ == Kind::DeferredNumber; }
  vm::Handle<> getDeferredNumber() const {
    assert(isDeferredNumber());
    return *raw_;
  }
#endif
  bool isBool() const {
    return kind_ == Kind::Bool;
  }
  bool isNumber() const {
    return kind_ == Kind::Number;
  }
  bool isString() const {
    return kind_ == Kind::String;
  }

  bool getBool() const {
    assert(isBool() && "Option is not a bool");
    return num_ == 1.0;
  }
  double getNumber() const {
    assert(isNumber() && "Option is not a number");
    return num_;
  }
  std::u16string &getString() {
    assert(isString() && "Option is not a string");
    return str_;
  }
  const std::u16string &getString() const {
    assert(isString() && "Option is not a string");
    return str_;
  }

 private:
  Kind kind_;
  // I am too lazy to make this a union.
  double num_;
  std::u16string str_;
#ifdef HERMES_USE_STATIC_INTL_PROVIDER
  // Borrowed root in the option reader's scope; never retain input options in
  // the constructed formatter. Deferred conversion can execute arbitrary JS.
  std::optional<vm::Handle<>> raw_;
#endif
};

using Options = std::unordered_map<std::u16string, Option>;
using Part = std::unordered_map<std::u16string, std::u16string>;

#ifdef HERMES_USE_STATIC_INTL_PROVIDER
class OptionReader {
 public:
  virtual ~OptionReader() = default;
  virtual vm::CallResult<std::optional<Option>> read(
      vm::Runtime &runtime, const std::u16string &name, Option::Kind kind) = 0;
};
#endif

/// The static ICU provider receives canonical locales and reads constructor
/// options through OptionReader, preserving observable reads and conversions.
/// Its deferred numeric inputs borrow GC roots only during initialization.
/// Other platform adapters still use the legacy normalized Options interface.

vm::CallResult<std::vector<std::u16string>> getCanonicalLocales(
    vm::Runtime &runtime,
    const std::vector<std::u16string> &locales);
vm::CallResult<std::u16string> toLocaleLowerCase(
    vm::Runtime &runtime,
    const std::vector<std::u16string> &locales,
    const std::u16string &str);
vm::CallResult<std::u16string> toLocaleUpperCase(
    vm::Runtime &runtime,
    const std::vector<std::u16string> &locales,
    const std::u16string &str);

enum class NativeType {
  Collator,
  DateTimeFormat,
  NumberFormat,
};

class Collator : public vm::DecoratedObject::Decoration {
 protected:
  Collator();

 public:
  ~Collator() override;

  static constexpr NativeType getNativeType() {
    return NativeType::Collator;
  }

  static vm::CallResult<std::vector<std::u16string>> supportedLocalesOf(
      vm::Runtime &runtime,
      const std::vector<std::u16string> &locales,
      const Options &options) noexcept;

  static vm::CallResult<std::unique_ptr<Collator>> create(
      vm::Runtime &runtime,
      const std::vector<std::u16string> &locales,
      const Options &options) noexcept;
#ifdef HERMES_USE_STATIC_INTL_PROVIDER
  static vm::CallResult<std::unique_ptr<Collator>> create(
      vm::Runtime &runtime,
      const std::vector<std::u16string> &locales,
      OptionReader &reader) noexcept;
#endif
  Options resolvedOptions() noexcept;

  double compare(const std::u16string &x, const std::u16string &y) noexcept;
};

class DateTimeFormat : public vm::DecoratedObject::Decoration {
 protected:
  DateTimeFormat();

 public:
  ~DateTimeFormat() override;

  static constexpr NativeType getNativeType() {
    return NativeType::DateTimeFormat;
  }

  static vm::CallResult<std::vector<std::u16string>> supportedLocalesOf(
      vm::Runtime &runtime,
      const std::vector<std::u16string> &locales,
      const Options &options) noexcept;

  static vm::CallResult<std::unique_ptr<DateTimeFormat>> create(
      vm::Runtime &runtime,
      const std::vector<std::u16string> &locales,
      const Options &options) noexcept;
#ifdef HERMES_USE_STATIC_INTL_PROVIDER
  static vm::CallResult<std::unique_ptr<DateTimeFormat>> create(
      vm::Runtime &runtime,
      const std::vector<std::u16string> &locales,
      OptionReader &reader,
      std::u16string_view required = u"any",
      std::u16string_view defaults = u"date") noexcept;
#endif
  Options resolvedOptions() noexcept;

#ifdef HERMES_USE_STATIC_INTL_PROVIDER
  vm::CallResult<std::u16string> format(vm::Runtime &runtime, double jsTimeValue) noexcept;
  vm::CallResult<std::vector<Part>> formatToParts(vm::Runtime &runtime, double jsTimeValue) noexcept;
  vm::CallResult<std::u16string> formatRange(vm::Runtime &runtime, double start, double end) noexcept;
  vm::CallResult<std::vector<Part>> formatRangeToParts(vm::Runtime &runtime, double start, double end) noexcept;
#else
  std::u16string format(double jsTimeValue) noexcept;
  std::vector<Part> formatToParts(double jsTimeValue) noexcept;
#endif
};

#ifdef HERMES_USE_STATIC_INTL_PROVIDER
using NumberFormatValue = std::variant<double, std::string>;
#endif

class NumberFormat : public vm::DecoratedObject::Decoration {
 protected:
  NumberFormat();

 public:
  ~NumberFormat() override;

  static constexpr NativeType getNativeType() {
    return NativeType::NumberFormat;
  }

  static vm::CallResult<std::vector<std::u16string>> supportedLocalesOf(
      vm::Runtime &runtime,
      const std::vector<std::u16string> &locales,
      const Options &options) noexcept;

  static vm::CallResult<std::unique_ptr<NumberFormat>> create(
      vm::Runtime &runtime,
      const std::vector<std::u16string> &locales,
      const Options &options) noexcept;
#ifdef HERMES_USE_STATIC_INTL_PROVIDER
  static vm::CallResult<std::unique_ptr<NumberFormat>> create(
      vm::Runtime &runtime,
      const std::vector<std::u16string> &locales,
      OptionReader &reader) noexcept;
#endif
  Options resolvedOptions() noexcept;

#ifdef HERMES_USE_STATIC_INTL_PROVIDER
  vm::CallResult<std::u16string> format(vm::Runtime &runtime, const NumberFormatValue &value) noexcept;
  vm::CallResult<std::vector<Part>> formatToParts(vm::Runtime &runtime, const NumberFormatValue &value) noexcept;
  vm::CallResult<std::u16string> formatRange(vm::Runtime &runtime, const NumberFormatValue &start, const NumberFormatValue &end) noexcept;
  vm::CallResult<std::vector<Part>> formatRangeToParts(vm::Runtime &runtime, const NumberFormatValue &start, const NumberFormatValue &end) noexcept;
#else
  std::u16string format(double jsTimeValue) noexcept;
  std::vector<Part> formatToParts(double jsTimeValue) noexcept;
#endif
};

} // namespace platform_intl
} // namespace hermes
#endif

#endif
