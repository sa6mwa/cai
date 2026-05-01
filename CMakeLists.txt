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
option(CAI_USE_LOCKDC_SDK "Use official liblockdc SDK release tarball for native dependencies." OFF)

set(CAI_VERSION_OVERRIDE "" CACHE STRING "Override detected cai package version.")
set(CAI_DIST_DIR "${CMAKE_BINARY_DIR}/dist" CACHE PATH "Distribution output directory.")
set(CAI_TARGET_ID "" CACHE STRING "Release archive target id.")
set(CAI_DEPS_DIR "${CMAKE_SOURCE_DIR}/.cache/deps"
    CACHE PATH "Downloaded third-party dependency cache.")
set(CAI_LOCKDC_SDK_VERSION "0.4.0" CACHE STRING "Pinned liblockdc SDK release used for curl/native dependencies.")
set(CAI_LONEJSON_VERSION "0.5.0")
set(CAI_LONEJSON_SHA256
    "b91427a66b72cf0c8a4f4d3bddef5f7fbebfdaf332f50be30b4fb282442ccceb")
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

function(cai_lockdc_sdk_sha256 target_id out_var)
  if(target_id STREQUAL "x86_64-linux-gnu")
    set(_sha256 "7cef184a1ba9b9e2f23cd35a3b2fed7acef7b774d1565c2cdaec9e916dcb954c")
  elseif(target_id STREQUAL "x86_64-linux-musl")
    set(_sha256 "6a58fb967add059308ff5e6ade8cd01e7cb91443acb1c33068d037519c046ad5")
  elseif(target_id STREQUAL "aarch64-linux-gnu")
    set(_sha256 "2a515e6dcb925c80e03ef15f09606d3552b6139533c2e786d1f03aa99e4ebb16")
  elseif(target_id STREQUAL "aarch64-linux-musl")
    set(_sha256 "8df41475dc9e580d99c3e3c9b8289d65dd0fdd4295db75db0d236c36782c0bff")
  elseif(target_id STREQUAL "armhf-linux-gnu")
    set(_sha256 "f0729e0d318236724bbe99b5084a58a6e2756049af8a071a1355951aa2177985")
  elseif(target_id STREQUAL "armhf-linux-musl")
    set(_sha256 "6419768dce53a1a0e3f9054790d8fcbe2792520f2b79a9e2ef98ffa0b1a845b6")
  elseif(target_id STREQUAL "arm64-apple-darwin")
    set(_sha256 "8d591e7687b2bd98408d5122f19ff8c53bc68b4920f4a98ef1f1a66a67ca1042")
  else()
    message(FATAL_ERROR "No pinned liblockdc SDK checksum for target ${target_id}")
  endif()
  set(${out_var} "${_sha256}" PARENT_SCOPE)
endfunction()

