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
option(CAI_BUILD_FUZZERS "Build fuzz harnesses." OFF)
option(CAI_ENABLE_COVERAGE "Build tests with coverage instrumentation." OFF)
option(CAI_INSTALL "Enable install rules." ON)
option(CAI_USE_LOCKDC_SDK "Deprecated alias for CAI_DEPENDENCY_MODE=pkt." OFF)

set(CAI_VERSION_OVERRIDE "" CACHE STRING "Override detected cai package version.")
set(CAI_DIST_DIR "${CMAKE_BINARY_DIR}/dist" CACHE PATH "Distribution output directory.")
set(CAI_TARGET_ID "" CACHE STRING "Release archive target id.")
set(CAI_DEPS_DIR "${CMAKE_SOURCE_DIR}/.cache/deps"
    CACHE PATH "Downloaded third-party dependency cache.")
set(CAI_DEPENDENCY_MODE "pkt" CACHE STRING
    "Dependency mode: pkt, host, or auto.")
set_property(CACHE CAI_DEPENDENCY_MODE PROPERTY STRINGS pkt host auto)
set(CAI_C_PKT_SYSTEMS_VERSION "0.1.0" CACHE STRING
    "Pinned c.pkt.systems release used for curl/native dependencies." FORCE)
set(CAI_LONEJSON_VERSION "0.8.0")
set(CAI_LONEJSON_SHA256
    "f1fd6468192a363d8f147e4f3909ab115012a2fc2443111cc421d489c7bfa614")
set(CAI_LONEJSON_URL
    "https://github.com/sa6mwa/lonejson/releases/download/v${CAI_LONEJSON_VERSION}/lonejson-${CAI_LONEJSON_VERSION}.h.gz")
set(CAI_PSLOG_VERSION "0.3.1")
set(CAI_PSLOG_SHA256
    "3fd34c48c7851692e7a851714c1af8b2fb880c2858b4c4c45c3424a45c77b094")
set(CAI_PSLOG_URL
    "https://github.com/sa6mwa/libpslog/releases/download/v${CAI_PSLOG_VERSION}/pslog-${CAI_PSLOG_VERSION}.h.gz")

if(NOT CAI_BUILD_STATIC AND NOT CAI_BUILD_SHARED)
  message(FATAL_ERROR "Enable at least one of CAI_BUILD_STATIC or CAI_BUILD_SHARED.")
endif()

if(CAI_USE_LOCKDC_SDK)
  message(WARNING
    "CAI_USE_LOCKDC_SDK is deprecated; using CAI_DEPENDENCY_MODE=pkt.")
  set(CAI_DEPENDENCY_MODE "pkt" CACHE STRING
      "Dependency mode: pkt, host, or auto." FORCE)
endif()
if(CAI_DEPENDENCY_MODE STREQUAL "lockdc")
  message(WARNING
    "CAI_DEPENDENCY_MODE=lockdc is deprecated; use pkt. Treating it as pkt.")
  set(CAI_DEPENDENCY_MODE "pkt" CACHE STRING
      "Dependency mode: pkt, host, or auto." FORCE)
endif()
if(NOT CAI_DEPENDENCY_MODE STREQUAL "pkt" AND
   NOT CAI_DEPENDENCY_MODE STREQUAL "host" AND
   NOT CAI_DEPENDENCY_MODE STREQUAL "auto")
  message(FATAL_ERROR
    "CAI_DEPENDENCY_MODE must be one of: pkt, host, auto.")
endif()

