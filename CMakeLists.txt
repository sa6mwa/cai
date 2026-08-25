cmake_minimum_required(VERSION 3.21)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CaiVersion)
include(CaiLonejsonAbi)
include(cai-rpath)

cai_detect_version(CAI_RESOLVED_VERSION "${CMAKE_CURRENT_SOURCE_DIR}")
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" CAI_CMAKE_PROJECT_VERSION
       "${CAI_RESOLVED_VERSION}")
if(CAI_CMAKE_PROJECT_VERSION STREQUAL "")
  message(FATAL_ERROR
    "cai release version must begin with major.minor.patch: "
    "${CAI_RESOLVED_VERSION}")
endif()
set(CAI_PACKAGE_VERSION "${CAI_RESOLVED_VERSION}")
project(cai VERSION ${CAI_CMAKE_PROJECT_VERSION} LANGUAGES C)

include(CMakePackageConfigHelpers)
include(CTest)
include(GNUInstallDirs)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CAI_BUILD_STATIC "Build the static cai library." ON)
option(CAI_BUILD_SHARED "Build the shared cai library." ON)
option(CAI_BUILD_TESTS "Build cai unit tests." ON)
option(CAI_BUILD_INTEGRATION_TESTS
       "Build subscription-auth integration tests that contact ChatGPT when run."
       OFF)
option(CAI_REGISTER_API_KEY_EXAMPLE_SMOKE
       "Register opt-in API-key example smoke coverage (may spend provider credits)."
       OFF)
option(CAI_BUILD_EXAMPLES "Build cai examples." ON)
option(CAI_BUILD_LUA "Build the Lua 5.5 binding and Lua tests." OFF)
option(CAI_BUILD_FUZZERS "Build fuzz harnesses." OFF)
option(CAI_ENABLE_COVERAGE "Build tests with coverage instrumentation." OFF)
option(CAI_ENABLE_VALGRIND_TESTS "Register native Valgrind memory-check tests." OFF)
option(CAI_INSTALL "Enable install rules." ON)

set(CAI_VERSION_OVERRIDE "" CACHE STRING "Override detected cai package version.")
set(CAI_DIST_DIR "${CMAKE_BINARY_DIR}/dist" CACHE PATH "Distribution output directory.")
set(CAI_TARGET_ID "" CACHE STRING "Release archive target id.")
set(CAI_DEPS_DIR "${CMAKE_SOURCE_DIR}/.cache/deps"
    CACHE PATH "Repo-local extracted third-party dependency roots.")
set(_cai_previous_cpkt_dependency_cache "")
set(_cai_previous_cai_dependency_cache "")
if(EXISTS "${CMAKE_BINARY_DIR}/CMakeCache.txt")
  file(STRINGS "${CMAKE_BINARY_DIR}/CMakeCache.txt"
       _cai_previous_dependency_cache_entries
       REGEX "^(CPKT_DEPENDENCY_CACHE|CAI_DEPENDENCY_CACHE):PATH=")
  foreach(_cai_previous_dependency_cache_entry
          IN LISTS _cai_previous_dependency_cache_entries)
    if(_cai_previous_dependency_cache_entry MATCHES
       "^CPKT_DEPENDENCY_CACHE:PATH=(.*)$")
      set(_cai_previous_cpkt_dependency_cache "${CMAKE_MATCH_1}")
    elseif(_cai_previous_dependency_cache_entry MATCHES
           "^CAI_DEPENDENCY_CACHE:PATH=(.*)$")
      set(_cai_previous_cai_dependency_cache "${CMAKE_MATCH_1}")
    endif()
  endforeach()
endif()
set(_cai_cpkt_dependency_cache_changed FALSE)
if(DEFINED CPKT_DEPENDENCY_CACHE AND
   NOT "${CPKT_DEPENDENCY_CACHE}" STREQUAL
       "${_cai_previous_cpkt_dependency_cache}")
  set(_cai_cpkt_dependency_cache_changed TRUE)
endif()
set(_cai_cai_dependency_cache_changed FALSE)
if(DEFINED CAI_DEPENDENCY_CACHE AND
   NOT "${CAI_DEPENDENCY_CACHE}" STREQUAL
       "${_cai_previous_cai_dependency_cache}")
  set(_cai_cai_dependency_cache_changed TRUE)
endif()
set(_cai_ignore_legacy_dependency_cache FALSE)
if(_cai_cpkt_dependency_cache_changed AND
   "${CPKT_DEPENDENCY_CACHE}" STREQUAL "")
  set(_cai_ignore_legacy_dependency_cache TRUE)
endif()
if(_cai_cpkt_dependency_cache_changed AND
   NOT "${CPKT_DEPENDENCY_CACHE}" STREQUAL "")
  set(_cai_dependency_cache_default "${CPKT_DEPENDENCY_CACHE}")
elseif(_cai_cai_dependency_cache_changed AND
       NOT "${CAI_DEPENDENCY_CACHE}" STREQUAL "" AND
       NOT _cai_ignore_legacy_dependency_cache)
  set(_cai_dependency_cache_default "${CAI_DEPENDENCY_CACHE}")
elseif(DEFINED CPKT_DEPENDENCY_CACHE AND NOT "${CPKT_DEPENDENCY_CACHE}" STREQUAL "")
  set(_cai_dependency_cache_default "${CPKT_DEPENDENCY_CACHE}")
elseif(DEFINED CAI_DEPENDENCY_CACHE AND NOT "${CAI_DEPENDENCY_CACHE}" STREQUAL "" AND
       NOT _cai_ignore_legacy_dependency_cache)
  set(_cai_dependency_cache_default "${CAI_DEPENDENCY_CACHE}")
elseif(DEFINED ENV{CPKT_DEPENDENCY_CACHE} AND NOT "$ENV{CPKT_DEPENDENCY_CACHE}" STREQUAL "")
  set(_cai_dependency_cache_default "$ENV{CPKT_DEPENDENCY_CACHE}")
