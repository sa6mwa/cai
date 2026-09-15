if(NOT DEFINED CAI_SOURCE_DIR OR CAI_SOURCE_DIR STREQUAL "")
  message(FATAL_ERROR "CAI_SOURCE_DIR is required")
endif()
if(NOT DEFINED CAI_BINARY_DIR OR CAI_BINARY_DIR STREQUAL "")
  message(FATAL_ERROR "CAI_BINARY_DIR is required")
endif()
if(NOT DEFINED CAI_GENERATOR OR CAI_GENERATOR STREQUAL "")
  message(FATAL_ERROR "CAI_GENERATOR is required")
endif()

file(REMOVE_RECURSE "${CAI_BINARY_DIR}")
string(REPLACE ";" "\\;" escaped_cmake_prefix_path "${CMAKE_PREFIX_PATH}")
set(configure_command
  "${CMAKE_COMMAND}"
  -S "${CAI_SOURCE_DIR}"
  -B "${CAI_BINARY_DIR}"
  "-G${CAI_GENERATOR}"
  "-DCAI_BUILD_INTEGRATION_TESTS=ON"
  "-DCAI_REGISTER_API_KEY_EXAMPLE_SMOKE=ON"
  "-DCAI_BUILD_LUA=OFF"
  "-DCAI_BUILD_EXAMPLES=ON"
  "-DCAI_BUILD_FUZZERS=OFF"
  "-DCAI_BUILD_TESTS=ON"
  "-DCAI_INSTALL=OFF"
  "-DCAI_DEPENDENCY_MODE=${CAI_DEPENDENCY_MODE}"
  "-DCAI_DEPS_DIR=${CAI_DEPS_DIR}"
  "-DCAI_C_PKT_SYSTEMS_PREFIX=${CAI_C_PKT_SYSTEMS_PREFIX}"
  "-DCAI_LONEJSON_PREFIX=${CAI_LONEJSON_PREFIX}"
  "-DCAI_PSLOG_PREFIX=${CAI_PSLOG_PREFIX}"
  "-DCAI_LONEJSON_INCLUDE_DIR=${CAI_LONEJSON_INCLUDE_DIR}"
  "-DCAI_LONEJSON_LIBRARY=${CAI_LONEJSON_LIBRARY}"
  "-DCAI_PSLOG_INCLUDE_DIR=${CAI_PSLOG_INCLUDE_DIR}"
  "-DCAI_PSLOG_LIBRARY=${CAI_PSLOG_LIBRARY}"
  "-DCMAKE_PREFIX_PATH=${escaped_cmake_prefix_path}"
  "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
  "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
if(DEFINED CAI_TOOLCHAIN_FILE AND NOT CAI_TOOLCHAIN_FILE STREQUAL "")
  list(APPEND configure_command "-DCMAKE_TOOLCHAIN_FILE=${CAI_TOOLCHAIN_FILE}")
endif()
if(DEFINED CPKT_TARGET_ID AND NOT CPKT_TARGET_ID STREQUAL "")
  list(APPEND configure_command "-DCPKT_TARGET_ID=${CPKT_TARGET_ID}")
endif()
if(DEFINED CAI_TARGET_ID AND NOT CAI_TARGET_ID STREQUAL "")
  list(APPEND configure_command "-DCAI_TARGET_ID=${CAI_TARGET_ID}")
endif()
execute_process(
  COMMAND ${configure_command}
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR
    "API-key example smoke configuration without the Lua binding failed:\n"
    "${configure_output}\n${configure_error}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${CAI_BINARY_DIR}"
    --target cai_lua_runner
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR
    "API-key example smoke runner did not build:\n${build_output}\n${build_error}")
endif()

execute_process(
  COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${CAI_BINARY_DIR}" -N
    -R "^cai_examples_live_smoke$"
  RESULT_VARIABLE ctest_result
  OUTPUT_VARIABLE ctest_output
  ERROR_VARIABLE ctest_error)
if(NOT ctest_result EQUAL 0 OR
   NOT ctest_output MATCHES "cai_examples_live_smoke")
  message(FATAL_ERROR
    "API-key example smoke was not registered without the Lua binding:\n"
    "${ctest_output}\n${ctest_error}")
endif()
