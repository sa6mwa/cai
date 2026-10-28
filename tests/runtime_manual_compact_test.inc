/* Manual compaction must commit its replacement before reporting success.
 * Inject a commit failure and prove the next request uses old history. */
static cai_agent_run_state manual_compact_settle(test_state *state,
                                                 cai_agent_runtime *runtime) {
  cai_agent_run_state run = CAI_AGENT_IDLE;
  cai_error error;
  int i;
  cai_error_init(&error);
  for (i = 0; i < 100; i++) {
    expect_int(state, "manual_compact_pump",
               cai_agent_runtime_pump(runtime, 50L, &error), CAI_OK);
    expect_int(state, "manual_compact_state",
               cai_agent_runtime_state(runtime, &run, &error), CAI_OK);
    if (run == CAI_AGENT_COMPLETED || run == CAI_AGENT_FAILED ||
        run == CAI_AGENT_CANCELLED)
      break;
  }
  cai_error_cleanup(&error);
  return run;
}
static void test_agent_runtime_manual_compaction(test_state *state) {
  static const char first[] =
      "data: "
      "{\"type\":\"response.output_text.delta\",\"delta\":\"MANUAL_ORIGINAL_"
      "ANSWER\"}\n\n"
      "data: "
      "{\"type\":\"response.completed\",\"response\":{\"id\":\"resp_manual_"
      "first\",\"usage\":{\"input_tokens\":10,\"output_tokens\":2,\"total_"
      "tokens\":12}}}\n\n";
  static const char compact[] =
      "data: "
      "{\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{"
      "\"id\":\"cmp_manual\",\"type\":\"compaction\",\"encrypted_content\":"
      "\"MANUAL_COMMIT_OPAQUE\"}}\n\n"
      "data: "
      "{\"type\":\"response.completed\",\"response\":{\"id\":\"resp_manual_"
      "compact\",\"usage\":{\"input_tokens\":12,\"output_tokens\":3,\"total_"
      "tokens\":15}}}\n\n";
  static const char *compact_required[] = {"compaction_trigger",
                                           "manual commit source"};
  static const char *opaque[] = {"MANUAL_COMMIT_OPAQUE"};
  static const char *original[] = {"MANUAL_ORIGINAL_ANSWER"};
  cai_client_config client_config;
  cai_agent_runtime_config config;
  cai_agent_session_store store;
  runtime_session_store_state saved;
  runtime_event_state events;
  cai_agent_runtime_metrics metrics;
  cai_agent_goal_request goal_request;
  cai_agent_goal_snapshot goal;
  http_mock_server server;
  mock_http_expectation script[3];
  cai_agent_runtime *runtime;
  cai_client *client;
  cai_error error;
  int fail, ready, i;
  for (fail = 0; fail < 2; fail++) {
    memset(script, 0, sizeof(script));
    script[0].request_prefix = "POST /v1/responses HTTP/";
    script[0].status = 200;
    script[0].status_text = "OK";
    script[0].content_type = "text/event-stream";
    script[0].body = first;
    script[1] = script[0];
    script[1].required = compact_required;
    script[1].required_count = 2U;
    script[1].forbidden = opaque;
    script[1].forbidden_count = 1U;
    script[1].body = compact;
    script[2] = script[0];
    script[2].required = fail ? original : opaque;
    script[2].required_count = 1U;
    script[2].forbidden = fail ? opaque : original;
    script[2].forbidden_count = 1U;
    if (http_mock_server_open_script(state, "manual_commit", script, 3U,
                                     &server) != 0)
      return;
    memset(&saved, 0, sizeof(saved));
    memset(&events, 0, sizeof(events));
    memset(&store, 0, sizeof(store));
    events.owner = pthread_self();
    store.context = &saved;
    store.commit = test_runtime_session_store_commit;
    store.load_latest = test_runtime_session_store_load;
    store.append_event = test_runtime_session_store_append_event;
    store.load_events_after = test_runtime_session_store_load_events_after;
    cai_error_init(&error);
    cai_client_config_init(&client_config);
    client_config.api_key = "fixture";
    client_config.base_url = server.base_url;
    client_config.http_2_disabled = 1;
    client_config.timeout_ms = 2000L;
    client = NULL;
    runtime = NULL;
    expect_int(state, "manual_commit_client",
               cai_client_open(&client_config, &client, &error), CAI_OK);
    cai_agent_runtime_config_init(&config);
    config.workspace_directory = "/tmp";
    config.model = CAI_MODEL_GPT_5_NANO;
    config.disable_terminal = 1;
    config.session_store = &store;
    config.event_callback = test_runtime_event;
    config.event_context = &events;
    expect_int(state, "manual_commit_open",
               cai_agent_runtime_open(client, &config, &runtime, &error),
               CAI_OK);
    if (runtime != NULL) {
      expect_int(
          state, "manual_commit_submit",
          cai_agent_runtime_submit(runtime, "manual commit source", &error),
          CAI_OK);
      expect_int(state, "manual_commit_first",
                 manual_compact_settle(state, runtime), CAI_AGENT_COMPLETED);
      if (fail) {
        saved.fail_commit_once = 1;
        strcpy(saved.fail_commit_session_id,
               cai_agent_runtime_session_id(runtime));
      }
      expect_int(state, "manual_commit_ready",
                 cai_agent_runtime_can_compact(runtime, &ready, &error),
                 CAI_OK);
      expect_int(state, "manual_commit_ready_value", ready, 1);
      expect_int(state, "manual_commit_request",
                 cai_agent_runtime_compact(runtime, &error), CAI_OK);
      expect_int(state, "manual_commit_finished",
                 manual_compact_settle(state, runtime),
                 fail ? CAI_AGENT_FAILED : CAI_AGENT_COMPLETED);
      expect_substr(state, "manual_commit_history", saved.saved_commit,
                    fail ? "MANUAL_ORIGINAL_ANSWER" : "MANUAL_COMMIT_OPAQUE");
      expect_int(state, "manual_commit_metrics",
                 cai_agent_runtime_get_metrics(runtime, &metrics, &error),
                 CAI_OK);
      expect_int(state, "manual_commit_usage",
                 metrics.session_usage.usage.total_tokens, 27L);
      cai_agent_goal_request_init(&goal_request);
      goal_request.objective = "Exercise an explicit fixture goal budget";
      goal_request.has_token_budget = 1;
      goal_request.token_budget = 1LL;
      expect_int(state, "manual_commit_create_goal",
                 cai_agent_runtime_create_goal(runtime, &goal_request, &error),
                 CAI_OK);
      for (i = 0; i < 100; i++) {
        (void)cai_agent_runtime_pump(runtime, 50L, &error);
        (void)cai_agent_runtime_get_goal(runtime, &goal, &error);
        (void)cai_agent_runtime_can_compact(runtime, &ready, &error);
        if (goal.has_goal && ready)
          break;
      }
      expect_int(state, "manual_commit_goal_ready", goal.has_goal && ready, 1);
      expect_int(state, "manual_commit_followup",
                 cai_agent_runtime_submit(
                     runtime, "continue after manual compaction", &error),
                 CAI_OK);
      expect_int(state, "manual_commit_followup_done",
                 manual_compact_settle(state, runtime), CAI_AGENT_COMPLETED);
      expect_int(state, "manual_commit_final_metrics",
                 cai_agent_runtime_get_metrics(runtime, &metrics, &error),
                 CAI_OK);
      expect_int(state, "manual_commit_final_usage",
                 metrics.session_usage.usage.total_tokens, 39L);
      expect_int(state, "manual_commit_budget_goal",
                 cai_agent_runtime_get_goal(runtime, &goal, &error), CAI_OK);
      expect_str(state, "manual_commit_budget_limited", goal.status,
                 "budget_limited");
      expect_int(state, "manual_commit_budget_reject",
                 cai_agent_runtime_compact(runtime, &error), CAI_ERR_LIMIT);
    }
    cai_agent_runtime_close(runtime);
    cai_client_close(client);
    cai_error_cleanup(&error);
    expect_child_exit(state, "manual_commit", server.pid, &server.child_status);
  }
}
