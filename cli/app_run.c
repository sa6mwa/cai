#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "app_internal.h"
#include "config.h"
#include "literal.h"
#include "log.h"
#include "login.h"
#include "mcp.h"
#include "messages.h"
#include "options.h"
#include "pouch.h"
#include "quota_refresh.h"
#include "reasoning.h"
#include "response.h"
#include "review_report.h"
#include "runtime_options.h"
#include "session_commands.h"
#include "shell.h"
#include "skills.h"
#include "status.h"
#include "todo.h"
#include <cai/chrome.h>
#include <locale.h>

#include <cai/agent_runtime.h>
#include <cai/auth.h>
#include <cai/display.h>
#include <cai/quota.h>
#include <cai/session_store.h>
#include <libmdf/mdf.h>
#include <softline/softline.h>

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef cai_cli_session cli_session;

static const char cli_welcome[] = "Welcome to CAI. Send /help for usage.";

/* Softline restores terminal input, raises SIGINT, then returns INTERRUPTED.
 * Let the prompt loop cancel the turn and keep the interactive session open. */
static void cli_keep_session_on_interrupt(int signo) { (void)signo; }

static int cli_cancel_editor(sl_t *sl, sl_key_t key, void *context,
                             sl_key_action_t *action) {
  (void)sl;
  (void)key;
  (void)context;
  *action = SL_KEY_ACTION_CANCEL;
  return SL_OK;
}

typedef struct cli_state {
  const cai_app_config *application;
  cai_app_bridge *bridge;
  sl_watch_id_t host_watch;
  int has_host_watch;
  int host_stopped;
  size_t batch_pending_turns;
  cai_cli_options options;
  cai_config *configuration;
  cai_agent_session_store store;
  cai_cli_pouch pouch;
  cai_chatgpt_auth *auth;
  cai_chatgpt_auth shared_auth;
  pthread_mutex_t auth_lock;
  int has_auth_lock;
  cai_client *client;
  cai_client *model_client;
  pslog_logger *model_log;
  cai_client *quota_client;
  cai_agent_runtime *runtime;
  cai_agent_runtime *review_runtime;
  sl_watch_id_t review_watch_id;
  int has_review_watch;
  cai_agent_runtime_metrics last_review_metrics;
  int has_review_metrics;
  cai_cli_mcp mcp;
  int mcp_initialized;
  cai_cli_messages *messages;
  sl_watch_id_t message_watch_id;
  int has_message_watch;
  sl_t *sl;
  mdf *renderer;
  mdf *notice_renderer;
  cai_tool_display *tool_display;
  cai_sink *tool_sink;
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
  unsigned short output_columns;
  unsigned short output_rows;
  int response_open;
  int documents_rendered;
  int reasoning_open;
  int feed_breaks;
  int feed_ansi;
  int exit_requested;
  int was_busy;
  int awaiting_turn;
  int show_welcome;
  int automation_done;
  int automation_failed;
  int auto_goal;
  size_t goal_starts_pending;
  size_t next_instruction;
  cai_chatgpt_quota quota;
  cai_cli_quota_refresh quota_refresh;
  sl_watch_id_t quota_watch_id;
  int has_quota_watch;
  cai_cli_status status;
  cai_cli_reasoning reasoning;
  int row_line_start;
  int row_ansi;
  int notice_open;
  int notice_first_row;
  int command_input;
  int command_ok;
  const char *pending_command;
  cai_cli_shell shell;
  sl_watch_id_t shell_watches[3];
  int shell_watched[3];
  char workspace[PATH_MAX];
  char message_queue_override[129];
  char clipboard_directory[PATH_MAX];
} cli_state;

static int cli_advance_automation(cli_state *state, cai_error *error);
static int cli_messages_process(cli_state *state, cai_error *error);
static int cli_flush_command(cli_state *state, int ok);
static int cli_submit_automation(cli_state *state, const char *instruction,
                                 cai_error *error);
static int cli_host_process(cli_state *state, cai_error *error);
static int cli_pump(cli_state *state, long timeout, cai_error *error);
static int cli_review_event(void *context, const cai_agent_runtime_event *event,
                            cai_error *error);

static int cli_load_model_metadata(const cai_cli_options *options, char **out,
                                   cai_error *error) {
  char path[PATH_MAX];
  char message[PATH_MAX + 128];
  const char *root;
  FILE *file;
  long length;
  size_t size;
  int written;

  *out = NULL;
  if (options->model_metadata != NULL) {
    written = snprintf(path, sizeof(path), "%s", options->model_metadata);
  } else {
    root = options->config_dir;
    if (root != NULL) {
      written = snprintf(path, sizeof(path), "%s/models.json", root);
    } else if (getenv("XDG_CONFIG_HOME") != NULL &&
               getenv("XDG_CONFIG_HOME")[0] != '\0') {
      written = snprintf(path, sizeof(path), "%s/cai/models.json",
                         getenv("XDG_CONFIG_HOME"));
    } else if (getenv("HOME") != NULL) {
      written = snprintf(path, sizeof(path), "%s/.config/cai/models.json",
                         getenv("HOME"));
    } else {
      return CAI_OK;
    }
  }
  if (written < 0 || (size_t)written >= sizeof(path)) {
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "model metadata path is too long");
  }
  file = fopen(path, "rb");
  if (file == NULL) {
    if (options->model_metadata == NULL && errno == ENOENT) {
      return CAI_OK;
    }
    snprintf(message, sizeof(message), "cannot open model metadata %s: %s",
             path, strerror(errno));
    return cai_cli_error(error, CAI_ERR_TRANSPORT, message);
  }
  if (fseek(file, 0L, SEEK_END) != 0 || (length = ftell(file)) < 0L ||
      length > 1024L * 1024L || fseek(file, 0L, SEEK_SET) != 0) {
    fclose(file);
    snprintf(message, sizeof(message),
             "cannot read model metadata %s (file must be at most 1 MiB)",
             path);
    return cai_cli_error(error, CAI_ERR_INVALID, message);
  }
  *out = (char *)malloc((size_t)length + 1U);
  if (*out == NULL) {
    fclose(file);
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "cannot allocate model metadata file");
  }
  size = fread(*out, 1U, (size_t)length, file);
  if (fclose(file) != 0 || size != (size_t)length ||
      memchr(*out, '\0', size) != NULL) {
    free(*out);
    *out = NULL;
    snprintf(message, sizeof(message),
             "cannot read model metadata %s (read error or embedded NUL)",
             path);
    return cai_cli_error(error, CAI_ERR_INVALID, message);
  }
  (*out)[size] = '\0';
  return CAI_OK;
}

static void cli_print_error(cli_state *state, const char *operation,
                            const cai_error *error) {
  cai_app_bridge_error(state->bridge, error);
  cai_cli_log_error(state->log.logger, operation, error);
}

static void cli_diagnostic(cli_state *state, pslog_level level,
                           const char *text) {
  cai_cli_log_message(state->log.logger, level, "diagnostic", "diagnostic",
                      text, strlen(text), NULL, 0U);
}

/* Both clients share credentials, while token refresh and token copies are
 * serialized. Network requests use their own client and copied bearer token. */
static int cli_auth_token(cai_chatgpt_auth *auth, char **out,
                          cai_error *error) {
  cli_state *state = auth->impl;
  int rc;
  pthread_mutex_lock(&state->auth_lock);
  rc = state->auth->access_token(state->auth, out, error);
  pthread_mutex_unlock(&state->auth_lock);
  return rc;
}

static int cli_auth_refresh(cai_chatgpt_auth *auth, cai_error *error) {
  cli_state *state = auth->impl;
  int rc;
  pthread_mutex_lock(&state->auth_lock);
  rc = state->auth->refresh(state->auth, error);
  pthread_mutex_unlock(&state->auth_lock);
  return rc;
}

static int cli_fetch_quota(void *context, cai_chatgpt_quota *out,
                           cai_error *error) {
  return cai_client_chatgpt_quota((cai_client *)context, out, error);
}

static void cli_refresh_quota(cli_state *state, int force) {
  cai_cli_quota_request(&state->quota_refresh, force, force);
  (void)cai_cli_quota_take(&state->quota_refresh, &state->quota);
}

/* Observe resize on the UI owner thread before entering a softline feed.
 * Include height-only changes and synchronous output after readline returns.
 * Ordinary fragments stay immediate; only changed geometry can need a CPR. */
static int cli_reconcile_output(cli_state *state) {
  struct winsize size;
  if (state->batch || ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0 ||
      size.ws_col == 0 || size.ws_row == 0 ||
      (size.ws_col == state->output_columns &&
       size.ws_row == state->output_rows))
    return 0;
  if (sl_output_stream_reconcile(state->sl) != SL_OK)
    return -1;
  state->output_columns = size.ws_col;
  state->output_rows = size.ws_row;
  return 0;
}

static int cli_sink(void *context, const char *bytes, size_t count) {
  cli_state *state;
  size_t i;
  state = (cli_state *)context;
  if (count == 0U)
    return 0;
  if (state->batch)
    return fwrite(bytes, 1U, count, stdout) == count ? 0 : -1;
  if (cli_reconcile_output(state) != 0 ||
      sl_output_stream_write(state->sl, bytes, count) != SL_OK)
    return -1;
  for (i = 0U; i < count; i++) {
    unsigned char ch = (unsigned char)bytes[i];
    if (state->feed_ansi == 1) {
      state->feed_ansi = ch == '[' ? 2 : 0;
    } else if (state->feed_ansi == 2) {
      if (ch >= 0x40U && ch <= 0x7eU)
        state->feed_ansi = 0;
    } else if (ch == 27U) {
      state->feed_ansi = 1;
    } else if (ch == '\n') {
      if (state->feed_breaks < 2)
        state->feed_breaks++;
    } else if (ch != '\r') {
      state->feed_breaks = 0;
    }
  }
  return 0;
}

static int cli_render_width(void) {
  return isatty(STDOUT_FILENO) ? mdf_terminal_width(STDOUT_FILENO, 80) : 80;
}

static int cli_tool_sink(void *context, const void *bytes, size_t count,
                         cai_error *error) {
  if (cli_sink(context, (const char *)bytes, count) == 0)
    return CAI_OK;
  return cai_cli_error(error, CAI_ERR_TRANSPORT, "cannot write tool activity");
}
static int cli_tool_begin_block(void *context, cai_error *error) {
  cli_state *state = context;
  while (state->feed_breaks < 2) {
    if (cli_sink(state, "\n", 1U) != 0)
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "cannot separate tool activity");
  }
  return CAI_OK;
}
/* Prefix visible renderer rows without retaining body text or inserting a
 * marker inside an ANSI sequence split across writes. Blank rows stay blank. */
static int cli_prefixed_sink(void *context, const char *bytes, size_t count) {
  cli_state *state = context;
  size_t i;
  size_t start = 0U;
  for (i = 0U; i < count; i++) {
    unsigned char ch = (unsigned char)bytes[i];
    if (state->row_line_start && state->row_ansi == 0 && ch != 27U &&
        ch != '\n' && ch != '\r') {
      const char *prefix = "? ";
      if (state->notice_open) {
        prefix = state->notice_first_row ? (state->command_input ? "$ " : "* ")
                                         : "  ";
        if (state->interactive && getenv("NO_COLOR") == NULL) {
          if (state->command_input) {
            prefix = !state->notice_first_row
                         ? "\033[38;2;215;160;95m  "
                         : (state->command_ok
                                ? "\033[0;92m$\033[0m \033[38;2;215;160;95m"
                                : "\033[0;91m$\033[0m \033[38;2;215;160;95m");
          } else if (state->notice_first_row)
            prefix = "\033[91m*\033[0m ";
        }
      }
      if (cli_sink(state, bytes + start, i - start) != 0 ||
          cli_sink(state, prefix, strlen(prefix)) != 0)
        return -1;
      start = i;
      state->row_line_start = 0;
      state->notice_first_row = 0;
    }
    if (state->row_ansi == 1) {
      state->row_ansi = ch == '[' ? 2 : ch == ']' ? 3 : 0;
    } else if (state->row_ansi == 2) {
      if (ch >= 0x40U && ch <= 0x7eU)
        state->row_ansi = 0;
    } else if (state->row_ansi == 3) {
      if (ch == 7U)
        state->row_ansi = 0;
      else if (ch == 27U)
        state->row_ansi = 4;
    } else if (state->row_ansi == 4) {
      state->row_ansi = ch == '\\' ? 0 : 3;
    } else if (ch == 27U) {
      state->row_ansi = 1;
    } else if (ch == '\n') {
      state->row_line_start = 1;
    }
  }
  return cli_sink(state, bytes + start, count - start);
}
/* Keep a single libmdf binding: replacing a sink discards its document state.
 */
static int cli_renderer_sink(void *context, const char *bytes, size_t count) {
  cli_state *state = context;
  return state->reasoning_open || state->notice_open
             ? cli_prefixed_sink(context, bytes, count)
             : cli_sink(context, bytes, count);
}
static int cli_tool_display_init(cli_state *state, cai_error *error) {
  cai_tool_display_config config;
  cai_sink_callbacks callbacks;
  int rc, width;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = cli_tool_sink;
  callbacks.context = state;
  rc = cai_sink_from_callbacks(&callbacks, &state->tool_sink, error);
  if (rc != CAI_OK)
    return rc;
  memset(&config, 0, sizeof(config));
  config.sink = state->tool_sink;
  config.mode = state->options.tool_output != NULL &&
                        strcmp(state->options.tool_output, "full") == 0
                    ? CAI_TOOL_OUTPUT_FULL
                    : CAI_TOOL_OUTPUT_COMPACT;
  config.color = state->interactive && getenv("NO_COLOR") == NULL;
  config.begin_block = cli_tool_begin_block;
  config.begin_block_context = state;
  rc = cai_tool_display_new(&config, &state->tool_display, error);
  width = cli_render_width();
  if (rc == CAI_OK)
    rc = cai_tool_display_set_width(state->tool_display,
                                    (size_t)(width < 32     ? 0
                                             : width > 4096 ? 4096
                                                            : width),
                                    error);
  return rc;
}

