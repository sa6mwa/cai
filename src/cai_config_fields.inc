/* section, key, storage, target, member, flags, choices, description */
F("client", "api_key", STR, CLIENT, api_key, CAI_CONFIG_SECRET, NULL,
  "Explicit API key; prefer api_key_env.")
F("client", "api_key_env", STR, CLIENT, api_key_env, 0, NULL,
  "API key variable; provider presets select their standard name.")
F("client", "base_url", STR, CLIENT, base_url, 0, NULL,
  "Explicit API endpoint; empty uses provider defaults.")
F("client", "organization_id", STR, CLIENT, organization_id, 0, NULL,
  "OpenAI organization header.")
F("client", "project_id", STR, CLIENT, project_id, 0, NULL,
  "OpenAI project header.")
F("client", "timeout_ms", LONG, CLIENT, timeout_ms, 0, NULL,
  "HTTP timeout in milliseconds; zero uses SDK default.")
F("client", "http_2_disabled", BOOL, CLIENT, http_2_disabled, 0, NULL,
  "Disable HTTP/2 negotiation.")
F("client", "insecure_skip_verify", BOOL, CLIENT, insecure_skip_verify, 0, NULL,
  "Disable TLS certificate verification.")
F("client", "ca_bundle_path", STR, CLIENT, ca_bundle_path, CAI_CONFIG_PATH,
  NULL, "TLS CA bundle.")
F("client", "ca_path", STR, CLIENT, ca_path, CAI_CONFIG_PATH, NULL,
  "TLS CA directory.")
F("client", "responses_websocket_fallback_disabled", BOOL, CLIENT,
  responses_websocket_fallback_disabled, 0, NULL,
  "Disable HTTPS fallback from WebSocket streaming.")
F("client", "json_response_limit_bytes", SIZE, CLIENT,
  json_response_limit_bytes, 0, NULL,
  "Maximum buffered JSON response bytes; zero uses SDK default.")
F("client", "logger_disabled", BOOL, CLIENT, logger_disabled, 0, NULL,
  "Disable client diagnostics.")
F("client", "api_protocol", PROTOCOL, CLIENT, api_protocol, 0,
  "responses|chat_completions",
  "Wire protocol; provider presets supply defaults.")
F("client", "metadata_provider", STR, CLIENT, metadata_provider, 0, NULL,
  "Fallback catalog identity; live discovery keys on endpoint.")
F("client", "model_metadata_json", STR, CLIENT, model_metadata_json, 0, NULL,
  "Inline fallback catalog JSON; schema checked by client open.")
F("client", "chat_max_attempts", UINT, CLIENT, chat_max_attempts, 0, NULL,
  "Chat attempts including first; zero selects 3, maximum 16.")
F("client", "chat_retry_initial_delay_ms", LONG, CLIENT,
  chat_retry_initial_delay_ms, 0, NULL,
  "Initial Chat retry delay; zero selects 500 ms.")
F("client", "chat_retry_max_delay_ms", LONG, CLIENT, chat_retry_max_delay_ms, 0,
  NULL, "Maximum Chat retry wait; zero selects 30000 ms.")
F("client_limits", "max_input_tokens", I64, CLIENT,
  usage_limits.max_input_tokens, 0, NULL,
  "Client input-token limit; zero disables.")
F("client_limits", "max_input_cached_tokens", I64, CLIENT,
  usage_limits.max_input_cached_tokens, 0, NULL,
  "Client cached-token limit; zero disables.")
F("client_limits", "max_output_tokens", I64, CLIENT,
  usage_limits.max_output_tokens, 0, NULL,
  "Client output-token limit; zero disables.")
F("client_limits", "max_output_reasoning_tokens", I64, CLIENT,
  usage_limits.max_output_reasoning_tokens, 0, NULL,
  "Client reasoning-token limit; zero disables.")
F("client_limits", "max_total_tokens", I64, CLIENT,
  usage_limits.max_total_tokens, 0, NULL,
  "Client total-token limit; zero disables.")
F("client_limits", "max_spend_usd", REAL, CLIENT, usage_limits.max_spend_usd, 0,
  NULL, "Estimated client USD limit; zero disables.")
F("response", "model", STR, AGENT, model, 0, NULL, "Low-level agent model.")
F("response", "developer_instructions", STR, AGENT, developer_instructions, 0,
  NULL, "Low-level instructions; quoted \\n supports multiple lines.")
F("response", "prompt_cache_key", STR, AGENT, prompt_cache_key, 0, NULL,
  "Prompt cache identity.")
F("response", "tool_choice", STR, AGENT, tool_choice, 0, NULL, "Tool choice.")
F("response", "tool_choice_json", STR, AGENT, tool_choice_json, 0, NULL,
  "Provider-specific tool choice JSON.")
