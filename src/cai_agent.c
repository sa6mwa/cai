#include "cai_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct cai_tool_output_capture {
  lonejson_spooled output;
} cai_tool_output_capture;

typedef struct cai_session_stream_context {
  cai_session *session;
  char *pending_items_json;
} cai_session_stream_context;

typedef struct cai_history_json_doc {
  lonejson_json_value value;
} cai_history_json_doc;

typedef struct cai_history_sink_context {
  lonejson_spooled *spool;
} cai_history_sink_context;

typedef struct cai_segment_reader {
  const char *segments[3];
  size_t lengths[3];
  size_t index;
  size_t offset;
} cai_segment_reader;

typedef struct cai_spooled_record_reader {
  lonejson_spooled cursor;
  unsigned char buffer[4096];
  size_t offset;
  size_t length;
  int eof;
} cai_spooled_record_reader;

static const lonejson_field cai_history_json_fields[] = {
    LONEJSON_FIELD_JSON_VALUE_REQ(cai_history_json_doc, value, "value")};
LONEJSON_MAP_DEFINE(cai_history_json_map, cai_history_json_doc,
                    cai_history_json_fields);

enum {
  CAI_SESSION_INPUT_TEXT = 0,
  CAI_SESSION_INPUT_IMAGE = 1,
  CAI_SESSION_INPUT_FUNCTION_CALL_OUTPUT = 2
};

static int cai_history_append(cai_session *session, const char *json,
                              cai_error *error);
static void cai_history_cleanup(cai_session *session);
static int
cai_session_prepare_history_params(cai_session *session,
                                   cai_response_create_params *params,
                                   char **out_pending_items, cai_error *error);
static int cai_session_after_response(cai_session *session,
                                      const char *pending_items_json,
                                      const cai_response *response,
                                      cai_error *error);
static int cai_session_after_stream(cai_session *session,
                                    const char *pending_items_json,
                                    const char *response_id,
                                    const cai_token_usage *usage,
                                    cai_error *error);
static int cai_session_maybe_compact(cai_session *session, cai_error *error);
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

void cai_agent_config_init(cai_agent_config *config) {
  if (config == NULL) {
    return;
  }
  config->model = NULL;
  config->instructions = NULL;
  config->reasoning_effort = NULL;
  config->reasoning_summary = NULL;
  config->text_format_name = NULL;
  config->text_format_description = NULL;
  config->text_format_schema_json = NULL;
  config->text_format_strict = 0;
  config->max_output_tokens = 0;
  config->parallel_tool_calls = -1;
  config->auto_compact = 0;
  config->auto_compact_token_limit = 0LL;
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
  agent = (cai_agent *)cai_alloc(&client->allocator, sizeof(*agent));
  if (agent == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate agent");
  }
  agent->client = client;
  agent->model = cai_strdup(&client->allocator, config->model);
  agent->instructions = cai_strdup(&client->allocator, config->instructions);
  agent->reasoning_effort =
      cai_strdup(&client->allocator, config->reasoning_effort);
  agent->reasoning_summary =
      cai_strdup(&client->allocator, config->reasoning_summary);
  agent->text_format_name =
      cai_strdup(&client->allocator, config->text_format_name);
  agent->text_format_description =
      cai_strdup(&client->allocator, config->text_format_description);
  agent->text_format_schema_json =
      cai_strdup(&client->allocator, config->text_format_schema_json);
  agent->text_format_strict = config->text_format_strict;
  agent->max_output_tokens = config->max_output_tokens;
  agent->parallel_tool_calls = config->parallel_tool_calls;
  agent->auto_compact =
      config->auto_compact || config->auto_compact_token_limit > 0LL ? 1 : 0;
  if (config->auto_compact_token_limit > 0LL) {
    agent->auto_compact_token_limit = config->auto_compact_token_limit;
  } else if (agent->auto_compact) {
    agent->auto_compact_token_limit =
        cai_model_auto_compact_token_limit(agent->model);
  } else {
    agent->auto_compact_token_limit = 0LL;
  }
  agent->history_memory_limit = config->history_memory_limit != 0U
                                    ? config->history_memory_limit
                                    : 128U * 1024U;
  agent->history_spool_dir =
      cai_strdup(&client->allocator, config->history_spool_dir);
  agent->tools = NULL;
  if (agent->model == NULL ||
      (config->instructions != NULL && agent->instructions == NULL) ||
      (config->reasoning_effort != NULL && agent->reasoning_effort == NULL) ||
      (config->reasoning_summary != NULL && agent->reasoning_summary == NULL) ||
      (config->text_format_name != NULL && agent->text_format_name == NULL) ||
      (config->text_format_description != NULL &&
       agent->text_format_description == NULL) ||
      (config->text_format_schema_json != NULL &&
       agent->text_format_schema_json == NULL) ||
      (config->history_spool_dir != NULL && agent->history_spool_dir == NULL)) {
    cai_agent_destroy(agent);
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate agent");
  }
  if (agent->auto_compact && agent->auto_compact_token_limit <= 0LL) {
    cai_agent_destroy(agent);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "model has unknown auto compact token limit");
  }
  if (cai_tool_registry_new(&agent->tools, error) != CAI_OK) {
    cai_agent_destroy(agent);
    return error != NULL ? error->code : CAI_ERR_NOMEM;
  }
  *out = agent;
  return CAI_OK;
}

