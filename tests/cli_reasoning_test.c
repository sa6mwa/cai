/* Drive the CLI's owner-thread event renderer with provider-shaped chunks. */
#define main cai_cli_app_main
int cai_cli_app_main(int argc, char **argv);
#include "../cli/main.c"
#undef main

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
  event.type = CAI_AGENT_EVENT_REASONING_SUMMARY;
  event.data = "Actual provider ";
  event.data_length = strlen(event.data);
  rc = cli_event(&state, &event, NULL);
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
  if (count < 0 || rc != CAI_OK || state.reasoning_open) {
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
