#include <cai/agent_runtime.h>
#include <cai/smith.h>
#include <cai/tools/goal.h>
#include <cai/tools/patch.h>
#include <cai/tools/read.h>
#include <cai/tools/view_image.h>

#include "cai_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern char *realpath(const char *path, char *resolved_path);

#define CAI_RUNTIME_DEFAULT_EVENT_LIMIT 256U
#define CAI_RUNTIME_DEFAULT_STEERING_LIMIT 32U

typedef struct cai_runtime_event_node {
  cai_agent_runtime_event event;
  char *data;
  char *tool_name;
  char *tool_path;
  char *tool_call_id;
  char *terminal_id;
  struct cai_runtime_event_node *next;
} cai_runtime_event_node;

typedef struct cai_runtime_path_doc {
  char *path;
} cai_runtime_path_doc;

static const lonejson_field cai_runtime_path_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_runtime_path_doc, path, "path")};
LONEJSON_MAP_DEFINE(cai_runtime_path_map, cai_runtime_path_doc,
                    cai_runtime_path_fields);

typedef struct cai_runtime_input_node {
  char *text;
  unsigned long long journal_sequence;
  struct cai_runtime_input_node *next;
} cai_runtime_input_node;

struct cai_agent_runtime {
  pthread_t owner_thread;
  pthread_t worker_thread;
  pthread_mutex_t lock;
  pthread_cond_t condition;
  int wakeup_read_fd;
  int wakeup_write_fd;
  int worker_started;
  int stopping;
  int pumping;
  int close_deferred;
  int destroying;
  size_t close_waiters;
  cai_client *client;
  cai_agent *agent;
  cai_session *session;
  cai_agent_session_store local_store;
  const cai_agent_session_store *session_store;
  int owns_local_store;
  char *session_scope;
  char *session_id;
  unsigned long long applied_event_sequence;
  unsigned long long next_event_sequence;
  int resume_compaction_pending;
  int accepting_steering;
  cai_agent_run_state state;
  unsigned long long next_sequence;
  size_t event_limit;
  size_t event_count;
  cai_runtime_event_node *event_head;
  cai_runtime_event_node *event_tail;
  size_t steering_limit;
  size_t steering_count;
  cai_runtime_input_node *turn_head;
  cai_runtime_input_node *turn_tail;
  cai_runtime_input_node *steering_head;
  cai_runtime_input_node *steering_tail;
  cai_agent_runtime_event_fn event_callback;
  void *event_context;
  cai_terminal_event_fn terminal_event_callback;
  void *terminal_event_context;
  char *terminal_origin_tool_call_id;
  int review_mode;
};

static pthread_mutex_t cai_runtime_session_id_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long cai_runtime_session_id_counter = 0U;

static void cai_agent_runtime_destroy(cai_agent_runtime *runtime);

static int cai_runtime_local_session_id_valid(const char *session_id) {
  const unsigned char *cursor;

  if (session_id == NULL || session_id[0] == '\0' || strlen(session_id) > 128U) {
    return 0;
  }
  for (cursor = (const unsigned char *)session_id; *cursor != '\0'; cursor++) {
    if (!((*cursor >= 'a' && *cursor <= 'z') ||
          (*cursor >= 'A' && *cursor <= 'Z') ||
          (*cursor >= '0' && *cursor <= '9') || *cursor == '-' ||
          *cursor == '_')) {
      return 0;
    }
  }
  return 1;
}

static int cai_runtime_wakeup_open(cai_agent_runtime *runtime,
                                   cai_error *error) {
  int fds[2] = {-1, -1};

  if (pipe(fds) != 0 || fcntl(fds[0], F_SETFL, O_NONBLOCK) != 0 ||
      fcntl(fds[1], F_SETFL, O_NONBLOCK) != 0 ||
      fcntl(fds[0], F_SETFD, FD_CLOEXEC) != 0 ||
      fcntl(fds[1], F_SETFD, FD_CLOEXEC) != 0) {
    if (fds[0] >= 0) {
      close(fds[0]);
    }
    if (fds[1] >= 0) {
      close(fds[1]);
    }
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to initialize agent runtime wakeup pipe");
  }
  runtime->wakeup_read_fd = fds[0];
  runtime->wakeup_write_fd = fds[1];
  return CAI_OK;
}

static void cai_runtime_wakeup_notify_locked(cai_agent_runtime *runtime) {
  unsigned char byte;

  if (runtime->wakeup_write_fd < 0) {
    return;
  }
  byte = 1U;
  while (write(runtime->wakeup_write_fd, &byte, sizeof(byte)) < 0 &&
         errno == EINTR) {
  }
}

static void cai_runtime_wakeup_drain_locked(cai_agent_runtime *runtime) {
  unsigned char buffer[128];

  if (runtime->wakeup_read_fd < 0) {
    return;
  }
  while (read(runtime->wakeup_read_fd, buffer, sizeof(buffer)) > 0) {
  }
}

static int cai_runtime_owner(const cai_agent_runtime *runtime,
                             cai_error *error) {
  if (runtime == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent runtime is required");
  }
  if (!pthread_equal(runtime->owner_thread, pthread_self())) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "agent runtime operation requires its owner thread");
  }
  return CAI_OK;
}

static void cai_runtime_event_node_free(cai_runtime_event_node *node) {
  if (node == NULL) {
    return;
  }
  cai_free_mem(NULL, node->data);
  cai_free_mem(NULL, node->tool_name);
  cai_free_mem(NULL, node->tool_path);
  cai_free_mem(NULL, node->tool_call_id);
  cai_free_mem(NULL, node->terminal_id);
  cai_free_mem(NULL, node);
}

static void cai_runtime_input_node_free(cai_runtime_input_node *node) {
  if (node == NULL) {
    return;
  }
  cai_free_mem(NULL, node->text);
  cai_free_mem(NULL, node);
}

static int cai_runtime_wait_event_capacity_locked(cai_agent_runtime *runtime,
                                                  cai_error *error) {
  while (!runtime->stopping && runtime->event_count >= runtime->event_limit) {
    pthread_cond_wait(&runtime->condition, &runtime->lock);
  }
  if (runtime->stopping) {
    return cai_set_error(error, CAI_ERR_CANCELLED, "agent runtime is closing");
  }
  return CAI_OK;
}

static int cai_runtime_require_event_capacity_locked(cai_agent_runtime *runtime,
                                                     cai_error *error) {
  if (runtime->stopping) {
    return cai_set_error(error, CAI_ERR_CANCELLED, "agent runtime is closing");
  }
  if (runtime->event_count >= runtime->event_limit) {
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "agent runtime event queue is full");
  }
  return CAI_OK;
}

static int cai_runtime_event_node_new(int type, const char *data,
                                      size_t data_length, const char *tool_name,
                                      const char *tool_call_id,
                                      cai_agent_run_state state,
                                      cai_runtime_event_node **out,
                                      cai_error *error) {
  cai_runtime_event_node *node;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "agent runtime event output is required");
  }
  *out = NULL;
  node = (cai_runtime_event_node *)cai_alloc(NULL, sizeof(*node));
  if (node == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate agent runtime event");
  }
  memset(node, 0, sizeof(*node));
  if (data != NULL && data_length > 0U) {
    node->data = cai_strndup(NULL, data, data_length);
    if (node->data == NULL) {
      cai_runtime_event_node_free(node);
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to copy agent runtime event data");
    }
  }
  if (tool_name != NULL) {
    node->tool_name = cai_strdup(NULL, tool_name);
    if (node->tool_name == NULL) {
      cai_runtime_event_node_free(node);
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to copy agent runtime tool name");
    }
  }
  if (tool_call_id != NULL) {
    node->tool_call_id = cai_strdup(NULL, tool_call_id);
    if (node->tool_call_id == NULL) {
      cai_runtime_event_node_free(node);
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to copy agent runtime tool call id");
    }
  }
  node->event.type = type;
  node->event.state = state;
  node->event.data = node->data;
  node->event.data_length = data_length;
  node->event.tool_name = node->tool_name;
  node->event.tool_call_id = node->tool_call_id;
  *out = node;
  return CAI_OK;
}

