#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "login.h"
#include "options.h"
#include "review_report.h"
#include "status.h"

#include <cai/agent_runtime.h>
#include <cai/auth.h>
#include <cai/quota.h>
#include <cai/session_store.h>
#include <libmdf/mdf.h>
#include <softline/softline.h>

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct cli_session {
  char id[CAI_AGENT_SESSION_ID_MAX];
  unsigned long long checkpoint_ns;
} cli_session;

typedef struct cli_state {
  cai_cli_options options;
  cai_agent_session_store store;
  cai_chatgpt_auth *auth;
  cai_client *client;
  cai_client *quota_client;
  cai_agent_runtime *runtime;
  sl_t *sl;
  mdf *renderer;
  FILE *activity;
  char *review_report;
  size_t review_report_length;
  char generated_goal[4096];
  cli_session *sessions;
  size_t session_count;
  size_t session_capacity;
  sl_watch_id_t watch_id;
  sl_watch_id_t timer_watch_id;
  int has_watch;
  int has_timer_watch;
  int timer_fd;
  int timer_armed;
  int interactive;
  int width;
  int response_open;
  int documents_rendered;
  int reasoning_open;
  int exit_requested;
  int was_busy;
  int awaiting_turn;
  int automation_done;
  int automation_failed;
  int auto_goal;
  size_t next_instruction;
  cai_chatgpt_quota quota;
  struct timespec quota_last_attempt;
  int quota_attempted;
  cai_cli_status status;
  char reasoning_summary_raw[512];
  size_t reasoning_summary_length;
  char workspace[PATH_MAX];
} cli_state;

static int cli_advance_automation(cli_state *state, cai_error *error);

static void cli_print_error(const char *operation, const cai_error *error) {
  fprintf(stderr, "cai: %s: %s\n", operation,
          error != NULL && error->message != NULL ? error->message
                                                  : "operation failed");
  if (error != NULL && error->detail != NULL) {
    fprintf(stderr, "cai: detail: %s\n", error->detail);
  }
}

static int cli_quota_due(int attempted, struct timespec last_attempt,
                         struct timespec now, int force) {
  time_t elapsed;
  if (force || !attempted || now.tv_sec < last_attempt.tv_sec)
    return 1;
  elapsed = now.tv_sec - last_attempt.tv_sec;
  return elapsed > 300 ||
         (elapsed == 300 && now.tv_nsec >= last_attempt.tv_nsec);
}

static void cli_refresh_quota(cli_state *state, int force) {
  struct timespec now;
  cai_error optional_error;
  cai_chatgpt_quota fresh;
  if (state->quota_client == NULL ||
      clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return;
  }
  if (!cli_quota_due(state->quota_attempted, state->quota_last_attempt, now,
                     force)) {
    return;
  }
  state->quota_attempted = 1;
  state->quota_last_attempt = now;
  memset(&fresh, 0, sizeof(fresh));
  cai_error_init(&optional_error);
  if (cai_client_chatgpt_quota(state->quota_client, &fresh, &optional_error) ==
      CAI_OK) {
    state->quota = fresh;
  } else {
    memset(&state->quota, 0, sizeof(state->quota));
  }
  cai_error_cleanup(&optional_error);
}

static int cli_sink(void *context, const char *bytes, size_t count) {
  cli_state *state;
  state = (cli_state *)context;
  if (state->activity != NULL)
    return fwrite(bytes, 1U, count, state->activity) == count ? 0 : -1;
  return sl_output_stream_write(state->sl, bytes, count) == SL_OK ? 0 : -1;
}

static int cli_geometry(cli_state *state) {
  int width;
  width = mdf_terminal_width(
      state->activity != NULL ? STDERR_FILENO : STDOUT_FILENO, 80);
  if (width == state->width) {
    return 0;
  }
  if (state->interactive && sl_set_bounds(state->sl, 0, 0, 0, 0) != SL_OK) {
    return -1;
  }
  if (state->renderer != NULL &&
      state->renderer->set_geometry(state->renderer, width, width >= 5 ? 2 : 0,
                                    0) != MDF_OK) {
    return -1;
  }
  state->width = width;
  return 0;
}

static int cli_finish_response(cli_state *state) {
  if (!state->response_open) {
    return 0;
  }
  if (state->renderer->finish_document(state->renderer) != MDF_OK) {
    return -1;
  }
  state->documents_rendered++;
  state->response_open = 0;
  return 0;
}

static int cli_finish_reasoning(cli_state *state) {
  if (!state->reasoning_open) {
    return 0;
  }
  if (state->renderer->finish_document(state->renderer) != MDF_OK) {
    return -1;
  }
  state->documents_rendered++;
  state->reasoning_open = 0;
  return 0;
}

static int cli_text(cli_state *state, const char *data, size_t length) {
  if (cli_finish_reasoning(state) != 0) {
    return -1;
  }
  if (cli_geometry(state) != 0) {
    return -1;
  }
  if (!state->response_open) {
    if (state->documents_rendered > 0 &&
        state->renderer->begin_document(state->renderer) != MDF_OK) {
      return -1;
    }
    state->response_open = 1;
  }
  return state->renderer->feed(state->renderer, data, length) == MDF_OK ? 0
                                                                        : -1;
}

static int cli_write(cli_state *state, const char *text) {
  if (cli_finish_response(state) != 0 || cli_finish_reasoning(state) != 0) {
    return -1;
  }
  if (state->activity != NULL)
    return fputs(text, state->activity) >= 0 ? 0 : -1;
  return sl_output_stream_write(state->sl, text, strlen(text)) == SL_OK ? 0
                                                                        : -1;
}

