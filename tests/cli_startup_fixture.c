#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "../cli/log.h"
#include "../src/cai_chrome_host.h"
#include "../src/cai_internal.h"
#include <assert.h>
#include <cai/quota.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Replace quota/catalog transport and optionally inject a pslog warning. The
 * application, encrypted storage, runtime, prompt, watch dispatch and renderer
 * are the production implementations. */
static int blocked_quota(cai_client *client, cai_chatgpt_quota *out,
                         cai_error *error) {
  int started = atoi(getenv("CAI_TEST_QUOTA_STARTED"));
  int release = atoi(getenv("CAI_TEST_QUOTA_RELEASE"));
  char signal = 'Q';
  const char *expected = getenv("CAI_TEST_QUOTA_TIMEOUT_MS");
  const char *bundle = getenv("CAI_TEST_QUOTA_CA_BUNDLE");
  const char *directory = getenv("CAI_TEST_QUOTA_CA_PATH");
  assert(CAI_CLIENT_IMPL(client)->timeout_ms ==
         (expected != NULL ? strtol(expected, NULL, 10) : 10000L));
  assert(bundle != NULL
             ? CAI_CLIENT_IMPL(client)->ca_bundle_path != NULL &&
                   strcmp(CAI_CLIENT_IMPL(client)->ca_bundle_path, bundle) == 0
             : CAI_CLIENT_IMPL(client)->ca_bundle_path == NULL);
  assert(directory != NULL
             ? CAI_CLIENT_IMPL(client)->ca_path != NULL &&
                   strcmp(CAI_CLIENT_IMPL(client)->ca_path, directory) == 0
             : CAI_CLIENT_IMPL(client)->ca_path == NULL);
  expected = getenv("CAI_TEST_QUOTA_INSECURE");
  assert(CAI_CLIENT_IMPL(client)->insecure_skip_verify ==
         (expected != NULL ? atoi(expected) : 0));
  expected = getenv("CAI_TEST_QUOTA_HTTP2_DISABLED");
  assert(CAI_CLIENT_IMPL(client)->http_2_disabled ==
         (expected != NULL ? atoi(expected) : 0));
  (void)error;
  if (write(started, &signal, 1U) != 1 || read(release, &signal, 1U) != 1)
    return CAI_ERR_TRANSPORT;
  if (signal == 'E')
    return CAI_ERR_TRANSPORT;
  out->has_weekly = 1;
  out->weekly_remaining_percent = 88;
  out->has_credit_balance = 1;
  out->credit_balance = 20.5;
  return CAI_OK;
}

int cai_test_list_models(cai_client *, cai_model_catalog_refresh_strategy,
                         cai_model_catalog **, cai_error *);
int cai_test_list_models(cai_client *client,
                         cai_model_catalog_refresh_strategy strategy,
                         cai_model_catalog **out, cai_error *error) {
  int started = atoi(getenv("CAI_TEST_QUOTA_STARTED"));
  char signal = 'C';
  (void)client;
  (void)strategy;
  (void)error;
  *out = NULL;
  (void)write(started, &signal, 1U);
  return CAI_ERR_TRANSPORT;
}

static int fixture_log_interactive(cai_cli_log *log, const char *directory,
                                   cai_error *error) {
  int rc = cai_cli_log_interactive_at(log, directory, error);
  const char *message = getenv("CAI_TEST_NOTICE_MESSAGE");
  if (rc == CAI_OK && message != NULL)
    log->logger->warn(log->logger, message, NULL, 0U);
  return rc;
}

#define cai_cli_log_interactive_at fixture_log_interactive
#define cai_client_list_models cai_test_list_models
#define cai_client_chatgpt_quota blocked_quota
#include "../cli/app_run.c"
#undef cai_client_chatgpt_quota
#undef cai_client_list_models
#undef cai_cli_log_interactive_at

int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "--test-chrome-system") == 0) {
    const char *paths[1];
    cai_error error;
    int rc;
    paths[0] = argv[2];
    cai_error_init(&error);
    rc = cai_cli_chrome_check_system_registration(paths, 1U, &error);
    if (rc != CAI_OK)
      fprintf(stderr, "%s: %s\n", error.message, error.detail);
    cai_error_cleanup(&error);
    return rc == CAI_OK ? 0 : 1;
  }
  return cai_cli_app_main(argc, argv, NULL);
}
