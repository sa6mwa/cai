#include <cai/cai.h>

#include "../common.h"

#include <stdio.h>
#include <stdlib.h>

static const char *example_model(void) {
  const char *model;

  model = getenv("CAI_OPENROUTER_EXAMPLE_MODEL");
  if (model == NULL || model[0] == '\0') {
    return CAI_OPENROUTER_MODEL_DEFAULT_RESPONSES;
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
  cai_client_config client_config;
  cai_response_create_params *params;
  cai_response *response;
  cai_client *client;
  cai_error error;
  char *dotenv_api_key;
  int rc;
  int exit_code;

  cai_error_init(&error);
  cai_client_config_init(&client_config);
  cai_client_config_use_openrouter(&client_config);
  client = NULL;
  params = NULL;
  response = NULL;
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
  rc = cai_response_create_params_new(&params, &error);
  if (rc == CAI_OK) {
    rc = params->set_model(params, example_model(), &error);
  }
  if (rc == CAI_OK) {
    rc = params->set_max_output_tokens(params, 96, &error);
  }
  if (rc == CAI_OK) {
    rc = params->set_reasoning(params, CAI_REASONING_EFFORT_NONE, NULL, &error);
  }
  if (rc == CAI_OK) {
    rc = params->add_text(params, "user",
                          "In one sentence, say hello from cai via OpenRouter.",
                          &error);
  }
  if (rc == CAI_OK) {
    rc = client->create_response(client, params, &response, &error);
  }
  if (rc != CAI_OK) {
    exit_code = print_error("cai_client_create_response", rc, &error);
  } else {
    printf("%s\n", response->output_text(response) != NULL
                       ? response->output_text(response)
                       : "");
    exit_code = 0;
  }

done:
  if (response != NULL) {
    response->close(response);
  }
  if (params != NULL) {
    params->close(params);
  }
  if (client != NULL) {
    client->close(client);
  }
  cai_string_destroy(dotenv_api_key);
  cai_error_cleanup(&error);
  return exit_code;
}
