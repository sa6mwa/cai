cmake_minimum_required(VERSION 3.21)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CaiVersion)

cai_detect_version(CAI_RESOLVED_VERSION)
project(cai VERSION ${CAI_RESOLVED_VERSION} LANGUAGES C)

include(CMakePackageConfigHelpers)
include(CTest)
include(GNUInstallDirs)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CAI_BUILD_STATIC "Build the static cai library." ON)
option(CAI_BUILD_SHARED "Build the shared cai library." ON)
option(CAI_BUILD_TESTS "Build cai unit tests." ON)
option(CAI_BUILD_INTEGRATION_TESTS "Build integration tests that hit OpenAI when run." OFF)
option(CAI_BUILD_EXAMPLES "Build cai examples." ON)
option(CAI_BUILD_LUA "Build the Lua 5.5 binding and Lua tests." OFF)
option(CAI_BUILD_FUZZERS "Build fuzz harnesses." OFF)
option(CAI_ENABLE_COVERAGE "Build tests with coverage instrumentation." OFF)
option(CAI_INSTALL "Enable install rules." ON)
option(CAI_USE_LOCKDC_SDK "Deprecated alias for CAI_DEPENDENCY_MODE=cpkt." OFF)

set(CAI_VERSION_OVERRIDE "" CACHE STRING "Override detected cai package version.")
set(CAI_DIST_DIR "${CMAKE_BINARY_DIR}/dist" CACHE PATH "Distribution output directory.")
set(CAI_TARGET_ID "" CACHE STRING "Release archive target id.")
set(CAI_DEPS_DIR "${CMAKE_SOURCE_DIR}/.cache/deps"
    CACHE PATH "Downloaded third-party dependency cache.")
set(CAI_DEPENDENCY_MODE "cpkt" CACHE STRING
    "Dependency mode: cpkt, host, or auto.")
set_property(CACHE CAI_DEPENDENCY_MODE PROPERTY STRINGS cpkt host auto)
set(CAI_C_PKT_SYSTEMS_VERSION "0.1.0" CACHE STRING
    "Pinned c.pkt.systems release used for curl/native dependencies." FORCE)
set(CAI_LONEJSON_VERSION "0.25.0")
set(CAI_PSLOG_VERSION "0.4.1")

if(NOT CAI_BUILD_STATIC AND NOT CAI_BUILD_SHARED)
  message(FATAL_ERROR "Enable at least one of CAI_BUILD_STATIC or CAI_BUILD_SHARED.")
endif()

if(CAI_USE_LOCKDC_SDK)
  message(WARNING
    "CAI_USE_LOCKDC_SDK is deprecated; using CAI_DEPENDENCY_MODE=cpkt.")
  set(CAI_DEPENDENCY_MODE "cpkt" CACHE STRING
      "Dependency mode: cpkt, host, or auto." FORCE)
endif()
if(CAI_DEPENDENCY_MODE STREQUAL "lockdc")
  message(WARNING
    "CAI_DEPENDENCY_MODE=lockdc is deprecated; use cpkt. Treating it as cpkt.")
  set(CAI_DEPENDENCY_MODE "cpkt" CACHE STRING
      "Dependency mode: cpkt, host, or auto." FORCE)
endif()
if(CAI_DEPENDENCY_MODE STREQUAL "pkt")
  message(WARNING
    "CAI_DEPENDENCY_MODE=pkt is deprecated; use cpkt. Treating it as cpkt.")
  set(CAI_DEPENDENCY_MODE "cpkt" CACHE STRING
      "Dependency mode: cpkt, host, or auto." FORCE)
endif()
if(NOT CAI_DEPENDENCY_MODE STREQUAL "cpkt" AND
   NOT CAI_DEPENDENCY_MODE STREQUAL "host" AND
   NOT CAI_DEPENDENCY_MODE STREQUAL "auto")
  message(FATAL_ERROR
    "CAI_DEPENDENCY_MODE must be one of: cpkt, host, auto.")
endif()

if(CAI_TARGET_ID STREQUAL "")
  string(TOLOWER "${CMAKE_SYSTEM_NAME}" _cai_system_name_lower)
  string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _cai_processor_lower)
  if(_cai_system_name_lower STREQUAL "linux")
    set(_cai_linux_abi "gnu")
    execute_process(
      COMMAND "${CMAKE_C_COMPILER}" -dumpmachine
      OUTPUT_VARIABLE _cai_compiler_machine
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_cai_compiler_machine MATCHES "musl")
      set(_cai_linux_abi "musl")
    endif()
    if(_cai_processor_lower STREQUAL "x86_64" OR
       _cai_processor_lower STREQUAL "amd64")
      set(CAI_TARGET_ID "x86_64-linux-${_cai_linux_abi}")
    elseif(_cai_processor_lower STREQUAL "aarch64" OR
           _cai_processor_lower STREQUAL "arm64")
      set(CAI_TARGET_ID "aarch64-linux-${_cai_linux_abi}")
    elseif(_cai_processor_lower MATCHES "^(armv7|armv6|armhf)")
      set(CAI_TARGET_ID "armhf-linux-${_cai_linux_abi}")
    else()
      set(CAI_TARGET_ID "${_cai_processor_lower}-linux-${_cai_linux_abi}")
    endif()
  elseif(_cai_system_name_lower STREQUAL "darwin")
    if(_cai_processor_lower STREQUAL "arm64" OR
       _cai_processor_lower STREQUAL "aarch64")
      set(CAI_TARGET_ID "arm64-apple-darwin")
    elseif(_cai_processor_lower STREQUAL "x86_64" OR
           _cai_processor_lower STREQUAL "amd64")
      set(CAI_TARGET_ID "x86_64-apple-darwin")
    else()
      set(CAI_TARGET_ID "${_cai_processor_lower}-apple-darwin")
    endif()
  else()
    set(CAI_TARGET_ID "${_cai_processor_lower}-${_cai_system_name_lower}")
  endif()
endif()

