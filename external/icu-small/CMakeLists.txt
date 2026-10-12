# Copyright (c) Microsoft Corporation. Licensed under the MIT License.
# Experimental static build of the pinned ICU source and locale dataset.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(ICU_DAT "${CMAKE_CURRENT_BINARY_DIR}/icudt78l.dat")
if(NOT EXISTS "${ICU_DAT}")
  execute_process(COMMAND ${Python3_EXECUTABLE} -c
    "import bz2,sys;open(sys.argv[2],'wb').write(bz2.open(sys.argv[1]).read())"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/data/in/icudt78l.dat.bz2" "${ICU_DAT}"
    COMMAND_ERROR_IS_FATAL ANY)
endif()
file(SHA256 "${ICU_DAT}" ICU_DATA_SHA256)
if(NOT ICU_DATA_SHA256 STREQUAL "a3d49eacd189624769dc77457c6aa1789a89219fb217a6d87d5b076c269aaf1f")
  message(FATAL_ERROR "Pinned ICU locale data digest differs")
endif()
# Code and data form one reusable archive. Application module edits do not
# regenerate this assembly or rebuild the data object.
set(ICU_DATA_ASSEMBLY "${CMAKE_CURRENT_BINARY_DIR}/hermes_icu_data.s")
set(ICU_DATA_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/hermes_icu_data.o")
if(APPLE)
  set(ICU_DATA_ASSEMBLY_CONTENT
    ".section __TEXT,__const\n.p2align 4\n.globl _icudt78_dat\n_icudt78_dat:\n.incbin \"${ICU_DAT}\"\n")
elseif(EMSCRIPTEN)
  set(ICU_DATA_ASSEMBLY_CONTENT
    ".section .rodata.icudt78_dat,\"\",@\n.p2align 4\n.globl icudt78_dat\n.type icudt78_dat,@object\nicudt78_dat:\n.incbin \"${ICU_DAT}\"\n.size icudt78_dat, .-icudt78_dat\n")
else()
  set(ICU_DATA_ASSEMBLY_CONTENT
    ".section .rodata.icudt78_dat,\"a\",@progbits\n.p2align 4\n.globl icudt78_dat\n.type icudt78_dat,@object\nicudt78_dat:\n.incbin \"${ICU_DAT}\"\n.size icudt78_dat, .-icudt78_dat\n.section .note.GNU-stack,\"\",@progbits\n")
endif()
file(GENERATE OUTPUT "${ICU_DATA_ASSEMBLY}" CONTENT "${ICU_DATA_ASSEMBLY_CONTENT}")
add_custom_command(OUTPUT "${ICU_DATA_OBJECT}"
  COMMAND "${CMAKE_C_COMPILER}" -c "${ICU_DATA_ASSEMBLY}" -o "${ICU_DATA_OBJECT}"
  DEPENDS "${ICU_DATA_ASSEMBLY}" "${ICU_DAT}"
  VERBATIM)
set_source_files_properties("${ICU_DATA_OBJECT}" PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
file(GLOB ICU_COMMON_SOURCES source/common/*.cpp source/common/*.c)
file(GLOB ICU_I18N_SOURCES source/i18n/*.cpp source/i18n/*.c)
add_library(hermesStaticIcu STATIC ${ICU_COMMON_SOURCES} ${ICU_I18N_SOURCES}
  hermes_icu_glue.cpp "${ICU_DATA_OBJECT}")
target_include_directories(hermesStaticIcu BEFORE PRIVATE source/common source/i18n ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(hermesStaticIcu PRIVATE U_STATIC_IMPLEMENTATION=1 U_DISABLE_RENAMING=1
  U_ENABLE_DYLOAD=0 UCONFIG_NO_SERVICE=1 U_HAVE_STD_STRING=1 U_ATTRIBUTE_DEPRECATED=)
set_property(SOURCE ${ICU_COMMON_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS U_COMMON_IMPLEMENTATION=1)
set_property(SOURCE ${ICU_I18N_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS U_I18N_IMPLEMENTATION=1)
target_compile_options(hermesStaticIcu PRIVATE -fexceptions -frtti)
if(EMSCRIPTEN)
  target_compile_options(hermesStaticIcu PRIVATE -fwasm-exceptions)
endif()
target_compile_options(hermesStaticIcu PRIVATE
  -Wno-deprecated-declarations -Wno-sign-compare -Wno-unused-variable)

option(HERMES_ICU_FAIL_STOP_ALLOCATION "Trap rather than return null after ICU allocation exhaustion" ON)
set_property(SOURCE source/common/cmemory.cpp APPEND PROPERTY COMPILE_DEFINITIONS
  HERMES_ICU_FAIL_STOP_ALLOCATION=$<BOOL:${HERMES_ICU_FAIL_STOP_ALLOCATION}>)
