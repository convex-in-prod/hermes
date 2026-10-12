/*
 * Copyright (c) Microsoft Corporation.
 * Licensed under the MIT License.
 */

/**
 * Portable Intl implementation using a statically linked ICU provider.
 *
 * This file implements the platform_intl interface (Collator, DateTimeFormat,
 * NumberFormat, getCanonicalLocales, toLocaleLowerCase, toLocaleUpperCase)
 * using a static ICU provider. The pinned provider vtable retains the imported
 * adapter interface; newer C/C++ APIs are linked directly inside Wasm.
 *
 * The ICU provider is required; provider discovery and platform fallbacks are
 * excluded from this build.
 */

#include <unicode/localebuilder.h>
#include "unicode/ubrk.h"
#include "unicode/uclean.h"
#include "unicode/ucurr.h"
#include "unicode/uenum.h"
#include "unicode/uldnames.h"
#include "unicode/uloc.h"
#include "unicode/unorm2.h"
#include "unicode/ures.h"
#include "unicode/uset.h"
#include "unicode/ustring.h"
#include "unicode/utext.h"
#include "unicode/utypes.h"
#include "unicode/uversion.h"

#include "unicode/locid.h"

// ICU i18n headers
#include "unicode/ucal.h"
#include "unicode/ucol.h"
#include "unicode/udat.h"
#include "unicode/udateintervalformat.h"
#include "unicode/udatpg.h"
#include "unicode/ufieldpositer.h"
#include "unicode/uformattedvalue.h"
#include "unicode/ulistformatter.h"
#include "unicode/unum.h"
#include "unicode/unumberformatter.h"
#include "unicode/unumberrangeformatter.h"
#include "unicode/unumsys.h"
#include "unicode/upluralrules.h"
#include "unicode/ureldatefmt.h"

#include "unicode/numberrangeformatter.h"

#include "IcuProviderStatic.h"
#include "IcuProviderUtils.h"

#include "hermes/Platform/Intl/BCP47Parser.h"
#include "hermes/Platform/Intl/PlatformIntl.h"
#include "hermes/Platform/Intl/hermes_icu.h"

#include <algorithm>
#include <array>
#include <memory>
#include <cassert>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace ::hermes;

namespace hermes {
namespace platform_intl {

// ============================================================================
// Locale resolution infrastructure (ported from impl_icu/LocaleResolver.cpp)
// ============================================================================

namespace {

class MapOptionReader final : public OptionReader {
 public:
  explicit MapOptionReader(const Options &options) : options_(options) {}
  vm::CallResult<std::optional<Option>> read(
      vm::Runtime &, const std::u16string &name, Option::Kind) override {
    auto found = options_.find(name);
    if (found == options_.end()) return std::optional<Option>{};
    return std::optional<Option>(found->second);
  }
 private:
  const Options &options_;
};

enum class LocaleResource { Collation, Calendar, NumberElements };
using AvailableLocales = std::unordered_map<std::string, std::string>;

vm::CallResult<bool> hasLocaleResource(
    vm::Runtime &runtime, const hermes_icu_vtable *icu,
    const std::string &localeICU, const std::u16string &tag,
    const char *package, const char *key) {
  UErrorCode status = U_ZERO_ERROR;
  auto *bundle = icu->ures_open(package, localeICU.c_str(), &status);
  bool found = status == U_ZERO_ERROR && bundle && !key;
  if (status == U_ZERO_ERROR && bundle && key) {
    auto *resource = icu->ures_getByKey(bundle, key, nullptr, &status);
    if (!resource && status == U_ZERO_ERROR) {
      icu->ures_close(bundle);
      return runtime.raiseError("ICU locale resource result is missing");
    }
    found = resource && status == U_ZERO_ERROR;
    if (resource) icu->ures_close(resource);
  }
  if (bundle) icu->ures_close(bundle);
  if ((U_FAILURE(status) && status != U_MISSING_RESOURCE_ERROR) ||
      (status == U_ZERO_ERROR && !bundle))
    return runtime.raiseError("ICU locale resource lookup failed");
  if (found) return true;
  auto parsed = ParsedLocaleIdentifier::parse(tag);
  if (!parsed) return runtime.raiseError("ICU available locale is invalid");
  auto &language = parsed->languageIdentifier;
  if (!language.scriptSubtag.empty() && !language.regionSubtag.empty()) {
    language.regionSubtag.clear();
  } else if (!language.scriptSubtag.empty() || !language.regionSubtag.empty()) {
    language.scriptSubtag.clear();
    language.regionSubtag.clear();
  } else {
    return false;
  }
  auto parent = parsed->canonicalize();
  auto parentICU = convertBCP47toICULocale(runtime, icu, parent);
  if (parentICU == vm::ExecutionStatus::EXCEPTION) return parentICU.getStatus();
  return hasLocaleResource(runtime, icu, *parentICU, parent, package, key);
}

// The dataset is immutable. Cache each service's validated locale set, and
// publish it only after all ICU resource checks succeed.
vm::CallResult<const AvailableLocales *> getAvailableLocales(
    vm::Runtime &runtime, const hermes_icu_vtable *icu, LocaleResource resource) {
  static std::mutex cacheMutex;
  static std::unordered_map<const hermes_icu_vtable *,
      std::array<std::unique_ptr<AvailableLocales>, 3>> cache;
  std::lock_guard<std::mutex> lock(cacheMutex);
  auto &entry = cache[icu][static_cast<size_t>(resource)];
  if (entry) return entry.get();
  auto locales = std::make_unique<AvailableLocales>();
  const char *key = resource == LocaleResource::Calendar ? "calendar" : "NumberElements";
  const char *package = resource == LocaleResource::Collation ? U_ICUDATA_NAME "-coll" : nullptr;
  if (package) key = nullptr;
  int32_t count = package ? ucol_countAvailable() : icu->uloc_countAvailable();
  if (count <= 0) return runtime.raiseError("ICU available locale list is empty");
  for (int32_t i = 0; i < count; ++i) {
    const char *locale = package ? ucol_getAvailable(i) : icu->uloc_getAvailable(i);
    if (!locale) return runtime.raiseError("ICU available locale entry is missing");
    auto converted = convertICUtoBCP47Locale(runtime, icu, locale);
    if (converted == vm::ExecutionStatus::EXCEPTION) return converted.getStatus();
    const auto &bcp47 = *converted;
    {
      auto supported = hasLocaleResource(runtime, icu, locale, toUTF16ASCII(bcp47), package, key);
      if (supported == vm::ExecutionStatus::EXCEPTION) return supported.getStatus();
      // Match V8's resource fallback for the Norwegian Bokmal alias.
      if (!*supported && bcp47 == "nb")
        supported = hasLocaleResource(runtime, icu, "no", u"no", package, key);
      if (supported == vm::ExecutionStatus::EXCEPTION) return supported.getStatus();
      if (!*supported) continue;
    }
    locales->emplace(toLowerASCII(bcp47), bcp47);
    auto parsed = ParsedLocaleIdentifier::parse(toUTF16ASCII(bcp47));
    if (!parsed) return runtime.raiseError("ICU available locale is invalid");
    if (!parsed->languageIdentifier.scriptSubtag.empty()) {
      parsed->languageIdentifier.scriptSubtag.clear();
      const auto shortened = toUTF8ASCII(parsed->canonicalize());
      locales->emplace(toLowerASCII(shortened), shortened);
    }
  }
  entry = std::move(locales);
  return entry.get();
}

vm::CallResult<std::vector<std::u16string>> getAvailableLocalesU16(
    vm::Runtime &runtime, const hermes_icu_vtable *icu, LocaleResource resource) {
  auto available = getAvailableLocales(runtime, icu, resource);
  if (available == vm::ExecutionStatus::EXCEPTION) return available.getStatus();
  std::vector<std::u16string> result;
  for (const auto &entry : **available) result.push_back(toUTF16ASCII(entry.second));
  std::sort(result.begin(), result.end());
  return result;
}

/// ECMA-402 BestAvailableLocale (lookup algorithm).
/// Returns properly-cased BCP47 locale string from the available set.
std::string bestAvailableLocaleICU(
    const AvailableLocales &avail,
    std::u16string_view locale) {
  std::string candidate = toLowerASCII(toUTF8ASCII(std::u16string(locale)));
  while (true) {
    auto it = avail.find(candidate);
    if (it != avail.end())
      return it->second; // Return properly-cased BCP47
    size_t pos = candidate.rfind('-');
    if (pos == std::string::npos)
      return std::string();
    if (pos >= 2 && candidate[pos - 2] == '-')
      pos -= 2;
    candidate.resize(pos);
  }
}

/// Get default locale as a ParsedLocaleIdentifier.
vm::CallResult<ParsedLocaleIdentifier> getDefaultLocale(
    vm::Runtime &runtime, const hermes_icu_vtable *icu) {
  auto bcp47 = convertICUtoBCP47Locale(runtime, icu, icu->uloc_getDefault());
  if (bcp47 == vm::ExecutionStatus::EXCEPTION) return bcp47.getStatus();
  auto parsed = ParsedLocaleIdentifier::parse(toUTF16ASCII(*bcp47));
  if (!parsed) return runtime.raiseError("ICU default locale is invalid");
  return std::move(*parsed);
}

/// Get the locale string without Unicode extensions, with canonical BCP47
/// casing: language=lowercase, script=Titlecase, region=UPPERCASE.
/// ParsedLocaleIdentifier stores all subtags in lowercase, so we must
/// re-apply casing here.
std::u16string getLocaleNoExt(const ParsedLocaleIdentifier &parsed) {
  auto toUpper = [](char16_t c) -> char16_t {
    return (c >= u'a' && c <= u'z') ? (c - u'a' + u'A') : c;
  };

  // Language subtag: lowercase (already is).
  std::u16string result = parsed.languageIdentifier.languageSubtag;

  // Script subtag: Titlecase (first char uppercase, rest lowercase).
  if (!parsed.languageIdentifier.scriptSubtag.empty()) {
    const auto &script = parsed.languageIdentifier.scriptSubtag;
    result += u'-';
    result += toUpper(script[0]);
    for (size_t i = 1; i < script.size(); i++)
      result += script[i];
  }

  // Region subtag: UPPERCASE.
  if (!parsed.languageIdentifier.regionSubtag.empty()) {
    result += u'-';
    for (char16_t c : parsed.languageIdentifier.regionSubtag)
      result += toUpper(c);
  }

  // Variant subtags: lowercase (already is).
  for (const auto &variant : parsed.languageIdentifier.variantSubtagList) {
    result += u'-';
    result += variant;
  }
  return result;
}

/// Construct a canonicalized BCP47 locale with given extensions.
std::u16string buildLocaleWithExtensions(
    const std::u16string &localeNoExt,
    const std::map<std::u16string, std::u16string> &extMap) {
  if (extMap.empty())
    return localeNoExt;
  std::u16string result = localeNoExt;
  result += u"-u";
  for (const auto &[key, value] : extMap) {
    result += u'-';
    result += key;
    if (!value.empty() && value != u"true") {
      result += u'-';
      result += value;
    }
  }
  return result;
}

/// Resolved locale result from resolveLocale.
struct ResolvedLocaleResult {
  std::u16string localeNoExt;
  std::map<std::u16string, std::u16string> extensions;
  Options resolvedOpts;

