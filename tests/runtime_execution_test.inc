/* Public runtime/store/tool integration: no event receiver is required. */
static int execution_test_checkpoint(void *, const char *, const char *,
                                     cai_source *, unsigned long long,
                                     cai_error *);
static void test_execution_record(test_state *state) {
  static const char valid[] =
      "{\"kind\":\"tool.partial_output\",\"session_"
      "id\":\"session\",\"tool_name\":\"probe\\u001b[31m\","
      "\"call_id\":\"call\",\"timestamp_ns\":1,"
      "\"offset\":0,\"final\":true,\"line_start\":true,"
      "\"data\":\"first\\n```\\u001b[31m\\u009bλ\"}";
  static const char *invalid[] = {
      "{}",
      "[]",
      "{",
      "{\"kind\":\"tool.failed\"}",
      "{\"kind\":\"tool.failed\",\"session_id\":\"s\",\"timestamp_ns\":1,"
      "\"offset\":-1,\"final\":true,\"line_start\":true,\"data\":\"\"}",
      "{\"kind\":\"tool.output\",\"session_id\":\"s\",\"timestamp_ns\":1,"
      "\"offset\":0,\"final\":true,\"line_start\":true,\"data\":"
      "\"a\\u0000b\"}",
      "{\"kind\":\"tool.output\",\"session_id\":\"s\",\"timestamp_ns\":1,"
      "\"offset\":0,\"final\":true,\"line_start\":true,\"data\":\"\300\"}"};
  cai_execution_record record;
  cai_sink_callbacks callbacks;
  cai_sink *sink = NULL;
  cai_error error;
  write_state output;
  char oversized[CAI_EXECUTION_RECORD_MAX_BYTES + 2U];
  char payload[CAI_EXECUTION_CHUNK_BYTES + 2U];
  cai_buffer_builder document;
  size_t i;
  cai_error_init(&error);
  cai_execution_record_init(&record);
  expect_int(state, "execution_parse",
             cai_execution_record_parse(valid, &record, &error), CAI_OK);
  expect_str(state, "execution_id", record.call_id, "call");
  expect_str(state, "execution_original_controls", record.data,
             "first\n```\033[31m\302\233λ");
  memset(&output, 0, sizeof(output));
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = test_write;
  callbacks.context = &output;
  expect_int(state, "execution_sink",
             cai_sink_from_callbacks(&callbacks, &sink, &error), CAI_OK);
  expect_int(state, "execution_markdown",
             cai_execution_record_write_markdown(&record, 0, sink, &error),
             CAI_OK);
  expect_substr(state, "execution_indented_fence", output.buffer, "    ```");
  expect_substr(state, "execution_safe_escape", output.buffer,
                "\\u001b[31m\\u009bλ");
  expect_substr(state, "execution_safe_heading", output.buffer,
                "Partial tool result: probe\\u001b[31m");
  if (strchr(output.buffer, '\033') != NULL)
    test_fail(state, "execution_no_terminal_control",
              "raw ESC in rendered output");
  cai_sink_close(sink);
  sink = NULL;
  expect_int(state, "execution_replace_owned",
             cai_execution_record_parse(valid, &record, &error), CAI_OK);
  expect_int(state, "execution_failed_parse",
             cai_execution_record_parse("{}", &record, &error),
             CAI_ERR_PROTOCOL);
  expect_str(state, "execution_preserved_on_failure", record.call_id, "call");
  cai_execution_record_cleanup(&record);
  for (i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    expect_int(state, "execution_invalid",
               cai_execution_record_parse(invalid[i], &record, &error),
               CAI_ERR_PROTOCOL);
    if (i >= 5U)
      expect_substr(state, "execution_invalid_text_detail", error.detail,
                    "invalid UTF-8 or embedded NUL");
    cai_execution_record_cleanup(&record);
    cai_error_cleanup(&error);
    cai_error_init(&error);
  }
  memset(oversized, ' ', sizeof(oversized) - 1U);
  oversized[sizeof(oversized) - 1U] = '\0';
  expect_int(state, "execution_encoded_limit",
             cai_execution_record_parse(oversized, &record, &error),
             CAI_ERR_LIMIT);
  memset(payload, 'x', sizeof(payload) - 1U);
  payload[sizeof(payload) - 1U] = '\0';
  memset(&document, 0, sizeof(document));
  (void)cai_buffer_append_cstr(
      &document,
      "{\"kind\":\"tool.output\",\"session_id\":\"s\",\"timestamp_ns\":1,"
      "\"offset\":0,\"final\":true,\"line_start\":true,\"data\":\"",
      &error);
  (void)cai_buffer_append_cstr(&document, payload, &error);
  (void)cai_buffer_append_cstr(&document, "\"}", &error);
  expect_int(state, "execution_payload_limit",
             cai_execution_record_parse(document.data, &record, &error),
             CAI_ERR_PROTOCOL);
  cai_free_mem(NULL, document.data);
  cai_execution_record_cleanup(&record);
  cai_error_cleanup(&error);
}

