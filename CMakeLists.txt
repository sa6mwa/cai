cmake_minimum_required(VERSION 3.21)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CaiVersion)
include(CaiLonejsonAbi)
include(cai-rpath)

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
set(CAI_LONEJSON_VERSION "0.31.0")
set(CAI_LONEJSON_ABI_VERSION "16")
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
    set(_sha256 "f3f1de0f04b4d9491dacc856de38cd1ecde488fb8befc539bf615ac4d9490b54")
  elseif(target_id STREQUAL "x86_64-linux-musl")
    set(_sha256 "967c797ca17040960ee0fc3339abda040ab3f52c6f9b8d79e729284f433ad1af")
  elseif(target_id STREQUAL "aarch64-linux-gnu")
    set(_sha256 "1594b8da68a5acda24addcad19a65f4001a75b2fac26a86e8455229352732ca8")
  elseif(target_id STREQUAL "aarch64-linux-musl")
    set(_sha256 "81f045090cb81b727bc390cfcc9bf46b9ee8a0ed80fcf75fd1b9cdced3f787e1")
  elseif(target_id STREQUAL "armhf-linux-gnu")
    set(_sha256 "34d03906e00888a2aa9fb01073668b398c271faef01e9f70232523bf0b4be481")
  elseif(target_id STREQUAL "armhf-linux-musl")
    set(_sha256 "c7fbeda5ba8318c01fda0fabe65903f8dce39d6f96564afa7efd1aca80230644")
  elseif(target_id STREQUAL "arm64-apple-darwin")
    set(_sha256 "b833e8b2f385294ba085d12dd5d576dd2d8ac271abcbabdf36c81237b27fa14a")
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
  set(_cai_auto_lonejson_abi_ok FALSE)
  if(_cai_auto_lonejson_library)
    cai_probe_lonejson_library_abi(
      "${_cai_auto_lonejson_library}" "${CAI_LONEJSON_ABI_VERSION}"
      _cai_auto_lonejson_abi_ok _cai_auto_lonejson_abi_reason)
  endif()
  if(CURL_FOUND AND _cai_auto_lonejson_include AND _cai_auto_pslog_include AND
     _cai_auto_lonejson_library AND _cai_auto_pslog_library AND
     _cai_auto_lonejson_abi_ok)
    set(CAI_RESOLVED_DEPENDENCY_MODE "host")
  else()
    set(CAI_RESOLVED_DEPENDENCY_MODE "cpkt")
  endif()
  unset(_cai_auto_lonejson_include CACHE)
  unset(_cai_auto_pslog_include CACHE)
  unset(_cai_auto_lonejson_library CACHE)
  unset(_cai_auto_pslog_library CACHE)
  unset(_cai_auto_lonejson_abi_ok)
  unset(_cai_auto_lonejson_abi_reason)
endif()
if(CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "cpkt" AND
   CAI_TARGET_ID STREQUAL "x86_64-apple-darwin")
  message(WARNING
    "No pinned cpkt dependency package exists for x86_64-apple-darwin; "
    "falling back to host dependencies. Use arm64-apple-darwin for cpkt "
    "Darwin release builds.")
  set(CAI_RESOLVED_DEPENDENCY_MODE "host")
endif()

cai_install_rpath_token("${CAI_TARGET_ID}" CAI_INSTALL_RPATH_TOKEN)

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
  cai_get_imported_target_location(lonejson::lonejson _cai_lonejson_library)
  if(NOT CAI_LONEJSON_INCLUDE_DIR AND _cai_lonejson_include)
    set(CAI_LONEJSON_INCLUDE_DIR "${_cai_lonejson_include}" PARENT_SCOPE)
  endif()
  if(NOT CAI_LONEJSON_LIBRARY AND _cai_lonejson_library)
    set(CAI_LONEJSON_LIBRARY "${_cai_lonejson_library}" PARENT_SCOPE)
    if(NOT CAI_LONEJSON_LINK)
      set(CAI_LONEJSON_LINK lonejson::lonejson PARENT_SCOPE)
    endif()
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
  if(NOT CAI_PSLOG_INCLUDE_DIR AND _cai_pslog_include)
    set(CAI_PSLOG_INCLUDE_DIR "${_cai_pslog_include}" PARENT_SCOPE)
  endif()
  if(NOT CAI_PSLOG_LIBRARY AND NOT CAI_PSLOG_LINK)
    set(CAI_PSLOG_LINK "${_cai_pslog_link}" PARENT_SCOPE)
  endif()
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
  list(APPEND CMAKE_INSTALL_RPATH "${CAI_INSTALL_RPATH_TOKEN}")
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
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)

