if(NOT DEFINED CAI_BINARY_DIR OR CAI_BINARY_DIR STREQUAL "")
  message(FATAL_ERROR "CAI_BINARY_DIR is required")
endif()
if(NOT DEFINED CAI_VERSION OR CAI_VERSION STREQUAL "")
  set(CAI_VERSION "0.0.0")
endif()
if(NOT DEFINED CAI_BUILD_TYPE)
  set(CAI_BUILD_TYPE "")
endif()
if(NOT DEFINED CAI_TARGET_ID)
  set(CAI_TARGET_ID "")
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
  "${prefix}/include/cai/mcp.h"
  "${prefix}/include/cai/models.h"
  "${prefix}/include/cai/tools/revgeo.h"
  "${prefix}/include/cai/tools/searxng.h"
  "${prefix}/include/cai/tools/todo.h"
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

file(GLOB installed_shared_libraries
  "${prefix}/lib/libcai.so*"
  "${prefix}/lib/libcai.*.dylib")
set(installed_shared_library "")
foreach(path IN LISTS installed_shared_libraries)
  if(NOT IS_SYMLINK "${path}")
    set(installed_shared_library "${path}")
  endif()
endforeach()
if(installed_shared_libraries AND installed_shared_library STREQUAL "")
  list(GET installed_shared_libraries 0 installed_shared_library)
endif()
if(installed_shared_library)
  file(STRINGS "${installed_shared_library}" installed_shared_strings
       REGEX "/home/|/Users/|/opt/|/tmp/|fsanitize|__asan|__ubsan|libasan|libubsan")
  string(TOLOWER "${CAI_BUILD_TYPE}" cai_build_type_lower)
  if(cai_build_type_lower STREQUAL "release")
    if(installed_shared_strings MATCHES "/home/|/Users/|/opt/|/tmp/")
      message(FATAL_ERROR
        "release libcai contains host-specific path: ${installed_shared_strings}")
    endif()
    if(installed_shared_strings MATCHES "fsanitize|__asan|__ubsan|libasan|libubsan")
      message(FATAL_ERROR
        "release libcai contains sanitizer artifact: ${installed_shared_strings}")
    endif()
  endif()
  if(NOT CAI_TARGET_ID MATCHES "darwin")
    find_program(readelf_bin NAMES readelf)
    if(readelf_bin)
      execute_process(
        COMMAND "${readelf_bin}" -d "${installed_shared_library}"
        RESULT_VARIABLE readelf_result
        OUTPUT_VARIABLE readelf_output
        ERROR_VARIABLE readelf_error)
      if(NOT readelf_result EQUAL 0)
        message(FATAL_ERROR "readelf failed: ${readelf_error}")
      endif()
      if(NOT readelf_output MATCHES "\\$ORIGIN")
        message(FATAL_ERROR "installed libcai runpath does not contain $ORIGIN")
      endif()
      if(readelf_output MATCHES "/home/|/Users/|/opt/|/tmp/")
        message(FATAL_ERROR
          "installed libcai runpath contains host-specific path: ${readelf_output}")
      endif()
      if(cai_build_type_lower STREQUAL "release" AND
         readelf_output MATCHES "libasan|libubsan")
        message(FATAL_ERROR
          "release libcai links sanitizer runtime: ${readelf_output}")
      endif()
    endif()
  endif()
endif()

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
string(FIND "${config_text}" "find_dependency(lonejson CONFIG QUIET)"
       lonejson_dep_pos)
string(FIND "${config_text}" "find_dependency(pslog CONFIG QUIET)"
       pslog_dep_pos)
if(curl_dep_pos EQUAL -1 OR threads_dep_pos EQUAL -1 OR
   lonejson_dep_pos EQUAL -1 OR pslog_dep_pos EQUAL -1)
  message(FATAL_ERROR "cai CMake package does not declare dependencies")
endif()

