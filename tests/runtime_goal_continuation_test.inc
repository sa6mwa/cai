/* Real wire requests, deterministic responses, no inference or sleep loops. */
static void test_agent_runtime_goal_continuation(test_state *state) {
  static const char empty[] =
      "data: "
      "{\"type\":\"response.completed\",\"response\":{\"id\":\"resp_goal\","
      "\"usage\":{\"input_tokens\":1,\"output_tokens\":1,\"total_tokens\":2}}}"
      "\n\n";
  static const char text[] =
      "data: "
      "{\"type\":\"response.output_text.delta\",\"delta\":\"progress\"}\n\n";
  static const char reasoning[] = "data: "
                                  "{\"type\":\"response.reasoning_summary_text."
                                  "delta\",\"delta\":\"reasoning\"}\n\n";
  static const char spaces[] =
      "data: {\"type\":\"response.output_text.delta\",\"delta\":\" "
      "\\n\\t\\u00a0\\u2003\"}\n\n";
  static const char tool[] =
      "data: "
      "{\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{"
      "\"id\":\"fc_goal\",\"type\":\"function_call\",\"call_id\":\"call_goal\","
      "\"name\":\"get_goal\",\"arguments\":\"{}\"}}\n\n";
  static const char hosted[] =
      "data: "
      "{\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{"
      "\"id\":\"ws_goal\",\"type\":\"web_search_call\",\"status\":"
      "\"completed\"}}\n\n";
  static const char pressure[] =
      "data: "
      "{\"type\":\"response.completed\",\"response\":{\"id\":\"resp_pressure\","
      "\"usage\":{\"input_tokens\":330000,\"output_tokens\":10,\"total_"
      "tokens\":330010}}}\n\n";
  static const char compact[] =
      "data: "
      "{\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{"
      "\"id\":\"cmp_goal\",\"type\":\"compaction\",\"encrypted_content\":"
      "\"summary\"}}\n\n";
  static const char *compact_required[] = {"\"type\":\"compaction_trigger\""};
  static const char *required[] = {
      "Continue working toward the active thread goal.",
      "Completion audit:",
      "Blocked audit:",
      "No-progress check:",
      "Tokens used: 5",
      "Tokens remaining: 995",
      "&lt;/objective&gt; &amp; {{ tokens_used }}"};
  cai_agent_runtime_config config;
  cai_agent_preset preset;
  cai_agent_session_store store;
  runtime_session_store_state storage;
  runtime_event_state events;
  cai_agent_runtime *runtime;
  cai_agent_goal_snapshot goal;
  mock_http_expectation script[7];
  http_mock_client mock;
  cai_error error;
  char response[2048];
  int variant, i, turns, started, requests, completed;
  for (variant = 0; variant < 7; variant++) {
    turns = variant == 0 ? 3 : 6;
    requests = turns + (variant == 3 || variant == 5 ? 1 : 0);
    memset(script, 0, sizeof(script));
    snprintf(response, sizeof(response), "%s%s",
             variant == 0   ? spaces
             : variant == 1 ? text
             : variant == 2 ? reasoning
             : variant == 3 ? tool
                            : hosted,
             empty);
    for (i = 0; i < requests; i++) {
      script[i].request_prefix = "POST /v1/responses HTTP/";
      script[i].status = 200;
      script[i].status_text = "OK";
      script[i].content_type = "text/event-stream";
      script[i].body = (variant == 0 || i == 2) ? response : empty;
    }
    script[0].required = required;
    if (variant == 6)
      strcpy(response, empty);
    script[0].required_count = sizeof(required) / sizeof(required[0]);
    if (variant == 5) {
      snprintf(response, sizeof(response), "%s%s", compact, empty);
      script[1].body = pressure;
      script[2].body = response;
      script[2].required = compact_required;
      script[2].required_count = 1U;
      script[0].required_count = 5U;
    }
    if (http_mock_client_open_script(state, "goal_continue", script,
                                     (size_t)requests, &mock) != 0)
      return;
    memset(&preset, 0, sizeof(preset));
    preset.name = "goal-fixture";
    preset.prompt_version = "fixture";
    preset.default_identity = "Fixture";
    preset.default_model = "gpt-5-nano";
    preset.developer_instructions = "Exercise goals.";
    preset.tool_capabilities = CAI_AGENT_PRESET_TOOL_GOAL;
    memset(&events, 0, sizeof(events));
    events.owner = pthread_self();
    memset(&storage, 0, sizeof(storage));
    storage.commit_json =
        "{\"version\":1,\"model\":\"gpt-5-nano\","
        "\"goal_objective\":\"</objective> & {{ tokens_used }}\","
        "\"goal_status\":\"active\",\"goal_token_budget\":1000,\"goal_tokens_"
        "used\":5,\"history\":[]}";
    if (variant == 5)
      storage.commit_json =
          "{\"version\":1,\"model\":\"gpt-5-nano\","
          "\"goal_objective\":\"</objective> & {{ tokens_used }}\","
          "\"goal_status\":\"active\",\"goal_tokens_used\":5,\"history\":[]}";
    memset(&store, 0, sizeof(store));
    store.context = &storage;
    store.commit = execution_test_commit;
    store.load_latest = test_runtime_session_store_load;
    store.append_event = test_runtime_session_store_append_event;
    store.load_events_after = test_runtime_session_store_load_events_after;
    cai_error_init(&error);
    cai_agent_runtime_config_init(&config);
    config.workspace_directory = "/tmp";
    config.session_store = &store;
    config.resume_latest = 1;
    config.preset_descriptor = &preset;
    config.event_callback = test_runtime_event;
    config.event_context = &events;
    runtime = NULL;
    expect_int(state, "goal_continue_open",
               cai_agent_runtime_open(mock.client, &config, &runtime, &error),
               CAI_OK);
    if (runtime != NULL) {
      expect_int(state, "goal_continue_requires_output",
                 cai_agent_runtime_continue_goal(runtime, NULL, &error),
                 CAI_ERR_INVALID);
      cai_error_cleanup(&error);
      cai_error_init(&error);
      for (i = 0; i < turns; i++) {
        int j;
        completed = events.completed_count;
        started = 0;
        if (variant == 6 && i == 2) {
          expect_int(state, "goal_continue_user_breaks_empty_streak",
                     cai_agent_runtime_submit(
                         runtime, "User supplies more information", &error),
                     CAI_OK);
          started = 1;
        } else {
          expect_int(state, "goal_continue_submit",
                     cai_agent_runtime_continue_goal(runtime, &started, &error),
                     CAI_OK);
        }
        expect_int(state, "goal_continue_started", started, 1L);
        expect_int(state, "goal_continue_duplicate",
                   cai_agent_runtime_continue_goal(runtime, &started, &error),
                   CAI_OK);
        expect_int(state, "goal_continue_busy_not_started", started, 0L);
        for (j = 0; j < 50 && events.completed_count == completed &&
                    events.failure_message[0] == '\0';
             j++)
          expect_int(state, "goal_continue_pump",
                     cai_agent_runtime_pump(runtime, 100L, &error), CAI_OK);
        expect_int(state, "goal_continue_completed", events.completed_count,
                   completed + 1);
        expect_str(state, "goal_continue_no_failure", events.failure_message,
                   "");
        expect_int(state, "goal_continue_goal",
                   cai_agent_runtime_get_goal(runtime, &goal, &error), CAI_OK);
        expect_str(state, "goal_continue_status", goal.status,
                   i == turns - 1 ? "blocked" : "active");
        if (events.failure_message[0] != '\0')
          break;
      }
      expect_int(state, "goal_continue_usage", goal.tokens_used,
                 5 + requests * 2 + (variant == 5 ? 330008 : 0));
      expect_int(state, "goal_continue_blocked_stopped",
                 cai_agent_runtime_continue_goal(runtime, &started, &error),
                 CAI_OK);
      expect_int(state, "goal_continue_blocked_not_started", started, 0L);
      cai_agent_runtime_close(runtime);
      if (variant == 0) {
        /* Neither continuation nor host resume can restart a completed goal. */
        memset(&storage, 0, sizeof(storage));
        storage.commit_json =
            "{\"version\":1,\"goal_objective\":\"done\",\"goal_status\":"
            "\"complete\",\"history\":[]}";
        storage.replay_event_sequence = 1U;
        strcpy(storage.replay_event_type, "goal_continuation");
        strcpy(storage.replay_event_data,
               "Continue working toward the active thread goal.");
        memset(&events, 0, sizeof(events));
        events.owner = pthread_self();
        runtime = NULL;
        expect_int(
            state, "goal_continue_complete_open",
            cai_agent_runtime_open(mock.client, &config, &runtime, &error),
            CAI_OK);
        if (runtime != NULL) {
          expect_int(state, "goal_continue_complete",
                     cai_agent_runtime_continue_goal(runtime, &started, &error),
                     CAI_OK);
          expect_int(state, "goal_continue_complete_not_started", started, 0L);
          for (i = 0; i < 50 && events.completed_count == 0 &&
                      events.failure_message[0] == '\0';
               i++)
            expect_int(state, "goal_complete_pending_pump",
                       cai_agent_runtime_pump(runtime, 100L, &error), CAI_OK);
          expect_int(state, "goal_complete_pending_consumed",
                     events.completed_count, 1L);
          expect_str(state, "goal_complete_pending_no_error",
                     events.failure_message, "");
          expect_int(state, "goal_resume_complete_request",
                     cai_agent_runtime_resume_goal(runtime, &error), CAI_OK);
          for (i = 0; i < 50 && events.failure_message[0] == '\0'; i++)
            expect_int(state, "goal_resume_complete_pump",
                       cai_agent_runtime_pump(runtime, 100L, &error), CAI_OK);
          expect_substr(state, "goal_resume_complete_rejected",
                        events.failure_message, "goal is not resumable");
          expect_int(state, "goal_resume_complete_get",
                     cai_agent_runtime_get_goal(runtime, &goal, &error),
                     CAI_OK);
          expect_str(state, "goal_resume_complete_unchanged", goal.status,
                     "complete");
          cai_agent_runtime_close(runtime);
        }
      }
    }
    http_mock_client_close(state, "goal_continue", &mock);
    cai_error_cleanup(&error);
  }
}