F("response", "reasoning_effort", STR, AGENT, reasoning_effort, 0, NULL,
  "Provider reasoning effort.")
F("response", "reasoning_summary", STR, AGENT, reasoning_summary, 0,
  "none|auto|concise|detailed", "Reasoning-summary mode.")
F("response", "reasoning_mode", STR, AGENT, reasoning_mode, 0, NULL,
  "Provider reasoning mode.")
F("response", "text_format_name", STR, AGENT, text_format_name, 0, NULL,
  "Structured output schema name.")
F("response", "text_format_description", STR, AGENT, text_format_description, 0,
  NULL, "Structured output description.")
F("response", "text_format_schema_json", STR, AGENT, text_format_schema_json, 0,
  NULL, "Structured output JSON schema.")
F("response", "text_format_strict", BOOL, AGENT, text_format_strict, 0, NULL,
  "Strict structured output.")
F("response", "max_output_tokens", INT, AGENT, max_output_tokens, 0, NULL,
  "Maximum output tokens; zero leaves unset.")
F("response", "max_tool_calls", INT, AGENT, max_tool_calls, 0, NULL,
  "Maximum tool calls; zero leaves unset.")
F("response", "disable_parallel_tool_calls", BOOL, AGENT,
  disable_parallel_tool_calls, 0, NULL, "Disable parallel calls.")
F("response", "session_continuity", CONTINUITY, AGENT, session_continuity, 0,
  "auto|server|client", "Continuation strategy.")
F("response", "disable_auto_compaction", BOOL, AGENT, disable_auto_compaction,
  0, NULL, "Disable automatic compaction.")
F("response", "compact_threshold_tokens", I64, AGENT, compact_threshold_tokens,
  0, NULL, "Explicit compaction threshold; zero automatic.")
F("response", "compact_threshold_percent", UINT, AGENT,
  compact_threshold_percent, 0, NULL,
  "Context compaction percentage; zero automatic.")
F("response", "enable_local_history", BOOL, AGENT, enable_local_history, 0,
  NULL, "Capture local low-level session history.")
F("response", "history_memory_limit", SIZE, AGENT, history_memory_limit, 0,
  NULL, "History spill threshold; zero automatic.")
F("response", "history_spool_dir", STR, AGENT, history_spool_dir,
  CAI_CONFIG_PATH, NULL, "History spool directory.")
F("session_limits", "max_input_tokens", I64, AGENT,
  session_usage_limits.max_input_tokens, 0, NULL,
  "Session input-token limit; zero disables.")
F("session_limits", "max_input_cached_tokens", I64, AGENT,
  session_usage_limits.max_input_cached_tokens, 0, NULL,
  "Session cached-token limit; zero disables.")
F("session_limits", "max_output_tokens", I64, AGENT,
  session_usage_limits.max_output_tokens, 0, NULL,
  "Session output-token limit; zero disables.")
F("session_limits", "max_output_reasoning_tokens", I64, AGENT,
  session_usage_limits.max_output_reasoning_tokens, 0, NULL,
  "Session reasoning-token limit; zero disables.")
F("session_limits", "max_total_tokens", I64, AGENT,
  session_usage_limits.max_total_tokens, 0, NULL,
  "Session total-token limit; zero disables.")
F("session_limits", "max_spend_usd", REAL, AGENT,
  session_usage_limits.max_spend_usd, 0, NULL,
  "Session estimated USD limit; zero disables.")
F("agent", "preset", STR, RUNTIME, preset, 0, "smith|smith-review",
  "Agent preset; empty selects Smith.")
F("agent", "identity", STR, RUNTIME, agent_identity, 0, NULL,
  "Agent identity; empty selects preset identity.")
F("agent", "model", STR, RUNTIME, model, 0, NULL,
  "New-conversation model; resume restores saved selection.")
F("agent", "reasoning_effort", STR, RUNTIME, reasoning_effort, 0,
  "none|minimal|low|medium|high|xhigh|max|ultra",
  "New-conversation reasoning effort.")
F("agent", "reasoning_summary", STR, RUNTIME, reasoning_summary, 0,
  "none|auto|concise|detailed", "New-conversation reasoning summary.")
F("agent", "developer_instructions", STR, RUNTIME,
  developer_instructions_extension, 0, NULL,
  "Instructions appended to the preset.")
F("agent", "codex_agents_md", BOOL, RUNTIME, codex_compat_agents_md, 0, NULL,
  "Load Codex global and ancestor AGENTS.md.")
F("agent", "image_generation", BOOL, RUNTIME, enable_image_generation, 0, NULL,
  "Enable provider image generation.")