static void cai_runtime_append_event_node_locked(cai_agent_runtime *runtime,
                                                 cai_runtime_event_node *node) {
  node->event.sequence = ++runtime->next_sequence;
  if (runtime->event_tail == NULL) {
    runtime->event_head = node;
  } else {
    runtime->event_tail->next = node;
  }
  runtime->event_tail = node;
  runtime->event_count++;
  cai_runtime_wakeup_notify_locked(runtime);
  pthread_cond_broadcast(&runtime->condition);
}

static int cai_runtime_enqueue_locked(cai_agent_runtime *runtime, int type,
                                      const char *data, size_t data_length,
                                      const char *tool_name,
                                      const char *tool_call_id,
                                      cai_agent_run_state state,
                                      cai_error *error) {
  cai_runtime_event_node *node;
  int rc;

  rc = cai_runtime_wait_event_capacity_locked(runtime, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_runtime_event_node_new(type, data, data_length, tool_name,
                                  tool_call_id, state, &node, error);
  if (rc != CAI_OK) {
    return rc;
  }
  cai_runtime_append_event_node_locked(runtime, node);
  return CAI_OK;
}

static int cai_runtime_enqueue_tool_locked(cai_agent_runtime *runtime, int type,
                                           const char *data, size_t data_length,
                                           const char *tool_name,
                                           const char *tool_call_id,
                                           int tool_action,
                                           const char *tool_path,
                                           size_t tool_path_count,
                                           cai_agent_run_state state,
                                           cai_error *error) {
  cai_runtime_event_node *node;
  int rc;

  rc = cai_runtime_wait_event_capacity_locked(runtime, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_runtime_event_node_new(type, data, data_length, tool_name,
                                  tool_call_id, state, &node, error);
  if (rc != CAI_OK) {
    return rc;
  }
  node->event.tool_action = tool_action;
  node->event.tool_path_count = tool_path_count;
  if (tool_path != NULL) {
    node->tool_path = cai_strdup(NULL, tool_path);
    if (node->tool_path == NULL) {
      cai_runtime_event_node_free(node);
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to copy agent runtime tool path");
    }
    node->event.tool_path = node->tool_path;
  }
  cai_runtime_append_event_node_locked(runtime, node);
  return CAI_OK;
}

static int cai_runtime_enqueue_terminal_locked(cai_agent_runtime *runtime,
                                               int type,
                                               const cai_terminal_event *event,
                                               cai_error *error) {
  cai_runtime_event_node *node;
  int rc;

  if (event->terminal_id == NULL || event->terminal_id[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "terminal lifecycle event has no terminal id");
  }
  rc = cai_runtime_wait_event_capacity_locked(runtime, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_runtime_event_node_new(type, event->output, event->output_length,
                                  NULL, runtime->terminal_origin_tool_call_id,
                                  runtime->state,
                                  &node, error);
  if (rc == CAI_OK) {
    node->terminal_id = cai_strdup(NULL, event->terminal_id);
    if (node->terminal_id == NULL) {
      cai_runtime_event_node_free(node);
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to copy agent runtime terminal id");
    }
    node->event.terminal_id = node->terminal_id;
    node->event.terminal_command_id = event->command_id;
    node->event.terminal_has_exit_code = event->has_exit_code;
    node->event.terminal_exit_code = event->exit_code;
    node->event.terminal_has_signal = event->has_signal;
    node->event.terminal_signal = event->signal;
    node->event.terminal_duration_ms = event->duration_ms;
    node->event.terminal_total_output_bytes = event->total_output_bytes;
    node->event.terminal_output_truncated = event->output_truncated;
    node->event.terminal_detached_processes_possible =
        event->detached_processes_possible;
    cai_runtime_append_event_node_locked(runtime, node);
  }
  return rc;
}

static void cai_runtime_set_state(cai_agent_runtime *runtime,
                                  cai_agent_run_state state) {
  cai_error ignored;

  cai_error_init(&ignored);
  pthread_mutex_lock(&runtime->lock);
  runtime->state = state;
  (void)cai_runtime_enqueue_locked(runtime, CAI_AGENT_EVENT_RUN_STATE_CHANGED,
                                   NULL, 0U, NULL, NULL, state, &ignored);
  pthread_cond_broadcast(&runtime->condition);
  pthread_mutex_unlock(&runtime->lock);
  cai_error_cleanup(&ignored);
}

static int cai_runtime_copy_string(const char *value, char **out,
                                   cai_error *error) {
  *out = NULL;
  if (value == NULL || value[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "agent runtime string is required");
  }
  *out = cai_strdup(NULL, value);
  if (*out == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to copy agent runtime string");
  }
  return CAI_OK;
}

static int cai_runtime_append_steering_locked(cai_agent_runtime *runtime,
                                              cai_runtime_input_node *input,
                                              cai_error *error) {
  cai_agent_session_event event;
  int rc;

  if (runtime->session_store == NULL) {
    return CAI_OK;
  }
  if (runtime->session_store->append_event == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session store does not support durable steering");
  }
  event.sequence = runtime->next_event_sequence + 1U;
  event.type = "steering_queued";
  event.data = input->text;
  rc = runtime->session_store->append_event(runtime->session_store->context,
                                            runtime->session_scope,
                                            runtime->session_id, &event, error);
  if (rc == CAI_OK) {
    runtime->next_event_sequence = event.sequence;
    input->journal_sequence = event.sequence;
  }
  return rc;
}

static int
cai_runtime_generate_session_id(char output[CAI_AGENT_SESSION_ID_MAX],
                                cai_error *error) {
  struct timespec now;
  unsigned long long counter;
  int length;

  if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to generate agent session identifier");
  }
  pthread_mutex_lock(&cai_runtime_session_id_lock);
  counter = ++cai_runtime_session_id_counter;
  pthread_mutex_unlock(&cai_runtime_session_id_lock);
  length = snprintf(output, CAI_AGENT_SESSION_ID_MAX, "smith-%lld-%ld-%llu",
                    (long long)now.tv_sec, now.tv_nsec, counter);
  if (length < 0 || (size_t)length >= CAI_AGENT_SESSION_ID_MAX) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to format agent session identifier");
  }
  return CAI_OK;
}

static int cai_runtime_checkpoint(cai_agent_runtime *runtime,
                                  cai_error *error) {
  cai_source *state;
  char *model;
  int rc;

  if (runtime->session_store == NULL) {
    return CAI_OK;
  }
  state = NULL;
  model = cai_strdup(&CAI_SESSION_CLIENT_IMPL(runtime->session)->allocator,
                     CAI_SESSION_AGENT_IMPL(runtime->session)->model);
  if (model == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to preserve session model for checkpoint");
  }
  cai_free_mem(&CAI_SESSION_CLIENT_IMPL(runtime->session)->allocator,
               CAI_SESSION_IMPL(runtime->session)->state_model);
  CAI_SESSION_IMPL(runtime->session)->state_model = model;
  rc = cai_session_export_state_source(runtime->session, &state, error);
  if (rc == CAI_OK) {
    rc = runtime->session_store->checkpoint(
        runtime->session_store->context, runtime->session_scope,
        runtime->session_id, state, runtime->applied_event_sequence, error);
  }
  cai_source_close(state);
  if (rc == CAI_OK) {
    pthread_mutex_lock(&runtime->lock);
    rc = cai_runtime_enqueue_locked(
        runtime, CAI_AGENT_EVENT_SESSION_CHECKPOINTED, runtime->session_id,
        strlen(runtime->session_id), NULL, NULL, runtime->state, error);
    pthread_mutex_unlock(&runtime->lock);
  }
  return rc;
}

static int cai_runtime_account_goal(cai_agent_runtime *runtime,
                                    int *out_budget_limited,
                                    cai_error *error) {
  cai_session_impl *session;
  long long total_tokens;
  long long delta;
  char *status;

  if (out_budget_limited != NULL) {
    *out_budget_limited = 0;
  }

  session = CAI_SESSION_IMPL(runtime->session);
  if (session->goal_status == NULL) {
    return CAI_OK;
  }
  if (strcmp(session->goal_status, "budget_limited") == 0) {
    if (out_budget_limited != NULL) {
      *out_budget_limited = 1;
    }
    return CAI_OK;
  }
  total_tokens = session->usage.usage.total_tokens;
  if (total_tokens >= session->goal_token_usage_baseline) {
    delta = total_tokens - session->goal_token_usage_baseline;
    if (delta > 0LL) {
      if (session->goal_tokens_used > LLONG_MAX - delta) {
        session->goal_tokens_used = LLONG_MAX;
      } else {
        session->goal_tokens_used += delta;
      }
    }
  }
  session->goal_token_usage_baseline = total_tokens;
  session->goal_updated_at = (long long)time(NULL);
  if (strcmp(session->goal_status, "active") != 0 ||
      !session->goal_has_token_budget ||
      session->goal_tokens_used < session->goal_token_budget) {
    return CAI_OK;
  }
  status = cai_strdup(&CAI_SESSION_CLIENT_IMPL(runtime->session)->allocator,
                      "budget_limited");
  if (status == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to record exhausted goal token budget");
  }
  cai_free_mem(&CAI_SESSION_CLIENT_IMPL(runtime->session)->allocator,
               session->goal_status);
  session->goal_status = status;
  if (out_budget_limited != NULL) {
    *out_budget_limited = 1;
  }
  return CAI_OK;
}

static int cai_runtime_goal_budget_limited(const cai_agent_runtime *runtime) {
  const cai_session_impl *session;

  session = CAI_SESSION_IMPL(runtime->session);
  return session->goal_status != NULL &&
         strcmp(session->goal_status, "budget_limited") == 0;
}

static int
cai_runtime_history_fits_context_window(const cai_agent_runtime *runtime,
                                        long long context_window) {
  size_t history_bytes;

  if (runtime == NULL || runtime->session == NULL || context_window <= 0LL) {
    return 0;
  }
  history_bytes =
      CAI_SESSION_IMPL(runtime->session)
          ->history.size_fn(&CAI_SESSION_IMPL(runtime->session)->history);
  return history_bytes <= (size_t)context_window;
}

static int cai_runtime_compact_resumed_history(cai_agent_runtime *runtime,
                                               cai_error *error) {
  const char *previous_model;
  const char *current_model;
  const char *previous_hash;
  const char *current_hash;
  long long previous_window;
  long long current_window;
  int rc;

  if (!runtime->resume_compaction_pending) {
    return CAI_OK;
  }
  previous_model = CAI_SESSION_IMPL(runtime->session)->state_model;
  current_model = CAI_SESSION_AGENT_IMPL(runtime->session)->model;
  previous_hash = cai_model_compaction_compatibility_hash(previous_model);
  current_hash = cai_model_compaction_compatibility_hash(current_model);
  previous_window = cai_model_context_window_tokens(previous_model);
  current_window = cai_model_context_window_tokens(current_model);
  if ((previous_model != NULL && current_model != NULL &&
       strcmp(previous_model, current_model) == 0) ||
      (previous_hash != NULL && current_hash != NULL &&
       strcmp(previous_hash, current_hash) == 0 && previous_window > 0LL &&
       (current_window >= previous_window ||
        cai_runtime_history_fits_context_window(runtime, current_window)))) {
    runtime->resume_compaction_pending = 0;
    return CAI_OK;
  }
  rc = cai_session_compact_experimental(runtime->session, error);
  if (rc == CAI_OK) {
    runtime->resume_compaction_pending = 0;
    return CAI_OK;
  }
  return rc;
}

static int cai_runtime_spooled_copy(const lonejson_spooled *spool, char **out,
                                    size_t *out_length, cai_error *error) {
  lonejson_spooled cursor;
  lonejson_error json_error;
  lonejson_read_result chunk;
  char *data;
  size_t length;
  size_t offset;

  *out = NULL;
  *out_length = 0U;
  if (spool == NULL || spool->size_fn(spool) == 0U) {
    return CAI_OK;
  }
  length = spool->size_fn(spool);
  data = (char *)cai_alloc(NULL, length + 1U);
  if (data == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate streamed text event");
  }
  cursor = *spool;
  lonejson_error_init(&json_error);
  if (cursor.rewind(&cursor, &json_error) != LONEJSON_STATUS_OK) {
    cai_free_mem(NULL, data);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to rewind streamed text event");
  }
  offset = 0U;
  while (offset < length) {
    chunk =
        cursor.read(&cursor, (unsigned char *)data + offset, length - offset);
    if (chunk.error_code != 0 || chunk.bytes_read == 0U) {
      cai_free_mem(NULL, data);
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "failed to read streamed text event");
    }
    offset += chunk.bytes_read;
  }
  data[length] = '\0';
  *out = data;
  *out_length = length;
  return CAI_OK;
}

