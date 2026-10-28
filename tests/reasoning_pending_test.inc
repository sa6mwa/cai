/* A real tool-round boundary, without sleeps: committing waits while the
 * owner switches models after set_reasoning has queued its raise. */
typedef struct reasoning_boundary {
  runtime_session_store_state store;
  pthread_mutex_t lock;
  pthread_cond_t condition;
  int entered, release;
} reasoning_boundary;

static int reasoning_commit(void *context, const char *scope,
                            const char *session_id, cai_source *source,
                            unsigned long long sequence, cai_error *error) {
  reasoning_boundary *barrier = context;
  int rc = test_runtime_session_store_commit(&barrier->store, scope, session_id,
                                             source, sequence, error);
  if (rc != CAI_OK ||
      strstr(barrier->store.saved_commit, "function_call_output") == NULL)
    return rc;
  pthread_mutex_lock(&barrier->lock);
  if (!barrier->entered) {
    barrier->entered = 1;
    pthread_cond_broadcast(&barrier->condition);
    while (!barrier->release)
      pthread_cond_wait(&barrier->condition, &barrier->lock);
  }
  pthread_mutex_unlock(&barrier->lock);
  return rc;
}
static void test_reasoning_model_boundary(test_state *state) {
  static const char body[] =
      "data: "
      "{\"id\":\"boundary\",\"choices\":[{\"index\":0,\"delta\":{\"tool_"
      "calls\":["
      "{\"index\":0,\"id\":\"raise\",\"type\":\"function\",\"function\":{"
      "\"name\":\"set_reasoning\",\"arguments\":\"{\\\"mode\\\":\\\"high\\\"}"
      "\"}}]},\"finish_reason\":\"tool_"
      "calls\"}]}\n\n"
      "data: [DONE]\n\n";
  static const char final[] =
      "data: "
      "{\"id\":\"finished\",\"choices\":[{\"index\":0,\"delta\":{\"content\":"
      "\"finished\"},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
  static const char *required[] = {"\"model\":\"fixture-b\"",
                                   "\"reasoning_effort\":\"medium\"",
                                   "\"content\":\"ok\""};
  static const mock_http_expectation script[] = {
      {"POST /v1/chat/completions HTTP/", NULL, 0U, NULL, 0U, 200, "OK",
       "text/event-stream", NULL, body},
      {"POST /v1/chat/completions HTTP/", required, 3U, NULL, 0U, 200, "OK",
       "text/event-stream", NULL, final}};
  reasoning_boundary barrier;
  http_mock_server server;
  cai_client_config client_config;
  cai_agent_runtime_config config;
  cai_agent_session_store store;
  cai_agent_runtime_settings requested, effective;
  cai_client *client = NULL;
  cai_agent_runtime *runtime = NULL;
  cai_agent_run_state run_state = CAI_AGENT_IDLE;
  cai_error error;
  test_log_capture logs;
  pslog_logger *logger;
  int rc, i;
  if (http_mock_server_open_script(state, "reasoning_model_boundary", script,
                                   2U, &server) != 0)
    return;
  memset(&barrier, 0, sizeof(barrier));
  memset(&store, 0, sizeof(store));
  store.context = &barrier;
  store.commit = reasoning_commit;
  store.load_latest = test_runtime_session_store_load;
  store.append_event = test_runtime_session_store_append_event;
  store.load_events_after = test_runtime_session_store_load_events_after;
  pthread_mutex_init(&barrier.lock, NULL);
  pthread_cond_init(&barrier.condition, NULL);
  cai_error_init(&error);
  logger = test_log_open(&logs);
  cai_client_config_init(&client_config);
  client_config.logger = logger;
  client_config.base_url = server.base_url;
  client_config.api_key = "fixture";
  client_config.api_protocol = CAI_API_PROTOCOL_CHAT_COMPLETIONS;
  client_config.chat_max_attempts = 1U;
  client_config.timeout_ms = 1000L;
  client_config.http_2_disabled = 1;
  client_config.metadata_provider = "custom";
  client_config.model_metadata_json =
      "{\"models\":[{\"provider\":\"custom\",\"id\":\"fixture-a\",\"reasoning_"
      "efforts\":[\"medium\",\"high\"],\"context_window_tokens\":65536,\"comp_"
      "hash\":\"shared\"},"
      "{\"provider\":\"custom\",\"id\":\"fixture-b\",\"reasoning_efforts\":["
      "\"medium\"],\"context_window_tokens\":65536,\"comp_hash\":\"shared\"}]}";
  rc = cai_client_open(&client_config, &client, &error);
  cai_agent_runtime_config_init(&config);
  config.workspace_directory = CAI_TEST_BINARY_DIR;
  config.model = "fixture-a";
  config.disable_terminal = config.disable_chrome = 1;
  config.disable_default_session_store = 1;
  config.reasoning_tool.enabled = 1;
  config.session_store = &store;
  if (rc == CAI_OK)
    rc = cai_agent_runtime_open(client, &config, &runtime, &error);
  if (rc == CAI_OK)
    rc = cai_agent_runtime_submit(runtime, "Continue working", &error);
  expect_int(state, "reasoning_boundary_submit", rc, CAI_OK);
  if (rc == CAI_OK) {
    pthread_mutex_lock(&barrier.lock);
    while (!barrier.entered)
      pthread_cond_wait(&barrier.condition, &barrier.lock);
    pthread_mutex_unlock(&barrier.lock);
    memset(&requested, 0, sizeof(requested));
    requested.present = CAI_AGENT_RUNTIME_SETTING_MODEL;
    requested.model = "fixture-b";
    rc = cai_agent_runtime_update_settings(runtime, &requested, NULL, &error);
    expect_int(state, "reasoning_boundary_select_model", rc, CAI_OK);
    pthread_mutex_lock(&barrier.lock);
    barrier.release = 1;
    pthread_cond_broadcast(&barrier.condition);
    pthread_mutex_unlock(&barrier.lock);
    for (i = 0; rc == CAI_OK && i < 40 && run_state != CAI_AGENT_COMPLETED &&
                run_state != CAI_AGENT_FAILED;
         i++) {
      rc = cai_agent_runtime_pump(runtime, 100L, &error);
      if (rc == CAI_OK)
        rc = cai_agent_runtime_state(runtime, &run_state, &error);
    }
    expect_int(state, "reasoning_boundary_completed", run_state,
               CAI_AGENT_COMPLETED);
    expect_int(
        state, "reasoning_boundary_settings",
        cai_agent_runtime_get_settings(runtime, &effective, NULL, &error),
        CAI_OK);
    expect_str(state, "reasoning_boundary_effort", effective.reasoning_effort,
               "medium");
    expect_str(state, "reasoning_boundary_model", effective.model, "fixture-b");
    expect_substr(state, "reasoning_boundary_warning", logs.buffer,
                  "cai.reasoning_tool.unavailable");
  }
  cai_agent_runtime_close(runtime);
  cai_client_close(client);
  cai_error_cleanup(&error);
  if (logger != NULL)
    logger->destroy(logger);
  pthread_cond_destroy(&barrier.condition);
  pthread_mutex_destroy(&barrier.lock);
  expect_child_exit(state, "reasoning_boundary_mock", server.pid,
                    &server.child_status);
}
