/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/static_h.h"
#include "hermes/hermes.h"
#include "jsi/instrumentation.h"

#include <stdexcept>

extern "C" SHUnit *sh_export_scoped_normalization(void);
extern "C" SHUnit *sh_export_scoped_scan(void);

extern "C" int scoped_normalization_run() {
  char program[] = "scoped-normalization";
  char testMethods[] = "-Xhermes-internal-test-methods";
  char *arguments[] = {program, testMethods};
  SHRuntime *runtime = _sh_init(2, arguments);
  if (!runtime)
    return 1;
  auto &js = *reinterpret_cast<facebook::jsi::Runtime *>(
      _sh_get_hermes_runtime(runtime));
  auto &hermes = static_cast<facebook::hermes::HermesRuntime &>(js);
  auto require = [](bool condition) {
    if (!condition)
      throw std::runtime_error("Scoped normalization contract failed");
  };
  require(_sh_ljs_has_array_join(runtime));
  require(_sh_initialize_units(runtime, 1, sh_export_scoped_scan));
  {
    auto scan = js.global().getPropertyAsFunction(js, "scanOwned");
    auto outer = hermes.createStringReuseScope();
    auto first = scan.call(js, "a:b").getObject(js).getArray(js);
    require(first.size(js) == 2);
    require(outer->statistics().misses == 1 && outer->statistics().stores == 1);
    first.setValueAtIndex(js, 0, "changed");
    js.instrumentation().collectGarbage("scan snapshot");
    auto second = scan.call(js, "a:b").getObject(js).getArray(js);
    require(outer->statistics().hits == 1);
    require(!facebook::jsi::Object::strictEquals(js, first, second));
    require(second.getValueAtIndex(js, 0).getString(js).utf8(js) == "a");
    {
      auto inner = hermes.createStringReuseScope();
      scan.call(js, "a:b");
      require(inner->statistics().misses == 1 && inner->statistics().hits == 0);
      js.instrumentation().collectGarbage("nested scan snapshot");
    }
    scan.call(js, "a:b");
    require(outer->statistics().hits == 2);
    auto beforeNested = outer->statistics();
    auto nested = scan.call(js, "a:*").getObject(js).getArray(js);
    auto nestedAgain = scan.call(js, "a:*").getObject(js).getArray(js);
    require(outer->statistics().stores == beforeNested.stores);
    require(outer->statistics().misses == beforeNested.misses + 2);
    require(!facebook::jsi::Object::strictEquals(js,
        nested.getValueAtIndex(js, 1).getObject(js),
        nestedAgain.getValueAtIndex(js, 1).getObject(js)));
    std::string many = "a";
    for (unsigned i = 1; i < 129; ++i) many += ":a";
    auto beforeLarge = outer->statistics();
    require(scan.call(js, many).getObject(js).getArray(js).size(js) == 129);
    require(scan.call(js, many).getObject(js).getArray(js).size(js) == 129);
    require(scan.call(js, std::string(40000, 'x'))
                .getObject(js).getArray(js).size(js) == 1);
    require(outer->statistics().stores == beforeLarge.stores);
    auto negative = js.global().getPropertyAsFunction(js, "scanNegative");
    negative.call(js, "a");
    negative.call(js, "a");
    require(outer->statistics().hits == beforeLarge.hits + 1);
    require(js.global().getPropertyAsFunction(js, "runScanMutationTests")
                .call(js).getBool());
  }
  {
    auto scope = hermes.createStringReuseScope();
    require(_sh_initialize_units(runtime, 1, sh_export_scoped_normalization));
    require(scope->statistics().hits > 0);
  }
  {
    auto first = js.global().getPropertyAsFunction(js, "scopeProbeFirst");
    auto second = js.global().getPropertyAsFunction(js, "scopeProbeSecond");
    facebook::jsi::Object record(js);
    record.setProperty(js, "left", "fresh");
    record.setProperty(js, "right", "value");
    auto outer = hermes.createStringReuseScope();
    require(outer->statistics().hits == 0 && outer->statistics().stores == 0);
    require(first.call(js, record).getString(js).utf8(js) == "fresh:value");
    require(outer->statistics().misses == 1 && outer->statistics().stores == 1);
    js.instrumentation().collectGarbage("scoped string result");
    require(second.call(js, record).getString(js).utf8(js) == "fresh:value");
    require(outer->statistics().hits == 1);
    {
      auto inner = hermes.createStringReuseScope();
      require(second.call(js, record).getString(js).utf8(js) == "fresh:value");
      require(inner->statistics().misses == 1 && inner->statistics().hits == 0);
      js.instrumentation().collectGarbage("nested string result");
    }
    require(first.call(js, record).getString(js).utf8(js) == "fresh:value");
    require(outer->statistics().hits == 2);
    record.setProperty(js, "left", "changed");
    require(second.call(js, record).getString(js).utf8(js) == "changed:value");
    require(outer->statistics().misses == 2);
    outer.reset();
    {
      auto fresh = hermes.createStringReuseScope();
      require(first.call(js, record).getString(js).utf8(js) == "changed:value");
      require(fresh->statistics().misses == 1 && fresh->statistics().hits == 0);
    }
    record.setProperty(js, "left", "");
    bool threw = false;
    try {
      auto failing = hermes.createStringReuseScope();
      first.call(js, record);
    } catch (const facebook::jsi::JSError &) {
      threw = true;
    }
    require(threw);
    auto afterFailure = hermes.createStringReuseScope();
    record.setProperty(js, "left", "restored");
    require(first.call(js, record).getString(js).utf8(js) == "restored:value");
    require(afterFailure->statistics().misses == 1);
    auto factory = js.global().getPropertyAsFunction(js, "scopeJoinFactory");
    auto colon = factory.call(js, ":").getObject(js).getFunction(js);
    auto slash = factory.call(js, "/").getObject(js).getFunction(js);
    require(colon.call(js, record).getString(js).utf8(js) == "restored:value");
    require(slash.call(js, record).getString(js).utf8(js) == "restored/value");
    auto beforeHit = afterFailure->statistics();
    require(colon.call(js, record).getString(js).utf8(js) == "restored:value");
    require(afterFailure->statistics().hits == beforeHit.hits + 1);
    record.setProperty(js, "left", std::string(40000, 'x'));
    auto beforeLarge = afterFailure->statistics();
    require(first.call(js, record).getString(js).utf8(js).size() == 40006);
    require(second.call(js, record).getString(js).utf8(js).size() == 40006);
    require(afterFailure->statistics().stores == beforeLarge.stores);
    require(afterFailure->statistics().misses == beforeLarge.misses + 2);
    {
      auto distinct = hermes.createStringReuseScope();
      for (unsigned i = 0; i < 64; ++i) {
        const auto left = std::to_string(i);
        record.setProperty(js, "left", left);
        require(first.call(js, record).getString(js).utf8(js) == left + ":value");
      }
      require(distinct->statistics().hits == 0);
      require(distinct->statistics().misses == 32);
      require(second.call(js, record).getString(js).utf8(js) == "63:value");
      require(distinct->statistics().misses == 32);
      // A later invocation must reconsider reuse independently of this one.
      auto nextInvocation = hermes.createStringReuseScope();
      require(first.call(js, record).getString(js).utf8(js) == "63:value");
      require(second.call(js, record).getString(js).utf8(js) == "63:value");
      require(nextInvocation->statistics().misses == 1);
      require(nextInvocation->statistics().hits == 1);
    }
  }
  _sh_done(runtime);
  return 0;
}

#ifndef __EMSCRIPTEN__
int main() {
  return scoped_normalization_run();
}
#endif