if(CAI_USE_LOCKDC_SDK)
  cai_lockdc_sdk_sha256("${CAI_TARGET_ID}" CAI_LOCKDC_SDK_SHA256)
  set(CAI_LOCKDC_SDK_NAME "liblockdc-${CAI_LOCKDC_SDK_VERSION}-${CAI_TARGET_ID}")
  set(CAI_LOCKDC_SDK_URL
      "https://github.com/sa6mwa/liblockdc/releases/download/v${CAI_LOCKDC_SDK_VERSION}/${CAI_LOCKDC_SDK_NAME}.tar.gz")
  set(CAI_LOCKDC_SDK_ARCHIVE "${CAI_DEPS_DIR}/${CAI_LOCKDC_SDK_NAME}.tar.gz")
  set(CAI_LOCKDC_SDK_PREFIX "${CAI_DEPS_DIR}/${CAI_LOCKDC_SDK_NAME}")

  file(MAKE_DIRECTORY "${CAI_DEPS_DIR}")
  set(_cai_lockdc_download 1)
  if(EXISTS "${CAI_LOCKDC_SDK_ARCHIVE}")
    file(SHA256 "${CAI_LOCKDC_SDK_ARCHIVE}" _cai_lockdc_actual_sha256)
    if(_cai_lockdc_actual_sha256 STREQUAL CAI_LOCKDC_SDK_SHA256)
      set(_cai_lockdc_download 0)
    endif()
  endif()
  if(_cai_lockdc_download)
    message(STATUS "Downloading liblockdc SDK ${CAI_LOCKDC_SDK_VERSION} ${CAI_TARGET_ID} from ${CAI_LOCKDC_SDK_URL}")
    file(DOWNLOAD
      "${CAI_LOCKDC_SDK_URL}"
      "${CAI_LOCKDC_SDK_ARCHIVE}"
      EXPECTED_HASH "SHA256=${CAI_LOCKDC_SDK_SHA256}"
      TLS_VERIFY ON
      SHOW_PROGRESS)
  endif()
  if(NOT EXISTS "${CAI_LOCKDC_SDK_PREFIX}/include/curl/curl.h")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E tar xzf "${CAI_LOCKDC_SDK_ARCHIVE}"
      WORKING_DIRECTORY "${CAI_DEPS_DIR}"
      RESULT_VARIABLE _cai_lockdc_extract_result)
    if(NOT _cai_lockdc_extract_result EQUAL 0)
      message(FATAL_ERROR "Failed to extract ${CAI_LOCKDC_SDK_ARCHIVE}")
    endif()
  endif()
  if(CAI_TARGET_ID STREQUAL "arm64-apple-darwin")
    set(_cai_lockdc_curl_library "${CAI_LOCKDC_SDK_PREFIX}/lib/libcurl.dylib")
  else()
    set(_cai_lockdc_curl_library "${CAI_LOCKDC_SDK_PREFIX}/lib/libcurl.so")
  endif()
  set(CURL_INCLUDE_DIR "${CAI_LOCKDC_SDK_PREFIX}/include" CACHE PATH "curl include directory" FORCE)
  set(CURL_LIBRARY "${_cai_lockdc_curl_library}" CACHE FILEPATH "curl library" FORCE)
  set(CURL_LIBRARY_RELEASE "${_cai_lockdc_curl_library}" CACHE FILEPATH "curl release library" FORCE)
  set(CMAKE_PREFIX_PATH "${CAI_LOCKDC_SDK_PREFIX};${CMAKE_PREFIX_PATH}")
  list(APPEND CMAKE_INSTALL_RPATH "$ORIGIN")
  list(APPEND CMAKE_BUILD_RPATH "${CAI_LOCKDC_SDK_PREFIX}/lib")
endif()

set(CAI_LOCKDC_SDK_SHA256_FOR_PACKAGE "")
if(CAI_USE_LOCKDC_SDK)
  set(CAI_LOCKDC_SDK_SHA256_FOR_PACKAGE "${CAI_LOCKDC_SDK_SHA256}")
else()
  cai_lockdc_sdk_sha256("${CAI_TARGET_ID}" CAI_LOCKDC_SDK_SHA256_FOR_PACKAGE)
endif()

find_package(CURL REQUIRED)
if(CAI_USE_LOCKDC_SDK AND TARGET CURL::libcurl)
  set_target_properties(CURL::libcurl PROPERTIES
    IMPORTED_LOCATION "${_cai_lockdc_curl_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${CAI_LOCKDC_SDK_PREFIX}/include")
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

find_program(GZIP_EXECUTABLE gzip REQUIRED)

set(CAI_LONEJSON_ROOT "${CAI_DEPS_DIR}/lonejson-${CAI_LONEJSON_VERSION}")
set(CAI_LONEJSON_INCLUDE_DIR "${CAI_LONEJSON_ROOT}/include")
set(CAI_LONEJSON_HEADER "${CAI_LONEJSON_INCLUDE_DIR}/lonejson.h")
set(CAI_LONEJSON_ARCHIVE
    "${CAI_DEPS_DIR}/lonejson-${CAI_LONEJSON_VERSION}.h.gz")
set(CAI_PSLOG_ROOT "${CAI_DEPS_DIR}/pslog-${CAI_PSLOG_VERSION}")
set(CAI_PSLOG_INCLUDE_DIR "${CAI_PSLOG_ROOT}/include")
set(CAI_PSLOG_HEADER "${CAI_PSLOG_INCLUDE_DIR}/pslog.h")
set(CAI_PSLOG_ARCHIVE "${CAI_DEPS_DIR}/pslog-${CAI_PSLOG_VERSION}.h.gz")

