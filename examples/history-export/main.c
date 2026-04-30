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

int main(void) {
  cai_agent_config agent_config;
  cai_client_config client_config;
  cai_client *client;
  cai_agent *agent;
  cai_session *session;
  cai_response *response;
  cai_source *history;
  cai_sink *stdout_sink;
  cai_error error;
  int exit_code;
  int rc;

  cai_error_init(&error);
  cai_client_config_init(&client_config);
  cai_agent_config_init(&agent_config);
  agent_config.model = example_model();
  agent_config.developer_instructions =
      "Answer with exactly one short sentence.";
  agent_config.enable_local_history = 1;
  agent_config.prompt_cache_key = "cai:example:history-export:v1";
  client = NULL;
  agent = NULL;
  session = NULL;
  response = NULL;
  history = NULL;
  stdout_sink = NULL;
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
  rc = session->send_text(session, "Say hello from exported history.",
                          &response, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_session_send_text", rc, &error);
    goto done;
  }
  rc = session->export_history_source(session, &history, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_session_export_history_source", rc, &error);
    goto done;
  }
  rc = cai_sink_stdout(&stdout_sink, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_sink_stdout", rc, &error);
    goto done;
  }
  rc = cai_source_copy_to_sink(history, stdout_sink, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_source_copy_to_sink", rc, &error);
    goto done;
  }
  fputc('\n', stdout);
  exit_code = 0;

done:
  cai_sink_close(stdout_sink);
  cai_source_close(history);
  cai_response_destroy(response);
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
