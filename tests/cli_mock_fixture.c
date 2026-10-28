/* The real CLI against local provider fixtures. Stateless presentation tests
 * explicitly omit session storage; replay/export tests retain the real store.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "../cli/messages.h"
#include "../cli/pouch.h"
#include <cai/agent_runtime.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fixture_late_steer(void *context,
                              const cai_agent_runtime_event *event,
                              cai_error *error);
static cai_agent_runtime_event_fn fixture_event_callback;

/* Test-only wait injection: exercise the real retry path without wall sleeps.
 */
extern void cai_runtime_test_set_capacity_wait_fn(int (*fn)(cai_agent_runtime *,
                                                            long, cai_error *));
static int fixture_capacity_wait(cai_agent_runtime *runtime, long delay,
                                 cai_error *error) {
  (void)runtime;
  (void)delay;
  (void)error;
  return CAI_OK;
}

static int fixture_prompt_append(cai_cli_pouch *pouch, const char *prompt,
                                 cai_error *error) {
  const char *failure = getenv("CAI_TEST_PROMPT_APPEND_FAILURE");
  if (failure != NULL && access(failure, F_OK) == 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "Injected prompt persistence failure");
  return cai_cli_pouch_prompt_append(pouch, prompt, error);
}

static int fixture_runtime_open(cai_client *client,
                                const cai_agent_runtime_config *config,
                                cai_agent_runtime **out, cai_error *error) {
  cai_agent_runtime_config selected = *config;
  cai_agent_preset worker;
  cai_agent_subagent_profile profile;
  const char *volatile_session = getenv("CAI_TEST_VOLATILE_SESSION");
  if (getenv("CAI_TEST_LATE_STEER") != NULL) {
    fixture_event_callback = selected.event_callback;
    selected.event_callback = fixture_late_steer;
  }
  cai_runtime_test_set_capacity_wait_fn(
      getenv("CAI_TEST_CAPACITY_WAIT") != NULL ? fixture_capacity_wait : NULL);
  if (getenv("CAI_TEST_STEER_WORKER") != NULL) {
    memset(&worker, 0, sizeof(worker));
    worker.name = "seam-worker";
    worker.prompt_version = "worker-1";
    worker.default_identity = "Worker";
    worker.default_model = "gpt-5.6-luna";
    worker.developer_instructions = "You are the seam test worker.";
    memset(&profile, 0, sizeof(profile));
    profile.name = "worker";
    profile.description = "Delegate to the seam test worker.";
    profile.preset = &worker;
    profile.expose_instructions = 1;
    profile.instruction_template = "Perform {{instructions}}.";
    selected.subagents = &profile;
    selected.subagent_count = 1U;
  }
  if (volatile_session != NULL && strcmp(volatile_session, "1") == 0) {
    selected.session_store = NULL;
    selected.disable_default_session_store = 1;
    selected.resume_latest = 0;
    selected.resume_session_id = NULL;
  }
  return cai_agent_runtime_open(client, &selected, out, error);
}
/* Fail after the services have started to exercise partial-open cleanup. */
static int fixture_messages_start(cai_cli_messages *messages, const char *id,
                                  const char *workspace, const char *queue,
                                  cai_error *error) {
  static int failed;
  int rc = cai_cli_messages_start(messages, id, workspace, queue, error);
  if (rc == CAI_OK && !failed &&
      getenv("CAI_TEST_MESSAGES_START_FAILURE") != NULL) {
    failed = 1;
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "Injected message listener startup failure");
  }
  return rc;
}
#define cai_agent_runtime_open fixture_runtime_open
#define cai_cli_messages_start fixture_messages_start
#define cai_cli_pouch_prompt_append fixture_prompt_append
#include "../cli/app_run.c"
#undef cai_cli_pouch_prompt_append
#undef cai_cli_messages_start
#undef cai_agent_runtime_open

static int fixture_late_steer(void *context,
                              const cai_agent_runtime_event *event,
                              cai_error *error) {
  static int inserted;
  cli_state *state = context;
  const char *mode = getenv("CAI_TEST_LATE_STEER");
  int rc = fixture_event_callback(context, event, error);
  size_t index;
  if (rc != CAI_OK || inserted || event->type != CAI_AGENT_EVENT_RUN_COMPLETED)
    return rc;
  inserted = 1;
  index = sl_prompt_queue_count(state->sl);
  /* Admission after the final active boundary deterministically reproduces
   * the live session's idle S entry with a Q theme command behind it. */
  if (sl_prompt_queue_append(state->sl, "LATE_STEER", strcmp(mode, "held") == 0,
                             NULL) != SL_OK ||
      sl_prompt_queue_set_mode(state->sl, index, SL_PROMPT_QUEUE_MODE_STEER) !=
          SL_OK ||
      sl_prompt_queue_append(state->sl, "/theme gruvbox", 0, NULL) != SL_OK)
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "inject late steer fixture");
  return CAI_OK;
}

int main(int argc, char **argv) { return cai_cli_app_main(argc, argv, NULL); }
