/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/Platform/Unicode/PlatformUnicode.h"

#include "gtest/gtest.h"

#include <string>

namespace {

using namespace hermes::platform_unicode;

#if HERMES_PLATFORM_UNICODE == HERMES_PLATFORM_UNICODE_LITE
TEST(PlatformUnicode, LiteAsciiCaseConversion) {
  llvh::SmallVector<char16_t, 16> lower = {u'I', u'V', u'A', u'N', u' ', u'X'};
  convertToCase(lower, CaseConversion::ToLower, true /* useCurrentLocale */);
  ASSERT_EQ(6, lower.size());
  EXPECT_EQ(u'i', lower[0]);
  EXPECT_EQ(u'v', lower[1]);
  EXPECT_EQ(u'a', lower[2]);
  EXPECT_EQ(u'n', lower[3]);
  EXPECT_EQ(u' ', lower[4]);
  EXPECT_EQ(u'x', lower[5]);

  llvh::SmallVector<char16_t, 16> upper = {u'i', u'v', u'a', u'n', u' ', u'x'};
  convertToCase(upper, CaseConversion::ToUpper, true /* useCurrentLocale */);
  ASSERT_EQ(6, upper.size());
  EXPECT_EQ(u'I', upper[0]);
  EXPECT_EQ(u'V', upper[1]);
  EXPECT_EQ(u'A', upper[2]);
  EXPECT_EQ(u'N', upper[3]);
  EXPECT_EQ(u' ', upper[4]);
  EXPECT_EQ(u'X', upper[5]);
}
#endif
TEST(PlatformUnicode, CaseTest) {
  llvh::SmallVector<char16_t, 16> str = {u'a', u'B', u'c', u'\u00df'};
  convertToCase(str, CaseConversion::ToUpper, false /* useCurrentLocale */);
  ASSERT_EQ(5, str.size());
  EXPECT_EQ(u'A', str[0]);
  EXPECT_EQ(u'B', str[1]);
  EXPECT_EQ(u'C', str[2]);
  EXPECT_EQ(u'S', str[3]);
  EXPECT_EQ(u'S', str[4]);
}

TEST(PlatformUnicode, VersionCheck) {
  // Make sure we have up-to-date Unicode data, including case-ignorable marks.
  llvh::SmallVector<char16_t, 16> str = {u'A', u'\u180e', u'\u03a3'};
  convertToCase(str, CaseConversion::ToLower, false /* useCurrentLocale */);
  ASSERT_EQ(3, str.size());
  EXPECT_EQ(u'a', str[0]);
  EXPECT_EQ(u'\u180e', str[1]);
  EXPECT_EQ(u'\u03c2', str[2]);
}

TEST(PlatformUnicode, DefaultCaseMapping) {
  const struct {
    const char16_t *input;
    const char16_t *lower;
    const char16_t *upper;
  } cases[] = {
      {u"\u0418\u0412\u0410\u041d",
       u"\u0438\u0432\u0430\u043d",
       u"\u0418\u0412\u0410\u041d"},
      {u"\u0401\u0416", u"\u0451\u0436", u"\u0401\u0416"},
      {u"Stra\u00dfe", u"stra\u00dfe", u"STRASSE"},
      {u"\u0130", u"i\u0307", u"\u0130"},
      {u"\ufb03", u"\ufb03", u"FFI"},
      {u"\u039f\u03a3", u"\u03bf\u03c2", u"\u039f\u03a3"},
      {u"\u03a3", u"\u03c3", u"\u03a3"},
      {u"A\u0301\u03a3\u0301", u"a\u0301\u03c2\u0301", u"A\u0301\u03a3\u0301"},
      {u"A\u03a3\u0301B", u"a\u03c3\u0301b", u"A\u03a3\u0301B"},
      {u"A \u03a3", u"a \u03c3", u"A \u03a3"},
      {u"A'.:^`\u03a3", u"a'.:^`\u03c2", u"A'.:^`\u03a3"},
      {u"A\u03a3'.:^`B", u"a\u03c3'.:^`b", u"A\u03a3'.:^`B"},
      {u"\u0345\u03a3", u"\u0345\u03c3", u"\u0399\u03a3"},
      {u"A\u03a3\u0345", u"a\u03c2\u0345", u"A\u03a3\u0399"},
      {u"\U00010400\U00010428",
       u"\U00010428\U00010428",
       u"\U00010400\U00010400"},
      {u"\xD800"
       u"a\xDC00",
       u"\xD800"
       u"a\xDC00",
       u"\xD800"
       u"A\xDC00"},
      {u"", u"", u""},
  };
  for (const auto &test : cases) {
    const std::u16string input(test.input);
    for (const auto conversion :
         {CaseConversion::ToLower, CaseConversion::ToUpper}) {
      llvh::SmallVector<char16_t, 16> value(input.begin(), input.end());
      convertToCase(value, conversion, false /* useCurrentLocale */);
      EXPECT_EQ(
          std::u16string(value.begin(), value.end()),
          conversion == CaseConversion::ToLower ? test.lower : test.upper);
    }
  }
  // Expansions must also work when the output exceeds the inline buffer.
  llvh::SmallVector<char16_t, 16> expanding(256, u'\u00df');
  convertToCase(expanding, CaseConversion::ToUpper, false);
  EXPECT_EQ(
      std::u16string(expanding.begin(), expanding.end()),
      std::u16string(512, u'S'));
}

#if HERMES_PLATFORM_UNICODE != HERMES_PLATFORM_UNICODE_LITE
TEST(PlatformUnicode, Normalize) {
  llvh::SmallVector<char16_t, 16> str = {u'\u1e9b', u'\u0323'};
  normalize(str, NormalizationForm::D);
  ASSERT_EQ(3, str.size());
  EXPECT_EQ(u'\u017f', str[0]);
  EXPECT_EQ(u'\u0323', str[1]);
  EXPECT_EQ(u'\u0307', str[2]);
}
#endif

} // namespace
