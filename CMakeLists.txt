cmake_minimum_required(VERSION 3.21)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CaiVersion)
include(CaiLonejsonAbi)
include(cai-export-policy)
include(cai-rpath)
include(CaiWorkspace)

cai_detect_version(CAI_RESOLVED_VERSION "${CMAKE_CURRENT_SOURCE_DIR}")
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" CAI_CMAKE_PROJECT_VERSION
       "${CAI_RESOLVED_VERSION}")
if(CAI_CMAKE_PROJECT_VERSION STREQUAL "")
  message(FATAL_ERROR
    "cai release version must begin with major.minor.patch: "
    "${CAI_RESOLVED_VERSION}")
endif()
set(CAI_PACKAGE_VERSION "${CAI_RESOLVED_VERSION}")
set(CAI_DEPENDENCY_MODE "cpkt" CACHE STRING "Dependency mode: cpkt, host, or auto.")
set_property(CACHE CAI_DEPENDENCY_MODE PROPERTY STRINGS cpkt host auto)
if(NOT CAI_DEPENDENCY_MODE MATCHES "^(cpkt|host|auto)$")
  message(FATAL_ERROR "CAI_DEPENDENCY_MODE must be one of: cpkt, host, auto.")
endif()
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchains/native-lifecycle.cmake"
      CACHE FILEPATH "Pinned native lifecycle compiler collection")
endif()
project(cai VERSION ${CAI_CMAKE_PROJECT_VERSION} LANGUAGES C)

include(CMakePackageConfigHelpers)
include(CTest)
include(GNUInstallDirs)
include(cai-pkgconfig)
cai_pkgconfig_prefix_from_libdir("${CMAKE_INSTALL_LIBDIR}"
  CAI_PC_PREFIX_FROM_PKGCONFIG)

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
option(CAI_BUILD_CLI "Build the standalone cai terminal agent." OFF)
option(CAI_BUILD_APP "Build the embeddable CAI terminal application." OFF)
if(CAI_BUILD_CLI)
  set(CAI_BUILD_APP ON)
endif()
option(CAI_BUILD_LUA "Build the Lua 5.5 binding and Lua tests." OFF)
option(CAI_BUILD_FUZZERS "Build fuzz harnesses." OFF)
option(CAI_ENABLE_COVERAGE "Build tests with coverage instrumentation." OFF)
option(CAI_ENABLE_VALGRIND_TESTS "Register native Valgrind memory-check tests." OFF)
option(CAI_INSTALL "Enable install rules." ON)

set(CAI_VERSION_OVERRIDE "" CACHE STRING "Override detected cai package version.")
set(CAI_DIST_DIR "${CMAKE_BINARY_DIR}/dist" CACHE PATH "Distribution output directory.")
set(CAI_TARGET_ID "" CACHE STRING "Release archive target id.")
set(CAI_CPKT_VERSION "0.6.1" CACHE STRING
    "Pinned cpkt release used for curl/native dependencies." FORCE)
# Consumer compatibility is independent of the producer's exact archive pin.
set(CAI_MINIMUM_CPKT_VERSION "0.3.0")
# ABI 4 is the latest released ABI. Values 3 and 5 through 7 were only used by
# unreleased development branches and must not survive a reconfigure. Preserve
# any other packager-selected ABI override.
if(NOT DEFINED CAI_ABI_VERSION OR CAI_ABI_VERSION STREQUAL "" OR
   CAI_ABI_VERSION STREQUAL "3" OR CAI_ABI_VERSION STREQUAL "4" OR
   CAI_ABI_VERSION STREQUAL "5" OR CAI_ABI_VERSION STREQUAL "6" OR
   CAI_ABI_VERSION STREQUAL "7")
  set(CAI_ABI_VERSION "5" CACHE STRING "Shared library ABI/SOVERSION." FORCE)
else()
  # Preserve an explicit packager value while giving its cache entry the
  # project-defined type and help text.
  set(CAI_ABI_VERSION "${CAI_ABI_VERSION}" CACHE STRING
      "Shared library ABI/SOVERSION." FORCE)
endif()
set(CAI_MINIMUM_CURL_VERSION "7.86.0")
set(CAI_LONEJSON_VERSION "0.45.0")
set(CAI_LONEJSON_ABI_VERSION "26")
set(CAI_PSLOG_VERSION "0.12.0")
set(CAI_SOFTLINE_VERSION "0.15.0")
set(CAI_LIBMDF_VERSION "0.17.0")
set(CAI_LOCKDC_VERSION "0.23.0")
set(CAI_LIBLQL_VERSION "0.4.0")

if(CAI_BUILD_APP AND NOT CAI_BUILD_STATIC)
  message(FATAL_ERROR "The CAI application requires CAI_BUILD_STATIC=ON")
endif()

if(NOT CAI_BUILD_STATIC AND NOT CAI_BUILD_SHARED)
  message(FATAL_ERROR "Enable at least one of CAI_BUILD_STATIC or CAI_BUILD_SHARED.")
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

if(CAI_BUILD_APP AND (NOT CAI_TARGET_ID MATCHES "^(x86_64|aarch64|armhf)-linux-(gnu|musl)$" OR
                      NOT CAI_DEPENDENCY_MODE STREQUAL "cpkt"))
  message(FATAL_ERROR
    "The CAI application requires Linux x86_64, aarch64 or armhf, glibc or musl, and cpkt dependencies")
endif()
if(CAI_BUILD_CLI AND NOT CAI_TARGET_ID MATCHES "^(x86_64|aarch64|armhf)-linux-(gnu|musl)$")
  message(FATAL_ERROR "The standalone cai CLI requires Linux x86_64, aarch64 or armhf")
endif()

function(cai_cpkt_sha256 target_id out_var)
  if(target_id STREQUAL "x86_64-linux-gnu")
    set(_sha256 "99625b7889f78b6e467d38e7dbec08d9d0ed6f599ff4571244060ce67c6ca62f")
  elseif(target_id STREQUAL "x86_64-linux-musl")
    set(_sha256 "f4b1b0b2b257d2347be7d2b437dbb84891c19e39067bba24464b415dd76eb67e")
  elseif(target_id STREQUAL "aarch64-linux-gnu")
    set(_sha256 "3c5f172a50b7df7e37e99dbf52014d3b09114cbc143339f1109c23715fa117a1")
  elseif(target_id STREQUAL "aarch64-linux-musl")
    set(_sha256 "bf39981f557b1ed10940ee376bb81d0fa734d3d290e5d95cbc4f786ac466cb93")
  elseif(target_id STREQUAL "armhf-linux-gnu")
    set(_sha256 "c2200e3a4a05ed8807d0bb19b0042179bd47c7ce83174932f521d36aee11b935")
  elseif(target_id STREQUAL "armhf-linux-musl")
    set(_sha256 "eb1ed255f3ea60c82c3973f1246571872b4f0fd7866364e544b5627e5cd3ee98")
  elseif(target_id STREQUAL "arm64-apple-darwin")
    set(_sha256 "8f32fe4a9e477196d75a927af329dafcc91eda5e4b505012a7319edcf92e8579")
  else()
    message(FATAL_ERROR "No pinned cpkt checksum for target ${target_id}")
  endif()
  set(${out_var} "${_sha256}" PARENT_SCOPE)
