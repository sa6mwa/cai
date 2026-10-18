/* The real CLI against local provider fixtures. Stateless presentation tests
 * explicitly omit session storage; replay/export tests retain the real store.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
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
#define cai_agent_runtime_open fixture_runtime_open
#define main cai_cli_app_main
int cai_cli_app_main(int argc, char **argv);
#include "../cli/main.c"
#undef main
#undef cai_agent_runtime_open

int main(int argc, char **argv) { return cai_cli_app_main(argc, argv); }
