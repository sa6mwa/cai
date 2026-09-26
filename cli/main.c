#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "log.h"
#include "login.h"
#include "options.h"
#include "pouch.h"
#include "response.h"
#include "review_report.h"
#include "session_commands.h"
#include "status.h"
#include <locale.h>

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

typedef cai_cli_session cli_session;

typedef struct cli_state {
  cai_cli_options options;
  cai_agent_session_store store;
  cai_cli_pouch pouch;
  cai_chatgpt_auth *auth;
  cai_client *client;
  cai_client *quota_client;
  cai_agent_runtime *runtime;
  sl_t *sl;
  mdf *renderer;
  cai_cli_log log;
  cai_cli_response final_response;
  int batch;
  sl_watch_id_t log_watch_id;
  int has_log_watch;
  char *review_report;
  size_t review_report_length;
  char generated_goal[4096];
  cli_session *sessions;
  size_t session_count;
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

static void cli_print_error(cli_state *state, const char *operation,
                            const cai_error *error) {
  cai_cli_log_error(state->log.logger, operation, error);
}

static void cli_diagnostic(cli_state *state, pslog_level level,
                           const char *text) {
  cai_cli_log_message(state->log.logger, level, "diagnostic", "diagnostic",
                      text, strlen(text), NULL, 0U);
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
  if (state->batch)
    return fwrite(bytes, 1U, count, stdout) == count ? 0 : -1;
  return sl_output_stream_write(state->sl, bytes, count) == SL_OK ? 0 : -1;
}

static int cli_render_width(void) {
  return isatty(STDOUT_FILENO) ? mdf_terminal_width(STDOUT_FILENO, 80) : 80;
}

static int cli_geometry(cli_state *state) {
  int width;
  width = cli_render_width();
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
  if (state->batch) {
    cli_diagnostic(state, PSLOG_LEVEL_INFO, text);
    return 0;
  }
  return sl_output_stream_write(state->sl, text, strlen(text)) == SL_OK ? 0
                                                                        : -1;
}

static int cli_prompt(cli_state *state, const char *text, int history) {
  if (state->batch)
    return 0;
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

static void cli_log_event(cli_state *state,
                          const cai_agent_runtime_event *event) {
  static const char *const names[] = {"unknown",
                                      "run_started",
                                      "run_state_changed",
                                      "assistant_text_delta",
                                      "tool_call_started",
                                      "tool_call_completed",
                                      "tool_call_failed",
                                      "steering_queued",
                                      "steering_delivered",
                                      "run_completed",
                                      "run_failed",
                                      "session_checkpointed",
                                      "terminal_command_started",
                                      "terminal_output",
                                      "terminal_waiting",
                                      "terminal_command_completed",
                                      "terminal_command_cancelled",
                                      "turn_queued",
                                      "review_report",
                                      "review_started",
                                      "review_handed_off",
                                      "reasoning_summary",
                                      "response_completed",
                                      "goal_changed",
                                      "subagent_started",
                                      "subagent_handed_off",
                                      "compaction_started",
                                      "compaction_progress",
                                      "compaction_completed",
                                      "run_cancelled"};
  pslog_field fields[16];
  size_t count;
  const char *name;
  const char *role;
  const char *kind;
  const char *text;
  size_t length;
  pslog_level level;
  name =
      event->type > 0 && (size_t)event->type < sizeof(names) / sizeof(names[0])
          ? names[event->type]
          : "unknown";
  role = "runtime";
  kind = NULL;
  level = PSLOG_LEVEL_INFO;
  if (event->type == CAI_AGENT_EVENT_RUN_STARTED ||
      event->type == CAI_AGENT_EVENT_TURN_QUEUED ||
      event->type == CAI_AGENT_EVENT_STEERING_QUEUED ||
      event->type == CAI_AGENT_EVENT_STEERING_DELIVERED) {
    role = "user";
    kind = event->type == CAI_AGENT_EVENT_RUN_STARTED   ? "normal"
           : event->type == CAI_AGENT_EVENT_TURN_QUEUED ? "queued"
                                                        : "steering";
  } else if (event->type == CAI_AGENT_EVENT_TEXT_DELTA ||
             event->type == CAI_AGENT_EVENT_REASONING_SUMMARY ||
             event->type == CAI_AGENT_EVENT_REVIEW_REPORT ||
             event->type == CAI_AGENT_EVENT_SUBAGENT_STARTED) {
    role = "assistant";
    kind = event->type == CAI_AGENT_EVENT_REASONING_SUMMARY
               ? "reasoning_summary"
           : event->type == CAI_AGENT_EVENT_REVIEW_REPORT    ? "review_findings"
           : event->type == CAI_AGENT_EVENT_SUBAGENT_STARTED ? "delegation"
                                                             : "normal";
  } else if (event->tool_name != NULL || event->terminal_id != NULL) {
    role = "tool";
  }
  if (event->type == CAI_AGENT_EVENT_RUN_FAILED)
    level = PSLOG_LEVEL_ERROR;
  else if (event->type == CAI_AGENT_EVENT_TOOL_CALL_FAILED)
    level = PSLOG_LEVEL_WARN;
  count = 0U;
  fields[count++] = pslog_i64("state", event->state);
  fields[count++] = pslog_u64("sequence", (pslog_uint64)event->sequence);
  if (kind != NULL)
    fields[count++] = pslog_str("prompt_kind", kind);
  if (event->runtime_session_id != NULL)
    fields[count++] = pslog_str("session_id", event->runtime_session_id);
  if (event->parent_tool_call_id != NULL)
    fields[count++] =
        pslog_str("parent_tool_call_id", event->parent_tool_call_id);
  if (event->subagent_name != NULL)
    fields[count++] = pslog_str("subagent", event->subagent_name);
  if (event->tool_name != NULL)
    fields[count++] = pslog_str("tool", event->tool_name);
  if (event->tool_call_id != NULL)
    fields[count++] = pslog_str("tool_call_id", event->tool_call_id);
  if (event->tool_path != NULL)
    fields[count++] = pslog_str("path", event->tool_path);
  if (event->terminal_id != NULL) {
    fields[count++] = pslog_str("terminal_id", event->terminal_id);
    fields[count++] =
        pslog_u64("command_id", (pslog_uint64)event->terminal_command_id);
    if (event->terminal_has_exit_code)
      fields[count++] =
          pslog_i64("exit_code", (pslog_int64)event->terminal_exit_code);
  }
  text = event->data != NULL ? event->data : name;
  length = event->data != NULL ? event->data_length : strlen(name);
  cai_cli_log_message(state->log.logger, level, name, role, text, length,
                      fields, count);
  if (event->subagent_instruction != NULL) {
    if (kind != NULL)
      fields[2] = pslog_str("prompt_kind", "delegated");
    else
      fields[count++] = pslog_str("prompt_kind", "delegated");
    cai_cli_log_message(state->log.logger, PSLOG_LEVEL_INFO,
                        "subagent_instruction", "user",
                        event->subagent_instruction,
                        strlen(event->subagent_instruction), fields, count);
  }
}

static int cli_event(void *context, const cai_agent_runtime_event *event,
                     cai_error *error) {
  cli_state *state;
  char message[320];
  int n;
  state = (cli_state *)context;
  cli_log_event(state, event);
  if (event->type == CAI_AGENT_EVENT_REVIEW_REPORT &&
      event->parent_tool_call_id == NULL && state->options.review) {
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
  if (event->parent_tool_call_id == NULL) {
    if (event->type == CAI_AGENT_EVENT_RUN_COMPLETED)
      state->awaiting_turn = 0;
    if (event->type == CAI_AGENT_EVENT_RUN_FAILED ||
        event->type == CAI_AGENT_EVENT_RUN_CANCELLED) {
      state->awaiting_turn = 0;
      state->automation_failed = 1;
    }
    if (event->type == CAI_AGENT_EVENT_RUN_STARTED) {
      state->reasoning_summary_length = 0U;
      state->reasoning_summary_raw[0] = '\0';
      cai_cli_response_boundary(&state->final_response);
    }
  }
  if (state->batch) {
    if (event->parent_tool_call_id == NULL && !state->options.review) {
      if (event->type == CAI_AGENT_EVENT_TEXT_DELTA)
        return cai_cli_response_append(&state->final_response,
                                       state->pouch.state_directory,
                                       event->data, event->data_length, error);
      if (event->type == CAI_AGENT_EVENT_RESPONSE_COMPLETED)
        cai_cli_response_boundary(&state->final_response);
    }
    return CAI_OK;
  }
  if (event->type == CAI_AGENT_EVENT_TEXT_DELTA)
    return cli_text(state, event->data, event->data_length) == 0
               ? CAI_OK
               : CAI_ERR_TRANSPORT;
  if (event->type == CAI_AGENT_EVENT_RESPONSE_COMPLETED)
    return cli_finish_response(state) == 0 && cli_finish_reasoning(state) == 0
               ? CAI_OK
               : CAI_ERR_TRANSPORT;
  if (event->type == CAI_AGENT_EVENT_TOOL_CALL_STARTED &&
      event->tool_name != NULL) {
    n = snprintf(message, sizeof(message), "\n[tool] %s\n", event->tool_name);
    if (n < 0 || (size_t)n >= sizeof(message) || cli_write(state, message) != 0)
      return CAI_ERR_TRANSPORT;
  } else if (event->type == CAI_AGENT_EVENT_REASONING_SUMMARY &&
             event->data != NULL && event->data_length > 0U) {
    if (event->parent_tool_call_id == NULL)
      cli_reasoning_append(state, event->data, event->data_length);
    if (cli_geometry(state) != 0 || cli_finish_response(state) != 0)
      return CAI_ERR_TRANSPORT;
    if (!state->reasoning_open) {
      if (cli_sink(state, "\n[reasoning]\n", 13U) != 0 ||
          (state->documents_rendered > 0 &&
           state->renderer->begin_document(state->renderer) != MDF_OK))
        return CAI_ERR_TRANSPORT;
    }
    state->reasoning_open = 1;
    if (state->renderer->feed(state->renderer, event->data,
                              event->data_length) != MDF_OK)
      return CAI_ERR_TRANSPORT;
  }
  return CAI_OK;
}

static int cli_notice(void *context, const char *text) {
  cli_state *state = (cli_state *)context;
  return cli_write(state, "\n") == 0 && cli_write(state, text) == 0 &&
                 cli_write(state, "\n") == 0
             ? 0
             : -1;
}

static int cli_startup_notice(void *context, const char *text) {
  (void)context;
  return fprintf(stderr, "%s\n", text) >= 0 ? 0 : -1;
}

static int cli_log_wakeup(sl_t *sl, const sl_watch_event_t *event,
                          void *context) {
  cli_state *state = (cli_state *)context;
  (void)sl;
  (void)event;
  return cai_cli_log_notices(&state->log, cli_notice, state) == 0 ? SL_OK
                                                                  : SL_ERROR_IO;
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
                       has_context, state->auth != NULL ? &state->quota : NULL,
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
    cli_print_error(state, "turn timer", &error);
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
    cli_print_error(state, "runtime event", &error);
  }
  cai_error_cleanup(&error);
  return rc == CAI_OK ? SL_OK : SL_ERROR_IO;
}

static int cli_replay_event(void *context, const cai_agent_session_event *event,
                            cai_error *error) {
  cli_state *state;
  (void)error;
  state = (cli_state *)context;
  if (event->data != NULL &&
      (strcmp(event->type, "turn_submitted") == 0 ||
       strcmp(event->type, "turn_queued") == 0 ||
       strcmp(event->type, "steering_queued") == 0 ||
       strcmp(event->type, "assistant_text_delta") == 0)) {
    pslog_field fields[2];
    int assistant = strcmp(event->type, "assistant_text_delta") == 0;
    fields[0] = pslog_bool("replay", 1);
    fields[1] = pslog_str(
        "prompt_kind", strcmp(event->type, "steering_queued") == 0 ? "steering"
                       : strcmp(event->type, "turn_queued") == 0   ? "queued"
                                                                   : "normal");
    cai_cli_log_message(state->log.logger, PSLOG_LEVEL_INFO, event->type,
                        assistant ? "assistant" : "user", event->data,
                        strlen(event->data), fields, 2U);
  }
  if (state->batch)
    return CAI_OK;
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
  config.logger = state->log.logger;
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
  rc = cai_cli_log_session(&state->log,
                           cai_agent_runtime_session_id(state->runtime), error);
  if (rc != CAI_OK)
    return rc;
  if (state->options.developer_instructions != NULL) {
    pslog_field kind = pslog_str("prompt_kind", "developer_instructions");
    cai_cli_log_message(
        state->log.logger, PSLOG_LEVEL_INFO, "developer_instructions",
        "developer", state->options.developer_instructions,
        strlen(state->options.developer_instructions), &kind, 1U);
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

static int cli_list_write(void *context, const void *bytes, size_t count,
                          cai_error *error) {
  cli_state *state;
  state = (cli_state *)context;
  if (cli_sink(state, bytes, count) != 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "write session rows");
  return CAI_OK;
}

static int cli_list_sessions(cli_state *state, cai_error *error) {
  cai_sink_callbacks callbacks;
  cai_sink *sink;
  int rc;
  free(state->sessions);
  state->sessions = NULL;
  state->session_count = 0U;
  rc = cai_cli_pouch_list(&state->pouch, state->workspace, &state->sessions,
                          &state->session_count, error);
  if (rc != CAI_OK)
    return rc;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.context = state;
  callbacks.write = cli_list_write;
  sink = NULL;
  rc = cai_sink_from_callbacks(&callbacks, &sink, error);
  if (rc == CAI_OK)
    rc = cai_cli_sessions_print(sink, state->sessions, state->session_count, 1,
                                state->width, error);
  cai_sink_close(sink);
  return rc;
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
    if (cai_cli_status_markdown(
            markdown, sizeof(markdown), model, effort, state->options.provider,
            &metrics, state->auth != NULL ? &state->quota : NULL) != 0 ||
        cli_text(state, markdown, strlen(markdown)) != 0 ||
        cli_finish_response(state) != 0) {
      return CAI_ERR_TRANSPORT;
    }
    return CAI_OK;
  }
  if (strcmp(line, "/export") == 0 || strncmp(line, "/export ", 8U) == 0) {
    cai_sink_callbacks callbacks;
    cai_sink *destination;
    const char *id;
    id = line[7] == '\0' ? cai_agent_runtime_session_id(state->runtime)
                         : line + 8U;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.context = state;
    callbacks.write = cli_list_write;
    destination = NULL;
    rc = cai_sink_from_callbacks(&callbacks, &destination, error);
    if (rc == CAI_OK)
      rc = cai_cli_session_export(
          &state->pouch, id, state->options.export_dir,
          strcmp(id, cai_agent_runtime_session_id(state->runtime)) == 0
              ? state->runtime
              : NULL,
          destination, error);
    cai_sink_close(destination);
    return rc;
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
      cli_diagnostic(state, PSLOG_LEVEL_WARN, goal.status);
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
    return cai_cli_error(error, CAI_ERR_PROTOCOL,
                         "review ended without a valid findings report");
  }
  if (cli_finish_response(state) != 0 || cli_finish_reasoning(state) != 0)
    return CAI_ERR_TRANSPORT;
  formatted = NULL;
  if (cai_cli_review_format(state->review_report, state->review_report_length,
                            state->options.output_type, &formatted) != 0) {
    return cai_cli_error(error, CAI_ERR_PROTOCOL,
                         "failed to format review findings");
  }
  fflush(stderr);
  destination =
      state->options.out != NULL ? fopen(state->options.out, "w") : stdout;
  if (destination == NULL) {
    cli_diagnostic(state, PSLOG_LEVEL_ERROR, "cannot open review output");
    free(formatted);
    return CAI_ERR_TRANSPORT;
  }
  rc = fputs(formatted, destination) < 0 || fflush(destination) != 0
           ? CAI_ERR_TRANSPORT
           : CAI_OK;
  if (state->options.out != NULL && fclose(destination) != 0)
    rc = CAI_ERR_TRANSPORT;
  if (rc == CAI_OK) {
    pslog_field kind = pslog_str("prompt_kind", "review_findings");
    cai_cli_log_message(state->log.logger, PSLOG_LEVEL_INFO, "final_response",
                        "assistant", formatted, strlen(formatted), &kind, 1U);
  }
  free(formatted);
  if (rc != CAI_OK)
    cai_cli_error(error, rc, "failed to write review findings");
  return rc;
}

static int cli_output_final(cli_state *state, cai_error *error) {
  cai_source *source;
  char bytes[8192];
  size_t n;
  int rc;
  source = NULL;
  rc = cai_cli_response_source(&state->final_response, &source, error);
  if (rc != CAI_OK || source == NULL)
    return rc;
  while ((n = cai_source_read(source, bytes, sizeof(bytes), error)) > 0U) {
    if (cli_text(state, bytes, n) != 0) {
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "render final assistant response");
      break;
    }
  }
  if (error->code != CAI_OK)
    rc = error->code;
  if (rc == CAI_OK && (cli_finish_response(state) != 0 || fflush(stdout) != 0))
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                       "write final assistant response");
  cai_source_close(source);
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
  const char *auth_file;
  char *line;
  pslog_level default_log_level;
  int utility_mode;
  int parsed;
  int handled;
  int rc;
  int result;

  memset(&state, 0, sizeof(state));
  state.timer_fd = -1;
  state.log.fd = -1;
  state.log.wakeup_fd = -1;
  result = 1;
  cai_error_init(&error);
  parsed = cai_cli_parse_options(argc, argv, &state.options);
  if (parsed <= 0) {
    if (parsed < 0 &&
        cai_cli_log_open(&state.log, PSLOG_LEVEL_WARN, 0, &error) == CAI_OK)
      cli_diagnostic(&state, PSLOG_LEVEL_ERROR, state.options.diagnostic);
    result = parsed == 0 ? 0 : 2;
    goto cleanup;
  }
  state.batch = state.options.review || state.options.non_interactive;
  utility_mode = state.options.login || state.options.list ||
                 state.options.resume_list || state.options.export_id != NULL ||
                 state.options.import_file != NULL;
  default_log_level = utility_mode ? PSLOG_LEVEL_WARN : PSLOG_LEVEL_TRACE;
  if (state.options.directory != NULL && chdir(state.options.directory) != 0) {
    int change_error = errno;
    pslog_field path = pslog_str("directory", state.options.directory);
    if (cai_cli_log_open(&state.log, default_log_level, 0, &error) == CAI_OK)
      cai_cli_log_message(state.log.logger, PSLOG_LEVEL_ERROR, "chdir",
                          "diagnostic", strerror(change_error),
                          strlen(strerror(change_error)), &path, 1U);
    result = 2;
    goto cleanup;
  }
  if (cai_cli_log_open(&state.log, default_log_level,
                       !state.batch && !utility_mode, &error) != CAI_OK)
    goto cleanup;
  (void)setlocale(LC_CTYPE, "");
  result = 1;
  if (realpath(".", state.workspace) == NULL) {
    cli_diagnostic(&state, PSLOG_LEVEL_ERROR, "cannot resolve workspace");
    result = 2;
    goto cleanup;
  }
  if (!state.batch && !utility_mode) {
    rc = cai_cli_log_interactive(&state.log, &error);
    if (rc != CAI_OK) {
      cli_print_error(&state, "open interactive log", &error);
      goto cleanup;
    }
  }
  rc = cai_cli_pouch_open(&state.pouch, state.options.lockd,
                          state.options.lockd_client_pem, state.log.logger,
                          &error);
  if (rc != CAI_OK) {
    cli_print_error(&state, "open lockd store", &error);
    goto cleanup;
  }
  state.store = state.pouch.store;
  if (state.options.login) {
    result = cai_cli_login(&state.pouch.credentials, state.log.logger);
    goto cleanup;
  }
  if (state.options.list || state.options.resume_list) {
    cai_sink *destination;
    destination = NULL;
    rc = cai_cli_pouch_list(&state.pouch,
                            state.options.list ? NULL : state.workspace,
                            &state.sessions, &state.session_count, &error);
    if (rc == CAI_OK)
      rc = cai_sink_stdout(&destination, &error);
    if (rc == CAI_OK)
      rc =
          cai_cli_sessions_print(destination, state.sessions,
                                 state.session_count, state.options.resume_list,
                                 mdf_terminal_width(STDOUT_FILENO, 80), &error);
    cai_sink_close(destination);
    if (rc != CAI_OK)
      cli_print_error(&state, "list conversations", &error);
    result = rc == CAI_OK ? 0 : 1;
    goto cleanup;
  }
  if (state.options.export_id != NULL) {
    cai_sink *destination;
    destination = NULL;
    rc = cai_sink_stdout(&destination, &error);
    if (rc == CAI_OK)
      rc = cai_cli_session_export(&state.pouch, state.options.export_id,
                                  state.options.export_dir, NULL, destination,
                                  &error);
    cai_sink_close(destination);
    if (rc != CAI_OK)
      cli_print_error(&state, "export conversation", &error);
    result = rc == CAI_OK ? 0 : 1;
    goto cleanup;
  }
  if (state.options.import_file != NULL) {
    char id[CAI_AGENT_SESSION_ID_MAX];
    rc = cai_cli_pouch_import(&state.pouch, state.options.import_file,
                              state.workspace, id, sizeof(id), &error);
    if (rc == CAI_OK)
      puts(id);
    else
      cli_print_error(&state, "import conversation", &error);
    result = rc == CAI_OK ? 0 : 1;
    goto cleanup;
  }
  auth_file = state.options.auth_json;
  if (strcmp(state.options.provider, "chatgpt") == 0) {
    if (auth_file == NULL && getenv("HOME") != NULL) {
      rc = snprintf(auth_path, sizeof(auth_path), "%s/.codex/auth.json",
                    getenv("HOME"));
      if (rc > 0 && (size_t)rc < sizeof(auth_path) &&
          access(auth_path, F_OK) == 0)
        auth_file = auth_path;
    }
    if (auth_file != NULL) {
      rc = cai_cli_pouch_seed_auth(&state.pouch, auth_file, &error);
      if (rc != CAI_OK) {
        cli_print_error(&state, "import ChatGPT auth", &error);
        goto cleanup;
      }
    }
  }
  state.interactive = !state.options.review && !state.options.non_interactive &&
                      isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);

  cai_cli_status_init(&state.status, state.workspace, getenv("HOME"));
  result = 1;
  if (!state.batch) {
    state.sl = sl_create();
    if (state.sl == NULL) {
      cli_diagnostic(&state, PSLOG_LEVEL_ERROR,
                     "cai: failed to create softline prompt\n");
      goto cleanup;
    }
  }
  state.width = cli_render_width();
  mdf_options_init(&mdf_config);
  mdf_config.output_fd = STDOUT_FILENO;
  mdf_config.width = state.width;
  mdf_config.margin_left = state.width >= 5 ? 2 : 0;
  mdf_config.boring = !state.interactive;
  if ((state.interactive && (sl_set_bounds(state.sl, 0, 0, 0, 0) != SL_OK ||
                             sl_set_statusline(state.sl, 1, 0) != SL_OK)) ||
      (!state.batch && sl_output_stream_begin(state.sl) != SL_OK) ||
      mdf_create(MDF_FORMAT_ANSI, &mdf_config, &state.renderer) != MDF_OK) {
    cli_diagnostic(&state, PSLOG_LEVEL_ERROR,
                   "cai: failed to initialize terminal renderers\n");
    goto cleanup;
  }
  sink.userdata = &state;
  sink.write = cli_sink;
  if (state.renderer->set_sink(state.renderer, &sink) != MDF_OK) {
    cli_diagnostic(&state, PSLOG_LEVEL_ERROR,
                   "cai: failed to connect Markdown renderer\n");
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
      cli_diagnostic(&state, PSLOG_LEVEL_ERROR,
                     "cai: failed to start turn status timer\n");
      goto cleanup;
    }
    state.has_timer_watch = 1;
    if (sl_watch_add(state.sl, state.log.wakeup_fd,
                     SL_WATCH_READ | SL_WATCH_ERROR | SL_WATCH_HANGUP,
                     cli_log_wakeup, &state, &state.log_watch_id) != SL_OK) {
      cli_diagnostic(&state, PSLOG_LEVEL_ERROR,
                     "watch interactive log notices");
      goto cleanup;
    }
    state.has_log_watch = 1;
  }
  cai_client_config_init(&client_config);
  client_config.logger = state.log.logger;
  if (strcmp(state.options.provider, "chatgpt") == 0) {
    cai_chatgpt_auth_config_init(&auth_config);
    auth_config.logger = state.log.logger;
    auth_config.storage = &state.pouch.credentials;
    rc = cai_chatgpt_auth_open(&auth_config, &state.auth, &error);
    if (rc != CAI_OK) {
      cli_print_error(&state, "open ChatGPT auth", &error);
      cli_diagnostic(&state, PSLOG_LEVEL_ERROR,
                     "cai: run cai --login to authenticate\n");
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
    cli_print_error(&state, "open client", &error);
    goto cleanup;
  }
  if (state.auth != NULL) {
    cai_error quota_error;
    cai_client_config quota_config;
    cai_error_init(&quota_error);
    cai_client_config_init(&quota_config);
    quota_config.logger = state.log.logger;
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
    cli_print_error(&state, "open agent session", &error);
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
      cli_print_error(&state, "review", &error);
    else
      result = 0;
    goto cleanup;
  }
  rc = cli_start_automation(&state, &error);
  if (rc != CAI_OK) {
    cli_print_error(&state, "start work", &error);
    goto cleanup;
  }
  if (state.options.non_interactive) {
    rc = cli_run_noninteractive(&state, &error);
    if (rc != CAI_OK) {
      if (!state.automation_failed || error.message != NULL)
        cli_print_error(&state, "non-interactive run", &error);
    } else {
      result = 0;
    }
    goto cleanup;
  }
  while (!state.exit_requested) {
    if (cai_cli_log_notices(&state.log, cli_notice, &state) != 0)
      goto cleanup;
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
        cli_diagnostic(&state, PSLOG_LEVEL_ERROR, "cai: prompt input failed\n");
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
        cli_print_error(&state, "input", &error);
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
      cli_print_error(&state, "status", &error);
      goto cleanup;
    }
  }
  if (state.runtime != NULL && !state.exit_requested) {
    rc = cli_drain(&state, &error);
    if (rc != CAI_OK) {
      cli_print_error(&state, "finish agent turn", &error);
      goto cleanup;
    }
  }
  result = 0;