  std::u16string getCanonicalizedLocaleId() const {
    return buildLocaleWithExtensions(localeNoExt, extensions);
  }
};

using ParsedLocaleList = std::vector<
    std::pair<std::u16string, std::map<std::u16string, std::u16string>>>;

/// Lookup matcher: find best locale from requested list.
vm::CallResult<ResolvedLocaleResult> lookupMatcher(
    vm::Runtime &runtime, const hermes_icu_vtable *icu,
    const ParsedLocaleList &requestedLocales, LocaleResource resource) {
  auto available = getAvailableLocales(runtime, icu, resource);
  if (available == vm::ExecutionStatus::EXCEPTION) return available.getStatus();
  for (const auto &[localeNoExt, extMap] : requestedLocales) {
    auto matched = bestAvailableLocaleICU(**available, localeNoExt);
    if (!matched.empty()) return ResolvedLocaleResult{toUTF16ASCII(matched), extMap, {}};
  }
  auto defaultLocale = getDefaultLocale(runtime, icu);
  if (defaultLocale == vm::ExecutionStatus::EXCEPTION) return defaultLocale.getStatus();
  return ResolvedLocaleResult{getLocaleNoExt(*defaultLocale), {}, {}};
}

/// ECMA-402 Table 2: Sanctioned simple unit identifiers.
bool isSanctionedSimpleUnitIdentifier(std::u16string_view unit) {
  // Keep sorted for binary search.
  static constexpr std::u16string_view sanctioned[] = {
      u"acre",
      u"bit",
      u"byte",
      u"celsius",
      u"centimeter",
      u"day",
      u"degree",
      u"fahrenheit",
      u"fluid-ounce",
      u"foot",
      u"gallon",
      u"gigabit",
      u"gigabyte",
      u"gram",
      u"hectare",
      u"hour",
      u"inch",
      u"kilobit",
      u"kilobyte",
      u"kilogram",
      u"kilometer",
      u"liter",
      u"megabit",
      u"megabyte",
      u"meter",
      u"microsecond",
      u"mile",
      u"mile-scandinavian",
      u"milliliter",
      u"millimeter",
      u"millisecond",
      u"minute",
      u"month",
      u"nanosecond",
      u"ounce",
      u"percent",
      u"petabyte",
      u"pound",
      u"second",
      u"stone",
      u"terabit",
      u"terabyte",
      u"week",
      u"yard",
      u"year",
  };
  return std::binary_search(
      std::begin(sanctioned), std::end(sanctioned), unit);
}

/// ECMA-402 §6.5 IsWellFormedUnitIdentifier.
bool isWellFormedUnitIdentifier(const std::u16string &unitIdentifier) {
  if (isSanctionedSimpleUnitIdentifier(unitIdentifier))
    return true;
  // Check for compound unit: numerator-per-denominator
  const std::u16string per = u"-per-";
  auto pos = unitIdentifier.find(per);
  if (pos == std::u16string::npos || pos == 0)
    return false;
  std::u16string_view numerator(unitIdentifier.data(), pos);
  std::u16string_view denominator(
      unitIdentifier.data() + pos + per.size(),
      unitIdentifier.size() - pos - per.size());
  return isSanctionedSimpleUnitIdentifier(numerator) &&
      isSanctionedSimpleUnitIdentifier(denominator);
}

/// Collator extension type validation callback.
vm::CallResult<bool> isCollatorExtensionTypeSupported(
    vm::Runtime &runtime, const hermes_icu_vtable *icu,
    std::u16string_view extensionKey,
    std::u16string_view extensionType,
    const std::u16string &localeNoExt) {
  static constexpr std::u16string_view validCaseFirsts[] = {
      u"upper", u"lower", u"false"};
  static constexpr std::u16string_view validNumerics[] = {u"true", u"false"};

  if (extensionKey == u"kf") {
    return std::find(
               std::begin(validCaseFirsts),
               std::end(validCaseFirsts),
               extensionType) != std::end(validCaseFirsts);
  }
  if (extensionKey == u"kn") {
    return std::find(
               std::begin(validNumerics),
               std::end(validNumerics),
               extensionType) != std::end(validNumerics);
  }
  if (extensionKey == u"co") {
    if (extensionType == u"standard" || extensionType == u"search")
      return false;

    const auto requestedType = toUTF8ASCII(std::u16string(extensionType));
    const char *legacyType = icu->uloc_toLegacyType("collation", requestedType.c_str());
    if (!legacyType) return false;

    auto icuLocale = convertBCP47toICULocale(runtime, icu, localeNoExt);
    if (icuLocale == vm::ExecutionStatus::EXCEPTION) return icuLocale.getStatus();
    UErrorCode err = U_ZERO_ERROR;
    UEnumeration *supportedTypes = icu->ucol_getKeywordValuesForLocale(
        "collation", icuLocale->c_str(), 0, &err);
    if (U_FAILURE(err) || !supportedTypes) {
      if (supportedTypes) icu->uenum_close(supportedTypes);
      return runtime.raiseError("ICU collation enumeration failed");
    }

    bool supported = false;
    // ucol_getKeywordValuesForLocale returns UTF-8 strings,
    // so use uenum_next (UTF-8) not uenum_unext (UTF-16).
    int32_t length = 0;
    err = U_ZERO_ERROR;
    const char *typeStr = icu->uenum_next(supportedTypes, &length, &err);
    while (typeStr != nullptr && U_SUCCESS(err)) {
      if (std::string_view(typeStr, length) == legacyType) {
        supported = true;
        break;
      }
      err = U_ZERO_ERROR;
      typeStr = icu->uenum_next(supportedTypes, &length, &err);
    }
    icu->uenum_close(supportedTypes);
    if (U_FAILURE(err)) return runtime.raiseError("ICU collation enumeration failed");
    return supported;
  }
  return false;
}

using ExtTypeValidator = std::function<vm::CallResult<bool>(
    std::u16string_view, /* extension key */
    std::u16string_view, /* extension type */
    const std::u16string & /* localeNoExt */)>;

/// ECMA-402 ResolveLocale algorithm.
vm::CallResult<ResolvedLocaleResult> resolveLocale(
    vm::Runtime &runtime, const hermes_icu_vtable *icu,
    const ParsedLocaleList &requestedLocales,
    const Options &opt,
    llvh::ArrayRef<std::u16string_view> relevantExtensionKeys,
    const ExtTypeValidator &isExtensionTypeSupported,
    LocaleResource resource) {
  // The reference V8 implements both modes as lookup by default. Its distinct
  // best-fit implementation remains behind an experimental runtime flag.
  auto matchResult = lookupMatcher(runtime, icu, requestedLocales, resource);
  if (matchResult == vm::ExecutionStatus::EXCEPTION) return matchResult.getStatus();
  auto &matched = *matchResult;

  std::map<std::u16string, std::u16string> supportedExts;
  for (const auto &[key, val] : matched.extensions) {
    std::u16string_view extType{val};
    if (extType.empty())
      extType = u"true";
    if (std::find(relevantExtensionKeys.begin(),
                  relevantExtensionKeys.end(),
                  key) != relevantExtensionKeys.end()) {
      auto supported = isExtensionTypeSupported(key, extType, matched.localeNoExt);
      if (supported == vm::ExecutionStatus::EXCEPTION) return supported.getStatus();
      if (*supported) supportedExts.emplace(key, extType);
    }
  }

  Options resolvedOpts;
  for (const auto &extView : relevantExtensionKeys) {
    std::u16string ext{extView};
    auto extIter = supportedExts.find(ext);
    if (extIter != supportedExts.end()) {
      resolvedOpts.emplace(ext, extIter->second);
    }
    auto optIter = opt.find(ext);
    if (optIter != opt.end()) {
      std::u16string_view optExtType;
      const Option &option = optIter->second;
      if (option.isBool()) {
        optExtType = option.getBool() ? u"true" : u"false";
      } else {
        optExtType = option.getString();
      }
      auto supported = isExtensionTypeSupported(ext, optExtType, matched.localeNoExt);
      if (supported == vm::ExecutionStatus::EXCEPTION) return supported.getStatus();
      if (*supported) {
        resolvedOpts.insert_or_assign(ext, optIter->second);
        if (extIter != supportedExts.end() &&
            extIter->second != optExtType) {
          supportedExts.erase(extIter);
        }
      }
    }
  }

  matched.extensions = supportedExts;
  matched.resolvedOpts = resolvedOpts;
  return matched;
}

/// Parse locale list into (localeNoExt, extensionMap) pairs.
vm::CallResult<ParsedLocaleList> parseLocaleList(
    vm::Runtime &runtime,
    const hermes_icu_vtable *icu,
    const std::vector<std::u16string> &locales) {
  ParsedLocaleList result;
  for (const auto &locale : locales) {
    auto normalized = normalizeLanguageTag(runtime, icu, locale);
    if (LLVM_UNLIKELY(normalized == vm::ExecutionStatus::EXCEPTION))
      return vm::ExecutionStatus::EXCEPTION;
    auto parsed = ParsedLocaleIdentifier::parse(normalized.getValue());
    if (parsed) {
      std::u16string noExt = getLocaleNoExt(*parsed);
      result.push_back({noExt, parsed->unicodeExtensionKeywords});
    }
  }
  return result;
}

vm::CallResult<ResolvedLocaleResult> resolveFormatterLocale(
    vm::Runtime &runtime,
    const hermes_icu_vtable *icu,
    const std::vector<std::u16string> &locales,
    Options &options,
    OptionReader &reader,
    llvh::ArrayRef<std::u16string_view> keys) {
  auto requested = parseLocaleList(runtime, icu, locales);
  if (requested == vm::ExecutionStatus::EXCEPTION) return requested.getStatus();
  auto matcher = getOptionString(runtime, reader, options, u"localeMatcher",
      {u"lookup", u"best fit"}, u"best fit");
  if (matcher == vm::ExecutionStatus::EXCEPTION) return matcher.getStatus();
  Options opt;
  opt.emplace(u"localeMatcher", Option(*matcher));
  for (const auto &key : keys) {
    const auto name = key == u"nu" ? u"numberingSystem" : key == u"ca" ? u"calendar" : u"hourCycle";
    if (key == u"hc" && readOption(runtime, reader, options, u"hour12", Option::Kind::Bool) ==
        vm::ExecutionStatus::EXCEPTION) return vm::ExecutionStatus::EXCEPTION;
    if (readOption(runtime, reader, options, name, Option::Kind::String) ==
        vm::ExecutionStatus::EXCEPTION) return vm::ExecutionStatus::EXCEPTION;
    auto found = options.find(name);
    if (found == options.end()) continue;
    const auto &value = found->second.getString();
    if (key == u"hc") {
      if (value != u"h11" && value != u"h12" && value != u"h23" && value != u"h24")
        return runtime.raiseRangeError("Invalid hourCycle");
    } else if (!isUnicodeExtensionType(value)) {
      return runtime.raiseRangeError("Invalid Unicode locale option");
    }
    opt.emplace(std::u16string(key), Option(value));
  }
  auto supported = [&](std::u16string_view key, std::u16string_view type,
                       const std::u16string &locale) -> vm::CallResult<bool> {
    if (key == u"hc")
      return type == u"h11" || type == u"h12" || type == u"h23" || type == u"h24";
    if (key == u"nu") {
      const auto name = toUTF8ASCII(std::u16string(type));
      UErrorCode status = U_ZERO_ERROR;
      auto *system = icu->unumsys_openByName(name.c_str(), &status);
      const bool found = U_SUCCESS(status) && system &&
          !icu->unumsys_isAlgorithmic(system) && icu->unumsys_getRadix(system) == 10;
      if (system) icu->unumsys_close(system);
      if ((U_FAILURE(status) && status != U_UNSUPPORTED_ERROR && status != U_MISSING_RESOURCE_ERROR) ||
          (U_SUCCESS(status) && !system))
        return runtime.raiseError("ICU numbering system lookup failed");
      return found;
    }
    auto localeICU = convertBCP47toICULocale(runtime, icu, locale);
    if (localeICU == vm::ExecutionStatus::EXCEPTION) return localeICU.getStatus();
    UErrorCode status = U_ZERO_ERROR;
    auto *values = icu->ucal_getKeywordValuesForLocale("calendar", localeICU->c_str(), false, &status);
    if (U_FAILURE(status) || !values) {
      if (values) icu->uenum_close(values);
      return runtime.raiseError("ICU calendar enumeration failed");
    }
    bool found = false;
    int32_t length;
    while (const char *value = icu->uenum_next(values, &length, &status)) {
      const auto *canonical = icu->uloc_toUnicodeLocaleType("ca", value);
      if (canonical && toUTF16ASCII(canonical) == type) found = true;
    }
    icu->uenum_close(values);
    if (U_FAILURE(status)) return runtime.raiseError("ICU calendar enumeration failed");
    return found;
  };
  auto resolved = resolveLocale(runtime, icu, *requested, opt, keys, supported,
      std::find(keys.begin(), keys.end(), u"ca") != keys.end()
          ? LocaleResource::Calendar : LocaleResource::NumberElements);
  if (resolved == vm::ExecutionStatus::EXCEPTION) return resolved.getStatus();
  if (options.count(u"hour12")) {
    resolved->extensions.erase(u"hc");
    resolved->resolvedOpts.erase(u"hc");
  }
  return resolved;
}

} // anonymous namespace

// ============================================================================
// getCanonicalLocales
// ============================================================================

vm::CallResult<std::vector<std::u16string>> getCanonicalLocales(
    vm::Runtime &runtime,
    const std::vector<std::u16string> &locales) {
  const hermes_icu_vtable *icu = getIcuVtableForRuntime(runtime);
  return canonicalizeLocaleList(runtime, icu, locales);
}

// ============================================================================
// toLocaleLowerCase / toLocaleUpperCase
// ============================================================================

vm::CallResult<std::u16string> toLocaleLowerCase(
    vm::Runtime &runtime,
    const std::vector<std::u16string> &locales,
    const std::u16string &str) {
  const hermes_icu_vtable *icu = getIcuVtableForRuntime(runtime);

  std::string locale8;
  if (!locales.empty()) {
    auto conv = UTF16toUTF8(runtime, locales.front());
    if (LLVM_UNLIKELY(conv == vm::ExecutionStatus::EXCEPTION))
      return vm::ExecutionStatus::EXCEPTION;
    locale8 = std::move(conv.getValue());
  }

  UErrorCode err = U_ZERO_ERROR;
  int32_t resultLength = icu->u_strToLower(
      nullptr,
      0,
      reinterpret_cast<const UChar *>(str.data()),
      static_cast<int32_t>(str.length()),
      locale8.empty() ? nullptr : locale8.c_str(),
      &err);
  if (U_FAILURE(err) && err != U_BUFFER_OVERFLOW_ERROR) {
    return runtime.raiseRangeError("toLocaleLowerCase failed");
  }

  std::u16string result(resultLength, u' ');
  err = U_ZERO_ERROR;
  resultLength = icu->u_strToLower(
      reinterpret_cast<UChar *>(&result[0]),
      resultLength,
      reinterpret_cast<const UChar *>(str.data()),
      static_cast<int32_t>(str.length()),
      locale8.empty() ? nullptr : locale8.c_str(),
      &err);
  if (U_FAILURE(err)) {
    return runtime.raiseRangeError("toLocaleLowerCase failed");
  }
  result.resize(resultLength);
  return result;
}

vm::CallResult<std::u16string> toLocaleUpperCase(
    vm::Runtime &runtime,
    const std::vector<std::u16string> &locales,
    const std::u16string &str) {
  const hermes_icu_vtable *icu = getIcuVtableForRuntime(runtime);

  std::string locale8;
  if (!locales.empty()) {
    auto conv = UTF16toUTF8(runtime, locales.front());
    if (LLVM_UNLIKELY(conv == vm::ExecutionStatus::EXCEPTION))
      return vm::ExecutionStatus::EXCEPTION;
    locale8 = std::move(conv.getValue());
  }

  UErrorCode err = U_ZERO_ERROR;
  int32_t resultLength = icu->u_strToUpper(
      nullptr,
      0,
      reinterpret_cast<const UChar *>(str.data()),
      static_cast<int32_t>(str.length()),
      locale8.empty() ? nullptr : locale8.c_str(),
      &err);
  if (U_FAILURE(err) && err != U_BUFFER_OVERFLOW_ERROR) {
    return runtime.raiseRangeError("toLocaleUpperCase failed");
  }

  std::u16string result(resultLength, u' ');
  err = U_ZERO_ERROR;
  resultLength = icu->u_strToUpper(
      reinterpret_cast<UChar *>(&result[0]),
      resultLength,
      reinterpret_cast<const UChar *>(str.data()),
      static_cast<int32_t>(str.length()),
      locale8.empty() ? nullptr : locale8.c_str(),
      &err);
  if (U_FAILURE(err)) {
    return runtime.raiseRangeError("toLocaleUpperCase failed");
  }
  result.resize(resultLength);
  return result;
}

// ============================================================================
// Collator (static ICU provider)
// ============================================================================

namespace {
class CollatorIcuProvider : public Collator {
 public:
  ~CollatorIcuProvider() override {
    if (coll_) icu_->ucol_close(coll_);
  }
  vm::ExecutionStatus initialize(vm::Runtime &runtime, const hermes_icu_vtable *icu,
      const std::vector<std::u16string> &locales, OptionReader &reader) noexcept;
  Options resolvedOptions() noexcept { return resolved_; }
  double compare(const std::u16string &x, const std::u16string &y) noexcept {
    assert(coll_ && "Only initialized collators may be published");
    return icu_->ucol_strcoll(coll_, reinterpret_cast<const UChar *>(x.data()), x.size(),
        reinterpret_cast<const UChar *>(y.data()), y.size());
  }
 private:
  const hermes_icu_vtable *icu_ = nullptr;
  UCollator *coll_ = nullptr;
  Options resolved_;
};

vm::ExecutionStatus CollatorIcuProvider::initialize(
    vm::Runtime &runtime, const hermes_icu_vtable *icu,
    const std::vector<std::u16string> &locales, OptionReader &reader) noexcept {
  icu_ = icu;
  Options options;
  auto requested = parseLocaleList(runtime, icu, locales);
  if (requested == vm::ExecutionStatus::EXCEPTION) return requested.getStatus();
  auto usage = getOptionString(runtime, reader, options, u"usage", {u"sort", u"search"}, u"sort");
  if (usage == vm::ExecutionStatus::EXCEPTION) return usage.getStatus();
  auto matcher = getOptionString(runtime, reader, options, u"localeMatcher", {u"lookup", u"best fit"}, u"best fit");
  if (matcher == vm::ExecutionStatus::EXCEPTION) return matcher.getStatus();
  auto collation = getOptionString(runtime, reader, options, u"collation", {}, {});
  if (collation == vm::ExecutionStatus::EXCEPTION) return collation.getStatus();
  if (options.count(u"collation") && !isUnicodeExtensionType(*collation))
    return runtime.raiseRangeError("Invalid collation option");
  if (readOption(runtime, reader, options, u"numeric", Option::Kind::Bool) == vm::ExecutionStatus::EXCEPTION)
    return vm::ExecutionStatus::EXCEPTION;
  auto numeric = getOptionBool(runtime, options, u"numeric", {});
  auto caseFirst = getOptionString(runtime, reader, options, u"caseFirst", {u"upper", u"lower", u"false"}, {});
  if (caseFirst == vm::ExecutionStatus::EXCEPTION) return caseFirst.getStatus();
  auto supported = [&](std::u16string_view key, std::u16string_view value, const std::u16string &locale) {
    return isCollatorExtensionTypeSupported(runtime, icu, key, value, locale);
  };
  static constexpr std::u16string_view keys[] = {u"co", u"kn", u"kf"};
  // The reference V8 resolves requested extensions first, then applies options
  // to the native collator. Option overrides do not rewrite the reported
  // requested locale when ICU returns a different valid locale.
  auto resolved = resolveLocale(runtime, icu, *requested, {}, keys, supported, LocaleResource::Collation);
  if (resolved == vm::ExecutionStatus::EXCEPTION) return resolved.getStatus();
  UErrorCode status = U_ZERO_ERROR;
  auto locale = icu::Locale::forLanguageTag(toUTF8ASCII(resolved->getCanonicalizedLocaleId()), status);
  if (U_FAILURE(status) || locale.isBogus()) return runtime.raiseError("ICU collator locale conversion failed");
  const auto foundCollation = resolved->extensions.find(u"co");
  if (options.count(u"collation") && foundCollation != resolved->extensions.end() && foundCollation->second != *collation)
    locale.setUnicodeKeywordValue("co", nullptr, status);
  if (*usage == u"search") locale.setUnicodeKeywordValue("co", "search", status);
  else if (options.count(u"collation")) {
    auto valid = supported(u"co", *collation, resolved->localeNoExt);
    if (valid == vm::ExecutionStatus::EXCEPTION) return valid.getStatus();
    if (*valid) locale.setUnicodeKeywordValue("co", toUTF8ASCII(*collation), status);
  }
  if (U_FAILURE(status)) return runtime.raiseError("ICU collator locale configuration failed");
  coll_ = icu->ucol_open(locale.getName(), &status);
  // ICU's normal locale fallback is allowed; an allocation/data failure must
  // never become an unrelated root collator or a binary string comparison.
  if (U_FAILURE(status) || !coll_) return runtime.raiseRangeError("ICU collator initialization failed");
  const char *validName = icu->ucol_getLocaleByType(coll_, ULOC_VALID_LOCALE, &status);
  if (U_FAILURE(status) || !validName) return runtime.raiseError("ICU collator locale retrieval failed");
  icu::Locale validLocale(validName);
  const bool preserveRequestedLocale = validLocale != locale;
  const auto legacyCollation = validLocale.getKeywordValue<std::string>("collation", status);
  if (U_FAILURE(status)) return runtime.raiseError("ICU collator collation retrieval failed");
  std::string validCollation;
  // An absent keyword is an ordinary default. ICU's Unicode-keyword accessor
  // treats that absence as an invalid value, so check the legacy value first.
  if (!legacyCollation.empty()) {
    const char *canonical = icu->uloc_toUnicodeLocaleType("co", legacyCollation.c_str());
    if (!canonical) return runtime.raiseError("ICU returned an invalid collation keyword");
    validCollation = canonical;
  }
  const bool search = validCollation == "search";
  if (search) validLocale.setUnicodeKeywordValue("co", nullptr, status);
  const auto validTag = validLocale.toLanguageTag<std::string>(status);
  if (U_FAILURE(status)) return runtime.raiseError("ICU collator locale serialization failed");
  resolved_.emplace(u"locale", preserveRequestedLocale ? resolved->getCanonicalizedLocaleId() : toUTF16ASCII(validTag));
  resolved_.emplace(u"usage", std::u16string(search ? u"search" : u"sort"));
  resolved_.emplace(u"collation", search || validCollation.empty() ? u"default" : toUTF16ASCII(validCollation));

  if (!numeric) {
    const auto found = resolved->extensions.find(u"kn");
    if (found != resolved->extensions.end()) numeric = found->second == u"true";
  }
  if (numeric) icu->ucol_setAttribute(coll_, ::UCOL_NUMERIC_COLLATION, *numeric ? ::UCOL_ON : ::UCOL_OFF, &status);
  if (caseFirst->empty()) {
    const auto found = resolved->extensions.find(u"kf");
    if (found != resolved->extensions.end()) *caseFirst = found->second;
  }
  if (!caseFirst->empty()) icu->ucol_setAttribute(coll_, ::UCOL_CASE_FIRST,
      *caseFirst == u"upper" ? ::UCOL_UPPER_FIRST : *caseFirst == u"lower" ? ::UCOL_LOWER_FIRST : ::UCOL_OFF, &status);
  icu->ucol_setAttribute(coll_, ::UCOL_NORMALIZATION_MODE, ::UCOL_ON, &status);
  if (U_FAILURE(status)) return runtime.raiseError("ICU collator attribute configuration failed");

  auto sensitivity = getOptionString(runtime, reader, options, u"sensitivity", {u"base", u"accent", u"case", u"variant"}, {});
  if (sensitivity == vm::ExecutionStatus::EXCEPTION) return sensitivity.getStatus();
  if (sensitivity->empty() && *usage == u"sort") *sensitivity = u"variant";
  if (!sensitivity->empty()) icu->ucol_setAttribute(coll_, ::UCOL_STRENGTH,
      *sensitivity == u"base" || *sensitivity == u"case" ? ::UCOL_PRIMARY :
      *sensitivity == u"accent" ? ::UCOL_SECONDARY : ::UCOL_TERTIARY, &status);
  if (*sensitivity == u"case") icu->ucol_setAttribute(coll_, ::UCOL_CASE_LEVEL, ::UCOL_ON, &status);
  if (U_FAILURE(status)) return runtime.raiseError("ICU collator sensitivity configuration failed");
  if (readOption(runtime, reader, options, u"ignorePunctuation", Option::Kind::Bool) == vm::ExecutionStatus::EXCEPTION)
    return vm::ExecutionStatus::EXCEPTION;
  // Preserve ICU's locale-specific punctuation default when the option is absent.
  auto punctuation = getOptionBool(runtime, options, u"ignorePunctuation", {});
  if (punctuation) icu->ucol_setAttribute(coll_, ::UCOL_ALTERNATE_HANDLING,
      *punctuation ? ::UCOL_SHIFTED : ::UCOL_NON_IGNORABLE, &status);
  auto actualNumeric = icu->ucol_getAttribute(coll_, ::UCOL_NUMERIC_COLLATION, &status);
  auto actualCase = icu->ucol_getAttribute(coll_, ::UCOL_CASE_FIRST, &status);
  auto actualStrength = icu->ucol_getAttribute(coll_, ::UCOL_STRENGTH, &status);
  auto actualCaseLevel = icu->ucol_getAttribute(coll_, ::UCOL_CASE_LEVEL, &status);
  auto actualPunctuation = icu->ucol_getAttribute(coll_, ::UCOL_ALTERNATE_HANDLING, &status);
  if (U_FAILURE(status)) return runtime.raiseError("ICU collator attribute retrieval failed");
  resolved_.emplace(u"numeric", actualNumeric == ::UCOL_ON);
  resolved_.emplace(u"caseFirst", std::u16string(actualCase == ::UCOL_UPPER_FIRST ? u"upper" : actualCase == ::UCOL_LOWER_FIRST ? u"lower" : u"false"));
  resolved_.emplace(u"sensitivity", std::u16string(actualStrength == ::UCOL_PRIMARY ? (actualCaseLevel == ::UCOL_ON ? u"case" : u"base") :
      actualStrength == ::UCOL_SECONDARY ? u"accent" : u"variant"));
  resolved_.emplace(u"ignorePunctuation", actualPunctuation == ::UCOL_SHIFTED);
  return vm::ExecutionStatus::RETURNED;
}
} // namespace

Collator::Collator() = default;
Collator::~Collator() = default;

vm::CallResult<std::vector<std::u16string>> Collator::supportedLocalesOf(
    vm::Runtime &runtime, const std::vector<std::u16string> &locales,
    const Options &options) noexcept {
  const auto *icu = getIcuVtableForRuntime(runtime);
  auto available = getAvailableLocalesU16(runtime, icu, LocaleResource::Collation);
  if (available == vm::ExecutionStatus::EXCEPTION) return available.getStatus();
  auto requested = getCanonicalLocales(runtime, locales);
  if (requested == vm::ExecutionStatus::EXCEPTION) return requested.getStatus();
  return supportedLocales(runtime, *available, *requested, options);
}

vm::CallResult<std::unique_ptr<Collator>> Collator::create(
    vm::Runtime &runtime, const std::vector<std::u16string> &locales,
    const Options &options) noexcept {
  MapOptionReader reader(options);
  return create(runtime, locales, reader);
}

vm::CallResult<std::unique_ptr<Collator>> Collator::create(
    vm::Runtime &runtime, const std::vector<std::u16string> &locales,
    OptionReader &reader) noexcept {
  auto instance = std::make_unique<CollatorIcuProvider>();
  if (instance->initialize(runtime, getIcuVtableForRuntime(runtime), locales, reader) == vm::ExecutionStatus::EXCEPTION)
    return vm::ExecutionStatus::EXCEPTION;
  return instance;
}

Options Collator::resolvedOptions() noexcept {
  return static_cast<CollatorIcuProvider *>(this)->resolvedOptions();
}

double Collator::compare(const std::u16string &x, const std::u16string &y) noexcept {
  return static_cast<CollatorIcuProvider *>(this)->compare(x, y);
}

// ============================================================================
// DateTimeFormat (static ICU provider)
// ============================================================================

namespace {
vm::CallResult<std::u16string> readDatePattern(
    vm::Runtime &runtime, const hermes_icu_vtable *icu, UDateFormat *formatter) {
  UErrorCode status = U_ZERO_ERROR;
  const int32_t length = icu->udat_toPattern(formatter, false, nullptr, 0, &status);
  if (status != U_BUFFER_OVERFLOW_ERROR || length <= 0)
    return runtime.raiseError("ICU date-pattern retrieval failed");
  std::u16string pattern(length, u'\0');
  status = U_ZERO_ERROR;
  icu->udat_toPattern(formatter, false,
      reinterpret_cast<UChar *>(pattern.data()), length, &status);
  if (U_FAILURE(status))
    return runtime.raiseError("ICU date-pattern retrieval failed");
  return pattern;
}

vm::CallResult<std::u16string> readDateSkeleton(
    vm::Runtime &runtime, const hermes_icu_vtable *icu, const std::u16string &pattern) {
  UErrorCode status = U_ZERO_ERROR;
  const int32_t length = icu->udatpg_getSkeleton(nullptr,
      reinterpret_cast<const UChar *>(pattern.data()), pattern.size(), nullptr, 0, &status);
  if (status != U_BUFFER_OVERFLOW_ERROR || length <= 0)
    return runtime.raiseError("ICU date-skeleton retrieval failed");
  std::u16string skeleton(length, u'\0');
  status = U_ZERO_ERROR;
  icu->udatpg_getSkeleton(nullptr,
      reinterpret_cast<const UChar *>(pattern.data()), pattern.size(),
      reinterpret_cast<UChar *>(skeleton.data()), length, &status);
  if (U_FAILURE(status)) return runtime.raiseError("ICU date-skeleton retrieval failed");
  return skeleton;
}

char16_t hourCycleSymbol(const std::u16string &cycle) {
  if (cycle == u"h11") return u'K';
  if (cycle == u"h12") return u'h';
  if (cycle == u"h23") return u'H';
  assert(cycle == u"h24");
  return u'k';
}

// ICU may normalize K/k skeletons to h/H. Restore the requested hour cycle
// after pattern selection, without modifying quoted literal text.
std::u16string replacePatternHourCycle(
    const std::u16string &pattern, char16_t replacement) {
  std::u16string result;
  bool quoted = false;
  char16_t previous = 0;
  for (char16_t c : pattern) {
    if (c == u'\'') quoted = !quoted;
    if (!quoted && (c == u'h' || c == u'H' || c == u'K' || c == u'k')) {
      if (previous == u'd') result += u' ';
      result += replacement;
    } else result += c;
    previous = c;
  }
  return result;
}
} // namespace

namespace {
class DateTimeFormatIcuProvider : public DateTimeFormat {
 public:
  DateTimeFormatIcuProvider() = default;
  ~DateTimeFormatIcuProvider();

