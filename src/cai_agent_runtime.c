#include <cai/agent_runtime.h>
#include <cai/smith.h>
#include <cai/tools/goal.h>

#include "cai_internal.h"

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
  struct cai_runtime_event_node *next;
} cai_runtime_event_node;

typedef struct cai_runtime_input_node {
  char *text;
  struct cai_runtime_input_node *next;
} cai_runtime_input_node;

struct cai_agent_runtime {
  pthread_t owner_thread;
  pthread_t worker_thread;
  pthread_mutex_t lock;
  pthread_cond_t condition;
  int worker_started;
  int stopping;
  cai_client *client;
  cai_agent *agent;
  cai_session *session;
  cai_agent_session_store local_store;
  const cai_agent_session_store *session_store;
  int owns_local_store;
  char *session_scope;
  char *session_id;
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
};

static pthread_mutex_t cai_runtime_session_id_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long cai_runtime_session_id_counter = 0U;

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
  cai_free_mem(NULL, node);
}

static void cai_runtime_input_node_free(cai_runtime_input_node *node) {
  if (node == NULL) {
    return;
  }
  cai_free_mem(NULL, node->text);
  cai_free_mem(NULL, node);
}

static int cai_runtime_enqueue_locked(cai_agent_runtime *runtime, int type,
                                      const char *data, size_t data_length,
                                      const char *tool_name,
                                      cai_agent_run_state state,
                                      cai_error *error) {
  cai_runtime_event_node *node;

  while (!runtime->stopping && runtime->event_count >= runtime->event_limit) {
    pthread_cond_wait(&runtime->condition, &runtime->lock);
  }
  if (runtime->stopping) {
    return cai_set_error(error, CAI_ERR_CANCELLED, "agent runtime is closing");
  }
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
  node->event.type = type;
  node->event.state = state;
  node->event.sequence = ++runtime->next_sequence;
  node->event.data = node->data;
  node->event.data_length = data_length;
  node->event.tool_name = node->tool_name;
  if (runtime->event_tail == NULL) {
    runtime->event_head = node;
  } else {
    runtime->event_tail->next = node;
  }
  runtime->event_tail = node;
  runtime->event_count++;
  pthread_cond_broadcast(&runtime->condition);
  return CAI_OK;
}

static void cai_runtime_set_state(cai_agent_runtime *runtime,
                                  cai_agent_run_state state) {
  cai_error ignored;

  cai_error_init(&ignored);
  pthread_mutex_lock(&runtime->lock);
  runtime->state = state;
  (void)cai_runtime_enqueue_locked(runtime, CAI_AGENT_EVENT_RUN_STATE_CHANGED,
                                   NULL, 0U, NULL, state, &ignored);
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
    rc = runtime->session_store->checkpoint(runtime->session_store->context,
                                            runtime->session_scope,
                                            runtime->session_id, state, error);
  }
  cai_source_close(state);
  if (rc == CAI_OK) {
    pthread_mutex_lock(&runtime->lock);
    rc = cai_runtime_enqueue_locked(
        runtime, CAI_AGENT_EVENT_SESSION_CHECKPOINTED, runtime->session_id,
        strlen(runtime->session_id), NULL, runtime->state, error);
    pthread_mutex_unlock(&runtime->lock);
  }
  return rc;
}

static void cai_runtime_account_goal(cai_agent_runtime *runtime) {
  cai_session_impl *session;

  session = CAI_SESSION_IMPL(runtime->session);
  if (session->goal_status != NULL) {
    session->goal_tokens_used =
        session->usage.usage.total_tokens - session->goal_token_usage_baseline;
    if (session->goal_tokens_used < 0LL) {
      session->goal_tokens_used = 0LL;
    }
    session->goal_updated_at = (long long)time(NULL);
  }
}