endfunction()

function(cai_lonejson_pkg_sha256 target_id out_var)
  if(target_id STREQUAL "x86_64-linux-gnu")
    set(_sha256 "aef11c4511bb6aa8ed4e0646204e14b7934affd6e5cec5b8e8bff36e80217826")
  elseif(target_id STREQUAL "x86_64-linux-musl")
    set(_sha256 "823efad9719011af0602cefacf0506ac603bfb54d5986883fcabed60db93544b")
  elseif(target_id STREQUAL "aarch64-linux-gnu")
    set(_sha256 "659cc67e2c26c9615f84fa6f1c26fae4a3a4b00980d8bbb02c04625b2facaf7e")
  elseif(target_id STREQUAL "aarch64-linux-musl")
    set(_sha256 "96e020ce3dab9de5dc13a64e83a278b7519cdf7fcc2d3c93fdfc740fe5fb0cd5")
  elseif(target_id STREQUAL "armhf-linux-gnu")
    set(_sha256 "ab2f89eb4054fbb1b4cd85b6be5d9d2005b364459ba362b910bc35ac1c6511b5")
  elseif(target_id STREQUAL "armhf-linux-musl")
    set(_sha256 "8b103a732181b06d90fc5f39c074a887969522d29e0d9006b30982f31969982d")
  elseif(target_id STREQUAL "arm64-apple-darwin")
    set(_sha256 "9dc6b277241068a2ab1a737357cf18982c5612ec43280f161044ce719aed50dc")
  else()
    message(FATAL_ERROR
      "No pinned liblonejson package checksum for target ${target_id}")
  endif()
  set(${out_var} "${_sha256}" PARENT_SCOPE)
endfunction()

function(cai_pslog_pkg_sha256 target_id out_var)
  if(target_id STREQUAL "x86_64-linux-gnu")
    set(_sha256 "21239262bc06eecb504e45e0902f374ccd86751dcb32087dd84af4d2553ea0e2")
  elseif(target_id STREQUAL "x86_64-linux-musl")
    set(_sha256 "3a43206260bbf1095a363ffc4ddb5514dac2a621aa030db0e7f9b4364a93bb4d")
  elseif(target_id STREQUAL "aarch64-linux-gnu")
    set(_sha256 "304575fa07f14e5ff4d7614f7c450a3e88a586d5f353352bff8b678ee739da76")
  elseif(target_id STREQUAL "aarch64-linux-musl")
    set(_sha256 "72a3b4e1f89c339cadb996137a79db8ee6e67be1f580f482269f6b0088cda115")
  elseif(target_id STREQUAL "armhf-linux-gnu")
    set(_sha256 "993461d481ec6c3912be29f5a0f95bcf5274aae6a1d2d698350e804c688122a9")
  elseif(target_id STREQUAL "armhf-linux-musl")
    set(_sha256 "037fa1d56a01dc8b9fccf54960b8083337cbc5edfaa5f15d715f96413d1f6b77")
  elseif(target_id STREQUAL "arm64-apple-darwin")
    set(_sha256 "a5945f2dc4ad16dd8a71572f40117ddfdfae371d185cb03dab5205cdf780c321")
  else()
    message(FATAL_ERROR
      "No pinned libpslog package checksum for target ${target_id}")
  endif()
  set(${out_var} "${_sha256}" PARENT_SCOPE)
endfunction()

if(NOT CAI_DEPENDENCY_MODE STREQUAL "cpkt")
  # Explicit dependency prefixes may live outside the compiler sysroot.
  list(APPEND CMAKE_FIND_ROOT_PATH ${CMAKE_PREFIX_PATH})
endif()
set(CAI_RESOLVED_DEPENDENCY_MODE "${CAI_DEPENDENCY_MODE}")
if(CAI_DEPENDENCY_MODE STREQUAL "auto")
  find_path(_cai_auto_curl_include curl/curl.h)
  find_library(_cai_auto_curl_library NAMES curl)
  find_path(_cai_auto_lonejson_include lonejson.h)
  find_path(_cai_auto_pslog_include pslog.h)
  find_library(_cai_auto_lonejson_library NAMES lonejson)
  find_library(_cai_auto_pslog_library NAMES pslog)
  foreach(component IN ITEMS Yaml Misc Core)
    find_file(_cai_auto_${component}_config NAMES Cpkt${component}Config.cmake
      PATH_SUFFIXES lib/cmake/Cpkt${component})
  endforeach()
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
     _cai_auto_lonejson_abi_ok AND _cai_auto_Yaml_config AND
     _cai_auto_Misc_config AND _cai_auto_Core_config)
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
  foreach(component IN ITEMS Yaml Misc Core)
    unset(_cai_auto_${component}_config CACHE)
  endforeach()
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

include(CaiAcquisition)

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

