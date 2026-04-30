#include "cai_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct cai_tool_output_capture {
  lonejson_spooled output;
} cai_tool_output_capture;

typedef struct cai_history_sink_context {
  lonejson_spooled *spool;
} cai_history_sink_context;

typedef struct cai_spooled_record_reader {
  lonejson_spooled cursor;
  unsigned char buffer[4096];
  size_t offset;
  size_t length;
  int eof;
} cai_spooled_record_reader;

typedef struct cai_spooled_reader_context {
  lonejson_spooled cursor;
} cai_spooled_reader_context;

enum {
  CAI_SESSION_INPUT_TEXT = 0,
  CAI_SESSION_INPUT_IMAGE = 1,
  CAI_SESSION_INPUT_FUNCTION_CALL_OUTPUT = 2
};

static int cai_history_append_spooled(cai_session *session,
                                      const lonejson_spooled *json,
                                      cai_error *error);
static int cai_history_append_array_record_spooled(cai_session *session,
                                                   const lonejson_spooled *json,
                                                   cai_error *error);
static void cai_history_cleanup(cai_session *session);
static int
cai_session_prepare_history_params(cai_session *session,
                                   cai_response_create_params *params,
                                   lonejson_spooled *out_pending_items,
                                   int *out_has_pending_items,
                                   cai_error *error);
static int cai_session_after_response(cai_session *session,
                                      const lonejson_spooled *pending_items,
                                      int has_pending_items,
                                      const cai_response *response,
                                      cai_error *error);
static int cai_session_after_stream(cai_session *session,
                                    const lonejson_spooled *pending_items,
                                    int has_pending_items,
                                    const char *response_id,
                                    const cai_token_usage *usage,
                                    cai_error *error);
static int cai_session_init_response_params(cai_session *session,
                                            cai_response_create_params **out,
                                            cai_error *error);
static int cai_session_remember_response_id(cai_session *session,
                                            const char *response_id,
                                            cai_error *error);
static int cai_session_remember_stream(cai_session *session,
                                       const char *response_id,
                                       const cai_token_usage *usage,
                                       cai_error *error);
static int cai_agent_add_user_text(cai_agent *agent, const char *text,
                                   cai_error *error);
static int cai_agent_add_user_image_url(cai_agent *agent, const char *url,
                                        const char *detail, cai_error *error);
static int cai_agent_run(cai_agent *agent, cai_response **out,
                         cai_error *error);
static int cai_agent_run_output(cai_agent *agent, cai_output **out,
                                cai_error *error);
static int cai_agent_run_auto(cai_agent *agent, const cai_run_options *options,
                              cai_response **out, cai_error *error);
static int cai_agent_run_auto_output(cai_agent *agent,
                                     const cai_run_options *options,
                                     cai_output **out, cai_error *error);
static int cai_agent_stream(cai_agent *agent, const cai_stream_sinks *sinks,
                            cai_error *error);
static int cai_agent_stream_text(cai_agent *agent, cai_sink *sink,
                                 cai_error *error);
static int cai_agent_open_text_source(cai_agent *agent, cai_source **out,
                                      cai_error *error);
static int cai_agent_send_text(cai_agent *agent, const char *text,
                               cai_response **out, cai_error *error);
static int cai_agent_last_usage(const cai_agent *agent, cai_token_usage *out,
                                cai_error *error);
static int cai_agent_context_percent(const cai_agent *agent, double *out,
                                     cai_error *error);
static void cai_agent_init_methods(cai_agent *agent);
static void cai_session_init_methods(cai_session *session);
static int cai_history_to_array_spool(cai_session *session,
                                      lonejson_spooled *out,
                                      cai_error *error);

void cai_agent_config_init(cai_agent_config *config) {
  if (config == NULL) {
    return;
  }
  config->model = NULL;
  config->developer_instructions = NULL;
  config->prompt_cache_key = NULL;
  config->reasoning_effort = NULL;
  config->reasoning_summary = NULL;
  config->text_format_name = NULL;
  config->text_format_description = NULL;
  config->text_format_schema_json = NULL;
  config->text_format_strict = 0;
  config->max_output_tokens = 0;
  config->parallel_tool_calls = -1;
  config->disable_auto_compaction = 0;
  config->compact_threshold_tokens = 0LL;
  config->compact_threshold_percent = 80U;
  config->auto_compact = 0;
  config->auto_compact_token_limit = 0LL;
  config->enable_local_history = 0;
  config->history_memory_limit = 128U * 1024U;
  config->history_spool_dir = NULL;
}

void cai_run_options_init(cai_run_options *options) {
  if (options == NULL) {
    return;
  }
  options->max_tool_rounds = 4;
  options->tool_output_memory_limit = 1024U * 1024U;
  options->tool_spool_dir = NULL;
}

int cai_client_new_agent(cai_client *client, const cai_agent_config *config,
                         cai_agent **out, cai_error *error) {
  cai_agent *agent;
  cai_agent_impl *impl;
  cai_client_impl *client_impl;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "agent output pointer is required");
  }
  *out = NULL;
  if (client == NULL || config == NULL || config->model == NULL ||
      config->model[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "client and agent model are required");
  }
  client_impl = CAI_CLIENT_IMPL(client);
  if (client_impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "client is closed");
  }
  agent = (cai_agent *)cai_alloc(&client_impl->allocator, sizeof(*agent));
  if (agent == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate agent");
  }
  memset(agent, 0, sizeof(*agent));
  impl = (cai_agent_impl *)cai_alloc(&client_impl->allocator, sizeof(*impl));
  if (impl == NULL) {
    cai_free_mem(&client_impl->allocator, agent);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate agent implementation");
  }
  memset(impl, 0, sizeof(*impl));
  agent->impl = impl;
  cai_agent_init_methods(agent);
  impl->client = client;
  impl->model = cai_strdup(&client_impl->allocator, config->model);
  impl->developer_instructions =
      cai_strdup(&client_impl->allocator, config->developer_instructions);
  impl->prompt_cache_key =
      cai_strdup(&client_impl->allocator, config->prompt_cache_key);
  impl->reasoning_effort =
      cai_strdup(&client_impl->allocator, config->reasoning_effort);
  impl->reasoning_summary =
      cai_strdup(&client_impl->allocator, config->reasoning_summary);
  impl->text_format_name =
      cai_strdup(&client_impl->allocator, config->text_format_name);
  impl->text_format_description =
      cai_strdup(&client_impl->allocator, config->text_format_description);
  impl->text_format_schema_json =
      cai_strdup(&client_impl->allocator, config->text_format_schema_json);
  impl->text_format_strict = config->text_format_strict;
  impl->max_output_tokens = config->max_output_tokens;
  impl->parallel_tool_calls = config->parallel_tool_calls;
  impl->auto_compact = config->disable_auto_compaction ? 0 : 1;
  impl->local_history_enabled = config->enable_local_history ? 1 : 0;
  impl->compact_threshold_percent =
      config->compact_threshold_percent != 0U
          ? config->compact_threshold_percent
          : 80U;
  if (config->compact_threshold_tokens > 0LL) {
    impl->auto_compact_token_limit = config->compact_threshold_tokens;
  } else if (config->auto_compact_token_limit > 0LL) {
    impl->auto_compact_token_limit = config->auto_compact_token_limit;
  } else if (impl->auto_compact) {
    long long context_window;

    context_window = cai_model_context_window_tokens(impl->model);
    impl->auto_compact_token_limit =
        context_window > 0LL
            ? (context_window * (long long)impl->compact_threshold_percent) /
                  100LL
            : 0LL;
  } else {
    impl->auto_compact_token_limit = 0LL;
  }
  impl->history_memory_limit = config->history_memory_limit != 0U
                                   ? config->history_memory_limit
                                   : 128U * 1024U;
  impl->history_spool_dir =
      cai_strdup(&client_impl->allocator, config->history_spool_dir);
  impl->tools = NULL;
  impl->default_session = NULL;
  if (impl->model == NULL ||
      (config->developer_instructions != NULL &&
       impl->developer_instructions == NULL) ||
      (config->prompt_cache_key != NULL && impl->prompt_cache_key == NULL) ||
      (config->reasoning_effort != NULL && impl->reasoning_effort == NULL) ||
      (config->reasoning_summary != NULL && impl->reasoning_summary == NULL) ||
      (config->text_format_name != NULL && impl->text_format_name == NULL) ||
      (config->text_format_description != NULL &&
       impl->text_format_description == NULL) ||
      (config->text_format_schema_json != NULL &&
       impl->text_format_schema_json == NULL) ||
      (config->history_spool_dir != NULL && impl->history_spool_dir == NULL)) {
    cai_agent_destroy(agent);
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate agent");
  }
  if (impl->auto_compact && impl->auto_compact_token_limit <= 0LL) {
    cai_agent_destroy(agent);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "model has unknown context window; set "
                         "compact_threshold_tokens or disable auto compaction");
  }
  if (impl->auto_compact && impl->compact_threshold_percent > 95U &&
      config->compact_threshold_tokens <= 0LL &&
      config->auto_compact_token_limit <= 0LL) {
    cai_agent_destroy(agent);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "compact threshold percent must be 1..95");
  }
  if (impl->auto_compact && impl->auto_compact_token_limit < 1000LL) {
    cai_agent_destroy(agent);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "compact threshold must be at least 1000 tokens");
  }
  if (cai_tool_registry_new(&impl->tools, error) != CAI_OK) {
    cai_agent_destroy(agent);
    return error != NULL ? error->code : CAI_ERR_NOMEM;
  }
  *out = agent;
  return CAI_OK;
}

