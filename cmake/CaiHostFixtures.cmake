if(CAI_BUILD_TESTS)
  find_program(CAI_TEST_PYTHON python3 REQUIRED)
  find_program(CAI_TEST_JQ jq REQUIRED)
  add_test(NAME cai_acquisition_test COMMAND bash
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/acquisition_test.sh"
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(cai_acquisition_test PROPERTIES LABELS "host;local;lifecycle" TIMEOUT 15)
  add_test(NAME cai_package_inventory_test COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_inventory_test.sh" "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(cai_package_inventory_test PROPERTIES LABELS "host;local;lifecycle" TIMEOUT 10)
  add_test(NAME cai_clean_test COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/clean_test.sh" "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(cai_clean_test PROPERTIES LABELS "host;local;lifecycle" TIMEOUT 10)
  if(CAI_INSTALL AND CAI_BUILD_STATIC AND CAI_BUILD_SHARED AND CAI_TARGET_ID STREQUAL "x86_64-linux-gnu")
    add_test(NAME cai_package_repro_test COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_repro_test.sh"
      "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_STRIP}")
    set_tests_properties(cai_package_repro_test PROPERTIES LABELS "local;packaging" TIMEOUT 15)
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-tmp/chrome-protocol")
    file(CHMOD "${CMAKE_CURRENT_BINARY_DIR}/test-tmp/chrome-protocol" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    add_executable(cai_chrome_host_protocol_test tests/chrome_host_protocol_test.c)
    cai_configure_c_target(cai_chrome_host_protocol_test)
    target_compile_options(cai_chrome_host_protocol_test PRIVATE -UNDEBUG)
    add_test(NAME cai_chrome_host_protocol COMMAND cai_chrome_host_protocol_test
      "$<TARGET_FILE:cai_chrome_host_fixture>")
    set_tests_properties(cai_chrome_host_protocol PROPERTIES LABELS "unit;local;offline" TIMEOUT 20
      ENVIRONMENT "XDG_RUNTIME_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-tmp/chrome-protocol;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-tmp/chrome-protocol;HOME=${CMAKE_CURRENT_BINARY_DIR}/test-tmp/chrome-protocol")
  endif()
endif()
