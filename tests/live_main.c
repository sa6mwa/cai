#include <cai/cai.h>

#include <stdio.h>
#include <stdlib.h>

int main(void) {
  const char *model;
  cai_client_config client_config;
  cai_response_create_params *params;
  cai_response *response;
  cai_client *client;
  cai_error error;
  int rc;

  model = getenv("CAI_TEST_MODEL");
  if (model == NULL || model[0] == '\0') {
    model = CAI_MODEL_GPT_5_NANO;
  }

  cai_error_init(&error);
  cai_client_config_init(&client_config);
  client = NULL;
  params = NULL;
  response = NULL;

  rc = cai_client_open(&client_config, &client, &error);
  if (rc != CAI_OK) {
    fprintf(stderr, "client open failed: %s\n",
            error.message != NULL ? error.message : cai_status_string(rc));
    cai_error_cleanup(&error);
    return 1;
  }
  rc = cai_response_create_params_new(&params, &error);
  if (rc == CAI_OK) {
    rc = cai_response_create_params_set_model(params, model, &error);
  }
  if (rc == CAI_OK) {
    rc = cai_response_create_params_add_text(
        params, "user", "Reply with exactly: pong", &error);
  }
  if (rc == CAI_OK) {
    rc = cai_client_create_response(client, params, &response, &error);
  }
  if (rc != CAI_OK) {
    fprintf(stderr, "live response failed: %s\n",
            error.message != NULL ? error.message : cai_status_string(rc));
    if (error.detail != NULL) {
      fprintf(stderr, "detail: %s\n", error.detail);
    }
    cai_response_create_params_destroy(params);
    cai_client_close(client);
    cai_error_cleanup(&error);
    return 1;
  }
  if (cai_response_output_text(response) == NULL ||
      cai_response_output_text(response)[0] == '\0') {
    fprintf(stderr, "live response had no output text\n");
    cai_response_destroy(response);
    cai_response_create_params_destroy(params);
    cai_client_close(client);
    cai_error_cleanup(&error);
    return 1;
  }

  cai_response_destroy(response);
  cai_response_create_params_destroy(params);
  cai_client_close(client);
  cai_error_cleanup(&error);
  return 0;
}