function(cai_c_pkt_systems_sha256 target_id out_var)
  if(target_id STREQUAL "x86_64-linux-gnu")
    set(_sha256 "4e6c4ca07c0647a05923b4a56ef12d440a1d1b53465224e30d990fc18777aa4e")
  elseif(target_id STREQUAL "x86_64-linux-musl")
    set(_sha256 "d44f70558b961125c96d356d27ce83fc7d50c9cc650a335c2016c8d3778d98aa")
  elseif(target_id STREQUAL "aarch64-linux-gnu")
    set(_sha256 "c20969872de3087f984e8bca3e01fa98e495a3581940e426d07ebed014cf8190")
  elseif(target_id STREQUAL "aarch64-linux-musl")
    set(_sha256 "8ff3cc3c457dc66918470beaea01744bc38c342a87c20c6b072761c56c858e19")
  elseif(target_id STREQUAL "armhf-linux-gnu")
    set(_sha256 "26787953d690b0f01a11538e8692f68f9c746b8e97a9baf47ac15241d9a947fc")
  elseif(target_id STREQUAL "armhf-linux-musl")
    set(_sha256 "f0172a6ff928111cfaeb503b01b48b3cdd2c05a04d54047630180ee79f65af31")
  elseif(target_id STREQUAL "arm64-apple-darwin")
    set(_sha256 "dba4424de9566c2418162f62e5e90c45b40266c6e750b5096d4a251bf96d8e9a")
  else()
    message(FATAL_ERROR "No pinned c.pkt.systems checksum for target ${target_id}")
  endif()
  set(${out_var} "${_sha256}" PARENT_SCOPE)
endfunction()

function(cai_lonejson_pkg_sha256 target_id out_var)
  if(target_id STREQUAL "x86_64-linux-gnu")
    set(_sha256 "0d87fee12896f7ca341b3a57e06b601c526a323f2ac9849a93558be9e64f8583")
  elseif(target_id STREQUAL "x86_64-linux-musl")
    set(_sha256 "c567cdcaf22fa0814fb5e73d57efde0299e15b61c6c61cf8fc3e8962be10a0d4")
  elseif(target_id STREQUAL "aarch64-linux-gnu")
    set(_sha256 "f03b37651d44897206c215f5b22682e709698e8daf84a47a5964bba4b1ca4d05")
  elseif(target_id STREQUAL "aarch64-linux-musl")
    set(_sha256 "83c4751c5b4c1a4478eccd35a9e9b252716d57a45f2cf555c7c5e2a001899184")
  elseif(target_id STREQUAL "armhf-linux-gnu")
    set(_sha256 "a6dba562e07d0ee2bc257464828f2c14a646fa0ba0d09f1d0258811ded451c56")
  elseif(target_id STREQUAL "armhf-linux-musl")
    set(_sha256 "b46d70f497c0e732e0700d6c86d6f820c32f071ad483ec7792ec66456e5a2be4")
  elseif(target_id STREQUAL "arm64-apple-darwin")
    set(_sha256 "46ee36b503a437d6b728810e44e31e6e30988252c6ca69ba1f1344a0f33435e3")
  else()
    message(FATAL_ERROR
      "No pinned liblonejson package checksum for target ${target_id}")
  endif()
  set(${out_var} "${_sha256}" PARENT_SCOPE)
endfunction()

function(cai_pslog_pkg_sha256 target_id out_var)
  if(target_id STREQUAL "x86_64-linux-gnu")
    set(_sha256 "91d2f93bc07bc66cf83d6a27a80cb6439c384d56bf84a2d11cd903215430d1d8")
  elseif(target_id STREQUAL "x86_64-linux-musl")
    set(_sha256 "b628d32f9207e5102c9a8ae3f7ad32ce36e61178c7db67e6aa4548eb9cae567d")
  elseif(target_id STREQUAL "aarch64-linux-gnu")
    set(_sha256 "d936ae9416f539c4f40aeaa023b9147cbd568bc87b7a3c3b091adfd217d935bb")
  elseif(target_id STREQUAL "aarch64-linux-musl")
    set(_sha256 "638725174cf39f3c5337fc6f118bc88c2a41d385a01be98170ff4bef3d57fcae")
  elseif(target_id STREQUAL "armhf-linux-gnu")
    set(_sha256 "bc8530a3773666deb6d551263c7dd59a64c92629fa56d1e89c278d637472f2dc")
  elseif(target_id STREQUAL "armhf-linux-musl")
    set(_sha256 "503d2bd882c053dc8f34dbfe718a328303a4973789bbb8fb37261e4822b3babe")
  elseif(target_id STREQUAL "arm64-apple-darwin")
    set(_sha256 "f8f4e18810ecad7278eb341fbfe7e3f9d85eb654891c4d08149f425f3a4c9b3d")
  else()
    message(FATAL_ERROR
      "No pinned libpslog package checksum for target ${target_id}")
  endif()
  set(${out_var} "${_sha256}" PARENT_SCOPE)
endfunction()

set(CAI_RESOLVED_DEPENDENCY_MODE "${CAI_DEPENDENCY_MODE}")
if(CAI_DEPENDENCY_MODE STREQUAL "auto")
  find_path(_cai_auto_lonejson_include lonejson.h)
  find_path(_cai_auto_pslog_include pslog.h)
  find_library(_cai_auto_lonejson_library NAMES lonejson)
  find_library(_cai_auto_pslog_library NAMES pslog)
  find_package(CURL QUIET)
  if(CURL_FOUND AND _cai_auto_lonejson_include AND _cai_auto_pslog_include AND
     _cai_auto_lonejson_library AND _cai_auto_pslog_library)
    set(CAI_RESOLVED_DEPENDENCY_MODE "host")
  else()
    set(CAI_RESOLVED_DEPENDENCY_MODE "cpkt")
  endif()
  unset(_cai_auto_lonejson_include CACHE)
  unset(_cai_auto_pslog_include CACHE)
  unset(_cai_auto_lonejson_library CACHE)
  unset(_cai_auto_pslog_library CACHE)
