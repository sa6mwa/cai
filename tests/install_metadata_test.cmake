if(NOT DEFINED CAI_BINARY_DIR OR CAI_BINARY_DIR STREQUAL "")
  message(FATAL_ERROR "CAI_BINARY_DIR is required")
endif()
if(NOT DEFINED CAI_VERSION OR CAI_VERSION STREQUAL "")
  set(CAI_VERSION "0.0.0")
endif()
if(NOT DEFINED CAI_BUILD_TYPE)
  set(CAI_BUILD_TYPE "")
endif()
if(NOT DEFINED CAI_BUILD_SHARED)
  set(CAI_BUILD_SHARED ON)
endif()
if(NOT DEFINED CAI_BUILD_STATIC)
  set(CAI_BUILD_STATIC ON)
endif()
if(NOT DEFINED CAI_TARGET_ID)
  set(CAI_TARGET_ID "")
endif()
if(NOT DEFINED CAI_INSTALL_RPATH_TOKEN)
  set(CAI_INSTALL_RPATH_TOKEN "$ORIGIN")
endif()

if(CAI_TARGET_ID MATCHES "darwin")
  if(NOT CAI_INSTALL_RPATH_TOKEN STREQUAL "@loader_path")
    message(FATAL_ERROR
      "Darwin install rpath token must be @loader_path, got ${CAI_INSTALL_RPATH_TOKEN}")
  endif()
else()
  if(NOT CAI_INSTALL_RPATH_TOKEN STREQUAL "$ORIGIN")
    message(FATAL_ERROR
      "ELF install rpath token must be $ORIGIN, got ${CAI_INSTALL_RPATH_TOKEN}")
  endif()
endif()
if(NOT DEFINED CAI_C_COMPILER OR CAI_C_COMPILER STREQUAL "")
  message(FATAL_ERROR "CAI_C_COMPILER is required")
endif()
if(NOT DEFINED CAI_LONEJSON_ABI_VERSION OR CAI_LONEJSON_ABI_VERSION STREQUAL "")
  message(FATAL_ERROR "CAI_LONEJSON_ABI_VERSION is required")
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
  "${prefix}/include/cai/tools/exec.h"
  "${prefix}/include/cai/tools/read.h"
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
if(CAI_BUILD_SHARED AND NOT installed_shared_library)
  message(FATAL_ERROR "shared build did not install a libcai shared library")
endif()
if(NOT CAI_BUILD_SHARED AND installed_shared_library)
  message(FATAL_ERROR "static-only build installed a libcai shared library")
endif()
if(CAI_BUILD_STATIC AND NOT EXISTS "${prefix}/lib/libcai.a")
  message(FATAL_ERROR "static build did not install libcai.a")
endif()
if(installed_shared_library)
  file(STRINGS "${installed_shared_library}" installed_shared_strings
       REGEX "/home/|/Users/|/opt/|/tmp/|fsanitize|__asan|__ubsan|__tsan|__msan|libasan|libubsan|libtsan|libmsan")
  string(TOLOWER "${CAI_BUILD_TYPE}" cai_build_type_lower)
  if(cai_build_type_lower STREQUAL "release")
    if(installed_shared_strings MATCHES "/home/|/Users/|/opt/|/tmp/")
      message(FATAL_ERROR
        "release libcai contains host-specific path: ${installed_shared_strings}")
    endif()
    if(installed_shared_strings MATCHES "fsanitize|__asan|__ubsan|__tsan|__msan|libasan|libubsan|libtsan|libmsan")
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
      if((NOT DEFINED CAI_RESOLVED_DEPENDENCY_MODE OR
          CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "cpkt") AND
         NOT readelf_output MATCHES "\\$ORIGIN")
        message(FATAL_ERROR "installed libcai runpath does not contain $ORIGIN")
      endif()
      if(readelf_output MATCHES "/home/|/Users/|/opt/|/tmp/")
        message(FATAL_ERROR
          "installed libcai runpath contains host-specific path: ${readelf_output}")
      endif()
      if(cai_build_type_lower STREQUAL "release" AND
         readelf_output MATCHES "libasan|libubsan|libtsan|libmsan")
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
string(FIND "${config_text}" "find_dependency(CURL 7.86.0)" curl_dep_pos)
string(FIND "${config_text}" "find_dependency(Threads)" threads_dep_pos)
string(FIND "${config_text}" "find_package(lonejson CONFIG QUIET)"
       lonejson_dep_pos)
