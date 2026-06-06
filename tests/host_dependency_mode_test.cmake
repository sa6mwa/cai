if(NOT DEFINED CAI_SOURCE_DIR OR CAI_SOURCE_DIR STREQUAL "")
  message(FATAL_ERROR "CAI_SOURCE_DIR is required")
endif()
if(NOT DEFINED CAI_BINARY_DIR OR CAI_BINARY_DIR STREQUAL "")
  message(FATAL_ERROR "CAI_BINARY_DIR is required")
endif()
if(NOT DEFINED CAI_C_COMPILER OR CAI_C_COMPILER STREQUAL "")
  message(FATAL_ERROR "CAI_C_COMPILER is required")
endif()
if(NOT DEFINED CAI_LONEJSON_ABI_VERSION OR CAI_LONEJSON_ABI_VERSION STREQUAL "")
  message(FATAL_ERROR "CAI_LONEJSON_ABI_VERSION is required")
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
if(DEFINED CAI_PARENT_CMAKE_PREFIX_PATH AND NOT CAI_PARENT_CMAKE_PREFIX_PATH STREQUAL "")
  list(APPEND prefix_paths ${CAI_PARENT_CMAKE_PREFIX_PATH})
endif()
if(prefix_paths)
  list(REMOVE_DUPLICATES prefix_paths)
endif()
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
if(DEFINED CAI_LONEJSON_INCLUDE_DIR AND NOT CAI_LONEJSON_INCLUDE_DIR STREQUAL "")
  list(APPEND configure_command "-DCAI_LONEJSON_INCLUDE_DIR=${CAI_LONEJSON_INCLUDE_DIR}")
endif()
if(DEFINED CAI_LONEJSON_LIBRARY AND NOT CAI_LONEJSON_LIBRARY STREQUAL "")
  list(APPEND configure_command "-DCAI_LONEJSON_LIBRARY=${CAI_LONEJSON_LIBRARY}")
endif()
if(DEFINED CAI_PSLOG_INCLUDE_DIR AND NOT CAI_PSLOG_INCLUDE_DIR STREQUAL "")
  list(APPEND configure_command "-DCAI_PSLOG_INCLUDE_DIR=${CAI_PSLOG_INCLUDE_DIR}")
endif()
if(DEFINED CAI_PSLOG_LIBRARY AND NOT CAI_PSLOG_LIBRARY STREQUAL "")
  list(APPEND configure_command "-DCAI_PSLOG_LIBRARY=${CAI_PSLOG_LIBRARY}")
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

set(abi_dir "${CAI_BINARY_DIR}/host-dependency-mode-wrong-lonejson-abi")
set(fake_abi_prefix "${abi_dir}/fake-host")
file(REMOVE_RECURSE "${abi_dir}")
file(MAKE_DIRECTORY "${fake_abi_prefix}/include" "${fake_abi_prefix}/lib")
file(WRITE "${fake_abi_prefix}/include/lonejson.h" "/* fake lonejson header */\n")
file(WRITE "${fake_abi_prefix}/wrong_abi.c" "void cai_fake_lonejson(void) {}\n")
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  set(fake_abi_library "${fake_abi_prefix}/lib/liblonejson.dylib")
  set(fake_abi_link_flag
      "-Wl,-install_name,@rpath/liblonejson.15.dylib")
else()
  set(fake_abi_library "${fake_abi_prefix}/lib/liblonejson.so")
  set(fake_abi_link_flag "-Wl,-soname,liblonejson.so.15")
endif()
execute_process(
  COMMAND "${CAI_C_COMPILER}" -shared
          "${fake_abi_link_flag}"
          -o "${fake_abi_library}"
          "${fake_abi_prefix}/wrong_abi.c"
  RESULT_VARIABLE abi_build_result
  OUTPUT_VARIABLE abi_build_output
  ERROR_VARIABLE abi_build_error)
if(NOT abi_build_result EQUAL 0)
  message(FATAL_ERROR
    "failed to build fake wrong-ABI lonejson library:\n${abi_build_error}\n"
    "${abi_build_output}")