static int cai_runtime_output_text_delta(void *context, const char *item_id,
                                         int output_index,
                                         const lonejson_spooled *delta,
                                         cai_error *error) {
  cai_agent_runtime *runtime;
  char *data;
  size_t length;
  int rc;

  (void)item_id;
  (void)output_index;
  runtime = (cai_agent_runtime *)context;
  data = NULL;
  length = 0U;
  rc = cai_runtime_spooled_copy(delta, &data, &length, error);
  if (rc == CAI_OK && length > 0U) {
    pthread_mutex_lock(&runtime->lock);
    rc = cai_runtime_enqueue_locked(runtime, CAI_AGENT_EVENT_TEXT_DELTA, data,
                                    length, NULL, NULL, runtime->state, error);
    pthread_mutex_unlock(&runtime->lock);
  }
  cai_free_mem(NULL, data);
  return rc;
}

static int cai_runtime_tool_action(const char *name) {
  if (name == NULL) {
    return CAI_AGENT_TOOL_ACTION_EXTERNAL;
  }
  if (strcmp(name, CAI_READ_DEFAULT_TOOL_NAME) == 0) {
    return CAI_AGENT_TOOL_ACTION_READ;
  }
  if (strcmp(name, CAI_LIST_FILES_DEFAULT_TOOL_NAME) == 0) {
    return CAI_AGENT_TOOL_ACTION_LIST;
  }
  if (strcmp(name, CAI_VIEW_IMAGE_DEFAULT_TOOL_NAME) == 0) {
    return CAI_AGENT_TOOL_ACTION_VIEW;
  }
  if (strcmp(name, CAI_PATCH_DEFAULT_TOOL_NAME) == 0) {
    return CAI_AGENT_TOOL_ACTION_PATCH;
  }
  if (strcmp(name, CAI_TERMINAL_EXEC_TOOL_NAME) == 0) {
    return CAI_AGENT_TOOL_ACTION_EXECUTE;
  }
  if (strcmp(name, CAI_TERMINAL_WRITE_TOOL_NAME) == 0) {
    return CAI_AGENT_TOOL_ACTION_WRITE_STDIN;
  }
  if (strcmp(name, CAI_GOAL_GET_TOOL_NAME) == 0) {
    return CAI_AGENT_TOOL_ACTION_GET_GOAL;
  }
  if (strcmp(name, CAI_GOAL_CREATE_TOOL_NAME) == 0) {
    return CAI_AGENT_TOOL_ACTION_CREATE_GOAL;
  }
  if (strcmp(name, CAI_GOAL_UPDATE_TOOL_NAME) == 0) {
    return CAI_AGENT_TOOL_ACTION_UPDATE_GOAL;
  }
  if (strcmp(name, CAI_GOAL_CLEAR_TOOL_NAME) == 0) {
    return CAI_AGENT_TOOL_ACTION_CLEAR_GOAL;
  }
  if (strcmp(name, "image_generation") == 0) {
    return CAI_AGENT_TOOL_ACTION_IMAGE_GENERATION;
  }
  return CAI_AGENT_TOOL_ACTION_EXTERNAL;
}

