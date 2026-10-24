static int refusal_cancel_delta(void *context, const char *id, int index,
                                const lonejson_spooled *text,
                                cai_error *error) {
  (void)id;
  (void)index;
  (void)text;
  (void)error;
  (*(int *)context)++;
  return CAI_ERR_CANCELLED;
}
static void test_chat_refusal(test_state *state) {
  static const char refusal[] =
      "data: "
      "{\"id\":\"refuse\",\"choices\":[{\"delta\":{\"content\":null,"
      "\"refusal\":\"Cannot \"}}]}\n\n"
      "data: "
      "{\"choices\":[{\"delta\":{\"refusal\":\"comply.\"},\"finish_reason\":"
      "\"stop\"}]}\n\ndata: [DONE]\n\n";
  static const char mixed[] =
      "data: {\"id\":\"mixed\",\"choices\":[{\"delta\":{\"content\":\"Visible "
      "text.\",\"refusal\":\"Cannot "
      "comply.\"},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
  static const char empty[] =
      "data: "
      "{\"id\":\"empty\",\"choices\":[{\"delta\":{\"content\":null,\"refusal\":"
      "null},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
  static const char invalid[] =
      "data: "
      "{\"id\":\"bad\",\"choices\":[{\"delta\":{\"refusal\":{}},\"finish_"
      "reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
  static const char *required[] = {
      "\"role\":\"assistant\",\"content\":null,\"refusal\":\"Cannot comply.\""};
  static const char *mixed_required[] = {
      "\"content\":\"Visible text.\",\"refusal\":\"Cannot comply.\""};
  mock_http_expectation script[9];
  http_mock_server server;
  cai_client_config config;
  cai_agent_config agent_config;
  cai_client *client = NULL, *limited = NULL;
  cai_agent *agent = NULL;
  cai_session *session = NULL, *restored = NULL;
  cai_response_create_params *params = NULL;
  cai_response *response = NULL;
  cai_stream_sinks sinks;
  cai_sink_callbacks callbacks;
  cai_sink *sink = NULL;
  cai_source *source = NULL;
  write_state visible;
  stream_output_item_state item;
  cai_error error;
  int rc, cancelled = 0;
  size_t step;
  memset(script, 0, sizeof(script));
  for (step = 0U; step < 9U; step++) {
    script[step].request_prefix = "POST /v1/chat/completions HTTP/";
    script[step].content_type = "text/event-stream";
    script[step].body = step == 2U || step == 3U ? mixed : refusal;
  }
  script[2].required = required;
  script[2].required_count = 1U;
  script[3].required = mixed_required;
  script[3].required_count = 1U;
  script[7].body = invalid;
  script[8].body = empty;
  if (http_mock_server_open_script(state, "chat_refusal", script, 9U,
                                   &server) != 0)
    return;
  cai_error_init(&error);
  cai_client_config_init(&config);
  config.base_url = server.base_url;
  config.api_key = "fixture";
  config.api_protocol = CAI_API_PROTOCOL_CHAT_COMPLETIONS;
  config.http_2_disabled = 1;
  config.chat_max_attempts = 1U;
  config.timeout_ms = 1000L;
  rc = cai_client_open(&config, &client, &error);
  cai_agent_config_init(&agent_config);
  agent_config.model = "test-model";
  if (rc == CAI_OK)
    rc = client->new_agent(client, &agent_config, &agent, &error);
  if (rc == CAI_OK)
    rc = agent->new_session(agent, &session, &error);
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = test_write;
  callbacks.context = &visible;
  if (rc == CAI_OK)
    rc = cai_sink_from_callbacks(&callbacks, &sink, &error);
  expect_int(state, "chat_refusal_setup", rc, CAI_OK);
  for (step = 0U; rc == CAI_OK && step < 9U; step++) {
    memset(&visible, 0, sizeof(visible));
    memset(&item, 0, sizeof(item));
    cai_error_cleanup(&error);
    cai_error_init(&error);
    cai_response_create_params_destroy(params);
    params = NULL;
    rc = cai_response_create_params_new(&params, &error);
    if (rc == CAI_OK)
      rc = params->set_model(params, "test-model", &error);
    if (rc == CAI_OK)
      rc = params->add_text(params, "user", "A prompt", &error);
    cai_stream_sinks_init(&sinks);
    sinks.output_text = sink;
    sinks.output_item_done = test_stream_output_item_done;
    sinks.output_item_context = &item;
    if (step == 0U || step == 5U || step == 8U) {
      if (step == 5U) {
        config.json_response_limit_bytes = 8U;
        rc = cai_client_open(&config, &limited, &error);
      }
      if (rc == CAI_OK)
        rc = cai_client_create_response(step == 5U ? limited : client, params,
                                        &response, &error);
      expect_int(state, "chat_refusal_response", rc,
                 step == 5U ? CAI_ERR_LIMIT : CAI_OK);
      if (response != NULL) {
        if (step == 8U)
          expect_int(state, "chat_refusal_empty_getter",
                     response->refusal(response) == NULL, 1);
        else
          expect_str(state, "chat_refusal_getter", response->refusal(response),
                     "Cannot comply.");
        expect_int(state, "chat_refusal_not_normal_text",
                   response->output_text(response) == NULL ||
                       response->output_text(response)[0] == '\0',
                   1);
        cai_response_destroy(response);
        response = NULL;
      }
    } else if (step >= 1U && step <= 3U) {
      if (rc == CAI_OK)
        rc = session->add_user_text(session, "Continue", &error);
      if (rc == CAI_OK)
        rc = session->stream(session, &sinks, &error);
      expect_int(state, "chat_refusal_stream", rc, CAI_OK);
      expect_substr(state, "chat_refusal_visible", visible.buffer,
                    "Cannot comply.");
      expect_substr(state, "chat_refusal_typed_item", item.json,
                    "\"type\":\"refusal\"");
      if (step == 1U) {
        expect_int(state, "chat_refusal_export",
                   session->export_state_source(session, &source, &error),
                   CAI_OK);
        expect_int(state, "chat_refusal_restore_new",
                   agent->new_session(agent, &restored, &error), CAI_OK);
        expect_int(state, "chat_refusal_restore",
                   restored->import_state_source(restored, source, &error),
                   CAI_OK);
        cai_source_close(source);
        source = NULL;
        cai_session_destroy(session);
        session = restored;
        restored = NULL;
      }
    } else {
      if (step == 4U)
        sinks.output_text = NULL;
      if (step == 6U) {
        sinks.output_item_done = NULL;
        sinks.output_text = NULL;
        sinks.output_text_delta = refusal_cancel_delta;
        sinks.output_text_context = &cancelled;
      }
      rc = cai_client_stream_response_with_id(client, params, &sinks, NULL,
                                              NULL, &error);
      expect_int(state, "chat_refusal_callback", rc,
                 step == 6U   ? CAI_ERR_CANCELLED
                 : step == 7U ? CAI_ERR_PROTOCOL
                              : CAI_OK);
      if (step == 4U)
        expect_substr(state, "chat_refusal_item_only", item.json,
                      "\"refusal\":\"Cannot comply.\"");
      if (step == 6U)
        expect_int(state, "chat_refusal_cancelled", cancelled, 1);
    }
    /* The following cases deliberately exercise independent failures. */
    rc = CAI_OK;
  }
  cai_response_create_params_destroy(params);
  cai_sink_close(sink);
  cai_source_close(source);
  cai_session_destroy(session);
  cai_session_destroy(restored);
  cai_agent_destroy(agent);
  cai_client_close(limited);
  cai_client_close(client);
  cai_error_cleanup(&error);
  expect_child_exit(state, "chat_refusal_mock", server.pid,
                    &server.child_status);
}
