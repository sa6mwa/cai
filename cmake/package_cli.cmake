if(NOT DEFINED CAI_BINARY_DIR OR CAI_BINARY_DIR STREQUAL "")
  message(FATAL_ERROR "CAI_BINARY_DIR is required")
endif()
if(NOT DEFINED CAI_DIST_DIR OR CAI_DIST_DIR STREQUAL "")
  message(FATAL_ERROR "CAI_DIST_DIR is required")
endif()
if(NOT DEFINED CAI_VERSION OR CAI_VERSION STREQUAL "")
  message(FATAL_ERROR "CAI_VERSION is required")
endif()

find_program(CAI_TAR_BIN NAMES tar REQUIRED)
find_program(CAI_GZIP_BIN NAMES gzip REQUIRED)
set(package_name "cai-cli-${CAI_VERSION}-x86_64-linux-musl")
set(stage_root "${CAI_BINARY_DIR}/package-cli-stage")
set(package_root "${stage_root}/${package_name}")
set(archive_base "${CAI_DIST_DIR}/${package_name}.tar")

file(REMOVE_RECURSE "${stage_root}")
file(MAKE_DIRECTORY "${package_root}" "${CAI_DIST_DIR}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${CAI_BINARY_DIR}"
    --component cai-cli --prefix "${package_root}"
  RESULT_VARIABLE install_result)
if(NOT install_result EQUAL 0 OR NOT EXISTS "${package_root}/bin/cai")
  message(FATAL_ERROR "failed to stage cai CLI executable")
endif()
foreach(required_doc
        "share/man/man1/cai.1"
        "share/doc/libcai/README.md"
        "share/doc/libcai/LICENSE"
        "share/doc/libcai/docs/model-metadata.md")
  if(NOT EXISTS "${package_root}/${required_doc}")
    message(FATAL_ERROR "failed to stage cai CLI file: ${required_doc}")
  endif()
endforeach()

if(DEFINED CAI_STRIP_BIN AND NOT CAI_STRIP_BIN STREQUAL "")
  execute_process(
    COMMAND "${CAI_STRIP_BIN}" --strip-debug "${package_root}/bin/cai"
    RESULT_VARIABLE strip_result)
  if(NOT strip_result EQUAL 0)
    message(FATAL_ERROR "failed to strip cai CLI executable")
  endif()
endif()

file(REMOVE "${archive_base}" "${archive_base}.gz")
execute_process(
  COMMAND "${CAI_TAR_BIN}" -cf "${archive_base}" --format=gnu
    --owner=0 --group=0 "${package_name}"
  WORKING_DIRECTORY "${stage_root}"
  RESULT_VARIABLE archive_result)
if(NOT archive_result EQUAL 0)
  message(FATAL_ERROR "failed to archive cai CLI")
endif()
execute_process(
  COMMAND "${CAI_GZIP_BIN}" -9 -f -n "${archive_base}"
  RESULT_VARIABLE gzip_result)
if(NOT gzip_result EQUAL 0)
  message(FATAL_ERROR "failed to compress cai CLI archive")
endif()
file(REMOVE_RECURSE "${stage_root}")
message(STATUS "Wrote ${archive_base}.gz")