static int cli_prompt(cli_state *state, const char *text, int history) {
  if (cli_finish_response(state) != 0 || cli_finish_reasoning(state) != 0) {
    return -1;
  }
  if (history && sl_history_add(state->sl, text) != SL_OK) {
    return -1;
  }
  return sl_output_stream_write_quoted_prompt(state->sl, text) == SL_OK ? 0
                                                                        : -1;
}

static void cli_reasoning_append(cli_state *state, const char *data,
                                 size_t length) {
  size_t capacity = sizeof(state->reasoning_summary_raw) - 1U;
  size_t shift;
  size_t i;
  if (length >= capacity) {
    data += length - capacity;
    length = capacity;
    state->reasoning_summary_length = 0U;
  } else if (state->reasoning_summary_length + length > capacity) {
    shift = state->reasoning_summary_length + length - capacity;
    memmove(state->reasoning_summary_raw, state->reasoning_summary_raw + shift,
            state->reasoning_summary_length - shift);
    state->reasoning_summary_length -= shift;
  }
  for (i = 0U; i < length; i++) {
    state->reasoning_summary_raw[state->reasoning_summary_length + i] =
        data[i] == '\0' ? ' ' : data[i];
  }
  state->reasoning_summary_length += length;
  state->reasoning_summary_raw[state->reasoning_summary_length] = '\0';
}

static int cli_event(void *context, const cai_agent_runtime_event *event,
                     cai_error *error) {
  cli_state *state;
  char message[320];
  int n;

  (void)error;
  state = (cli_state *)context;
  if (event->type == CAI_AGENT_EVENT_REVIEW_REPORT) {
    char *report;
    if (event->data == NULL || event->data_length == (size_t)-1 ||
        memchr(event->data, '\0', event->data_length) != NULL)
      return CAI_ERR_PROTOCOL;
    report = (char *)malloc(event->data_length + 1U);
    if (report == NULL)
      return CAI_ERR_NOMEM;
    memcpy(report, event->data, event->data_length);
    report[event->data_length] = '\0';
    free(state->review_report);
    state->review_report = report;
    state->review_report_length = event->data_length;
  }
  if (event->parent_tool_call_id == NULL &&
      event->type == CAI_AGENT_EVENT_RUN_COMPLETED)
    state->awaiting_turn = 0;
  if (event->parent_tool_call_id == NULL &&
      (event->type == CAI_AGENT_EVENT_RUN_FAILED ||
       event->type == CAI_AGENT_EVENT_RUN_CANCELLED)) {
    state->awaiting_turn = 0;
    state->automation_failed = 1;
  }
  if (event->type == CAI_AGENT_EVENT_RUN_STARTED &&
      event->parent_tool_call_id == NULL) {
    state->reasoning_summary_length = 0U;
    state->reasoning_summary_raw[0] = '\0';
  }
  if (event->type == CAI_AGENT_EVENT_TEXT_DELTA) {
    return cli_text(state, event->data, event->data_length) == 0
               ? CAI_OK
               : CAI_ERR_TRANSPORT;
  }
  if (event->type == CAI_AGENT_EVENT_RESPONSE_COMPLETED) {
    return cli_finish_response(state) == 0 && cli_finish_reasoning(state) == 0
               ? CAI_OK
               : CAI_ERR_TRANSPORT;
  }
  if (event->type == CAI_AGENT_EVENT_RUN_FAILED) {
    if (event->data != NULL) {
      if (cli_write(state, "\n[error] ") != 0 ||
          cli_sink(state, event->data, event->data_length) != 0 ||
          cli_write(state, "\n") != 0) {
        return CAI_ERR_TRANSPORT;
      }
    }
  } else if (event->type == CAI_AGENT_EVENT_TOOL_CALL_STARTED &&
             event->tool_name != NULL) {
    n = snprintf(message, sizeof(message), "\n[tool] %s\n", event->tool_name);
    if (n < 0 || (size_t)n >= sizeof(message) ||
        cli_write(state, message) != 0) {
      return CAI_ERR_TRANSPORT;
    }
  } else if (state->activity != NULL &&
             (event->type == CAI_AGENT_EVENT_TOOL_CALL_COMPLETED ||
              event->type == CAI_AGENT_EVENT_TOOL_CALL_FAILED)) {
    if (cli_write(state, event->type == CAI_AGENT_EVENT_TOOL_CALL_COMPLETED
                             ? "\n[tool result] "
                             : "\n[tool error] ") != 0 ||
        (event->data != NULL &&
         cli_sink(state, event->data, event->data_length) != 0) ||
        cli_write(state, "\n") != 0)
      return CAI_ERR_TRANSPORT;
  } else if (state->activity != NULL &&
             event->type == CAI_AGENT_EVENT_TERMINAL_OUTPUT &&
             event->data != NULL) {
    if (cli_write(state, "\n[terminal] ") != 0 ||
        cli_sink(state, event->data, event->data_length) != 0 ||
        cli_write(state, "\n") != 0)
      return CAI_ERR_TRANSPORT;
  } else if (event->type == CAI_AGENT_EVENT_REASONING_SUMMARY &&
             event->data != NULL && event->data_length > 0U) {
    if (event->parent_tool_call_id == NULL)
      cli_reasoning_append(state, event->data, event->data_length);
    if (cli_geometry(state) != 0 || cli_finish_response(state) != 0) {
      return CAI_ERR_TRANSPORT;
    }
    if (!state->reasoning_open) {
      if (cli_sink(state, "\n[reasoning]\n", 13U) != 0 ||
          (state->documents_rendered > 0 &&
           state->renderer->begin_document(state->renderer) != MDF_OK)) {
        return CAI_ERR_TRANSPORT;
      }
    }
    state->reasoning_open = 1;
    if (state->renderer->feed(state->renderer, event->data,
                              event->data_length) != MDF_OK) {
      return CAI_ERR_TRANSPORT;
    }
  }
  if (state->activity != NULL &&
      (event->type == CAI_AGENT_EVENT_RUN_STARTED ||
       event->type == CAI_AGENT_EVENT_RUN_COMPLETED ||
       event->type == CAI_AGENT_EVENT_RUN_CANCELLED)) {
    const char *phase = event->type == CAI_AGENT_EVENT_RUN_STARTED
                            ? "[review started]\n"
                        : event->type == CAI_AGENT_EVENT_RUN_COMPLETED
                            ? "\n[review completed]\n"
                            : "\n[review cancelled]\n";
    if (cli_write(state, phase) != 0)
      return CAI_ERR_TRANSPORT;
  }
  if (state->options.verbosity > 0) {
    if (state->options.verbosity > 1) {
      fprintf(stderr, "cai: event=%d state=%d sequence=%llu\n", event->type,
              event->state, event->sequence);
    } else {
      fprintf(stderr, "cai: event=%d state=%d\n", event->type, event->state);
    }
  }
  return CAI_OK;
}