static int cli_geometry(cli_state *state) {
  int width;
  width = cli_render_width();
  if (width == state->width) {
    return 0;
  }
  /* Softline owns native terminal sizing; only libmdf needs the new width. */
  if (state->renderer != NULL &&
      state->renderer->set_geometry(
          state->renderer,
          state->reasoning_open ? (width >= 5 ? width - 2 : 3) : width,
          state->reasoning_open ? 0 : (width >= 5 ? 2 : 0), 0) != MDF_OK) {
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
  if (state->renderer->set_geometry(state->renderer, state->width,
                                    state->width >= 5 ? 2 : 0, 0) != MDF_OK)
    return -1;
  return 0;
}

static int cli_reasoning_body(void *context, const char *data, size_t length) {
  cli_state *state = context;
  if (cli_finish_response(state) != 0 || cli_geometry(state) != 0)
    return -1;
  if (!state->reasoning_open) {
    if (state->documents_rendered > 0 &&
        state->renderer->begin_document(state->renderer) != MDF_OK)
      return -1;
    if (state->renderer->set_geometry(state->renderer,
                                      state->width >= 5 ? state->width - 2 : 3,
                                      0, 0) != MDF_OK)
      return -1;
    state->reasoning_open = 1;
    state->row_line_start = 1;
    state->row_ansi = 0;
  }
  return state->renderer->feed(state->renderer, data, length) == MDF_OK ? 0
                                                                        : -1;
}

static int cli_text(cli_state *state, const char *data, size_t length) {
  if (cli_flush_command(state, 1) != 0)
    return -1;
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

/* Manual text tokens have their own renderer, separate from model Markdown. */
static int cli_literal_prepare(cli_state *state, int prefixed) {
  mdf_options options;
  mdf_sink sink;
  if (state->notice_renderer == NULL) {
    mdf_options_init(&options);
    options.width = state->width;
    options.boring = 1;
    options.ansi_mode = MDF_ANSI_OFF;
    if (mdf_create(MDF_FORMAT_ANSI, &options, &state->notice_renderer) !=
        MDF_OK)
      return -1;
    sink.userdata = state;
    sink.write = cli_renderer_sink;
    if (state->notice_renderer->set_sink(state->notice_renderer, &sink) !=
        MDF_OK)
      return -1;
  }
  return state->notice_renderer->set_geometry(
             state->notice_renderer,
             prefixed ? (state->width >= 5 ? state->width - 2 : 3)
                      : state->width,
             prefixed ? 0 : (state->width >= 5 ? 2 : 0), 0) == MDF_OK
             ? 0
             : -1;
}
static int cli_write(cli_state *state, const char *text) {
  if (cli_flush_command(state, 1) != 0)
    return -1;
  if (cli_finish_response(state) != 0 || cli_finish_reasoning(state) != 0)
    return -1;
  if (state->batch) {
    cli_diagnostic(state, PSLOG_LEVEL_INFO, text);
    return 0;
  }
  if (cli_geometry(state) != 0 || cli_literal_prepare(state, 0) != 0)
    return -1;
  while (state->feed_breaks < 2)
    if (cli_sink(state, "\n", 1U) != 0)
      return -1;
  while (*text == '\n' || *text == '\r')
    text++;
  if (cai_cli_literal_render(state->notice_renderer, text) != 0)
    return -1;
  while (state->feed_breaks < 2)
    if (cli_sink(state, "\n", 1U) != 0)
      return -1;
  return 0;
}

static int cli_prompt(cli_state *state, const char *text) {
  if (state->batch)
    return 0;
  if (cli_flush_command(state, 1) != 0)
    return -1;
  if (cli_finish_response(state) != 0 || cli_finish_reasoning(state) != 0) {
    return -1;
  }
  if (cli_reconcile_output(state) != 0 ||
      sl_output_stream_write_quoted_prompt(state->sl, text) != SL_OK)
    return -1;
  state->feed_breaks = 2;
  return 0;
}

static void cli_command_input(cli_state *state, const char *text) {
  pslog_field field =
      pslog_str("prompt_kind", text[0] == '!' ? "shell_command" : "command");
  cai_cli_log_message(state->log.logger, PSLOG_LEVEL_INFO, "command_input",
                      "user", text, strlen(text), &field, 1U);
  state->pending_command = text;
}

/* Delay the echo until validation succeeds or fails, but always emit it
 * before command results. No command output is buffered for this purpose. */
static int cli_flush_command(cli_state *state, int ok) {
  const char *text = state->pending_command;
  int rc;
  if (text == NULL)
    return 0;
  state->pending_command = NULL;
  state->command_ok = ok;
  if (cli_finish_response(state) != 0 || cli_finish_reasoning(state) != 0 ||
      cli_geometry(state) != 0 || cli_literal_prepare(state, 1) != 0)
    return -1;
  while (state->feed_breaks < 2)
    if (cli_sink(state, "\n", 1U) != 0)
      return -1;
  state->notice_open = 1;
  state->notice_first_row = 1;
  state->command_input = 1;
  state->row_line_start = 1;
  state->row_ansi = 0;
  rc = cai_cli_literal_render(state->notice_renderer, text);
  state->notice_open = 0;
  state->command_input = 0;
  if (state->interactive && getenv("NO_COLOR") == NULL &&
      cli_sink(state, "\033[0m", 4U) != 0)
    rc = -1;
  while (rc == 0 && state->feed_breaks < 2)
    rc = cli_sink(state, "\n", 1U);
  return rc;
}

static int cli_shell_literal(void *context, const char *bytes, size_t length,
                             cai_error *error) {
  return cli_sink(context, bytes, length) == 0
             ? CAI_OK
             : cai_cli_error(error, CAI_ERR_TRANSPORT,
                             "cannot display shell output");
}

static int cli_shell_output(void *context, const char *bytes, size_t length,
                            cai_error *error) {
  cli_state *state = context;
  int rc =
      cai_agent_runtime_user_shell_write(state->runtime, bytes, length, error);
  if (rc != CAI_OK)
    return rc;
  if (cli_finish_response(state) != 0 || cli_finish_reasoning(state) != 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "cannot finish output before shell text");
  return cai_cli_shell_plain(&state->shell, bytes, length, 0, cli_shell_literal,
                             state, error);
}

static void cli_shell_unwatch(cli_state *state) {
  size_t i;
  for (i = 0U; i < 3U; i++)
    if (state->shell_watched[i]) {
      (void)sl_watch_remove(state->sl, state->shell_watches[i]);
      state->shell_watched[i] = 0;
    }
}

static int cli_shell_pump(cli_state *state, cai_error *error) {
  char message[160];
  int rc = cai_cli_shell_pump(&state->shell, cli_shell_output, state, error);
  if (rc != CAI_OK)
    return rc;
  if (state->shell.exited && state->shell_watched[1]) {
    (void)sl_watch_remove(state->sl, state->shell_watches[1]);
    state->shell_watched[1] = 0;
  }
  if (state->shell.eof && state->shell_watched[0]) {
    (void)sl_watch_remove(state->sl, state->shell_watches[0]);
    state->shell_watched[0] = 0;
  }
  if (state->shell.exited && state->shell.eof) {
    rc = cai_cli_shell_plain(&state->shell, NULL, 0U, 1, cli_shell_literal,
                             state, error);
    if (rc == CAI_OK)
      rc = cai_agent_runtime_user_shell_finish(
          state->runtime, state->shell.exit_code,
          cai_cli_shell_elapsed(&state->shell), error);
    if (rc != CAI_OK)
      return rc;
    snprintf(message, sizeof(message), "Shell command exited %d (%.3fs).",
             state->shell.exit_code,
             (double)cai_cli_shell_elapsed(&state->shell) / 1000.0);
    cli_shell_unwatch(state);
    cai_cli_shell_close(&state->shell);
    if (cli_write(state, message) != 0)
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "cannot display shell completion");
  }
  return CAI_OK;
}

static int cli_shell_stop(cli_state *state, cai_error *error) {
  int rc;
  cai_cli_shell_cancel(&state->shell);
  while (state->shell.pid > 0) {
    struct pollfd fd;
    fd.fd = state->shell.output_fd;
    fd.events = POLLIN | POLLHUP;
    fd.revents = 0;
    (void)poll(&fd, 1U, 50);
    rc = cli_shell_pump(state, error);
    if (rc != CAI_OK)
      return rc;
  }
  return CAI_OK;
}

static int cli_shell_wakeup(sl_t *sl, const sl_watch_event_t *event,
                            void *context) {
  cli_state *state = context;
  cai_error error;
  int rc;
  (void)sl;
  (void)event;
  cai_error_init(&error);
  rc = cli_shell_pump(state, &error);
  if (rc != CAI_OK)
    cli_print_error(state, "user shell command", &error);
  cai_error_cleanup(&error);
  return rc == CAI_OK ? SL_OK : SL_ERROR_IO;
}

static int cli_shell_start(cli_state *state, const char *command,
                           cai_error *error) {
  int fds[3], rc;
  size_t i;
  if (state->shell.pid > 0)
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "a shell command is running; cancel it with Ctrl-C first");
  while (*command == ' ' || *command == '\t')
    command++;
  rc = cai_agent_runtime_user_shell_begin(state->runtime, command,
                                          state->options.shell_context, error);
  if (rc != CAI_OK)
    return rc;
  rc = cai_cli_shell_start(&state->shell, command, state->workspace, error);
  if (rc != CAI_OK) {
    cai_error record_error;
    cai_error_init(&record_error);
    (void)cai_agent_runtime_user_shell_finish(state->runtime, 127, 0U,
                                              &record_error);
    cai_error_cleanup(&record_error);
    return rc;
  }
  fds[0] = state->shell.output_fd;
  fds[1] = state->shell.exit_fd;
  fds[2] = state->shell.timer_fd;
  for (i = 0U; i < 3U; i++) {
    if (sl_watch_add(
            state->sl, fds[i], SL_WATCH_READ | SL_WATCH_ERROR | SL_WATCH_HANGUP,
            cli_shell_wakeup, state, &state->shell_watches[i]) != SL_OK) {
      cli_shell_unwatch(state);
      rc = cli_shell_stop(state, error);
      if (rc != CAI_OK)
        return rc;
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "cannot watch user shell command");
    }
    state->shell_watched[i] = 1;
  }
  return CAI_OK;
}

static int cli_is_terminal_shell(const char *line) {
  return strcmp(line, "!sh") == 0 || strcmp(line, "/shell") == 0 ||
         strcmp(line, "!sh+") == 0 || strcmp(line, "/shellshare") == 0;
}

static int cli_terminal_shell_ready(cli_state *state, cai_error *error) {
  cai_agent_run_state run_state;
  cai_agent_runtime_metrics metrics;
  int rc;
  if (!state->interactive)
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "interactive shell requires terminal input and output");
  rc = cai_agent_runtime_state(state->runtime, &run_state, error);
  if (rc == CAI_OK)
    rc = cai_agent_runtime_get_metrics(state->runtime, &metrics, error);
  if (rc != CAI_OK)
    return rc;
  if (state->review_runtime != NULL || state->shell.pid > 0 ||
      metrics.turn_active || state->awaiting_turn || !state->automation_done ||
      sl_prompt_queue_count(state->sl) > 0 || run_state == CAI_AGENT_SAMPLING ||
      run_state == CAI_AGENT_DISPATCHING_TOOL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "Shell is available only between turns.");
  return CAI_OK;
}

/* Key callbacks only guard submission. Terminal handoff runs in the owner
 * loop after next_prompt returns, outside every active editor callback. */
static int cli_shell_editor_key(sl_t *sl, sl_key_t key, void *context,
                                sl_key_action_t *action) {
  cli_state *state = context;
  cai_error error;
  int rc = SL_OK;
  *action = SL_KEY_ACTION_PASS;
  if (sl_history_search_active(sl) || !cli_is_terminal_shell(sl_buffer(sl)))
    return SL_OK;
  cai_error_init(&error);
  if (key == SL_KEY_TAB || cli_terminal_shell_ready(state, &error) != CAI_OK) {
    *action = SL_KEY_ACTION_HANDLED;
    rc = sl_set_status_message(
        sl, key == SL_KEY_TAB
                ? "Use Enter to open the shell; it cannot be queued."
                : error.message);
  }
  cai_error_cleanup(&error);
  return rc;
}

static int cli_shell_restore_foreground(pid_t foreground, cai_error *error) {
  struct sigaction ignore, previous;
  int rc, saved_errno;
  char message[256];
  if (foreground < 0)
    return CAI_OK;
  memset(&ignore, 0, sizeof(ignore));
  ignore.sa_handler = SIG_IGN;
  sigemptyset(&ignore.sa_mask);
  if (sigaction(SIGTTOU, &ignore, &previous) != 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "protect terminal foreground restoration");
  rc = tcsetpgrp(STDIN_FILENO, foreground);
  saved_errno = errno;
  if (sigaction(SIGTTOU, &previous, NULL) != 0 && rc == 0) {
    rc = -1;
    saved_errno = errno;
  }
  if (rc == 0)
    return CAI_OK;
  snprintf(message, sizeof(message), "restore terminal foreground: %s",
           strerror(saved_errno));
  return cai_cli_error(error, CAI_ERR_TRANSPORT, message);
}

static int cli_terminal_shell_output(void *context, const char *bytes,
                                     size_t length, cai_error *error) {
  cli_state *state = context;
  return cai_agent_runtime_user_shell_write(state->runtime, bytes, length,
                                            error);
}