void cai_agent_destroy(cai_agent *agent) {
  cai_allocator *allocator;
  cai_agent_impl *impl;

  if (agent == NULL) {
    return;
  }
  impl = CAI_AGENT_IMPL(agent);
  if (impl == NULL) {
    return;
  }
  allocator = &CAI_CLIENT_IMPL(impl->client)->allocator;
  cai_session_destroy(impl->default_session);
  cai_free_mem(allocator, impl->model);
  cai_free_mem(allocator, impl->developer_instructions);
  cai_free_mem(allocator, impl->prompt_cache_key);
  cai_free_mem(allocator, impl->reasoning_effort);
  cai_free_mem(allocator, impl->reasoning_summary);
  cai_free_mem(allocator, impl->text_format_name);
  cai_free_mem(allocator, impl->text_format_description);
  cai_free_mem(allocator, impl->text_format_schema_json);
  cai_free_mem(allocator, impl->history_spool_dir);
  cai_tool_registry_destroy(impl->tools);
  cai_free_mem(allocator, impl);
  agent->impl = NULL;
  cai_free_mem(allocator, agent);
}

int cai_agent_register_tool(cai_agent *agent, const char *name,
                            const char *description,
                            const lonejson_map *params_map,
                            const lonejson_map *result_map,
                            cai_tool_fn callback, void *context,
                            cai_error *error) {
  return cai_agent_register_lonejson_tool(agent, name, description, params_map,
                                          result_map, callback, context, error);
}

int cai_agent_register_lonejson_tool(cai_agent *agent, const char *name,
                                     const char *description,
                                     const lonejson_map *params_map,
                                     const lonejson_map *result_map,
                                     cai_tool_fn callback, void *context,
                                     cai_error *error) {
  cai_agent_impl *impl;

  if (agent == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  impl = CAI_AGENT_IMPL(agent);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is closed");
  }
  return cai_tool_registry_register_lonejson(impl->tools, name, description,
                                             params_map, result_map, callback,
                                             context, error);
}

int cai_agent_register_raw_tool(cai_agent *agent, const char *name,
                                const char *description,
                                const char *schema_json, int strict,
                                cai_tool_raw_fn callback, void *context,
                                cai_error *error) {
  cai_agent_impl *impl;

  if (agent == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  impl = CAI_AGENT_IMPL(agent);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is closed");
  }
  return cai_tool_registry_register_raw(impl->tools, name, description,
                                        schema_json, strict, callback, context,
                                        error);
}

int cai_agent_new_session(cai_agent *agent, cai_session **out,
                          cai_error *error) {
  cai_session *session;
  cai_session_impl *impl;
  cai_agent_impl *agent_impl;
  cai_client_impl *client_impl;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session output pointer is required");
  }
  *out = NULL;
  if (agent == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  agent_impl = CAI_AGENT_IMPL(agent);
  if (agent_impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is closed");
  }
  client_impl = CAI_CLIENT_IMPL(agent_impl->client);
  session =
      (cai_session *)cai_alloc(&client_impl->allocator, sizeof(*session));
  if (session == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate session");
  }
  memset(session, 0, sizeof(*session));
  impl = (cai_session_impl *)cai_alloc(&client_impl->allocator, sizeof(*impl));
  if (impl == NULL) {
    cai_free_mem(&client_impl->allocator, session);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate session implementation");
  }
  memset(impl, 0, sizeof(*impl));
  session->impl = impl;
  cai_session_init_methods(session);
  impl->agent = agent;
  impl->previous_response_id = NULL;
  impl->conversation_id = NULL;
  memset(&impl->last_usage, 0, sizeof(impl->last_usage));
  impl->has_last_usage = 0;
  {
    lonejson_spool_options options;

    options = lonejson_default_spool_options();
    options.memory_limit = agent_impl->history_memory_limit;
    options.max_bytes = 0U;
    options.temp_dir = agent_impl->history_spool_dir;
    lonejson_spooled_init(&impl->history, &options);
  }
  impl->inputs = NULL;
  impl->input_count = 0U;
  impl->input_capacity = 0U;
  *out = session;
  return CAI_OK;
}

int cai_agent_new_conversation_session(cai_agent *agent, cai_session **out,
                                       cai_error *error) {
  cai_conversation *conversation;
  cai_session *session;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session output pointer is required");
  }
  *out = NULL;
  if (agent == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  conversation = NULL;
  session = NULL;
  rc = cai_client_create_conversation(CAI_AGENT_IMPL(agent)->client, &conversation, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_agent_new_session(agent, &session, error);
  if (rc == CAI_OK) {
    rc = cai_session_set_conversation_id(
        session, cai_conversation_id(conversation), error);
  }
  cai_conversation_destroy(conversation);
  if (rc != CAI_OK) {
    cai_session_destroy(session);
    return rc;
  }
  *out = session;
  return CAI_OK;
}

int cai_agent_new_session_for_conversation(cai_agent *agent,
                                           const cai_conversation *conversation,
                                           cai_session **out,
                                           cai_error *error) {
  cai_session *session;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session output pointer is required");
  }
  *out = NULL;
  if (agent == NULL || conversation == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "agent and conversation are required");
  }
  session = NULL;
  rc = cai_agent_new_session(agent, &session, error);
  if (rc == CAI_OK) {
    rc = cai_session_set_conversation(session, conversation, error);
  }
  if (rc != CAI_OK) {
    cai_session_destroy(session);
    return rc;
  }
  *out = session;
  return CAI_OK;
}

static void cai_session_clear_inputs(cai_session *session) {
  cai_allocator *allocator;
  size_t i;

  if (session == NULL) {
    return;
  }
  allocator = &CAI_SESSION_CLIENT_IMPL(session)->allocator;
  for (i = 0U; i < CAI_SESSION_IMPL(session)->input_count; i++) {
    cai_free_mem(allocator, CAI_SESSION_IMPL(session)->inputs[i].role);
    cai_free_mem(allocator, CAI_SESSION_IMPL(session)->inputs[i].text);
    cai_free_mem(allocator, CAI_SESSION_IMPL(session)->inputs[i].image_url);
    cai_free_mem(allocator, CAI_SESSION_IMPL(session)->inputs[i].detail);
    cai_free_mem(allocator, CAI_SESSION_IMPL(session)->inputs[i].call_id);
    cai_free_mem(allocator, CAI_SESSION_IMPL(session)->inputs[i].output);
  }
  CAI_SESSION_IMPL(session)->input_count = 0U;
}

static int cai_history_append_bytes(lonejson_spooled *history,
                                    const void *bytes, size_t length,
                                    cai_error *error) {
  lonejson_error json_error;

  if (length == 0U) {
    return CAI_OK;
  }
  lonejson_error_init(&json_error);
  if (lonejson_spooled_append(history, bytes, length, &json_error) ==
      LONEJSON_STATUS_OK) {
    return CAI_OK;
  }
  return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                              "failed to append history spool",
                              json_error.message);
}

static lonejson_status cai_history_lonejson_sink(void *user, const void *data,
                                                 size_t len,
                                                 lonejson_error *error) {
  cai_history_sink_context *context;
  (void)error;
  context = (cai_history_sink_context *)user;
  if (lonejson_spooled_append(context->spool, data, len, error) ==
      LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_OK;
  }
  return LONEJSON_STATUS_CALLBACK_FAILED;
}

static void cai_history_init_spooled(cai_session *session,
                                     lonejson_spooled *spool) {
  lonejson_spool_options options;

  options = lonejson_default_spool_options();
  options.memory_limit = CAI_SESSION_AGENT_IMPL(session)->history_memory_limit;
  options.max_bytes = 0U;
  options.temp_dir = CAI_SESSION_AGENT_IMPL(session)->history_spool_dir;
  lonejson_spooled_init(spool, &options);
}

static int cai_history_capture_compact_array_spooled(
    cai_session *session, const lonejson_spooled *json, lonejson_spooled *item,
    cai_error *error) {
  lonejson_spooled cursor;
  lonejson_error json_error;
  lonejson_read_result chunk;
  unsigned char buffer[4096];
  int rc;

  cai_history_init_spooled(session, item);
  if (json == NULL || lonejson_spooled_size(json) == 0U) {
    return CAI_OK;
  }
  lonejson_error_init(&json_error);
  if (lonejson_spooled_append(item, "[", 1U, &json_error) !=
      LONEJSON_STATUS_OK) {
    lonejson_spooled_cleanup(item);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to start JSON history spool",
                                json_error.message);
  }
  cursor = *json;
  if (lonejson_spooled_rewind(&cursor, &json_error) != LONEJSON_STATUS_OK) {
    lonejson_spooled_cleanup(item);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to rewind JSON history spool",
                                json_error.message);
  }
  do {
    chunk = lonejson_spooled_read(&cursor, buffer, sizeof(buffer));
    if (chunk.error_code != 0) {
      lonejson_spooled_cleanup(item);
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "failed to read JSON history spool");
    }
    if (chunk.bytes_read > 0U) {
      rc = cai_history_append_bytes(item, buffer, chunk.bytes_read, error);
      if (rc != CAI_OK) {
        lonejson_spooled_cleanup(item);
        return rc;
      }
    }
  } while (!chunk.eof);
  if (lonejson_spooled_append(item, "]", 1U, &json_error) !=
      LONEJSON_STATUS_OK) {
    lonejson_spooled_cleanup(item);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to finish JSON history spool",
                                json_error.message);
  }
  return CAI_OK;
}

