if(NOT DEFINED CAI_SOURCE_DIR OR CAI_SOURCE_DIR STREQUAL "")
  message(FATAL_ERROR "CAI_SOURCE_DIR is required")
endif()
if(NOT DEFINED CAI_BINARY_DIR OR CAI_BINARY_DIR STREQUAL "")
  message(FATAL_ERROR "CAI_BINARY_DIR is required")
endif()

set(_cai_prompt "${CAI_SOURCE_DIR}/prompts/smith/gpt-5.6-codex.md")
set(_cai_manifest "${CAI_SOURCE_DIR}/prompts/smith/manifest.json")
if(NOT EXISTS "${_cai_prompt}" OR NOT EXISTS "${_cai_manifest}")
  message(FATAL_ERROR "Smith GPT-5.6 prompt asset and manifest are required")
endif()

file(SHA256 "${_cai_prompt}" _cai_prompt_sha256)
if(NOT _cai_prompt_sha256 STREQUAL
       "35d8b5d513fff3b55344d5f9f3169305cc276aee053f5be140a77708e0926e7c")
  message(FATAL_ERROR "Smith GPT-5.6 prompt asset does not match Codex source")
endif()
file(READ "${_cai_prompt}" _cai_prompt_contents)
if(NOT _cai_prompt_contents MATCHES
       "^You are Codex, an agent based on GPT-5\\.")
  message(FATAL_ERROR "Smith prompt asset is not the upstream Codex baseline")
endif()
if(_cai_prompt_contents MATCHES "\\{\\{agent_identity\\}\\}")
  message(FATAL_ERROR "Smith prompt asset must retain the upstream identity")
endif()

file(READ "${_cai_manifest}" _cai_manifest_contents)
string(JSON _cai_manifest_hash GET "${_cai_manifest_contents}" upstream base sha256)
string(JSON _cai_manifest_version GET "${_cai_manifest_contents}"
       rendered_asset_version)
if(NOT _cai_manifest_hash STREQUAL _cai_prompt_sha256 OR
   NOT _cai_manifest_version STREQUAL "smith-7")
  message(FATAL_ERROR "Smith prompt manifest does not describe the source asset")
endif()

set(_cai_generated_header
    "${CAI_BINARY_DIR}/generated/private/cai_smith_gpt_5_6_prompt.h")
if(NOT EXISTS "${_cai_generated_header}")
  message(FATAL_ERROR "Smith generated prompt header is required")
endif()
file(TIMESTAMP "${_cai_generated_header}" _cai_header_before "%s.%f" UTC)
execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${CAI_SOURCE_DIR}" -B "${CAI_BINARY_DIR}"
  RESULT_VARIABLE _cai_reconfigure_result)
if(NOT _cai_reconfigure_result EQUAL 0)
  message(FATAL_ERROR "Smith prompt reconfigure failed")
endif()
file(TIMESTAMP "${_cai_generated_header}" _cai_header_after "%s.%f" UTC)
if(NOT _cai_header_before STREQUAL _cai_header_after)
  message(FATAL_ERROR "Smith generated prompt header changed without content changes")
endif()

set(_cai_build_ninja "${CAI_BINARY_DIR}/build.ninja")
if(EXISTS "${_cai_build_ninja}")
  file(READ "${_cai_build_ninja}" _cai_build_graph)
  string(FIND "${_cai_build_graph}" "${_cai_prompt}"
         _cai_prompt_dependency_offset)
  if(_cai_prompt_dependency_offset EQUAL -1)
    message(FATAL_ERROR
      "Smith prompt asset is not tracked as an incremental build dependency")
  endif()
endif()