void cai_agent_destroy(cai_agent *agent) {
  cai_allocator *allocator;

  if (agent == NULL) {
    return;
  }
  allocator = &agent->client->allocator;
  cai_free_mem(allocator, agent->model);
  cai_free_mem(allocator, agent->instructions);
  cai_free_mem(allocator, agent->reasoning_effort);
  cai_free_mem(allocator, agent->reasoning_summary);
  cai_free_mem(allocator, agent->text_format_name);
  cai_free_mem(allocator, agent->text_format_description);
  cai_free_mem(allocator, agent->text_format_schema_json);
  cai_free_mem(allocator, agent->history_spool_dir);
  cai_tool_registry_destroy(agent->tools);
  cai_free_mem(allocator, agent);
}

int cai_agent_register_lonejson_tool(cai_agent *agent, const char *name,
                                     const char *description,
                                     const lonejson_map *map,
                                     const char *schema_json, int strict,
                                     cai_tool_lonejson_fn callback,
                                     void *context, cai_error *error) {
  if (agent == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  return cai_tool_registry_register_lonejson(agent->tools, name, description,
                                             map, schema_json, strict, callback,
                                             context, error);
}

int cai_agent_register_raw_tool(cai_agent *agent, const char *name,
                                const char *description,
                                const char *schema_json, int strict,
                                cai_tool_raw_fn callback, void *context,
                                cai_error *error) {
  if (agent == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  return cai_tool_registry_register_raw(agent->tools, name, description,
                                        schema_json, strict, callback, context,
                                        error);
}

int cai_agent_new_session(cai_agent *agent, cai_session **out,
                          cai_error *error) {
  cai_session *session;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session output pointer is required");
  }
  *out = NULL;
  if (agent == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  session =
      (cai_session *)cai_alloc(&agent->client->allocator, sizeof(*session));
  if (session == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate session");
  }
  session->agent = agent;
  session->previous_response_id = NULL;
  session->conversation_id = NULL;
  memset(&session->last_usage, 0, sizeof(session->last_usage));
  session->has_last_usage = 0;
  {
    lonejson_spool_options options;

    options = lonejson_default_spool_options();
    options.memory_limit = agent->history_memory_limit;
    options.max_bytes = 0U;
    options.temp_dir = agent->history_spool_dir;
    lonejson_spooled_init(&session->history, &options);
  }
  session->inputs = NULL;
  session->input_count = 0U;
  session->input_capacity = 0U;
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
  rc = cai_client_create_conversation(agent->client, &conversation, error);
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
  allocator = &session->agent->client->allocator;
  for (i = 0U; i < session->input_count; i++) {
    cai_free_mem(allocator, session->inputs[i].role);
    cai_free_mem(allocator, session->inputs[i].text);
    cai_free_mem(allocator, session->inputs[i].image_url);
    cai_free_mem(allocator, session->inputs[i].detail);
    cai_free_mem(allocator, session->inputs[i].call_id);
    cai_free_mem(allocator, session->inputs[i].output);
  }
  session->input_count = 0U;
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

static lonejson_read_result
cai_segment_reader_read(void *user, unsigned char *buffer, size_t capacity) {
  cai_segment_reader *reader;
  lonejson_read_result result;
  size_t available;
  size_t take;

  reader = (cai_segment_reader *)user;
  result = lonejson_default_read_result();
  if (capacity == 0U) {
    return result;
  }
  while (reader->index < 3U &&
         reader->offset >= reader->lengths[reader->index]) {
    reader->index++;
    reader->offset = 0U;
  }
  if (reader->index >= 3U) {
    result.eof = 1;
    return result;
  }
  available = reader->lengths[reader->index] - reader->offset;
  take = available < capacity ? available : capacity;
  if (take > 0U) {
    memcpy(buffer, reader->segments[reader->index] + reader->offset, take);
    reader->offset += take;
    result.bytes_read = take;
  }
  return result;
}

static void cai_history_init_spooled(cai_session *session,
                                     lonejson_spooled *spool) {
  lonejson_spool_options options;

  options = lonejson_default_spool_options();
  options.memory_limit = session->agent->history_memory_limit;
  options.max_bytes = 0U;
  options.temp_dir = session->agent->history_spool_dir;
  lonejson_spooled_init(spool, &options);
}

static int cai_history_capture_compact_array(cai_session *session,
                                             const char *json,
                                             lonejson_spooled *item,
                                             cai_error *error) {
  cai_history_json_doc doc;
  cai_history_sink_context sink_context;
  cai_segment_reader reader;
  lonejson_parse_options options;
  lonejson_error json_error;

  cai_history_init_spooled(session, item);
  if (json == NULL || json[0] == '\0') {
    return CAI_OK;
  }
  reader.segments[0] = "{\"value\":[";
  reader.segments[1] = json;
  reader.segments[2] = "]}";
  reader.lengths[0] = strlen(reader.segments[0]);
  reader.lengths[1] = strlen(reader.segments[1]);
  reader.lengths[2] = strlen(reader.segments[2]);
  reader.index = 0U;
  reader.offset = 0U;
  sink_context.spool = item;
  lonejson_error_init(&json_error);
  lonejson_json_value_init(&doc.value);
  options = lonejson_default_parse_options();
  options.clear_destination = 0;
  if (lonejson_json_value_set_parse_sink(&doc.value, cai_history_lonejson_sink,
                                         &sink_context,
                                         &json_error) != LONEJSON_STATUS_OK ||
      lonejson_parse_reader(&cai_history_json_map, &doc,
                            cai_segment_reader_read, &reader, &options,
                            &json_error) != LONEJSON_STATUS_OK) {
    lonejson_json_value_cleanup(&doc.value);
    lonejson_spooled_cleanup(item);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to validate JSON history item",
                                json_error.message);
  }
  lonejson_json_value_cleanup(&doc.value);
  return CAI_OK;
}

static int cai_history_append(cai_session *session, const char *json,
                              cai_error *error) {
  lonejson_spooled item;
  cai_history_sink_context sink_context;
  lonejson_error json_error;
  char header[32];
  size_t header_length;
  int rc;

  if (json == NULL || json[0] == '\0') {
    return CAI_OK;
  }
  rc = cai_history_capture_compact_array(session, json, &item, error);
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
  rc = cai_history_append_bytes(&session->history, header, header_length,
                                error);
  if (rc == CAI_OK) {
    sink_context.spool = &session->history;
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
    rc = cai_history_append_bytes(&session->history, "\n", 1U, error);
  }
  lonejson_spooled_cleanup(&item);
  return rc;
}

static void cai_history_reset(cai_session *session) {
  lonejson_spooled_reset(&session->history);
}

static void cai_history_cleanup(cai_session *session) {
  if (session != NULL) {
    lonejson_spooled_cleanup(&session->history);
  }
}

void cai_session_destroy(cai_session *session) {
  cai_allocator *allocator;

  if (session == NULL) {
    return;
  }
  allocator = &session->agent->client->allocator;
  cai_session_clear_inputs(session);
  cai_history_cleanup(session);
  cai_free_mem(allocator, session->inputs);
  cai_free_mem(allocator, session->previous_response_id);
  cai_free_mem(allocator, session->conversation_id);
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
  allocator = &session->agent->client->allocator;
  copy = NULL;
  if (conversation_id != NULL) {
    copy = cai_strdup(allocator, conversation_id);
    if (copy == NULL) {
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate conversation id");
    }
  }
  cai_free_mem(allocator, session->conversation_id);
  session->conversation_id = copy;
  if (copy != NULL) {
    cai_free_mem(allocator, session->previous_response_id);
    session->previous_response_id = NULL;
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
  return session != NULL ? session->conversation_id : NULL;
}

static int cai_session_grow_inputs(cai_session *session, cai_error *error) {
  size_t new_capacity;
  void *grown;

  if (session->input_count < session->input_capacity) {
    return CAI_OK;
  }
  new_capacity =
      session->input_capacity == 0U ? 2U : session->input_capacity * 2U;
  grown = cai_realloc_mem(&session->agent->client->allocator, session->inputs,
                          new_capacity * sizeof(session->inputs[0]));
  if (grown == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to grow session input list");
  }
  session->inputs = (cai_session_input *)grown;
  session->input_capacity = new_capacity;
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
  allocator = &session->agent->client->allocator;
  input = &session->inputs[session->input_count];
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
  session->input_count++;
  return CAI_OK;
}

int cai_session_add_text(cai_session *session, const char *role,
                         const char *text, cai_error *error) {
  if (text == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "text is required");
  }
  return cai_session_add_input(session, CAI_SESSION_INPUT_TEXT, role, text,
                               NULL, NULL, NULL, NULL, error);
}

int cai_session_add_image_url(cai_session *session, const char *role,
                              const char *url, const char *detail,
                              cai_error *error) {
  if (url == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "image URL is required");
  }
  return cai_session_add_input(session, CAI_SESSION_INPUT_IMAGE, role, NULL,
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
  for (i = 0U; rc == CAI_OK && i < session->input_count; i++) {
    if (session->inputs[i].kind == CAI_SESSION_INPUT_IMAGE) {
      rc = cai_response_create_params_add_image_url(
          params, session->inputs[i].role, session->inputs[i].image_url,
          session->inputs[i].detail, error);
    } else if (session->inputs[i].kind ==
               CAI_SESSION_INPUT_FUNCTION_CALL_OUTPUT) {
      rc = cai_response_create_params_add_function_call_output(
          params, session->inputs[i].call_id, session->inputs[i].output, error);
    } else {
      rc = cai_response_create_params_add_text(params, session->inputs[i].role,
                                               session->inputs[i].text, error);
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

static int cai_history_to_array_items(cai_session *session, char **out,
                                      cai_error *error) {
  cai_spooled_record_reader reader;
  cai_json_builder builder;
  unsigned long item_length;
  unsigned long pos;
  unsigned char ch;
  int need_comma;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "history output pointer is required");
  }
  *out = NULL;
  builder.data = NULL;
  builder.length = 0U;
  builder.capacity = 0U;
  builder.sink = NULL;
  builder.sink_user = NULL;
  builder.sink_error = NULL;
  reader.cursor = session->history;
  reader.offset = 0U;
  reader.length = 0U;
  reader.eof = 0;
  {
    lonejson_error json_error;

    lonejson_error_init(&json_error);
    if (lonejson_spooled_rewind(&reader.cursor, &json_error) !=
        LONEJSON_STATUS_OK) {
      return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                  "failed to rewind history spool",
                                  json_error.message);
    }
  }
  need_comma = 0;
  for (;;) {
    rc = cai_spooled_record_reader_next(&reader, &ch, error);
    if (rc < 0) {
      cai_free_mem(NULL, builder.data);
      return error != NULL ? error->code : CAI_ERR_TRANSPORT;
    }
    while (rc > 0 && (ch == '\n' || ch == '\r')) {
      rc = cai_spooled_record_reader_next(&reader, &ch, error);
      if (rc < 0) {
        cai_free_mem(NULL, builder.data);
        return error != NULL ? error->code : CAI_ERR_TRANSPORT;
      }
    }
    if (rc == 0) {
      break;
    }
    if (ch < '0' || ch > '9') {
      cai_free_mem(NULL, builder.data);
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "invalid history record length");
    }
    item_length = 0UL;
    do {
      if (item_length > (ULONG_MAX / 10UL)) {
        cai_free_mem(NULL, builder.data);
        return cai_set_error(error, CAI_ERR_PROTOCOL,
                             "history record length overflow");
      }
      item_length = item_length * 10UL + (unsigned long)(ch - '0');
      rc = cai_spooled_record_reader_next(&reader, &ch, error);
      if (rc <= 0) {
        cai_free_mem(NULL, builder.data);
        return rc < 0 ? (error != NULL ? error->code : CAI_ERR_TRANSPORT)
                      : cai_set_error(error, CAI_ERR_PROTOCOL,
                                      "truncated history record length");
      }
    } while (ch >= '0' && ch <= '9');
    if (ch != '\n') {
      cai_free_mem(NULL, builder.data);
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "invalid history record length");
    }
    if (item_length < 2UL) {
      cai_free_mem(NULL, builder.data);
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "invalid history record JSON array");
    }
    for (pos = 0UL; pos < item_length; pos++) {
      rc = cai_spooled_record_reader_next(&reader, &ch, error);
      if (rc <= 0) {
        cai_free_mem(NULL, builder.data);
        return rc < 0 ? (error != NULL ? error->code : CAI_ERR_TRANSPORT)
                      : cai_set_error(error, CAI_ERR_PROTOCOL,
                                      "truncated history record JSON array");
      }
      if ((pos == 0UL && ch != '[') ||
          (pos == item_length - 1UL && ch != ']')) {
        cai_free_mem(NULL, builder.data);
        return cai_set_error(error, CAI_ERR_PROTOCOL,
                             "invalid history record JSON array");
      }
      if (pos > 0UL && pos < item_length - 1UL) {
        if (!need_comma) {
          need_comma = 1;
        } else if (pos == 1UL) {
          rc = cai_json_builder_append(&builder, ",", 1U, error);
          if (rc != CAI_OK) {
            cai_free_mem(NULL, builder.data);
            return rc;
          }
        }
        rc = cai_json_builder_append(&builder, (const char *)&ch, 1U, error);
        if (rc != CAI_OK) {
          cai_free_mem(NULL, builder.data);
          return rc;
        }
      }
    }
  }
  if (builder.data == NULL) {
    builder.data = cai_strdup(NULL, "");
    if (builder.data == NULL) {
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate history items");
    }
  }
  *out = builder.data;
  return CAI_OK;
}

