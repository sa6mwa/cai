if(NOT DEFINED CAI_SOURCE_DIR OR CAI_SOURCE_DIR STREQUAL "")
  message(FATAL_ERROR "CAI_SOURCE_DIR is required")
endif()
if(NOT DEFINED CAI_BINARY_DIR OR CAI_BINARY_DIR STREQUAL "")
  message(FATAL_ERROR "CAI_BINARY_DIR is required")
endif()

set(host_dir "${CAI_BINARY_DIR}/host-dependency-mode-unsupported-target")
file(REMOVE_RECURSE "${host_dir}")

set(prefix_paths "")
foreach(path IN ITEMS
    "${CAI_C_PKT_SYSTEMS_PREFIX}"
    "${CAI_LONEJSON_PREFIX}"
    "${CAI_PSLOG_PREFIX}")
  if(NOT path STREQUAL "" AND EXISTS "${path}")
    list(APPEND prefix_paths "${path}")
  endif()
endforeach()
list(JOIN prefix_paths ";" prefix_path)
string(REPLACE ";" "\\;" prefix_path_arg "${prefix_path}")

set(configure_command
  "${CMAKE_COMMAND}"
  -S "${CAI_SOURCE_DIR}"
  -B "${host_dir}"
  -DCAI_BUILD_STATIC=OFF
  -DCAI_BUILD_SHARED=ON
  -DCAI_BUILD_TESTS=OFF
  -DCAI_BUILD_INTEGRATION_TESTS=OFF
  -DCAI_BUILD_EXAMPLES=OFF
  -DCAI_BUILD_LUA=OFF
  -DCAI_BUILD_FUZZERS=OFF
  -DCAI_INSTALL=ON
  -DCAI_DEPENDENCY_MODE=host
  -DCAI_TARGET_ID=unsupported-host-target)
if(NOT prefix_path_arg STREQUAL "")
  list(APPEND configure_command "-DCMAKE_PREFIX_PATH=${prefix_path_arg}")
endif()
if(DEFINED CAI_GENERATOR AND NOT CAI_GENERATOR STREQUAL "")
  list(APPEND configure_command -G "${CAI_GENERATOR}")
endif()

execute_process(
  COMMAND ${configure_command}
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR
    "host dependency mode should not require bundled checksums for unsupported "
    "targets:\n${configure_error}\n${configure_output}")
endif()

file(READ "${host_dir}/cai.pc" pc_text)
if(pc_text MATCHES "c_pkt_systems_sha256=[^\n]+")
  message(FATAL_ERROR
    "host dependency mode should not emit a bundled c.pkt.systems checksum")
endif()
