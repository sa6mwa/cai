#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <cai/quota.h>
#include <stdlib.h>
#include <unistd.h>

/* Replace only quota transport. The application, encrypted storage, runtime,
 * prompt, watch dispatch and renderer are the production implementations. */
static int blocked_quota(cai_client *client, cai_chatgpt_quota *out,
                         cai_error *error) {
  int started = atoi(getenv("CAI_TEST_QUOTA_STARTED"));
  int release = atoi(getenv("CAI_TEST_QUOTA_RELEASE"));
  char signal = 'Q';
  (void)client;
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

#define cai_client_list_models cai_test_list_models
#define cai_client_chatgpt_quota blocked_quota
#define main cai_cli_app_main
int cai_cli_app_main(int argc, char **argv);
#include "../cli/main.c"
#undef main
#undef cai_client_chatgpt_quota
#undef cai_client_list_models

int main(int argc, char **argv) { return cai_cli_app_main(argc, argv); }
