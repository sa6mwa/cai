#include "cai_internal.h"

#include <cai/auth.h>

#include <string.h>

static void cai_client_clear_secret(char *value) {
  volatile char *p;
  size_t len;

  if (value == NULL) {
    return;
  }
  len = strlen(value);
  p = (volatile char *)value;
  while (len-- > 0U) {
    *p++ = '\0';
  }
}

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
  cai_client_clear_secret(impl->api_key);
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
  impl->chatgpt_auth = effective->chatgpt_auth;
  impl->base_url = NULL;
  impl->organization_id = NULL;
  impl->project_id = NULL;
  impl->timeout_ms = effective->timeout_ms > 0L ? effective->timeout_ms
                                                : CAI_DEFAULT_HTTP_TIMEOUT_MS;
  impl->http_2_disabled = effective->http_2_disabled;
  impl->insecure_skip_verify = effective->insecure_skip_verify;
  impl->json_response_limit_bytes = effective->json_response_limit_bytes;
  impl->logger = effective->logger_disabled ? NULL : effective->logger;
  impl->logger_disabled = effective->logger_disabled;

  if (impl->json_response_limit_bytes == 0U) {
    impl->json_response_limit_bytes = CAI_DEFAULT_JSON_RESPONSE_LIMIT;
  }

  if (effective->chatgpt_auth != NULL) {
    rc = cai_chatgpt_auth_access_token(effective->chatgpt_auth, &impl->api_key,
                                       error);
  } else {
    rc = cai_resolve_api_key(&impl->allocator, effective->api_key,
                             effective->api_key_env != NULL
                                 ? effective->api_key_env
                                 : CAI_OPENAI_API_KEY_ENV,
                             &impl->api_key, error);
  }
  if (rc != CAI_OK) {
    cai_free_mem(&impl->allocator, impl);
    cai_free_mem(&effective->allocator, client);
    return rc;
  }
  impl->base_url =
      cai_strdup(&impl->allocator, effective->base_url != NULL
                                       ? effective->base_url
                                       : (effective->chatgpt_auth != NULL
                                              ? CAI_CHATGPT_CODEX_BASE_URL
                                              : CAI_DEFAULT_BASE_URL));
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

int cai_client_refresh_chatgpt_auth(cai_client *client, cai_error *error) {
  cai_client_impl *impl;
  char *access_token;
  int rc;

  if (client == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "client is required");
  }
  impl = CAI_CLIENT_IMPL(client);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "client is closed");
  }
  if (impl->chatgpt_auth == NULL) {
    return CAI_OK;
  }
  access_token = NULL;
  rc = cai_chatgpt_auth_refresh(impl->chatgpt_auth, error);
  if (rc == CAI_OK) {
    rc =
        cai_chatgpt_auth_access_token(impl->chatgpt_auth, &access_token, error);
  }
  if (rc != CAI_OK) {
    cai_free_mem(NULL, access_token);
    return rc;
  }
  cai_client_clear_secret(impl->api_key);
  cai_free_mem(&impl->allocator, impl->api_key);
  impl->api_key = cai_strdup(&impl->allocator, access_token);
  cai_client_clear_secret(access_token);
  cai_free_mem(NULL, access_token);
  if (impl->api_key == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate refreshed access token");
  }
  return CAI_OK;
}

int cai_client_refresh_chatgpt_auth_after_http(cai_client *client,
                                               long http_status,
                                               cai_error *error) {
  if (http_status != 401L && http_status != 403L) {
    return CAI_OK;
  }
  if (client == NULL || CAI_CLIENT_IMPL(client)->chatgpt_auth == NULL) {
    return CAI_OK;
  }
  return cai_client_refresh_chatgpt_auth(client, error);
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