static int cli_terminal_shell(cli_state *state, int share, cai_error *error) {
  pslog_field fields[3];
  char message[256];
  const char *shell = getenv("SHELL");
  pid_t foreground;
  int rc, status, shell_rc;
  cai_error shell_error;
  rc = cli_terminal_shell_ready(state, error);
  if (rc != CAI_OK)
    return rc;
  if (shell == NULL || shell[0] == '\0')
    shell = "/bin/sh";
  if (share) {
    rc = cai_agent_runtime_user_shell_begin(state->runtime, shell, 1, error);
    if (rc != CAI_OK)
      return rc;
  }
  (void)clock_gettime(CLOCK_MONOTONIC, &state->shell.started);
  foreground = tcgetpgrp(STDIN_FILENO);
  if (cli_flush_command(state, 1) != 0 || cli_finish_response(state) != 0 ||
      cli_finish_reasoning(state) != 0 ||
      sl_output_stream_end(state->sl) != SL_OK) {
    state->exit_requested = 1;
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "release terminal for interactive shell");
  }
  fields[0] = pslog_str("shell", shell);
  cai_cli_log_message(state->log.logger, PSLOG_LEVEL_INFO,
                      "terminal_shell_started", "user", "Interactive shell",
                      strlen("Interactive shell"), fields, 1U);
  cai_error_init(&shell_error);
  shell_rc = cai_cli_shell_handoff(shell, state->workspace,
                                   share ? cli_terminal_shell_output : NULL,
                                   state, &status, &shell_error);
  /* Recover ownership before any logger or renderer can write to the TTY.
   * A killed shell cannot restore its previous foreground process group. */
  rc = cli_shell_restore_foreground(foreground, error);
  if (rc != CAI_OK) {
    cai_error_cleanup(&shell_error);
    state->exit_requested = 1;
    return rc;
  }
  state->feed_breaks = 0;
  state->feed_ansi = 0;
  fields[1] =
      pslog_i64("exit_code", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
  fields[2] = pslog_i64("signal", WIFSIGNALED(status) ? WTERMSIG(status) : 0);
  cai_cli_log_message(state->log.logger, PSLOG_LEVEL_INFO,
                      "terminal_shell_completed", "user", "Interactive shell",
                      strlen("Interactive shell"), fields, 3U);
  if (sl_output_stream_begin(state->sl) != SL_OK || cli_geometry(state) != 0) {
    cai_error_cleanup(&shell_error);
    state->exit_requested = 1;
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "restore CAI terminal after interactive shell");
  }
  cai_cli_status_refresh_branch(&state->status, state->workspace);
  if (share) {
    rc = cai_agent_runtime_user_shell_finish(
        state->runtime,
        shell_rc != CAI_OK  ? 127
        : WIFEXITED(status) ? WEXITSTATUS(status)
                            : 128 + WTERMSIG(status),
        cai_cli_shell_elapsed(&state->shell), error);
    if (rc != CAI_OK) {
      cai_error_cleanup(&shell_error);
      state->exit_requested = 1;
      return rc;
    }
  }
  if (shell_rc != CAI_OK) {
    rc = cai_cli_error(error, shell_rc, shell_error.message);
    cai_error_cleanup(&shell_error);
    return rc;
  }
  cai_error_cleanup(&shell_error);
  if (WIFSIGNALED(status))
    snprintf(message, sizeof(message),
             "interactive shell terminated by signal %d", WTERMSIG(status));
  else if (WEXITSTATUS(status) != 0)
    snprintf(message, sizeof(message), "interactive shell exited %d",
             WEXITSTATUS(status));
  else
    return cli_write(state, "Shell exited 0.") == 0
               ? CAI_OK
               : cai_cli_error(error, CAI_ERR_TRANSPORT,
                               "cannot display shell completion");
  return cai_cli_error(error, CAI_ERR_TRANSPORT, message);
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
                                      "run_cancelled",
                                      "tool_patch",
                                      "tool_patch_locations",
                                      "policy_diagnostic",
                                      "reasoning_summary_started"};
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
  if (event->type == CAI_AGENT_EVENT_POLICY_DIAGNOSTIC_STARTED) {
    role = "user";
    kind = "policy_diagnostic";
  } else if (event->type == CAI_AGENT_EVENT_RUN_STARTED ||
             event->type == CAI_AGENT_EVENT_TURN_QUEUED ||
             event->type == CAI_AGENT_EVENT_STEERING_QUEUED ||
             event->type == CAI_AGENT_EVENT_STEERING_DELIVERED) {
    role = "user";
    kind = event->type == CAI_AGENT_EVENT_RUN_STARTED   ? "normal"
           : event->type == CAI_AGENT_EVENT_TURN_QUEUED ? "queued"
                                                        : "steering";
  } else if (event->type == CAI_AGENT_EVENT_TEXT_DELTA ||
             event->type == CAI_AGENT_EVENT_REASONING_SUMMARY ||
             event->type == CAI_AGENT_EVENT_REASONING_SUMMARY_STARTED ||
             event->type == CAI_AGENT_EVENT_REVIEW_REPORT ||
             event->type == CAI_AGENT_EVENT_SUBAGENT_STARTED) {
    role = "assistant";
    kind = event->type == CAI_AGENT_EVENT_REASONING_SUMMARY ||
                   event->type == CAI_AGENT_EVENT_REASONING_SUMMARY_STARTED
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
  state = (cli_state *)context;
  if (state->application != NULL &&
      state->application->event_callback != NULL) {
    int observed = state->application->event_callback(
        state->application->event_context, event, error);
    if (observed != CAI_OK) {
      if (error->code == CAI_OK)
        cai_cli_error(error, observed, "application event observer failed");
      cai_app_bridge_fail(state->bridge, error);
      return observed;
    }
  }
  cli_log_event(state, event);
  if (event->parent_tool_call_id != NULL &&
      strcmp(event->parent_tool_call_id, "/review") == 0) {
    if (event->type == CAI_AGENT_EVENT_TEXT_DELTA)
      return CAI_OK; /* Render the validated findings rather than JSON deltas.
                      */
    if (event->type == CAI_AGENT_EVENT_REVIEW_REPORT) {
      char *formatted = NULL;
      int rc = cai_cli_review_format(event->data, event->data_length,
                                     "markdown", &formatted) == 0
                   ? CAI_OK
                   : CAI_ERR_PROTOCOL;
      if (rc == CAI_OK && (cli_finish_response(state) != 0 ||
                           cli_finish_reasoning(state) != 0 ||
                           cli_text(state, formatted, strlen(formatted)) != 0 ||
                           cli_finish_response(state) != 0))
        rc = CAI_ERR_TRANSPORT;
      free(formatted);
      return rc;
    }
  }
  if (event->type == CAI_AGENT_EVENT_POLICY_DIAGNOSTIC_STARTED) {
    if (!state->batch &&
        (cli_finish_response(state) != 0 || cli_finish_reasoning(state) != 0))
      return CAI_ERR_TRANSPORT;
    cai_cli_response_boundary(&state->final_response);
    cai_cli_reasoning_reset(&state->reasoning);
    if (!state->batch && cli_prompt(state, event->data) != 0)
      return CAI_ERR_TRANSPORT;
  }
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
    if (state->batch && state->batch_pending_turns > 0U &&
        (event->type == CAI_AGENT_EVENT_RUN_COMPLETED ||
         event->type == CAI_AGENT_EVENT_RUN_FAILED ||
         event->type == CAI_AGENT_EVENT_RUN_CANCELLED))
      state->batch_pending_turns--;
    if (event->type == CAI_AGENT_EVENT_GOAL_CHANGED &&
        state->goal_starts_pending > 0U && event->data != NULL &&
        ((event->data_length == 8U &&
          memcmp(event->data, "replaced", 8U) == 0) ||
         (event->data_length == 7U && memcmp(event->data, "resumed", 7U) == 0)))
      state->goal_starts_pending--;
    if (event->type == CAI_AGENT_EVENT_RUN_COMPLETED)
      state->awaiting_turn = 0;
    if (event->type == CAI_AGENT_EVENT_RUN_FAILED ||
        event->type == CAI_AGENT_EVENT_RUN_CANCELLED) {
      state->awaiting_turn = 0;
      state->automation_failed = 1;
      state->goal_starts_pending = 0U;
      cai_app_bridge_outcome(state->bridge,
                             event->type == CAI_AGENT_EVENT_RUN_CANCELLED
                                 ? CAI_APP_CANCELLED
                                 : CAI_APP_FAILED);
      if (event->type == CAI_AGENT_EVENT_RUN_FAILED && event->data != NULL) {
        cai_error failed;
        char *message = malloc(event->data_length + 1U);
        cai_error_init(&failed);
        if (message == NULL)
          return cai_cli_error(error, CAI_ERR_NOMEM, "copy runtime failure");
        memcpy(message, event->data, event->data_length);
        message[event->data_length] = '\0';
        failed.code = CAI_ERR_TRANSPORT;
        failed.message = message;
        cai_app_bridge_error(state->bridge, &failed);
        cai_error_cleanup(&failed);
      }
    }
    if (event->type == CAI_AGENT_EVENT_RUN_STARTED) {
      state->has_review_metrics = 0;
      cai_app_bridge_outcome(state->bridge, CAI_APP_RUNNING);
      if (state->batch && state->batch_pending_turns > 0U) {
        state->awaiting_turn = 1;
        state->automation_failed = 0;
      }
      state->show_welcome = 0;
      cai_cli_reasoning_reset(&state->reasoning);
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
  if (event->type == CAI_AGENT_EVENT_RESPONSE_COMPLETED) {
    int rc = cli_finish_response(state) == 0 && cli_finish_reasoning(state) == 0
                 ? CAI_OK
                 : CAI_ERR_TRANSPORT;
    cai_cli_reasoning_end(&state->reasoning);
    return rc;
  }
  if ((event->type >= CAI_AGENT_EVENT_TOOL_CALL_STARTED &&
       event->type <= CAI_AGENT_EVENT_TOOL_CALL_FAILED) ||
      (event->type >= CAI_AGENT_EVENT_TERMINAL_COMMAND_STARTED &&
       event->type <= CAI_AGENT_EVENT_TERMINAL_COMMAND_CANCELLED) ||
      event->type == CAI_AGENT_EVENT_TOOL_PATCH ||
      event->type == CAI_AGENT_EVENT_TOOL_PATCH_LOCATIONS ||
      event->type == CAI_AGENT_EVENT_RUN_FAILED ||
      event->type == CAI_AGENT_EVENT_RUN_CANCELLED) {
    if (cli_finish_response(state) != 0 || cli_finish_reasoning(state) != 0)
      return CAI_ERR_TRANSPORT;
    {
      int width = cli_render_width();
      int rc = cai_tool_display_set_width(state->tool_display,
                                          (size_t)(width < 32     ? 0
                                                   : width > 4096 ? 4096
                                                                  : width),
                                          error);
      return rc == CAI_OK
                 ? cai_tool_display_event(state->tool_display, event, error)
                 : rc;
    }
  } else if (event->type == CAI_AGENT_EVENT_REASONING_SUMMARY_STARTED) {
    if (cli_finish_reasoning(state) != 0)
      return CAI_ERR_TRANSPORT;
    cai_cli_reasoning_end(&state->reasoning);
  } else if (event->type == CAI_AGENT_EVENT_REASONING_SUMMARY &&
             event->data != NULL && event->data_length > 0U) {
    if (cai_cli_reasoning_feed(&state->reasoning, event->data,
                               event->data_length, cli_reasoning_body,
                               state) != 0)
      return CAI_ERR_TRANSPORT;
  }
  return CAI_OK;
}

static int cli_review_event(void *context, const cai_agent_runtime_event *event,
                            cai_error *error) {
  cai_agent_runtime_event forwarded = *event;
  forwarded.parent_tool_call_id = "/review";
  forwarded.subagent_name = "review";
  return cli_event(context, &forwarded, error);
}

static int cli_notice(void *context, const char *text) {
  cli_state *state = (cli_state *)context;
  int rc;
  if (cli_finish_response(state) != 0 || cli_finish_reasoning(state) != 0 ||
      cli_geometry(state) != 0)
    return -1;
  /* Keep one blank row around each notice; the renderer supplies its newline.
   */
  while (state->feed_breaks < 2)
    if (cli_sink(state, "\n", 1U) != 0)
      return -1;
  if (cli_literal_prepare(state, 1) != 0)
    return -1;
  state->notice_open = 1;
  state->notice_first_row = 1;
  state->row_line_start = 1;
  state->row_ansi = 0;
  if (strncmp(text, "* ", 2U) == 0)
    text += 2;
  rc = cai_cli_literal_render(state->notice_renderer, text);
  state->notice_open = 0;
  if (rc != 0)
    return -1;
  while (state->feed_breaks < 2)
    if (cli_sink(state, "\n", 1U) != 0)
      return -1;
  return 0;
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
  cai_agent_runtime *display_runtime =
      state->review_runtime != NULL ? state->review_runtime : state->runtime;
  cai_agent_run_state run_state;
  cai_agent_runtime_settings settings;
  cai_agent_goal_snapshot goal;
  cai_agent_runtime_metrics metrics;
  const cai_agent_runtime_metrics *turn_metrics;
  cai_error optional_error;
  struct itimerspec timer_spec;
  char turn_message[640];
  double context_percent;
  int has_context;
  const char *model;
  const char *effort;
  const char *tier;
  const char *reasoning_title;
  int busy;
  int rc;
  rc = cai_app_bridge_publish(state->bridge, state->runtime, error);
  if (rc != CAI_OK)
    return rc;
  if (!state->interactive) {
    return CAI_OK;
  }
  rc = cai_agent_runtime_state(display_runtime, &run_state, error);
  if (rc != CAI_OK) {
    return rc;
  }
  model = state->options.model;
  effort = state->options.reasoning_effort;
  tier = state->options.service_tier;
  memset(&settings, 0, sizeof(settings));
  cai_error_init(&optional_error);
  if (cai_agent_runtime_get_settings(display_runtime, &settings, NULL,
                                     &optional_error) == CAI_OK) {
    if (settings.model != NULL) {
      model = settings.model;
    }
    if (settings.reasoning_effort != NULL) {
      effort = settings.reasoning_effort;
    }
    tier = settings.service_tier;
  }
  cai_error_cleanup(&optional_error);
  rc = cai_agent_runtime_context_percent(display_runtime, &context_percent,
                                         &has_context, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_agent_runtime_get_metrics(display_runtime, &metrics, error);
  if (rc != CAI_OK)
    return rc;
  busy = metrics.turn_active || run_state == CAI_AGENT_SAMPLING ||
         run_state == CAI_AGENT_DISPATCHING_TOOL;
  /* Softline resets its animation clock/frame on every setter call, even
   * when the value is unchanged. Only a turn boundary changes this state. */
  if (state->was_busy != busy) {
    if (sl_set_status_busy(state->sl, busy) != SL_OK ||
        sl_set_status_spinner(state->sl, busy) != SL_OK)
      return CAI_ERR_TRANSPORT;
    state->was_busy = busy;
    if (!busy) {
      cai_cli_status_refresh_branch(&state->status, state->workspace);
      cli_refresh_quota(state, 0);
    }
  }
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
  turn_metrics = state->review_runtime == NULL && state->has_review_metrics &&
                         !metrics.turn_active
                     ? &state->last_review_metrics
                     : &metrics;
  if (cai_cli_reasoning_title(&state->reasoning, &reasoning_title) != 0 ||
      cai_cli_turn_status_message(turn_message, sizeof(turn_message),
                                  reasoning_title, turn_metrics) != 0)
    return CAI_ERR_TRANSPORT;
  if (state->show_welcome && !metrics.turn_active)
    strcpy(turn_message, cli_welcome);
  rc = cai_agent_runtime_get_goal(state->runtime, &goal, error);
  if (rc != CAI_OK) {
    return rc;
  }
  cai_cli_status_build(&state->status, model, effort, tier, context_percent,
                       has_context, state->auth != NULL ? &state->quota : NULL,
                       metrics.session_usage.estimated_spend_usd,
                       metrics.session_usage.estimated_spend_eur,
                       state->options.provider, &goal);
  if (sl_set_status_message(state->sl, turn_message) != SL_OK ||
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

/* Queue delivery is owned by the UI, not by a consumer thread. Approved
 * injected prompts remain editable until a normal turn can begin. */
static int cli_messages_process(cli_state *state, cai_error *error) {
  cai_agent_runtime_metrics metrics;
  cai_agent_run_state run_state;
  sl_prompt_queue_entry_info_t info;
  size_t index;
  char *text;
  int rc;
  if (state->exit_requested || state->batch || state->runtime == NULL)
    return CAI_OK;
  rc = cai_cli_messages_sync(state->messages, state->sl, error);
  if (rc != CAI_OK)
    return rc;
  rc = cai_agent_runtime_get_metrics(state->runtime, &metrics, error);
  if (rc == CAI_OK)
    rc = cai_agent_runtime_state(state->runtime, &run_state, error);
  if (rc != CAI_OK || state->review_runtime != NULL || metrics.turn_active ||
      state->awaiting_turn || state->shell.pid > 0 ||
      run_state == CAI_AGENT_SAMPLING ||
      run_state == CAI_AGENT_DISPATCHING_TOOL)
    return rc;
  for (index = 0U; index < sl_prompt_queue_count(state->sl); index++) {
    if (sl_prompt_queue_info(state->sl, index, &info) != SL_OK)
      return cai_cli_error(error, CAI_ERR_TRANSPORT, "inspect queued prompt");
    if (info.held || info.mode != SL_PROMPT_QUEUE_MODE_QUEUED)
      continue;
    text = NULL;
    if (sl_prompt_queue_take(state->sl, index, &text) != SL_OK)
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "take approved queued prompt");
    rc = cli_prompt(state, text) == 0
             ? cai_agent_runtime_submit_queued(state->runtime, text, error)
             : CAI_ERR_TRANSPORT;
    if (rc == CAI_OK) {
      state->awaiting_turn = 1;
      rc = cai_cli_messages_consumed(state->messages, info.id, error);
    } else {
      sl_prompt_id_t replacement;
      if (sl_prompt_queue_insert(state->sl, index, text, 0, &replacement) ==
          SL_OK)
        cai_cli_messages_rebind(state->messages, info.id, replacement);
    }
    sl_free_string(state->sl, text);
    return rc;
  }
  return CAI_OK;
}
static int cli_message_wakeup(sl_t *sl, const sl_watch_event_t *event,
                              void *context) {
  cli_state *state = context;
  cai_error error;
  int rc;
  (void)sl;
  (void)event;
  cai_error_init(&error);
  rc = cli_messages_process(state, &error);
  if (rc != CAI_OK)
    cli_print_error(state, "receive agent message", &error);
  cai_error_cleanup(&error);
  return rc == CAI_OK ? SL_OK : SL_ERROR_IO;
}
static void cli_message_idle(sl_t *sl, void *context) {
  (void)cli_message_wakeup(sl, NULL, context);
}

static int cli_wakeup(sl_t *sl, const sl_watch_event_t *event, void *context) {
  cli_state *state;
  cai_error error;
  int rc;
  (void)sl;
  (void)event;
  state = (cli_state *)context;
  cai_error_init(&error);
  rc = cli_geometry(state) == 0 ? cli_pump(state, 0L, &error)
                                : CAI_ERR_TRANSPORT;
  if (rc == CAI_OK)
    rc = cli_messages_process(state, &error);
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
  if (state->application != NULL &&
      state->application->replay_callback != NULL) {
    int observed = state->application->replay_callback(
        state->application->replay_context, event, error);
    if (observed != CAI_OK) {
      if (error->code == CAI_OK)
        cai_cli_error(error, observed, "application replay observer failed");
      cai_app_bridge_fail(state->bridge, error);
      return observed;
    }
  }
  if (event->data != NULL &&
      strcmp(event->type, "user_shell_output_tail") == 0) {
    pslog_field fields[2];
    fields[0] = pslog_bool("replay", 1);
    fields[1] = pslog_str("prompt_kind", "shell_output");
    cai_cli_log_message(state->log.logger, PSLOG_LEVEL_INFO,
                        "user_shell_output_tail", "user", event->data,
                        strlen(event->data), fields, 2U);
    return state->batch ? CAI_OK
                        : cai_cli_shell_plain(&state->shell, event->data,
                                              strlen(event->data), 1,
                                              cli_shell_literal, state, error);
  }
  if (!state->batch && event->data != NULL &&
      strcmp(event->type, "user_shell_started") == 0) {
    char command[8194];
    if (strlen(event->data) > 8192U)
      return cai_cli_error(error, CAI_ERR_PROTOCOL,
                           "saved shell command is too long");
    snprintf(command, sizeof(command), "!%s", event->data);
    if (cai_cli_shell_plain(&state->shell, NULL, 0U, 1, cli_shell_literal,
                            state, error) != CAI_OK)
      return CAI_ERR_TRANSPORT;
    if (cli_flush_command(state, 1) != 0)
      return CAI_ERR_TRANSPORT;
    cli_command_input(state, command);
    return cli_flush_command(state, 1) == 0 ? CAI_OK : CAI_ERR_TRANSPORT;
  }
  if (event->data != NULL &&
      strcmp(event->type, "user_shell_output_hex") == 0) {
    static const char hex[] = "0123456789abcdef";
    char bytes[4096];
    size_t i, n = strlen(event->data);
    pslog_field fields[2];
    if (n > 8192U || n % 2U != 0U)
      return cai_cli_error(error, CAI_ERR_PROTOCOL,
                           "invalid saved shell output");
    for (i = 0U; i < n; i += 2U) {
      const char *a = strchr(hex, event->data[i]);
      const char *b = strchr(hex, event->data[i + 1U]);
      if (a == NULL || b == NULL)
        return cai_cli_error(error, CAI_ERR_PROTOCOL,
                             "invalid saved shell output byte");
      bytes[i / 2U] = (char)(((a - hex) << 4) | (b - hex));
    }
    fields[0] = pslog_bool("replay", 1);
    fields[1] = pslog_str("prompt_kind", "shell_output");
    cai_cli_log_message(state->log.logger, PSLOG_LEVEL_INFO,
                        "user_shell_output", "user", bytes, n / 2U, fields, 2U);
    return state->batch ? CAI_OK
                        : cai_cli_shell_plain(&state->shell, bytes, n / 2U, 0,
                                              cli_shell_literal, state, error);
  }
  if (!state->batch && (strcmp(event->type, "user_shell_completed") == 0 ||
                        strcmp(event->type, "user_shell_unshared") == 0)) {
    int rc = cai_cli_shell_plain(&state->shell, NULL, 0U, 1, cli_shell_literal,
                                 state, error);
    return rc != CAI_OK ? rc
           : cli_write(state, "Shell command finished.") == 0
               ? CAI_OK
               : CAI_ERR_TRANSPORT;
  }
  if (event->data != NULL &&
      (strcmp(event->type, "policy_diagnostic") == 0 ||
       strcmp(event->type, "turn_submitted") == 0 ||
       strcmp(event->type, "turn_queued") == 0 ||
       strcmp(event->type, "steering_queued") == 0 ||
       strcmp(event->type, "assistant_text_delta") == 0)) {
    pslog_field fields[2];
    int assistant = strcmp(event->type, "assistant_text_delta") == 0;
    fields[0] = pslog_bool("replay", 1);
    fields[1] = pslog_str(
        "prompt_kind",
        strcmp(event->type, "policy_diagnostic") == 0 ? "policy_diagnostic"
        : strcmp(event->type, "steering_queued") == 0 ? "steering"
        : strcmp(event->type, "turn_queued") == 0     ? "queued"
                                                      : "normal");
    cai_cli_log_message(state->log.logger, PSLOG_LEVEL_INFO, event->type,
                        assistant ? "assistant" : "user", event->data,
                        strlen(event->data), fields, 2U);
  }
  if (state->batch)
    return CAI_OK;
  if ((strcmp(event->type, "policy_diagnostic") == 0 ||
       strcmp(event->type, "turn_submitted") == 0 ||
       strcmp(event->type, "turn_queued") == 0 ||
       strcmp(event->type, "steering_queued") == 0) &&
      event->data != NULL) {
    return cli_prompt(state, event->data) == 0 ? CAI_OK : CAI_ERR_TRANSPORT;
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

static int cli_replay_begin(void *context, const char *id, cai_error *error) {
  cli_state *state = context;
  return cai_cli_log_session(&state->log, id, error);
}

static int cli_quota_wakeup(sl_t *sl, const sl_watch_event_t *event,
                            void *context) {
  cli_state *state = context;
  cai_error error;
  int rc = CAI_OK;
  (void)sl;
  (void)event;
  if (!cai_cli_quota_take(&state->quota_refresh, &state->quota))
    return SL_OK;
  cai_error_init(&error);
  if (state->runtime != NULL)
    rc = cli_sync_status(state, &error);
  if (rc != CAI_OK)
    cli_print_error(state, "quota status", &error);
  cai_error_cleanup(&error);
  return rc == CAI_OK ? SL_OK : SL_ERROR_IO;
}

static int cli_register_agent_tools(void *context, cai_agent *agent,
                                    cai_error *error) {
  cli_state *state = context;
  int rc = CAI_OK;
  if (state->messages != NULL)
    rc = cai_cli_messages_register(state->messages, agent, error);
  if (rc == CAI_OK && state->application != NULL &&
      state->application->register_agent_tools != NULL)
    rc = state->application->register_agent_tools(
        state->application->register_agent_tools_context, agent, error);
  return rc;
}

static int cli_open_runtime(cli_state *state, const char *resume_id,
                            int new_session, cai_error *error) {
  cai_agent_runtime_config config;
  cai_agent_preset preset;
  cai_skill_config skills;
  cai_terminal_tool_config terminal;
  int wakeup_fd;
  int rc;

  state->show_welcome = 1;
  state->has_review_metrics = 0;
  cai_agent_runtime_config_init(&config);
  if (state->configuration != NULL &&
      state->configuration->apply_runtime(state->configuration, &config,
                                          error) != CAI_OK)
    return error->code;
  memset(&terminal, 0, sizeof(terminal));
  if (state->configuration != NULL &&
      state->configuration->apply_terminal(state->configuration, &terminal,
                                           error) != CAI_OK)
    return error->code;
  config.terminal_tool_config = &terminal;
  cai_cli_apply_runtime_options(&state->options, &config, &preset, &skills);
  if (state->options.messaging && !state->options.review) {
    rc = cai_cli_messages_new(&state->pouch,
                              state->options.message_ack_timeout_ms,
                              &state->messages, error);
    if (rc != CAI_OK)
      return rc;
  }
  if (state->messages != NULL ||
      (state->application != NULL &&
       state->application->register_agent_tools != NULL)) {
    config.register_agent_tools = cli_register_agent_tools;
    config.register_agent_tools_context = state;
  }
  if (config.enable_todo) {
    config.todo.store = cai_cli_pouch_todo_callbacks();
    config.todo.store_context = &state->pouch;
  }
  config.logger = state->log.logger;
  config.workspace_directory = state->workspace;
  config.image_directory =
      state->clipboard_directory[0] != '\0' ? state->clipboard_directory : NULL;
  config.session_store = &state->store;
  config.resume_latest =
      !state->options.review && !new_session && resume_id == NULL;
  config.resume_session_id = state->options.review ? NULL : resume_id;
  config.record_transcript = 1;
  config.event_callback = cli_event;
  config.review_event_callback = cli_review_event;
  config.review_event_context = state;
  config.event_context = state;
  if (!state->options.review) {
    state->pouch.replay_begin = cli_replay_begin;
    config.replay_callback = cli_replay_event;
    config.replay_context = state;
    state->pouch.replay_context = state;
    state->pouch.replay_scope = state->workspace;
  }
  if (!state->mcp_initialized) {
    rc = cai_cli_mcp_open(&state->mcp, state->configuration,
                          state->options.mcp && !state->options.review,
                          state->log.logger, error);
    if (rc != CAI_OK)
      return rc;
    state->mcp_initialized = 1;
  }
  config.mcp_clients = state->mcp.clients;
  config.mcp_client_count = state->mcp.client_count;
  rc = cai_agent_runtime_open(state->client, &config, &state->runtime, error);
  state->pouch.replay_begin = NULL;
  state->pouch.replay_event = NULL;
  state->pouch.replay_context = NULL;
  state->pouch.replay_scope = NULL;
  if (rc != CAI_OK) {
    return rc;
  }
  if (state->options.board_explicit && state->options.todo) {
    rc = cai_agent_runtime_select_board(state->runtime, state->options.board, 1,
                                        error);
    if (rc != CAI_OK)
      return rc;
    state->options.board_explicit = 0;
  }
  if (state->messages != NULL) {
    rc = cai_cli_messages_start(
        state->messages, cai_agent_runtime_session_id(state->runtime),
        state->workspace, state->options.message_queue, error);
    if (rc != CAI_OK)
      return rc;
    if (state->interactive) {
      if (sl_set_auto_approval(state->sl, state->options.enable_auto_enqueue) !=
              SL_OK ||
          sl_set_prompt_queue_delivery(
              state->sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) != SL_OK ||
          sl_set_idle_callback(state->sl, cli_message_idle, state) != SL_OK ||
          sl_watch_add(state->sl, cai_cli_messages_fd(state->messages),
                       SL_WATCH_READ | SL_WATCH_ERROR, cli_message_wakeup,
                       state, &state->message_watch_id) != SL_OK)
        return cai_cli_error(error, CAI_ERR_TRANSPORT,
                             "watch agent message service");
      state->has_message_watch = 1;
    }
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
  rc = cli_finish_response(state) == 0 ? CAI_OK : CAI_ERR_TRANSPORT;
  if (rc == CAI_OK) {
    rc = cli_sync_status(state, error);
  }
  if (rc == CAI_OK && state->interactive && state->bridge != NULL) {
    if (sl_set_auto_approval(state->sl, state->options.enable_auto_enqueue) !=
            SL_OK ||
        sl_set_prompt_queue_delivery(
            state->sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) != SL_OK ||
        sl_set_idle_callback(state->sl, cli_message_idle, state) != SL_OK)
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "configure host prompt queue");
  }
  return rc;
}

static int cli_prepare_clipboard(cli_state *state, cai_error *error) {
  char cache[PATH_MAX];
  char path[PATH_MAX];
  struct stat status;
  int length;
  int rc;

  rc = cai_cli_cache_directory(cache, sizeof(cache), error);
  if (rc != CAI_OK)
    return rc;
  length = snprintf(path, sizeof(path), "%s/clipboard", cache);
  if (length < 0 || (size_t)length >= sizeof(path))
    return cai_cli_error(error, CAI_ERR_INVALID, "clipboard path is too long");
  if (mkdir(path, 0700) != 0 && errno != EEXIST)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "cannot create cai clipboard directory");
  if (lstat(path, &status) != 0 || !S_ISDIR(status.st_mode) ||
      status.st_uid != geteuid() || (status.st_mode & 077U) != 0U)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "cai clipboard directory must be owned by you with "
                         "mode 0700 and must not be a symlink");
  if (realpath(path, state->clipboard_directory) == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "cannot resolve cai clipboard directory");
  return CAI_OK;
}

static void cli_close_runtime(cli_state *state) {
  if (state->has_review_watch) {
    (void)sl_watch_remove(state->sl, state->review_watch_id);
    state->has_review_watch = 0;
  }
  if (state->review_runtime != NULL) {
    /* Abandoning the UI preserves the parent's durable review-pending marker
     * for recovery, just as an interrupted reviewer does. Close the owned
     * child before its parent; never start queued work during shutdown. */
    (void)cai_agent_runtime_cancel_turn(state->review_runtime, NULL);
    cai_agent_runtime_close(state->review_runtime);
    state->review_runtime = NULL;
  }
  (void)cai_app_bridge_publish(state->bridge, NULL, NULL);
  if (state->has_message_watch) {
    (void)sl_watch_remove(state->sl, state->message_watch_id);
    state->has_message_watch = 0;
  }
  if (state->messages != NULL && state->sl != NULL) {
    (void)sl_set_idle_callback(state->sl, NULL, NULL);
    cai_cli_messages_detach(state->messages, state->sl);
  }
  cai_cli_messages_stop(state->messages);
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
  cai_cli_messages_close(state->messages);
  state->messages = NULL;
  state->goal_starts_pending = 0U;
  cai_cli_reasoning_reset(&state->reasoning);
}

static int cli_list_write(void *context, const void *bytes, size_t count,
                          cai_error *error) {
  cli_state *state;
  state = (cli_state *)context;
  if (cli_flush_command(state, 1) != 0 || cli_sink(state, bytes, count) != 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "write session rows");
  return CAI_OK;
}

static int cli_load_sessions(cli_state *state, cai_error *error) {
  free(state->sessions);
  state->sessions = NULL;
  state->session_count = 0U;
  return cai_cli_pouch_list(&state->pouch, state->workspace, &state->sessions,
                            &state->session_count, error);
}

static int cli_list_sessions(cli_state *state, cai_error *error) {
  cai_sink_callbacks callbacks;
  cai_sink *sink;
  int rc = cli_load_sessions(state, error);
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
  if (state->shell.pid > 0 || run_state == CAI_AGENT_SAMPLING ||
      run_state == CAI_AGENT_DISPATCHING_TOOL) {
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "Wait for the current turn to finish.");
  }
  if (cli_finish_response(state) != 0)
    return CAI_ERR_TRANSPORT;
  cli_close_runtime(state);
  if (cli_write(state, "\n--- session ---\n") != 0) {
    return CAI_ERR_TRANSPORT;
  }
  return cli_open_runtime(state, id, new_session, error);
}

static int cli_goal_command(cli_state *state, const char *arg,
                            cai_error *error) {
  static const char usage[] =
      "Use /goal [status|help|pause|resume|clear] or /goal new TEXT. "
      "Prefer asking the agent to create and manage goals.";
  cai_agent_goal_snapshot goal;
  cai_agent_run_state run_state;
  pslog_field fields[2];
  char metrics[256];
  const char *objective = NULL;
  const char *action;
  int rc;

  arg += strspn(arg, " \t\r\n");
  if (strcmp(arg, "help") == 0) {
    static const char *const help[] = {
        "## Goal commands\n\n"
        "Prefer asking the agent to create and manage goals. These explicit "
        "controls apply at the next safe model/tool boundary.\n\n"
        "| Command | Action |\n| --- | --- |\n"
        "| `/goal`, `/goal status` | Show the current durable goal. |\n"
        "| `/goal new TEXT` | Start a fresh, unbudgeted goal, replacing the "
        "current goal without confirmation, and pursue it. |\n",
        "| `/goal pause` | Pause pursuit after the current request/tool round. "
        "|\n"
        "| `/goal resume` | Resume pursuit of a paused or limited goal. |\n"
        "| `/goal clear` | Remove goal tracking; the current user turn "
        "continues. |\n"
        "| `/goal help` | Show this help. |\n"};
    size_t i;
    for (i = 0U; i < sizeof(help) / sizeof(help[0]); i++)
      if (cli_text(state, help[i], strlen(help[i])) != 0)
        return CAI_ERR_TRANSPORT;
    return cli_finish_response(state) == 0 ? CAI_OK : CAI_ERR_TRANSPORT;
  }
  if (*arg == '\0' || strcmp(arg, "status") == 0) {
    rc = cai_agent_runtime_get_goal(state->runtime, &goal, error);
    if (rc != CAI_OK)
      return rc;
    if (!goal.has_goal)
      return cli_write(state, "No goal. Use /goal new TEXT, or ask the agent "
                              "to create and manage one.\n") == 0
                 ? CAI_OK
                 : CAI_ERR_TRANSPORT;
    snprintf(metrics, sizeof(metrics), "## Goal\n\nStatus: **%s**\n\n",
             goal.status);
    if (cli_text(state, metrics, strlen(metrics)) != 0 ||
        cli_text(state, goal.objective, strlen(goal.objective)) != 0)
      return CAI_ERR_TRANSPORT;
    snprintf(metrics, sizeof(metrics),
             "\n\nTokens used: %lld. Active time: %lld seconds.\n\n",
             goal.tokens_used, goal.elapsed_seconds);
    if (cli_text(state, metrics, strlen(metrics)) != 0)
      return CAI_ERR_TRANSPORT;
    if (goal.has_token_budget)
      snprintf(metrics, sizeof(metrics),
               "Token budget: %lld; remaining: %lld.\n", goal.token_budget,
               goal.remaining_tokens);
    else
      snprintf(metrics, sizeof(metrics), "Token budget: unbounded.\n");
    return cli_text(state, metrics, strlen(metrics)) == 0 &&
                   cli_finish_response(state) == 0
               ? CAI_OK
               : CAI_ERR_TRANSPORT;
  }
  if (strncmp(arg, "new", 3U) == 0 &&
      (arg[3] == '\0' || strchr(" \t\r\n", arg[3]) != NULL)) {
    objective = arg + 3U;
    objective += strspn(objective, " \t\r\n");
    if (*objective == '\0')
      return cai_cli_error(error, CAI_ERR_INVALID, usage);
    action = "new";
    rc = cai_agent_runtime_replace_goal(state->runtime, objective, error);
  } else if (strcmp(arg, "pause") == 0) {
    rc = cai_agent_runtime_get_goal(state->runtime, &goal, error);
    if (rc != CAI_OK)
      return rc;
    if (!goal.has_goal || strcmp(goal.status, "active") != 0)
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "only an active goal can pause");
    action = "pause";
    rc = cai_agent_runtime_pause_goal(state->runtime, error);
  } else if (strcmp(arg, "resume") == 0) {
    rc = cai_agent_runtime_get_goal(state->runtime, &goal, error);
    if (rc != CAI_OK)
      return rc;
    if (!goal.has_goal || (strcmp(goal.status, "paused") != 0 &&
                           strcmp(goal.status, "blocked") != 0 &&
                           strcmp(goal.status, "budget_limited") != 0 &&
                           strcmp(goal.status, "usage_limited") != 0))
      return cai_cli_error(error, CAI_ERR_INVALID, "goal is not resumable");
    action = "resume";
    rc = cai_agent_runtime_resume_goal(state->runtime, error);
  } else if (strcmp(arg, "clear") == 0) {
    action = "clear";
    rc = cai_agent_runtime_clear_goal(state->runtime, error);
  } else {
    return cai_cli_error(error, CAI_ERR_INVALID, usage);
  }
  if (rc != CAI_OK)
    return rc;
  if (objective != NULL || strcmp(action, "resume") == 0)
    state->goal_starts_pending++;
  fields[0] = pslog_str("command", action);
  fields[1] = pslog_str("prompt_kind", "goal_control");
  cai_cli_log_message(state->log.logger, PSLOG_LEVEL_INFO, "goal_control",
                      "user", arg, strlen(arg), fields, 2U);
  if (cli_write(state, "Goal control queued for the next safe model/tool "
                       "boundary.\n") != 0)
    return CAI_ERR_TRANSPORT;
  if (objective == NULL && strcmp(action, "resume") != 0) {
    state->auto_goal = 0;
    state->automation_done = 1;
    return CAI_OK;
  }
  state->auto_goal = 1;
  state->automation_done = 0;
  state->automation_failed = 0;
  rc = cai_agent_runtime_state(state->runtime, &run_state, error);
  if (rc != CAI_OK)
    return rc;
  if (run_state == CAI_AGENT_SAMPLING ||
      run_state == CAI_AGENT_DISPATCHING_TOOL) {
    state->awaiting_turn = 1;
    return CAI_OK;
  }
  return cli_submit_automation(
      state,
      objective != NULL ? objective : "Resume pursuing the current goal.",
      error);
}

static int cli_host_process(cli_state *state, cai_error *error) {
  cai_app_command *command;
  size_t count;
  int rc;
  for (count = 0U;
       count < 64U && (command = cai_app_bridge_take(state->bridge)) != NULL;
       count++) {
    cai_error applied;
    cai_error_init(&applied);
    rc = CAI_OK;
    if (state->exit_requested) {
      rc = cai_cli_error(&applied, CAI_ERR_INVALID, "application is stopping");
    } else if (command->type == CAI_APP_STOP) {
      state->host_stopped = state->exit_requested = 1;
      state->automation_done = 1;
      /* Signal active work before acknowledging STOP. An already idle turn
       * needs no cancellation. Close joins workers without draining events,
       * which could otherwise start an already queued next turn. */
      if (state->runtime != NULL) {
        rc = cai_agent_runtime_cancel_turn(state->runtime, &applied);
        if (rc == CAI_ERR_INVALID) {
          cai_error_cleanup(&applied);
          cai_error_init(&applied);
          rc = CAI_OK;
        }
      }
      cai_cli_messages_cancel_send(state->messages);
      if (state->review_runtime != NULL)
        (void)cai_agent_runtime_cancel_turn(state->review_runtime, NULL);
      if (state->sl != NULL)
        (void)sl_cancel(state->sl);
    } else if (state->runtime == NULL) {
      rc = cai_cli_error(&applied, CAI_ERR_INVALID,
                         "agent runtime is not ready");
    } else if (command->type == CAI_APP_QUEUE_PROMPT) {
      if (state->batch) {
        rc = cai_agent_runtime_submit_queued(state->runtime, command->text,
                                             &applied);
        if (rc == CAI_OK) {
          state->batch_pending_turns++;
          state->automation_done = 0;
        }
      } else {
        sl_prompt_id_t id;
        if (sl_inject_queued_prompt(state->sl, command->text, &id) != SL_OK)
          rc = cai_cli_error(&applied, CAI_ERR_TRANSPORT,
                             "inject host prompt into approval queue");
      }
    } else if (command->type == CAI_APP_STEER) {
      rc = cai_agent_runtime_submit_steering(state->runtime, command->text,
                                             &applied);
    } else if (command->type == CAI_APP_CANCEL_TURN) {
      rc = cai_agent_runtime_cancel_turn(state->review_runtime != NULL
                                             ? state->review_runtime
                                             : state->runtime,
                                         &applied);
      cai_cli_messages_cancel_send(state->messages);
    } else {
      char *goal_command = NULL;
      const char *action = command->type == CAI_APP_PAUSE_GOAL    ? "pause"
                           : command->type == CAI_APP_RESUME_GOAL ? "resume"
                           : command->type == CAI_APP_CLEAR_GOAL  ? "clear"
                                                                  : NULL;
      if (action == NULL) {
        size_t length = strlen(command->text);
        goal_command = malloc(length + 5U);
        if (goal_command == NULL)
          rc = cai_cli_error(&applied, CAI_ERR_NOMEM, "copy goal control");
        else {
          memcpy(goal_command, "new ", 4U);
          memcpy(goal_command + 4U, command->text, length + 1U);
          action = goal_command;
        }
      }
      if (rc == CAI_OK)
        rc = cli_goal_command(state, action, &applied);
      free(goal_command);
    }
    {
      pslog_field fields[3];
      int published =
          cai_app_bridge_publish(state->bridge, state->runtime, error);
      const char *message = rc == CAI_OK              ? "Host control accepted"
                            : applied.message != NULL ? applied.message
                                                      : cai_status_string(rc);
      fields[0] = pslog_u64("control_id", (pslog_uint64)command->id);
      fields[1] = pslog_i64("control", command->type);
      fields[2] = pslog_i64("code", rc);
      cai_cli_log_message(
          state->log.logger, rc == CAI_OK ? PSLOG_LEVEL_INFO : PSLOG_LEVEL_WARN,
          "host_control", "user", message, strlen(message), fields, 3U);
      cai_app_bridge_ack(state->bridge, command, rc, &applied);
      if (published != CAI_OK) {
        cai_error_cleanup(&applied);
        return published;
      }
    }
    cai_error_cleanup(&applied);
  }
  return cai_app_bridge_publish(state->bridge, state->runtime, error);
}

/* Both descriptors are level-triggered. Batch runs wake immediately for host
 * commands instead of waiting for the runtime's timed condition variable. */
static int cli_pump(cli_state *state, long timeout, cai_error *error) {
  struct pollfd descriptors[2];
  int fd, rc;
  if (state->bridge != NULL && timeout > 0L) {
    rc = cai_agent_runtime_wakeup_fd(state->runtime, &fd, error);
    if (rc != CAI_OK)
      return rc;
    memset(descriptors, 0, sizeof(descriptors));
    descriptors[0].fd = fd;
    descriptors[1].fd = state->bridge->fd;
    descriptors[0].events = descriptors[1].events = POLLIN;
    do {
      rc = poll(descriptors, 2U, (int)timeout);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0)
      return cai_cli_error(error, CAI_ERR_TRANSPORT, "wait for app events");
    timeout = 0L;
  }
  rc = cai_agent_runtime_pump(state->runtime, timeout, error);
  if (rc == CAI_OK)
    rc = cli_host_process(state, error);
  return rc;
}
static int cli_host_wakeup(sl_t *sl, const sl_watch_event_t *event,
                           void *context) {
  return cli_wakeup(sl, event, context);
}

static const char *const cli_reasoning_efforts[] = {
    "none", "minimal", "low", "medium", "high", "xhigh", "max", "ultra"};

static int cli_effort_known(const char *value) {
  size_t i;
  for (i = 0U;
       i < sizeof(cli_reasoning_efforts) / sizeof(cli_reasoning_efforts[0]);
       i++)
    if (strcmp(value, cli_reasoning_efforts[i]) == 0)
      return 1;
  return 0;
}

static int cli_tier_command(cli_state *state, const char *arguments, int fast,
                            cai_error *error) {
  cai_agent_runtime_settings effective, pending, requested;
  cai_agent_runtime_control_result result;
  cai_model_catalog *catalog = NULL;
  const cai_model_catalog_entry *entry;
  const char *choices[66], *current, *selected, *fast_tier = NULL;
  char buffer[128], message[512], *argument, *extra, *save = NULL;
  size_t count = 1U, i, j;
  int known = 0, listing, rc;
  pslog_field fields[2];
  if (strlen(arguments) >= sizeof(buffer))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "service-tier input is too long");
  strcpy(buffer, arguments);
  argument = strtok_r(buffer, " \t\r\n", &save);
  extra = strtok_r(NULL, " \t\r\n", &save);
  if (extra != NULL || (fast && argument != NULL))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "Usage: /tier [list | NAME] or /fast");
  rc = cai_agent_runtime_get_settings(state->runtime, &effective, &pending,
                                      error);
  if (rc != CAI_OK)
    return rc;
  current = pending.service_tier != NULL ? pending.service_tier
                                         : effective.service_tier;
  listing = argument != NULL && strcmp(argument, "list") == 0;
  choices[0] = "default";
  rc = cai_client_list_models(state->model_client,
                              CAI_MODEL_CATALOG_REFRESH_ONLINE_IF_UNCACHED,
                              &catalog, error);
  if (rc != CAI_OK) {
    cai_error_cleanup(error);
    cai_error_init(error);
  }
  entry = cai_model_catalog_find(
      catalog, pending.model != NULL ? pending.model : effective.model);
  if (entry != NULL && entry->service_tiers_known) {
    known = 1;
    for (i = 0U; i < entry->service_tier_count && count < 66U; i++) {
      for (j = 0U; j < count; j++)
        if (strcmp(choices[j], entry->service_tiers[i]) == 0)
          break;
      if (j == count)
        choices[count++] = entry->service_tiers[i];
    }
  } else if (strcmp(state->options.provider, "openai") == 0) {
    choices[count++] = "auto";
    choices[count++] = "priority";
    choices[count++] = "flex";
  }
  for (i = 0U; i < count; i++)
    if (strcmp(choices[i], "priority") == 0 ||
        strcmp(choices[i], "fast") == 0) {
      fast_tier = choices[i];
      break;
    }
  if (listing) {
    snprintf(message, sizeof(message),
             "## Service tiers\n\nSelected: `%s`\n\nPending: `%s`\n\n"
             "| Tier | Routing |\n| --- | --- |\n",
             effective.service_tier != NULL ? effective.service_tier
                                            : "unset (provider routing)",
             pending.service_tier != NULL ? pending.service_tier : "none");
    rc = cli_text(state, message, strlen(message)) == 0 ? CAI_OK
                                                        : CAI_ERR_TRANSPORT;
    for (i = 0U; rc == CAI_OK && i < count; i++) {
      snprintf(message, sizeof(message), "| `%s` | %s |\n", choices[i],
               i == 0U ? "Explicit normal routing"
               : known ? "Advertised by provider"
                       : "Request option; provider validates account/model "
                         "availability");
      if (cli_text(state, message, strlen(message)) != 0)
        rc = CAI_ERR_TRANSPORT;
    }
    if (rc == CAI_OK && !known && count == 1U) {
      static const char note[] =
          "\nTier metadata is unavailable. Explicit `/tier NAME` is "
          "forwarded to the provider; cycling and `/fast` require known "
          "choices.\n";
      if (cli_text(state, note, sizeof(note) - 1U) != 0)
        rc = CAI_ERR_TRANSPORT;
    }
    if (rc == CAI_OK && cli_finish_response(state) != 0)
      rc = CAI_ERR_TRANSPORT;
    cai_model_catalog_close(catalog);
    return rc;
  }
  selected = argument;
  if (fast) {
    if (current != NULL &&
        (strcmp(current, "priority") == 0 || strcmp(current, "fast") == 0))
      selected = "default";
    else
      selected = fast_tier;
  } else if (selected == NULL && count > 1U) {
    for (i = 0U; i < count; i++)
      if (current != NULL && strcmp(current, choices[i]) == 0)
        break;
    selected = choices[i < count ? (i + 1U) % count : 1U];
  }
  if (selected != NULL && strcmp(selected, "fast") == 0 && fast_tier != NULL)
    selected = fast_tier;
  if (selected == NULL) {
    cai_model_catalog_close(catalog);
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "no available tier choices; use /tier list or explicit /tier NAME");
  }
  if (known) {
    for (i = 0U; i < count; i++)
      if (strcmp(selected, choices[i]) == 0)
        break;
    if (i == count) {
      cai_model_catalog_close(catalog);
      return cai_cli_error(
          error, CAI_ERR_INVALID,
          "tier is not advertised for this model; use /tier list");
    }
  }
  memset(&requested, 0, sizeof(requested));
  requested.present = CAI_AGENT_RUNTIME_SETTING_SERVICE_TIER;
  requested.service_tier = selected;
  rc = cai_agent_runtime_update_settings(state->runtime, &requested, &result,
                                         error);
  if (rc == CAI_OK) {
    fields[0] = pslog_str("service_tier", selected);
    fields[1] = pslog_bool("applied", result.applied);
    state->log.logger->info(state->log.logger, "cai.service_tier.selected",
                            fields, 2U);
    snprintf(message, sizeof(message), "Service tier %s: `%s`.\n",
             result.applied ? "selected" : "queued for the next safe boundary",
             selected);
    if (cli_text(state, message, strlen(message)) != 0 ||
        cli_finish_response(state) != 0)
      rc = CAI_ERR_TRANSPORT;
  }
  cai_model_catalog_close(catalog);
  return rc;
}

