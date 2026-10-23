#define _POSIX_C_SOURCE 200809L
#include "app_internal.h"
#include "pouch_stream.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

static void free_string(const char *string) {
  void *owned;
  memcpy(&owned, &string, sizeof(owned));
  free(owned);
}

void cai_app_status_init(cai_app_status *s) {
  if (s != NULL)
    memset(s, 0, sizeof(*s));
}
void cai_app_status_cleanup(cai_app_status *s) {
  if (s == NULL)
    return;
  free_string(s->session_id);
  free_string(s->effective.model);
  free_string(s->effective.reasoning_effort);
  free_string(s->effective.reasoning_summary);
  free_string(s->effective.service_tier);
  free_string(s->pending.model);
  free_string(s->pending.reasoning_effort);
  free_string(s->pending.reasoning_summary);
  free_string(s->pending.service_tier);
  free_string(s->goal.objective);
  free_string(s->goal.status);
  cai_error_cleanup(&s->error);
  cai_error_cleanup(&s->control_error);
  cai_app_status_init(s);
}
static int error_copy(cai_error *to, const cai_error *from) {
  cai_error copy;
  cai_error_init(&copy);
  if (from != NULL) {
    copy.code = from->code;
    copy.http_status = from->http_status;
#define COPY_ERROR(field)                                                      \
  if (from->field != NULL && (copy.field = strdup(from->field)) == NULL)       \
  goto fail
    COPY_ERROR(message);
    COPY_ERROR(detail);
    COPY_ERROR(server_code);
    COPY_ERROR(request_id);
#undef COPY_ERROR
  }
  cai_error_cleanup(to);
  *to = copy;
  return CAI_OK;
fail:
  cai_error_cleanup(&copy);
  return CAI_ERR_NOMEM;
}
static int status_copy(cai_app_status *to, const cai_app_status *from,
                       cai_error *error) {
  cai_app_status copy = *from;
  copy.session_id = NULL;
  memset(&copy.effective, 0, sizeof(copy.effective));
  memset(&copy.pending, 0, sizeof(copy.pending));
  copy.goal.objective = copy.goal.status = NULL;
  cai_error_init(&copy.error);
  cai_error_init(&copy.control_error);
#define COPY_STRING(field)                                                     \
  if (from->field != NULL && (copy.field = strdup(from->field)) == NULL)       \
  goto fail
  COPY_STRING(session_id);
  COPY_STRING(effective.model);
  COPY_STRING(effective.reasoning_effort);
  COPY_STRING(effective.reasoning_summary);
  COPY_STRING(effective.service_tier);
  COPY_STRING(pending.model);
  COPY_STRING(pending.reasoning_effort);
  COPY_STRING(pending.reasoning_summary);
  COPY_STRING(pending.service_tier);
  COPY_STRING(goal.objective);
  COPY_STRING(goal.status);
#undef COPY_STRING
  copy.effective.present = from->effective.present;
  copy.pending.present = from->pending.present;
  if (error_copy(&copy.error, &from->error) != CAI_OK ||
      error_copy(&copy.control_error, &from->control_error) != CAI_OK)
    goto fail;
  cai_app_status_cleanup(to);
  *to = copy;
  return CAI_OK;
fail:
  cai_app_status_cleanup(&copy);
  return cai_cli_error(error, CAI_ERR_NOMEM, "copy application status");
}
int cai_app_bridge_open(cai_app_bridge **out, const cai_app_config *config,
                        cai_error *error) {
  cai_app_bridge *b = calloc(1U, sizeof(*b));
  *out = NULL;
  if (b == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate host controls");
  if (pthread_mutex_init(&b->lock, NULL) != 0) {
    free(b);
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "initialize host controls");
  }
  b->fd = eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE);
  if (b->fd < 0) {
    pthread_mutex_destroy(&b->lock);
    free(b);
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "open host control wakeup");
  }
  b->config = config;
  *out = b;
  return CAI_OK;
}
void cai_app_bridge_begin(cai_app_bridge *b) {
  uint64_t value;
  pthread_mutex_lock(&b->lock);
  while (read(b->fd, &value, sizeof(value)) > 0) {
  }
  cai_app_status_cleanup(&b->status);
  b->status.running = 1;
  b->status.outcome = CAI_APP_RUNNING;
  b->accepting = 1;
  b->next_id = 0U;
  pthread_mutex_unlock(&b->lock);
}
int cai_app_bridge_control(cai_app_bridge *b, cai_app_control_type type,
                           const char *text, unsigned long long *id,
                           cai_error *error) {
  cai_app_command *c;
  uint64_t value = 1U;
  int needs_text = type == CAI_APP_QUEUE_PROMPT || type == CAI_APP_STEER ||
                   type == CAI_APP_REPLACE_GOAL;
  if (id != NULL)
    *id = 0U;
  if (b == NULL || id == NULL || type < CAI_APP_QUEUE_PROMPT ||
      type > CAI_APP_CLEAR_GOAL ||
      (needs_text ? text == NULL || text[0] == '\0' : text != NULL))
    return cai_cli_error(error, CAI_ERR_INVALID, "invalid host control");
  c = calloc(1U, sizeof(*c));
  if (c == NULL || (needs_text && (c->text = strdup(text)) == NULL)) {
    free(c);
    return cai_cli_error(error, CAI_ERR_NOMEM, "copy host control");
  }
  c->type = type;
  pthread_mutex_lock(&b->lock);
  if (!b->accepting || (!b->status.runtime_ready && type != CAI_APP_STOP) ||
      b->count == 64U) {
    pthread_mutex_unlock(&b->lock);
    free(c->text);
    free(c);
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "host control requires a ready run and queue space");
  }
  /* Signal while holding the queue lock: the owner cannot consume the wakeup
   * before the corresponding command becomes visible. */
  while (write(b->fd, &value, sizeof(value)) < 0) {
    if (errno == EINTR)
      continue;
    if (errno == EAGAIN)
      break;
    pthread_mutex_unlock(&b->lock);
    free(c->text);
    free(c);
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "wake host control queue");
  }
  *id = c->id = ++b->next_id;
  if (b->tail != NULL)
    b->tail->next = c;
  else
    b->head = c;
  b->tail = c;
  b->count++;
  pthread_mutex_unlock(&b->lock);
  return CAI_OK;
}
cai_app_command *cai_app_bridge_take(cai_app_bridge *b) {
  cai_app_command *c;
  uint64_t value;
  if (b == NULL)
    return NULL;
  pthread_mutex_lock(&b->lock);
  c = b->head;
  if (c != NULL) {
    while (read(b->fd, &value, sizeof(value)) < 0 && errno == EINTR) {
    }
    b->head = c->next;
    if (b->head == NULL)
      b->tail = NULL;
    b->count--;
  }
  pthread_mutex_unlock(&b->lock);
  return c;
}
void cai_app_bridge_ack(cai_app_bridge *b, cai_app_command *c, int status,
                        const cai_error *error) {
  pthread_mutex_lock(&b->lock);
  b->status.control_id = c->id;
  b->status.control_status = status;
  if (error_copy(&b->status.control_error, error) != CAI_OK)
    b->status.control_error.code = CAI_ERR_NOMEM;
  pthread_mutex_unlock(&b->lock);
  if (b->config->control_callback != NULL)
    b->config->control_callback(b->config->control_context, c->id, status,
                                error);
  free(c->text);
  free(c);
}
int cai_app_bridge_status(cai_app_bridge *b, cai_app_status *out,
                          cai_error *error) {
  int rc;
  if (out == NULL || b == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID, "status output is required");
  pthread_mutex_lock(&b->lock);
  rc = status_copy(out, &b->status, error);
  pthread_mutex_unlock(&b->lock);
  return rc;
}
int cai_app_bridge_publish(cai_app_bridge *b, cai_agent_runtime *runtime,
                           cai_error *error) {
  cai_app_status view, copy;
  int rc;
  if (b == NULL)
    return CAI_OK;
  cai_app_status_init(&view);
  cai_app_status_init(&copy);
  if (runtime != NULL) {
    view.session_id = cai_agent_runtime_session_id(runtime);
    rc = cai_agent_runtime_state(runtime, &view.state, error);
    if (rc == CAI_OK)
      rc = cai_agent_runtime_get_settings(runtime, &view.effective,
                                          &view.pending, error);
    if (rc == CAI_OK)
      rc = cai_agent_runtime_get_goal(runtime, &view.goal, error);
    if (rc == CAI_OK)
      rc = cai_agent_runtime_get_metrics(runtime, &view.metrics, error);
    if (rc == CAI_OK)
      rc = status_copy(&copy, &view, error);
    if (rc != CAI_OK)
      return rc;
  }
  pthread_mutex_lock(&b->lock);
  if (runtime != NULL) {
    copy.running = b->status.running;
    copy.runtime_ready = 1;
    copy.outcome = b->status.outcome;
    copy.control_id = b->status.control_id;
    copy.control_status = b->status.control_status;
    copy.error = b->status.error;
    copy.control_error = b->status.control_error;
    cai_error_init(&b->status.error);
    cai_error_init(&b->status.control_error);
    cai_app_status_cleanup(&b->status);
    b->status = copy;
  } else {
    b->status.runtime_ready = 0;
  }
  pthread_mutex_unlock(&b->lock);
  return CAI_OK;
}
void cai_app_bridge_error(cai_app_bridge *b, const cai_error *error) {
  if (b == NULL || error == NULL || error->code == CAI_OK)
    return;
  pthread_mutex_lock(&b->lock);
  if (error_copy(&b->status.error, error) != CAI_OK)
    b->status.error.code = CAI_ERR_NOMEM;
  pthread_mutex_unlock(&b->lock);
}
void cai_app_bridge_outcome(cai_app_bridge *b, cai_app_outcome outcome) {
  if (b != NULL) {
    pthread_mutex_lock(&b->lock);
    b->status.outcome = outcome;
    if (outcome == CAI_APP_RUNNING)
      cai_error_cleanup(&b->status.error);
    pthread_mutex_unlock(&b->lock);
  }
}
void cai_app_bridge_end(cai_app_bridge *b, int code, const cai_error *error) {
  cai_app_command *c;
  cai_error abandoned;
  cai_app_bridge_error(b, error);
  pthread_mutex_lock(&b->lock);
  b->accepting = 0;
  b->status.running = b->status.runtime_ready = 0;
  b->status.exit_code = code;
  if (error != NULL && error->code != CAI_OK)
    b->status.outcome = CAI_APP_FAILED;
  else if (code != 0 && b->status.outcome != CAI_APP_CANCELLED &&
           b->status.outcome != CAI_APP_STOPPED)
    b->status.outcome = CAI_APP_FAILED;
  else if (b->status.outcome == CAI_APP_RUNNING)
    b->status.outcome = CAI_APP_COMPLETED;
  if (b->status.outcome == CAI_APP_FAILED && b->status.error.code == CAI_OK)
    cai_cli_error(&b->status.error, CAI_ERR_INVALID, "application failed");
  pthread_mutex_unlock(&b->lock);
  cai_error_init(&abandoned);
  cai_cli_error(&abandoned, CAI_ERR_INVALID,
                "application ended before host control could apply");
  while ((c = cai_app_bridge_take(b)) != NULL)
    cai_app_bridge_ack(b, c, CAI_ERR_INVALID, &abandoned);
  cai_error_cleanup(&abandoned);
}
void cai_app_bridge_close(cai_app_bridge *b) {
  if (b == NULL)
    return;
  cai_app_status_cleanup(&b->status);
  close(b->fd);
  pthread_mutex_destroy(&b->lock);
  free(b);
}
