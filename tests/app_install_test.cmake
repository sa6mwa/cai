cmake_minimum_required(VERSION 3.21)
set(root "${CAI_BINARY_DIR}/app-install-test")
file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${root}/source")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${CAI_BINARY_DIR}"
  --prefix "${root}/stage" RESULT_VARIABLE result OUTPUT_QUIET)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "install CAI application SDK failed")
endif()
file(RENAME "${root}/stage" "${root}/relocated")
set(prefix "${root}/relocated")
foreach(path include/cai/app.h lib/libcai_app.a lib/pkgconfig/cai-app.pc
    lib/cmake/cai_app/cai_app-config.cmake share/doc/libcai_app/embedding.md
    share/cai_app/dependencies.json
    share/doc/libcai_app/examples/main.c share/doc/libcai_app/examples/app.lua)
  if(NOT EXISTS "${prefix}/${path}")
    message(FATAL_ERROR "missing application SDK payload: ${path}")
  endif()
endforeach()
file(READ "${prefix}/share/cai_app/dependencies.json" manifest)
string(JSON abi GET "${manifest}" abiVersion)
string(JSON dependency_count LENGTH "${manifest}" dependencies)
if(NOT abi EQUAL 0 OR NOT dependency_count EQUAL 4)
  message(FATAL_ERROR "invalid application dependency manifest")
endif()
foreach(index RANGE 0 3)
  string(JSON digest GET "${manifest}" dependencies ${index} sha256)
  string(LENGTH "${digest}" length)
  if(NOT length EQUAL 64 OR NOT digest MATCHES "^[a-f0-9]+$")
    message(FATAL_ERROR "invalid application dependency digest")
  endif()
endforeach()
file(COPY "${prefix}/share/doc/libcai_app/examples/main.c" DESTINATION "${root}/source")
file(RENAME "${root}/source/main.c" "${root}/source/example.c")
file(WRITE "${root}/source/main.c" [=[
#define main cai_example_main
#include "example.c"
#undef main
#include <assert.h>
#include <cai/skills.h>
int main(int argc, char **argv) {
  cai_app *app = NULL;
  cai_app_status status;
  cai_error error;
  cai_skill_metadata *metadata = NULL;
  unsigned long long id;
  cai_error_init(&error);
  assert(cai_skill_metadata_open(NULL, NULL, &metadata, &error) == CAI_ERR_INVALID);
  assert(metadata == NULL);
  cai_error_cleanup(&error);
  cai_app_status_init(&status);
  assert(cai_app_open(NULL, &app, &error) == CAI_OK);
  assert(cai_app_get_status(app, &status, &error) == CAI_OK);
  assert(status.outcome == CAI_APP_NOT_RUN && app->get_status != NULL);
  assert(cai_app_control(app, CAI_APP_STOP, NULL, &id, &error) == CAI_ERR_INVALID);
  assert(id == 0U && app->control != NULL);
  cai_app_status_cleanup(&status);
  cai_error_cleanup(&error);
  app->close(app);
  return cai_example_main(argc, argv);
}
]=])
file(WRITE "${root}/source/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.21)
project(app_consumer C)
find_package(cai_app CONFIG REQUIRED)
add_executable(app_static main.c)
target_link_libraries(app_static PRIVATE cai::app_static)
target_compile_options(app_static PRIVATE -std=c89 -Wall -Wextra -Werror -Wpedantic -Wno-long-long)
if(TARGET cai::app_shared)
  add_executable(app_shared main.c)
  target_link_libraries(app_shared PRIVATE cai::app_shared)
  target_compile_options(app_shared PRIVATE -std=c89 -Wall -Wextra -Werror -Wpedantic -Wno-long-long)
endif()
]=])
set(prefixes "${prefix};${CAI_DEPENDENCY_PREFIXES}")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${root}/source" -B "${root}/consumer"
  -G Ninja "-DCMAKE_C_COMPILER=${CAI_C_COMPILER}"
  "-DCMAKE_PREFIX_PATH=${prefixes}" -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "configure public installed application consumer:\n${output}\n${errors}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${root}/source" -B "${root}/missing-yaml"
  -G Ninja "-DCMAKE_C_COMPILER=${CAI_C_COMPILER}"
  "-DCMAKE_PREFIX_PATH=${prefixes}" -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON
  -DCMAKE_DISABLE_FIND_PACKAGE_CpktYaml=TRUE
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(result EQUAL 0 OR NOT errors MATCHES "CpktYaml")
  message(FATAL_ERROR "installed application must reject unavailable CpktYaml: ${output}\n${errors}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${root}/consumer" --parallel 4
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "link public installed application consumer:\n${output}\n${errors}")
endif()
set(loader "${prefix}/lib")
set(pkg_path "${prefix}/lib/pkgconfig")
foreach(dependency IN LISTS CAI_DEPENDENCY_PREFIXES)
  string(APPEND loader ":${dependency}/lib")
  string(APPEND pkg_path ":${dependency}/lib/pkgconfig")