static int
cai_session_prepare_history_params(cai_session *session,
                                   cai_response_create_params *params,
                                   char **out_pending_items, cai_error *error) {
  char *history_items;
  char *pending_items;
  cai_json_builder builder;
  int rc;

  if (out_pending_items != NULL) {
    *out_pending_items = NULL;
  }
  if (session->agent->auto_compact_token_limit <= 0LL) {
    return cai_session_add_pending_inputs(session, params, error);
  }
  history_items = NULL;
  pending_items = NULL;
  rc = cai_history_to_array_items(session, &history_items, error);
  if (rc == CAI_OK) {
    rc = cai_session_add_pending_inputs(session, params, error);
  }
  if (rc == CAI_OK) {
    rc = cai_response_params_input_items_json(params, &pending_items, error);
  }
  builder.data = NULL;
  builder.length = 0U;
  builder.capacity = 0U;
  builder.sink = NULL;
  builder.sink_user = NULL;
  builder.sink_error = NULL;
  if (rc == CAI_OK) {
    rc = cai_json_builder_lit(&builder, history_items, error);
  }
  if (rc == CAI_OK && history_items[0] != '\0' && pending_items[0] != '\0') {
    rc = cai_json_builder_lit(&builder, ",", error);
  }
  if (rc == CAI_OK) {
    rc = cai_json_builder_lit(&builder, pending_items, error);
  }
  if (rc == CAI_OK) {
    rc = cai_response_create_params_set_raw_input_json(params, builder.data,
                                                       error);
  }
  if (rc == CAI_OK) {
    cai_response_create_params_clear_input(params);
  }
  if (out_pending_items != NULL && rc == CAI_OK) {
    *out_pending_items = pending_items;
    pending_items = NULL;
  }
  cai_free_mem(&session->agent->client->allocator, history_items);
  cai_free_mem(NULL, pending_items);
  cai_free_mem(NULL, builder.data);
  return rc;
}