endif()
if(CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "cpkt" AND
   CAI_TARGET_ID STREQUAL "x86_64-apple-darwin")
  message(WARNING
    "No pinned cpkt dependency package exists for x86_64-apple-darwin; "
    "falling back to host dependencies. Use arm64-apple-darwin for cpkt "
    "Darwin release builds.")
  set(CAI_RESOLVED_DEPENDENCY_MODE "host")
endif()

function(cai_download_tarball name url archive prefix sha256 stamp_file)
  file(MAKE_DIRECTORY "${CAI_DEPS_DIR}")
  set(_cai_download 1)
  if(EXISTS "${archive}")
    file(SHA256 "${archive}" _cai_actual_sha256)
    if(_cai_actual_sha256 STREQUAL "${sha256}")
      set(_cai_download 0)
    endif()
  endif()
  if(_cai_download)
    message(STATUS "Downloading ${name} from ${url}")
    file(DOWNLOAD
      "${url}"
      "${archive}"
      EXPECTED_HASH "SHA256=${sha256}"
      TLS_VERIFY ON
      SHOW_PROGRESS)
  endif()
  if(NOT EXISTS "${stamp_file}")
    file(REMOVE_RECURSE "${prefix}")
    file(MAKE_DIRECTORY "${prefix}")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E tar xzf "${archive}"
      WORKING_DIRECTORY "${CAI_DEPS_DIR}"
      RESULT_VARIABLE _cai_extract_result)
    if(NOT _cai_extract_result EQUAL 0)
      message(FATAL_ERROR "Failed to extract ${archive}")
    endif()
  endif()
endfunction()

function(cai_set_lonejson_from_target)
  if(NOT TARGET lonejson::lonejson)
    return()
  endif()
  get_target_property(_cai_lonejson_include lonejson::lonejson
    INTERFACE_INCLUDE_DIRECTORIES)
  get_target_property(_cai_lonejson_library lonejson::lonejson
    IMPORTED_LOCATION)
  if(_cai_lonejson_include AND _cai_lonejson_library)
    set(CAI_LONEJSON_INCLUDE_DIR "${_cai_lonejson_include}" PARENT_SCOPE)
    set(CAI_LONEJSON_LIBRARY "${_cai_lonejson_library}" PARENT_SCOPE)
    set(CAI_LONEJSON_LINK lonejson::lonejson PARENT_SCOPE)
  endif()
endfunction()

function(cai_set_pslog_from_target)
  if(TARGET pslog::pslog_shared)
    get_target_property(_cai_pslog_include pslog::pslog_shared
      INTERFACE_INCLUDE_DIRECTORIES)
    set(_cai_pslog_link pslog::pslog_shared)
  elseif(TARGET pslog::pslog_static)
    get_target_property(_cai_pslog_include pslog::pslog_static
      INTERFACE_INCLUDE_DIRECTORIES)
    set(_cai_pslog_link pslog::pslog_static)
  else()
    return()
  endif()
  if(_cai_pslog_include)
    set(CAI_PSLOG_INCLUDE_DIR "${_cai_pslog_include}" PARENT_SCOPE)
  endif()
  set(CAI_PSLOG_LINK "${_cai_pslog_link}" PARENT_SCOPE)
endfunction()

