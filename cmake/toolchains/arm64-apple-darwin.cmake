set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR arm64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(DEFINED ENV{OSXCROSS_ROOT} AND NOT "$ENV{OSXCROSS_ROOT}" STREQUAL "")
  set(CAI_OSXCROSS_ROOT "$ENV{OSXCROSS_ROOT}")
elseif(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
  set(CAI_OSXCROSS_ROOT "$ENV{HOME}/.local/cross/osxcross")
else()
  message(FATAL_ERROR "OSXCROSS_ROOT is not set and HOME is unavailable")
endif()

set(CAI_OSXCROSS_HOST "arm64-apple-darwin25" CACHE STRING "osxcross target host triple")
set(CAI_MACOS_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum macOS deployment target")
set(CMAKE_OSX_DEPLOYMENT_TARGET "${CAI_MACOS_DEPLOYMENT_TARGET}" CACHE STRING "" FORCE)

set(CAI_OSXCROSS_BIN_DIR "${CAI_OSXCROSS_ROOT}/bin")
set(ENV{PATH} "${CAI_OSXCROSS_BIN_DIR}:$ENV{PATH}")
set(CMAKE_C_COMPILER "${CAI_OSXCROSS_BIN_DIR}/${CAI_OSXCROSS_HOST}-clang" CACHE FILEPATH "")
set(CMAKE_CXX_COMPILER "${CAI_OSXCROSS_BIN_DIR}/${CAI_OSXCROSS_HOST}-clang++" CACHE FILEPATH "")
set(CMAKE_AR "${CAI_OSXCROSS_BIN_DIR}/${CAI_OSXCROSS_HOST}-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${CAI_OSXCROSS_BIN_DIR}/${CAI_OSXCROSS_HOST}-ranlib" CACHE FILEPATH "")
set(CMAKE_LINKER "${CAI_OSXCROSS_BIN_DIR}/${CAI_OSXCROSS_HOST}-ld" CACHE FILEPATH "")
set(CMAKE_STRIP "${CAI_OSXCROSS_BIN_DIR}/${CAI_OSXCROSS_HOST}-strip" CACHE FILEPATH "")
set(CMAKE_INSTALL_NAME_TOOL "${CAI_OSXCROSS_BIN_DIR}/${CAI_OSXCROSS_HOST}-install_name_tool" CACHE FILEPATH "")
set(CPKT_OTOOL "${CAI_OSXCROSS_BIN_DIR}/${CAI_OSXCROSS_HOST}-otool" CACHE FILEPATH "")

foreach(_cai_required_tool
        CMAKE_C_COMPILER
        CMAKE_AR
        CMAKE_RANLIB
        CMAKE_LINKER
        CMAKE_STRIP
        CPKT_OTOOL
        CMAKE_INSTALL_NAME_TOOL)
  if(NOT EXISTS "${${_cai_required_tool}}")
    message(FATAL_ERROR
      "The arm64 Apple Darwin osxcross toolchain is missing ${_cai_required_tool}: "
      "${${_cai_required_tool}}. Set OSXCROSS_ROOT or install osxcross under $HOME/.local/cross/osxcross.")
  endif()
endforeach()

set(_cai_darwin_linker_flag "-fuse-ld=${CMAKE_LINKER}")
foreach(_cai_linker_flags
        CMAKE_EXE_LINKER_FLAGS
        CMAKE_SHARED_LINKER_FLAGS
        CMAKE_MODULE_LINKER_FLAGS)
  if(NOT "${${_cai_linker_flags}}" MATCHES "(^| )-fuse-ld=")
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