if(CAI_BUILD_APP)
  if(CAI_TARGET_ID STREQUAL "x86_64-linux-gnu")
    set(_cai_liblql_sha256 "a64ec9860305e48739e35d4609bd03796de7262ab9ca011755c696c85f8f65d3")
    set(_cai_lockdc_sha256 "f57928ce318d0e08914d41603fadfe22209ae6b6a4b40ae2d5f7b5f4c06bacfa")
    set(_cai_softline_sha256 "f69afc8e16a7bf6d0e7db82fa84743dbc2f346ecea8ac68beb1f5f6a6e4b98d1")
    set(_cai_libmdf_sha256 "7563dcd74b48afad1d7358815d0115c798c1a46219dc7cabdbcbb45e4edac53c")
  elseif(CAI_TARGET_ID STREQUAL "x86_64-linux-musl")
    set(_cai_liblql_sha256 "e2ffbcbb67090046a76d1b3f5a4c151d3c2dbe86ab4bdef472c92f0fd2b46d9d")
    set(_cai_lockdc_sha256 "b3c41e9e3ff97834541051c7c86bf455c8afb02bd054cf9cceaa3d39c84a2f59")
    set(_cai_softline_sha256 "bb918bc5358d76b0c367693d4559ab7e25b344febee46d30a9807a10a2d5a1e8")
    set(_cai_libmdf_sha256 "08e11370793274b3db78ea35b45ee9459f906103487c3345c0188c07ce897f3f")
  elseif(CAI_TARGET_ID STREQUAL "aarch64-linux-gnu")
    set(_cai_liblql_sha256 "c092e0dc6aad961f787e9388899e508736946271e94560baf676d5f1bc46c769")
    set(_cai_lockdc_sha256 "39b0f524fcd3b30dbbd2006cfdd595e080e085d3e7a064da77bc6f6f28ae5f21")
    set(_cai_softline_sha256 "c7a4b2c93999b9845a324c47b99ae13dc5c129047854f5c3d51e030fd6c21cd2")
    set(_cai_libmdf_sha256 "366e5b9e1cb0bae99a07a367d6b92e67aa616139cda59298446b8e4661cf50f0")
  elseif(CAI_TARGET_ID STREQUAL "aarch64-linux-musl")
    set(_cai_liblql_sha256 "2cdc421ee7011c50236ba1219ae93c9afe141618a9599e51c823e7528c438143")
    set(_cai_lockdc_sha256 "252b52cc306048846d062e34d66e60773f49f9e4d33f14d2747adf6bc5ee2746")
    set(_cai_softline_sha256 "d2476ee51c69f58c17453c46a812caa9c0a3b23de665b6b39b0519803dc6b7d2")
    set(_cai_libmdf_sha256 "348c544a44b98ff9b4762c9ef4e9095f5e03817e99c134da6afbc62a2d0fb0bc")
  elseif(CAI_TARGET_ID STREQUAL "armhf-linux-gnu")
    set(_cai_liblql_sha256 "de7e7f901d2aa08ecf7c5f1b196cad0fd57f0b215a883d4ad4d906fa81aa0047")
    set(_cai_lockdc_sha256 "8d23b08946740e5c4a32bc87c8f5c999aa354d0a0be0f950ca737b14d93b1818")
    set(_cai_softline_sha256 "58f75adccb60b38e831423b1b08a83b32a5a4f5a209ad215fecc4c7c279dae4a")
    set(_cai_libmdf_sha256 "6aa7e024b2378ed1a7cf3ddab02f836d3c448d1d83e86eeb7b7cc0fdaaabe424")
  elseif(CAI_TARGET_ID STREQUAL "armhf-linux-musl")
    set(_cai_liblql_sha256 "cb55af9e0cfca11eda67fd63c4b6a73df52b08ea4c57d03472531e9dc436d7d6")
    set(_cai_lockdc_sha256 "bcee038dec683d0d3e96acd172a82b52000d9eb44810f43c70125e008e8005f0")
    set(_cai_softline_sha256 "0240d348a6f6b6dbf098bb9cb08fcd8b0c4fbb4a5ec5776059e579e4e2a215c3")
    set(_cai_libmdf_sha256 "3c56e55e115c939496f088f3f5f0ff8fb6ac229bed40da643239fde554d5f47b")
  endif()
  set(_cai_liblql_name "liblql-${CAI_LIBLQL_VERSION}-${CAI_TARGET_ID}")
  set(_cai_liblql_prefix "${CAI_DEPS_DIR}/${_cai_liblql_name}")
  cai_dependency_archive_path("${_cai_liblql_name}.tar.gz"
    "${_cai_liblql_sha256}"
    _cai_liblql_archive)
  cai_download_tarball("liblql ${CAI_LIBLQL_VERSION} ${CAI_TARGET_ID}"
    "https://github.com/sa6mwa/liblql/releases/download/v${CAI_LIBLQL_VERSION}/${_cai_liblql_name}.tar.gz"
    "${_cai_liblql_archive}" "${_cai_liblql_prefix}"
    "${_cai_liblql_sha256}"
    "${_cai_liblql_prefix}/include/lql/lql.h")
  set(_cai_lockdc_name "liblockdc-${CAI_LOCKDC_VERSION}-${CAI_TARGET_ID}")
  set(_cai_lockdc_prefix "${CAI_DEPS_DIR}/${_cai_lockdc_name}")
  cai_dependency_archive_path("${_cai_lockdc_name}.tar.gz"
    "${_cai_lockdc_sha256}"
    _cai_lockdc_archive)
  cai_download_tarball("liblockdc ${CAI_LOCKDC_VERSION} ${CAI_TARGET_ID}"
    "https://github.com/sa6mwa/liblockdc/releases/download/v${CAI_LOCKDC_VERSION}/${_cai_lockdc_name}.tar.gz"
    "${_cai_lockdc_archive}" "${_cai_lockdc_prefix}"
    "${_cai_lockdc_sha256}"
    "${_cai_lockdc_prefix}/include/lc/lc.h")
  list(PREPEND CMAKE_PREFIX_PATH "${_cai_lockdc_prefix}" "${_cai_liblql_prefix}")
  list(APPEND CMAKE_FIND_ROOT_PATH "${_cai_lockdc_prefix}" "${_cai_liblql_prefix}")
  set(lockdc_DIR "${_cai_lockdc_prefix}/lib/cmake/lockdc"
    CACHE PATH "liblockdc package directory" FORCE)
  set(liblql_DIR "${_cai_liblql_prefix}/lib/cmake/liblql"
    CACHE PATH "liblql package directory" FORCE)
  set(_cai_softline_name
      "softline-${CAI_SOFTLINE_VERSION}-${CAI_TARGET_ID}")
  set(_cai_softline_prefix "${CAI_DEPS_DIR}/${_cai_softline_name}")
  cai_dependency_archive_path(
    "${_cai_softline_name}.tar.gz"
    "${_cai_softline_sha256}"
    _cai_softline_archive)
  cai_download_tarball(
    "softline ${CAI_SOFTLINE_VERSION} ${CAI_TARGET_ID}"
    "https://github.com/sa6mwa/softline/releases/download/v${CAI_SOFTLINE_VERSION}/${_cai_softline_name}.tar.gz"
    "${_cai_softline_archive}" "${_cai_softline_prefix}"
    "${_cai_softline_sha256}"
    "${_cai_softline_prefix}/include/softline/softline.h")
  set(_cai_libmdf_name "libmdf-${CAI_LIBMDF_VERSION}-${CAI_TARGET_ID}")
  set(_cai_libmdf_prefix "${CAI_DEPS_DIR}/${_cai_libmdf_name}")
  cai_dependency_archive_path(
    "${_cai_libmdf_name}.tar.gz"
    "${_cai_libmdf_sha256}"
    _cai_libmdf_archive)
  cai_download_tarball(
    "libmdf ${CAI_LIBMDF_VERSION} ${CAI_TARGET_ID}"
    "https://github.com/sa6mwa/libmdf/releases/download/v${CAI_LIBMDF_VERSION}/${_cai_libmdf_name}.tar.gz"
    "${_cai_libmdf_archive}" "${_cai_libmdf_prefix}"
    "${_cai_libmdf_sha256}"
    "${_cai_libmdf_prefix}/include/libmdf/mdf.h")
  list(PREPEND CMAKE_PREFIX_PATH "${_cai_softline_prefix}"
       "${_cai_libmdf_prefix}")
  list(APPEND CMAKE_FIND_ROOT_PATH "${_cai_softline_prefix}"
       "${_cai_libmdf_prefix}")
  set(softline_DIR "${_cai_softline_prefix}/lib/cmake/softline"
      CACHE PATH "softline package directory" FORCE)
  set(libmdf_DIR "${_cai_libmdf_prefix}/lib/cmake/libmdf"
      CACHE PATH "libmdf package directory" FORCE)
  find_package(softline CONFIG REQUIRED)
  find_package(libmdf CONFIG REQUIRED)