static int cai_history_append_spooled(cai_session *session,
                                      const lonejson_spooled *json,
                                      cai_error *error) {
  lonejson_spooled item;
  cai_history_sink_context sink_context;
  lonejson_error json_error;
  char header[32];
  size_t header_length;
  int rc;

  if (json == NULL || lonejson_spooled_size(json) == 0U) {
    return CAI_OK;
  }
  rc = cai_history_capture_compact_array_spooled(session, json, &item, error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (lonejson_spooled_size(&item) == 0U) {
    lonejson_spooled_cleanup(&item);
    return CAI_OK;
  }
  snprintf(header, sizeof(header), "%lu\n",
           (unsigned long)lonejson_spooled_size(&item));
  header_length = strlen(header);
  rc = cai_history_append_bytes(&CAI_SESSION_IMPL(session)->history, header, header_length,
                                error);
  if (rc == CAI_OK) {
    sink_context.spool = &CAI_SESSION_IMPL(session)->history;
    lonejson_error_init(&json_error);
    if (lonejson_spooled_write_to_sink(&item, cai_history_lonejson_sink,
                                       &sink_context,
                                       &json_error) != LONEJSON_STATUS_OK) {
      rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to copy history spool",
                                json_error.message);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_history_append_bytes(&CAI_SESSION_IMPL(session)->history, "\n", 1U, error);
  }
  lonejson_spooled_cleanup(&item);
  return rc;
}

static int cai_history_append_array_record_spooled(cai_session *session,
                                                   const lonejson_spooled *json,
                                                   cai_error *error) {
  cai_history_sink_context sink_context;
  lonejson_error json_error;
  char header[32];
  size_t header_length;
  int rc;

  if (json == NULL || lonejson_spooled_size(json) == 0U) {
    return CAI_OK;
  }
  snprintf(header, sizeof(header), "%lu\n",
           (unsigned long)lonejson_spooled_size(json));
  header_length = strlen(header);
  rc = cai_history_append_bytes(&CAI_SESSION_IMPL(session)->history, header,
                                header_length, error);
  if (rc == CAI_OK) {
    sink_context.spool = &CAI_SESSION_IMPL(session)->history;
    lonejson_error_init(&json_error);
    if (lonejson_spooled_write_to_sink(json, cai_history_lonejson_sink,
                                       &sink_context,
                                       &json_error) != LONEJSON_STATUS_OK) {
      rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to copy history spool",
                                json_error.message);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_history_append_bytes(&CAI_SESSION_IMPL(session)->history, "\n", 1U,
                                  error);
  }
  return rc;
}

static void cai_history_reset(cai_session *session) {
  lonejson_spooled_reset(&CAI_SESSION_IMPL(session)->history);
}

static void cai_history_cleanup(cai_session *session) {
  if (session != NULL) {
    lonejson_spooled_cleanup(&CAI_SESSION_IMPL(session)->history);
  }
}

void cai_session_destroy(cai_session *session) {
  cai_allocator *allocator;
  cai_session_impl *impl;

  if (session == NULL) {
    return;
  }
  impl = CAI_SESSION_IMPL(session);
  if (impl == NULL) {
    return;
  }
  allocator = &CAI_SESSION_CLIENT_IMPL(session)->allocator;
  cai_session_clear_inputs(session);
  cai_history_cleanup(session);
  cai_free_mem(allocator, impl->inputs);
  cai_free_mem(allocator, impl->previous_response_id);
  cai_free_mem(allocator, impl->conversation_id);
  cai_free_mem(allocator, impl);
  session->impl = NULL;
  cai_free_mem(allocator, session);
}

int cai_session_set_conversation_id(cai_session *session,
                                    const char *conversation_id,
                                    cai_error *error) {
  char *copy;
  cai_allocator *allocator;

  if (session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "session is required");
  }
  allocator = &CAI_SESSION_CLIENT_IMPL(session)->allocator;
  copy = NULL;
  if (conversation_id != NULL) {
    copy = cai_strdup(allocator, conversation_id);
    if (copy == NULL) {
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate conversation id");
    }
  }
  cai_free_mem(allocator, CAI_SESSION_IMPL(session)->conversation_id);
  CAI_SESSION_IMPL(session)->conversation_id = copy;
  if (copy != NULL) {
    cai_free_mem(allocator, CAI_SESSION_IMPL(session)->previous_response_id);
    CAI_SESSION_IMPL(session)->previous_response_id = NULL;
  }
  return CAI_OK;
}

int cai_session_set_conversation(cai_session *session,
                                 const cai_conversation *conversation,
                                 cai_error *error) {
  if (conversation == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "conversation is required");
  }
  return cai_session_set_conversation_id(
      session, cai_conversation_id(conversation), error);
}

const char *cai_session_conversation_id(const cai_session *session) {
  return session != NULL ? CAI_SESSION_IMPL(session)->conversation_id : NULL;
}

int cai_session_set_previous_response_id(cai_session *session,
                                         const char *response_id,
                                         cai_error *error) {
  char *copy;
  cai_allocator *allocator;

  if (session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "session is required");
  }
  allocator = &CAI_SESSION_CLIENT_IMPL(session)->allocator;
  copy = NULL;
  if (response_id != NULL) {
    if (response_id[0] == '\0') {
      return cai_set_error(error, CAI_ERR_INVALID,
                           "previous response id must not be empty");
    }
    copy = cai_strdup(allocator, response_id);
    if (copy == NULL) {
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate previous response id");
    }
  }
  cai_free_mem(allocator, CAI_SESSION_IMPL(session)->previous_response_id);
  CAI_SESSION_IMPL(session)->previous_response_id = copy;
  if (copy != NULL) {
    cai_free_mem(allocator, CAI_SESSION_IMPL(session)->conversation_id);
    CAI_SESSION_IMPL(session)->conversation_id = NULL;
  }
  return CAI_OK;
}

const char *cai_session_previous_response_id(const cai_session *session) {
  return session != NULL ? CAI_SESSION_IMPL(session)->previous_response_id
                         : NULL;
}

static int cai_session_grow_inputs(cai_session *session, cai_error *error) {
  size_t new_capacity;
  void *grown;

  if (CAI_SESSION_IMPL(session)->input_count < CAI_SESSION_IMPL(session)->input_capacity) {
    return CAI_OK;
  }
  new_capacity =
      CAI_SESSION_IMPL(session)->input_capacity == 0U ? 2U : CAI_SESSION_IMPL(session)->input_capacity * 2U;
  grown = cai_realloc_mem(&CAI_SESSION_CLIENT_IMPL(session)->allocator, CAI_SESSION_IMPL(session)->inputs,
                          new_capacity * sizeof(CAI_SESSION_IMPL(session)->inputs[0]));
  if (grown == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to grow session input list");
  }
  CAI_SESSION_IMPL(session)->inputs = (cai_session_input *)grown;
  CAI_SESSION_IMPL(session)->input_capacity = new_capacity;
  return CAI_OK;
}

static int cai_session_add_input(cai_session *session, int kind,
                                 const char *role, const char *text,
                                 const char *image_url, const char *detail,
                                 const char *call_id, const char *output,
                                 cai_error *error) {
  cai_session_input *input;
  cai_allocator *allocator;
  int rc;

  if (session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "session is required");
  }
  if (kind != CAI_SESSION_INPUT_FUNCTION_CALL_OUTPUT &&
      (role == NULL || role[0] == '\0')) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session and role are required");
  }
  rc = cai_session_grow_inputs(session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  allocator = &CAI_SESSION_CLIENT_IMPL(session)->allocator;
  input = &CAI_SESSION_IMPL(session)->inputs[CAI_SESSION_IMPL(session)->input_count];
  memset(input, 0, sizeof(*input));
  input->kind = kind;
  input->role = cai_strdup(allocator, role);
  input->text = cai_strdup(allocator, text);
  input->image_url = cai_strdup(allocator, image_url);
  input->detail = cai_strdup(allocator, detail);
  input->call_id = cai_strdup(allocator, call_id);
  input->output = cai_strdup(allocator, output);
  if ((role != NULL && input->role == NULL) ||
      (text != NULL && input->text == NULL) ||
      (image_url != NULL && input->image_url == NULL) ||
      (detail != NULL && input->detail == NULL) ||
      (call_id != NULL && input->call_id == NULL) ||
      (output != NULL && input->output == NULL)) {
    cai_free_mem(allocator, input->role);
    cai_free_mem(allocator, input->text);
    cai_free_mem(allocator, input->image_url);
    cai_free_mem(allocator, input->detail);
    cai_free_mem(allocator, input->call_id);
    cai_free_mem(allocator, input->output);
    memset(input, 0, sizeof(*input));
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate session input");
  }
  CAI_SESSION_IMPL(session)->input_count++;
  return CAI_OK;
}

int cai_session_add_user_text(cai_session *session, const char *text,
                              cai_error *error) {
  if (text == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "text is required");
  }
  return cai_session_add_input(session, CAI_SESSION_INPUT_TEXT, "user", text,
                               NULL, NULL, NULL, NULL, error);
}

int cai_session_add_user_image_url(cai_session *session, const char *url,
                                   const char *detail, cai_error *error) {
  if (url == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "image URL is required");
  }
  return cai_session_add_input(session, CAI_SESSION_INPUT_IMAGE, "user", NULL,
                               url, detail, NULL, NULL, error);
}

