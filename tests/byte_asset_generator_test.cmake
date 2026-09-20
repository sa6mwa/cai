if(NOT DEFINED CAI_SOURCE_DIR OR CAI_SOURCE_DIR STREQUAL "")
  message(FATAL_ERROR "CAI_SOURCE_DIR is required")
endif()
if(NOT DEFINED CAI_BINARY_DIR OR CAI_BINARY_DIR STREQUAL "")
  message(FATAL_ERROR "CAI_BINARY_DIR is required")
endif()

set(_cai_generator "${CAI_SOURCE_DIR}/cmake/generate_byte_asset.cmake")
set(_cai_nul_fixture "${CAI_SOURCE_DIR}/tests/fixtures/byte_asset_nul.bin")
if(NOT EXISTS "${_cai_generator}" OR NOT EXISTS "${_cai_nul_fixture}")
  message(FATAL_ERROR "byte asset generator and NUL fixture are required")
endif()

set(_cai_work "${CAI_BINARY_DIR}/byte-asset-generator-test")
file(REMOVE_RECURSE "${_cai_work}")
file(MAKE_DIRECTORY "${_cai_work}")
file(WRITE "${_cai_work}/empty.bin" "")
file(WRITE "${_cai_work}/raw.bin" "AB")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -DCAI_ASSET_MODE=raw
          "-DCAI_ASSET_INPUT=${_cai_work}/empty.bin"
          "-DCAI_ASSET_OUTPUT=${_cai_work}/raw-empty.inc"
          -P "${_cai_generator}"
  RESULT_VARIABLE _cai_raw_empty_result)
if(_cai_raw_empty_result EQUAL 0)
  message(FATAL_ERROR "raw-byte generator must reject empty input")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -DCAI_ASSET_MODE=c-string
          "-DCAI_ASSET_INPUT=${_cai_work}/empty.bin"
          "-DCAI_ASSET_OUTPUT=${_cai_work}/string-empty.inc"
          -P "${_cai_generator}"
  RESULT_VARIABLE _cai_empty_string_result)
if(NOT _cai_empty_string_result EQUAL 0)
  message(FATAL_ERROR "C-string generator must represent empty input")
endif()
file(READ "${_cai_work}/string-empty.inc" _cai_empty_string_contents)
if(NOT _cai_empty_string_contents STREQUAL "  0x00\n")
  message(FATAL_ERROR "empty C-string asset must contain exactly one terminator")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -DCAI_ASSET_MODE=c-string
          "-DCAI_ASSET_INPUT=${_cai_nul_fixture}"
          "-DCAI_ASSET_OUTPUT=${_cai_work}/nul-string.inc"
          -P "${_cai_generator}"
  RESULT_VARIABLE _cai_nul_string_result)
if(_cai_nul_string_result EQUAL 0)
  message(FATAL_ERROR "C-string generator must reject NUL input")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -DCAI_ASSET_MODE=raw
          "-DCAI_ASSET_INPUT=${_cai_work}/raw.bin"
          "-DCAI_ASSET_OUTPUT=${_cai_work}/raw.inc"
          -P "${_cai_generator}"
  RESULT_VARIABLE _cai_raw_result)
if(NOT _cai_raw_result EQUAL 0)
  message(FATAL_ERROR "raw-byte generator failed")
endif()
file(READ "${_cai_work}/raw.inc" _cai_raw_contents)
if(NOT _cai_raw_contents STREQUAL "  0x41, 0x42\n")
  message(FATAL_ERROR "raw-byte generator altered payload or syntax")
endif()

set(_cai_fixture_source "${_cai_work}/source")
file(MAKE_DIRECTORY "${_cai_fixture_source}")
file(COPY "${_cai_work}/raw.bin" DESTINATION "${_cai_fixture_source}")
file(WRITE "${_cai_fixture_source}/CMakeLists.txt"
"cmake_minimum_required(VERSION 3.21)\nproject(cai_byte_asset_fixture NONE)\n\nadd_custom_command(\n  OUTPUT \"\${CMAKE_CURRENT_BINARY_DIR}/generated/payload.inc\"\n  COMMAND \"${CMAKE_COMMAND}\"\n    -DCAI_ASSET_MODE=raw\n    \"-DCAI_ASSET_INPUT=${_cai_fixture_source}/raw.bin\"\n    \"-DCAI_ASSET_OUTPUT=\${CMAKE_CURRENT_BINARY_DIR}/generated/payload.inc\"\n    -P \"${_cai_generator}\"\n  DEPENDS \"${_cai_fixture_source}/raw.bin\" \"${_cai_generator}\"\n  VERBATIM)\nadd_custom_target(asset ALL DEPENDS \"\${CMAKE_CURRENT_BINARY_DIR}/generated/payload.inc\")\n")

foreach(_cai_build_name IN ITEMS first second)
  set(_cai_build "${_cai_work}/${_cai_build_name}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${_cai_fixture_source}" -B "${_cai_build}"
    RESULT_VARIABLE _cai_configure_result)
  if(NOT _cai_configure_result EQUAL 0)
    message(FATAL_ERROR "independent byte asset fixture configuration failed")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_cai_build}" --parallel 2
    RESULT_VARIABLE _cai_fixture_build_result)
  if(NOT _cai_fixture_build_result EQUAL 0)
    message(FATAL_ERROR "parallel byte asset fixture build failed")
  endif()
endforeach()
file(SHA256 "${_cai_work}/first/generated/payload.inc" _cai_first_hash)
file(SHA256 "${_cai_work}/second/generated/payload.inc" _cai_second_hash)
if(NOT _cai_first_hash STREQUAL _cai_second_hash)
  message(FATAL_ERROR "independent byte asset builds must be reproducible")
endif()
