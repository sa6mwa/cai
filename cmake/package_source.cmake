if(NOT DEFINED CAI_ROOT OR CAI_ROOT STREQUAL "")
  message(FATAL_ERROR "CAI_ROOT is required")
endif()
if(NOT DEFINED CAI_DIST_DIR OR CAI_DIST_DIR STREQUAL "")
  set(CAI_DIST_DIR "${CAI_ROOT}/dist")
endif()
if(NOT DEFINED CAI_VERSION OR CAI_VERSION STREQUAL "")
  set(CAI_VERSION "0.0.0")
endif()

find_program(CAI_TAR_BIN NAMES tar REQUIRED)
find_program(CAI_GZIP_BIN NAMES gzip REQUIRED)

set(source_root_name "cai-${CAI_VERSION}")
set(archive_base "${CAI_DIST_DIR}/${source_root_name}.tar")
set(archive_path "${archive_base}.gz")
set(stage_root "${CAI_DIST_DIR}/.source-pack")
set(stage_dir "${stage_root}/${source_root_name}")

file(MAKE_DIRECTORY "${CAI_DIST_DIR}")
file(REMOVE_RECURSE "${stage_root}")
file(REMOVE "${archive_base}" "${archive_path}")
file(MAKE_DIRECTORY "${stage_root}")

execute_process(
  COMMAND "${CAI_ROOT}/scripts/stage_release_sources.sh"
          "${CAI_ROOT}" "${stage_dir}" "${CAI_VERSION}"
  RESULT_VARIABLE stage_result)
if(NOT stage_result EQUAL 0)
  file(REMOVE_RECURSE "${stage_root}")
  message(FATAL_ERROR "failed to stage source archive tree")
endif()

execute_process(
  COMMAND "${CAI_TAR_BIN}" -cf "${archive_base}" --format=gnu --owner=0 --group=0
          "${source_root_name}"
  WORKING_DIRECTORY "${stage_root}"
  RESULT_VARIABLE archive_result)
if(NOT archive_result EQUAL 0)
  file(REMOVE_RECURSE "${stage_root}")
  message(FATAL_ERROR "failed to create source archive")
endif()

execute_process(
  COMMAND "${CAI_GZIP_BIN}" -9 -f -n "${archive_base}"
  RESULT_VARIABLE gzip_result)
if(NOT gzip_result EQUAL 0)
  file(REMOVE_RECURSE "${stage_root}")
  message(FATAL_ERROR "failed to gzip source archive")
endif()

file(REMOVE_RECURSE "${stage_root}")
message(STATUS "Wrote ${archive_path}")
