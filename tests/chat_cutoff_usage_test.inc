/* Cutoffs consume tokens even without a complete message or executable call.
 * Exercise real wire usage, history and limits without inference or sleeps. */
static const char *const cutoff_deltas[] = {
    "{\"reasoning_content\":\"unfinished reasoning\"}",
    "{\"tool_calls\":[{\"index\":0,\"id\":\"unused_call\","
    "\"type\":\"function\",\"function\":{\"name\":\"raw_echo\","
    "\"arguments\":\"{\"}}]}",
    "{}"};

static void cutoff_body(char *out, size_t size, size_t variant) {
  snprintf(out, size,
           "data: {\"id\":\"cutoff\",\"choices\":[{\"index\":0,"
           "\"delta\":%s,\"finish_reason\":\"length\"}]}\n\n"
           "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":20,"
           "\"prompt_tokens_details\":{\"cached_tokens\":5},"
           "\"completion_tokens\":8,\"completion_tokens_details\":{"
           "\"reasoning_tokens\":8},\"total_tokens\":28}}\n\n"
           "data: [DONE]\n\n",
           cutoff_deltas[variant]);
}

static void test_chat_length_empty_usage_case(test_state *state, size_t variant,
                                              int after_tool, int limit_kind) {
  static const char call[] =
      "data: {\"id\":\"call\",\"choices\":[{\"index\":0,\"delta\":{"
      "\"tool_calls\":[{\"index\":0,\"id\":\"used_call\",\"type\":\"function\","
      "\"function\":{\"name\":\"raw_echo\",\"arguments\":\"{}\"}}]},"
      "\"finish_reason\":\"tool_calls\"}],\"usage\":{\"prompt_tokens\":6,"
      "\"completion_tokens\":4,\"total_tokens\":10}}\n\ndata: [DONE]\n\n";
  static const char final[] =
      "data: {\"id\":\"final\",\"choices\":[{\"index\":0,\"delta\":{"
      "\"content\":\"done\"},\"finish_reason\":\"stop\"}],\"usage\":{"
      "\"prompt_tokens\":6,\"completion_tokens\":4,\"total_tokens\":10}}\n\n"
      "data: [DONE]\n\n";
  static const char *required[] = {"cutoff usage prompt", "Continue"};
  static const char *forbidden[] = {"unused_call", "unfinished reasoning"};
  mock_http_expectation script[3];
  http_mock_server server;
  cai_client_config config;
  cai_agent_config agent_config;
  cai_client *client = NULL;
  cai_agent *agent = NULL;
  cai_session *session = NULL;
  cai_sink *sink = NULL;
  cai_source *history = NULL;
  cai_sink_callbacks callbacks;
  cai_stream_sinks sinks;
  cai_usage_limits limits;
  cai_usage_accounting usage;
  raw_tool_state tool;
  write_state output;
  cai_error error;
  char body[2048], name[96], history_json[4096];
  size_t i, count = (size_t)after_tool + 1U + (limit_kind == 0 ? 1U : 0U);
  long total = after_tool ? 38L : 28L;
  int rc;

  snprintf(name, sizeof(name), "chat_cutoff_usage_%lu_%d_%d",
           (unsigned long)variant, after_tool, limit_kind);
  cutoff_body(body, sizeof(body), variant);
  memset(script, 0, sizeof(script));
  for (i = 0U; i < count; i++) {
    script[i].request_prefix = "POST /v1/chat/completions HTTP/";
    script[i].status = 200;
    script[i].status_text = "OK";
    script[i].content_type = "text/event-stream";
  }
  if (after_tool)
    script[0].body = call;
  script[after_tool].body = body;
  if (limit_kind == 0) {
    script[count - 1U].body = final;
    script[count - 1U].required = required;
    script[count - 1U].required_count = 2U;
    script[count - 1U].forbidden = forbidden;
    script[count - 1U].forbidden_count = 2U;
  }
  if (http_mock_server_open_script(state, name, script, count, &server) != 0)
    return;
  cai_error_init(&error);
  cai_usage_limits_init(&limits);
  limits.max_total_tokens = total - 1L;
  cai_client_config_init(&config);
  config.api_key = "mock-key";
  config.base_url = server.base_url;
  config.api_protocol = CAI_API_PROTOCOL_CHAT_COMPLETIONS;
  config.chat_max_attempts = 1U;
  config.http_2_disabled = 1;
  config.timeout_ms = 1000L;
  cai_agent_config_init(&agent_config);
  agent_config.model = "test-model";
  agent_config.disable_auto_compaction = 1;
  memset(&output, 0, sizeof(output));
  memset(&tool, 0, sizeof(tool));
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = test_write;
  callbacks.context = &output;
  cai_stream_sinks_init(&sinks);
  rc = cai_sink_from_callbacks(&callbacks, &sink, &error);
  sinks.output_text = sink;
  if (rc == CAI_OK)
    rc = cai_client_open(&config, &client, &error);
  if (rc == CAI_OK && limit_kind == 2)
    rc = cai_client_set_usage_limits(client, &limits, &error);
  if (rc == CAI_OK)
    rc = cai_client_new_agent(client, &agent_config, &agent, &error);
  if (rc == CAI_OK)
    rc = cai_agent_register_raw_tool(agent, "raw_echo", "Echo",
                                     "{\"type\":\"object\"}", 0, test_raw_tool,
                                     &tool, &error);
  if (rc == CAI_OK)
    rc = cai_agent_new_session(agent, &session, &error);
  if (rc == CAI_OK && limit_kind == 1)
    rc = cai_session_set_usage_limits(session, &limits, &error);
  if (rc == CAI_OK)
    rc = cai_session_add_user_text(session, "cutoff usage prompt", &error);
  expect_int(state, name, rc, CAI_OK);
  if (rc == CAI_OK) {
    rc = cai_session_stream_auto(session, NULL, &sinks, &error);
    expect_int(state, name, rc, CAI_ERR_LIMIT);
    if (limit_kind == 0)
      expect_str(state, name, error.server_code, "output_length");
    expect_str(state, name, output.buffer, "");
    expect_str(state, name, tool.seen, after_tool ? "{}" : "");
    cai_error_cleanup(&error);
    cai_error_init(&error);
    expect_int(state, name, cai_session_usage(session, &usage, &error), CAI_OK);
    expect_int(state, name, usage.usage.total_tokens, total);
    expect_int(state, name, usage.usage.input_cached_tokens, 5L);
    expect_int(state, name, usage.usage.output_reasoning_tokens, 8L);
    expect_int(state, name, usage.limit_exceeded, limit_kind == 1);
    expect_int(state, name, cai_client_usage(client, &usage, &error), CAI_OK);
    expect_int(state, name, usage.usage.total_tokens, total);
    expect_int(state, name, usage.limit_exceeded, limit_kind == 2);
    if (limit_kind == 0) {
      rc = cai_session_export_history_source(session, &history, &error);
      expect_int(state, name, rc, CAI_OK);
      if (rc == CAI_OK && read_source_text(state, name, history, history_json,
                                           sizeof(history_json), &error)) {
        expect_int(
            state, name,
            (long)test_count_substrings(history_json, "cutoff usage prompt"),
            1L);
        expect_int(state, name, strstr(history_json, "unused_call") == NULL,
                   1L);
      }
    }
    rc = cai_session_add_user_text(session, "Continue", &error);
    if (rc == CAI_OK)
      rc = cai_session_stream_auto(session, NULL, &sinks, &error);
    expect_int(state, name, rc, limit_kind == 0 ? CAI_OK : CAI_ERR_LIMIT);
    expect_str(state, name, output.buffer, limit_kind == 0 ? "done" : "");
    expect_int(state, name, cai_session_usage(session, &usage, NULL), CAI_OK);
    expect_int(state, name, usage.usage.total_tokens,
               total + (limit_kind == 0 ? 10L : 0L));
  }
  cai_source_close(history);
  cai_sink_close(sink);
  cai_session_destroy(session);
  cai_agent_destroy(agent);
  cai_client_close(client);
  cai_error_cleanup(&error);
  expect_child_exit(state, name, server.pid, &server.child_status);
}