static char *cai_runtime_tool_path_from_arguments(const cai_tool_event *event) {
  cai_runtime_path_doc doc;
  cai_error error;
  lonejson_error json_error;
  lonejson_status status;
  const char *arguments;
  char *arguments_copy;
  char *path;
  size_t arguments_length;

  if (event == NULL) {
    return NULL;
  }
  arguments_copy = NULL;
  arguments_length = 0U;
  arguments = event->arguments_json;
  if (arguments == NULL && event->arguments_json_spooled != NULL &&
      event->arguments_json_spooled->size_fn(event->arguments_json_spooled) >
          0U &&
      event->arguments_json_spooled->size_fn(event->arguments_json_spooled) <=
          64U * 1024U) {
    cai_error_init(&error);
    if (cai_runtime_spooled_copy(event->arguments_json_spooled, &arguments_copy,
                                 &arguments_length, &error) == CAI_OK) {
      arguments = arguments_copy;
    }
    cai_error_cleanup(&error);
  }
  if (arguments == NULL || arguments[0] == '\0') {
    cai_free_mem(NULL, arguments_copy);
    return NULL;
  }
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
  status = CAI_LJ->parse_cstr(CAI_LJ, &cai_runtime_path_map, &doc,
                              arguments, &json_error);
  if (status != LONEJSON_STATUS_OK || doc.path == NULL || doc.path[0] == '\0') {
    CAI_LJ->cleanup(CAI_LJ, &cai_runtime_path_map, &doc);
    cai_free_mem(NULL, arguments_copy);
    return NULL;
  }
  path = cai_strdup(NULL, doc.path);
  CAI_LJ->cleanup(CAI_LJ, &cai_runtime_path_map, &doc);
  cai_free_mem(NULL, arguments_copy);
  return path;
}

static char *cai_runtime_patch_path_from_output(const char *data,
                                                size_t data_length,
                                                size_t *out_count) {
  static const char prefix[] = "Success. Updated the following files:\n";
  const char *cursor;
  const char *end;
  const char *line_end;
  char *path;
  size_t count;

  if (out_count != NULL) {
    *out_count = 0U;
  }
  if (data == NULL || data_length < sizeof(prefix) - 1U ||
      memcmp(data, prefix, sizeof(prefix) - 1U) != 0) {
    return NULL;
  }
  cursor = data + sizeof(prefix) - 1U;
  end = data + data_length;
  path = NULL;
  count = 0U;
  while (cursor < end) {
    line_end = memchr(cursor, '\n', (size_t)(end - cursor));
    if (line_end == NULL) {
      line_end = end;
    }
    if (line_end - cursor > 2 &&
        (cursor[0] == 'A' || cursor[0] == 'M' || cursor[0] == 'D') &&
        cursor[1] == ' ') {
      count++;
      if (path == NULL) {
        path = cai_strndup(NULL, cursor + 2, (size_t)(line_end - cursor - 2));
      }
    }
    if (line_end == end) {
      break;
    }
    cursor = line_end + 1;
  }
  if (out_count != NULL) {
    *out_count = count;
  }
  return path;
}

static int cai_runtime_tool_event(void *context, const cai_tool_event *event,
                                  cai_error *error) {
  cai_agent_runtime *runtime;
  char *data;
  size_t data_length;
  const char *message;
  char *tool_path;
  int tool_action;
  size_t tool_path_count;
  int type;
  int rc;

  runtime = (cai_agent_runtime *)context;
  if (runtime == NULL || event == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "invalid runtime tool event");
  }
  type = event->type == CAI_TOOL_EVENT_START ? CAI_AGENT_EVENT_TOOL_CALL_STARTED
         : event->type == CAI_TOOL_EVENT_OUTPUT
             ? CAI_AGENT_EVENT_TOOL_CALL_COMPLETED
             : CAI_AGENT_EVENT_TOOL_CALL_FAILED;
  tool_action = cai_runtime_tool_action(event->name);
  tool_path = cai_runtime_tool_path_from_arguments(event);
  tool_path_count = 0U;
  data = NULL;
  data_length = 0U;
  if (event->type == CAI_TOOL_EVENT_OUTPUT && event->output_json != NULL) {
    rc = cai_runtime_spooled_copy(event->output_json, &data, &data_length,
                                  error);
    if (rc != CAI_OK) {
      return rc;
    }
  } else if (event->type == CAI_TOOL_EVENT_ERROR) {
    message = event->tool_error != NULL && event->tool_error->message != NULL
                  ? event->tool_error->message
                  : "tool execution failed";
    data = cai_strdup(NULL, message);
    if (data == NULL) {
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to copy tool failure event");
    }
    data_length = strlen(data);
  }
  if (event->type == CAI_TOOL_EVENT_OUTPUT &&
      tool_action == CAI_AGENT_TOOL_ACTION_PATCH) {
    cai_free_mem(NULL, tool_path);
    tool_path = cai_runtime_patch_path_from_output(data, data_length,
                                                   &tool_path_count);
  }
  pthread_mutex_lock(&runtime->lock);
  runtime->state = CAI_AGENT_DISPATCHING_TOOL;
  if (event->type == CAI_TOOL_EVENT_START && event->name != NULL &&
      strcmp(event->name, CAI_TERMINAL_EXEC_TOOL_NAME) == 0) {
    char *origin;

    origin = event->call_id != NULL ? cai_strdup(NULL, event->call_id) : NULL;
    if (event->call_id != NULL && origin == NULL) {
      pthread_mutex_unlock(&runtime->lock);
      cai_free_mem(NULL, data);
      cai_free_mem(NULL, tool_path);
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to copy terminal tool call id");
    }
    cai_free_mem(NULL, runtime->terminal_origin_tool_call_id);
    runtime->terminal_origin_tool_call_id = origin;
  }
  rc = cai_runtime_enqueue_tool_locked(
      runtime, type, data, data_length, event->name, event->call_id,
      tool_action, tool_path, tool_path_count, runtime->state, error);
  if (event->type != CAI_TOOL_EVENT_START) {
    runtime->state = CAI_AGENT_SAMPLING;
  }
  pthread_mutex_unlock(&runtime->lock);
  cai_free_mem(NULL, data);
  cai_free_mem(NULL, tool_path);
  return rc;
}