file(MAKE_DIRECTORY "${CAI_DEPS_DIR}" "${CAI_LONEJSON_INCLUDE_DIR}"
                    "${CAI_PSLOG_INCLUDE_DIR}")

set(_cai_lonejson_download 1)
if(EXISTS "${CAI_LONEJSON_ARCHIVE}")
  file(SHA256 "${CAI_LONEJSON_ARCHIVE}" _cai_lonejson_actual_sha256)
  if(_cai_lonejson_actual_sha256 STREQUAL CAI_LONEJSON_SHA256)
    set(_cai_lonejson_download 0)
  endif()
endif()
if(_cai_lonejson_download)
  message(STATUS "Downloading lonejson ${CAI_LONEJSON_VERSION} from ${CAI_LONEJSON_URL}")
  file(DOWNLOAD
    "${CAI_LONEJSON_URL}"
    "${CAI_LONEJSON_ARCHIVE}"
    EXPECTED_HASH "SHA256=${CAI_LONEJSON_SHA256}"
    TLS_VERIFY ON
    SHOW_PROGRESS)
endif()

execute_process(
  COMMAND "${GZIP_EXECUTABLE}" -dc "${CAI_LONEJSON_ARCHIVE}"
  OUTPUT_FILE "${CAI_LONEJSON_HEADER}"
  RESULT_VARIABLE _cai_lonejson_gzip_result)
if(NOT _cai_lonejson_gzip_result EQUAL 0)
  message(FATAL_ERROR "Failed to decompress ${CAI_LONEJSON_ARCHIVE}")
endif()

set(_cai_pslog_download 1)
if(EXISTS "${CAI_PSLOG_ARCHIVE}")
  file(SHA256 "${CAI_PSLOG_ARCHIVE}" _cai_pslog_actual_sha256)
  if(_cai_pslog_actual_sha256 STREQUAL CAI_PSLOG_SHA256)
    set(_cai_pslog_download 0)
  endif()
endif()
if(_cai_pslog_download)
  message(STATUS "Downloading pslog ${CAI_PSLOG_VERSION} from ${CAI_PSLOG_URL}")
  file(DOWNLOAD
    "${CAI_PSLOG_URL}"
    "${CAI_PSLOG_ARCHIVE}"
    EXPECTED_HASH "SHA256=${CAI_PSLOG_SHA256}"
    TLS_VERIFY ON
    SHOW_PROGRESS)
endif()

execute_process(
  COMMAND "${GZIP_EXECUTABLE}" -dc "${CAI_PSLOG_ARCHIVE}"
  OUTPUT_FILE "${CAI_PSLOG_HEADER}"
  RESULT_VARIABLE _cai_pslog_gzip_result)
if(NOT _cai_pslog_gzip_result EQUAL 0)
  message(FATAL_ERROR "Failed to decompress ${CAI_PSLOG_ARCHIVE}")
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
  src/cai_lonejson_impl.c
  src/cai_models.c
  src/cai_response.c
  src/cai_source.c
  src/cai_stream.c
  src/cai_tool.c)

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
  target_link_libraries(cai_shared PRIVATE CURL::libcurl Threads::Threads)
endif()
if(TARGET cai_static)
  target_link_libraries(cai_static INTERFACE $<INSTALL_INTERFACE:lockdc::static>)
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
        Threads::Threads)
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
    Threads::Threads)
  cai_configure_c_target(cai_tests)
  add_test(NAME cai_tests COMMAND cai_tests)

  if(CAI_INSTALL)
    add_test(NAME cai_install_metadata_test
      COMMAND "${CMAKE_COMMAND}"
        "-DCAI_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        "-DCAI_VERSION=${PROJECT_VERSION}"
        "-DCAI_LOCKDC_SDK_PREFIX=${CAI_LOCKDC_SDK_PREFIX}"
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
      Threads::Threads)
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
    Threads::Threads)
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
