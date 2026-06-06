#!/usr/bin/env sh
set -eu

repo_root=${1:-.}

failures=$(
  grep -R -n -E \
    'cai_(client_(new_agent|create_response|count_response_input_tokens|stream_response_text|open_response_text_source|retrieve_response|cancel_response|delete_response|list_response_input_items|create_conversation|retrieve_conversation|retrieve_conversation_handle|update_conversation_metadata|update_conversation_metadata_handle|delete_conversation|delete_conversation_handle|list_conversation_items|list_conversation_items_handle|delete_conversation_item|delete_conversation_item_handle|retrieve_conversation_item|retrieve_conversation_item_handle|create_conversation_items|create_conversation_items_handle|set_usage_limits|usage)|agent_(new_session|new_conversation_session|new_session_for_conversation|add_user_text|add_user_text_spooled|add_user_text_source|add_user_image_url|add_user_file_data_spooled|add_user_file_source|add_user_file_path|run|run_output|run_auto|run_auto_output|stream_auto|stream|stream_text|open_text_source|send_text|last_usage|set_session_usage_limits|usage|context_percent)|session_(set_conversation_id|set_conversation|conversation_id|set_previous_response_id|previous_response_id|add_user_text|add_user_text_spooled|add_user_text_source|add_user_image_url|add_user_file_data_spooled|add_user_file_source|add_user_file_path|add_function_call_output|run|run_output|run_auto|run_auto_output|stream_auto|stream|stream_text|open_text_source|send_text|last_usage|set_usage_limits|usage|close_with_usage|context_window_tokens|auto_compact_token_limit|context_percent|history_spilled|export_history_source|import_history_source|export_state_source|import_state_source|save_state_path|load_state_path)|source_(read|reset|copy_to_sink|close)|sink_(write|close)|mcp_handler_(handle_http|destroy)|chatgpt_auth_(access_token|refresh|close)|chatgpt_login_(handle_callback|completed|close)|tool_registry_(destroy|register_lonejson|register_raw|register_raw_spooled|add_to_response_params|run|run_spooled))\(' \
    "$repo_root/examples" || true
)

if [ -n "$failures" ]; then
  printf '%s\n' "examples must use receiver methods for handle operations:" >&2
  printf '%s\n' "$failures" >&2
  exit 1
fi
