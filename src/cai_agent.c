#include "cai_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct cai_tool_output_capture {
  char *data;
  char *spool_path;
  FILE *spool;
  const char *spool_dir;
  size_t length;
  size_t capacity;
  size_t limit;
} cai_tool_output_capture;

enum {
  CAI_SESSION_INPUT_TEXT = 0,
  CAI_SESSION_INPUT_IMAGE = 1,
  CAI_SESSION_INPUT_FUNCTION_CALL_OUTPUT = 2
};

static int cai_capture_open_spool(cai_tool_output_capture *capture,
                                  cai_error *error);
static int cai_session_init_response_params(cai_session *session,
                                            cai_response_create_params **out,
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
  agent->tools = NULL;
  if (agent->model == NULL ||
      (config->instructions != NULL && agent->instructions == NULL) ||
      (config->reasoning_effort != NULL && agent->reasoning_effort == NULL) ||
      (config->reasoning_summary != NULL && agent->reasoning_summary == NULL) ||
      (config->text_format_name != NULL && agent->text_format_name == NULL) ||
      (config->text_format_description != NULL &&
       agent->text_format_description == NULL) ||
      (config->text_format_schema_json != NULL &&
       agent->text_format_schema_json == NULL)) {
    cai_agent_destroy(agent);
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate agent");
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

void cai_session_destroy(cai_session *session) {
  cai_allocator *allocator;

  if (session == NULL) {
    return;
  }
  allocator = &session->agent->client->allocator;
  cai_session_clear_inputs(session);
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

static int cai_session_remember_response(cai_session *session,
                                         const cai_response *response,
                                         cai_error *error) {
  char *next_response_id;

  next_response_id =
      cai_strdup(&session->agent->client->allocator, cai_response_id(response));
  if (next_response_id == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to remember previous response id");
  }
  cai_free_mem(&session->agent->client->allocator,
               session->previous_response_id);
  session->previous_response_id = next_response_id;
  return CAI_OK;
}

static int
cai_session_create_response_from_params(cai_session *session,
                                        cai_response_create_params *params,
                                        cai_response **out, cai_error *error) {
  cai_response *response;
  int rc;

  response = NULL;
  rc = cai_client_create_response(session->agent->client, params, &response,
                                  error);
  if (rc != CAI_OK) {
    cai_response_destroy(response);
    return rc;
  }
  rc = cai_session_remember_response(session, response, error);
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
  rc = cai_session_init_response_params(session, &params, error);
  if (rc == CAI_OK) {
    rc = cai_session_add_pending_inputs(session, params, error);
  }
  if (rc == CAI_OK) {
    rc = cai_session_create_response_from_params(session, params, out, error);
  }
  cai_response_create_params_destroy(params);
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
  size_t needed;
  size_t new_capacity;
  char *grown;

  capture = (cai_tool_output_capture *)context;
  needed = capture->length + count + 1U;
  if (capture->limit > 0U && needed - 1U > capture->limit) {
    if (cai_capture_open_spool(capture, error) != CAI_OK) {
      return error != NULL ? error->code : CAI_ERR_TRANSPORT;
    }
  }
  if (capture->spool != NULL) {
    if (count > 0U && fwrite(bytes, 1U, count, capture->spool) != count) {
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "failed to write tool output spool file");
    }
    capture->length += count;
    return CAI_OK;
  }
  if (needed > capture->capacity) {
    new_capacity = capture->capacity == 0U ? 256U : capture->capacity;
    while (new_capacity < needed) {
      new_capacity *= 2U;
    }
    grown = (char *)cai_realloc_mem(NULL, capture->data, new_capacity);
    if (grown == NULL) {
      return cai_set_error(error, CAI_ERR_NOMEM, "failed to grow tool output");
    }
    capture->data = grown;
    capture->capacity = new_capacity;
  }
  if (count > 0U) {
    memcpy(capture->data + capture->length, bytes, count);
    capture->length += count;
  }
  capture->data[capture->length] = '\0';
  return CAI_OK;
}

static int cai_capture_open_spool(cai_tool_output_capture *capture,
                                  cai_error *error) {
  static const char suffix[] = "/cai-tool-output-XXXXXX";
  size_t dir_len;
  size_t suffix_len;
  int fd;

  if (capture->spool != NULL) {
    return CAI_OK;
  }
  if (capture->spool_dir == NULL || capture->spool_dir[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "tool output exceeded memory limit");
  }
  dir_len = strlen(capture->spool_dir);
  suffix_len = sizeof(suffix) - 1U;
  capture->spool_path = (char *)cai_alloc(NULL, dir_len + suffix_len + 1U);
  if (capture->spool_path == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate tool output spool path");
  }
  memcpy(capture->spool_path, capture->spool_dir, dir_len);
  memcpy(capture->spool_path + dir_len, suffix, suffix_len);
  capture->spool_path[dir_len + suffix_len] = '\0';
  fd = mkstemp(capture->spool_path);
  if (fd < 0) {
    cai_free_mem(NULL, capture->spool_path);
    capture->spool_path = NULL;
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to create tool output spool file");
  }
  capture->spool = fdopen(fd, "w+b");
  if (capture->spool == NULL) {
    close(fd);
    unlink(capture->spool_path);
    cai_free_mem(NULL, capture->spool_path);
    capture->spool_path = NULL;
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to open tool output spool file");
  }
  if (capture->length > 0U && fwrite(capture->data, 1U, capture->length,
                                     capture->spool) != capture->length) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to write tool output spool file");
  }
  cai_free_mem(NULL, capture->data);
  capture->data = NULL;
  capture->capacity = 0U;
  return CAI_OK;
}

static void cai_capture_cleanup(cai_tool_output_capture *capture) {
  if (capture == NULL) {
    return;
  }
  if (capture->spool != NULL) {
    fclose(capture->spool);
    capture->spool = NULL;
  }
  if (capture->spool_path != NULL) {
    unlink(capture->spool_path);
    cai_free_mem(NULL, capture->spool_path);
    capture->spool_path = NULL;
  }
  cai_free_mem(NULL, capture->data);
  capture->data = NULL;
}

static int cai_capture_materialize(cai_tool_output_capture *capture, char **out,
                                   cai_error *error) {
  char *data;
  size_t nread;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "tool output pointer is required");
  }
  *out = NULL;
  if (capture->spool == NULL) {
    data = cai_strdup(NULL, capture->data != NULL ? capture->data : "");
    if (data == NULL) {
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate tool output");
    }
    *out = data;
    return CAI_OK;
  }
  if (fflush(capture->spool) != 0 || fseek(capture->spool, 0L, SEEK_SET) != 0) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to rewind tool output spool file");
  }
  data = (char *)cai_alloc(NULL, capture->length + 1U);
  if (data == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate spooled tool output");
  }
  nread = fread(data, 1U, capture->length, capture->spool);
  if (nread != capture->length) {
    cai_free_mem(NULL, data);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to read tool output spool file");
  }
  data[capture->length] = '\0';
  *out = data;
  return CAI_OK;
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
  if (rc == CAI_OK && session->previous_response_id != NULL) {
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
  char *tool_output;
  size_t i;
  int rc;

  params = NULL;
  tool_output = NULL;
  rc = cai_session_init_response_params(session, &params, error);
  for (i = 0U; rc == CAI_OK && i < cai_response_tool_call_count(response);
       i++) {
    capture.data = NULL;
    capture.spool_path = NULL;
    capture.spool = NULL;
    capture.spool_dir = options->tool_spool_dir;
    capture.length = 0U;
    capture.capacity = 0U;
    capture.limit = options->tool_output_memory_limit;
    tool_output = NULL;
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
      rc = cai_capture_materialize(&capture, &tool_output, error);
    }
    if (rc == CAI_OK) {
      rc = cai_response_create_params_add_function_call_output(
          params, cai_response_tool_call_id(response, i), tool_output, error);
    }
    cai_free_mem(NULL, tool_output);
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