  vm::ExecutionStatus initialize(
      vm::Runtime &runtime,
      const hermes_icu_vtable *icu,
      const std::vector<std::u16string> &locales,
      OptionReader &reader,
      std::u16string_view required,
      std::u16string_view defaults) noexcept;

  Options resolvedOptions() noexcept;
  vm::CallResult<std::u16string> format(vm::Runtime &runtime, double jsTimeValue) noexcept;
  vm::CallResult<std::vector<Part>> formatToParts(vm::Runtime &runtime, double x) noexcept;
  vm::CallResult<std::u16string> formatRange(vm::Runtime &runtime, double start, double end) noexcept;
  vm::CallResult<std::vector<Part>> formatRangeToParts(vm::Runtime &runtime, double start, double end) noexcept;

 private:
  vm::CallResult<const UFormattedValue *> formatRangeValue(vm::Runtime &runtime, double start, double end) noexcept;
  UDateIntervalFormat *interval_ = nullptr;
  UFormattedDateInterval *intervalResult_ = nullptr;
  std::u16string locale_;
  std::u16string timeZone_;
  std::u16string weekday_;
  std::u16string era_;
  std::u16string year_;
  std::u16string month_;
  std::u16string day_;
  std::u16string dayPeriod_;
  std::u16string hour_;
  std::u16string minute_;
  std::u16string second_;
  std::u16string timeZoneName_;
  std::u16string dateStyle_;
  std::u16string timeStyle_;
  std::u16string calendar_;
  std::u16string numberingSystem_;
  std::u16string hourCycle_;
  int fractionalSecondDigits_ = 0;
  UDateFormat *dtf_ = nullptr;
  std::string locale8_;
  const hermes_icu_vtable *icu_ = nullptr;
  UDateFormat *getUDateFormatter(vm::Runtime &runtime);
  vm::CallResult<std::u16string> getDefaultHourCycle(vm::Runtime &runtime);
};
} // namespace

DateTimeFormatIcuProvider::~DateTimeFormatIcuProvider() {
  if (intervalResult_) icu_->udtitvfmt_closeResult(intervalResult_);
  if (interval_) icu_->udtitvfmt_close(interval_);
  if (dtf_ && icu_) {
    icu_->udat_close(dtf_);
  }
}

DateTimeFormat::DateTimeFormat() = default;
DateTimeFormat::~DateTimeFormat() = default;

vm::CallResult<std::vector<std::u16string>> DateTimeFormat::supportedLocalesOf(
    vm::Runtime &runtime,
    const std::vector<std::u16string> &locales,
    const Options &options) noexcept {
  const hermes_icu_vtable *icu = getIcuVtableForRuntime(runtime);

  auto availableLocales = getAvailableLocalesU16(runtime, icu, LocaleResource::Calendar);
  if (availableLocales == vm::ExecutionStatus::EXCEPTION) return availableLocales.getStatus();
  auto requestedLocales = getCanonicalLocales(runtime, locales);
  if (LLVM_UNLIKELY(requestedLocales == vm::ExecutionStatus::EXCEPTION))
    return vm::ExecutionStatus::EXCEPTION;
  return supportedLocales(
      runtime, *availableLocales, requestedLocales.getValue(), options);
}

vm::ExecutionStatus DateTimeFormatIcuProvider::initialize(
    vm::Runtime &runtime,
    const hermes_icu_vtable *icu,
    const std::vector<std::u16string> &locales,
    OptionReader &reader,
    std::u16string_view required,
    std::u16string_view defaults) noexcept {
  icu_ = icu;
  if (!icu_) {
    (void)runtime.raiseRangeError("DateTimeFormat requires ICU support");
    return vm::ExecutionStatus::EXCEPTION;
  }

  Options options;
  static constexpr std::u16string_view keys[]{u"ca", u"nu", u"hc"};
  auto resolved = resolveFormatterLocale(runtime, icu_, locales, options, reader, keys);
  if (resolved == vm::ExecutionStatus::EXCEPTION) return resolved.getStatus();
  locale_ = resolved->getCanonicalizedLocaleId();
  auto localeConversion = convertBCP47toICULocale(runtime, icu_, locale_);
  if (localeConversion == vm::ExecutionStatus::EXCEPTION) return localeConversion.getStatus();
  locale8_ = std::move(*localeConversion);
  for (const auto &entry : resolved->resolvedOpts) {
    if (entry.first == u"ca") calendar_ = entry.second.getString();
    else if (entry.first == u"nu") numberingSystem_ = entry.second.getString();
    else if (entry.first == u"hc") hourCycle_ = entry.second.getString();
  }
  auto hour12 = getOptionBool(runtime, options, u"hour12", {});

  if (readOption(runtime, reader, options, u"timeZone", Option::Kind::String) ==
      vm::ExecutionStatus::EXCEPTION) return vm::ExecutionStatus::EXCEPTION;
  auto timeZoneRes = options.find(u"timeZone");
  if (timeZoneRes != options.end()) {
    std::u16string tz(timeZoneRes->second.getString());
    if (tz.empty()) {
      return runtime.raiseRangeError("Invalid time zone");
    }
    // Try to validate and canonicalize the timezone via ICU.
    auto tzResult = validateAndCanonicalizeTimeZone(runtime, icu_, tz);
    if (tzResult.getStatus() == vm::ExecutionStatus::EXCEPTION)
      return vm::ExecutionStatus::EXCEPTION;
    timeZone_ = tzResult.getValue();
  }
  // If no timezone was specified, get the system default timezone.
  if (timeZone_.empty()) {
    UChar tzBuf[128];
    UErrorCode tzErr = U_ZERO_ERROR;
    int32_t tzLen =
        icu_->ucal_getDefaultTimeZone(tzBuf, 128, &tzErr);
    if (U_FAILURE(tzErr) || tzLen <= 0 || tzLen > 128)
      return runtime.raiseError("ICU default time-zone resolution failed");
    {
      timeZone_.assign(
          reinterpret_cast<const char16_t *>(tzBuf), tzLen);
      // Canonicalize common aliases to "UTC".
      if (timeZone_ == u"Etc/UTC" || timeZone_ == u"Etc/GMT" ||
          timeZone_ == u"GMT") {
        timeZone_ = u"UTC";
      }
    }
  }

  static const std::vector<std::u16string> weekdayValues = {
      u"narrow", u"short", u"long"};
  auto weekdayRes =
      getOptionString(runtime, reader, options, u"weekday", weekdayValues, {});
  if (weekdayRes.getStatus() == vm::ExecutionStatus::EXCEPTION)
    return weekdayRes.getStatus();
  weekday_ = weekdayRes.getValue();

  static const std::vector<std::u16string> eraValues = {
      u"narrow", u"short", u"long"};
  auto eraRes = getOptionString(runtime, reader, options, u"era", eraValues, {});
  if (eraRes.getStatus() == vm::ExecutionStatus::EXCEPTION)
    return eraRes.getStatus();
  era_ = *eraRes;

  static const std::vector<std::u16string> yearValues = {
      u"2-digit", u"numeric"};
  auto yearRes = getOptionString(runtime, reader, options, u"year", yearValues, {});
  if (yearRes.getStatus() == vm::ExecutionStatus::EXCEPTION)
    return yearRes.getStatus();
  year_ = *yearRes;

  static const std::vector<std::u16string> monthValues = {
      u"2-digit", u"numeric", u"narrow", u"short", u"long"};
  auto monthRes = getOptionString(runtime, reader, options, u"month", monthValues, {});
  if (monthRes.getStatus() == vm::ExecutionStatus::EXCEPTION)
    return monthRes.getStatus();
  month_ = *monthRes;

  static const std::vector<std::u16string> dayValues = {
      u"2-digit", u"numeric"};
  auto dayRes = getOptionString(runtime, reader, options, u"day", dayValues, {});
  if (dayRes.getStatus() == vm::ExecutionStatus::EXCEPTION)
    return dayRes.getStatus();
  day_ = *dayRes;

  static const std::vector<std::u16string> dayPeriodValues = {
      u"narrow", u"short", u"long"};
  auto dayPeriodRes =
      getOptionString(runtime, reader, options, u"dayPeriod", dayPeriodValues, {});
  if (dayPeriodRes.getStatus() == vm::ExecutionStatus::EXCEPTION)
    return dayPeriodRes.getStatus();
  dayPeriod_ = *dayPeriodRes;

  static const std::vector<std::u16string> hourValues = {
      u"2-digit", u"numeric"};
  auto hourRes = getOptionString(runtime, reader, options, u"hour", hourValues, {});
  if (hourRes.getStatus() == vm::ExecutionStatus::EXCEPTION)
    return hourRes.getStatus();
  hour_ = *hourRes;

  static const std::vector<std::u16string> minuteValues = {
      u"2-digit", u"numeric"};
  auto minuteRes =
      getOptionString(runtime, reader, options, u"minute", minuteValues, {});
  if (minuteRes.getStatus() == vm::ExecutionStatus::EXCEPTION)
    return minuteRes.getStatus();
  minute_ = *minuteRes;

  static const std::vector<std::u16string> secondValues = {
      u"2-digit", u"numeric"};
  auto secondRes =
      getOptionString(runtime, reader, options, u"second", secondValues, {});
  if (secondRes.getStatus() == vm::ExecutionStatus::EXCEPTION)
    return secondRes.getStatus();
  second_ = *secondRes;

  // fractionalSecondDigits (integer 1-3)
  auto fsdRes = getNumberOption(
      runtime, reader, options, u"fractionalSecondDigits", 1, 3, {});
  if (LLVM_UNLIKELY(fsdRes == vm::ExecutionStatus::EXCEPTION))
    return fsdRes.getStatus();
  fractionalSecondDigits_ = fsdRes.getValue().value_or(0);

  static const std::vector<std::u16string> timeZoneNameValues = {
      u"short", u"long", u"shortOffset", u"longOffset",
      u"shortGeneric", u"longGeneric"};
  auto timeZoneNameRes = getOptionString(
      runtime, reader, options, u"timeZoneName", timeZoneNameValues, {});
  if (timeZoneNameRes.getStatus() == vm::ExecutionStatus::EXCEPTION)
    return timeZoneNameRes.getStatus();
  timeZoneName_ = *timeZoneNameRes;

  // formatMatcher
  static const std::vector<std::u16string> validFormatMatchers = {
      u"basic", u"best fit"};
  auto fmRes = getOptionString(
      runtime, reader, options, u"formatMatcher", validFormatMatchers, u"best fit");
  if (LLVM_UNLIKELY(fmRes == vm::ExecutionStatus::EXCEPTION))
    return fmRes.getStatus();

  static const std::vector<std::u16string> dateStyles = {
      u"full", u"long", u"medium", u"short"};
  auto dateStyleRes =
      getOptionString(runtime, reader, options, u"dateStyle", dateStyles, {});
  if (dateStyleRes.getStatus() == vm::ExecutionStatus::EXCEPTION)
    return dateStyleRes.getStatus();
  dateStyle_ = dateStyleRes.getValue();

  static const std::vector<std::u16string> timeStyles = {
      u"full", u"long", u"medium", u"short"};
  auto timeStyleRes =
      getOptionString(runtime, reader, options, u"timeStyle", timeStyles, {});
  if (timeStyleRes.getStatus() == vm::ExecutionStatus::EXCEPTION)
    return timeStyleRes.getStatus();
  timeStyle_ = timeStyleRes.getValue();

  if ((!dateStyle_.empty() || !timeStyle_.empty()) &&
      (!weekday_.empty() || !era_.empty() || !year_.empty() || !month_.empty() ||
       !day_.empty() || !dayPeriod_.empty() || !hour_.empty() || !minute_.empty() ||
       !second_.empty() || fractionalSecondDigits_ || !timeZoneName_.empty()))
    return runtime.raiseTypeError("Date/time styles cannot be combined with components");
  auto defaulted = toDateTimeOptions(runtime, options, required, defaults);
  if (defaulted == vm::ExecutionStatus::EXCEPTION) return defaulted.getStatus();
  for (const auto &field : {u"year", u"month", u"day", u"hour", u"minute", u"second"}) {
    auto value = defaulted->find(field);
    if (value != defaulted->end()) {
      if (field == std::u16string_view(u"year")) year_ = value->second.getString();
      else if (field == std::u16string_view(u"month")) month_ = value->second.getString();
      else if (field == std::u16string_view(u"day")) day_ = value->second.getString();
      else if (field == std::u16string_view(u"hour")) hour_ = value->second.getString();
      else if (field == std::u16string_view(u"minute")) minute_ = value->second.getString();
      else second_ = value->second.getString();
    }
  }

  if (hour_.empty() && timeStyle_.empty()) {
    hourCycle_ = u"";
  } else {
    auto defaultCycle = getDefaultHourCycle(runtime);
    if (defaultCycle == vm::ExecutionStatus::EXCEPTION) return defaultCycle.getStatus();
    std::u16string hcDefault = *defaultCycle;
    auto hc = hourCycle_;
    if (hc.empty())
      hc = hcDefault;
    if (hour12.has_value()) {
      if (*hour12) {
        // V8's preferred 12-hour cycle follows CLDR timeData: Japan uses
        // h11 when its ordinary cycle is 24-hour; other regions use h12.
        const auto parsed = ParsedLocaleIdentifier::parse(resolved->localeNoExt);
        const bool japan = parsed && parsed->languageIdentifier.regionSubtag == u"jp";
        hc = hcDefault == u"h11" || (hcDefault == u"h23" && japan) ? u"h11" : u"h12";
      } else {
        hc = hcDefault == u"h24" ? u"h24" : u"h23";
      }
    }
    hourCycle_ = hc;
  }

  // Apply resolved calendar and numberingSystem to the ICU locale string
  // so they take effect when creating the formatter.
  {
    std::string icuLocale = locale8_;
    if (!calendar_.empty()) {
      std::string calUtf8(calendar_.begin(), calendar_.end());
      icuLocale +=
          (icuLocale.find('@') != std::string::npos ? ";" : "@");
      icuLocale += "calendar=" + calUtf8;
    }
    if (!numberingSystem_.empty()) {
      std::string nuUtf8(numberingSystem_.begin(), numberingSystem_.end());
      icuLocale +=
          (icuLocale.find('@') != std::string::npos ? ";" : "@");
      icuLocale += "numbers=" + nuUtf8;
    }
    locale8_ = icuLocale;
  }

  dtf_ = getUDateFormatter(runtime);
  if (!dtf_)
    return runtime.raiseError("ICU date formatter initialization failed");
  UErrorCode status = U_ZERO_ERROR;
  const auto *calendar = icu_->ucal_getType(icu_->udat_getCalendar(dtf_), &status);
  if (U_FAILURE(status) || !calendar)
    return runtime.raiseError("ICU calendar resolution failed");
  const auto *calendarTag = icu_->uloc_toUnicodeLocaleType("ca", calendar);
  if (!calendarTag)
    return runtime.raiseError("ICU calendar has no Unicode locale identifier");
  calendar_ = toUTF16ASCII(calendarTag);
  if (calendar_ == u"gregory" || calendar_ == u"iso8601") {
    auto *proleptic = icu_->ucal_clone(icu_->udat_getCalendar(dtf_), &status);
    if (U_FAILURE(status) || !proleptic)
      return runtime.raiseError("ICU calendar allocation failed");
    // ECMAScript dates use the proleptic Gregorian calendar, including dates
    // before the historical Gregorian cutover.
    icu_->ucal_setGregorianChange(proleptic, -9007199254740992.0, &status);
    if (U_SUCCESS(status)) icu_->udat_setCalendar(dtf_, proleptic);
    icu_->ucal_close(proleptic);
    if (U_FAILURE(status)) return runtime.raiseError("ICU calendar initialization failed");
  }
  auto *numbering = icu_->unumsys_open(locale8_.c_str(), &status);
  if (U_FAILURE(status) || !numbering)
    return runtime.raiseError("ICU date numbering-system resolution failed");
  numberingSystem_ = toUTF16ASCII(icu_->unumsys_getName(numbering));
  icu_->unumsys_close(numbering);

  auto patternResult = readDatePattern(runtime, icu_, dtf_);
  if (patternResult == vm::ExecutionStatus::EXCEPTION) return patternResult.getStatus();
  const auto &pattern = *patternResult;
  weekday_.clear(); era_.clear(); year_.clear(); month_.clear(); day_.clear();
  dayPeriod_.clear(); hour_.clear(); minute_.clear(); second_.clear(); timeZoneName_.clear();
  fractionalSecondDigits_ = 0;
  if (dateStyle_.empty() && timeStyle_.empty()) {
    // Match V8's ordered substring rules for resolvedOptions, including quoted
    // text and the omission of one/two-letter era patterns. This compatibility
    // metadata must not be used to derive the actual formatting pattern.
    auto resolveField = [&](std::u16string &field,
        std::initializer_list<std::pair<std::u16string_view, std::u16string_view>> candidates) {
      for (const auto &candidate : candidates) {
        if (pattern.find(candidate.first) != std::u16string::npos) {
          field = candidate.second;
          return;
        }
      }
    };
    resolveField(weekday_, {{u"EEEEE", u"narrow"}, {u"EEEE", u"long"}, {u"EEE", u"short"},
        {u"ccccc", u"narrow"}, {u"cccc", u"long"}, {u"ccc", u"short"}});
    resolveField(era_, {{u"GGGGG", u"narrow"}, {u"GGGG", u"long"}, {u"GGG", u"short"}});
    resolveField(year_, {{u"yy", u"2-digit"}, {u"y", u"numeric"}});
    resolveField(month_, {{u"MMMMM", u"narrow"}, {u"MMMM", u"long"}, {u"MMM", u"short"},
        {u"MM", u"2-digit"}, {u"M", u"numeric"}, {u"LLLLL", u"narrow"}, {u"LLLL", u"long"},
        {u"LLL", u"short"}, {u"LL", u"2-digit"}, {u"L", u"numeric"}});
    resolveField(day_, {{u"dd", u"2-digit"}, {u"d", u"numeric"}});
    resolveField(dayPeriod_, {{u"BBBBB", u"narrow"}, {u"bbbbb", u"narrow"}, {u"BBBB", u"long"},
        {u"bbbb", u"long"}, {u"B", u"short"}, {u"b", u"short"}});
    resolveField(hour_, {{u"HH", u"2-digit"}, {u"H", u"numeric"}, {u"hh", u"2-digit"},
        {u"h", u"numeric"}, {u"kk", u"2-digit"}, {u"k", u"numeric"},
        {u"KK", u"2-digit"}, {u"K", u"numeric"}});
    resolveField(minute_, {{u"mm", u"2-digit"}, {u"m", u"numeric"}});
    resolveField(second_, {{u"ss", u"2-digit"}, {u"s", u"numeric"}});
    resolveField(timeZoneName_, {{u"zzzz", u"long"}, {u"z", u"short"}, {u"OOOO", u"longOffset"},
        {u"O", u"shortOffset"}, {u"vvvv", u"longGeneric"}, {u"v", u"shortGeneric"}});
    fractionalSecondDigits_ = std::min(3, static_cast<int>(std::count(pattern.begin(), pattern.end(), u'S')));
  }

  return vm::ExecutionStatus::RETURNED;
}

vm::CallResult<std::unique_ptr<DateTimeFormat>> DateTimeFormat::create(
    vm::Runtime &runtime,
    const std::vector<std::u16string> &locales,
    const Options &options) noexcept {
  MapOptionReader reader(options);
  return create(runtime, locales, reader);
}

vm::CallResult<std::unique_ptr<DateTimeFormat>> DateTimeFormat::create(
    vm::Runtime &runtime,
    const std::vector<std::u16string> &locales,
    OptionReader &reader,
    std::u16string_view required,
    std::u16string_view defaults) noexcept {
  const hermes_icu_vtable *icu = getIcuVtableForRuntime(runtime);
  auto instance = std::make_unique<DateTimeFormatIcuProvider>();
  if (LLVM_UNLIKELY(
          instance->initialize(runtime, icu, locales, reader, required, defaults) ==
          vm::ExecutionStatus::EXCEPTION)) {
    return vm::ExecutionStatus::EXCEPTION;
  }
  return instance;
}

Options DateTimeFormatIcuProvider::resolvedOptions() noexcept {
  Options options;
  options.emplace(u"locale", Option(locale_));
  assert(!calendar_.empty() && !numberingSystem_.empty());
  options.emplace(u"calendar", Option(calendar_));
  options.emplace(u"numberingSystem", Option(numberingSystem_));
  options.emplace(u"timeZone", Option(timeZone_));
  // hourCycle and hour12: only present when hour component is in the format
  if (!hourCycle_.empty()) {
    options.emplace(u"hourCycle", Option(hourCycle_));
    bool h12 = (hourCycle_ == u"h11" || hourCycle_ == u"h12");
    options.emplace(u"hour12", Option(h12));
  }
  // ECMA-402: only include component properties that were actually resolved.
  if (!weekday_.empty())
    options.emplace(u"weekday", weekday_);
  if (!era_.empty())
    options.emplace(u"era", era_);
  if (!year_.empty())
    options.emplace(u"year", year_);
  if (!month_.empty())
    options.emplace(u"month", month_);
  if (!day_.empty())
    options.emplace(u"day", day_);
  if (!dayPeriod_.empty())
    options.emplace(u"dayPeriod", dayPeriod_);
  if (!hour_.empty())
    options.emplace(u"hour", hour_);
  if (!minute_.empty())
    options.emplace(u"minute", minute_);
  if (!second_.empty())
    options.emplace(u"second", second_);
  if (fractionalSecondDigits_ > 0) {
    options.emplace(
        u"fractionalSecondDigits",
        Option(static_cast<double>(fractionalSecondDigits_)));
  }
  if (!timeZoneName_.empty())
    options.emplace(u"timeZoneName", timeZoneName_);
  if (!dateStyle_.empty())
    options.emplace(u"dateStyle", dateStyle_);
  if (!timeStyle_.empty())
    options.emplace(u"timeStyle", timeStyle_);
  return options;
}

Options DateTimeFormat::resolvedOptions() noexcept {
  return static_cast<DateTimeFormatIcuProvider *>(this)->resolvedOptions();
}

vm::CallResult<std::u16string> DateTimeFormatIcuProvider::format(
    vm::Runtime &runtime, double jsTimeValue) noexcept {
  assert(icu_ && dtf_);
  UErrorCode status = U_ZERO_ERROR;
  const int32_t length = icu_->udat_format(dtf_, jsTimeValue, nullptr, 0, nullptr, &status);
  if (status != U_BUFFER_OVERFLOW_ERROR || length <= 0)
    return runtime.raiseError("ICU date formatting failed");
  std::u16string result(length, u'\0');
  status = U_ZERO_ERROR;
  icu_->udat_format(dtf_, jsTimeValue,
      reinterpret_cast<UChar *>(result.data()), length, nullptr, &status);
  if (U_FAILURE(status)) return runtime.raiseError("ICU date formatting failed");
  // V8 keeps pre-ICU-72 spacing for format(), while parts retain ICU spacing.
  std::replace(result.begin(), result.end(), char16_t(0x202f), u' ');
  return result;
}

vm::CallResult<std::u16string> DateTimeFormat::format(
    vm::Runtime &runtime, double jsTimeValue) noexcept {
  return static_cast<DateTimeFormatIcuProvider *>(this)->format(runtime, jsTimeValue);
}

vm::CallResult<std::u16string> DateTimeFormatIcuProvider::getDefaultHourCycle(
    vm::Runtime &runtime) {
  UErrorCode status = U_ZERO_ERROR;
  auto *generator = icu_->udatpg_open(locale8_.c_str(), &status);
  if (U_FAILURE(status) || !generator)
    return runtime.raiseError("ICU hour-cycle resolution failed");
  const auto cycle = icu_->udatpg_getDefaultHourCycle(generator, &status);
  icu_->udatpg_close(generator);
  if (U_FAILURE(status))
    return runtime.raiseError("ICU hour-cycle resolution failed");
  switch (cycle) {
    case 0: return std::u16string(u"h11");
    case 1: return std::u16string(u"h12");
    case 2: return std::u16string(u"h23");
    case 3: return std::u16string(u"h24");
    default: return runtime.raiseError("ICU returned an unknown hour cycle");
  }
}

UDateFormat *DateTimeFormatIcuProvider::getUDateFormatter(vm::Runtime &runtime) {
  UErrorCode status = U_ZERO_ERROR;
  const auto zone = !timeZone_.empty() && (timeZone_[0] == u'+' || timeZone_[0] == u'-')
      ? u"GMT" + timeZone_ : timeZone_;
  auto open = [&](int32_t timeStyle, int32_t dateStyle, const std::u16string &pattern) {
    return icu_->udat_open(static_cast<UDateFormatStyle>(timeStyle),
        static_cast<UDateFormatStyle>(dateStyle), locale8_.c_str(),
        reinterpret_cast<const UChar *>(zone.data()), zone.size(),
        reinterpret_cast<const UChar *>(pattern.data()), pattern.size(), &status);
  };
  std::unique_ptr<UDateFormat, decltype(icu_->udat_close)> formatter(nullptr, icu_->udat_close);
  std::u16string skeleton;
  if (!timeStyle_.empty() || !dateStyle_.empty()) {
    auto style = [](const std::u16string &value) {
      if (value == u"full") return kUDAT_FULL;
      if (value == u"long") return kUDAT_LONG;
      if (value == u"medium") return kUDAT_MEDIUM;
      if (value == u"short") return kUDAT_SHORT;
      return kUDAT_NONE;
    };
    formatter.reset(open(style(timeStyle_), style(dateStyle_), {}));
    if (U_FAILURE(status) || !formatter) return nullptr;
    if (timeStyle_.empty()) return formatter.release();
    auto pattern = readDatePattern(runtime, icu_, formatter.get());
    if (pattern == vm::ExecutionStatus::EXCEPTION) return nullptr;
    const auto requestedHour = hourCycleSymbol(hourCycle_);
    bool quoted = false, matches = false;
    for (char16_t c : *pattern) {
      if (c == u'\'') quoted = !quoted;
      if (!quoted && c == requestedHour) matches = true;
    }
    if (matches) return formatter.release();
    // Switching between 12/24-hour styles also changes day-period placement
    // and punctuation. Regenerate from the style's skeleton, as V8 does.
    auto skeletonResult = readDateSkeleton(runtime, icu_, *pattern);
    if (skeletonResult == vm::ExecutionStatus::EXCEPTION) return nullptr;
    skeleton = std::move(*skeletonResult);
    skeleton.erase(std::remove_if(skeleton.begin(), skeleton.end(),
        [](char16_t c) { return c == u'a' || c == u'b' || c == u'B'; }), skeleton.end());
    for (auto &c : skeleton)
      if (c == u'h' || c == u'H' || c == u'K' || c == u'k') c = requestedHour;
  } else {
    if (weekday_ == u"narrow") skeleton += u"EEEEE";
    else if (weekday_ == u"long") skeleton += u"EEEE";
    else if (weekday_ == u"short") skeleton += u"EEE";
    if (era_ == u"narrow") skeleton += u"GGGGG";
    else if (era_ == u"long") skeleton += u"GGGG";
    else if (era_ == u"short") skeleton += u"GGG";
    if (!year_.empty()) skeleton += year_ == u"2-digit" ? u"yy" : u"y";
    if (month_ == u"2-digit") skeleton += u"MM";
    else if (month_ == u"numeric") skeleton += u"M";
    else if (month_ == u"narrow") skeleton += u"MMMMM";
    else if (month_ == u"short") skeleton += u"MMM";
    else if (month_ == u"long") skeleton += u"MMMM";
    if (!day_.empty()) skeleton += day_ == u"2-digit" ? u"dd" : u"d";
    if (!dayPeriod_.empty())
      skeleton += dayPeriod_ == u"narrow" ? u"BBBBB" : dayPeriod_ == u"long" ? u"BBBB" : u"B";
    if (!hour_.empty()) skeleton += std::u16string(hour_ == u"2-digit" ? 2 : 1, hourCycleSymbol(hourCycle_));
    if (!minute_.empty()) skeleton += minute_ == u"2-digit" ? u"mm" : u"m";
    if (!second_.empty()) skeleton += second_ == u"2-digit" ? u"ss" : u"s";
    skeleton += std::u16string(fractionalSecondDigits_, u'S');
    if (timeZoneName_ == u"short") skeleton += u"z";
    else if (timeZoneName_ == u"long") skeleton += u"zzzz";
    else if (timeZoneName_ == u"shortOffset") skeleton += u"O";
    else if (timeZoneName_ == u"longOffset") skeleton += u"OOOO";
    else if (timeZoneName_ == u"shortGeneric") skeleton += u"v";
    else if (timeZoneName_ == u"longGeneric") skeleton += u"vvvv";
  }
  status = U_ZERO_ERROR;
  std::unique_ptr<UDateTimePatternGenerator, decltype(icu_->udatpg_close)> generator(
      icu_->udatpg_open(locale8_.c_str(), &status), icu_->udatpg_close);
  if (U_FAILURE(status) || !generator) return nullptr;
  const int32_t length = icu_->udatpg_getBestPatternWithOptions(generator.get(),
      reinterpret_cast<const UChar *>(skeleton.data()), skeleton.size(),
      UDATPG_MATCH_HOUR_FIELD_LENGTH, nullptr, 0, &status);
  if (status != U_BUFFER_OVERFLOW_ERROR || length <= 0) return nullptr;
  std::u16string pattern(length, u'\0');
  status = U_ZERO_ERROR;
  icu_->udatpg_getBestPatternWithOptions(generator.get(),
      reinterpret_cast<const UChar *>(skeleton.data()), skeleton.size(),
      UDATPG_MATCH_HOUR_FIELD_LENGTH,
      reinterpret_cast<UChar *>(pattern.data()), length, &status);
  if (U_FAILURE(status)) return nullptr;
  if (!hourCycle_.empty()) pattern = replacePatternHourCycle(pattern, hourCycleSymbol(hourCycle_));
  formatter.reset(open(kUDAT_PATTERN, kUDAT_PATTERN, pattern));
  if (U_FAILURE(status)) return nullptr;
  return formatter.release();
}

namespace {
struct IntlFieldRange { int32_t begin, end, type; };

const char16_t *rangeSource(const std::vector<IntlFieldRange> &sources, int32_t begin, int32_t end) {
  std::array<const IntlFieldRange *, 2> latest{nullptr, nullptr};
  for (const auto &source : sources) latest[source.type] = &source;
  // ICU interval spans can overlap. V8 keeps each endpoint's most recent span
  // and gives the start endpoint precedence over the end endpoint.
  for (size_t index = 0; index < latest.size(); ++index)
    if (latest[index] && latest[index]->begin <= begin && latest[index]->end >= end)
      return index == 0 ? u"startRange" : u"endRange";
  return u"shared";
}


vm::CallResult<std::vector<Part>> partitionDateFields(
    vm::Runtime &runtime, const std::u16string &formatted,
    std::vector<IntlFieldRange> fields, const std::vector<IntlFieldRange> &sources,
    bool outputSource) {
  for (const auto *ranges : std::array<const std::vector<IntlFieldRange> *, 2>{&fields, &sources})
    for (const auto &range : *ranges)
      if (range.begin < 0 || range.begin >= range.end || static_cast<size_t>(range.end) > formatted.size())
        return runtime.raiseError("ICU returned an invalid date field range");
  for (const auto &source : sources)
    if (source.type != 0 && source.type != 1)
      return runtime.raiseError("ICU returned an invalid date-range source");
  std::sort(fields.begin(), fields.end(), [](const IntlFieldRange &a, const IntlFieldRange &b) {
    return a.begin < b.begin;
  });
  std::vector<Part> parts;
  auto addPart = [&](int32_t begin, int32_t end, std::u16string type) {
    Part part{{u"type", std::move(type)}, {u"value", formatted.substr(begin, end - begin)}};
    if (outputSource) {
      part.emplace(u"source", rangeSource(sources, begin, end));
    }
    parts.push_back(std::move(part));
  };
  int32_t position = 0;
  for (const auto &field : fields) {
    if (field.begin < position) return runtime.raiseError("ICU returned overlapping date fields");
    if (field.begin > position) addPart(position, field.begin, u"literal");
    addPart(field.begin, field.end, dateFieldToPartType(field.type));
    position = field.end;
  }
  if (static_cast<size_t>(position) < formatted.size()) addPart(position, formatted.size(), u"literal");
  return parts;
}
} // namespace

vm::CallResult<std::vector<Part>> DateTimeFormatIcuProvider::formatToParts(
    vm::Runtime &runtime, double jsTimeValue) noexcept {
  assert(icu_ && dtf_);
  UErrorCode status = U_ZERO_ERROR;
  std::unique_ptr<UFieldPositionIterator, decltype(icu_->ufieldpositer_close)> fields(
      icu_->ufieldpositer_open(&status), icu_->ufieldpositer_close);
  if (U_FAILURE(status) || !fields)
    return runtime.raiseError("ICU date field-iterator allocation failed");
  const int32_t length = icu_->udat_formatForFields(dtf_, jsTimeValue, nullptr, 0, nullptr, &status);
  if (status != U_BUFFER_OVERFLOW_ERROR || length <= 0)
    return runtime.raiseError("ICU date field formatting failed");
  std::u16string formatted(length, u'\0');
  status = U_ZERO_ERROR;
  icu_->udat_formatForFields(dtf_, jsTimeValue,
      reinterpret_cast<UChar *>(formatted.data()), length, fields.get(), &status);
  if (U_FAILURE(status)) return runtime.raiseError("ICU date field formatting failed");
  std::vector<IntlFieldRange> ranges;
  int32_t begin, end, type;
  while ((type = icu_->ufieldpositer_next(fields.get(), &begin, &end)) >= 0)
    ranges.push_back({begin, end, type});
  return partitionDateFields(runtime, formatted, std::move(ranges), {}, false);
}

vm::CallResult<const UFormattedValue *> DateTimeFormatIcuProvider::formatRangeValue(
    vm::Runtime &runtime, double start, double end) noexcept {
  UErrorCode status = U_ZERO_ERROR;
  if (!interval_) {
    auto pattern = readDatePattern(runtime, icu_, dtf_);
    if (pattern == vm::ExecutionStatus::EXCEPTION) return pattern.getStatus();
    auto skeleton = readDateSkeleton(runtime, icu_, *pattern);
    if (skeleton == vm::ExecutionStatus::EXCEPTION) return skeleton.getStatus();
    icu::Locale locale(locale8_.c_str());
    // DateIntervalFormat needs the resolved hour cycle in the locale as well
    // as the skeleton, including explicit h11/h24 selection.
    if (!hourCycle_.empty()) {
      const std::string hourCycle(hourCycle_.begin(), hourCycle_.end());
      locale.setUnicodeKeywordValue("hc", hourCycle, status);
    }
    const auto zone = !timeZone_.empty() && (timeZone_[0] == u'+' || timeZone_[0] == u'-')
        ? u"GMT" + timeZone_ : timeZone_;
    std::unique_ptr<UDateIntervalFormat, decltype(icu_->udtitvfmt_close)> formatter(
        icu_->udtitvfmt_open(locale.getName(), reinterpret_cast<const UChar *>(skeleton->data()),
            skeleton->size(), reinterpret_cast<const UChar *>(zone.data()), zone.size(), &status),
        icu_->udtitvfmt_close);
    if (U_FAILURE(status) || !formatter)
      return runtime.raiseError("ICU date-range formatter initialization failed");
    std::unique_ptr<UFormattedDateInterval, decltype(icu_->udtitvfmt_closeResult)> result(
        icu_->udtitvfmt_openResult(&status), icu_->udtitvfmt_closeResult);
    if (U_FAILURE(status) || !result)
      return runtime.raiseError("ICU date-range result allocation failed");
    interval_ = formatter.release();
    intervalResult_ = result.release();
  }
  // Clone the actual formatter calendar, preserving its proleptic Gregorian
  // cutover, locale/calendar options and timezone across both endpoints.
  std::unique_ptr<UCalendar, decltype(icu_->ucal_close)> first(
      icu_->ucal_clone(icu_->udat_getCalendar(dtf_), &status), icu_->ucal_close);
  std::unique_ptr<UCalendar, decltype(icu_->ucal_close)> second(
      icu_->ucal_clone(icu_->udat_getCalendar(dtf_), &status), icu_->ucal_close);
  if (U_FAILURE(status) || !first || !second)
    return runtime.raiseError("ICU date-range calendar allocation failed");
  icu_->ucal_setMillis(first.get(), std::trunc(start), &status);
  icu_->ucal_setMillis(second.get(), std::trunc(end), &status);
  // This ICU C API preserves calendar settings; the older date-only provider
  // entry point constructs fresh calendars and loses the Gregorian adjustment.
  udtitvfmt_formatCalendarToResult(interval_, first.get(), second.get(), intervalResult_, &status);
  if (U_FAILURE(status)) return runtime.raiseTypeError("ICU date-range formatting failed");
  const auto *value = icu_->udtitvfmt_resultAsValue(intervalResult_, &status);
  if (U_FAILURE(status) || !value) return runtime.raiseError("ICU date-range result retrieval failed");
  return value;
}

vm::CallResult<std::u16string> DateTimeFormatIcuProvider::formatRange(
    vm::Runtime &runtime, double start, double end) noexcept {
  auto value = formatRangeValue(runtime, start, end);
  if (value == vm::ExecutionStatus::EXCEPTION) return value.getStatus();
  UErrorCode status = U_ZERO_ERROR;
  std::unique_ptr<UConstrainedFieldPosition, decltype(icu_->ucfpos_close)> position(
      icu_->ucfpos_open(&status), icu_->ucfpos_close);
  if (U_FAILURE(status) || !position) return runtime.raiseError("ICU date-range iterator allocation failed");
  bool hasRange = false;
  while (icu_->ufmtval_nextPosition(*value, position.get(), &status))
    if (icu_->ucfpos_getCategory(position.get(), &status) == UFIELD_CATEGORY_DATE_INTERVAL_SPAN) hasRange = true;
  if (U_FAILURE(status)) return runtime.raiseError("ICU date-range field retrieval failed");
  // Collapsed intervals use the original single-date formatter, preserving its
  // widths and V8's string-only spacing compatibility rule.
  if (!hasRange) return format(runtime, std::trunc(start));
  int32_t length;
  const auto *text = icu_->ufmtval_getString(*value, &length, &status);
  if (U_FAILURE(status) || !text) return runtime.raiseError("ICU date-range string retrieval failed");
  return std::u16string(reinterpret_cast<const char16_t *>(text), length);
}

vm::CallResult<std::vector<Part>> DateTimeFormatIcuProvider::formatRangeToParts(
    vm::Runtime &runtime, double start, double end) noexcept {
  auto value = formatRangeValue(runtime, start, end);
  if (value == vm::ExecutionStatus::EXCEPTION) return value.getStatus();
  UErrorCode status = U_ZERO_ERROR;
  int32_t length;
  const auto *text = icu_->ufmtval_getString(*value, &length, &status);
  if (U_FAILURE(status) || !text) return runtime.raiseError("ICU date-range string retrieval failed");
  std::u16string formatted(reinterpret_cast<const char16_t *>(text), length);
  std::unique_ptr<UConstrainedFieldPosition, decltype(icu_->ucfpos_close)> position(
      icu_->ucfpos_open(&status), icu_->ucfpos_close);
  if (U_FAILURE(status) || !position) return runtime.raiseError("ICU date-range iterator allocation failed");
  std::vector<IntlFieldRange> fields, sources;
  while (icu_->ufmtval_nextPosition(*value, position.get(), &status)) {
    IntlFieldRange field;
    icu_->ucfpos_getIndexes(position.get(), &field.begin, &field.end, &status);
    field.type = icu_->ucfpos_getField(position.get(), &status);
    const auto category = icu_->ucfpos_getCategory(position.get(), &status);
    if (U_FAILURE(status)) return runtime.raiseError("ICU date-range field retrieval failed");
    if (category == UFIELD_CATEGORY_DATE_INTERVAL_SPAN) sources.push_back(field);
    else if (category == UFIELD_CATEGORY_DATE) fields.push_back(field);
    else return runtime.raiseError("ICU date-range field category is invalid");
  }
  if (U_FAILURE(status)) return runtime.raiseError("ICU date-range field retrieval failed");
  if (sources.empty()) {
    // Reference V8's FormatRangeCommon passes the original, unclipped start to
    // its parts fallback. For negative fractional milliseconds this can differ
    // from both formatRange() and the ordinary formatToParts() binding.
    auto parts = formatToParts(runtime, start);
    if (parts == vm::ExecutionStatus::EXCEPTION) return parts.getStatus();
    for (auto &part : *parts) part.emplace(u"source", u"shared");
    return std::move(*parts);
  }
  return partitionDateFields(runtime, formatted, std::move(fields), sources, true);
}

vm::CallResult<std::u16string> DateTimeFormat::formatRange(
    vm::Runtime &runtime, double start, double end) noexcept {
  return static_cast<DateTimeFormatIcuProvider *>(this)->formatRange(runtime, start, end);
}

vm::CallResult<std::vector<Part>> DateTimeFormat::formatRangeToParts(
    vm::Runtime &runtime, double start, double end) noexcept {
  return static_cast<DateTimeFormatIcuProvider *>(this)->formatRangeToParts(runtime, start, end);
}

vm::CallResult<std::vector<Part>> DateTimeFormat::formatToParts(
    vm::Runtime &runtime, double value) noexcept {
  return static_cast<DateTimeFormatIcuProvider *>(this)->formatToParts(runtime, value);
}

// ============================================================================
// NumberFormat (Phase 2: ICU-backed implementation)
// ============================================================================

namespace {

class NumberFormatIcuProvider : public NumberFormat {
 public:
  NumberFormatIcuProvider() = default;
  ~NumberFormatIcuProvider() override;