if(CAI_TARGET_ID STREQUAL "")
  string(TOLOWER "${CMAKE_SYSTEM_NAME}" _cai_system_name_lower)
  string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _cai_processor_lower)
  if(_cai_system_name_lower STREQUAL "linux" AND
     (_cai_processor_lower STREQUAL "x86_64" OR
      _cai_processor_lower STREQUAL "amd64"))
    set(CAI_TARGET_ID "x86_64-linux-gnu")
  elseif(_cai_system_name_lower STREQUAL "darwin" AND
         (_cai_processor_lower STREQUAL "arm64" OR
          _cai_processor_lower STREQUAL "aarch64"))
    set(CAI_TARGET_ID "arm64-apple-darwin")
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
    set(_sha256 "bfce5ec3c0a918258649eac741704fe3786dfb7ba5c2a80c7b1bdc994ed7fe7e")
  elseif(target_id STREQUAL "x86_64-linux-musl")
    set(_sha256 "63b4215450e8ca5b4c5066c4662f992346dcbd34fd821f1894780243a30cd282")
  elseif(target_id STREQUAL "aarch64-linux-gnu")
    set(_sha256 "4ea196d1e22e4537e0f6f73814171ed1e3b487c110eae1b75147929a05d123ab")
  elseif(target_id STREQUAL "aarch64-linux-musl")
    set(_sha256 "d3065ed8ce89c4e40c6a4df0770d33cc83e75657a35e56c709aa2285c042eec6")
  elseif(target_id STREQUAL "armhf-linux-gnu")
    set(_sha256 "bd3d58e1a35ea8ff753b7412577eb2fd4602f47d21a0e8892ae8a11e8fc7b233")
  elseif(target_id STREQUAL "armhf-linux-musl")
    set(_sha256 "dc134dc2fd4b9805078348799367afc6c48b93e68781ad6bbd7963d39ad5552b")
  else()
    message(FATAL_ERROR
      "No pinned liblonejson package checksum for target ${target_id}")
  endif()
  set(${out_var} "${_sha256}" PARENT_SCOPE)
endfunction()

function(cai_pslog_pkg_sha256 target_id out_var)
  if(target_id STREQUAL "x86_64-linux-gnu")
    set(_sha256 "4113d490dfd0c7706a518d32cef5d8acfc6d8bae8b42488edb76fd64e55dd14d")
  elseif(target_id STREQUAL "x86_64-linux-musl")
    set(_sha256 "f71f325de984677594da1bea589ab154a14b363ad4d3fa6125443a6a8fb0e448")
  elseif(target_id STREQUAL "aarch64-linux-gnu")
    set(_sha256 "f47808e9d87cb3368888c40329f8370e6ebd1a4782f2709da135441cdf4d8239")
  elseif(target_id STREQUAL "aarch64-linux-musl")
    set(_sha256 "fdfac09bf98dcc0bfdec3bcd55478d7b9b1446ff85d14dbad2fd9cdad166a3cb")
  elseif(target_id STREQUAL "armhf-linux-gnu")
    set(_sha256 "57b22d018220754e8d2824df15faebca123103d20921b8b1bd73187f866bdeff")
  elseif(target_id STREQUAL "armhf-linux-musl")
    set(_sha256 "a2cd382af639c8cf923e317f21604e20c866e6cc00ac9d30b20dbabb39d7c489")
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
  find_package(CURL QUIET)
  if(CURL_FOUND AND _cai_auto_lonejson_include AND _cai_auto_pslog_include AND
     _cai_auto_lonejson_library)
    set(CAI_RESOLVED_DEPENDENCY_MODE "host")
  else()
    set(CAI_RESOLVED_DEPENDENCY_MODE "pkt")
  endif()
  unset(_cai_auto_lonejson_include CACHE)
  unset(_cai_auto_pslog_include CACHE)
  unset(_cai_auto_lonejson_library CACHE)
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