static int cli_settings_command(cli_state *state, const char *arguments,
                                int reasoning_only, cai_error *error) {
  cai_agent_runtime_settings effective, pending, requested;
  cai_agent_runtime_control_result result;
  cai_model_catalog *catalog = NULL;
  const cai_model_catalog_entry *entry;
  pslog_field fields[3];
  char buffer[512], message[1024], target_model[257];
  char *first, *second, *extra, *save = NULL;
  const char *model, *effort;
  size_t i;
  int rc, listing;
  while (*arguments == ' ' || *arguments == '\t')
    arguments++;
  if (strlen(arguments) >= sizeof(buffer))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "model settings input is too long");
  strcpy(buffer, arguments);
  first = strtok_r(buffer, " \t\r\n", &save);
  second = strtok_r(NULL, " \t\r\n", &save);
  extra = strtok_r(NULL, " \t\r\n", &save);
  if (extra != NULL || (reasoning_only && second != NULL) ||
      (second != NULL && !cli_effort_known(second)))
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        reasoning_only ? "use /reasoning EFFORT (see /reasoning for choices)"
                       : "use /model MODEL [EFFORT] or /model EFFORT (see "
                         "/reasoning for choices)");
  memset(&requested, 0, sizeof(requested));
  if (first != NULL) {
    if (reasoning_only || (second == NULL && cli_effort_known(first))) {
      if (!cli_effort_known(first))
        return cai_cli_error(
            error, CAI_ERR_INVALID,
            "unknown reasoning effort; use /reasoning for choices");
      requested.present = CAI_AGENT_RUNTIME_SETTING_REASONING_EFFORT;
      requested.reasoning_effort = first;
    } else {
      if (strlen(first) > 256U)
        return cai_cli_error(error, CAI_ERR_INVALID,
                             "model ID must contain at most 256 bytes");
      requested.present = CAI_AGENT_RUNTIME_SETTING_MODEL;
      requested.model = first;
      if (second != NULL) {
        requested.present |= CAI_AGENT_RUNTIME_SETTING_REASONING_EFFORT;
        requested.reasoning_effort = second;
      }
    }
  }
  rc = cai_agent_runtime_get_settings(state->runtime, &effective, &pending,
                                      error);
  if (rc != CAI_OK)
    return rc;
  listing = first == NULL;
  model = requested.model != NULL ? requested.model
          : pending.model != NULL ? pending.model
                                  : effective.model;
  if (snprintf(target_model, sizeof(target_model), "%s", model) >=
      (int)sizeof(target_model))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "current model ID is too long");
  if (listing) {
    snprintf(message, sizeof(message),
             "## %s\n\nCurrent: `%s`\n\nReasoning effort: `%s`\n\n%s%s%s%s%s%s",
             reasoning_only ? "Reasoning" : "Model", effective.model,
             effective.reasoning_effort != NULL ? effective.reasoning_effort
                                                : "provider default",
             pending.model != NULL ? "Pending: `" : "",
             pending.model != NULL ? pending.model : "",
             pending.model != NULL ? "`\n\n" : "",
             pending.reasoning_effort != NULL ? "Pending reasoning: `" : "",
             pending.reasoning_effort != NULL ? pending.reasoning_effort : "",
             pending.reasoning_effort != NULL ? "`\n\n" : "");
    if (!reasoning_only && strcmp(state->options.provider, "openrouter") == 0) {
      static const char note[] =
          "OpenRouter's large catalog is not listed here. "
          "Use `/model MODEL [EFFORT]` or `/reasoning EFFORT`.\n";
      if (cli_text(state, message, strlen(message)) != 0 ||
          cli_text(state, note, strlen(note)) != 0)
        return CAI_ERR_TRANSPORT;
      return cli_finish_response(state) == 0 ? CAI_OK : CAI_ERR_TRANSPORT;
    }
  }
  /* A separate discovery client avoids touching the agent worker's transport.
   */
  if (listing || requested.reasoning_effort != NULL) {
    rc = cai_client_list_models(
        state->model_client,
        listing ? CAI_MODEL_CATALOG_REFRESH_ONLINE
                : CAI_MODEL_CATALOG_REFRESH_ONLINE_IF_UNCACHED,
        &catalog, error);
    if (rc != CAI_OK) {
      if (listing && cli_flush_command(state, 0) != 0)
        return CAI_ERR_TRANSPORT;
      cai_error_cleanup(error);
      cai_error_init(error);
      /* An explicit retry may fail while a previous successful catalog is
       * still useful. Offline lookup performs no further network request. */
      (void)cai_client_list_models(state->model_client,
                                   CAI_MODEL_CATALOG_REFRESH_OFFLINE, &catalog,
                                   error);
      cai_error_cleanup(error);
      cai_error_init(error);
    }
  }
  if (listing && cli_text(state, message, strlen(message)) != 0) {
    cai_model_catalog_close(catalog);
    return CAI_ERR_TRANSPORT;
  }
  entry = cai_model_catalog_find(catalog, target_model);
  if (!listing && requested.reasoning_effort != NULL && entry != NULL &&
      entry->reasoning_efforts_known) {
    for (i = 0U; i < entry->reasoning_effort_count; i++)
      if (strcmp(requested.reasoning_effort, entry->reasoning_efforts[i]) == 0)
        break;
    if (i == entry->reasoning_effort_count) {
      snprintf(message, sizeof(message),
               "Model %s does not advertise reasoning effort %s; use "
               "/reasoning for choices.",
               target_model, requested.reasoning_effort);
      cai_model_catalog_close(catalog);
      return cai_cli_error(error, CAI_ERR_INVALID, message);
    }
  }
  if (listing && reasoning_only) {
    int known = entry != NULL && entry->reasoning_efforts_known;
    size_t count = known ? entry->reasoning_effort_count
                         : sizeof(cli_reasoning_efforts) /
                               sizeof(cli_reasoning_efforts[0]);
    const char *const *choices =
        known ? (const char *const *)entry->reasoning_efforts
              : cli_reasoning_efforts;
    const char *note =
        known ? "Provider-advertised choices:\n\n"
              : "Model-specific reasoning choices are unavailable. CAI accepts "
                "the "
                "following values; support depends on the provider/model.\n\n";
    snprintf(message, sizeof(message), "Choices for `%s`:\n\n", target_model);
    rc = cli_text(state, message, strlen(message)) == 0 ? CAI_OK
                                                        : CAI_ERR_TRANSPORT;
    if (rc == CAI_OK)
      rc =
          cli_text(state, note, strlen(note)) == 0 ? CAI_OK : CAI_ERR_TRANSPORT;
    if (rc == CAI_OK && count == 0U)
      rc =
          cli_text(
              state, "This model advertises no reasoning-effort choices.\n",
              strlen("This model advertises no reasoning-effort choices.\n")) ==
                  0
              ? CAI_OK
              : CAI_ERR_TRANSPORT;
    for (i = 0U; rc == CAI_OK && i < count; i++) {
      snprintf(message, sizeof(message), "- `%s`%s\n", choices[i],
               entry != NULL && entry->default_reasoning_effort != NULL &&
                       strcmp(choices[i], entry->default_reasoning_effort) == 0
                   ? " (provider default)"
                   : "");
      if (cli_text(state, message, strlen(message)) != 0)
        rc = CAI_ERR_TRANSPORT;
    }
  } else if (listing) {
    if (catalog == NULL) {
      const char *note = "Model list unavailable. Use /model to retry, or "
                         "/model MODEL [EFFORT] to select directly.\n";
      rc =
          cli_text(state, note, strlen(note)) == 0 ? CAI_OK : CAI_ERR_TRANSPORT;
    } else {
      size_t limit = catalog->count;
      if (strcmp(state->options.provider, "chatgpt") != 0 && limit > 50U)
        limit = 50U;
      rc = catalog->is_stale
               ? (cli_text(state,
                           "Using cached model catalog; /model retries "
                           "discovery.\n\n",
                           strlen("Using cached model catalog; /model retries "
                                  "discovery.\n\n")) == 0
                      ? CAI_OK
                      : CAI_ERR_TRANSPORT)
               : CAI_OK;
      for (i = 0U; rc == CAI_OK && i < limit; i++) {
        snprintf(message, sizeof(message), "- `%s`%s%s\n",
                 catalog->entries[i].slug,
                 catalog->entries[i].display_name != NULL ? " — " : "",
                 catalog->entries[i].display_name != NULL
                     ? catalog->entries[i].display_name
                     : "");
        if (cli_text(state, message, strlen(message)) != 0)
          rc = CAI_ERR_TRANSPORT;
      }
      if (rc == CAI_OK && catalog->count > limit) {
        snprintf(message, sizeof(message),
                 "\nShowing %lu of %lu models. Enter any model ID directly.\n",
                 (unsigned long)limit, (unsigned long)catalog->count);
        if (cli_text(state, message, strlen(message)) != 0)
          rc = CAI_ERR_TRANSPORT;
      }
    }
  }
  cai_model_catalog_close(catalog);
  if (listing)
    return rc == CAI_OK && cli_finish_response(state) == 0 ? CAI_OK
                                                           : CAI_ERR_TRANSPORT;
  rc = cai_agent_runtime_update_settings(state->runtime, &requested, &result,
                                         error);
  if (rc != CAI_OK)
    return rc;
  effort = requested.reasoning_effort;
  if (requested.model != NULL)
    snprintf(
        message, sizeof(message),
        result.applied
            ? "Model changed to %s%s%s.\n"
            : "Model %s%s%s queued for the next safe model/tool boundary.\n",
        requested.model, effort != NULL ? "; reasoning effort " : "",
        effort != NULL ? effort : "");
  else
    snprintf(message, sizeof(message),
             result.applied ? "Reasoning effort changed to %s.\n"
                            : "Reasoning effort %s queued for the next safe "
                              "model/tool boundary.\n",
             effort);
  fields[0] = pslog_str("model", target_model);
  fields[1] =
      pslog_str("reasoning_effort",
                effort != NULL ? effort : result.effective.reasoning_effort);
  fields[2] = pslog_bool("applied", result.applied);
  cai_cli_log_message(state->log.logger, PSLOG_LEVEL_INFO,
                      requested.model != NULL ? "model_selection"
                                              : "reasoning_selection",
                      "user", message, strlen(message), fields, 3U);
  return cli_write(state, message) == 0 ? CAI_OK : CAI_ERR_TRANSPORT;
}

