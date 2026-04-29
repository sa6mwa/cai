cmake_minimum_required(VERSION 3.21)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CaiVersion)

cai_detect_version(CAI_RESOLVED_VERSION)
project(cai VERSION ${CAI_RESOLVED_VERSION} LANGUAGES C)

include(CTest)
include(GNUInstallDirs)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CAI_BUILD_STATIC "Build the static cai library." ON)
option(CAI_BUILD_SHARED "Build the shared cai library." ON)
option(CAI_BUILD_TESTS "Build cai unit tests." ON)
option(CAI_ENABLE_COVERAGE "Build tests with coverage instrumentation." OFF)
option(CAI_INSTALL "Enable install rules." ON)

set(CAI_VERSION_OVERRIDE "" CACHE STRING "Override detected cai package version.")
set(CAI_DIST_DIR "${CMAKE_BINARY_DIR}/dist" CACHE PATH "Distribution output directory.")
set(CAI_EXTERNAL_ROOT "${CMAKE_SOURCE_DIR}/.cache/deps/host-debug"
    CACHE PATH "Provisioned third-party dependency root.")
set(CAI_LONEJSON_SOURCE_DIR "${CMAKE_SOURCE_DIR}/../lonejson"
    CACHE PATH "Development lonejson checkout used until SDK dependency provisioning lands.")

if(NOT CAI_BUILD_STATIC AND NOT CAI_BUILD_SHARED)
  message(FATAL_ERROR "Enable at least one of CAI_BUILD_STATIC or CAI_BUILD_SHARED.")
endif()

function(cai_configure_c_target target)
  get_target_property(_cai_target_type ${target} TYPE)
  set_target_properties(${target} PROPERTIES
    C_STANDARD 90
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON)
  if(UNIX OR APPLE OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    target_compile_definitions(${target} PRIVATE _POSIX_C_SOURCE=200809L)
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_compile_definitions(${target} PRIVATE _DARWIN_C_SOURCE=1)
  endif()
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${target} PRIVATE
      -std=c89
      -Wall
      -Wextra
      -Werror
      -Wpedantic
      -Wno-long-long
      -Wstrict-prototypes
      -Wmissing-prototypes
      -Wshadow
      -Wpointer-arith
      -Wcast-qual
      -Wwrite-strings
      -Wdeclaration-after-statement)
  endif()
  if(CAI_ENABLE_COVERAGE AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${target} PRIVATE --coverage -O0)
    if(NOT _cai_target_type STREQUAL "STATIC_LIBRARY")
      target_link_options(${target} PRIVATE --coverage)
    endif()
  endif()
endfunction()

set(CAI_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai_version.h.in"
  "${CAI_GENERATED_INCLUDE_DIR}/cai/version.h"
  @ONLY)

set(CAI_LONEJSON_INCLUDE_DIR "")
if(EXISTS "${CAI_EXTERNAL_ROOT}/include/lonejson.h")
  set(CAI_LONEJSON_INCLUDE_DIR "${CAI_EXTERNAL_ROOT}/include")
  find_library(CAI_LONEJSON_LIBRARY NAMES lonejson PATHS "${CAI_EXTERNAL_ROOT}/lib" NO_DEFAULT_PATH)
elseif(EXISTS "${CAI_LONEJSON_SOURCE_DIR}/include/lonejson.h")
  set(CAI_LONEJSON_INCLUDE_DIR "${CAI_LONEJSON_SOURCE_DIR}/include")
  add_library(cai_lonejson STATIC "${CAI_LONEJSON_SOURCE_DIR}/src/lonejson.c")
  target_include_directories(cai_lonejson PUBLIC "${CAI_LONEJSON_SOURCE_DIR}/include")
  set_target_properties(cai_lonejson PROPERTIES
    C_STANDARD 90
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON)
  target_compile_definitions(cai_lonejson PRIVATE
    _POSIX_C_SOURCE=200809L
    _ISOC99_SOURCE=1)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(cai_lonejson PRIVATE
      -std=c89
      -Wall
      -Wextra
      -Wpedantic
      -Wno-long-long)
  endif()
endif()

if(CAI_LONEJSON_INCLUDE_DIR STREQUAL "")
  message(FATAL_ERROR
    "lonejson is required. Provision ${CAI_EXTERNAL_ROOT}/include/lonejson.h "
    "or set CAI_LONEJSON_SOURCE_DIR to a lonejson checkout.")
endif()

set(CAI_SOURCES
  src/cai_allocator.c
  src/cai_client.c
  src/cai_env.c
  src/cai_error.c
  src/cai_models.c
  src/cai_response.c
  src/cai_source.c)

set(CAI_PUBLIC_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(CAI_BUILD_STATIC)
  add_library(cai_static STATIC ${CAI_SOURCES})
  add_library(cai::static ALIAS cai_static)
  set_target_properties(cai_static PROPERTIES OUTPUT_NAME cai)
endif()

if(CAI_BUILD_SHARED)
  add_library(cai_shared SHARED ${CAI_SOURCES})
  add_library(cai::shared ALIAS cai_shared)
  set_target_properties(cai_shared PROPERTIES
    OUTPUT_NAME cai
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
endif()

foreach(_cai_target IN ITEMS cai_static cai_shared)
  if(TARGET ${_cai_target})
    target_include_directories(${_cai_target}
      PUBLIC
        $<BUILD_INTERFACE:${CAI_PUBLIC_INCLUDE_DIR}>
        $<BUILD_INTERFACE:${CAI_GENERATED_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_include_directories(${_cai_target} PRIVATE "${CAI_LONEJSON_INCLUDE_DIR}")
    if(TARGET cai_lonejson)
      target_link_libraries(${_cai_target} PRIVATE cai_lonejson)
    elseif(CAI_LONEJSON_LIBRARY)
      target_link_libraries(${_cai_target} PRIVATE "${CAI_LONEJSON_LIBRARY}")
    else()
      target_link_libraries(${_cai_target} PRIVATE lonejson)
    endif()
    cai_configure_c_target(${_cai_target})
  endif()
endforeach()

if(CAI_BUILD_TESTS)
  enable_testing()
  add_executable(cai_tests tests/test_main.c)
  target_include_directories(cai_tests PRIVATE
    "${CAI_PUBLIC_INCLUDE_DIR}"
    "${CAI_GENERATED_INCLUDE_DIR}"
    "${CAI_LONEJSON_INCLUDE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(cai_tests PRIVATE cai_static)
  cai_configure_c_target(cai_tests)
  add_test(NAME cai_tests COMMAND cai_tests)
endif()

find_program(CLANG_FORMAT_BIN NAMES clang-format)
file(GLOB_RECURSE CAI_FORMAT_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.h")
if(CLANG_FORMAT_BIN)
  add_custom_target(clang-format
    COMMAND "${CLANG_FORMAT_BIN}" -i -style=file ${CAI_FORMAT_SOURCES}
    VERBATIM)
else()
  add_custom_target(clang-format
    COMMAND "${CMAKE_COMMAND}" -E echo "clang-format not found"
    VERBATIM)
endif()

if(CAI_INSTALL)
  install(DIRECTORY "${CAI_PUBLIC_INCLUDE_DIR}/" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(DIRECTORY "${CAI_GENERATED_INCLUDE_DIR}/" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  if(TARGET cai_static)
    install(TARGETS cai_static EXPORT caiTargets
      ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
      LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
      RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  endif()
  if(TARGET cai_shared)
    install(TARGETS cai_shared EXPORT caiTargets
      ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
      LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
      RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  endif()
endif()
