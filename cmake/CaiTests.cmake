
if(CAI_BUILD_TESTS)
  enable_testing()
  add_executable(cai_skill_metadata_test tests/skill_metadata_test.c)
  cai_link_internal_consumer(cai_skill_metadata_test)
  cai_configure_c_target(cai_skill_metadata_test)
  target_compile_options(cai_skill_metadata_test PRIVATE -UNDEBUG)
  add_test(NAME cai_skill_metadata_test COMMAND cai_skill_metadata_test)
  set_tests_properties(cai_skill_metadata_test PROPERTIES LABELS "unit;local" TIMEOUT 20)
  add_executable(cai_yaml_c89_test tests/yaml_c89_test.c)
  target_link_libraries(cai_yaml_c89_test PRIVATE cai_yaml_static)
  cai_configure_c_target(cai_yaml_c89_test)
  target_compile_options(cai_yaml_c89_test PRIVATE -UNDEBUG)
  add_test(NAME cai_yaml_c89_test COMMAND cai_yaml_c89_test)
  set_tests_properties(cai_yaml_c89_test PROPERTIES LABELS "unit;local" TIMEOUT 5)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux"
      AND CMAKE_SYSTEM_PROCESSOR STREQUAL CMAKE_HOST_SYSTEM_PROCESSOR
      AND CAI_TARGET_ID MATCHES "-linux-gnu$")
    add_test(NAME cai_yaml_producer_test COMMAND "${CMAKE_COMMAND}"
      "-DCAI_ROOT=${CMAKE_CURRENT_SOURCE_DIR}" "-DCAI_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
      "-DCAI_YAML_SOURCE=${CAI_LIBYAML_SOURCE}" "-DCAI_YAML_ARCHIVE=${yaml_archive}"
      "-DCAI_C_COMPILER=${CMAKE_C_COMPILER}" "-DCAI_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
      "-DCAI_TARGET_ID=${CAI_TARGET_ID}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/yaml_producer_test.cmake")
    set_tests_properties(cai_yaml_producer_test PROPERTIES LABELS "unit;local;lifecycle" TIMEOUT 60)
  endif()
  add_executable(cai_config_test tests/config_test.c)
  cai_link_internal_consumer(cai_config_test)
  cai_configure_c_target(cai_config_test)
  target_compile_options(cai_config_test PRIVATE -UNDEBUG)
  add_test(NAME cai_config_test COMMAND cai_config_test "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(cai_config_test PROPERTIES LABELS "unit;local" TIMEOUT 20)
  find_program(CAI_LOCALEDEF NAMES localedef)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux"
      AND CMAKE_SYSTEM_PROCESSOR STREQUAL CMAKE_HOST_SYSTEM_PROCESSOR
      AND CAI_TARGET_ID MATCHES "-linux-gnu$"
      AND CAI_LOCALEDEF AND EXISTS "/usr/share/i18n/locales/sv_SE"
      AND EXISTS "/usr/share/i18n/charmaps/UTF-8.gz")
    add_test(NAME cai_config_numeric_locale
      COMMAND "${CMAKE_COMMAND}" "-DCAI_CONFIG_TEST=$<TARGET_FILE:cai_config_test>"
        "-DCAI_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        "-DCAI_LOCALEDEF=${CAI_LOCALEDEF}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/config_locale_test.cmake")
    set_tests_properties(cai_config_numeric_locale PROPERTIES LABELS "unit;local" TIMEOUT 20)
  endif()
  add_executable(cai_tests tests/test_main.c)
    target_include_directories(cai_tests PRIVATE
    "${CAI_PUBLIC_INCLUDE_DIR}"
    "${CAI_GENERATED_INCLUDE_DIR}"
    "${CAI_LONEJSON_INCLUDE_DIR}"
    "${CAI_PSLOG_INCLUDE_DIR}"
    ${CURL_INCLUDE_DIRS}
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_definitions(cai_tests PRIVATE CAI_TESTING
    CAI_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
  cai_link_internal_consumer(cai_tests)
  if(CAI_PSLOG_LINK)
    target_link_libraries(cai_tests PRIVATE ${CAI_PSLOG_LINK})
  else()
    message(FATAL_ERROR
      "The logging integration tests require libpslog. Install libpslog, set "
      "CAI_PSLOG_LIBRARY, or configure with CAI_BUILD_TESTS=OFF.")
  endif()
  cai_configure_c_target(cai_tests)
  if(CAI_BUILD_EXAMPLES)
    add_executable(cai_smith_terminal_renderer_test
      tests/smith_terminal_renderer_test.c)
    target_include_directories(cai_smith_terminal_renderer_test PRIVATE
      "${CAI_PUBLIC_INCLUDE_DIR}"
      "${CAI_GENERATED_INCLUDE_DIR}"
      "${CAI_LONEJSON_INCLUDE_DIR}"
      "${CAI_PSLOG_INCLUDE_DIR}")
    cai_link_internal_consumer(cai_smith_terminal_renderer_test)
    if(CAI_PSLOG_LINK)
      target_link_libraries(cai_smith_terminal_renderer_test PRIVATE
        ${CAI_PSLOG_LINK})
    endif()
    cai_configure_c_target(cai_smith_terminal_renderer_test)
    add_test(NAME cai_smith_terminal_renderer_test
      COMMAND cai_smith_terminal_renderer_test)
    set_tests_properties(cai_smith_terminal_renderer_test PROPERTIES
      LABELS "unit;local;example-smoke"
      TIMEOUT 3)
  endif()
  add_executable(cai_chrome_client_fixture tests/chrome_client_fixture.c)
  cai_link_internal_consumer(cai_chrome_client_fixture)
  cai_configure_c_target(cai_chrome_client_fixture)
  if(CAI_TARGET_ID MATCHES "linux-musl$")
    target_link_options(cai_chrome_client_fixture PRIVATE -static)
  endif()
  add_test(NAME cai_chrome_client
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/chrome_test.sh"
      client "$<TARGET_FILE:cai_chrome_client_fixture>" "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(cai_chrome_client PROPERTIES LABELS "unit;local;offline" TIMEOUT 30)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(cai_chrome_host_fixture tests/chrome_host_fixture.c
      cli/chrome_host.c cli/chrome_setup.c)
    target_include_directories(cai_chrome_host_fixture PRIVATE "${CAI_PSLOG_INCLUDE_DIR}")
    cai_link_internal_consumer(cai_chrome_host_fixture)
    cai_configure_c_target(cai_chrome_host_fixture)
    if(CAI_TARGET_ID MATCHES "linux-musl$")
      target_link_options(cai_chrome_host_fixture PRIVATE -static)
    endif()
    add_test(NAME cai_chrome_host
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/chrome_test.sh"
        host "$<TARGET_FILE:cai_chrome_host_fixture>" "${CMAKE_CURRENT_BINARY_DIR}" "$<TARGET_FILE:cai_chrome_client_fixture>")
    set_tests_properties(cai_chrome_host PROPERTIES LABELS "unit;local;offline" TIMEOUT 30)
  endif()
  add_executable(cai_user_shell_test tests/user_shell_test.c)
  cai_link_internal_consumer(cai_user_shell_test)
  cai_configure_c_target(cai_user_shell_test)
  target_compile_options(cai_user_shell_test PRIVATE -UNDEBUG)
  add_test(NAME cai_user_shell COMMAND cai_user_shell_test "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(cai_user_shell PROPERTIES LABELS "unit;local;offline" TIMEOUT 15)
  add_executable(cai_display_test tests/display_test.c)
  cai_link_internal_consumer(cai_display_test)
  cai_configure_c_target(cai_display_test)
  target_compile_options(cai_display_test PRIVATE -UNDEBUG)
  add_test(NAME cai_display COMMAND cai_display_test)
  set_tests_properties(cai_display PROPERTIES LABELS "unit;local" TIMEOUT 10)
  set(CAI_UNIT_TEST_GROUPS
    mcp_raw_input_cancellation
    mcp_raw_input_registration_ownership
    mcp_raw_input_refresh_ownership
    mcp_raw_input_agent_round_trip
    mcp_raw_input_transport_failures
    mcp_raw_input_failures
    mcp_raw_input_metadata
    mcp_raw_input_policy
    mcp_raw_input_recovery
    mcp_raw_input_server
    mcp_raw_input_upload
    chatgpt_quota_parse
    chatgpt_quota_http
    model_capabilities
    env_precedence
    source_sink
    lonejson_nested_mapped_array_stream
    lonejson_selected_array_rewrite
    tool_registry
    mcp_client_config
    mcp_client_rejects_partial_allocator
    mcp_client_open_failure_cleans_allocator
    mcp_client_open_failure_keeps_receiver_cleanup
    mcp_streamable_http_header_callback_bounds
    mcp_client_receiver_surface
    mcp_client_registry_adapter
    mcp_streamable_http_client_roundtrip
    mcp_streamable_http_client_api_key_rejects_unsafe_token
    mcp_streamable_http_client_api_key_rejects_remote_http
    mcp_streamable_http_client_api_key_rejects_127_hostname
    mcp_streamable_http_client_api_key_rejects_userinfo_loopback
    mcp_streamable_http_client_api_key_rejects_short_url
    mcp_streamable_http_client_oauth2_client_credentials
    mcp_streamable_http_client_oauth2_rejects_remote_http_token
    mcp_streamable_http_client_oauth2_rejects_discovered_remote_http_token
    mcp_streamable_http_client_oauth2_rejects_loopback_http_oidc_discovery
    mcp_streamable_http_client_oauth2_rejects_non_bearer_token
    mcp_streamable_http_client_oauth2_rejects_unsafe_token
    mcp_streamable_http_client_oauth2_token_http_error
    mcp_streamable_http_client_oauth2_token_response_limit
    mcp_streamable_http_client_streaming_oauth2_error
    mcp_streamable_http_client_custom_allocator_metadata
    mcp_streamable_http_client_logging
    mcp_streamable_http_call_result_must_be_object
    mcp_streamable_http_ping_error
    mcp_streamable_http_ping_error_missing_code
    mcp_streamable_http_ping_error_missing_message
    mcp_streamable_http_ping_error_empty
    mcp_streamable_http_sse_ping_error
    mcp_streamable_http_sse_ping_error_missing_code
    mcp_streamable_http_sse_ping_error_missing_message
    mcp_streamable_http_sse_ping_error_empty
    mcp_streamable_http_initialize_missing_capabilities
    mcp_streamable_http_initialize_capabilities_not_object
    mcp_streamable_http_initialize_server_info_metadata
    mcp_streamable_http_initialize_server_info_icons_not_array
    mcp_streamable_http_initialize_server_info_icon_missing_src
    mcp_streamable_http_initialize_server_info_icon_bad_theme
    mcp_streamable_http_initialize_server_info_description_not_string
    mcp_streamable_http_initialize_instructions_not_string
    mcp_streamable_http_initialize_missing_server_info
    mcp_streamable_http_initialize_missing_protocol_version
    mcp_streamable_http_initialize_unsupported_protocol_version
    mcp_streamable_http_initialize_empty_session_id
    mcp_streamable_http_initialize_space_session_id
    mcp_streamable_http_long_session_id_header
    mcp_streamable_http_initialize_response_id_mismatch
    mcp_streamable_http_initialize_error_id_mismatch
    mcp_streamable_http_response_id_mismatch
    mcp_streamable_http_error_response_id_mismatch
    mcp_streamable_http_response_invalid_id_type
    mcp_streamable_http_response_null_id
    mcp_streamable_http_sse_request_invalid_id_type
    mcp_streamable_http_sse_request_null_id
    mcp_streamable_http_response_jsonrpc_invalid
    mcp_streamable_http_sse_jsonrpc_invalid
    mcp_streamable_http_sse_method_with_result
    mcp_streamable_http_sse_response_missing_id
    mcp_streamable_http_sse_response_missing_result_error
    mcp_streamable_http_sse_response_result_and_error
    mcp_streamable_http_utility_notifications_ignore_malformed
    mcp_streamable_http_active_progress_missing_progress
    mcp_streamable_http_active_progress_invalid_progress
    mcp_streamable_http_active_progress_negative_progress
    mcp_streamable_http_active_progress_negative_total
    mcp_streamable_http_active_progress_equal_progress
    mcp_streamable_http_active_progress_decreasing_progress
    mcp_streamable_http_cancelled_notification_aborts_request
    mcp_streamable_http_sse_resume_get
    mcp_streamable_http_sse_resume_caps_retry
    mcp_streamable_http_sse_resume_rejects_unsafe_id
    mcp_streamable_http_streaming_sse_resume_get
    mcp_streamable_http_streaming_sse_resume_before_result_bytes
    mcp_streamable_http_ignores_sse_data_before_non_message_event
    mcp_streamable_http_sse_data_before_message_event
    mcp_streamable_http_response_content_type_parameters
    mcp_streamable_http_sse_content_type_parameters
    mcp_streamable_http_response_missing_result_error
    mcp_streamable_http_response_result_and_error
    mcp_streamable_http_response_null_result
    mcp_streamable_http_tools_list_missing_tools
    mcp_streamable_http_tools_list_missing_input_schema
    mcp_streamable_http_tools_list_null_input_schema
    mcp_streamable_http_tools_list_array_input_schema
    mcp_streamable_http_tools_list_missing_input_schema_type
    mcp_streamable_http_tools_list_string_input_schema_type
    mcp_streamable_http_tools_list_input_schema_array_properties
    mcp_streamable_http_tools_list_input_schema_array_schema
    mcp_streamable_http_tools_list_input_schema_nonobject_property
    mcp_streamable_http_tools_list_input_schema_object_required
    mcp_streamable_http_tools_list_input_schema_nonstring_required
    mcp_streamable_http_tools_list_input_schema_escaped_property
    mcp_streamable_http_tools_list_array_output_schema
    mcp_streamable_http_tools_list_string_output_schema_type
    mcp_streamable_http_tools_list_numeric_output_schema_type
    mcp_streamable_http_tools_list_output_schema_array_schema
    mcp_streamable_http_tools_list_output_schema_nonobject_property
    mcp_streamable_http_tools_list_array_annotations
    mcp_streamable_http_tools_list_annotation_hint_not_bool
    mcp_streamable_http_tools_list_object_icons
    mcp_streamable_http_tools_list_icon_missing_src
    mcp_streamable_http_tools_list_array_execution
    mcp_streamable_http_tools_list_execution_task_support_invalid
    mcp_streamable_http_resources_list_missing_resources
    mcp_streamable_http_resources_list_object_icons
    mcp_streamable_http_resources_list_icon_invalid_theme
    mcp_streamable_http_resources_list_array_annotations
    mcp_streamable_http_resources_list_annotation_invalid_role
    mcp_streamable_http_resources_list_annotation_priority_not_number
    mcp_streamable_http_resources_list_annotation_priority_out_of_range
    mcp_streamable_http_resources_list_negative_size
    mcp_streamable_http_resource_templates_list_missing_templates
    mcp_streamable_http_resource_templates_list_object_icons
    mcp_streamable_http_resource_templates_list_icon_size_not_string
    mcp_streamable_http_resource_templates_list_array_annotations
    mcp_streamable_http_resource_templates_list_annotation_invalid_role
    mcp_streamable_http_resource_templates_list_annotation_priority_out_of_range
    mcp_streamable_http_resource_templates_list_annotation_modified_not_string
    mcp_streamable_http_prompts_list_missing_prompts
    mcp_streamable_http_prompts_list_object_arguments
    mcp_streamable_http_prompts_list_argument_missing_name
    mcp_streamable_http_prompts_list_argument_required_not_bool
    mcp_streamable_http_prompts_list_object_icons
    mcp_streamable_http_prompts_list_icon_metadata
    mcp_streamable_http_tool_call_omits_null_arguments
    mcp_streamable_http_prompt_get_omits_null_arguments
    mcp_streamable_http_tool_call_missing_content
    mcp_streamable_http_tool_call_missing_content_type
    mcp_streamable_http_tool_call_text_missing_text
    mcp_streamable_http_tool_call_image_missing_mime
    mcp_streamable_http_tool_call_image_invalid_base64
    mcp_streamable_http_tool_call_audio_content
    mcp_streamable_http_tool_call_content_array_annotations
    mcp_streamable_http_tool_call_content_invalid_annotation_role
    mcp_streamable_http_tool_call_content_priority_out_of_range
    mcp_streamable_http_tool_call_unknown_content_type
    mcp_streamable_http_tool_call_resource_link_missing_name
    mcp_streamable_http_tool_call_resource_link_icons_not_array
    mcp_streamable_http_tool_call_resource_link_invalid_icon_theme
    mcp_streamable_http_tool_call_resource_link_negative_size
    mcp_streamable_http_tool_call_resource_missing_resource
    mcp_streamable_http_tool_call_resource_missing_uri
    mcp_streamable_http_tool_call_resource_missing_body
    mcp_streamable_http_tool_call_resource_text_and_blob
    mcp_streamable_http_tool_call_resource_invalid_blob
    mcp_streamable_http_tool_call_array_structured_content
    mcp_streamable_http_resource_read_missing_contents
    mcp_streamable_http_resource_read_missing_body
    mcp_streamable_http_resource_read_text_and_blob
    mcp_streamable_http_resource_read_blob_content
    mcp_streamable_http_resource_read_invalid_blob
    mcp_streamable_http_prompt_get_missing_messages
    mcp_streamable_http_prompt_get_invalid_role
    mcp_streamable_http_prompt_get_array_content
    mcp_streamable_http_prompt_get_text_missing_text
    mcp_streamable_http_prompt_get_content_priority_not_number
    mcp_streamable_http_prompt_get_content_priority_out_of_range
    mcp_streamable_http_prompt_get_audio_content
    mcp_streamable_http_prompt_get_audio_invalid_base64
    mcp_streamable_http_prompt_get_resource_missing_body
    mcp_streamable_http_prompt_get_resource_link_negative_size
    mcp_streamable_http_prompt_get_unknown_content_type
    mcp_streamable_http_completion_result
    mcp_streamable_http_completion_missing_values
    mcp_streamable_http_completion_non_string_value
    mcp_streamable_http_completion_too_many_values
    mcp_streamable_http_completion_negative_total
    mcp_streamable_http_completion_string_has_more
    mcp_streamable_http_tool_call_array_arguments
    mcp_streamable_http_prompt_get_array_arguments
    mcp_streamable_http_prompt_get_non_string_argument
    mcp_streamable_http_completion_array_context_arguments
    mcp_streamable_http_completion_non_string_context_argument
    mcp_streamable_http_completion_invalid_ref_type
    mcp_streamable_http_completion_resource_ref_uses_uri
    mcp_streamable_http_list_changed_invalidates_cache
    mcp_streamable_http_tools_list_changed_array_params
    mcp_streamable_http_resources_list_changed_array_params
    mcp_streamable_http_prompts_list_changed_array_params
    mcp_streamable_http_send_request_params
    mcp_streamable_http_send_request_scalar_result
    mcp_streamable_http_send_request_json_error
    mcp_streamable_http_send_request_sse_error
    mcp_streamable_http_send_request_sse_error_after_notification
    mcp_streamable_http_send_request_sse_error_missing_code
    mcp_streamable_http_send_request_sse_empty_message_event
    mcp_streamable_http_tool_call_writes_chunked_result
    mcp_streamable_http_tool_call_writes_sse_result
    mcp_streamable_http_resource_read_writes_chunked_result
    mcp_streamable_http_prompt_get_writes_chunked_result
    mcp_streamable_http_completion_writes_chunked_result
    mcp_streamable_http_send_request_writes_chunked_result
    mcp_streamable_http_writes_result_one_byte_chunks
    mcp_streamable_http_writes_sse_result_one_byte_chunks
    mcp_streamable_http_writes_default_sse_message_immediately
    mcp_streamable_http_resource_read_default_sse_message
    mcp_streamable_http_prompt_get_default_sse_message
    mcp_streamable_http_completion_default_sse_message
    mcp_streamable_http_send_request_default_sse_message
    mcp_streamable_http_rejects_streamed_text_plain_result
    mcp_streamable_http_writes_sse_result_after_notification
    mcp_streamable_http_resource_read_sse_result_after_notification
    mcp_streamable_http_prompt_get_sse_result_after_notification
    mcp_streamable_http_completion_sse_result_after_notification
    mcp_streamable_http_send_request_sse_result_after_notification
    mcp_streamable_http_writes_sse_result_after_server_ping_request
    mcp_streamable_http_resource_read_sse_result_after_server_ping_request
    mcp_streamable_http_prompt_get_sse_result_after_server_ping_request
    mcp_streamable_http_completion_sse_result_after_server_ping_request
    mcp_streamable_http_send_request_sse_result_after_server_ping_request
    mcp_streamable_http_writes_sse_result_after_empty_data_event
    mcp_streamable_http_result_sink_failure
    mcp_streamable_http_json_error_result_not_written
    mcp_streamable_http_sse_error_result_not_written
    mcp_streamable_http_result_rejects_mismatched_id
    mcp_streamable_http_result_before_mismatched_id
    mcp_streamable_http_result_rejects_invalid_jsonrpc
    mcp_streamable_http_result_rejects_method_with_result
    mcp_streamable_http_result_rejects_missing_id
    mcp_streamable_http_result_accepts_result_before_id
    mcp_streamable_http_result_field_order_variants
    mcp_streamable_http_result_rejects_duplicate_result
    mcp_streamable_http_result_before_id_rejects_duplicate_result
    mcp_streamable_http_sse_rejects_result_and_error
    mcp_streamable_http_sse_rejects_result_before_error
    mcp_streamable_http_result_rejects_trailing_garbage
    mcp_streamable_http_result_rejects_bad_literal
    mcp_streamable_http_result_rejects_bad_object
    mcp_streamable_http_tool_call_http_error
    mcp_streamable_http_resource_read_http_error
    mcp_streamable_http_prompt_get_http_error
    mcp_streamable_http_complete_http_error
    mcp_streamable_http_send_request_http_error
    mcp_streamable_http_send_notification_params
    mcp_streamable_http_notification_response_wrong_status
    mcp_streamable_http_notification_response_body
    mcp_streamable_http_notification_callback_error
    mcp_streamable_http_logging_notification_null_data
    mcp_streamable_http_logging_notification_missing_data
    mcp_streamable_http_logging_notification_invalid_level
    mcp_streamable_http_logging_notification_array_params
    mcp_streamable_http_resource_updated_notification
    mcp_streamable_http_resource_updated_missing_uri
    mcp_streamable_http_resource_updated_array_params
    mcp_streamable_http_server_ping_array_params
    mcp_streamable_http_server_request_unsupported
    mcp_streamable_http_server_ping_request
    mcp_streamable_http_session_recovery
    mcp_streamable_http_session_recovery_preserves_request_id
    mcp_streamable_http_session_recovery_chunked_result
    mcp_streamable_http_initialized_failure_retries
    mcp_streamable_http_server_error_no_recovery
    mcp_handler
    client_open
    mike_mind_prompt_contract
    response_json
    response_request_tools_cleanup
    response_spooled_request_arrays
    response_array_serialization_invariants
    response_large_text_parse
    response_large_instructions_parse
    response_large_tool_arguments_spooled
    http_create_response
    http_create_response_upload_setup_failure
    http_create_response_output_validation
    chatgpt_auth_refresh_retry
    chatgpt_auth_invalid_refresh_retryable
    chatgpt_auth_refresh_http_timeout
    chatgpt_auth_save_failure_preserves_file
    chatgpt_auth_expired_refreshes_before_request
    chatgpt_auth_reloads_changed_file_before_refresh
    chatgpt_auth_rejects_changed_file_account
    chatgpt_auth_stream_refresh_retry
    chatgpt_auth_default_path
    chatgpt_auth_blob_store
    chatgpt_auth_open_default_path
    chatgpt_auth_client_defaults_to_codex_backend
    chatgpt_auth_client_custom_allocator_owns_token
    chatgpt_auth_agent_uses_client_history
    chatgpt_login_authorize_url
    chatgpt_login_browser_helper
    chatgpt_login_callback_validation
    chatgpt_login_callback_exchange
    chatgpt_login_exchange_http_timeout
    chatgpt_login_default_path_write
    chatgpt_auth_rejects_partial_allocator
    http_retrieve_response
    http_cancel_response
    http_delete_response
    http_count_response_input_tokens
    http_list_response_input_items
    usage_accounting
    usage_limits
    usage_limit_lanes
    usage_client_limits
    usage_spend_limit
    usage_spend_limit_requires_pricing
    usage_stream_limits
    usage_stream_missing_usage_limit
    http_mock_incomplete_request_timeout
    http_error_details
    agent_session
    session_spooled_input_failure_ownership
    agent_client_history_continuity
    global_skills_catalog
    smith_profile
    smith_review_runtime
    smith_review_parent_handoff
    agent_runtime_subagent
    agent_runtime_subagent_mcp_snapshot
    agent_runtime_failed_subagent_resume
    agent_runtime_poll_only_subagent
    agent_runtime_review_subagent
    smith_review_pause_checkpoint_failure
    agent_runtime_lifecycle
    agent_runtime_controls
    agent_runtime_cancel_terminal
    agent_runtime_model_switch
    agent_runtime_model_switch_catalog_recovery
    agent_runtime_model_switch_live_catalog_outage
    agent_runtime_model_switch_api_key_metadata_policy
    agent_runtime_auto_compaction_boundaries
    agent_runtime_terminal_callback_close
    agent_runtime_queued_turns
    agent_runtime_semantic_events
    agent_runtime_patch_events
    agent_runtime_poll_only
    agent_runtime_host_goal_controls
    agent_runtime_goal_replace
    agent_runtime_goal_replace_queued_failure
    agent_runtime_goal_checkpoint_watermark
    agent_runtime_goal_create_status_allocation_failure
    agent_runtime_goal_budget
    agent_runtime_goal_usage_limit
    agent_local_session_store
    agent_runtime_explicit_local_store_opaque_id
    agent_local_session_store_scope_parent_sync
    agent_local_session_store_root_parent_sync
    agent_local_session_store_rejects_long_default_path
    agent_local_session_store_rejects_file_root
    agent_local_session_store_rejects_insecure_root
    agent_local_session_store_rejects_unsafe_scope
    agent_runtime_resume
    agent_runtime_markdown_export
    goal_tools
    goal_create_allocation_failure
    patch_tool
    agent_tool_declarations
    agent_tool_manual_step
    agent_auto_compaction
    http_response_limit
    agent_tool_auto_run
    agent_custom_tool_auto_run
    agent_custom_tool_client_history_auto_run
    agent_client_history_tool_auto_run
    agent_client_history_tool_callback_durability
    agent_view_image_auto_run
    revgeo_tool
    todo_file_store_initializes_under_lock
    todo_tool
    todo_callback_store
    searxng_registry_tool
    terminal_output_limit_clamp
    terminal_registration_rollback
    terminal_tools
    terminal_buffered_output_returns_immediately
    terminal_sink_failure_preserves_output
    terminal_capture_truncation
    terminal_reader_start_failure
    terminal_external_child_reap
    terminal_closes_inherited_fds
    terminal_concurrent_start
    terminal_concurrent_operation
    exec_tool
    read_tool
    view_image_tool
    agent_searxng_tool_auto_run
    agent_multi_tool_auto_run
    agent_tool_auto_round_limit
    agent_tool_auto_default_unbounded
    agent_tool_error_output_continues
    agent_tool_output_max_bytes
    conversations
    stream_response_text
    provider_model_metadata
    chat_client_compaction
    chat_compaction_failure_preserves_history
    chat_compaction_tiny_context
    chat_parallel_tool_roundtrip
    berget_live_compaction
    berget_live_vision
    berget_live_canaries
    chat_length_continuation
    chat_length_unfinished_tool
    chat_runtime_overflow_recovery
    chat_runtime_recovery_failure_modes
    chat_runtime_overflow_variants
    chat_stream_failure_modes
    chat_connection_failure
    chat_logging
    codex_catalog_large_fields
    codex_catalog_validation
    model_catalog_logging
    model_catalog_outage_recovery
    chat_transient_retries
    chat_request_metrics
    chat_transport_failure_modes
    chat_failed_tool_stream_preserves_session
    chat_completions_adapter
    stream_compaction_progress_callback
    stream_upload_setup_failure
    stream_responses_websocket
    stream_responses_websocket_large_frame
    stream_responses_websocket_transient_retry
    stream_responses_websocket_midstream_drop
    stream_responses_websocket_https_fallback
    stream_responses_websocket_upgrade_failure_no_fallback
    stream_responses_websocket_no_fallback_after_output
    stream_responses_websocket_no_fallback_after_sink_failure
    stream_responses_websocket_fallback_disabled_before_output
    stream_responses_websocket_https_fallback_disabled
    stream_responses_websocket_fragmented_event
    session_stream_responses_websocket_multi_turn
    session_stream_responses_websocket_stale_reconnect
    stream_non_function_output_item
    stream_output_delta_failure
    stream_output_item_failure
    stream_output_suffix_segments
    stream_provider_failure_details
    stream_http_error_preserves_openai_error
    stream_source_error_preserves_openai_error
    stream_openrouter_metadata_events
    session_stream_auto_tool_run
    session_stream_auto_custom_empty_tool_run
    session_stream_auto_tool_error_output_continues
    session_stream_auto_source_tool_run
    session_stream_auto_reasoning_tool_response
    session_stream_auto_multi_tool_run
    session_stream_auto_duplicate_tool_done
    session_stream_auto_callback_failure
    session_stream_auto_round_limit
    session_stream_auto_tool_output_max_bytes
    stream_sse_event_limit
    stream_large_instructions_field
    stream_large_content_part_done_field
    stream_history_preserves_pretty_json
    stream_client_history_captures_output
    stream_client_history_tool_order
    local_history_opt_in
    session_resume_and_history_import
    session_state_validation)
  string(REPLACE ";" "\n" _cai_unit_test_groups_text
    "${CAI_UNIT_TEST_GROUPS}")
  file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/cai_unit_test_groups.txt"
    CONTENT "${_cai_unit_test_groups_text}\n")
  foreach(_cai_unit_test IN LISTS CAI_UNIT_TEST_GROUPS)
    add_test(NAME cai_tests.${_cai_unit_test}
      COMMAND cai_tests --only ${_cai_unit_test})
    set_tests_properties(cai_tests.${_cai_unit_test} PROPERTIES
      LABELS "unit;local"
      TIMEOUT 3)
  endforeach()
  set_tests_properties(cai_tests.mcp_raw_input_cancellation PROPERTIES TIMEOUT 12)
  # This test intentionally exercises two queued network-backed turns. Its
  # internal HTTP and pump bounds exceed the default short unit-test timeout.
  set_tests_properties(cai_tests.agent_runtime_queued_turns PROPERTIES
    TIMEOUT 30)
  set_tests_properties(cai_tests.berget_live_compaction
    cai_tests.berget_live_vision PROPERTIES
    LABELS "integration;live"
    TIMEOUT 180)
  set_tests_properties(cai_tests.chat_transient_retries PROPERTIES TIMEOUT 15)
  set_tests_properties(cai_tests.berget_live_canaries PROPERTIES
    LABELS "integration;live" TIMEOUT 900)
  set_tests_properties(cai_tests.lonejson_selected_array_rewrite PROPERTIES
    LABELS "unit;local;memory-external-boundary"
    TIMEOUT 3)
  set_tests_properties(
    cai_tests.model_capabilities
    cai_tests.env_precedence
    cai_tests.source_sink
    cai_tests.lonejson_nested_mapped_array_stream
    cai_tests.client_open
    cai_tests.mike_mind_prompt_contract
    PROPERTIES
      LABELS "unit;local;valgrind-smoke"
      TIMEOUT 3)
  set_tests_properties(cai_tests.mcp_handler PROPERTIES
    LABELS "unit;local;memory-external-boundary"
    TIMEOUT 3)
  set_tests_properties(cai_tests.response_json PROPERTIES
    LABELS "unit;local;memory-external-boundary"
    TIMEOUT 3)
  set_tests_properties(cai_tests.response_spooled_request_arrays PROPERTIES
    LABELS "unit;local;memory-external-boundary"
    TIMEOUT 3)
  set_tests_properties(cai_tests.response_array_serialization_invariants PROPERTIES
    LABELS "unit;local;memory-external-boundary"
    TIMEOUT 3)
  set_tests_properties(cai_tests.tool_registry PROPERTIES
    LABELS "unit;local;memory-external-boundary"
    TIMEOUT 3)
  add_test(NAME cai_tests.registration
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit_test_registration_test.sh"
      $<TARGET_FILE:cai_tests>
      "${CMAKE_CURRENT_BINARY_DIR}/cai_unit_test_groups.txt")
  set_tests_properties(cai_tests.registration PROPERTIES
    LABELS "unit;local"
    TIMEOUT 3)
  add_test(NAME cai_tests.registration_script
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit_test_registration_script_test.sh"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit_test_registration_test.sh")
  set_tests_properties(cai_tests.registration_script PROPERTIES
    LABELS "unit;local"
    TIMEOUT 3)
  if(CAI_ENABLE_VALGRIND_TESTS)
    add_test(NAME cai_tests.valgrind
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_valgrind.sh"
        $<TARGET_FILE:cai_tests>
        model_capabilities
        env_precedence
        source_sink
        lonejson_nested_mapped_array_stream
        client_open
        mike_mind_prompt_contract
        patch_tool)
    set_tests_properties(cai_tests.valgrind PROPERTIES
      LABELS "valgrind;local;memory-check"
      TIMEOUT 180)
  endif()
  add_test(NAME cai_receiver_usage_test
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/receiver_usage_test.sh"
      "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(cai_receiver_usage_test PROPERTIES
    LABELS "unit;local"
    TIMEOUT 3)
  add_test(NAME cai_build_toolchain_change_test
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_toolchain_change_test.sh"
      "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(cai_build_toolchain_change_test PROPERTIES
    LABELS "local;packaging" TIMEOUT 10)
  add_test(NAME cai_lifecycle_execution_test
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/lifecycle_execution_test.sh"
      "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(cai_lifecycle_execution_test PROPERTIES
    LABELS "local;packaging" TIMEOUT 30)
  add_test(NAME cai_makefile_targets_test
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/makefile_targets_test.sh"
      "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(cai_makefile_targets_test PROPERTIES
    LABELS "unit;local"
    TIMEOUT 3)
  add_test(NAME cai_devenv_test
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/devenv_test.sh"
      "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(cai_devenv_test PROPERTIES
    LABELS "unit;local" TIMEOUT 30)
  add_test(NAME cai_release_live_gate_test
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_live_gate_test.sh"
      "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(cai_release_live_gate_test PROPERTIES
    LABELS "unit;local;packaging"
    TIMEOUT 3)
  add_test(NAME cai_mcp_client_source_contract_test
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_client_source_contract_test.sh"
      "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(cai_mcp_client_source_contract_test PROPERTIES
    LABELS "unit;local"
    TIMEOUT 3)
  add_test(NAME cai_smith_prompt_asset_test
    COMMAND "${CMAKE_COMMAND}"
      "-DCAI_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DCAI_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/smith_prompt_asset_test.cmake")
  set_tests_properties(cai_smith_prompt_asset_test PROPERTIES
    LABELS "unit;local"
    TIMEOUT 3)
  add_test(NAME cai_byte_asset_generator_test
    COMMAND "${CMAKE_COMMAND}"
      "-DCAI_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DCAI_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/byte_asset_generator_test.cmake")
  set_tests_properties(cai_byte_asset_generator_test PROPERTIES
    LABELS "unit;local"
    TIMEOUT 3)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
     CMAKE_SYSTEM_NAME STREQUAL CMAKE_HOST_SYSTEM_NAME AND
     CMAKE_SYSTEM_PROCESSOR STREQUAL CMAKE_HOST_SYSTEM_PROCESSOR)
    add_test(NAME cai_export_policy_incremental_test
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/export_policy_incremental_test.sh"
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}"
        "${CMAKE_C_COMPILER}" "${CMAKE_NM}" "${CMAKE_COMMAND}")
    set_tests_properties(cai_export_policy_incremental_test PROPERTIES
      LABELS "unit;local" TIMEOUT 15)
  endif()
  if(CAI_BUILD_SHARED)
    string(TOLOWER "${CMAKE_SYSTEM_NAME}" _cai_export_policy_platform)
    set(_cai_shared_export_policy_test_args
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/shared_export_policy_test.sh"
      "${CMAKE_NM}"
      "$<TARGET_FILE:cai_shared>"
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cai_shared.exports"
      "${_cai_export_policy_platform}")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      list(APPEND _cai_shared_export_policy_test_args "${CMAKE_C_COMPILER}")
    endif()
    add_test(NAME cai_shared_export_policy_test
      COMMAND ${_cai_shared_export_policy_test_args})
    set_tests_properties(cai_shared_export_policy_test PROPERTIES
      LABELS "unit;local;packaging"
      TIMEOUT 5)
    unset(_cai_shared_export_policy_test_args)
    unset(_cai_export_policy_platform)
  endif()

  add_executable(cai_mcp_http_server tests/mcp_http_server.c)
  target_include_directories(cai_mcp_http_server PRIVATE
    "${CAI_PUBLIC_INCLUDE_DIR}"
    "${CAI_GENERATED_INCLUDE_DIR}"
    "${CAI_LONEJSON_INCLUDE_DIR}")
  cai_link_internal_consumer(cai_mcp_http_server)
  cai_configure_c_target(cai_mcp_http_server)

  add_executable(cai_mcp_http_smoke tests/mcp_http_smoke.c)
  cai_configure_c_target(cai_mcp_http_smoke)
  add_test(NAME cai_mcp_http_smoke
    COMMAND cai_mcp_http_smoke $<TARGET_FILE:cai_mcp_http_server>)
  set_tests_properties(cai_mcp_http_smoke PROPERTIES
    LABELS "smoke;local"
    TIMEOUT 3)
  add_executable(cai_mcp_everything_e2e tests/mcp_everything_e2e.c)
  target_include_directories(cai_mcp_everything_e2e PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  cai_link_internal_consumer(cai_mcp_everything_e2e)
  cai_configure_c_target(cai_mcp_everything_e2e)
  add_executable(cai_mcp_client_e2e_driver tests/mcp_client_e2e.c)
  target_include_directories(cai_mcp_client_e2e_driver PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  cai_link_internal_consumer(cai_mcp_client_e2e_driver)
  cai_configure_c_target(cai_mcp_client_e2e_driver)
  add_test(NAME cai_mcp_client_e2e
    COMMAND /bin/sh
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_client_e2e.sh"
      $<TARGET_FILE:cai_mcp_http_server>
      $<TARGET_FILE:cai_mcp_client_e2e_driver>)
  set_tests_properties(cai_mcp_client_e2e PROPERTIES
    LABELS "e2e;local"
    TIMEOUT 5)
  set(_cai_examples_help_smoke_args
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/examples_help_smoke.sh"
    "${CMAKE_CURRENT_SOURCE_DIR}")
  if(TARGET cai_example_terminal_chat)
    list(APPEND _cai_examples_help_smoke_args
      $<TARGET_FILE:cai_example_terminal_chat>)
  endif()
  if(TARGET cai_example_chatgpt_login)
    list(APPEND _cai_examples_help_smoke_args
      $<TARGET_FILE:cai_example_chatgpt_login>)
  endif()
  if(TARGET cai_example_smith_terminal)
    list(APPEND _cai_examples_help_smoke_args
      $<TARGET_FILE:cai_example_smith_terminal>)
  endif()
  add_test(NAME cai_examples_help_smoke
    COMMAND ${_cai_examples_help_smoke_args})
  set_tests_properties(cai_examples_help_smoke PROPERTIES
    LABELS "smoke;local;example-smoke"
    TIMEOUT 3)
  add_executable(cai_mcp_example_smoke tests/mcp_example_smoke.c)
  cai_configure_c_target(cai_mcp_example_smoke)
  if(TARGET cai_example_mcp_server)
    add_test(NAME cai_mcp_example_smoke
      COMMAND cai_mcp_example_smoke $<TARGET_FILE:cai_example_mcp_server>)
    set_tests_properties(cai_mcp_example_smoke PROPERTIES
      LABELS "smoke;local;example-smoke"
      TIMEOUT 3)
  endif()
  if("$ENV{CAI_MCP_INSPECTOR_E2E}" STREQUAL "1")
    add_test(NAME cai_mcp_inspector_e2e
    COMMAND /bin/bash
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_inspector_e2e.sh"
      $<TARGET_FILE:cai_mcp_http_server>)
  set_tests_properties(cai_mcp_inspector_e2e PROPERTIES
    LABELS "smoke;local"
      TIMEOUT 180)
  endif()

  if(CAI_INSTALL)
    add_test(NAME cai_install_metadata_test
      COMMAND "${CMAKE_COMMAND}"
        "-DCAI_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        "-DCAI_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DCAI_VERSION=${CAI_PACKAGE_VERSION}"
        "-DCAI_BUILD_SHARED=${CAI_BUILD_SHARED}"
        "-DCAI_BUILD_STATIC=${CAI_BUILD_STATIC}"
        "-DCAI_C_PKT_SYSTEMS_PREFIX=${CAI_C_PKT_SYSTEMS_PREFIX}"
        "-DCAI_LONEJSON_PREFIX=${CAI_LONEJSON_PREFIX}"
        "-DCAI_PSLOG_PREFIX=${CAI_PSLOG_PREFIX}"
        "-DCAI_RESOLVED_DEPENDENCY_MODE=${CAI_RESOLVED_DEPENDENCY_MODE}"
        "-DCAI_DEPS_DIR=${CAI_DEPS_DIR}"
        "-DCAI_LONEJSON_INCLUDE_DIR=${CAI_LONEJSON_INCLUDE_DIR}"
        "-DCAI_LONEJSON_LIBRARY=${CAI_LONEJSON_LIBRARY}"
        "-DCAI_PSLOG_INCLUDE_DIR=${CAI_PSLOG_INCLUDE_DIR}"
        "-DCAI_PSLOG_LIBRARY=${CAI_PSLOG_LIBRARY}"
        "-DCAI_PARENT_CMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
        "-DCAI_GENERATOR=${CMAKE_GENERATOR}"
        "-DCAI_C_COMPILER=${CMAKE_C_COMPILER}"
        "-DCAI_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
        "-DCAI_CPKT_TARGET_ID=${CPKT_TARGET_ID}"
        "-DCAI_READELF=${CMAKE_READELF}"
        "-DCAI_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
        "-DCAI_TARGET_ID=${CAI_TARGET_ID}"
        "-DCAI_INSTALL_RPATH_TOKEN=${CAI_INSTALL_RPATH_TOKEN}"
        "-DCAI_ABI_VERSION=${CAI_ABI_VERSION}"
        "-DCAI_LONEJSON_ABI_VERSION=${CAI_LONEJSON_ABI_VERSION}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/install_metadata_test.cmake")
    set_tests_properties(cai_install_metadata_test PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 30)
    set(CAI_HOST_DEPENDENCY_MODE_TEST_ARGS
      "-DCAI_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DCAI_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
      "-DCAI_DEPS_DIR=${CAI_DEPS_DIR}"
      "-DCAI_C_PKT_SYSTEMS_PREFIX=${CAI_C_PKT_SYSTEMS_PREFIX}"
      "-DCAI_C_PKT_SYSTEMS_VERSION=${CAI_C_PKT_SYSTEMS_VERSION}"
      "-DCAI_LONEJSON_PREFIX=${CAI_LONEJSON_PREFIX}"
      "-DCAI_PSLOG_PREFIX=${CAI_PSLOG_PREFIX}"
      "-DCAI_LONEJSON_INCLUDE_DIR=${CAI_LONEJSON_INCLUDE_DIR}"
      "-DCAI_LONEJSON_LIBRARY=${CAI_LONEJSON_LIBRARY}"
      "-DCAI_PSLOG_INCLUDE_DIR=${CAI_PSLOG_INCLUDE_DIR}"
      "-DCAI_PSLOG_LIBRARY=${CAI_PSLOG_LIBRARY}"
      "-DCAI_PARENT_CMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
      "-DCAI_C_COMPILER=${CMAKE_C_COMPILER}"
      "-DCAI_LONEJSON_ABI_VERSION=${CAI_LONEJSON_ABI_VERSION}")
    add_test(NAME cai_host_dependency_mode_unsupported_target
      COMMAND "${CMAKE_COMMAND}"
        ${CAI_HOST_DEPENDENCY_MODE_TEST_ARGS}
        "-DCAI_GENERATOR=${CMAKE_GENERATOR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/host_dependency_mode_test.cmake")
    set_tests_properties(cai_host_dependency_mode_unsupported_target PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 10)
    add_test(NAME cai_host_dependency_mode_unsupported_target_makefiles
      COMMAND "${CMAKE_COMMAND}"
        ${CAI_HOST_DEPENDENCY_MODE_TEST_ARGS}
        "-DCAI_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/host-dependency-mode-makefiles-parent"
        "-DCAI_GENERATOR=Unix Makefiles"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/host_dependency_mode_test.cmake")
    set_tests_properties(cai_host_dependency_mode_unsupported_target_makefiles PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 10)
    add_test(NAME cai_rpath_token_test
      COMMAND "${CMAKE_COMMAND}"
        "-DCAI_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/rpath_token_test.cmake")
    set_tests_properties(cai_rpath_token_test PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 3)
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND
       CAI_TARGET_ID STREQUAL "x86_64-linux-gnu")
      cai_local_dependency_runtime_dirs(_cai_test_runtime_dirs)
      add_test(NAME cai_local_bootlin_runtime_test
        COMMAND "${CMAKE_COMMAND}"
          "-DCAI_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
          "-DCAI_READELF=${CMAKE_READELF}"
          "-DCAI_SYSROOT=${CMAKE_SYSROOT}"
          "-DCAI_DEPENDENCY_RUNTIME_DIRS=${_cai_test_runtime_dirs}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/local_bootlin_runtime_test.cmake")
      set_tests_properties(cai_local_bootlin_runtime_test PROPERTIES
        LABELS "packaging;local"
        TIMEOUT 3)
      if(CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "cpkt")
        add_test(NAME cai_host_dependency_runtime_test
          COMMAND "${CMAKE_COMMAND}"
            "-DCAI_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DCAI_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            "-DCAI_PREFIXES=${CAI_C_PKT_SYSTEMS_PREFIX};${CAI_LONEJSON_PREFIX};${CAI_PSLOG_PREFIX}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/host_dependency_runtime_test.cmake")
        set_tests_properties(cai_host_dependency_runtime_test PROPERTIES
          LABELS "packaging;local" TIMEOUT 120)
      endif()
    endif()
    add_test(NAME cai_target_tool_discovery_test
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/target_tool_discovery_test.sh"
        "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(cai_target_tool_discovery_test PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 3)
    add_test(NAME cai_darwin_linker_route_test
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/darwin_linker_route_test.sh"
        "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(cai_darwin_linker_route_test PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 3)
    add_test(NAME cai_release_artifacts_darwin_metadata_test
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_artifacts_darwin_metadata_test.sh"
        "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(cai_release_artifacts_darwin_metadata_test PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 3)
    add_test(NAME cai_release_export_policy_test
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_export_policy_test.sh"
        "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(cai_release_export_policy_test PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 3)
    add_test(NAME cai_release_manifest_gate_test
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_manifest_gate_test.sh"
        "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(cai_release_manifest_gate_test PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 3)
    add_test(NAME cai_release_toolchain_cache_guard_test
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_toolchain_cache_guard_test.sh"
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(cai_release_toolchain_cache_guard_test PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 3)
    add_test(NAME cai_release_staging_safety_test
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_staging_safety_test.sh"
        "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(cai_release_staging_safety_test PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 3)
    add_test(NAME cai_cmake_presets_test
      COMMAND "${CMAKE_COMMAND}"
        "-DCAI_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DCAI_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake_presets_test.cmake")
    set_tests_properties(cai_cmake_presets_test PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 15)
    if(CAI_RESOLVED_DEPENDENCY_MODE STREQUAL "cpkt")
      add_test(NAME cai_cpkt_link_targets_test
        COMMAND "${CMAKE_COMMAND}"
          "-DCAI_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
          "-DCAI_C_PKT_SYSTEMS_PREFIX=${CAI_C_PKT_SYSTEMS_PREFIX}"
          "-DCAI_TARGET_ID=${CAI_TARGET_ID}"
          "-DCAI_BUILD_SHARED=${CAI_BUILD_SHARED}"
          "-DCAI_BUILD_STATIC=${CAI_BUILD_STATIC}"
          "-DCAI_BUILD_LUA=${CAI_BUILD_LUA}"
          "-DCAI_BUILD_EXAMPLES=${CAI_BUILD_EXAMPLES}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cpkt_link_targets_test.cmake")
      set_tests_properties(cai_cpkt_link_targets_test PROPERTIES
        LABELS "packaging;local"
        TIMEOUT 3)
    endif()
    set(CAI_NESTED_CONFIGURE_TOOLCHAIN_ARGS "")
    if(DEFINED CMAKE_TOOLCHAIN_FILE AND NOT CMAKE_TOOLCHAIN_FILE STREQUAL "")
      list(APPEND CAI_NESTED_CONFIGURE_TOOLCHAIN_ARGS
        "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}")
    endif()
    if(DEFINED CPKT_TARGET_ID AND NOT CPKT_TARGET_ID STREQUAL "")
      list(APPEND CAI_NESTED_CONFIGURE_TOOLCHAIN_ARGS
        "-DCPKT_TARGET_ID=${CPKT_TARGET_ID}")
    endif()
    if(DEFINED CAI_TARGET_ID AND NOT CAI_TARGET_ID STREQUAL "")
      list(APPEND CAI_NESTED_CONFIGURE_TOOLCHAIN_ARGS
        "-DCAI_TARGET_ID=${CAI_TARGET_ID}")
    endif()

    add_test(NAME cai_c_integration_config_without_lua
      COMMAND "${CMAKE_COMMAND}"
        -S "${CMAKE_CURRENT_SOURCE_DIR}"
        -B "${CMAKE_CURRENT_BINARY_DIR}/c-integration-without-lua"
        "-G${CMAKE_GENERATOR}"
        ${CAI_NESTED_CONFIGURE_TOOLCHAIN_ARGS}
        "-DCAI_BUILD_INTEGRATION_TESTS=ON"
        "-DCAI_BUILD_LUA=OFF"
        "-DCAI_BUILD_EXAMPLES=OFF"
        "-DCAI_BUILD_FUZZERS=OFF"
        "-DCAI_BUILD_TESTS=ON"
        "-DCAI_INSTALL=OFF"
        "-DCAI_DEPENDENCY_MODE=${CAI_RESOLVED_DEPENDENCY_MODE}"
        "-DCAI_DEPS_DIR=${CAI_DEPS_DIR}"
        "-DCAI_C_PKT_SYSTEMS_PREFIX=${CAI_C_PKT_SYSTEMS_PREFIX}"
        "-DCAI_LONEJSON_PREFIX=${CAI_LONEJSON_PREFIX}"
        "-DCAI_PSLOG_PREFIX=${CAI_PSLOG_PREFIX}"
        "-DCAI_LONEJSON_INCLUDE_DIR=${CAI_LONEJSON_INCLUDE_DIR}"
        "-DCAI_LONEJSON_LIBRARY=${CAI_LONEJSON_LIBRARY}"
        "-DCAI_PSLOG_INCLUDE_DIR=${CAI_PSLOG_INCLUDE_DIR}"
        "-DCAI_PSLOG_LIBRARY=${CAI_PSLOG_LIBRARY}"
        "-DCAI_LUA_EXECUTABLE=CAI_LUA_EXECUTABLE-NOTFOUND"
        "-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
        "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
        "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
    set_tests_properties(cai_c_integration_config_without_lua PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 10)
    add_test(NAME cai_api_key_example_smoke_without_lua_config
      COMMAND "${CMAKE_COMMAND}"
        "-DCAI_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DCAI_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/api-key-example-smoke-without-lua"
        "-DCAI_GENERATOR=${CMAKE_GENERATOR}"
        "-DCAI_DEPENDENCY_MODE=${CAI_RESOLVED_DEPENDENCY_MODE}"
        "-DCAI_DEPS_DIR=${CAI_DEPS_DIR}"
        "-DCAI_C_PKT_SYSTEMS_PREFIX=${CAI_C_PKT_SYSTEMS_PREFIX}"
        "-DCAI_LONEJSON_PREFIX=${CAI_LONEJSON_PREFIX}"
        "-DCAI_PSLOG_PREFIX=${CAI_PSLOG_PREFIX}"
        "-DCAI_LONEJSON_INCLUDE_DIR=${CAI_LONEJSON_INCLUDE_DIR}"
        "-DCAI_LONEJSON_LIBRARY=${CAI_LONEJSON_LIBRARY}"
        "-DCAI_PSLOG_INCLUDE_DIR=${CAI_PSLOG_INCLUDE_DIR}"
        "-DCAI_PSLOG_LIBRARY=${CAI_PSLOG_LIBRARY}"
        "-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
        "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
        "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
        "-DCAI_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
        "-DCPKT_TARGET_ID=${CPKT_TARGET_ID}"
        "-DCAI_TARGET_ID=${CAI_TARGET_ID}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/api_key_example_smoke_registration_test.cmake")
    set_tests_properties(cai_api_key_example_smoke_without_lua_config PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 60)
    add_test(NAME cai_release_lua_version_test
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_lua_version_test.sh"
        "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(cai_release_lua_version_test PROPERTIES
      LABELS "packaging;local"
      TIMEOUT 10)
  endif()

  if(CAI_BUILD_LUA)
    string(TOLOWER "${CMAKE_SYSTEM_NAME}" _cai_lua_export_policy_platform)
    add_test(NAME cai_lua_export_policy_test
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/shared_export_policy_test.sh"
        "${CMAKE_NM}"
        "$<TARGET_FILE:cai_lua>"
        "${CMAKE_CURRENT_SOURCE_DIR}/lua/cai_lua.exports"
        "${_cai_lua_export_policy_platform}")
    set_tests_properties(cai_lua_export_policy_test PROPERTIES
      LABELS "unit;local;packaging"
      TIMEOUT 5)
    unset(_cai_lua_export_policy_platform)
    add_test(NAME cai_lua_runner_test
      COMMAND "${CMAKE_COMMAND}"
        "-DCAI_LUA_RUNNER=$<TARGET_FILE:cai_lua_runner>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/runner_test.cmake")
    set_tests_properties(cai_lua_runner_test PROPERTIES LABELS "unit;local" TIMEOUT 3)
    add_test(NAME cai_lua_tests
      COMMAND "${CAI_LUA_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/test_lua.lua"
        "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(cai_lua_tests PROPERTIES
      LABELS "unit;local"
      ENVIRONMENT "${CAI_LUA_TEST_ENV}"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT 3)
    add_test(NAME cai_lua_chrome
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/chrome_test.sh"
        lua "${CAI_LUA_EXECUTABLE}" "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(cai_lua_chrome PROPERTIES
      LABELS "integration;local;offline" ENVIRONMENT "${CAI_LUA_TEST_ENV}" TIMEOUT 30)
    add_test(NAME cai_lua_mcp_todo_e2e
      COMMAND "${CAI_LUA_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/e2e_mcp_todo.lua")
    set_tests_properties(cai_lua_mcp_todo_e2e PROPERTIES
      LABELS "smoke;local"
      ENVIRONMENT "${CAI_LUA_TEST_ENV}"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT 3)
    add_test(NAME cai_lua_mcp_client_e2e
      COMMAND /bin/sh
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua_mcp_client_e2e.sh"
        $<TARGET_FILE:cai_mcp_http_server>
        "${CAI_LUA_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/e2e_mcp_client.lua")
    set_tests_properties(cai_lua_mcp_client_e2e PROPERTIES
      LABELS "smoke;local"
      ENVIRONMENT "${CAI_LUA_TEST_ENV}"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT 20)
  endif()

  if(CAI_BUILD_INTEGRATION_TESTS)
    set(CAI_INTEGRATION_TIMEOUT_SHORT 10)
    set(CAI_INTEGRATION_TIMEOUT_MEDIUM 30)
    set(CAI_INTEGRATION_TIMEOUT_LONG 240)
    # Smith review has separate coding, review, and handoff turns (240 + 180
    # + 240 seconds) and needs cleanup headroom beyond one long turn.
    set(CAI_INTEGRATION_TIMEOUT_SMITH_REVIEW 720)
    add_executable(cai_integration_tests tests/integration_main.c)
    target_include_directories(cai_integration_tests PRIVATE
      "${CAI_PUBLIC_INCLUDE_DIR}"
      "${CAI_GENERATED_INCLUDE_DIR}"
      "${CAI_LONEJSON_INCLUDE_DIR}"
      "${CAI_PSLOG_INCLUDE_DIR}"
      ${CURL_INCLUDE_DIRS})
    cai_link_internal_consumer(cai_integration_tests)
    cai_configure_c_target(cai_integration_tests)
    add_test(NAME cai_integration_tests COMMAND cai_integration_tests)
    set_tests_properties(cai_integration_tests PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_todo_workflow
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_TODO_WORKFLOW=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_todo_workflow PROPERTIES
      LABELS "integration;offline"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_SHORT})
    add_test(NAME cai_integration_chatgpt_e2e
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_E2E=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_e2e PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_LONG})
    add_test(NAME cai_integration_chatgpt_astra_smoke
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_ASTRA_SMOKE=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_astra_smoke PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_chatgpt_auth_rejection
      COMMAND /bin/sh
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/chatgpt_auth_failure_test.sh"
        c
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_auth_rejection PROPERTIES
      LABELS "integration;offline"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_SHORT})
    add_test(NAME cai_integration_chatgpt_auth_ownership
      COMMAND /bin/sh
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/chatgpt_auth_failure_test.sh"
        ownership-c
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_auth_ownership PROPERTIES
      LABELS "integration;offline"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_SHORT})
    add_test(NAME cai_integration_chatgpt_usage_limits
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_USAGE_LIMITS=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_usage_limits PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_chatgpt_defaults
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_DEFAULTS=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_defaults PROPERTIES
      LABELS "integration;offline"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_SHORT})
    add_test(NAME cai_integration_provider_retry_classifier
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_PROVIDER_RETRY_CLASSIFIER=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_provider_retry_classifier
      PROPERTIES
      LABELS "integration;offline"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_SHORT})
    add_test(NAME cai_integration_chatgpt_subscription_e2e
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_SUBSCRIPTION_E2E=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_subscription_e2e PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_LONG})
    add_test(NAME cai_integration_chatgpt_smith_e2e
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_SMITH_E2E=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_smith_e2e PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_LONG})
    add_test(NAME cai_integration_chatgpt_smith_goal_e2e
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_SMITH_GOAL_E2E=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_smith_goal_e2e PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_LONG})
    add_test(NAME cai_integration_chatgpt_smith_subagents_e2e
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_SMITH_SUBAGENTS_E2E=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_smith_subagents_e2e PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_SMITH_REVIEW})
    add_test(NAME cai_integration_chatgpt_smith_review_e2e
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_SMITH_REVIEW_E2E=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_smith_review_e2e PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_SMITH_REVIEW})
    add_test(NAME cai_integration_chatgpt_smith_mcp_review_e2e
      COMMAND /bin/sh
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/smith_mcp_live_e2e.sh"
        $<TARGET_FILE:cai_mcp_http_server>
        "${CMAKE_COMMAND}" -E env
          "CAI_INTEGRATION_CHATGPT_SMITH_MCP_REVIEW_E2E=1"
          $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_smith_mcp_review_e2e PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_SMITH_REVIEW})
    add_test(NAME cai_integration_chatgpt_state_restore
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_STATE_RESTORE=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_state_restore PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_chatgpt_compaction
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_COMPACTION=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_compaction PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_chatgpt_tool_security
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_TOOL_SECURITY=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_tool_security PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_chatgpt_hosted_web_search
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_HOSTED_WEB_SEARCH=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_hosted_web_search PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_LONG})
    add_test(NAME cai_integration_chatgpt_mcp_client_tool
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_mcp_everything_live_test.sh"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_mcp_client_tool PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_LONG})
    add_test(NAME cai_integration_chatgpt_responses_websocket_e2e
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_RESPONSES_WEBSOCKET_E2E=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_responses_websocket_e2e PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_LONG})
    add_test(NAME cai_integration_chatgpt_exec_tool
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_EXEC_TOOL=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_exec_tool PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_chatgpt_read_tool
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_CHATGPT_READ_TOOL=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_chatgpt_read_tool PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_openrouter_basic
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_OPENROUTER=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_openrouter_basic PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_openrouter_e2e
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_OPENROUTER_E2E=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_openrouter_e2e PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_LONG})
    add_test(NAME cai_integration_openrouter_session
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_OPENROUTER_SESSION=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_openrouter_session PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_openrouter_tool
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_OPENROUTER_TOOL=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_openrouter_tool PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_openrouter_stream_tool
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_OPENROUTER_STREAM_TOOL=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_openrouter_stream_tool PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_openrouter_read_tool
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_OPENROUTER_READ_TOOL=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_openrouter_read_tool PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_openrouter_stream_history
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_OPENROUTER_STREAM_HISTORY=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_openrouter_stream_history PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    add_test(NAME cai_integration_openrouter_tool_security
      COMMAND "${CMAKE_COMMAND}" -E env
        "CAI_INTEGRATION_OPENROUTER_TOOL_SECURITY=1"
        $<TARGET_FILE:cai_integration_tests>)
    set_tests_properties(cai_integration_openrouter_tool_security PROPERTIES
      LABELS integration
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    if(CAI_BUILD_LUA)
      add_test(NAME cai_lua_chatgpt_auth_rejection
        COMMAND /bin/sh
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/chatgpt_auth_failure_test.sh"
          lua
          "${CAI_LUA_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/e2e_usage_limits.lua")
      set_tests_properties(cai_lua_chatgpt_auth_rejection PROPERTIES
        LABELS "integration;offline"
        ENVIRONMENT "${CAI_LUA_TEST_ENV}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        TIMEOUT ${CAI_INTEGRATION_TIMEOUT_SHORT})
      add_test(NAME cai_lua_chatgpt_auth_ownership
        COMMAND /bin/sh
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/chatgpt_auth_failure_test.sh"
          ownership-lua
          "${CAI_LUA_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/e2e_usage_limits.lua")
      set_tests_properties(cai_lua_chatgpt_auth_ownership PROPERTIES
        LABELS "integration;offline"
        ENVIRONMENT "${CAI_LUA_TEST_ENV}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        TIMEOUT ${CAI_INTEGRATION_TIMEOUT_SHORT})
      add_test(NAME cai_lua_chatgpt_tool_stream_e2e
        COMMAND "${CAI_LUA_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/e2e_tool_stream.lua")
      set_tests_properties(cai_lua_chatgpt_tool_stream_e2e PROPERTIES
        LABELS integration
        ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_LUA_TOOL_STREAM_E2E=1"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
      add_test(NAME cai_lua_chatgpt_session_continuity_e2e
        COMMAND "${CAI_LUA_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/e2e_session_continuity.lua")
      set_tests_properties(cai_lua_chatgpt_session_continuity_e2e PROPERTIES
        LABELS integration
        ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_LUA_SESSION_E2E=1"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
      add_test(NAME cai_lua_chatgpt_usage_limits_e2e
        COMMAND "${CAI_LUA_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/e2e_usage_limits.lua")
      set_tests_properties(cai_lua_chatgpt_usage_limits_e2e PROPERTIES
        LABELS integration
        ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_LUA_USAGE_LIMITS_E2E=1"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
      add_test(NAME cai_lua_chatgpt_hosted_web_search_e2e
        COMMAND "${CAI_LUA_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/e2e_hosted_web_search.lua")
      set_tests_properties(cai_lua_chatgpt_hosted_web_search_e2e PROPERTIES
        LABELS integration
        ENVIRONMENT "${CAI_LUA_TEST_ENV};CAI_LUA_HOSTED_WEB_SEARCH_E2E=1"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        TIMEOUT ${CAI_INTEGRATION_TIMEOUT_MEDIUM})
    endif()
    if(CAI_BUILD_LUA)
      add_test(NAME cai_lua_smith_e2e
        COMMAND "/bin/sh"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua_rock_e2e.sh"
          "${CMAKE_CURRENT_SOURCE_DIR}"
          "${CAI_LUA_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/e2e_smith.lua")
      set_tests_properties(cai_lua_smith_e2e PROPERTIES
        LABELS integration
        ENVIRONMENT "${CAI_LUA_SMITH_E2E_ENV}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        TIMEOUT ${CAI_INTEGRATION_TIMEOUT_SMITH_REVIEW})
      add_test(NAME cai_lua_smith_mcp_e2e
        COMMAND "/bin/sh"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/smith_mcp_live_e2e.sh"
          $<TARGET_FILE:cai_mcp_http_server>
          "/bin/sh"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua_rock_e2e.sh"
          "${CMAKE_CURRENT_SOURCE_DIR}"
          "${CAI_LUA_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua/e2e_smith.lua")
      set_tests_properties(cai_lua_smith_mcp_e2e PROPERTIES
        LABELS integration
        ENVIRONMENT "${CAI_LUA_SMITH_E2E_ENV}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        TIMEOUT ${CAI_INTEGRATION_TIMEOUT_SMITH_REVIEW})
    endif()
    if(CAI_REGISTER_API_KEY_EXAMPLE_SMOKE AND
        TARGET cai_example_basic_response AND
        TARGET cai_example_streaming_text AND
        TARGET cai_example_history_export AND
        TARGET cai_example_session_state AND
        TARGET cai_example_terminal_chat AND
        CAI_LUA_EXECUTABLE)
      add_test(NAME cai_examples_live_smoke
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/examples_live_smoke.sh"
          "${CMAKE_CURRENT_SOURCE_DIR}"
          "$<TARGET_FILE:cai_example_basic_response>"
          "$<TARGET_FILE:cai_example_streaming_text>"
          "$<TARGET_FILE:cai_example_history_export>"
          "$<TARGET_FILE:cai_example_session_state>"
          "$<TARGET_FILE:cai_example_terminal_chat>"
          "${CAI_LUA_EXECUTABLE}")
      set_tests_properties(cai_examples_live_smoke PROPERTIES
        LABELS "integration;example-smoke;api-key"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        TIMEOUT ${CAI_INTEGRATION_TIMEOUT_LONG})
    endif()
  endif()
endif()
