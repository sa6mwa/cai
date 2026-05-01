if(NOT DEFINED CAI_ROOT OR CAI_ROOT STREQUAL "")
  message(FATAL_ERROR "CAI_ROOT is required")
endif()
if(NOT DEFINED CAI_BINARY_DIR OR CAI_BINARY_DIR STREQUAL "")
  message(FATAL_ERROR "CAI_BINARY_DIR is required")
endif()
if(NOT DEFINED CAI_DIST_DIR OR CAI_DIST_DIR STREQUAL "")
  set(CAI_DIST_DIR "${CAI_ROOT}/dist")
endif()
if(NOT DEFINED CAI_VERSION OR CAI_VERSION STREQUAL "")
  set(CAI_VERSION "0.0.0")
endif()
if(NOT DEFINED CAI_TARGET_ID OR CAI_TARGET_ID STREQUAL "")
  set(CAI_TARGET_ID "host")
endif()

find_program(CAI_TAR_BIN NAMES tar REQUIRED)
find_program(CAI_GZIP_BIN NAMES gzip REQUIRED)

set(package_name "cai-${CAI_VERSION}-${CAI_TARGET_ID}")
set(stage_root "${CAI_BINARY_DIR}/package-stage")
set(package_root "${stage_root}/${package_name}")
set(archive_base "${CAI_DIST_DIR}/${package_name}.tar")
set(archive_path "${archive_base}.gz")

file(REMOVE_RECURSE "${stage_root}")
file(MAKE_DIRECTORY "${package_root}" "${CAI_DIST_DIR}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${CAI_BINARY_DIR}" --prefix "${package_root}"
  RESULT_VARIABLE install_result)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "failed to install cai package staging tree")
endif()

file(REMOVE "${archive_base}" "${archive_path}")
execute_process(
  COMMAND "${CAI_TAR_BIN}" -cf "${archive_base}" --format=gnu --owner=0 --group=0 "${package_name}"
  WORKING_DIRECTORY "${stage_root}"
  RESULT_VARIABLE tar_result)
if(NOT tar_result EQUAL 0)
  message(FATAL_ERROR "failed to create ${archive_base}")
endif()

execute_process(
  COMMAND "${CAI_GZIP_BIN}" -f -n "${archive_base}"
  RESULT_VARIABLE gzip_result)
if(NOT gzip_result EQUAL 0)
  message(FATAL_ERROR "failed to gzip ${archive_base}")
endif()

message(STATUS "Wrote ${archive_path}")