endif()

set(abi_configure_command
  "${CMAKE_COMMAND}"
  -S "${CAI_SOURCE_DIR}"
  -B "${abi_dir}"
  -DCAI_BUILD_STATIC=OFF
  -DCAI_BUILD_SHARED=ON
  -DCAI_BUILD_TESTS=OFF
  -DCAI_BUILD_INTEGRATION_TESTS=OFF
  -DCAI_BUILD_EXAMPLES=OFF
  -DCAI_BUILD_LUA=OFF
  -DCAI_BUILD_FUZZERS=OFF
  -DCAI_INSTALL=ON
  -DCAI_DEPENDENCY_MODE=host
  "-DCAI_LONEJSON_INCLUDE_DIR=${fake_abi_prefix}/include"
  "-DCAI_LONEJSON_LIBRARY=${fake_abi_library}")
set(abi_prefix_paths "")
foreach(path IN ITEMS "${CAI_C_PKT_SYSTEMS_PREFIX}" "${CAI_PSLOG_PREFIX}")
  if(NOT path STREQUAL "" AND EXISTS "${path}")
    list(APPEND abi_prefix_paths "${path}")
  endif()
endforeach()
if(DEFINED CAI_PARENT_CMAKE_PREFIX_PATH AND NOT CAI_PARENT_CMAKE_PREFIX_PATH STREQUAL "")
  list(APPEND abi_prefix_paths ${CAI_PARENT_CMAKE_PREFIX_PATH})
endif()
if(abi_prefix_paths)
  list(REMOVE_DUPLICATES abi_prefix_paths)
endif()
list(JOIN abi_prefix_paths ";" abi_prefix_path)
string(REPLACE ";" "\\;" abi_prefix_path_arg "${abi_prefix_path}")
if(NOT abi_prefix_path_arg STREQUAL "")
  list(APPEND abi_configure_command "-DCMAKE_PREFIX_PATH=${abi_prefix_path_arg}")
endif()
if(DEFINED CAI_PSLOG_INCLUDE_DIR AND NOT CAI_PSLOG_INCLUDE_DIR STREQUAL "")
  list(APPEND abi_configure_command "-DCAI_PSLOG_INCLUDE_DIR=${CAI_PSLOG_INCLUDE_DIR}")
endif()
if(DEFINED CAI_PSLOG_LIBRARY AND NOT CAI_PSLOG_LIBRARY STREQUAL "")
  list(APPEND abi_configure_command "-DCAI_PSLOG_LIBRARY=${CAI_PSLOG_LIBRARY}")
endif()
if(DEFINED CAI_GENERATOR AND NOT CAI_GENERATOR STREQUAL "")
  list(APPEND abi_configure_command -G "${CAI_GENERATOR}")
endif()

execute_process(
  COMMAND ${abi_configure_command}
  RESULT_VARIABLE abi_configure_result
  OUTPUT_VARIABLE abi_configure_output
  ERROR_VARIABLE abi_configure_error)
if(abi_configure_result EQUAL 0)
  message(FATAL_ERROR
    "host dependency mode should reject liblonejson with the wrong ABI")
endif()
if(NOT abi_configure_error MATCHES
      "required ABI ${CAI_LONEJSON_ABI_VERSION}")
  message(FATAL_ERROR
    "wrong-ABI liblonejson did not emit actionable error:\n"
    "${abi_configure_error}\n${abi_configure_output}")
endif()

set(auto_abi_dir "${CAI_BINARY_DIR}/auto-dependency-mode-wrong-lonejson-abi")
set(auto_abi_prefix "${auto_abi_dir}/fake-host")
file(REMOVE_RECURSE "${auto_abi_dir}")
file(MAKE_DIRECTORY "${auto_abi_prefix}/include" "${auto_abi_prefix}/lib")
file(WRITE "${auto_abi_prefix}/include/pslog.h" "/* fake pslog header */\n")
file(WRITE "${auto_abi_prefix}/include/lonejson.h" "/* fake lonejson header */\n")
file(WRITE "${auto_abi_prefix}/wrong_abi.c" "void cai_fake_lonejson(void) {}\n")
file(WRITE "${auto_abi_prefix}/fake_pslog.c" "void cai_fake_pslog(void) {}\n")
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  set(auto_abi_lonejson_library "${auto_abi_prefix}/lib/liblonejson.dylib")
  set(auto_abi_lonejson_link_flag
      "-Wl,-install_name,@rpath/liblonejson.15.dylib")