typedef struct execution_review_state {
  runtime_session_store_state base;
  pthread_mutex_t lock;
  pthread_cond_t ready;
  char parent_id[CAI_AGENT_SESSION_ID_MAX];
  int done, parent_records, child_records, child_text, child_markdown;
} execution_review_state;
static int execution_review_append(void *context, const char *scope,
                                   const char *id,
                                   const cai_agent_session_event *event,
                                   cai_error *error) {
  execution_review_state *s = context;
  cai_execution_record record;
  int rc;
  (void)scope;
  if (strcmp(event->type, "execution") != 0 &&
      strcmp(event->type, "subagent") != 0)
    return CAI_OK;
  cai_execution_record_init(&record);
  rc = cai_execution_record_parse(event->data, &record, error);
  if (rc == CAI_OK && strcmp(event->type, "subagent") == 0) {
    pthread_mutex_lock(&s->lock);
    if (strcmp(id, s->parent_id) == 0 && record.subagent != NULL &&
        strcmp(record.subagent, "review") == 0 && record.source_sequence > 0U) {
      if (strcmp(record.kind, "assistant_text_delta") == 0)
        s->child_text++;
      if (strcmp(record.kind, "review_markdown_delta") == 0)
        s->child_markdown++;
    }
    pthread_mutex_unlock(&s->lock);
    cai_execution_record_cleanup(&record);
    return rc;
  }
  if (rc == CAI_OK && record.parent_session_id != NULL) {
    pthread_mutex_lock(&s->lock);
    if (strcmp(id, s->parent_id) == 0)
      s->parent_records++;
    else {
      s->child_records++;
      if (strcmp(record.kind, "turn.completed") == 0) {
        s->done = 1;
        pthread_cond_broadcast(&s->ready);
      }
    }
    pthread_mutex_unlock(&s->lock);
  }
  cai_execution_record_cleanup(&record);
  return rc;
}

