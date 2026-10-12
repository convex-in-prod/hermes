/* Copyright (c) Microsoft Corporation. Licensed under the MIT License. */
#pragma once
#include "hermes/Platform/Intl/hermes_icu.h"
namespace hermes { namespace vm { class Runtime; }
namespace platform_intl {
const hermes_icu_vtable *getIcuVtableForRuntime(vm::Runtime &runtime);
}}
