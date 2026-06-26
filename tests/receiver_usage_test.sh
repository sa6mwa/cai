#!/usr/bin/env sh
set -eu

repo_root=${1:-.}

failures=$(
  grep -R -n -E \
    -e 'cai_client_(close|new_agent|create_response|count_response_input_tokens|stream_response_text|open_response_text_source|retrieve_response|cancel_response|delete_response|list_response_input_items|create_conversation|retrieve_conversation|retrieve_conversation_handle|update_conversation_metadata|update_conversation_metadata_handle|delete_conversation|delete_conversation_handle|list_conversation_items|list_conversation_items_handle|delete_conversation_item|delete_conversation_item_handle|retrieve_conversation_item|retrieve_conversation_item_handle|create_conversation_items|create_conversation_items_handle|set_usage_limits|usage)\(' \
    -e 'cai_agent_(new_session|new_conversation_session|new_session_for_conversation|add_user_text|add_user_text_spooled|add_user_text_source|add_user_image_url|add_user_file_data_spooled|add_user_file_source|add_user_file_path|run|run_output|run_auto|run_auto_output|stream_auto|stream|stream_text|open_text_source|send_text|last_usage|set_session_usage_limits|usage|context_percent)\(' \
    -e 'cai_session_(set_conversation_id|set_conversation|conversation_id|set_previous_response_id|previous_response_id|add_user_text|add_user_text_spooled|add_user_text_source|add_user_image_url|add_user_file_data_spooled|add_user_file_source|add_user_file_path|add_function_call_output|run|run_output|run_auto|run_auto_output|stream_auto|stream|stream_text|open_text_source|send_text|last_usage|set_usage_limits|usage|close_with_usage|context_window_tokens|auto_compact_token_limit|context_percent|history_spilled|export_history_source|import_history_source|export_state_source|import_state_source|save_state_path|load_state_path)\(' \
    -e 'cai_source_(read|reset|copy_to_sink|close)\(' \
    -e 'cai_sink_(write|close)\(' \
    -e 'cai_mcp_handler_(handle_http|destroy)\(' \
    -e 'cai_mcp_client_(initialize|ping|refresh_tools|tool_count|tool_at|call_tool|refresh_resources|resource_count|resource_at|read_resource|refresh_resource_templates|resource_template_count|resource_template_at|refresh_prompts|prompt_count|prompt_at|get_prompt|complete|send_request|send_notification|destroy)\(' \
    -e 'cai_chatgpt_auth_(access_token|refresh|close)\(' \
    -e 'cai_chatgpt_login_(handle_callback|completed|close)\(' \
    -e 'cai_tool_event_(write_output|write_arguments)\(' \
    -e 'cai_tool_registry_(destroy|register_lonejson|register_raw|register_raw_spooled|add_to_response_params|run|run_spooled)\(' \
    -e 'cai_output_(response|text|refusal|raw_json|write_text|write_refusal|write_raw_json|write_json|as_lc_source|destroy)\(' \
    -e 'cai_response_(id|status|model|conversation_id|created_at|output_text|refusal|write_output_text|write_refusal|raw_json|output_items_json|write_output_items_json|error_code|error_message|incomplete_reason|input_tokens|input_cached_tokens|output_tokens|output_reasoning_tokens|total_tokens|usage|tool_call_count|tool_call_id|tool_call_name|tool_call_arguments|tool_call_arguments_spooled|output_item_count|output_item_id|output_item_type|output_item_status|output_item_role|output_item_call_id|output_item_name|destroy)\(' \
    -e 'cai_response_create_params_(destroy|set_model|set_instructions|set_previous_response_id|set_conversation_id|set_prompt_cache_key|set_background|set_store|set_service_tier|set_truncation|set_metadata_json|set_include_json|set_prompt_json|set_tool_choice|set_tool_choice_json|set_max_output_tokens|set_max_tool_calls|set_reasoning|set_parallel_tool_calls|set_compact_threshold|set_text_format_json_object|set_text_format_json_schema|set_text_verbosity|add_text|add_text_spooled|add_image_url|add_image_file_id|add_file_id|add_file_url|add_file_data_spooled|add_function_tool|add_hosted_tool_json|add_simple_hosted_tool|add_hosted_mcp_tool|add_function_call_output|add_function_call_output_text|add_function_call_output_image_url|add_function_call_output_file_id|add_function_call_output_file_data_spooled)\(' \
    -e 'cai_input_item_list_(count|has_more|first_id|last_id|raw_json|destroy)\(' \
    -e 'cai_input_item_(id|type|role)\(' \
    -e 'cai_conversation_(id|object|destroy)\(' \
    -e 'cai_conversation_item_(id|type|role|raw_json|destroy)\(' \
    -e 'cai_conversation_items_params_(destroy|add_text|add_text_spooled|add_text_source|add_image_url|add_image_file_id|add_file_id|add_file_url|add_file_data_spooled)\(' \
    "$repo_root/examples" || true
)

if [ -n "$failures" ]; then
  printf '%s\n' "examples must use receiver methods for handle operations:" >&2
  printf '%s\n' "$failures" >&2
  exit 1
fi