else()
  set(auto_abi_lonejson_library "${auto_abi_prefix}/lib/liblonejson.so")
  set(auto_abi_lonejson_link_flag "-Wl,-soname,liblonejson.so.15")
endif()
execute_process(
  COMMAND "${CAI_C_COMPILER}" -shared
          "${auto_abi_lonejson_link_flag}"
          -o "${auto_abi_lonejson_library}"
          "${auto_abi_prefix}/wrong_abi.c"
  RESULT_VARIABLE auto_abi_lonejson_result
  OUTPUT_VARIABLE auto_abi_lonejson_output
  ERROR_VARIABLE auto_abi_lonejson_error)
if(NOT auto_abi_lonejson_result EQUAL 0)
  message(FATAL_ERROR
    "failed to build fake wrong-ABI lonejson library for auto mode:\n"
    "${auto_abi_lonejson_error}\n${auto_abi_lonejson_output}")
endif()
execute_process(
  COMMAND "${CAI_C_COMPILER}" -shared
          -o "${auto_abi_prefix}/lib/libpslog.so"
          "${auto_abi_prefix}/fake_pslog.c"
  RESULT_VARIABLE auto_abi_pslog_result
  OUTPUT_VARIABLE auto_abi_pslog_output
  ERROR_VARIABLE auto_abi_pslog_error)
if(NOT auto_abi_pslog_result EQUAL 0)
  message(FATAL_ERROR
    "failed to build fake pslog library:\n${auto_abi_pslog_error}\n"
    "${auto_abi_pslog_output}")
endif()

set(auto_abi_configure_command
  "${CMAKE_COMMAND}"
  -S "${CAI_SOURCE_DIR}"
  -B "${auto_abi_dir}"
  -DCAI_BUILD_STATIC=OFF
  -DCAI_BUILD_SHARED=ON
  -DCAI_BUILD_TESTS=OFF
  -DCAI_BUILD_INTEGRATION_TESTS=OFF
  -DCAI_BUILD_EXAMPLES=OFF
  -DCAI_BUILD_LUA=OFF
  -DCAI_BUILD_FUZZERS=OFF
  -DCAI_INSTALL=ON
  -DCAI_DEPENDENCY_MODE=auto
  -DCAI_TARGET_ID=x86_64-linux-gnu
  "-DCAI_DEPS_DIR=${CAI_DEPS_DIR}"
  "-DCMAKE_INCLUDE_PATH=${auto_abi_prefix}/include"
  "-DCMAKE_LIBRARY_PATH=${auto_abi_prefix}/lib")
if(DEFINED CAI_GENERATOR AND NOT CAI_GENERATOR STREQUAL "")
  list(APPEND auto_abi_configure_command -G "${CAI_GENERATOR}")
endif()

execute_process(
  COMMAND ${auto_abi_configure_command}
  RESULT_VARIABLE auto_abi_configure_result
  OUTPUT_VARIABLE auto_abi_configure_output
  ERROR_VARIABLE auto_abi_configure_error)
if(NOT auto_abi_configure_result EQUAL 0)
  message(FATAL_ERROR
    "auto dependency mode should fall back to cpkt when host liblonejson has "
    "the wrong ABI:\n${auto_abi_configure_error}\n${auto_abi_configure_output}")
endif()

file(READ "${auto_abi_dir}/cai.pc" auto_abi_pc_text)
if(NOT auto_abi_pc_text MATCHES "c_pkt_systems_sha256=[0-9a-f]+")
  message(FATAL_ERROR
    "auto dependency mode should fall back to cpkt when host liblonejson has "
    "the wrong ABI")
endif()
