/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/StringPrimitive.h"
#include "hermes/VM/ScopedStringReuse.h"
#include "hermes/VM/StringView.h"
#include "hermes/VM/WeakRef.h"

#include "llvh/Support/AlignOf.h"

#include "VMRuntimeTestHelpers.h"

#include <climits>
#include <random>

#include "gtest/gtest.h"

using namespace hermes::vm;

namespace {

using StringPrimTest = RuntimeTestFixture;
using ScopedStringReuseTest = LargeHeapRuntimeTestFixture;

TEST_F(ScopedStringReuseTest, DoesNotRetainGrowingInputsOrResults) {
  for (unsigned position = 0; position < 3; ++position) {
    ScopedStringReuse scope(runtime);
    llvh::Optional<WeakRef<StringPrimitive>> weak;
    {
      GCScopeMarkerRAII marker{runtime};
      const std::u16string text(StringPrimitive::CONCAT_STRING_MIN_SIZE, u'\u0430');
      auto base = StringPrimitive::createNoThrow(runtime, createUTF16Ref(text.c_str()));
      auto suffix = StringPrimitive::createNoThrow(runtime, "x");
      auto joined = StringPrimitive::concat(runtime, base, suffix);
      ASSERT_NE(joined, ExecutionStatus::EXCEPTION);
      auto prefix = runtime.makeHandle<BufferedUTF16StringPrimitive>(*joined);
      weak.emplace(runtime, prefix);
      auto fixed = StringPrimitive::createNoThrow(runtime, "fixed");
      auto result = runtime.makeMutableHandle(fixed.getHermesValue());
      if (position == 1) {
        result = prefix.getHermesValue();
      } else if (position == 2) {
        auto created = JSArray::create(runtime, 1, 1);
        ASSERT_NE(created, ExecutionStatus::EXCEPTION);
        auto array = runtime.makeHandle(std::move(*created));
        JSArray::unsafeSetExistingElementAt(*array, runtime, 0,
            SmallHermesValue::encodeStringValue(*prefix, runtime));
        result = array.getHermesValue();
      }
      const PinnedHermesValue input(position == 0 ? prefix.getHermesValue()
                                                 : fixed.getHermesValue());
      scope.store(&position, {&input, 1}, result.getHermesValue());

      // A short immutable prefix shares storage with later concatenations.
      // Admitting it by length alone can retain arbitrarily larger storage.
      const std::u16string extension(65536, u'\u0430');
      auto tail = StringPrimitive::createNoThrow(runtime, createUTF16Ref(extension.c_str()));
      auto grown = StringPrimitive::concat(runtime, prefix, tail);
      ASSERT_NE(grown, ExecutionStatus::EXCEPTION);
      auto appended = runtime.makeHandle<BufferedUTF16StringPrimitive>(*grown);
      EXPECT_EQ(prefix->testGetConcatBuffer(), appended->testGetConcatBuffer());
      EXPECT_EQ(text.size() + 1, prefix->getStringLength());
      EXPECT_GT(appended->getStringLength(), 32768u);
    }
    runtime.collect("reuse must not retain concatenation storage");
    EXPECT_FALSE(weak->isValid()) << "retained position " << position;
    weak->releaseSlot();
  }
}

TEST_F(ScopedStringReuseTest, UTF8DoesNotRetainGrowingInputs) {
  ScopedStringReuse scope(runtime);
  llvh::Optional<WeakRef<StringPrimitive>> weak;
  {
    GCScopeMarkerRAII marker{runtime};
    const std::u16string text(StringPrimitive::CONCAT_STRING_MIN_SIZE, u'\u0430');
    auto base = StringPrimitive::createNoThrow(runtime, createUTF16Ref(text.c_str()));
    auto suffix = StringPrimitive::createNoThrow(runtime, "x");
    auto joined = StringPrimitive::concat(runtime, base, suffix);
    ASSERT_NE(joined, ExecutionStatus::EXCEPTION);
    auto prefix = runtime.makeHandle<BufferedUTF16StringPrimitive>(*joined);
    weak.emplace(runtime, prefix);
    // Mirror the API's admission boundary without manufacturing a cache store
    // that a correctly rejecting caller would never make.
    if (scope.allowsUTF8Reuse(*prefix)) {
      EXPECT_EQ(nullptr, scope.lookupUTF8(*prefix));
      std::string bytes;
      for (size_t i = 0; i < text.size(); ++i)
        bytes += "\xd0\xb0";
      bytes += 'x';
      scope.storeUTF8(*prefix, bytes);
    }
    const std::u16string extension(65536, u'\u0430');
    auto tail = StringPrimitive::createNoThrow(runtime, createUTF16Ref(extension.c_str()));
    auto grown = StringPrimitive::concat(runtime, prefix, tail);
    ASSERT_NE(grown, ExecutionStatus::EXCEPTION);
    auto appended = runtime.makeHandle<BufferedUTF16StringPrimitive>(*grown);
    EXPECT_EQ(prefix->testGetConcatBuffer(), appended->testGetConcatBuffer());
  }
  runtime.collect("UTF-8 reuse must not retain concatenation storage");
  EXPECT_FALSE(weak->isValid());
  weak->releaseSlot();
}

TEST_F(ScopedStringReuseTest, ExternalCapacityCountsTowardRetention) {
  ScopedStringReuse scope(runtime);
  std::u16string text(StringPrimitive::CONCAT_STRING_MIN_SIZE, u'\u0430');
  auto fixed = StringPrimitive::createNoThrow(runtime, createUTF16Ref(text.c_str()));
  text.reserve(65536);
  auto created = StringPrimitive::createEfficient(runtime, std::move(text));
  ASSERT_NE(created, ExecutionStatus::EXCEPTION);
  auto external = runtime.makeHandle<StringPrimitive>(*created);
  ASSERT_TRUE(external->isExternal());
  ASSERT_FALSE(isBufferedStringPrimitive(*external));
  ASSERT_GT(external->getVT()->getMallocSize(*external), 65536u);
  ASSERT_TRUE(external->equals(*fixed));
  EXPECT_FALSE(scope.allowsUTF8Reuse(*external));
  EXPECT_TRUE(scope.allowsUTF8Reuse(*fixed));
  const PinnedHermesValue input(external.getHermesValue());
  scope.store(this, {&input, 1}, fixed.getHermesValue());
  EXPECT_TRUE(scope.lookup(this, {&input, 1}).isUndefined());
  const PinnedHermesValue bounded(fixed.getHermesValue());
  scope.store(this, {&bounded, 1}, external.getHermesValue());
  EXPECT_TRUE(scope.lookup(this, {&bounded, 1}).isUndefined());
  auto arrayResult = JSArray::create(runtime, 1, 1);
  ASSERT_NE(arrayResult, ExecutionStatus::EXCEPTION);
  auto array = runtime.makeHandle(std::move(*arrayResult));
  JSArray::unsafeSetExistingElementAt(*array, runtime, 0,
      SmallHermesValue::encodeStringValue(*external, runtime));
  scope.store(this, {&bounded, 1}, array.getHermesValue());
  EXPECT_TRUE(scope.lookup(this, {&bounded, 1}).isUndefined());
  scope.store(this, {&bounded, 1}, fixed.getHermesValue());
  runtime.collect("bounded reuse remains live");
  const PinnedHermesValue moved(fixed.getHermesValue());
  EXPECT_TRUE(scope.lookup(this, {&moved, 1}).getString()->equals(*fixed));
}

TEST_F(ScopedStringReuseTest, BufferedLookupDoesNotRetainInput) {
  ScopedStringReuse scope(runtime);
  std::u16string text(StringPrimitive::CONCAT_STRING_MIN_SIZE, u'\u0430');
  text += u'x';
  auto fixed = StringPrimitive::createNoThrow(runtime, createUTF16Ref(text.c_str()));
  auto result = StringPrimitive::createNoThrow(runtime, "result");
  const PinnedHermesValue key(fixed.getHermesValue());
  scope.store(this, {&key, 1}, result.getHermesValue());
  llvh::Optional<WeakRef<StringPrimitive>> weak;
  {
    GCScopeMarkerRAII marker{runtime};
    text.pop_back();
    auto base = StringPrimitive::createNoThrow(runtime, createUTF16Ref(text.c_str()));
    auto suffix = StringPrimitive::createNoThrow(runtime, "x");
    auto joined = StringPrimitive::concat(runtime, base, suffix);
    ASSERT_NE(joined, ExecutionStatus::EXCEPTION);
    auto prefix = runtime.makeHandle<BufferedUTF16StringPrimitive>(*joined);
    weak.emplace(runtime, prefix);
    const PinnedHermesValue input(prefix.getHermesValue());
    // Equality lookup may use a safely retained key without retaining the new
    // input, even when that input would not be admitted as a cache key.
    auto found = scope.lookup(this, {&input, 1});
    ASSERT_TRUE(found.isString());
    EXPECT_TRUE(found.getString()->equals(*result));
    const std::u16string extension(65536, u'\u0430');
    auto tail = StringPrimitive::createNoThrow(runtime, createUTF16Ref(extension.c_str()));
    auto grown = StringPrimitive::concat(runtime, prefix, tail);
    ASSERT_NE(grown, ExecutionStatus::EXCEPTION);
  }
  runtime.collect("lookup must not retain a new input");
  EXPECT_FALSE(weak->isValid());
  weak->releaseSlot();
  const PinnedHermesValue moved(fixed.getHermesValue());
  EXPECT_TRUE(scope.lookup(this, {&moved, 1}).getString()->equals(*result));
}

TEST_F(ScopedStringReuseTest, CombinedExternalStorageIsBounded) {
  ScopedStringReuse scope(runtime);
  const std::u16string text(128, u'\u0430');
  auto fixed = StringPrimitive::createNoThrow(runtime, createUTF16Ref(text.c_str()));
  auto reserved = text;
  reserved.reserve(20000);
  auto created = StringPrimitive::createEfficient(runtime, std::move(reserved));
  ASSERT_NE(created, ExecutionStatus::EXCEPTION);
  auto external = runtime.makeHandle<StringPrimitive>(*created);
  ASSERT_TRUE(external->isExternal());
  const auto storage = external->getVT()->getMallocSize(*external);
  ASSERT_GT(storage, ScopedStringReuse::maxEntryStorageBytes / 2);
  ASSERT_LE(storage, ScopedStringReuse::maxEntryStorageBytes - 2 * text.size());
  const PinnedHermesValue input(external.getHermesValue());
  scope.store(this, {&input, 1}, external.getHermesValue());
  EXPECT_TRUE(scope.lookup(this, {&input, 1}).isUndefined());
  scope.store(this, {&input, 1}, fixed.getHermesValue());
  auto found = scope.lookup(this, {&input, 1});
  ASSERT_TRUE(found.isString());
  EXPECT_TRUE(found.getString()->equals(*fixed));
}

TEST_F(StringPrimTest, CreateTest) {
  auto str1 = StringPrimitive::createNoThrow(runtime, "hello");
  auto str2 = StringPrimitive::createNoThrow(runtime, "foo");

  EXPECT_EQ(5u, str1->getStringLength());
  EXPECT_TRUE(
      StringPrimitive::createStringView(runtime, str1)
          .equals(createUTF16Ref(u"hello")));

  EXPECT_EQ(3u, str2->getStringLength());
  EXPECT_TRUE(
      StringPrimitive::createStringView(runtime, str2)
          .equals(createUTF16Ref(u"foo")));
}

TEST_F(StringPrimTest, EqualityTest) {
  auto s1 = StringPrimitive::createNoThrow(runtime, createUTF16Ref(u"abc"));
  auto s2 = StringPrimitive::createNoThrow(runtime, createUTF16Ref(u"abc"));
  auto s3 = StringPrimitive::createNoThrow(runtime, createUTF16Ref(u"abcdef"));
  EXPECT_TRUE(s1->equals(s2.get()));
  EXPECT_FALSE(s1->equals(s3.get()));
}

TEST_F(StringPrimTest, CreateASCIITest) {
  auto s1 = StringPrimitive::createNoThrow(runtime, createUTF16Ref(u"hello"));
  auto s2 = StringPrimitive::createNoThrow(runtime, "hello");
  EXPECT_TRUE(s1->equals(s2.get()));
}

TEST_F(StringPrimTest, CreateEfficientTest) {
  auto handlefy = [&](CallResult<HermesValue> cr) {
    return Handle<StringPrimitive>::vmcast(runtime, *cr);
  };

  static_assert(
      StringPrimitive::EXTERNAL_STRING_MIN_SIZE > 3,
      "Test strings should be shorter than EXTERNAL_STRING_MIN_SIZE");
  std::string narrowShort = "foo";
  std::u16string wideShort(u"foo");
  auto s1 = handlefy(
      StringPrimitive::createEfficient(
          runtime, createASCIIRef(narrowShort.c_str())));
  auto s2 = handlefy(
      StringPrimitive::createEfficient(
          runtime, createUTF16Ref(wideShort.c_str())));
  auto s3 = handlefy(
      StringPrimitive::createEfficient(runtime, std::move(narrowShort)));
  auto s4 =
      handlefy(StringPrimitive::createEfficient(runtime, std::move(wideShort)));
  for (auto s : {s1, s2, s3, s4}) {
    EXPECT_TRUE(s->equals(s1.get()));
    EXPECT_EQ(3, s->getStringLength());
    EXPECT_FALSE(s->isExternal());
  }

  size_t longLength = StringPrimitive::EXTERNAL_STRING_MIN_SIZE;
  std::string narrowLong(longLength, '!');
  std::u16string wideLong(longLength, u'!');
  auto e1 = handlefy(
      StringPrimitive::createEfficient(
          runtime, createASCIIRef(narrowLong.c_str())));
  auto e2 = handlefy(
      StringPrimitive::createEfficient(
          runtime, createUTF16Ref(wideLong.c_str())));
  auto e3 = handlefy(
      StringPrimitive::createEfficient(runtime, std::move(narrowLong)));
  auto e4 =
      handlefy(StringPrimitive::createEfficient(runtime, std::move(wideLong)));
  for (auto s : {e1, e2, e3, e4}) {
    EXPECT_TRUE(s->equals(e1.get()));
    EXPECT_EQ(longLength, s->getStringLength());
    if (s == e3 || s == e4) {
      EXPECT_TRUE(s->isExternal());
    } else {
      EXPECT_FALSE(s->isExternal());
    }
  }
}

TEST_F(StringPrimTest, CompareTest) {
#define TEST_CMP(v, a, b)                                 \
  {                                                       \
    auto s1 = StringPrimitive::createNoThrow(runtime, a); \
    auto s2 = StringPrimitive::createNoThrow(runtime, b); \
    EXPECT_EQ(v, s1->compare(s2.get()));                  \
  }

  TEST_CMP(0, "", "");
  TEST_CMP(-1, "", "a");
  TEST_CMP(+1, "a", "");

  TEST_CMP(0, "123", "123");
  TEST_CMP(-1, "12", "123");
  TEST_CMP(+1, "123", "12");

  TEST_CMP(-1, "123", "4");
  TEST_CMP(+1, "4", "123");

  TEST_CMP(-1, "123", "456");
  TEST_CMP(+1, "456", "123");

#undef TEST_CMP
}

TEST_F(StringPrimTest, ConcatTest) {
  {
    auto a = StringPrimitive::createNoThrow(runtime, createUTF16Ref(u"abc"));
    auto b = StringPrimitive::createNoThrow(runtime, createUTF16Ref(u"def"));
    auto strRes = StringPrimitive::concat(runtime, a, b);
    ASSERT_NE(ExecutionStatus::EXCEPTION, strRes.getStatus());
    EXPECT_TRUE(
        StringPrimitive::createStringView(
            runtime, runtime.makeHandle<StringPrimitive>(*strRes))
            .equals(createUTF16Ref(u"abcdef")));
  }

  {
    auto a = StringPrimitive::createNoThrow(runtime, createUTF16Ref(u""));
    auto b = StringPrimitive::createNoThrow(runtime, createUTF16Ref(u"abc"));
    auto strRes = StringPrimitive::concat(runtime, a, b);
    ASSERT_NE(ExecutionStatus::EXCEPTION, strRes.getStatus());
    EXPECT_TRUE(
        StringPrimitive::createStringView(
            runtime, runtime.makeHandle<StringPrimitive>(*strRes))
            .equals(createUTF16Ref(u"abc")));
  }
}

// This attempts to test that strings above a sufficient length may be freely
// memcpy'd around. This would not be true if the small-string optimization used
// an interior pointer, or if someone else maintained a pointer to the string.
template <typename StringType>
void test1StringMemcpySafety(const StringType &s) {
  // Grub through the string's bits, looking for pointers to the interior of the
  // string.
  intptr_t start = reinterpret_cast<intptr_t>(&s);
  intptr_t end = reinterpret_cast<intptr_t>(&s + 1);
  const intptr_t *stringGuts = reinterpret_cast<const intptr_t *>(&s);
  for (size_t i = 0; i < sizeof(StringType) / sizeof(void *); i++) {
    intptr_t p = stringGuts[i];
    EXPECT_FALSE(start <= p && p <= end) << "string size: " << s.size();
    // Mask off low bits in case it uses them as flags.
    p &= ~(intptr_t)1;
    EXPECT_FALSE(start <= p && p <= end) << "string size: " << s.size();
    p &= ~(intptr_t)2;
    EXPECT_FALSE(start <= p && p <= end) << "string size: " << s.size();
  }

  // memcpy our string and verify the buffer, when interpreted as a string, is
  // equal to the original.
  llvh::AlignedCharArrayUnion<StringType> mb;
  memcpy(mb.buffer, &s, sizeof(StringType));
  const StringType *memcpydStr =
      reinterpret_cast<const StringType *>(mb.buffer);
  EXPECT_EQ(s, *memcpydStr) << "string size: " << s.size();
  EXPECT_EQ(s.size(), memcpydStr->size()) << "string size: " << s.size();
  EXPECT_TRUE(std::equal(s.begin(), s.end(), memcpydStr->begin()))
      << "string size: " << s.size();

  // std::move a string. If any bits changed, it means the string is not memcpy
  // safe, because the move constructor does something nontrivial. Note it is
  // not the case in general that a moved-to string will be bitwise identical to
  // its source; the string may have an internal buffer whose last bytes are
  // unused when the string is externally allocated, and these last bytes may
  // not be copied by the move ctor. That is, the move ctor may be conceptually
  // a memcpy of only a prefix of the string. To avoid this, construct the
  // strings in buffers that are initially zeroed out. This isn't guaranteed to
  // always work because the constructors and the new operator may still modify
  // those bytes, however, it seems to work in practice.
  auto s1 = new (calloc(1, sizeof(StringType))) StringType(s);
  llvh::AlignedCharArrayUnion<StringType> savedBits;
  memcpy(savedBits.buffer, s1, sizeof(StringType));

  auto movedString =
      new (calloc(1, sizeof(StringType))) StringType(std::move(*s1));
  // We expect the move-constructed string to be bitwise identical to the string
  // before it was moved from. That is, the move ctor should be a memcpy.
  EXPECT_EQ(0, memcmp(movedString, savedBits.buffer, sizeof(StringType)))
      << "string size: " << s.size();
  // Sanity check: the moved-from string should have changed, e.g. zeroed
  // pointer.
  EXPECT_NE(0, memcmp(s1, savedBits.buffer, sizeof(StringType)))
      << "string size: " << s.size();
  movedString->~StringType();
  free(movedString);
  s1->~StringType();
  free(s1);
}

TEST_F(StringPrimTest, StringsAreMemcpySafe) {
  for (size_t length = StringPrimitive::EXTERNAL_STRING_MIN_SIZE; length < 2048;
       length++) {
    test1StringMemcpySafety(std::string(length, '!'));
    test1StringMemcpySafety(std::u16string(length, u'!'));
  }
}

/// Containers (including strings) are not memcpy-safe in Microsoft STL in debug
/// mode, because they contain a pointer to a "container proxy", which in turn
/// points back to the container. memcpy() of the container leaves the proxy
/// pointing to garbage.
/// We statically detect and work around this in our CopyableBasicString. The
/// purpose of this test is to crash if we detected wrong.
TEST_F(StringPrimTest, StringsAreMemcpySafeMicrosoftSTL) {
  using Str = CopyableBasicString<char>;
  Str *orig = new (::malloc(sizeof(Str))) Str(100, 'a');
  Str *copy = (Str *)::malloc(sizeof(Str));
  ::memcpy((void *)copy, orig, sizeof(Str));
  ::memset((void *)orig, 0xAA, sizeof(Str));
  ::free(orig);

  ASSERT_EQ(100, copy->size());
  // This should crash.
  copy->append(&copy->data()[0], &copy->data()[50]);

  copy->~Str();
  ::free(copy);
}

TEST_F(StringPrimTest, FastConcatTest) {
  CallResult<HermesValue> cr{ExecutionStatus::EXCEPTION};
  std::string bigStrA(300, 'a');
  std::string strB("small");

  //=======================================
  // ASCII + ASCII
  auto a = StringPrimitive::createNoThrow(runtime, bigStrA);
  auto b = StringPrimitive::createNoThrow(runtime, strB);
  cr = StringPrimitive::concat(runtime, a, b);
  ASSERT_NE(ExecutionStatus::EXCEPTION, cr);
  auto resASCII_1 = runtime.makeHandle<BufferedASCIIStringPrimitive>(*cr);

  std::string asciiStr1 = bigStrA + strB;
  auto asciiRef = resASCII_1->getStringRef<char>();
  EXPECT_TRUE(asciiRef.size() == asciiStr1.size());
  EXPECT_TRUE(std::equal(asciiStr1.begin(), asciiStr1.end(), asciiRef.begin()));

  //=======================================
  // Add more ASCII
  cr = StringPrimitive::concat(runtime, resASCII_1, b);
  ASSERT_NE(ExecutionStatus::EXCEPTION, cr);
  auto resASCII_2 = runtime.makeHandle<BufferedASCIIStringPrimitive>(*cr);

  // The same buffer must have been reused.
  EXPECT_TRUE(
      resASCII_1->testGetConcatBuffer() == resASCII_2->testGetConcatBuffer());

  std::string asciiStr2 = asciiStr1 + strB;
  asciiRef = resASCII_2->getStringRef<char>();
  EXPECT_TRUE(asciiRef.size() == asciiStr2.size());
  EXPECT_TRUE(std::equal(asciiStr2.begin(), asciiStr2.end(), asciiRef.begin()));

  //=======================================
  // Append some the first result again.
  cr = StringPrimitive::concat(runtime, resASCII_1, b);
  ASSERT_NE(ExecutionStatus::EXCEPTION, cr);
  auto resASCII_3 = runtime.makeHandle<BufferedASCIIStringPrimitive>(*cr);

  // The buffer cannot be reused.
  EXPECT_TRUE(
      resASCII_3->testGetConcatBuffer() != resASCII_1->testGetConcatBuffer());

  std::string asciiStr3 = asciiStr1 + strB;
  asciiRef = resASCII_3->getStringRef<char>();
  EXPECT_TRUE(asciiRef.size() == asciiStr3.size());
  EXPECT_TRUE(std::equal(asciiStr3.begin(), asciiStr3.end(), asciiRef.begin()));

  //=======================================
  // ASCII + UTF16
  std::u16string strC(u"utf16\u1234");
  b = StringPrimitive::createNoThrow(
      runtime, UTF16Ref(strC.data(), strC.size()));
  cr = StringPrimitive::concat(runtime, resASCII_2, b);
  ASSERT_NE(ExecutionStatus::EXCEPTION, cr);
  auto resUTF_1 = runtime.makeHandle<BufferedUTF16StringPrimitive>(*cr);

  std::u16string utfStr1;
  utfStr1.append(asciiStr2.begin(), asciiStr2.end());
  utfStr1.append(strC);
  auto utf16Ref = resUTF_1->getStringRef<char16_t>();
  EXPECT_TRUE(utf16Ref.size() == utfStr1.size());
  EXPECT_TRUE(std::equal(utfStr1.begin(), utfStr1.end(), utf16Ref.begin()));

  //=======================================
  // UTF16 + ASCII
  cr = StringPrimitive::concat(runtime, resUTF_1, b);
  ASSERT_NE(ExecutionStatus::EXCEPTION, cr);
  auto resUTF_2 = runtime.makeHandle<BufferedUTF16StringPrimitive>(*cr);

  // The same buffer must have been reused.
  EXPECT_TRUE(
      resUTF_1->testGetConcatBuffer() == resUTF_2->testGetConcatBuffer());

  std::u16string utfStr2 = utfStr1 + strC;
  utf16Ref = resUTF_2->getStringRef<char16_t>();
  EXPECT_TRUE(utf16Ref.size() == utfStr2.size());
  EXPECT_TRUE(std::equal(utfStr2.begin(), utfStr2.end(), utf16Ref.begin()));

  //=======================================
  // Add some more UTF16 to resUTF_1
  cr = StringPrimitive::concat(runtime, resUTF_1, b);
  ASSERT_NE(ExecutionStatus::EXCEPTION, cr);
  auto resUTF_3 = runtime.makeHandle<BufferedUTF16StringPrimitive>(*cr);

  // The buffer cannot be reused.
  EXPECT_TRUE(
      resUTF_1->testGetConcatBuffer() != resUTF_3->testGetConcatBuffer());

  std::u16string utfStr3 = utfStr1 + strC;
  utf16Ref = resUTF_3->getStringRef<char16_t>();
  EXPECT_TRUE(utf16Ref.size() == utfStr3.size());
  EXPECT_TRUE(std::equal(utfStr3.begin(), utfStr3.end(), utf16Ref.begin()));
}
} // namespace
