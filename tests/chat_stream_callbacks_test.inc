typedef struct chat_callback_capture {
  write_state delta[2], done[2], rendered;
  int deltas[2], completions[2];
  int delta_only, done_only, fail_delta, fail_done, fail_suffix;
  int suffixes, suffixes_at_answer;
  int count_only;
  size_t received_bytes;
} chat_callback_capture;
typedef struct chat_callback_body {
  char *bytes;
  size_t length, capacity;
} chat_callback_body;
static lonejson_status chat_body_append(void *context, const void *bytes,
                                        size_t length, lonejson_error *error) {
  chat_callback_body *body = context;
  char *grown;
  size_t capacity;
  (void)error;
  /* Only fixture wire data is buffered here, with a 4 MiB bound. */
  if (length > 4U * 1024U * 1024U - body->length)
    return LONEJSON_STATUS_OVERFLOW;
  if (length + 1U > body->capacity - body->length) {
    capacity = (body->length + length + 4096U) / 4096U * 4096U;
    grown = realloc(body->bytes, capacity);
    if (grown == NULL)
      return LONEJSON_STATUS_ALLOCATION_FAILED;
    body->bytes = grown;
    body->capacity = capacity;
  }
  memcpy(body->bytes + body->length, bytes, length);
  body->length += length;
  body->bytes[body->length] = '\0';
  return LONEJSON_STATUS_OK;
}
static void chat_body_tool(chat_callback_body *body, int index, int metadata,
                           const char *bytes, size_t length) {
  char prefix[256];
  lonejson_error error;
  lonejson_status status;
  lonejson_error_init(&error);
  snprintf(prefix, sizeof(prefix),
           "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
           "\"index\":%d,",
           index);
  status = chat_body_append(body, prefix, strlen(prefix), &error);
  if (metadata) {
    snprintf(prefix, sizeof(prefix),
             "\"id\":\"call_%d\",\"function\":{\"name\":\"raw\",\"arguments\":",
             index);
  } else {
    strcpy(prefix, "\"function\":{\"arguments\":");
  }
  if (status == LONEJSON_STATUS_OK)
    status = chat_body_append(body, prefix, strlen(prefix), &error);
  if (status == LONEJSON_STATUS_OK)
    status = CAI_LJ->write_json_string_buffer_sink(
        CAI_LJ, bytes, length, chat_body_append, body, &error);
  if (status == LONEJSON_STATUS_OK)
    status =
        chat_body_append(body, "}}]}}]}\n\n", strlen("}}]}}]}\n\n"), &error);
  if (status != LONEJSON_STATUS_OK)
    abort();
}
static void chat_body_finish(chat_callback_body *body) {
  static const char finish[] =
      "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":"
      "\"tool_calls\"}]}\n\ndata: [DONE]\n\n";
  if (chat_body_append(body, finish, sizeof(finish) - 1U, NULL) !=
      LONEJSON_STATUS_OK)
    abort();
}
static lonejson_status chat_count_bytes(void *context, const void *bytes,
                                        size_t length, lonejson_error *error) {
  chat_callback_capture *capture = context;
  const unsigned char *text = bytes;
  size_t i;
  (void)error;
  for (i = 0U; i < length; i++)
    if (text[i] != 'x')
      return LONEJSON_STATUS_CALLBACK_FAILED;
  capture->received_bytes += length;
  return LONEJSON_STATUS_OK;
}
static int chat_callback_delta(void *context, const char *id, int index,
                               const lonejson_spooled *delta,
                               cai_error *error) {
  chat_callback_capture *capture = context;
  char expected[32];
  lonejson_error local;
  (void)error;
  if (index < 1 || index > 2)
    return CAI_ERR_INVALID;
  snprintf(expected, sizeof(expected), "call_%d", index - 1);
  if (strcmp(id, expected) != 0)
    return CAI_ERR_INVALID;
  capture->deltas[index - 1]++;
  lonejson_error_init(&local);
  if (delta->write_to_sink(
          delta,
          capture->count_only ? chat_count_bytes : test_lonejson_capture_sink,
          capture->count_only ? (void *)capture : &capture->delta[index - 1],
          &local) != LONEJSON_STATUS_OK)
    return CAI_ERR_LIMIT;
  return capture->fail_delta ? CAI_ERR_CANCELLED : CAI_OK;
}
static int chat_callback_done(void *context, const char *id, int index,
                              const char *call_id, const char *name,
                              const lonejson_spooled *input, cai_error *error) {
  chat_callback_capture *capture = context;
  lonejson_error local;
  char expected[32];
  (void)error;
  if (index < 1 || index > 2)
    return CAI_ERR_INVALID;
  snprintf(expected, sizeof(expected), "call_%d", index - 1);
  if (strcmp(id, expected) != 0 || strcmp(id, call_id) != 0 ||
      strcmp(name, "raw") != 0)
    return CAI_ERR_INVALID;
  capture->completions[index - 1]++;
  lonejson_error_init(&local);
  if (input->write_to_sink(input, test_lonejson_capture_sink,
                           &capture->done[index - 1],
                           &local) != LONEJSON_STATUS_OK)
    return CAI_ERR_LIMIT;
  return capture->fail_done ? CAI_ERR_INVALID : CAI_OK;
}
static const char *chat_reasoning_suffix(void *context) {
  chat_callback_capture *capture = context;
  capture->suffixes++;
  return "</r>";
}
static int chat_render_write(void *context, const void *bytes, size_t length,
                             cai_error *error) {
  chat_callback_capture *capture = context;
  if (capture->fail_suffix && length == 4U && memcmp(bytes, "</r>", 4U) == 0)
    return CAI_ERR_TRANSPORT;
  return test_write(&capture->rendered, bytes, length, error);
}
static int chat_answer_delta(void *context, const char *id, int index,
                             const lonejson_spooled *text, cai_error *error) {
  chat_callback_capture *capture = context;
  (void)id;
  (void)index;
  (void)text;
  (void)error;
  capture->suffixes_at_answer = capture->suffixes;
  return CAI_OK;
}
static void chat_callback_run(test_state *state, const char *name,
                              const char *body, chat_callback_capture *capture,
                              int expected) {
  mock_http_expectation script;
  chat_callback_body response;
  http_mock_server server;
  cai_client_config config;
  cai_agent_config agent_config;
  cai_client *client = NULL;
  cai_agent *agent = NULL;
  cai_stream_sinks sinks;
  cai_sink_callbacks callbacks;
  cai_sink *sink = NULL;
  cai_custom_tool_format format;
  raw_tool_state raw;
  cai_error error;
  int rc;
  memset(&script, 0, sizeof(script));
  memset(&response, 0, sizeof(response));
  if (chat_body_append(&response, "data: {\"id\":\"chat_callbacks\"}\n\n",
                       strlen("data: {\"id\":\"chat_callbacks\"}\n\n"),
                       NULL) != LONEJSON_STATUS_OK ||
      chat_body_append(&response, body, strlen(body), NULL) !=
          LONEJSON_STATUS_OK)
    abort();
  script.request_prefix = "POST /v1/chat/completions HTTP/";
  script.content_type = "text/event-stream";
  script.body = response.bytes;
  if (http_mock_server_open_script(state, name, &script, 1U, &server) != 0) {
    free(response.bytes);
    return;
  }
  cai_error_init(&error);
  cai_client_config_init(&config);
  config.base_url = server.base_url;
  config.api_key = "fixture";
  config.api_protocol = CAI_API_PROTOCOL_CHAT_COMPLETIONS;
  config.chat_max_attempts = 1U;
  config.timeout_ms = 1000L;
  config.http_2_disabled = 1;
  rc = cai_client_open(&config, &client, &error);
  cai_agent_config_init(&agent_config);
  agent_config.model = "test-model";
  if (rc == CAI_OK)
    rc = client->new_agent(client, &agent_config, &agent, &error);
  memset(&format, 0, sizeof(format));
  memset(&raw, 0, sizeof(raw));
  format.type = "grammar";
  format.syntax = "lark";
  format.definition = "start: /./*";
  if (rc == CAI_OK)
    rc = cai_agent_register_custom_tool(agent, "raw", "Raw input", &format,
                                        test_raw_tool, &raw, &error);
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = chat_render_write;
  callbacks.context = capture;
  if (rc == CAI_OK)
    rc = cai_sink_from_callbacks(&callbacks, &sink, &error);
  cai_stream_sinks_init(&sinks);
  sinks.output_text = sinks.reasoning_summary = sink;
  sinks.output_text_prefix.text = "<a>";
  sinks.output_text_suffix.text = "</a>";
  sinks.reasoning_summary_prefix.text = "<r>";
  sinks.reasoning_summary_suffix.callback = chat_reasoning_suffix;
  sinks.reasoning_summary_suffix.context = capture;
  sinks.output_text_delta = chat_answer_delta;
  sinks.output_text_context = capture;
  if (!capture->done_only)
    sinks.custom_tool_call_input_delta = chat_callback_delta;
  if (!capture->delta_only)
    sinks.custom_tool_call_input_done = chat_callback_done;
  /* Unclassified custom arguments must never leak into regular callbacks. */
  sinks.function_call_arguments_delta = chat_callback_delta;
  sinks.function_call_context = capture;
  sinks.custom_tool_call_context = capture;
  if (rc == CAI_OK)
    rc =
        agent->add_user_text(agent, "Exercise the streaming callbacks", &error);
  if (rc == CAI_OK)
    rc = agent->stream(agent, &sinks, &error);
  expect_int(state, name, rc, expected);
  if (rc != expected && error.message != NULL)
    test_fail(state, name, error.message);
  cai_sink_close(sink);
  cai_agent_destroy(agent);
  cai_client_close(client);
  cai_error_cleanup(&error);
  expect_child_exit(state, name, server.pid, &server.child_status);
  free(response.bytes);
}