if(CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "pkt")
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
  if(CAI_PSLOG_PKG_NAME MATCHES "armhf|aarch64|x86_64")
    set(_cai_pslog_stamp "${CAI_PSLOG_PREFIX}/include/pslog.h")
  else()
    set(_cai_pslog_stamp "${CAI_PSLOG_PREFIX}/include/pslog.h")
  endif()
  if(NOT EXISTS "${_cai_pslog_stamp}")
    file(REMOVE_RECURSE "${CAI_PSLOG_PREFIX}")
    file(MAKE_DIRECTORY "${CAI_PSLOG_PREFIX}")
    set(_cai_pslog_download 1)
    if(EXISTS "${CAI_PSLOG_PKG_ARCHIVE}")
      file(SHA256 "${CAI_PSLOG_PKG_ARCHIVE}" _cai_pslog_actual_sha256)
      if(_cai_pslog_actual_sha256 STREQUAL CAI_PSLOG_PKG_SHA256)
        set(_cai_pslog_download 0)
      endif()
    endif()
    if(_cai_pslog_download)
      message(STATUS "Downloading libpslog ${CAI_PSLOG_VERSION} ${CAI_TARGET_ID} from ${CAI_PSLOG_PKG_URL}")
      file(DOWNLOAD
        "${CAI_PSLOG_PKG_URL}"
        "${CAI_PSLOG_PKG_ARCHIVE}"
        EXPECTED_HASH "SHA256=${CAI_PSLOG_PKG_SHA256}"
        TLS_VERIFY ON
        SHOW_PROGRESS)
    endif()
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E tar xzf "${CAI_PSLOG_PKG_ARCHIVE}"
      WORKING_DIRECTORY "${CAI_PSLOG_PREFIX}"
      RESULT_VARIABLE _cai_pslog_extract_result)
    if(NOT _cai_pslog_extract_result EQUAL 0)
      message(FATAL_ERROR "Failed to extract ${CAI_PSLOG_PKG_ARCHIVE}")
    endif()
  endif()
  if(CAI_TARGET_ID STREQUAL "arm64-apple-darwin")
    set(_cai_pkt_curl_library "${CAI_C_PKT_SYSTEMS_PREFIX}/lib/libcurl.dylib")
    set(_cai_lonejson_library "${CAI_LONEJSON_PREFIX}/lib/liblonejson.dylib")
  else()
    set(_cai_pkt_curl_library "${CAI_C_PKT_SYSTEMS_PREFIX}/lib/libcurl.so")
    set(_cai_lonejson_library "${CAI_LONEJSON_PREFIX}/lib/liblonejson.so")
  endif()
  set(CURL_INCLUDE_DIR "${CAI_C_PKT_SYSTEMS_PREFIX}/include" CACHE PATH "curl include directory" FORCE)
  set(CURL_LIBRARY "${_cai_pkt_curl_library}" CACHE FILEPATH "curl library" FORCE)
  set(CURL_LIBRARY_RELEASE "${_cai_pkt_curl_library}" CACHE FILEPATH "curl release library" FORCE)
  set(CAI_LONEJSON_INCLUDE_DIR "${CAI_LONEJSON_PREFIX}/include")
  set(CAI_LONEJSON_LIBRARY "${_cai_lonejson_library}")
  set(CAI_PSLOG_INCLUDE_DIR "${CAI_PSLOG_PREFIX}/include")
  set(CMAKE_PREFIX_PATH
      "${CAI_C_PKT_SYSTEMS_PREFIX};${CAI_LONEJSON_PREFIX};${CAI_PSLOG_PREFIX};${CMAKE_PREFIX_PATH}")
  list(APPEND CMAKE_INSTALL_RPATH "$ORIGIN")
  list(APPEND CMAKE_BUILD_RPATH
       "${CAI_C_PKT_SYSTEMS_PREFIX}/lib" "${CAI_LONEJSON_PREFIX}/lib")
endif()

set(CAI_C_PKT_SYSTEMS_SHA256_FOR_PACKAGE "")
if(CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "pkt")
  set(CAI_C_PKT_SYSTEMS_SHA256_FOR_PACKAGE "${CAI_C_PKT_SYSTEMS_SHA256}")
else()
  cai_c_pkt_systems_sha256("${CAI_TARGET_ID}" CAI_C_PKT_SYSTEMS_SHA256_FOR_PACKAGE)
endif()

find_package(CURL REQUIRED)
if(CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "pkt" AND TARGET CURL::libcurl)
  set_target_properties(CURL::libcurl PROPERTIES
    IMPORTED_LOCATION "${_cai_pkt_curl_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${CAI_C_PKT_SYSTEMS_PREFIX}/include")
