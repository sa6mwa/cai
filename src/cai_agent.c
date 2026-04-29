#include "cai_internal.h"

void cai_agent_config_init(cai_agent_config *config) {
  if (config == NULL) {
    return;
  }
  config->model = NULL;
  config->instructions = NULL;
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
  agent->tools = NULL;
  if (agent->model == NULL ||
      (config->instructions != NULL && agent->instructions == NULL)) {
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
  session->inputs = NULL;
  session->input_count = 0U;
  session->input_capacity = 0U;
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
  cai_free_mem(allocator, session);
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

static int cai_session_add_input(cai_session *session, const char *role,
                                 const char *text, const char *image_url,
                                 const char *detail, int is_image,
                                 cai_error *error) {
  cai_session_input *input;
  cai_allocator *allocator;
  int rc;

  if (session == NULL || role == NULL || role[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session and role are required");
  }
  rc = cai_session_grow_inputs(session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  allocator = &session->agent->client->allocator;
  input = &session->inputs[session->input_count];
  input->role = cai_strdup(allocator, role);
  input->text = cai_strdup(allocator, text);
  input->image_url = cai_strdup(allocator, image_url);
  input->detail = cai_strdup(allocator, detail);
  input->is_image = is_image;
  if (input->role == NULL || (text != NULL && input->text == NULL) ||
      (image_url != NULL && input->image_url == NULL) ||
      (detail != NULL && input->detail == NULL)) {
    cai_free_mem(allocator, input->role);
    cai_free_mem(allocator, input->text);
    cai_free_mem(allocator, input->image_url);
    cai_free_mem(allocator, input->detail);
    input->role = NULL;
    input->text = NULL;
    input->image_url = NULL;
    input->detail = NULL;
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
  return cai_session_add_input(session, role, text, NULL, NULL, 0, error);
}

int cai_session_add_image_url(cai_session *session, const char *role,
                              const char *url, const char *detail,
                              cai_error *error) {
  if (url == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "image URL is required");
  }
  return cai_session_add_input(session, role, NULL, url, detail, 1, error);
}

int cai_session_run(cai_session *session, cai_response **out,
                    cai_error *error) {
  cai_response_create_params *params;
  cai_response *response;
  char *next_response_id;
  size_t i;
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
  response = NULL;
  rc = cai_response_create_params_new(&params, error);
  if (rc == CAI_OK) {
    rc = cai_response_create_params_set_model(params, session->agent->model,
                                              error);
  }
  if (rc == CAI_OK && session->agent->instructions != NULL) {
    rc = cai_response_create_params_set_instructions(
        params, session->agent->instructions, error);
  }
  if (rc == CAI_OK && session->previous_response_id != NULL) {
    rc = cai_response_create_params_set_previous_response_id(
        params, session->previous_response_id, error);
  }
  if (rc == CAI_OK) {
    rc = cai_tool_registry_add_to_response_params(session->agent->tools, params,
                                                  error);
  }
  for (i = 0U; rc == CAI_OK && i < session->input_count; i++) {
    if (session->inputs[i].is_image) {
      rc = cai_response_create_params_add_image_url(
          params, session->inputs[i].role, session->inputs[i].image_url,
          session->inputs[i].detail, error);
    } else {
      rc = cai_response_create_params_add_text(params, session->inputs[i].role,
                                               session->inputs[i].text, error);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_client_create_response(session->agent->client, params, &response,
                                    error);
  }
  cai_response_create_params_destroy(params);
  if (rc != CAI_OK) {
    cai_response_destroy(response);
    return rc;
  }
  next_response_id =
      cai_strdup(&session->agent->client->allocator, cai_response_id(response));
  if (next_response_id == NULL) {
    cai_response_destroy(response);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to remember previous response id");
  }
  cai_free_mem(&session->agent->client->allocator,
               session->previous_response_id);
  session->previous_response_id = next_response_id;
  cai_session_clear_inputs(session);
  *out = response;
  return CAI_OK;
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
