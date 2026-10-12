# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_hermes_library(hermesBCP47Parser BCP47Parser.cpp)

if(HERMES_ENABLE_INTL)
  if(HERMES_USE_STATIC_INTL_PROVIDER)
    add_subdirectory(${CMAKE_SOURCE_DIR}/external/icu-small ${CMAKE_BINARY_DIR}/external/icu-small)
    add_hermes_library(hermesPlatformIntl
      impl_icu_provider/PlatformIntlIcuProvider.cpp
      impl_icu_provider/IcuProviderUtils.cpp
      impl_icu_provider/IcuProviderStatic.cpp
      LINK_LIBS hermesStaticIcu
      LINK_OBJLIBS hermesBCP47Parser hermesPublic)
    target_include_directories(hermesPlatformIntl_obj BEFORE PRIVATE
      ${CMAKE_SOURCE_DIR}/external/icu-small/source/common
      ${CMAKE_SOURCE_DIR}/external/icu-small/source/i18n)
    target_compile_definitions(hermesPlatformIntl_obj PRIVATE
      U_STATIC_IMPLEMENTATION=1 U_DISABLE_RENAMING=1)
  elseif(HERMES_IS_ANDROID)
    add_hermes_library(hermesPlatformIntl PlatformIntlAndroid.cpp
        LINK_LIBS fbjni::fbjni
        LINK_OBJLIBS hermesPublic
    )
    target_compile_options(hermesPlatformIntl_obj PRIVATE -frtti -fexceptions)
  elseif(APPLE)
    add_hermes_library(hermesPlatformIntl PlatformIntlApple.mm
        LINK_LIBS ${FOUNDATION}
        LINK_OBJLIBS hermesPublic
    )
    # Work around a bug in unity builds where it tries to build Obj-C as C++.
    set_target_properties(hermesPlatformIntl_obj PROPERTIES UNITY_BUILD false)
    target_compile_options(hermesPlatformIntl_obj PRIVATE -fobjc-arc)
  else()
    message(FATAL_ERROR
      "HERMES_ENABLE_INTL requires a native Apple/Android backend or "
      "HERMES_USE_STATIC_INTL_PROVIDER=ON on Emscripten, Linux, or Apple.")
  endif()
endif()