static int cai_session_remember_response(cai_session *session,
                                         const cai_response *response,
                                         cai_error *error) {
  int rc;

  rc = cai_session_remember_response_id(session, cai_response_id(response),
                                        error);
  if (rc == CAI_OK && response != NULL) {
    session->last_usage.input_tokens = cai_response_input_tokens(response);
    session->last_usage.input_cached_tokens =
        cai_response_input_cached_tokens(response);
    session->last_usage.output_tokens = cai_response_output_tokens(response);
    session->last_usage.output_reasoning_tokens =
        cai_response_output_reasoning_tokens(response);
    session->last_usage.total_tokens = cai_response_total_tokens(response);
    session->has_last_usage = 1;
  }
  return rc;
}

static int cai_session_compact(cai_session *session, cai_error *error) {
  cai_response_create_params *params;
  cai_response *response;
  lonejson_spooled request_json;
  size_t request_json_len;
  int has_request_json;
  char *history_items;
  char *body;
  char *request_id;
  char *output_items;
  long http_status;
  int rc;

  params = NULL;
  response = NULL;
  memset(&request_json, 0, sizeof(request_json));
  request_json_len = 0U;
  has_request_json = 0;
  history_items = NULL;
  body = NULL;
  request_id = NULL;
  output_items = NULL;
  rc = cai_history_to_array_items(session, &history_items, error);
  if (rc == CAI_OK && history_items[0] == '\0') {
    rc = CAI_OK;
    goto done;
  }
  if (rc == CAI_OK) {
    rc = cai_session_init_response_params(session, &params, error);
  }
  if (rc == CAI_OK) {
    rc = cai_response_create_params_set_raw_input_json(params, history_items,
                                                       error);
  }
  if (rc == CAI_OK) {
    cai_response_create_params_clear_input(params);
    cai_free_mem(&params->allocator, params->previous_response_id);
    params->previous_response_id = NULL;
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
        session->agent->client, "POST", "responses/compact", &request_json,
        request_json_len, &body, &http_status, &request_id, error);
  }
  if (rc == CAI_OK && (http_status < 200L || http_status >= 300L)) {
    rc = cai_set_openai_error(error, http_status, body, request_id);
  }
  if (rc == CAI_OK) {
    rc = cai_response_parse_json(body != NULL ? body : "", &response, error);
  }
  if (rc == CAI_OK) {
    rc = cai_response_output_items_json(response, &output_items, error);
  }
  if (rc == CAI_OK) {
    cai_history_reset(session);
    rc = cai_history_append(session, output_items, error);
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
  cai_free_mem(&session->agent->client->allocator, history_items);
  cai_free_mem(NULL, body);
  cai_free_mem(NULL, request_id);
  cai_free_mem(NULL, output_items);
  return rc;
}