static int cai_runtime_terminal_event(void *context,
                                      const cai_terminal_event *event,
                                      cai_error *error) {
  cai_agent_runtime *runtime;
  cai_terminal_event display_event;
  int type;
  int rc;

  runtime = (cai_agent_runtime *)context;
  if (runtime == NULL || event == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "invalid runtime terminal event");
  }
  if (runtime->terminal_event_callback != NULL) {
    rc = runtime->terminal_event_callback(runtime->terminal_event_context,
                                          event, error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  type = event->type == CAI_TERMINAL_EVENT_COMMAND_STARTED
             ? CAI_AGENT_EVENT_TERMINAL_COMMAND_STARTED
         : event->type == CAI_TERMINAL_EVENT_OUTPUT
             ? CAI_AGENT_EVENT_TERMINAL_OUTPUT
         : event->type == CAI_TERMINAL_EVENT_WAITING
             ? CAI_AGENT_EVENT_TERMINAL_WAITING
         : event->type == CAI_TERMINAL_EVENT_COMMAND_CANCELLED
             ? CAI_AGENT_EVENT_TERMINAL_COMMAND_CANCELLED
             : CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED;
  display_event = *event;
  if (event->type == CAI_TERMINAL_EVENT_COMMAND_STARTED) {
    display_event.output = event->command;
    display_event.output_length =
        event->command != NULL ? strlen(event->command) : 0U;
  }
  pthread_mutex_lock(&runtime->lock);
  rc = cai_runtime_enqueue_terminal_locked(runtime, type, &display_event,
                                           error);
  pthread_mutex_unlock(&runtime->lock);
  return rc;
}

static int cai_runtime_deliver_steering_after_tool_round(void *context,
                                                         cai_session *session,
                                                         cai_error *error) {
  cai_agent_runtime *runtime;
  cai_runtime_input_node *input;
  unsigned long long applied_sequence;
  int budget_limited;
  int rc;

  runtime = (cai_agent_runtime *)context;
  if (runtime == NULL || session != runtime->session) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "invalid Smith steering tool-round boundary");
  }
  rc = CAI_OK;
  applied_sequence = runtime->applied_event_sequence;
  pthread_mutex_lock(&runtime->lock);
  while (rc == CAI_OK && runtime->steering_head != NULL) {
    input = runtime->steering_head;
    rc = cai_session_add_user_text(session, input->text, error);
    if (rc == CAI_OK) {
      runtime->steering_head = input->next;
      if (runtime->steering_head == NULL) {
        runtime->steering_tail = NULL;
      }
      runtime->steering_count--;
      if (input->journal_sequence > applied_sequence) {
        applied_sequence = input->journal_sequence;
      }
      rc = cai_runtime_enqueue_locked(
          runtime, CAI_AGENT_EVENT_STEERING_DELIVERED, input->text,
          strlen(input->text), NULL, NULL, CAI_AGENT_SAMPLING, error);
      cai_runtime_input_node_free(input);
    }
  }
  pthread_mutex_unlock(&runtime->lock);
  budget_limited = 0;
  if (rc == CAI_OK) {
    runtime->applied_event_sequence = applied_sequence;
    rc = cai_runtime_account_goal(runtime, &budget_limited, error);
  }
  /* The tool-loop durable boundary checkpoints only after it has committed
   * the safe tool result that accompanies this round. */
  (void)budget_limited;
  return rc;
}

static int cai_runtime_checkpoint_durable_tool_round(void *context,
                                                     cai_session *session,
                                                     cai_error *error) {
  cai_agent_runtime *runtime;

  runtime = (cai_agent_runtime *)context;
  if (runtime == NULL || session != runtime->session) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "invalid Smith durable tool-round boundary");
  }
  return cai_runtime_checkpoint(runtime, error);
}

/* A response that finishes without another tool call still needs a durable
 * steering boundary. Tool rounds leave this commit to their safe tool-output
 * history path so the tool output remains ordered before steering. */
static int cai_runtime_deliver_steering_after_response(
    cai_agent_runtime *runtime, cai_error *error) {
  int rc;

  rc = cai_runtime_deliver_steering_after_tool_round(runtime, runtime->session,
                                                       error);
  if (rc == CAI_OK) {
    rc = cai_session_commit_pending_inputs(runtime->session, error);
  }
  return rc;
}

static cai_runtime_input_node *
cai_runtime_take_input_locked(cai_runtime_input_node **head,
                              cai_runtime_input_node **tail) {
  cai_runtime_input_node *node;

  node = *head;
  if (node != NULL) {
    *head = node->next;
    if (*head == NULL) {
      *tail = NULL;
    }
    node->next = NULL;
  }
  return node;
}

static int cai_runtime_replay_journal_event(
    void *context, const cai_agent_session_event *event, cai_error *error) {
  cai_agent_runtime *runtime;
  cai_runtime_input_node *input;

  runtime = (cai_agent_runtime *)context;
  if (runtime == NULL || event == NULL || event->type == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "invalid session journal replay event");
  }
  if (event->sequence > runtime->next_event_sequence) {
    runtime->next_event_sequence = event->sequence;
  }
  if (strcmp(event->type, "steering_queued") != 0) {
    return CAI_OK;
  }
  if (event->data == NULL || event->data[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "queued steering event has no text");
  }
  input = (cai_runtime_input_node *)cai_alloc(NULL, sizeof(*input));
  if (input == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to restore queued steering input");
  }
  memset(input, 0, sizeof(*input));
  input->text = cai_strdup(NULL, event->data);
  if (input->text == NULL) {
    cai_runtime_input_node_free(input);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to copy queued steering input");
  }
  input->journal_sequence = event->sequence;
  if (runtime->turn_tail == NULL) {
    runtime->turn_head = input;
  } else {
    runtime->turn_tail->next = input;
  }
  runtime->turn_tail = input;
  return CAI_OK;
}

static void *cai_runtime_worker(void *context) {
  cai_agent_runtime *runtime;
  cai_stream_sinks sinks;
  cai_run_options options;
  cai_runtime_input_node *input;
  cai_error error;
  int budget_limited;
  int rc;

  runtime = (cai_agent_runtime *)context;
  cai_stream_sinks_init(&sinks);
  sinks.output_text_delta = cai_runtime_output_text_delta;
  sinks.output_text_context = runtime;
  cai_run_options_init(&options);
  options.max_tool_calls_per_round = 1;
  options.tool_event = cai_runtime_tool_event;
  options.tool_event_context = runtime;
  options.tool_round_completed = cai_runtime_deliver_steering_after_tool_round;
  options.tool_round_completed_context = runtime;
  options.tool_round_durable = cai_runtime_checkpoint_durable_tool_round;
  options.tool_round_durable_context = runtime;
  for (;;) {
    pthread_mutex_lock(&runtime->lock);
    while (!runtime->stopping && runtime->turn_head == NULL) {
      pthread_cond_wait(&runtime->condition, &runtime->lock);
    }
    if (runtime->stopping) {
      pthread_mutex_unlock(&runtime->lock);
      break;
    }
    input =
        cai_runtime_take_input_locked(&runtime->turn_head, &runtime->turn_tail);
    pthread_mutex_unlock(&runtime->lock);
    if (input == NULL) {
      continue;
    }
    cai_error_init(&error);
    rc = cai_runtime_compact_resumed_history(runtime, &error);
    if (rc == CAI_OK) {
      rc = cai_session_add_user_text(runtime->session, input->text, &error);
    }
    if (rc == CAI_OK &&
        input->journal_sequence > runtime->applied_event_sequence) {
      runtime->applied_event_sequence = input->journal_sequence;
    }
    cai_runtime_input_node_free(input);
    if (rc == CAI_OK) {
      rc = cai_session_commit_pending_inputs(runtime->session, &error);
    }
    budget_limited = 0;
    if (rc == CAI_OK) {
      rc = cai_runtime_account_goal(runtime, &budget_limited, &error);
    }
    if (rc == CAI_OK) {
      rc = cai_runtime_checkpoint(runtime, &error);
    }
    while (rc == CAI_OK && !budget_limited) {
      cai_runtime_set_state(runtime, CAI_AGENT_SAMPLING);
      rc = cai_session_stream_auto(runtime->session, &options, &sinks, &error);
      if (rc != CAI_OK) {
        break;
      }
      pthread_mutex_lock(&runtime->lock);
      if (runtime->steering_head == NULL) {
        runtime->accepting_steering = 0;
        pthread_mutex_unlock(&runtime->lock);
        break;
      }
      pthread_mutex_unlock(&runtime->lock);
      rc = cai_runtime_deliver_steering_after_response(runtime, &error);
      if (rc == CAI_OK && cai_runtime_goal_budget_limited(runtime)) {
        budget_limited = 1;
        rc = cai_set_error(
            &error, CAI_ERR_LIMIT,
            "goal token budget exhausted before another model request");
      }
    }
    if (rc == CAI_ERR_LIMIT && cai_runtime_goal_budget_limited(runtime)) {
      /* The completed tool round has committed client history; make that
       * exact state durable before reporting the next-request boundary. */
      cai_error_cleanup(&error);
      cai_error_init(&error);
      rc = cai_runtime_checkpoint(runtime, &error);
    }
    if (rc == CAI_OK) {
      rc = cai_runtime_account_goal(runtime, &budget_limited, &error);
    }
    if (rc == CAI_OK) {
      rc = cai_runtime_checkpoint(runtime, &error);
    }
    pthread_mutex_lock(&runtime->lock);
    runtime->accepting_steering = 0;
    if (rc == CAI_OK ||
        (rc == CAI_ERR_LIMIT && cai_runtime_goal_budget_limited(runtime))) {
      runtime->state = CAI_AGENT_COMPLETED;
      (void)cai_runtime_enqueue_locked(runtime, CAI_AGENT_EVENT_RUN_COMPLETED,
                                       NULL, 0U, NULL, NULL, runtime->state,
                                       &error);
    } else {
      const char *message;

      runtime->state =
          rc == CAI_ERR_CANCELLED ? CAI_AGENT_CANCELLED : CAI_AGENT_FAILED;
      message = error.message != NULL ? error.message : "agent run failed";
      (void)cai_runtime_enqueue_locked(runtime, CAI_AGENT_EVENT_RUN_FAILED,
                                       message, strlen(message), NULL,
                                       NULL, runtime->state, &error);
    }
    pthread_cond_broadcast(&runtime->condition);
    pthread_mutex_unlock(&runtime->lock);
    cai_error_cleanup(&error);
  }
  return NULL;
}