elseif(DEFINED ENV{XDG_CACHE_HOME} AND NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
  set(_cai_dependency_cache_default "$ENV{XDG_CACHE_HOME}/c.pkt.systems/deps")
elseif(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
  set(_cai_dependency_cache_default "$ENV{HOME}/.cache/c.pkt.systems/deps")
else()
  message(FATAL_ERROR
    "HOME, XDG_CACHE_HOME, or CPKT_DEPENDENCY_CACHE is required for the "
    "shared c.pkt.systems dependency archive cache")
endif()
set(CPKT_DEPENDENCY_CACHE "${_cai_dependency_cache_default}" CACHE PATH
    "Shared checksum-pinned external archive cache." FORCE)
set(CAI_DEPENDENCY_CACHE "${CPKT_DEPENDENCY_CACHE}" CACHE PATH
    "Compatibility alias for CPKT_DEPENDENCY_CACHE." FORCE)
set(CAI_DEPENDENCY_MODE "cpkt" CACHE STRING
    "Dependency mode: cpkt, host, or auto.")
set_property(CACHE CAI_DEPENDENCY_MODE PROPERTY STRINGS cpkt host auto)
set(CAI_C_PKT_SYSTEMS_VERSION "0.9.0" CACHE STRING
    "Pinned c.pkt.systems release used for curl/native dependencies." FORCE)
# ABI 3 is the latest released ABI. Values 5 through 7 were only used by this
# unreleased development branch and must not survive a reconfigure. Preserve
# any other packager-selected ABI override.
if(NOT DEFINED CAI_ABI_VERSION OR CAI_ABI_VERSION STREQUAL "" OR
   CAI_ABI_VERSION STREQUAL "3" OR CAI_ABI_VERSION STREQUAL "5" OR
   CAI_ABI_VERSION STREQUAL "6" OR CAI_ABI_VERSION STREQUAL "7")
  set(CAI_ABI_VERSION "4" CACHE STRING "Shared library ABI/SOVERSION." FORCE)
else()
  # Preserve an explicit packager value while giving its cache entry the
  # project-defined type and help text.
  set(CAI_ABI_VERSION "${CAI_ABI_VERSION}" CACHE STRING
      "Shared library ABI/SOVERSION." FORCE)
endif()
set(CAI_MINIMUM_CURL_VERSION "7.86.0")
set(CAI_LONEJSON_VERSION "0.42.0")
set(CAI_LONEJSON_ABI_VERSION "25")
set(CAI_PSLOG_VERSION "0.9.0")

if(NOT CAI_BUILD_STATIC AND NOT CAI_BUILD_SHARED)
  message(FATAL_ERROR "Enable at least one of CAI_BUILD_STATIC or CAI_BUILD_SHARED.")
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
set(CAI_TARGET_ID "${CAI_TARGET_ID}" CACHE STRING "Release archive target id." FORCE)

function(cai_c_pkt_systems_sha256 target_id out_var)
  if(target_id STREQUAL "x86_64-linux-gnu")
    set(_sha256 "0bbb1cbaf60b0a94fb5a6b3756123088b45e2bef9e38079038f22e3c07febb2e")
  elseif(target_id STREQUAL "x86_64-linux-musl")
    set(_sha256 "e867e7d8649bba6d6c4bed254f3a666faa090f8ccb31a3eb10b1323b694f2f21")
  elseif(target_id STREQUAL "aarch64-linux-gnu")
    set(_sha256 "3fb1fdeb83bfd58da48a3319dfc2c6d35384265b2db2074b63216240bf0fe2ad")
  elseif(target_id STREQUAL "aarch64-linux-musl")
    set(_sha256 "a915993c294e96c9a84b072bed45384c23f0058c9e18cd5caeba344aaa9b5d39")
  elseif(target_id STREQUAL "armhf-linux-gnu")
    set(_sha256 "18738e2d8e9661ebdcc0b54f4f292f0571d04218fceb1f281e051a50928d1694")
  elseif(target_id STREQUAL "armhf-linux-musl")
    set(_sha256 "7f5365014ef2222cb95c08525c0b123afb30b4f220f4edcd669f354a9af4ccab")
  elseif(target_id STREQUAL "arm64-apple-darwin")
    set(_sha256 "8bc25d47d30cb40b24eb5d07c2aad7850150fdea680eccadd1c819ce945901af")
  else()
    message(FATAL_ERROR "No pinned c.pkt.systems checksum for target ${target_id}")
  endif()
  set(${out_var} "${_sha256}" PARENT_SCOPE)
endfunction()

function(cai_lonejson_pkg_sha256 target_id out_var)
  if(target_id STREQUAL "x86_64-linux-gnu")
    set(_sha256 "e04f80b907d92f7e38f825fbd339297e85372fc1ce110abb9a93715ee450ece3")
  elseif(target_id STREQUAL "x86_64-linux-musl")
    set(_sha256 "ca0811bd920f6cf59f82d45e04525b562bba238564e5c5b9a00aa18331b5a5ca")
  elseif(target_id STREQUAL "aarch64-linux-gnu")
    set(_sha256 "d7f9c700be6f9af7e46b18d59a0be14a42bd19644a30684b5a1135f96ee2daed")
  elseif(target_id STREQUAL "aarch64-linux-musl")
    set(_sha256 "813950b50620cfa48e01ae0c5b30ae338b79066e750c45cefeb9a86076466903")
  elseif(target_id STREQUAL "armhf-linux-gnu")
    set(_sha256 "3aeff1901078917a4430dc945c253cf4cec193311f35245b4ef1c62056d181c1")
  elseif(target_id STREQUAL "armhf-linux-musl")
    set(_sha256 "37ba738c675b41c563b1b03ea322ad1e65dbe76749ebfd294809b50abafb2d32")
  elseif(target_id STREQUAL "arm64-apple-darwin")
    set(_sha256 "b351df4221e16d62b7b86940a6a6a6a4d38fffb850b2d118fca2f9f5a9bb5488")
  else()
    message(FATAL_ERROR
      "No pinned liblonejson package checksum for target ${target_id}")
  endif()
  set(${out_var} "${_sha256}" PARENT_SCOPE)
endfunction()

function(cai_pslog_pkg_sha256 target_id out_var)
  if(target_id STREQUAL "x86_64-linux-gnu")
    set(_sha256 "7981ce7e60f6f1e144042e7a9192bb661472756ae34336fb0c2ed8316b31945f")
  elseif(target_id STREQUAL "x86_64-linux-musl")
    set(_sha256 "d05e59e8d88018a2e78e0941d2db211f3c08e4fd7539065ed2de79ce7e371055")
  elseif(target_id STREQUAL "aarch64-linux-gnu")
    set(_sha256 "38bb08ca6646cf186925a724b61fb534fa49ec0d5e77ca95953dd7a5b18f76e1")
  elseif(target_id STREQUAL "aarch64-linux-musl")
    set(_sha256 "fce3c4f95b317563427437313ef2eb1987dc43973b0b0bf5169763d0a2705f69")
  elseif(target_id STREQUAL "armhf-linux-gnu")
    set(_sha256 "eff69fe9223cd2ad56572ad6acd768b560ac3e863e379c65367ad6338dbfffef")
  elseif(target_id STREQUAL "armhf-linux-musl")
    set(_sha256 "19eeadacfb82b7eba4187b1fc405225bf85a8866ea81939e2eaa841a23d3785c")
  elseif(target_id STREQUAL "arm64-apple-darwin")
    set(_sha256 "ff5d2106bcbc5ea5bce8dfdbca54d21650f350e50fd214a4b52ac65b4f834073")
  else()
    message(FATAL_ERROR
      "No pinned libpslog package checksum for target ${target_id}")
  endif()
  set(${out_var} "${_sha256}" PARENT_SCOPE)
endfunction()

set(CAI_RESOLVED_DEPENDENCY_MODE "${CAI_DEPENDENCY_MODE}")
if(CAI_DEPENDENCY_MODE STREQUAL "auto")
  find_path(_cai_auto_curl_include curl/curl.h)
  find_library(_cai_auto_curl_library NAMES curl)
  find_path(_cai_auto_lonejson_include lonejson.h)
  find_path(_cai_auto_pslog_include pslog.h)
  find_library(_cai_auto_lonejson_library NAMES lonejson)
  find_library(_cai_auto_pslog_library NAMES pslog)
  set(_cai_auto_curl_ok FALSE)
  set(_cai_auto_curl_version_header
      "${_cai_auto_curl_include}/curl/curlver.h")
  if(_cai_auto_curl_include AND _cai_auto_curl_library AND
     EXISTS "${_cai_auto_curl_version_header}")
    file(STRINGS "${_cai_auto_curl_version_header}" _cai_auto_curl_version_line
         REGEX "^#define LIBCURL_VERSION \"[^\"]+\"")
    if(_cai_auto_curl_version_line MATCHES
       "^#define LIBCURL_VERSION \"([^\"]+)\"")
      set(_cai_auto_curl_version "${CMAKE_MATCH_1}")
      if(_cai_auto_curl_version VERSION_GREATER_EQUAL
         CAI_MINIMUM_CURL_VERSION)
        set(_cai_auto_curl_ok TRUE)
      endif()
    endif()
  endif()
  set(_cai_auto_lonejson_abi_ok FALSE)
  if(_cai_auto_lonejson_library)
    cai_probe_lonejson_library_abi(
      "${_cai_auto_lonejson_library}" "${CAI_LONEJSON_ABI_VERSION}"
      _cai_auto_lonejson_abi_ok _cai_auto_lonejson_abi_reason)
  endif()
  if(_cai_auto_curl_ok AND _cai_auto_lonejson_include AND
     _cai_auto_pslog_include AND _cai_auto_lonejson_library AND
     _cai_auto_pslog_library AND
     _cai_auto_lonejson_abi_ok)
    set(CAI_RESOLVED_DEPENDENCY_MODE "host")
  else()
    set(CAI_RESOLVED_DEPENDENCY_MODE "cpkt")
  endif()
  unset(_cai_auto_curl_include CACHE)
  unset(_cai_auto_curl_library CACHE)
  unset(_cai_auto_curl_ok)
  unset(_cai_auto_curl_version)
  unset(_cai_auto_curl_version_header)
  unset(_cai_auto_curl_version_line)
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

function(cai_dependency_archive_path archive_name sha256 out_var)
  set(_cai_archive
      "${CPKT_DEPENDENCY_CACHE}/archives/sha256/${sha256}/${archive_name}")
  set(${out_var} "${_cai_archive}" PARENT_SCOPE)
endfunction()

function(cai_download_tarball name url archive prefix sha256 stamp_file)
  get_filename_component(_cai_archive_dir "${archive}" DIRECTORY)
  file(MAKE_DIRECTORY "${_cai_archive_dir}" "${CAI_DEPS_DIR}")
  file(MAKE_DIRECTORY "${CPKT_DEPENDENCY_CACHE}/locks")
  file(LOCK "${CPKT_DEPENDENCY_CACHE}/locks/${sha256}.lock" GUARD FUNCTION
       TIMEOUT 600)
  set(_cai_download 1)
  if(EXISTS "${archive}")
    file(SHA256 "${archive}" _cai_actual_sha256)
    if(_cai_actual_sha256 STREQUAL "${sha256}")
      set(_cai_download 0)
    else()
      message(WARNING
        "Discarding corrupt cached ${name} archive: expected ${sha256}, got "
        "${_cai_actual_sha256}")
      file(REMOVE "${archive}")
    endif()
  endif()
  if(_cai_download)
    message(STATUS "Downloading ${name} from ${url}")
    set(_cai_tmp_archive "${archive}.tmp.${CMAKE_HOST_SYSTEM_PROCESSOR}.$ENV{USER}")
    file(REMOVE "${_cai_tmp_archive}")
    set(_cai_download_ok FALSE)
    set(_cai_download_status_code 1)
    set(_cai_download_status_text "not attempted")
    foreach(_cai_download_attempt RANGE 1 5)
      file(REMOVE "${archive}")
      file(DOWNLOAD
        "${url}"
        "${_cai_tmp_archive}"
        TLS_VERIFY ON
        SHOW_PROGRESS
        STATUS _cai_download_status)
      list(GET _cai_download_status 0 _cai_download_status_code)
      list(GET _cai_download_status 1 _cai_download_status_text)
      if(_cai_download_status_code EQUAL 0)
        file(SHA256 "${_cai_tmp_archive}" _cai_download_actual_sha256)
        if(_cai_download_actual_sha256 STREQUAL "${sha256}")
          set(_cai_download_ok TRUE)
          break()
        endif()
        set(_cai_download_status_text
            "SHA256 mismatch: expected ${sha256}, got ${_cai_download_actual_sha256}")
      endif()
      if(_cai_download_attempt LESS 5)
        message(WARNING
          "Download failed for ${name} on attempt ${_cai_download_attempt}/5: "
          "${_cai_download_status_text}; retrying")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 2)
      endif()
    endforeach()
    if(NOT _cai_download_ok)
      file(REMOVE "${_cai_tmp_archive}")
      message(FATAL_ERROR
        "Failed to download ${name} after 5 attempts: "
        "${_cai_download_status_text}")
    endif()
    file(RENAME "${_cai_tmp_archive}" "${archive}")
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
  cai_dependency_archive_path(
    "${CAI_C_PKT_SYSTEMS_NAME}.tar.gz" "${CAI_C_PKT_SYSTEMS_SHA256}"
    CAI_C_PKT_SYSTEMS_ARCHIVE)
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
  cai_dependency_archive_path(
    "${CAI_LONEJSON_PKG_NAME}.tar.gz" "${CAI_LONEJSON_PKG_SHA256}"
    CAI_LONEJSON_PKG_ARCHIVE)
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
  cai_dependency_archive_path(
    "${CAI_PSLOG_PKG_NAME}.tar.gz" "${CAI_PSLOG_PKG_SHA256}"
    CAI_PSLOG_PKG_ARCHIVE)
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
  set(CURL_DIR "${CAI_C_PKT_SYSTEMS_PREFIX}/lib/cmake/CURL"
      CACHE PATH "curl package directory" FORCE)
  set(OpenSSL_DIR "${CAI_C_PKT_SYSTEMS_PREFIX}/lib/cmake/OpenSSL"
      CACHE PATH "OpenSSL package directory" FORCE)
  find_package(lonejson CONFIG REQUIRED COMPONENTS curl oidc openssl)
  find_package(pslog CONFIG REQUIRED)
  cai_set_lonejson_from_target()
  cai_set_pslog_from_target()
  list(APPEND CMAKE_INSTALL_RPATH "${CAI_INSTALL_RPATH_TOKEN}")
  get_filename_component(_cai_lonejson_library_dir
    "${CAI_LONEJSON_LIBRARY}" DIRECTORY)
  list(APPEND CMAKE_BUILD_RPATH
       "${CAI_C_PKT_SYSTEMS_PREFIX}/lib" "${_cai_lonejson_library_dir}")
endif()

set(CAI_C_PKT_SYSTEMS_SHA256_FOR_PACKAGE "")
set(CAI_PSLOG_PKG_SHA256_FOR_PACKAGE "")
if(CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "cpkt")
  set(CAI_C_PKT_SYSTEMS_SHA256_FOR_PACKAGE "${CAI_C_PKT_SYSTEMS_SHA256}")
  set(CAI_PSLOG_PKG_SHA256_FOR_PACKAGE "${CAI_PSLOG_PKG_SHA256}")
endif()

find_package(CURL ${CAI_MINIMUM_CURL_VERSION} REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)

set(CAI_CURL_STATIC_LINK CURL::libcurl)
set(CAI_OPENSSL_CRYPTO_STATIC_LINK OpenSSL::Crypto)
set(CAI_CURL_SHARED_LINK CURL::libcurl)
set(CAI_OPENSSL_CRYPTO_SHARED_LINK OpenSSL::Crypto)
if(CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "cpkt")
  if(TARGET cpkt::curl_shared)
    set(CAI_CURL_SHARED_LINK cpkt::curl_shared)
  endif()
  if(TARGET cpkt::openssl_crypto_shared)
    set(CAI_OPENSSL_CRYPTO_SHARED_LINK cpkt::openssl_crypto_shared)
  endif()
endif()

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
  set(CAI_LUA_ROCK_SHARE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/luarocks/share/lua/5.5")
  set(CAI_LUA_ROCK_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/luarocks/lib/lua/5.5")
  set(CAI_LUA_TEST_ENV
      "LUA_PATH=${CAI_LUA_ROCK_SHARE_DIR}/?.lua\;${CAI_LUA_ROCK_SHARE_DIR}/?/init.lua\;\;"
      "LUA_CPATH=${CMAKE_CURRENT_BINARY_DIR}/lua-test/?.so\;${CMAKE_CURRENT_BINARY_DIR}/lua-test/?.dylib\;${CMAKE_CURRENT_BINARY_DIR}/lua/?.so\;${CMAKE_CURRENT_BINARY_DIR}/lua/?.dylib\;${CAI_LUA_ROCK_LIB_DIR}/?.so\;\;")
  set(CAI_LUA_SMITH_E2E_ENV
      "CAI_LUA_SMITH_E2E=1"
      "CAI_LUA_LD_LIBRARY_PATH=${CMAKE_CURRENT_SOURCE_DIR}/build/luarocks/cai-prefix/lib:${CAI_LONEJSON_PREFIX}/lib:${CAI_C_PKT_SYSTEMS_PREFIX}/lib:${CAI_PSLOG_PREFIX}/lib")
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

# Smith ships the exact upstream GPT-5.6 Codex instruction asset. The
# compiled representation replaces only identity phrases with Smith's renderer
# token and appends the narrow CAI capability overlay token.
set(CAI_SMITH_GPT_5_6_PROMPT_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/prompts/smith/gpt-5.6-codex.md")
set(CAI_SMITH_PRIVATE_INCLUDE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated/private")
file(SHA256 "${CAI_SMITH_GPT_5_6_PROMPT_SOURCE}"
     CAI_SMITH_GPT_5_6_PROMPT_SHA256)
if(NOT CAI_SMITH_GPT_5_6_PROMPT_SHA256 STREQUAL
       "35d8b5d513fff3b55344d5f9f3169305cc276aee053f5be140a77708e0926e7c")
  message(FATAL_ERROR
    "Smith GPT-5.6 Codex prompt asset hash does not match its pinned upstream "
    "revision")
endif()
file(READ "${CAI_SMITH_GPT_5_6_PROMPT_SOURCE}" CAI_SMITH_PROMPT_TEMPLATE)
string(REGEX REPLACE
  "^You are Codex, an agent based on GPT-5\\."
  "You are {{agent_identity}}, an agent based on GPT-5."
  CAI_SMITH_PROMPT_TEMPLATE "${CAI_SMITH_PROMPT_TEMPLATE}")
string(REPLACE "As Codex," "As {{agent_identity}},"
               CAI_SMITH_PROMPT_TEMPLATE "${CAI_SMITH_PROMPT_TEMPLATE}")
string(APPEND CAI_SMITH_PROMPT_TEMPLATE "\n{{agent_tools}}\n")
string(LENGTH "${CAI_SMITH_PROMPT_TEMPLATE}" CAI_SMITH_PROMPT_LENGTH)
set(CAI_SMITH_PROMPT_OFFSET 0)
set(CAI_SMITH_PROMPT_C "")
while(CAI_SMITH_PROMPT_OFFSET LESS CAI_SMITH_PROMPT_LENGTH)
  math(EXPR CAI_SMITH_PROMPT_REMAINING
       "${CAI_SMITH_PROMPT_LENGTH} - ${CAI_SMITH_PROMPT_OFFSET}")
  if(CAI_SMITH_PROMPT_REMAINING GREATER 400)
    set(CAI_SMITH_PROMPT_CHUNK_LENGTH 400)
  else()
    set(CAI_SMITH_PROMPT_CHUNK_LENGTH ${CAI_SMITH_PROMPT_REMAINING})
  endif()
  string(SUBSTRING "${CAI_SMITH_PROMPT_TEMPLATE}"
         ${CAI_SMITH_PROMPT_OFFSET} ${CAI_SMITH_PROMPT_CHUNK_LENGTH}
         CAI_SMITH_PROMPT_CHUNK)
  string(REPLACE "\\" "\\\\" CAI_SMITH_PROMPT_CHUNK
         "${CAI_SMITH_PROMPT_CHUNK}")
  string(REPLACE "\"" "\\\"" CAI_SMITH_PROMPT_CHUNK
         "${CAI_SMITH_PROMPT_CHUNK}")
  string(REPLACE "\r" "\\r" CAI_SMITH_PROMPT_CHUNK
         "${CAI_SMITH_PROMPT_CHUNK}")
  string(REPLACE "\n" "\\n" CAI_SMITH_PROMPT_CHUNK
         "${CAI_SMITH_PROMPT_CHUNK}")
  string(APPEND CAI_SMITH_PROMPT_C "\"${CAI_SMITH_PROMPT_CHUNK}\",\n")
  math(EXPR CAI_SMITH_PROMPT_OFFSET
       "${CAI_SMITH_PROMPT_OFFSET} + ${CAI_SMITH_PROMPT_CHUNK_LENGTH}")
endwhile()
file(MAKE_DIRECTORY "${CAI_SMITH_PRIVATE_INCLUDE_DIR}")
file(WRITE "${CAI_SMITH_PRIVATE_INCLUDE_DIR}/cai_smith_gpt_5_6_prompt.h"
  "/* Generated from prompts/smith/gpt-5.6-codex.md. */\n"
  "#ifndef CAI_SMITH_GPT_5_6_PROMPT_H\n"
  "#define CAI_SMITH_GPT_5_6_PROMPT_H\n"
  "#define CAI_SMITH_GPT_5_6_PROMPT_SOURCE_SHA256 \"${CAI_SMITH_GPT_5_6_PROMPT_SHA256}\"\n"
  "static const char *const cai_smith_gpt_5_6_prompt_parts[] = {\n${CAI_SMITH_PROMPT_C}NULL};\n"
  "#endif\n")

if(NOT CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "cpkt")
  find_package(lonejson CONFIG QUIET COMPONENTS curl oidc openssl)
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
  src/cai_agent_runtime.c
  src/cai_session_store.c
  src/cai_skills.c
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
  src/cai_mcp_client.c
  src/cai_models.c
  src/cai_response.c
  src/cai_smith.c
  src/cai_source.c
  src/cai_stream.c
  src/cai_tool.c
  src/tools/exec.c
  src/tools/goal.c
  src/tools/patch.c
  src/tools/read.c
  src/tools/revgeo.c
  src/tools/searxng.c
  src/tools/terminal.c
  src/tools/todo.c
  src/tools/view_image.c)

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
    SOVERSION ${CAI_ABI_VERSION})
  if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set_target_properties(cai_shared PROPERTIES
      BUILD_WITH_INSTALL_RPATH ON
      INSTALL_NAME_DIR "@rpath"
      MACOSX_RPATH ON)
  endif()
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
      "${CAI_SMITH_PRIVATE_INCLUDE_DIR}"
      "${CAI_PSLOG_INCLUDE_DIR}")
    target_include_directories(${_cai_target} PRIVATE ${CURL_INCLUDE_DIRS})
    target_compile_definitions(${_cai_target} PRIVATE
      LONEJSON_WITH_CURL LONEJSON_WITH_JWT LONEJSON_WITH_OIDC
      LONEJSON_WITH_OPENSSL)
    cai_configure_c_target(${_cai_target})
  endif()
endforeach()
if(TARGET cai_shared)
  target_link_libraries(cai_shared PRIVATE ${CAI_CURL_SHARED_LINK}
    Threads::Threads ${CAI_OPENSSL_CRYPTO_SHARED_LINK} ${CAI_LONEJSON_LINK})
endif()
if(TARGET cai_static)
  target_link_libraries(cai_static INTERFACE
    $<BUILD_INTERFACE:${CAI_CURL_STATIC_LINK}>
    $<BUILD_INTERFACE:Threads::Threads>
    $<BUILD_INTERFACE:${CAI_OPENSSL_CRYPTO_STATIC_LINK}>
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

function(cai_link_internal_consumer target)
  target_link_libraries(${target} PRIVATE ${_cai_internal_link_target})
endfunction()

function(cai_link_direct_curl_user target)
  if(_cai_internal_link_target STREQUAL "cai_static")
    target_link_libraries(${target} PRIVATE ${CAI_CURL_STATIC_LINK})
  else()
    target_link_libraries(${target} PRIVATE ${CAI_CURL_SHARED_LINK})
  endif()
endfunction()

if(CAI_BUILD_EXAMPLES)
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
    smith_terminal:smith-terminal
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
    if(_cai_example STREQUAL "mcp_server" OR _cai_example STREQUAL "smith_terminal")
      target_include_directories(cai_example_${_cai_example} PRIVATE
        "${CAI_PSLOG_INCLUDE_DIR}")
    endif()
    cai_link_internal_consumer(cai_example_${_cai_example})
    if(_cai_example STREQUAL "smhi_weather")
      cai_link_direct_curl_user(cai_example_${_cai_example})
    endif()
    if(_cai_example STREQUAL "mcp_server" OR _cai_example STREQUAL "smith_terminal")
      if(CAI_PSLOG_LINK)
        target_link_libraries(cai_example_${_cai_example} PRIVATE
          ${CAI_PSLOG_LINK})
      else()
        message(FATAL_ERROR
          "The ${_cai_example} example requires libpslog. Install libpslog, set "
          "CAI_PSLOG_LIBRARY, or configure with CAI_BUILD_EXAMPLES=OFF.")
      endif()
    endif()
    cai_configure_c_target(cai_example_${_cai_example})
  endforeach()
endif()

if(CAI_BUILD_LUA)
  set(_cai_pslog_lua_include_hints
    "${CMAKE_CURRENT_SOURCE_DIR}/build/luarocks/share/lua/5.5"
    "${CMAKE_CURRENT_BINARY_DIR}/luarocks/share/lua/5.5")
  find_program(CAI_LUAROCKS_EXECUTABLE NAMES luarocks)
  if(CAI_LUAROCKS_EXECUTABLE)
    execute_process(
      COMMAND "${CAI_LUAROCKS_EXECUTABLE}" config deploy_lua_dir
      RESULT_VARIABLE _cai_luarocks_deploy_lua_dir_result
      OUTPUT_VARIABLE _cai_luarocks_deploy_lua_dir
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_cai_luarocks_deploy_lua_dir_result EQUAL 0 AND
       NOT _cai_luarocks_deploy_lua_dir STREQUAL "")
      list(APPEND _cai_pslog_lua_include_hints
        "${_cai_luarocks_deploy_lua_dir}")
    endif()
  endif()
  list(APPEND _cai_pslog_lua_include_hints
    "/usr/local/share/lua/5.5"
    "/usr/share/lua/5.5"
    "/opt/homebrew/share/lua/5.5")
  find_path(CAI_PSLOG_LUA_INCLUDE_DIR pslog_lua.h
    HINTS ${_cai_pslog_lua_include_hints}
    NO_CMAKE_FIND_ROOT_PATH)
  if(NOT CAI_PSLOG_LUA_INCLUDE_DIR)
    message(FATAL_ERROR
      "CAI_BUILD_LUA requires pslog_lua.h from lua-pslog. Install lua-pslog "
      "or set CAI_PSLOG_LUA_INCLUDE_DIR.")
  endif()
  set(_cai_lua_link_target "${_cai_internal_link_target}")
  if(TARGET cai_shared)
    set(_cai_lua_link_target cai_shared)
  endif()
  add_library(cai_lua MODULE lua/cai_lua.c)
  set_target_properties(cai_lua PROPERTIES
    OUTPUT_NAME cai
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lua")
  target_include_directories(cai_lua PRIVATE
    "${CAI_PUBLIC_INCLUDE_DIR}"
    "${CAI_GENERATED_INCLUDE_DIR}"
    "${CAI_LONEJSON_INCLUDE_DIR}"
    "${CAI_PSLOG_LUA_INCLUDE_DIR}"
    "${CAI_PSLOG_INCLUDE_DIR}"
    "${CAI_LUA_INCLUDE_DIR}")
  target_link_libraries(cai_lua PRIVATE ${_cai_lua_link_target}
    ${CAI_LONEJSON_LINK})
  if(APPLE)
    target_link_options(cai_lua PRIVATE "-undefined" "dynamic_lookup")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(cai_lua PRIVATE "-Wl,--allow-shlib-undefined")
    target_link_libraries(cai_lua PRIVATE ${CMAKE_DL_LIBS})
  else()
    target_link_libraries(cai_lua PRIVATE "${CAI_LUA_LIBRARY}")
  endif()
  cai_configure_c_target(cai_lua)
  if(CAI_BUILD_TESTS)
    add_library(cai_lua_native_todo_store_test MODULE
      tests/lua/native_todo_store.c)
    set_target_properties(cai_lua_native_todo_store_test PROPERTIES
      OUTPUT_NAME cai_native_todo_store_test
      PREFIX ""
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lua-test")
    target_include_directories(cai_lua_native_todo_store_test PRIVATE
      "${CAI_PUBLIC_INCLUDE_DIR}"
      "${CAI_GENERATED_INCLUDE_DIR}"
      "${CAI_LONEJSON_INCLUDE_DIR}"
      "${CAI_LUA_INCLUDE_DIR}")
    if(APPLE)
      target_link_options(cai_lua_native_todo_store_test PRIVATE
        "-undefined" "dynamic_lookup")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      target_link_options(cai_lua_native_todo_store_test PRIVATE
        "-Wl,--allow-shlib-undefined")
    else()
      target_link_libraries(cai_lua_native_todo_store_test PRIVATE
        "${CAI_LUA_LIBRARY}")
    endif()
    cai_configure_c_target(cai_lua_native_todo_store_test)
  endif()
  unset(_cai_lua_link_target)
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
  cai_link_internal_consumer(cai_tests)
  cai_configure_c_target(cai_tests)
  if(CAI_BUILD_EXAMPLES)
    add_executable(cai_smith_terminal_renderer_test
      tests/smith_terminal_renderer_test.c)
    target_include_directories(cai_smith_terminal_renderer_test PRIVATE
      "${CAI_PUBLIC_INCLUDE_DIR}"
      "${CAI_GENERATED_INCLUDE_DIR}"
      "${CAI_LONEJSON_INCLUDE_DIR}"
      "${CAI_PSLOG_INCLUDE_DIR}")
    cai_link_internal_consumer(cai_smith_terminal_renderer_test)
    if(CAI_PSLOG_LINK)
      target_link_libraries(cai_smith_terminal_renderer_test PRIVATE
        ${CAI_PSLOG_LINK})
    endif()
    cai_configure_c_target(cai_smith_terminal_renderer_test)
    add_test(NAME cai_smith_terminal_renderer_test
      COMMAND cai_smith_terminal_renderer_test)
    set_tests_properties(cai_smith_terminal_renderer_test PROPERTIES
      LABELS "unit;local;example-smoke"
      TIMEOUT 3)
  endif()
  set(CAI_UNIT_TEST_GROUPS
    model_capabilities
    env_precedence
    source_sink
    lonejson_nested_mapped_array_stream
    lonejson_selected_array_rewrite
    tool_registry
    mcp_client_config
    mcp_client_rejects_partial_allocator
    mcp_client_open_failure_cleans_allocator
    mcp_client_open_failure_keeps_receiver_cleanup
    mcp_streamable_http_header_callback_bounds
    mcp_client_receiver_surface
    mcp_client_registry_adapter
    mcp_streamable_http_client_roundtrip
    mcp_streamable_http_client_api_key_rejects_unsafe_token
    mcp_streamable_http_client_api_key_rejects_remote_http
    mcp_streamable_http_client_api_key_rejects_127_hostname
    mcp_streamable_http_client_api_key_rejects_userinfo_loopback
    mcp_streamable_http_client_api_key_rejects_short_url
    mcp_streamable_http_client_oauth2_client_credentials
    mcp_streamable_http_client_oauth2_rejects_remote_http_token
    mcp_streamable_http_client_oauth2_rejects_discovered_remote_http_token
    mcp_streamable_http_client_oauth2_rejects_loopback_http_oidc_discovery
    mcp_streamable_http_client_oauth2_rejects_non_bearer_token
    mcp_streamable_http_client_oauth2_rejects_unsafe_token
    mcp_streamable_http_client_oauth2_token_http_error
    mcp_streamable_http_client_oauth2_token_response_limit
    mcp_streamable_http_client_streaming_oauth2_error
    mcp_streamable_http_client_custom_allocator_metadata
    mcp_streamable_http_client_logging
    mcp_streamable_http_call_result_must_be_object
    mcp_streamable_http_ping_error
    mcp_streamable_http_ping_error_missing_code
    mcp_streamable_http_ping_error_missing_message
    mcp_streamable_http_ping_error_empty
    mcp_streamable_http_sse_ping_error
    mcp_streamable_http_sse_ping_error_missing_code
    mcp_streamable_http_sse_ping_error_missing_message
    mcp_streamable_http_sse_ping_error_empty
    mcp_streamable_http_initialize_missing_capabilities
    mcp_streamable_http_initialize_capabilities_not_object
    mcp_streamable_http_initialize_server_info_metadata
    mcp_streamable_http_initialize_server_info_icons_not_array
    mcp_streamable_http_initialize_server_info_icon_missing_src
    mcp_streamable_http_initialize_server_info_icon_bad_theme
    mcp_streamable_http_initialize_server_info_description_not_string
    mcp_streamable_http_initialize_instructions_not_string
    mcp_streamable_http_initialize_missing_server_info
    mcp_streamable_http_initialize_missing_protocol_version
    mcp_streamable_http_initialize_unsupported_protocol_version
    mcp_streamable_http_initialize_empty_session_id
    mcp_streamable_http_initialize_space_session_id
    mcp_streamable_http_long_session_id_header
    mcp_streamable_http_initialize_response_id_mismatch
    mcp_streamable_http_initialize_error_id_mismatch
    mcp_streamable_http_response_id_mismatch
    mcp_streamable_http_error_response_id_mismatch
    mcp_streamable_http_response_invalid_id_type
    mcp_streamable_http_response_null_id
    mcp_streamable_http_sse_request_invalid_id_type
    mcp_streamable_http_sse_request_null_id
    mcp_streamable_http_response_jsonrpc_invalid
    mcp_streamable_http_sse_jsonrpc_invalid
    mcp_streamable_http_sse_method_with_result
    mcp_streamable_http_sse_response_missing_id
    mcp_streamable_http_sse_response_missing_result_error
    mcp_streamable_http_sse_response_result_and_error
    mcp_streamable_http_utility_notifications_ignore_malformed
    mcp_streamable_http_active_progress_missing_progress
    mcp_streamable_http_active_progress_invalid_progress
    mcp_streamable_http_active_progress_negative_progress
    mcp_streamable_http_active_progress_negative_total
    mcp_streamable_http_active_progress_equal_progress
    mcp_streamable_http_active_progress_decreasing_progress
    mcp_streamable_http_cancelled_notification_aborts_request
    mcp_streamable_http_sse_resume_get
    mcp_streamable_http_sse_resume_caps_retry
    mcp_streamable_http_sse_resume_rejects_unsafe_id
    mcp_streamable_http_streaming_sse_resume_get
    mcp_streamable_http_streaming_sse_resume_before_result_bytes
    mcp_streamable_http_ignores_sse_data_before_non_message_event
    mcp_streamable_http_sse_data_before_message_event
    mcp_streamable_http_response_content_type_parameters
    mcp_streamable_http_sse_content_type_parameters
    mcp_streamable_http_response_missing_result_error
    mcp_streamable_http_response_result_and_error
    mcp_streamable_http_response_null_result
    mcp_streamable_http_tools_list_missing_tools
    mcp_streamable_http_tools_list_missing_input_schema
    mcp_streamable_http_tools_list_null_input_schema
    mcp_streamable_http_tools_list_array_input_schema
    mcp_streamable_http_tools_list_missing_input_schema_type
    mcp_streamable_http_tools_list_string_input_schema_type
    mcp_streamable_http_tools_list_input_schema_array_properties
    mcp_streamable_http_tools_list_input_schema_array_schema
    mcp_streamable_http_tools_list_input_schema_nonobject_property
    mcp_streamable_http_tools_list_input_schema_object_required
    mcp_streamable_http_tools_list_input_schema_nonstring_required
    mcp_streamable_http_tools_list_input_schema_escaped_property
    mcp_streamable_http_tools_list_array_output_schema
    mcp_streamable_http_tools_list_string_output_schema_type
    mcp_streamable_http_tools_list_numeric_output_schema_type
    mcp_streamable_http_tools_list_output_schema_array_schema
    mcp_streamable_http_tools_list_output_schema_nonobject_property
    mcp_streamable_http_tools_list_array_annotations
    mcp_streamable_http_tools_list_annotation_hint_not_bool
    mcp_streamable_http_tools_list_object_icons
    mcp_streamable_http_tools_list_icon_missing_src
    mcp_streamable_http_tools_list_array_execution
    mcp_streamable_http_tools_list_execution_task_support_invalid
    mcp_streamable_http_resources_list_missing_resources
    mcp_streamable_http_resources_list_object_icons
    mcp_streamable_http_resources_list_icon_invalid_theme
    mcp_streamable_http_resources_list_array_annotations
    mcp_streamable_http_resources_list_annotation_invalid_role
    mcp_streamable_http_resources_list_annotation_priority_not_number
    mcp_streamable_http_resources_list_annotation_priority_out_of_range
    mcp_streamable_http_resources_list_negative_size
    mcp_streamable_http_resource_templates_list_missing_templates
    mcp_streamable_http_resource_templates_list_object_icons
    mcp_streamable_http_resource_templates_list_icon_size_not_string
    mcp_streamable_http_resource_templates_list_array_annotations
    mcp_streamable_http_resource_templates_list_annotation_invalid_role
    mcp_streamable_http_resource_templates_list_annotation_priority_out_of_range
    mcp_streamable_http_resource_templates_list_annotation_modified_not_string
    mcp_streamable_http_prompts_list_missing_prompts
    mcp_streamable_http_prompts_list_object_arguments
    mcp_streamable_http_prompts_list_argument_missing_name
    mcp_streamable_http_prompts_list_argument_required_not_bool
    mcp_streamable_http_prompts_list_object_icons
    mcp_streamable_http_prompts_list_icon_metadata
    mcp_streamable_http_tool_call_omits_null_arguments
    mcp_streamable_http_prompt_get_omits_null_arguments
    mcp_streamable_http_tool_call_missing_content
    mcp_streamable_http_tool_call_missing_content_type
    mcp_streamable_http_tool_call_text_missing_text
    mcp_streamable_http_tool_call_image_missing_mime
    mcp_streamable_http_tool_call_image_invalid_base64
    mcp_streamable_http_tool_call_audio_content
    mcp_streamable_http_tool_call_content_array_annotations
    mcp_streamable_http_tool_call_content_invalid_annotation_role
    mcp_streamable_http_tool_call_content_priority_out_of_range
    mcp_streamable_http_tool_call_unknown_content_type
    mcp_streamable_http_tool_call_resource_link_missing_name
    mcp_streamable_http_tool_call_resource_link_icons_not_array
    mcp_streamable_http_tool_call_resource_link_invalid_icon_theme
    mcp_streamable_http_tool_call_resource_link_negative_size
    mcp_streamable_http_tool_call_resource_missing_resource
    mcp_streamable_http_tool_call_resource_missing_uri
    mcp_streamable_http_tool_call_resource_missing_body
    mcp_streamable_http_tool_call_resource_text_and_blob
    mcp_streamable_http_tool_call_resource_invalid_blob
    mcp_streamable_http_tool_call_array_structured_content
    mcp_streamable_http_resource_read_missing_contents
    mcp_streamable_http_resource_read_missing_body
    mcp_streamable_http_resource_read_text_and_blob
    mcp_streamable_http_resource_read_blob_content
    mcp_streamable_http_resource_read_invalid_blob
    mcp_streamable_http_prompt_get_missing_messages
    mcp_streamable_http_prompt_get_invalid_role
    mcp_streamable_http_prompt_get_array_content
    mcp_streamable_http_prompt_get_text_missing_text
    mcp_streamable_http_prompt_get_content_priority_not_number
    mcp_streamable_http_prompt_get_content_priority_out_of_range
    mcp_streamable_http_prompt_get_audio_content
    mcp_streamable_http_prompt_get_audio_invalid_base64
    mcp_streamable_http_prompt_get_resource_missing_body
    mcp_streamable_http_prompt_get_resource_link_negative_size
    mcp_streamable_http_prompt_get_unknown_content_type
    mcp_streamable_http_completion_result
    mcp_streamable_http_completion_missing_values
    mcp_streamable_http_completion_non_string_value
    mcp_streamable_http_completion_too_many_values
    mcp_streamable_http_completion_negative_total
    mcp_streamable_http_completion_string_has_more
    mcp_streamable_http_tool_call_array_arguments
    mcp_streamable_http_prompt_get_array_arguments
    mcp_streamable_http_prompt_get_non_string_argument
    mcp_streamable_http_completion_array_context_arguments
    mcp_streamable_http_completion_non_string_context_argument
    mcp_streamable_http_completion_invalid_ref_type
    mcp_streamable_http_completion_resource_ref_uses_uri
    mcp_streamable_http_list_changed_invalidates_cache
    mcp_streamable_http_tools_list_changed_array_params
    mcp_streamable_http_resources_list_changed_array_params
    mcp_streamable_http_prompts_list_changed_array_params
    mcp_streamable_http_send_request_params
    mcp_streamable_http_send_request_scalar_result
    mcp_streamable_http_send_request_json_error
    mcp_streamable_http_send_request_sse_error
    mcp_streamable_http_send_request_sse_error_after_notification
    mcp_streamable_http_send_request_sse_error_missing_code
    mcp_streamable_http_send_request_sse_empty_message_event
    mcp_streamable_http_tool_call_writes_chunked_result
    mcp_streamable_http_tool_call_writes_sse_result
    mcp_streamable_http_resource_read_writes_chunked_result
    mcp_streamable_http_prompt_get_writes_chunked_result
    mcp_streamable_http_completion_writes_chunked_result
    mcp_streamable_http_send_request_writes_chunked_result
    mcp_streamable_http_writes_result_one_byte_chunks
    mcp_streamable_http_writes_sse_result_one_byte_chunks
    mcp_streamable_http_writes_default_sse_message_immediately
    mcp_streamable_http_resource_read_default_sse_message
    mcp_streamable_http_prompt_get_default_sse_message
    mcp_streamable_http_completion_default_sse_message
    mcp_streamable_http_send_request_default_sse_message
    mcp_streamable_http_rejects_streamed_text_plain_result
    mcp_streamable_http_writes_sse_result_after_notification
    mcp_streamable_http_resource_read_sse_result_after_notification
    mcp_streamable_http_prompt_get_sse_result_after_notification
    mcp_streamable_http_completion_sse_result_after_notification
    mcp_streamable_http_send_request_sse_result_after_notification
    mcp_streamable_http_writes_sse_result_after_server_ping_request
    mcp_streamable_http_resource_read_sse_result_after_server_ping_request
    mcp_streamable_http_prompt_get_sse_result_after_server_ping_request
    mcp_streamable_http_completion_sse_result_after_server_ping_request
    mcp_streamable_http_send_request_sse_result_after_server_ping_request
    mcp_streamable_http_writes_sse_result_after_empty_data_event
    mcp_streamable_http_result_sink_failure
    mcp_streamable_http_json_error_result_not_written
    mcp_streamable_http_sse_error_result_not_written
    mcp_streamable_http_result_rejects_mismatched_id
    mcp_streamable_http_result_before_mismatched_id
    mcp_streamable_http_result_rejects_invalid_jsonrpc
    mcp_streamable_http_result_rejects_method_with_result
    mcp_streamable_http_result_rejects_missing_id
    mcp_streamable_http_result_accepts_result_before_id
    mcp_streamable_http_result_field_order_variants
    mcp_streamable_http_result_rejects_duplicate_result
    mcp_streamable_http_result_before_id_rejects_duplicate_result
    mcp_streamable_http_sse_rejects_result_and_error
    mcp_streamable_http_sse_rejects_result_before_error
    mcp_streamable_http_result_rejects_trailing_garbage
    mcp_streamable_http_result_rejects_bad_literal
    mcp_streamable_http_result_rejects_bad_object
    mcp_streamable_http_tool_call_http_error
    mcp_streamable_http_resource_read_http_error
    mcp_streamable_http_prompt_get_http_error
    mcp_streamable_http_complete_http_error
    mcp_streamable_http_send_request_http_error
    mcp_streamable_http_send_notification_params
    mcp_streamable_http_notification_response_wrong_status
    mcp_streamable_http_notification_response_body
    mcp_streamable_http_notification_callback_error
    mcp_streamable_http_logging_notification_null_data
    mcp_streamable_http_logging_notification_missing_data
    mcp_streamable_http_logging_notification_invalid_level
    mcp_streamable_http_logging_notification_array_params
    mcp_streamable_http_resource_updated_notification
    mcp_streamable_http_resource_updated_missing_uri
    mcp_streamable_http_resource_updated_array_params
    mcp_streamable_http_server_ping_array_params
    mcp_streamable_http_server_request_unsupported
    mcp_streamable_http_server_ping_request
    mcp_streamable_http_session_recovery
    mcp_streamable_http_session_recovery_preserves_request_id
    mcp_streamable_http_session_recovery_chunked_result
    mcp_streamable_http_initialized_failure_retries
    mcp_streamable_http_server_error_no_recovery
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
    http_create_response_upload_setup_failure
    http_create_response_output_validation
    chatgpt_auth_refresh_retry
    chatgpt_auth_invalid_refresh_retryable
    chatgpt_auth_refresh_http_timeout
    chatgpt_auth_save_failure_preserves_file
    chatgpt_auth_expired_refreshes_before_request
    chatgpt_auth_reloads_changed_file_before_refresh
    chatgpt_auth_rejects_changed_file_account
    chatgpt_auth_stream_refresh_retry
    chatgpt_auth_default_path
    chatgpt_auth_blob_store
    chatgpt_auth_open_default_path
    chatgpt_auth_client_defaults_to_codex_backend
    chatgpt_auth_client_custom_allocator_owns_token
    chatgpt_auth_agent_uses_client_history
    chatgpt_login_authorize_url
    chatgpt_login_browser_helper
    chatgpt_login_callback_validation
    chatgpt_login_callback_exchange
    chatgpt_login_exchange_http_timeout
    chatgpt_login_default_path_write
    chatgpt_auth_rejects_partial_allocator
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
    global_skills_catalog
    smith_profile
    smith_review_runtime
    smith_review_parent_handoff
    agent_runtime_subagent
    agent_runtime_poll_only_subagent
    agent_runtime_review_subagent
    smith_review_pause_checkpoint_failure
    agent_runtime_lifecycle
    agent_runtime_terminal_callback_close
    agent_runtime_queued_turns
    agent_runtime_semantic_events
    agent_runtime_poll_only
    agent_runtime_host_goal_controls
    agent_runtime_goal_budget
    agent_local_session_store
    agent_local_session_store_rejects_unsafe_scope
    agent_runtime_resume
    agent_runtime_markdown_export
    goal_tools
    goal_create_allocation_failure
    patch_tool
    agent_tool_declarations
    agent_tool_manual_step
    agent_auto_compaction
    http_response_limit
    agent_tool_auto_run
    agent_custom_tool_auto_run
    agent_custom_tool_client_history_auto_run
    agent_client_history_tool_auto_run
    agent_client_history_tool_callback_durability
    agent_view_image_auto_run
    revgeo_tool
    todo_file_store_initializes_under_lock
    todo_tool
    todo_callback_store
    searxng_registry_tool
    terminal_registration_rollback
    terminal_tools
    terminal_capture_truncation
    terminal_concurrent_start
    terminal_concurrent_operation
    exec_tool
    read_tool
    view_image_tool
    agent_searxng_tool_auto_run
    agent_multi_tool_auto_run
    agent_tool_auto_round_limit
    agent_tool_auto_default_unbounded
    agent_tool_error_output_continues
    agent_tool_output_max_bytes
    conversations
    stream_response_text
    stream_upload_setup_failure
    stream_responses_websocket
    stream_responses_websocket_large_frame
    stream_responses_websocket_transient_retry
    stream_responses_websocket_midstream_drop
    stream_responses_websocket_https_fallback
    stream_responses_websocket_upgrade_failure_no_fallback
    stream_responses_websocket_no_fallback_after_output
    stream_responses_websocket_no_fallback_after_sink_failure
    stream_responses_websocket_fallback_disabled_before_output
    stream_responses_websocket_https_fallback_disabled
    stream_responses_websocket_fragmented_event
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
    session_stream_auto_custom_empty_tool_run
    session_stream_auto_tool_error_output_continues
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
    LABELS "unit;local;memory-external-boundary"
    TIMEOUT 3)
  set_tests_properties(
    cai_tests.model_capabilities
    cai_tests.env_precedence
    cai_tests.source_sink
    cai_tests.lonejson_nested_mapped_array_stream
    cai_tests.client_open
    cai_tests.mike_mind_prompt_contract
    PROPERTIES
      LABELS "unit;local;valgrind-smoke"
      TIMEOUT 3)
  set_tests_properties(cai_tests.mcp_handler PROPERTIES
    LABELS "unit;local;memory-external-boundary"
    TIMEOUT 3)
  set_tests_properties(cai_tests.response_json PROPERTIES
    LABELS "unit;local;memory-external-boundary"
    TIMEOUT 3)
  set_tests_properties(cai_tests.response_spooled_request_arrays PROPERTIES
    LABELS "unit;local;memory-external-boundary"
    TIMEOUT 3)
  set_tests_properties(cai_tests.response_array_serialization_invariants PROPERTIES
    LABELS "unit;local;memory-external-boundary"
    TIMEOUT 3)
  set_tests_properties(cai_tests.tool_registry PROPERTIES
    LABELS "unit;local;memory-external-boundary"
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
  if(CAI_ENABLE_VALGRIND_TESTS)
    add_test(NAME cai_tests.valgrind
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_valgrind.sh"
        $<TARGET_FILE:cai_tests>
        model_capabilities
        env_precedence
        source_sink
        lonejson_nested_mapped_array_stream
        client_open
        mike_mind_prompt_contract
        patch_tool)
    set_tests_properties(cai_tests.valgrind PROPERTIES
      LABELS "valgrind;local;memory-check"
      TIMEOUT 180)
  endif()
  add_test(NAME cai_receiver_usage_test
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/receiver_usage_test.sh"
      "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(cai_receiver_usage_test PROPERTIES
    LABELS "unit;local"
    TIMEOUT 3)
  add_test(NAME cai_makefile_targets_test
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/makefile_targets_test.sh"
      "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(cai_makefile_targets_test PROPERTIES
    LABELS "unit;local"
    TIMEOUT 3)
  add_test(NAME cai_e2e_service_ownership_test
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e_service_ownership_test.sh"
      "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(cai_e2e_service_ownership_test PROPERTIES
    LABELS "unit;local"
    TIMEOUT 3)
  add_test(NAME cai_mcp_everything_live_wrapper_test
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_everything_live_wrapper_test.sh"
      "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(cai_mcp_everything_live_wrapper_test PROPERTIES
    LABELS "unit;local"
    TIMEOUT 3)
  add_test(NAME cai_release_live_gate_test
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_live_gate_test.sh"
      "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(cai_release_live_gate_test PROPERTIES
    LABELS "unit;local;packaging"
    TIMEOUT 3)
  add_test(NAME cai_mcp_client_source_contract_test
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_client_source_contract_test.sh"
      "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(cai_mcp_client_source_contract_test PROPERTIES
    LABELS "unit;local"
    TIMEOUT 3)
  add_test(NAME cai_smith_prompt_asset_test
    COMMAND "${CMAKE_COMMAND}"
      "-DCAI_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/smith_prompt_asset_test.cmake")
  set_tests_properties(cai_smith_prompt_asset_test PROPERTIES
    LABELS "unit;local"
    TIMEOUT 3)

  add_executable(cai_mcp_http_server tests/mcp_http_server.c)
  target_include_directories(cai_mcp_http_server PRIVATE
    "${CAI_PUBLIC_INCLUDE_DIR}"
    "${CAI_GENERATED_INCLUDE_DIR}"
    "${CAI_LONEJSON_INCLUDE_DIR}")
  cai_link_internal_consumer(cai_mcp_http_server)
  cai_configure_c_target(cai_mcp_http_server)

  add_executable(cai_mcp_http_smoke tests/mcp_http_smoke.c)
  cai_configure_c_target(cai_mcp_http_smoke)
  add_test(NAME cai_mcp_http_smoke
    COMMAND cai_mcp_http_smoke $<TARGET_FILE:cai_mcp_http_server>)
  set_tests_properties(cai_mcp_http_smoke PROPERTIES
    LABELS "smoke;local"
    TIMEOUT 3)
  add_executable(cai_mcp_everything_e2e tests/mcp_everything_e2e.c)
  target_include_directories(cai_mcp_everything_e2e PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  cai_link_internal_consumer(cai_mcp_everything_e2e)
  cai_configure_c_target(cai_mcp_everything_e2e)
  add_executable(cai_mcp_client_e2e_driver tests/mcp_client_e2e.c)
  target_include_directories(cai_mcp_client_e2e_driver PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  cai_link_internal_consumer(cai_mcp_client_e2e_driver)
  cai_configure_c_target(cai_mcp_client_e2e_driver)
  add_test(NAME cai_mcp_client_e2e
    COMMAND /bin/sh
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_client_e2e.sh"
      $<TARGET_FILE:cai_mcp_http_server>
      $<TARGET_FILE:cai_mcp_client_e2e_driver>)
  set_tests_properties(cai_mcp_client_e2e PROPERTIES
    LABELS "e2e;local"
    TIMEOUT 5)
  set(_cai_examples_help_smoke_args
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/examples_help_smoke.sh"
    "${CMAKE_CURRENT_SOURCE_DIR}")
  if(TARGET cai_example_terminal_chat)
    list(APPEND _cai_examples_help_smoke_args
      $<TARGET_FILE:cai_example_terminal_chat>)
  endif()
  if(TARGET cai_example_chatgpt_login)
    list(APPEND _cai_examples_help_smoke_args
      $<TARGET_FILE:cai_example_chatgpt_login>)
  endif()
  if(TARGET cai_example_smith_terminal)
    list(APPEND _cai_examples_help_smoke_args
      $<TARGET_FILE:cai_example_smith_terminal>)
  endif()
  add_test(NAME cai_examples_help_smoke
    COMMAND ${_cai_examples_help_smoke_args})
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
    TIMEOUT 180)

  if(CAI_INSTALL)
    add_test(NAME cai_install_metadata_test
      COMMAND "${CMAKE_COMMAND}"
        "-DCAI_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        "-DCAI_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DCAI_VERSION=${CAI_PACKAGE_VERSION}"
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
        "-DCAI_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
        "-DCAI_CPKT_TARGET_ID=${CPKT_TARGET_ID}"
        "-DCAI_READELF=${CMAKE_READELF}"
        "-DCAI_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
        "-DCAI_TARGET_ID=${CAI_TARGET_ID}"
        "-DCAI_INSTALL_RPATH_TOKEN=${CAI_INSTALL_RPATH_TOKEN}"
        "-DCAI_LONEJSON_ABI_VERSION=${CAI_LONEJSON_ABI_VERSION}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/install_metadata_test.cmake")
    set_tests_properties(cai_install_metadata_test PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 30)
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
      TIMEOUT 10)
    add_test(NAME cai_rpath_token_test
      COMMAND "${CMAKE_COMMAND}"
        "-DCAI_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/rpath_token_test.cmake")
    set_tests_properties(cai_rpath_token_test PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 3)
    add_test(NAME cai_target_tool_discovery_test
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/target_tool_discovery_test.sh"
        "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(cai_target_tool_discovery_test PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 3)
    add_test(NAME cai_darwin_linker_route_test
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/darwin_linker_route_test.sh"
        "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(cai_darwin_linker_route_test PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 3)
    add_test(NAME cai_release_artifacts_darwin_metadata_test
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_artifacts_darwin_metadata_test.sh"
        "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(cai_release_artifacts_darwin_metadata_test PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 3)
    add_test(NAME cai_release_manifest_gate_test
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_manifest_gate_test.sh"
        "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(cai_release_manifest_gate_test PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 3)
    add_test(NAME cai_release_staging_safety_test
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_staging_safety_test.sh"
        "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(cai_release_staging_safety_test PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 3)
    add_test(NAME cai_cmake_presets_test
      COMMAND "${CMAKE_COMMAND}"
        "-DCAI_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DCAI_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake_presets_test.cmake")
    set_tests_properties(cai_cmake_presets_test PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 15)
    if(CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "cpkt" AND
       CMAKE_GENERATOR MATCHES "Ninja")
      add_test(NAME cai_cpkt_link_targets_test
        COMMAND "${CMAKE_COMMAND}"
          "-DCAI_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
          "-DCAI_C_PKT_SYSTEMS_PREFIX=${CAI_C_PKT_SYSTEMS_PREFIX}"
          "-DCAI_TARGET_ID=${CAI_TARGET_ID}"
          "-DCAI_BUILD_SHARED=${CAI_BUILD_SHARED}"
          "-DCAI_BUILD_STATIC=${CAI_BUILD_STATIC}"
          "-DCAI_BUILD_LUA=${CAI_BUILD_LUA}"
          "-DCAI_BUILD_EXAMPLES=${CAI_BUILD_EXAMPLES}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cpkt_link_targets_test.cmake")
      set_tests_properties(cai_cpkt_link_targets_test PROPERTIES
        LABELS "packaging;local"
        TIMEOUT 3)
    endif()
    set(CAI_NESTED_CONFIGURE_TOOLCHAIN_ARGS "")
    if(DEFINED CMAKE_TOOLCHAIN_FILE AND NOT CMAKE_TOOLCHAIN_FILE STREQUAL "")
      list(APPEND CAI_NESTED_CONFIGURE_TOOLCHAIN_ARGS
        "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}")
    endif()
    if(DEFINED CPKT_TARGET_ID AND NOT CPKT_TARGET_ID STREQUAL "")
      list(APPEND CAI_NESTED_CONFIGURE_TOOLCHAIN_ARGS
        "-DCPKT_TARGET_ID=${CPKT_TARGET_ID}")
    endif()
    if(DEFINED CAI_TARGET_ID AND NOT CAI_TARGET_ID STREQUAL "")
      list(APPEND CAI_NESTED_CONFIGURE_TOOLCHAIN_ARGS
        "-DCAI_TARGET_ID=${CAI_TARGET_ID}")
    endif()

    add_test(NAME cai_c_integration_config_without_lua
      COMMAND "${CMAKE_COMMAND}"
        -S "${CMAKE_CURRENT_SOURCE_DIR}"
        -B "${CMAKE_CURRENT_BINARY_DIR}/c-integration-without-lua"
        "-G${CMAKE_GENERATOR}"
        ${CAI_NESTED_CONFIGURE_TOOLCHAIN_ARGS}
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
      TIMEOUT 10)
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
    add_test(NAME cai_lua_mcp_client_e2e
      COMMAND /bin/sh
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua_mcp_client_e2e.sh"
        $<TARGET_FILE:cai_mcp_http_server>
        "${CAI_LUA_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/e2e_mcp_client.lua")
    set_tests_properties(cai_lua_mcp_client_e2e PROPERTIES
      LABELS "smoke;local"
      ENVIRONMENT "${CAI_LUA_TEST_ENV}"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT 5)
  endif()

  if(CAI_BUILD_INTEGRATION_TESTS)
    set(CAI_INTEGRATION_TIMEOUT_SHORT 10)
    set(CAI_INTEGRATION_TIMEOUT_MEDIUM 30)
    set(CAI_INTEGRATION_TIMEOUT_LONG 240)
    # Smith review has separate coding, review, and handoff turns (240 + 180
    # + 240 seconds) and needs cleanup headroom beyond one long turn.
    set(CAI_INTEGRATION_TIMEOUT_SMITH_REVIEW 720)
    add_executable(cai_integration_tests tests/integration_main.c)
    target_include_directories(cai_integration_tests PRIVATE
      "${CAI_PUBLIC_INCLUDE_DIR}"
      "${CAI_GENERATED_INCLUDE_DIR}"
      "${CAI_LONEJSON_INCLUDE_DIR}"
      "${CAI_PSLOG_INCLUDE_DIR}"
      ${CURL_INCLUDE_DIRS})
    cai_link_internal_consumer(cai_integration_tests)
    cai_configure_c_target(cai_integration_tests)
    add_test(NAME cai_integration_tests COMMAND cai_integration_tests)
    set_tests_properties(cai_integration_tests PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_todo_workflow
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_TODO_WORKFLOW=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_todo_workflow PROPERTIES
      LABELS "integration;offline"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_SHORT})
    add_test(NAME cai_integration_chatgpt_e2e
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_E2E=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_e2e PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_LONG})
    add_test(NAME cai_integration_chatgpt_auth_rejection
      COMMAND /bin/sh
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/chatgpt_auth_failure_test.sh"
        c
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_auth_rejection PROPERTIES
      LABELS "integration;offline"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_SHORT})
    add_test(NAME cai_integration_chatgpt_auth_ownership
      COMMAND /bin/sh
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/chatgpt_auth_failure_test.sh"
        ownership-c
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_auth_ownership PROPERTIES
      LABELS "integration;offline"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_SHORT})
    add_test(NAME cai_integration_chatgpt_usage_limits
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_USAGE_LIMITS=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_usage_limits PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_chatgpt_defaults
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_DEFAULTS=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_defaults PROPERTIES
      LABELS "integration;offline"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_SHORT})
    add_test(NAME cai_integration_provider_retry_classifier
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_PROVIDER_RETRY_CLASSIFIER=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_provider_retry_classifier
      PROPERTIES
      LABELS "integration;offline"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_SHORT})
    add_test(NAME cai_integration_chatgpt_subscription_e2e
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_SUBSCRIPTION_E2E=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_subscription_e2e PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_LONG})
    add_test(NAME cai_integration_chatgpt_smith_e2e
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_SMITH_E2E=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_smith_e2e PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_LONG})
    add_test(NAME cai_integration_chatgpt_smith_goal_e2e
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_SMITH_GOAL_E2E=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_smith_goal_e2e PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_LONG})
    add_test(NAME cai_integration_chatgpt_smith_subagents_e2e
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_SMITH_SUBAGENTS_E2E=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_smith_subagents_e2e PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_SMITH_REVIEW})
    add_test(NAME cai_integration_chatgpt_smith_review_e2e
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_SMITH_REVIEW_E2E=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_smith_review_e2e PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_SMITH_REVIEW})
    add_test(NAME cai_integration_chatgpt_smith_mcp_review_e2e
      COMMAND /bin/sh
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/smith_mcp_live_e2e.sh"
        $<TARGET_FILE:cai_mcp_http_server>
        "${CMAKE_COMMAND}" -E env
          "CAI_INTEGRATION_CHATGPT_SMITH_MCP_REVIEW_E2E=1"
          $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_smith_mcp_review_e2e PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_SMITH_REVIEW})
    add_test(NAME cai_integration_chatgpt_state_restore
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_STATE_RESTORE=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_state_restore PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_chatgpt_compaction
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_COMPACTION=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_compaction PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_chatgpt_tool_security
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_TOOL_SECURITY=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_tool_security PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_chatgpt_hosted_web_search
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_HOSTED_WEB_SEARCH=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_hosted_web_search PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_LONG})
    add_test(NAME cai_integration_chatgpt_mcp_client_tool
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_mcp_everything_live_test.sh"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_mcp_client_tool PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_LONG})
    add_test(NAME cai_integration_chatgpt_responses_websocket_e2e
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_RESPONSES_WEBSOCKET_E2E=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_responses_websocket_e2e PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_LONG})
    add_test(NAME cai_integration_chatgpt_exec_tool
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_EXEC_TOOL=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_exec_tool PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_chatgpt_read_tool
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_READ_TOOL=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_read_tool PROPERTIES
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
      add_test(NAME cai_lua_chatgpt_auth_rejection
        COMMAND /bin/sh
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/chatgpt_auth_failure_test.sh"
          lua
          "${CAI_LUA_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/e2e_usage_limits.lua")
      set_tests_properties(cai_lua_chatgpt_auth_rejection PROPERTIES
        LABELS "integration;offline"
        ENVIRONMENT "${CAI_LUA_TEST_ENV}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        TIMEOUT ${CAI_INTEGRATION_TIMEOUT_SHORT})
      add_test(NAME cai_lua_chatgpt_auth_ownership
        COMMAND /bin/sh
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/chatgpt_auth_failure_test.sh"
          ownership-lua
          "${CAI_LUA_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/e2e_usage_limits.lua")
      set_tests_properties(cai_lua_chatgpt_auth_ownership PROPERTIES
        LABELS "integration;offline"
        ENVIRONMENT "${CAI_LUA_TEST_ENV}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        TIMEOUT ${CAI_INTEGRATION_TIMEOUT_SHORT})
      add_test(NAME cai_lua_chatgpt_tool_stream_e2e
        COMMAND "${CAI_LUA_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/e2e_tool_stream.lua")
      set_tests_properties(cai_lua_chatgpt_tool_stream_e2e PROPERTIES
        LABELS integration
        ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_LUA_TOOL_STREAM_E2E=1"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
      add_test(NAME cai_lua_chatgpt_session_continuity_e2e
        COMMAND "${CAI_LUA_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/e2e_session_continuity.lua")
      set_tests_properties(cai_lua_chatgpt_session_continuity_e2e PROPERTIES
        LABELS integration
        ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_LUA_SESSION_E2E=1"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
      add_test(NAME cai_lua_chatgpt_usage_limits_e2e
        COMMAND "${CAI_LUA_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/e2e_usage_limits.lua")
      set_tests_properties(cai_lua_chatgpt_usage_limits_e2e PROPERTIES
        LABELS integration
        ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_LUA_USAGE_LIMITS_E2E=1"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
      add_test(NAME cai_lua_chatgpt_hosted_web_search_e2e
        COMMAND "${CAI_LUA_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/e2e_hosted_web_search.lua")
      set_tests_properties(cai_lua_chatgpt_hosted_web_search_e2e PROPERTIES
        LABELS integration
        ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_LUA_HOSTED_WEB_SEARCH_E2E=1"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    endif()
    if(CAI_BUILD_LUA)
      add_test(NAME cai_lua_smith_e2e
        COMMAND "/bin/sh"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua_rock_e2e.sh"
          "${CMAKE_CURRENT_SOURCE_DIR}"
          "${CAI_LUA_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/e2e_smith.lua")
      set_tests_properties(cai_lua_smith_e2e PROPERTIES
        LABELS integration
        ENVIRONMENT "${CAI_LUA_SMITH_E2E_ENV}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        TIMEOUT ${CAI_INTEGRATION_TIMEOUT_SMITH_REVIEW})
      add_test(NAME cai_lua_smith_mcp_e2e
        COMMAND "/bin/sh"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/smith_mcp_live_e2e.sh"
          $<TARGET_FILE:cai_mcp_http_server>
          "/bin/sh"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua_rock_e2e.sh"
          "${CMAKE_CURRENT_SOURCE_DIR}"
          "${CAI_LUA_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/e2e_smith.lua")
      set_tests_properties(cai_lua_smith_mcp_e2e PROPERTIES
        LABELS integration
        ENVIRONMENT "${CAI_LUA_SMITH_E2E_ENV}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        TIMEOUT ${CAI_INTEGRATION_TIMEOUT_SMITH_REVIEW})
    endif()
    if(CAI_REGISTER_API_KEY_EXAMPLE_SMOKE AND
        TARGET cai_example_basic_response AND
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
        LABELS "integration;example-smoke;api-key"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        TIMEOUT ${CAI_INTEGRATION_TIMEOUT_LONG})
    endif()
  endif()
endif()

if(CAI_BUILD_FUZZERS)
  if(NOT TARGET cai_static)
    message(FATAL_ERROR "CAI_BUILD_FUZZERS requires CAI_BUILD_STATIC=ON.")
  endif()
  function(cai_add_fuzzer target source corpus_relpath)
    add_executable(${target} ${source} tests/fuzz_driver.c)
    target_include_directories(${target} PRIVATE
      "${CAI_PUBLIC_INCLUDE_DIR}"
      "${CAI_GENERATED_INCLUDE_DIR}"
      "${CAI_LONEJSON_INCLUDE_DIR}"
      "${CAI_PSLOG_INCLUDE_DIR}"
      ${CURL_INCLUDE_DIRS}
      "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(${target} PRIVATE cai_static)
    cai_configure_c_target(${target})
    set(corpus_dir "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz-corpus/${corpus_relpath}")
    add_test(NAME ${target}.smoke
      COMMAND $<TARGET_FILE:${target}> "${corpus_dir}")
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
  cai_add_fuzzer(cai_patch_fuzz tests/patch_fuzz.c patch)
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
  set(CAI_C_PKT_SYSTEMS_URL_FOR_PACKAGE "host")
  set(CAI_C_PKT_SYSTEMS_ARCHIVE_NAME_FOR_PACKAGE "host")
  if(CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "cpkt")
    set(CAI_C_PKT_SYSTEMS_URL_FOR_PACKAGE "${CAI_C_PKT_SYSTEMS_URL}")
    get_filename_component(CAI_C_PKT_SYSTEMS_ARCHIVE_NAME_FOR_PACKAGE
      "${CAI_C_PKT_SYSTEMS_URL}" NAME)
  endif()
  set(CAI_LONEJSON_PKG_URL_FOR_PACKAGE "host")
  set(CAI_LONEJSON_ARCHIVE_NAME_FOR_PACKAGE "host")
  if(CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "cpkt")
    set(CAI_LONEJSON_PKG_URL_FOR_PACKAGE "${CAI_LONEJSON_PKG_URL}")
    get_filename_component(CAI_LONEJSON_ARCHIVE_NAME_FOR_PACKAGE
      "${CAI_LONEJSON_PKG_URL}" NAME)
  endif()
  set(CAI_PSLOG_PKG_URL_FOR_PACKAGE "host")
  set(CAI_PSLOG_ARCHIVE_NAME_FOR_PACKAGE "host")
  if(CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "cpkt")
    set(CAI_PSLOG_PKG_URL_FOR_PACKAGE "${CAI_PSLOG_PKG_URL}")
    get_filename_component(CAI_PSLOG_ARCHIVE_NAME_FOR_PACKAGE
      "${CAI_PSLOG_PKG_URL}" NAME)
  endif()
  set(CAI_PACKAGE_REQUIRES
      "lonejson lonejson-curl lonejson-oidc lonejson-openssl")
  if(CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "cpkt")
    set(CAI_PACKAGE_REQUIRES_PRIVATE
        "libcurl >= ${CAI_MINIMUM_CURL_VERSION}, openssl")
    set(CAI_PC_DEPENDENCY_CFLAGS "")
    set(CAI_PC_DEPENDENCY_LIBS_PRIVATE "")
    set(CAI_CONFIG_LONEJSON_INCLUDE_HINT "")
    set(CAI_CONFIG_PSLOG_INCLUDE_HINT "")
    set(CAI_CONFIG_LONEJSON_LIBRARY_HINT "")
  else()
    set(CAI_PACKAGE_REQUIRES_PRIVATE
        "libcurl >= ${CAI_MINIMUM_CURL_VERSION}, openssl")
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
  if(NOT DEFINED CAI_PSLOG_PKG_SHA256_FOR_PACKAGE OR
     CAI_PSLOG_PKG_SHA256_FOR_PACKAGE STREQUAL "")
    set(CAI_PSLOG_PKG_SHA256_FOR_PACKAGE "host")
  endif()
  configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cai-config.cmake"
    INSTALL_DESTINATION "${CAI_INSTALL_CMAKEDIR}")
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai-config-version.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cai-config-version.cmake"
    @ONLY)
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
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai-dependencies.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/dependencies.json"
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
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/dependencies.json"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/cai")

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
      "-DCAI_VERSION=${CAI_PACKAGE_VERSION}"
      "-DCAI_TARGET_ID=${CAI_TARGET_ID}"
      "-DCAI_STRIP_BIN=${CMAKE_STRIP}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/package_archive.cmake"
    DEPENDS ${_cai_package_deps}
    VERBATIM)
  add_custom_target(cai_package_source
    COMMAND "${CMAKE_COMMAND}"
      "-DCAI_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DCAI_DIST_DIR=${CAI_DIST_DIR}"
      "-DCAI_VERSION=${CAI_PACKAGE_VERSION}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/package_source.cmake"
    VERBATIM)
  add_custom_target(cai_package_checksums
    COMMAND "${CMAKE_COMMAND}"
      "-DCAI_DIST_DIR=${CAI_DIST_DIR}"
      "-DCAI_VERSION=${CAI_PACKAGE_VERSION}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/package_checksums.cmake"
    DEPENDS cai_package_archive cai_package_source
    VERBATIM)
endif()