static void test_execution_poll_review(test_state *state) {
  static const char response[] =
      "data: "
      "{\"type\":\"response.output_text.delta\",\"delta\":\"{\\\"findings\\\":["
      "],\\\"overall_correctness\\\":\\\"patch is "
      "correct\\\",\\\"overall_explanation\\\":\\\"Fixture "
      "review\\\",\\\"overall_confidence_score\\\":1}\"}\n\n"
      "data: "
      "{\"type\":\"response.completed\",\"response\":{\"id\":\"resp_review_"
      "execution\"}}\n\n";
  static const mock_http_expectation script[] = {
      {"POST /v1/responses HTTP/", NULL, 0U, NULL, 0U, 200, "OK",
       "text/event-stream", NULL, response}};
  http_mock_client mock;
  execution_review_state s;
  cai_agent_session_store store;
  cai_agent_runtime_config config;
  cai_agent_runtime *parent = NULL, *review = NULL;
  cai_agent_review_request request;
  cai_agent_run_state run_state;
  cai_error error;
  struct timespec deadline;
  struct pollfd event_fd;
  int i;
  if (http_mock_client_open_script(state, "execution_poll_review", script, 1U,
                                   &mock) != 0)
    return;
  memset(&s, 0, sizeof(s));
  pthread_mutex_init(&s.lock, NULL);
  pthread_cond_init(&s.ready, NULL);
  memset(&store, 0, sizeof(store));
  store.context = &s;
  store.checkpoint = execution_test_checkpoint;
  store.load_latest = test_runtime_session_store_load;
  store.append_event = execution_review_append;
  store.load_events_after = test_runtime_session_store_load_events_after;
  cai_error_init(&error);
  cai_agent_runtime_config_init(&config);
  config.workspace_directory = "/tmp";
  config.session_store = &store;
  config.record_transcript = 1;
  config.disable_default_session_store = 1;
  config.disable_chrome = config.disable_web_search = config.disable_terminal =
      1;
  expect_int(state, "execution_poll_parent",
             cai_agent_runtime_open(mock.client, &config, &parent, &error),
             CAI_OK);
  if (parent != NULL) {
    snprintf(s.parent_id, sizeof(s.parent_id), "%s",
             cai_agent_runtime_session_id(parent));
    cai_agent_review_request_init(&request);
    request.target = CAI_AGENT_REVIEW_CUSTOM;
    request.instructions = "Review the fixture";
    expect_int(
        state, "execution_poll_review",
        cai_agent_runtime_start_review(parent, &request, &review, &error),
        CAI_OK);
    if (review != NULL) {
      clock_gettime(CLOCK_REALTIME, &deadline);
      deadline.tv_sec++;
      pthread_mutex_lock(&s.lock);
      while (!s.done &&
             pthread_cond_timedwait(&s.ready, &s.lock, &deadline) == 0) {
      }
      expect_int(state, "execution_poll_finished_without_pump", s.done, 1L);
      pthread_mutex_unlock(&s.lock);
      memset(&event_fd, 0, sizeof(event_fd));
      event_fd.events = POLLIN;
      (void)cai_agent_runtime_wakeup_fd(review, &event_fd.fd, &error);
      expect_int(state, "execution_poll_no_hidden_consumer",
                 poll(&event_fd, 1U, 0), 0L);
      run_state = CAI_AGENT_SAMPLING;
      for (i = 0; i < 30 && run_state == CAI_AGENT_SAMPLING; i++) {
        (void)cai_agent_runtime_state(review, &run_state, &error);
        if (run_state == CAI_AGENT_SAMPLING)
          (void)cai_agent_runtime_pump(review, 100L, &error);
      }
      expect_int(state, "execution_poll_review_completed", run_state,
                 CAI_AGENT_COMPLETED);
      pthread_mutex_lock(&s.lock);
      expect_int(state, "execution_poll_parent_has_stack", s.parent_records > 0,
                 1L);
      expect_int(state, "execution_poll_complete_mirror", s.parent_records,
                 s.child_records);
      expect_int(state, "execution_poll_child_text", s.child_text > 0, 1L);
      expect_int(state, "execution_poll_child_markdown", s.child_markdown > 0,
                 1L);
      pthread_mutex_unlock(&s.lock);
      expect_int(state, "execution_poll_handoff",
                 cai_agent_runtime_finish_review(parent, review, &error),
                 CAI_OK);
      cai_agent_runtime_close(review);
    }
    cai_agent_runtime_close(parent);
  }
  pthread_cond_destroy(&s.ready);
  pthread_mutex_destroy(&s.lock);
  cai_error_cleanup(&error);
  http_mock_client_close(state, "execution_poll_review", &mock);
}

typedef struct execution_test_node {
  struct execution_test_node *next;
  unsigned long long sequence;
  char *json;
} execution_test_node;
typedef struct execution_test_state {
  runtime_session_store_state base;
  unsigned long long sequence;
  int mode, calls, records, invalid, saw_failure, saw_detail, saw_request;
  int saw_partial, saw_completed, compaction_started, compaction_completed;
  int compaction_failed, request_failed;
  char arguments[20000], output[20000], partial[256];
  size_t arguments_length, output_length, partial_length;
  execution_test_node *first, *last;
} execution_test_state;