if(CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "cpkt")
  cai_c_pkt_systems_sha256("${CAI_TARGET_ID}" CAI_C_PKT_SYSTEMS_SHA256)
  set(CAI_C_PKT_SYSTEMS_NAME
      "c.pkt.systems-${CAI_C_PKT_SYSTEMS_VERSION}-${CAI_TARGET_ID}")
  set(CAI_C_PKT_SYSTEMS_URL
      "https://github.com/sa6mwa/c.pkt.systems/releases/download/v${CAI_C_PKT_SYSTEMS_VERSION}/${CAI_C_PKT_SYSTEMS_NAME}.tar.gz")
  set(CAI_C_PKT_SYSTEMS_ARCHIVE
      "${CAI_DEPS_DIR}/${CAI_C_PKT_SYSTEMS_NAME}.tar.gz")
  set(CAI_C_PKT_SYSTEMS_PREFIX
      "${CAI_DEPS_DIR}/${CAI_C_PKT_SYSTEMS_NAME}")
  cai_download_tarball(
    "c.pkt.systems ${CAI_C_PKT_SYSTEMS_VERSION} ${CAI_TARGET_ID}"
    "${CAI_C_PKT_SYSTEMS_URL}"
    "${CAI_C_PKT_SYSTEMS_ARCHIVE}"
    "${CAI_C_PKT_SYSTEMS_PREFIX}"
    "${CAI_C_PKT_SYSTEMS_SHA256}"
    "${CAI_C_PKT_SYSTEMS_PREFIX}/include/curl/curl.h")

  cai_lonejson_pkg_sha256("${CAI_TARGET_ID}" CAI_LONEJSON_PKG_SHA256)
  set(CAI_LONEJSON_PKG_NAME
      "liblonejson-${CAI_LONEJSON_VERSION}-${CAI_TARGET_ID}")
  set(CAI_LONEJSON_PKG_URL
      "https://github.com/sa6mwa/lonejson/releases/download/v${CAI_LONEJSON_VERSION}/${CAI_LONEJSON_PKG_NAME}.tar.gz")
  set(CAI_LONEJSON_PKG_ARCHIVE
      "${CAI_DEPS_DIR}/${CAI_LONEJSON_PKG_NAME}.tar.gz")
  set(CAI_LONEJSON_PREFIX "${CAI_DEPS_DIR}/${CAI_LONEJSON_PKG_NAME}")
  cai_download_tarball(
    "liblonejson ${CAI_LONEJSON_VERSION} ${CAI_TARGET_ID}"
    "${CAI_LONEJSON_PKG_URL}"
    "${CAI_LONEJSON_PKG_ARCHIVE}"
    "${CAI_LONEJSON_PREFIX}"
    "${CAI_LONEJSON_PKG_SHA256}"
    "${CAI_LONEJSON_PREFIX}/include/lonejson.h")

  cai_pslog_pkg_sha256("${CAI_TARGET_ID}" CAI_PSLOG_PKG_SHA256)
  set(CAI_PSLOG_PKG_NAME "libpslog-${CAI_PSLOG_VERSION}-${CAI_TARGET_ID}")
  set(CAI_PSLOG_PKG_URL
      "https://github.com/sa6mwa/libpslog/releases/download/v${CAI_PSLOG_VERSION}/${CAI_PSLOG_PKG_NAME}.tar.gz")
  set(CAI_PSLOG_PKG_ARCHIVE "${CAI_DEPS_DIR}/${CAI_PSLOG_PKG_NAME}.tar.gz")
  set(CAI_PSLOG_PREFIX "${CAI_DEPS_DIR}/${CAI_PSLOG_PKG_NAME}")
  cai_download_tarball(
    "libpslog ${CAI_PSLOG_VERSION} ${CAI_TARGET_ID}"
    "${CAI_PSLOG_PKG_URL}"
    "${CAI_PSLOG_PKG_ARCHIVE}"
    "${CAI_PSLOG_PREFIX}"
    "${CAI_PSLOG_PKG_SHA256}"
    "${CAI_PSLOG_PREFIX}/include/pslog.h")
  set(CMAKE_PREFIX_PATH
      "${CAI_C_PKT_SYSTEMS_PREFIX};${CAI_LONEJSON_PREFIX};${CAI_PSLOG_PREFIX};${CMAKE_PREFIX_PATH}")
  if(CMAKE_CROSSCOMPILING)
    list(APPEND CMAKE_FIND_ROOT_PATH
         "${CAI_C_PKT_SYSTEMS_PREFIX}"
         "${CAI_LONEJSON_PREFIX}"
         "${CAI_PSLOG_PREFIX}")
  endif()
  set(lonejson_DIR "${CAI_LONEJSON_PREFIX}/lib/cmake/lonejson"
      CACHE PATH "lonejson package directory" FORCE)
  set(pslog_DIR "${CAI_PSLOG_PREFIX}/lib/cmake/pslog"
      CACHE PATH "pslog package directory" FORCE)
  find_package(lonejson CONFIG REQUIRED)
  find_package(pslog CONFIG REQUIRED)
  cai_set_lonejson_from_target()
  cai_set_pslog_from_target()
  if(CAI_TARGET_ID STREQUAL "arm64-apple-darwin")
    set(_cai_cpkt_curl_library "${CAI_C_PKT_SYSTEMS_PREFIX}/lib/libcurl.dylib")
  else()
    set(_cai_cpkt_curl_library "${CAI_C_PKT_SYSTEMS_PREFIX}/lib/libcurl.so")
  endif()
  set(CURL_INCLUDE_DIR "${CAI_C_PKT_SYSTEMS_PREFIX}/include" CACHE PATH "curl include directory" FORCE)
  set(CURL_LIBRARY "${_cai_cpkt_curl_library}" CACHE FILEPATH "curl library" FORCE)
  set(CURL_LIBRARY_RELEASE "${_cai_cpkt_curl_library}" CACHE FILEPATH "curl release library" FORCE)
  list(APPEND CMAKE_INSTALL_RPATH "$ORIGIN")
  get_filename_component(_cai_lonejson_library_dir
    "${CAI_LONEJSON_LIBRARY}" DIRECTORY)
  list(APPEND CMAKE_BUILD_RPATH
       "${CAI_C_PKT_SYSTEMS_PREFIX}/lib" "${_cai_lonejson_library_dir}")
endif()

set(CAI_C_PKT_SYSTEMS_SHA256_FOR_PACKAGE "")
if(CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "cpkt")
  set(CAI_C_PKT_SYSTEMS_SHA256_FOR_PACKAGE "${CAI_C_PKT_SYSTEMS_SHA256}")
endif()

find_package(CURL REQUIRED)
if(CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "cpkt" AND TARGET CURL::libcurl)
  set_target_properties(CURL::libcurl PROPERTIES
    IMPORTED_LOCATION "${_cai_cpkt_curl_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${CAI_C_PKT_SYSTEMS_PREFIX}/include")
endif()
find_package(Threads REQUIRED)

