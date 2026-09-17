#include "cai_internal.h"

#include <string.h>
#include <time.h>

#define CAI_MODEL_CATALOG_CACHE_SECONDS 300

typedef struct cai_model_catalog_impl {
  cai_allocator allocator;
  cai_model_catalog_entry *entries;
} cai_model_catalog_impl;

typedef struct cai_model_catalog_item_doc {
  char *slug;
  char *display_name;
  char *description;
  long long context_window;
  int has_context_window;
  long long auto_compact_token_limit;
  int has_auto_compact_token_limit;
  char *comp_hash;
  int supported_in_api;
  int has_supported_in_api;
} cai_model_catalog_item_doc;

typedef struct cai_model_catalog_doc {
  lonejson_object_array models;
} cai_model_catalog_doc;

static const lonejson_field cai_model_catalog_item_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_model_catalog_item_doc, slug, "slug"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_model_catalog_item_doc,
                                          display_name, "display_name"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_model_catalog_item_doc,
                                          description, "description"),
    LONEJSON_FIELD_I64_PRESENT(cai_model_catalog_item_doc, context_window,
                               has_context_window, "context_window"),
    LONEJSON_FIELD_I64_PRESENT_NULLABLE(
        cai_model_catalog_item_doc, auto_compact_token_limit,
        has_auto_compact_token_limit, "auto_compact_token_limit"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_model_catalog_item_doc, comp_hash,
                                          "comp_hash"),
    LONEJSON_FIELD_BOOL_PRESENT(cai_model_catalog_item_doc, supported_in_api,
                                has_supported_in_api, "supported_in_api")};
LONEJSON_MAP_DEFINE(cai_model_catalog_item_map, cai_model_catalog_item_doc,
                    cai_model_catalog_item_fields);

static const lonejson_field cai_model_catalog_fields[] = {
    LONEJSON_FIELD_OBJECT_ARRAY(
        cai_model_catalog_doc, models, "models", cai_model_catalog_item_doc,
        &cai_model_catalog_item_map, LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(cai_model_catalog_map, cai_model_catalog_doc,
                    cai_model_catalog_fields);

static void cai_model_catalog_destroy(cai_model_catalog *catalog) {
  cai_model_catalog_impl *impl;
  size_t i;

  if (catalog == NULL) {
    return;
  }
  impl = (cai_model_catalog_impl *)catalog->impl;
  if (impl != NULL) {
    for (i = 0U; i < catalog->count; i++) {
      cai_free_mem(&impl->allocator, impl->entries[i].slug);
      cai_free_mem(&impl->allocator, impl->entries[i].display_name);
      cai_free_mem(&impl->allocator, impl->entries[i].description);
      cai_free_mem(&impl->allocator,
                   impl->entries[i].compaction_compatibility_hash);
    }
    cai_free_mem(&impl->allocator, impl->entries);
    cai_free_mem(&impl->allocator, impl);
  }
  cai_free_mem(NULL, catalog);
}

void cai_model_catalog_close(cai_model_catalog *catalog) {
  if (catalog != NULL && catalog->close != NULL) {
    catalog->close(catalog);
  }
}

const cai_model_catalog_entry *
cai_model_catalog_find(const cai_model_catalog *catalog, const char *slug) {
  size_t i;

  if (catalog == NULL || slug == NULL) {
    return NULL;
  }
  for (i = 0U; i < catalog->count; i++) {
    if (catalog->entries[i].slug != NULL &&
        strcmp(catalog->entries[i].slug, slug) == 0) {
      return &catalog->entries[i];
    }
  }
  return NULL;
}

static int cai_model_catalog_allocate(const cai_allocator *allocator,
                                      size_t count, cai_model_catalog **out,
                                      cai_error *error) {
  cai_model_catalog *catalog;
  cai_model_catalog_impl *impl;

  *out = NULL;
  catalog = (cai_model_catalog *)cai_alloc(NULL, sizeof(*catalog));
  impl = (cai_model_catalog_impl *)cai_alloc(allocator, sizeof(*impl));
  if (catalog == NULL || impl == NULL) {
    cai_free_mem(NULL, catalog);
    cai_free_mem(allocator, impl);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate model catalog");
  }
  memset(catalog, 0, sizeof(*catalog));
  memset(impl, 0, sizeof(*impl));
  impl->allocator = *allocator;
  if (count > 0U) {
    impl->entries = (cai_model_catalog_entry *)cai_alloc(
        allocator, count * sizeof(*impl->entries));
    if (impl->entries == NULL) {
      cai_free_mem(allocator, impl);
      cai_free_mem(NULL, catalog);
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate model catalog entries");
    }
    memset(impl->entries, 0, count * sizeof(*impl->entries));
  }
  catalog->count = count;
  catalog->entries = impl->entries;
  catalog->close = cai_model_catalog_destroy;
  catalog->impl = impl;
  *out = catalog;
  return CAI_OK;
}

static int cai_model_catalog_copy_entry(const cai_allocator *allocator,
                                        cai_model_catalog_entry *destination,
                                        const cai_model_catalog_entry *source,
                                        cai_error *error) {
  destination->slug = cai_strdup(allocator, source->slug);
  destination->display_name = cai_strdup(allocator, source->display_name);
  destination->description = cai_strdup(allocator, source->description);
  destination->compaction_compatibility_hash =
      cai_strdup(allocator, source->compaction_compatibility_hash);
  destination->context_window_tokens = source->context_window_tokens;
  destination->auto_compact_token_limit = source->auto_compact_token_limit;
  destination->supported_in_api = source->supported_in_api;
  if (destination->slug == NULL ||
      (source->display_name != NULL && destination->display_name == NULL) ||
      (source->description != NULL && destination->description == NULL) ||
      (source->compaction_compatibility_hash != NULL &&
       destination->compaction_compatibility_hash == NULL)) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to copy model catalog entry");
  }
  return CAI_OK;
}

static int cai_model_catalog_clone(const cai_model_catalog *source,
                                   cai_model_catalog **out, cai_error *error) {
  cai_model_catalog_impl *source_impl;
  cai_model_catalog_impl *copy_impl;
  cai_model_catalog *copy;
  size_t i;
  int rc;

  source_impl = source != NULL ? (cai_model_catalog_impl *)source->impl : NULL;
  if (source == NULL || source_impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "model catalog is required");
  }
  rc = cai_model_catalog_allocate(&source_impl->allocator, source->count, &copy,
                                  error);
  copy_impl = copy != NULL ? (cai_model_catalog_impl *)copy->impl : NULL;
  for (i = 0U; rc == CAI_OK && i < source->count; i++) {
    rc = cai_model_catalog_copy_entry(&source_impl->allocator,
                                      copy_impl->entries + i,
                                      source->entries + i, error);
  }
  if (rc != CAI_OK) {
    cai_model_catalog_close(copy);
    return rc;
  }
  *out = copy;
  return CAI_OK;
}

