/*
 * Copyright (c) Microsoft Corporation.
 * Licensed under the MIT License.
 */

/**
 * Shared helper functions for the Windows Intl implementation.
 *
 * Extracted from the former PlatformIntlShared.cpp and
 * PlatformIntlWindows.cpp to allow separate compilation (no textual
 * includes) and to isolate Windows-specific code from upstream files.
 */

#include "IcuProviderUtils.h"
#include "hermes/VM/Operations.h"

#include "hermes/Platform/Intl/BCP47Parser.h"
#include "llvh/Support/ConvertUTF.h"

#include <cmath>
#include <limits>
#include <unordered_set>

using namespace ::hermes;

namespace hermes {
namespace platform_intl {

vm::CallResult<std::u16string> UTF8toUTF16(
    vm::Runtime &runtime,
    std::string_view in) {
  std::u16string out;
  size_t length = in.length();
  out.resize(length);
  const llvh::UTF8 *sourceStart =
      reinterpret_cast<const llvh::UTF8 *>(in.data());
  const llvh::UTF8 *sourceEnd = sourceStart + length;
  llvh::UTF16 *targetStart = reinterpret_cast<llvh::UTF16 *>(&out[0]);
  llvh::UTF16 *targetEnd = targetStart + out.size();
  llvh::ConversionResult convRes = ConvertUTF8toUTF16(
      &sourceStart,
      sourceEnd,
      &targetStart,
      targetEnd,
      llvh::lenientConversion);
  if (convRes != llvh::ConversionResult::conversionOK) {
    return runtime.raiseRangeError("utf8 to utf16 conversion failed");
  }
  out.resize(reinterpret_cast<char16_t *>(targetStart) - &out[0]);
  return out;
}

vm::CallResult<std::string> UTF16toUTF8(
    vm::Runtime &runtime,
    const std::u16string &in) {
  std::string out;
  size_t length = in.length();
  // UTF-8 may need up to 4 bytes per code point.
  out.resize(length * 4);
  const llvh::UTF16 *sourceStart =
      reinterpret_cast<const llvh::UTF16 *>(in.data());
  const llvh::UTF16 *sourceEnd = sourceStart + length;
  llvh::UTF8 *targetStart = reinterpret_cast<llvh::UTF8 *>(&out[0]);
  llvh::UTF8 *targetEnd = targetStart + out.size();
  llvh::ConversionResult convRes = ConvertUTF16toUTF8(
      &sourceStart,
      sourceEnd,
      &targetStart,
      targetEnd,
      llvh::lenientConversion);
  if (convRes != llvh::ConversionResult::conversionOK) {
    return runtime.raiseRangeError("utf16 to utf8 conversion failed");
  }
  out.resize(reinterpret_cast<char *>(targetStart) - &out[0]);
  return out;
}

vm::CallResult<std::u16string> normalizeLanguageTag(
    vm::Runtime &runtime,
    const hermes_icu_vtable *icu,
    const std::u16string &locale) {
  if (locale.empty()) {
    return runtime.raiseRangeError("Invalid language tag:");
  }

  // Step 1: Structural validation via BCP47 parser.
  // ICU's uloc_forLanguageTag is more lenient than ECMA-402 — it accepts
  // underscores, private-use-only tags, invalid grandfathered tags, and
  // some structurally invalid extensions. BCP47Parser enforces strict
  // ECMA-402 structural validation (matching upstream impl_icu behavior).
  auto parsed = ParsedLocaleIdentifier::parse(locale);
  if (!parsed) {
    return runtime.raiseRangeError(
        vm::TwineChar16("Invalid language tag: ") +
        vm::TwineChar16(locale.c_str()));
  }

  // Match the reference V8's fast path for canonical two-letter languages.
  // All other inputs use ICU, including non-lowercase spellings of these tags.
  static const std::unordered_set<std::u16string> legacyLanguages = {
      u"in", u"iw", u"ji", u"jw", u"mo", u"sh", u"tl", u"no"};
  if ((locale.size() == 2 && locale[0] >= u'a' && locale[0] <= u'z' &&
       locale[1] >= u'a' && locale[1] <= u'z' && !legacyLanguages.count(locale)) ||
      locale == u"fil")
    return locale;

  auto conversion = UTF16toUTF8(runtime, locale);
  if (conversion == vm::ExecutionStatus::EXCEPTION) return conversion.getStatus();
  auto langTag = toLowerASCII(*conversion);
  assert(icu->canonicalize_locale_tag && "Static ICU canonicalizer is required");
  std::vector<char> result(kULOC_FULLNAME_CAPACITY);
  UErrorCode status = U_ZERO_ERROR;
  int32_t length = icu->canonicalize_locale_tag(
      langTag.c_str(), result.data(), result.size(), &status);
  if (status == U_BUFFER_OVERFLOW_ERROR) {
    if (length <= static_cast<int32_t>(result.size()))
      return runtime.raiseError("ICU canonical locale size is invalid");
    // The provider reports the required capacity including the terminator.
    result.resize(length);
    status = U_ZERO_ERROR;
    length = icu->canonicalize_locale_tag(
        langTag.c_str(), result.data(), result.size(), &status);
  }
  if (status == U_ILLEGAL_ARGUMENT_ERROR)
    return runtime.raiseRangeError("Invalid language tag");
  if (U_FAILURE(status) || length <= 0 || length >= static_cast<int32_t>(result.size()))
    return runtime.raiseError("ICU locale canonicalization failed");
  return toUTF16ASCII(std::string(result.data(), length));
}

vm::CallResult<std::vector<std::u16string>> canonicalizeLocaleList(
    vm::Runtime &runtime,
    const hermes_icu_vtable *icu,
    const std::vector<std::u16string> &locales) {
  if (locales.empty()) {
    return std::vector<std::u16string>{};
  }

  std::vector<std::u16string> seen;
  for (size_t k = 0; k < locales.size(); k++) {
    auto canonicalizedTag = normalizeLanguageTag(runtime, icu, locales[k]);
    if (LLVM_UNLIKELY(canonicalizedTag == vm::ExecutionStatus::EXCEPTION)) {
      return vm::ExecutionStatus::EXCEPTION;
    }
    if (std::find(seen.begin(), seen.end(), canonicalizedTag.getValue()) ==
        seen.end()) {
      seen.push_back(std::move(canonicalizedTag.getValue()));
    }
  }
  return seen;
}

vm::CallResult<std::u16string> getOptionString(
    vm::Runtime &runtime,
    const Options &options,
    const std::u16string &property,
    const std::vector<std::u16string> &validValues,
    const std::u16string &fallback) {
  auto valueIt = options.find(property);
  if (valueIt == options.end())
    return std::u16string(fallback);

  const auto &value = valueIt->second.getString();
  if (!validValues.empty() &&
      llvh::find(validValues, value) == validValues.end())
    return runtime.raiseRangeError(
        vm::TwineChar16(property.c_str()) +
        vm::TwineChar16(" value is invalid."));
  return std::u16string(value);
}

vm::ExecutionStatus readOption(
    vm::Runtime &runtime, OptionReader &reader, Options &options,
    const std::u16string &property, Option::Kind kind) {
  auto value = reader.read(runtime, property, kind);
  if (value == vm::ExecutionStatus::EXCEPTION) return value.getStatus();
  if (value->has_value()) options.emplace(property, std::move(value->value()));
  return vm::ExecutionStatus::RETURNED;
}

vm::CallResult<std::u16string> getOptionString(
    vm::Runtime &runtime, OptionReader &reader, Options &options,
    const std::u16string &property,
    const std::vector<std::u16string> &validValues,
    const std::u16string &fallback) {
  if (readOption(runtime, reader, options, property, Option::Kind::String) ==
      vm::ExecutionStatus::EXCEPTION) return vm::ExecutionStatus::EXCEPTION;
  return getOptionString(runtime, options, property, validValues, fallback);
}

vm::CallResult<std::optional<int>> getNumberOption(
    vm::Runtime &runtime, OptionReader &reader, Options &options,
    const std::u16string &property, int minimum, int maximum,
    std::optional<int> fallback) {
  if (readOption(runtime, reader, options, property, Option::Kind::Number) ==
      vm::ExecutionStatus::EXCEPTION) return vm::ExecutionStatus::EXCEPTION;
  return getNumberOption(runtime, options, property, minimum, maximum, fallback);
}

std::optional<bool> getOptionBool(
    vm::Runtime &runtime,
    const Options &options,
    const std::u16string &property,
    std::optional<bool> fallback) {
  auto value = options.find(property);
  if (value == options.end()) {
    return fallback;
  }
  return value->second.getBool();
}

std::optional<std::u16string> bestAvailableLocale(
    const std::vector<std::u16string> &availableLocales,
    const std::u16string &locale) {
  std::u16string candidate = locale;
  while (true) {
    if (llvh::find(availableLocales, candidate) != availableLocales.end())
      return candidate;
    size_t pos = candidate.rfind(u'-');
    if (pos == std::u16string::npos)
      return std::nullopt;
    if (pos >= 2 && candidate[pos - 2] == '-')
      pos -= 2;
    candidate.resize(pos);
  }
}

std::vector<std::u16string> lookupSupportedLocales(
    const std::vector<std::u16string> &availableLocales,
    const std::vector<std::u16string> &requestedLocales) {
  std::vector<std::u16string> subset;
  for (const std::u16string &locale : requestedLocales) {
    std::optional<std::u16string> availableLocale =
        bestAvailableLocale(availableLocales, locale);
    if (availableLocale) {
      subset.push_back(locale);
    }
  }
  return subset;
}

vm::CallResult<std::vector<std::u16string>> supportedLocales(
    vm::Runtime &runtime,
    const std::vector<std::u16string> &availableLocales,
    const std::vector<std::u16string> &requestedLocales,
    const Options &options) {
  // ECMA-402 9.2.10: Validate localeMatcher option.
  static const std::vector<std::u16string> validMatchers = {
      u"lookup", u"best fit"};
  auto matcherRes = getOptionString(
      runtime, options, u"localeMatcher", validMatchers, u"best fit");
  if (LLVM_UNLIKELY(matcherRes == vm::ExecutionStatus::EXCEPTION))
    return vm::ExecutionStatus::EXCEPTION;
  return lookupSupportedLocales(availableLocales, requestedLocales);
}

vm::CallResult<Options> toDateTimeOptions(
    vm::Runtime &runtime,
    Options options,
    std::u16string_view required,
    std::u16string_view defaults) {
  bool needDefaults = true;
  if (required == u"date" || required == u"any") {
    static constexpr std::u16string_view props[] = {
        u"weekday", u"year", u"month", u"day"};
    for (const auto &prop : props) {
      if (options.find(std::u16string(prop)) != options.end()) {
        needDefaults = false;
      }
    }
  }
  if (required == u"time" || required == u"any") {
    static constexpr std::u16string_view props[] = {
        u"dayPeriod",
        u"hour",
        u"minute",
        u"second",
        u"fractionalSecondDigits"};
    for (const auto &prop : props) {
      if (options.find(std::u16string(prop)) != options.end()) {
        needDefaults = false;
      }
    }
  }
  auto dateStyle = options.find(u"dateStyle");
  auto timeStyle = options.find(u"timeStyle");
  if (dateStyle != options.end() || timeStyle != options.end()) {
    needDefaults = false;
  }
  if (required == u"date" && timeStyle != options.end()) {
    return runtime.raiseTypeError(
        "Unexpectedly found timeStyle option for \"date\" property");
  }
  if (required == u"time" && dateStyle != options.end()) {
    return runtime.raiseTypeError(
        "Unexpectedly found dateStyle option for \"time\" property");
  }
  if (needDefaults && (defaults == u"date" || defaults == u"all")) {
    static constexpr std::u16string_view props[] = {u"year", u"month", u"day"};
    for (const auto &prop : props) {
      options.emplace(prop, Option(std::u16string(u"numeric")));
    }
  }
  if (needDefaults && (defaults == u"time" || defaults == u"all")) {
    static constexpr std::u16string_view props[] = {
        u"hour", u"minute", u"second"};
    for (const auto &prop : props) {
      options.emplace(prop, Option(std::u16string(u"numeric")));
    }
  }
  return options;
}

vm::CallResult<std::optional<int>> getNumberOption(
    vm::Runtime &runtime,
    const Options &options,
    const std::u16string &property,
    int minimum,
    int maximum,
    std::optional<int> fallback) {
  auto valueIt = options.find(property);
  if (valueIt == options.end())
    return fallback;

  double value;
  if (valueIt->second.isDeferredNumber()) {
    auto converted = vm::toNumber_RJS(runtime, valueIt->second.getDeferredNumber());
    if (converted == vm::ExecutionStatus::EXCEPTION) return converted.getStatus();
    value = converted->getNumber();
  } else {
    value = valueIt->second.getNumber();
  }
  if (std::isnan(value) || value < minimum || value > maximum) {
    return runtime.raiseRangeError(
        vm::TwineChar16(property.c_str()) +
        vm::TwineChar16(" value is invalid."));
  }
  return static_cast<int>(std::floor(value));
}

namespace {
// ICU locale IDs and BCP47 tags can exceed ULOC_FULLNAME_CAPACITY. Preflight
// their actual size, then reject an incomplete or failed conversion explicitly.
template <typename Convert>
vm::CallResult<std::string> readLocaleConversion(vm::Runtime &runtime, Convert convert) {
  UErrorCode status = U_ZERO_ERROR;
  const int32_t length = convert(nullptr, 0, &status);
  if ((U_FAILURE(status) && status != U_BUFFER_OVERFLOW_ERROR) ||
      length < 0 || length == std::numeric_limits<int32_t>::max())
    return runtime.raiseError("ICU locale conversion sizing failed");
  // An empty ICU ID represents the root locale, including BCP47 `und`.
  std::string result(static_cast<size_t>(length) + 1, '\0');
  status = U_ZERO_ERROR;
  const int32_t written = convert(result.data(), length + 1, &status);
  if (U_FAILURE(status) || written != length)
    return runtime.raiseError("ICU locale conversion failed");
  result.resize(length);
  return result;
}
} // namespace

vm::CallResult<std::string> convertBCP47toICULocale(
    vm::Runtime &runtime, const hermes_icu_vtable *icu,
    const std::u16string &localeBCP47) {
  auto locale8 = UTF16toUTF8(runtime, localeBCP47);
  if (locale8 == vm::ExecutionStatus::EXCEPTION) return locale8.getStatus();
  int32_t parsedLength = 0;
  auto result = readLocaleConversion(runtime, [&](char *buffer, int32_t capacity, UErrorCode *status) {
    return icu->uloc_forLanguageTag(locale8->c_str(), buffer, capacity, &parsedLength, status);
  });
  if (result == vm::ExecutionStatus::EXCEPTION) return result.getStatus();
  if (parsedLength < 0 || static_cast<size_t>(parsedLength) != locale8->size())
    return runtime.raiseError("ICU locale conversion did not consume the complete tag");
  return std::move(*result);
}

vm::CallResult<std::string> convertICUtoBCP47Locale(
    vm::Runtime &runtime, const hermes_icu_vtable *icu, const char *localeICU) {
  if (!localeICU) return runtime.raiseError("ICU locale identifier is missing");
  return readLocaleConversion(runtime, [&](char *buffer, int32_t capacity, UErrorCode *status) {
    return icu->uloc_toLanguageTag(localeICU, buffer, capacity, true, status);
  });
}

std::string toUTF8ASCII(const std::u16string &str) {
  std::string result;
  result.reserve(str.size());
  for (char16_t c : str) {
    result += static_cast<char>(c < 128 ? c : '?');
  }
  return result;
}

std::u16string toUTF16ASCII(const std::string &str) {
  std::u16string result;
  result.reserve(str.size());
  for (char c : str) {
    result += static_cast<char16_t>(static_cast<unsigned char>(c));
  }
  return result;
}

bool convertToBool(const std::u16string &str) {
  return str == u"true";
}

std::string toLowerASCII(const std::string &str) {
  std::string result = str;
  for (auto &c : result) {
    if (c >= 'A' && c <= 'Z')
      c = c - 'A' + 'a';
  }
  return result;
}

// ============================================================================
// Field type mapping for formatToParts (ECMA-402 Section 11.5.7 / 15.5.5)
// ============================================================================

const char16_t *dateFieldToPartType(int32_t field) {
  switch (field) {
    case kUDAT_ERA_FIELD:
      return u"era";
    case kUDAT_YEAR_FIELD:
    case kUDAT_YEAR_WOY_FIELD:
    case kUDAT_EXTENDED_YEAR_FIELD:
      return u"year";
    case kUDAT_RELATED_YEAR_FIELD:
      return u"relatedYear";
    case kUDAT_YEAR_NAME_FIELD:
      return u"yearName";
    case kUDAT_MONTH_FIELD:
    case kUDAT_STANDALONE_MONTH_FIELD:
      return u"month";
    case kUDAT_DATE_FIELD:
      return u"day";
    case kUDAT_HOUR_OF_DAY1_FIELD:
    case kUDAT_HOUR_OF_DAY0_FIELD:
    case kUDAT_HOUR1_FIELD:
    case kUDAT_HOUR0_FIELD:
      return u"hour";
    case kUDAT_MINUTE_FIELD:
      return u"minute";
    case kUDAT_SECOND_FIELD:
      return u"second";
    case kUDAT_FRACTIONAL_SECOND_FIELD:
      return u"fractionalSecond";
    case kUDAT_DAY_OF_WEEK_FIELD:
    case kUDAT_DOW_LOCAL_FIELD:
    case kUDAT_STANDALONE_DAY_FIELD:
      return u"weekday";
    case kUDAT_AM_PM_FIELD:
    case kUDAT_AM_PM_MIDNIGHT_NOON_FIELD:
    case kUDAT_FLEXIBLE_DAY_PERIOD_FIELD:
      return u"dayPeriod";
    case kUDAT_TIMEZONE_FIELD:
    case kUDAT_TIMEZONE_RFC_FIELD:
    case kUDAT_TIMEZONE_GENERIC_FIELD:
    case kUDAT_TIMEZONE_SPECIAL_FIELD:
    case kUDAT_TIMEZONE_LOCALIZED_GMT_OFFSET_FIELD:
    case kUDAT_TIMEZONE_ISO_FIELD:
    case kUDAT_TIMEZONE_ISO_LOCAL_FIELD:
      return u"timeZoneName";
    default:
      return u"literal";
  }
}

const char16_t *numberFieldToPartType(int32_t field) {
  switch (field) {
    case kUNUM_INTEGER_FIELD:
      return u"integer";
    case kUNUM_FRACTION_FIELD:
      return u"fraction";
    case kUNUM_DECIMAL_SEPARATOR_FIELD:
      return u"decimal";
    case kUNUM_EXPONENT_SYMBOL_FIELD:
      return u"exponentSeparator";
    case kUNUM_EXPONENT_SIGN_FIELD:
      return u"exponentMinusSign";
    case kUNUM_EXPONENT_FIELD:
      return u"exponentInteger";
    case kUNUM_GROUPING_SEPARATOR_FIELD:
      return u"group";
    case kUNUM_CURRENCY_FIELD:
      return u"currency";
    case kUNUM_PERCENT_FIELD:
      return u"percentSign";
    case kUNUM_PERMILL_FIELD:
      return u"percentSign";
    case kUNUM_SIGN_FIELD:
      return u"minusSign";
    default:
      return u"literal";
  }
}

// ============================================================================
// Timezone validation and canonicalization
// ============================================================================

namespace {
/// Check if all characters are ASCII.
bool isAllASCII(const std::u16string &str) {
  for (auto c : str) {
    if (c > 127)
      return false;
  }
  return true;
}

/// Parse an offset timezone string. Returns true and sets the canonical
/// offset string if valid. Format: [+-]HH, [+-]HH:MM or [+-]HHMM.
/// Hours: 00-23, Minutes: 00-59.
bool parseOffsetTimeZone(
    const std::u16string &tz,
    std::u16string &canonical) {
  if (tz.empty())
    return false;
  size_t pos = 0;
  char16_t sign = tz[0];
  // Also handle Unicode minus sign U+2212
  if (sign != u'+' && sign != u'-' && sign != u'\u2212')
    return false;
  pos = 1;

  size_t remaining = tz.size() - pos;
  int hours = -1, minutes = -1;
  const auto isDigit = [](char16_t value) {
    return value >= u'0' && value <= u'9';
  };

  if (remaining == 2) {
    if (!isDigit(tz[pos]) || !isDigit(tz[pos + 1]))
      return false;
    hours = (tz[pos] - u'0') * 10 + (tz[pos + 1] - u'0');
    minutes = 0;
  } else if (remaining == 4) {
    // +HHMM
    if (!isDigit(tz[pos]) || !isDigit(tz[pos + 1]) ||
        !isDigit(tz[pos + 2]) || !isDigit(tz[pos + 3]))
      return false;
    hours = (tz[pos] - u'0') * 10 + (tz[pos + 1] - u'0');
    minutes = (tz[pos + 2] - u'0') * 10 + (tz[pos + 3] - u'0');
  } else if (remaining == 5 && tz[pos + 2] == u':') {
    // +HH:MM
    if (!isDigit(tz[pos]) || !isDigit(tz[pos + 1]) ||
        !isDigit(tz[pos + 3]) || !isDigit(tz[pos + 4]))
      return false;
    hours = (tz[pos] - u'0') * 10 + (tz[pos + 1] - u'0');
    minutes = (tz[pos + 3] - u'0') * 10 + (tz[pos + 4] - u'0');
  } else {
    return false;
  }

  if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59)
    return false;

