/* Drive the CLI's owner-thread event renderer with provider-shaped chunks. */
#define main cai_cli_app_main
int cai_cli_app_main(int argc, char **argv);
#include "../cli/main.c"
#undef main

typedef struct event_log_capture {
  char bytes[16384];
  size_t length;
} event_log_capture;

static int test_goal_handoff(void) {
  cli_state state;
  cai_agent_runtime_event event;
  memset(&state, 0, sizeof(state));
  memset(&event, 0, sizeof(event));
  state.batch = 1;
  state.auto_goal = 1;
  state.awaiting_turn = 1;
  state.goal_starts_pending = 2U;
  event.type = CAI_AGENT_EVENT_RUN_COMPLETED;
  if (cli_event(&state, &event, NULL) != CAI_OK || state.awaiting_turn ||
      cli_advance_automation(&state, NULL) != CAI_OK || state.automation_done ||
      !state.auto_goal)
    return 1;
  /* An older turn completing must not consult an obsolete goal projection.
   * Unrelated and child goal events cannot acknowledge these host starts. */
  event.type = CAI_AGENT_EVENT_GOAL_CHANGED;
  event.data = "paused";
  event.data_length = 6U;
  if (cli_event(&state, &event, NULL) != CAI_OK ||
      state.goal_starts_pending != 2U)
    return 1;
  event.data = "replaced";
  event.data_length = 8U;
  event.parent_tool_call_id = "child";
  if (cli_event(&state, &event, NULL) != CAI_OK ||
      state.goal_starts_pending != 2U)
    return 1;
  event.parent_tool_call_id = NULL;
  if (cli_event(&state, &event, NULL) != CAI_OK ||
      state.goal_starts_pending != 1U ||
      cli_advance_automation(&state, NULL) != CAI_OK || state.automation_done)
    return 1;
  event.data = "resumed";
  event.data_length = 7U;
  if (cli_event(&state, &event, NULL) != CAI_OK ||
      state.goal_starts_pending != 0U ||
      cli_event(&state, &event, NULL) != CAI_OK ||
      state.goal_starts_pending != 0U)
    return 1;
  state.goal_starts_pending = 1U;
  state.awaiting_turn = 1;
  event.type = CAI_AGENT_EVENT_RUN_FAILED;
  if (cli_event(&state, &event, NULL) != CAI_OK ||
      state.goal_starts_pending != 0U || state.awaiting_turn ||
      cli_advance_automation(&state, NULL) != CAI_OK || !state.automation_done)
    return 1;
  return 0;
}

static int event_log_write(void *context, const char *bytes, size_t count,
                           size_t *written) {
  event_log_capture *capture = (event_log_capture *)context;
  if (count >= sizeof(capture->bytes) - capture->length)
    return EIO;
  memcpy(capture->bytes + capture->length, bytes, count);
  capture->length += count;
  capture->bytes[capture->length] = '\0';
  *written = count;
  return 0;
}

