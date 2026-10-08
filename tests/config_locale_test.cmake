file(MAKE_DIRECTORY "${CAI_TEST_BINARY_DIR}/config-locales")
execute_process(
  COMMAND "${CAI_LOCALEDEF}" --no-archive -i sv_SE -f UTF-8
    "${CAI_TEST_BINARY_DIR}/config-locales/sv_SE.UTF-8"
  RESULT_VARIABLE locale_result OUTPUT_VARIABLE locale_output ERROR_VARIABLE locale_error)
if(NOT locale_result EQUAL 0)
  message(FATAL_ERROR "Cannot build numeric locale fixture: ${locale_output}${locale_error}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
    "LOCPATH=${CAI_TEST_BINARY_DIR}/config-locales"
    "CAI_TEST_CONFIG_LOCALE=sv_SE.UTF-8"
    "${CAI_CONFIG_TEST}" "${CAI_TEST_BINARY_DIR}"
  RESULT_VARIABLE test_result OUTPUT_VARIABLE test_output ERROR_VARIABLE test_error)
if(NOT test_result EQUAL 0)
  message(FATAL_ERROR "Locale-independent INI numbers failed: ${test_output}${test_error}")
endif()