endif()

if(CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "cpkt")
  cai_cpkt_sha256("${CAI_TARGET_ID}" CAI_CPKT_SHA256)
  set(CAI_CPKT_NAME
      "cpkt-${CAI_CPKT_VERSION}-${CAI_TARGET_ID}")
  set(CAI_CPKT_URL
      "https://github.com/c89systems/cpkt/releases/download/v${CAI_CPKT_VERSION}/${CAI_CPKT_NAME}.tar.gz")
  cai_dependency_archive_path(
    "${CAI_CPKT_NAME}.tar.gz" "${CAI_CPKT_SHA256}"
    CAI_CPKT_ARCHIVE)
  set(CAI_CPKT_PREFIX
      "${CAI_DEPS_DIR}/${CAI_CPKT_NAME}")
  cai_download_tarball(
    "cpkt ${CAI_CPKT_VERSION} ${CAI_TARGET_ID}"
    "${CAI_CPKT_URL}"
    "${CAI_CPKT_ARCHIVE}"
    "${CAI_CPKT_PREFIX}"
    "${CAI_CPKT_SHA256}"
    "${CAI_CPKT_PREFIX}/include/curl/curl.h")

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
      "${CAI_CPKT_PREFIX};${CAI_LONEJSON_PREFIX};${CAI_PSLOG_PREFIX};${CMAKE_PREFIX_PATH}")
  if(CMAKE_CROSSCOMPILING)
    list(APPEND CMAKE_FIND_ROOT_PATH
         "${CAI_CPKT_PREFIX}"
         "${CAI_LONEJSON_PREFIX}"
         "${CAI_PSLOG_PREFIX}")
  endif()
  set(lonejson_DIR "${CAI_LONEJSON_PREFIX}/lib/cmake/lonejson"
      CACHE PATH "lonejson package directory" FORCE)
  set(pslog_DIR "${CAI_PSLOG_PREFIX}/lib/cmake/pslog"
      CACHE PATH "pslog package directory" FORCE)
  set(CURL_DIR "${CAI_CPKT_PREFIX}/lib/cmake/CURL"
      CACHE PATH "curl package directory" FORCE)
  set(OpenSSL_DIR "${CAI_CPKT_PREFIX}/lib/cmake/OpenSSL"
      CACHE PATH "OpenSSL package directory" FORCE)
  # Keep transitive package discovery in the selected SDK after an upgrade;
  # CMake otherwise retains *_DIR entries from the previous cpkt prefix.
  set(ZLIB_DIR "${CAI_CPKT_PREFIX}/lib/cmake/zlib"
      CACHE PATH "zlib package directory" FORCE)
  set(nghttp2_DIR "${CAI_CPKT_PREFIX}/lib/cmake/nghttp2"
      CACHE PATH "nghttp2 package directory" FORCE)
  set(Libssh2_DIR "${CAI_CPKT_PREFIX}/lib/cmake/libssh2"
      CACHE PATH "libssh2 package directory" FORCE)
  set(CpktCore_DIR "${CAI_CPKT_PREFIX}/lib/cmake/CpktCore"
      CACHE PATH "cpkt core package directory" FORCE)
  find_package(CpktCore ${CAI_CPKT_VERSION} EXACT CONFIG REQUIRED)
  if(NOT CpktCore_TARGET_ID STREQUAL CAI_TARGET_ID)
    message(FATAL_ERROR "cpkt target ${CpktCore_TARGET_ID} does not match ${CAI_TARGET_ID}")
  endif()
  find_package(lonejson CONFIG REQUIRED COMPONENTS curl oidc openssl)
  find_package(pslog CONFIG REQUIRED)
  cai_set_lonejson_from_target()
  cai_set_pslog_from_target()
  if(CAI_BUILD_CLI)
    set(CAI_LONEJSON_LINK lonejson::lonejson_static)
  endif()
  list(APPEND CMAKE_INSTALL_RPATH "${CAI_INSTALL_RPATH_TOKEN}")
  get_filename_component(_cai_lonejson_library_dir
    "${CAI_LONEJSON_LIBRARY}" DIRECTORY)
  list(APPEND CMAKE_BUILD_RPATH
       "${CAI_CPKT_PREFIX}/lib" "${_cai_lonejson_library_dir}")
endif()

include(CaiYaml)

set(CAI_CPKT_SHA256_FOR_PACKAGE "")
set(CAI_PSLOG_PKG_SHA256_FOR_PACKAGE "")
if(CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "cpkt")
  set(CAI_CPKT_SHA256_FOR_PACKAGE "${CAI_CPKT_SHA256}")
  set(CAI_CPKT_PACKAGE_ID_FOR_PACKAGE "${CpktCore_PACKAGE_ID}")
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

file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/lua-dependency-paths.txt"
  CONTENT "${CMAKE_C_COMPILER}\n${CAI_CPKT_PREFIX}\n${CAI_LONEJSON_PREFIX}\n${CAI_PSLOG_PREFIX}\n${CPKT_DEPENDENCY_CACHE}\n")

set(CAI_BUILD_LUA_RUNNER OFF)
if(CAI_BUILD_LUA OR
   (CAI_BUILD_INTEGRATION_TESTS AND CAI_REGISTER_API_KEY_EXAMPLE_SMOKE))
  set(CAI_BUILD_LUA_RUNNER ON)
endif()

