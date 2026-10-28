/* Covered input acknowledgements may repeat after interrupted commits. */
static void test_agent_runtime_input_ack(test_state *state) {
  static const char *const kinds[] = {"turn_submitted", "turn_queued",
                                      "steering_queued", "goal_continuation"};
  static const char *const invalid[] = {"0", "-1", "2x", "3", "99"};
  cai_agent_runtime_config config;
  cai_agent_session_store store;
  runtime_session_store_state storage;
  cai_agent_preset preset;
  cai_client_config cc;
  cai_client *client = NULL;
  cai_agent_runtime *runtime;
  cai_agent_run_state run_state;
  cai_sink_callbacks callbacks;
  cai_sink *sink;
  write_state output;
  cai_error error;
  size_t variant, i;
  cai_error_init(&error);
  cai_client_config_init(&cc);
  cc.api_key = "fixture";
  cc.base_url = "http://127.0.0.1:1/v1";
  cc.timeout_ms = 100L;
  cc.http_2_disabled = 1;
  cc.logger_disabled = 1;
  expect_int(state, "input_ack_client", cai_client_open(&cc, &client, &error),
             CAI_OK);
  if (client == NULL)
    return;
  memset(&preset, 0, sizeof(preset));
  preset.name = "ack-fixture";
  preset.prompt_version = "fixture";
  preset.default_identity = "Fixture";
  preset.default_model = "gpt-5-nano";
  preset.developer_instructions = "Keep saved conversation context.";
  for (variant = 0; variant < 4U + 5U; variant++) {
    memset(&storage, 0, sizeof(storage));
    storage.commit_json =
        "{\"version\":1,\"history\":[{\"type\":\"message\",\"role\":\"user\","
        "\"content\":[{\"type\":\"input_text\",\"text\":\"Saved question\"}]},"
        "{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{"
        "\"type\":\"output_text\",\"text\":\"Saved answer\"}]}]}";
    storage.load_applied_event_sequence = 5U;
    storage.replay_event_count = 5U;
    for (i = 0; i < 5U; i++) {
      storage.replay_event_sequences[i] = (unsigned long long)i + 1U;
      strcpy(storage.replay_event_types[i],
             i == 0U   ? "input_journal_v2"
             : i == 1U ? kinds[variant < 4U ? variant : 0U]
                       : "input_consumed");
      strcpy(storage.replay_event_data_items[i], i == 1U  ? "Saved question"
                                                 : i > 1U ? "2"
                                                          : "");
    }
    if (variant >= 4U)
      strcpy(storage.replay_event_data_items[2], invalid[variant - 4U]);
    memset(&store, 0, sizeof(store));
    store.context = &storage;
    store.commit = test_runtime_session_store_commit;
    store.load_latest = test_runtime_session_store_load;
    store.load_events_after = test_runtime_session_store_load_events_after;
    store.append_event = test_runtime_session_store_append_event;
    cai_agent_runtime_config_init(&config);
    config.workspace_directory = "/tmp";
    config.session_store = &store;
    config.resume_latest = 1;
    config.preset_descriptor = &preset;
    runtime = NULL;
    expect_int(state, "input_ack_open",
               cai_agent_runtime_open(client, &config, &runtime, &error),
               variant < 4U ? CAI_OK : CAI_ERR_INVALID);
    if (runtime != NULL) {
      expect_int(state, "input_ack_state",
                 cai_agent_runtime_state(runtime, &run_state, &error), CAI_OK);
      expect_int(state, "input_ack_no_reexecution", run_state, CAI_AGENT_IDLE);
      memset(&output, 0, sizeof(output));
      memset(&callbacks, 0, sizeof(callbacks));
      callbacks.write = test_write;
      callbacks.context = &output;
      sink = NULL;
      expect_int(state, "input_ack_sink",
                 cai_sink_from_callbacks(&callbacks, &sink, &error), CAI_OK);
      if (sink != NULL) {
        expect_int(state, "input_ack_export",
                   cai_agent_runtime_export_markdown(runtime, sink, &error),
                   CAI_OK);
        expect_substr(state, "input_ack_question_preserved", output.buffer,
                      "Saved question");
        expect_substr(state, "input_ack_answer_preserved", output.buffer,
                      "Saved answer");
        cai_sink_close(sink);
      }
      cai_agent_runtime_close(runtime);
    }
    cai_error_cleanup(&error);
    cai_error_init(&error);
  }
  cai_client_close(client);
  cai_error_cleanup(&error);
}
