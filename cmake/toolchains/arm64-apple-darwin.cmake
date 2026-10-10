set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR arm64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CPKT_LIFECYCLE_SCRIPTS_DIR)
if(NOT DEFINED CPKT_LIFECYCLE_SCRIPTS_DIR)
  set(CPKT_LIFECYCLE_SCRIPTS_DIR "${CMAKE_CURRENT_LIST_DIR}/../../scripts"
      CACHE PATH "Declared lifecycle helper directory")
endif()
if(DEFINED ENV{CPKT_OSXCROSS_HOST} AND NOT "$ENV{CPKT_OSXCROSS_HOST}" STREQUAL "" AND
    NOT "$ENV{CPKT_OSXCROSS_HOST}" MATCHES "^arm64-apple-darwin")
  message(FATAL_ERROR
    "arm64-apple-darwin-release requires an arm64 osxcross host triple")
endif()
execute_process(COMMAND "${CPKT_LIFECYCLE_SCRIPTS_DIR}/cpkt-toolchains.sh" discover arm64-apple-darwin
  RESULT_VARIABLE _cai_discovery_result OUTPUT_VARIABLE _cai_description ERROR_VARIABLE _cai_discovery_error)
if(NOT _cai_discovery_result EQUAL 0 OR NOT _cai_description MATCHES "(^|[\r\n])status=ready([\r\n]|$)")
  message(FATAL_ERROR "Missing complete osxcross and pinned host MIG collection; prepare with ${CPKT_LIFECYCLE_SCRIPTS_DIR}/cpkt-toolchains.sh ensure arm64-apple-darwin: ${_cai_discovery_error}")
endif()
foreach(key root prefix cc cxx ld ar ranlib strip nm otool mig migcom mig_root)
  string(REGEX MATCH "(^|[\r\n])${key}=([^\r\n]+)" _cai_match "${_cai_description}")
  if(NOT _cai_match)
    message(FATAL_ERROR "Darwin resolver did not report ${key}")
  endif()
  set(_cai_${key} "${CMAKE_MATCH_2}")
endforeach()
set(CAI_OSXCROSS_ROOT "${_cai_root}")
set(CAI_OSXCROSS_HOST "${_cai_prefix}" CACHE STRING "osxcross target host triple" FORCE)
set(CAI_MACOS_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum macOS deployment target")
set(CMAKE_OSX_DEPLOYMENT_TARGET "${CAI_MACOS_DEPLOYMENT_TARGET}" CACHE STRING "" FORCE)

set(CAI_OSXCROSS_BIN_DIR "${CAI_OSXCROSS_ROOT}/bin")
set(ENV{PATH} "${CAI_OSXCROSS_BIN_DIR}:$ENV{PATH}")
set(ENV{PATH} "${_cai_mig_root}/bin:$ENV{PATH}")
set(ENV{MIG} "${_cai_mig}")
set(ENV{MIGCOM} "${_cai_migcom}")
set(CMAKE_C_COMPILER "${_cai_cc}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${_cai_cxx}" CACHE FILEPATH "" FORCE)
set(CMAKE_AR "${_cai_ar}" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${_cai_ranlib}" CACHE FILEPATH "" FORCE)
set(CMAKE_LINKER "${_cai_ld}" CACHE FILEPATH "" FORCE)
set(CMAKE_STRIP "${_cai_strip}" CACHE FILEPATH "" FORCE)
set(CMAKE_NM "${_cai_nm}" CACHE FILEPATH "" FORCE)
set(CMAKE_INSTALL_NAME_TOOL "${CAI_OSXCROSS_BIN_DIR}/${CAI_OSXCROSS_HOST}-install_name_tool" CACHE FILEPATH "")
set(CPKT_OTOOL "${_cai_otool}" CACHE FILEPATH "" FORCE)

foreach(_cai_required_tool
        CMAKE_C_COMPILER
        CMAKE_AR
        CMAKE_RANLIB
        CMAKE_LINKER
        CPKT_OTOOL)
  if(NOT EXISTS "${${_cai_required_tool}}")
    message(FATAL_ERROR
      "The arm64 Apple Darwin osxcross toolchain is missing ${_cai_required_tool}: "
      "${${_cai_required_tool}}. Set OSXCROSS_ROOT or install osxcross under $HOME/.local/cross/osxcross.")
  endif()
endforeach()

set(_cai_darwin_linker_flag "--ld-path=${CMAKE_LINKER}")
foreach(_cai_linker_flags
        CMAKE_EXE_LINKER_FLAGS
        CMAKE_SHARED_LINKER_FLAGS
        CMAKE_MODULE_LINKER_FLAGS)
  if(NOT "${${_cai_linker_flags}}" MATCHES "(^| )--ld-path=")
    set(${_cai_linker_flags} "${_cai_darwin_linker_flag} ${${_cai_linker_flags}}" CACHE STRING "" FORCE)
  endif()
endforeach()

file(GLOB _cai_osxcross_sdks LIST_DIRECTORIES true "${CAI_OSXCROSS_ROOT}/SDK/MacOSX*.sdk")
if(NOT _cai_osxcross_sdks)
  message(FATAL_ERROR "failed to locate a usable osxcross macOS SDK under ${CAI_OSXCROSS_ROOT}/SDK")
endif()
list(SORT _cai_osxcross_sdks)
list(REVERSE _cai_osxcross_sdks)
list(GET _cai_osxcross_sdks 0 CAI_OSXCROSS_SDK)
if(NOT EXISTS "${CAI_OSXCROSS_SDK}/usr/include")
  message(FATAL_ERROR "failed to locate a usable osxcross macOS SDK under ${CAI_OSXCROSS_ROOT}/SDK")
endif()

set(CMAKE_OSX_SYSROOT "${CAI_OSXCROSS_SDK}" CACHE PATH "" FORCE)
set(CMAKE_FIND_ROOT_PATH "${CAI_OSXCROSS_SDK}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