static int cli_tools_write(void *context, const char *bytes, size_t count) {
  return cli_text(context, bytes, count);
}

static int cli_trim_row(void *context, const cai_agent_history_entry *entry,
                        cai_error *error) {
  cli_state *state = context;
  char row[1200];
  cai_cli_history_row(row, sizeof(row), entry, cli_render_width() - 2);
  return cli_text(state, row, strlen(row)) == 0
             ? CAI_OK
             : cai_cli_error(error, CAI_ERR_TRANSPORT, "display trim history");
}

static int cli_trim_command(cli_state *state, const char *arg,
                            cai_error *error) {
  unsigned long long value;
  size_t count;
  char *end;
  char message[128];
  int list, rc;
  while (*arg == ' ' || *arg == '\t')
    arg++;
  list = *arg == '\0' || (strncmp(arg, "list", 4U) == 0 &&
                          (arg[4] == '\0' || arg[4] == ' ' || arg[4] == '\t'));
  if (*arg != '\0' && list)
    arg += 4U;
  while (*arg == ' ' || *arg == '\t')
    arg++;
  count = 20U;
  if (*arg != '\0') {
    if (*arg < '0' || *arg > '9')
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "Usage: /trim [COUNT | list [LIMIT]]");
    errno = 0;
    value = strtoull(arg, &end, 10);
    while (*end == ' ' || *end == '\t')
      end++;
    if (errno != 0 || *end != '\0' || value > (unsigned long long)(size_t)-1)
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "trim count must be a nonnegative integer");
    count = (size_t)value;
  }
  if (list) {
    /* Use a literal Markdown block so summaries cannot become headings,
     * nested lists, links or terminal escapes. libmdf owns all output. */
    if (cli_text(
            state, "## Trim history\n\n```text\n0. Keep current history\n",
            strlen("## Trim history\n\n```text\n0. Keep current history\n")) !=
        0)
      return CAI_ERR_TRANSPORT;
    rc = cai_agent_runtime_visit_history(state->runtime, count, cli_trim_row,
                                         state, error);
    if (cli_text(state, "```\n", 4U) != 0 && rc == CAI_OK)
      rc = CAI_ERR_TRANSPORT;
    if (cli_finish_response(state) != 0 && rc == CAI_OK)
      rc = CAI_ERR_TRANSPORT;
    return rc;
  }
  rc = cai_agent_runtime_trim_history(state->runtime, count, error);
  if (rc == CAI_OK) {
    snprintf(message, sizeof(message),
             "Trimmed %lu newest history %s. External changes remain.",
             (unsigned long)count, count == 1U ? "entry" : "entries");
    if (cli_write(state, message) != 0)
      return CAI_ERR_TRANSPORT;
    rc = cli_sync_status(state, error);
  }
  return rc;
}