endif()
find_package(Threads REQUIRED)

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

if(NOT CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "pkt")
  find_path(CAI_LONEJSON_INCLUDE_DIR lonejson.h)
  find_path(CAI_PSLOG_INCLUDE_DIR pslog.h)
  find_library(CAI_LONEJSON_LIBRARY NAMES lonejson)
endif()
if(NOT CAI_LONEJSON_INCLUDE_DIR OR
   NOT EXISTS "${CAI_LONEJSON_INCLUDE_DIR}/lonejson.h")
  message(FATAL_ERROR
    "Failed to find lonejson.h. Use CAI_DEPENDENCY_MODE=pkt or set "
    "CAI_LONEJSON_INCLUDE_DIR to an official installed lonejson include path.")
endif()
if(NOT CAI_PSLOG_INCLUDE_DIR OR NOT EXISTS "${CAI_PSLOG_INCLUDE_DIR}/pslog.h")
  message(FATAL_ERROR
    "Failed to find pslog.h. Use CAI_DEPENDENCY_MODE=pkt or set "
    "CAI_PSLOG_INCLUDE_DIR to an official installed pslog include path.")
endif()
if(NOT CAI_LONEJSON_LIBRARY)
  message(FATAL_ERROR
    "Failed to find liblonejson. Use CAI_DEPENDENCY_MODE=pkt or set "
    "CAI_LONEJSON_LIBRARY to an official installed lonejson library.")
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
  src/cai_mcp.c
  src/cai_models.c
  src/cai_response.c
  src/cai_source.c
  src/cai_stream.c
  src/cai_tool.c
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
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_include_directories(${_cai_target} PRIVATE "${CAI_LONEJSON_INCLUDE_DIR}")
    target_include_directories(${_cai_target} PRIVATE "${CAI_PSLOG_INCLUDE_DIR}")
    target_compile_definitions(${_cai_target} PRIVATE LONEJSON_WITH_CURL)
    cai_configure_c_target(${_cai_target})
  endif()
endforeach()
if(TARGET cai_shared)
  target_link_libraries(cai_shared PRIVATE CURL::libcurl Threads::Threads
    "${CAI_LONEJSON_LIBRARY}")
endif()
if(TARGET cai_static)
  target_link_libraries(cai_static INTERFACE
    $<INSTALL_INTERFACE:CURL::libcurl>
    $<INSTALL_INTERFACE:Threads::Threads>)
endif()

