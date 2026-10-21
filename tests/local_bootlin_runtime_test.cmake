if(NOT DEFINED CAI_RUNTIME_TARGETS OR CAI_RUNTIME_TARGETS STREQUAL "")
  message(FATAL_ERROR "CAI_RUNTIME_TARGETS must contain configured executables")
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

file(GLOB _loaders "${CAI_SYSROOT}/lib/ld-linux*.so*")
list(LENGTH _loaders _loader_count)
if(NOT _loader_count EQUAL 1)
  message(FATAL_ERROR "expected one Bootlin loader under ${CAI_SYSROOT}/lib")
endif()
list(GET _loaders 0 _loader)

foreach(_cai_runtime_target IN LISTS CAI_RUNTIME_TARGETS)
  if(NOT EXISTS "${_cai_runtime_target}" OR IS_DIRECTORY "${_cai_runtime_target}")
    message(FATAL_ERROR "configured executable is missing: ${_cai_runtime_target}")
  endif()
  execute_process(
    COMMAND "${CAI_READELF}" -l "${_cai_runtime_target}"
    RESULT_VARIABLE _cai_program_result
    OUTPUT_VARIABLE _cai_program_headers
    ERROR_VARIABLE _cai_program_error)
  string(FIND "${_cai_program_headers}" "Requesting program interpreter: ${_loader}]" _interpreter)
  if(NOT _cai_program_result EQUAL 0 OR _interpreter EQUAL -1)
    message(FATAL_ERROR
      "${_cai_runtime_target} does not select the Bootlin loader:\n"
      "${_cai_program_error}\n${_cai_program_headers}")
  endif()
  execute_process(
    COMMAND "${CAI_READELF}" -d "${_cai_runtime_target}"
    RESULT_VARIABLE _cai_dynamic_result
    OUTPUT_VARIABLE _cai_dynamic_headers
    ERROR_VARIABLE _cai_dynamic_error)
  string(FIND "${_cai_dynamic_headers}" "${CAI_SYSROOT}/lib" _sysroot_path)
  if(NOT _cai_dynamic_result EQUAL 0 OR
     NOT _cai_dynamic_headers MATCHES "RPATH" OR
     _cai_dynamic_headers MATCHES "RUNPATH" OR _sysroot_path EQUAL -1)
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
  execute_process(COMMAND "${_loader}" --list "${_cai_runtime_target}"
    RESULT_VARIABLE _result OUTPUT_VARIABLE _loaded ERROR_VARIABLE _error)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "Runtime dependencies do not resolve: ${_error}\n${_loaded}")
  endif()
  # Paths can contain spaces. Strip the final address, not the first whitespace.
  string(REPLACE "\n" ";" _loaded_lines "${_loaded}")
  set(_found_libc FALSE)
  foreach(_entry IN LISTS _loaded_lines)
    if(_entry MATCHES "^[ \t]*(lib(c|m|pthread|dl|rt)\\.so[^ \t]*)[ \t]+=>[ \t]+(.*)[ \t]+\\(0x[0-9a-fA-F]+\\)[ \t]*$")
      set(_name "${CMAKE_MATCH_1}")
      set(_path "${CMAKE_MATCH_3}")
      if(_name MATCHES "^libc\\.so")
        set(_found_libc TRUE)
      endif()
      string(FIND "${_path}" "${CAI_SYSROOT}/" _found)
      if(NOT _found EQUAL 0)
        message(FATAL_ERROR "${_cai_runtime_target} loads a foreign libc component: ${_entry}")
      endif()
    endif()
  endforeach()
  if(NOT _found_libc)
    message(FATAL_ERROR "${_cai_runtime_target}: loader did not identify the loaded libc:\n${_loaded}")
  endif()
endforeach()
