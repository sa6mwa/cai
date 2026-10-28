/* Exact wire retries; backoff is simulated except cancellation/close probes. */
typedef struct capacity_test_state {
  cai_agent_runtime *runtime;
  runtime_event_state events;
  int retries, restarted, tools, cancel, close_on_retry, errors, records;
} capacity_test_state;
static long capacity_test_delays[16];
static int capacity_test_waits;
static int capacity_test_wait(cai_agent_runtime *runtime, long delay,
                              cai_error *error) {
  (void)runtime;
  (void)error;
  if (capacity_test_waits < 16)
    capacity_test_delays[capacity_test_waits] = delay;
  capacity_test_waits++;
  return CAI_OK;
}
static int capacity_test_event(void *context,
                               const cai_agent_runtime_event *event,
                               cai_error *error) {
  capacity_test_state *s = context;
  int rc = test_runtime_event(&s->events, event, error);
  if (event->type == CAI_AGENT_EVENT_TOOL_CALL_STARTED)
    s->tools++;
  if (event->type == CAI_AGENT_EVENT_EXECUTION_RECORD) {
    if (strstr(event->data, "request.failed") != NULL)
      s->errors++;
    if (strstr(event->data, "request.retry") != NULL)
      s->records++;
  }
  if (event->type == CAI_AGENT_EVENT_REQUEST_RETRY_STARTED)
    s->restarted++;
  if (event->type == CAI_AGENT_EVENT_REQUEST_RETRY) {
    s->retries++;
    if (s->cancel)
      rc = cai_agent_runtime_cancel_turn(s->runtime, error);
    if (s->close_on_retry) {
      cai_agent_runtime_close(s->runtime);
      s->runtime = NULL;
    }
  }
  return rc;
}
static void test_agent_runtime_capacity(test_state *state) {
  static const char done[] =
      "data: {\"type\":\"response.output_text.delta\",\"delta\":\"FINAL\"}\n\n"
      "data: "
      "{\"type\":\"response.completed\",\"response\":{\"id\":\"resp_ok\"}}\n\n";
  static const char chat_done[] =
      "data: "
      "{\"id\":\"chat_ok\",\"choices\":[{\"index\":0,\"delta\":{\"content\":"
      "\"FINAL\"},\"finish_reason\":\"stop\"}]}\n\n"
      "data: [DONE]\n\n";
  static const char tool[] =
      "data: "
      "{\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{"
      "\"id\":\"fc\",\"type\":\"function_call\",\"call_id\":\"call_capacity\","
      "\"name\":\"get_goal\",\"arguments\":\"{}\"}}\n\n";
  static const char chat_tool[] =
      "data: "
      "{\"id\":\"chat_tool\",\"choices\":[{\"index\":0,\"delta\":{\"tool_"
      "calls\":[{\"index\":0,\"id\":\"call_capacity\",\"type\":\"function\","
      "\"function\":{\"name\":\"get_goal\",\"arguments\":\"{}\"}}]},\"finish_"
      "reason\":\"tool_calls\"}]}\n\n";
  static const char *forbidden[] = {"Proceed.", "Continue working toward"};
  static const char *tool_result[] = {"call_capacity", "has_goal"};
  static const char *codes[] = {"rate_limit_exceeded", "insufficient_quota",
                                "server_error"};
  mock_http_expectation script[12];
  cai_agent_runtime_config config;
  cai_agent_preset preset;
  capacity_test_state s;
  http_mock_client mock;
  cai_error error;
  cai_agent_run_state run_state;
  char failure[2048], tool_response[2048];
  int protocol, mode, i, count, expected, turns;
  for (protocol = 0; protocol < 2; protocol++) {
    for (mode = 0; mode < 14; mode++) {
      /* 0 exhaustion, 1 recovery/reset, 2 opt-out, 3-5 other codes,
       * 6 cancel, 7 close, 8 completed tool, 9 incomplete tool. */
      const char *code =
          mode >= 3 && mode <= 5 ? codes[mode - 3] : "server_is_overloaded";
      if (mode >= 12 && !protocol)
        continue;
      count = mode == 0 ? 11 : mode == 1 ? 5 : mode >= 8 ? 3 : 1;
      expected = mode == 0 ? 10 : mode == 1 ? 3 : mode >= 6 ? 1 : 0;
      snprintf(failure, sizeof(failure),
               "data: {\"%s\":{\"code\":\"%s\",\"message\":\"Selected model is "
               "at capacity\"}%s}\n\n",
               "error", code, protocol ? "" : ",\"type\":\"error\"");
      snprintf(tool_response, sizeof(tool_response), "%s%s",
               protocol ? chat_tool : tool,
               protocol ? "data: [DONE]\n\n"
                        : "data: "
                          "{\"type\":\"response.completed\",\"response\":{"
                          "\"id\":\"resp_tool\"}}\n\n");
      memset(script, 0, sizeof(script));
      for (i = 0; i < count; i++) {
        script[i].request_prefix = protocol ? "POST /v1/chat/completions HTTP/"
                                            : "POST /v1/responses HTTP/";
        script[i].status = 200;
        script[i].status_text = "OK";
        script[i].content_type = "text/event-stream";
        script[i].body = failure;
        script[i].forbidden = forbidden;
        script[i].forbidden_count = 2U;
      }
      if (mode == 1) {
        script[2].body = script[4].body = protocol ? chat_done : done;
      }
      if (mode == 8) {
        script[0].body = tool_response;
        script[2].body = protocol ? chat_done : done;
        for (i = 1; i < 3; i++) {
          script[i].required = tool_result;
          script[i].required_count = 2U;
        }
      }
      if (mode == 9) {
        /* A completed call item from a failed response must not execute. */
        snprintf(tool_response, sizeof(tool_response), "%s%s",
                 protocol ? chat_tool : tool, failure);
        script[0].body = tool_response;
        script[1].body = protocol ? chat_done : done;
        count = 2;
      }
      if (mode >= 10) {
        count = 2;
        script[1].body = protocol ? chat_done : done;
        if (mode == 10) {
          script[0].status = 503;
          script[0].status_text = "Service Unavailable";
          script[0].content_type = "application/json";
          script[0].body = "{\"error\":{\"code\":\"server_is_overloaded\","
                           "\"message\":\"capacity\"}}";
        } else if (!protocol)
          script[0].body =
              "data: "
              "{\"type\":\"response.failed\",\"response\":{\"error\":{\"code\":"
              "\"server_is_overloaded\",\"message\":\"capacity\"}}}\n\n";
      }
      if (mode >= 12) {
        snprintf(
            tool_response, sizeof(tool_response),
            "data: "
            "{\"id\":\"chat_usage\",\"choices\":[],\"usage\":{\"prompt_"
            "tokens\":20,\"completion_tokens\":8,\"total_tokens\":28}}\n\n%s",
            failure);
        script[0].body = tool_response;
        if (mode == 12) {
          count = 1;
          expected = 0;
        }
      }
      if (http_mock_client_open_script(state, "capacity_wire", script,
                                       (size_t)count, &mock) != 0)
        return;
      if (protocol) {
        cai_client_close(mock.client);
        mock.client = NULL;
        mock.config.api_protocol = CAI_API_PROTOCOL_CHAT_COMPLETIONS;
        if (mode == 12)
          mock.config.usage_limits.max_total_tokens = 1;
        expect_int(state, "capacity_chat_open",
                   cai_client_open(&mock.config, &mock.client, &mock.error),
                   CAI_OK);
      }
      cai_error_init(&error);
      memset(&s, 0, sizeof(s));
      s.events.owner = pthread_self();
      s.cancel = mode == 6;
      s.close_on_retry = mode == 7;
      memset(&preset, 0, sizeof(preset));
      preset.name = "capacity-fixture";
      preset.prompt_version = "fixture";
      preset.default_identity = "Fixture";
      preset.default_model = "gpt-5-nano";
      preset.developer_instructions = "Exercise request capacity.";
      preset.tool_capabilities = CAI_AGENT_PRESET_TOOL_GOAL;
      cai_agent_runtime_config_init(&config);
      config.workspace_directory = "/tmp";
      config.disable_default_session_store = 1;
      config.record_transcript = 1;
      config.preset_descriptor = &preset;
      config.event_callback = capacity_test_event;
      config.event_context = &s;
      config.disable_capacity_retry = mode == 2;
      capacity_test_waits = 0;
      cai_runtime_test_set_capacity_wait_fn(
          mode == 6 || mode == 7 ? NULL : capacity_test_wait);
      expect_int(
          state, "capacity_open",
          cai_agent_runtime_open(mock.client, &config, &s.runtime, &error),
          CAI_OK);
      turns = mode == 1 ? 2 : 1;
      for (i = 0; i < turns && s.runtime != NULL; i++) {
        int j, terminal = s.events.completed_count + s.events.failed_count +
                          s.events.cancelled_count;
        expect_int(
            state, "capacity_submit",
            cai_agent_runtime_submit(s.runtime, "original request", &error),
            CAI_OK);
        for (j = 0;
             j < 100 && s.runtime != NULL &&
             terminal == s.events.completed_count + s.events.failed_count +
                             s.events.cancelled_count;
             j++)
          expect_int(state, "capacity_pump",
                     cai_agent_runtime_pump(s.runtime, 50L, &error), CAI_OK);
      }
      expect_int(state, "capacity_retry_count", s.retries, expected);
      expect_int(state, "capacity_no_spurious_tools", s.tools,
                 mode == 8 ? 1 : 0);
      expect_int(state, "capacity_owner_thread", s.events.wrong_thread, 0);
      expect_int(state, "capacity_retry_records", s.records, expected);
      if (mode != 7) {
        expect_int(state, "capacity_terminal_state",
                   cai_agent_runtime_state(s.runtime, &run_state, &error),
                   CAI_OK);
        expect_int(state, "capacity_expected_state", run_state,
                   mode == 1 || (mode >= 8 && mode != 12) ? CAI_AGENT_COMPLETED
                   : mode == 6                            ? CAI_AGENT_CANCELLED
                                                          : CAI_AGENT_FAILED);
      }
      if (mode == 0) {
        for (i = 0; i < 10; i++)
          expect_int(state, "capacity_exact_backoff", capacity_test_delays[i],
                     30000L * (1L << (i < 3 ? i : 3)));
        expect_int(state, "capacity_no_false_success", s.events.completed_count,
                   0);
      }
      if (mode == 1) {
        expect_int(state, "capacity_two_turns", s.events.completed_count, 2);
        expect_int(state, "capacity_reset_delay", capacity_test_delays[2],
                   30000L);
      }
      expect_int(state, "capacity_actual_waits", capacity_test_waits,
                 mode == 6 || mode == 7 ? 0 : expected);
      if (mode >= 12) {
        cai_usage_accounting usage;
        expect_int(state, "capacity_failed_usage",
                   cai_client_usage(mock.client, &usage, &error), CAI_OK);
        expect_int(state, "capacity_failed_tokens", usage.usage.total_tokens,
                   28);
      }
      cai_agent_runtime_close(s.runtime);
      cai_runtime_test_set_capacity_wait_fn(NULL);
      http_mock_client_close(state, "capacity_wire", &mock);
      cai_error_cleanup(&error);
    }
  }
}