if(CAI_BUILD_EXAMPLES)
  set(_cai_example_link_target "")
  if(TARGET cai_static)
    set(_cai_example_link_target cai_static)
  elseif(TARGET cai_shared)
    set(_cai_example_link_target cai_shared)
  endif()
  set(_cai_examples
    basic_response:basic-response
    conversation_handles:conversation-handles
    history_export:history-export
    openrouter_response:openrouter-response
    searxng_search:searxng-search
    session_state:session-state
    smhi_weather:smhi-weather
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
      "${CAI_LONEJSON_INCLUDE_DIR}")
    target_link_libraries(cai_example_${_cai_example} PRIVATE
      ${_cai_example_link_target})
    if(_cai_example_link_target STREQUAL "cai_static")
      target_link_libraries(cai_example_${_cai_example} PRIVATE CURL::libcurl
        Threads::Threads "${CAI_LONEJSON_LIBRARY}")
    elseif(_cai_example STREQUAL "smhi_weather")
      target_link_libraries(cai_example_${_cai_example} PRIVATE CURL::libcurl)
    endif()
    cai_configure_c_target(cai_example_${_cai_example})
  endforeach()
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
  target_link_libraries(cai_tests PRIVATE cai_static CURL::libcurl
    Threads::Threads "${CAI_LONEJSON_LIBRARY}")
  cai_configure_c_target(cai_tests)
  add_test(NAME cai_tests COMMAND cai_tests)

  add_executable(cai_mcp_http_server tests/mcp_http_server.c)
  target_include_directories(cai_mcp_http_server PRIVATE
    "${CAI_PUBLIC_INCLUDE_DIR}"
    "${CAI_GENERATED_INCLUDE_DIR}"
    "${CAI_LONEJSON_INCLUDE_DIR}")
  target_link_libraries(cai_mcp_http_server PRIVATE cai_static CURL::libcurl
    Threads::Threads "${CAI_LONEJSON_LIBRARY}")
  cai_configure_c_target(cai_mcp_http_server)

  add_executable(cai_mcp_http_smoke tests/mcp_http_smoke.c)
  cai_configure_c_target(cai_mcp_http_smoke)
  add_test(NAME cai_mcp_http_smoke
    COMMAND cai_mcp_http_smoke $<TARGET_FILE:cai_mcp_http_server>)
  add_test(NAME cai_mcp_inspector_e2e
    COMMAND /bin/sh
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_inspector_e2e.sh"
      $<TARGET_FILE:cai_mcp_http_server>)

  if(CAI_INSTALL)
    add_test(NAME cai_install_metadata_test
      COMMAND "${CMAKE_COMMAND}"
        "-DCAI_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        "-DCAI_VERSION=${PROJECT_VERSION}"
        "-DCAI_C_PKT_SYSTEMS_PREFIX=${CAI_C_PKT_SYSTEMS_PREFIX}"
        "-DCAI_LONEJSON_PREFIX=${CAI_LONEJSON_PREFIX}"
        "-DCAI_PSLOG_PREFIX=${CAI_PSLOG_PREFIX}"
        "-DCAI_RESOLVED_DEPENDENCY_MODE=${CAI_RESOLVED_DEPENDENCY_MODE}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/install_metadata_test.cmake")
  endif()

  if(CAI_BUILD_INTEGRATION_TESTS)
    add_executable(cai_integration_tests tests/integration_main.c)
    target_include_directories(cai_integration_tests PRIVATE
      "${CAI_PUBLIC_INCLUDE_DIR}"
      "${CAI_GENERATED_INCLUDE_DIR}"
      "${CAI_LONEJSON_INCLUDE_DIR}"
      "${CAI_PSLOG_INCLUDE_DIR}"
      ${CURL_INCLUDE_DIRS})
    target_link_libraries(cai_integration_tests PRIVATE cai_static CURL::libcurl
      Threads::Threads "${CAI_LONEJSON_LIBRARY}")
    cai_configure_c_target(cai_integration_tests)
    add_test(NAME cai_integration_tests COMMAND cai_integration_tests)
    set_tests_properties(cai_integration_tests PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
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
    Threads::Threads "${CAI_LONEJSON_LIBRARY}")
  cai_configure_c_target(cai_tool_fuzz)
  target_compile_options(cai_tool_fuzz PRIVATE -fsanitize=address,undefined)
  target_link_options(cai_tool_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

find_program(CLANG_FORMAT_BIN NAMES clang-format)
file(GLOB_RECURSE CAI_FORMAT_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.c"
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
  if(CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "pkt")
    set(CAI_PACKAGE_REQUIRES_PRIVATE "libcurl")
    set(CAI_PC_DEPENDENCY_CFLAGS "")
    set(CAI_PC_DEPENDENCY_LIBS_PRIVATE "-llonejson")
    set(CAI_CONFIG_LONEJSON_INCLUDE_HINT "")
    set(CAI_CONFIG_PSLOG_INCLUDE_HINT "")
    set(CAI_CONFIG_LONEJSON_LIBRARY_HINT "")
  else()
    set(CAI_PACKAGE_REQUIRES_PRIVATE "libcurl")
    get_filename_component(CAI_CONFIG_LONEJSON_LIBRARY_HINT
      "${CAI_LONEJSON_LIBRARY}" DIRECTORY)
    set(CAI_PC_DEPENDENCY_CFLAGS
      "-I${CAI_LONEJSON_INCLUDE_DIR} -I${CAI_PSLOG_INCLUDE_DIR}")
    set(CAI_PC_DEPENDENCY_LIBS_PRIVATE "${CAI_LONEJSON_LIBRARY}")
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