endforeach()
foreach(variant static shared)
  if(EXISTS "${root}/consumer/app_${variant}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "LD_LIBRARY_PATH=${loader}"
      "${root}/consumer/app_${variant}" --help RESULT_VARIABLE result OUTPUT_VARIABLE output)
    if(NOT result EQUAL 0 OR NOT output MATCHES "Usage: cai ")
      message(FATAL_ERROR "run installed ${variant} application consumer failed")
    endif()
  endif()
endforeach()
find_program(PKG_CONFIG pkg-config REQUIRED)
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "PKG_CONFIG_PATH=${pkg_path}"
  "${PKG_CONFIG}" --cflags --libs --static cai-app
  RESULT_VARIABLE result OUTPUT_VARIABLE flags ERROR_VARIABLE errors OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "installed application pkg-config metadata: ${errors}")
endif()
separate_arguments(arguments UNIX_COMMAND "${flags}")
execute_process(COMMAND "${CAI_C_COMPILER}" -std=c89 -Wall -Wextra -Werror
  "${root}/source/main.c" ${arguments} -o "${root}/pkg-consumer"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "link application pkg-config consumer: ${output}\n${errors}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "LD_LIBRARY_PATH=${loader}"
  "${root}/pkg-consumer" --version RESULT_VARIABLE result OUTPUT_QUIET)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "run installed pkg-config application consumer failed")
endif()
if(NOT CAI_LUA_RUNNER STREQUAL "")
  if(NOT EXISTS "${prefix}/lib/lua/5.5/cai/app.so")
    message(FATAL_ERROR "application Lua module was not installed")
  endif()
  file(WRITE "${root}/lua-consumer.lua" [=[
local app = assert(require("cai.app").open {program_name = "installed-host", invocation_name = "installed host", no_config = true})
assert(app:help():find("Usage: installed host [options]", 1, true))
assert(app:run {"--help"} == 0)
assert(app:get_status().outcome == "completed")
assert(not app:control("stop"))
app:close()
]=])
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env "LD_LIBRARY_PATH=${loader}"
    "LUA_CPATH=${prefix}/lib/lua/5.5/?.so;;"
    "${CAI_LUA_RUNNER}" "${root}/lua-consumer.lua"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "run installed Lua application facade: ${output}\n${errors}")
  endif()
  # Compile the shipped facade sources against the relocated public SDK.
  # No cli/ or src/ private headers are available in this source product.
  execute_process(COMMAND bash "${CAI_SOURCE_DIR}/scripts/stage_lua_rock_sources.sh"
    "${CAI_SOURCE_DIR}" "${root}/lua-source" 0.0.0
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "stage Lua facade sources: ${output}\n${errors}")
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env "PKG_CONFIG_PATH=${pkg_path}"
    "${CMAKE_COMMAND}" -S "${root}/lua-source/lua" -B "${root}/lua-build" -G Ninja
    "-DCMAKE_C_COMPILER=${CAI_C_COMPILER}"
    "-DCAI_LUA_INCLUDE_DIR=${CAI_LUA_INCLUDE_DIR}"
    "-DCAI_PSLOG_LUA_INCLUDE_DIR=${CAI_PSLOG_LUA_INCLUDE_DIR}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "configure packaged Lua facade: ${output}\n${errors}")
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" --build "${root}/lua-build" --parallel 4
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "build packaged Lua facade: ${output}\n${errors}")
  endif()
  if(NOT EXISTS "${root}/lua-build/cai/app.so")
    message(FATAL_ERROR "packaged Lua build omitted the application facade")
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env "LD_LIBRARY_PATH=${loader}"
    "LUA_CPATH=${root}/lua-build/?.so;;"
    "${CAI_LUA_RUNNER}" "${root}/lua-consumer.lua"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "run packaged Lua application facade: ${output}\n${errors}")
  endif()
endif()
file(REMOVE_RECURSE "${root}")
