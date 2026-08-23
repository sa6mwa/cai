#include <cai/agent_runtime.h>
#include <lonejson.h>

#include "../common.h"

#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define RESET "\033[0m"
#define GRAY "\033[90m"
#define GREEN "\033[32m"
#define MAGENTA "\033[35m"
#define RED "\033[31m"

typedef struct render_state {
  int terminal_lines;
  int terminal_omitted;
  int text_open;
  int reasoning_open;
  int suppress_review_text;
  int review_report_visible;
  char command[161];
} render_state;

typedef struct review_line_range {
  lonejson_int64 start;
  lonejson_int64 end;
} review_line_range;

typedef struct review_code_location {
  char *absolute_file_path;
  review_line_range line_range;
} review_code_location;

typedef struct review_finding {
  char *title;
  char *body;
  double confidence_score;
  lonejson_int64 priority;
  int priority_present;
  review_code_location code_location;
} review_finding;

typedef struct review_report {
  lonejson_object_array findings;
  char *overall_correctness;
  char *overall_explanation;
  double overall_confidence_score;
} review_report;

static const lonejson_field review_line_range_fields[] = {
    LONEJSON_FIELD_I64_REQ(review_line_range, start, "start"),
    LONEJSON_FIELD_I64_REQ(review_line_range, end, "end")};
LONEJSON_MAP_DEFINE(review_line_range_map, review_line_range,
                    review_line_range_fields);

static const lonejson_field review_code_location_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(review_code_location, absolute_file_path,
                                    "absolute_file_path"),
    LONEJSON_FIELD_OBJECT_REQ(review_code_location, line_range, "line_range",
                              &review_line_range_map)};
LONEJSON_MAP_DEFINE(review_code_location_map, review_code_location,
                    review_code_location_fields);

static const lonejson_field review_finding_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(review_finding, title, "title"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(review_finding, body, "body"),
    LONEJSON_FIELD_F64_REQ(review_finding, confidence_score,
                           "confidence_score"),
    LONEJSON_FIELD_I64_PRESENT_NULLABLE(review_finding, priority,
                                        priority_present, "priority"),
    LONEJSON_FIELD_OBJECT_REQ(review_finding, code_location, "code_location",
                              &review_code_location_map)};
LONEJSON_MAP_DEFINE(review_finding_map, review_finding, review_finding_fields);