  vm::ExecutionStatus initialize(
      vm::Runtime &runtime,
      const hermes_icu_vtable *icu,
      const std::vector<std::u16string> &locales,
      OptionReader &reader) noexcept;

  Options resolvedOptions() noexcept;
  vm::CallResult<std::u16string> format(vm::Runtime &runtime, const NumberFormatValue &number) noexcept;
  vm::CallResult<std::vector<Part>> formatToParts(vm::Runtime &runtime, const NumberFormatValue &number) noexcept;
  vm::CallResult<std::u16string> formatRange(vm::Runtime &runtime, const NumberFormatValue &start, const NumberFormatValue &end) noexcept;
  vm::CallResult<std::vector<Part>> formatRangeToParts(vm::Runtime &runtime, const NumberFormatValue &start, const NumberFormatValue &end) noexcept;

 private:
  vm::CallResult<icu::number::FormattedNumberRange> formatRangeValue(
      vm::Runtime &runtime, const NumberFormatValue &start, const NumberFormatValue &end) noexcept;
  std::u16string skeleton_;
  std::string localeICU_;
  std::optional<icu::number::LocalizedNumberRangeFormatter> rangeFormatter_;
  const hermes_icu_vtable *icu_ = nullptr;
  vm::CallResult<const UFormattedValue *> formatValue(vm::Runtime &runtime, const NumberFormatValue &number) noexcept;
  UNumberFormatter *unumf_ = nullptr;
  UFormattedNumber *unumfResult_ = nullptr;

