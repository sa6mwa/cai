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
  if (agent->model == NULL ||
      (config->instructions != NULL && agent->instructions == NULL)) {
    cai_agent_destroy(agent);
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate agent");
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
  cai_free_mem(allocator, agent);
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
  *out = session;
  return CAI_OK;
}

void cai_session_destroy(cai_session *session) {
  cai_allocator *allocator;

  if (session == NULL) {
    return;
  }
  allocator = &session->agent->client->allocator;
  cai_free_mem(allocator, session->previous_response_id);
  cai_free_mem(allocator, session);
}

int cai_session_send_text(cai_session *session, const char *text,
                          cai_response **out, cai_error *error) {
  cai_response_create_params *params;
  cai_response *response;
  char *next_response_id;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response output pointer is required");
  }
  *out = NULL;
  if (session == NULL || text == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session and text are required");
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
    rc = cai_response_create_params_add_text(params, "user", text, error);
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
  *out = response;
  return CAI_OK;
}