static int cai_runtime_compact_resumed_history(cai_agent_runtime *runtime,
                                               cai_error *error) {
  const char *previous_model;
  const char *current_model;
  const char *previous_hash;
  const char *current_hash;
  int rc;

  if (!runtime->resume_compaction_pending) {
    return CAI_OK;
  }
  previous_model = CAI_SESSION_IMPL(runtime->session)->state_model;
  current_model = CAI_SESSION_AGENT_IMPL(runtime->session)->model;
  previous_hash = cai_model_compaction_compatibility_hash(previous_model);
  current_hash = cai_model_compaction_compatibility_hash(current_model);
  if (previous_hash == NULL || current_hash == NULL ||
      strcmp(previous_hash, current_hash) == 0) {
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
                                    length, NULL, runtime->state, error);
    pthread_mutex_unlock(&runtime->lock);
  }
  cai_free_mem(NULL, data);
  return rc;
}

static int cai_runtime_tool_event(void *context, const cai_tool_event *event,
                                  cai_error *error) {
  cai_agent_runtime *runtime;
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
  pthread_mutex_lock(&runtime->lock);
  runtime->state = CAI_AGENT_DISPATCHING_TOOL;
  rc = cai_runtime_enqueue_locked(runtime, type, NULL, 0U, event->name,
                                  runtime->state, error);
  if (event->type != CAI_TOOL_EVENT_START) {
    runtime->state = CAI_AGENT_SAMPLING;
  }
  pthread_mutex_unlock(&runtime->lock);
  return rc;
}

