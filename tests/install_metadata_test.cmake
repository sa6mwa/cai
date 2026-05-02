if(NOT DEFINED CAI_BINARY_DIR OR CAI_BINARY_DIR STREQUAL "")
  message(FATAL_ERROR "CAI_BINARY_DIR is required")
endif()
if(NOT DEFINED CAI_VERSION OR CAI_VERSION STREQUAL "")
  set(CAI_VERSION "0.0.0")
endif()

set(prefix "${CAI_BINARY_DIR}/install-metadata-test")
file(REMOVE_RECURSE "${prefix}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${CAI_BINARY_DIR}" --prefix "${prefix}"
  RESULT_VARIABLE install_result)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "failed to install cai metadata test tree")
endif()

set(required_files
  "${prefix}/include/cai/cai.h"
  "${prefix}/include/cai/models.h"
  "${prefix}/include/cai/version.h"
  "${prefix}/lib/cmake/cai/cai-config.cmake"
  "${prefix}/lib/cmake/cai/cai-config-version.cmake"
  "${prefix}/lib/cmake/cai/cai-targets.cmake"
  "${prefix}/lib/pkgconfig/cai.pc")
foreach(path IN LISTS required_files)
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "missing installed file: ${path}")
  endif()
endforeach()

file(READ "${prefix}/lib/pkgconfig/cai.pc" pc_text)
string(FIND "${pc_text}" "prefix=\${pcfiledir}/../.." pc_prefix_pos)
if(pc_prefix_pos EQUAL -1)
  message(FATAL_ERROR "cai.pc is not relocatable")
endif()
string(FIND "${pc_text}" "Version: ${CAI_VERSION}" pc_version_pos)
if(pc_version_pos EQUAL -1)
  message(FATAL_ERROR "cai.pc version mismatch")
endif()

file(READ "${prefix}/lib/cmake/cai/cai-config.cmake" config_text)
string(FIND "${config_text}" "find_dependency(lockdc CONFIG)" lockdc_dep_pos)
string(FIND "${config_text}" "find_dependency(CURL)" curl_dep_pos)
string(FIND "${config_text}" "find_dependency(Threads)" threads_dep_pos)
if(threads_dep_pos EQUAL -1)
  message(FATAL_ERROR "cai CMake package does not declare dependencies")
endif()
if(DEFINED CAI_RESOLVED_DEPENDENCY_MODE AND
   CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "host")
  if(curl_dep_pos EQUAL -1)
    message(FATAL_ERROR "cai host CMake package does not declare curl")
  endif()
else()
  if(lockdc_dep_pos EQUAL -1)
    message(FATAL_ERROR "cai lockdc CMake package does not declare lockdc")
  endif()
endif()

string(FIND "${pc_text}" "Requires.private: lockdc" pc_lockdc_pos)
string(FIND "${pc_text}" "Requires.private: libcurl" pc_curl_pos)
string(FIND "${pc_text}" "lockdc_sdk_url=" pc_lockdc_url_pos)
if(DEFINED CAI_RESOLVED_DEPENDENCY_MODE AND
   CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "host")
  if(pc_curl_pos EQUAL -1)
    message(FATAL_ERROR "cai.pc does not point at host curl dependency")
  endif()
else()
  if(pc_lockdc_pos EQUAL -1 OR pc_lockdc_url_pos EQUAL -1)
    message(FATAL_ERROR "cai.pc does not point at the lockdc SDK dependency")
  endif()
endif()

if((NOT DEFINED CAI_RESOLVED_DEPENDENCY_MODE OR
    NOT CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "host") AND
   DEFINED CAI_LOCKDC_SDK_PREFIX AND NOT CAI_LOCKDC_SDK_PREFIX STREQUAL "" AND
   EXISTS "${CAI_LOCKDC_SDK_PREFIX}/lib/cmake/lockdc/lockdcConfig.cmake")
  set(consumer_dir "${CAI_BINARY_DIR}/install-metadata-consumer")
  file(REMOVE_RECURSE "${consumer_dir}")
  file(MAKE_DIRECTORY "${consumer_dir}")
  file(WRITE "${consumer_dir}/CMakeLists.txt"
"cmake_minimum_required(VERSION 3.21)
project(cai_consumer_smoke LANGUAGES C)
find_package(cai CONFIG REQUIRED)
add_executable(cai_consumer_smoke main.c)
target_link_libraries(cai_consumer_smoke PRIVATE cai::cai_static)
")
  file(WRITE "${consumer_dir}/main.c"
"#include <cai/cai.h>
#include <lonejson.h>
#include <pslog.h>
int main(void) {
  cai_error error;
  (void)sizeof(error);
  return 0;
}
")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${consumer_dir}" -B "${consumer_dir}/build"
            -G Ninja "-DCMAKE_PREFIX_PATH=${prefix};${CAI_LOCKDC_SDK_PREFIX}"
    RESULT_VARIABLE consumer_configure_result)
  if(NOT consumer_configure_result EQUAL 0)
    message(FATAL_ERROR "failed to configure cai installed-package consumer")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${consumer_dir}/build"
    RESULT_VARIABLE consumer_build_result)
  if(NOT consumer_build_result EQUAL 0)
    message(FATAL_ERROR "failed to build cai installed-package consumer")
  endif()
endif()
