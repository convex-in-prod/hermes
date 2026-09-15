/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/Platform/Unicode/PlatformUnicode.h"

#if HERMES_PLATFORM_UNICODE == HERMES_PLATFORM_UNICODE_LITE

#include "hermes/Platform/Unicode/CharacterProperties.h"

#include <algorithm>

namespace hermes {
namespace platform_unicode {

namespace {
struct CaseMappingRange {
  uint32_t start;
  uint32_t count;
  int32_t delta;
  uint32_t modulo;
};

struct CaseMappingExpansion {
  uint32_t codePoint;
  uint32_t length;
  uint32_t mapped[3];
};

struct CasePropertyRange {
  uint32_t first;
  uint32_t last;
};

#include "UnicodeCaseMapping.inc"

bool hasCaseProperty(llvh::ArrayRef<CasePropertyRange> ranges, uint32_t cp) {
  auto range = std::lower_bound(
      ranges.begin(),
      ranges.end(),
      cp,
      [](const CasePropertyRange &range, uint32_t cp) {
        return range.last < cp;
      });
  return range != ranges.end() && range->first <= cp;
}

uint32_t nextCodePoint(llvh::ArrayRef<char16_t> input, size_t &index) {
  uint32_t cp = input[index++];
  if (isHighSurrogate(cp) && index < input.size() &&
      isLowSurrogate(input[index])) {
    cp = utf16SurrogatePairToCodePoint(cp, input[index++]);
  }
  return cp;
}

void appendCodePoint(llvh::SmallVectorImpl<char16_t> &output, uint32_t cp) {
  if (cp <= UNICODE_LAST_BMP) {
    // An unpaired surrogate is preserved, not replaced or decoded.
    output.push_back(static_cast<char16_t>(cp));
  } else {
    cp -= 0x10000;
    output.push_back(static_cast<char16_t>(UTF16_HIGH_SURROGATE + (cp >> 10)));
    output.push_back(static_cast<char16_t>(UTF16_LOW_SURROGATE + (cp & 0x3ff)));
  }
}

bool followedByCasedLetter(llvh::ArrayRef<char16_t> input, size_t index) {
  while (index < input.size()) {
    const uint32_t cp = nextCodePoint(input, index);
    if (!hasCaseProperty(CASE_IGNORABLE, cp)) {
      return hasCaseProperty(CASED, cp);
    }
  }
  return false;
}
} // namespace

int localeCompare(
    llvh::ArrayRef<char16_t> left,
    llvh::ArrayRef<char16_t> right) {
  for (size_t i = 0; i < left.size(); i++) {
    if (i >= right.size()) {
      return 1;
    }
    if (left[i] > right[i]) {
      return 1;
    } else if (left[i] < right[i]) {
      return -1;
    }
  }
  return left.size() < right.size() ? -1 : 0;
}

void dateFormat(
    double unixtimeMs,
    bool formatDate,
    bool formatTime,
    llvh::SmallVectorImpl<char16_t> &buf) {
  // FIXME: implement this.
  llvh::ArrayRef<char> str{"dateFormat not implemented"};
  buf.assign(str.begin(), str.end());
}

void convertToCase(
    llvh::SmallVectorImpl<char16_t> &buf,
    CaseConversion targetCase,
    bool useCurrentLocale) {
  // Unicode-Lite has no configured locale. Use Unicode default casing for
  // both callers; explicit language-sensitive casing still needs locale data.
  (void)useCurrentLocale;
  const bool upper = targetCase == CaseConversion::ToUpper;
  const llvh::ArrayRef<CaseMappingRange> mappings =
      upper ? llvh::ArrayRef(UPPER_MAPPINGS) : llvh::ArrayRef(LOWER_MAPPINGS);
  const llvh::ArrayRef<CaseMappingExpansion> expansions = upper
      ? llvh::ArrayRef(UPPER_EXPANSIONS)
      : llvh::ArrayRef(LOWER_EXPANSIONS);
  llvh::SmallVector<char16_t, 64> output;
  output.reserve(buf.size());
  bool precededByCasedLetter = false;
  for (size_t index = 0; index < buf.size();) {
    const uint32_t cp = nextCodePoint(buf, index);
    if (cp < 0x80) {
      const bool asciiUpper = 'A' <= cp && cp <= 'Z';
      const bool asciiLower = 'a' <= cp && cp <= 'z';
      if (asciiUpper || asciiLower) {
        precededByCasedLetter = true;
      } else if (!hasCaseProperty(CASE_IGNORABLE, cp)) {
        precededByCasedLetter = false;
      }
      output.push_back(static_cast<char16_t>(
          upper && asciiLower        ? cp - ('a' - 'A')
              : !upper && asciiUpper ? cp + ('a' - 'A')
                                     : cp));
      continue;
    }
    // Context is evaluated on the original string, including ignorable marks.
    // Only Final_Sigma is context-sensitive in locale-independent full casing.
    const bool finalSigma = !upper && cp == 0x03a3 && precededByCasedLetter &&
        !followedByCasedLetter(buf, index);
    if (!hasCaseProperty(CASE_IGNORABLE, cp)) {
      precededByCasedLetter = hasCaseProperty(CASED, cp);
    }
    if (finalSigma) {
      output.push_back(0x03c2);
      continue;
    }
    auto expansion = std::lower_bound(
        expansions.begin(),
        expansions.end(),
        cp,
        [](const CaseMappingExpansion &mapping, uint32_t cp) {
          return mapping.codePoint < cp;
        });
    if (expansion != expansions.end() && expansion->codePoint == cp) {
      for (uint32_t index = 0; index < expansion->length; ++index) {
        appendCodePoint(output, expansion->mapped[index]);
      }
      continue;
    }
    auto mapping = std::lower_bound(
        mappings.begin(),
        mappings.end(),
        cp,
        [](const CaseMappingRange &mapping, uint32_t cp) {
          return mapping.start + mapping.count <= cp;
        });
    uint32_t mapped = cp;
    if (mapping != mappings.end() && mapping->start <= cp &&
        (cp - mapping->start) % mapping->modulo == 0) {
      mapped = static_cast<uint32_t>(static_cast<int32_t>(cp) + mapping->delta);
    }
    appendCodePoint(output, mapped);
  }
  buf.assign(output.begin(), output.end());
}

void normalize(llvh::SmallVectorImpl<char16_t> &buf, NormalizationForm form) {
  // FIXME: implement this.
}

} // namespace platform_unicode
} // namespace hermes

#endif // HERMES_PLATFORM_UNICODE_LITE