static int cai_model_catalog_parse(cai_client *client, const char *json,
                                   cai_model_catalog **out, cai_error *error) {
  cai_model_catalog_doc doc;
  cai_model_catalog_item_doc *items;
  cai_model_catalog *catalog;
  cai_model_catalog_impl *catalog_impl;
  lonejson_error json_error;
  lonejson_status status;
  size_t i;
  int rc;

  memset(&doc, 0, sizeof(doc));
  CAI_LJ->init(CAI_LJ, &cai_model_catalog_map, &doc);
  status = CAI_LJ->parse_cstr(CAI_LJ, &cai_model_catalog_map, &doc, json,
                              &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_model_catalog_map, &doc);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to parse Codex model catalog",
                                json_error.message);
  }
  rc = cai_model_catalog_allocate(&CAI_CLIENT_IMPL(client)->allocator,
                                  doc.models.count, &catalog, error);
  catalog_impl =
      catalog != NULL ? (cai_model_catalog_impl *)catalog->impl : NULL;
  items = (cai_model_catalog_item_doc *)doc.models.items;
  for (i = 0U; rc == CAI_OK && i < doc.models.count; i++) {
    cai_model_catalog_entry source;

    memset(&source, 0, sizeof(source));
    source.slug = items[i].slug;
    source.display_name = items[i].display_name;
    source.description = items[i].description;
    source.context_window_tokens =
        items[i].has_context_window ? items[i].context_window : 0LL;
    source.auto_compact_token_limit = items[i].has_auto_compact_token_limit
                                          ? items[i].auto_compact_token_limit
                                          : 0LL;
    source.compaction_compatibility_hash = items[i].comp_hash;
    source.supported_in_api =
        items[i].has_supported_in_api ? items[i].supported_in_api : 0;
    rc =
        cai_model_catalog_copy_entry(&CAI_CLIENT_IMPL(client)->allocator,
                                     catalog_impl->entries + i, &source, error);
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_model_catalog_map, &doc);
  if (rc != CAI_OK) {
    cai_model_catalog_close(catalog);
    return rc;
  }
  *out = catalog;
  return CAI_OK;
}

int cai_client_list_models(cai_client *client,
                           cai_model_catalog_refresh_strategy strategy,
                           cai_model_catalog **out, cai_error *error) {
  cai_client_impl *impl;
  cai_model_catalog *catalog;
  char *body;
  char *request_id;
  long http_status;
  time_t now;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "model catalog output pointer is required");
  }
  *out = NULL;
  if (client == NULL || client->impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "client is required");
  }
  if (strategy != CAI_MODEL_CATALOG_REFRESH_ONLINE &&
      strategy != CAI_MODEL_CATALOG_REFRESH_OFFLINE &&
      strategy != CAI_MODEL_CATALOG_REFRESH_ONLINE_IF_UNCACHED) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "invalid model catalog refresh strategy");
  }
  impl = CAI_CLIENT_IMPL(client);
  if (impl->chatgpt_auth == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Codex model discovery requires ChatGPT auth");
  }
  now = time(NULL);
  if (impl->model_catalog != NULL &&
      (strategy == CAI_MODEL_CATALOG_REFRESH_OFFLINE ||
       (strategy == CAI_MODEL_CATALOG_REFRESH_ONLINE_IF_UNCACHED &&
        now >= impl->model_catalog_fetched_at &&
        now - impl->model_catalog_fetched_at <=
            CAI_MODEL_CATALOG_CACHE_SECONDS))) {
    return cai_model_catalog_clone(impl->model_catalog, out, error);
  }
  if (strategy == CAI_MODEL_CATALOG_REFRESH_OFFLINE) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "no cached ChatGPT model catalog is available");
  }
  body = NULL;
  request_id = NULL;
  http_status = 0L;
  rc = cai_http_json_request(client, "GET",
                             "models?client_version=" CAI_VERSION_STRING, NULL,
                             &body, &http_status, &request_id, error);
  if (rc == CAI_OK && (http_status < 200L || http_status >= 300L)) {
    rc = cai_set_openai_error(error, http_status, body, request_id);
  }
  if (rc == CAI_OK) {
    rc = cai_model_catalog_parse(client, body, &catalog, error);
  }
  cai_free_mem(NULL, body);
  cai_free_mem(NULL, request_id);
  if (rc != CAI_OK) {
    return rc;
  }
  cai_model_catalog_close(impl->model_catalog);
  impl->model_catalog = catalog;
  impl->model_catalog_fetched_at = now;
  return cai_model_catalog_clone(impl->model_catalog, out, error);
}