int cai_session_add_function_call_output(cai_session *session,
                                         const char *call_id,
                                         const char *output, cai_error *error) {
  if (call_id == NULL || call_id[0] == '\0' || output == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "function call id and output are required");
  }
  return cai_session_add_input(session, CAI_SESSION_INPUT_FUNCTION_CALL_OUTPUT,
                               NULL, NULL, NULL, NULL, call_id, output, error);
}

static int cai_session_add_pending_inputs(cai_session *session,
                                          cai_response_create_params *params,
                                          cai_error *error) {
  size_t i;
  int rc;

  rc = CAI_OK;
  for (i = 0U; rc == CAI_OK && i < CAI_SESSION_IMPL(session)->input_count; i++) {
    if (CAI_SESSION_IMPL(session)->inputs[i].kind == CAI_SESSION_INPUT_IMAGE) {
      rc = cai_response_create_params_add_image_url(
          params, CAI_SESSION_IMPL(session)->inputs[i].role, CAI_SESSION_IMPL(session)->inputs[i].image_url,
          CAI_SESSION_IMPL(session)->inputs[i].detail, error);
    } else if (CAI_SESSION_IMPL(session)->inputs[i].kind ==
               CAI_SESSION_INPUT_FUNCTION_CALL_OUTPUT) {
      rc = cai_response_create_params_add_function_call_output(
          params, CAI_SESSION_IMPL(session)->inputs[i].call_id, CAI_SESSION_IMPL(session)->inputs[i].output, error);
    } else {
      rc = cai_response_create_params_add_text(params, CAI_SESSION_IMPL(session)->inputs[i].role,
                                               CAI_SESSION_IMPL(session)->inputs[i].text, error);
    }
  }
  return rc;
}

static int cai_spooled_record_reader_next(cai_spooled_record_reader *reader,
                                          unsigned char *out,
                                          cai_error *error) {
  lonejson_read_result chunk;

  if (reader->offset >= reader->length) {
    if (reader->eof) {
      return 0;
    }
    chunk = lonejson_spooled_read(&reader->cursor, reader->buffer,
                                  sizeof(reader->buffer));
    if (chunk.error_code != 0) {
      (void)cai_set_error(error, CAI_ERR_TRANSPORT,
                          "failed to read history spool");
      return -1;
    }
    reader->offset = 0U;
    reader->length = chunk.bytes_read;
    reader->eof = chunk.eof;
    if (reader->length == 0U) {
      return 0;
    }
  }
  *out = reader->buffer[reader->offset];
  reader->offset++;
  return 1;
}

static int cai_history_to_array_items_spool(cai_session *session,
                                            lonejson_spooled *out,
                                            size_t *out_len,
                                            cai_error *error) {
  cai_spooled_record_reader reader;
  unsigned long item_length;
  unsigned long pos;
  unsigned char ch;
  size_t length;
  int need_comma;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "history spool output pointer is required");
  }
  cai_history_init_spooled(session, out);
  reader.cursor = CAI_SESSION_IMPL(session)->history;
  reader.offset = 0U;
  reader.length = 0U;
  reader.eof = 0;
  {
    lonejson_error json_error;

    lonejson_error_init(&json_error);
    if (lonejson_spooled_rewind(&reader.cursor, &json_error) !=
        LONEJSON_STATUS_OK) {
      lonejson_spooled_cleanup(out);
      return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                  "failed to rewind history spool",
                                  json_error.message);
    }
  }
  need_comma = 0;
  length = 0U;
  for (;;) {
    rc = cai_spooled_record_reader_next(&reader, &ch, error);
    if (rc < 0) {
      lonejson_spooled_cleanup(out);
      return error != NULL ? error->code : CAI_ERR_TRANSPORT;
    }
    while (rc > 0 && (ch == '\n' || ch == '\r')) {
      rc = cai_spooled_record_reader_next(&reader, &ch, error);
      if (rc < 0) {
        lonejson_spooled_cleanup(out);
        return error != NULL ? error->code : CAI_ERR_TRANSPORT;
      }
    }
    if (rc == 0) {
      break;
    }
    if (ch < '0' || ch > '9') {
      lonejson_spooled_cleanup(out);
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "invalid history record length");
    }
    item_length = 0UL;
    do {
      if (item_length > (ULONG_MAX / 10UL)) {
        lonejson_spooled_cleanup(out);
        return cai_set_error(error, CAI_ERR_PROTOCOL,
                             "history record length overflow");
      }
      item_length = item_length * 10UL + (unsigned long)(ch - '0');
      rc = cai_spooled_record_reader_next(&reader, &ch, error);
      if (rc <= 0) {
        lonejson_spooled_cleanup(out);
        return rc < 0 ? (error != NULL ? error->code : CAI_ERR_TRANSPORT)
                      : cai_set_error(error, CAI_ERR_PROTOCOL,
                                      "truncated history record length");
      }
    } while (ch >= '0' && ch <= '9');
    if (ch != '\n') {
      lonejson_spooled_cleanup(out);
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "invalid history record length");
    }
    if (item_length < 2UL) {
      lonejson_spooled_cleanup(out);
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "invalid history record JSON array");
    }
    for (pos = 0UL; pos < item_length; pos++) {
      rc = cai_spooled_record_reader_next(&reader, &ch, error);
      if (rc <= 0) {
        lonejson_spooled_cleanup(out);
        return rc < 0 ? (error != NULL ? error->code : CAI_ERR_TRANSPORT)
                      : cai_set_error(error, CAI_ERR_PROTOCOL,
                                      "truncated history record JSON array");
      }
      if ((pos == 0UL && ch != '[') ||
          (pos == item_length - 1UL && ch != ']')) {
        lonejson_spooled_cleanup(out);
        return cai_set_error(error, CAI_ERR_PROTOCOL,
                             "invalid history record JSON array");
      }
      if (pos > 0UL && pos < item_length - 1UL) {
        if (!need_comma) {
          need_comma = 1;
        } else if (pos == 1UL) {
          if (cai_history_append_bytes(out, ",", 1U, error) != CAI_OK) {
            lonejson_spooled_cleanup(out);
            return error != NULL ? error->code : CAI_ERR_TRANSPORT;
          }
          length++;
        }
        if (cai_history_append_bytes(out, &ch, 1U, error) != CAI_OK) {
          lonejson_spooled_cleanup(out);
          return error != NULL ? error->code : CAI_ERR_TRANSPORT;
        }
        length++;
      }
    }
  }
  if (out_len != NULL) {
    *out_len = length;
  }
  return CAI_OK;
}

static int cai_history_to_array_spool(cai_session *session,
                                      lonejson_spooled *out,
                                      cai_error *error) {
  lonejson_spooled items;
  cai_history_sink_context sink_context;
  lonejson_error json_error;
  size_t items_len;
  int has_items;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "history array spool output pointer is required");
  }
  memset(&items, 0, sizeof(items));
  items_len = 0U;
  has_items = 0;
  cai_history_init_spooled(session, out);
  rc = cai_history_append_bytes(out, "[", 1U, error);
  if (rc == CAI_OK) {
    rc = cai_history_to_array_items_spool(session, &items, &items_len, error);
    if (rc == CAI_OK) {
      has_items = 1;
    }
  }
  if (rc == CAI_OK && items_len > 0U) {
    sink_context.spool = out;
    lonejson_error_init(&json_error);
    if (lonejson_spooled_rewind(&items, &json_error) !=
        LONEJSON_STATUS_OK) {
      rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to rewind history items",
                                json_error.message);
    }
  }
  if (rc == CAI_OK && items_len > 0U) {
    if (lonejson_spooled_write_to_sink(&items, cai_history_lonejson_sink,
                                       &sink_context,
                                       &json_error) != LONEJSON_STATUS_OK) {
      rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to copy history items",
                                json_error.message);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_history_append_bytes(out, "]", 1U, error);
  }
  if (has_items) {
    lonejson_spooled_cleanup(&items);
  }
  if (rc != CAI_OK) {
    lonejson_spooled_cleanup(out);
  }
  return rc;
}

static int
cai_session_prepare_history_params(cai_session *session,
                                   cai_response_create_params *params,
                                   lonejson_spooled *out_pending_items,
                                   int *out_has_pending_items,
                                   cai_error *error) {
  lonejson_spooled pending_items;
  size_t pending_len;
  int has_pending_items;
  int rc;

  if (out_has_pending_items != NULL) {
    *out_has_pending_items = 0;
  }
  if (!CAI_SESSION_AGENT_IMPL(session)->local_history_enabled) {
    return cai_session_add_pending_inputs(session, params, error);
  }
  memset(&pending_items, 0, sizeof(pending_items));
  pending_len = 0U;
  has_pending_items = 0;
  rc = cai_session_add_pending_inputs(session, params, error);
  if (rc == CAI_OK) {
    rc = cai_response_params_input_items_spool(params, &pending_items,
                                               &pending_len, error);
    if (rc == CAI_OK) {
      has_pending_items = 1;
    }
  }
  if (out_pending_items != NULL && out_has_pending_items != NULL &&
      rc == CAI_OK) {
    *out_pending_items = pending_items;
    *out_has_pending_items = has_pending_items;
    has_pending_items = 0;
  }
  if (has_pending_items) {
    lonejson_spooled_cleanup(&pending_items);
  }
  return rc;
}