F("agent", "terminal", NOTBOOL, RUNTIME, disable_terminal, 0, NULL,
  "Enable managed terminal tools.")
F("agent", "review_subagent", NOTBOOL, RUNTIME, disable_review_subagent, 0,
  NULL, "Enable the built-in reviewer.")
F("review", "model", STR, RUNTIME, review_model, 0, NULL,
  "Reviewer model; empty inherits active agent.")
F("review", "reasoning_effort", STR, RUNTIME, review_reasoning_effort, 0,
  "none|minimal|low|medium|high|xhigh|max|ultra",
  "Reviewer effort; empty inherits.")
F("review", "reasoning_summary", STR, RUNTIME, review_reasoning_summary, 0,
  "none|auto|concise|detailed", "Reviewer summary; empty inherits.")
F("runtime", "workspace_directory", STR, RUNTIME, workspace_directory,
  CAI_CONFIG_PATH, NULL, "Workspace path; applications select their workspace.")
F("runtime", "agent_config_directory", STR, RUNTIME, agent_config_directory,
  CAI_CONFIG_PATH, NULL, "Global agent configuration directory.")
F("runtime", "global_agents_md_path", STR, RUNTIME, global_agents_md_path,
  CAI_CONFIG_PATH, NULL, "Global AGENTS.md override.")
F("runtime", "image_directory", STR, RUNTIME, image_directory, CAI_CONFIG_PATH,
  NULL, "Additional permitted image directory.")
F("runtime", "session_scope", STR, RUNTIME, session_scope, 0, NULL,
  "Session namespace; empty uses workspace.")
F("runtime", "session_id", STR, RUNTIME, session_id, 0, NULL,
  "New-session identifier; empty generates one.")
F("runtime", "resume_latest", BOOL, RUNTIME, resume_latest, 0, NULL,
  "Restore latest matching SDK session.")
F("runtime", "resume_session_id", STR, RUNTIME, resume_session_id, 0, NULL,
  "Restore explicit SDK session.")
F("runtime", "record_transcript", BOOL, RUNTIME, record_transcript, 0, NULL,
  "Record display transcript.")
F("runtime", "disable_default_session_store", BOOL, RUNTIME,
  disable_default_session_store, 0, NULL,
  "Disable the default local SDK store.")
F("runtime", "event_queue_limit", SIZE, RUNTIME, event_queue_limit, 0, NULL,
  "Event queue bound; zero uses SDK default.")
F("runtime", "steering_queue_limit", SIZE, RUNTIME, steering_queue_limit, 0,
  NULL, "Steering queue bound; zero uses SDK default.")
F("runtime", "turn_queue_limit", SIZE, RUNTIME, turn_queue_limit, 0, NULL,
  "Normal-turn queue bound; zero uses SDK default.")
F("runtime", "logger_disabled", BOOL, RUNTIME, logger_disabled, 0, NULL,
  "Disable runtime diagnostics.")
F("terminal", "root_path", STR, TERMINAL, root_path, CAI_CONFIG_PATH, NULL,
  "Terminal workspace root.")
F("terminal", "default_workdir", STR, TERMINAL, default_workdir,
  CAI_CONFIG_PATH, NULL, "Initial terminal working directory.")
F("terminal", "shell_path", STR, TERMINAL, shell_path, CAI_CONFIG_PATH, NULL,
  "Agent command shell; empty uses /bin/sh.")
F("terminal", "default_yield_time_ms", LONG, TERMINAL, default_yield_time_ms, 0,
  NULL, "Initial exec wait; zero selects 10000 ms.")
F("terminal", "max_yield_time_ms", LONG, TERMINAL, max_yield_time_ms, 0, NULL,
  "Maximum initial wait; zero selects 30000 ms.")
F("terminal", "output_max_bytes", SIZE, TERMINAL, output_max_bytes, 0, NULL,
  "Retained output limit; zero selects 3 MiB.")
F("terminal", "default_write_yield_time_ms", LONG, TERMINAL,
  default_write_yield_time_ms, 0, NULL, "Write wait; zero selects 250 ms.")
F("terminal", "max_write_yield_time_ms", LONG, TERMINAL,
  max_write_yield_time_ms, 0, NULL,
  "Maximum write wait; zero selects 30000 ms.")
F("terminal", "default_poll_yield_time_ms", LONG, TERMINAL,
  default_poll_yield_time_ms, 0, NULL, "Empty poll wait; zero selects 5000 ms.")
F("terminal", "max_poll_yield_time_ms", LONG, TERMINAL, max_poll_yield_time_ms,
  0, NULL, "Maximum poll wait; zero selects 300000 ms.")
