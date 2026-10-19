  if(CAI_BUILD_TESTS)
    add_library(cai_cli_storage_fixture OBJECT tests/cli_storage_fixture.c)
    target_link_libraries(cai_cli_storage_fixture PRIVATE cai_cli_support)
    cai_configure_cli_target(cai_cli_storage_fixture)
    function(cai_cli_use_storage_fixture target)
      target_sources(${target} PRIVATE $<TARGET_OBJECTS:cai_cli_storage_fixture>)
      target_link_options(${target} PRIVATE "-Wl,--wrap=lc_client_open")
    endfunction()
    add_executable(cai_cli_mock_fixture tests/cli_mock_fixture.c tests/cli_mcp_config_fixture.c)
    target_link_libraries(cai_cli_mock_fixture PRIVATE cai_cli_support)
    cai_configure_cli_target(cai_cli_mock_fixture)
    cai_cli_use_storage_fixture(cai_cli_mock_fixture)
    target_link_options(cai_cli_mock_fixture PRIVATE "-Wl,--wrap=cai_mcp_streamable_http_client_open")
    add_executable(cai_terminal_peer tests/terminal_peer.c)
    cai_configure_cli_target(cai_terminal_peer)
    add_test(NAME cai_cli_terminal_peer
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/terminal_peer_test.sh"
        "$<TARGET_FILE:cai_terminal_peer>")
    set_tests_properties(cai_cli_terminal_peer PROPERTIES TIMEOUT 5)
    find_program(CAI_TEST_PYTHON python3 REQUIRED)
    find_program(CAI_TEST_JQ jq REQUIRED)
    find_program(CAI_TEST_TMUX tmux REQUIRED)
    add_executable(cai_cli_softline_resize_repro tests/softline_resize_repro.c)
    target_link_libraries(cai_cli_softline_resize_repro PRIVATE softline::softline_static)
    cai_configure_cli_target(cai_cli_softline_resize_repro)
    add_executable(cai_cli_resize_fixture tests/cli_resize_fixture.c)
    target_link_libraries(cai_cli_resize_fixture PRIVATE cai_cli_support)
    cai_configure_cli_target(cai_cli_resize_fixture)
    cai_cli_use_storage_fixture(cai_cli_resize_fixture)
    add_test(NAME cai_cli_resize_settled
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_terminal_resize_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>" "${CAI_TEST_TMUX}" settled)
    add_test(NAME cai_cli_resize_producer
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_terminal_resize_test.sh"
        "$<TARGET_FILE:cai_cli_resize_fixture>" "${CAI_TEST_TMUX}" producer)
    set_tests_properties(cai_cli_resize_settled cai_cli_resize_producer PROPERTIES TIMEOUT 30)
    foreach(_cai_resize_mode watch yield retained)
      set(_cai_resize_draft early)
      if(_cai_resize_mode STREQUAL "retained")
        set(_cai_resize_draft late)
      endif()
      add_test(NAME cai_cli_softline_resize_${_cai_resize_mode}_shrink
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/softline_resize_repro.sh"
          "$<TARGET_FILE:cai_cli_softline_resize_repro>" 120 30 60 18 "${_cai_resize_mode}" partial "${_cai_resize_draft}")
      add_test(NAME cai_cli_softline_resize_${_cai_resize_mode}_grow
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/softline_resize_repro.sh"
          "$<TARGET_FILE:cai_cli_softline_resize_repro>" 60 30 130 36 "${_cai_resize_mode}" complete "${_cai_resize_draft}")
      set_tests_properties(cai_cli_softline_resize_${_cai_resize_mode}_shrink
        cai_cli_softline_resize_${_cai_resize_mode}_grow PROPERTIES TIMEOUT 20)
    endforeach()
    # BUG-001: this diagnostic deterministically exposes softline 0.10.0's
    # documented unobserved-retained-frame limitation. Keep it visible, but
    # run its failing assertion manually until the upstream repair is consumed.
    # See docs/softline-retained-resize-report.md; all other resize cases gate.
    set_tests_properties(cai_cli_softline_resize_retained_shrink PROPERTIES DISABLED TRUE)
    add_test(NAME cai_cli_help COMMAND cai_cli --help)
    add_test(NAME cai_cli_config
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_config_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>")
    set_tests_properties(cai_cli_config PROPERTIES TIMEOUT 45)
    foreach(_cai_mcp_protocol responses chat_completions)
      add_test(NAME cai_cli_mcp_inventory_${_cai_mcp_protocol}
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_mcp_inventory_test.sh"
          "$<TARGET_FILE:cai_cli_mock_fixture>" "${_cai_mcp_protocol}")
      set_tests_properties(cai_cli_mcp_inventory_${_cai_mcp_protocol}
        PROPERTIES TIMEOUT 45)
    endforeach()
    foreach(_cai_agents_protocol responses chat_completions)
      add_test(NAME cai_cli_tool_policy_${_cai_agents_protocol}
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_tool_policy_test.sh"
          "$<TARGET_FILE:cai_cli_mock_fixture>" "${_cai_agents_protocol}")
      set_tests_properties(cai_cli_tool_policy_${_cai_agents_protocol} PROPERTIES TIMEOUT 45)
      add_test(NAME cai_cli_agents_md_${_cai_agents_protocol}
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_agents_md_test.sh"
          "$<TARGET_FILE:cai_cli_mock_fixture>" "${_cai_agents_protocol}")
      set_tests_properties(cai_cli_agents_md_${_cai_agents_protocol} PROPERTIES TIMEOUT 45)
    endforeach()
    add_test(NAME cai_cli_skills
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_skills_test.sh" "$<TARGET_FILE:cai_cli_mock_fixture>")
    set_tests_properties(cai_cli_skills PROPERTIES TIMEOUT 45)
    add_test(NAME cai_cli_chrome
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/chrome_test.sh"
        cli "$<TARGET_FILE:cai_cli_mock_fixture>" "${CMAKE_CURRENT_BINARY_DIR}"
        "$<TARGET_FILE:cai_cli_startup_fixture>")
    set_tests_properties(cai_cli_chrome PROPERTIES LABELS "integration;local;offline" TIMEOUT 90)

    add_executable(cai_cli_shell_fixture tests/cli_shell_fixture.c)
    target_link_libraries(cai_cli_shell_fixture PRIVATE cai_cli_support)
    cai_configure_cli_target(cai_cli_shell_fixture)
    cai_cli_use_storage_fixture(cai_cli_shell_fixture)
    add_test(NAME cai_cli_trim
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_trim_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>" "$<TARGET_FILE:cai_cli_shell_fixture>")
    set_tests_properties(cai_cli_trim PROPERTIES TIMEOUT 45)
    add_executable(cai_cli_settings_fixture tests/cli_settings_fixture.c)
    target_link_libraries(cai_cli_settings_fixture PRIVATE cai_cli_support)
    cai_configure_cli_target(cai_cli_settings_fixture)
    cai_cli_use_storage_fixture(cai_cli_settings_fixture)
    add_test(NAME cai_cli_tier
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_tier_test.sh"
        "$<TARGET_FILE:cai_cli_settings_fixture>")
    set_tests_properties(cai_cli_tier PROPERTIES TIMEOUT 30)
    add_executable(cai_cli_spinner_test tests/cli_spinner_test.c)
    target_link_libraries(cai_cli_spinner_test PRIVATE cai_cli_support)
    cai_configure_cli_target(cai_cli_spinner_test)
    add_test(NAME cai_cli_spinner COMMAND cai_cli_spinner_test)
    add_test(NAME cai_cli_model_catalog
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_model_catalog_test.sh"
        "$<TARGET_FILE:cai_cli_settings_fixture>")
    set_tests_properties(cai_cli_model_catalog PROPERTIES TIMEOUT 30)
    add_test(NAME cai_cli_context_metadata
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_context_metadata_test.sh"
        "$<TARGET_FILE:cai_cli_settings_fixture>")
    add_test(NAME cai_cli_pty_capture
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_pty_capture_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>")
    set_tests_properties(cai_cli_context_metadata cai_cli_pty_capture PROPERTIES TIMEOUT 45)
    foreach(_cai_settings_protocol responses chat_completions)
      add_test(NAME cai_cli_settings_${_cai_settings_protocol}
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_settings_test.sh"
          "$<TARGET_FILE:cai_cli_settings_fixture>" "${_cai_settings_protocol}")
      set_tests_properties(cai_cli_settings_${_cai_settings_protocol} PROPERTIES TIMEOUT 45)
    endforeach()
    add_test(NAME cai_cli_shell_context
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_shell_context_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>" "$<TARGET_FILE:cai_cli_shell_fixture>")
    set_tests_properties(cai_cli_shell_context PROPERTIES TIMEOUT 30)
    add_executable(cai_cli_shell_runner_test tests/cli_shell_runner_test.c cli/shell.c cli/pouch_stream.c)
    target_link_libraries(cai_cli_shell_runner_test PRIVATE cai_static lockdc::static)
    cai_configure_cli_target(cai_cli_shell_runner_test)
    target_compile_options(cai_cli_shell_runner_test PRIVATE -UNDEBUG)
    add_test(NAME cai_cli_shell_runner COMMAND cai_cli_shell_runner_test "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(cai_cli_shell_runner PROPERTIES TIMEOUT 15)
    add_executable(cai_cli_options_test tests/cli_options_test.c cli/options.c cli/help.c)
    target_link_libraries(cai_cli_options_test PRIVATE cai_static)
    target_include_directories(cai_cli_options_test PRIVATE
      "${CAI_PUBLIC_INCLUDE_DIR}" "${CAI_GENERATED_INCLUDE_DIR}")
    cai_configure_cli_target(cai_cli_options_test)
    add_test(NAME cai_cli_options COMMAND cai_cli_options_test)
    add_executable(cai_cli_runtime_options_test tests/cli_runtime_options_test.c)
    target_link_libraries(cai_cli_runtime_options_test PRIVATE cai_cli_support)
    cai_configure_cli_target(cai_cli_runtime_options_test)
    add_test(NAME cai_cli_runtime_options COMMAND cai_cli_runtime_options_test
      "${CMAKE_CURRENT_BINARY_DIR}")
    add_executable(cai_cli_storage_stream_test
      tests/cli_storage_stream_test.c cli/pouch_stream.c)
    target_link_libraries(cai_cli_storage_stream_test PRIVATE cai_static lockdc::static)
    cai_configure_cli_target(cai_cli_storage_stream_test)
    add_test(NAME cai_cli_storage_stream COMMAND cai_cli_storage_stream_test)
    set_tests_properties(cai_cli_storage_stream PROPERTIES TIMEOUT 30)
    add_executable(cai_cli_reasoning_test tests/cli_reasoning_test.c)
    target_link_libraries(cai_cli_reasoning_test PRIVATE cai_cli_support)
    cai_configure_cli_target(cai_cli_reasoning_test)
    add_test(NAME cai_cli_reasoning COMMAND cai_cli_reasoning_test)
    add_executable(cai_cli_log_test tests/cli_log_test.c
      cli/log.c cli/response.c cli/pouch_stream.c)
    target_link_libraries(cai_cli_log_test PRIVATE cai_static lockdc::static)
    cai_configure_cli_target(cai_cli_log_test)
    add_test(NAME cai_cli_log COMMAND cai_cli_log_test "${CMAKE_CURRENT_BINARY_DIR}")
    add_executable(cai_cli_startup_fixture tests/cli_startup_fixture.c)
    target_link_libraries(cai_cli_startup_fixture PRIVATE cai_cli_support)
    cai_configure_cli_target(cai_cli_startup_fixture)
    cai_cli_use_storage_fixture(cai_cli_startup_fixture)
    add_test(NAME cai_cli_startup
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_startup_test.sh"
        "$<TARGET_FILE:cai_cli_startup_fixture>")
    set_tests_properties(cai_cli_startup PROPERTIES TIMEOUT 30)
    add_executable(cai_cli_quota_refresh_test tests/cli_quota_refresh_test.c
      cli/quota_refresh.c cli/pouch_stream.c)
    target_link_libraries(cai_cli_quota_refresh_test PRIVATE cai_static lockdc::static)
    cai_configure_cli_target(cai_cli_quota_refresh_test)
    target_compile_options(cai_cli_quota_refresh_test PRIVATE -UNDEBUG)
    add_test(NAME cai_cli_quota_refresh COMMAND cai_cli_quota_refresh_test)
    set_tests_properties(cai_cli_quota_refresh PROPERTIES TIMEOUT 30)
    add_executable(cai_cli_pouch_startup_test tests/cli_pouch_startup_test.c
      cli/pouch_stream.c)
    target_link_libraries(cai_cli_pouch_startup_test PRIVATE cai_static lockdc::static)
    cai_configure_cli_target(cai_cli_pouch_startup_test)
    target_compile_options(cai_cli_pouch_startup_test PRIVATE -UNDEBUG)
    add_test(NAME cai_cli_pouch_startup
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_pouch_startup_test.sh"
        "$<TARGET_FILE:cai_cli_pouch_startup_test>" "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(cai_cli_pouch_startup PROPERTIES TIMEOUT 60)
    add_test(NAME cai_cli_replay_limits
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_replay_limits_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>" "$<TARGET_FILE:cai_cli_pouch_startup_test>")
    set_tests_properties(cai_cli_replay_limits PROPERTIES TIMEOUT 30)
    add_executable(cai_cli_pouch_compaction_test tests/cli_pouch_compaction_test.c
      cli/pouch_stream.c)
    target_link_libraries(cai_cli_pouch_compaction_test PRIVATE cai_static lockdc::static)
    cai_configure_cli_target(cai_cli_pouch_compaction_test)
    target_compile_options(cai_cli_pouch_compaction_test PRIVATE -UNDEBUG)
    target_link_options(cai_cli_pouch_compaction_test PRIVATE "-Wl,--wrap=pthread_cond_timedwait")
    add_test(NAME cai_cli_pouch_compaction
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_pouch_compaction_test.sh"
        "$<TARGET_FILE:cai_cli>" "$<TARGET_FILE:cai_cli_pouch_compaction_test>")
    set_tests_properties(cai_cli_pouch_compaction PROPERTIES TIMEOUT 60)
    add_test(NAME cai_cli_pouch_recovery
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_pouch_recovery_test.sh"
        "$<TARGET_FILE:cai_cli>")
    set_tests_properties(cai_cli_pouch_recovery PROPERTIES TIMEOUT 30)
    add_test(NAME cai_cli_idle
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_idle_test.sh"
        "$<TARGET_FILE:cai_cli>")
    set_tests_properties(cai_cli_idle PROPERTIES TIMEOUT 45)
    add_executable(cai_cli_status_test tests/cli_status_test.c cli/status.c)
    target_link_libraries(cai_cli_status_test PRIVATE cai_static
      softline::softline_static)
    cai_configure_cli_target(cai_cli_status_test)
    add_test(NAME cai_cli_status COMMAND cai_cli_status_test)
    add_executable(cai_cli_review_report_test
      tests/cli_review_report_test.c cli/review_report.c)
    target_link_libraries(cai_cli_review_report_test PRIVATE cai_static)
    cai_configure_cli_target(cai_cli_review_report_test)
    add_test(NAME cai_cli_review_report COMMAND cai_cli_review_report_test)
    add_test(NAME cai_cli_sessions
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_sessions_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>" "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME cai_cli_providers
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_providers_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>" "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME cai_cli_login
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_login_test.sh"
        "$<TARGET_FILE:cai_cli>")
    add_test(NAME cai_cli_modes
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_modes_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>")
    add_test(NAME cai_cli_terminal
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_terminal_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>")
    set_tests_properties(cai_cli_terminal PROPERTIES TIMEOUT 60)
    add_test(NAME cai_cli_commands
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_commands_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>")
    set_tests_properties(cai_cli_commands PROPERTIES TIMEOUT 45)
    add_test(NAME cai_cli_terminal_handoff
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_terminal_handoff_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>")
    set_tests_properties(cai_cli_terminal_handoff PROPERTIES TIMEOUT 45)
    add_test(NAME cai_cli_history
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_history_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>")
    set_tests_properties(cai_cli_history PROPERTIES TIMEOUT 45)
    add_test(NAME cai_cli_reasoning_presentation
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_reasoning_presentation_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>")
    set_tests_properties(cai_cli_reasoning_presentation PROPERTIES TIMEOUT 45)
    add_test(NAME cai_cli_notices
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_notices_test.sh"
        "$<TARGET_FILE:cai_cli_startup_fixture>")
    set_tests_properties(cai_cli_notices PROPERTIES TIMEOUT 30)
    add_test(NAME cai_cli_goal
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_goal_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>")
    set_tests_properties(cai_cli_goal PROPERTIES TIMEOUT 45)
    add_test(NAME cai_cli_shell
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_shell_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>")
    set_tests_properties(cai_cli_shell PROPERTIES TIMEOUT 45)
    add_test(NAME cai_cli_tool_display
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_tool_display_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>")
    set_tests_properties(cai_cli_tool_display PROPERTIES TIMEOUT 60)
    add_test(NAME cai_cli_terminal_failures
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_terminal_failures_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>")
    set_tests_properties(cai_cli_terminal_failures PROPERTIES TIMEOUT 60)
    foreach(_policy_protocol responses chat_completions)
      add_test(NAME cai_cli_policy_diagnostic_${_policy_protocol}
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_policy_diagnostic_test.sh"
          "$<TARGET_FILE:cai_cli_mock_fixture>" "${_policy_protocol}")
      set_tests_properties(cai_cli_policy_diagnostic_${_policy_protocol}
        PROPERTIES TIMEOUT 45)
    endforeach()
    add_test(NAME cai_cli_lockd_endpoint
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_lockd_endpoint_test.sh"
        "$<TARGET_FILE:cai_cli>")
    add_test(NAME cai_cli_package_layout
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_package_layout_test.sh"
        "${CMAKE_CURRENT_BINARY_DIR}")
    get_property(_cai_cli_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    list(FILTER _cai_cli_targets INCLUDE REGEX "^cai_cli($|_)|^cai_terminal_peer$")
    add_custom_target(cai_cli_tests DEPENDS ${_cai_cli_targets})
    add_dependencies(cai_cli_tests cai_app_test cai_app_host)
    get_property(_cai_cli_tests DIRECTORY PROPERTY TESTS)
    list(FILTER _cai_cli_tests INCLUDE REGEX "^cai_cli_")
    set_tests_properties(${_cai_cli_tests} PROPERTIES LABELS "cli;local;offline"
      ENVIRONMENT "CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>;CAI_TEST_PYTHON=${CAI_TEST_PYTHON};CAI_TEST_JQ=${CAI_TEST_JQ};CAI_TEST_TMUX=${CAI_TEST_TMUX}")
    # These cases assert CLI presentation/tool wiring, not stored history.
    # Stateful cases retain real encrypted checkpoint/replay/export coverage.
    set_property(TEST cai_cli_mcp_inventory_responses
      cai_cli_mcp_inventory_chat_completions cai_cli_tool_policy_responses
      cai_cli_tool_policy_chat_completions cai_cli_skills cai_cli_commands
      cai_cli_tool_display cai_cli_reasoning_presentation cai_cli_terminal_handoff
      APPEND PROPERTY ENVIRONMENT "CAI_TEST_VOLATILE_SESSION=1")
    foreach(_cai_cli_test IN LISTS _cai_cli_tests)
      get_test_property(${_cai_cli_test} TIMEOUT _cai_cli_timeout)
      if(NOT _cai_cli_timeout)
        set_tests_properties(${_cai_cli_test} PROPERTIES TIMEOUT 90)
      endif()
    endforeach()
  endif()
