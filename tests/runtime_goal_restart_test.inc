typedef struct goal_restart_state {
  runtime_event_state events;
  cai_agent_runtime *runtime;
  int cancel;
} goal_restart_state;

static int goal_restart_event(void *context,
                              const cai_agent_runtime_event *event,
                              cai_error *error) {
  goal_restart_state *test = context;
  int rc = test_runtime_event(&test->events, event, error);
  if (rc == CAI_OK && test->cancel &&
      event->type == CAI_AGENT_EVENT_INPUT_BOUNDARY) {
    test->cancel = 0;
    rc = cai_agent_runtime_cancel_turn(test->runtime, error);
  }
  return rc;
}

static void test_agent_runtime_goal_restart(test_state *state) {
  static const char response[] =
      "data: {\"type\":\"response.output_text.delta\",\"delta\":\"progress\"}"
      "\n\ndata: {\"type\":\"response.completed\",\"response\":{"
      "\"id\":\"resp_restart\",\"usage\":{\"input_tokens\":1,"
      "\"output_tokens\":1,\"total_tokens\":2}}}\n\n";
  static const char *existing[] = {"Existing objective", "Completion audit:"};
  static const char *replacement[] = {"Replacement objective",
                                      "Completion audit:"};
  cai_agent_runtime_config config;
  cai_agent_session_store store;
  runtime_session_store_state storage;
  cai_agent_preset preset;
  cai_agent_goal_snapshot goal;
  cai_agent_run_state run_state;
  goal_restart_state test;
  mock_http_expectation script[2];
  http_mock_client mock;
  cai_error error;
  long long used, elapsed;
  int mode, i, started;
  for (mode = 0; mode < 4; mode++) {
    memset(script, 0, sizeof(script));
    for (i = 0; i < 2; i++) {
      script[i].request_prefix = "POST /v1/responses HTTP/";
      script[i].status = 200;
      script[i].status_text = "OK";
      script[i].content_type = "text/event-stream";
      script[i].body = response;
    }
    if (mode >= 2)
      script[0].body = "data: {invalid}\n\n";
    script[1].required = mode % 2 ? replacement : existing;
    script[1].required_count = 2U;
    if (http_mock_client_open_script(state, "goal_restart", script, 2U, &mock))
      return;
    memset(&storage, 0, sizeof(storage));
    storage.commit_json =
        "{\"version\":1,\"model\":\"gpt-5-nano\",\"goal_objective\":"
        "\"Existing objective\",\"goal_status\":\"active\","
        "\"goal_tokens_used\":7,\"goal_elapsed_seconds\":123,\"history\":[]}";
    memset(&store, 0, sizeof(store));
    store.context = &storage;
    store.commit = execution_test_commit;
    store.load_latest = test_runtime_session_store_load;
    store.load_events_after = test_runtime_session_store_load_events_after;
    store.append_event = test_runtime_session_store_append_event;
    memset(&preset, 0, sizeof(preset));
    preset.name = "restart-fixture";
    preset.prompt_version = "fixture";
    preset.default_identity = "Fixture";
    preset.default_model = "gpt-5-nano";
    preset.developer_instructions = "Exercise goal lifecycle.";
    preset.tool_capabilities = CAI_AGENT_PRESET_TOOL_GOAL;
    memset(&test, 0, sizeof(test));
    test.events.owner = pthread_self();
    test.cancel = mode < 2;
    cai_error_init(&error);
    cai_agent_runtime_config_init(&config);
    config.workspace_directory = "/tmp";
    config.session_store = &store;
    config.resume_latest = 1;
    config.preset_descriptor = &preset;
    config.synchronize_input_boundaries = 1;
    config.event_callback = goal_restart_event;
    config.event_context = &test;
    expect_int(
        state, "restart_open",
        cai_agent_runtime_open(mock.client, &config, &test.runtime, &error),
        CAI_OK);
    if (test.runtime != NULL) {
      expect_int(
          state, "restart_initial",
          cai_agent_runtime_continue_goal(test.runtime, &started, &error),
          CAI_OK);
      expect_int(state, "restart_initial_started", started, 1L);
      for (i = 0; i < 100 &&
                  test.events.cancelled_count + test.events.failed_count == 0;
           i++)
        expect_int(state, "restart_first_pump",
                   cai_agent_runtime_pump(test.runtime, 10L, &error), CAI_OK);
      expect_int(state, "restart_stopped_state",
                 cai_agent_runtime_state(test.runtime, &run_state, &error),
                 CAI_OK);
      expect_int(state, "restart_stopped", run_state,
                 mode < 2 ? CAI_AGENT_CANCELLED : CAI_AGENT_FAILED);
      expect_int(state, "restart_previous_goal",
                 cai_agent_runtime_get_goal(test.runtime, &goal, &error),
                 CAI_OK);
      used = goal.tokens_used;
      elapsed = goal.elapsed_seconds;
      expect_int(state, "restart_control",
                 mode % 2 ? cai_agent_runtime_replace_goal(
                                test.runtime, "Replacement objective", &error)
                          : cai_agent_runtime_resume_goal(test.runtime, &error),
                 CAI_OK);
      started = 0;
      for (i = 0; i < 100 && !started; i++) {
        expect_int(state, "restart_control_pump",
                   cai_agent_runtime_pump(test.runtime, 1L, &error), CAI_OK);
        expect_int(
            state, "restart_admit",
            cai_agent_runtime_continue_goal(test.runtime, &started, &error),
            CAI_OK);
      }
      expect_int(state, "restart_resumed_started", started, 1L);
      if (started) {
        for (i = 0; i < 100 && test.events.completed_count == 0; i++)
          expect_int(state, "restart_completion_pump",
                     cai_agent_runtime_pump(test.runtime, 10L, &error), CAI_OK);
        expect_int(state, "restart_completed", test.events.completed_count, 1L);
        expect_int(state, "restart_two_turns", test.events.started_count, 2L);
        expect_int(state, "restart_goal",
                   cai_agent_runtime_get_goal(test.runtime, &goal, &error),
                   CAI_OK);
        expect_str(state, "restart_objective", goal.objective,
                   mode % 2 ? "Replacement objective" : "Existing objective");
        expect_int(state, "restart_accounting", goal.tokens_used,
                   mode % 2 ? 2L : used + 2LL);
        if (!(mode % 2))
          expect_int(state, "restart_elapsed_preserved",
                     goal.elapsed_seconds >= elapsed, 1L);
      } else {
        /* Fail fast without leaving a fixture waiting for a missing request. */
        kill(mock.pid, SIGTERM);
        waitpid(mock.pid, &mock.child_status, 0);
        mock.pid = -1;
      }
      cai_agent_runtime_close(test.runtime);
    }
    http_mock_client_close(state, "goal_restart", &mock);
    cai_error_cleanup(&error);
  }
}