static void capacity_ws_child(int pipe_fd) {
  char headers[4096], first[8192], next[8192];
  int server = -1, client = -1, i;
  signal(SIGALRM, mock_child_timeout_handler);
  alarm(2U);
  if (mock_websocket_listen_and_publish_port(pipe_fd, &server) != 0)
    _exit(2);
  for (i = 0; i < 2; i++) {
    if (mock_websocket_accept_openai_request(server, &client, headers,
                                             sizeof(headers)) != 0 ||
        mock_websocket_read_text(client, next, sizeof(next)) != 0)
      _exit(3);
    if (i == 0) {
      strcpy(first, next);
      if (mock_websocket_write_text(
              client,
              "{\"type\":\"response.failed\",\"response\":{\"error\":{\"code\":"
              "\"server_is_overloaded\",\"message\":\"capacity\"}}}") != 0)
        _exit(4);
    } else {
      if (strcmp(first, next) != 0)
        _exit(5);
      if (mock_websocket_write_text(client,
                                    "{\"type\":\"response.output_text.delta\","
                                    "\"delta\":\"Recovered\"}") != 0 ||
          mock_websocket_write_text(
              client, "{\"type\":\"response.completed\",\"response\":{\"id\":"
                      "\"resp_capacity\"}}") != 0)
        _exit(6);
    }
    close(client);
  }
  close(server);
  _exit(0);
}
static void test_agent_runtime_capacity_websocket(test_state *state) {
  websocket_mock_server server;
  cai_client_config client_config;
  cai_agent_runtime_config config;
  cai_agent_preset preset;
  cai_client *client = NULL;
  capacity_test_state s;
  cai_error error;
  test_env_snapshot force, disable;
  int i;
  if (websocket_mock_server_open_child(state, "capacity_ws", capacity_ws_child,
                                       &server) != 0)
    return;
  test_env_capture(&force, "CAI_TEST_FORCE_RESPONSES_WEBSOCKET");
  test_env_capture(&disable, "CAI_TEST_DISABLE_RESPONSES_WEBSOCKET");
  setenv(force.name, "1", 1);
  unsetenv(disable.name);
  cai_error_init(&error);
  cai_client_config_init(&client_config);
  client_config.api_key = "mock-key";
  client_config.base_url = server.base_url;
  client_config.timeout_ms = 500L;
  expect_int(state, "capacity_ws_client",
             cai_client_open(&client_config, &client, &error), CAI_OK);
  memset(&s, 0, sizeof(s));
  s.events.owner = pthread_self();
  memset(&preset, 0, sizeof(preset));
  preset.name = "capacity-fixture";
  preset.prompt_version = "fixture";
  preset.default_identity = "Fixture";
  preset.default_model = "gpt-5-nano";
  preset.developer_instructions = "Exercise capacity.";
  cai_agent_runtime_config_init(&config);
  config.preset_descriptor = &preset;
  config.workspace_directory = "/tmp";
  config.disable_default_session_store = 1;
  config.event_callback = capacity_test_event;
  config.event_context = &s;
  capacity_test_waits = 0;
  cai_runtime_test_set_capacity_wait_fn(capacity_test_wait);
  expect_int(state, "capacity_ws_open",
             cai_agent_runtime_open(client, &config, &s.runtime, &error),
             CAI_OK);
  if (s.runtime != NULL) {
    expect_int(state, "capacity_ws_submit",
               cai_agent_runtime_submit(s.runtime, "original request", &error),
               CAI_OK);
    for (i = 0; i < 50 && !s.events.completed_count && !s.events.failed_count;
         i++)
      expect_int(state, "capacity_ws_pump",
                 cai_agent_runtime_pump(s.runtime, 50L, &error), CAI_OK);
    expect_int(state, "capacity_ws_success", s.events.completed_count, 1);
    expect_int(state, "capacity_ws_retry", s.retries, 1);
    expect_int(state, "capacity_ws_backoff", capacity_test_waits, 1);
  }
  cai_agent_runtime_close(s.runtime);
  cai_client_close(client);
  cai_runtime_test_set_capacity_wait_fn(NULL);
  test_env_restore(&force);
  test_env_restore(&disable);
  cai_error_cleanup(&error);
  expect_child_exit(state, "capacity_ws", server.pid, &server.child_status);
}