static int cai_session_remember_response(cai_session *session,
                                         const cai_response *response,
                                         cai_error *error) {
  int rc;

  rc = cai_session_remember_response_id(session, cai_response_id(response),
                                        error);
  if (rc == CAI_OK && response != NULL) {
    CAI_SESSION_IMPL(session)->last_usage.input_tokens = cai_response_input_tokens(response);
    CAI_SESSION_IMPL(session)->last_usage.input_cached_tokens =
        cai_response_input_cached_tokens(response);
    CAI_SESSION_IMPL(session)->last_usage.output_tokens = cai_response_output_tokens(response);
    CAI_SESSION_IMPL(session)->last_usage.output_reasoning_tokens =
        cai_response_output_reasoning_tokens(response);
    CAI_SESSION_IMPL(session)->last_usage.total_tokens = cai_response_total_tokens(response);
    CAI_SESSION_IMPL(session)->has_last_usage = 1;
  }
  return rc;
}

static int cai_token_usage_is_empty(const cai_token_usage *usage) {
  if (usage == NULL) {
    return 1;
  }
  return usage->input_tokens == 0LL && usage->input_cached_tokens == 0LL &&
         usage->output_tokens == 0LL &&
         usage->output_reasoning_tokens == 0LL && usage->total_tokens == 0LL;
}

int cai_session_compact_experimental(cai_session *session, cai_error *error) {
  cai_response_create_params *params;
  cai_response *response;
  lonejson_spooled request_json;
  lonejson_spooled history_items;
  size_t request_json_len;
  size_t history_len;
  int has_request_json;
  int has_history_items;
  lonejson_spooled output_items;
  size_t output_items_len;
  int has_output_items;
  char *body;
  char *request_id;
  long http_status;
  int rc;

  params = NULL;
  response = NULL;
  memset(&request_json, 0, sizeof(request_json));
  memset(&history_items, 0, sizeof(history_items));
  request_json_len = 0U;
  history_len = 0U;
  has_request_json = 0;
  has_history_items = 0;
  memset(&output_items, 0, sizeof(output_items));
  output_items_len = 0U;
  has_output_items = 0;
  body = NULL;
  request_id = NULL;
  if (session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "session is required");
  }
  if (!CAI_SESSION_AGENT_IMPL(session)->local_history_enabled) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "local history capture is disabled");
  }
  rc = cai_history_to_array_items_spool(session, &history_items, &history_len,
                                        error);
  if (rc == CAI_OK) {
    has_history_items = 1;
  }
  if (rc == CAI_OK && history_len == 0U) {
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "session has no local history to compact");
    goto done;
  }
  if (rc == CAI_OK) {
    rc = cai_response_create_params_new(&params, error);
  }
  if (rc == CAI_OK) {
    rc = cai_response_create_params_set_model(params, CAI_SESSION_AGENT_IMPL(session)->model,
                                              error);
  }
  if (rc == CAI_OK && CAI_SESSION_AGENT_IMPL(session)->developer_instructions != NULL) {
    rc = cai_response_create_params_set_instructions(
        params, CAI_SESSION_AGENT_IMPL(session)->developer_instructions, error);
  }
  if (rc == CAI_OK) {
    rc = cai_response_create_params_set_raw_input_spooled(params, &history_items,
                                                          error);
    if (rc == CAI_OK) {
      has_history_items = 0;
    }
  }
  if (rc == CAI_OK) {
    rc = cai_response_create_params_spool_json(params, 0, &request_json,
                                               &request_json_len, error);
    if (rc == CAI_OK) {
      has_request_json = 1;
    }
  }
  if (rc == CAI_OK) {
    rc = cai_http_json_request_spooled(
        CAI_SESSION_AGENT_IMPL(session)->client, "POST", "responses/compact", &request_json,
        request_json_len, &body, &http_status, &request_id, error);
  }
  if (rc == CAI_OK && (http_status < 200L || http_status >= 300L)) {
    rc = cai_set_openai_error(error, http_status, body, request_id);
  }
  if (rc == CAI_OK) {
    rc = cai_response_parse_json(body != NULL ? body : "", &response, error);
  }
  if (rc == CAI_OK) {
    rc = cai_response_output_items_spool(response, &output_items,
                                         &output_items_len, error);
    if (rc == CAI_OK) {
      has_output_items = 1;
    }
  }
  if (rc == CAI_OK) {
    cai_history_reset(session);
    if (output_items_len > 0U) {
      rc = cai_history_append_spooled(session, &output_items, error);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_session_remember_response(session, response, error);
  }

done:
  cai_response_create_params_destroy(params);
  cai_response_destroy(response);
  if (has_request_json) {
    lonejson_spooled_cleanup(&request_json);
  }
  if (has_history_items) {
    lonejson_spooled_cleanup(&history_items);
  }
  cai_free_mem(NULL, body);
  cai_free_mem(NULL, request_id);
  if (has_output_items) {
    lonejson_spooled_cleanup(&output_items);
  }
  return rc;
}

static int cai_session_after_response(cai_session *session,
                                      const lonejson_spooled *pending_items,
                                      int has_pending_items,
                                      const cai_response *response,
                                      cai_error *error) {
  lonejson_spooled output_items;
  size_t output_items_len;
  int has_output_items;
  int rc;

  rc = cai_session_remember_response(session, response, error);
  if (rc != CAI_OK || !CAI_SESSION_AGENT_IMPL(session)->local_history_enabled) {
    return rc;
  }
  memset(&output_items, 0, sizeof(output_items));
  output_items_len = 0U;
  has_output_items = 0;
  rc = cai_response_output_items_spool(response, &output_items, &output_items_len,
                                       error);
  if (rc == CAI_OK) {
    has_output_items = 1;
  }
  if (rc == CAI_OK && has_pending_items) {
    rc = cai_history_append_spooled(session, pending_items, error);
  }
  if (rc == CAI_OK && output_items_len > 0U) {
    rc = cai_history_append_spooled(session, &output_items, error);
  }
  if (has_output_items) {
    lonejson_spooled_cleanup(&output_items);
  }
  return rc;
}

static int cai_session_after_stream(cai_session *session,
                                    const lonejson_spooled *pending_items,
                                    int has_pending_items,
                                    const char *response_id,
                                    const cai_token_usage *usage,
                                    cai_error *error) {
  cai_response *response;
  lonejson_spooled output_items;
  size_t output_items_len;
  int has_output_items;
  int rc;

  response = NULL;
  memset(&output_items, 0, sizeof(output_items));
  output_items_len = 0U;
  has_output_items = 0;
  if (!CAI_SESSION_AGENT_IMPL(session)->local_history_enabled) {
    return cai_session_remember_stream(session, response_id, usage, error);
  }
  rc = cai_session_remember_response_id(session, response_id, error);
  if (rc == CAI_OK && !cai_token_usage_is_empty(usage)) {
    CAI_SESSION_IMPL(session)->last_usage = *usage;
    CAI_SESSION_IMPL(session)->has_last_usage = 1;
  }
  if (rc == CAI_OK) {
    rc = cai_client_retrieve_response(CAI_SESSION_AGENT_IMPL(session)->client,
                                      response_id, &response, error);
  }
  if (rc == CAI_OK && cai_token_usage_is_empty(usage)) {
    CAI_SESSION_IMPL(session)->last_usage.input_tokens =
        cai_response_input_tokens(response);
    CAI_SESSION_IMPL(session)->last_usage.input_cached_tokens =
        cai_response_input_cached_tokens(response);
    CAI_SESSION_IMPL(session)->last_usage.output_tokens =
        cai_response_output_tokens(response);
    CAI_SESSION_IMPL(session)->last_usage.output_reasoning_tokens =
        cai_response_output_reasoning_tokens(response);
    CAI_SESSION_IMPL(session)->last_usage.total_tokens =
        cai_response_total_tokens(response);
    CAI_SESSION_IMPL(session)->has_last_usage = 1;
  }
  if (rc == CAI_OK) {
    rc = cai_response_output_items_spool(response, &output_items,
                                         &output_items_len, error);
    if (rc == CAI_OK) {
      has_output_items = 1;
    }
  }
  if (rc == CAI_OK && has_pending_items) {
    rc = cai_history_append_spooled(session, pending_items, error);
  }
  if (rc == CAI_OK && output_items_len > 0U) {
    rc = cai_history_append_spooled(session, &output_items, error);
  }
  cai_response_destroy(response);
  if (has_output_items) {
    lonejson_spooled_cleanup(&output_items);
  }
  return rc;
}

static int cai_session_remember_response_id(cai_session *session,
                                            const char *response_id,
                                            cai_error *error) {
  char *next_response_id;

  if (response_id == NULL || response_id[0] == '\0') {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "streaming response did not include a response id");
  }
  next_response_id =
      cai_strdup(&CAI_SESSION_CLIENT_IMPL(session)->allocator, response_id);
  if (next_response_id == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to remember previous response id");
  }
  cai_free_mem(&CAI_SESSION_CLIENT_IMPL(session)->allocator,
               CAI_SESSION_IMPL(session)->previous_response_id);
  CAI_SESSION_IMPL(session)->previous_response_id = next_response_id;
  return CAI_OK;
}