static int execution_test_append(void *context, const char *scope,
                                 const char *id,
                                 const cai_agent_session_event *event,
                                 cai_error *error) {
  execution_test_state *s = context;
  cai_execution_record record;
  execution_test_node *node;
  char *target = NULL;
  size_t *length = NULL, n, capacity = 0U;
  int rc;
  (void)scope;
  if (event->sequence <= s->sequence)
    s->invalid = 1;
  if (strcmp(event->type, "execution") != 0) {
    s->sequence = event->sequence;
    return CAI_OK;
  }
  cai_execution_record_init(&record);
  rc = cai_execution_record_parse(event->data, &record, error);
  if (rc != CAI_OK) {
    s->invalid = 1;
    cai_execution_record_cleanup(&record);
    return rc;
  }
  s->records++;
  if (strcmp(id, record.session_id) != 0 || record.timestamp_ns == 0U ||
      record.source_sequence != event->sequence)
    s->invalid = 1;
  if ((s->mode == 2 && strcmp(record.kind, "tool.arguments") == 0) ||
      (s->mode == 4 && strcmp(record.kind, "compaction.started") == 0)) {
    cai_execution_record_cleanup(&record);
    return cai_set_error(error, CAI_ERR_TRANSPORT, "execution append failure");
  }
  s->sequence = event->sequence;
  node = calloc(1U, sizeof(*node));
  if (node == NULL) {
    cai_execution_record_cleanup(&record);
    return CAI_ERR_NOMEM;
  }
  node->sequence = event->sequence;
  node->json = strdup(event->data);
  if (node->json == NULL) {
    free(node);
    cai_execution_record_cleanup(&record);
    return CAI_ERR_NOMEM;
  }
  if (s->last != NULL)
    s->last->next = node;
  else
    s->first = node;
  s->last = node;
  if (record.field == NULL) {
    if (strcmp(record.kind, "compaction.started") == 0)
      s->compaction_started++;
    if (strcmp(record.kind, "compaction.completed") == 0)
      s->compaction_completed++;
    if (strcmp(record.kind, "compaction.failed") == 0)
      s->compaction_failed++;
    if (strcmp(record.kind, "request.failed") == 0)
      s->request_failed++;
  }
  if (strcmp(record.kind, "tool.arguments") == 0) {
    target = s->arguments;
    length = &s->arguments_length;
    capacity = sizeof(s->arguments);
  } else if (strcmp(record.kind, "tool.output") == 0) {
    target = s->output;
    length = &s->output_length;
    capacity = sizeof(s->output);
  } else if (strcmp(record.kind, "tool.partial_output") == 0) {
    target = s->partial;
    length = &s->partial_length;
    capacity = sizeof(s->partial);
    s->saw_partial = record.final;
  }
  if (target != NULL) {
    n = strlen(record.data);
    if (record.offset != *length || *length + n >= capacity)
      s->invalid = 1;
    else {
      memcpy(target + *length, record.data, n + 1U);
      *length += n;
    }
  }
  if (strcmp(record.kind, "tool.failed") == 0) {
    if (record.code != CAI_ERR_INVALID || record.http_status != 422L)
      s->invalid = 1;
    s->saw_failure = 1;
    if (record.field != NULL && strcmp(record.field, "detail") == 0 &&
        strcmp(record.data, "complete error detail") == 0)
      s->saw_detail = 1;
    if (record.field != NULL && strcmp(record.field, "request_id") == 0 &&
        strcmp(record.data, "req_execution_probe") == 0)
      s->saw_request = 1;
  }
  if (strcmp(record.kind, "turn.completed") == 0)
    s->saw_completed = 1;
  cai_execution_record_cleanup(&record);
  return CAI_OK;
}
static int execution_test_load(void *context, const char *scope, const char *id,
                               unsigned long long after,
                               cai_agent_session_event_fn visit, void *user,
                               cai_error *error) {
  execution_test_state *s = context;
  execution_test_node *node;
  cai_agent_session_event event;
  int rc;
  (void)scope;
  (void)id;
  for (node = s->first; node != NULL; node = node->next) {
    if (node->sequence <= after)
      continue;
    event.sequence = node->sequence;
    event.type = "execution";
    event.data = node->json;
    rc = visit(user, &event, error);
    if (rc != CAI_OK)
      return rc;
  }
  return CAI_OK;
}
static void execution_test_cleanup(execution_test_state *s) {
  execution_test_node *node;
  while (s->first != NULL) {
    node = s->first;
    s->first = node->next;
    free(node->json);
    free(node);
  }
}
typedef struct execution_test_inspection {
  int heading, arguments, controls;
} execution_test_inspection;
static int execution_test_inspect(void *context, const void *bytes,
                                  size_t count, cai_error *error) {
  execution_test_inspection *out = context;
  static const char heading[] = "\n\n## Execution history\n";
  static const char arguments[] = "Tool arguments";
  size_t i;
  (void)error;
  for (i = 0U; i + sizeof(heading) - 1U <= count; i++)
    if (memcmp((const char *)bytes + i, heading, sizeof(heading) - 1U) == 0)
      out->heading = 1;
  if (count == sizeof(arguments) - 1U && memcmp(bytes, arguments, count) == 0)
    out->arguments = 1;
  if (count != 0U && memchr(bytes, '\033', count) != NULL)
    out->controls = 1;
  return CAI_OK;
}
static int execution_test_checkpoint(void *context, const char *scope,
                                     const char *id, cai_source *source,
                                     unsigned long long sequence,
                                     cai_error *error) {
  char bytes[4096];
  (void)context;
  (void)scope;
  (void)id;
  (void)sequence;
  while (cai_source_read(source, bytes, sizeof(bytes), error) != 0U) {
  }
  return error->code;
}
static int execution_test_tool(void *context, const char *arguments,
                               cai_sink *sink, cai_error *error) {
  execution_test_state *s = context;
  size_t i;
  int rc;
  s->calls++;
  if (s->mode == 1) {
    static const char partial[] = "partial output λ\033[31m";
    rc = cai_sink_write(sink, partial, sizeof(partial) - 1U, error);
    if (rc != CAI_OK)
      return rc;
    cai_set_error(error, CAI_ERR_INVALID, "execution tool failed");
    error->http_status = 422;
    error->detail = strdup("complete error detail");
    error->server_code = strdup("execution_probe_error");
    error->request_id = strdup("req_execution_probe");
    return CAI_ERR_INVALID;
  }
  for (i = 0U; arguments[i] != '\0'; i++) {
    rc = cai_sink_write(sink, arguments + i, 1U, error);
    if (rc != CAI_OK)
      return rc;
  }
  return CAI_OK;
}
static int execution_test_register(void *context, cai_agent *agent,
                                   cai_error *error) {
  return cai_agent_register_raw_tool(agent, "execution_probe",
                                     "Execution probe", "{\"type\":\"object\"}",
                                     0, execution_test_tool, context, error);
}
static void test_agent_runtime_execution(test_state *state) {
  static const char final[] =
      "data: {\"type\":\"response.output_text.delta\",\"delta\":\"done\"}\n\n"
      "data: "
      "{\"type\":\"response.completed\",\"response\":{\"id\":\"resp_execution_"
      "done\"}}\n\n";
  cai_buffer_builder args, response;
  char padding[4087];
  cai_error error;
  cai_agent_session_store store;
  cai_agent_runtime_config config;
  cai_agent_runtime *runtime;
  cai_agent_run_state run_state;
  execution_test_state s;
  http_mock_client mock;
  mock_http_expectation script[2];
  int mode, i;
  cai_error_init(&error);
  memset(&args, 0, sizeof(args));
  memset(&response, 0, sizeof(response));
  memset(padding, 'x', sizeof(padding) - 1U);
  padding[sizeof(padding) - 1U] = '\0';
  (void)cai_buffer_append_cstr(&args, "{\"text\":\"", &error);
  (void)cai_buffer_append_cstr(&args, padding, &error);
  (void)cai_buffer_append_cstr(&args, "λ", &error);
  (void)cai_buffer_append_cstr(&args, padding, &error);
  (void)cai_buffer_append_cstr(&args, "\"}", &error);
  (void)cai_buffer_append_cstr(
      &response,
      "data: "
      "{\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{"
      "\"type\":\"function_call\",\"id\":\"fc_execution\",\"call_id\":\"call_"
      "execution\","
      "\"name\":\"execution_probe\",\"arguments\":",
      &error);
  (void)cai_buffer_append_json_string(&response, args.data, &error);
  (void)cai_buffer_append_cstr(&response,
                               "}}\n\ndata: "
                               "{\"type\":\"response.completed\",\"response\":{"
                               "\"id\":\"resp_execution\"}}\n\n",
                               &error);
  memset(script, 0, sizeof(script));
  script[0].request_prefix = script[1].request_prefix =
      "POST /v1/responses HTTP/";
  script[0].status = script[1].status = 200;
  script[0].status_text = script[1].status_text = "OK";
  script[0].content_type = script[1].content_type = "text/event-stream";
  script[0].body = response.data;
  script[1].body = final;
  for (mode = 0; mode < 3; mode++) {
    memset(&s, 0, sizeof(s));
    s.mode = mode;
    if (http_mock_client_open_script(state, "runtime_execution", script,
                                     mode == 2 ? 1U : 2U, &mock) != 0)
      break;
    memset(&store, 0, sizeof(store));
    store.context = &s;
    store.checkpoint = execution_test_checkpoint;
    store.load_latest = test_runtime_session_store_load;
    store.append_event = execution_test_append;
    store.load_events_after = execution_test_load;
    cai_agent_runtime_config_init(&config);
    config.workspace_directory = "/tmp";
    config.session_store = &store;
    config.disable_default_session_store = 1;
    config.disable_terminal = config.disable_chrome =
        config.disable_web_search = 1;
    config.record_transcript = 1;
    config.register_agent_tools = execution_test_register;
    config.register_agent_tools_context = &s;
    runtime = NULL;
    expect_int(state, "execution_open",
               cai_agent_runtime_open(mock.client, &config, &runtime, &error),
               CAI_OK);
    if (runtime != NULL) {
      expect_int(state, "execution_submit",
                 cai_agent_runtime_submit(runtime, "run probe", &error),
                 CAI_OK);
      run_state = CAI_AGENT_SAMPLING;
      for (i = 0; i < 100 && run_state == CAI_AGENT_SAMPLING; i++) {
        expect_int(state, "execution_pump",
                   cai_agent_runtime_pump(runtime, 100L, &error), CAI_OK);
        (void)cai_agent_runtime_state(runtime, &run_state, &error);
        if (run_state == CAI_AGENT_DISPATCHING_TOOL)
          run_state = CAI_AGENT_SAMPLING;
      }
      expect_int(state, "execution_terminal", run_state,
                 mode == 2 ? CAI_AGENT_FAILED : CAI_AGENT_COMPLETED);
      expect_int(state, "execution_valid", s.invalid, 0L);
      expect_int(state, "execution_tool_calls", s.calls, mode == 2 ? 0L : 1L);
      if (mode != 2)
        expect_str(state, "execution_exact_arguments", s.arguments, args.data);
      if (mode == 0)
        expect_str(state, "execution_exact_output", s.output, args.data);
      if (mode == 1) {
        expect_int(state, "execution_full_error",
                   s.saw_failure && s.saw_detail && s.saw_request &&
                       s.saw_partial,
                   1L);
        expect_str(state, "execution_partial_output", s.partial,
                   "partial output λ\033[31m");
      }
      if (mode != 2)
        expect_int(state, "execution_complete_without_receiver",
                   s.saw_completed, 1L);
      if (mode == 0) {
        cai_sink_callbacks callbacks;
        cai_sink *sink = NULL;
        execution_test_inspection out;
        memset(&out, 0, sizeof(out));
        memset(&callbacks, 0, sizeof(callbacks));
        callbacks.write = execution_test_inspect;
        callbacks.context = &out;
        expect_int(state, "execution_inspection_sink",
                   cai_sink_from_callbacks(&callbacks, &sink, &error), CAI_OK);
        expect_int(state, "execution_handover_export",
                   cai_agent_runtime_export_markdown(runtime, sink, &error),
                   CAI_OK);
        expect_int(state, "execution_handover_excludes_sideband", out.heading,
                   0L);
        expect_int(
            state, "execution_inspection_export",
            cai_agent_runtime_export_execution_markdown(runtime, sink, &error),
            CAI_OK);
        expect_int(state, "execution_inspection_has_sideband",
                   out.heading && out.arguments, 1L);
        expect_int(state, "execution_inspection_safe_text", out.controls, 0L);
        cai_sink_close(sink);
      }
      cai_agent_runtime_close(runtime);
    }
    http_mock_client_close(state, "runtime_execution", &mock);
    execution_test_cleanup(&s);
    cai_error_cleanup(&error);
    cai_error_init(&error);
  }
  cai_free_mem(NULL, args.data);
  cai_free_mem(NULL, response.data);
  cai_error_cleanup(&error);
}

