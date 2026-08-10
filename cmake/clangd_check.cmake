if(NOT DEFINED CAI_SOURCE_DIR OR CAI_SOURCE_DIR STREQUAL "")
  message(FATAL_ERROR "CAI_SOURCE_DIR is required")
endif()
if(NOT DEFINED CAI_BUILD_DIR OR CAI_BUILD_DIR STREQUAL "")
  message(FATAL_ERROR "CAI_BUILD_DIR is required")
endif()

set(_cai_compile_commands "${CAI_BUILD_DIR}/compile_commands.json")
if(NOT EXISTS "${_cai_compile_commands}")
  message(FATAL_ERROR
    "Native compile database is missing: ${_cai_compile_commands}. Run make "
    "build-debug first.")
endif()

find_program(CAI_CLANGD_BIN NAMES clangd)
if(NOT CAI_CLANGD_BIN)
  message(FATAL_ERROR
    "clangd is required for the native editor gate. Install clangd and retry.")
endif()

file(READ "${_cai_compile_commands}" _cai_compile_commands_json)
string(JSON _cai_entry_count LENGTH "${_cai_compile_commands_json}")
if(_cai_entry_count EQUAL 0)
  message(FATAL_ERROR
    "Native compile database has no translation units: ${_cai_compile_commands}")
endif()

math(EXPR _cai_last_entry "${_cai_entry_count} - 1")
set(_cai_checked_sources)
foreach(_cai_entry RANGE 0 ${_cai_last_entry})
  string(JSON _cai_source_file GET "${_cai_compile_commands_json}" ${_cai_entry} file)
  list(FIND _cai_checked_sources "${_cai_source_file}" _cai_seen_index)
  if(NOT _cai_seen_index EQUAL -1)
    continue()
  endif()
  list(APPEND _cai_checked_sources "${_cai_source_file}")

  execute_process(
    COMMAND "${CAI_CLANGD_BIN}" "--compile-commands-dir=${CAI_BUILD_DIR}"
            "--check=${_cai_source_file}" --log=error
    WORKING_DIRECTORY "${CAI_SOURCE_DIR}"
    RESULT_VARIABLE _cai_clangd_result
    OUTPUT_VARIABLE _cai_clangd_stdout
    ERROR_VARIABLE _cai_clangd_stderr)
  if(NOT _cai_clangd_result EQUAL 0)
    message(FATAL_ERROR
      "clangd native editor check failed for ${_cai_source_file}\n"
      "stdout:\n${_cai_clangd_stdout}\n"
      "stderr:\n${_cai_clangd_stderr}")
  endif()
endforeach()

list(LENGTH _cai_checked_sources _cai_checked_count)
message(STATUS
  "clangd native editor check passed for ${_cai_checked_count} translation units")