static int cli_review_wakeup(sl_t *sl, const sl_watch_event_t *event,
                             void *context) {
  cli_state *state = context;
  cai_agent_runtime_metrics metrics;
  cai_agent_run_state run_state;
  cai_error error;
  int rc;
  (void)sl;
  (void)event;
  cai_error_init(&error);
  rc = cai_agent_runtime_pump(state->review_runtime, 0L, &error);
  if (rc == CAI_OK)
    rc = cai_agent_runtime_state(state->review_runtime, &run_state, &error);
  if (rc == CAI_OK)
    rc = cai_agent_runtime_get_metrics(state->review_runtime, &metrics, &error);
  if (rc == CAI_OK && !metrics.turn_active &&
      (run_state == CAI_AGENT_COMPLETED || run_state == CAI_AGENT_FAILED ||
       run_state == CAI_AGENT_CANCELLED)) {
    rc = cai_agent_runtime_finish_review(state->runtime, state->review_runtime,
                                         &error);
    if (rc == CAI_OK) {
      state->last_review_metrics = metrics;
      state->has_review_metrics = 1;
      (void)sl_watch_remove(state->sl, state->review_watch_id);
      state->has_review_watch = 0;
      cai_agent_runtime_close(state->review_runtime);
      state->review_runtime = NULL;
    }
  }
  if (rc == CAI_OK)
    rc = cli_sync_status(state, &error);
  if (rc != CAI_OK)
    cli_print_error(state, "review", &error);
  cai_error_cleanup(&error);
  return rc == CAI_OK ? SL_OK : SL_ERROR_IO;
}

static int cli_review_command(cli_state *state, const char *arguments,
                              cai_error *error) {
  cai_agent_review_request request;
  int rc, fd, started;
  while (*arguments == ' ' || *arguments == '\t')
    arguments++;
  cai_agent_review_request_init(&request);
  if (strcmp(arguments, "uncommitted") == 0)
    request.target = CAI_AGENT_REVIEW_UNCOMMITTED;
  else if (strncmp(arguments, "base ", 5U) == 0) {
    request.target = CAI_AGENT_REVIEW_BASE_BRANCH;
    request.base_branch = arguments + 5;
  } else if (strncmp(arguments, "custom ", 7U) == 0) {
    request.target = CAI_AGENT_REVIEW_CUSTOM;
    request.instructions = arguments + 7;
  } else if (strncmp(arguments, "commit ", 7U) == 0) {
    request.target = CAI_AGENT_REVIEW_COMMIT;
    request.commit = arguments + 7;
  } else if (*arguments != '\0') {
    request.target = CAI_AGENT_REVIEW_CUSTOM;
    request.instructions = arguments;
  }
  if (!state->options.review_subagent)
    return cai_cli_error(error, CAI_ERR_INVALID, "review subagent is disabled");
  if (state->review_runtime != NULL || state->shell.pid > 0)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "finish the active review or shell before reviewing");
  rc = cai_agent_runtime_start_review(state->runtime, &request,
                                      &state->review_runtime, error);
  started = rc;
  if (state->review_runtime == NULL)
    return rc;
  rc = cai_agent_runtime_wakeup_fd(state->review_runtime, &fd, error);
  if (rc == CAI_OK &&
      sl_watch_add(state->sl, fd,
                   SL_WATCH_READ | SL_WATCH_HANGUP | SL_WATCH_ERROR,
                   cli_review_wakeup, state, &state->review_watch_id) != SL_OK)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "watch isolated reviewer");
  if (rc == CAI_OK) {
    state->has_review_watch = 1;
    state->show_welcome = 0;
    cai_cli_reasoning_reset(&state->reasoning);
  } else {
    state->exit_requested = 1;
  }
  return rc != CAI_OK ? rc : started;
}

