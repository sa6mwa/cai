#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "../src/cai_internal.h"
#include <stdlib.h>

/* Substitute only the transport endpoint/protocol and its catalog policy.
 * The runtime, encrypted storage, settings controls and UI are production. */
static int fixture_client_open(const cai_client_config *config,
                               cai_client **out, cai_error *error) {
  cai_client_config selected = *config;
  int rc;
  selected.base_url = getenv("CAI_TEST_API_BASE_URL");
  selected.http_2_disabled = 1;
  if (getenv("CAI_TEST_CHAT_COMPLETIONS") != NULL)
    selected.api_protocol = CAI_API_PROTOCOL_CHAT_COMPLETIONS;
  rc = cai_client_open(&selected, out, error);
  if (rc == CAI_OK)
    CAI_CLIENT_IMPL(*out)->catalog_policy_origin = CAI_CHATGPT_CODEX_BASE_URL;
  return rc;
}
#define cai_client_open fixture_client_open
#define main cai_cli_app_main
int cai_cli_app_main(int argc, char **argv);
#include "../cli/main.c"
#undef main
#undef cai_client_open

int main(int argc, char **argv) { return cai_cli_app_main(argc, argv); }