static int test_event_logging(void) {
  cli_state state;
  cai_agent_runtime_event event;
  cai_error error;
  cai_source *source;
  pslog_config config;
  event_log_capture capture;
  char bytes[128];
  size_t count;
  int rc;
  memset(&state, 0, sizeof(state));
  memset(&event, 0, sizeof(event));
  memset(&capture, 0, sizeof(capture));
  state.batch = 1;
  strcpy(state.pouch.state_directory, ".");
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.output.write = event_log_write;
  config.output.userdata = &capture;
  config.output.close = NULL;
  config.output.isatty = NULL;
  config.output.owned = 0;
  state.log.logger = pslog_new(&config);
  if (state.log.logger == NULL)
    return 1;
  cai_error_init(&error);
  event.type = CAI_AGENT_EVENT_RUN_STARTED;
  event.data = "normal prompt";
  event.data_length = strlen(event.data);
  rc = cli_event(&state, &event, &error);
  event.type = CAI_AGENT_EVENT_STEERING_QUEUED;
  event.data = "steer prompt";
  event.data_length = strlen(event.data);
  if (rc == CAI_OK)
    rc = cli_event(&state, &event, &error);
  event.type = CAI_AGENT_EVENT_STEERING_DELIVERED;
  if (rc == CAI_OK)
    rc = cli_event(&state, &event, &error);
  event.type = CAI_AGENT_EVENT_TURN_QUEUED;
  event.data = "queued prompt";
  event.data_length = strlen(event.data);
  if (rc == CAI_OK)
    rc = cli_event(&state, &event, &error);
  event.type = CAI_AGENT_EVENT_TEXT_DELTA;
  event.data = "parent answer";
  event.data_length = strlen(event.data);
  if (rc == CAI_OK)
    rc = cli_event(&state, &event, &error);
  event.parent_tool_call_id = "child-invocation";
  event.data = "child answer";
  event.data_length = strlen(event.data);
  if (rc == CAI_OK)
    rc = cli_event(&state, &event, &error);
  event.type = CAI_AGENT_EVENT_REVIEW_REPORT;
  if (rc == CAI_OK)
    rc = cli_event(&state, &event, &error);
  event.type = CAI_AGENT_EVENT_RESPONSE_COMPLETED;
  event.data = NULL;
  event.data_length = 0U;
  if (rc == CAI_OK)
    rc = cli_event(&state, &event, &error);
  event.parent_tool_call_id = NULL;
  event.type = CAI_AGENT_EVENT_TEXT_DELTA;
  event.data = " continues";
  event.data_length = strlen(event.data);
  if (rc == CAI_OK)
    rc = cli_event(&state, &event, &error);
  source = NULL;
  if (rc == CAI_OK)
    rc = cai_cli_response_source(&state.final_response, &source, &error);
  count = source != NULL ? cai_source_read(source, bytes, sizeof(bytes), &error)
                         : 0U;
  cai_source_close(source);
  cai_cli_response_close(&state.final_response);
  state.log.logger->destroy(state.log.logger);
  cai_error_cleanup(&error);
  if (rc != CAI_OK || count != strlen("parent answer continues") ||
      memcmp(bytes, "parent answer continues", count) != 0 ||
      state.review_report != NULL ||
      strstr(capture.bytes, "\"lvl\":\"info\"") == NULL ||
      strstr(capture.bytes, "\"role\":\"user\"") == NULL ||
      strstr(capture.bytes, "\"role\":\"assistant\"") == NULL ||
      strstr(capture.bytes, "\"prompt_kind\":\"normal\"") == NULL ||
      strstr(capture.bytes, "\"prompt_kind\":\"steering\"") == NULL ||
      strstr(capture.bytes, "\"prompt_kind\":\"queued\"") == NULL ||
      strstr(capture.bytes, "child answer") == NULL ||
      strstr(capture.bytes, "\"parent_tool_call_id\":\"child-invocation\"") ==
          NULL ||
      strstr(capture.bytes, "\"app\"") != NULL) {
    fprintf(stderr, "event logging or parent output selection failed: %s\n",
            capture.bytes);
    return 1;
  }
  return 0;
}

typedef struct auth_fixture {
  pthread_mutex_t lock;
  pthread_cond_t condition;
  int calls;
  int released;
  int second_started;
} auth_fixture;

static int test_auth_call(cai_chatgpt_auth *auth, cai_error *error) {
  auth_fixture *fixture = auth->impl;
  int call;
  (void)error;
  pthread_mutex_lock(&fixture->lock);
  call = ++fixture->calls;
  pthread_cond_broadcast(&fixture->condition);
  while (fixture->released < call)
    pthread_cond_wait(&fixture->condition, &fixture->lock);
  pthread_mutex_unlock(&fixture->lock);
  return CAI_OK;
}

static int test_auth_token(cai_chatgpt_auth *auth, char **out,
                           cai_error *error) {
  *out = NULL;
  return test_auth_call(auth, error);
}

static void *test_token_thread(void *context) {
  cli_state *state = context;
  cai_error error;
  char *token;
  cai_error_init(&error);
  (void)cli_auth_token(&state->shared_auth, &token, &error);
  cai_error_cleanup(&error);
  return NULL;
}

static void *test_refresh_thread(void *context) {
  cli_state *state = context;
  auth_fixture *fixture = state->auth->impl;
  cai_error error;
  pthread_mutex_lock(&fixture->lock);
  fixture->second_started = 1;
  pthread_cond_broadcast(&fixture->condition);
  pthread_mutex_unlock(&fixture->lock);
  cai_error_init(&error);
  (void)cli_auth_refresh(&state->shared_auth, &error);
  cai_error_cleanup(&error);
  return NULL;
}

