#include <cai/cai.h>

#include "../common.h"

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

int main(int argc, char **argv) {
  cai_agent_config agent_config;
  cai_client_config client_config;
  cai_input_item_list *items;
  cai_conversation *conversation;
  cai_output *output;
  cai_session *session;
  cai_client *client;
  cai_agent *agent;
  cai_error error;
  char *dotenv_api_key;
  int exit_code;
  int rc;

  cai_error_init(&error);
  cai_client_config_init(&client_config);
  cai_agent_config_init(&agent_config);
  agent_config.model = example_model();
  agent_config.developer_instructions =
      "Answer briefly. You are running inside a C SDK conversation example.";
  client = NULL;
  agent = NULL;
  session = NULL;
  output = NULL;
  conversation = NULL;
  items = NULL;
  dotenv_api_key = NULL;
  exit_code = 1;

  rc = cai_example_load_dotenv_api_key(&client_config, &dotenv_api_key, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_load_dotenv_api_key", rc, &error);
    goto done;
  }
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
  if (argc > 1 && argv[1] != NULL && argv[1][0] != '\0') {
    rc = cai_conversation_from_id(argv[1], &conversation, &error);
  } else {
    rc = client->create_conversation(client, &conversation, &error);
  }
  if (rc != CAI_OK) {
    exit_code = print_error("conversation handle setup", rc, &error);
    goto done;
  }
  rc = agent->new_session_for_conversation(agent, conversation, &session,
                                           &error);
  if (rc != CAI_OK) {
    exit_code =
        print_error("cai_agent_new_session_for_conversation", rc, &error);
    goto done;
  }
  rc = session->add_user_text(session,
                              "Say one short sentence about reusable "
                              "conversation handles.",
                              &error);
  if (rc == CAI_OK) {
    rc = session->run_output(session, &output, &error);
  }
  if (rc != CAI_OK) {
    exit_code = print_error("cai_session_run_output", rc, &error);
    goto done;
  }
  printf("conversation: %s\n", cai_conversation_id(conversation));
  printf("response: %s\n",
         output->text(output) != NULL ? output->text(output) : "");

  rc = client->list_conversation_items_handle(client, conversation, NULL,
                                              &items, &error);
  if (rc != CAI_OK) {
    exit_code =
        print_error("cai_client_list_conversation_items_handle", rc, &error);
    goto done;
  }
  printf("items: %lu\n", (unsigned long)cai_input_item_list_count(items));
  exit_code = 0;

done:
  cai_input_item_list_destroy(items);
  if (output != NULL) {
    output->close(output);
  }
  if (session != NULL) {
    session->close(session);
  }
  cai_conversation_destroy(conversation);
  if (agent != NULL) {
    agent->close(agent);
  }
  if (client != NULL) {
    client->close(client);
  }
  cai_string_destroy(dotenv_api_key);
  cai_error_cleanup(&error);
  return exit_code;
}
