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

set(auto_dir "${CAI_BINARY_DIR}/auto-dependency-mode-partial-pslog")
set(fake_prefix "${auto_dir}/fake-host")
file(REMOVE_RECURSE "${auto_dir}")
file(MAKE_DIRECTORY "${fake_prefix}/include" "${fake_prefix}/lib")
file(WRITE "${fake_prefix}/include/pslog.h" "/* fake pslog header */\n")
file(WRITE "${fake_prefix}/include/lonejson.h" "/* fake lonejson header */\n")
file(WRITE "${fake_prefix}/lib/liblonejson.a" "")

set(auto_configure_command
  "${CMAKE_COMMAND}"
  -S "${CAI_SOURCE_DIR}"
  -B "${auto_dir}"
  -DCAI_BUILD_STATIC=OFF
  -DCAI_BUILD_SHARED=ON
  -DCAI_BUILD_TESTS=OFF
  -DCAI_BUILD_INTEGRATION_TESTS=OFF
  -DCAI_BUILD_EXAMPLES=ON
  -DCAI_BUILD_LUA=OFF
  -DCAI_BUILD_FUZZERS=OFF
  -DCAI_INSTALL=ON
  -DCAI_DEPENDENCY_MODE=auto
  -DCAI_TARGET_ID=x86_64-linux-gnu
  "-DCAI_DEPS_DIR=${CAI_DEPS_DIR}"
  "-DCMAKE_INCLUDE_PATH=${fake_prefix}/include"
  "-DCMAKE_LIBRARY_PATH=${fake_prefix}/lib")
if(DEFINED CAI_GENERATOR AND NOT CAI_GENERATOR STREQUAL "")
  list(APPEND auto_configure_command -G "${CAI_GENERATOR}")
endif()

execute_process(
  COMMAND ${auto_configure_command}
  RESULT_VARIABLE auto_configure_result
  OUTPUT_VARIABLE auto_configure_output
  ERROR_VARIABLE auto_configure_error)
if(NOT auto_configure_result EQUAL 0)
  message(FATAL_ERROR
    "auto dependency mode should fall back to cpkt when pslog.h exists "
    "without a linkable libpslog:\n${auto_configure_error}\n"
    "${auto_configure_output}")
endif()