string(FIND "${pc_text}" "Requires.private: libcurl" pc_curl_pos)
string(FIND "${pc_text}" "Requires: lonejson pslog" pc_public_deps_pos)
string(FIND "${pc_text}" "c_pkt_systems_url=" pc_c_pkt_url_pos)
if(pc_curl_pos EQUAL -1 OR pc_public_deps_pos EQUAL -1 OR
   pc_c_pkt_url_pos EQUAL -1)
  message(FATAL_ERROR "cai.pc does not point at required dependencies")
endif()

if((NOT DEFINED CAI_RESOLVED_DEPENDENCY_MODE OR
    CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "cpkt") AND
   DEFINED CAI_C_PKT_SYSTEMS_PREFIX AND
   NOT CAI_C_PKT_SYSTEMS_PREFIX STREQUAL "" AND
   EXISTS "${CAI_C_PKT_SYSTEMS_PREFIX}/include/curl/curl.h" AND
   DEFINED CAI_LONEJSON_PREFIX AND NOT CAI_LONEJSON_PREFIX STREQUAL "" AND
   EXISTS "${CAI_LONEJSON_PREFIX}/include/lonejson.h" AND
   DEFINED CAI_PSLOG_PREFIX AND NOT CAI_PSLOG_PREFIX STREQUAL "" AND
   EXISTS "${CAI_PSLOG_PREFIX}/include/pslog.h")
  set(consumer_dir "${CAI_BINARY_DIR}/install-metadata-consumer")
  file(REMOVE_RECURSE "${consumer_dir}")
  file(MAKE_DIRECTORY "${consumer_dir}")
  file(WRITE "${consumer_dir}/CMakeLists.txt"
"cmake_minimum_required(VERSION 3.21)
project(cai_consumer_smoke LANGUAGES C)
find_package(cai CONFIG REQUIRED)
add_executable(cai_consumer_smoke main.c)
if(TARGET cai::cai_shared)
  target_link_libraries(cai_consumer_smoke PRIVATE cai::cai_shared)
else()
  target_link_libraries(cai_consumer_smoke PRIVATE cai::cai_static)
endif()
")
  file(WRITE "${consumer_dir}/main.c"
"#include <cai/cai.h>
#include <cai/mcp.h>
#include <cai/tools/revgeo.h>
#include <cai/tools/searxng.h>
#include <cai/tools/todo.h>
#include <lonejson.h>
#include <pslog.h>
int main(void) {
  cai_error error;
  cai_tool_registry *registry = 0;
  int (*register_revgeo)(cai_tool_registry *, const cai_revgeo_tool_config *,
                         cai_error *);
  int (*register_searxng)(cai_tool_registry *, const cai_searxng_tool_config *,
                          cai_error *);
  int (*register_todo)(cai_tool_registry *, const cai_todo_tool_config *,
                       cai_error *);
  void (*mcp_config_init)(cai_mcp_handler_config *);
  int (*mcp_new)(const cai_mcp_handler_config *, cai_mcp_handler **,
                 cai_error *);
  int (*mcp_handle)(cai_mcp_handler *, const cai_mcp_http_request *,
                    cai_mcp_http_response *, cai_error *);
  void (*mcp_destroy)(cai_mcp_handler *);
  (void)sizeof(error);
  register_revgeo = cai_tool_registry_register_revgeo_tool;
  register_searxng = cai_tool_registry_register_searxng_tool;
  register_todo = cai_tool_registry_register_todo_tool;
  mcp_config_init = cai_mcp_handler_config_init;
  mcp_new = cai_mcp_handler_new;
  mcp_handle = cai_mcp_handler_handle_http;
  mcp_destroy = cai_mcp_handler_destroy;
  if (register_revgeo == 0 || register_searxng == 0 || register_todo == 0 ||
      mcp_config_init == 0 || mcp_new == 0 || mcp_handle == 0 ||
      mcp_destroy == 0) {
    return 1;
  }
  (void)registry;
  return 0;
}
")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${consumer_dir}" -B "${consumer_dir}/build"
            -G Ninja
            "-DCMAKE_PREFIX_PATH=${prefix};${CAI_C_PKT_SYSTEMS_PREFIX};${CAI_LONEJSON_PREFIX};${CAI_PSLOG_PREFIX}"
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
