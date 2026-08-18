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
  char command[161];
} render_state;

static void remember_command(render_state *state, const char *data,
                             size_t length) {
  size_t count;

  count = length < sizeof(state->command) - 1U ? length
                                                : sizeof(state->command) - 1U;
  if (count > 3U && count < length) {
    count -= 3U;
    memcpy(state->command, data, count);
    memcpy(state->command + count, "...", 3U);
    count += 3U;
  } else if (count > 0U) {
    memcpy(state->command, data, count);
  }
  state->command[count] = '\0';
}

static void render_terminal_output(render_state *state, const char *data,
                                   size_t length) {
  size_t i;
  size_t start;

  start = 0U;
  while (start < length) {
    for (i = start; i < length && data[i] != '\n'; i++) {
    }
    if (state->terminal_lines < 10) {
      fwrite(GRAY, 1U, strlen(GRAY), stdout);
      fwrite(data + start, 1U, i - start, stdout);
      if (i < length) {
        fputc('\n', stdout);
        state->terminal_lines++;
      }
      fwrite(RESET, 1U, strlen(RESET), stdout);
    } else if (!state->terminal_omitted) {
      fputs(GRAY "… more output omitted" RESET "\n", stdout);
      state->terminal_omitted = 1;
    }
    if (i == length) {
      break;
    }
    start = i + 1U;
  }
}

static void render_terminal_finish(const cai_agent_runtime_event *event,
                                   const char *verb,
                                   const render_state *state) {
  const char *status;

  if (event->terminal_has_exit_code) {
    fprintf(stdout, "%s %s (exit %lld, %.1fs)\n", verb, state->command,
            event->terminal_exit_code,
            (double)event->terminal_duration_ms / 1000.0);
    return;
  }
  status = event->terminal_has_signal ? "signal" : "status unavailable";
  if (event->terminal_has_signal) {
    fprintf(stdout, "%s %s (%s %lld, %.1fs)\n", verb, state->command, status,
            event->terminal_signal,
            (double)event->terminal_duration_ms / 1000.0);
  } else {
    fprintf(stdout, "%s %s (%s, %.1fs)\n", verb, state->command, status,
            (double)event->terminal_duration_ms / 1000.0);
  }
}

static const char *tool_action_verb(int action) {
  switch (action) {
  case CAI_AGENT_TOOL_ACTION_READ:
    return "Read";
  case CAI_AGENT_TOOL_ACTION_LIST:
    return "Listed";
  case CAI_AGENT_TOOL_ACTION_VIEW:
    return "Viewed";
  case CAI_AGENT_TOOL_ACTION_PATCH:
    return "Patched";
  case CAI_AGENT_TOOL_ACTION_GET_GOAL:
    return "Read goal";
  case CAI_AGENT_TOOL_ACTION_CREATE_GOAL:
    return "Created goal";
  case CAI_AGENT_TOOL_ACTION_UPDATE_GOAL:
    return "Updated goal";
  case CAI_AGENT_TOOL_ACTION_CLEAR_GOAL:
    return "Cleared goal";
  case CAI_AGENT_TOOL_ACTION_IMAGE_GENERATION:
    return "Generated image";
  default:
    return NULL;
  }
}

static void render_tool_completion(const cai_agent_runtime_event *event) {
  const char *verb;

  verb = tool_action_verb(event->tool_action);
  if (verb != NULL && event->tool_path != NULL) {
    fprintf(stdout, GRAY "%s %s" RESET "\n", verb, event->tool_path);
  } else if (verb != NULL && event->tool_path_count > 1U) {
    fprintf(stdout, GRAY "%s %lu files" RESET "\n", verb,
            (unsigned long)event->tool_path_count);
  } else if (verb != NULL) {
    fprintf(stdout, GRAY "%s" RESET "\n", verb);
  } else {
    fprintf(stdout, GRAY "Completed %s" RESET "\n",
            event->tool_name != NULL ? event->tool_name : "tool");
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
    remember_command(state, event->data, event->data_length);
    fprintf(stdout, "$ %.*s\n", (int)event->data_length, event->data);
  } else if (event->type == CAI_AGENT_EVENT_TERMINAL_OUTPUT) {
    render_terminal_output(state, event->data, event->data_length);
  } else if (event->type == CAI_AGENT_EVENT_TERMINAL_WAITING) {
    fputs(GRAY "Waiting for terminal progress…" RESET "\n", stdout);
  } else if (event->type == CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED) {
    render_terminal_finish(event, "Ran", state);
  } else if (event->type == CAI_AGENT_EVENT_TERMINAL_COMMAND_CANCELLED) {
    render_terminal_finish(event, "Cancelled", state);
  } else if (event->type == CAI_AGENT_EVENT_TOOL_CALL_COMPLETED &&
             event->tool_name != NULL &&
             strcmp(event->tool_name, CAI_TERMINAL_EXEC_TOOL_NAME) != 0 &&
             strcmp(event->tool_name, CAI_TERMINAL_WRITE_TOOL_NAME) != 0) {
    render_tool_completion(event);
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
    if (strcmp(line, "/exit") == 0 || strcmp(line, "/quit") == 0) {
      break;
    }
    if (line[0] == '\0') {
      continue;
    }
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