static int cai_session_remember_stream(cai_session *session,
                                       const char *response_id,
                                       const cai_token_usage *usage,
                                       cai_error *error) {
  cai_response *response;
  int rc;

  rc = cai_session_remember_response_id(session, response_id, error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (!cai_token_usage_is_empty(usage)) {
    CAI_SESSION_IMPL(session)->last_usage = *usage;
    CAI_SESSION_IMPL(session)->has_last_usage = 1;
    return CAI_OK;
  }
  response = NULL;
  rc = cai_client_retrieve_response(CAI_SESSION_AGENT_IMPL(session)->client, response_id,
                                    &response, error);
  if (rc == CAI_OK) {
    rc = cai_session_remember_response(session, response, error);
  }
  cai_response_destroy(response);
  if (rc != CAI_OK) {
    CAI_SESSION_IMPL(session)->has_last_usage = 0;
  }
  return rc;
}

static int cai_session_stream_complete(void *context, const char *response_id,
                                       const cai_token_usage *usage) {
  return cai_session_remember_stream((cai_session *)context, response_id, usage,
                                     NULL);
}

static int cai_session_create_response_from_params(
    cai_session *session, cai_response_create_params *params,
    const lonejson_spooled *pending_items, int has_pending_items,
    cai_response **out, cai_error *error) {
  cai_response *response;
  int rc;

  response = NULL;
  rc = cai_client_create_response(CAI_SESSION_AGENT_IMPL(session)->client, params, &response,
                                  error);
  if (rc != CAI_OK) {
    cai_response_destroy(response);
    return rc;
  }
  rc = cai_session_after_response(session, pending_items, has_pending_items,
                                  response, error);
  if (rc != CAI_OK) {
    cai_response_destroy(response);
    return rc;
  }
  *out = response;
  return CAI_OK;
}

int cai_session_run(cai_session *session, cai_response **out,
                    cai_error *error) {
  cai_response_create_params *params;
  lonejson_spooled pending_items;
  int has_pending_items;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response output pointer is required");
  }
  *out = NULL;
  if (session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "session is required");
  }
  if (CAI_SESSION_IMPL(session)->input_count == 0U) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session has no pending input");
  }
  params = NULL;
  memset(&pending_items, 0, sizeof(pending_items));
  has_pending_items = 0;
  rc = cai_session_init_response_params(session, &params, error);
  if (rc == CAI_OK) {
    rc = cai_session_prepare_history_params(session, params,
                                            &pending_items, &has_pending_items,
                                            error);
  }
  if (rc == CAI_OK) {
    rc = cai_session_create_response_from_params(
        session, params, &pending_items, has_pending_items, out, error);
  }
  cai_response_create_params_destroy(params);
  if (has_pending_items) {
    lonejson_spooled_cleanup(&pending_items);
  }
  if (rc == CAI_OK) {
    cai_session_clear_inputs(session);
  }
  return rc;
}

int cai_session_run_output(cai_session *session, cai_output **out,
                           cai_error *error) {
  cai_response *response;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "output pointer is required");
  }
  *out = NULL;
  response = NULL;
  rc = cai_session_run(session, &response, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_output_from_response(response, out, error);
  if (rc != CAI_OK) {
    cai_response_destroy(response);
  }
  return rc;
}

static int cai_capture_tool_output(void *context, const void *bytes,
                                   size_t count, cai_error *error) {
  cai_tool_output_capture *capture;
  lonejson_error json_error;

  capture = (cai_tool_output_capture *)context;
  lonejson_error_init(&json_error);
  if (lonejson_spooled_append(&capture->output, bytes, count, &json_error) ==
      LONEJSON_STATUS_OK) {
    return CAI_OK;
  }
  return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                              "failed to spool tool output",
                              json_error.message);
}

static void cai_capture_cleanup(cai_tool_output_capture *capture) {
  if (capture == NULL) {
    return;
  }
  lonejson_spooled_cleanup(&capture->output);
}

static int cai_session_init_response_params(cai_session *session,
                                            cai_response_create_params **out,
                                            cai_error *error) {
  cai_response_create_params *params;
  int rc;

  params = NULL;
  rc = cai_response_create_params_new(&params, error);
  if (rc == CAI_OK) {
    rc = cai_response_create_params_set_model(params, CAI_SESSION_AGENT_IMPL(session)->model,
                                              error);
  }
  if (rc == CAI_OK && CAI_SESSION_AGENT_IMPL(session)->developer_instructions != NULL) {
    rc = cai_response_create_params_set_instructions(
        params, CAI_SESSION_AGENT_IMPL(session)->developer_instructions, error);
  }
  if (rc == CAI_OK && CAI_SESSION_AGENT_IMPL(session)->prompt_cache_key != NULL) {
    rc = cai_response_create_params_set_prompt_cache_key(
        params, CAI_SESSION_AGENT_IMPL(session)->prompt_cache_key, error);
  }
  if (rc == CAI_OK && CAI_SESSION_AGENT_IMPL(session)->max_output_tokens > 0) {
    rc = cai_response_create_params_set_max_output_tokens(
        params, CAI_SESSION_AGENT_IMPL(session)->max_output_tokens, error);
  }
  if (rc == CAI_OK && (CAI_SESSION_AGENT_IMPL(session)->reasoning_effort != NULL ||
                       CAI_SESSION_AGENT_IMPL(session)->reasoning_summary != NULL)) {
    rc = cai_response_create_params_set_reasoning(
        params, CAI_SESSION_AGENT_IMPL(session)->reasoning_effort,
        CAI_SESSION_AGENT_IMPL(session)->reasoning_summary, error);
  }
  if (rc == CAI_OK && CAI_SESSION_AGENT_IMPL(session)->text_format_schema_json != NULL) {
    rc = cai_response_create_params_set_text_format_json_schema(
        params, CAI_SESSION_AGENT_IMPL(session)->text_format_name,
        CAI_SESSION_AGENT_IMPL(session)->text_format_description,
        CAI_SESSION_AGENT_IMPL(session)->text_format_schema_json,
        CAI_SESSION_AGENT_IMPL(session)->text_format_strict, error);
  }
  if (rc == CAI_OK && CAI_SESSION_AGENT_IMPL(session)->parallel_tool_calls >= 0) {
    rc = cai_response_create_params_set_parallel_tool_calls(
        params, CAI_SESSION_AGENT_IMPL(session)->parallel_tool_calls, error);
  }
  if (rc == CAI_OK && CAI_SESSION_AGENT_IMPL(session)->auto_compact &&
      CAI_SESSION_AGENT_IMPL(session)->auto_compact_token_limit > 0LL) {
    rc = cai_response_create_params_set_compact_threshold(
        params, CAI_SESSION_AGENT_IMPL(session)->auto_compact_token_limit, error);
  }
  if (rc == CAI_OK && CAI_SESSION_IMPL(session)->conversation_id != NULL) {
    rc = cai_response_create_params_set_conversation_id(
        params, CAI_SESSION_IMPL(session)->conversation_id, error);
  }
  if (rc == CAI_OK && CAI_SESSION_IMPL(session)->previous_response_id != NULL) {
    rc = cai_response_create_params_set_previous_response_id(
        params, CAI_SESSION_IMPL(session)->previous_response_id, error);
  }
  if (rc == CAI_OK) {
    rc = cai_tool_registry_add_to_response_params(CAI_SESSION_AGENT_IMPL(session)->tools, params,
                                                  error);
  }
  if (rc != CAI_OK) {
    cai_response_create_params_destroy(params);
    return rc;
  }
  *out = params;
  return CAI_OK;
}

static int cai_session_run_tool_round(cai_session *session,
                                      const cai_response *response,
                                      const cai_run_options *options,
                                      cai_response **out, cai_error *error) {
  cai_response_create_params *params;
  cai_sink_callbacks callbacks;
  cai_sink *sink;
  cai_tool_output_capture capture;
  lonejson_spool_options spool_options;
  size_t i;
  int rc;

  params = NULL;
  rc = cai_session_init_response_params(session, &params, error);
  spool_options = lonejson_default_spool_options();
  spool_options.memory_limit = options->tool_output_memory_limit;
  spool_options.max_bytes = 0U;
  spool_options.temp_dir = options->tool_spool_dir;
  for (i = 0U; rc == CAI_OK && i < cai_response_tool_call_count(response);
       i++) {
    lonejson_spooled_init(&capture.output, &spool_options);
    callbacks.write = cai_capture_tool_output;
    callbacks.close = NULL;
    callbacks.context = &capture;
    sink = NULL;
    rc = cai_sink_from_callbacks(&callbacks, &sink, error);
    if (rc == CAI_OK) {
      rc = cai_tool_registry_run(
          CAI_SESSION_AGENT_IMPL(session)->tools, cai_response_tool_call_name(response, i),
          cai_response_tool_call_arguments(response, i), sink, error);
    }
    cai_sink_close(sink);
    if (rc == CAI_OK) {
      rc = cai_response_create_params_add_function_call_output_spooled(
          params, cai_response_tool_call_id(response, i), &capture.output,
          error);
      if (rc == CAI_OK) {
        memset(&capture.output, 0, sizeof(capture.output));
      }
    }
    cai_capture_cleanup(&capture);
  }
  if (rc == CAI_OK) {
    rc = cai_client_create_response(CAI_SESSION_AGENT_IMPL(session)->client, params, out, error);
  }
  cai_response_create_params_destroy(params);
  return rc;
}

