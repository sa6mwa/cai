#include "cai_internal.h"

#include <string.h>

void cai_client_config_init(cai_client_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
}

void cai_client_config_use_openrouter(cai_client_config *config) {
  if (config == NULL) {
    return;
  }
  config->api_key_env = CAI_OPENROUTER_API_KEY_ENV;
  config->base_url = CAI_OPENROUTER_BASE_URL;
  config->organization_id = NULL;
  config->project_id = NULL;
}

static int cai_allocator_is_empty(const cai_allocator *allocator) {
  return allocator->malloc_fn == NULL && allocator->realloc_fn == NULL &&
         allocator->free_fn == NULL;
}

static int cai_allocator_is_complete(const cai_allocator *allocator) {
  return allocator->malloc_fn != NULL && allocator->realloc_fn != NULL &&
         allocator->free_fn != NULL;
}

static void cai_client_destroy_fields(cai_client_impl *impl) {
  if (impl == NULL) {
    return;
  }
  cai_free_mem(&impl->allocator, impl->api_key);
  cai_free_mem(&impl->allocator, impl->base_url);
  cai_free_mem(&impl->allocator, impl->organization_id);
  cai_free_mem(&impl->allocator, impl->project_id);
}

int cai_client_open(const cai_client_config *config, cai_client **out,
                    cai_error *error) {
  cai_client_config defaults;
  const cai_client_config *effective;
  cai_client *client;
  cai_client_impl *impl;
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
  if (!cai_allocator_is_empty(&effective->allocator) &&
      !cai_allocator_is_complete(&effective->allocator)) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "custom allocator requires malloc, realloc, and free");
  }
  client = (cai_client *)cai_alloc(&effective->allocator, sizeof(*client));
  if (client == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate client");
  }
  memset(client, 0, sizeof(*client));
  impl = (cai_client_impl *)cai_alloc(&effective->allocator, sizeof(*impl));
  if (impl == NULL) {
    cai_free_mem(&effective->allocator, client);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate client implementation");
  }
  memset(impl, 0, sizeof(*impl));
  impl->allocator = effective->allocator;
  impl->api_key = NULL;
  impl->base_url = NULL;
  impl->organization_id = NULL;
  impl->project_id = NULL;
  impl->timeout_ms = effective->timeout_ms;
  impl->http_2_disabled = effective->http_2_disabled;
  impl->insecure_skip_verify = effective->insecure_skip_verify;
  impl->json_response_limit_bytes = effective->json_response_limit_bytes;
  impl->logger = effective->logger_disabled ? NULL : effective->logger;
  impl->logger_disabled = effective->logger_disabled;

  if (impl->json_response_limit_bytes == 0U) {
    impl->json_response_limit_bytes = CAI_DEFAULT_JSON_RESPONSE_LIMIT;
  }

  rc = cai_resolve_api_key(
      &impl->allocator, effective->api_key,
      effective->api_key_env != NULL ? effective->api_key_env
                                     : CAI_OPENAI_API_KEY_ENV,
      &impl->api_key, error);
  if (rc != CAI_OK) {
    cai_free_mem(&impl->allocator, impl);
    cai_free_mem(&effective->allocator, client);
    return rc;
  }
  impl->base_url = cai_strdup(
      &impl->allocator,
      effective->base_url != NULL ? effective->base_url : CAI_DEFAULT_BASE_URL);
  if (impl->base_url == NULL) {
    cai_client_destroy_fields(impl);
    cai_free_mem(&impl->allocator, impl);
    cai_free_mem(&effective->allocator, client);
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate base URL");
  }
  impl->organization_id =
      cai_strdup(&impl->allocator, effective->organization_id);
  if (effective->organization_id != NULL && impl->organization_id == NULL) {
    cai_client_destroy_fields(impl);
    cai_free_mem(&impl->allocator, impl);
    cai_free_mem(&effective->allocator, client);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate organization id");
  }
  impl->project_id = cai_strdup(&impl->allocator, effective->project_id);
  if (effective->project_id != NULL && impl->project_id == NULL) {
    cai_client_destroy_fields(impl);
    cai_free_mem(&impl->allocator, impl);
    cai_free_mem(&effective->allocator, client);
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate project id");
  }
  client->new_agent = cai_client_new_agent;
  client->create_conversation = cai_client_create_conversation;
  client->close = cai_client_close;
  client->impl = impl;
  *out = client;
  cai_log_client_opened(impl);
  return CAI_OK;
}

void cai_client_close(cai_client *client) {
  cai_allocator allocator;
  cai_client_impl *impl;

  if (client == NULL) {
    return;
  }
  impl = CAI_CLIENT_IMPL(client);
  if (impl == NULL) {
    return;
  }
  allocator = impl->allocator;
  cai_client_destroy_fields(impl);
  cai_free_mem(&allocator, impl);
  client->impl = NULL;
  cai_free_mem(&allocator, client);
}
