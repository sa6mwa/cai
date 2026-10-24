/* The real CLI against local provider fixtures. Stateless presentation tests
 * explicitly omit session storage; replay/export tests retain the real store.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "../cli/messages.h"
#include <cai/agent_runtime.h>
#include <stdlib.h>
#include <string.h>

static int fixture_runtime_open(cai_client *client,
                                const cai_agent_runtime_config *config,
                                cai_agent_runtime **out, cai_error *error) {
  cai_agent_runtime_config selected = *config;
  const char *volatile_session = getenv("CAI_TEST_VOLATILE_SESSION");
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
#include "../cli/app_run.c"
#undef cai_cli_messages_start
#undef cai_agent_runtime_open

int main(int argc, char **argv) { return cai_cli_app_main(argc, argv, NULL); }