static void test_execution_compaction(test_state *state) {
  static const char *forbidden[] = {"timestamp_ns", "request.failed",
                                    "compaction.started"};
  static const char *summary_required[] = {"Transcript fragment:",
                                           "execution objective"};
  static const char *final_required[] = {"execution summary"};
  static const char summary[] =
      "data: "
      "{\"id\":\"summary\",\"choices\":[{\"index\":0,\"delta\":{\"content\":"
      "\"execution summary\"},\"finish_reason\":\"stop\"}]}\n\ndata: "
      "[DONE]\n\n";
  static const char final[] =
      "data: "
      "{\"id\":\"done\",\"choices\":[{\"index\":0,\"delta\":{\"content\":"
      "\"recovered\"},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
  mock_http_expectation script[] = {
      {"POST /v1/chat/completions HTTP/", NULL, 0U, forbidden, 3U, 400,
       "Bad Request", "application/json", "req_overflow",
       "{\"error\":{\"code\":\"context_length_exceeded\",\"message\":\"prompt "
       "exceeds context window\"}}"},
      {"POST /v1/chat/completions HTTP/", summary_required, 2U, forbidden, 3U,
       200, "OK", "text/event-stream", NULL, summary},
      {"POST /v1/chat/completions HTTP/", final_required, 1U, forbidden, 3U,
       200, "OK", "text/event-stream", NULL, final}};
  http_mock_client mock;
  execution_test_state s;
  cai_agent_session_store store;
  cai_agent_runtime_config config;
  cai_agent_runtime *runtime;
  cai_agent_run_state run_state;
  cai_error error;
  int mode, i;
  for (mode = 0; mode < 3; mode++) {
    script[1].status = mode == 1 ? 500 : 200;
    script[1].content_type =
        mode == 1 ? "application/json" : "text/event-stream";
    script[1].body = mode == 1
                         ? "{\"error\":{\"message\":\"summary unavailable\"}}"
                         : summary;
    if (http_mock_client_open_script(state, "execution_compaction", script,
                                     mode == 0   ? 3U
                                     : mode == 1 ? 2U
                                                 : 1U,
                                     &mock) != 0)
      return;
    cai_client_close(mock.client);
    mock.client = NULL;
    mock.config.api_protocol = CAI_API_PROTOCOL_CHAT_COMPLETIONS;
    mock.config.chat_max_attempts = 1U;
    cai_error_init(&error);
    expect_int(state, "execution_chat_client",
               cai_client_open(&mock.config, &mock.client, &error), CAI_OK);
    memset(&s, 0, sizeof(s));
    s.mode = mode == 2 ? 4 : 0;
    memset(&store, 0, sizeof(store));
    store.context = &s;
    store.checkpoint = execution_test_checkpoint;
    store.load_latest = test_runtime_session_store_load;
    store.append_event = execution_test_append;
    store.load_events_after = execution_test_load;
    cai_agent_runtime_config_init(&config);
    config.workspace_directory = "/tmp";
    config.model = "test-model";
    config.session_store = &store;
    config.disable_default_session_store = 1;
    config.disable_terminal = config.disable_chrome =
        config.disable_web_search = 1;
    config.record_transcript = 1;
    runtime = NULL;
    expect_int(state, "execution_compaction_open",
               cai_agent_runtime_open(mock.client, &config, &runtime, &error),
               CAI_OK);
    if (runtime != NULL) {
      expect_int(
          state, "execution_compaction_submit",
          cai_agent_runtime_submit(runtime, "execution objective", &error),
          CAI_OK);
      run_state = CAI_AGENT_SAMPLING;
      for (i = 0; i < 100 && run_state == CAI_AGENT_SAMPLING; i++) {
        (void)cai_agent_runtime_pump(runtime, 100L, &error);
        (void)cai_agent_runtime_state(runtime, &run_state, &error);
      }
      expect_int(state, "execution_compaction_state", run_state,
                 mode == 0 ? CAI_AGENT_COMPLETED : CAI_AGENT_FAILED);
      expect_int(state, "execution_compaction_records_valid", s.invalid, 0L);
      expect_int(state, "execution_overflow_retained", s.request_failed > 0,
                 1L);
      expect_int(state, "execution_compaction_started", s.compaction_started,
                 mode == 2 ? 0L : 1L);
      expect_int(state, "execution_compaction_finished", s.compaction_completed,
                 mode == 0 ? 1L : 0L);
      expect_int(state, "execution_compaction_failed", s.compaction_failed,
                 mode == 0 ? 0L : 1L);
      cai_agent_runtime_close(runtime);
    }
    cai_error_cleanup(&error);
    http_mock_client_close(state, "execution_compaction", &mock);
    execution_test_cleanup(&s);
  }
}
