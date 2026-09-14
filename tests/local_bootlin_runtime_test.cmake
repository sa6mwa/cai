if(NOT DEFINED CAI_BINARY_DIR OR CAI_BINARY_DIR STREQUAL "")
  message(FATAL_ERROR "CAI_BINARY_DIR is required")
endif()
if(NOT DEFINED CAI_READELF OR NOT EXISTS "${CAI_READELF}")
  message(FATAL_ERROR "CAI_READELF is required")
endif()
if(NOT DEFINED CAI_SYSROOT OR CAI_SYSROOT STREQUAL "")
  message(FATAL_ERROR "CAI_SYSROOT is required")
endif()
if(NOT DEFINED CAI_DEPENDENCY_RUNTIME_DIRS)
  message(FATAL_ERROR "CAI_DEPENDENCY_RUNTIME_DIRS is required")
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
     NOT _cai_dynamic_headers MATCHES "${CAI_SYSROOT}/lib")
    message(FATAL_ERROR
      "${_cai_runtime_target} does not carry the complete Bootlin runtime path:\n"
      "${_cai_dynamic_error}\n${_cai_dynamic_headers}")
  endif()
  foreach(_dir IN LISTS CAI_DEPENDENCY_RUNTIME_DIRS)
    string(FIND "${_cai_dynamic_headers}" "${_dir}" _found)
    if(_found EQUAL -1)
      message(FATAL_ERROR "${_cai_runtime_target} lacks dependency runtime directory ${_dir}")
    endif()
  endforeach()
  file(GLOB _loaders "${CAI_SYSROOT}/lib/ld-linux*.so*")
  list(GET _loaders 0 _loader)
  execute_process(COMMAND "${_loader}" --list "${_cai_runtime_target}"
    RESULT_VARIABLE _result OUTPUT_VARIABLE _loaded ERROR_VARIABLE _error)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "Runtime dependencies do not resolve: ${_error}\n${_loaded}")
  endif()
  string(REGEX MATCHALL "lib(c|m|pthread|dl|rt)\\.so[^\n]* => [^ \n]+" _libc_entries "${_loaded}")
  foreach(_entry IN LISTS _libc_entries)
    string(FIND "${_entry}" "${CAI_SYSROOT}/" _found)
    if(_found EQUAL -1)
      message(FATAL_ERROR "${_cai_runtime_target} loads a foreign libc component: ${_entry}")
    endif()
  endforeach()
endforeach()
