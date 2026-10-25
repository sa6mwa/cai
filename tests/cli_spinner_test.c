/* Exercise the production status projection with controlled runtime snapshots.
 * A tick or tool boundary must not restart softline's animation. No model,
 * database, clock delay or terminal process is needed for this regression. */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <cai/agent_runtime.h>
#include <softline/softline.h>
#include <stdio.h>
#include <string.h>

static cai_agent_runtime_metrics fixture_metrics;
static cai_agent_run_state fixture_state;
static int busy_calls, spinner_calls, busy_value, spinner_value;
static char status_message[640];

static int fixture_get_state(cai_agent_runtime *runtime,
                             cai_agent_run_state *out, cai_error *error) {
  (void)runtime;
  (void)error;
  *out = fixture_state;
  return CAI_OK;
}
static int fixture_get_metrics(cai_agent_runtime *runtime,
                               cai_agent_runtime_metrics *out,
                               cai_error *error) {
  (void)runtime;
  (void)error;
  *out = fixture_metrics;
  return CAI_OK;
}
static int fixture_get_settings(cai_agent_runtime *runtime,
                                cai_agent_runtime_settings *out,
                                cai_agent_runtime_settings *pending,
                                cai_error *error) {
  (void)runtime;
  (void)error;
  if (pending != NULL)
    memset(pending, 0, sizeof(*pending));
  memset(out, 0, sizeof(*out));
  out->model = "gpt-6-luna";
  out->reasoning_effort = "medium";
  return CAI_OK;
}
static int fixture_get_context(cai_agent_runtime *runtime, double *out,
                               int *available, cai_error *error) {
  (void)runtime;
  (void)error;
  *out = 0.0;
  *available = 0;
  return CAI_OK;
}
static int fixture_get_goal(cai_agent_runtime *runtime,
                            cai_agent_goal_snapshot *out, cai_error *error) {
  (void)runtime;
  (void)error;
  memset(out, 0, sizeof(*out));
  return CAI_OK;
}
static int fixture_busy(sl_t *sl, int value) {
  busy_calls++;
  busy_value = value;
  return sl_set_status_busy(sl, value);
}
static int fixture_spinner(sl_t *sl, int value) {
  spinner_calls++;
  spinner_value = value;
  return sl_set_status_spinner(sl, value);
}
static int fixture_message(sl_t *sl, const char *value) {
  snprintf(status_message, sizeof(status_message), "%s", value);
  return sl_set_status_message(sl, value);
}
#define cai_agent_runtime_state fixture_get_state
#define cai_agent_runtime_get_metrics fixture_get_metrics
#define cai_agent_runtime_get_settings fixture_get_settings
#define cai_agent_runtime_context_percent fixture_get_context
#define cai_agent_runtime_get_goal fixture_get_goal
#define sl_set_status_busy fixture_busy
#define sl_set_status_spinner fixture_spinner
#define sl_set_status_message fixture_message
#include "../cli/app_run.c"

int main(void) {
  static const cai_agent_run_state endings[] = {
      CAI_AGENT_COMPLETED, CAI_AGENT_FAILED, CAI_AGENT_CANCELLED};
  cli_state state;
  cai_error error;
  size_t i;
  int tick, failed = 0;
  memset(&state, 0, sizeof(state));
  cai_cli_options_init(&state.options);
  state.interactive = 1;
  state.timer_fd = -1;
  state.sl = sl_create();
  strcpy(state.workspace, "/");
  cai_error_init(&error);
  if (state.sl == NULL)
    return 1;
  if (cli_sync_status(&state, &error) != CAI_OK || busy_calls || spinner_calls)
    failed = 1;
  for (i = 0U; !failed && i < sizeof(endings) / sizeof(endings[0]); i++) {
    fixture_metrics.turn_active = 1;
    for (tick = 0; tick < 30; tick++) {
      fixture_state = tick % 3 == 0   ? CAI_AGENT_DISPATCHING_TOOL
                      : tick % 3 == 1 ? CAI_AGENT_SAMPLING
                                      : CAI_AGENT_IDLE;
      fixture_metrics.turn_elapsed_ms = (unsigned long long)tick * 1000U;
      if (cli_sync_status(&state, &error) != CAI_OK ||
          busy_calls != (int)(i * 2U + 1U) || spinner_calls != busy_calls ||
          busy_value != 1 || spinner_value != 1) {
        failed = 1;
        break;
      }
    }
    fixture_metrics.turn_active = 0;
    fixture_state = endings[i];
    for (tick = 0; tick < 3; tick++)
      if (cli_sync_status(&state, &error) != CAI_OK ||
          busy_calls != (int)(i * 2U + 2U) || spinner_calls != busy_calls ||
          busy_value != 0 || spinner_value != 0)
        failed = 1;
  }
  /* The owner loop publishes once more after handling /quit. It must retain
   * the goodbye rather than restore the welcome, reasoning or worked text. */
  state.exit_requested = 1;
  state.show_welcome = 1;
  fixture_metrics.turn_active = 1;
  fixture_state = CAI_AGENT_SAMPLING;
  if (cli_sync_status(&state, &error) != CAI_OK || busy_value ||
      spinner_value || strcmp(status_message, "Goodbye.") != 0)
    failed = 1;
  sl_destroy(state.sl);
  cai_error_cleanup(&error);
  if (failed)
    fprintf(stderr, "status refresh restarted or lost the spinner\n");
  return failed;
}
