cmake_minimum_required(VERSION 3.25)
project(beam_erlang_re VERSION 0.0.1 LANGUAGES C)
find_package(BEAM CONFIG REQUIRED COMPONENTS NIF)
if(NOT DEFINED BEAM_SDK_HAS_LEGACY_RE_BIFS OR BEAM_SDK_HAS_LEGACY_RE_BIFS)
    message(FATAL_ERROR "The selected SDK must declare BEAM_SDK_HAS_LEGACY_RE_BIFS=OFF; legacy re BIFs would replace the package implementation")
endif()
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_LIBRARIES BEAM::NIF)
check_c_source_compiles("#include <beam/native.h>\nint main(void) { return beam_native_stack_has_space(10240); }" BEAM_RE_HAS_STACK_API)
unset(CMAKE_REQUIRED_LIBRARIES)
if(NOT BEAM_RE_HAS_STACK_API)
    message(FATAL_ERROR "The selected installed SDK lacks beam_native_stack_has_space")
endif()
if(NOT COMMAND beam_package_native_output)
    set(BEAM_PACKAGE_NATIVE_HELPER "" CACHE FILEPATH "Selected package-owned native output helper")
    if(NOT BEAM_PACKAGE_NATIVE_HELPER)
        get_filename_component(_beam_library "${BEAM_SDK_METADATA}" DIRECTORY)
        set(BEAM_PACKAGE_NATIVE_HELPER "${_beam_library}/apps/beam-pkg/cmake/BEAMPackageNative.cmake")
    endif()
    include("${BEAM_PACKAGE_NATIVE_HELPER}")
endif()
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
set(BEAM_RE_NATIVE_KIND MODULE CACHE STRING "Native output: MODULE or STATIC")
set_property(CACHE BEAM_RE_NATIVE_KIND PROPERTY STRINGS MODULE STATIC)
if(NOT BEAM_RE_NATIVE_KIND MATCHES "^(MODULE|STATIC)$")
    message(FATAL_ERROR "BEAM_RE_NATIVE_KIND must be MODULE or STATIC")
endif()
option(BEAM_RE_TEST_INSTRUMENTATION "Build package-owned native test controls" OFF)
set(_pcre_sources
    pcre2_auto_possess.c pcre2_chartables.c pcre2_chkdint.c pcre2_compile.c
    pcre2_compile_cgroup.c pcre2_compile_class.c pcre2_config.c pcre2_context.c
    pcre2_error.c pcre2_extuni.c pcre2_find_bracket.c pcre2_match.c
    pcre2_match_data.c pcre2_newline.c pcre2_ord2utf.c pcre2_pattern_info.c
    pcre2_script_run.c pcre2_serialize.c pcre2_string_utils.c pcre2_study.c
    pcre2_substring.c pcre2_tables.c pcre2_ucd.c pcre2_valid_utf.c pcre2_xclass.c)
list(TRANSFORM _pcre_sources PREPEND "vendor/pcre2/")
set(_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${_generated}")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_backend.py"
    --source "${CMAKE_CURRENT_SOURCE_DIR}/vendor/pcre2" --output "${_generated}"
    COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    vendor/pcre2/pcre2_match.c vendor/pcre2/pcre2.h vendor/pcre2/pcre2_internal.h tools/generate_backend.py)
add_library(re_native ${BEAM_RE_NATIVE_KIND} native/re_nif.c ${_pcre_sources})
target_compile_features(re_native PRIVATE c_std_11)
target_compile_definitions(re_native PRIVATE ERLANG_INTEGRATION PCRE2_CODE_UNIT_WIDTH=8
    PCRE2_STATIC HAVE_CONFIG_H)
if(BEAM_RE_TEST_INSTRUMENTATION)
    target_compile_definitions(re_native PRIVATE BEAM_RE_TESTING DEBUG)
endif()
target_include_directories(re_native PRIVATE vendor/pcre2 "${_generated}")
set_target_properties(re_native PROPERTIES PREFIX "" OUTPUT_NAME re
    POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
if(MSVC)
    target_compile_options(re_native PRIVATE "/FI${_generated}/backend_symbols.h")
else()
    target_compile_options(re_native PRIVATE -include "${_generated}/backend_symbols.h")
endif()
beam_target_nif(re_native MODULE re)
beam_package_native_output(re_native NAME re)

include(CTest)
if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)
    set(BEAM_RE_TEST_SDK "" CACHE PATH "Exact installed candidate BEAM SDK")
    if(NOT BEAM_RE_TEST_SDK)
        get_filename_component(BEAM_RE_TEST_SDK "${BEAM_SDK_METADATA}/../../.." ABSOLUTE)
    endif()
    add_test(NAME re-installed COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/acceptance.py" --sdk "${BEAM_RE_TEST_SDK}"
        --source "${CMAKE_CURRENT_SOURCE_DIR}" --work "${CMAKE_CURRENT_BINARY_DIR}/acceptance"
        --cmake "${CMAKE_COMMAND}")
    set_tests_properties(re-installed PROPERTIES TIMEOUT 1800 RUN_SERIAL TRUE)
endif()
