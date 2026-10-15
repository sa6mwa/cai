#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "../src/cai_internal.h"
#include <softline/softline.h>
#include <stdio.h>
#include <stdlib.h>

/* Trace calls reaching softline: reapplying either setter resets animation.
 * Keep the real editor and runtime so provider/tool/timer wakeups are covered.
 */
static void fixture_spinner_trace(const char *setting, int value) {
  const char *path = getenv("CAI_TEST_SPINNER_TRACE");
  FILE *file;
  if (path == NULL)
    return;
  file = fopen(path, "a");
  if (file == NULL)
    abort();
  if (fprintf(file, "%s %d\n", setting, value) < 0 || fclose(file) != 0)
    abort();
}

static int fixture_status_busy(sl_t *sl, int busy) {
  fixture_spinner_trace("busy", busy);
  return sl_set_status_busy(sl, busy);
}

static int fixture_status_spinner(sl_t *sl, int enabled) {
  fixture_spinner_trace("spinner", enabled);
  return sl_set_status_spinner(sl, enabled);
}

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
#define sl_set_status_busy fixture_status_busy
#define sl_set_status_spinner fixture_status_spinner
#define main cai_cli_app_main
int cai_cli_app_main(int argc, char **argv);
#include "../cli/main.c"
#undef main
#undef cai_client_open
#undef sl_set_status_busy
#undef sl_set_status_spinner

int main(int argc, char **argv) { return cai_cli_app_main(argc, argv); }
