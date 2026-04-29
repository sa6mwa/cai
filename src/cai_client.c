#include "cai_internal.h"

void cai_client_config_init(cai_client_config *config) {
  if (config == NULL) {
    return;
  }
  config->api_key = NULL;
  config->base_url = CAI_DEFAULT_BASE_URL;
  config->organization_id = NULL;
  config->project_id = NULL;
  config->timeout_ms = 0L;
  config->http_2_disabled = 0;
  config->insecure_skip_verify = 0;
  config->json_response_limit_bytes = CAI_DEFAULT_JSON_RESPONSE_LIMIT;
  config->logger = NULL;
  config->allocator.malloc_fn = NULL;
  config->allocator.realloc_fn = NULL;
  config->allocator.free_fn = NULL;
  config->allocator.context = NULL;
}

static void cai_client_destroy_fields(cai_client *client) {
  if (client == NULL) {
    return;
  }
  cai_free_mem(&client->allocator, client->api_key);
  cai_free_mem(&client->allocator, client->base_url);
  cai_free_mem(&client->allocator, client->organization_id);
  cai_free_mem(&client->allocator, client->project_id);
}

int cai_client_open(const cai_client_config *config, cai_client **out,
                    cai_error *error) {
  cai_client_config defaults;
  const cai_client_config *effective;
  cai_client *client;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "client output pointer is required");
  }
  *out = NULL;
  if (config == NULL) {
    cai_client_config_init(&defaults);
    effective = &defaults;
  } else {
    effective = config;
  }
  client = (cai_client *)cai_alloc(&effective->allocator, sizeof(*client));
  if (client == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate client");
  }
  client->allocator = effective->allocator;
  client->api_key = NULL;
  client->base_url = NULL;
  client->organization_id = NULL;
  client->project_id = NULL;
  client->timeout_ms = effective->timeout_ms;
  client->http_2_disabled = effective->http_2_disabled;
  client->insecure_skip_verify = effective->insecure_skip_verify;
  client->json_response_limit_bytes = effective->json_response_limit_bytes;
  client->logger = effective->logger;

  if (client->json_response_limit_bytes == 0U) {
    client->json_response_limit_bytes = CAI_DEFAULT_JSON_RESPONSE_LIMIT;
  }

  rc = cai_resolve_api_key(&client->allocator, effective->api_key,
                           &client->api_key, error);
  if (rc != CAI_OK) {
    cai_free_mem(&client->allocator, client);
    return rc;
  }
  client->base_url = cai_strdup(&client->allocator, effective->base_url);
  if (client->base_url == NULL) {
    cai_client_destroy_fields(client);
    cai_free_mem(&client->allocator, client);
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate base URL");
  }
  client->organization_id =
      cai_strdup(&client->allocator, effective->organization_id);
  if (effective->organization_id != NULL && client->organization_id == NULL) {
    cai_client_destroy_fields(client);
    cai_free_mem(&client->allocator, client);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate organization id");
  }
  client->project_id = cai_strdup(&client->allocator, effective->project_id);
  if (effective->project_id != NULL && client->project_id == NULL) {
    cai_client_destroy_fields(client);
    cai_free_mem(&client->allocator, client);
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate project id");
  }
  *out = client;
  return CAI_OK;
}

void cai_client_close(cai_client *client) {
  cai_allocator allocator;

  if (client == NULL) {
    return;
  }
  allocator = client->allocator;
  cai_client_destroy_fields(client);
  cai_free_mem(&allocator, client);
}