static int cli_sync_status(cli_state *state, cai_error *error) {
  cai_agent_run_state run_state;
  cai_agent_runtime_settings settings;
  cai_agent_goal_snapshot goal;
  cai_agent_runtime_metrics metrics;
  cai_error optional_error;
  struct itimerspec timer_spec;
  char turn_message[640];
  double context_percent;
  int has_context;
  const char *model;
  const char *effort;
  int busy;
  int rc;
  if (!state->interactive) {
    return CAI_OK;
  }
  rc = cai_agent_runtime_state(state->runtime, &run_state, error);
  if (rc != CAI_OK) {
    return rc;
  }
  busy = run_state == CAI_AGENT_SAMPLING ||
         run_state == CAI_AGENT_DISPATCHING_TOOL;
  if (state->was_busy && !busy) {
    cai_cli_status_refresh_branch(&state->status, state->workspace);
    cli_refresh_quota(state, 0);
  }
  state->was_busy = busy;
  model = state->options.model;
  effort = state->options.reasoning_effort;
  memset(&settings, 0, sizeof(settings));
  cai_error_init(&optional_error);
  if (cai_agent_runtime_get_settings(state->runtime, &settings, NULL,
                                     &optional_error) == CAI_OK) {
    if (settings.model != NULL) {
      model = settings.model;
    }
    if (settings.reasoning_effort != NULL) {
      effort = settings.reasoning_effort;
    }
  }
  cai_error_cleanup(&optional_error);
  rc = cai_agent_runtime_context_percent(state->runtime, &context_percent,
                                         &has_context, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_agent_runtime_get_metrics(state->runtime, &metrics, error);
  if (rc != CAI_OK)
    return rc;
  if (state->timer_fd >= 0 && state->timer_armed != metrics.turn_active) {
    memset(&timer_spec, 0, sizeof(timer_spec));
    if (metrics.turn_active) {
      timer_spec.it_value.tv_sec = 1;
      timer_spec.it_interval.tv_sec = 1;
    }
    if (timerfd_settime(state->timer_fd, 0, &timer_spec, NULL) != 0)
      return CAI_ERR_TRANSPORT;
    state->timer_armed = metrics.turn_active;
  }
  if (cai_cli_turn_status_message(turn_message, sizeof(turn_message),
                                  state->reasoning_summary_raw, &metrics) != 0)
    return CAI_ERR_TRANSPORT;
  rc = cai_agent_runtime_get_goal(state->runtime, &goal, error);
  if (rc != CAI_OK) {
    return rc;
  }
  cai_cli_status_build(&state->status, model, effort, context_percent,
                       has_context,
                       state->quota_client != NULL ? &state->quota : NULL,
                       metrics.session_usage.estimated_spend_usd, &goal);
  if (sl_set_status_busy(state->sl, busy) != SL_OK ||
      sl_set_status_spinner(state->sl, busy) != SL_OK ||
      sl_set_status_message(state->sl, turn_message) != SL_OK ||
      cai_cli_status_apply(state->sl, &state->status) != 0) {
    return CAI_ERR_TRANSPORT;
  }
  return CAI_OK;
}

static int cli_timer_wakeup(sl_t *sl, const sl_watch_event_t *event,
                            void *context) {
  cli_state *state = (cli_state *)context;
  uint64_t expirations;
  cai_error error;
  ssize_t read_count;
  int rc;
  (void)sl;
  (void)event;
  read_count = read(state->timer_fd, &expirations, sizeof(expirations));
  if (read_count < 0 && (errno == EAGAIN || errno == EINTR))
    return SL_OK;
  if (read_count != (ssize_t)sizeof(expirations))
    return SL_ERROR_IO;
  cai_error_init(&error);
  rc = cli_sync_status(state, &error);
  if (rc != CAI_OK)
    cli_print_error("turn timer", &error);
  cai_error_cleanup(&error);
  return rc == CAI_OK ? SL_OK : SL_ERROR_IO;
}

static int cli_wakeup(sl_t *sl, const sl_watch_event_t *event, void *context) {
  cli_state *state;
  cai_error error;
  int rc;
  (void)sl;
  (void)event;
  state = (cli_state *)context;
  cai_error_init(&error);
  rc = cli_geometry(state) == 0
           ? cai_agent_runtime_pump(state->runtime, 0L, &error)
           : CAI_ERR_TRANSPORT;
  if (rc == CAI_OK)
    rc = cli_advance_automation(state, &error);
  if (rc == CAI_OK) {
    rc = cli_sync_status(state, &error);
  }
  if (rc != CAI_OK) {
    cli_print_error("runtime event", &error);
  }
  cai_error_cleanup(&error);
  return rc == CAI_OK ? SL_OK : SL_ERROR_IO;
}

static int cli_replay_event(void *context, const cai_agent_session_event *event,
                            cai_error *error) {
  cli_state *state;
  (void)error;
  state = (cli_state *)context;
  if ((strcmp(event->type, "turn_submitted") == 0 ||
       strcmp(event->type, "turn_queued") == 0 ||
       strcmp(event->type, "steering_queued") == 0) &&
      event->data != NULL) {
    return cli_prompt(state, event->data, 1) == 0 ? CAI_OK : CAI_ERR_TRANSPORT;
  }
  if (strcmp(event->type, "assistant_text_delta") == 0 && event->data != NULL) {
    return cli_text(state, event->data, strlen(event->data)) == 0
               ? CAI_OK
               : CAI_ERR_TRANSPORT;
  }
  if (strcmp(event->type, "assistant_text_end") == 0) {
    return cli_finish_response(state) == 0 ? CAI_OK : CAI_ERR_TRANSPORT;
  }
  return CAI_OK;
}

static int cli_replay(cli_state *state, cai_error *error) {
  int rc;
  rc = state->store.load_events_after(
      state->store.context, state->workspace,
      cai_agent_runtime_session_id(state->runtime), 0U, cli_replay_event, state,
      error);
  if (rc == CAI_OK && cli_finish_response(state) != 0) {
    rc = CAI_ERR_TRANSPORT;
  }
  return rc;
}

static int cli_open_runtime(cli_state *state, const char *resume_id,
                            int new_session, cai_error *error) {
  cai_agent_runtime_config config;
  cai_skill_config skills;
  int wakeup_fd;
  int rc;

  cai_agent_runtime_config_init(&config);
  cai_skill_config_init(&skills);
  if (state->options.review)
    config.preset = CAI_SMITH_REVIEW_PRESET;
  skills.skills_directory = state->options.skills_dir;
  config.workspace_directory = state->workspace;
  config.session_store = &state->store;
  config.resume_latest =
      !state->options.review && !new_session && resume_id == NULL;
  config.resume_session_id = state->options.review ? NULL : resume_id;
  config.record_transcript = 1;
  config.model = state->options.model;
  config.reasoning_effort = state->options.reasoning_effort;
  config.reasoning_summary = state->options.reasoning_summary;
  config.review_model = state->options.review_model;
  config.review_reasoning_effort = state->options.review_reasoning_effort;
  config.review_reasoning_summary = state->options.review_reasoning_summary;
  config.agent_config_directory = state->options.config_dir;
  config.global_agents_md_path = state->options.agents_md;
  config.skills = state->options.skills_dir != NULL ? &skills : NULL;
  config.agent_identity = state->options.identity;
  config.developer_instructions_extension =
      state->options.developer_instructions;
  config.codex_compat_agents_md = state->options.codex_agents_md;
  config.enable_image_generation = state->options.image_generation;
  config.disable_terminal = !state->options.terminal;
  config.disable_review_subagent = !state->options.review_subagent;
  config.event_callback = cli_event;
  config.event_context = state;
  rc = cai_agent_runtime_open(state->client, &config, &state->runtime, error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (state->interactive) {
    rc = cai_agent_runtime_wakeup_fd(state->runtime, &wakeup_fd, error);
    if (rc == CAI_OK &&
        sl_watch_add(state->sl, wakeup_fd,
                     SL_WATCH_READ | SL_WATCH_HANGUP | SL_WATCH_ERROR,
                     cli_wakeup, state, &state->watch_id) != SL_OK) {
      rc = CAI_ERR_TRANSPORT;
    }
    if (rc != CAI_OK) {
      cai_agent_runtime_close(state->runtime);
      state->runtime = NULL;
      return rc;
    }
    state->has_watch = 1;
  }
  rc = state->options.review ? CAI_OK : cli_replay(state, error);
  if (rc == CAI_OK) {
    rc = cli_sync_status(state, error);
  }
  return rc;
}

static void cli_close_runtime(cli_state *state) {
  if (state->timer_fd >= 0 && state->timer_armed) {
    struct itimerspec stopped;
    memset(&stopped, 0, sizeof(stopped));
    (void)timerfd_settime(state->timer_fd, 0, &stopped, NULL);
    state->timer_armed = 0;
  }
  if (state->has_watch) {
    (void)sl_watch_remove(state->sl, state->watch_id);
    state->has_watch = 0;
  }
  cai_agent_runtime_close(state->runtime);
  state->runtime = NULL;
  state->reasoning_summary_length = 0U;
  state->reasoning_summary_raw[0] = '\0';
}

static int cli_visit_session(void *context, const char *session_id,
                             unsigned long long timestamp, cai_error *error) {
  cli_state *state;
  cli_session *entries;
  size_t capacity;
  (void)error;
  state = (cli_state *)context;
  if (state->session_count == state->session_capacity) {
    capacity =
        state->session_capacity == 0U ? 8U : state->session_capacity * 2U;
    entries =
        (cli_session *)realloc(state->sessions, capacity * sizeof(*entries));
    if (entries == NULL) {
      return CAI_ERR_NOMEM;
    }
    state->sessions = entries;
    state->session_capacity = capacity;
  }
  memcpy(state->sessions[state->session_count].id, session_id,
         strlen(session_id) + 1U);
  state->sessions[state->session_count].checkpoint_ns = timestamp;
  state->session_count++;
  return CAI_OK;
}

static int cli_session_compare(const void *left, const void *right) {
  const cli_session *a;
  const cli_session *b;
  a = (const cli_session *)left;
  b = (const cli_session *)right;
  if (a->checkpoint_ns > b->checkpoint_ns)
    return -1;
  if (a->checkpoint_ns < b->checkpoint_ns)
    return 1;
  return strcmp(b->id, a->id);
}

static int cli_list_sessions(cli_state *state, cai_error *error) {
  size_t i;
  char line[230];
  int n;
  int rc;
  state->session_count = 0U;
  rc = cai_agent_local_session_store_list(&state->store, state->workspace,
                                          cli_visit_session, state, error);
  if (rc != CAI_OK) {
    return rc;
  }
  qsort(state->sessions, state->session_count, sizeof(*state->sessions),
        cli_session_compare);
  if (state->session_count == 0U) {
    return cli_write(state, "\nNo sessions for this directory.\n") == 0
               ? CAI_OK
               : CAI_ERR_TRANSPORT;
  }
  if (cli_write(state, "\nSessions for this directory:\n") != 0) {
    return CAI_ERR_TRANSPORT;
  }
  for (i = 0U; i < state->session_count; i++) {
    n = snprintf(line, sizeof(line), "  %lu  %s%s\n", (unsigned long)(i + 1U),
                 state->sessions[i].id,
                 state->runtime != NULL && strcmp(state->sessions[i].id,
                                                  cai_agent_runtime_session_id(
                                                      state->runtime)) == 0
                     ? "  (current)"
                     : "");
    if (n < 0 || (size_t)n >= sizeof(line) || cli_write(state, line) != 0) {
      return CAI_ERR_TRANSPORT;
    }
  }
  return CAI_OK;
}

static int cli_switch_session(cli_state *state, const char *id, int new_session,
                              cai_error *error) {
  cai_agent_run_state run_state;
  int rc;
  rc = cai_agent_runtime_state(state->runtime, &run_state, error);
  if (rc != CAI_OK)
    return rc;
  if (run_state == CAI_AGENT_SAMPLING ||
      run_state == CAI_AGENT_DISPATCHING_TOOL) {
    return cli_write(state, "\nWait for the current turn to finish.\n") == 0
               ? CAI_OK
               : CAI_ERR_TRANSPORT;
  }
  if (cli_finish_response(state) != 0)
    return CAI_ERR_TRANSPORT;
  cli_close_runtime(state);
  if (sl_history_set_max_len(state->sl, 0) != SL_OK ||
      sl_history_set_max_len(state->sl, 1000) != SL_OK) {
    return CAI_ERR_TRANSPORT;
  }
  if (cli_write(state, "\n--- session ---\n") != 0) {
    return CAI_ERR_TRANSPORT;
  }
  return cli_open_runtime(state, id, new_session, error);
}

static int cli_handle_command(cli_state *state, const char *line,
                              cai_error *error, int *handled) {
  const char *arg;
  char *end;
  unsigned long number;
  int rc;
  *handled = 1;
  if (strcmp(line, "/quit") == 0 || strcmp(line, "/exit") == 0) {
    state->exit_requested = 1;
    return CAI_OK;
  }
  if (strcmp(line, "/new") == 0) {
    return cli_switch_session(state, NULL, 1, error);
  }
  if (strcmp(line, "/status") == 0) {
    cai_agent_runtime_settings settings;
    cai_agent_runtime_metrics metrics;
    cai_error optional_error;
    char markdown[2048];
    const char *model = state->options.model;
    const char *effort = state->options.reasoning_effort;
    memset(&settings, 0, sizeof(settings));
    cai_error_init(&optional_error);
    if (cai_agent_runtime_get_settings(state->runtime, &settings, NULL,
                                       &optional_error) == CAI_OK) {
      if (settings.model != NULL)
        model = settings.model;
      if (settings.reasoning_effort != NULL)
        effort = settings.reasoning_effort;
    }
    cai_error_cleanup(&optional_error);
    rc = cai_agent_runtime_get_metrics(state->runtime, &metrics, error);
    if (rc != CAI_OK)
      return rc;
    cli_refresh_quota(state, 1);
    if (cai_cli_status_markdown(markdown, sizeof(markdown), model, effort,
                                state->options.provider, &metrics,
                                state->quota_client != NULL ? &state->quota
                                                            : NULL) != 0 ||
        cli_text(state, markdown, strlen(markdown)) != 0 ||
        cli_finish_response(state) != 0) {
      return CAI_ERR_TRANSPORT;
    }
    return CAI_OK;
  }
  if (strcmp(line, "/resume") == 0) {
    return cli_list_sessions(state, error);
  }
  if (strncmp(line, "/resume ", 8U) == 0) {
    arg = line + 8U;
    errno = 0;
    number = strtoul(arg, &end, 10);
    if (errno != 0 || arg == end || *end != '\0' || number == 0U) {
      return cli_write(state, "\nUsage: /resume <number>\n") == 0
                 ? CAI_OK
                 : CAI_ERR_TRANSPORT;
    }
    rc = cli_list_sessions(state, error);
    if (rc != CAI_OK)
      return rc;
    if (number > state->session_count) {
      return cli_write(state, "\nNo session with that number.\n") == 0
                 ? CAI_OK
                 : CAI_ERR_TRANSPORT;
    }
    return cli_switch_session(state, state->sessions[number - 1U].id, 0, error);
  }
  *handled = 0;
  return CAI_OK;
}

static int cli_submit_automation(cli_state *state, const char *instruction,
                                 cai_error *error) {
  int rc;
  if (instruction == NULL || cli_prompt(state, instruction, 1) != 0)
    return CAI_ERR_TRANSPORT;
  state->awaiting_turn = 1;
  rc = cai_agent_runtime_submit_interactive(state->runtime, instruction, error);
  if (rc != CAI_OK)
    state->awaiting_turn = 0;
  return rc;
}

static int cli_start_goal(cli_state *state, const char *objective,
                          cai_error *error) {
  cai_agent_goal_snapshot goal;
  cai_agent_goal_request request;
  int rc;

  rc = cai_agent_runtime_get_goal(state->runtime, &goal, error);
  if (rc != CAI_OK)
    return rc;
  if (goal.has_goal && strcmp(goal.status, "complete") != 0) {
    rc = cai_agent_runtime_clear_goal(state->runtime, error);
    if (rc != CAI_OK)
      return rc;
    do {
      rc = cai_agent_runtime_pump(state->runtime, 100L, error);
      if (rc != CAI_OK)
        return rc;
      rc = cai_agent_runtime_get_goal(state->runtime, &goal, error);
    } while (rc == CAI_OK && goal.has_goal);
    if (rc != CAI_OK)
      return rc;
  }
  cai_agent_goal_request_init(&request);
  request.objective = objective;
  rc = cai_agent_runtime_create_goal(state->runtime, &request, error);
  if (rc != CAI_OK)
    return rc;
  do {
    rc = cai_agent_runtime_pump(state->runtime, 100L, error);
    if (rc != CAI_OK)
      return rc;
    rc = cai_agent_runtime_get_goal(state->runtime, &goal, error);
  } while (rc == CAI_OK &&
           (!goal.has_goal || strcmp(goal.objective, objective) != 0));
  return rc;
}

static int cli_start_automation(cli_state *state, cai_error *error) {
  const char *objective;
  const char *instruction;
  int length;
  int rc;

  objective = state->options.goal;
  if (state->options.review_and_fix) {
    const char *format =
        "Review the current changes with the built-in run_review subagent. "
        "Fix every actionable finding while preserving project invariants. "
        "After each fix, run relevant verification, then repeat the review "
        "until it reports no actionable findings. Explain why any reported "
        "finding is irrelevant. Mark this goal complete only after a clean "
        "review and passing verification.";
    if (state->options.base != NULL) {
      length = snprintf(state->generated_goal, sizeof(state->generated_goal),
                        "Review changes against base %s with the built-in "
                        "run_review subagent. Fix every actionable finding "
                        "while preserving project invariants. After each "
                        "fix, run relevant verification, then repeat review "
                        "against the same base until no actionable findings "
                        "remain. Explain why any finding is irrelevant. "
                        "Mark this goal complete only after a clean review "
                        "and passing verification.",
                        state->options.base);
    } else {
      length = snprintf(state->generated_goal, sizeof(state->generated_goal),
                        "%s", format);
    }
    if (length < 0 || (size_t)length >= sizeof(state->generated_goal))
      return CAI_ERR_INVALID;
    objective = state->generated_goal;
  }
  state->auto_goal = objective != NULL;
  if (objective != NULL) {
    rc = cli_start_goal(state, objective, error);
    if (rc != CAI_OK)
      return rc;
  }
  if (state->options.instruction_count > 0U) {
    instruction = cai_cli_instruction_at(&state->options, 0U);
    state->next_instruction = 1U;
  } else {
    instruction = objective;
  }
  if (instruction == NULL) {
    state->automation_done = 1;
    return CAI_OK;
  }
  return cli_submit_automation(state, instruction, error);
}

static int cli_advance_automation(cli_state *state, cai_error *error) {
  cai_agent_goal_snapshot goal;
  const char *next;
  int rc;

  if (state->automation_done || state->awaiting_turn)
    return CAI_OK;
  if (state->automation_failed) {
    state->automation_done = 1;
    return CAI_OK;
  }
  if (state->auto_goal) {
    rc = cai_agent_runtime_get_goal(state->runtime, &goal, error);
    if (rc != CAI_OK)
      return rc;
    if (!goal.has_goal || goal.status == NULL) {
      state->automation_failed = 1;
      state->automation_done = 1;
      return CAI_OK;
    }
    if (strcmp(goal.status, "active") == 0)
      return cli_submit_automation(
          state,
          "Continue pursuing the active goal. Finish the remaining "
          "work, verify the result, and update the goal status when "
          "appropriate.",
          error);
    if (strcmp(goal.status, "complete") != 0) {
      state->automation_failed = 1;
      state->automation_done = 1;
      fprintf(stderr, "cai: goal stopped with status %s\n", goal.status);
      return CAI_OK;
    }
    state->auto_goal = 0;
  }
  next = cai_cli_instruction_at(&state->options, state->next_instruction);
  if (next != NULL) {
    state->next_instruction++;
    return cli_submit_automation(state, next, error);
  }
  state->automation_done = 1;
  return CAI_OK;
}

static int cli_run_noninteractive(cli_state *state, cai_error *error) {
  int rc;
  while (!state->automation_done) {
    rc = cai_agent_runtime_pump(state->runtime, 100L, error);
    if (rc != CAI_OK)
      return rc;
    rc = cli_advance_automation(state, error);
    if (rc != CAI_OK)
      return rc;
  }
  return state->automation_failed ? CAI_ERR_TRANSPORT : CAI_OK;
}

static int cli_drain(cli_state *state, cai_error *error) {
  cai_agent_run_state run_state;
  int rc;
  for (;;) {
    rc = cai_agent_runtime_pump(state->runtime, 100L, error);
    if (rc != CAI_OK)
      return rc;
    rc = cai_agent_runtime_state(state->runtime, &run_state, error);
    if (rc != CAI_OK)
      return rc;
    if (run_state != CAI_AGENT_SAMPLING &&
        run_state != CAI_AGENT_DISPATCHING_TOOL) {
      return cai_agent_runtime_pump(state->runtime, 0L, error);
    }
  }
}

static int cli_run_review(cli_state *state, cai_error *error) {
  cai_agent_review_request request;
  cai_agent_run_state run_state;
  const char *instruction;
  FILE *destination;
  char *formatted;
  int rc;

  cai_agent_review_request_init(&request);
  instruction = cai_cli_instruction_at(&state->options, 0U);
  if (instruction != NULL) {
    request.target = CAI_AGENT_REVIEW_CUSTOM;
    request.instructions = instruction;
  } else if (state->options.base != NULL) {
    request.target = CAI_AGENT_REVIEW_BASE_BRANCH;
    request.base_branch = state->options.base;
  } else {
    request.target = CAI_AGENT_REVIEW_UNCOMMITTED;
  }
  rc = cai_agent_runtime_submit_review(state->runtime, &request, error);
  if (rc != CAI_OK)
    return rc;
  rc = cli_drain(state, error);
  if (rc != CAI_OK)
    return rc;
  rc = cai_agent_runtime_state(state->runtime, &run_state, error);
  if (rc != CAI_OK)
    return rc;
  if (run_state != CAI_AGENT_COMPLETED || state->review_report == NULL) {
    fputs("cai: review ended without a valid findings report\n", stderr);
    return CAI_ERR_PROTOCOL;
  }
  if (cli_finish_response(state) != 0 || cli_finish_reasoning(state) != 0)
    return CAI_ERR_TRANSPORT;
  formatted = NULL;
  if (cai_cli_review_format(state->review_report, state->review_report_length,
                            state->options.output_type, &formatted) != 0) {
    fputs("cai: failed to format review findings\n", stderr);
    return CAI_ERR_PROTOCOL;
  }
  fflush(stderr);
  destination =
      state->options.out != NULL ? fopen(state->options.out, "w") : stdout;
  if (destination == NULL) {
    fprintf(stderr, "cai: cannot open review output: %s\n", strerror(errno));
    free(formatted);
    return CAI_ERR_TRANSPORT;
  }
  rc = fputs(formatted, destination) < 0 || fflush(destination) != 0
           ? CAI_ERR_TRANSPORT
           : CAI_OK;
  if (state->options.out != NULL && fclose(destination) != 0)
    rc = CAI_ERR_TRANSPORT;
  free(formatted);
  if (rc != CAI_OK)
    fputs("cai: failed to write review findings\n", stderr);
  return rc;
}

int main(int argc, char **argv) {
  cli_state state;
  cai_client_config client_config;
  cai_chatgpt_auth_config auth_config;
  mdf_options mdf_config;
  mdf_sink sink;
  cai_error error;
  sl_readline_status_t prompt_status;
  char auth_path[PATH_MAX];
  char *state_auth_path;
  const char *auth_file;
  const char *home_directory;
  char *line;
  int parsed;
  int handled;
  int rc;
  int result;

  memset(&state, 0, sizeof(state));
  state.timer_fd = -1;
  cai_error_init(&error);
  parsed = cai_cli_parse_options(argc, argv, &state.options);
  if (parsed <= 0)
    return parsed == 0 ? 0 : 2;
  if (state.options.directory != NULL && chdir(state.options.directory) != 0) {
    fprintf(stderr, "cai: cannot change directory to %s: %s\n",
            state.options.directory, strerror(errno));
    cai_error_cleanup(&error);
    return 2;
  }
  if (state.options.login)
    return cai_cli_login(state.options.auth_json);
  if (realpath(".", state.workspace) == NULL) {
    fprintf(stderr, "cai: cannot resolve workspace: %s\n", strerror(errno));
    return 2;
  }
  auth_file = state.options.auth_json;
  state_auth_path = NULL;
  if (strcmp(state.options.provider, "chatgpt") == 0 && auth_file == NULL) {
    home_directory = getenv("HOME");
    if (home_directory != NULL &&
        snprintf(auth_path, sizeof(auth_path), "%s/.codex/auth.json",
                 home_directory) < (int)sizeof(auth_path) &&
        access(auth_path, F_OK) == 0) {
      auth_file = auth_path;
    } else {
      rc = cai_chatgpt_auth_default_path(&state_auth_path, &error);
      if (rc != CAI_OK) {
        cli_print_error("resolve cai auth path", &error);
        cai_error_cleanup(&error);
        return 2;
      }
      if (access(state_auth_path, F_OK) != 0) {
        fputs("cai: ChatGPT authentication not found; run cai --login (-l) "
              "to authenticate\n",
              stderr);
        cai_string_destroy(state_auth_path);
        cai_error_cleanup(&error);
        return 2;
      }
      auth_file = state_auth_path;
    }
  }
  state.interactive = !state.options.review && !state.options.non_interactive &&
                      isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
  if (state.options.review)
    state.activity = stderr;
  cai_cli_status_init(&state.status, state.workspace, getenv("HOME"));
  result = 1;
  if (!state.options.review) {
    state.sl = sl_create();
    if (state.sl == NULL) {
      fputs("cai: failed to create softline prompt\n", stderr);
      goto cleanup;
    }
  }
  state.width = mdf_terminal_width(
      state.options.review ? STDERR_FILENO : STDOUT_FILENO, 80);
  mdf_options_init(&mdf_config);
  mdf_config.width = state.width;
  mdf_config.margin_left = state.width >= 5 ? 2 : 0;
  mdf_config.boring =
      state.options.review ? !isatty(STDERR_FILENO) : !state.interactive;
  if ((state.interactive &&
       (sl_set_bounds(state.sl, 0, 0, 0, 0) != SL_OK ||
        sl_set_statusline(state.sl, 1, 0) != SL_OK ||
        sl_set_status_message_prefix(state.sl, "") != SL_OK)) ||
      (!state.options.review && sl_output_stream_begin(state.sl) != SL_OK) ||
      mdf_create(MDF_FORMAT_ANSI, &mdf_config, &state.renderer) != MDF_OK) {
    fputs("cai: failed to initialize terminal renderers\n", stderr);
    goto cleanup;
  }
  sink.userdata = &state;
  sink.write = cli_sink;
  if (state.renderer->set_sink(state.renderer, &sink) != MDF_OK) {
    fputs("cai: failed to connect Markdown renderer\n", stderr);
    goto cleanup;
  }
  if (state.interactive) {
    state.timer_fd =
        timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (state.timer_fd < 0 ||
        sl_watch_add(state.sl, state.timer_fd,
                     SL_WATCH_READ | SL_WATCH_ERROR | SL_WATCH_HANGUP,
                     cli_timer_wakeup, &state,
                     &state.timer_watch_id) != SL_OK) {
      fputs("cai: failed to start turn status timer\n", stderr);
      goto cleanup;
    }
    state.has_timer_watch = 1;
  }
  rc = cai_agent_local_session_store_open(NULL, &state.store, &error);
  if (rc != CAI_OK) {
    cli_print_error("open session store", &error);
    goto cleanup;
  }
  cai_client_config_init(&client_config);
  if (strcmp(state.options.provider, "chatgpt") == 0) {
    cai_chatgpt_auth_config_init(&auth_config);
    auth_config.auth_json_path = auth_file;
    rc = cai_chatgpt_auth_open(&auth_config, &state.auth, &error);
    if (rc != CAI_OK) {
      cli_print_error("open ChatGPT auth", &error);
      fputs("cai: run cai --login (-l) to authenticate\n", stderr);
      goto cleanup;
    }
    client_config.chatgpt_auth = state.auth;
  } else if (strcmp(state.options.provider, "openrouter") == 0) {
    cai_client_config_use_openrouter(&client_config);
  } else if (strcmp(state.options.provider, "custom") == 0) {
    client_config.base_url = state.options.endpoint;
    client_config.api_key_env = state.options.api_key_env;
  }
  rc = cai_client_open(&client_config, &state.client, &error);
  if (rc != CAI_OK) {
    cli_print_error("open client", &error);
    goto cleanup;
  }
  if (state.auth != NULL) {
    cai_error quota_error;
    cai_client_config quota_config;
    cai_error_init(&quota_error);
    cai_client_config_init(&quota_config);
    quota_config.chatgpt_auth = state.auth;
    quota_config.timeout_ms = 1500L;
    if (cai_client_open(&quota_config, &state.quota_client, &quota_error) ==
        CAI_OK) {
      if (state.interactive)
        cli_refresh_quota(&state, 0);
    }
    cai_error_cleanup(&quota_error);
  }
  rc = cli_open_runtime(&state, state.options.resume_id,
                        state.options.new_session, &error);
  if (rc != CAI_OK) {
    cli_print_error("open agent session", &error);
    goto cleanup;
  }
  if (state.interactive &&
      cli_write(&state,
                "\nCai Smith. Enter sends; /resume lists sessions; "
                "/new starts fresh; /status shows usage; /quit exits.\n") !=
          0) {
    goto cleanup;
  }
  if (state.options.review) {
    rc = cli_run_review(&state, &error);
    if (rc != CAI_OK)
      cli_print_error("review", &error);
    else
      result = 0;
    goto cleanup;
  }
  rc = cli_start_automation(&state, &error);
  if (rc != CAI_OK) {
    cli_print_error("start work", &error);
    goto cleanup;
  }
  if (state.options.non_interactive) {
    rc = cli_run_noninteractive(&state, &error);
    if (rc != CAI_OK)
      cli_print_error("non-interactive run", &error);
    else
      result = 0;
    goto cleanup;
  }
  while (!state.exit_requested) {
    line = sl_next_prompt(state.sl, "> ", NULL);
    if (line == NULL) {
      prompt_status = sl_last_readline_status(state.sl);
      if (prompt_status == SL_READLINE_CANCELLED ||
          prompt_status == SL_READLINE_INTERRUPTED) {
        rc = cai_agent_runtime_cancel_turn(state.runtime, &error);
        if (rc != CAI_OK) {
          cai_error_cleanup(&error);
          cai_error_init(&error);
        }
        continue;
      }
      if (prompt_status == SL_READLINE_ERROR) {
        fputs("cai: prompt input failed\n", stderr);
        goto cleanup;
      }
      break;
    }
    if (line[0] != '\0') {
      rc = cli_handle_command(&state, line, &error, &handled);
      if (rc == CAI_OK && !handled) {
        if (cli_prompt(&state, line, 1) != 0) {
          rc = CAI_ERR_TRANSPORT;
        } else {
          rc =
              cai_agent_runtime_submit_interactive(state.runtime, line, &error);
        }
      }
      if (rc != CAI_OK) {
        cli_print_error("input", &error);
        if (state.runtime == NULL) {
          sl_free_string(state.sl, line);
          goto cleanup;
        }
        cai_error_cleanup(&error);
        cai_error_init(&error);
      }
    }
    sl_free_string(state.sl, line);
    if (state.runtime != NULL && cli_sync_status(&state, &error) != CAI_OK) {
      cli_print_error("status", &error);
      goto cleanup;
    }
  }
  if (state.runtime != NULL && !state.exit_requested) {
    rc = cli_drain(&state, &error);
    if (rc != CAI_OK) {
      cli_print_error("finish agent turn", &error);
      goto cleanup;
    }
  }
  result = 0;
cleanup:
  if (state.runtime != NULL)
    cli_close_runtime(&state);
  if (state.has_timer_watch)
    (void)sl_watch_remove(state.sl, state.timer_watch_id);
  if (state.timer_fd >= 0)
    close(state.timer_fd);
  if (state.client != NULL)
    state.client->close(state.client);
  if (state.quota_client != NULL)
    state.quota_client->close(state.quota_client);
  if (state.auth != NULL)
    state.auth->close(state.auth);
  cai_string_destroy(state_auth_path);
  if (state.store.context != NULL)
    cai_agent_local_session_store_close(&state.store);
  if (state.renderer != NULL) {
    (void)cli_finish_response(&state);
    state.renderer->destroy(state.renderer);
  }
  if (state.sl != NULL) {
    (void)sl_output_stream_end(state.sl);
    sl_destroy(state.sl);
  }
  free(state.sessions);
  free(state.review_report);
  cai_error_cleanup(&error);
  return result;
}
