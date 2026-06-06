if(NOT DEFINED CAI_DIST_DIR OR CAI_DIST_DIR STREQUAL "")
  set(CAI_DIST_DIR "${CMAKE_CURRENT_LIST_DIR}/../dist")
endif()
if(NOT DEFINED CAI_VERSION OR CAI_VERSION STREQUAL "")
  set(CAI_VERSION "0.0.0")
endif()

find_program(CAI_SHA256SUM_BIN NAMES sha256sum shasum REQUIRED)
file(GLOB artifacts LIST_DIRECTORIES false
  "${CAI_DIST_DIR}/cai-${CAI_VERSION}.tar.gz"
  "${CAI_DIST_DIR}/cai-${CAI_VERSION}-*.tar.gz"
  "${CAI_DIST_DIR}/cai-lua-${CAI_VERSION}.tar.gz"
  "${CAI_DIST_DIR}/cai-${CAI_VERSION}-*.rockspec"
  "${CAI_DIST_DIR}/cai-${CAI_VERSION}-*.src.rock")
if(NOT artifacts)
  message(FATAL_ERROR "no cai release archives found in ${CAI_DIST_DIR}")
endif()
list(SORT artifacts)
set(checksums_path "${CAI_DIST_DIR}/cai-${CAI_VERSION}-CHECKSUMS")
file(WRITE "${checksums_path}" "")
foreach(artifact IN LISTS artifacts)
  get_filename_component(artifact_name "${artifact}" NAME)
  if(CAI_SHA256SUM_BIN MATCHES "shasum$")
    execute_process(
      COMMAND "${CAI_SHA256SUM_BIN}" -a 256 "${artifact_name}"
      WORKING_DIRECTORY "${CAI_DIST_DIR}"
      OUTPUT_VARIABLE checksum_line
      RESULT_VARIABLE checksum_result)
  else()
    execute_process(
      COMMAND "${CAI_SHA256SUM_BIN}" "${artifact_name}"
      WORKING_DIRECTORY "${CAI_DIST_DIR}"
      OUTPUT_VARIABLE checksum_line
      RESULT_VARIABLE checksum_result)
  endif()
  if(NOT checksum_result EQUAL 0)
    message(FATAL_ERROR "failed to checksum ${artifact}")
  endif()
  file(APPEND "${checksums_path}" "${checksum_line}")
endforeach()
message(STATUS "Wrote ${checksums_path}")