if(CAI_BUILD_LUA)
  find_program(CAI_LUA_EXECUTABLE NAMES lua5.5 lua REQUIRED)
  find_path(CAI_LUA_INCLUDE_DIR lua.h
    PATHS /usr/local/include /usr/include /opt/homebrew/include
    PATH_SUFFIXES lua5.5)
  find_library(CAI_LUA_LIBRARY NAMES lua5.5 lua
    PATHS /usr/local/lib /usr/lib /usr/lib/x86_64-linux-gnu
          /opt/homebrew/lib)
  if(NOT CAI_LUA_INCLUDE_DIR OR NOT CAI_LUA_LIBRARY)
    message(FATAL_ERROR
      "CAI_BUILD_LUA requires Lua 5.5 headers and library. Set "
      "CAI_BUILD_LUA=OFF to skip the binding.")
  endif()
  set(CAI_LUA_TEST_ENV
      "LUA_CPATH=${CMAKE_CURRENT_BINARY_DIR}/lua/?.so;${CMAKE_CURRENT_BINARY_DIR}/lua/?.dylib;;")
  if(CMAKE_C_FLAGS_DEBUG MATCHES "fsanitize=address" AND
     CMAKE_C_COMPILER_ID MATCHES "Clang|GNU" AND
     CMAKE_SYSTEM_NAME STREQUAL "Linux")
    execute_process(
      COMMAND "${CMAKE_C_COMPILER}" -print-file-name=libasan.so
      OUTPUT_VARIABLE CAI_ASAN_LIBRARY
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(CAI_ASAN_LIBRARY AND EXISTS "${CAI_ASAN_LIBRARY}")
      list(APPEND CAI_LUA_TEST_ENV "LD_PRELOAD=${CAI_ASAN_LIBRARY}")
    endif()
  endif()
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

if(NOT CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "cpkt")
  find_package(lonejson CONFIG QUIET)
  find_package(pslog CONFIG QUIET)
  cai_set_lonejson_from_target()
  cai_set_pslog_from_target()
  if(NOT CAI_LONEJSON_INCLUDE_DIR)
    find_path(CAI_LONEJSON_INCLUDE_DIR lonejson.h)
  endif()
  if(NOT CAI_PSLOG_INCLUDE_DIR)
    find_path(CAI_PSLOG_INCLUDE_DIR pslog.h)
  endif()
  if(NOT CAI_PSLOG_LINK)
    find_library(CAI_PSLOG_LIBRARY NAMES pslog)
  endif()
  if(NOT CAI_LONEJSON_LIBRARY)
    find_library(CAI_LONEJSON_LIBRARY NAMES lonejson)
  endif()
endif()
if(NOT CAI_LONEJSON_INCLUDE_DIR OR
   NOT EXISTS "${CAI_LONEJSON_INCLUDE_DIR}/lonejson.h")
  message(FATAL_ERROR
    "Failed to find lonejson.h. Use CAI_DEPENDENCY_MODE=cpkt or set "
    "CAI_LONEJSON_INCLUDE_DIR to an official installed lonejson include path.")
endif()
if(NOT CAI_PSLOG_INCLUDE_DIR OR NOT EXISTS "${CAI_PSLOG_INCLUDE_DIR}/pslog.h")
  message(FATAL_ERROR
    "Failed to find pslog.h. Use CAI_DEPENDENCY_MODE=cpkt or set "
    "CAI_PSLOG_INCLUDE_DIR to an official installed pslog include path.")
endif()
if(NOT CAI_LONEJSON_LIBRARY)
  message(FATAL_ERROR
    "Failed to find liblonejson. Use CAI_DEPENDENCY_MODE=cpkt or set "
    "CAI_LONEJSON_LIBRARY to an official installed lonejson library.")
endif()
if(NOT CAI_LONEJSON_LINK)
  set(CAI_LONEJSON_LINK "${CAI_LONEJSON_LIBRARY}")
endif()
if(NOT CAI_PSLOG_LINK AND CAI_PSLOG_LIBRARY)
  set(CAI_PSLOG_LINK "${CAI_PSLOG_LIBRARY}")
endif()

set(CAI_SOURCES
  src/cai_agent.c
  src/cai_allocator.c
  src/cai_client.c
  src/cai_conversation.c
  src/cai_env.c
  src/cai_error.c
  src/cai_http.c
  src/cai_input_item.c
  src/cai_lj.c
  src/cai_log.c
  src/cai_mcp.c
  src/cai_models.c
  src/cai_response.c
  src/cai_source.c
  src/cai_stream.c
  src/cai_tool.c
  src/tools/exec.c
  src/tools/read.c
  src/tools/revgeo.c
  src/tools/searxng.c
  src/tools/todo.c)

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
        $<BUILD_INTERFACE:${CAI_LONEJSON_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_include_directories(${_cai_target} PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src"
      "${CAI_PSLOG_INCLUDE_DIR}")
    target_include_directories(${_cai_target} PRIVATE ${CURL_INCLUDE_DIRS}
      "${CURL_INCLUDE_DIR}")
    target_compile_definitions(${_cai_target} PRIVATE LONEJSON_WITH_CURL)
    cai_configure_c_target(${_cai_target})
  endif()
endforeach()
if(TARGET cai_shared)
  target_link_libraries(cai_shared PRIVATE CURL::libcurl Threads::Threads
    ${CAI_LONEJSON_LINK})
endif()
if(TARGET cai_static)
  target_link_libraries(cai_static INTERFACE
    $<BUILD_INTERFACE:CURL::libcurl>
    $<BUILD_INTERFACE:Threads::Threads>
    $<BUILD_INTERFACE:${CAI_LONEJSON_LINK}>
    $<INSTALL_INTERFACE:CURL::libcurl>
    $<INSTALL_INTERFACE:Threads::Threads>
    $<INSTALL_INTERFACE:lonejson::lonejson>)
endif()

set(_cai_internal_link_target "")
if(TARGET cai_static)
  set(_cai_internal_link_target cai_static)
elseif(TARGET cai_shared)
  set(_cai_internal_link_target cai_shared)
else()
  message(FATAL_ERROR "At least one of CAI_BUILD_STATIC or CAI_BUILD_SHARED must be enabled.")
endif()

if(CAI_BUILD_EXAMPLES)
  set(_cai_example_link_target "${_cai_internal_link_target}")
  set(_cai_examples
    basic_response:basic-response
    conversation_handles:conversation-handles
    history_export:history-export
    openrouter_response:openrouter-response
    searxng_search:searxng-search
    session_state:session-state
    smhi_weather:smhi-weather
    mcp_server:mcp-server
    streaming_text:streaming-text
    terminal_chat:terminal-chat
    mike_mind:mike-mind)
  foreach(_cai_example_spec IN LISTS _cai_examples)
    string(REPLACE ":" ";" _cai_example_parts "${_cai_example_spec}")
    list(GET _cai_example_parts 0 _cai_example)
    list(GET _cai_example_parts 1 _cai_example_dir)
    add_executable(cai_example_${_cai_example}
      "examples/${_cai_example_dir}/main.c")
    target_include_directories(cai_example_${_cai_example} PRIVATE
      "${CAI_PUBLIC_INCLUDE_DIR}"
      "${CAI_GENERATED_INCLUDE_DIR}"
      "${CMAKE_CURRENT_SOURCE_DIR}/src"
      "${CAI_LONEJSON_INCLUDE_DIR}")
    if(_cai_example STREQUAL "mcp_server")
      target_include_directories(cai_example_${_cai_example} PRIVATE
        "${CAI_PSLOG_INCLUDE_DIR}")
    endif()
    target_link_libraries(cai_example_${_cai_example} PRIVATE
      ${_cai_example_link_target})
    if(_cai_example_link_target STREQUAL "cai_static" OR
        _cai_example_link_target STREQUAL "cai_shared")
      target_link_libraries(cai_example_${_cai_example} PRIVATE CURL::libcurl
        Threads::Threads ${CAI_LONEJSON_LINK})
    endif()
    if(_cai_example STREQUAL "mcp_server")
      if(CAI_PSLOG_LINK)
        target_link_libraries(cai_example_${_cai_example} PRIVATE
          ${CAI_PSLOG_LINK})
      else()
        message(FATAL_ERROR
          "The mcp-server example requires libpslog. Install libpslog, set "
          "CAI_PSLOG_LIBRARY, or configure with CAI_BUILD_EXAMPLES=OFF.")
      endif()
    endif()
    cai_configure_c_target(cai_example_${_cai_example})
  endforeach()
endif()

if(CAI_BUILD_LUA)
  add_library(cai_lua MODULE lua/cai_lua.c)
  set_target_properties(cai_lua PROPERTIES
    OUTPUT_NAME cai
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lua")
  target_include_directories(cai_lua PRIVATE
    "${CAI_PUBLIC_INCLUDE_DIR}"
    "${CAI_GENERATED_INCLUDE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CAI_LONEJSON_INCLUDE_DIR}"
    "${CAI_PSLOG_INCLUDE_DIR}"
    "${CAI_LUA_INCLUDE_DIR}")
  target_link_libraries(cai_lua PRIVATE ${_cai_internal_link_target} CURL::libcurl
    Threads::Threads ${CAI_LONEJSON_LINK})
  if(APPLE)
    target_link_options(cai_lua PRIVATE "-undefined" "dynamic_lookup")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(cai_lua PRIVATE "-Wl,--allow-shlib-undefined")
  else()
    target_link_libraries(cai_lua PRIVATE "${CAI_LUA_LIBRARY}")
  endif()
  cai_configure_c_target(cai_lua)
endif()

if(CAI_BUILD_TESTS)
  enable_testing()
  add_executable(cai_tests tests/test_main.c)
    target_include_directories(cai_tests PRIVATE
    "${CAI_PUBLIC_INCLUDE_DIR}"
    "${CAI_GENERATED_INCLUDE_DIR}"
    "${CAI_LONEJSON_INCLUDE_DIR}"
    "${CAI_PSLOG_INCLUDE_DIR}"
    ${CURL_INCLUDE_DIRS}
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(cai_tests PRIVATE ${_cai_internal_link_target} CURL::libcurl
    Threads::Threads ${CAI_LONEJSON_LINK})
  cai_configure_c_target(cai_tests)
  set(CAI_UNIT_TEST_GROUPS
    model_capabilities
    env_precedence
    source_sink
    lonejson_nested_mapped_array_stream
    lonejson_selected_array_rewrite
    tool_registry
    mcp_handler
    client_open
    mike_mind_prompt_contract
    response_json
    response_spooled_request_arrays
    response_array_serialization_invariants
    response_large_text_parse
    http_create_response
    http_retrieve_response
    http_cancel_response
    http_delete_response
    http_count_response_input_tokens
    http_list_response_input_items
    http_mock_incomplete_request_timeout
    http_error_details
    agent_session
    agent_client_history_continuity
    agent_tool_declarations
    agent_tool_manual_step
    agent_auto_compaction
    http_response_limit
    agent_tool_auto_run
    agent_client_history_tool_auto_run
    revgeo_tool
    todo_tool
    todo_callback_store
    searxng_registry_tool
    exec_tool
    read_tool
    agent_searxng_tool_auto_run
    agent_multi_tool_auto_run
    agent_tool_auto_round_limit
    agent_tool_output_max_bytes
    conversations
    stream_response_text
    stream_non_function_output_item
    stream_output_delta_failure
    stream_output_suffix_segments
    stream_http_error_preserves_openai_error
    stream_source_error_preserves_openai_error
    stream_openrouter_metadata_events
    session_stream_auto_tool_run
    session_stream_auto_source_tool_run
    session_stream_auto_reasoning_tool_response
    session_stream_auto_multi_tool_run
    session_stream_auto_duplicate_tool_done
    session_stream_auto_callback_failure
    session_stream_auto_round_limit
    session_stream_auto_tool_output_max_bytes
    stream_sse_event_limit
    stream_large_instructions_field
    stream_large_content_part_done_field
    stream_history_preserves_pretty_json
    stream_client_history_captures_output
    stream_client_history_tool_order
    local_history_opt_in
    session_resume_and_history_import
    session_state_validation)
  foreach(_cai_unit_test IN LISTS CAI_UNIT_TEST_GROUPS)
    add_test(NAME cai_tests.${_cai_unit_test}
      COMMAND cai_tests --only ${_cai_unit_test})
    set_tests_properties(cai_tests.${_cai_unit_test} PROPERTIES
      LABELS "unit;local"
      TIMEOUT 3)
  endforeach()

  add_executable(cai_mcp_http_server tests/mcp_http_server.c)
  target_include_directories(cai_mcp_http_server PRIVATE
    "${CAI_PUBLIC_INCLUDE_DIR}"
    "${CAI_GENERATED_INCLUDE_DIR}"
    "${CAI_LONEJSON_INCLUDE_DIR}")
  target_link_libraries(cai_mcp_http_server PRIVATE ${_cai_internal_link_target} CURL::libcurl
    Threads::Threads ${CAI_LONEJSON_LINK})
  cai_configure_c_target(cai_mcp_http_server)

  add_executable(cai_mcp_http_smoke tests/mcp_http_smoke.c)
  cai_configure_c_target(cai_mcp_http_smoke)
  add_test(NAME cai_mcp_http_smoke
    COMMAND cai_mcp_http_smoke $<TARGET_FILE:cai_mcp_http_server>)
  set_tests_properties(cai_mcp_http_smoke PROPERTIES
    LABELS "smoke;local"
    TIMEOUT 3)
  add_executable(cai_mcp_example_smoke tests/mcp_example_smoke.c)
  cai_configure_c_target(cai_mcp_example_smoke)
  if(TARGET cai_example_mcp_server)
    add_test(NAME cai_mcp_example_smoke
      COMMAND cai_mcp_example_smoke $<TARGET_FILE:cai_example_mcp_server>)
    set_tests_properties(cai_mcp_example_smoke PROPERTIES
      LABELS "smoke;local"
      TIMEOUT 3)
  endif()
  add_test(NAME cai_mcp_inspector_e2e
    COMMAND /bin/sh
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_inspector_e2e.sh"
      $<TARGET_FILE:cai_mcp_http_server>)
  set_tests_properties(cai_mcp_inspector_e2e PROPERTIES
    LABELS "smoke;local"
    TIMEOUT 3)

  if(CAI_INSTALL)
    add_test(NAME cai_install_metadata_test
      COMMAND "${CMAKE_COMMAND}"
        "-DCAI_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        "-DCAI_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DCAI_VERSION=${PROJECT_VERSION}"
        "-DCAI_C_PKT_SYSTEMS_PREFIX=${CAI_C_PKT_SYSTEMS_PREFIX}"
        "-DCAI_LONEJSON_PREFIX=${CAI_LONEJSON_PREFIX}"
        "-DCAI_PSLOG_PREFIX=${CAI_PSLOG_PREFIX}"
        "-DCAI_RESOLVED_DEPENDENCY_MODE=${CAI_RESOLVED_DEPENDENCY_MODE}"
        "-DCAI_DEPS_DIR=${CAI_DEPS_DIR}"
        "-DCAI_LONEJSON_INCLUDE_DIR=${CAI_LONEJSON_INCLUDE_DIR}"
        "-DCAI_LONEJSON_LIBRARY=${CAI_LONEJSON_LIBRARY}"
        "-DCAI_PSLOG_INCLUDE_DIR=${CAI_PSLOG_INCLUDE_DIR}"
        "-DCAI_PARENT_CMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
        "-DCAI_GENERATOR=${CMAKE_GENERATOR}"
        "-DCAI_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
        "-DCAI_TARGET_ID=${CAI_TARGET_ID}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/install_metadata_test.cmake")
    set_tests_properties(cai_install_metadata_test PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 10)
    add_test(NAME cai_host_dependency_mode_unsupported_target
      COMMAND "${CMAKE_COMMAND}"
        "-DCAI_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DCAI_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        "-DCAI_DEPS_DIR=${CAI_DEPS_DIR}"
        "-DCAI_C_PKT_SYSTEMS_PREFIX=${CAI_C_PKT_SYSTEMS_PREFIX}"
        "-DCAI_LONEJSON_PREFIX=${CAI_LONEJSON_PREFIX}"
        "-DCAI_PSLOG_PREFIX=${CAI_PSLOG_PREFIX}"
        "-DCAI_GENERATOR=${CMAKE_GENERATOR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/host_dependency_mode_test.cmake")
    set_tests_properties(cai_host_dependency_mode_unsupported_target PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 3)
    add_test(NAME cai_release_lua_version_test
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_lua_version_test.sh"
        "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(cai_release_lua_version_test PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 3)
  endif()

  if(CAI_BUILD_LUA)
    add_test(NAME cai_lua_tests
      COMMAND "${CAI_LUA_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/test_lua.lua")
    set_tests_properties(cai_lua_tests PROPERTIES
      LABELS "unit;local"
      ENVIRONMENT "${CAI_LUA_TEST_ENV}"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT 3)
    add_test(NAME cai_lua_mcp_todo_e2e
      COMMAND "${CAI_LUA_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/e2e_mcp_todo.lua")
    set_tests_properties(cai_lua_mcp_todo_e2e PROPERTIES
      LABELS "smoke;local"
      ENVIRONMENT "${CAI_LUA_TEST_ENV}"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT 3)
  endif()

  if(CAI_BUILD_INTEGRATION_TESTS)
    set(CAI_INTEGRATION_TIMEOUT_SHORT 10)
    set(CAI_INTEGRATION_TIMEOUT_MEDIUM 30)
    set(CAI_INTEGRATION_TIMEOUT_LONG 120)
    add_executable(cai_integration_tests tests/integration_main.c)
    target_include_directories(cai_integration_tests PRIVATE
      "${CAI_PUBLIC_INCLUDE_DIR}"
      "${CAI_GENERATED_INCLUDE_DIR}"
      "${CAI_LONEJSON_INCLUDE_DIR}"
      "${CAI_PSLOG_INCLUDE_DIR}"
      ${CURL_INCLUDE_DIRS})
    target_link_libraries(cai_integration_tests PRIVATE ${_cai_internal_link_target} CURL::libcurl
      Threads::Threads ${CAI_LONEJSON_LINK})
    cai_configure_c_target(cai_integration_tests)
    add_test(NAME cai_integration_tests COMMAND cai_integration_tests)
    set_tests_properties(cai_integration_tests PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_SHORT})
    add_test(NAME cai_integration_todo_workflow
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_TODO_WORKFLOW=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_todo_workflow PROPERTIES
      LABELS "integration;offline"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_SHORT})
    add_test(NAME cai_integration_openai_e2e
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_E2E=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_openai_e2e PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_LONG})
    add_test(NAME cai_integration_state_restore
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_STATE_RESTORE=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_state_restore PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_compaction
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_COMPACTION=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_compaction PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_tool_security
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_TOOL_SECURITY=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_tool_security PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_hosted_web_search
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_HOSTED_WEB_SEARCH=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_hosted_web_search PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_exec_tool
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_EXEC_TOOL=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_exec_tool PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_read_tool
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_READ_TOOL=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_read_tool PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_openrouter_basic
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_OPENROUTER=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_openrouter_basic PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_openrouter_e2e
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_OPENROUTER_E2E=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_openrouter_e2e PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_LONG})
    add_test(NAME cai_integration_openrouter_session
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_OPENROUTER_SESSION=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_openrouter_session PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_openrouter_tool
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_OPENROUTER_TOOL=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_openrouter_tool PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_openrouter_stream_tool
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_OPENROUTER_STREAM_TOOL=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_openrouter_stream_tool PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_openrouter_read_tool
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_OPENROUTER_READ_TOOL=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_openrouter_read_tool PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_openrouter_stream_history
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_OPENROUTER_STREAM_HISTORY=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_openrouter_stream_history PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_openrouter_tool_security
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_OPENROUTER_TOOL_SECURITY=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_openrouter_tool_security PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    if(CAI_BUILD_LUA)
      add_test(NAME cai_lua_tool_stream_e2e
        COMMAND "${CAI_LUA_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/e2e_tool_stream.lua")
      set_tests_properties(cai_lua_tool_stream_e2e PROPERTIES
        LABELS integration
        ENVIRONMENT "${CAI_LUA_TEST_ENV}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
      add_test(NAME cai_lua_session_continuity_e2e
        COMMAND "${CAI_LUA_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/e2e_session_continuity.lua")
      set_tests_properties(cai_lua_session_continuity_e2e PROPERTIES
        LABELS integration
        ENVIRONMENT "${CAI_LUA_TEST_ENV}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
      add_test(NAME cai_lua_hosted_web_search_e2e
        COMMAND "${CAI_LUA_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/e2e_hosted_web_search.lua")
      set_tests_properties(cai_lua_hosted_web_search_e2e PROPERTIES
        LABELS integration
        ENVIRONMENT "${CAI_LUA_TEST_ENV}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    endif()
  endif()
endif()

if(CAI_BUILD_FUZZERS)
  if(NOT TARGET cai_static)
    message(FATAL_ERROR "CAI_BUILD_FUZZERS requires CAI_BUILD_STATIC=ON.")
  endif()
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "CAI_BUILD_FUZZERS currently requires Clang/libFuzzer.")
  endif()
  add_executable(cai_tool_fuzz tests/tool_fuzz.c)
  target_include_directories(cai_tool_fuzz PRIVATE
    "${CAI_PUBLIC_INCLUDE_DIR}"
    "${CAI_GENERATED_INCLUDE_DIR}"
    "${CAI_LONEJSON_INCLUDE_DIR}"
    ${CURL_INCLUDE_DIRS})
  target_link_libraries(cai_tool_fuzz PRIVATE cai_static CURL::libcurl
    Threads::Threads ${CAI_LONEJSON_LINK})
  cai_configure_c_target(cai_tool_fuzz)
  target_compile_options(cai_tool_fuzz PRIVATE -fsanitize=address,undefined)
  target_link_options(cai_tool_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

find_program(CLANG_FORMAT_BIN NAMES clang-format)
file(GLOB_RECURSE CAI_FORMAT_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/lua/*.c"
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
  set(CAI_INSTALL_CMAKEDIR
      "${CMAKE_INSTALL_LIBDIR}/cmake/cai"
      CACHE STRING "Install path for cai CMake package files.")
  set(CAI_PACKAGE_REQUIRES "lonejson")
  if(CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "cpkt")
    set(CAI_PACKAGE_REQUIRES_PRIVATE "libcurl")
    set(CAI_PC_DEPENDENCY_CFLAGS "")
    set(CAI_PC_DEPENDENCY_LIBS_PRIVATE "")
    set(CAI_CONFIG_LONEJSON_INCLUDE_HINT "")
    set(CAI_CONFIG_PSLOG_INCLUDE_HINT "")
    set(CAI_CONFIG_LONEJSON_LIBRARY_HINT "")
  else()
    set(CAI_PACKAGE_REQUIRES_PRIVATE "libcurl")
    get_filename_component(CAI_CONFIG_LONEJSON_LIBRARY_HINT
      "${CAI_LONEJSON_LIBRARY}" DIRECTORY)
    set(CAI_PC_DEPENDENCY_CFLAGS "")
    set(CAI_PC_DEPENDENCY_LIBS_PRIVATE "")
    set(CAI_CONFIG_LONEJSON_INCLUDE_HINT "${CAI_LONEJSON_INCLUDE_DIR}")
    set(CAI_CONFIG_PSLOG_INCLUDE_HINT "${CAI_PSLOG_INCLUDE_DIR}")
  endif()
  configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cai-config.cmake"
    INSTALL_DESTINATION "${CAI_INSTALL_CMAKEDIR}")
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/cai-config-version.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion)
  set(CAI_PC_PREFIX_FROM_PKGCONFIG "..")
  string(REPLACE "/" ";" _cai_pc_libdir_parts "${CMAKE_INSTALL_LIBDIR}")
  foreach(_cai_pc_libdir_part IN LISTS _cai_pc_libdir_parts)
    if(NOT _cai_pc_libdir_part STREQUAL "")
      string(APPEND CAI_PC_PREFIX_FROM_PKGCONFIG "/..")
    endif()
  endforeach()
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cai.pc"
    @ONLY)

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
  install(EXPORT caiTargets
    NAMESPACE cai::
    FILE cai-targets.cmake
    DESTINATION "${CAI_INSTALL_CMAKEDIR}")
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/cai-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/cai-config-version.cmake"
    DESTINATION "${CAI_INSTALL_CMAKEDIR}")
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cai.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
  install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/libcai")

  set(_cai_package_deps "")
  if(TARGET cai_static)
    list(APPEND _cai_package_deps cai_static)
  endif()
  if(TARGET cai_shared)
    list(APPEND _cai_package_deps cai_shared)
  endif()
  add_custom_target(cai_package_archive
    COMMAND "${CMAKE_COMMAND}"
      "-DCAI_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DCAI_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
      "-DCAI_DIST_DIR=${CAI_DIST_DIR}"
      "-DCAI_VERSION=${PROJECT_VERSION}"
      "-DCAI_TARGET_ID=${CAI_TARGET_ID}"
      "-DCAI_STRIP_BIN=${CMAKE_STRIP}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/package_archive.cmake"
    DEPENDS ${_cai_package_deps}
    VERBATIM)
  add_custom_target(cai_package_source
    COMMAND "${CMAKE_COMMAND}"
      "-DCAI_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DCAI_DIST_DIR=${CAI_DIST_DIR}"
      "-DCAI_VERSION=${PROJECT_VERSION}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/package_source.cmake"
    VERBATIM)
  add_custom_target(cai_package_checksums
    COMMAND "${CMAKE_COMMAND}"
      "-DCAI_DIST_DIR=${CAI_DIST_DIR}"
      "-DCAI_VERSION=${PROJECT_VERSION}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/package_checksums.cmake"
    DEPENDS cai_package_archive cai_package_source
    VERBATIM)
endif()