static int test_shared_auth(void) {
  cli_state state;
  cai_chatgpt_auth auth;
  auth_fixture fixture;
  pthread_t token;
  pthread_t refresh;
  memset(&state, 0, sizeof(state));
  memset(&auth, 0, sizeof(auth));
  memset(&fixture, 0, sizeof(fixture));
  if (pthread_mutex_init(&fixture.lock, NULL) != 0 ||
      pthread_cond_init(&fixture.condition, NULL) != 0 ||
      pthread_mutex_init(&state.auth_lock, NULL) != 0)
    return 1;
  auth.impl = &fixture;
  auth.access_token = test_auth_token;
  auth.refresh = test_auth_call;
  state.auth = &auth;
  state.shared_auth.impl = &state;
  if (pthread_create(&token, NULL, test_token_thread, &state) != 0)
    return 1;
  pthread_mutex_lock(&fixture.lock);
  while (fixture.calls == 0)
    pthread_cond_wait(&fixture.condition, &fixture.lock);
  pthread_mutex_unlock(&fixture.lock);
  if (pthread_create(&refresh, NULL, test_refresh_thread, &state) != 0)
    return 1;
  pthread_mutex_lock(&fixture.lock);
  while (!fixture.second_started)
    pthread_cond_wait(&fixture.condition, &fixture.lock);
  if (fixture.calls != 1)
    return 1;
  fixture.released = 2;
  pthread_cond_broadcast(&fixture.condition);
  pthread_mutex_unlock(&fixture.lock);
  pthread_join(token, NULL);
  pthread_join(refresh, NULL);
  pthread_cond_destroy(&fixture.condition);
  pthread_mutex_destroy(&fixture.lock);
  pthread_mutex_destroy(&state.auth_lock);
  return fixture.calls == 2 ? 0 : 1;
}

/* Use the production libmdf -> softline sink, then revoke its output fd.
 * This verifies errors cross both renderer boundaries rather than substituting
 * a renderer mock that always returns failure. */
static int test_output_failure(int operation) {
  cli_state state;
  sl_config_t config;
  mdf_options options;
  mdf_sink sink;
  cai_agent_runtime_event event;
  cai_agent_session_event replay;
  int pipe_fd[2];
  int rc = -1;
  int handled = 0;
  memset(&state, 0, sizeof(state));
  memset(&event, 0, sizeof(event));
  memset(&replay, 0, sizeof(replay));
  if (pipe(pipe_fd) != 0)
    return 1;
  sl_config_init(&config);
  config.output_fd = pipe_fd[1];
  state.sl = sl_create_with_config(&config);
  state.width = 80;
  mdf_options_init(&options);
  options.width = state.width;
  options.boring = 1;
  if (state.sl == NULL || sl_output_stream_begin(state.sl) != SL_OK ||
      mdf_create(MDF_FORMAT_ANSI, &options, &state.renderer) != MDF_OK)
    goto cleanup;
  sink.userdata = &state;
  sink.write = cli_renderer_sink;
  if (state.renderer->set_sink(state.renderer, &sink) != MDF_OK)
    goto cleanup;
  if (operation == 2) {
    const char *table = "| Left | Right |\n| --- | --- |\n| a | b |\n";
    if (cli_text(&state, table, strlen(table)) != 0)
      goto cleanup;
  }
  close(pipe_fd[1]);
  pipe_fd[1] = -1;
  event.data = "Output unavailable.\n\n";
  event.data_length = strlen(event.data);
  if (operation == 0) {
    event.type = CAI_AGENT_EVENT_TEXT_DELTA;
    rc = cli_event(&state, &event, NULL);
  } else if (operation == 1) {
    event.data = "Heading\n\nOutput unavailable.\n\n";
    event.data_length = strlen(event.data);
    event.type = CAI_AGENT_EVENT_REASONING_SUMMARY;
    rc = cli_event(&state, &event, NULL);
  } else if (operation == 2) {
    event.type = CAI_AGENT_EVENT_RESPONSE_COMPLETED;
    rc = cli_event(&state, &event, NULL);
  } else if (operation == 3) {
    rc = cli_prompt(&state, "literal prompt") != 0 ? CAI_ERR_TRANSPORT : CAI_OK;
  } else if (operation == 4) {
    rc = cli_handle_command(&state, "/help", NULL, &handled);
    if (!handled)
      rc = CAI_OK;
  } else if (operation == 6) {
    rc = cli_notice(&state, "* WARN: Output unavailable") != 0
             ? CAI_ERR_TRANSPORT
             : CAI_OK;
  } else if (operation == 7) {
    rc = cli_write(&state, "Output unavailable") != 0 ? CAI_ERR_TRANSPORT
                                                      : CAI_OK;
  } else {
    replay.type = "assistant_text_delta";
    replay.data = event.data;
    rc = cli_replay_event(&state, &replay, NULL);
  }
cleanup:
  if (state.notice_renderer != NULL)
    state.notice_renderer->destroy(state.notice_renderer);
  if (state.renderer != NULL)
    state.renderer->destroy(state.renderer);
  if (state.sl != NULL)
    sl_destroy(state.sl);
  close(pipe_fd[0]);
  if (pipe_fd[1] >= 0)
    close(pipe_fd[1]);
  if (rc != CAI_ERR_TRANSPORT) {
    fprintf(stderr, "output failure operation %d was not propagated: %d\n",
            operation, rc);
    return 1;
  }
  return 0;
}

