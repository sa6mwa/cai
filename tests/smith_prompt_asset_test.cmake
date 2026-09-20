if(NOT DEFINED CAI_SOURCE_DIR OR CAI_SOURCE_DIR STREQUAL "")
  message(FATAL_ERROR "CAI_SOURCE_DIR is required")
endif()
if(NOT DEFINED CAI_BINARY_DIR OR CAI_BINARY_DIR STREQUAL "")
  message(FATAL_ERROR "CAI_BINARY_DIR is required")
endif()

set(_cai_prompt "${CAI_SOURCE_DIR}/prompts/smith/gpt-5.6-codex.md")
set(_cai_template
    "${CAI_SOURCE_DIR}/prompts/smith/gpt-5.6-codex-smith-template.md")
set(_cai_manifest "${CAI_SOURCE_DIR}/prompts/smith/manifest.json")
if(NOT EXISTS "${_cai_prompt}" OR NOT EXISTS "${_cai_template}" OR
   NOT EXISTS "${_cai_manifest}")
  message(FATAL_ERROR "Smith prompt assets and manifest are required")
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

set(_cai_expected_template "${_cai_prompt_contents}")
string(REGEX REPLACE
  "^You are Codex, an agent based on GPT-5\\."
  "You are {{agent_identity}}, an agent based on GPT-5."
  _cai_expected_template "${_cai_expected_template}")
string(REPLACE "As Codex," "As {{agent_identity}},"
               _cai_expected_template "${_cai_expected_template}")
string(APPEND _cai_expected_template "\n{{agent_tools}}\n")
string(HEX "${_cai_expected_template}" _cai_expected_template_hex)
string(TOLOWER "${_cai_expected_template_hex}" _cai_expected_template_hex)
file(READ "${_cai_template}" _cai_template_hex HEX)
string(TOLOWER "${_cai_template_hex}" _cai_template_hex)
if(NOT _cai_template_hex STREQUAL _cai_expected_template_hex)
  message(FATAL_ERROR
    "Smith template must be the byte-exact upstream rendering template")
endif()
if(_cai_template_hex MATCHES "00")
  message(FATAL_ERROR "Smith C-string template must not contain NUL bytes")
endif()

file(READ "${_cai_manifest}" _cai_manifest_contents)
string(JSON _cai_manifest_hash GET "${_cai_manifest_contents}" upstream base sha256)
string(JSON _cai_manifest_template_hash GET "${_cai_manifest_contents}"
       template sha256)
string(JSON _cai_manifest_template_asset GET "${_cai_manifest_contents}"
       template asset)
if(NOT _cai_manifest_hash STREQUAL _cai_prompt_sha256 OR
   NOT _cai_manifest_template_hash STREQUAL
       "a188f43d51e0a8b5a515f12d3e4e478bb88fa3ccd97ac1213fca6bf269841da9" OR
   NOT _cai_manifest_template_asset STREQUAL
       "prompts/smith/gpt-5.6-codex-smith-template.md")
  message(FATAL_ERROR "Smith prompt manifest does not describe its assets")
endif()

set(_cai_generated_inc
    "${CAI_BINARY_DIR}/generated/private/cai_smith_gpt_5_6_prompt.inc")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${CAI_BINARY_DIR}"
          --target cai_smith_prompt_asset --parallel 2
  RESULT_VARIABLE _cai_build_result)
if(NOT _cai_build_result EQUAL 0)
  message(FATAL_ERROR "Smith prompt asset target build failed")
endif()
if(NOT EXISTS "${_cai_generated_inc}")
  message(FATAL_ERROR "Smith generated prompt byte include is required")
endif()

file(READ "${_cai_generated_inc}" _cai_generated_inc_contents)
if(_cai_generated_inc_contents MATCHES "[,][ \t\r\n]*$")
  message(FATAL_ERROR "Smith generated prompt include must not end with a comma")
endif()
if(_cai_generated_inc_contents MATCHES "[^0-9a-fA-Fx, \t\r\n]")
  message(FATAL_ERROR "Smith generated prompt include contains non-byte syntax")
endif()
file(STRINGS "${_cai_generated_inc}" _cai_generated_inc_lines)
foreach(_cai_generated_inc_line IN LISTS _cai_generated_inc_lines)
  string(LENGTH "${_cai_generated_inc_line}" _cai_generated_inc_line_length)
  if(_cai_generated_inc_line_length GREATER 72)
    message(FATAL_ERROR "Smith generated prompt include exceeds 72 columns")
  endif()
  if(NOT _cai_generated_inc_line MATCHES
         "^  0x[0-9a-fA-F][0-9a-fA-F](, 0x[0-9a-fA-F][0-9a-fA-F])*,?$")
    message(FATAL_ERROR "Smith generated prompt include must contain byte literals")
  endif()
endforeach()
string(REGEX MATCHALL "0x[0-9a-fA-F][0-9a-fA-F]"
       _cai_generated_prompt_bytes "${_cai_generated_inc_contents}")
list(LENGTH _cai_generated_prompt_bytes _cai_generated_prompt_byte_count)
string(LENGTH "${_cai_template_hex}" _cai_template_hex_length)
math(EXPR _cai_expected_prompt_byte_count "${_cai_template_hex_length} / 2 + 1")
if(NOT _cai_generated_prompt_byte_count EQUAL _cai_expected_prompt_byte_count)
  message(FATAL_ERROR "Smith generated prompt byte count does not match template")
endif()
set(_cai_generated_prompt_hex "${_cai_generated_inc_contents}")
string(REPLACE "0x" "" _cai_generated_prompt_hex
               "${_cai_generated_prompt_hex}")
string(REGEX REPLACE "[, \t\r\n]" "" _cai_generated_prompt_hex
                     "${_cai_generated_prompt_hex}")
string(TOLOWER "${_cai_generated_prompt_hex}" _cai_generated_prompt_hex)
if(NOT _cai_generated_prompt_hex STREQUAL "${_cai_template_hex}00")
  message(FATAL_ERROR
    "Smith generated prompt bytes must match the template plus one terminator")
endif()