  // Canonicalize to +HH:MM format
  char16_t canonSign = (sign == u'+' || (hours == 0 && minutes == 0)) ? u'+' : u'-';
  canonical = std::u16string(1, canonSign);
  canonical += static_cast<char16_t>(u'0' + hours / 10);
  canonical += static_cast<char16_t>(u'0' + hours % 10);
  canonical += u':';
  canonical += static_cast<char16_t>(u'0' + minutes / 10);
  canonical += static_cast<char16_t>(u'0' + minutes % 10);
  return true;
}
} // namespace

vm::CallResult<std::u16string> validateAndCanonicalizeTimeZone(
    vm::Runtime &runtime,
    const hermes_icu_vtable *icu,
    const std::u16string &timeZone) {
  // Offset grammar also permits U+2212; named zones remain ASCII-only.
  std::u16string canonicalOffset;
  if (parseOffsetTimeZone(timeZone, canonicalOffset)) {
    return canonicalOffset;
  }
  // Reject non-ASCII timezone names.
  if (!isAllASCII(timeZone)) {
    return runtime.raiseRangeError("Invalid time zone");
  }

  // Check for UTC/GMT aliases (case-insensitive).
  std::string upper;
  for (auto c : timeZone) {
    upper += static_cast<char>(
        (c >= u'a' && c <= u'z') ? (c - u'a' + u'A') : c);
  }
  if (upper == "UTC" || upper == "GMT" || upper == "ETC/UTC" ||
      upper == "ETC/GMT") {
    return std::u16string(u"UTC");
  }

  // Use ICU to validate and canonicalize the timezone.
  if (icu && icu->ucal_getCanonicalTimeZoneID) {
    UChar buf[256];
    UBool isSystemID = 0;
    UErrorCode err = U_ZERO_ERROR;
    int32_t len = icu->ucal_getCanonicalTimeZoneID(
        reinterpret_cast<const UChar *>(timeZone.data()),
        static_cast<int32_t>(timeZone.size()),
        buf, 256, &isSystemID, &err);
    if (U_SUCCESS(err) && len > 0 && isSystemID) {
      std::u16string canonical(
          reinterpret_cast<const char16_t *>(buf), len);
      // Normalize Etc/UTC variants to "UTC"
      if (canonical == u"Etc/UTC" || canonical == u"Etc/GMT")
        return std::u16string(u"UTC");
      return canonical;
    }
  }

  return runtime.raiseRangeError("Invalid time zone");
}

// ============================================================================
// Unicode extension helpers
// ============================================================================

/// Find the position and length of the "-u-" unicode extension in a BCP47
/// locale string. Returns {start, end} where start is the index of the '-'
/// before 'u' and end is one past the last character of the extension
/// (before the next singleton or end of string). Returns {npos, npos} if not
/// found.
static std::pair<size_t, size_t> findUnicodeExtension(
    const std::u16string &locale) {
  // Look for "-u-" as a standalone singleton extension.
  // The pattern "-u-" guarantees 'u' is between two separators (a singleton).
  size_t found = locale.find(u"-u-");
  if (found == std::u16string::npos)
    return {std::u16string::npos, std::u16string::npos};

  // Find the end: scan forward until we hit another singleton extension
  // (a single alphanum char between two hyphens) or end of string.
  size_t end = found + 3; // skip "-u-"
  while (end < locale.size()) {
    if (locale[end] == u'-') {
      // Check if this is another singleton: "-X-" where X is 1 alphanum
      size_t nextCharPos = end + 1;
      if (nextCharPos < locale.size()) {
        size_t afterNext = nextCharPos + 1;
        if (afterNext >= locale.size() || locale[afterNext] == u'-') {
          // 'X' is a single-char subtag (singleton) — end of unicode ext
          break;
        }
      }
    }
    ++end;
  }
  return {found, end};
}

std::u16string getUnicodeExtensionValue(
    const std::u16string &locale,
    const std::u16string &key) {
  auto [extStart, extEnd] = findUnicodeExtension(locale);
  if (extStart == std::u16string::npos)
    return u"";

  // Parse the extension content (after "-u-")
  std::u16string ext = locale.substr(extStart + 3, extEnd - extStart - 3);
  // Split into subtags by '-'
  size_t pos = 0;
  while (pos < ext.size()) {
    size_t dash = ext.find(u'-', pos);
    if (dash == std::u16string::npos)
      dash = ext.size();
    std::u16string subtag = ext.substr(pos, dash - pos);

    // Keys are 2-character subtags
    if (subtag.size() == 2 && subtag == key) {
      // Collect value subtags (3+ chars each)
      std::u16string value;
      pos = dash + 1;
      while (pos < ext.size()) {
        size_t nextDash = ext.find(u'-', pos);
        if (nextDash == std::u16string::npos)
          nextDash = ext.size();
        std::u16string valSubtag = ext.substr(pos, nextDash - pos);
        if (valSubtag.size() == 2)
          break; // next key
        if (!value.empty())
          value += u'-';
        value += valSubtag;
        pos = nextDash + 1;
      }
      return value;
    }
    pos = dash + 1;
  }
  return u"";
}

std::u16string removeUnicodeExtensionKey(
    const std::u16string &locale,
    const std::u16string &key) {
  auto [extStart, extEnd] = findUnicodeExtension(locale);
  if (extStart == std::u16string::npos)
    return locale;

  // Parse extension content to find and remove the specific key
  std::u16string ext = locale.substr(extStart + 3, extEnd - extStart - 3);
  std::u16string newExt;
  size_t pos = 0;
  while (pos < ext.size()) {
    size_t dash = ext.find(u'-', pos);
    if (dash == std::u16string::npos)
      dash = ext.size();
    std::u16string subtag = ext.substr(pos, dash - pos);

    if (subtag.size() == 2 && subtag == key) {
      // Skip this key and its value subtags
      pos = dash + 1;
      while (pos < ext.size()) {
        size_t nextDash = ext.find(u'-', pos);
        if (nextDash == std::u16string::npos)
          nextDash = ext.size();
        std::u16string valSubtag = ext.substr(pos, nextDash - pos);
        if (valSubtag.size() == 2)
          break; // next key
        pos = nextDash + 1;
      }
    } else {
      // Keep this key and its value subtags
      if (!newExt.empty())
        newExt += u'-';
      newExt += subtag;
      pos = dash + 1;
      while (pos < ext.size()) {
        size_t nextDash = ext.find(u'-', pos);
        if (nextDash == std::u16string::npos)
          nextDash = ext.size();
        std::u16string valSubtag = ext.substr(pos, nextDash - pos);
        if (valSubtag.size() == 2)
          break; // next key
        newExt += u'-';
        newExt += valSubtag;
        pos = nextDash + 1;
      }
    }
  }

  // Rebuild locale
  std::u16string result = locale.substr(0, extStart);
  if (!newExt.empty()) {
    result += u"-u-";
    result += newExt;
  }
  if (extEnd < locale.size()) {
    result += locale.substr(extEnd);
  }
  return result;
}

std::u16string filterUnicodeExtensions(
    const std::u16string &locale,
    const std::unordered_set<std::u16string> &relevantKeys) {
  auto [extStart, extEnd] = findUnicodeExtension(locale);
  if (extStart == std::u16string::npos)
    return locale;

  // Parse extension content and keep only relevant keys with valid values
  std::u16string ext = locale.substr(extStart + 3, extEnd - extStart - 3);
  std::u16string newExt;
  size_t pos = 0;
  while (pos < ext.size()) {
    size_t dash = ext.find(u'-', pos);
    if (dash == std::u16string::npos)
      dash = ext.size();
    std::u16string subtag = ext.substr(pos, dash - pos);

    if (subtag.size() == 2) {
      // Collect value subtags
      std::u16string value;
      size_t valStart = dash + 1;
      size_t valEnd = valStart;
      while (valEnd < ext.size()) {
        size_t nextDash = ext.find(u'-', valEnd);
        if (nextDash == std::u16string::npos)
          nextDash = ext.size();
        std::u16string valSubtag = ext.substr(valEnd, nextDash - valEnd);
        if (valSubtag.size() == 2)
          break; // next key
        if (!value.empty())
          value += u'-';
        value += valSubtag;
        valEnd = nextDash + 1;
      }

      bool keep = relevantKeys.count(subtag) > 0;
      // Validate value for known keys per ECMA-402.
      if (keep && subtag == u"hc") {
        keep = (value == u"h11" || value == u"h12" ||
                value == u"h23" || value == u"h24");
      } else if (keep && subtag == u"nu") {
        // Must be a valid numbering system identifier and NOT a special
        // abstract value. "native", "traditio", "finance" are abstract
        // values that resolve to locale-specific numbering systems and
        // must not appear in resolvedOptions per ECMA-402.
        keep = !value.empty() && isUnicodeExtensionType(value) &&
            value != u"native" && value != u"traditio" &&
            value != u"finance";
      } else if (keep && !value.empty()) {
        keep = isUnicodeExtensionType(value);
      }

      if (keep) {
        if (!newExt.empty())
          newExt += u'-';
        newExt += subtag;
        if (!value.empty()) {
          newExt += u'-';
          newExt += value;
        }
      }
      pos = valEnd;
    } else {
      // Bare type value (no key) — attribute, skip it
      pos = dash + 1;
    }
  }

  // Rebuild locale
  std::u16string result = locale.substr(0, extStart);
  if (!newExt.empty()) {
    result += u"-u-";
    result += newExt;
  }
  if (extEnd < locale.size()) {
    result += locale.substr(extEnd);
  }
  return result;
}

// ============================================================================
// ECMA-402 unit → CLDR measure unit mapping
// ============================================================================

std::string ecma402UnitToCldrMeasureUnit(const std::u16string &unit) {
  // Sorted for binary search. Maps ECMA-402 simple unit to CLDR measure unit.
  struct UnitMapping {
    const char16_t *ecma;
    const char *cldr;
  };
  static const UnitMapping mappings[] = {
      {u"acre", "area-acre"},
      {u"bit", "digital-bit"},
      {u"byte", "digital-byte"},
      {u"celsius", "temperature-celsius"},
      {u"centimeter", "length-centimeter"},
      {u"day", "duration-day"},
      {u"degree", "angle-degree"},
      {u"fahrenheit", "temperature-fahrenheit"},
      {u"fluid-ounce", "volume-fluid-ounce"},
      {u"foot", "length-foot"},
      {u"gallon", "volume-gallon"},
      {u"gigabit", "digital-gigabit"},
      {u"gigabyte", "digital-gigabyte"},
      {u"gram", "mass-gram"},
      {u"hectare", "area-hectare"},
      {u"hour", "duration-hour"},
      {u"inch", "length-inch"},
      {u"kilobit", "digital-kilobit"},
      {u"kilobyte", "digital-kilobyte"},
      {u"kilogram", "mass-kilogram"},
      {u"kilometer", "length-kilometer"},
      {u"liter", "volume-liter"},
      {u"megabit", "digital-megabit"},
      {u"megabyte", "digital-megabyte"},
      {u"meter", "length-meter"},
      {u"microsecond", "duration-microsecond"},
      {u"mile", "length-mile"},
      {u"mile-scandinavian", "length-mile-scandinavian"},
      {u"milliliter", "volume-milliliter"},
      {u"millimeter", "length-millimeter"},
      {u"millisecond", "duration-millisecond"},
      {u"minute", "duration-minute"},
      {u"month", "duration-month"},
      {u"nanosecond", "duration-nanosecond"},
      {u"ounce", "mass-ounce"},
      {u"percent", "concentr-percent"},
      {u"petabyte", "digital-petabyte"},
      {u"pound", "mass-pound"},
      {u"second", "duration-second"},
      {u"stone", "mass-stone"},
      {u"terabit", "digital-terabit"},
      {u"terabyte", "digital-terabyte"},
      {u"week", "duration-week"},
      {u"yard", "length-yard"},
      {u"year", "duration-year"},
  };
  constexpr size_t numMappings = sizeof(mappings) / sizeof(mappings[0]);

  // Binary search
  std::u16string key(unit);
  size_t lo = 0, hi = numMappings;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int cmp = key.compare(mappings[mid].ecma);
    if (cmp == 0)
      return mappings[mid].cldr;
    if (cmp < 0)
      hi = mid;
    else
      lo = mid + 1;
  }
  return "";
}

} // namespace platform_intl
} // namespace hermes