  std::u16string locale_;
  std::u16string style_;
  std::u16string currency_;
  std::u16string currencyDisplay_;
  std::u16string unit_;
  std::u16string unitDisplay_;
  std::u16string notation_;
  std::u16string signDisplay_;
  std::u16string compactDisplay_;
  std::u16string numberingSystem_;
  std::u16string currencySign_;
  int minimumIntegerDigits_ = 1;
  int minimumFractionDigits_ = -1;
  int maximumFractionDigits_ = -1;
  int minimumSignificantDigits_ = -1;
  int maximumSignificantDigits_ = -1;
  std::u16string useGrouping_;
  std::u16string roundingPriority_, roundingMode_, trailingZeroDisplay_;
  int roundingIncrement_ = 1;
};

NumberFormatIcuProvider::~NumberFormatIcuProvider() {
  if (unumfResult_ && icu_) {
    icu_->unumf_closeResult(unumfResult_);
  }
  if (unumf_ && icu_) {
    icu_->unumf_close(unumf_);
  }
}

vm::ExecutionStatus NumberFormatIcuProvider::initialize(
    vm::Runtime &runtime,
    const hermes_icu_vtable *icu,
    const std::vector<std::u16string> &locales,
    OptionReader &reader) noexcept {
  icu_ = icu;
  Options options;
  if (!icu_) {
    (void)runtime.raiseRangeError("NumberFormat requires ICU support");
    return vm::ExecutionStatus::EXCEPTION;
  }

  static constexpr std::u16string_view keys[]{u"nu"};
  auto resolved = resolveFormatterLocale(runtime, icu_, locales, options, reader, keys);
  if (resolved == vm::ExecutionStatus::EXCEPTION) return resolved.getStatus();
  locale_ = resolved->getCanonicalizedLocaleId();
  auto numberingOption = resolved->resolvedOpts.find(u"nu");
  if (numberingOption != resolved->resolvedOpts.end())
    numberingSystem_ = numberingOption->second.getString();

  // style
  static const std::vector<std::u16string> validStyles = {
      u"decimal", u"percent", u"currency", u"unit"};
  auto styleRes =
      getOptionString(runtime, reader, options, u"style", validStyles, u"decimal");
  if (LLVM_UNLIKELY(styleRes == vm::ExecutionStatus::EXCEPTION))
    return styleRes.getStatus();
  style_ = styleRes.getValue();

  // currency (ECMA-402 steps 10-12)
  auto currencyRes = getOptionString(runtime, reader, options, u"currency", {}, {});
  if (LLVM_UNLIKELY(currencyRes == vm::ExecutionStatus::EXCEPTION))
    return currencyRes.getStatus();
  currency_ = currencyRes.getValue();
  bool hasCurrency = options.find(u"currency") != options.end();
  // Step 11: currency not provided + style:currency → TypeError
  if (!hasCurrency && style_ == u"currency") {
    return runtime.raiseTypeError(
        "Currency code is required with currency style");
  }
  // Step 12: currency provided → must be well-formed (exactly 3 ASCII letters)
  if (hasCurrency) {
    bool wellFormed = (currency_.size() == 3);
    if (wellFormed) {
      for (auto c : currency_) {
        if (!((c >= u'A' && c <= u'Z') || (c >= u'a' && c <= u'z'))) {
          wellFormed = false;
          break;
        }
      }
    }
    if (!wellFormed) {
      return runtime.raiseRangeError("Invalid currency code");
    }
  }

  // currencyDisplay
  static const std::vector<std::u16string> validCurrencyDisplays = {
      u"code", u"symbol", u"narrowSymbol", u"name"};
  auto currencyDisplayRes = getOptionString(
      runtime, reader, options, u"currencyDisplay", validCurrencyDisplays, u"symbol");
  if (LLVM_UNLIKELY(currencyDisplayRes == vm::ExecutionStatus::EXCEPTION))
    return currencyDisplayRes.getStatus();
  currencyDisplay_ = currencyDisplayRes.getValue();

  // currencySign
  static const std::vector<std::u16string> validCurrencySigns = {
      u"standard", u"accounting"};
  auto csRes = getOptionString(
      runtime, reader, options, u"currencySign", validCurrencySigns, u"standard");
  if (LLVM_UNLIKELY(csRes == vm::ExecutionStatus::EXCEPTION))
    return csRes.getStatus();
  currencySign_ = csRes.getValue();

  // unit
  auto unitRes = getOptionString(runtime, reader, options, u"unit", {}, {});
  if (LLVM_UNLIKELY(unitRes == vm::ExecutionStatus::EXCEPTION))
    return unitRes.getStatus();
  unit_ = unitRes.getValue();
  if (style_ == u"unit" && !options.count(u"unit")) {
    return runtime.raiseTypeError("Unit is required with unit style");
  }
  // Unit identifier validation (ECMA-402 §6.5)
  if (options.count(u"unit") && !isWellFormedUnitIdentifier(unit_)) {
    return runtime.raiseRangeError("Invalid unit identifier");
  }

  // unitDisplay
  static const std::vector<std::u16string> validUnitDisplays = {
      u"short", u"narrow", u"long"};
  auto udRes = getOptionString(
      runtime, reader, options, u"unitDisplay", validUnitDisplays, u"short");
  if (LLVM_UNLIKELY(udRes == vm::ExecutionStatus::EXCEPTION))
    return udRes.getStatus();
  unitDisplay_ = udRes.getValue();

  // notation
  static const std::vector<std::u16string> validNotations = {
      u"standard", u"scientific", u"engineering", u"compact"};
  auto notationRes = getOptionString(
      runtime, reader, options, u"notation", validNotations, u"standard");
  if (LLVM_UNLIKELY(notationRes == vm::ExecutionStatus::EXCEPTION))
    return notationRes.getStatus();
  notation_ = notationRes.getValue();

  auto localeConversion = convertBCP47toICULocale(runtime, icu_, locale_);
  if (localeConversion == vm::ExecutionStatus::EXCEPTION) return localeConversion.getStatus();
  std::string localeICU = std::move(*localeConversion);
  // Unsupported numbering systems fall back to the locale's default. An
  // algorithmic numbering system is not an ECMA-402 decimal numbering system.
  if (!numberingSystem_.empty()) {
    UErrorCode status = U_ZERO_ERROR;
    auto name = toUTF8ASCII(numberingSystem_);
    auto *system = icu_->unumsys_openByName(name.c_str(), &status);
    const bool supported = U_SUCCESS(status) && system &&
        !icu_->unumsys_isAlgorithmic(system) && icu_->unumsys_getRadix(system) == 10;
    if (system)
      icu_->unumsys_close(system);
    if ((U_FAILURE(status) && status != U_UNSUPPORTED_ERROR && status != U_MISSING_RESOURCE_ERROR) ||
        (U_SUCCESS(status) && !system))
      return runtime.raiseError("ICU numbering system lookup failed");
    if (supported)
      localeICU += (localeICU.find('@') == std::string::npos ? "@numbers=" : ";numbers=") + name;
  }
  UErrorCode status = U_ZERO_ERROR;
  auto *numbering = icu_->unumsys_open(localeICU.c_str(), &status);
  if (U_FAILURE(status) || !numbering)
    return runtime.raiseError("ICU numbering system resolution failed");
  numberingSystem_ = toUTF16ASCII(icu_->unumsys_getName(numbering));
  icu_->unumsys_close(numbering);

  auto minInt = getNumberOption(runtime, reader, options, u"minimumIntegerDigits", 1, 21, 1);
  if (minInt == vm::ExecutionStatus::EXCEPTION)
    return minInt.getStatus();
  minimumIntegerDigits_ = minInt->value();

  for (const auto *name : {u"minimumFractionDigits", u"maximumFractionDigits",
                           u"minimumSignificantDigits", u"maximumSignificantDigits"}) {
    if (readOption(runtime, reader, options, name, Option::Kind::DeferredNumber) ==
        vm::ExecutionStatus::EXCEPTION) return vm::ExecutionStatus::EXCEPTION;
  }
  auto increment = getNumberOption(runtime, reader, options, u"roundingIncrement", 1, 5000, 1);
  if (increment == vm::ExecutionStatus::EXCEPTION)
    return increment.getStatus();
  roundingIncrement_ = increment->value();
  static const std::unordered_set<int> increments{
      1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 2500, 5000};
  if (!increments.count(roundingIncrement_))
    return runtime.raiseRangeError("Invalid roundingIncrement");

  auto rounding = getOptionString(runtime, reader, options, u"roundingMode",
      {u"ceil", u"floor", u"expand", u"trunc", u"halfCeil", u"halfFloor",
       u"halfExpand", u"halfTrunc", u"halfEven"}, u"halfExpand");
  if (rounding == vm::ExecutionStatus::EXCEPTION)
    return rounding.getStatus();
  roundingMode_ = *rounding;
  auto priority = getOptionString(runtime, reader, options, u"roundingPriority",
      {u"auto", u"morePrecision", u"lessPrecision"}, u"auto");
  if (priority == vm::ExecutionStatus::EXCEPTION)
    return priority.getStatus();
  roundingPriority_ = *priority;
  auto trailing = getOptionString(runtime, reader, options, u"trailingZeroDisplay",
      {u"auto", u"stripIfInteger"}, u"auto");
  if (trailing == vm::ExecutionStatus::EXCEPTION)
    return trailing.getStatus();
  trailingZeroDisplay_ = *trailing;
  int defaultMinFD = 0;
  int defaultMaxFD = style_ == u"percent" ? 0 : 3;
  if (style_ == u"currency") {
    for (auto &character : currency_)
      if (character >= u'a' && character <= u'z')
        character -= u'a' - u'A';
    status = U_ZERO_ERROR;
    defaultMinFD = defaultMaxFD = icu_->ucurr_getDefaultFractionDigits(
        reinterpret_cast<const UChar *>(currency_.c_str()), &status);
    if (U_FAILURE(status))
      return runtime.raiseError("ICU currency precision resolution failed");
  }
  if (roundingIncrement_ != 1)
    defaultMaxFD = defaultMinFD;
  const bool hasSD = options.count(u"minimumSignificantDigits") || options.count(u"maximumSignificantDigits");
  const bool hasFD = options.count(u"minimumFractionDigits") || options.count(u"maximumFractionDigits");
  bool needSD = roundingPriority_ != u"auto" || hasSD;
  bool needFD = roundingPriority_ != u"auto" || (!hasSD && (hasFD || notation_ != u"compact"));
  if (needSD) {
    auto minimum = getNumberOption(runtime, options, u"minimumSignificantDigits", 1, 21, 1);
    if (minimum == vm::ExecutionStatus::EXCEPTION) return minimum.getStatus();
    minimumSignificantDigits_ = minimum->value();
    auto maximum = getNumberOption(runtime, options, u"maximumSignificantDigits", minimumSignificantDigits_, 21, 21);
    if (maximum == vm::ExecutionStatus::EXCEPTION) return maximum.getStatus();
    maximumSignificantDigits_ = maximum->value();
  }
  if (needFD) {
    auto minimum = getNumberOption(runtime, options, u"minimumFractionDigits", 0, 100, {});
    if (minimum == vm::ExecutionStatus::EXCEPTION) return minimum.getStatus();
    auto maximum = getNumberOption(runtime, options, u"maximumFractionDigits", 0, 100, {});
    if (maximum == vm::ExecutionStatus::EXCEPTION) return maximum.getStatus();
    minimumFractionDigits_ = minimum->value_or(std::min(defaultMinFD, maximum->value_or(defaultMaxFD)));
    maximumFractionDigits_ = maximum->value_or(std::max(defaultMaxFD, minimumFractionDigits_));
    if (minimumFractionDigits_ > maximumFractionDigits_)
      return runtime.raiseRangeError("Invalid fraction digit range");
  }
  if (!needFD && !needSD) {
    minimumFractionDigits_ = maximumFractionDigits_ = 0;
    minimumSignificantDigits_ = 1;
    maximumSignificantDigits_ = 2;
    roundingPriority_ = u"morePrecision";
    needFD = needSD = true;
  }
  if (roundingIncrement_ != 1) {
    if (needSD || roundingPriority_ != u"auto")
      return runtime.raiseTypeError("roundingIncrement requires fraction precision");
    if (minimumFractionDigits_ != maximumFractionDigits_)
      return runtime.raiseRangeError("roundingIncrement requires equal fraction bounds");
  }

  // compactDisplay
  static const std::vector<std::u16string> validCompactDisplays = {
      u"short", u"long"};
  auto compactRes = getOptionString(
      runtime, reader, options, u"compactDisplay", validCompactDisplays, u"short");
  if (LLVM_UNLIKELY(compactRes == vm::ExecutionStatus::EXCEPTION))
    return compactRes.getStatus();
  compactDisplay_ = compactRes.getValue();

  if (readOption(runtime, reader, options, u"useGrouping", Option::Kind::BoolOrString) ==
      vm::ExecutionStatus::EXCEPTION) return vm::ExecutionStatus::EXCEPTION;
  useGrouping_ = notation_ == u"compact" ? u"min2" : u"auto";
  auto grouping = options.find(u"useGrouping");
  if (grouping != options.end()) {
    if (grouping->second.isBool()) {
      useGrouping_ = grouping->second.getBool() ? u"always" : u"off";
    } else {
      const auto &value = grouping->second.getString();
      if (value == u"auto" || value == u"always" || value == u"min2")
        useGrouping_ = value;
      else if (value != u"true" && value != u"false")
        return runtime.raiseRangeError("Invalid useGrouping");
    }
  }

  // signDisplay
  static const std::vector<std::u16string> validSignDisplays = {
      u"auto", u"never", u"always", u"exceptZero", u"negative"};
  auto signRes = getOptionString(
      runtime, reader, options, u"signDisplay", validSignDisplays, u"auto");
  if (LLVM_UNLIKELY(signRes == vm::ExecutionStatus::EXCEPTION))
    return signRes.getStatus();
  signDisplay_ = signRes.getValue();

  std::u16string skeleton;
  if (style_ == u"currency") {
    skeleton += u"currency/" + currency_;
    if (currencyDisplay_ == u"code") skeleton += u" unit-width-iso-code";
    else if (currencyDisplay_ == u"name") skeleton += u" unit-width-full-name";
    else if (currencyDisplay_ == u"narrowSymbol") skeleton += u" unit-width-narrow";
    else skeleton += u" unit-width-short";
  } else if (style_ == u"unit") {
    skeleton += u"unit/" + unit_;
    if (unitDisplay_ == u"long") skeleton += u" unit-width-full-name";
    else if (unitDisplay_ == u"narrow") skeleton += u" unit-width-narrow";
    else skeleton += u" unit-width-short";
  } else if (style_ == u"percent") {
    skeleton += u"percent scale/100";
  }
  if (notation_ == u"scientific") skeleton += u" scientific";
  else if (notation_ == u"engineering") skeleton += u" engineering";
  else if (notation_ == u"compact")
    skeleton += compactDisplay_ == u"long" ? u" compact-long" : u" compact-short";

  const bool accounting = style_ == u"currency" && currencySign_ == u"accounting";
  if (signDisplay_ == u"never") skeleton += u" sign-never";
  else if (signDisplay_ == u"always")
    skeleton += accounting ? u" sign-accounting-always" : u" sign-always";
  else if (signDisplay_ == u"exceptZero")
    skeleton += accounting ? u" sign-accounting-except-zero" : u" sign-except-zero";
  else if (signDisplay_ == u"negative")
    skeleton += accounting ? u" sign-accounting-negative" : u" sign-negative";
  else if (accounting) skeleton += u" sign-accounting";
  else skeleton += u" sign-auto";

  skeleton += u" group-";
  skeleton += useGrouping_ == u"always" ? u"on-aligned" : useGrouping_;
  skeleton += u" integer-width/+" + std::u16string(minimumIntegerDigits_, u'0');
  static const std::unordered_map<std::u16string, std::u16string> roundingModes{
      {u"ceil", u"ceiling"}, {u"floor", u"floor"}, {u"expand", u"up"},
      {u"trunc", u"down"}, {u"halfCeil", u"half-ceiling"},
      {u"halfFloor", u"half-floor"}, {u"halfExpand", u"half-up"},
      {u"halfTrunc", u"half-down"}, {u"halfEven", u"half-even"}};
  skeleton += u" rounding-mode-" + roundingModes.at(roundingMode_);
  skeleton += u" ";
  if (roundingIncrement_ != 1) {
    std::string decimal = std::to_string(roundingIncrement_);
    if (maximumFractionDigits_ > 0) {
      if (decimal.size() <= static_cast<size_t>(maximumFractionDigits_))
        decimal.insert(0, maximumFractionDigits_ + 1 - decimal.size(), '0');
      decimal.insert(decimal.size() - maximumFractionDigits_, 1, '.');
    }
    skeleton += u"precision-increment/" + toUTF16ASCII(decimal);
  } else {
    if (needFD) {
      if (maximumFractionDigits_ == 0) skeleton += u"precision-integer";
      else skeleton += u"." + std::u16string(minimumFractionDigits_, u'0') +
          std::u16string(maximumFractionDigits_ - minimumFractionDigits_, u'#');
    }
    if (needSD) {
      if (needFD) skeleton += u"/";
      skeleton += std::u16string(minimumSignificantDigits_, u'@') +
          std::u16string(maximumSignificantDigits_ - minimumSignificantDigits_, u'#');
      if (needFD) skeleton += roundingPriority_ == u"morePrecision" ? u"r" : u"s";
    }
  }
  if (trailingZeroDisplay_ == u"stripIfInteger") skeleton += u"/w";
  skeleton_ = skeleton;
  localeICU_ = localeICU;
  status = U_ZERO_ERROR;
  unumf_ = icu_->unumf_openForSkeletonAndLocale(
      reinterpret_cast<const UChar *>(skeleton.data()), skeleton.size(),
      localeICU.c_str(), &status);
  if (U_FAILURE(status) || !unumf_)
    return runtime.raiseError("ICU number formatter initialization failed");
  status = U_ZERO_ERROR;
  unumfResult_ = icu_->unumf_openResult(&status);
  if (U_FAILURE(status) || !unumfResult_)
    return runtime.raiseError("ICU formatted-number allocation failed");
  return vm::ExecutionStatus::RETURNED;
}

Options NumberFormatIcuProvider::resolvedOptions() noexcept {
  Options options;
  options.emplace(u"locale", Option(locale_));
  options.emplace(u"style", Option(style_));
  if (style_ == u"currency") {
    std::u16string currUpper;
    for (auto c : currency_) {
      currUpper += (c >= u'a' && c <= u'z') ? (c - u'a' + u'A') : c;
    }
    options.emplace(u"currency", Option(currUpper));
    options.emplace(u"currencyDisplay", Option(currencyDisplay_));
  }
  if (style_ == u"unit" && !unit_.empty()) {
    options.emplace(u"unit", Option(unit_));
    options.emplace(u"unitDisplay", Option(unitDisplay_));
  }
  if (style_ == u"currency") {
    options.emplace(u"currencySign", Option(currencySign_));
  }
  if (!numberingSystem_.empty()) {
    options.emplace(u"numberingSystem", Option(numberingSystem_));
  } else {
    options.emplace(u"numberingSystem", Option(std::u16string(u"latn")));
  }
  options.emplace(u"notation", Option(notation_));
  if (notation_ == u"compact") {
    options.emplace(u"compactDisplay", Option(compactDisplay_));
  }
  options.emplace(u"signDisplay", Option(signDisplay_));
  if (useGrouping_ == u"off") options.emplace(u"useGrouping", Option(false));
  else options.emplace(u"useGrouping", Option(useGrouping_));
  options.emplace(u"roundingIncrement", Option(static_cast<double>(roundingIncrement_)));
  options.emplace(u"roundingMode", Option(roundingMode_));
  options.emplace(u"roundingPriority", Option(roundingPriority_));
  options.emplace(u"trailingZeroDisplay", Option(trailingZeroDisplay_));
  options.emplace(
      u"minimumIntegerDigits",
      Option(static_cast<double>(minimumIntegerDigits_)));
  if (minimumSignificantDigits_ >= 0) {
    options.emplace(
        u"minimumSignificantDigits",
        Option(static_cast<double>(minimumSignificantDigits_)));
    options.emplace(
        u"maximumSignificantDigits",
        Option(static_cast<double>(maximumSignificantDigits_)));
  }
  if (minimumFractionDigits_ >= 0) {
    options.emplace(
        u"minimumFractionDigits",
        Option(static_cast<double>(minimumFractionDigits_)));
    options.emplace(
        u"maximumFractionDigits",
        Option(static_cast<double>(maximumFractionDigits_)));
  }
  return options;
}

vm::CallResult<const UFormattedValue *> NumberFormatIcuProvider::formatValue(
    vm::Runtime &runtime, const NumberFormatValue &number) noexcept {
  // Hermes NaN boxing may set a NaN's sign bit. Intl never emits a negative NaN.
  UErrorCode status = U_ZERO_ERROR;
  if (const auto *binary = std::get_if<double>(&number)) {
    const double normalized = std::isnan(*binary) ? std::numeric_limits<double>::quiet_NaN() : *binary;
    icu_->unumf_formatDouble(unumf_, normalized, unumfResult_, &status);
  } else {
    const auto &decimal = std::get<std::string>(number);
    icu_->unumf_formatDecimal(unumf_, decimal.data(), decimal.size(), unumfResult_, &status);
  }
  if (U_FAILURE(status))
    return runtime.raiseError("ICU number formatting failed");
  const auto *value = icu_->unumf_resultAsValue(unumfResult_, &status);
  if (U_FAILURE(status) || !value)
    return runtime.raiseError("ICU formatted-number retrieval failed");
  return value;
}

vm::CallResult<std::u16string> NumberFormatIcuProvider::format(
    vm::Runtime &runtime, const NumberFormatValue &number) noexcept {
  auto value = formatValue(runtime, number);
  if (value == vm::ExecutionStatus::EXCEPTION)
    return value.getStatus();
  UErrorCode status = U_ZERO_ERROR;
  int32_t length;
  const auto *text = icu_->ufmtval_getString(*value, &length, &status);
  if (U_FAILURE(status) || !text)
    return runtime.raiseError("ICU formatted-number string retrieval failed");
  return std::u16string(reinterpret_cast<const char16_t *>(text), length);
}

vm::CallResult<std::vector<Part>> partitionNumberFields(
    vm::Runtime &runtime, const std::u16string &formatted,
    const std::vector<IntlFieldRange> &fields,
    const std::vector<IntlFieldRange> &sources, bool outputSource,
    bool isNaN, bool styleIsUnit) {
  const int32_t length = formatted.size();
  std::vector<int32_t> endpoints{0, length};
  for (const auto *ranges : {&fields, &sources})
    for (const auto &field : *ranges) {
      if (field.begin < 0 || field.begin >= field.end || field.end > length)
        return runtime.raiseError("ICU number-field range is invalid");
      if (ranges == &fields) {
        endpoints.push_back(field.begin);
        endpoints.push_back(field.end);
      }
    }
  for (const auto &span : sources)
    if (span.type != 0 && span.type != 1)
      return runtime.raiseError("ICU number-range source is invalid");
  // Source spans classify complete parts; they do not split literals. A literal
  // crossing an endpoint boundary is shared, as in V8's field partitioning.
  // Integer fields include group fields: select the most specific covering field.
  std::sort(endpoints.begin(), endpoints.end());
  endpoints.erase(std::unique(endpoints.begin(), endpoints.end()), endpoints.end());
  std::vector<Part> parts;
  for (size_t i = 1; i < endpoints.size(); ++i) {
    const auto begin = endpoints[i - 1], end = endpoints[i];
    const IntlFieldRange *selected = nullptr;
    for (const auto &field : fields)
      if (field.begin <= begin && field.end >= end &&
          (!selected || field.end - field.begin < selected->end - selected->begin ||
           (field.end - field.begin == selected->end - selected->begin && field.type > selected->type)))
        selected = &field;
    std::u16string type = u"literal";
    std::u16string source = rangeSource(sources, begin, end);
    auto segment = formatted.substr(begin, end - begin);
    if (selected) {
      type = numberFieldToPartType(selected->type);
      if (selected->type == UNUM_MEASURE_UNIT_FIELD ||
          (selected->type == UNUM_PERCENT_FIELD && styleIsUnit)) type = u"unit";
      else if (selected->type == UNUM_COMPACT_FIELD) type = u"compact";
      else if (selected->type == UNUM_APPROXIMATELY_SIGN_FIELD) type = u"approximatelySign";
      else if (selected->type == UNUM_SIGN_FIELD)
        type = formatted[begin] == u'+' ? u"plusSign" : u"minusSign";
      else if (selected->type == UNUM_INTEGER_FIELD && isNaN) type = u"nan";
      else if (selected->type == UNUM_INTEGER_FIELD &&
          (formatted[begin] == u'\u221e' || segment == u"INF")) type = u"infinity";
    }
    if (!parts.empty() && parts.back().at(u"type") == type &&
        (!outputSource || parts.back().at(u"source") == source))
      parts.back().at(u"value") += segment;
    else {
      Part part{{u"type", std::move(type)}, {u"value", std::move(segment)}};
      if (outputSource) part.emplace(u"source", std::move(source));
      parts.push_back(std::move(part));
    }
  }
  return parts;
}

vm::CallResult<std::vector<Part>> NumberFormatIcuProvider::formatToParts(
    vm::Runtime &runtime, const NumberFormatValue &number) noexcept {
  auto value = formatValue(runtime, number);
  if (value == vm::ExecutionStatus::EXCEPTION)
    return value.getStatus();
  UErrorCode status = U_ZERO_ERROR;
  int32_t length;
  const auto *text = icu_->ufmtval_getString(*value, &length, &status);
  if (U_FAILURE(status) || !text)
    return runtime.raiseError("ICU formatted-number string retrieval failed");
  const std::u16string formatted(reinterpret_cast<const char16_t *>(text), length);
  auto *positions = icu_->ufieldpositer_open(&status);
  if (U_FAILURE(status) || !positions)
    return runtime.raiseError("ICU number-field iterator allocation failed");
  icu_->unumf_resultGetAllFieldPositions(unumfResult_, positions, &status);
  if (U_FAILURE(status)) {
    icu_->ufieldpositer_close(positions);
    return runtime.raiseError("ICU number-field retrieval failed");
  }
  std::vector<IntlFieldRange> fields;
  int32_t begin, end, type;
  while ((type = icu_->ufieldpositer_next(positions, &begin, &end)) >= 0)
    fields.push_back({begin, end, type});
  icu_->ufieldpositer_close(positions);
  const auto *binary = std::get_if<double>(&number);
  return partitionNumberFields(runtime, formatted, fields, {}, false,
      binary && std::isnan(*binary), style_ == u"unit");
}

vm::CallResult<icu::number::FormattedNumberRange> NumberFormatIcuProvider::formatRangeValue(
    vm::Runtime &runtime, const NumberFormatValue &start, const NumberFormatValue &end) noexcept {
  UErrorCode status = U_ZERO_ERROR;
  if (!rangeFormatter_) {
    auto formatter = icu::number::NumberRangeFormatter::with()
        .numberFormatterBoth(icu::number::NumberFormatter::forSkeleton(
            icu::UnicodeString(skeleton_.data(), skeleton_.size()), status))
        .locale(icu::Locale(localeICU_.c_str()));
    if (U_FAILURE(status) || formatter.copyErrorTo(status))
      return runtime.raiseError("ICU number-range formatter initialization failed");
    rangeFormatter_.emplace(std::move(formatter));
  }
  // ICU's C range API accepts two doubles or two decimal strings. Its C++ API
  // preserves mixed endpoint types without rounding either exact decimal input.
  icu::Formattable first, second;
  if (const auto *binary = std::get_if<double>(&start)) first.setDouble(*binary);
  else first.setDecimalNumber(icu::StringPiece(std::get<std::string>(start)), status);
  if (const auto *binary = std::get_if<double>(&end)) second.setDouble(*binary);
  else second.setDecimalNumber(icu::StringPiece(std::get<std::string>(end)), status);
  if (U_FAILURE(status)) return runtime.raiseError("ICU number-range endpoint conversion failed");
  auto result = rangeFormatter_->formatFormattableRange(first, second, status);
  if (U_FAILURE(status)) return runtime.raiseError("ICU number-range formatting failed");
  return std::move(result);
}

vm::CallResult<std::u16string> NumberFormatIcuProvider::formatRange(
    vm::Runtime &runtime, const NumberFormatValue &start, const NumberFormatValue &end) noexcept {
  auto value = formatRangeValue(runtime, start, end);
  if (value == vm::ExecutionStatus::EXCEPTION) return value.getStatus();
  UErrorCode status = U_ZERO_ERROR;
  auto text = value->toString(status);
  if (U_FAILURE(status)) return runtime.raiseError("ICU number-range string retrieval failed");
  return std::u16string(text.getBuffer(), text.length());
}

vm::CallResult<std::vector<Part>> NumberFormatIcuProvider::formatRangeToParts(
    vm::Runtime &runtime, const NumberFormatValue &start, const NumberFormatValue &end) noexcept {
  auto value = formatRangeValue(runtime, start, end);
  if (value == vm::ExecutionStatus::EXCEPTION) return value.getStatus();
  UErrorCode status = U_ZERO_ERROR;
  auto text = value->toString(status);
  std::vector<IntlFieldRange> fields, sources;
  icu::ConstrainedFieldPosition position;
  while (value->nextPosition(position, status)) {
    IntlFieldRange field{position.getStart(), position.getLimit(), position.getField()};
    if (position.getCategory() == UFIELD_CATEGORY_NUMBER_RANGE_SPAN) sources.push_back(field);
    else if (position.getCategory() == UFIELD_CATEGORY_NUMBER) fields.push_back(field);
    else return runtime.raiseError("ICU number-range field category is invalid");
  }
  if (U_FAILURE(status)) return runtime.raiseError("ICU number-range field retrieval failed");
  return partitionNumberFields(runtime, std::u16string(text.getBuffer(), text.length()),
      fields, sources, true, false, style_ == u"unit");
}

} // namespace

NumberFormat::NumberFormat() = default;
NumberFormat::~NumberFormat() = default;

vm::CallResult<std::vector<std::u16string>> NumberFormat::supportedLocalesOf(
    vm::Runtime &runtime,
    const std::vector<std::u16string> &locales,
    const Options &options) noexcept {
  const hermes_icu_vtable *icu = getIcuVtableForRuntime(runtime);

  auto availableLocales = getAvailableLocalesU16(runtime, icu, LocaleResource::NumberElements);
  if (availableLocales == vm::ExecutionStatus::EXCEPTION) return availableLocales.getStatus();
  auto requestedLocales = getCanonicalLocales(runtime, locales);
  if (LLVM_UNLIKELY(requestedLocales == vm::ExecutionStatus::EXCEPTION))
    return vm::ExecutionStatus::EXCEPTION;
  return supportedLocales(
      runtime, *availableLocales, requestedLocales.getValue(), options);
}

vm::CallResult<std::unique_ptr<NumberFormat>> NumberFormat::create(
    vm::Runtime &runtime,
    const std::vector<std::u16string> &locales,
    const Options &options) noexcept {
  MapOptionReader reader(options);
  return create(runtime, locales, reader);
}

vm::CallResult<std::unique_ptr<NumberFormat>> NumberFormat::create(
    vm::Runtime &runtime,
    const std::vector<std::u16string> &locales,
    OptionReader &reader) noexcept {
  const hermes_icu_vtable *icu = getIcuVtableForRuntime(runtime);
  auto instance = std::make_unique<NumberFormatIcuProvider>();
  if (LLVM_UNLIKELY(
          instance->initialize(runtime, icu, locales, reader) ==
          vm::ExecutionStatus::EXCEPTION)) {
    return vm::ExecutionStatus::EXCEPTION;
  }
  return instance;
}

Options NumberFormat::resolvedOptions() noexcept {
  return static_cast<NumberFormatIcuProvider *>(this)->resolvedOptions();
}

vm::CallResult<std::u16string> NumberFormat::format(vm::Runtime &runtime, const NumberFormatValue &number) noexcept {
  return static_cast<NumberFormatIcuProvider *>(this)->format(runtime, number);
}

vm::CallResult<std::vector<Part>> NumberFormat::formatToParts(vm::Runtime &runtime, const NumberFormatValue &number) noexcept {
  return static_cast<NumberFormatIcuProvider *>(this)->formatToParts(runtime, number);
}

vm::CallResult<std::u16string> NumberFormat::formatRange(
    vm::Runtime &runtime, const NumberFormatValue &start, const NumberFormatValue &end) noexcept {
  return static_cast<NumberFormatIcuProvider *>(this)->formatRange(runtime, start, end);
}

vm::CallResult<std::vector<Part>> NumberFormat::formatRangeToParts(
    vm::Runtime &runtime, const NumberFormatValue &start, const NumberFormatValue &end) noexcept {
  return static_cast<NumberFormatIcuProvider *>(this)->formatRangeToParts(runtime, start, end);
}

} // namespace platform_intl
} // namespace hermes