typedef struct reasoning_capture {
  char bytes[2048];
  size_t length;
  int fail;
} reasoning_capture;
static int reasoning_capture_body(void *context, const char *data,
                                  size_t length) {
  reasoning_capture *capture = context;
  if (capture->fail || length >= sizeof(capture->bytes) - capture->length)
    return -1;
  memcpy(capture->bytes + capture->length, data, length);
  capture->length += length;
  capture->bytes[capture->length] = '\0';
  return 0;
}
static int test_reasoning_sections(void) {
  static const char *inputs[] = {"**Applying patch**",
                                 "**Applying patch**\n\n",
                                 "\n  **Applying patch**\r\n\n",
                                 "**Applying patch**\n\nBody å🚀\nNext",
                                 "**Applying patch**\r\n  Body å🚀\nNext",
                                 "**Å🚀 title**\nbody"};
  static const char *bodies[] = {
      "", "", "", "Body å🚀\nNext", "  Body å🚀\nNext", "body"};
  static const char *titles[] = {"Applying patch", "Applying patch",
                                 "Applying patch", "Applying patch",
                                 "Applying patch", "Å🚀 title"};
  cai_cli_reasoning reasoning;
  reasoning_capture capture;
  const char *title;
  char long_header[1024];
  size_t n, chunk, offset, count;
  for (n = 0U; n < sizeof(inputs) / sizeof(inputs[0]); n++) {
    for (chunk = 1U; chunk <= strlen(inputs[n]); chunk++) {
      cai_cli_reasoning_reset(&reasoning);
      memset(&capture, 0, sizeof(capture));
      for (offset = 0U; offset < strlen(inputs[n]); offset += count) {
        count = strlen(inputs[n]) - offset;
        if (count > chunk)
          count = chunk;
        if (cai_cli_reasoning_feed(&reasoning, inputs[n] + offset, count,
                                   reasoning_capture_body, &capture) != 0 ||
            cai_cli_reasoning_title(&reasoning, &title) != 0) {
          fprintf(stderr,
                  "fragmented heading failed at section %lu offset %lu\n",
                  (unsigned long)n, (unsigned long)offset);
          return 1;
        }
      }
      if (cai_cli_reasoning_title(&reasoning, &title) != 0 ||
          strcmp(title, titles[n]) != 0 ||
          strcmp(capture.bytes, bodies[n]) != 0) {
        fprintf(stderr, "section %lu chunk %lu: title=%s body=%s\n",
                (unsigned long)n, (unsigned long)chunk, title, capture.bytes);
        return 1;
      }
      cai_cli_reasoning_end(&reasoning);
      if (cai_cli_reasoning_title(&reasoning, &title) != 0 ||
          strcmp(title, titles[n]) != 0)
        return 1;
      if (cai_cli_reasoning_feed(&reasoning, "# Fresh", 7U,
                                 reasoning_capture_body, &capture) != 0 ||
          cai_cli_reasoning_title(&reasoning, &title) != 0 ||
          strcmp(title, "# Fresh") != 0) {
        fprintf(stderr, "fresh heading: %s\n", title);
        return 1;
      }
    }
  }
  memset(long_header, 'x', sizeof(long_header));
  cai_cli_reasoning_reset(&reasoning);
  memset(&capture, 0, sizeof(capture));
  if (cai_cli_reasoning_feed(&reasoning, long_header, sizeof(long_header),
                             reasoning_capture_body, &capture) != 0 ||
      reasoning.header_length != sizeof(reasoning.header) - 1U ||
      capture.length != 0U ||
      cai_cli_reasoning_feed(&reasoning, "\nbody", 5U, reasoning_capture_body,
                             &capture) != 0 ||
      strcmp(capture.bytes, "body") != 0)
    return 1;
  /* The body is delivered before section completion; sink failures propagate.
   */
  capture.fail = 1;
  if (cai_cli_reasoning_feed(&reasoning, "more", 4U, reasoning_capture_body,
                             &capture) != -1)
    return 1;
  cai_cli_reasoning_reset(&reasoning);
  if (cai_cli_reasoning_feed(&reasoning, "title\n  body", 12U,
                             reasoning_capture_body, &capture) != -1)
    return 1;
  cai_cli_reasoning_reset(&reasoning);
  if (cai_cli_reasoning_title(&reasoning, &title) != 0 || title[0] != '\0')
    return 1;
  return 0;
}