static int cli_handle_command(cli_state *state, const char *line,
                              cai_error *error, int *handled) {
  const char *arg;
  char *end;
  unsigned long number;
  int rc;
  *handled = 1;
  if (strcmp(line, "/review") == 0 || strncmp(line, "/review ", 8U) == 0 ||
      strncmp(line, "/review\t", 8U) == 0)
    return cli_review_command(state, line + 7U, error);
  if (strcmp(line, "/todo") == 0 || strncmp(line, "/todo ", 6U) == 0 ||
      strncmp(line, "/todo\t", 6U) == 0) {
    rc = cai_cli_todo_command(state->runtime, line + 5U, cli_tools_write, state,
                              error);
    if (rc == CAI_OK && cli_finish_response(state) != 0)
      rc = CAI_ERR_TRANSPORT;
    return rc;
  }
  if (strcmp(line, "/queue") == 0 || strncmp(line, "/queue ", 7U) == 0 ||
      strncmp(line, "/queue\t", 7U) == 0) {
    cai_agent_runtime_metrics metrics;
    cai_agent_run_state run_state;
    char name[129], message[256];
    size_t length;
    if (state->messages == NULL)
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "Messaging is disabled; start with --messaging "
                           "or set [messaging] enabled = true.");
    arg = line + 6U;
    while (*arg == ' ' || *arg == '\t')
      arg++;
    length = strlen(arg);
    while (length > 0U && (arg[length - 1U] == ' ' || arg[length - 1U] == '\t'))
      length--;
    if (length > 0U) {
      if (length >= sizeof(name))
        return cai_cli_error(error, CAI_ERR_INVALID,
                             "Queue names must be at most 128 characters.");
      memcpy(name, arg, length);
      name[length] = '\0';
      rc = cai_agent_runtime_get_metrics(state->runtime, &metrics, error);
      if (rc == CAI_OK)
        rc = cai_agent_runtime_state(state->runtime, &run_state, error);
      if (rc != CAI_OK)
        return rc;
      if (metrics.turn_active || state->awaiting_turn ||
          run_state == CAI_AGENT_SAMPLING ||
          run_state == CAI_AGENT_DISPATCHING_TOOL)
        return cai_cli_error(error, CAI_ERR_INVALID,
                             "Change the message queue between turns.");
      strcpy(message, cai_cli_messages_name(state->messages));
      rc = cai_cli_messages_set_queue(state->messages, name, error);
      /* A selected name survives /new and /resume in this app, never in
       * config.ini or the conversation checkpoint. */
      if (rc == CAI_OK ||
          strcmp(cai_cli_messages_name(state->messages), message) != 0) {
        strcpy(state->message_queue_override,
               cai_cli_messages_name(state->messages));
        state->options.message_queue = state->message_queue_override;
      }
      if (rc != CAI_OK)
        return rc;
    }
    snprintf(message, sizeof(message), "Message queue: %s",
             cai_cli_messages_name(state->messages));
    return cli_write(state, message) == 0 ? CAI_OK : CAI_ERR_TRANSPORT;
  }
  if (strcmp(line, "/autoqueue") == 0 ||
      strncmp(line, "/autoqueue ", 11U) == 0) {
    int enabled;
    if (state->messages == NULL)
      return cai_cli_error(error, CAI_ERR_INVALID, "Messaging is disabled.");
    if (sl_get_auto_approval(state->sl, &enabled) != SL_OK)
      return cai_cli_error(error, CAI_ERR_TRANSPORT, "read autoqueue setting");
    arg = line + 10U;
    while (*arg == ' ')
      arg++;
    if (*arg == '\0')
      enabled = !enabled;
    else if (strcmp(arg, "on") == 0)
      enabled = 1;
    else if (strcmp(arg, "off") == 0)
      enabled = 0;
    else
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "Usage: /autoqueue [on|off]");
    if (sl_set_auto_approval(state->sl, enabled) != SL_OK)
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "set incoming message approval");
    return cli_write(
               state,
               enabled
                   ? "Autoqueue on: waiting and future incoming prompts are "
                     "automatically approved."
                   : "Autoqueue off: new incoming prompts require approval "
                     "(Alt-A, then Alt-Y; Alt-N rejects, Alt-E edits).") == 0
               ? CAI_OK
               : CAI_ERR_TRANSPORT;
  }
  if (strcmp(line, "/trim") == 0 || strncmp(line, "/trim ", 6U) == 0 ||
      strncmp(line, "/trim\t", 6U) == 0)
    return cli_trim_command(state, line + 5U, error);
  if (strcmp(line, "/tier") == 0 || strncmp(line, "/tier ", 6U) == 0 ||
      strncmp(line, "/tier\t", 6U) == 0)
    return cli_tier_command(state, line + 5U, 0, error);
  if (strcmp(line, "/fast") == 0 || strncmp(line, "/fast ", 6U) == 0 ||
      strncmp(line, "/fast\t", 6U) == 0)
    return cli_tier_command(state, line + 5U, 1, error);
  if (cli_is_terminal_shell(line))
    return cli_terminal_shell(
        state, strcmp(line, "/shellshare") == 0 || strcmp(line, "!sh+") == 0,
        error);
  if (line[0] == '!')
    return cli_shell_start(state, line + 1U, error);
  if (strcmp(line, "/shell-context") == 0 ||
      strncmp(line, "/shell-context ", 15U) == 0) {
    arg = line + 14U;
    while (*arg == ' ')
      arg++;
    if (strcmp(arg, "on") == 0)
      state->options.shell_context = 1;
    else if (strcmp(arg, "off") == 0)
      state->options.shell_context = 0;
    else if (*arg != '\0')
      return cli_write(state, "Usage: /shell-context [on|off]") == 0
                 ? CAI_OK
                 : CAI_ERR_TRANSPORT;
    return cli_write(state, state->options.shell_context
                                ? "Shell results will be shared with the model "
                                  "for future commands."
                                : "Shell results will stay out of model "
                                  "context for future commands.") == 0
               ? CAI_OK
               : CAI_ERR_TRANSPORT;
  }
  if (strcmp(line, "/goal") == 0 || strncmp(line, "/goal ", 6U) == 0 ||
      strncmp(line, "/goal\t", 6U) == 0)
    return cli_goal_command(state, line + 5U, error);
  if (strcmp(line, "/model") == 0 || strncmp(line, "/model ", 7U) == 0 ||
      strncmp(line, "/model\t", 7U) == 0)
    return cli_settings_command(state, line + 6U, 0, error);
  if (strcmp(line, "/reasoning") == 0 ||
      strncmp(line, "/reasoning ", 11U) == 0 ||
      strncmp(line, "/reasoning\t", 11U) == 0)
    return cli_settings_command(state, line + 10U, 1, error);
  if (strcmp(line, "/output") == 0 || strncmp(line, "/output ", 8U) == 0) {
    arg = strcmp(line, "/output") == 0 ? "" : line + 8;
    if (*arg == '\0')
      return cli_write(state,
                       state->options.tool_output != NULL &&
                               strcmp(state->options.tool_output, "full") == 0
                           ? "Tool output: full. Use /output compact or "
                             "/output full.\n"
                           : "Tool output: compact. Use /output compact or "
                             "/output full.\n") == 0
                 ? CAI_OK
                 : CAI_ERR_TRANSPORT;
    if (strcmp(arg, "compact") != 0 && strcmp(arg, "full") != 0)
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "use /output compact or /output full");
    rc = cai_tool_display_set_mode(state->tool_display,
                                   strcmp(arg, "full") == 0
                                       ? CAI_TOOL_OUTPUT_FULL
                                       : CAI_TOOL_OUTPUT_COMPACT,
                                   error);
    if (rc == CAI_OK)
      state->options.tool_output =
          strcmp(arg, "full") == 0 ? "full" : "compact";
    if (rc == CAI_OK &&
        cli_write(state,
                  "Tool output setting applies from the next tool call.\n") !=
            0)
      rc = CAI_ERR_TRANSPORT;
    return rc;
  }
  if (strcmp(line, "/help") == 0) {
    static const char *const help[] = {
        "# CAI help\n\n"
        "Your coding agent for the current directory. Enter a prompt to "
        "start a turn; send another while it works to steer the agent.\n\n"
        "## Slash commands\n\n"
        "| Command | Action |\n"
        "| --- | --- |\n"
        "| `/help` | Show this help. |\n",
        "| `/review [uncommitted\\|base REF\\|custom TEXT]` | Run an isolated "
        "reviewer; no argument compares against the Git default branch. |\n",
        "| `/todo [COMMAND]` | Show the selected board; list, use NAME, "
        "new NAME, or help. Requires --todo. |\n",
        "| `/queue [NAME]` | Show or replace the full mailbox name between "
        "turns; requires --messaging. Applies to this app, including /new "
        "and /resume; leaves config.ini unchanged. |\n",
        "| `/autoqueue [on\\|off]` | Toggle automatic approval of incoming "
        "next-turn messages (default off). Alt-A reviews; Alt-Y approves; "
        "Alt-N rejects; Alt-E edits. |\n",
        "| `/trim [COUNT]` | List 20 recent whole entries, or remove COUNT "
        "newest entries; 0 removes nothing. |\n"
        "| `/trim list [LIMIT]` | List a different number of entries; "
        "compaction is indivisible. |\n"
        "| `/goal [COMMAND]` | Show a goal; new TEXT replaces it, pause/resume "
        "control pursuit, clear removes it, help explains commands. Prefer "
        "asking the agent to manage goals. |\n",
        "| `/shell`, `!sh` | Open your interactive shell between turns; return "
        "to CAI when it exits. Terminal output stays local. |\n",
        "| `/shellshare`, `!sh+` | Open a shell and share sanitized output "
        "when it exits. A ring buffer retains only the newest text that fits "
        "the configured line, byte and replay limits; older text falls out. "
        "|\n",
        "| `/model [MODEL [EFFORT]]` | Show models or change model/effort at "
        "the next safe "
        "model/tool boundary, keeping this conversation. |\n",
        "| `/reasoning [EFFORT]` | Show model-specific reasoning choices or "
        "change effort at the next safe model/tool boundary. |\n",
        "| `/tier [list\\|NAME]` | Cycle service tiers, list choices, or "
        "select a tier at the next safe boundary. |\n"
        "| `/fast` | Toggle Fast/priority and explicit normal routing. |\n",
        "| `/output [compact\\|full]` | Show or change tool previews, starting "
        "with the next tool call. Compact commands show five lines; patches "
        "collapse unchanged context. |\n",
        "| `/status` | Show model, reasoning, context, and available cost or "
        "quota metrics and the tool summary. Refresh ChatGPT quota "
        "immediately. |\n"
        "| `/tools` | List installed tools, MCP servers, discovery errors and "
        "tool descriptions. |\n"
        "| `/resume` | List conversations for the current directory. |\n",
        "| `/resume NUMBER` | Resume a listed conversation and replay prompts "
        "and assistant text. |\n"
        "| `/new` | Start a fresh conversation in this directory. |\n"
        "| `/export [ID]` | Export the current or selected conversation as "
        "Markdown and resumable JSONL. |\n"
        "| `/quit`, `/exit` | Exit CAI. |\n\n",
        "## Prompt keys\n\n"
        "| Key | Action |\n"
        "| --- | --- |\n"
        "| Enter | Send the prompt. |\n"
        "| Ctrl-J | Insert a newline. |\n"
        "| `!COMMAND` | Run a user shell command without a PTY; save its "
        "literal output. |\n",
        "| `/shell-context [on\\|off]` | Share future `!COMMAND` results with "
        "the "
        "model, or keep them local. |\n"
        "| Up / Down | Recall prompts for this directory. |\n"
        "| Ctrl-R | Search history; Enter selects for editing, Enter again "
        "sends. |\n"
        "| Ctrl-V | Paste a PNG or JPEG image from the X11 clipboard. |\n"
        "| Escape / Ctrl-C | Cancel the active turn. |\n"
        "| Ctrl-D (empty prompt) | Exit. |\n\n",
        "## Starting CAI\n\n"
        "```sh\n"
        "cai -N -C ./project                  # new conversation\n"
        "cai -i 'Explain this project'        # send a prompt at startup\n"
        "cai -Nni 'Fix the failing tests'     # run non-interactively\n"
        "cai --review                         # review against default branch\n"
        "```\n\n"
        "Run `cai --help` for all startup options or `man cai` for the "
        "manual.\n"};
    size_t i;
    if (cli_finish_response(state) != 0 || cli_finish_reasoning(state) != 0)
      return CAI_ERR_TRANSPORT;
    for (i = 0U; i < sizeof(help) / sizeof(help[0]); i++)
      if (cli_text(state, help[i], strlen(help[i])) != 0)
        return CAI_ERR_TRANSPORT;
    return cli_finish_response(state) == 0 ? CAI_OK : CAI_ERR_TRANSPORT;
  }
  if (strcmp(line, "/quit") == 0 || strcmp(line, "/exit") == 0) {
    state->exit_requested = 1;
    return CAI_OK;
  }
  if (strcmp(line, "/new") == 0) {
    return cli_switch_session(state, NULL, 1, error);
  }
  if (strcmp(line, "/tools") == 0) {
    if (cli_finish_response(state) != 0 || cli_finish_reasoning(state) != 0)
      return CAI_ERR_TRANSPORT;
    rc = cai_cli_tools_markdown(&state->mcp, state->runtime, 1, cli_tools_write,
                                state, error);
    if (rc == CAI_OK && cli_finish_response(state) != 0)
      rc = CAI_ERR_TRANSPORT;
    return rc;
  }
  if (strcmp(line, "/status") == 0) {
    cai_agent_instruction_status instructions;
    cai_agent_runtime_settings settings;
    cai_agent_runtime_metrics metrics;
    cai_error optional_error;
    char markdown[2048], mailbox_row[256];
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
    rc = cai_agent_runtime_get_instruction_status(state->runtime, &instructions,
                                                  error);
    if (rc != CAI_OK)
      return rc;
    cli_refresh_quota(state, 1);
    snprintf(mailbox_row, sizeof(mailbox_row), "| Message queue | `%s` |\n",
             state->messages != NULL ? cai_cli_messages_name(state->messages)
                                     : "disabled");
    if (cai_cli_status_markdown(markdown, sizeof(markdown), model, effort,
                                state->options.provider, &metrics,
                                state->auth != NULL ? &state->quota : NULL,
                                &instructions) != 0 ||
        cli_text(state, markdown, strlen(markdown)) != 0 ||
        cli_text(state, mailbox_row, strlen(mailbox_row)) != 0 ||
        cai_cli_todo_status(state->runtime, cli_tools_write, state, error) !=
            CAI_OK ||
        cai_cli_tools_markdown(&state->mcp, state->runtime, 0, cli_tools_write,
                               state, error) != CAI_OK ||
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
      return cai_cli_error(error, CAI_ERR_INVALID, "Usage: /resume <number>");
    }
    rc = cli_load_sessions(state, error);
    if (rc != CAI_OK)
      return rc;
    if (number > state->session_count) {
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "No session with that number.");
    }
    return cli_switch_session(state, state->sessions[number - 1U].id, 0, error);
  }
  if (line[0] == '/')
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "Unknown command. Send /help for usage.");
  *handled = 0;
  return CAI_OK;
}

static int cli_submit_automation(cli_state *state, const char *instruction,
                                 cai_error *error) {
  int rc;
  if (state->interactive && instruction != NULL &&
      sl_history_add(state->sl, instruction) != SL_OK)
    return cai_cli_error(error, CAI_ERR_TRANSPORT, sl_last_error(state->sl));
  if (instruction == NULL || cli_prompt(state, instruction) != 0)
    return CAI_ERR_TRANSPORT;
  state->awaiting_turn = 1;
  /* Preserve ordinary prompt journal entries. If another batch turn is
   * pending, this instruction must remain a new turn rather than steering it.
   */
  rc = state->batch && state->batch_pending_turns > 0U
           ? cai_agent_runtime_submit_queued(state->runtime, instruction, error)
           : cai_agent_runtime_submit_interactive(state->runtime, instruction,
                                                  error);
  if (rc == CAI_OK && state->batch)
    state->batch_pending_turns++;
  if (rc != CAI_OK)
    state->awaiting_turn = 0;
  return rc;
}

