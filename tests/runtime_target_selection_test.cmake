foreach(_required CAI_SOURCE_DIR CAI_BINARY_DIR CAI_C_COMPILER CAI_READELF
    CAI_SYSROOT CAI_BOOTLIN_ROOT)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "${_required} is required")
  endif()
endforeach()
set(_root "${CAI_BINARY_DIR}/runtime-target-selection")
file(REMOVE_RECURSE "${_root}")
file(MAKE_DIRECTORY "${_root}/src" "${_root}/build")
file(WRITE "${_root}/src/main.c" "int main(void) { return 0; }\n")
file(WRITE "${_root}/src/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.21)
project(runtime_selection C)
enable_testing()
set(CAI_BUILD_TESTS ON)
set(CAI_TARGET_ID x86_64-linux-gnu)
include("${CAI_SOURCE_DIR}/cmake/cai-rpath.cmake")
add_executable(selected main.c)
cai_configure_local_runtime(selected)
add_library(not_an_executable STATIC main.c)
# Output names/directories must come from TARGET_FILE, not filename patterns.
add_executable(late_helper main.c)
set_target_properties(late_helper PROPERTIES OUTPUT_NAME "renamed helper"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin space")
cai_configure_local_runtime(late_helper)
include("${CAI_SOURCE_DIR}/cmake/CaiRuntimeTests.cmake")
]=])
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_root}/src" -B "${_root}/build" -G Ninja
    "-DCAI_SOURCE_DIR=${CAI_SOURCE_DIR}"
    "-DCMAKE_C_COMPILER=${CAI_C_COMPILER}"
    "-DCMAKE_READELF=${CAI_READELF}"
    "-DCMAKE_SYSROOT=${CAI_SYSROOT}"
    "-DCPKT_BOOTLIN_ROOT=${CAI_BOOTLIN_ROOT}"
  RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "fixture configuration failed: ${_out}\n${_err}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${_root}/build" --parallel 1
    --target selected late_helper
  RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "fixture build failed: ${_out}\n${_err}")
endif()
# These formerly matched the runtime check's glob. Keep them present and invalid.
foreach(_stale cai_example_disabled cai_lua_runner cai_mcp_obsolete cai_old_fuzz)
  file(WRITE "${_root}/build/${_stale}" "obsolete, not an ELF executable\n")
endforeach()
function(check expected diagnostic)
  execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${_root}/build"
      --no-tests=error --output-on-failure --stop-on-failure --parallel 1
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(expected STREQUAL "pass")
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "configured executables should pass: ${_out}\n${_err}")
    endif()
  elseif(_rc EQUAL 0 OR NOT "${_out}\n${_err}" MATCHES "${diagnostic}")
    message(FATAL_ERROR "expected ${diagnostic}: ${_out}\n${_err}")
  endif()
endfunction()
check(pass "")
# A selected missing or invalid ELF must fail, rather than be filtered away.
set(_helper "${_root}/build/bin space/renamed helper")
file(RENAME "${_helper}" "${_helper}.saved")
check(fail "configured executable is missing")
file(WRITE "${_helper}" "invalid selected ELF\n")
check(fail "does not select the Bootlin loader")
file(REMOVE "${_helper}")
file(RENAME "${_helper}.saved" "${_helper}")
# Keep runtime path checks strict for an actual selected binary.
execute_process(COMMAND "${CMAKE_COMMAND}"
    "-DCAI_RUNTIME_TARGETS=${_helper}"
    "-DCAI_READELF=${CAI_READELF}" "-DCAI_SYSROOT=${CAI_SYSROOT}"
    "-DCAI_DEPENDENCY_RUNTIME_DIRS=${_root}/missing-runtime"
    -P "${CAI_SOURCE_DIR}/tests/local_bootlin_runtime_test.cmake"
  RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(_rc EQUAL 0 OR NOT _err MATCHES "lacks dependency runtime directory")
  message(FATAL_ERROR "missing dependency path was accepted: ${_out}\n${_err}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}"
    "-DCAI_RUNTIME_TARGETS=" "-DCAI_READELF=${CAI_READELF}"
    "-DCAI_SYSROOT=${CAI_SYSROOT}" "-DCAI_DEPENDENCY_RUNTIME_DIRS="
    -P "${CAI_SOURCE_DIR}/tests/local_bootlin_runtime_test.cmake"
  RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(_rc EQUAL 0 OR NOT _err MATCHES "must contain configured executables")
  message(FATAL_ERROR "empty target selection was accepted: ${_out}\n${_err}")
endif()
set(_fake "${_root}/loader fixture")
file(MAKE_DIRECTORY "${_fake}/lib")
file(WRITE "${_fake}/readelf" [=[#!/usr/bin/env bash
set -eu
case "$1" in
  -l) cat "${0%/*}/program" ;;
  -d) cat "${0%/*}/dynamic" ;;
  *) exit 1 ;;
esac
]=])
file(WRITE "${_fake}/lib/ld-linux-fixture.so.2" [=[#!/usr/bin/env bash
set -eu
cat "${0%/*}/../loaded"
]=])
file(CHMOD "${_fake}/readelf" "${_fake}/lib/ld-linux-fixture.so.2"
  PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
file(WRITE "${_fake}/program"
  "[Requesting program interpreter: ${_fake}/lib/ld-linux-fixture.so.2]\n")
file(WRITE "${_fake}/dynamic" "(RPATH) Library rpath: [${_fake}/lib]\n")
function(check_loaded expected diagnostic)
  execute_process(COMMAND "${CMAKE_COMMAND}"
      "-DCAI_RUNTIME_TARGETS=${_helper}"
      "-DCAI_READELF=${_fake}/readelf" "-DCAI_SYSROOT=${_fake}"
      "-DCAI_DEPENDENCY_RUNTIME_DIRS="
      -P "${CAI_SOURCE_DIR}/tests/local_bootlin_runtime_test.cmake"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(expected STREQUAL "pass")
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "valid loader output failed: ${_out}\n${_err}")
    endif()
  elseif(_rc EQUAL 0 OR NOT _err MATCHES "${diagnostic}")
    message(FATAL_ERROR "expected ${diagnostic}: ${_out}\n${_err}")
  endif()
endfunction()
file(WRITE "${_fake}/loaded" "libc.so.6 => ${_fake}/lib/libc.so.6 (0x1)\n")
check_loaded(pass "")
file(WRITE "${_fake}/loaded" "libc.so.6 => /foreign/lib/libc.so.6 (0x1)\n")
check_loaded(fail "loads a foreign libc component")
file(WRITE "${_fake}/loaded" "libc.so.6 => /foreign${_fake}/lib/libc.so.6 (0x1)\n")
check_loaded(fail "loads a foreign libc component")
file(WRITE "${_fake}/loaded" "unrecognized loader output\n")
check_loaded(fail "did not identify the loaded libc")
file(WRITE "${_fake}/program"
  "[Requesting program interpreter: ${_fake}/lib/ld-linux-wrong.so.2]\n")
check_loaded(fail "does not select the Bootlin loader")
file(REMOVE_RECURSE "${_root}")
