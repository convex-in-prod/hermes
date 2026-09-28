/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "VMRuntimeTestHelpers.h"

#include "hermes/VM/PropertyCache.h"
#include "hermes/VM/StaticHUtils.h"
#include "hermes/VM/static_h.h"

#include "gtest/gtest.h"

#include <array>
#include <cstdlib>
#include <utility>

using namespace hermes::vm;

namespace {

uint32_t unitMainCallCount;

SHLegacyValue unitMain(SHRuntime *) {
  ++unitMainCallCount;
  return _sh_ljs_undefined();
}

const SHNativeFuncInfo unitMainInfo{};
const uint32_t unitStrings[]{0, 0, 0};

struct TestUnitData {
  SHUnit unit;
  SHSymbolID symbols[1];
};

template <size_t UnitNumber>
SHUnit *createUnit() {
  static uint32_t index;
  auto *data =
      static_cast<TestUnitData *>(std::calloc(1, sizeof(TestUnitData)));
  if (!data) {
    std::abort();
  }

  data->unit.index = &index;
  data->unit.num_symbols = 1;
  data->unit.ascii_pool = "";
  data->unit.strings = unitStrings;
  data->unit.symbols = data->symbols;
  data->unit.unit_main = unitMain;
  data->unit.unit_main_info = &unitMainInfo;
  data->unit.unit_name = "test";
  return &data->unit;
}

template <size_t... UnitNumbers>
constexpr auto createUnitFactories(std::index_sequence<UnitNumbers...>) {
  return std::array<SHUnitCreator, sizeof...(UnitNumbers)>{
      createUnit<UnitNumbers>...};
}

TEST(StaticHUnitTest, InitializesAllUsableUnitSlots) {
  constexpr auto creators = createUnitFactories(
      std::make_index_sequence<SH_UNIT_REGISTRY_CAPACITY>{});

  unitMainCallCount = 0;
  auto runtime = Runtime::create(kTestRTConfig);
  SHRuntime *shRuntime = getSHRuntime(*runtime);
  for (size_t i = 0; i < SH_UNIT_REGISTRY_CAPACITY - 1; ++i) {
    SHLegacyValue result;
    ASSERT_TRUE(_sh_unit_init_guarded(shRuntime, creators[i], &result));
    ASSERT_NE(nullptr, shRuntime->units[i + 1]);
    EXPECT_EQ(i + 1, *shRuntime->units[i + 1]->index);
  }
  EXPECT_EQ(SH_UNIT_REGISTRY_CAPACITY - 1, unitMainCallCount);

  // Runtime owns a concurrent GC thread, so re-execute the test binary in the
  // death test child instead of continuing directly after fork.
  const auto previousDeathTestStyle = ::testing::FLAGS_gtest_death_test_style;
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_DEATH_IF_SUPPORTED(
      {
        SHLegacyValue result;
        _sh_unit_init_guarded(
            shRuntime, creators[SH_UNIT_REGISTRY_CAPACITY - 1], &result);
      },
      "Too many SH units registered");
  ::testing::FLAGS_gtest_death_test_style = previousDeathTestStyle;
}

TEST(StaticHUnitTest, AcceptsMatchingStaticABI) {
  _sh_check_abi(
      &_sh_static_abi_descriptor, sizeof(_sh_static_abi_descriptor));
}

TEST(StaticHUnitTest, RejectsMismatchedStaticABI) {
  SHStaticABIDescriptor mismatched = _sh_static_abi_descriptor;
  ++mismatched.runtime_units_capacity;
  EXPECT_DEATH_IF_SUPPORTED(
      _sh_check_abi(&mismatched, sizeof(mismatched)),
      "Static Hermes ABI mismatch: generated.*runtime archive");
}

using StaticHCacheTest = RuntimeTestFixture;

TEST_F(StaticHCacheTest, RetainsTwoShapesForNamedAndComputedReads) {
  auto first = runtime.makeHandle(JSObject::create(runtime));
  auto second = runtime.makeHandle(JSObject::create(runtime));
  const auto key = Predefined::getSymbolID(Predefined::value);
  const auto otherKey = Predefined::getSymbolID(Predefined::length);
  ASSERT_TRUE(*JSObject::putNamed_RJS(
      first, runtime, key, runtime.makeHandle(1.0_hd)));
  ASSERT_TRUE(*JSObject::putNamed_RJS(
      second, runtime, otherKey, runtime.makeHandle(2.0_hd)));
  ASSERT_TRUE(*JSObject::putNamed_RJS(
      second, runtime, key, runtime.makeHandle(3.0_hd)));
  ASSERT_NE(first->getClass(runtime), second->getClass(runtime));

  SHReadPropertyCacheEntry named[SH_NAMED_READ_CACHE_WAYS]{};
  SHComputedReadCacheEntry computed[SH_COMPUTED_READ_CACHE_WAYS]{};
  SHLegacyValue keyValue = HermesValue::encodeStringValue(
      runtime.getStringPrimFromSymbolID(key));
  SHLegacyValue sources[]{first.getHermesValue(), second.getHermesValue()};
  auto *shr = getSHRuntime(runtime);
  // No allocation or user code is possible for these own data reads. JS tests
  // separately exercise traced unit caches across collections and invalidation.
  for (unsigned round = 0; round < 10; ++round) {
    for (unsigned i = 0; i < 2; ++i) {
      EXPECT_EQ(
          1.0 + 2 * i,
          _sh_ljs_get_by_id_polymorphic_rjs(
              shr, &sources[i], key.unsafeGetRaw(), named).f64);
      EXPECT_EQ(
          1.0 + 2 * i,
          _sh_ljs_get_by_val_cached_rjs(
              shr, &sources[i], &keyValue, computed).f64);
    }
  }
  // Warm alternating receivers must not refill the named cache or overwrite
  // the other receiver's computed entry.
  EXPECT_EQ(2, named[0].numChanges + named[1].numChanges);
  auto *computedEntries = reinterpret_cast<ComputedReadCacheEntry *>(computed);
  unsigned retained = 0;
  for (unsigned i = 0; i < SH_COMPUTED_READ_CACHE_WAYS; ++i) {
    if (computedEntries[i].key == key &&
        (computedEntries[i].clazz == first->getClassGCPtr() ||
         computedEntries[i].clazz == second->getClassGCPtr()))
      ++retained;
  }
  EXPECT_EQ(2u, retained);
}

} // namespace