static int cai_session_maybe_compact(cai_session *session, cai_error *error) {
  long long limit;

  limit = session->agent->auto_compact_token_limit;
  if (limit <= 0LL) {
    return CAI_OK;
  }
  if (!session->has_last_usage || session->last_usage.total_tokens < limit) {
    return CAI_OK;
  }
  return cai_session_compact(session, error);
}

static int cai_session_after_response(cai_session *session,
                                      const char *pending_items_json,
                                      const cai_response *response,
                                      cai_error *error) {
  char *output_items;
  int rc;

  rc = cai_session_remember_response(session, response, error);
  if (rc != CAI_OK || session->agent->auto_compact_token_limit <= 0LL) {
    return rc;
  }
  output_items = NULL;
  rc = cai_response_output_items_json(response, &output_items, error);
  if (rc == CAI_OK) {
    rc = cai_history_append(session, pending_items_json, error);
  }
  if (rc == CAI_OK) {
    rc = cai_history_append(session, output_items, error);
  }
  cai_free_mem(NULL, output_items);
  if (rc == CAI_OK) {
    rc = cai_session_maybe_compact(session, error);
  }
  return rc;
}

static int cai_session_after_stream(cai_session *session,
                                    const char *pending_items_json,
                                    const char *response_id,
                                    const cai_token_usage *usage,
                                    cai_error *error) {
  cai_response *response;
  char *output_items;
  int rc;

  rc = cai_session_remember_stream(session, response_id, usage, error);
  if (rc != CAI_OK || session->agent->auto_compact_token_limit <= 0LL) {
    return rc;
  }
  response = NULL;
  output_items = NULL;
  rc = cai_client_retrieve_response(session->agent->client, response_id,
                                    &response, error);
  if (rc == CAI_OK) {
    rc = cai_response_output_items_json(response, &output_items, error);
  }
  if (rc == CAI_OK) {
    rc = cai_history_append(session, pending_items_json, error);
  }
  if (rc == CAI_OK) {
    rc = cai_history_append(session, output_items, error);
  }
  cai_free_mem(NULL, output_items);
  cai_response_destroy(response);
  if (rc == CAI_OK) {
    rc = cai_session_maybe_compact(session, error);
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
      cai_strdup(&session->agent->client->allocator, response_id);
  if (next_response_id == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to remember previous response id");
  }
  cai_free_mem(&session->agent->client->allocator,
               session->previous_response_id);
  session->previous_response_id = next_response_id;
  return CAI_OK;
}

static int cai_session_remember_stream(cai_session *session,
                                       const char *response_id,
                                       const cai_token_usage *usage,
                                       cai_error *error) {
  int rc;

  rc = cai_session_remember_response_id(session, response_id, error);
  if (rc == CAI_OK && usage != NULL) {
    session->last_usage = *usage;
    session->has_last_usage = 1;
  }
  return rc;
}

static int cai_session_stream_complete(void *context, const char *response_id,
                                       const cai_token_usage *usage) {
  return cai_session_remember_stream((cai_session *)context, response_id, usage,
                                     NULL);
}

static int cai_session_history_stream_complete(void *context,
                                               const char *response_id,
                                               const cai_token_usage *usage) {
  cai_session_stream_context *stream_context;
  int rc;

  stream_context = (cai_session_stream_context *)context;
  if (stream_context == NULL) {
    return CAI_ERR_INVALID;
  }
  rc = cai_session_after_stream(stream_context->session,
                                stream_context->pending_items_json, response_id,
                                usage, NULL);
  cai_free_mem(NULL, stream_context->pending_items_json);
  cai_free_mem(NULL, stream_context);
  return rc;
}

static int cai_session_create_response_from_params(
    cai_session *session, cai_response_create_params *params,
    const char *pending_items_json, cai_response **out, cai_error *error) {
  cai_response *response;
  int rc;

  response = NULL;
  rc = cai_client_create_response(session->agent->client, params, &response,
                                  error);
  if (rc != CAI_OK) {
    cai_response_destroy(response);
    return rc;
  }
  rc = cai_session_after_response(session, pending_items_json, response, error);
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
  char *pending_items_json;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response output pointer is required");
  }
  *out = NULL;
  if (session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "session is required");
  }
  if (session->input_count == 0U) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session has no pending input");
  }
  params = NULL;
  pending_items_json = NULL;
  rc = cai_session_init_response_params(session, &params, error);
  if (rc == CAI_OK) {
    rc = cai_session_prepare_history_params(session, params,
                                            &pending_items_json, error);
  }
  if (rc == CAI_OK) {
    rc = cai_session_create_response_from_params(
        session, params, pending_items_json, out, error);
  }
  cai_response_create_params_destroy(params);
  cai_free_mem(NULL, pending_items_json);
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
    rc = cai_response_create_params_set_model(params, session->agent->model,
                                              error);
  }
  if (rc == CAI_OK && session->agent->instructions != NULL) {
    rc = cai_response_create_params_set_instructions(
        params, session->agent->instructions, error);
  }
  if (rc == CAI_OK && session->agent->max_output_tokens > 0) {
    rc = cai_response_create_params_set_max_output_tokens(
        params, session->agent->max_output_tokens, error);
  }
  if (rc == CAI_OK && (session->agent->reasoning_effort != NULL ||
                       session->agent->reasoning_summary != NULL)) {
    rc = cai_response_create_params_set_reasoning(
        params, session->agent->reasoning_effort,
        session->agent->reasoning_summary, error);
  }
  if (rc == CAI_OK && session->agent->text_format_schema_json != NULL) {
    rc = cai_response_create_params_set_text_format_json_schema(
        params, session->agent->text_format_name,
        session->agent->text_format_description,
        session->agent->text_format_schema_json,
        session->agent->text_format_strict, error);
  }
  if (rc == CAI_OK && session->agent->parallel_tool_calls >= 0) {
    rc = cai_response_create_params_set_parallel_tool_calls(
        params, session->agent->parallel_tool_calls, error);
  }
  if (rc == CAI_OK && session->conversation_id != NULL) {
    rc = cai_response_create_params_set_conversation_id(
        params, session->conversation_id, error);
  }
  if (rc == CAI_OK && session->previous_response_id != NULL &&
      session->agent->auto_compact_token_limit <= 0LL) {
    rc = cai_response_create_params_set_previous_response_id(
        params, session->previous_response_id, error);
  }
  if (rc == CAI_OK) {
    rc = cai_tool_registry_add_to_response_params(session->agent->tools, params,
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
          session->agent->tools, cai_response_tool_call_name(response, i),
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
    rc = cai_client_create_response(session->agent->client, params, out, error);
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

int cai_session_stream_text(cai_session *session, cai_sink *sink,
                            cai_error *error) {
  cai_response_create_params *params;
  char *response_id;
  char *pending_items_json;
  cai_token_usage usage;
  int rc;

  if (session == NULL || sink == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session and sink are required");
  }
  params = NULL;
  response_id = NULL;
  pending_items_json = NULL;
  memset(&usage, 0, sizeof(usage));
  rc = cai_session_init_response_params(session, &params, error);
  if (rc == CAI_OK) {
    rc = cai_session_prepare_history_params(session, params,
                                            &pending_items_json, error);
  }
  if (rc == CAI_OK) {
    rc = cai_client_stream_response_text_with_id(
        session->agent->client, params, sink, &response_id, &usage, error);
  }
  if (rc == CAI_OK) {
    rc = cai_session_after_stream(session, pending_items_json, response_id,
                                  &usage, error);
  }
  cai_response_create_params_destroy(params);
  cai_free_mem(NULL, response_id);
  cai_free_mem(NULL, pending_items_json);
  if (rc == CAI_OK) {
    cai_session_clear_inputs(session);
  }
  return rc;
}

int cai_session_open_text_source(cai_session *session, cai_source **out,
                                 cai_error *error) {
  cai_response_create_params *params;
  cai_session_stream_context *stream_context;
  char *pending_items_json;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "source output pointer is required");
  }
  *out = NULL;
  if (session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "session is required");
  }
  params = NULL;
  stream_context = NULL;
  pending_items_json = NULL;
  rc = cai_session_init_response_params(session, &params, error);
  if (rc == CAI_OK) {
    rc = cai_session_prepare_history_params(session, params,
                                            &pending_items_json, error);
  }
  if (rc == CAI_OK && session->agent->auto_compact_token_limit > 0LL) {
    stream_context =
        (cai_session_stream_context *)cai_alloc(NULL, sizeof(*stream_context));
    if (stream_context == NULL) {
      rc = cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate stream context");
    } else {
      stream_context->session = session;
      stream_context->pending_items_json = pending_items_json;
      pending_items_json = NULL;
    }
  }
  if (rc == CAI_OK) {
    if (session->agent->auto_compact_token_limit > 0LL) {
      rc = cai_client_open_response_text_source_with_complete(
          session->agent->client, params, cai_session_history_stream_complete,
          stream_context, out, error);
      if (rc == CAI_OK) {
        stream_context = NULL;
      }
    } else {
      rc = cai_client_open_response_text_source_with_complete(
          session->agent->client, params, cai_session_stream_complete, session,
          out, error);
    }
  }
  cai_response_create_params_destroy(params);
  if (stream_context != NULL) {
    cai_free_mem(NULL, stream_context->pending_items_json);
    cai_free_mem(NULL, stream_context);
  }
  cai_free_mem(NULL, pending_items_json);
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
  rc = cai_session_add_text(session, "user", text, error);
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
  if (!session->has_last_usage) {
    memset(out, 0, sizeof(*out));
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session has no completed response usage");
  }
  *out = session->last_usage;
  return CAI_OK;
}

long long cai_session_context_window_tokens(const cai_session *session) {
  if (session == NULL || session->agent == NULL) {
    return 0LL;
  }
  return cai_model_context_window_tokens(session->agent->model);
}

long long cai_session_auto_compact_token_limit(const cai_session *session) {
  if (session == NULL || session->agent == NULL) {
    return 0LL;
  }
  return session->agent->auto_compact_token_limit;
}

int cai_session_context_percent(const cai_session *session, double *out,
                                cai_error *error) {
  long long window;

  if (session == NULL || out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session and percent output are required");
  }
  if (!session->has_last_usage) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session has no completed response usage");
  }
  window = cai_session_context_window_tokens(session);
  if (window <= 0LL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session model has unknown context window");
  }
  *out = ((double)session->last_usage.total_tokens * 100.0) / (double)window;
  return CAI_OK;
}

int cai_session_history_spilled(const cai_session *session) {
  if (session == NULL) {
    return 0;
  }
  return lonejson_spooled_spilled(&session->history);
}
