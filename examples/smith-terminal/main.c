#include <cai/agent_runtime.h>

#include "../common.h"

#include <errno.h>
#include <poll.h>
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

static const char *skip_space(const char *text) {
  while (*text == ' ' || *text == '\t') {
    text++;
  }
  return text;
}

static int start_review(cai_agent_runtime *parent, const char *command,
                        cai_agent_runtime **out_review, cai_error *error) {
  cai_agent_review_request request;
  const char *argument;

  if (out_review == NULL) {
    return CAI_ERR_INVALID;
  }
  *out_review = NULL;
  argument = skip_space(command + 7U);
  cai_agent_review_request_init(&request);
  if (*argument == '\0' || strcmp(argument, "uncommitted") == 0) {
    request.target = CAI_AGENT_REVIEW_UNCOMMITTED;
  } else if (strncmp(argument, "base ", 5U) == 0) {
    request.target = CAI_AGENT_REVIEW_BASE_BRANCH;
    request.base_branch = skip_space(argument + 5U);
  } else if (strncmp(argument, "commit ", 7U) == 0) {
    request.target = CAI_AGENT_REVIEW_COMMIT;
    request.commit = skip_space(argument + 7U);
  } else {
    /* Preserve Codex-like free-form review scope verbatim. */
    request.target = CAI_AGENT_REVIEW_CUSTOM;
    request.instructions = argument;
  }
  return cai_agent_runtime_start_review(parent, &request, out_review, error);
}

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
  } else if (event->type == CAI_AGENT_EVENT_TURN_QUEUED) {
    fprintf(stdout, GRAY "Queued next turn" RESET "\n");
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
  cai_agent_runtime *review;
  cai_agent_run_state status;
  cai_error error;
  struct pollfd poll_fds[3];
  render_state renderer;
  char *dotenv_api_key;
  char workspace[4096];
  char line[4096];
  char exported_path[8192];
  int exit_requested;
  int prompt_shown;
  int wakeup_fd;
  int rc;

  cai_error_init(&error);
  client = NULL;
  runtime = NULL;
  review = NULL;
  dotenv_api_key = NULL;
  exit_requested = 0;
  prompt_shown = 0;
  wakeup_fd = -1;
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
  while (rc == CAI_OK && !exit_requested) {
    int poll_result;

    if (!prompt_shown) {
      if (fputs("smith> ", stdout) < 0 || fflush(stdout) != 0) {
        fputs("smith-terminal: failed to write prompt\n", stderr);
        rc = CAI_ERR_TRANSPORT;
        break;
      }
      prompt_shown = 1;
    }
    rc = cai_agent_runtime_wakeup_fd(runtime, &wakeup_fd, &error);
    if (rc != CAI_OK) {
      break;
    }
    memset(poll_fds, 0, sizeof(poll_fds));
    poll_fds[0].fd = STDIN_FILENO;
    poll_fds[0].events = POLLIN;
    poll_fds[1].fd = wakeup_fd;
    poll_fds[1].events = POLLIN;
    if (review != NULL) {
      rc = cai_agent_runtime_wakeup_fd(review, &poll_fds[2].fd, &error);
      if (rc != CAI_OK) {
        break;
      }
      poll_fds[2].events = POLLIN;
    }
    poll_result = poll(poll_fds, review != NULL ? 3U : 2U, 100);
    if (poll_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      fputs("smith-terminal: failed to poll input and runtime events\n",
            stderr);
      rc = CAI_ERR_TRANSPORT;
      break;
    }
    if ((poll_fds[1].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
      rc = cai_agent_runtime_pump(runtime, 0L, &error);
      if (rc != CAI_OK) {
        break;
      }
    }
    if (review != NULL &&
        (poll_fds[2].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
      rc = cai_agent_runtime_pump(review, 0L, &error);
      if (rc != CAI_OK) {
        break;
      }
      rc = cai_agent_runtime_state(review, &status, &error);
      if (rc != CAI_OK) {
        break;
      }
      if (status == CAI_AGENT_COMPLETED || status == CAI_AGENT_FAILED ||
          status == CAI_AGENT_CANCELLED) {
        rc = cai_agent_runtime_finish_review(runtime, review, &error);
        if (rc != CAI_OK) {
          break;
        }
        cai_agent_runtime_close(review);
        review = NULL;
      }
    }
    if ((poll_fds[0].revents & (POLLIN | POLLERR | POLLHUP)) == 0) {
      continue;
    }
    if (fgets(line, sizeof(line), stdin) == NULL) {
      break;
    }
    prompt_shown = 0;
    line[strcspn(line, "\r\n")] = '\0';
    if (strcmp(line, "/exit") == 0 || strcmp(line, "/quit") == 0) {
      exit_requested = 1;
      continue;
    }
    if (strcmp(line, "/export") == 0) {
      rc = cai_agent_runtime_export_markdown_file(
          runtime, "cai", NULL, exported_path, sizeof(exported_path), &error);
      if (rc == CAI_OK) {
        fprintf(stdout, GRAY "Exported %s" RESET "\n", exported_path);
      } else {
        fprintf(stderr, "smith-terminal: export failed: %s\n",
                error.message != NULL ? error.message : "unknown error");
        cai_error_cleanup(&error);
        cai_error_init(&error);
        rc = CAI_OK;
      }
      continue;
    }
    if (strncmp(line, "/review", 7U) == 0 &&
        (line[7] == '\0' || line[7] == ' ' || line[7] == '\t')) {
      if (review != NULL) {
        fputs(GRAY "Review already in progress" RESET "\n", stdout);
        continue;
      }
      rc = start_review(runtime, line, &review, &error);
      if (review != NULL) {
        fputs(GRAY "Started isolated review" RESET "\n", stdout);
      }
      if (rc != CAI_OK) {
        fprintf(stderr, "smith-terminal: review start failed: %s\n",
                error.message != NULL ? error.message : "unknown error");
        cai_error_cleanup(&error);
        cai_error_init(&error);
        /* A durable-pause checkpoint error may still return a live child. */
        if (review != NULL) {
          rc = CAI_OK;
        }
      }
      continue;
    }
    if (line[0] == '\0') {
      continue;
    }
    if (strncmp(line, "/queue ", 7U) == 0) {
      rc = cai_agent_runtime_submit_queued(runtime, line + 7, &error);
    } else if (review != NULL) {
      rc = cai_agent_runtime_submit_queued(runtime, line, &error);
    } else {
      rc = cai_agent_runtime_state(runtime, &status, &error);
      if (rc == CAI_OK && (status == CAI_AGENT_SAMPLING ||
                           status == CAI_AGENT_DISPATCHING_TOOL)) {
        rc = cai_agent_runtime_submit_steering(runtime, line, &error);
      } else if (rc == CAI_OK) {
        rc = cai_agent_runtime_submit(runtime, line, &error);
      }
    }
  }
  if (rc != CAI_OK && error.message != NULL) {
    fprintf(stderr, "smith-terminal: %s\n", error.message);
  }
  cai_agent_runtime_close(review);
  cai_agent_runtime_close(runtime);
  if (client != NULL) {
    client->close(client);
  }
  cai_string_destroy(dotenv_api_key);
  cai_error_cleanup(&error);
  return rc == CAI_OK ? 0 : 1;
}
