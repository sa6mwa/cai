#include <cai/agent_runtime.h>

#include "../common.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define RESET "\033[0m"
#define GRAY "\033[90m"
#define GREEN "\033[32m"

typedef struct render_state {
  int terminal_lines;
  int terminal_omitted;
} render_state;

static void render_terminal_output(render_state *state, const char *data,
                                   size_t length) {
  size_t i;
  size_t start;

  start = 0U;
  for (i = 0U; i <= length; i++) {
    if (i != length && data[i] != '\n') {
      continue;
    }
    if (state->terminal_lines < 10) {
      fprintf(stdout, GRAY "%.*s" RESET "\n", (int)(i - start), data + start);
      state->terminal_lines++;
    } else if (!state->terminal_omitted) {
      fputs(GRAY "… more output omitted" RESET "\n", stdout);
      state->terminal_omitted = 1;
    }
    start = i + 1U;
  }
}

static int render_event(void *context, const cai_agent_runtime_event *event,
                        cai_error *error) {
  render_state *state;

  (void)error;
  state = (render_state *)context;
  if (event->type == CAI_AGENT_EVENT_TEXT_DELTA) {
    fwrite(event->data, 1U, event->data_length, stdout);
    fflush(stdout);
  } else if (event->type == CAI_AGENT_EVENT_TERMINAL_COMMAND_STARTED) {
    state->terminal_lines = 0;
    state->terminal_omitted = 0;
    fprintf(stdout, "$ %.*s\n", (int)event->data_length, event->data);
  } else if (event->type == CAI_AGENT_EVENT_TERMINAL_OUTPUT) {
    render_terminal_output(state, event->data, event->data_length);
  } else if (event->type == CAI_AGENT_EVENT_TERMINAL_WAITING) {
    fputs(GRAY "Waiting for terminal progress…" RESET "\n", stdout);
  } else if (event->type == CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED) {
    fputs("Ran terminal command\n", stdout);
  } else if (event->type == CAI_AGENT_EVENT_TOOL_CALL_COMPLETED &&
             event->tool_name != NULL) {
    fprintf(stdout, GRAY "Completed %s" RESET "\n", event->tool_name);
  }
  return CAI_OK;
}

int main(void) {
  cai_client_config client_config;
  cai_agent_runtime_config runtime_config;
  cai_client *client;
  cai_agent_runtime *runtime;
  cai_agent_run_state status;
  cai_error error;
  render_state renderer;
  char *dotenv_api_key;
  char workspace[4096];
  char line[4096];
  int rc;

  cai_error_init(&error);
  client = NULL;
  runtime = NULL;
  dotenv_api_key = NULL;
  memset(&renderer, 0, sizeof(renderer));
  if (getcwd(workspace, sizeof(workspace)) == NULL) {
    fputs("getcwd failed\n", stderr);
    return 1;
  }
  cai_client_config_init(&client_config);
  rc = cai_example_load_dotenv_api_key(&client_config, &dotenv_api_key, &error);
  if (rc == CAI_OK) {
    rc = cai_client_open(&client_config, &client, &error);
  }
  cai_agent_runtime_config_init(&runtime_config);
  runtime_config.workspace_directory = workspace;
  runtime_config.event_callback = render_event;
  runtime_config.event_context = &renderer;
  if (rc == CAI_OK) {
    rc = cai_agent_runtime_open(client, &runtime_config, &runtime, &error);
  }
  while (rc == CAI_OK && fputs("smith> ", stdout) >= 0 &&
         fflush(stdout) == 0 && fgets(line, sizeof(line), stdin) != NULL) {
    line[strcspn(line, "\r\n")] = '\0';
    rc = cai_agent_runtime_submit(runtime, line, &error);
    while (rc == CAI_OK) {
      rc = cai_agent_runtime_pump(runtime, 100L, &error);
      if (rc == CAI_OK) {
        rc = cai_agent_runtime_state(runtime, &status, &error);
      }
      if (rc == CAI_OK && (status == CAI_AGENT_COMPLETED ||
                           status == CAI_AGENT_FAILED ||
                           status == CAI_AGENT_CANCELLED)) {
        fputc('\n', stdout);
        break;
      }
    }
  }
  if (rc != CAI_OK && error.message != NULL) {
    fprintf(stderr, "smith-terminal: %s\n", error.message);
  }
  cai_agent_runtime_close(runtime);
  cai_client_close(client);
  cai_string_destroy(dotenv_api_key);
  cai_error_cleanup(&error);
  return rc == CAI_OK ? 0 : 1;
}
