#define _POSIX_C_SOURCE 200809L
#include "../cli/quota_refresh.h"
#include <assert.h>
#include <poll.h>
#include <string.h>

typedef struct fixture {
  pthread_mutex_t lock;
  pthread_cond_t condition;
  int calls;
  int released;
  int fail;
} fixture;

static int fetch(void *context, cai_chatgpt_quota *out, cai_error *error) {
  fixture *f = context;
  int call;
  int fail;
  (void)error;
  pthread_mutex_lock(&f->lock);
  call = ++f->calls;
  pthread_cond_broadcast(&f->condition);
  while (f->released < call)
    pthread_cond_wait(&f->condition, &f->lock);
  fail = f->fail;
  pthread_mutex_unlock(&f->lock);
  out->has_credit_balance = 1;
  out->credit_balance = call;
  return fail ? CAI_ERR_TRANSPORT : CAI_OK;
}

static void wait_call(fixture *f, int call) {
  pthread_mutex_lock(&f->lock);
  while (f->calls < call)
    pthread_cond_wait(&f->condition, &f->lock);
  assert(f->calls == call);
  pthread_mutex_unlock(&f->lock);
}

static void release_call(fixture *f, int call) {
  pthread_mutex_lock(&f->lock);
  f->released = call;
  pthread_cond_broadcast(&f->condition);
  pthread_mutex_unlock(&f->lock);
}

static void *force(void *context) {
  cai_cli_quota_request(context, 1, 1);
  return NULL;
}

static void *close_refresh(void *context) {
  cai_cli_quota_close(context);
  return NULL;
}

int main(void) {
  fixture f;
  cai_cli_quota_refresh refresh;
  cai_chatgpt_quota out;
  cai_error error;
  pthread_t forced;
  pthread_t closing;
  struct pollfd fd;
  struct timespec start = {100, 500000000L};
  struct timespec before = {400, 499999999L};
  struct timespec due = {400, 500000000L};
  int i;
  assert(cai_cli_quota_due(0, start, start, 0));
  assert(!cai_cli_quota_due(1, start, before, 0));
  assert(cai_cli_quota_due(1, start, due, 0));
  assert(cai_cli_quota_due(1, start, before, 1));
  memset(&f, 0, sizeof(f));
  assert(pthread_mutex_init(&f.lock, NULL) == 0);
  assert(pthread_cond_init(&f.condition, NULL) == 0);
  cai_error_init(&error);
  assert(cai_cli_quota_open(&refresh, fetch, &f, &error) == CAI_OK);
  fd.fd = refresh.fd;
  fd.events = POLLIN;
  /* An idle worker makes no request, and startup returns with fetch blocked. */
  assert(poll(&fd, 1U, 20) == 0);
  assert(f.calls == 0);
  cai_cli_quota_request(&refresh, 0, 0);
  wait_call(&f, 1);
  for (i = 0; i < 100; i++)
    cai_cli_quota_request(&refresh, 0, 0);
  assert(poll(&fd, 1U, 0) == 0);
  assert(!cai_cli_quota_take(&refresh, &out));
  /* /status queues its own request even if startup is still in flight. */
  assert(pthread_create(&forced, NULL, force, &refresh) == 0);
  release_call(&f, 1);
  wait_call(&f, 2);
  release_call(&f, 2);
  pthread_join(forced, NULL);
  assert(cai_cli_quota_take(&refresh, &out));
  assert(out.has_credit_balance && out.credit_balance == 2.0);
  assert(!cai_cli_quota_take(&refresh, &out));
  for (i = 0; i < 100; i++)
    cai_cli_quota_request(&refresh, 0, 0);
  assert(poll(&fd, 1U, 20) == 0);
  pthread_mutex_lock(&f.lock);
  assert(f.calls == 2);
  f.fail = 1;
  pthread_mutex_unlock(&f.lock);
  release_call(&f, 3);
  cai_cli_quota_request(&refresh, 1, 1);
  assert(cai_cli_quota_take(&refresh, &out));
  assert(!out.has_credit_balance);
  /* Failures are debounced too, but explicit /status always tries again. */
  cai_cli_quota_request(&refresh, 0, 0);
  assert(poll(&fd, 1U, 20) == 0);
  pthread_mutex_lock(&refresh.lock);
  refresh.last_attempt.tv_sec -= 301;
  pthread_mutex_unlock(&refresh.lock);
  cai_cli_quota_request(&refresh, 0, 0);
  wait_call(&f, 4);
  /* Teardown joins an outstanding request before its client can be destroyed.
   */
  assert(pthread_create(&closing, NULL, close_refresh, &refresh) == 0);
  release_call(&f, 4);
  pthread_join(closing, NULL);
  assert(!refresh.started && refresh.fd == -1);
  cai_cli_quota_close(&refresh);
  pthread_cond_destroy(&f.condition);
  pthread_mutex_destroy(&f.lock);
  cai_error_cleanup(&error);
  return 0;
}