static int literal_capture_write(void *context, const char *data,
                                 size_t length) {
  size_t written;
  return event_log_write(context, data, length, &written);
}
static void without_spaces(char *out, const char *input) {
  while (*input != '\0') {
    if (*input != ' ' && *input != '\n' && *input != '\r' && *input != '\t')
      *out++ = *input;
    input++;
  }
  *out = '\0';
}
static int test_literal_tokens(void) {
  static const char *inputs[] = {
      "**stars** _path_ `tick` [name](url) <tag> {value} a+b-1.! \\path &amp;",
      "café λ å🚀 **stars** _path_ `tick` &amp;",
      "invalid \xff**stars** \xc3[_path_]",
      "control \033[2J **stars**",
      "first\nsecond\n\nlast",
      ""};
  static const char *expected[] = {
      "**stars** _path_ `tick` [name](url) <tag> {value} a+b-1.! \\path &amp;",
      "café λ å🚀 **stars** _path_ `tick` &amp;",
      "invalid ?**stars** ?[_path_]",
      "control [2J **stars**",
      "first\nsecond\n\nlast",
      ""};
  event_log_capture capture;
  char actual_text[16384];
  char expected_text[16384];
  char long_text[6001];
  mdf_options options;
  mdf_sink sink;
  mdf *renderer;
  size_t i, n, columns;
  int rc = 0;
  mdf_options_init(&options);
  options.width = 40;
  options.boring = 1;
  options.ansi_mode = MDF_ANSI_OFF;
  if (mdf_create(MDF_FORMAT_ANSI, &options, &renderer) != MDF_OK)
    return 1;
  memset(&capture, 0, sizeof(capture));
  sink.userdata = &capture;
  sink.write = literal_capture_write;
  if (renderer->set_sink(renderer, &sink) != MDF_OK) {
    renderer->destroy(renderer);
    return 1;
  }
  for (i = 0U; i < sizeof(long_text) - 1U; i++)
    long_text[i] = "a*\\_[x]&"[i % (sizeof("a*\\_[x]&") - 1U)];
  long_text[i] = '\0';
  for (n = 0U; n <= sizeof(inputs) / sizeof(inputs[0]); n++) {
    const char *input =
        n < sizeof(inputs) / sizeof(inputs[0]) ? inputs[n] : long_text;
    const char *wanted =
        n < sizeof(inputs) / sizeof(inputs[0]) ? expected[n] : long_text;
    memset(&capture, 0, sizeof(capture));
    if (cai_cli_literal_render(renderer, input) != 0) {
      rc = 1;
      break;
    }
    without_spaces(actual_text, capture.bytes);
    without_spaces(expected_text, wanted);
    if (strcmp(actual_text, expected_text) != 0 ||
        strchr(capture.bytes, '\033') != NULL) {
      fprintf(stderr, "literal token case %lu changed text\n",
              (unsigned long)n);
      rc = 1;
      break;
    }
    if (n == 1U)
      continue; /* Unicode display width is also checked by the PTY test. */
    columns = 0U;
    for (i = 0U; i < capture.length; i++) {
      if (capture.bytes[i] == '\n')
        columns = 0U;
      else if (++columns > 40U) {
        fprintf(stderr, "literal token case %lu exceeded wrap width\n",
                (unsigned long)n);
        rc = 1;
        break;
      }
    }
    if (rc != 0)
      break;
  }
  renderer->destroy(renderer);
  return rc;
}

