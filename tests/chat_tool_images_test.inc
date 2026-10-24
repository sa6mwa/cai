/* Validate the message-role constraints that permissive HTTP mocks previously
 * missed. Track tool-result adjacency as well as text-only tool content. */
typedef struct chat_image_schema {
  char role[16], value[32];
  size_t used, calls, pending, messages;
  int content, nontext;
} chat_image_schema;

static int chat_image_path(const lonejson_value_path *path, size_t depth,
                           size_t at, const char *key) {
  return path->segment_count == depth && path->segments[0].len == 8U &&
         memcmp(path->segments[0].data, "messages", 8U) == 0 &&
         path->segments[at].len == strlen(key) &&
         memcmp(path->segments[at].data, key, strlen(key)) == 0;
}

static lonejson_status chat_image_object_begin(void *user,
                                               const lonejson_value_path *path,
                                               lonejson_error *error) {
  chat_image_schema *schema = user;
  (void)error;
  if (chat_image_path(path, 2U, 0U, "messages")) {
    schema->role[0] = '\0';
    schema->calls = 0U;
    schema->content = schema->nontext = 0;
  } else if (chat_image_path(path, 4U, 2U, "tool_calls")) {
    schema->calls++;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status chat_image_object_end(void *user,
                                             const lonejson_value_path *path,
                                             lonejson_error *error) {
  chat_image_schema *schema = user;
  (void)error;
  if (!chat_image_path(path, 2U, 0U, "messages"))
    return LONEJSON_STATUS_OK;
  schema->messages++;
  if (strcmp(schema->role, "tool") == 0) {
    if (schema->pending == 0U || !schema->content || schema->nontext)
      return LONEJSON_STATUS_INVALID_ARGUMENT;
    schema->pending--;
  } else {
    if (schema->pending != 0U)
      return LONEJSON_STATUS_INVALID_ARGUMENT;
    if (strcmp(schema->role, "assistant") == 0)
      schema->pending = schema->calls;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status chat_image_content_begin(void *user,
                                                const lonejson_value_path *path,
                                                lonejson_error *error) {
  chat_image_schema *schema = user;
  (void)error;
  if (chat_image_path(path, 3U, 2U, "content"))
    schema->content = 1;
  schema->used = 0U;
  schema->value[0] = '\0';
  return LONEJSON_STATUS_OK;
}

static lonejson_status chat_image_string_chunk(void *user,
                                               const lonejson_value_path *path,
                                               const char *data, size_t count,
                                               lonejson_error *error) {
  chat_image_schema *schema = user;
  (void)error;
  if (!chat_image_path(path, 3U, 2U, "role") &&
      !chat_image_path(path, 5U, 4U, "type"))
    return LONEJSON_STATUS_OK;
  if (count >= sizeof(schema->value) - schema->used)
    return LONEJSON_STATUS_OVERFLOW;
  memcpy(schema->value + schema->used, data, count);
  schema->used += count;
  schema->value[schema->used] = '\0';
  return LONEJSON_STATUS_OK;
}

static lonejson_status chat_image_string_end(void *user,
                                             const lonejson_value_path *path,
                                             lonejson_error *error) {
  chat_image_schema *schema = user;
  (void)error;
  if (chat_image_path(path, 3U, 2U, "role")) {
    if (schema->used >= sizeof(schema->role))
      return LONEJSON_STATUS_OVERFLOW;
    memcpy(schema->role, schema->value, schema->used + 1U);
  } else if (chat_image_path(path, 5U, 4U, "type") &&
             strcmp(schema->value, "text") != 0) {
    schema->nontext = 1;
  }
  return LONEJSON_STATUS_OK;
}

static int test_chat_image_messages_valid(const char *json) {
  lonejson_path_value_visitor visitor = lonejson_default_path_value_visitor();
  lonejson_error error;
  chat_image_schema schema;
  memset(&schema, 0, sizeof(schema));
  lonejson_error_init(&error);
  visitor.object_begin = chat_image_object_begin;
  visitor.object_end = chat_image_object_end;
  visitor.array_begin = chat_image_content_begin;
  visitor.string_begin = chat_image_content_begin;
  visitor.string_chunk = chat_image_string_chunk;
  visitor.string_end = chat_image_string_end;
  return CAI_LJ->visit_path_value_cstr(CAI_LJ, json, &visitor, &schema,
                                       &error) == LONEJSON_STATUS_OK &&
         schema.messages != 0U && schema.pending == 0U;
}

static void test_chat_image_tool_roundtrip(test_state *state) {
  static const unsigned char png[8192] = {
      0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU, 0x00U, 0x00U,
      0x00U, 0x0dU, 0x49U, 0x48U, 0x44U, 0x52U, 0x00U, 0x00U, 0x00U, 0x01U,
      0x00U, 0x00U, 0x00U, 0x01U, 0x08U, 0x06U, 0x00U, 0x00U, 0x00U, 0x1fU,
      0x15U, 0xc4U, 0x89U, 0x00U, 0x00U, 0x00U, 0x0dU, 0x49U, 0x44U, 0x41U,
      0x54U, 0x08U, 0xd7U, 0x63U, 0xf8U, 0xcfU, 0xc0U, 0xf0U, 0x1fU, 0x00U,
      0x05U, 0x00U, 0x01U, 0xffU, 0x89U, 0x99U, 0x07U, 0x00U, 0x00U, 0x00U,
      0x00U, 0x49U, 0x45U, 0x4eU, 0x44U, 0xaeU, 0x42U, 0x60U, 0x82U};
  static const char calls[] =
      "data: {\"id\":\"images\",\"choices\":[{\"delta\":{\"tool_calls\":["
      "{\"index\":0,\"id\":\"image_call\",\"function\":{\"name\":\"view_"
      "image\","
      "\"arguments\":\"{\\\"path\\\":\\\"pixel.png\\\"}\"}},"
      "{\"index\":1,\"id\":\"text_call\",\"function\":{\"name\":\"echo\","
      "\"arguments\":\"{}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\n"
      "data: [DONE]\n\n";
  static const char answer[] =
      "data: {\"id\":\"answer\",\"choices\":[{\"delta\":{\"content\":\"seen\"},"
      "\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
  static const char *continuation[] = {
      "\"tool_call_id\":\"image_call\"", "\"tool_call_id\":\"text_call\"",
      "\"role\":\"user\",\"content\":[", "data:image/png;base64,iVBORw0KGgo"};
  static const char *replayed[] = {"\"tool_call_id\":\"image_call\"",
                                   "\"tool_call_id\":\"text_call\"",
                                   "continue"};
  static const char *private_payload[] = {"data:image/"};
  static const mock_http_expectation script[] = {
      {"POST /v1/chat/completions HTTP/", NULL, 0U, NULL, 0U, 200, "OK",
       "text/event-stream", NULL, calls},
      {"POST /v1/chat/completions HTTP/", continuation, 4U, NULL, 0U, 200, "OK",
       "text/event-stream", NULL, answer},
      {"POST /v1/chat/completions HTTP/", replayed, 3U, private_payload, 1U,
       200, "OK", "text/event-stream", NULL, answer}};
  http_mock_server server;
  cai_client_config config;
  cai_agent_config agent_config;
  cai_view_image_tool_config view_config;
  cai_client *client = NULL;
  cai_agent *agent = NULL;
  cai_session *session = NULL, *resumed = NULL;
  cai_source *source = NULL;
  cai_sink *sink = NULL;
  cai_sink_callbacks callbacks;
  cai_stream_sinks sinks;
  cai_response_create_params *params = NULL;
  cai_chat_upload *upload = NULL;
  failing_callback_state echo;
  write_state output;
  cai_error error;
  char workspace[] = CAI_TEST_BINARY_DIR "/test-tmp/chat-image-XXXXXX";
  char path[PATH_MAX];
  int rc;

  expect_int(
      state, "chat_image_accept_user_image",
      test_chat_image_messages_valid(
          "{\"messages\":[{\"role\":\"assistant\",\"tool_calls\":[{}]},"
          "{\"role\":\"tool\",\"content\":[{\"type\":\"text\","
          "\"text\":\"ok\"}]},{\"role\":\"user\",\"content\":[{"
          "\"type\":\"image_url\",\"image_url\":{\"url\":\"image\"}}]}]}"),
      1);
  expect_int(
      state, "chat_image_reject_tool_image",
      test_chat_image_messages_valid(
          "{\"messages\":[{\"role\":\"assistant\",\"tool_calls\":[{}]},"
          "{\"role\":\"tool\",\"content\":[{\"type\":\"image_url\"}]}]}"),
      0);
  expect_int(
      state, "chat_image_reject_interleaved_user",
      test_chat_image_messages_valid(
          "{\"messages\":[{\"role\":\"assistant\",\"tool_calls\":[{},{}]},"
          "{\"role\":\"tool\",\"content\":\"ok\"},"
          "{\"role\":\"user\",\"content\":\"image\"},"
          "{\"role\":\"tool\",\"content\":\"ok\"}]}"),
      0);
  cai_error_init(&error);
  rc = cai_response_create_params_new(&params, &error);
  if (rc == CAI_OK)
    rc = params->set_model(params, "test-model", &error);
  if (rc == CAI_OK)
    rc = params->add_function_call_output_file_id(params, "file_call",
                                                  "file-id", NULL, &error);
  expect_int(state, "chat_image_file_setup", rc, CAI_OK);
  if (rc == CAI_OK)
    expect_int(state, "chat_image_reject_unsupported_file",
               cai_chat_upload_open(params, &upload, &error), CAI_ERR_INVALID);
  expect_int(state, "chat_image_no_unsupported_upload", upload == NULL, 1);
  cai_chat_upload_close(upload, NULL);
  cai_response_create_params_destroy(params);
  cai_error_cleanup(&error);
  if (mkdtemp(workspace) == NULL) {
    test_fail(state, "chat_image_workspace", "mkdtemp failed");
    return;
  }
  snprintf(path, sizeof(path), "%s/pixel.png", workspace);
  write_bytes_or_die(path, png, sizeof(png));
  if (http_mock_server_open_script(state, "chat_image_roundtrip", script, 3U,
                                   &server) != 0) {
    unlink(path);
    rmdir(workspace);
    return;
  }
  cai_error_init(&error);
  cai_client_config_init(&config);
  config.api_key = "fixture";
  config.base_url = server.base_url;
  config.api_protocol = CAI_API_PROTOCOL_CHAT_COMPLETIONS;
  config.http_2_disabled = 1;
  config.chat_max_attempts = 1U;
  config.timeout_ms = 1000L;
  cai_agent_config_init(&agent_config);
  agent_config.model = "test-model";
  agent_config.disable_auto_compaction = 1;
  memset(&view_config, 0, sizeof(view_config));
  view_config.root_path = view_config.default_workdir = workspace;
  memset(&callbacks, 0, sizeof(callbacks));
  memset(&output, 0, sizeof(output));
  memset(&echo, 0, sizeof(echo));
  callbacks.write = test_write;
  callbacks.context = &output;
  cai_stream_sinks_init(&sinks);
  rc = cai_sink_from_callbacks(&callbacks, &sink, &error);
  sinks.output_text = sink;
  if (rc == CAI_OK)
    rc = cai_client_open(&config, &client, &error);
  if (rc == CAI_OK)
    rc = cai_client_new_agent(client, &agent_config, &agent, &error);
  if (rc == CAI_OK)
    rc = cai_agent_register_view_image_tool(agent, &view_config, &error);
  if (rc == CAI_OK)
    rc = cai_agent_register_raw_tool(agent, "echo", "Echo",
                                     "{\"type\":\"object\"}", 0,
                                     test_chat_counting_echo, &echo, &error);
  if (rc == CAI_OK)
    rc = cai_agent_new_session(agent, &session, &error);
  if (rc == CAI_OK)
    rc = cai_session_add_user_text(session, "inspect pixel.png and echo",
                                   &error);
  if (rc == CAI_OK)
    rc = cai_session_stream_auto(session, NULL, &sinks, &error);
  expect_int(state, "chat_image_continued", rc, CAI_OK);
  expect_str(state, "chat_image_answer", output.buffer, "seen");
  expect_int(state, "chat_image_echo_once", echo.calls, 1);
  if (rc == CAI_OK)
    rc = cai_session_export_state_source(session, &source, &error);
  if (rc == CAI_OK)
    rc = cai_agent_new_session(agent, &resumed, &error);
  if (rc == CAI_OK)
    rc = cai_session_import_state_source(resumed, source, &error);
  if (rc == CAI_OK)
    rc = cai_session_add_user_text(resumed, "continue", &error);
  memset(&output, 0, sizeof(output));
  if (rc == CAI_OK)
    rc = cai_session_stream_auto(resumed, NULL, &sinks, &error);
  expect_int(state, "chat_image_resumed", rc, CAI_OK);
  expect_str(state, "chat_image_resumed_answer", output.buffer, "seen");
  expect_int(state, "chat_image_not_reexecuted", echo.calls, 1);
  cai_source_close(source);
  cai_sink_close(sink);
  cai_session_destroy(resumed);
  cai_session_destroy(session);
  cai_agent_destroy(agent);
  cai_client_close(client);
  cai_error_cleanup(&error);
  expect_child_exit(state, "chat_image_roundtrip", server.pid,
                    &server.child_status);
  unlink(path);
  rmdir(workspace);
}