void cai_agent_runtime_config_init(cai_agent_runtime_config *config) {
  if (config != NULL) {
    memset(config, 0, sizeof(*config));
  }
}

int cai_agent_runtime_open(cai_client *client,
                           const cai_agent_runtime_config *config,
                           cai_agent_runtime **out, cai_error *error) {
  cai_agent_runtime *runtime;
  cai_smith_config smith;
  cai_terminal_tool_config terminal_config;
  int review_mode;
  size_t i;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "agent runtime output pointer is required");
  }
  *out = NULL;
  if (client == NULL || config == NULL || config->workspace_directory == NULL ||
      config->workspace_directory[0] == '\0') {
    return cai_set_error(
        error, CAI_ERR_INVALID,
        "agent runtime client and workspace directory are required");
  }
  review_mode = config->preset != NULL &&
                strcmp(config->preset, CAI_SMITH_REVIEW_PRESET) == 0;
  if (config->preset != NULL && strcmp(config->preset, CAI_SMITH_PRESET) != 0 &&
      !review_mode) {
    return cai_set_error(error, CAI_ERR_INVALID, "unsupported agent preset");
  }
  runtime = (cai_agent_runtime *)cai_alloc(NULL, sizeof(*runtime));
  if (runtime == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate agent runtime");
  }
  memset(runtime, 0, sizeof(*runtime));
  runtime->wakeup_read_fd = -1;
  runtime->wakeup_write_fd = -1;
  runtime->owner_thread = pthread_self();
  runtime->client = client;
  runtime->state = CAI_AGENT_IDLE;
  runtime->event_limit = config->event_queue_limit != 0U
                             ? config->event_queue_limit
                             : CAI_RUNTIME_DEFAULT_EVENT_LIMIT;
  runtime->steering_limit = config->steering_queue_limit != 0U
                                ? config->steering_queue_limit
                                : CAI_RUNTIME_DEFAULT_STEERING_LIMIT;
  runtime->event_callback = config->event_callback;
  runtime->event_context = config->event_context;
  runtime->review_mode = review_mode;
  if (pthread_mutex_init(&runtime->lock, NULL) != 0) {
    cai_free_mem(NULL, runtime);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to initialize agent runtime synchronization");
  }
  if (pthread_cond_init(&runtime->condition, NULL) != 0) {
    pthread_mutex_destroy(&runtime->lock);
    cai_free_mem(NULL, runtime);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to initialize agent runtime synchronization");
  }
  rc = cai_runtime_wakeup_open(runtime, error);
  if (rc != CAI_OK) {
    pthread_cond_destroy(&runtime->condition);
    pthread_mutex_destroy(&runtime->lock);
    cai_free_mem(NULL, runtime);
    return rc;
  }
  cai_smith_config_init(&smith);
  smith.workspace_directory = config->workspace_directory;
  smith.agent_identity = config->agent_identity;
  smith.model = config->model;
  smith.reasoning_effort = config->reasoning_effort;
  smith.developer_instructions_extension =
      config->developer_instructions_extension;
  memset(&terminal_config, 0, sizeof(terminal_config));
  if (config->terminal_tool_config != NULL) {
    terminal_config = *config->terminal_tool_config;
    runtime->terminal_event_callback = terminal_config.event_callback;
    runtime->terminal_event_context = terminal_config.event_context;
  }
  terminal_config.event_callback = cai_runtime_terminal_event;
  terminal_config.event_context = runtime;
  smith.terminal_tool_config = &terminal_config;
  smith.disable_terminal = review_mode || config->disable_terminal;
  rc = review_mode
           ? cai_client_new_smith_review_agent(client, &smith, &runtime->agent,
                                               error)
           : cai_client_new_smith_agent(client, &smith, &runtime->agent, error);
  if (rc == CAI_OK && review_mode &&
      (config->mcp_client_count > 0U || config->enable_image_generation)) {
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "Smith review runtime does not support MCP or image generation tools");
  }
  if (rc == CAI_OK && config->mcp_client_count > 0U &&
      config->mcp_clients == NULL) {
    rc = cai_set_error(
        error, CAI_ERR_INVALID,
        "MCP client array is required when MCP clients are configured");
  }
  for (i = 0U; rc == CAI_OK && i < config->mcp_client_count; i++) {
    if (config->mcp_clients[i] == NULL) {
      rc = cai_set_error(error, CAI_ERR_INVALID,
                         "configured MCP client must not be null");
    } else {
      rc = cai_agent_register_mcp_client_tools(runtime->agent,
                                               config->mcp_clients[i],
                                               config->mcp_tool_config, error);
    }
  }
  if (rc == CAI_OK && config->enable_image_generation) {
    rc = cai_agent_add_simple_hosted_tool(
        runtime->agent, CAI_HOSTED_TOOL_IMAGE_GENERATION, error);
  }
  if (rc == CAI_OK) {
    rc = cai_agent_new_session(runtime->agent, &runtime->session, error);
  }
  if (rc == CAI_OK && !review_mode) {
    rc = cai_agent_register_goal_tools(runtime->agent, runtime->session, error);
  }
  if (rc == CAI_OK) {
    const char *scope;
    char session_id[CAI_AGENT_SESSION_ID_MAX];
    char workspace[4096];

    if (realpath(config->workspace_directory, workspace) == NULL) {
      rc = cai_set_error(error, CAI_ERR_INVALID,
                         "agent workspace directory must exist");
    }
    scope = config->session_scope != NULL ? config->session_scope : workspace;
    if (rc == CAI_OK) {
      rc = cai_runtime_copy_string(scope, &runtime->session_scope, error);
    }
    if (rc == CAI_OK && config->session_store != NULL) {
      if (config->session_store->checkpoint == NULL ||
          config->session_store->load_latest == NULL ||
          config->session_store->append_event == NULL ||
          config->session_store->load_events_after == NULL) {
        rc = cai_set_error(error, CAI_ERR_INVALID,
                           "durable session store callbacks are required");
      } else {
        runtime->session_store = config->session_store;
      }
    }
    if (rc == CAI_OK && config->session_store == NULL &&
        !config->disable_default_session_store) {
      rc = cai_agent_local_session_store_open(NULL, &runtime->local_store,
                                              error);
      if (rc == CAI_OK) {
        runtime->session_store = &runtime->local_store;
        runtime->owns_local_store = 1;
      }
    }
    if (rc == CAI_OK && config->resume_latest &&
        runtime->session_store != NULL) {
      cai_source *state;

      state = NULL;
      rc = runtime->session_store->load_latest(
          runtime->session_store->context, runtime->session_scope, session_id,
          sizeof(session_id), &state, &runtime->applied_event_sequence, error);
      if (rc == CAI_OK && state != NULL) {
        rc = cai_session_import_state_source(runtime->session, state, error);
        if (rc == CAI_OK) {
          if (CAI_SESSION_IMPL(runtime->session)->goal_status != NULL) {
            CAI_SESSION_IMPL(runtime->session)->goal_token_usage_baseline =
                CAI_SESSION_IMPL(runtime->session)->usage.usage.total_tokens;
          }
          rc = cai_runtime_copy_string(session_id, &runtime->session_id, error);
          runtime->resume_compaction_pending = 1;
        }
      }
      cai_source_close(state);
    }
    if (rc == CAI_OK && runtime->session_id == NULL) {
      if (config->session_id != NULL) {
        if (runtime->owns_local_store &&
            !cai_runtime_local_session_id_valid(config->session_id)) {
          rc = cai_set_error(error, CAI_ERR_INVALID,
                             "local session identifiers use only letters, digits, - and _");
        } else {
          rc = cai_runtime_copy_string(config->session_id, &runtime->session_id,
                                       error);
        }
      } else {
        rc = cai_runtime_generate_session_id(session_id, error);
        if (rc == CAI_OK) {
          rc = cai_runtime_copy_string(session_id, &runtime->session_id, error);
        }
      }
    }
    if (rc == CAI_OK && runtime->session_store != NULL &&
        config->resume_latest) {
      runtime->next_event_sequence = runtime->applied_event_sequence;
      rc = runtime->session_store->load_events_after(
          runtime->session_store->context, runtime->session_scope,
          runtime->session_id, runtime->applied_event_sequence,
          cai_runtime_replay_journal_event, runtime, error);
    }
  }
  if (rc == CAI_OK && pthread_create(&runtime->worker_thread, NULL,
                                     cai_runtime_worker, runtime) != 0) {
    rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                       "failed to start agent runtime worker");
  }
  if (rc != CAI_OK) {
    if (runtime->session != NULL) {
      cai_session_destroy(runtime->session);
    }
    if (runtime->agent != NULL) {
      cai_agent_destroy(runtime->agent);
    }
    if (runtime->owns_local_store) {
      cai_agent_local_session_store_close(&runtime->local_store);
    }
    cai_free_mem(NULL, runtime->session_scope);
    cai_free_mem(NULL, runtime->session_id);
    close(runtime->wakeup_read_fd);
    close(runtime->wakeup_write_fd);
    pthread_cond_destroy(&runtime->condition);
    pthread_mutex_destroy(&runtime->lock);
    cai_free_mem(NULL, runtime);
    return rc;
  }
  runtime->worker_started = 1;
  *out = runtime;
  return CAI_OK;
}