static int cli_start_goal(cli_state *state, const char *objective,
                          cai_error *error) {
  cai_agent_goal_snapshot goal;
  int rc;

  rc = cai_agent_runtime_replace_goal(state->runtime, objective, error);
  if (rc != CAI_OK)
    return rc;
  do {
    rc = cli_pump(state, 100L, error);
    if (rc != CAI_OK)
      return rc;
    if (state->host_stopped)
      return CAI_OK;
    if (state->automation_failed)
      return cai_cli_error(
          error, CAI_ERR_TRANSPORT,
          "goal could not be started; see the preceding error");
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
  char review_base[512];

  objective = state->options.goal;
  if (state->options.review_and_fix) {
    const char *format =
        "Review staged, unstaged, and untracked changes with the built-in "
        "run_review subagent using target=uncommitted. "
        "Fix every actionable finding while preserving project invariants. "
        "After each fix, run relevant verification, then repeat the review "
        "until it reports no actionable findings. Explain why any reported "
        "finding is irrelevant. Mark this goal complete only after a clean "
        "review and passing verification.";
    if (!state->options.review_uncommitted) {
      const char *base = state->options.base;
      if (base == NULL) {
        rc = cai_agent_review_default_base(state->workspace, review_base,
                                           sizeof(review_base), error);
        if (rc != CAI_OK)
          return rc;
        base = review_base;
      }
      length = snprintf(state->generated_goal, sizeof(state->generated_goal),
                        "Review changes against base %s with the built-in "
                        "run_review subagent. Fix every actionable finding "
                        "while preserving project invariants. After each "
                        "fix, run relevant verification, then repeat review "
                        "against the same base until no actionable findings "
                        "remain. Explain why any finding is irrelevant. "
                        "Mark this goal complete only after a clean review "
                        "and passing verification.",
                        base);
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
    if (state->exit_requested)
      return CAI_OK;
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

  if (state->automation_done || state->awaiting_turn ||
      state->batch_pending_turns > 0U || state->exit_requested)
    return CAI_OK;
  if (state->automation_failed) {
    state->automation_done = 1;
    return CAI_OK;
  }
  /* A completed turn may be delivered before a new/resumed host goal is
   * applied. Do not consume that older goal projection as the new outcome. */
  if (state->goal_starts_pending > 0U)
    return CAI_OK;
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
  cai_agent_runtime_metrics metrics;
  cai_agent_run_state run_state;
  int rc;
  for (;;) {
    rc = cli_pump(state, 100L, error);
    if (rc != CAI_OK)
      return rc;
    rc = cli_advance_automation(state, error);
    if (rc != CAI_OK)
      return rc;
    if (state->host_stopped)
      break;
    rc = cai_agent_runtime_get_metrics(state->runtime, &metrics, error);
    if (rc == CAI_OK)
      rc = cai_agent_runtime_state(state->runtime, &run_state, error);
    if (rc != CAI_OK)
      return rc;
    if (state->automation_done && !state->awaiting_turn &&
        state->batch_pending_turns == 0U && !metrics.turn_active &&
        run_state != CAI_AGENT_SAMPLING &&
        run_state != CAI_AGENT_DISPATCHING_TOOL)
      break;
  }
  return state->automation_failed ? CAI_ERR_TRANSPORT : CAI_OK;
}

static int cli_drain(cli_state *state, cai_error *error) {
  cai_agent_run_state run_state;
  int rc;
  for (;;) {
    rc = cli_pump(state, 100L, error);
    if (rc != CAI_OK)
      return rc;
    if (state->host_stopped)
      return CAI_OK;
    rc = cai_agent_runtime_state(state->runtime, &run_state, error);
    if (rc != CAI_OK)
      return rc;
    if (run_state != CAI_AGENT_SAMPLING &&
        run_state != CAI_AGENT_DISPATCHING_TOOL) {
      return cli_pump(state, 0L, error);
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
  } else if (state->options.review_uncommitted) {
    request.target = CAI_AGENT_REVIEW_UNCOMMITTED;
  }
  rc = cai_agent_runtime_submit_review(state->runtime, &request, error);
  if (rc != CAI_OK)
    return rc;
  rc = cli_drain(state, error);
  if (rc != CAI_OK)
    return rc;
  if (state->host_stopped)
    return CAI_OK;
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

int cai_cli_app_run(int argc, char **argv, const cai_app_config *application,
                    cai_app_bridge *bridge) {
  cli_state state;
  cai_client_config client_config;
  cai_chatgpt_auth_config auth_config;
  mdf_options mdf_config;
  mdf_sink sink;
  sl_config_t prompt_config;
  cai_error error;
  sl_readline_status_t prompt_status;
  char auth_path[PATH_MAX];
  const char *auth_file;
  char *metadata_json;
  char *line;
  pslog_level default_log_level;
  int utility_mode;
  int parsed;
  int handled;
  int rc;
  int result;

  memset(&state, 0, sizeof(state));
  state.application = application;
  state.bridge = bridge;
  state.timer_fd = -1;
  cai_cli_shell_init(&state.shell);
  state.log.fd = -1;
  state.log.wakeup_fd = -1;
  result = 1;
  metadata_json = NULL;
  cai_error_init(&error);
  if (argc > 1 && (strncmp(argv[1], "chrome-extension://", 19U) == 0 ||
                   strcmp(argv[1], "--chrome-native-host") == 0)) {
    int explicit_mode = strcmp(argv[1], "--chrome-native-host") == 0;
    /* Native messaging owns stdout; LOG_OUTPUT cannot redirect diagnostics
     * there. */
    (void)unsetenv("LOG_OUTPUT");
    if (cai_cli_log_open_with_logger(
            &state.log, PSLOG_LEVEL_WARN, 0,
            application != NULL ? application->logger : NULL, &error) != CAI_OK)
      goto cleanup;
    if (argc != (explicit_mode ? 3 : 2))
      rc = cai_cli_error(&error, CAI_ERR_INVALID,
                         "Invalid Chrome native host arguments");
    else
      rc = cai_chrome_native_host_run(argv[explicit_mode ? 2 : 1],
                                      state.log.logger, &error);
    if (rc != CAI_OK)
      cai_cli_log_error(state.log.logger, "Chrome native host", &error);
    result = rc == CAI_OK ? 0 : 1;
    goto cleanup;
  }
  parsed = cai_cli_configure(argc, argv, &state.options, &state.configuration,
                             application, &error);
  if (parsed <= 0) {
    if (parsed < 0 &&
        cai_cli_log_open_with_logger(
            &state.log, PSLOG_LEVEL_WARN, 0,
            application != NULL ? application->logger : NULL, &error) == CAI_OK)
      cli_diagnostic(&state, PSLOG_LEVEL_ERROR,
                     error.message != NULL ? error.message
                                           : state.options.diagnostic);
    result = parsed == 0 ? 0 : 2;
    if (parsed < 0 && error.code == CAI_OK)
      cai_cli_error(&error, CAI_ERR_INVALID, state.options.diagnostic);
    goto cleanup;
  }
  state.batch = state.options.review || state.options.non_interactive;
  utility_mode = state.options.chrome_setup || state.options.chrome_remove ||
                 state.options.login || state.options.list ||
                 state.options.resume_list || state.options.export_id != NULL ||
                 state.options.import_file != NULL ||
                 state.options.install_skill != NULL;
  default_log_level = utility_mode ? PSLOG_LEVEL_WARN : PSLOG_LEVEL_TRACE;
  if (cai_cli_log_open_with_logger(
          &state.log, default_log_level, !state.batch && !utility_mode,
          application != NULL ? application->logger : NULL, &error) != CAI_OK)
    goto cleanup;
  (void)setlocale(LC_CTYPE, "");
  result = 1;
  if (realpath(".", state.workspace) == NULL) {
    cli_diagnostic(&state, PSLOG_LEVEL_ERROR, "cannot resolve workspace");
    result = 2;
    goto cleanup;
  }
  if (state.options.install_skill != NULL) {
    result = cai_cli_install_skill(&state.options, state.log.logger);
    goto cleanup;
  }
  if (!state.batch && !utility_mode) {
    rc = cai_cli_log_interactive(&state.log, &error);
    if (rc != CAI_OK) {
      cli_print_error(&state, "open interactive log", &error);
      goto cleanup;
    }
  }
  if (state.options.chrome_setup || state.options.chrome_remove) {
    rc = cai_chrome_native_host_setup(state.options.chrome_manifest,
                                      state.options.chrome_remove,
                                      state.log.logger, &error);
    if (rc != CAI_OK)
      cli_print_error(&state, "Chrome registration", &error);
    result = rc == CAI_OK ? 0 : 1;
    goto cleanup;
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
  if (state.interactive &&
      signal(SIGINT, cli_keep_session_on_interrupt) == SIG_ERR) {
    cli_diagnostic(&state, PSLOG_LEVEL_ERROR,
                   "cai: failed to install interactive interrupt handler\n");
    goto cleanup;
  }

  cai_cli_status_init(&state.status, state.workspace, getenv("HOME"));
  result = 1;
  rc = cli_prepare_clipboard(&state, &error);
  if (rc != CAI_OK) {
    cli_print_error(&state, "initialize clipboard", &error);
    goto cleanup;
  }
  if (!state.batch) {
    if (state.interactive && application != NULL &&
        application->banner != NULL && application->banner[0] != '\0' &&
        (fputs(application->banner, stdout) == EOF ||
         (application->banner[strlen(application->banner) - 1U] != '\n' &&
          fputc('\n', stdout) == EOF) ||
         fflush(stdout) != 0)) {
      cli_diagnostic(&state, PSLOG_LEVEL_ERROR, "cannot write startup banner");
      goto cleanup;
    }
    sl_config_init(&prompt_config);
    prompt_config.prompt_queue = 1;
    prompt_config.line_max_len = 65536U + 512U;
    prompt_config.history_max_len = 1000;
    prompt_config.image_paste_path_template =
        "{{XDG_CACHE_HOME}}/cai/clipboard/*";
    state.sl = sl_create_with_config(&prompt_config);
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
  if ((state.interactive &&
       (sl_bind_key(state.sl, SL_KEY_ESCAPE, cli_cancel_editor, NULL) !=
            SL_OK ||
        sl_set_statusline(state.sl, 1, 0) != SL_OK ||
        sl_set_status_message(state.sl, cli_welcome) != SL_OK)) ||
      (!state.batch && sl_output_stream_begin(state.sl) != SL_OK) ||
      mdf_create(MDF_FORMAT_ANSI, &mdf_config, &state.renderer) != MDF_OK) {
    cli_diagnostic(&state, PSLOG_LEVEL_ERROR,
                   "cai: failed to initialize terminal renderers\n");
    goto cleanup;
  }
  sink.userdata = &state;
  sink.write = cli_renderer_sink;
  if (state.renderer->set_sink(state.renderer, &sink) != MDF_OK) {
    cli_diagnostic(&state, PSLOG_LEVEL_ERROR,
                   "cai: failed to connect Markdown renderer\n");
    goto cleanup;
  }
  if (!state.batch && cli_tool_display_init(&state, &error) != CAI_OK) {
    cli_print_error(&state, "initialize tool renderer", &error);
    goto cleanup;
  }
  if (state.interactive) {
    char history_directory[PATH_MAX];
    rc = snprintf(history_directory, sizeof(history_directory), "%s/history",
                  state.pouch.state_directory);
    if (rc < 0 || (size_t)rc >= sizeof(history_directory)) {
      cai_cli_error(&error, CAI_ERR_INVALID, "prompt history path is too long");
      cli_print_error(&state, "initialize prompt history", &error);
      goto cleanup;
    }
    if (sl_history_open(state.sl, state.workspace, history_directory) !=
            SL_OK ||
        sl_history_set_auto_add(state.sl, 1) != SL_OK) {
      cai_cli_error(&error, CAI_ERR_TRANSPORT, sl_last_error(state.sl));
      cli_print_error(&state, "initialize prompt history", &error);
      goto cleanup;
    }
  }
  if (state.interactive) {
    if (sl_bind_key(state.sl, SL_KEY_ENTER, cli_shell_editor_key, &state) !=
            SL_OK ||
        sl_bind_key(state.sl, SL_KEY_ALT_ENTER, cli_shell_editor_key, &state) !=
            SL_OK ||
        sl_bind_key(state.sl, SL_KEY_TAB, cli_shell_editor_key, &state) !=
            SL_OK) {
      cli_diagnostic(&state, PSLOG_LEVEL_ERROR,
                     "bind interactive shell submission guards");
      goto cleanup;
    }
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
    if (state.log.wakeup_fd >= 0 &&
        sl_watch_add(state.sl, state.log.wakeup_fd,
                     SL_WATCH_READ | SL_WATCH_ERROR | SL_WATCH_HANGUP,
                     cli_log_wakeup, &state, &state.log_watch_id) != SL_OK) {
      cli_diagnostic(&state, PSLOG_LEVEL_ERROR,
                     "watch interactive log notices");
      goto cleanup;
    }
    state.has_log_watch = state.log.wakeup_fd >= 0;
  }
  cai_client_config_init(&client_config);
  client_config.logger = state.log.logger;
  rc = cli_load_model_metadata(&state.options, &metadata_json, &error);
  if (rc != CAI_OK) {
    cli_print_error(&state, "load model metadata", &error);
    goto cleanup;
  }
  client_config.model_metadata_json = metadata_json;
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
    if (pthread_mutex_init(&state.auth_lock, NULL) != 0) {
      cai_cli_error(&error, CAI_ERR_TRANSPORT, "initialize shared auth lock");
      cli_print_error(&state, "open ChatGPT auth", &error);
      goto cleanup;
    }
    state.has_auth_lock = 1;
    state.shared_auth.access_token = cli_auth_token;
    state.shared_auth.refresh = cli_auth_refresh;
    state.shared_auth.impl = &state;
    client_config.chatgpt_auth = &state.shared_auth;
  } else if (strcmp(state.options.provider, "openrouter") == 0) {
    cai_client_config_use_openrouter(&client_config);
  } else if (strcmp(state.options.provider, "berget") == 0) {
    cai_client_config_use_berget(&client_config);
  } else if (strcmp(state.options.provider, "custom") == 0) {
    client_config.base_url = state.options.endpoint;
    client_config.api_key_env = state.options.api_key_env;
  }
  client_config.metadata_provider = state.options.provider;
  if (state.configuration != NULL &&
      state.configuration->apply_client(state.configuration, &client_config,
                                        &error) != CAI_OK) {
    cli_print_error(&state, "apply client configuration", &error);
    goto cleanup;
  }
  if (state.options.endpoint != NULL)
    client_config.base_url = state.options.endpoint;
  if (state.options.api_key_env != NULL)
    client_config.api_key_env = state.options.api_key_env;
  if (metadata_json != NULL)
    client_config.model_metadata_json = metadata_json;
  rc = cai_client_open(&client_config, &state.client, &error);
  if (rc != CAI_OK) {
    cli_print_error(&state, "open client", &error);
    goto cleanup;
  }
  /* Explicit catalog commands inherit the configured request deadline. They
   * are independent of the short, opportunistic quota-refresh timeout. */
  {
    pslog_field operation = pslog_str("operation", "model catalog");
    state.model_log = state.log.logger->with(state.log.logger, &operation, 1U);
    if (state.model_log == NULL) {
      cai_cli_error(&error, CAI_ERR_NOMEM, "create model catalog logger");
      cli_print_error(&state, "open model catalog client", &error);
      goto cleanup;
    }
    client_config.logger = state.model_log;
  }
  rc = cai_client_open(&client_config, &state.model_client, &error);
  if (rc != CAI_OK) {
    cli_print_error(&state, "open model catalog client", &error);
    goto cleanup;
  }
  if (state.auth != NULL) {
    cai_error quota_error;
    cai_client_config quota_config;
    cai_error_init(&quota_error);
    cai_client_config_init(&quota_config);
    quota_config.logger = state.log.logger;
    quota_config.chatgpt_auth = &state.shared_auth;
    quota_config.timeout_ms = 1500L;
    if (cai_client_open(&quota_config, &state.quota_client, &quota_error) ==
        CAI_OK) {
      if (state.interactive &&
          cai_cli_quota_open(&state.quota_refresh, cli_fetch_quota,
                             state.quota_client, &quota_error) == CAI_OK) {
        if (sl_watch_add(state.sl, state.quota_refresh.fd,
                         SL_WATCH_READ | SL_WATCH_ERROR, cli_quota_wakeup,
                         &state, &state.quota_watch_id) == SL_OK)
          state.has_quota_watch = 1;
        else {
          cai_cli_quota_close(&state.quota_refresh);
          cli_diagnostic(
              &state, PSLOG_LEVEL_WARN,
              "quota updates unavailable: cannot watch quota worker");
        }
      }
    }
    if (quota_error.code != CAI_OK)
      cai_cli_log_error(state.log.logger, "initialize quota updates",
                        &quota_error);
    cai_error_cleanup(&quota_error);
  }
  rc = cli_open_runtime(&state, state.options.resume_id,
                        state.options.new_session, &error);
  if (rc != CAI_OK) {
    cli_print_error(&state, "open agent session", &error);
    goto cleanup;
  }
  rc = cai_app_bridge_publish(bridge, state.runtime, &error);
  if (rc != CAI_OK)
    goto cleanup;
  rc = cli_host_process(&state, &error);
  if (rc != CAI_OK)
    goto cleanup;
  if (state.exit_requested) {
    result = 0;
    goto cleanup;
  }
  if (state.interactive && bridge != NULL) {
    if (sl_set_auto_approval(state.sl, state.options.enable_auto_enqueue) !=
            SL_OK ||
        sl_set_prompt_queue_delivery(
            state.sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) != SL_OK ||
        sl_set_idle_callback(state.sl, cli_message_idle, &state) != SL_OK ||
        sl_watch_add(state.sl, bridge->fd, SL_WATCH_READ | SL_WATCH_ERROR,
                     cli_host_wakeup, &state, &state.host_watch) != SL_OK) {
      cai_cli_error(&error, CAI_ERR_TRANSPORT, "watch host controls");
      goto cleanup;
    }
    state.has_host_watch = 1;
  }
  if (state.options.review) {
    rc = cli_run_review(&state, &error);
    if (rc != CAI_OK)
      cli_print_error(&state, "review", &error);
    else
      result = 0;
    goto cleanup;
  }
  if (state.interactive)
    cli_refresh_quota(&state, 0);
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
    if (state.exit_requested) {
      if (line != NULL)
        sl_free_string(state.sl, line);
      break;
    }
    if (line == NULL) {
      prompt_status = sl_last_readline_status(state.sl);
      if (prompt_status == SL_READLINE_CANCELLED ||
          prompt_status == SL_READLINE_INTERRUPTED) {
        if (state.shell.pid > 0) {
          cai_cli_shell_cancel(&state.shell);
          continue;
        }
        cai_cli_messages_cancel_send(state.messages);
        rc = cai_agent_runtime_cancel_turn(
            state.review_runtime != NULL ? state.review_runtime : state.runtime,
            &error);
        if (rc != CAI_OK) {
          cai_error_cleanup(&error);
          cai_error_init(&error);
        }
        continue;
      }
      if (prompt_status == SL_READLINE_ERROR) {
        /* A failed observer has already reported its detailed error through
         * the watched callback. Softline's generic I/O wrapper adds no cause.
         */
        if (bridge == NULL || !bridge->failed) {
          cai_cli_error(&error, CAI_ERR_TRANSPORT, sl_last_error(state.sl));
          cli_print_error(&state, "prompt input failed", &error);
        }
        goto cleanup;
      }
      break;
    }
    if (line[0] != '\0') {
      if (line[0] == '/' || line[0] == '!')
        cli_command_input(&state, line);
      rc = cli_handle_command(&state, line, &error, &handled);
      if (cli_flush_command(&state, rc == CAI_OK) != 0)
        rc = CAI_ERR_TRANSPORT;
      if (rc == CAI_OK && !handled) {
        if (cli_prompt(&state, line) != 0) {
          rc = CAI_ERR_TRANSPORT;
        } else {
          if (state.review_runtime != NULL || state.shell.pid > 0 ||
              cai_cli_messages_is_message(state.messages,
                                          sl_last_prompt_id(state.sl)))
            rc = cai_agent_runtime_submit_queued(state.runtime, line, &error);
          else
            rc = cai_agent_runtime_submit_interactive(state.runtime, line,
                                                      &error);
          if (rc == CAI_OK)
            rc = cai_cli_messages_consumed(state.messages,
                                           sl_last_prompt_id(state.sl), &error);
        }
      }
      if (rc != CAI_OK) {
        cli_print_error(&state, "input", &error);
        if (state.runtime == NULL || state.exit_requested) {
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
  if (state.shell.pid > 0 && cli_shell_stop(&state, &error) != CAI_OK) {
    cli_print_error(&state, "finish user shell command", &error);
    goto cleanup;
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
  if (state.host_stopped && state.runtime != NULL) {
    cai_app_bridge_outcome(bridge, CAI_APP_STOPPED);
  }
  if (bridge != NULL) {
    cai_error snapshot;
    cai_error_init(&snapshot);
    if (cai_app_bridge_publish(bridge, state.runtime, &snapshot) != CAI_OK) {
      cai_app_bridge_error(bridge, &snapshot);
      result = 1;
    }
    cai_error_cleanup(&snapshot);
    cai_app_bridge_error(bridge, &error);
  }
  cli_shell_unwatch(&state);
  cai_cli_shell_close(&state.shell);
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
  else {
    cai_cli_messages_close(state.messages);
    state.messages = NULL;
  }
  cai_cli_mcp_close(&state.mcp);
  if (state.has_host_watch)
    (void)sl_watch_remove(state.sl, state.host_watch);
  if (state.has_log_watch)
    (void)sl_watch_remove(state.sl, state.log_watch_id);
  if (state.has_timer_watch)
    (void)sl_watch_remove(state.sl, state.timer_watch_id);
  if (state.timer_fd >= 0)
    close(state.timer_fd);
  if (state.has_quota_watch)
    (void)sl_watch_remove(state.sl, state.quota_watch_id);
  cai_cli_quota_close(&state.quota_refresh);
  if (state.client != NULL)
    state.client->close(state.client);
  if (state.model_client != NULL)
    state.model_client->close(state.model_client);
  if (state.model_log != NULL)
    state.model_log->destroy(state.model_log);
  if (state.quota_client != NULL)
    state.quota_client->close(state.quota_client);
  if (state.auth != NULL)
    state.auth->close(state.auth);
  if (state.has_auth_lock)
    pthread_mutex_destroy(&state.auth_lock);
  cai_cli_pouch_close(&state.pouch);
  if (state.sl != NULL)
    (void)cai_cli_log_notices(&state.log, cli_notice, &state);
  if (state.renderer != NULL) {
    (void)cli_finish_response(&state);
    (void)cli_finish_reasoning(&state);
    state.renderer->destroy(state.renderer);
  }
  if (state.notice_renderer != NULL)
    state.notice_renderer->destroy(state.notice_renderer);
  cai_tool_display_close(state.tool_display);
  cai_sink_close(state.tool_sink);
  if (state.sl != NULL) {
    (void)sl_output_stream_end(state.sl);
    sl_destroy(state.sl);
  }
  if (state.sl == NULL)
    (void)cai_cli_log_notices(&state.log, cli_startup_notice, &state);
  free(state.sessions);
  free(state.review_report);
  free(metadata_json);
  cai_config_close(state.configuration);
  cai_error_cleanup(&error);
  if (cai_cli_log_close(&state.log) != 0)
    result = 1;
  return result;
}

int cai_cli_app_main(int argc, char **argv, const cai_app_config *application) {
  return cai_cli_app_run(argc, argv, application, NULL);
}
