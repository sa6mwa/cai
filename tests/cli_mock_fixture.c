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

int main(int argc, char **argv) { return cai_cli_app_main(argc, argv, NULL); }