if(CAI_BUILD_LUA_RUNNER)
  if(CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "cpkt")
    set(CAI_LUA_INCLUDE_DIR "${CAI_CPKT_PREFIX}/include")
    set(CAI_LUA_LIBRARY "${CAI_CPKT_PREFIX}/lib/liblua.a")
  endif()
  find_path(CAI_LUA_INCLUDE_DIR lua.h
    PATHS /usr/local/include /usr/include /opt/homebrew/include
    PATH_SUFFIXES lua5.5)
  find_library(CAI_LUA_LIBRARY NAMES lua5.5 lua
    PATHS /usr/local/lib /usr/lib /usr/lib/x86_64-linux-gnu
          /opt/homebrew/lib)
  if(NOT CAI_LUA_INCLUDE_DIR OR NOT CAI_LUA_LIBRARY)
    if(CAI_BUILD_LUA)
      message(FATAL_ERROR
        "CAI_BUILD_LUA requires Lua 5.5 headers and library. Set "
        "CAI_BUILD_LUA=OFF to skip the binding.")
    endif()
    message(FATAL_ERROR
      "CAI_REGISTER_API_KEY_EXAMPLE_SMOKE requires Lua 5.5 headers and "
      "library to build its verification runner.")
  endif()
  set(CAI_LUA_ROCK_SHARE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/luarocks/share/lua/5.5")
  set(CAI_LUA_ROCK_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/luarocks/lib/lua/5.5")
  set(CAI_LUA_TEST_ENV
      "LUA_PATH=${CAI_LUA_ROCK_SHARE_DIR}/?.lua\;${CAI_LUA_ROCK_SHARE_DIR}/?/init.lua\;\;"
      "LUA_CPATH=${CMAKE_CURRENT_BINARY_DIR}/lua-test/?.so\;${CMAKE_CURRENT_BINARY_DIR}/lua-test/?.dylib\;${CMAKE_CURRENT_BINARY_DIR}/lua/?.so\;${CMAKE_CURRENT_BINARY_DIR}/lua/?.dylib\;${CAI_LUA_ROCK_LIB_DIR}/?.so\;\;")
  set(CAI_LUA_SMITH_E2E_ENV
      "CAI_LUA_SMITH_E2E=1")
  # Lua tests use our sanitizer-linked runner. Preloading libasan here is
  # redundant and would instrument unrelated host utilities spawned by Lua.

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
  if(_cai_target_type STREQUAL "EXECUTABLE")
    cai_configure_local_runtime(${target})
  endif()
endfunction()

set(CAI_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai_version.h.in"
  "${CAI_GENERATED_INCLUDE_DIR}/cai/version.h"
  @ONLY)

# Pinned model-specific sources are imported explicitly, never fetched by builds.
set(CAI_SMITH_PRIVATE_INCLUDE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated/private")
set(CAI_SMITH_PROMPT_GENERATOR
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_byte_asset.cmake")
include(cmake/CaiSmithPrompts.cmake)
set(_cai_goal_prompt_inc "${CAI_SMITH_PRIVATE_INCLUDE_DIR}/cai_goal_continuation.inc")
add_custom_command(OUTPUT "${_cai_goal_prompt_inc}"
  COMMAND "${CMAKE_COMMAND}"
    "-DCAI_ASSET_INPUT=${CMAKE_CURRENT_SOURCE_DIR}/prompts/goal/continuation.md"
    "-DCAI_ASSET_OUTPUT=${_cai_goal_prompt_inc}" -DCAI_ASSET_MODE=c-string
    -P "${CAI_SMITH_PROMPT_GENERATOR}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/prompts/goal/continuation.md"
    "${CAI_SMITH_PROMPT_GENERATOR}" VERBATIM)
list(APPEND CAI_SMITH_PROMPT_INC_PATHS "${_cai_goal_prompt_inc}")
add_custom_target(cai_smith_prompt_asset
  DEPENDS ${CAI_SMITH_PROMPT_INC_PATHS})
foreach(_kanban_asset html js css)
  set(_kanban_inc "${CAI_SMITH_PRIVATE_INCLUDE_DIR}/cai_kanban_${_kanban_asset}.inc")
  add_custom_command(OUTPUT "${_kanban_inc}"
    COMMAND "${CMAKE_COMMAND}"
      "-DCAI_ASSET_INPUT=${CMAKE_CURRENT_SOURCE_DIR}/src/tools/kanban.${_kanban_asset}"
      "-DCAI_ASSET_OUTPUT=${_kanban_inc}" -DCAI_ASSET_MODE=c-string
      -P "${CAI_SMITH_PROMPT_GENERATOR}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/tools/kanban.${_kanban_asset}"
      "${CAI_SMITH_PROMPT_GENERATOR}" VERBATIM)
  list(APPEND CAI_KANBAN_ASSETS "${_kanban_inc}")
endforeach()

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
  src/cai_execution.c
  src/cai_terminal_text.c
  src/cai_session_store.c
  src/cai_skills.c
  src/cai_skill_metadata.c
  src/cai_allocator.c
  src/cai_auth.c
  src/cai_chrome.c
  src/cai_chrome_protocol.c
  src/cai_client.c
  src/cai_conversation.c
  src/cai_env.c
  src/cai_process_environment.c
  src/cai_config.c
  src/cai_display.c
  src/cai_error.c
  src/cai_http.c
  src/cai_input_item.c
  src/cai_lj.c
  src/cai_log.c
  src/cai_mcp.c
  src/cai_mcp_client.c
  src/cai_model_catalog.c
  src/cai_quota.c
  src/cai_models.c
  src/cai_response.c
  src/cai_smith.c
  src/cai_source.c
  src/cai_stream.c
  src/cai_chat.c
  src/cai_tool.c
  src/cai_raw_input.c
  src/tools/chrome.c
  src/tools/exec.c
  src/tools/goal.c
  src/tools/patch.c
  src/tools/read.c
  src/tools/revgeo.c
  src/tools/searxng.c
  src/tools/terminal.c
  src/tools/todo.c
  src/tools/todo_http.c
  ${CAI_KANBAN_ASSETS}
  src/tools/view_image.c)

set(CAI_PUBLIC_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND CAI_SOURCES src/cai_chrome_host.c src/cai_chrome_setup.c)
else()
  list(APPEND CAI_SOURCES src/cai_chrome_host_stub.c)
endif()

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
  cai_apply_export_policy(cai_shared
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai_shared.exports")
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
    add_dependencies(${_cai_target} cai_smith_prompt_asset)
    cai_configure_c_target(${_cai_target})
  endif()
endforeach()
if(TARGET cai_shared)
  target_link_libraries(cai_shared PRIVATE ${CAI_CURL_SHARED_LINK}
    Threads::Threads ${CAI_OPENSSL_CRYPTO_SHARED_LINK} ${CAI_LONEJSON_LINK} CpktYaml::Yaml)
endif()
if(TARGET cai_static)
  target_link_libraries(cai_static PRIVATE CpktYaml::Yaml)
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

function(cai_configure_cli_target target)
  if(CAI_TARGET_ID MATCHES "-linux-musl$")
    target_link_options(${target} PRIVATE -static)
  endif()
  cai_configure_c_target(${target})
endfunction()

if(CAI_BUILD_APP)
  find_package(lockdc CONFIG REQUIRED COMPONENTS static)
  # The product and fixture entrypoints share these exact sources/options.
  # Compile them once per configuration; keep fixture substitutions in main.
  add_library(cai_cli_support OBJECT cli/identity.c cli/app_control.c cli/options.c cli/help.c cli/config.c cli/runtime_options.c cli/mcp.c cli/skills.c cli/status.c cli/reasoning.c cli/literal.c cli/shell.c cli/login.c
    cli/review_report.c cli/pouch.c cli/pouch_stream.c cli/quota_refresh.c cli/session_commands.c
    cli/log.c cli/response.c cli/messages.c cli/message_names.c cli/todo.c cli/theme.c cli/stores.c cli/temp.c cli/replay.c cli/replay_tools.c cli/tool_result.c)
  target_link_libraries(cai_cli_support PUBLIC cai_static
    softline::softline_static libmdf::mdf_static lockdc::static)
  cai_configure_c_target(cai_cli_support)
  include(CaiApp)
endif()

if(CAI_BUILD_CLI)
  add_executable(cai_cli cli/main.c)
  set_target_properties(cai_cli PROPERTIES OUTPUT_NAME cai)
  if(CAI_TARGET_ID MATCHES "-linux-musl$")
    set_target_properties(cai_cli PROPERTIES SKIP_BUILD_RPATH ON)
  endif()
  target_link_libraries(cai_cli PRIVATE cai_app_static)
  cai_configure_cli_target(cai_cli)
  if(CAI_INSTALL)
    install(TARGETS cai_cli RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
      COMPONENT cai-cli EXCLUDE_FROM_ALL)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
      "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
      DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/libcai"
      COMPONENT cai-cli EXCLUDE_FROM_ALL)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/man/cai.1"
      DESTINATION "${CMAKE_INSTALL_MANDIR}/man1"
      COMPONENT cai-cli EXCLUDE_FROM_ALL)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/prompts/goal/LICENSE"
      "${CMAKE_CURRENT_SOURCE_DIR}/prompts/goal/NOTICE"
      "${CMAKE_CURRENT_SOURCE_DIR}/prompts/goal/README.md"
      DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/libcai/third_party/codex"
      COMPONENT cai-cli EXCLUDE_FROM_ALL)
    install(FILES "${CAI_LIBYAML_LICENSE}"
      DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/libcai/third_party/libyaml"
      COMPONENT cai-cli EXCLUDE_FROM_ALL)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/model-metadata.md"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/chrome.md"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/embedding.md"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/agent-messaging.md"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/kanban.md"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/journal.md"
      DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/libcai/docs"
      COMPONENT cai-cli EXCLUDE_FROM_ALL)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/cli/main.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/examples/app.lua"
      DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/libcai/examples"
      COMPONENT cai-cli EXCLUDE_FROM_ALL)
    foreach(_cai_cli_dep_doc
        "${CAI_DEPS_DIR}/libpslog-${CAI_PSLOG_VERSION}-${CAI_TARGET_ID}/share/doc/libpslog"
        "${CAI_LONEJSON_PREFIX}/share/doc/liblonejson"
        "${_cai_lockdc_prefix}/share/doc/liblockdc"
        "${_cai_liblql_prefix}/share/doc/liblql"
        "${CAI_DEPS_DIR}/softline-${CAI_SOFTLINE_VERSION}-${CAI_TARGET_ID}/share/doc/softline"
        "${CAI_DEPS_DIR}/libmdf-${CAI_LIBMDF_VERSION}-${CAI_TARGET_ID}/share/doc/libmdf")
      get_filename_component(_cai_cli_dep_name "${_cai_cli_dep_doc}" NAME)
      install(FILES "${_cai_cli_dep_doc}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/libcai/third_party/${_cai_cli_dep_name}"
        COMPONENT cai-cli EXCLUDE_FROM_ALL)
    endforeach()
    foreach(_cai_cli_core_dep curl openssl zlib nghttp2 libssh2)
      install(FILES
        "${CAI_CPKT_PREFIX}/share/doc/cpkt/core/third_party/${_cai_cli_core_dep}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/libcai/third_party/${_cai_cli_core_dep}"
        COMPONENT cai-cli EXCLUDE_FROM_ALL)
    endforeach()
    install(DIRECTORY
      "${CAI_DEPS_DIR}/softline-${CAI_SOFTLINE_VERSION}-${CAI_TARGET_ID}/share/softline/licenses/"
      DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/libcai/third_party/softline/licenses"
      COMPONENT cai-cli EXCLUDE_FROM_ALL)
    if(CAI_TARGET_ID MATCHES "-linux-musl$")
      add_custom_target(cai_package_cli
        COMMAND "${CMAKE_COMMAND}"
          "-DCAI_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
          "-DCAI_DIST_DIR=${CAI_DIST_DIR}"
          "-DCAI_VERSION=${CAI_PACKAGE_VERSION}"
          "-DCAI_TARGET_ID=${CAI_TARGET_ID}"
          "-DCAI_STRIP_BIN=${CMAKE_STRIP}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/package_cli.cmake"
        DEPENDS cai_cli
        VERBATIM)
    endif()
  endif()
  include(CaiCliTests)