int cai_session_run_auto(cai_session *session, const cai_run_options *options,
                         cai_response **out, cai_error *error) {
  cai_run_options defaults;
  const cai_run_options *effective;
  cai_response *current;
  cai_response *next;
  int rounds;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response output pointer is required");
  }
  *out = NULL;
  if (session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "session is required");
  }
  cai_run_options_init(&defaults);
  effective = options != NULL ? options : &defaults;
  if (effective->max_tool_rounds < 0) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "max tool rounds cannot be negative");
  }
  current = NULL;
  rc = cai_session_run(session, &current, error);
  rounds = 0;
  while (rc == CAI_OK && cai_response_tool_call_count(current) > 0U &&
         rounds < effective->max_tool_rounds) {
    if (cai_session_remember_response(session, current, error) != CAI_OK) {
      rc = error != NULL ? error->code : CAI_ERR_NOMEM;
      break;
    }
    next = NULL;
    rc = cai_session_run_tool_round(session, current, effective, &next, error);
    cai_response_destroy(current);
    current = next;
    rounds++;
    if (rc == CAI_OK) {
      rc = cai_session_remember_response(session, current, error);
    }
  }
  if (rc != CAI_OK) {
    cai_response_destroy(current);
    return rc;
  }
  if (cai_response_tool_call_count(current) > 0U) {
    cai_response_destroy(current);
    return cai_set_error(error, CAI_ERR_CANCELLED,
                         "tool auto-run exhausted max tool rounds");
  }
  *out = current;
  return CAI_OK;
}

int cai_session_run_auto_output(cai_session *session,
                                const cai_run_options *options,
                                cai_output **out, cai_error *error) {
  cai_response *response;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "output pointer is required");
  }
  *out = NULL;
  response = NULL;
  rc = cai_session_run_auto(session, options, &response, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_output_from_response(response, out, error);
  if (rc != CAI_OK) {
    cai_response_destroy(response);
  }
  return rc;
}

int cai_session_stream(cai_session *session, const cai_stream_sinks *sinks,
                       cai_error *error) {
  cai_response_create_params *params;
  char *response_id;
  lonejson_spooled pending_items;
  int has_pending_items;
  cai_token_usage usage;
  int rc;

  if (session == NULL || sinks == NULL ||
      (sinks->output_text == NULL && sinks->reasoning_summary == NULL)) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session and at least one stream sink are required");
  }
  params = NULL;
  response_id = NULL;
  memset(&pending_items, 0, sizeof(pending_items));
  has_pending_items = 0;
  memset(&usage, 0, sizeof(usage));
  rc = cai_session_init_response_params(session, &params, error);
  if (rc == CAI_OK) {
    rc = cai_session_prepare_history_params(session, params,
                                            &pending_items, &has_pending_items,
                                            error);
  }
  if (rc == CAI_OK) {
    rc = cai_client_stream_response_with_id(
        CAI_SESSION_AGENT_IMPL(session)->client, params, sinks, &response_id,
        &usage, error);
  }
  if (rc == CAI_OK) {
    rc = cai_session_after_stream(session, &pending_items, has_pending_items,
                                  response_id, &usage, error);
  }
  cai_response_create_params_destroy(params);
  cai_free_mem(NULL, response_id);
  if (has_pending_items) {
    lonejson_spooled_cleanup(&pending_items);
  }
  if (rc == CAI_OK) {
    cai_session_clear_inputs(session);
  }
  return rc;
}

int cai_session_stream_text(cai_session *session, cai_sink *sink,
                            cai_error *error) {
  cai_stream_sinks sinks;

  cai_stream_sinks_init(&sinks);
  sinks.output_text = sink;
  return cai_session_stream(session, &sinks, error);
}

int cai_session_open_text_source(cai_session *session, cai_source **out,
                                 cai_error *error) {
  cai_response_create_params *params;
  lonejson_spooled pending_items;
  int has_pending_items;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "source output pointer is required");
  }
  *out = NULL;
  if (session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "session is required");
  }
  if (CAI_SESSION_AGENT_IMPL(session)->local_history_enabled) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "local history capture is not supported for sources");
  }
  params = NULL;
  memset(&pending_items, 0, sizeof(pending_items));
  has_pending_items = 0;
  rc = cai_session_init_response_params(session, &params, error);
  if (rc == CAI_OK) {
    rc = cai_session_prepare_history_params(session, params,
                                            &pending_items, &has_pending_items,
                                            error);
  }
  if (rc == CAI_OK) {
    rc = cai_client_open_response_text_source_with_complete(
        CAI_SESSION_AGENT_IMPL(session)->client, params, cai_session_stream_complete, session,
        out, error);
  }
  cai_response_create_params_destroy(params);
  if (has_pending_items) {
    lonejson_spooled_cleanup(&pending_items);
  }
  if (rc == CAI_OK) {
    cai_session_clear_inputs(session);
  }
  return rc;
}

int cai_session_send_text(cai_session *session, const char *text,
                          cai_response **out, cai_error *error) {
  int rc;

  if (session == NULL || text == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session and text are required");
  }
  rc = cai_session_add_user_text(session, text, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_session_run(session, out, error);
  if (rc != CAI_OK) {
    cai_session_clear_inputs(session);
  }
  return rc;
}

int cai_session_last_usage(const cai_session *session, cai_token_usage *out,
                           cai_error *error) {
  if (session == NULL || out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session and usage output are required");
  }
  if (!CAI_SESSION_IMPL(session)->has_last_usage) {
    memset(out, 0, sizeof(*out));
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session has no completed response usage");
  }
  *out = CAI_SESSION_IMPL(session)->last_usage;
  return CAI_OK;
}

long long cai_session_context_window_tokens(const cai_session *session) {
  if (session == NULL || CAI_SESSION_IMPL(session)->agent == NULL) {
    return 0LL;
  }
  return cai_model_context_window_tokens(CAI_SESSION_AGENT_IMPL(session)->model);
}

long long cai_session_auto_compact_token_limit(const cai_session *session) {
  if (session == NULL || CAI_SESSION_IMPL(session)->agent == NULL) {
    return 0LL;
  }
  return CAI_SESSION_AGENT_IMPL(session)->auto_compact_token_limit;
}

int cai_session_context_percent(const cai_session *session, double *out,
                                cai_error *error) {
  long long window;

  if (session == NULL || out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session and percent output are required");
  }
  if (!CAI_SESSION_IMPL(session)->has_last_usage) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session has no completed response usage");
  }
  window = cai_session_context_window_tokens(session);
  if (window <= 0LL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session model has unknown context window");
  }
  *out = ((double)CAI_SESSION_IMPL(session)->last_usage.total_tokens * 100.0) / (double)window;
  return CAI_OK;
}

int cai_session_history_spilled(const cai_session *session) {
  if (session == NULL) {
    return 0;
  }
  return lonejson_spooled_spilled(&CAI_SESSION_IMPL(session)->history);
}

static int cai_json_is_ws(unsigned char ch) {
  return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t';
}

static lonejson_read_result cai_history_spooled_reader(void *user,
                                                       unsigned char *buffer,
                                                       size_t capacity) {
  cai_spooled_reader_context *context;
  lonejson_read_result result;

  context = (cai_spooled_reader_context *)user;
  if (context == NULL) {
    result = lonejson_default_read_result();
    result.eof = 1;
    result.error_code = 1;
    return result;
  }
  return lonejson_spooled_read(&context->cursor, buffer, capacity);
}

static int cai_history_source_to_array_spooled(cai_session *session,
                                               cai_source *source,
                                               lonejson_spooled *out,
                                               cai_error *error) {
  lonejson_spooled raw;
  lonejson_spooled trimmed;
  lonejson_error json_error;
  cai_spooled_reader_context reader_context;
  unsigned char buffer[4096];
  size_t nread;
  size_t i;
  size_t pos;
  size_t first_pos;
  size_t last_pos;
  size_t global_start;
  size_t global_end;
  size_t start;
  size_t end;
  unsigned char first_ch;
  unsigned char last_ch;
  lonejson_read_result chunk;
  int have_nonspace;
  int has_raw;
  int has_trimmed;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "history spool output pointer is required");
  }
  memset(&raw, 0, sizeof(raw));
  memset(&trimmed, 0, sizeof(trimmed));
  has_raw = 0;
  has_trimmed = 0;
  pos = 0U;
  first_pos = 0U;
  last_pos = 0U;
  first_ch = 0U;
  last_ch = 0U;
  have_nonspace = 0;
  rc = CAI_OK;
  cai_history_init_spooled(session, &raw);
  has_raw = 1;
  for (;;) {
    nread = cai_source_read(source, buffer, sizeof(buffer), error);
    if (nread == 0U) {
      break;
    }
    lonejson_error_init(&json_error);
    if (lonejson_spooled_append(&raw, buffer, nread, &json_error) !=
        LONEJSON_STATUS_OK) {
      rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to append imported history",
                                json_error.message);
      goto done;
    }
    for (i = 0U; i < nread; i++) {
      if (!cai_json_is_ws(buffer[i])) {
        if (!have_nonspace) {
          first_pos = pos + i;
          first_ch = buffer[i];
          have_nonspace = 1;
        }
        last_pos = pos + i;
        last_ch = buffer[i];
      }
    }
    pos += nread;
  }
  if (!have_nonspace) {
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "imported history JSON is empty");
    goto done;
  }
  if (first_ch != '[' || last_ch != ']') {
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "imported history must be a JSON array");
    goto done;
  }
  reader_context.cursor = raw;
  lonejson_error_init(&json_error);
  if (lonejson_spooled_rewind(&reader_context.cursor, &json_error) !=
      LONEJSON_STATUS_OK) {
    rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                              "failed to rewind imported history",
                              json_error.message);
    goto done;
  }
  lonejson_error_init(&json_error);
  if (lonejson_validate_reader(cai_history_spooled_reader, &reader_context,
                               &json_error) != LONEJSON_STATUS_OK) {
    rc = cai_set_error_detail(error, CAI_ERR_INVALID,
                              "imported history is not valid JSON",
                              json_error.message);
    goto done;
  }
  cai_history_init_spooled(session, &trimmed);
  has_trimmed = 1;
  lonejson_error_init(&json_error);
  if (lonejson_spooled_rewind(&raw, &json_error) != LONEJSON_STATUS_OK) {
    rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                              "failed to rewind imported history",
                              json_error.message);
    goto done;
  }
  pos = 0U;
  for (;;) {
    chunk = lonejson_spooled_read(&raw, buffer, sizeof(buffer));
    if (chunk.error_code != 0) {
      rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to read imported history");
      goto done;
    }
    if (chunk.bytes_read > 0U) {
      global_start = pos;
      global_end = pos + chunk.bytes_read - 1U;
      if (!(global_end < first_pos || global_start > last_pos)) {
        start = first_pos > global_start ? first_pos - global_start : 0U;
        end = last_pos < global_end ? last_pos - global_start + 1U
                                    : chunk.bytes_read;
        lonejson_error_init(&json_error);
        if (lonejson_spooled_append(&trimmed, buffer + start, end - start,
                                    &json_error) != LONEJSON_STATUS_OK) {
          rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                    "failed to copy imported history",
                                    json_error.message);
          goto done;
        }
      }
      pos += chunk.bytes_read;
    }
    if (chunk.eof) {
      break;
    }
  }
  *out = trimmed;
  has_trimmed = 0;

