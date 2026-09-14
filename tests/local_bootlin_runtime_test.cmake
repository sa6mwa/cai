if(NOT DEFINED CAI_BINARY_DIR OR CAI_BINARY_DIR STREQUAL "")
  message(FATAL_ERROR "CAI_BINARY_DIR is required")
endif()
if(NOT DEFINED CAI_READELF OR NOT EXISTS "${CAI_READELF}")
  message(FATAL_ERROR "CAI_READELF is required")
endif()
if(NOT DEFINED CAI_SYSROOT OR CAI_SYSROOT STREQUAL "")
  message(FATAL_ERROR "CAI_SYSROOT is required")
endif()
if(NOT DEFINED CAI_C_PKT_SYSTEMS_PREFIX OR
   CAI_C_PKT_SYSTEMS_PREFIX STREQUAL "")
  message(FATAL_ERROR "CAI_C_PKT_SYSTEMS_PREFIX is required")
endif()
if(NOT DEFINED CAI_LONEJSON_PREFIX OR CAI_LONEJSON_PREFIX STREQUAL "")
  message(FATAL_ERROR "CAI_LONEJSON_PREFIX is required")
endif()

file(GLOB _cai_runtime_targets
  "${CAI_BINARY_DIR}/cai_tests"
  "${CAI_BINARY_DIR}/cai_example_*"
  "${CAI_BINARY_DIR}/cai_mcp_*"
  "${CAI_BINARY_DIR}/cai_*_fuzz")
if(NOT _cai_runtime_targets)
  message(FATAL_ERROR "no local executable targets found under ${CAI_BINARY_DIR}")
endif()

foreach(_cai_runtime_target IN LISTS _cai_runtime_targets)
  if(IS_DIRECTORY "${_cai_runtime_target}")
    continue()
  endif()
  execute_process(
    COMMAND "${CAI_READELF}" -l "${_cai_runtime_target}"
    RESULT_VARIABLE _cai_program_result
    OUTPUT_VARIABLE _cai_program_headers
    ERROR_VARIABLE _cai_program_error)
  if(NOT _cai_program_result EQUAL 0 OR
     NOT _cai_program_headers MATCHES
         "Requesting program interpreter: ${CAI_SYSROOT}/lib/ld-linux")
    message(FATAL_ERROR
      "${_cai_runtime_target} does not select the Bootlin loader:\n"
      "${_cai_program_error}\n${_cai_program_headers}")
  endif()
  execute_process(
    COMMAND "${CAI_READELF}" -d "${_cai_runtime_target}"
    RESULT_VARIABLE _cai_dynamic_result
    OUTPUT_VARIABLE _cai_dynamic_headers
    ERROR_VARIABLE _cai_dynamic_error)
  if(NOT _cai_dynamic_result EQUAL 0 OR
     NOT _cai_dynamic_headers MATCHES "RPATH" OR
     _cai_dynamic_headers MATCHES "RUNPATH" OR
     NOT _cai_dynamic_headers MATCHES "${CAI_SYSROOT}/lib" OR
     NOT _cai_dynamic_headers MATCHES "${CAI_C_PKT_SYSTEMS_PREFIX}/lib" OR
     NOT _cai_dynamic_headers MATCHES "${CAI_LONEJSON_PREFIX}/lib")
    message(FATAL_ERROR
      "${_cai_runtime_target} does not carry the complete Bootlin runtime path:\n"
      "${_cai_dynamic_error}\n${_cai_dynamic_headers}")
  endif()
endforeach()