string(FIND "${config_text}" "cai requires external liblonejson"
       lonejson_error_pos)
string(FIND "${config_text}" "cai_LONEJSON_URL" lonejson_url_pos)
string(FIND "${config_text}" "cai_LONEJSON_ABI_VERSION" lonejson_abi_pos)
if(curl_dep_pos EQUAL -1 OR threads_dep_pos EQUAL -1 OR
   lonejson_dep_pos EQUAL -1 OR lonejson_error_pos EQUAL -1 OR
   lonejson_url_pos EQUAL -1 OR lonejson_abi_pos EQUAL -1)
  message(FATAL_ERROR "cai CMake package does not declare dependencies")
endif()

string(FIND "${pc_text}" "Requires.private: libcurl >= 7.86.0" pc_curl_pos)
string(FIND "${pc_text}" "Requires: lonejson" pc_public_deps_pos)
string(FIND "${pc_text}" "c_pkt_systems_url=" pc_c_pkt_url_pos)
string(FIND "${pc_text}" "lonejson_url=" pc_lonejson_url_pos)
string(FIND "${pc_text}" "lonejson_abi_version=" pc_lonejson_abi_pos)
if(pc_curl_pos EQUAL -1 OR pc_public_deps_pos EQUAL -1 OR
   pc_c_pkt_url_pos EQUAL -1 OR pc_lonejson_url_pos EQUAL -1 OR
   pc_lonejson_abi_pos EQUAL -1)
  message(FATAL_ERROR "cai.pc does not point at required dependencies")
endif()

if(DEFINED CAI_SOURCE_DIR AND NOT CAI_SOURCE_DIR STREQUAL "")
  set(multiarch_dir "${CAI_BINARY_DIR}/pkgconfig-multiarch-test")
  file(REMOVE_RECURSE "${multiarch_dir}")
  set(configure_command
    "${CMAKE_COMMAND}"
    -S "${CAI_SOURCE_DIR}"
    -B "${multiarch_dir}"
    -DCAI_BUILD_STATIC=OFF
    -DCAI_BUILD_SHARED=ON
    -DCAI_BUILD_TESTS=OFF
    -DCAI_BUILD_INTEGRATION_TESTS=OFF
    -DCAI_BUILD_EXAMPLES=OFF
    -DCAI_BUILD_LUA=OFF
    -DCAI_BUILD_FUZZERS=OFF
    -DCAI_INSTALL=ON
    -DCAI_DEPENDENCY_MODE=${CAI_RESOLVED_DEPENDENCY_MODE}
    -DCAI_DEPS_DIR=${CAI_DEPS_DIR}
    -DCMAKE_INSTALL_LIBDIR=lib/x86_64-linux-gnu)
  if(DEFINED CAI_GENERATOR AND NOT CAI_GENERATOR STREQUAL "")
    list(APPEND configure_command -G "${CAI_GENERATOR}")
  endif()
  if(DEFINED CAI_LONEJSON_INCLUDE_DIR AND
     NOT CAI_LONEJSON_INCLUDE_DIR STREQUAL "")
    list(APPEND configure_command
         -DCAI_LONEJSON_INCLUDE_DIR=${CAI_LONEJSON_INCLUDE_DIR})
  endif()
  if(DEFINED CAI_LONEJSON_LIBRARY AND NOT CAI_LONEJSON_LIBRARY STREQUAL "")
    list(APPEND configure_command
         -DCAI_LONEJSON_LIBRARY=${CAI_LONEJSON_LIBRARY})
  endif()
  if(DEFINED CAI_PSLOG_INCLUDE_DIR AND NOT CAI_PSLOG_INCLUDE_DIR STREQUAL "")
    list(APPEND configure_command -DCAI_PSLOG_INCLUDE_DIR=${CAI_PSLOG_INCLUDE_DIR})
  endif()
  if(DEFINED CAI_PARENT_CMAKE_PREFIX_PATH AND
     NOT CAI_PARENT_CMAKE_PREFIX_PATH STREQUAL "")
    list(APPEND configure_command
         -DCMAKE_PREFIX_PATH=${CAI_PARENT_CMAKE_PREFIX_PATH})
  endif()
  execute_process(
    COMMAND ${configure_command}
    RESULT_VARIABLE multiarch_configure_result
    OUTPUT_VARIABLE multiarch_configure_output
    ERROR_VARIABLE multiarch_configure_error)
  if(NOT multiarch_configure_result EQUAL 0)
    message(FATAL_ERROR
      "multiarch pkg-config configure failed: ${multiarch_configure_error}\n${multiarch_configure_output}")
  endif()
  file(READ "${multiarch_dir}/cai.pc" multiarch_pc_text)
  string(FIND "${multiarch_pc_text}"
              "prefix=\${pcfiledir}/../../.." multiarch_prefix_pos)
  string(FIND "${multiarch_pc_text}"
              "libdir=\${exec_prefix}/lib/x86_64-linux-gnu"
              multiarch_libdir_pos)
  string(FIND "${multiarch_pc_text}" "includedir=\${prefix}/include"
              multiarch_includedir_pos)
  if(multiarch_prefix_pos EQUAL -1 OR multiarch_libdir_pos EQUAL -1 OR
     multiarch_includedir_pos EQUAL -1)
    message(FATAL_ERROR
      "cai.pc does not derive prefix correctly for multiarch libdirs")
  endif()