static void test_chat_length_goal_budget(test_state *state, size_t attempts) {
  mock_http_expectation script[4];
  http_mock_server server;
  cai_client_config config;
  cai_agent_runtime_config runtime_config;
  cai_agent_session_store store;
  runtime_session_store_state saved;
  runtime_event_state events;
  cai_client *client = NULL;
  cai_agent_runtime *runtime = NULL;
  cai_agent_goal_snapshot goal;
  cai_agent_runtime_metrics metrics;
  cai_error error;
  char body[2048], commit[512];
  int rc, i;
  size_t n;
  const char *name = "chat_cutoff_goal_budget";

  cutoff_body(body, sizeof(body), 0U);
  memset(script, 0, sizeof(script));
  for (n = 0U; n < attempts; n++) {
    script[n].request_prefix = "POST /v1/chat/completions HTTP/";
    script[n].status = 200;
    script[n].status_text = "OK";
    script[n].content_type = "text/event-stream";
    script[n].body = body;
  }
  if (http_mock_server_open_script(state, name, script, attempts, &server) != 0)
    return;
  cai_error_init(&error);
  memset(&saved, 0, sizeof(saved));
  memset(&store, 0, sizeof(store));
  memset(&events, 0, sizeof(events));
  snprintf(
      commit, sizeof(commit),
      "{\"version\":1,\"model\":\"test-model\","
      "\"goal_objective\":\"bounded reasoning\",\"goal_status\":\"active\","
      "\"goal_token_budget\":%d,\"goal_token_usage_baseline\":0,"
      "\"goal_tokens_used\":0,\"history\":[]}",
      attempts == 1U ? 20 : 100);
  saved.commit_json = commit;
  store.commit = test_runtime_session_store_commit;
  store.load_latest = test_runtime_session_store_load;
  store.append_event = test_runtime_session_store_append_event;
  store.load_events_after = test_runtime_session_store_load_events_after;
  store.context = &saved;
  events.owner = pthread_self();
  cai_client_config_init(&config);
  config.api_key = "mock-key";
  config.base_url = server.base_url;
  config.api_protocol = CAI_API_PROTOCOL_CHAT_COMPLETIONS;
  config.chat_max_attempts = 1U;
  config.http_2_disabled = 1;
  config.timeout_ms = 1000L;
  rc = cai_client_open(&config, &client, &error);
  cai_agent_runtime_config_init(&runtime_config);
  runtime_config.workspace_directory = CAI_TEST_BINARY_DIR;
  runtime_config.model = "test-model";
  runtime_config.disable_terminal = 1;
  runtime_config.session_store = &store;
  runtime_config.resume_latest = 1;
  runtime_config.event_callback = test_runtime_event;
  runtime_config.event_context = &events;
  if (rc == CAI_OK)
    rc = cai_agent_runtime_open(client, &runtime_config, &runtime, &error);
  if (rc == CAI_OK)
    rc = cai_agent_runtime_submit(runtime, "bounded reasoning", &error);
  for (i = 0; rc == CAI_OK && i < 100 && events.completed_count == 0 &&
              events.failed_count == 0;
       i++)
    rc = cai_agent_runtime_pump(runtime, 100L, &error);
  expect_int(state, name, rc, CAI_OK);
  expect_int(state, name, events.completed_count, 1L);
  expect_int(state, name, events.failed_count, 0L);
  if (events.failed_count != 0)
    fprintf(stderr, "%s: %s\n", name, events.failure_message);
  if (runtime != NULL) {
    expect_int(state, name, cai_agent_runtime_get_goal(runtime, &goal, &error),
               CAI_OK);
    expect_str(state, name, goal.status, "budget_limited");
    expect_int(state, name, goal.tokens_used, (long)attempts * 28L);
    expect_int(state, name,
               cai_agent_runtime_get_metrics(runtime, &metrics, &error),
               CAI_OK);
    expect_int(state, name, metrics.session_usage.usage.total_tokens,
               (long)attempts * 28L);
    expect_substr(state, name, saved.saved_commit,
                  "\"goal_status\":\"budget_limited\"");
    expect_int(state, name, cai_agent_runtime_submit(runtime, "More", &error),
               CAI_ERR_LIMIT);
  }
  cai_agent_runtime_close(runtime);
  cai_client_close(client);
  cai_error_cleanup(&error);
  expect_child_exit(state, name, server.pid, &server.child_status);
}

static void test_chat_length_empty_usage(test_state *state) {
  size_t variant;
  int after_tool, limit_kind;
  for (variant = 0U; variant < 3U; variant++)
    for (after_tool = 0; after_tool < 2; after_tool++)
      for (limit_kind = 0; limit_kind < 3; limit_kind++)
        test_chat_length_empty_usage_case(state, variant, after_tool,
                                          limit_kind);
  test_chat_length_goal_budget(state, 1U);
  test_chat_length_goal_budget(state, 4U);
}
