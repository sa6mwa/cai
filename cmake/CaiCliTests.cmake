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
    add_executable(cai_cli_geometry_test tests/cli_geometry_test.c)
    target_link_libraries(cai_cli_geometry_test PRIVATE cai_cli_support)
    cai_configure_cli_target(cai_cli_geometry_test)
    add_test(NAME cai_cli_geometry COMMAND cai_cli_geometry_test)
    add_executable(cai_cli_messages_test tests/cli_messages_test.c
      cli/identity.c cli/pouch.c cli/pouch_stream.c cli/session_commands.c cli/log.c cli/message_names.c)
    target_link_libraries(cai_cli_messages_test PRIVATE cai_static
      softline::softline_static lockdc::static)
    cai_configure_cli_target(cai_cli_messages_test)
    target_compile_options(cai_cli_messages_test PRIVATE -UNDEBUG)
    cai_cli_use_storage_fixture(cai_cli_messages_test)
    add_test(NAME cai_cli_message_names COMMAND cai_cli_messages_test --names-only)
    add_test(NAME cai_cli_messages_native COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_messages_test.sh"
      "$<TARGET_FILE:cai_cli_messages_test>")
    set_tests_properties(cai_cli_messages_native PROPERTIES TIMEOUT 15)
    add_test(NAME cai_cli_messages_startup_cancel COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_messages_test.sh"
      "$<TARGET_FILE:cai_cli_messages_test>" startup)
    set_tests_properties(cai_cli_messages_startup_cancel PROPERTIES TIMEOUT 3)
    add_test(NAME cai_cli_messages_integration COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_messages_integration_test.sh"
      "$<TARGET_FILE:cai_cli_mock_fixture>")
    set_tests_properties(cai_cli_messages_integration PROPERTIES TIMEOUT 45)
    add_test(NAME cai_cli_messages_toggle_failure COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_messages_toggle_failure_test.sh"
      "$<TARGET_FILE:cai_cli_mock_fixture>")
    set_tests_properties(cai_cli_messages_toggle_failure PROPERTIES TIMEOUT 15)
    add_test(NAME cai_cli_help COMMAND cai_cli --help)
    add_test(NAME cai_cli_startup_text COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_startup_text_test.sh"
      "$<TARGET_FILE:cai_cli>" cai)
    set_tests_properties(cai_cli_startup_text PROPERTIES TIMEOUT 15)
    add_test(NAME cai_app_startup_text COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/app_startup_text_test.sh"
      "$<TARGET_FILE:cai_app_host>" host)
    set_tests_properties(cai_app_startup_text PROPERTIES LABELS "cli;local;offline" TIMEOUT 15
      ENVIRONMENT "CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>")
    add_test(NAME cai_cli_startup_spacing COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_startup_spacing_test.sh"
      "$<TARGET_FILE:cai_cli_mock_fixture>" "$<TARGET_FILE:cai_app_host>")
    set_tests_properties(cai_cli_startup_spacing PROPERTIES TIMEOUT 15)
    add_test(NAME cai_cli_config
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_config_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>")
    add_test(NAME cai_cli_home COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_home_test.sh"
      "$<TARGET_FILE:cai_cli_mock_fixture>")
    set_tests_properties(cai_cli_home PROPERTIES TIMEOUT 15)
    set_tests_properties(cai_cli_config PROPERTIES TIMEOUT 45)
    foreach(_final_protocol responses chat_completions)
      add_test(NAME cai_cli_final_output_${_final_protocol} COMMAND bash
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_final_output_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>" "${_final_protocol}")
      set_tests_properties(cai_cli_final_output_${_final_protocol} PROPERTIES TIMEOUT 10)
    endforeach()
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
    add_test(NAME cai_cli_skills_inventory COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_skills_inventory_test.sh"
      "$<TARGET_FILE:cai_cli_mock_fixture>")
    set_tests_properties(cai_cli_skills_inventory PROPERTIES TIMEOUT 15)
    add_test(NAME cai_cli_review_defaults COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_review_defaults_test.sh"
      "$<TARGET_FILE:cai_cli_mock_fixture>")
    set_tests_properties(cai_cli_review_defaults PROPERTIES TIMEOUT 15)
    add_test(NAME cai_cli_review_and_fix COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_review_and_fix_test.sh"
      "$<TARGET_FILE:cai_cli_mock_fixture>")
    set_tests_properties(cai_cli_review_and_fix PROPERTIES TIMEOUT 15)
    add_test(NAME cai_cli_chrome
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/chrome_test.sh"
        cli "$<TARGET_FILE:cai_cli_mock_fixture>" "${CMAKE_CURRENT_BINARY_DIR}"
        "$<TARGET_FILE:cai_chrome_client_fixture>")
    set_tests_properties(cai_cli_chrome PROPERTIES LABELS "integration;local;offline" TIMEOUT 90)
    add_test(NAME cai_cli_chrome_stop COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/chrome_stop_test.sh"
      "$<TARGET_FILE:cai_cli_mock_fixture>" "$<TARGET_FILE:cai_chrome_client_fixture>")
    set_tests_properties(cai_cli_chrome_stop PROPERTIES LABELS "cli;local;offline" TIMEOUT 15)

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
    target_link_options(cai_cli_shell_runner_test PRIVATE "-Wl,--wrap=read")
    add_test(NAME cai_cli_shell_runner COMMAND cai_cli_shell_runner_test "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(cai_cli_shell_runner PROPERTIES TIMEOUT 15)
    add_test(NAME cai_cli_shell_draft COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_shell_draft_test.sh"
      "$<TARGET_FILE:cai_cli_mock_fixture>")
    set_tests_properties(cai_cli_shell_draft PROPERTIES TIMEOUT 15)
    add_executable(cai_cli_options_test tests/cli_options_test.c cli/options.c cli/help.c)
    add_executable(cai_cli_todo_view_test tests/cli_todo_view_test.c)
    target_link_libraries(cai_cli_todo_view_test PRIVATE cai_app_static)
    cai_configure_cli_target(cai_cli_todo_view_test)
    target_compile_options(cai_cli_todo_view_test PRIVATE -UNDEBUG)
    add_test(NAME cai_cli_todo_view COMMAND cai_cli_todo_view_test)
    target_link_libraries(cai_cli_options_test PRIVATE cai_static)
    target_include_directories(cai_cli_options_test PRIVATE
      "${CAI_PUBLIC_INCLUDE_DIR}" "${CAI_GENERATED_INCLUDE_DIR}")
    cai_configure_cli_target(cai_cli_options_test)
    add_test(NAME cai_cli_options COMMAND cai_cli_options_test)
    foreach(_cai_ids_protocol responses chat_completions)
      add_test(NAME cai_cli_status_ids_${_cai_ids_protocol} COMMAND bash
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_status_ids_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>" "${_cai_ids_protocol}")
      set_tests_properties(cai_cli_status_ids_${_cai_ids_protocol} PROPERTIES TIMEOUT 15)
    endforeach()
    add_executable(cai_cli_replay_test tests/cli_replay_test.c)
    target_link_libraries(cai_cli_replay_test PRIVATE cai_cli_support)
    cai_configure_cli_target(cai_cli_replay_test)
    target_compile_options(cai_cli_replay_test PRIVATE -UNDEBUG)
    add_test(NAME cai_cli_replay_unit COMMAND cai_cli_replay_test)
    set_tests_properties(cai_cli_replay_unit PROPERTIES TIMEOUT 5)
    add_test(NAME cai_cli_replay_tail COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_replay_tail_test.sh"
      "$<TARGET_FILE:cai_cli_mock_fixture>")
    set_tests_properties(cai_cli_replay_tail PROPERTIES TIMEOUT 20)
    add_executable(cai_cli_temp_unit_test tests/cli_temp_unit_test.c)
    target_link_libraries(cai_cli_temp_unit_test PRIVATE cai_cli_support)
    cai_configure_cli_target(cai_cli_temp_unit_test)
    target_compile_options(cai_cli_temp_unit_test PRIVATE -UNDEBUG)
    add_test(NAME cai_cli_temp_unit COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_temp_unit_test.sh"
      "$<TARGET_FILE:cai_cli_temp_unit_test>" "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(cai_cli_temp_unit PROPERTIES TIMEOUT 5)
    add_test(NAME cai_cli_temp COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_temp_test.sh"
      "$<TARGET_FILE:cai_cli_mock_fixture>" "$<TARGET_FILE:cai_app_controls_host>")
    set_tests_properties(cai_cli_temp PROPERTIES TIMEOUT 30)
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
      cli/identity.c cli/log.c cli/response.c cli/pouch_stream.c)
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
      cli/identity.c cli/pouch_stream.c cli/log.c)
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
      cli/identity.c cli/pouch_stream.c cli/log.c)
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
    add_executable(cai_cli_theme_test tests/cli_theme_test.c)
    target_link_libraries(cai_cli_theme_test PRIVATE cai_cli_support)
    cai_configure_cli_target(cai_cli_theme_test)
    target_compile_options(cai_cli_theme_test PRIVATE -UNDEBUG)
    add_test(NAME cai_cli_theme COMMAND cai_cli_theme_test)
    add_test(NAME cai_cli_themes COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_themes_test.sh"
      "$<TARGET_FILE:cai_cli_mock_fixture>")
    set_tests_properties(cai_cli_themes PROPERTIES TIMEOUT 20)
    add_executable(cai_cli_review_report_test
      tests/cli_review_report_test.c cli/review_report.c)
    target_link_libraries(cai_cli_review_report_test PRIVATE cai_static)
    cai_configure_cli_target(cai_cli_review_report_test)
    add_test(NAME cai_cli_review_report COMMAND cai_cli_review_report_test)
    add_test(NAME cai_cli_sessions
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_sessions_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>" "${CMAKE_CURRENT_BINARY_DIR}")
    add_executable(cai_cli_resume_selection_test tests/cli_resume_selection_test.c)
    target_link_libraries(cai_cli_resume_selection_test PRIVATE cai_cli_support)
    target_link_options(cai_cli_resume_selection_test PRIVATE
      "-Wl,--wrap=cai_cli_pouch_list" "-Wl,--wrap=cai_cli_pouch_deletable")
    cai_configure_cli_target(cai_cli_resume_selection_test)
    target_compile_options(cai_cli_resume_selection_test PRIVATE -UNDEBUG)
    add_test(NAME cai_cli_resume_selection COMMAND cai_cli_resume_selection_test)
    add_test(NAME cai_cli_providers
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_providers_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>" "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME cai_cli_login
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_login_test.sh"
        "$<TARGET_FILE:cai_cli>")
    add_executable(cai_cli_login_response_test tests/cli_login_response_test.c)
    target_link_libraries(cai_cli_login_response_test PRIVATE cai_cli_support)
    cai_configure_cli_target(cai_cli_login_response_test)
    target_compile_options(cai_cli_login_response_test PRIVATE -UNDEBUG)
    add_test(NAME cai_cli_login_response COMMAND cai_cli_login_response_test)
    set_tests_properties(cai_cli_login_response PROPERTIES TIMEOUT 3)
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
    add_test(NAME cai_cli_draft_quit COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_draft_quit_test.sh"
      "$<TARGET_FILE:cai_cli_mock_fixture>")
    set_tests_properties(cai_cli_draft_quit PROPERTIES TIMEOUT 15)
    add_test(NAME cai_cli_yolo COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_yolo_test.sh" "$<TARGET_FILE:cai_cli_mock_fixture>")
    set_tests_properties(cai_cli_yolo PROPERTIES TIMEOUT 15)
    add_test(NAME cai_cli_delete COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_delete_test.sh" "$<TARGET_FILE:cai_cli_mock_fixture>")
    set_tests_properties(cai_cli_delete PROPERTIES TIMEOUT 20)
    foreach(_compact_protocol responses chat_completions)
      add_test(NAME cai_cli_compact_${_compact_protocol} COMMAND bash
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_compact_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>" "${_compact_protocol}")
      set_tests_properties(cai_cli_compact_${_compact_protocol} PROPERTIES TIMEOUT 30)
    endforeach()
    add_executable(cai_cli_pouch_delete_test tests/cli_pouch_delete_test.c)
    target_link_libraries(cai_cli_pouch_delete_test PRIVATE cai_cli_support)
    cai_configure_cli_target(cai_cli_pouch_delete_test)
    target_compile_options(cai_cli_pouch_delete_test PRIVATE -UNDEBUG)
    add_test(NAME cai_cli_pouch_delete COMMAND cai_cli_pouch_delete_test "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(cai_cli_pouch_delete PROPERTIES TIMEOUT 5)
    foreach(_steering_protocol responses chat_completions)
      add_test(NAME cai_cli_yolo_live_${_steering_protocol} COMMAND bash
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_yolo_live_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>" "${_steering_protocol}")
      set_tests_properties(cai_cli_yolo_live_${_steering_protocol} PROPERTIES TIMEOUT 20)
      add_test(NAME cai_cli_command_queue_${_steering_protocol} COMMAND bash
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_command_queue_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>" "${_steering_protocol}")
      set_tests_properties(cai_cli_command_queue_${_steering_protocol}
        PROPERTIES TIMEOUT 20)
      add_test(NAME cai_cli_execution_${_steering_protocol} COMMAND bash
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_execution_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>" "${_steering_protocol}")
      set_tests_properties(cai_cli_execution_${_steering_protocol} PROPERTIES TIMEOUT 15)
      add_test(NAME cai_cli_subagent_replay_${_steering_protocol} COMMAND bash
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_subagent_replay_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>" "${_steering_protocol}")
      set_tests_properties(cai_cli_subagent_replay_${_steering_protocol}
        PROPERTIES TIMEOUT 20)
      add_test(NAME cai_cli_steering_seam_${_steering_protocol} COMMAND bash
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_steering_seam_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>" "${_steering_protocol}" worker)
      set_tests_properties(cai_cli_steering_seam_${_steering_protocol}
        PROPERTIES TIMEOUT 30)
    endforeach()
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
    foreach(_get_goal_protocol responses chat_completions)
      add_test(NAME cai_cli_get_goal_${_get_goal_protocol} COMMAND bash
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_get_goal_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>" "${_get_goal_protocol}")
      set_tests_properties(cai_cli_get_goal_${_get_goal_protocol} PROPERTIES TIMEOUT 10)
    endforeach()
    foreach(_goal_protocol responses chat_completions)
      add_test(NAME cai_cli_goal_resume_${_goal_protocol} COMMAND bash
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_goal_resume_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>" "${_goal_protocol}")
      set_tests_properties(cai_cli_goal_resume_${_goal_protocol} PROPERTIES TIMEOUT 20)
      foreach(_goal_mode interactive batch)
        add_test(NAME cai_cli_goal_pursuit_${_goal_protocol}_${_goal_mode}
          COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_goal_pursuit_test.sh"
            "$<TARGET_FILE:cai_cli_mock_fixture>" "${_goal_protocol}" "${_goal_mode}")
        set_tests_properties(cai_cli_goal_pursuit_${_goal_protocol}_${_goal_mode}
          PROPERTIES TIMEOUT 30)
      endforeach()
    endforeach()
    add_test(NAME cai_cli_goal_disabled
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_goal_disabled_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>")
    set_tests_properties(cai_cli_goal_disabled PROPERTIES TIMEOUT 10)
    add_test(NAME cai_cli_send_message COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_send_message_test.sh"
      "$<TARGET_FILE:cai_cli_mock_fixture>")
    set_tests_properties(cai_cli_send_message PROPERTIES TIMEOUT 10)
    foreach(_reasoning_protocol responses chat_completions)
      foreach(_reasoning_case persistence unsupported interactive)
        add_test(NAME cai_cli_reasoning_tool_${_reasoning_protocol}_${_reasoning_case}
          COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_reasoning_tool_test.sh"
            "$<TARGET_FILE:cai_cli_mock_fixture>" "${_reasoning_protocol}" "${_reasoning_case}")
        set_tests_properties(cai_cli_reasoning_tool_${_reasoning_protocol}_${_reasoning_case}
          PROPERTIES TIMEOUT 15)
      endforeach()
    endforeach()
    add_test(NAME cai_cli_reasoning_catalog COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_reasoning_catalog_test.sh"
      "$<TARGET_FILE:cai_cli_settings_fixture>")
    set_tests_properties(cai_cli_reasoning_catalog PROPERTIES TIMEOUT 5)
    add_test(NAME cai_cli_catalog_empty_choices
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_catalog_empty_choices_test.sh"
        "$<TARGET_FILE:cai_cli_mock_fixture>")
    set_tests_properties(cai_cli_catalog_empty_choices PROPERTIES TIMEOUT 10)
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
    add_dependencies(cai_cli_tests cai_app_test cai_app_home_test cai_app_status_test cai_app_host cai_app_controls_host cai_app_startup_goal_test cai_app_utility_stop_test cai_kanban_test cai_chrome_client_fixture)
    add_test(NAME cai_cli_kanban COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_kanban_test.sh"
      "$<TARGET_FILE:cai_cli_mock_fixture>")
    get_property(_cai_cli_tests DIRECTORY PROPERTY TESTS)
    list(FILTER _cai_cli_tests INCLUDE REGEX "^cai_cli_")
    set_tests_properties(${_cai_cli_tests} PROPERTIES LABELS "cli;local;offline"
      ENVIRONMENT "CAI_TERMINAL_PEER=$<TARGET_FILE:cai_terminal_peer>;CAI_TEST_PYTHON=${CAI_TEST_PYTHON};CAI_TEST_JQ=${CAI_TEST_JQ};CAI_TEST_TMUX=${CAI_TEST_TMUX}")
    # These cases assert CLI presentation/tool wiring, not stored history.
    # Stateful cases retain real encrypted checkpoint/replay/export coverage.
    set_property(TEST cai_cli_mcp_inventory_responses
      cai_cli_mcp_inventory_chat_completions cai_cli_tool_policy_responses
      cai_cli_tool_policy_chat_completions cai_cli_skills cai_cli_skills_inventory cai_cli_commands
      cai_cli_tool_display cai_cli_reasoning_presentation cai_cli_terminal_handoff
      APPEND PROPERTY ENVIRONMENT "CAI_TEST_VOLATILE_SESSION=1")
    foreach(_cai_cli_test IN LISTS _cai_cli_tests)
      get_test_property(${_cai_cli_test} TIMEOUT _cai_cli_timeout)
      if(NOT _cai_cli_timeout)
        set_tests_properties(${_cai_cli_test} PROPERTIES TIMEOUT 90)
      endif()
    endforeach()
  endif()