endif()

if((NOT DEFINED CAI_RESOLVED_DEPENDENCY_MODE OR
    CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "cpkt") AND
   DEFINED CAI_C_PKT_SYSTEMS_PREFIX AND
   NOT CAI_C_PKT_SYSTEMS_PREFIX STREQUAL "" AND
   EXISTS "${CAI_C_PKT_SYSTEMS_PREFIX}/include/curl/curl.h" AND
   DEFINED CAI_LONEJSON_PREFIX AND NOT CAI_LONEJSON_PREFIX STREQUAL "" AND
   EXISTS "${CAI_LONEJSON_PREFIX}/include/lonejson.h")
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
#include <cai/tools/exec.h>
#include <cai/tools/read.h>
#include <cai/tools/revgeo.h>
#include <cai/tools/searxng.h>
#include <cai/tools/todo.h>
#include <lonejson.h>
int main(void) {
  cai_error error;
  cai_tool_registry *registry = 0;
  int (*register_exec)(cai_tool_registry *, const cai_exec_tool_config *,
                       cai_error *);
  int (*register_read)(cai_tool_registry *, const cai_read_tool_config *,
                       cai_error *);
  int (*register_list_files)(cai_tool_registry *, const cai_read_tool_config *,
                             cai_error *);
  int (*register_revgeo)(cai_tool_registry *, const cai_revgeo_tool_config *,
                         cai_error *);
  int (*register_searxng)(cai_tool_registry *, const cai_searxng_tool_config *,
                          cai_error *);
  int (*register_todo)(cai_tool_registry *, const cai_todo_tool_config *,
                       cai_error *);
  int (*register_raw_spooled)(cai_tool_registry *, const char *, const char *,
                              const char *, int, cai_tool_raw_spooled_fn,
                              void *, cai_error *);
  int (*agent_register_mcp)(cai_agent *, cai_mcp_client *,
                            const cai_mcp_tool_registration_config *,
                            cai_error *);
  int (*set_tool_choice_json)(cai_response_create_params *, const char *,
                              cai_error *);
  int (*set_max_tool_calls)(cai_response_create_params *, int, cai_error *);
  int (*count_input_tokens)(cai_client *, const cai_response_create_params *,
                            cai_token_usage *, cai_error *);
  void (*mcp_config_init)(cai_mcp_handler_config *);
  int (*mcp_new)(const cai_mcp_handler_config *, cai_mcp_handler **,
                 cai_error *);
  int (*mcp_handle)(cai_mcp_handler *, const cai_mcp_http_request *,
                    cai_mcp_http_response *, cai_error *);
  void (*mcp_destroy)(cai_mcp_handler *);
  void (*mcp_client_config_init)(cai_mcp_streamable_http_client_config *);
  int (*mcp_client_open)(const cai_mcp_streamable_http_client_config *,
                         cai_mcp_client **, cai_error *);
  int (*mcp_client_initialize)(cai_mcp_client *, cai_error *);
  int (*mcp_client_ping)(cai_mcp_client *, cai_error *);
  int (*mcp_client_refresh_tools)(cai_mcp_client *, cai_error *);
  size_t (*mcp_client_tool_count)(const cai_mcp_client *);
  const cai_mcp_client_tool *(*mcp_client_tool_at)(const cai_mcp_client *,
                                                   size_t);
  int (*mcp_client_call_tool)(cai_mcp_client *, const char *,
                              struct lonejson_spooled *, cai_sink *,
                              cai_error *);
  int (*mcp_client_call_tool_task)(cai_mcp_client *, const char *,
                                   struct lonejson_spooled *, long long,
                                   cai_sink *, cai_error *);
  int (*mcp_client_refresh_resources)(cai_mcp_client *, cai_error *);
  size_t (*mcp_client_resource_count)(const cai_mcp_client *);
  const cai_mcp_client_resource *(*mcp_client_resource_at)(
      const cai_mcp_client *, size_t);
  int (*mcp_client_read_resource)(cai_mcp_client *, const char *, cai_sink *,
                                  cai_error *);
  int (*mcp_client_subscribe_resource)(cai_mcp_client *, const char *,
                                       cai_error *);
  int (*mcp_client_unsubscribe_resource)(cai_mcp_client *, const char *,
                                         cai_error *);
  int (*mcp_client_refresh_resource_templates)(cai_mcp_client *, cai_error *);
  size_t (*mcp_client_resource_template_count)(const cai_mcp_client *);
  const cai_mcp_client_resource_template *(*mcp_client_resource_template_at)(
      const cai_mcp_client *, size_t);
  int (*mcp_client_refresh_prompts)(cai_mcp_client *, cai_error *);
  size_t (*mcp_client_prompt_count)(const cai_mcp_client *);
  const cai_mcp_client_prompt *(*mcp_client_prompt_at)(const cai_mcp_client *,
                                                       size_t);
  int (*mcp_client_get_prompt)(cai_mcp_client *, const char *,
                               struct lonejson_spooled *, cai_sink *,
                               cai_error *);
  int (*mcp_client_complete)(cai_mcp_client *, const char *, const char *,
                             const char *, const char *,
                             struct lonejson_spooled *, cai_sink *,
                             cai_error *);
  int (*mcp_client_set_log_level)(cai_mcp_client *, const char *, cai_error *);
  int (*mcp_client_terminate_session)(cai_mcp_client *, cai_error *);
  int (*mcp_client_send_request)(cai_mcp_client *, const char *,
                                 struct lonejson_spooled *, cai_sink *,
                                 cai_error *);
  int (*mcp_client_send_notification)(cai_mcp_client *, const char *,
                                      struct lonejson_spooled *, cai_error *);
  int (*mcp_client_notify_roots_list_changed)(cai_mcp_client *, cai_error *);
  int (*mcp_client_list_tasks)(cai_mcp_client *, const char *, cai_sink *,
                               cai_error *);
  int (*mcp_client_get_task)(cai_mcp_client *, const char *, cai_sink *,
                             cai_error *);
  int (*mcp_client_get_task_result)(cai_mcp_client *, const char *, cai_sink *,
                                    cai_error *);
  int (*mcp_client_cancel_task)(cai_mcp_client *, const char *, cai_sink *,
                                cai_error *);
  int (*mcp_client_drain_events)(cai_mcp_client *, cai_error *);
  int (*mcp_client_register)(cai_mcp_client *, cai_tool_registry *,
                             const cai_mcp_tool_registration_config *,
                             cai_error *);
  void (*mcp_client_destroy)(cai_mcp_client *);
  cai_mcp_session_callbacks session_callbacks;
  cai_mcp_session_state session_state;
  (void)sizeof(error);
  register_exec = cai_tool_registry_register_exec_tool;
  register_read = cai_tool_registry_register_read_tool;
  register_list_files = cai_tool_registry_register_list_files_tool;
  register_revgeo = cai_tool_registry_register_revgeo_tool;
  register_searxng = cai_tool_registry_register_searxng_tool;
  register_todo = cai_tool_registry_register_todo_tool;
  register_raw_spooled = cai_tool_registry_register_raw_spooled;
  agent_register_mcp = cai_agent_register_mcp_client_tools;
  set_tool_choice_json = cai_response_create_params_set_tool_choice_json;
  set_max_tool_calls = cai_response_create_params_set_max_tool_calls;
  count_input_tokens = cai_client_count_response_input_tokens;
  mcp_config_init = cai_mcp_handler_config_init;
  mcp_new = cai_mcp_handler_new;
  mcp_handle = cai_mcp_handler_handle_http;
  mcp_destroy = cai_mcp_handler_destroy;
  mcp_client_config_init = cai_mcp_streamable_http_client_config_init;
  mcp_client_open = cai_mcp_streamable_http_client_open;
  mcp_client_initialize = cai_mcp_client_initialize;
  mcp_client_ping = cai_mcp_client_ping;
  mcp_client_refresh_tools = cai_mcp_client_refresh_tools;
  mcp_client_tool_count = cai_mcp_client_tool_count;
  mcp_client_tool_at = cai_mcp_client_tool_at;
  mcp_client_call_tool = cai_mcp_client_call_tool;
  mcp_client_call_tool_task = cai_mcp_client_call_tool_task;
  mcp_client_refresh_resources = cai_mcp_client_refresh_resources;
  mcp_client_resource_count = cai_mcp_client_resource_count;
  mcp_client_resource_at = cai_mcp_client_resource_at;
  mcp_client_read_resource = cai_mcp_client_read_resource;
  mcp_client_subscribe_resource = cai_mcp_client_subscribe_resource;
  mcp_client_unsubscribe_resource = cai_mcp_client_unsubscribe_resource;
  mcp_client_refresh_resource_templates =
      cai_mcp_client_refresh_resource_templates;
  mcp_client_resource_template_count = cai_mcp_client_resource_template_count;
  mcp_client_resource_template_at = cai_mcp_client_resource_template_at;
  mcp_client_refresh_prompts = cai_mcp_client_refresh_prompts;
  mcp_client_prompt_count = cai_mcp_client_prompt_count;
  mcp_client_prompt_at = cai_mcp_client_prompt_at;
  mcp_client_get_prompt = cai_mcp_client_get_prompt;
  mcp_client_complete = cai_mcp_client_complete;
  mcp_client_set_log_level = cai_mcp_client_set_log_level;
  mcp_client_terminate_session = cai_mcp_client_terminate_session;
  mcp_client_send_request = cai_mcp_client_send_request;
  mcp_client_send_notification = cai_mcp_client_send_notification;
  mcp_client_notify_roots_list_changed =
      cai_mcp_client_notify_roots_list_changed;
  mcp_client_list_tasks = cai_mcp_client_list_tasks;
  mcp_client_get_task = cai_mcp_client_get_task;
  mcp_client_get_task_result = cai_mcp_client_get_task_result;
  mcp_client_cancel_task = cai_mcp_client_cancel_task;
  mcp_client_drain_events = cai_mcp_client_drain_events;
  mcp_client_register = cai_mcp_client_register_tools;
  mcp_client_destroy = cai_mcp_client_destroy;
  (void)sizeof(session_callbacks);
  (void)sizeof(session_state);
  if (register_exec == 0 || register_read == 0 ||
      register_list_files == 0 || register_revgeo == 0 ||
      register_searxng == 0 || register_todo == 0 ||
      register_raw_spooled == 0 || set_tool_choice_json == 0 ||
      set_max_tool_calls == 0 || count_input_tokens == 0 ||
      agent_register_mcp == 0 ||
      mcp_config_init == 0 || mcp_new == 0 || mcp_handle == 0 ||
      mcp_destroy == 0 || mcp_client_config_init == 0 ||
      mcp_client_open == 0 || mcp_client_initialize == 0 ||
      mcp_client_ping == 0 ||
      mcp_client_refresh_tools == 0 || mcp_client_tool_count == 0 ||
      mcp_client_tool_at == 0 || mcp_client_call_tool == 0 ||
      mcp_client_call_tool_task == 0 ||
      mcp_client_refresh_resources == 0 || mcp_client_resource_count == 0 ||
      mcp_client_resource_at == 0 || mcp_client_read_resource == 0 ||
      mcp_client_subscribe_resource == 0 ||
      mcp_client_unsubscribe_resource == 0 ||
      mcp_client_refresh_resource_templates == 0 ||
      mcp_client_resource_template_count == 0 ||
      mcp_client_resource_template_at == 0 ||
      mcp_client_refresh_prompts == 0 || mcp_client_prompt_count == 0 ||
      mcp_client_prompt_at == 0 || mcp_client_get_prompt == 0 ||
      mcp_client_complete == 0 || mcp_client_set_log_level == 0 ||
      mcp_client_terminate_session == 0 ||
      mcp_client_send_request == 0 || mcp_client_send_notification == 0 ||
      mcp_client_notify_roots_list_changed == 0 ||
      mcp_client_list_tasks == 0 || mcp_client_get_task == 0 ||
      mcp_client_get_task_result == 0 || mcp_client_cancel_task == 0 ||
      mcp_client_drain_events == 0 || mcp_client_register == 0 ||
      mcp_client_destroy == 0) {
    return 1;
  }
  (void)registry;
  return 0;
}
")
  set(consumer_prefix_path
      "${prefix};${CAI_C_PKT_SYSTEMS_PREFIX};${CAI_LONEJSON_PREFIX};${CAI_PSLOG_PREFIX}")
  string(REPLACE ";" "\\;" consumer_prefix_path_arg "${consumer_prefix_path}")
  set(consumer_configure_command
    "${CMAKE_COMMAND}"
    -S "${consumer_dir}"
    -B "${consumer_dir}/build"
    "-DCMAKE_PREFIX_PATH=${consumer_prefix_path_arg}")
  if(DEFINED CAI_GENERATOR AND NOT CAI_GENERATOR STREQUAL "")
    list(APPEND consumer_configure_command -G "${CAI_GENERATOR}")
  endif()
  execute_process(
    COMMAND ${consumer_configure_command}
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

  set(fallback_deps_dir "${CAI_BINARY_DIR}/install-metadata-fallback-deps")
  file(REMOVE_RECURSE "${fallback_deps_dir}")
  file(MAKE_DIRECTORY "${fallback_deps_dir}/include" "${fallback_deps_dir}/lib")
  file(COPY "${CAI_LONEJSON_PREFIX}/include/"
       DESTINATION "${fallback_deps_dir}/include")
  file(GLOB fallback_lonejson_libs
       "${CAI_LONEJSON_PREFIX}/lib/liblonejson.*"
       "${CAI_LONEJSON_PREFIX}/lib/liblonejson.so*")
  foreach(fallback_lib IN LISTS fallback_lonejson_libs)
    file(COPY "${fallback_lib}" DESTINATION "${fallback_deps_dir}/lib"
         FOLLOW_SYMLINK_CHAIN)
  endforeach()

  set(fallback_consumer_dir
      "${CAI_BINARY_DIR}/install-metadata-fallback-consumer")
  file(REMOVE_RECURSE "${fallback_consumer_dir}")
  file(MAKE_DIRECTORY "${fallback_consumer_dir}")
  file(WRITE "${fallback_consumer_dir}/CMakeLists.txt"
"cmake_minimum_required(VERSION 3.21)
project(cai_consumer_fallback_smoke LANGUAGES C)
find_package(cai CONFIG REQUIRED)
add_executable(cai_consumer_fallback_smoke main.c)
if(TARGET cai::cai_shared)
  target_link_libraries(cai_consumer_fallback_smoke PRIVATE cai::cai_shared)
else()
  target_link_libraries(cai_consumer_fallback_smoke PRIVATE cai::cai_static)
endif()
")
  file(WRITE "${fallback_consumer_dir}/main.c"
"#include <cai/cai.h>
#include <lonejson.h>
int main(void) {
  cai_client_config config;
  cai_client_config_init(&config);
  return sizeof(config) == 0 ? 1 : 0;
}
")
  set(fallback_prefix_path
      "${prefix};${CAI_C_PKT_SYSTEMS_PREFIX};${fallback_deps_dir}")
  string(REPLACE ";" "\\;" fallback_prefix_path_arg "${fallback_prefix_path}")
  set(fallback_consumer_configure_command
    "${CMAKE_COMMAND}"
    -S "${fallback_consumer_dir}"
    -B "${fallback_consumer_dir}/build"
    "-DCMAKE_PREFIX_PATH=${fallback_prefix_path_arg}")
  if(DEFINED CAI_GENERATOR AND NOT CAI_GENERATOR STREQUAL "")
    list(APPEND fallback_consumer_configure_command -G "${CAI_GENERATOR}")
  endif()
  execute_process(
    COMMAND ${fallback_consumer_configure_command}
    RESULT_VARIABLE fallback_consumer_configure_result)
  if(NOT fallback_consumer_configure_result EQUAL 0)
    message(FATAL_ERROR
      "failed to configure cai installed-package fallback consumer")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${fallback_consumer_dir}/build"
    RESULT_VARIABLE fallback_consumer_build_result)
  if(NOT fallback_consumer_build_result EQUAL 0)
    message(FATAL_ERROR
      "failed to build cai installed-package fallback consumer")
  endif()

  if(CAI_BUILD_SHARED)
    set(shared_only_consumer_dir
        "${CAI_BINARY_DIR}/install-metadata-shared-only-consumer")
    set(shared_only_fake_lonejson_dir
        "${shared_only_consumer_dir}/fake-lonejson")
    file(REMOVE_RECURSE "${shared_only_consumer_dir}")
    file(MAKE_DIRECTORY "${shared_only_consumer_dir}"
                        "${shared_only_fake_lonejson_dir}/lib")
    file(WRITE "${shared_only_fake_lonejson_dir}/fake_lonejson.c"
               "void cai_fake_lonejson(void) {}\n")
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
      set(shared_only_fake_lonejson_library
          "${shared_only_fake_lonejson_dir}/lib/liblonejson.dylib")
      set(shared_only_fake_lonejson_link_flag
          "-Wl,-install_name,@rpath/liblonejson.${CAI_LONEJSON_ABI_VERSION}.dylib")
    else()
      set(shared_only_fake_lonejson_library
          "${shared_only_fake_lonejson_dir}/lib/liblonejson.so")
      set(shared_only_fake_lonejson_link_flag
          "-Wl,-soname,liblonejson.so.${CAI_LONEJSON_ABI_VERSION}")
    endif()
    execute_process(
      COMMAND "${CAI_C_COMPILER}" -shared
              "${shared_only_fake_lonejson_link_flag}"
              -o "${shared_only_fake_lonejson_library}"
              "${shared_only_fake_lonejson_dir}/fake_lonejson.c"
      RESULT_VARIABLE shared_only_fake_lib_result
      OUTPUT_VARIABLE shared_only_fake_lib_output
      ERROR_VARIABLE shared_only_fake_lib_error)
    if(NOT shared_only_fake_lib_result EQUAL 0)
      message(FATAL_ERROR
        "failed to build fake lonejson shared library:\n"
        "${shared_only_fake_lib_error}\n${shared_only_fake_lib_output}")
    endif()
    file(WRITE "${shared_only_consumer_dir}/CMakeLists.txt"
"cmake_minimum_required(VERSION 3.21)
project(cai_shared_only_consumer LANGUAGES C)
find_package(cai CONFIG REQUIRED)
add_executable(cai_shared_only_consumer main.c)
target_link_libraries(cai_shared_only_consumer PRIVATE cai::cai_shared)
")
    file(WRITE "${shared_only_consumer_dir}/main.c"
"#include <cai/cai.h>
int main(void) {
  cai_error error;
  cai_error_init(&error);
  cai_error_cleanup(&error);
  return 0;
}
")
    set(shared_only_consumer_configure_command
      "${CMAKE_COMMAND}"
      -S "${shared_only_consumer_dir}"
      -B "${shared_only_consumer_dir}/build"
      "-Dcai_DIR=${prefix}/lib/cmake/cai"
      "-DCMAKE_LIBRARY_PATH=${shared_only_fake_lonejson_dir}/lib")
    execute_process(
      COMMAND ${shared_only_consumer_configure_command}
      RESULT_VARIABLE shared_only_consumer_configure_result
      OUTPUT_VARIABLE shared_only_consumer_configure_output
      ERROR_VARIABLE shared_only_consumer_configure_error)
    if(NOT shared_only_consumer_configure_result EQUAL 0)
      message(FATAL_ERROR
        "shared-only installed-package consumer should configure with "
        "liblonejson but without lonejson headers:\n"
        "${shared_only_consumer_configure_error}\n"
        "${shared_only_consumer_configure_output}")
    endif()
  endif()

  set(missing_dep_consumer_dir
      "${CAI_BINARY_DIR}/install-metadata-missing-lonejson-consumer")
  file(REMOVE_RECURSE "${missing_dep_consumer_dir}")
  file(MAKE_DIRECTORY "${missing_dep_consumer_dir}")
  file(WRITE "${missing_dep_consumer_dir}/CMakeLists.txt"
"cmake_minimum_required(VERSION 3.21)
project(cai_missing_lonejson_consumer LANGUAGES C)
find_package(cai CONFIG REQUIRED)
")
  set(missing_dep_ignore_path
      "${CAI_C_PKT_SYSTEMS_PREFIX};${CAI_LONEJSON_PREFIX};${prefix};/usr;/usr/local;/lib;/lib64")
  string(REPLACE ";" "\\;" missing_dep_ignore_path_arg
         "${missing_dep_ignore_path}")
  set(missing_dep_consumer_configure_command
    "${CMAKE_COMMAND}"
    -S "${missing_dep_consumer_dir}"
    -B "${missing_dep_consumer_dir}/build"
    "-Dcai_DIR=${prefix}/lib/cmake/cai"
    "-DCMAKE_IGNORE_PATH=${missing_dep_ignore_path_arg}")
  execute_process(
    COMMAND ${missing_dep_consumer_configure_command}
    RESULT_VARIABLE missing_dep_consumer_configure_result
    OUTPUT_VARIABLE missing_dep_consumer_configure_output
    ERROR_VARIABLE missing_dep_consumer_configure_error)
  if(missing_dep_consumer_configure_result EQUAL 0)
    message(FATAL_ERROR
      "installed-package consumer should fail when liblonejson is not "
      "discoverable")
  endif()
  if(NOT missing_dep_consumer_configure_error MATCHES
        "cai requires external liblonejson")
    message(FATAL_ERROR
      "missing-lonejson consumer did not emit actionable error:\n"
      "${missing_dep_consumer_configure_error}\n"
      "${missing_dep_consumer_configure_output}")
  endif()

  set(build_tree_consumer_dir "${CAI_BINARY_DIR}/build-tree-static-consumer")
  file(REMOVE_RECURSE "${build_tree_consumer_dir}")
  file(MAKE_DIRECTORY "${build_tree_consumer_dir}")
  file(WRITE "${build_tree_consumer_dir}/CMakeLists.txt"
"cmake_minimum_required(VERSION 3.21)
project(cai_build_tree_static_consumer LANGUAGES C)
set(CAI_BUILD_STATIC ON CACHE BOOL \"\" FORCE)
set(CAI_BUILD_SHARED OFF CACHE BOOL \"\" FORCE)
set(CAI_BUILD_TESTS OFF CACHE BOOL \"\" FORCE)
set(CAI_BUILD_INTEGRATION_TESTS OFF CACHE BOOL \"\" FORCE)
set(CAI_BUILD_EXAMPLES OFF CACHE BOOL \"\" FORCE)
set(CAI_BUILD_LUA OFF CACHE BOOL \"\" FORCE)
set(CAI_BUILD_FUZZERS OFF CACHE BOOL \"\" FORCE)
set(CAI_INSTALL OFF CACHE BOOL \"\" FORCE)
set(CAI_DEPENDENCY_MODE \"${CAI_RESOLVED_DEPENDENCY_MODE}\" CACHE STRING \"\" FORCE)
set(CAI_DEPS_DIR \"${CAI_DEPS_DIR}\" CACHE PATH \"\" FORCE)
set(CAI_LONEJSON_INCLUDE_DIR \"${CAI_LONEJSON_INCLUDE_DIR}\" CACHE PATH \"\" FORCE)
set(CAI_LONEJSON_LIBRARY \"${CAI_LONEJSON_LIBRARY}\" CACHE FILEPATH \"\" FORCE)
set(CAI_PSLOG_INCLUDE_DIR \"${CAI_PSLOG_INCLUDE_DIR}\" CACHE PATH \"\" FORCE)
add_subdirectory(\"${CAI_SOURCE_DIR}\" cai-src)
add_executable(cai_build_tree_static_consumer main.c)
target_link_libraries(cai_build_tree_static_consumer PRIVATE cai::static)
")
  file(WRITE "${build_tree_consumer_dir}/main.c"
"#include <cai/cai.h>
int main(void) {
  cai_client_config config;
  int (*open_fn)(const cai_client_config *, cai_client **, cai_error *);
  cai_client_config_init(&config);
  open_fn = cai_client_open;
  return open_fn == 0 ? 1 : 0;
}
")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${build_tree_consumer_dir}"
            -B "${build_tree_consumer_dir}/build"
            -G Ninja
            "-DCMAKE_PREFIX_PATH=${CAI_C_PKT_SYSTEMS_PREFIX};${CAI_LONEJSON_PREFIX};${CAI_PSLOG_PREFIX}"
    RESULT_VARIABLE build_tree_configure_result
    OUTPUT_VARIABLE build_tree_configure_output
    ERROR_VARIABLE build_tree_configure_error)
  if(NOT build_tree_configure_result EQUAL 0)
    message(FATAL_ERROR
      "failed to configure cai build-tree static consumer: ${build_tree_configure_error}\n${build_tree_configure_output}")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${build_tree_consumer_dir}/build"
    RESULT_VARIABLE build_tree_build_result)
  if(NOT build_tree_build_result EQUAL 0)
    message(FATAL_ERROR "failed to build cai build-tree static consumer")
  endif()
endif()
