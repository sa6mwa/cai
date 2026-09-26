/* Drive the CLI's owner-thread event renderer with provider-shaped chunks. */
#define main cai_cli_app_main
int cai_cli_app_main(int argc, char **argv);
#include "../cli/main.c"
#undef main

typedef struct event_log_capture {
  char bytes[16384];
  size_t length;
} event_log_capture;

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

int main(void) {
  cli_state state;
  cai_agent_runtime_event event;
  mdf_options renderer_options;
  mdf_sink sink;
  int pipe_fd[2];
  int saved_stdout;
  char output[512];
  char *label;
  ssize_t count;
  int rc;
  struct timespec start = {100, 500000000L};
  struct timespec before = {400, 499999999L};
  struct timespec boundary = {400, 500000000L};

  if (test_event_logging() != 0)
    return 1;
  memset(&state, 0, sizeof(state));
  if (!cli_quota_due(0, start, start, 0) ||
      cli_quota_due(1, start, before, 0) ||
      !cli_quota_due(1, start, boundary, 0) ||
      !cli_quota_due(1, start, before, 1)) {
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
  sink.write = cli_sink;
  if (state.renderer->set_sink(state.renderer, &sink) != MDF_OK) {
    return 1;
  }
  strcpy(state.reasoning_summary_raw, "Previous turn");
  state.reasoning_summary_length = strlen(state.reasoning_summary_raw);
  event.type = CAI_AGENT_EVENT_RUN_STARTED;
  rc = cli_event(&state, &event, NULL);
  if (state.reasoning_summary_length != 0U)
    return 1;
  event.type = CAI_AGENT_EVENT_REASONING_SUMMARY;
  event.data = "Actual provider ";
  event.data_length = strlen(event.data);
  if (rc == CAI_OK)
    rc = cli_event(&state, &event, NULL);
  event.type = CAI_AGENT_EVENT_RUN_STARTED;
  event.parent_tool_call_id = "child-tool";
  if (rc == CAI_OK)
    rc = cli_event(&state, &event, NULL);
  if (strcmp(state.reasoning_summary_raw, "Actual provider ") != 0)
    return 1;
  event.parent_tool_call_id = NULL;
  event.type = CAI_AGENT_EVENT_REASONING_SUMMARY;
  event.data = "summary";
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
  sl_destroy(state.sl);
  if (dup2(saved_stdout, STDOUT_FILENO) < 0) {
    return 1;
  }
  close(saved_stdout);
  count = read(pipe_fd[0], output, sizeof(output) - 1U);
  close(pipe_fd[0]);
  if (count < 0 || rc != CAI_OK || state.reasoning_open ||
      strcmp(state.reasoning_summary_raw, "Actual provider summary") != 0) {
    return 1;
  }
  output[count] = '\0';
  label = strstr(output, "[reasoning]");
  if (label == NULL || strstr(label + 1, "[reasoning]") != NULL ||
      strstr(output, "Actual provider summary") == NULL ||
      strstr(output, "Thinking...") != NULL) {
    fprintf(stderr, "provider reasoning was not streamed once: %s\n", output);
    return 1;
  }
  return 0;
}