done:
  if (has_raw) {
    lonejson_spooled_cleanup(&raw);
  }
  if (has_trimmed) {
    lonejson_spooled_cleanup(&trimmed);
  }
  return rc;
}

int cai_session_export_history_source(cai_session *session, cai_source **out,
                                      cai_error *error) {
  lonejson_spooled history_json;
  int has_history_json;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "history source output pointer is required");
  }
  *out = NULL;
  if (session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "session is required");
  }
  if (!CAI_SESSION_AGENT_IMPL(session)->local_history_enabled) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "local history capture is disabled");
  }
  memset(&history_json, 0, sizeof(history_json));
  has_history_json = 0;
  rc = cai_history_to_array_spool(session, &history_json, error);
  if (rc == CAI_OK) {
    has_history_json = 1;
    rc = cai_source_from_spooled(&history_json, out, error);
    if (rc == CAI_OK) {
      has_history_json = 0;
    }
  }
  if (has_history_json) {
    lonejson_spooled_cleanup(&history_json);
  }
  return rc;
}

int cai_session_import_history_source(cai_session *session, cai_source *source,
                                      cai_error *error) {
  lonejson_spooled history_json;
  int has_history_json;
  int rc;

  if (session == NULL || source == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session and history source are required");
  }
  if (!CAI_SESSION_AGENT_IMPL(session)->local_history_enabled) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "local history capture is disabled");
  }
  memset(&history_json, 0, sizeof(history_json));
  has_history_json = 0;
  rc = cai_history_source_to_array_spooled(session, source, &history_json,
                                           error);
  if (rc == CAI_OK) {
    has_history_json = 1;
    cai_history_reset(session);
    rc = cai_history_append_array_record_spooled(session, &history_json,
                                                 error);
  }
  if (has_history_json) {
    lonejson_spooled_cleanup(&history_json);
  }
  return rc;
}

static void cai_agent_init_methods(cai_agent *agent) {
  agent->register_tool = cai_agent_register_tool;
  agent->register_raw_tool = cai_agent_register_raw_tool;
  agent->new_session = cai_agent_new_session;
  agent->new_conversation_session = cai_agent_new_conversation_session;
  agent->new_session_for_conversation = cai_agent_new_session_for_conversation;
  agent->add_user_text = cai_agent_add_user_text;
  agent->add_user_image_url = cai_agent_add_user_image_url;
  agent->run = cai_agent_run;
  agent->run_output = cai_agent_run_output;
  agent->run_auto = cai_agent_run_auto;
  agent->run_auto_output = cai_agent_run_auto_output;
  agent->stream = cai_agent_stream;
  agent->stream_text = cai_agent_stream_text;
  agent->open_text_source = cai_agent_open_text_source;
  agent->send_text = cai_agent_send_text;
  agent->last_usage = cai_agent_last_usage;
  agent->context_percent = cai_agent_context_percent;
  agent->close = cai_agent_destroy;
}

static void cai_session_init_methods(cai_session *session) {
  session->set_conversation_id = cai_session_set_conversation_id;
  session->set_conversation = cai_session_set_conversation;
  session->conversation_id = cai_session_conversation_id;
  session->set_previous_response_id = cai_session_set_previous_response_id;
  session->previous_response_id = cai_session_previous_response_id;
  session->add_user_text = cai_session_add_user_text;
  session->add_user_image_url = cai_session_add_user_image_url;
  session->add_function_call_output = cai_session_add_function_call_output;
  session->run = cai_session_run;
  session->run_output = cai_session_run_output;
  session->run_auto = cai_session_run_auto;
  session->run_auto_output = cai_session_run_auto_output;
  session->stream = cai_session_stream;
  session->stream_text = cai_session_stream_text;
  session->open_text_source = cai_session_open_text_source;
  session->send_text = cai_session_send_text;
  session->last_usage = cai_session_last_usage;
  session->context_window_tokens = cai_session_context_window_tokens;
  session->auto_compact_token_limit = cai_session_auto_compact_token_limit;
  session->context_percent = cai_session_context_percent;
  session->history_spilled = cai_session_history_spilled;
  session->export_history_source = cai_session_export_history_source;
  session->import_history_source = cai_session_import_history_source;
  session->close = cai_session_destroy;
}

static int cai_agent_default_session(cai_agent *agent, cai_session **out,
                                     cai_error *error) {
  cai_agent_impl *impl;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session output pointer is required");
  }
  *out = NULL;
  if (agent == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  impl = CAI_AGENT_IMPL(agent);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is closed");
  }
  if (impl->default_session == NULL) {
    rc = cai_agent_new_session(agent, &impl->default_session, error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  *out = impl->default_session;
  return CAI_OK;
}

static int cai_agent_add_user_text(cai_agent *agent, const char *text,
                                   cai_error *error) {
  cai_session *session;
  int rc;

  rc = cai_agent_default_session(agent, &session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_session_add_user_text(session, text, error);
}

static int cai_agent_add_user_image_url(cai_agent *agent, const char *url,
                                        const char *detail, cai_error *error) {
  cai_session *session;
  int rc;

  rc = cai_agent_default_session(agent, &session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_session_add_user_image_url(session, url, detail, error);
}

static int cai_agent_run(cai_agent *agent, cai_response **out,
                         cai_error *error) {
  cai_session *session;
  int rc;

  rc = cai_agent_default_session(agent, &session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_session_run(session, out, error);
}

static int cai_agent_run_output(cai_agent *agent, cai_output **out,
                                cai_error *error) {
  cai_session *session;
  int rc;

  rc = cai_agent_default_session(agent, &session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_session_run_output(session, out, error);
}

static int cai_agent_run_auto(cai_agent *agent, const cai_run_options *options,
                              cai_response **out, cai_error *error) {
  cai_session *session;
  int rc;

  rc = cai_agent_default_session(agent, &session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_session_run_auto(session, options, out, error);
}

static int cai_agent_run_auto_output(cai_agent *agent,
                                     const cai_run_options *options,
                                     cai_output **out, cai_error *error) {
  cai_session *session;
  int rc;

  rc = cai_agent_default_session(agent, &session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_session_run_auto_output(session, options, out, error);
}

static int cai_agent_stream_text(cai_agent *agent, cai_sink *sink,
                                 cai_error *error) {
  cai_stream_sinks sinks;

  cai_stream_sinks_init(&sinks);
  sinks.output_text = sink;
  return cai_agent_stream(agent, &sinks, error);
}

static int cai_agent_stream(cai_agent *agent, const cai_stream_sinks *sinks,
                            cai_error *error) {
  cai_session *session;
  int rc;

  rc = cai_agent_default_session(agent, &session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_session_stream(session, sinks, error);
}

static int cai_agent_open_text_source(cai_agent *agent, cai_source **out,
                                      cai_error *error) {
  cai_session *session;
  int rc;

  rc = cai_agent_default_session(agent, &session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_session_open_text_source(session, out, error);
}

static int cai_agent_send_text(cai_agent *agent, const char *text,
                               cai_response **out, cai_error *error) {
  cai_session *session;
  int rc;

  rc = cai_agent_default_session(agent, &session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_session_send_text(session, text, out, error);
}

static int cai_agent_last_usage(const cai_agent *agent, cai_token_usage *out,
                                cai_error *error) {
  const cai_agent_impl *impl;

  if (agent == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  impl = CAI_AGENT_IMPL(agent);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is closed");
  }
  if (impl->default_session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "agent has no default session usage");
  }
  return cai_session_last_usage(impl->default_session, out, error);
}

static int cai_agent_context_percent(const cai_agent *agent, double *out,
                                     cai_error *error) {
  const cai_agent_impl *impl;

  if (agent == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  impl = CAI_AGENT_IMPL(agent);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is closed");
  }
  if (impl->default_session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "agent has no default session usage");
  }
  return cai_session_context_percent(impl->default_session, out, error);
}
