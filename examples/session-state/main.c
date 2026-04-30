#include <cai/cai.h>

#include <stdio.h>
#include <stdlib.h>

static const char *example_model(void) {
  const char *model;

  model = getenv("CAI_EXAMPLE_MODEL");
  if (model == NULL || model[0] == '\0') {
    return CAI_MODEL_GPT_5_NANO;
  }
  return model;
}

static int print_error(const char *operation, int rc, const cai_error *error) {
  fprintf(stderr, "%s failed: %s\n", operation,
          error->message != NULL ? error->message : cai_status_string(rc));
  if (error->detail != NULL) {
    fprintf(stderr, "detail: %s\n", error->detail);
  }
  return 1;
}

static void print_response(const char *label, const cai_response *response) {
  const char *text;

  text = cai_response_output_text(response);
  printf("%s: %s\n", label, text != NULL ? text : "");
}

int main(int argc, char **argv) {
  const char *path;
  cai_agent_config agent_config;
  cai_client_config client_config;
  cai_client *client;
  cai_agent *agent;
  cai_session *session;
  cai_session *restored;
  cai_response *response;
  cai_source *state_source;
  cai_sink *state_sink;
  cai_error error;
  FILE *fp;
  int exit_code;
  int rc;

  path = argc > 1 ? argv[1] : "cai-session-state.json";
  cai_error_init(&error);
  cai_client_config_init(&client_config);
  cai_agent_config_init(&agent_config);
  agent_config.model = example_model();
  agent_config.developer_instructions =
      "Answer briefly and preserve the user's named facts across turns.";
  agent_config.enable_local_history = 1;
  agent_config.prompt_cache_key = "cai:example:session-state:v1";
  client = NULL;
  agent = NULL;
  session = NULL;
  restored = NULL;
  response = NULL;
  state_source = NULL;
  state_sink = NULL;
  fp = NULL;
  exit_code = 1;

  rc = cai_client_open(&client_config, &client, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_client_open", rc, &error);
    goto done;
  }
  rc = client->new_agent(client, &agent_config, &agent, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_client_new_agent", rc, &error);
    goto done;
  }
  rc = agent->new_session(agent, &session, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_agent_new_session", rc, &error);
    goto done;
  }
  rc = session->send_text(
      session,
      "Remember this exact project codename: amber-river. Reply with ok.",
      &response, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("first cai_session_send_text", rc, &error);
    goto done;
  }
  print_response("first", response);
  cai_response_destroy(response);
  response = NULL;

  rc = session->export_state_source(session, &state_source, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_session_export_state_source", rc, &error);
    goto done;
  }
  fp = fopen(path, "wb");
  if (fp == NULL) {
    fprintf(stderr, "fopen for write failed: %s\n", path);
    goto done;
  }
  rc = cai_sink_file(fp, 1, &state_sink, &error);
  fp = NULL;
  if (rc != CAI_OK) {
    exit_code = print_error("cai_sink_file", rc, &error);
    goto done;
  }
  rc = cai_source_copy_to_sink(state_source, state_sink, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_source_copy_to_sink", rc, &error);
    goto done;
  }
  cai_sink_close(state_sink);
  state_sink = NULL;
  cai_source_close(state_source);
  state_source = NULL;
  printf("saved: %s\n", path);

  rc = agent->new_session(agent, &restored, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("restored cai_agent_new_session", rc, &error);
    goto done;
  }
  fp = fopen(path, "rb");
  if (fp == NULL) {
    fprintf(stderr, "fopen for read failed: %s\n", path);
    goto done;
  }
  rc = cai_source_file(fp, 1, &state_source, &error);
  fp = NULL;
  if (rc != CAI_OK) {
    exit_code = print_error("cai_source_file", rc, &error);
    goto done;
  }
  rc = restored->import_state_source(restored, state_source, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_session_import_state_source", rc, &error);
    goto done;
  }
  rc = restored->send_text(restored,
                           "What project codename did I ask you to remember?",
                           &response, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("second cai_session_send_text", rc, &error);
    goto done;
  }
  print_response("restored", response);
  exit_code = 0;

done:
  if (fp != NULL) {
    fclose(fp);
  }
  cai_sink_close(state_sink);
  cai_source_close(state_source);
  cai_response_destroy(response);
  if (restored != NULL) {
    restored->close(restored);
  }
  if (session != NULL) {
    session->close(session);
  }
  if (agent != NULL) {
    agent->close(agent);
  }
  if (client != NULL) {
    client->close(client);
  }
  cai_error_cleanup(&error);
  return exit_code;
}