static int cai_runtime_enqueue_input(cai_agent_runtime *runtime,
                                     const char *text, int steering,
                                     cai_error *error) {
  cai_runtime_input_node *node;
  cai_runtime_event_node *event_node;
  int type;
  int rc;

  if (text == NULL || text[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "agent input text is required");
  }
  node = (cai_runtime_input_node *)cai_alloc(NULL, sizeof(*node));
  if (node == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate agent input");
  }
  memset(node, 0, sizeof(*node));
  event_node = NULL;
  node->text = cai_strdup(NULL, text);
  if (node->text == NULL) {
    cai_runtime_input_node_free(node);
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to copy agent input");
  }
  pthread_mutex_lock(&runtime->lock);
  if (steering && !runtime->accepting_steering) {
    pthread_mutex_unlock(&runtime->lock);
    cai_runtime_input_node_free(node);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "steering safe boundary has already passed");
  }
  if (steering && runtime->steering_count >= runtime->steering_limit) {
    pthread_mutex_unlock(&runtime->lock);
    cai_runtime_input_node_free(node);
    return cai_set_error(error, CAI_ERR_LIMIT, "agent steering queue is full");
  }
  if (steering) {
    rc = cai_runtime_require_event_capacity_locked(runtime, error);
    if (rc == CAI_OK) {
      rc = cai_runtime_event_node_new(CAI_AGENT_EVENT_STEERING_QUEUED, text,
                                      strlen(text), NULL, NULL, runtime->state,
                                      &event_node, error);
    }
    if (rc != CAI_OK) {
      pthread_mutex_unlock(&runtime->lock);
      cai_runtime_input_node_free(node);
      return rc;
    }
    rc = cai_runtime_append_steering_locked(runtime, node, error);
    if (rc != CAI_OK) {
      pthread_mutex_unlock(&runtime->lock);
      cai_runtime_event_node_free(event_node);
      cai_runtime_input_node_free(node);
      return rc;
    }
  }
  type =
      steering ? CAI_AGENT_EVENT_STEERING_QUEUED : CAI_AGENT_EVENT_RUN_STARTED;
  if (steering) {
    cai_runtime_append_event_node_locked(runtime, event_node);
    rc = CAI_OK;
  } else {
    rc = cai_runtime_enqueue_locked(runtime, type, text, strlen(text), NULL,
                                    NULL, runtime->state, error);
  }
  if (rc != CAI_OK) {
    pthread_mutex_unlock(&runtime->lock);
    cai_runtime_input_node_free(node);
    return rc;
  }
  if (steering) {
    if (runtime->steering_tail == NULL) {
      runtime->steering_head = node;
    } else {
      runtime->steering_tail->next = node;
    }
    runtime->steering_tail = node;
    runtime->steering_count++;
  } else {
    if (runtime->turn_tail == NULL) {
      runtime->turn_head = node;
    } else {
      runtime->turn_tail->next = node;
    }
    runtime->turn_tail = node;
  }
  pthread_cond_broadcast(&runtime->condition);
  pthread_mutex_unlock(&runtime->lock);
  return CAI_OK;
}

int cai_agent_runtime_submit(cai_agent_runtime *runtime, const char *text,
                             cai_error *error) {
  cai_agent_run_state previous_state;
  int rc;

  rc = cai_runtime_owner(runtime, error);
  if (rc != CAI_OK) {
    return rc;
  }
  pthread_mutex_lock(&runtime->lock);
  if (runtime->state != CAI_AGENT_IDLE &&
      runtime->state != CAI_AGENT_COMPLETED &&
      runtime->state != CAI_AGENT_FAILED &&
      runtime->state != CAI_AGENT_CANCELLED) {
    pthread_mutex_unlock(&runtime->lock);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "agent runtime already has an active turn");
  }
  if (cai_runtime_goal_budget_limited(runtime)) {
    pthread_mutex_unlock(&runtime->lock);
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "goal token budget is exhausted");
  }
  previous_state = runtime->state;
  runtime->state = CAI_AGENT_SAMPLING;
  runtime->accepting_steering = 1;
  pthread_mutex_unlock(&runtime->lock);
  rc = cai_runtime_enqueue_input(runtime, text, 0, error);
  if (rc != CAI_OK) {
    pthread_mutex_lock(&runtime->lock);
    if (runtime->turn_head == NULL && runtime->state == CAI_AGENT_SAMPLING) {
      runtime->state = previous_state;
      runtime->accepting_steering = 0;
    }
    pthread_mutex_unlock(&runtime->lock);
  }
  return rc;
}