static int cai_runtime_deliver_steering_after_tool_round(void *context,
                                                         cai_session *session,
                                                         cai_error *error) {
  cai_agent_runtime *runtime;
  cai_runtime_input_node *input;
  int rc;

  runtime = (cai_agent_runtime *)context;
  if (runtime == NULL || session != runtime->session) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "invalid Smith steering tool-round boundary");
  }
  rc = CAI_OK;
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
      rc = cai_runtime_enqueue_locked(
          runtime, CAI_AGENT_EVENT_STEERING_DELIVERED, input->text,
          strlen(input->text), NULL, CAI_AGENT_SAMPLING, error);
      cai_runtime_input_node_free(input);
    }
  }
  pthread_mutex_unlock(&runtime->lock);
  if (rc == CAI_OK) {
    rc = cai_session_commit_pending_inputs(runtime->session, error);
  }
  if (rc == CAI_OK) {
    cai_runtime_account_goal(runtime);
    rc = cai_runtime_checkpoint(runtime, error);
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

static void *cai_runtime_worker(void *context) {
  cai_agent_runtime *runtime;
  cai_stream_sinks sinks;
  cai_run_options options;
  cai_runtime_input_node *input;
  cai_error error;
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
    cai_runtime_input_node_free(input);
    if (rc == CAI_OK) {
      rc = cai_session_commit_pending_inputs(runtime->session, &error);
    }
    if (rc == CAI_OK) {
      cai_runtime_account_goal(runtime);
      rc = cai_runtime_checkpoint(runtime, &error);
    }
    while (rc == CAI_OK) {
      cai_runtime_set_state(runtime, CAI_AGENT_SAMPLING);
      rc = cai_session_stream_auto(runtime->session, &options, &sinks, &error);
      if (rc != CAI_OK) {
        break;
      }
      pthread_mutex_lock(&runtime->lock);
      input = cai_runtime_take_input_locked(&runtime->steering_head,
                                            &runtime->steering_tail);
      if (input != NULL) {
        runtime->steering_count--;
        (void)cai_runtime_enqueue_locked(
            runtime, CAI_AGENT_EVENT_STEERING_DELIVERED, input->text,
            strlen(input->text), NULL, CAI_AGENT_SAMPLING, &error);
      } else {
        runtime->accepting_steering = 0;
      }
      pthread_mutex_unlock(&runtime->lock);
      if (input == NULL) {
        break;
      }
      rc = cai_session_add_user_text(runtime->session, input->text, &error);
      cai_runtime_input_node_free(input);
      if (rc == CAI_OK) {
        rc = cai_session_commit_pending_inputs(runtime->session, &error);
      }
      if (rc == CAI_OK) {
        cai_runtime_account_goal(runtime);
        rc = cai_runtime_checkpoint(runtime, &error);
      }
    }
    if (rc == CAI_OK) {
      cai_runtime_account_goal(runtime);
      rc = cai_runtime_checkpoint(runtime, &error);
    }
    pthread_mutex_lock(&runtime->lock);
    runtime->accepting_steering = 0;
    if (rc == CAI_OK) {
      runtime->state = CAI_AGENT_COMPLETED;
      (void)cai_runtime_enqueue_locked(runtime, CAI_AGENT_EVENT_RUN_COMPLETED,
                                       NULL, 0U, NULL, runtime->state, &error);
    } else {
      const char *message;

      runtime->state =
          rc == CAI_ERR_CANCELLED ? CAI_AGENT_CANCELLED : CAI_AGENT_FAILED;
      message = error.message != NULL ? error.message : "agent run failed";
      (void)cai_runtime_enqueue_locked(runtime, CAI_AGENT_EVENT_RUN_FAILED,
                                       message, strlen(message), NULL,
                                       runtime->state, &error);
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
  if (config->preset != NULL && strcmp(config->preset, CAI_SMITH_PRESET) != 0) {
    return cai_set_error(error, CAI_ERR_INVALID, "unsupported agent preset");
  }
  runtime = (cai_agent_runtime *)cai_alloc(NULL, sizeof(*runtime));
  if (runtime == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate agent runtime");
  }
  memset(runtime, 0, sizeof(*runtime));
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
  cai_smith_config_init(&smith);
  smith.workspace_directory = config->workspace_directory;
  smith.agent_identity = config->agent_identity;
  smith.model = config->model;
  smith.reasoning_effort = config->reasoning_effort;
  smith.developer_instructions_extension =
      config->developer_instructions_extension;
  rc = cai_client_new_smith_agent(client, &smith, &runtime->agent, error);
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
  if (rc == CAI_OK) {
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
          config->session_store->load_latest == NULL) {
        rc = cai_set_error(error, CAI_ERR_INVALID,
                           "session store callbacks are required");
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
          sizeof(session_id), &state, error);
      if (rc == CAI_OK && state != NULL) {
        rc = cai_session_import_state_source(runtime->session, state, error);
        if (rc == CAI_OK) {
          rc = cai_runtime_copy_string(session_id, &runtime->session_id, error);
          runtime->resume_compaction_pending = 1;
        }
      }
      cai_source_close(state);
    }
    if (rc == CAI_OK && runtime->session_id == NULL) {
      if (config->session_id != NULL) {
        rc = cai_runtime_copy_string(config->session_id, &runtime->session_id,
                                     error);
      } else {
        rc = cai_runtime_generate_session_id(session_id, error);
        if (rc == CAI_OK) {
          rc = cai_runtime_copy_string(session_id, &runtime->session_id, error);
        }
      }
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
  type =
      steering ? CAI_AGENT_EVENT_STEERING_QUEUED : CAI_AGENT_EVENT_RUN_STARTED;
  rc = cai_runtime_enqueue_locked(runtime, type, text, strlen(text), NULL,
                                  runtime->state, error);
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
        return rc;
      }
    }
    cai_runtime_event_node_free(node);
    pthread_mutex_lock(&runtime->lock);
  }
  pthread_mutex_unlock(&runtime->lock);
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

void cai_agent_runtime_close(cai_agent_runtime *runtime) {
  cai_runtime_event_node *event;
  cai_runtime_input_node *input;

  if (runtime == NULL) {
    return;
  }
  pthread_mutex_lock(&runtime->lock);
  runtime->stopping = 1;
  pthread_cond_broadcast(&runtime->condition);
  pthread_mutex_unlock(&runtime->lock);
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
  pthread_cond_destroy(&runtime->condition);
  pthread_mutex_destroy(&runtime->lock);
  cai_free_mem(NULL, runtime);
}
