if(NOT DEFINED CPKT_TARGET_ID OR CPKT_TARGET_ID STREQUAL "")
  message(FATAL_ERROR "CPKT_TARGET_ID is required for linux-bootlin.cmake")
endif()
set(CPKT_TARGET_ID "${CPKT_TARGET_ID}" CACHE STRING
    "pkt.systems lifecycle target id" FORCE)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CPKT_TARGET_ID)

set(_cpkt_toolchain_script
    "${CMAKE_CURRENT_LIST_DIR}/../../scripts/cpkt-toolchains.sh")
if(NOT EXISTS "${_cpkt_toolchain_script}")
  message(FATAL_ERROR "missing lifecycle toolchain resolver: ${_cpkt_toolchain_script}")
endif()

execute_process(
  COMMAND "${_cpkt_toolchain_script}" ensure "${CPKT_TARGET_ID}"
  RESULT_VARIABLE _cpkt_toolchain_result
  OUTPUT_VARIABLE _cpkt_toolchain_description
  ERROR_VARIABLE _cpkt_toolchain_error)
if(NOT _cpkt_toolchain_result EQUAL 0)
  message(FATAL_ERROR
    "failed to provision pinned ${CPKT_TARGET_ID} toolchain:\n"
    "${_cpkt_toolchain_error}")
endif()

function(_cpkt_toolchain_value key out_var)
  string(REGEX MATCH "(^|\n)${key}=([^\n]*)" _match
         "${_cpkt_toolchain_description}")
  if(NOT _match)
    message(FATAL_ERROR
      "pinned ${CPKT_TARGET_ID} toolchain description is missing ${key}")
  endif()
  set(${out_var} "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

_cpkt_toolchain_value(root _cpkt_root)
_cpkt_toolchain_value(sysroot _cpkt_sysroot)
_cpkt_toolchain_value(cc _cpkt_cc)
_cpkt_toolchain_value(cxx _cpkt_cxx)
_cpkt_toolchain_value(ld _cpkt_ld)
_cpkt_toolchain_value(ar _cpkt_ar)
_cpkt_toolchain_value(ranlib _cpkt_ranlib)
_cpkt_toolchain_value(strip _cpkt_strip)
_cpkt_toolchain_value(nm _cpkt_nm)
_cpkt_toolchain_value(objcopy _cpkt_objcopy)
_cpkt_toolchain_value(objdump _cpkt_objdump)
_cpkt_toolchain_value(addr2line _cpkt_addr2line)
_cpkt_toolchain_value(gdb _cpkt_gdb)
_cpkt_toolchain_value(readelf _cpkt_readelf)
_cpkt_toolchain_value(target_triple _cpkt_target_triple)

function(_cpkt_realpath path out_var)
  if(IS_ABSOLUTE "${path}" AND EXISTS "${path}")
    file(REAL_PATH "${path}" _cpkt_resolved)
    set(${out_var} "${_cpkt_resolved}" PARENT_SCOPE)
    return()
  endif()
  set(${out_var} "${path}" PARENT_SCOPE)
endfunction()

if(CPKT_TARGET_ID MATCHES "^x86_64-")
  set(CMAKE_SYSTEM_PROCESSOR x86_64)
elseif(CPKT_TARGET_ID MATCHES "^aarch64-")
  set(CMAKE_SYSTEM_PROCESSOR aarch64)
elseif(CPKT_TARGET_ID MATCHES "^armhf-")
  set(CMAKE_SYSTEM_PROCESSOR arm)
else()
  message(FATAL_ERROR "unsupported Linux target id: ${CPKT_TARGET_ID}")
endif()

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_SYSROOT "${_cpkt_sysroot}" CACHE PATH "" FORCE)
set(CMAKE_C_COMPILER "${_cpkt_cc}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${_cpkt_cxx}" CACHE FILEPATH "" FORCE)
set(CMAKE_LINKER "${_cpkt_ld}" CACHE FILEPATH "" FORCE)
set(CMAKE_AR "${_cpkt_ar}" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${_cpkt_ranlib}" CACHE FILEPATH "" FORCE)
set(CMAKE_STRIP "${_cpkt_strip}" CACHE FILEPATH "" FORCE)
set(CMAKE_NM "${_cpkt_nm}" CACHE FILEPATH "" FORCE)
set(CMAKE_OBJCOPY "${_cpkt_objcopy}" CACHE FILEPATH "" FORCE)
set(CMAKE_OBJDUMP "${_cpkt_objdump}" CACHE FILEPATH "" FORCE)
set(CMAKE_ADDR2LINE "${_cpkt_addr2line}" CACHE FILEPATH "" FORCE)
set(CMAKE_GDB "${_cpkt_gdb}" CACHE FILEPATH "" FORCE)
set(CMAKE_READELF "${_cpkt_readelf}" CACHE FILEPATH "" FORCE)
set(CPKT_BOOTLIN_ROOT "${_cpkt_root}" CACHE PATH "Pinned Bootlin toolchain root" FORCE)
set(CPKT_TARGET_TRIPLE "${_cpkt_target_triple}" CACHE STRING
    "Pinned Bootlin target triple" FORCE)

execute_process(
  COMMAND "${CMAKE_C_COMPILER}" -print-prog-name=ld
  RESULT_VARIABLE _cpkt_print_ld_result
  OUTPUT_VARIABLE _cpkt_compiler_ld
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _cpkt_print_ld_result EQUAL 0 OR _cpkt_compiler_ld STREQUAL "")
  message(FATAL_ERROR
    "failed to inspect pinned ${CPKT_TARGET_ID} compiler linker route")
endif()
execute_process(
  COMMAND "${CMAKE_C_COMPILER}" -print-file-name=libc.so
  RESULT_VARIABLE _cpkt_print_libc_result
  OUTPUT_VARIABLE _cpkt_compiler_libc
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _cpkt_print_libc_result EQUAL 0 OR
   _cpkt_compiler_libc STREQUAL "" OR
   _cpkt_compiler_libc STREQUAL "libc.so")
  message(FATAL_ERROR
    "failed to inspect pinned ${CPKT_TARGET_ID} compiler libc route")
endif()
_cpkt_realpath("${_cpkt_root}" _cpkt_root_real)
_cpkt_realpath("${_cpkt_sysroot}" _cpkt_sysroot_real)
_cpkt_realpath("${_cpkt_compiler_ld}" _cpkt_compiler_ld_real)
_cpkt_realpath("${_cpkt_compiler_libc}" _cpkt_compiler_libc_real)
string(FIND "${_cpkt_compiler_ld_real}" "${_cpkt_root_real}/" _cpkt_ld_root_index)
if(NOT _cpkt_ld_root_index EQUAL 0)
  message(FATAL_ERROR
    "pinned ${CPKT_TARGET_ID} compiler selects linker outside Bootlin root: "
    "${_cpkt_compiler_ld_real}")
endif()
string(FIND "${_cpkt_compiler_libc_real}" "${_cpkt_sysroot_real}/" _cpkt_libc_sysroot_index)
if(NOT _cpkt_libc_sysroot_index EQUAL 0)
  message(FATAL_ERROR
    "pinned ${CPKT_TARGET_ID} compiler selects libc outside Bootlin sysroot: "
    "${_cpkt_compiler_libc_real}")
endif()

set(CMAKE_FIND_ROOT_PATH "${_cpkt_sysroot}" "${_cpkt_root}" CACHE STRING "" FORCE)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER CACHE STRING "" FORCE)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY CACHE STRING "" FORCE)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY CACHE STRING "" FORCE)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY CACHE STRING "" FORCE)
