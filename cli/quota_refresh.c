#define _POSIX_C_SOURCE 200809L
#include "quota_refresh.h"
#include "pouch_stream.h"
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

int cai_cli_quota_due(int attempted, struct timespec last, struct timespec now,
                      int force) {
  time_t elapsed;
  if (force || !attempted || now.tv_sec < last.tv_sec)
    return 1;
  elapsed = now.tv_sec - last.tv_sec;
  return elapsed > 300 || (elapsed == 300 && now.tv_nsec >= last.tv_nsec);
}

static void *quota_worker(void *context) {
  cai_cli_quota_refresh *refresh = context;
  pthread_mutex_lock(&refresh->lock);
  for (;;) {
    cai_chatgpt_quota fresh;
    cai_error error;
    uint64_t signal = 1U;
    while (!refresh->stopping && refresh->completed == refresh->requested)
      pthread_cond_wait(&refresh->condition, &refresh->lock);
    if (refresh->stopping)
      break;
    pthread_mutex_unlock(&refresh->lock);
    memset(&fresh, 0, sizeof(fresh));
    cai_error_init(&error);
    if (refresh->fetch(refresh->context, &fresh, &error) != CAI_OK)
      memset(&fresh, 0, sizeof(fresh));
    cai_error_cleanup(&error);
    pthread_mutex_lock(&refresh->lock);
    refresh->result = fresh;
    refresh->completed++;
    pthread_cond_broadcast(&refresh->condition);
    while (write(refresh->fd, &signal, sizeof(signal)) < 0 && errno == EINTR) {
    }
  }
  pthread_mutex_unlock(&refresh->lock);
  return NULL;
}

int cai_cli_quota_open(cai_cli_quota_refresh *refresh,
                       cai_cli_quota_fetch_fn fetch, void *context,
                       cai_error *error) {
  if (refresh == NULL || fetch == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "quota worker and fetch callback are required");
  memset(refresh, 0, sizeof(*refresh));
  refresh->fd = eventfd(0U, EFD_NONBLOCK | EFD_CLOEXEC);
  if (refresh->fd < 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "create quota wakeup fd");
  if (pthread_mutex_init(&refresh->lock, NULL) != 0) {
    close(refresh->fd);
    refresh->fd = -1;
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "initialize quota lock");
  }
  if (pthread_cond_init(&refresh->condition, NULL) != 0) {
    pthread_mutex_destroy(&refresh->lock);
    close(refresh->fd);
    refresh->fd = -1;
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "initialize quota condition");
  }
  refresh->fetch = fetch;
  refresh->context = context;
  if (pthread_create(&refresh->thread, NULL, quota_worker, refresh) != 0) {
    pthread_cond_destroy(&refresh->condition);
    pthread_mutex_destroy(&refresh->lock);
    close(refresh->fd);
    refresh->fd = -1;
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "start quota worker");
  }
  refresh->started = 1;
  return CAI_OK;
}

void cai_cli_quota_request(cai_cli_quota_refresh *refresh, int force,
                           int wait) {
  struct timespec now;
  unsigned long long ticket;
  if (!refresh->started || clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return;
  pthread_mutex_lock(&refresh->lock);
  if (!refresh->stopping &&
      (force || refresh->requested == refresh->completed) &&
      cai_cli_quota_due(refresh->attempted, refresh->last_attempt, now,
                        force)) {
    refresh->attempted = 1;
    refresh->last_attempt = now;
    refresh->requested++;
    pthread_cond_broadcast(&refresh->condition);
  }
  ticket = refresh->requested;
  while (wait && !refresh->stopping && refresh->completed < ticket)
    pthread_cond_wait(&refresh->condition, &refresh->lock);
  pthread_mutex_unlock(&refresh->lock);
}

int cai_cli_quota_take(cai_cli_quota_refresh *refresh, cai_chatgpt_quota *out) {
  uint64_t signal;
  int changed;
  if (!refresh->started)
    return 0;
  while (read(refresh->fd, &signal, sizeof(signal)) < 0 && errno == EINTR) {
  }
  pthread_mutex_lock(&refresh->lock);
  changed = refresh->completed != refresh->consumed;
  if (changed) {
    *out = refresh->result;
    refresh->consumed = refresh->completed;
  }
  pthread_mutex_unlock(&refresh->lock);
  return changed;
}

void cai_cli_quota_close(cai_cli_quota_refresh *refresh) {
  if (!refresh->started)
    return;
  pthread_mutex_lock(&refresh->lock);
  refresh->stopping = 1;
  pthread_cond_broadcast(&refresh->condition);
  pthread_mutex_unlock(&refresh->lock);
  pthread_join(refresh->thread, NULL);
  close(refresh->fd);
  pthread_cond_destroy(&refresh->condition);
  pthread_mutex_destroy(&refresh->lock);
  refresh->started = 0;
  refresh->fd = -1;
}