int main(void) {
  cli_state state;
  cai_agent_runtime_event event;
  mdf_options renderer_options;
  mdf_sink sink;
  int pipe_fd[2];
  int saved_stdout;
  char output[4096];
  const char *title;
  ssize_t count;
  int rc;
  struct timespec start = {100, 500000000L};
  struct timespec before = {400, 499999999L};
  struct timespec boundary = {400, 500000000L};

  for (rc = 0; rc < 8; rc++)
    if (test_output_failure(rc) != 0)
      return 1;
  if (test_goal_handoff() != 0 || test_shared_auth() != 0 ||
      test_event_logging() != 0 || test_reasoning_sections() != 0 ||
      test_literal_tokens() != 0)
    return 1;
  memset(&state, 0, sizeof(state));
  if (!cai_cli_quota_due(0, start, start, 0) ||
      cai_cli_quota_due(1, start, before, 0) ||
      !cai_cli_quota_due(1, start, boundary, 0) ||
      !cai_cli_quota_due(1, start, before, 1)) {
    fprintf(stderr, "quota refresh interval or forced refresh failed\n");
    return 1;
  }
  memset(&event, 0, sizeof(event));
  if (pipe(pipe_fd) != 0) {
    return 1;
  }
  saved_stdout = dup(STDOUT_FILENO);
  if (saved_stdout < 0 || dup2(pipe_fd[1], STDOUT_FILENO) < 0) {
    return 1;
  }
  close(pipe_fd[1]);
  state.sl = sl_create();
  if (state.sl == NULL || sl_output_stream_begin(state.sl) != SL_OK) {
    return 1;
  }
  state.width = 80;
  mdf_options_init(&renderer_options);
  renderer_options.output_fd = STDOUT_FILENO;
  renderer_options.width = state.width;
  renderer_options.boring = 1;
  if (mdf_create(MDF_FORMAT_ANSI, &renderer_options, &state.renderer) !=
      MDF_OK) {
    return 1;
  }
  sink.userdata = &state;
  sink.write = cli_renderer_sink;
  if (state.renderer->set_sink(state.renderer, &sink) != MDF_OK) {
    return 1;
  }
  if (cli_prompt(&state, "Hello") != 0 ||
      cli_notice(&state, "* WARN: useful diagnostic") != 0 ||
      cli_prompt(&state, "Next") != 0)
    return 1;
  state.interactive = 1;
  if (unsetenv("NO_COLOR") != 0)
    return 1;
  if (cli_notice(&state, "* WARN: colored diagnostic") != 0)
    return 1;
  if (setenv("NO_COLOR", "1", 1) != 0 ||
      cli_notice(&state, "* ERROR: no color") != 0 || unsetenv("NO_COLOR") != 0)
    return 1;
  state.interactive = 0;
  if (cli_notice(&state,
                 "* WARN: **literal** _file_ `tick` [name](url) "
                 "word01 word02 word03 word04 word05 word06 word07 "
                 "word08 word09 word10 word11 word12 word13 word14 "
                 "word15 word16 word17 word18 word19 word20 END") != 0 ||
      cli_write(&state, "Local **literal** _file_ `tick` [name](url)\n") != 0)
    return 1;
  strcpy(state.reasoning.header, "Previous turn");
  state.reasoning.header_length = strlen(state.reasoning.header);
  event.type = CAI_AGENT_EVENT_RUN_STARTED;
  rc = cli_event(&state, &event, NULL);
  if (state.reasoning.header_length != 0U)
    return 1;
  event.type = CAI_AGENT_EVENT_REASONING_SUMMARY;
  event.data = "**Actual provider ";
  event.data_length = strlen(event.data);
  if (rc == CAI_OK)
    rc = cli_event(&state, &event, NULL);
  event.type = CAI_AGENT_EVENT_RUN_STARTED;
  event.parent_tool_call_id = "child-tool";
  if (rc == CAI_OK)
    rc = cli_event(&state, &event, NULL);
  if (strcmp(state.reasoning.header, "**Actual provider ") != 0)
    return 1;
  event.parent_tool_call_id = NULL;
  event.type = CAI_AGENT_EVENT_REASONING_SUMMARY;
  event.data = "summary**";
  event.data_length = strlen(event.data);
  if (rc == CAI_OK) {
    rc = cli_event(&state, &event, NULL);
  }
  event.type = CAI_AGENT_EVENT_RESPONSE_COMPLETED;
  event.data = NULL;
  event.data_length = 0U;
  if (rc == CAI_OK) {
    rc = cli_event(&state, &event, NULL);
  }
  if (cai_cli_reasoning_title(&state.reasoning, &title) != 0 ||
      strcmp(title, "Actual provider summary") != 0 || state.reasoning_open) {
    fprintf(stderr, "single header: %s open=%d\n", title, state.reasoning_open);
    return 1;
  }
  event.type = CAI_AGENT_EVENT_REASONING_SUMMARY;
  event.data = "**Next header**\r";
  event.data_length = strlen(event.data);
  if (rc == CAI_OK)
    rc = cli_event(&state, &event, NULL);
  event.data = "\n\nFirst body line\nSecond body ";
  event.data_length = strlen(event.data);
  if (rc == CAI_OK)
    rc = cli_event(&state, &event, NULL);
  event.data = "line\n";
  event.data_length = strlen(event.data);
  if (rc == CAI_OK)
    rc = cli_event(&state, &event, NULL);
  event.type = CAI_AGENT_EVENT_TEXT_DELTA;
  event.data = "Assistant text\n";
  event.data_length = strlen(event.data);
  if (rc == CAI_OK)
    rc = cli_event(&state, &event, NULL);
  event.type = CAI_AGENT_EVENT_RESPONSE_COMPLETED;
  event.data = NULL;
  event.data_length = 0U;
  if (rc == CAI_OK)
    rc = cli_event(&state, &event, NULL);
  /* A split SGR sequence and a reset-only blank row never receive markers. */
  state.row_line_start = 1;
  state.row_ansi = 0;
  if (cli_prefixed_sink(&state, "\033[", 2U) != 0 ||
      cli_prefixed_sink(&state, "1mDecorated\n\033[0",
                        strlen("1mDecorated\n\033[0")) != 0 ||
      cli_prefixed_sink(&state, "m\nPlain row\n", strlen("m\nPlain row\n")) !=
          0)
    return 1;
  state.timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
  if (state.timer_fd < 0 || cli_timer_wakeup(state.sl, NULL, &state) != SL_OK) {
    return 1;
  }
  close(state.timer_fd);
  state.timer_fd = -1;
  if (sl_output_stream_end(state.sl) != SL_OK) {
    rc = CAI_ERR_TRANSPORT;
  }
  state.renderer->destroy(state.renderer);
  if (state.notice_renderer != NULL)
    state.notice_renderer->destroy(state.notice_renderer);
  sl_destroy(state.sl);
  if (dup2(saved_stdout, STDOUT_FILENO) < 0) {
    return 1;
  }
  close(saved_stdout);
  count = read(pipe_fd[0], output, sizeof(output) - 1U);
  close(pipe_fd[0]);
  if (count < 0 || rc != CAI_OK || state.reasoning_open ||
      cai_cli_reasoning_title(&state.reasoning, &title) != 0 ||
      strcmp(title, "Next header") != 0) {
    fprintf(stderr, "finished reasoning: rc=%d title=%s open=%d\n", rc, title,
            state.reasoning_open);
    return 1;
  }
  output[count] = '\0';
  if (strstr(output, "\033[91m*\033[0m WARN: colored diagnostic") == NULL ||
      strstr(output, "\n\n* ERROR: no color\n\n") == NULL) {
    fprintf(stderr, "interactive warning star/spacing mismatch: %s\n", output);
    return 1;
  }
  if (strstr(output, "> Hello\n\n* WARN: useful diagnostic\n\n> Next\n\n") ==
      NULL) {
    fprintf(stderr, "inconsistent prompt/notice spacing: %s\n", output);
    return 1;
  }
  if (strstr(output, "[reasoning]") != NULL ||
      strstr(output, "Actual provider") != NULL ||
      strstr(output, "Next header") != NULL ||
      strstr(output, "? First body line Second body line") == NULL ||
      strstr(output, "Assistant text") == NULL ||
      strstr(output, "? Assistant text") != NULL ||
      strstr(output, "\033[1m? Decorated\n\033[0m\n? Plain row\n") == NULL ||
      strstr(output, "Thinking...") != NULL ||
      strstr(output, "* WARN: **literal** _file_ `tick` [name](url)") == NULL ||
      strstr(output, "\n  word") == NULL ||
      strstr(output,
             "Local **literal** _file_ `tick` [name](url)\n\n? First body") ==
          NULL) {
    fprintf(stderr, "reasoning header/body routing failed: %s\n", output);
    return 1;
  }
  return 0;
}