if(CAI_BUILD_LUA)
  find_program(CAI_LUA_EXECUTABLE NAMES lua5.5 lua REQUIRED)
elseif(CAI_BUILD_INTEGRATION_TESTS)
  find_program(CAI_LUA_EXECUTABLE NAMES lua5.5 lua)
endif()

if(CAI_BUILD_LUA)
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
if(NOT CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "cpkt")
  cai_validate_lonejson_library_abi(
    "${CAI_LONEJSON_LIBRARY}" "${CAI_LONEJSON_ABI_VERSION}"
    "cai host dependency check")
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
  src/cai_auth.c
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
  if(CAI_BUILD_TESTS)
    target_compile_definitions(cai_static PRIVATE CAI_TESTING)
  endif()
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
    OpenSSL::Crypto ${CAI_LONEJSON_LINK})
endif()
if(TARGET cai_static)
  target_link_libraries(cai_static INTERFACE
    $<BUILD_INTERFACE:CURL::libcurl>
    $<BUILD_INTERFACE:Threads::Threads>
    $<BUILD_INTERFACE:OpenSSL::Crypto>
    $<BUILD_INTERFACE:${CAI_LONEJSON_LINK}>
    $<INSTALL_INTERFACE:CURL::libcurl>
    $<INSTALL_INTERFACE:Threads::Threads>
    $<INSTALL_INTERFACE:OpenSSL::Crypto>
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
    chatgpt_login:chatgpt-login
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
  target_compile_definitions(cai_tests PRIVATE CAI_TESTING)
  target_link_libraries(cai_tests PRIVATE ${_cai_internal_link_target} CURL::libcurl
    Threads::Threads OpenSSL::Crypto ${CAI_LONEJSON_LINK})
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
    response_request_tools_cleanup
    response_spooled_request_arrays
    response_array_serialization_invariants
    response_large_text_parse
    response_large_instructions_parse
    response_large_tool_arguments_spooled
    http_create_response
    chatgpt_auth_refresh_retry
    chatgpt_auth_invalid_refresh_retryable
    chatgpt_auth_save_failure_preserves_file
    chatgpt_auth_expired_refreshes_before_request
    chatgpt_auth_reloads_changed_file_before_refresh
    chatgpt_auth_rejects_changed_file_account
    chatgpt_auth_stream_refresh_retry
    chatgpt_auth_default_path
    chatgpt_auth_open_default_path
    chatgpt_auth_client_defaults_to_codex_backend
    chatgpt_auth_agent_keeps_server_continuity
    chatgpt_login_authorize_url
    chatgpt_login_callback_validation
    chatgpt_login_callback_exchange
    chatgpt_login_default_path_write
    http_retrieve_response
    http_cancel_response
    http_delete_response
    http_count_response_input_tokens
    http_list_response_input_items
    usage_accounting
    usage_limits
    usage_limit_lanes
    usage_client_limits
    usage_spend_limit
    usage_spend_limit_requires_pricing
    usage_stream_limits
    usage_stream_missing_usage_limit
    http_mock_incomplete_request_timeout
    http_error_details
    agent_session
    session_spooled_input_failure_ownership
    agent_client_history_continuity
    agent_tool_declarations
    agent_tool_manual_step
    agent_auto_compaction
    http_response_limit
    agent_tool_auto_run
    agent_client_history_tool_auto_run
    revgeo_tool
    todo_file_store_initializes_under_lock
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
    stream_responses_websocket
    stream_responses_websocket_large_frame
    stream_responses_websocket_transient_retry
    stream_responses_websocket_midstream_drop
    session_stream_responses_websocket_multi_turn
    session_stream_responses_websocket_stale_reconnect
    stream_non_function_output_item
    stream_output_delta_failure
    stream_output_item_failure
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
  string(REPLACE ";" "\n" _cai_unit_test_groups_text
    "${CAI_UNIT_TEST_GROUPS}")
  file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/cai_unit_test_groups.txt"
    CONTENT "${_cai_unit_test_groups_text}\n")
  foreach(_cai_unit_test IN LISTS CAI_UNIT_TEST_GROUPS)
    add_test(NAME cai_tests.${_cai_unit_test}
      COMMAND cai_tests --only ${_cai_unit_test})
    set_tests_properties(cai_tests.${_cai_unit_test} PROPERTIES
      LABELS "unit;local"
      TIMEOUT 3)
  endforeach()
  set_tests_properties(cai_tests.lonejson_selected_array_rewrite PROPERTIES
    LABELS "unit;local;msan-external-boundary"
    TIMEOUT 3)
  set_tests_properties(
    cai_tests.model_capabilities
    cai_tests.env_precedence
    cai_tests.source_sink
    cai_tests.lonejson_nested_mapped_array_stream
    cai_tests.client_open
    cai_tests.mike_mind_prompt_contract
    PROPERTIES
      LABELS "unit;local;msan-smoke"
      TIMEOUT 3)
  set_tests_properties(cai_tests.mcp_handler PROPERTIES
    LABELS "unit;local;msan-external-boundary"
    TIMEOUT 3)
  set_tests_properties(cai_tests.response_json PROPERTIES
    LABELS "unit;local;msan-external-boundary"
    TIMEOUT 3)
  set_tests_properties(cai_tests.response_spooled_request_arrays PROPERTIES
    LABELS "unit;local;msan-external-boundary"
    TIMEOUT 3)
  set_tests_properties(cai_tests.response_array_serialization_invariants PROPERTIES
    LABELS "unit;local;msan-external-boundary"
    TIMEOUT 3)
  set_tests_properties(cai_tests.tool_registry PROPERTIES
    LABELS "unit;local;msan-external-boundary"
    TIMEOUT 3)
  add_test(NAME cai_tests.registration
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit_test_registration_test.sh"
      $<TARGET_FILE:cai_tests>
      "${CMAKE_CURRENT_BINARY_DIR}/cai_unit_test_groups.txt")
  set_tests_properties(cai_tests.registration PROPERTIES
    LABELS "unit;local"
    TIMEOUT 3)
  add_test(NAME cai_tests.registration_script
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit_test_registration_script_test.sh"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit_test_registration_test.sh")
  set_tests_properties(cai_tests.registration_script PROPERTIES
    LABELS "unit;local"
    TIMEOUT 3)

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
  add_test(NAME cai_examples_help_smoke
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/examples_help_smoke.sh"
      "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(cai_examples_help_smoke PROPERTIES
    LABELS "smoke;local;example-smoke"
    TIMEOUT 3)
  add_executable(cai_mcp_example_smoke tests/mcp_example_smoke.c)
  cai_configure_c_target(cai_mcp_example_smoke)
  if(TARGET cai_example_mcp_server)
    add_test(NAME cai_mcp_example_smoke
      COMMAND cai_mcp_example_smoke $<TARGET_FILE:cai_example_mcp_server>)
    set_tests_properties(cai_mcp_example_smoke PROPERTIES
      LABELS "smoke;local;example-smoke"
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
        "-DCAI_BUILD_SHARED=${CAI_BUILD_SHARED}"
        "-DCAI_BUILD_STATIC=${CAI_BUILD_STATIC}"
        "-DCAI_C_PKT_SYSTEMS_PREFIX=${CAI_C_PKT_SYSTEMS_PREFIX}"
        "-DCAI_LONEJSON_PREFIX=${CAI_LONEJSON_PREFIX}"
        "-DCAI_PSLOG_PREFIX=${CAI_PSLOG_PREFIX}"
        "-DCAI_RESOLVED_DEPENDENCY_MODE=${CAI_RESOLVED_DEPENDENCY_MODE}"
        "-DCAI_DEPS_DIR=${CAI_DEPS_DIR}"
        "-DCAI_LONEJSON_INCLUDE_DIR=${CAI_LONEJSON_INCLUDE_DIR}"
        "-DCAI_LONEJSON_LIBRARY=${CAI_LONEJSON_LIBRARY}"
        "-DCAI_PSLOG_INCLUDE_DIR=${CAI_PSLOG_INCLUDE_DIR}"
        "-DCAI_PSLOG_LIBRARY=${CAI_PSLOG_LIBRARY}"
        "-DCAI_PARENT_CMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
        "-DCAI_GENERATOR=${CMAKE_GENERATOR}"
        "-DCAI_C_COMPILER=${CMAKE_C_COMPILER}"
        "-DCAI_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
        "-DCAI_TARGET_ID=${CAI_TARGET_ID}"
        "-DCAI_INSTALL_RPATH_TOKEN=${CAI_INSTALL_RPATH_TOKEN}"
        "-DCAI_LONEJSON_ABI_VERSION=${CAI_LONEJSON_ABI_VERSION}"
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
        "-DCAI_LONEJSON_INCLUDE_DIR=${CAI_LONEJSON_INCLUDE_DIR}"
        "-DCAI_LONEJSON_LIBRARY=${CAI_LONEJSON_LIBRARY}"
        "-DCAI_PSLOG_INCLUDE_DIR=${CAI_PSLOG_INCLUDE_DIR}"
        "-DCAI_PSLOG_LIBRARY=${CAI_PSLOG_LIBRARY}"
        "-DCAI_PARENT_CMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
        "-DCAI_GENERATOR=${CMAKE_GENERATOR}"
        "-DCAI_C_COMPILER=${CMAKE_C_COMPILER}"
        "-DCAI_LONEJSON_ABI_VERSION=${CAI_LONEJSON_ABI_VERSION}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/host_dependency_mode_test.cmake")
    set_tests_properties(cai_host_dependency_mode_unsupported_target PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 3)
    add_test(NAME cai_rpath_token_test
      COMMAND "${CMAKE_COMMAND}"
        "-DCAI_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/rpath_token_test.cmake")
    set_tests_properties(cai_rpath_token_test PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 3)
    add_test(NAME cai_cmake_presets_test
      COMMAND "${CMAKE_COMMAND}"
        "-DCAI_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake_presets_test.cmake")
    set_tests_properties(cai_cmake_presets_test PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 3)
    add_test(NAME cai_c_integration_config_without_lua
      COMMAND "${CMAKE_COMMAND}"
        -S "${CMAKE_CURRENT_SOURCE_DIR}"
        -B "${CMAKE_CURRENT_BINARY_DIR}/c-integration-without-lua"
        "-G${CMAKE_GENERATOR}"
        "-DCAI_BUILD_INTEGRATION_TESTS=ON"
        "-DCAI_BUILD_LUA=OFF"
        "-DCAI_BUILD_EXAMPLES=OFF"
        "-DCAI_BUILD_FUZZERS=OFF"
        "-DCAI_BUILD_TESTS=ON"
        "-DCAI_INSTALL=OFF"
        "-DCAI_DEPENDENCY_MODE=${CAI_RESOLVED_DEPENDENCY_MODE}"
        "-DCAI_DEPS_DIR=${CAI_DEPS_DIR}"
        "-DCAI_C_PKT_SYSTEMS_PREFIX=${CAI_C_PKT_SYSTEMS_PREFIX}"
        "-DCAI_LONEJSON_PREFIX=${CAI_LONEJSON_PREFIX}"
        "-DCAI_PSLOG_PREFIX=${CAI_PSLOG_PREFIX}"
        "-DCAI_LONEJSON_INCLUDE_DIR=${CAI_LONEJSON_INCLUDE_DIR}"
        "-DCAI_LONEJSON_LIBRARY=${CAI_LONEJSON_LIBRARY}"
        "-DCAI_PSLOG_INCLUDE_DIR=${CAI_PSLOG_INCLUDE_DIR}"
        "-DCAI_PSLOG_LIBRARY=${CAI_PSLOG_LIBRARY}"
        "-DCAI_LUA_EXECUTABLE=CAI_LUA_EXECUTABLE-NOTFOUND"
        "-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
        "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
        "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
    set_tests_properties(cai_c_integration_config_without_lua PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 10)
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
    set(CAI_INTEGRATION_TIMEOUT_LONG 240)
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
    add_test(NAME cai_integration_usage_limits
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_USAGE_LIMITS=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_usage_limits PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_chatgpt_subscription_e2e
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_SUBSCRIPTION_E2E=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_subscription_e2e PROPERTIES
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
      add_test(NAME cai_lua_usage_limits_e2e
        COMMAND "${CMAKE_COMMAND}" -E env
          "CAI_LUA_USAGE_LIMITS_E2E=1"
          "${CAI_LUA_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/e2e_usage_limits.lua")
      set_tests_properties(cai_lua_usage_limits_e2e PROPERTIES
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
    if(TARGET cai_example_basic_response AND
        TARGET cai_example_streaming_text AND
        TARGET cai_example_history_export AND
        TARGET cai_example_session_state AND
        TARGET cai_example_terminal_chat AND
        CAI_LUA_EXECUTABLE)
      add_test(NAME cai_examples_live_smoke
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/examples_live_smoke.sh"
          "${CMAKE_CURRENT_SOURCE_DIR}"
          "$<TARGET_FILE:cai_example_basic_response>"
          "$<TARGET_FILE:cai_example_streaming_text>"
          "$<TARGET_FILE:cai_example_history_export>"
          "$<TARGET_FILE:cai_example_session_state>"
          "$<TARGET_FILE:cai_example_terminal_chat>"
          "${CAI_LUA_EXECUTABLE}")
      set_tests_properties(cai_examples_live_smoke PROPERTIES
        LABELS "integration;example-smoke"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        TIMEOUT ${CAI_INTEGRATION_TIMEOUT_LONG})
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
  function(cai_add_fuzzer target source corpus_relpath)
    add_executable(${target} ${source})
    target_include_directories(${target} PRIVATE
      "${CAI_PUBLIC_INCLUDE_DIR}"
      "${CAI_GENERATED_INCLUDE_DIR}"
      "${CAI_LONEJSON_INCLUDE_DIR}"
      "${CAI_PSLOG_INCLUDE_DIR}"
      ${CURL_INCLUDE_DIRS}
      "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(${target} PRIVATE cai_static CURL::libcurl
      Threads::Threads ${CAI_LONEJSON_LINK})
    cai_configure_c_target(${target})
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined)
    target_link_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined)
    set(corpus_dir "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz-corpus/${corpus_relpath}")
    add_test(NAME ${target}.smoke
      COMMAND $<TARGET_FILE:${target}> "${corpus_dir}" -runs=1)
    set_tests_properties(${target}.smoke PROPERTIES
      LABELS "fuzz;local"
      TIMEOUT 3)
  endfunction()

  cai_add_fuzzer(cai_tool_fuzz tests/tool_fuzz.c tool)
  cai_add_fuzzer(cai_stream_fuzz tests/stream_fuzz.c stream)
  cai_add_fuzzer(cai_response_fuzz tests/response_fuzz.c response)
  cai_add_fuzzer(cai_mcp_fuzz tests/mcp_fuzz.c mcp)
  cai_add_fuzzer(cai_session_fuzz tests/session_fuzz.c session)
  cai_add_fuzzer(cai_todo_fuzz tests/todo_fuzz.c todo)
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
    set(CAI_PACKAGE_REQUIRES_PRIVATE "libcurl openssl")
    set(CAI_PC_DEPENDENCY_CFLAGS "")
    set(CAI_PC_DEPENDENCY_LIBS_PRIVATE "")
    set(CAI_CONFIG_LONEJSON_INCLUDE_HINT "")
    set(CAI_CONFIG_PSLOG_INCLUDE_HINT "")
    set(CAI_CONFIG_LONEJSON_LIBRARY_HINT "")
  else()
    set(CAI_PACKAGE_REQUIRES_PRIVATE "libcurl openssl")
    get_filename_component(CAI_CONFIG_LONEJSON_LIBRARY_HINT
      "${CAI_LONEJSON_LIBRARY}" DIRECTORY)
    set(CAI_PC_DEPENDENCY_CFLAGS "")
    set(CAI_PC_DEPENDENCY_LIBS_PRIVATE "")
    set(CAI_CONFIG_LONEJSON_INCLUDE_HINT "${CAI_LONEJSON_INCLUDE_DIR}")
    set(CAI_CONFIG_PSLOG_INCLUDE_HINT "${CAI_PSLOG_INCLUDE_DIR}")
  endif()
  set(CAI_LONEJSON_PKG_SHA256_FOR_PACKAGE "${CAI_LONEJSON_PKG_SHA256}")
  if(NOT DEFINED CAI_LONEJSON_PKG_SHA256_FOR_PACKAGE OR
     CAI_LONEJSON_PKG_SHA256_FOR_PACKAGE STREQUAL "")
    set(CAI_LONEJSON_PKG_SHA256_FOR_PACKAGE "host")
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
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CaiLonejsonAbi.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/cai-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/cai-config-version.cmake"
    DESTINATION "${CAI_INSTALL_CMAKEDIR}")
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cai.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
  install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/libcai")
  install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/model-metadata.md"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/libcai/docs")

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
