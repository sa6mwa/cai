#ifndef CAI_CLI_QUOTA_REFRESH_H
#define CAI_CLI_QUOTA_REFRESH_H

#include <cai/quota.h>
#include <pthread.h>
#include <time.h>

/* A sleeping worker: only explicit startup, turn completion and /status
 * requests cause network activity. Results are consumed on the UI thread. */
typedef int (*cai_cli_quota_fetch_fn)(void *, cai_chatgpt_quota *, cai_error *);
typedef struct cai_cli_quota_refresh {
  pthread_t thread;
  pthread_mutex_t lock;
  pthread_cond_t condition;
  cai_cli_quota_fetch_fn fetch;
  void *context;
  cai_chatgpt_quota result;
  struct timespec last_attempt;
  unsigned long long requested;
  unsigned long long completed;
  unsigned long long consumed;
  int fd;
  int started;
  int stopping;
  int attempted;
} cai_cli_quota_refresh;

int cai_cli_quota_due(int attempted, struct timespec last, struct timespec now,
                      int force);
int cai_cli_quota_open(cai_cli_quota_refresh *refresh,
                       cai_cli_quota_fetch_fn fetch, void *context,
                       cai_error *error);
/* Normal requests coalesce and debounce; forced requests always fetch anew.
 * wait is used only for /status, which must display its requested result. */
void cai_cli_quota_request(cai_cli_quota_refresh *refresh, int force, int wait);
int cai_cli_quota_take(cai_cli_quota_refresh *refresh, cai_chatgpt_quota *out);
void cai_cli_quota_close(cai_cli_quota_refresh *refresh);

#endif
