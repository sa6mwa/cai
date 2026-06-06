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

int main(void) {
  cai_client_config client_config;
  cai_response_create_params *params;
  cai_source *source;
  cai_client *client;
  cai_error error;
  char *dotenv_api_key;
  char buffer[256];
  size_t got;
  int exit_code;
  int rc;

  cai_error_init(&error);
  cai_client_config_init(&client_config);
  client = NULL;
  params = NULL;
  source = NULL;
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
    rc = params->add_text(params, "user",
                          "Write one short sentence about streaming in C.",
                          &error);
  }
  if (rc == CAI_OK) {
    rc = client->open_response_text_source(client, params, &source, &error);
  }
  if (rc != CAI_OK) {
    exit_code = print_error("cai_client_open_response_text_source", rc, &error);
    goto done;
  }

  while ((got = source->read(source, buffer, sizeof(buffer), &error)) > 0U) {
    fwrite(buffer, 1U, got, stdout);
    fflush(stdout);
  }
  fputc('\n', stdout);
  exit_code = 0;

done:
  if (source != NULL) {
    source->close(source);
  }
  cai_response_create_params_destroy(params);
  if (client != NULL) {
    client->close(client);
  }
  cai_string_destroy(dotenv_api_key);
  cai_error_cleanup(&error);
  return exit_code;
}