endif()

if(CAI_BUILD_LUA_RUNNER)
  add_executable(cai_lua_runner tests/lua/runner.c)
  target_include_directories(cai_lua_runner PRIVATE "${CAI_LUA_INCLUDE_DIR}")
  target_link_libraries(cai_lua_runner PRIVATE "${CAI_LUA_LIBRARY}" ${CMAKE_DL_LIBS} m)
  set_target_properties(cai_lua_runner PROPERTIES ENABLE_EXPORTS ON)
  cai_configure_c_target(cai_lua_runner)
  set_property(TARGET cai_lua_runner APPEND PROPERTY BUILD_RPATH
    "${CMAKE_CURRENT_SOURCE_DIR}/build/luarocks/cai-prefix/lib")
  set(CAI_LUA_EXECUTABLE "$<TARGET_FILE:cai_lua_runner>")
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
  add_library(cai_lua MODULE lua/cai_lua.c lua/todo_lua.c)
  if(CAI_BUILD_APP)
    if(NOT TARGET cai_app_shared)
      message(FATAL_ERROR "The cai.app Lua facade requires the public shared application library")
    endif()
    add_library(cai_app_lua MODULE lua/app_lua.c lua/todo_lua.c)
    set_target_properties(cai_app_lua PROPERTIES OUTPUT_NAME app PREFIX ""
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lua/cai")
    target_include_directories(cai_app_lua PRIVATE "${CAI_LUA_INCLUDE_DIR}"
      "${CAI_PSLOG_LUA_INCLUDE_DIR}" "${CAI_PSLOG_INCLUDE_DIR}")
    target_link_libraries(cai_app_lua PRIVATE cai_app_shared ${CMAKE_DL_LIBS})
    target_link_options(cai_app_lua PRIVATE "-Wl,--allow-shlib-undefined")
    cai_configure_c_target(cai_app_lua)
    cai_apply_export_policy(cai_app_lua "${CMAKE_CURRENT_SOURCE_DIR}/lua/cai_app.exports")
    if(CAI_INSTALL)
      install(TARGETS cai_app_lua LIBRARY
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/lua/5.5/cai" COMPONENT cai-app-lua)
    endif()
    if(TARGET cai_cli_tests)
      add_dependencies(cai_cli_tests cai_app_lua cai_lua cai_lua_runner)
    endif()
    if(CAI_BUILD_TESTS)
      add_test(NAME cai_app_lua_kanban COMMAND bash
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/kanban_test.sh"
        "${CAI_LUA_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/kanban_test.lua"
        "${CMAKE_CURRENT_BINARY_DIR}")
      set_tests_properties(cai_app_lua_kanban PROPERTIES LABELS "cli;local;offline" TIMEOUT 15
        ENVIRONMENT "${CAI_LUA_TEST_ENV}")
      add_test(NAME cai_app_lua COMMAND ${CAI_LUA_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/app_test.lua")
      set_tests_properties(cai_app_lua PROPERTIES LABELS "cli;local;offline" TIMEOUT 10
        ENVIRONMENT "${CAI_LUA_TEST_ENV}")
      add_test(NAME cai_app_lua_exports COMMAND bash
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/shared_export_policy_test.sh"
        "${CMAKE_NM}" "$<TARGET_FILE:cai_app_lua>"
        "${CMAKE_CURRENT_SOURCE_DIR}/lua/cai_app.exports" linux)
      add_test(NAME cai_lua_review_fix COMMAND ${CAI_LUA_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/review_fix_test.lua")
      set_tests_properties(cai_lua_review_fix PROPERTIES LABELS "cli;local;offline"
        TIMEOUT 5 WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        ENVIRONMENT "${CAI_LUA_TEST_ENV}")
      add_test(NAME cai_lua_display_duration COMMAND ${CAI_LUA_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/display_duration_test.lua")
      set_tests_properties(cai_lua_display_duration PROPERTIES LABELS "cli;local;offline"
        TIMEOUT 5 ENVIRONMENT "${CAI_LUA_TEST_ENV}")
      set_tests_properties(cai_app_lua_exports PROPERTIES LABELS "cli;local;offline" TIMEOUT 5)
      if(CAI_BUILD_CLI)
        add_test(NAME cai_app_lua_home COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_home_test.sh"
          "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host" host)
        set_tests_properties(cai_app_lua_home PROPERTIES LABELS "cli;local;offline" TIMEOUT 15
          ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
        file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/app-lua-controls"
          CONTENT "#!/usr/bin/env bash\nexec \"${CAI_LUA_EXECUTABLE}\" \"${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/app_controls_host.lua\" \"\$@\"\n"
          FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
        add_test(NAME cai_app_lua_controls COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/app_controls_test.sh"
          "${CMAKE_CURRENT_BINARY_DIR}/app-lua-controls")
        set_tests_properties(cai_app_lua_controls PROPERTIES LABELS "cli;local;offline" TIMEOUT 15
          ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
        file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host"
          CONTENT "#!/usr/bin/env bash\nexec \"${CAI_LUA_EXECUTABLE}\" \"${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/app_host.lua\" \"\$@\"\n"
          FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
        add_test(NAME cai_app_identity_lua COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_identity_cli_test.sh"
          "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host")
        set_tests_properties(cai_app_identity_lua PROPERTIES LABELS "cli;local;offline" TIMEOUT 15
          ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
        add_test(NAME cai_app_lua_goal_pursuit COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_goal_pursuit_test.sh"
          "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host" responses interactive basic)
        set_tests_properties(cai_app_lua_goal_pursuit PROPERTIES LABELS "cli;local;offline" TIMEOUT 30
          ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
        add_test(NAME cai_app_lua_queue_policy COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_queue_policy_test.sh"
          "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host" responses)
        set_tests_properties(cai_app_lua_queue_policy PROPERTIES LABELS "cli;local;offline" TIMEOUT 10
          ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
        add_test(NAME cai_app_lua_draft_actions COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_draft_actions_test.sh"
          "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host" editing)
        set_tests_properties(cai_app_lua_draft_actions PROPERTIES LABELS "cli;local;offline" TIMEOUT 15
          ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
        add_test(NAME cai_app_lua_review_fix_tool COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_review_fix_tool_test.sh"
          "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host" responses smoke)
        set_tests_properties(cai_app_lua_review_fix_tool PROPERTIES
          LABELS "cli;local;offline" TIMEOUT 15
          ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
        add_test(NAME cai_app_lua_execution COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_execution_test.sh"
          "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host" responses lua)
        set_tests_properties(cai_app_lua_execution PROPERTIES LABELS "cli;local;offline" TIMEOUT 15
          ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
        add_test(NAME cai_app_lua_subagent_replay COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_subagent_replay_test.sh"
          "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host" responses lua)
        set_tests_properties(cai_app_lua_subagent_replay PROPERTIES LABELS "cli;local;offline" TIMEOUT 20
          ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
        add_test(NAME cai_app_lua_reasoning_replay COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_reasoning_replay_test.sh"
          "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host" responses host)
        set_tests_properties(cai_app_lua_reasoning_replay PROPERTIES LABELS "cli;local;offline" TIMEOUT 20
          ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
        add_test(NAME cai_app_lua_yolo COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_yolo_test.sh"
          "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host")
        set_tests_properties(cai_app_lua_yolo PROPERTIES LABELS "cli;local;offline" TIMEOUT 15
          ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
        add_test(NAME cai_app_lua_yolo_live COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_yolo_live_test.sh"
          "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host" responses)
        set_tests_properties(cai_app_lua_yolo_live PROPERTIES LABELS "cli;local;offline" TIMEOUT 20
          ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
        add_test(NAME cai_app_lua_get_goal COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_get_goal_test.sh"
          "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host" responses)
        set_tests_properties(cai_app_lua_get_goal PROPERTIES LABELS "cli;local;offline" TIMEOUT 10
          ENVIRONMENT "${CAI_LUA_TEST_ENV}")
        add_test(NAME cai_app_lua_delete COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_delete_test.sh"
          "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host")
        set_tests_properties(cai_app_lua_delete PROPERTIES LABELS "cli;local;offline" TIMEOUT 20
          ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
        add_test(NAME cai_app_lua_compact COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_compact_test.sh"
          "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host" responses)
        set_tests_properties(cai_app_lua_compact PROPERTIES LABELS "cli;local;offline" TIMEOUT 45
          ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
        add_test(NAME cai_app_lua_skills_inventory COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_skills_inventory_test.sh"
          "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host" "${CAI_LUA_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/skills_inventory.lua")
        set_tests_properties(cai_app_lua_skills_inventory PROPERTIES LABELS "cli;local;offline" TIMEOUT 15
          ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
        add_test(NAME cai_app_lua_shell_draft COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_shell_draft_test.sh"
          "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host")
        set_tests_properties(cai_app_lua_shell_draft PROPERTIES LABELS "cli;local;offline" TIMEOUT 15
          ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
        add_test(NAME cai_app_lua_draft_quit COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_draft_quit_test.sh"
          "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host"
          "$<TARGET_FILE:cai_cli_prompt_history_test>" production)
        set_tests_properties(cai_app_lua_draft_quit PROPERTIES LABELS "cli;local;offline" TIMEOUT 15
          ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
        add_test(NAME cai_app_lua_history COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_history_test.sh"
          "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host"
          "$<TARGET_FILE:cai_cli_prompt_history_test>")
        set_tests_properties(cai_app_lua_history PROPERTIES LABELS "cli;local;offline" TIMEOUT 15
          ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
        add_test(NAME cai_app_lua_steering_seam COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_steering_seam_test.sh"
          "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host" responses)
        set_tests_properties(cai_app_lua_steering_seam PROPERTIES LABELS "cli;local;offline" TIMEOUT 30
          ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
        add_test(NAME cai_app_lua_themes COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_themes_test.sh"
          "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host" host)
        add_test(NAME cai_app_lua_status_fields COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_status_fields_test.sh"
          "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host" host)
        set_tests_properties(cai_app_lua_status_fields PROPERTIES
          LABELS "cli;local;offline" TIMEOUT 10
          ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
        set_tests_properties(cai_app_lua_themes PROPERTIES LABELS "cli;local;offline" TIMEOUT 15
          ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
        add_test(NAME cai_app_lua_temp COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_temp_test.sh"
          "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host" "$<TARGET_FILE:cai_app_controls_host>" host)
        set_tests_properties(cai_app_lua_temp PROPERTIES LABELS "cli;local;offline" TIMEOUT 30
          ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
        add_test(NAME cai_app_lua_replay_tail COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_replay_tail_test.sh"
          "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host" host)
        set_tests_properties(cai_app_lua_replay_tail PROPERTIES LABELS "cli;local;offline" TIMEOUT 15
          ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
        add_test(NAME cai_app_lua_startup_text COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_startup_text_test.sh"
          "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host" host)
        set_tests_properties(cai_app_lua_startup_text PROPERTIES LABELS "cli;local;offline" TIMEOUT 15
          ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
        add_test(NAME cai_app_lua_embedding COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_embedding_test.sh"
          "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host")
        add_test(NAME cai_app_lua_export_paths COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_export_paths_test.sh"
          "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host")
        set_tests_properties(cai_app_lua_export_paths PROPERTIES
          LABELS "cli;local;offline" TIMEOUT 15
          ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
        add_test(NAME cai_app_lua_todo_layout COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_kanban_test.sh" "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host")
        set_tests_properties(cai_app_lua_todo_layout PROPERTIES LABELS "cli;local;offline" TIMEOUT 30
          ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
        set_tests_properties(cai_app_lua_embedding PROPERTIES LABELS "cli;local;offline" TIMEOUT 15
          ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
        add_test(NAME cai_app_lua_messages COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_messages_integration_test.sh"
          "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host")
        set_tests_properties(cai_app_lua_messages PROPERTIES LABELS "cli;local;offline" TIMEOUT 45
          ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
        add_test(NAME cai_app_messages_roundtrip COMMAND bash
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_messages_roundtrip_test.sh"
          "$<TARGET_FILE:cai_app_host>" "${CMAKE_CURRENT_BINARY_DIR}/app-lua-host")
        set_tests_properties(cai_app_messages_roundtrip PROPERTIES LABELS "cli;local;offline" TIMEOUT 30
          ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
      endif()
    endif()
  endif()
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
  cai_apply_export_policy(cai_lua
    "${CMAKE_CURRENT_SOURCE_DIR}/lua/cai_lua.exports")
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
    target_link_libraries(cai_lua_native_todo_store_test PRIVATE cai_shared)
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

include(CaiTests)
include(CaiCrossTests)

if(CAI_BUILD_TESTS)
  get_property(_cai_registered_tests DIRECTORY PROPERTY TESTS)
  foreach(_test IN LISTS _cai_registered_tests)
    if(_test MATCHES "^cai_(receiver_usage|build_toolchain_change|lifecycle_execution|makefile_targets|devenv|release_live_gate|mcp_client_source_contract|smith_prompt_asset|byte_asset_generator|export_policy_incremental|target_tool_discovery|darwin_linker_route|release_artifacts_darwin_metadata|release_export_policy|release_manifest_gate|release_toolchain_cache_guard|release_staging_safety|cmake_presets|release_lua_version)_test$" OR _test STREQUAL "cai_tests.registration_script")
      set_property(TEST ${_test} APPEND PROPERTY LABELS host)
    endif()
  endforeach()
endif()

if(CAI_BUILD_FUZZERS)
  if(NOT TARGET cai_static)
    message(FATAL_ERROR "CAI_BUILD_FUZZERS requires CAI_BUILD_STATIC=ON.")
  endif()
  function(cai_add_fuzzer target source corpus_relpath)
    add_executable(${target} ${source} tests/fuzz_driver.c)
    target_compile_definitions(${target} PRIVATE CAI_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-tmp")
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
  get_property(_cai_fuzz_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  list(FILTER _cai_fuzz_targets INCLUDE REGEX "^cai_.*_fuzz$")
  add_custom_target(cai_fuzzers DEPENDS ${_cai_fuzz_targets})
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME cai_fuzz_crash_handling
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz_crash_handling_test.sh"
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(cai_fuzz_crash_handling PROPERTIES
      LABELS "fuzz;local" TIMEOUT 60)
  endif()
endif()

add_custom_target(clang-format
  COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/format.sh" VERBATIM)
add_custom_target(format-check
  COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/format.sh" check VERBATIM)

if(CAI_INSTALL)
  set(CAI_INSTALL_CMAKEDIR
      "${CMAKE_INSTALL_LIBDIR}/cmake/cai"
      CACHE STRING "Install path for cai CMake package files.")
  set(CAI_CPKT_URL_FOR_PACKAGE "host")
  set(CAI_CPKT_ARCHIVE_NAME_FOR_PACKAGE "host")
  if(CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "cpkt")
    set(CAI_CPKT_URL_FOR_PACKAGE "${CAI_CPKT_URL}")
    get_filename_component(CAI_CPKT_ARCHIVE_NAME_FOR_PACKAGE
      "${CAI_CPKT_URL}" NAME)
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
  string(APPEND CAI_PACKAGE_REQUIRES_PRIVATE ", yaml-0.1 >= ${CAI_LIBYAML_VERSION}")
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
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cai.pc"
    @ONLY)
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai-dependencies.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/dependencies.json"
    @ONLY)

  install(DIRECTORY "${CAI_PUBLIC_INCLUDE_DIR}/" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    PATTERN "app.h" EXCLUDE)
  install(DIRECTORY "${CAI_GENERATED_INCLUDE_DIR}/" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(FILES "${CAI_LIBYAML_LICENSE}"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/libcai/third_party/libyaml")
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
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/chrome.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/embedding.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/agent-messaging.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/kanban.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/journal.md"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/libcai/docs")
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/dependencies.json"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/cai")
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/prompts/goal/LICENSE"
    "${CMAKE_CURRENT_SOURCE_DIR}/prompts/goal/NOTICE"
    "${CMAKE_CURRENT_SOURCE_DIR}/prompts/goal/README.md"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/libcai/third_party/codex")

  set(_cai_package_deps "")
  if(TARGET cai_static)
    list(APPEND _cai_package_deps cai_static)
  endif()
  if(TARGET cai_shared)
    list(APPEND _cai_package_deps cai_shared)
  endif()
  foreach(_cai_app_target IN ITEMS cai_app_static cai_app_shared)
    if(TARGET ${_cai_app_target})
      list(APPEND _cai_package_deps ${_cai_app_target})
    endif()
  endforeach()
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
      "-DCAI_TARGETS=${CAI_TARGET_ID}"
      "-DCAI_PACKAGE_SCOPE=binary"
      "-DCAI_CHECKSUMS_PATH=${CMAKE_CURRENT_BINARY_DIR}/package-CHECKSUMS"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/package_checksums.cmake"
    DEPENDS cai_package_archive
    VERBATIM)
endif()

include(CaiHostFixtures)
# Register after the full target graph, including optional fuzzers and helpers.
include(CaiRuntimeTests)
include(CaiTestEnvironment)