static const lonejson_field review_report_fields[] = {
    {"findings", sizeof("findings") - 1U, (unsigned char)'f',
     (unsigned char)'s', offsetof(review_report, findings),
     LONEJSON_FIELD_KIND_OBJECT_ARRAY, LONEJSON_STORAGE_DYNAMIC,
     LONEJSON_OVERFLOW_FAIL, LONEJSON_FIELD_REQUIRED, 0U,
     sizeof(review_finding), &review_finding_map, NULL, 0U,
     LONEJSON_SPOOL_CLASS_DEFAULT},
    LONEJSON_FIELD_STRING_ALLOC_REQ(review_report, overall_correctness,
                                    "overall_correctness"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(review_report, overall_explanation,
                                    "overall_explanation"),
    LONEJSON_FIELD_F64_REQ(review_report, overall_confidence_score,
                           "overall_confidence_score")};
LONEJSON_MAP_DEFINE(review_report_map, review_report, review_report_fields);

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

static void render_close_message(render_state *state) {
  if (state->text_open || state->reasoning_open) {
    fputs(RESET "\n", stdout);
    state->text_open = 0;
    state->reasoning_open = 0;
  }
}

/* Reviewer strings are model-controlled. Render control code points visibly
 * instead of allowing decoded JSON escapes to become terminal commands. */
static void render_review_text(const char *text, size_t length) {
  size_t i;

  for (i = 0U; i < length; i++) {
    unsigned char character;

    character = (unsigned char)text[i];
    /* U+0080 through U+009F are encoded as C2 80 through C2 9F in UTF-8.
     * Treat those C1 controls visibly too, without disturbing other UTF-8. */
    if (character == 0xC2U && i + 1U < length &&
        (unsigned char)text[i + 1U] >= 0x80U &&
        (unsigned char)text[i + 1U] <= 0x9FU) {
      fprintf(stdout, "\\u00%02X", (unsigned char)text[i + 1U]);
      i++;
    } else if (character < 0x20U ||
               (character >= 0x7FU && character <= 0x9FU)) {
      fprintf(stdout, "\\x%02X", character);
    } else {
      fputc((int)character, stdout);
    }
  }
}

static void render_review_string(const char *text) {
  render_review_text(text, strlen(text));
}

static void render_review_body(const char *body) {
  const char *line;
  const char *end;

  line = body;
  while (*line != '\0') {
    end = strchr(line, '\n');
    if (end == NULL) {
      fputs("  ", stdout);
      render_review_text(line, strlen(line));
      fputc('\n', stdout);
      return;
    }
    fputs("  ", stdout);
    render_review_text(line, (size_t)(end - line));
    fputc('\n', stdout);
    line = end + 1;
  }
}

/* The runtime validates reviewer output before it emits the report. The event
 * retains its portable JSON payload, while this terminal presentation renders
 * the validated fields as an operator-facing review result. */
static void render_review_report(const char *data, size_t length) {
  review_report report;
  review_finding *findings;
  lonejson *json;
  lonejson_error error;
  lonejson_status status;
  size_t i;

  memset(&report, 0, sizeof(report));
  lonejson_error_init(&error);
  json = lonejson_new(NULL, &error);
  if (json == NULL) {
    fputs(RED "Reviewer report could not be displayed" RESET "\n", stdout);
    return;
  }
  lonejson_init(json, &review_report_map, &report);
  status = lonejson_parse_buffer(json, &review_report_map, &report, data,
                                 length, &error);
  if (status != LONEJSON_STATUS_OK) {
    fputs(RED "Reviewer report could not be displayed" RESET "\n", stdout);
    lonejson_cleanup(&review_report_map, &report);
    lonejson_free(json);
    return;
  }
  if (report.overall_explanation[0] != '\0') {
    render_review_string(report.overall_explanation);
    fputc('\n', stdout);
  }
  findings = (review_finding *)report.findings.items;
  if (report.findings.count > 0U) {
    if (report.overall_explanation[0] != '\0') {
      fputc('\n', stdout);
    }
    fputs(report.findings.count > 1U ? "Full review comments:\n"
                                     : "Review comment:\n",
          stdout);
  } else if (report.overall_explanation[0] == '\0') {
    /* The review contract permits a valid verdict without prose or findings.
     * It is still a completed review, not a failed reviewer response. */
    fputs("Review completed: ", stdout);
    render_review_string(report.overall_correctness);
    fputs(".\n", stdout);
  }
  for (i = 0U; i < report.findings.count; i++) {
    fputc('\n', stdout);
    fputs("- ", stdout);
    render_review_string(findings[i].title);
    fputs(" — ", stdout);
    render_review_string(findings[i].code_location.absolute_file_path);
    fprintf(stdout, ":%lld-%lld\n", findings[i].code_location.line_range.start,
            findings[i].code_location.line_range.end);
    render_review_body(findings[i].body);
  }
  lonejson_cleanup(&review_report_map, &report);
  lonejson_free(json);
}

static int runtime_accepts_prompt(cai_agent_run_state state) {
  return state == CAI_AGENT_IDLE || state == CAI_AGENT_COMPLETED ||
         state == CAI_AGENT_FAILED || state == CAI_AGENT_CANCELLED;
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
  if (event->type == CAI_AGENT_EVENT_REASONING_SUMMARY) {
    if (!state->reasoning_open) {
      render_close_message(state);
      fputs(MAGENTA "Thinking: " RESET, stdout);
      state->reasoning_open = 1;
    }
    fwrite(event->data, 1U, event->data_length, stdout);
    fflush(stdout);
  } else if (event->type == CAI_AGENT_EVENT_TEXT_DELTA) {
    if (state->suppress_review_text) {
      return CAI_OK;
    }
    if (!state->text_open) {
      render_close_message(state);
      fputs(GREEN "Smith: " RESET, stdout);
      state->text_open = 1;
    }
    fwrite(event->data, 1U, event->data_length, stdout);
    fflush(stdout);
  } else if (event->type == CAI_AGENT_EVENT_RESPONSE_COMPLETED) {
    /* A steering follow-up remains in the same run but starts a new model
     * response, so the next text/reasoning delta receives its own label. */
    render_close_message(state);
  } else if (event->type == CAI_AGENT_EVENT_TERMINAL_COMMAND_STARTED) {
    render_close_message(state);
    state->terminal_lines = 0;
    state->terminal_omitted = 0;
    remember_command(state, event->data, event->data_length);
    fprintf(stdout, "$ %.*s\n", (int)event->data_length, event->data);
  } else if (event->type == CAI_AGENT_EVENT_TERMINAL_OUTPUT) {
    render_close_message(state);
    render_terminal_output(state, event->data, event->data_length);
  } else if (event->type == CAI_AGENT_EVENT_TERMINAL_WAITING) {
    render_close_message(state);
    fputs(GRAY "Waiting for terminal progress…" RESET "\n", stdout);
  } else if (event->type == CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED) {
    render_close_message(state);
    render_terminal_finish(event, "Ran", state);
  } else if (event->type == CAI_AGENT_EVENT_TERMINAL_COMMAND_CANCELLED) {
    render_close_message(state);
    render_terminal_finish(event, "Cancelled", state);
  } else if (event->type == CAI_AGENT_EVENT_TURN_QUEUED) {
    render_close_message(state);
    fprintf(stdout, GRAY "Queued next turn" RESET "\n");
  } else if (event->type == CAI_AGENT_EVENT_REVIEW_REPORT) {
    render_close_message(state);
    render_review_report(event->data, event->data_length);
    state->review_report_visible = 1;
  } else if (event->type == CAI_AGENT_EVENT_REVIEW_HANDED_OFF) {
    render_close_message(state);
    /* A review child normally emitted its report while it ran. The durable
     * parent handoff carries that same report as a fallback, so never replace
     * a result with an opaque implementation receipt. */
    if (!state->review_report_visible && event->data_length > 0U) {
      render_review_report(event->data, event->data_length);
    }
    state->review_report_visible = 0;
  } else if (event->type == CAI_AGENT_EVENT_TOOL_CALL_COMPLETED &&
             event->tool_name != NULL &&
             strcmp(event->tool_name, CAI_TERMINAL_EXEC_TOOL_NAME) != 0 &&
             strcmp(event->tool_name, CAI_TERMINAL_WRITE_TOOL_NAME) != 0) {
    render_close_message(state);
    render_tool_completion(event);
  } else if (event->type == CAI_AGENT_EVENT_RUN_FAILED) {
    render_close_message(state);
    fprintf(stdout, RED "Smith failed: %.*s" RESET "\n",
            (int)event->data_length,
            event->data != NULL ? event->data : "agent run failed");
  } else if (event->type == CAI_AGENT_EVENT_RUN_COMPLETED) {
    render_close_message(state);
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
  int input_enabled;
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

    rc = cai_agent_runtime_state(runtime, &status, &error);
    if (rc != CAI_OK) {
      break;
    }
    /* A canonical terminal cannot redraw a live input line around streamed
     * output. Keep this compact example coherent by prompting only at a stable
     * boundary; a TUI can instead submit steering or queued turns while active.
     */
    input_enabled = review == NULL && runtime_accepts_prompt(status);
    if (input_enabled && !prompt_shown) {
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
    poll_fds[0].fd = input_enabled ? STDIN_FILENO : -1;
    poll_fds[0].events = input_enabled ? POLLIN : 0;
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
      renderer.suppress_review_text = 1;
      rc = cai_agent_runtime_pump(review, 0L, &error);
      renderer.suppress_review_text = 0;
      if (rc != CAI_OK) {
        break;
      }
      rc = cai_agent_runtime_state(review, &status, &error);
      if (rc != CAI_OK) {
        break;
      }
      if (status == CAI_AGENT_COMPLETED || status == CAI_AGENT_FAILED ||
          status == CAI_AGENT_CANCELLED) {
        /* State becomes terminal before a host necessarily observes every
         * final event. Drain once more so the report cannot be skipped. */
        renderer.suppress_review_text = 1;
        rc = cai_agent_runtime_pump(review, 0L, &error);
        renderer.suppress_review_text = 0;
        if (rc != CAI_OK) {
          break;
        }
        rc = cai_agent_runtime_finish_review(runtime, review, &error);
        if (rc != CAI_OK) {
          break;
        }
        /* Drain the durable parent handoff before the next prompt. */
        rc = cai_agent_runtime_pump(runtime, 0L, &error);
        if (rc != CAI_OK) {
          break;
        }
        cai_agent_runtime_close(review);
        review = NULL;
      }
    }
    if (!input_enabled ||
        (poll_fds[0].revents & (POLLIN | POLLERR | POLLHUP)) == 0) {
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
    rc = cai_agent_runtime_submit(runtime, line, &error);
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
