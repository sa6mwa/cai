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
string(FIND "${config_text}" "find_dependency(CURL)" curl_dep_pos)
string(FIND "${config_text}" "find_dependency(Threads)" threads_dep_pos)
if(curl_dep_pos EQUAL -1 OR threads_dep_pos EQUAL -1)
  message(FATAL_ERROR "cai CMake package does not declare dependencies")
endif()