int cai_agent_runtime_submit_steering_threadsafe(cai_agent_runtime *runtime,
                                                 const char *text,
                                                 cai_error *error) {
  if (runtime == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent runtime is required");
  }
  pthread_mutex_lock(&runtime->lock);
  if ((runtime->state != CAI_AGENT_SAMPLING &&
       runtime->state != CAI_AGENT_DISPATCHING_TOOL) ||
      !runtime->accepting_steering) {
    pthread_mutex_unlock(&runtime->lock);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "steering requires an active agent turn");
  }
  pthread_mutex_unlock(&runtime->lock);
  return cai_runtime_enqueue_input(runtime, text, 1, error);
}

int cai_agent_runtime_submit_steering(cai_agent_runtime *runtime,
                                      const char *text, cai_error *error) {
  int rc;

  rc = cai_runtime_owner(runtime, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_agent_runtime_submit_steering_threadsafe(runtime, text, error);
}

int cai_agent_runtime_pump(cai_agent_runtime *runtime, long timeout_ms,
                           cai_error *error) {
  cai_runtime_event_node *node;
  int close_deferred;
  int rc;

  rc = cai_runtime_owner(runtime, error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (timeout_ms < 0L) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "pump timeout must not be negative");
  }
  pthread_mutex_lock(&runtime->lock);
  if (runtime->pumping) {
    pthread_mutex_unlock(&runtime->lock);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "agent runtime pump is already active");
  }
  runtime->pumping = 1;
  if (runtime->event_head == NULL) {
    cai_runtime_wakeup_drain_locked(runtime);
  }
  if (runtime->event_head == NULL && timeout_ms > 0L && !runtime->stopping) {
    struct timespec deadline;

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000L;
    deadline.tv_nsec += (timeout_ms % 1000L) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
      deadline.tv_sec++;
      deadline.tv_nsec -= 1000000000L;
    }
    (void)pthread_cond_timedwait(&runtime->condition, &runtime->lock,
                                 &deadline);
  }
  while ((node = runtime->event_head) != NULL) {
    runtime->event_head = node->next;
    if (runtime->event_head == NULL) {
      runtime->event_tail = NULL;
    }
    runtime->event_count--;
    pthread_cond_broadcast(&runtime->condition);
    pthread_mutex_unlock(&runtime->lock);
    if (runtime->event_callback != NULL) {
      rc = runtime->event_callback(runtime->event_context, &node->event, error);
      if (rc != CAI_OK) {
        cai_runtime_event_node_free(node);
        pthread_mutex_lock(&runtime->lock);
        break;
      }
    }
    cai_runtime_event_node_free(node);
    pthread_mutex_lock(&runtime->lock);
    if (runtime->close_deferred) {
      /*
       * The callback may have released its event context as part of closing
       * this runtime. Do not dispatch any more queued events to it.
       */
      break;
    }
  }
  if (runtime->event_head == NULL) {
    cai_runtime_wakeup_drain_locked(runtime);
  }
  runtime->pumping = 0;
  close_deferred = runtime->close_deferred;
  if (close_deferred) {
    runtime->destroying = 1;
  }
  pthread_cond_broadcast(&runtime->condition);
  while (close_deferred && runtime->close_waiters > 0U) {
    pthread_cond_wait(&runtime->condition, &runtime->lock);
  }
  pthread_mutex_unlock(&runtime->lock);
  if (close_deferred) {
    cai_agent_runtime_destroy(runtime);
  }
  return rc;
}

int cai_agent_runtime_wakeup_fd(const cai_agent_runtime *runtime, int *out_fd,
                                cai_error *error) {
  if (runtime == NULL || out_fd == NULL || runtime->wakeup_read_fd < 0) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "agent runtime wakeup descriptor is required");
  }
  *out_fd = runtime->wakeup_read_fd;
  return CAI_OK;
}

int cai_agent_runtime_state(cai_agent_runtime *runtime,
                            cai_agent_run_state *out, cai_error *error) {
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "agent state output is required");
  }
  rc = cai_runtime_owner(runtime, error);
  if (rc != CAI_OK) {
    return rc;
  }
  pthread_mutex_lock(&runtime->lock);
  *out = runtime->state;
  pthread_mutex_unlock(&runtime->lock);
  return CAI_OK;
}

const char *cai_agent_runtime_session_id(const cai_agent_runtime *runtime) {
  return runtime != NULL ? runtime->session_id : NULL;
}

static void cai_agent_runtime_destroy(cai_agent_runtime *runtime) {
  cai_runtime_event_node *event;
  cai_runtime_input_node *input;

  if (runtime->worker_started) {
    pthread_join(runtime->worker_thread, NULL);
  }
  while ((event = runtime->event_head) != NULL) {
    runtime->event_head = event->next;
    cai_runtime_event_node_free(event);
  }
  while ((input = runtime->turn_head) != NULL) {
    runtime->turn_head = input->next;
    cai_runtime_input_node_free(input);
  }
  while ((input = runtime->steering_head) != NULL) {
    runtime->steering_head = input->next;
    cai_runtime_input_node_free(input);
  }
  if (runtime->session != NULL) {
    cai_session_destroy(runtime->session);
  }
  if (runtime->agent != NULL) {
    cai_agent_destroy(runtime->agent);
  }
  if (runtime->owns_local_store) {
    cai_agent_local_session_store_close(&runtime->local_store);
  }
  cai_free_mem(NULL, runtime->session_scope);
  cai_free_mem(NULL, runtime->session_id);
  cai_free_mem(NULL, runtime->terminal_origin_tool_call_id);
  close(runtime->wakeup_read_fd);
  close(runtime->wakeup_write_fd);
  pthread_cond_destroy(&runtime->condition);
  pthread_mutex_destroy(&runtime->lock);
  cai_free_mem(NULL, runtime);
}

void cai_agent_runtime_close(cai_agent_runtime *runtime) {
  int owner_callback;

  if (runtime == NULL) {
    return;
  }
  pthread_mutex_lock(&runtime->lock);
  if (runtime->destroying) {
    pthread_mutex_unlock(&runtime->lock);
    return;
  }
  runtime->stopping = 1;
  owner_callback = pthread_equal(runtime->owner_thread, pthread_self());
  if (runtime->pumping && owner_callback) {
    runtime->close_deferred = 1;
    pthread_cond_broadcast(&runtime->condition);
    pthread_mutex_unlock(&runtime->lock);
    return;
  }
  while (runtime->pumping) {
    runtime->close_waiters++;
    pthread_cond_wait(&runtime->condition, &runtime->lock);
    runtime->close_waiters--;
    pthread_cond_broadcast(&runtime->condition);
  }
  /*
   * An owner callback requested destruction and pump has claimed it. A
   * concurrent closer has waited until that callback is no longer active, so
   * it may now safely return without touching callback-owned resources.
   */
  if (runtime->destroying) {
    pthread_mutex_unlock(&runtime->lock);
    return;
  }
  runtime->destroying = 1;
  pthread_cond_broadcast(&runtime->condition);
  while (runtime->close_waiters > 0U) {
    pthread_cond_wait(&runtime->condition, &runtime->lock);
  }
  pthread_mutex_unlock(&runtime->lock);
  cai_agent_runtime_destroy(runtime);
}