static void test_chat_custom_deltas(test_state *state) {
  static const char input[] =
      "{\"input\":\"line\\n\\\"\\\\\\u00dc\\ud83d\\ude80\\u0000tail\"}";
  static const char decoded[] = "line\n\"\\Ü🚀\0tail";
  static const char other[] = "{\"input\":\"second\"}";
  static const char *const invalid[] = {"{}",
                                        "{\"input\":null}",
                                        "{\"input\":42}",
                                        "{\"input\":true}",
                                        "{\"input\":[]}",
                                        "{\"input\":{}}",
                                        "{\"input\":\"\\q\"}",
                                        "{\"input\":\"\\ud800x\"}",
                                        "{\"input\":\"unfinished",
                                        "{\"input\":\"ok\"}x"};
  chat_callback_body body;
  chat_callback_capture capture;
  size_t i, mode;
  for (mode = 0U; mode < 3U; mode++) {
    memset(&body, 0, sizeof(body));
    memset(&capture, 0, sizeof(capture));
    capture.delta_only = mode == 1U;
    for (i = 0U; i < sizeof(input) - 1U; i++) {
      chat_body_tool(&body, 0, i == (mode == 2U ? 12U : 0U), input + i, 1U);
      if (i < sizeof(other) - 1U)
        chat_body_tool(&body, 1, i == 0U, other + i, 1U);
    }
    chat_body_finish(&body);
    chat_callback_run(state, "chat_custom_interleaved", body.bytes, &capture,
                      CAI_OK);
    expect_int(state, "chat_custom_live", capture.deltas[0] > 1, 1);
    expect_int(state, "chat_custom_decoded_size", capture.delta[0].length,
               sizeof(decoded) - 1U);
    if (memcmp(capture.delta[0].buffer, decoded, sizeof(decoded) - 1U) != 0)
      test_fail(state, "chat_custom_decoded", "decoded input differs");
    expect_str(state, "chat_custom_second", capture.delta[1].buffer, "second");
    expect_int(state, "chat_custom_done_once", capture.completions[0],
               capture.delta_only ? 0 : 1);
    if (!capture.delta_only) {
      expect_int(state, "chat_custom_done_bytes", capture.done[0].length,
                 capture.delta[0].length);
      if (memcmp(capture.done[0].buffer, decoded, sizeof(decoded) - 1U) != 0)
        test_fail(state, "chat_custom_done_decoded", "completed input differs");
    }
    free(body.bytes);
  }
  for (i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    memset(&body, 0, sizeof(body));
    memset(&capture, 0, sizeof(capture));
    chat_body_tool(&body, 0, 1, invalid[i], strlen(invalid[i]));
    chat_body_finish(&body);
    chat_callback_run(state, "chat_custom_invalid", body.bytes, &capture,
                      CAI_ERR_PROTOCOL);
    expect_int(state, "chat_custom_invalid_no_done", capture.completions[0], 0);
    free(body.bytes);
  }
  memset(&body, 0, sizeof(body));
  memset(&capture, 0, sizeof(capture));
  capture.fail_delta = 1;
  chat_body_tool(&body, 0, 1, "{\"input\":\"prefix", 16U);
  /* Neither the input string nor the response finishes. The delta callback
   * must already run and interrupt the request, not await JSON completion. */
  chat_callback_run(state, "chat_custom_cancel_live", body.bytes, &capture,
                    CAI_ERR_CANCELLED);
  expect_int(state, "chat_custom_cancel_callback", capture.deltas[0], 1);
  expect_int(state, "chat_custom_cancel_no_done", capture.completions[0], 0);
  free(body.bytes);
  for (mode = 0U; mode < 2U; mode++) {
    memset(&body, 0, sizeof(body));
    memset(&capture, 0, sizeof(capture));
    capture.done_only = mode == 0U;
    capture.fail_done = mode == 1U;
    chat_body_tool(&body, 0, 1, "{\"input\":\"\"}", 12U);
    chat_body_finish(&body);
    chat_callback_run(state, "chat_custom_empty", body.bytes, &capture,
                      capture.fail_done ? CAI_ERR_INVALID : CAI_OK);
    expect_int(state, "chat_custom_empty_delta", capture.deltas[0], 0);
    expect_int(state, "chat_custom_empty_done", capture.completions[0], 1);
    free(body.bytes);
  }
  {
    size_t count = 1024U * 1024U + 17U;
    char *large = malloc(count + 13U);
    if (large == NULL)
      abort();
    memcpy(large, "{\"input\":\"", 10U);
    memset(large + 10U, 'x', count);
    memcpy(large + 10U + count, "\"}", 3U);
    memset(&body, 0, sizeof(body));
    memset(&capture, 0, sizeof(capture));
    capture.delta_only = capture.count_only = 1;
    chat_body_tool(&body, 0, 1, large, count + 12U);
    chat_body_finish(&body);
    chat_callback_run(state, "chat_custom_large_delta", body.bytes, &capture,
                      CAI_OK);
    expect_int(state, "chat_custom_large_bytes", capture.received_bytes, count);
    free(large);
    free(body.bytes);
  }
}
static void test_chat_reasoning_transitions(test_state *state) {
  static const char *const bodies[] = {
      "data: "
      "{\"choices\":[{\"delta\":{\"reasoning_content\":\"thought\",\"content\":"
      "\"answer\"},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n",
      "data: "
      "{\"choices\":[{\"delta\":{\"reasoning_content\":\"thought\"}}]}\n\n"
      "data: {\"choices\":[{\"delta\":{\"content\":\"an\"}}]}\n\n"
      "data: "
      "{\"choices\":[{\"delta\":{\"content\":\"swer\"},\"finish_reason\":"
      "\"stop\"}]}\n\ndata: [DONE]\n\n",
      "data: "
      "{\"choices\":[{\"delta\":{\"reasoning_content\":\"thought\"},\"finish_"
      "reason\":\"stop\"}]}\n\ndata: [DONE]\n\n",
      "data: "
      "{\"choices\":[{\"delta\":{\"content\":\"answer\"},\"finish_reason\":"
      "\"stop\"}]}\n\ndata: [DONE]\n\n",
      "data: "
      "{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: "
      "[DONE]\n\n"};
  static const char *const expected[] = {"<r>thought</r><a>answer</a>",
                                         "<r>thought</r><a>answer</a>",
                                         "<r>thought</r>", "<a>answer</a>", ""};
  chat_callback_capture capture;
  size_t i;
  for (i = 0U; i < sizeof(bodies) / sizeof(bodies[0]); i++) {
    memset(&capture, 0, sizeof(capture));
    chat_callback_run(state, "chat_reasoning_affixes", bodies[i], &capture,
                      CAI_OK);
    expect_str(state, "chat_reasoning_affix_order", capture.rendered.buffer,
               expected[i]);
    expect_int(state, "chat_reasoning_suffix_once", capture.suffixes,
               i < 3U ? 1 : 0);
    if (i < 2U)
      expect_int(state, "chat_reasoning_closed_before_delta",
                 capture.suffixes_at_answer, 1);
  }
  memset(&capture, 0, sizeof(capture));
  capture.fail_suffix = 1;
  chat_callback_run(state, "chat_reasoning_suffix_failure", bodies[0], &capture,
                    CAI_ERR_TRANSPORT);
  expect_str(state, "chat_reasoning_no_answer_after_failure",
             capture.rendered.buffer, "<r>thought");
}