cleanup:
  if (state.batch && !state.options.review && state.renderer != NULL) {
    cai_error final_error;
    cai_error_init(&final_error);
    if (cli_output_final(&state, &final_error) != CAI_OK) {
      cli_print_error(&state, "final response", &final_error);
      result = 1;
    }
    cai_error_cleanup(&final_error);
  }
  cai_cli_response_close(&state.final_response);
  if (state.runtime != NULL)
    cli_close_runtime(&state);
  if (state.has_log_watch)
    (void)sl_watch_remove(state.sl, state.log_watch_id);
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
  cai_cli_pouch_close(&state.pouch);
  if (state.sl != NULL)
    (void)cai_cli_log_notices(&state.log, cli_notice, &state);
  if (state.renderer != NULL) {
    (void)cli_finish_response(&state);
    (void)cli_finish_reasoning(&state);
    state.renderer->destroy(state.renderer);
  }
  if (state.sl != NULL) {
    (void)sl_output_stream_end(state.sl);
    sl_destroy(state.sl);
  }
  if (state.sl == NULL)
    (void)cai_cli_log_notices(&state.log, cli_startup_notice, &state);
  free(state.sessions);
  free(state.review_report);
  cai_error_cleanup(&error);
  if (cai_cli_log_close(&state.log) != 0)
    result = 1;
  return result;
}
