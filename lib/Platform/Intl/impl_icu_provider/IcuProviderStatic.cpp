/* Copyright (c) Microsoft Corporation. Licensed under the MIT License. */
#include "IcuProviderStatic.h"
#include <cassert>
extern "C" const hermes_icu_vtable *hermes_icu_get_vtable(void);
namespace hermes { namespace platform_intl {
const hermes_icu_vtable *getIcuVtableForRuntime(vm::Runtime &) {
  const auto *provider = hermes_icu_get_vtable();
  assert(provider && "Static ICU provider is required");
  return provider;
}
}}
