#include "cai_internal.h"
#include "cai_model_contexts.h"

#include <string.h>
#include <strings.h>
#include <time.h>

#define CAI_MODEL_CATALOG_CACHE_SECONDS 300

typedef struct cai_catalog_policy {
  const char *host;
  const char *request_path;
  int provider_format;
  const cai_model_context_seed *contexts;
  size_t context_count;
} cai_catalog_policy;

static const cai_catalog_policy cai_catalog_policies[] = {
    {"chatgpt.com", "models?client_version=" CAI_VERSION_STRING, 0, NULL, 0U},
    {"api.berget.ai", "models", 1, cai_berget_contexts,
     sizeof(cai_berget_contexts) / sizeof(cai_berget_contexts[0])}};

static void cai_model_catalog_seed_contexts(cai_client *client,
                                            cai_model_catalog *catalog);

static const cai_catalog_policy *cai_catalog_policy_for_url(const char *url) {
  CURLU *parsed;
  char *host;
  const cai_catalog_policy *policy;
  size_t i;

  if (url == NULL) {
    return NULL;
  }
  parsed = curl_url();
  if (parsed == NULL) {
    return NULL;
  }
  host = NULL;
  policy = NULL;
  if (curl_url_set(parsed, CURLUPART_URL, url, 0U) == CURLUE_OK &&
      curl_url_get(parsed, CURLUPART_HOST, &host, 0U) == CURLUE_OK) {
    for (i = 0U;
         i < sizeof(cai_catalog_policies) / sizeof(cai_catalog_policies[0]);
         i++) {
      if (strcasecmp(host, cai_catalog_policies[i].host) == 0) {
        policy = &cai_catalog_policies[i];
        break;
      }
    }
  }
  curl_free(host);
  curl_url_cleanup(parsed);
  return policy;
}

static const cai_catalog_policy *
cai_client_catalog_policy(const cai_client *client) {
  const cai_client_impl *impl;
  if (client == NULL || client->impl == NULL) {
    return NULL;
  }
  impl = CAI_CLIENT_IMPL(client);
  return cai_catalog_policy_for_url(impl->catalog_policy_origin != NULL
                                        ? impl->catalog_policy_origin
                                        : impl->base_url);
}

int cai_client_has_live_model_catalog(const cai_client *client) {
  return cai_client_catalog_policy(client) != NULL;
}

static long long cai_catalog_seed_context(const cai_catalog_policy *policy,
                                          const char *model) {
  size_t i;
  if (policy == NULL || model == NULL) {
    return 0LL;
  }
  for (i = 0U; i < policy->context_count; i++) {
    if (strcmp(policy->contexts[i].id, model) == 0) {
      return policy->contexts[i].tokens;
    }
  }
  return 0LL;
}

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

typedef struct cai_provider_capabilities_doc {
  int streaming;
  int has_streaming;
  int function_calling;
  int has_function_calling;
  int vision;
  int has_vision;
  int formatted_output;
  int has_formatted_output;
  int json_mode;
  int has_json_mode;
} cai_provider_capabilities_doc;

typedef struct cai_provider_pricing_doc {
  char *currency;
  char *unit;
  double input;
  double output;
  int has_input;
  int has_output;
} cai_provider_pricing_doc;

typedef struct cai_provider_model_doc {
  char *provider;
  char *id;
  char *name;
  char *description;
  char *model_type;
  char *lifecycle_status;
  char *comp_hash;
  long long context_window_tokens;
  int has_context_window_tokens;
  long long auto_compact_token_limit;
  int has_auto_compact_token_limit;
  cai_provider_capabilities_doc capabilities;
  cai_provider_pricing_doc pricing;
} cai_provider_model_doc;

typedef struct cai_provider_catalog_doc {
  lonejson_object_array data;
  lonejson_object_array models;
} cai_provider_catalog_doc;

static const lonejson_field cai_provider_capabilities_fields[] = {
    LONEJSON_FIELD_BOOL_PRESENT(cai_provider_capabilities_doc, streaming,
                                has_streaming, "streaming"),
    LONEJSON_FIELD_BOOL_PRESENT(cai_provider_capabilities_doc, function_calling,
                                has_function_calling, "function_calling"),
    LONEJSON_FIELD_BOOL_PRESENT(cai_provider_capabilities_doc, vision,
                                has_vision, "vision"),
    LONEJSON_FIELD_BOOL_PRESENT(cai_provider_capabilities_doc, formatted_output,
                                has_formatted_output, "formatted_output"),
    LONEJSON_FIELD_BOOL_PRESENT(cai_provider_capabilities_doc, json_mode,
                                has_json_mode, "json_mode")};
LONEJSON_MAP_DEFINE(cai_provider_capabilities_map,
                    cai_provider_capabilities_doc,
                    cai_provider_capabilities_fields);

static const lonejson_field cai_provider_pricing_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_provider_pricing_doc, currency,
                                          "currency"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_provider_pricing_doc, unit,
                                          "unit"),
    LONEJSON_FIELD_F64_PRESENT(cai_provider_pricing_doc, input, has_input,
                               "input"),
    LONEJSON_FIELD_F64_PRESENT(cai_provider_pricing_doc, output, has_output,
                               "output")};
LONEJSON_MAP_DEFINE(cai_provider_pricing_map, cai_provider_pricing_doc,
                    cai_provider_pricing_fields);

static const lonejson_field cai_provider_model_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_provider_model_doc, id, "id"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_provider_model_doc, provider,
                                          "provider"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_provider_model_doc, name, "name"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_provider_model_doc, description,
                                          "description"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_provider_model_doc, model_type,
                                          "model_type"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_provider_model_doc,
                                          lifecycle_status, "lifecycle_status"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_provider_model_doc, comp_hash,
                                          "comp_hash"),
    LONEJSON_FIELD_I64_PRESENT(cai_provider_model_doc, context_window_tokens,
                               has_context_window_tokens,
                               "context_window_tokens"),
    LONEJSON_FIELD_I64_PRESENT(cai_provider_model_doc, auto_compact_token_limit,
                               has_auto_compact_token_limit,
                               "auto_compact_token_limit"),
    LONEJSON_FIELD_OBJECT_OMIT_EMPTY(cai_provider_model_doc, capabilities,
                                     "capabilities",
                                     &cai_provider_capabilities_map),
    LONEJSON_FIELD_OBJECT_OMIT_EMPTY(cai_provider_model_doc, pricing, "pricing",
                                     &cai_provider_pricing_map)};
LONEJSON_MAP_DEFINE(cai_provider_model_map, cai_provider_model_doc,
                    cai_provider_model_fields);

static const lonejson_field cai_provider_catalog_fields[] = {
    LONEJSON_FIELD_OBJECT_ARRAY_OMIT_EMPTY(
        cai_provider_catalog_doc, data, "data", cai_provider_model_doc,
        &cai_provider_model_map, LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_OBJECT_ARRAY_OMIT_EMPTY(
        cai_provider_catalog_doc, models, "models", cai_provider_model_doc,
        &cai_provider_model_map, LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(cai_provider_catalog_map, cai_provider_catalog_doc,
                    cai_provider_catalog_fields);

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
      cai_free_mem(&impl->allocator, impl->entries[i].model_type);
      cai_free_mem(&impl->allocator, impl->entries[i].lifecycle_status);
      cai_free_mem(&impl->allocator, impl->entries[i].pricing_currency);
      cai_free_mem(&impl->allocator, impl->entries[i].pricing_unit);
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
  destination->model_type = cai_strdup(allocator, source->model_type);
  destination->lifecycle_status =
      cai_strdup(allocator, source->lifecycle_status);
  destination->pricing_currency =
      cai_strdup(allocator, source->pricing_currency);
  destination->pricing_unit =
      cai_strdup(allocator, source->pricing_unit != NULL
                                ? source->pricing_unit
                                : (source->has_pricing ? "M Token" : NULL));
  destination->context_window_tokens = source->context_window_tokens;
  destination->auto_compact_token_limit = source->auto_compact_token_limit;
  destination->supported_in_api = source->supported_in_api;
  destination->capabilities = source->capabilities;
  destination->capabilities_known = source->capabilities_known;
  destination->input_price = source->input_price;
  destination->output_price = source->output_price;
  destination->has_pricing = source->has_pricing;
  if (destination->slug == NULL ||
      (source->display_name != NULL && destination->display_name == NULL) ||
      (source->description != NULL && destination->description == NULL) ||
      (source->compaction_compatibility_hash != NULL &&
       destination->compaction_compatibility_hash == NULL) ||
      (source->model_type != NULL && destination->model_type == NULL) ||
      (source->lifecycle_status != NULL &&
       destination->lifecycle_status == NULL) ||
      (source->pricing_currency != NULL &&
       destination->pricing_currency == NULL) ||
      (source->has_pricing && destination->pricing_unit == NULL)) {
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

static unsigned int cai_provider_capability_flags(
    const cai_provider_capabilities_doc *capabilities) {
  unsigned int flags;

  flags = 0U;
  if (capabilities->streaming) {
    flags |= CAI_MODEL_CAP_STREAMING;
  }
  if (capabilities->function_calling) {
    flags |= CAI_MODEL_CAP_FUNCTION_CALLING;
  }
  if (capabilities->vision) {
    flags |= CAI_MODEL_CAP_IMAGE_INPUT;
  }
  if (capabilities->formatted_output || capabilities->json_mode) {
    flags |= CAI_MODEL_CAP_STRUCTURED_OUTPUTS;
  }
  return flags;
}

static unsigned int cai_provider_capability_known(
    const cai_provider_capabilities_doc *capabilities) {
  unsigned int flags;

  flags = 0U;
  if (capabilities->has_streaming) {
    flags |= CAI_MODEL_CAP_STREAMING;
  }
  if (capabilities->has_function_calling) {
    flags |= CAI_MODEL_CAP_FUNCTION_CALLING;
  }
  if (capabilities->has_vision) {
    flags |= CAI_MODEL_CAP_IMAGE_INPUT;
  }
  if (capabilities->has_formatted_output || capabilities->has_json_mode) {
    flags |= CAI_MODEL_CAP_STRUCTURED_OUTPUTS;
  }
  return flags;
}

static int cai_model_catalog_parse_provider(cai_client *client,
                                            const char *json, int fallback,
                                            cai_model_catalog **out,
                                            cai_error *error) {
  cai_client_impl *impl;
  cai_provider_catalog_doc doc;
  cai_provider_model_doc *models;
  cai_model_catalog *catalog;
  cai_model_catalog_impl *catalog_impl;
  lonejson_error json_error;
  lonejson_status status;
  size_t model_count;
  size_t selected_count;
  size_t i;
  size_t j;
  int rc;

  impl = CAI_CLIENT_IMPL(client);
  memset(&doc, 0, sizeof(doc));
  CAI_LJ->init(CAI_LJ, &cai_provider_catalog_map, &doc);
  status = CAI_LJ->parse_cstr(CAI_LJ, &cai_provider_catalog_map, &doc, json,
                              &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_provider_catalog_map, &doc);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                fallback ? "invalid model metadata JSON"
                                         : "invalid provider model catalog",
                                json_error.message);
  }
  models =
      (cai_provider_model_doc *)(fallback ? doc.models.items : doc.data.items);
  model_count = fallback ? doc.models.count : doc.data.count;
  selected_count = 0U;
  for (i = 0U; i < model_count; i++) {
    if (models[i].id == NULL || models[i].id[0] == '\0' ||
        (fallback &&
         (models[i].provider == NULL || models[i].provider[0] == '\0')) ||
        models[i].context_window_tokens < 0LL ||
        models[i].auto_compact_token_limit < 0LL ||
        models[i].pricing.input < 0.0 || models[i].pricing.output < 0.0) {
      CAI_LJ->cleanup(CAI_LJ, &cai_provider_catalog_map, &doc);
      return cai_set_error(error, CAI_ERR_INVALID,
                           "invalid model metadata entry");
    }
    for (j = 0U; j < i; j++) {
      if (strcmp(models[j].id, models[i].id) == 0 &&
          (!fallback || strcmp(models[j].provider, models[i].provider) == 0)) {
        CAI_LJ->cleanup(CAI_LJ, &cai_provider_catalog_map, &doc);
        return cai_set_error(error, CAI_ERR_INVALID,
                             "duplicate provider model metadata entry");
      }
    }
    if (!fallback || strcmp(models[i].provider, impl->metadata_provider) == 0) {
      selected_count++;
    }
  }
  catalog = NULL;
  rc = cai_model_catalog_allocate(&impl->allocator, selected_count, &catalog,
                                  error);
  catalog_impl =
      catalog != NULL ? (cai_model_catalog_impl *)catalog->impl : NULL;
  selected_count = 0U;
  for (i = 0U; rc == CAI_OK && i < model_count; i++) {
    cai_model_catalog_entry source;
    const cai_provider_pricing_doc *price;

    if (fallback && strcmp(models[i].provider, impl->metadata_provider) != 0) {
      continue;
    }
    memset(&source, 0, sizeof(source));
    source.slug = models[i].id;
    source.display_name = models[i].name;
    source.description = models[i].description;
    source.model_type = models[i].model_type;
    source.lifecycle_status = models[i].lifecycle_status;
    source.compaction_compatibility_hash = models[i].comp_hash;
    source.context_window_tokens = models[i].context_window_tokens;
    source.auto_compact_token_limit = models[i].auto_compact_token_limit;
    source.capabilities =
        cai_provider_capability_flags(&models[i].capabilities);
    source.capabilities_known =
        cai_provider_capability_known(&models[i].capabilities);
    price = &models[i].pricing;
    if (price->currency != NULL && price->currency[0] != '\0' &&
        price->has_input && price->has_output &&
        (fallback || (price->unit != NULL && price->unit[0] != '\0'))) {
      source.pricing_currency = price->currency;
      source.pricing_unit = price->unit;
      source.input_price = price->input;
      source.output_price = price->output;
      source.has_pricing = 1;
    }
    rc = cai_model_catalog_copy_entry(&impl->allocator,
                                      catalog_impl->entries + selected_count,
                                      &source, error);
    selected_count++;
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_provider_catalog_map, &doc);
  if (rc != CAI_OK) {
    cai_model_catalog_close(catalog);
    return rc;
  }
  *out = catalog;
  return CAI_OK;
}

int cai_model_catalog_load_fallback(cai_client *client, const char *json,
                                    cai_error *error) {
  cai_model_catalog *catalog;
  int rc;

  catalog = NULL;
  rc = cai_model_catalog_parse_provider(client, json, 1, &catalog, error);
  if (rc == CAI_OK) {
    cai_model_catalog_seed_contexts(client, catalog);
    CAI_CLIENT_IMPL(client)->fallback_catalog = catalog;
  }
  return rc;
}

const cai_model_catalog_entry *
cai_client_cached_model_metadata(const cai_client *client, const char *model) {
  const cai_client_impl *impl;
  const cai_model_catalog_entry *entry;

  if (client == NULL || client->impl == NULL || model == NULL) {
    return NULL;
  }
  impl = CAI_CLIENT_IMPL(client);
  entry = cai_model_catalog_find(impl->model_catalog, model);
  return entry != NULL ? entry
                       : cai_model_catalog_find(impl->fallback_catalog, model);
}

static int
cai_model_catalog_has_token_price(const cai_model_catalog_entry *entry) {
  return entry != NULL && entry->has_pricing && entry->pricing_unit != NULL &&
         strstr(entry->pricing_unit, "M Token") != NULL;
}

static int cai_client_static_model_matches(const cai_client *client,
                                           const char *model) {
  const cai_model_info *info;
  const char *provider;
  int openrouter_row;

  if (client == NULL || client->impl == NULL) {
    return 0;
  }
  info = cai_model_info_by_id(model);
  if (info == NULL) {
    return 0;
  }
  provider = CAI_CLIENT_IMPL(client)->metadata_provider;
  openrouter_row =
      (info->metadata_flags & CAI_MODEL_META_PROVIDER_OPENROUTER) != 0U;
  if (strcmp(provider, "openrouter") == 0) {
    return openrouter_row;
  }
  return !openrouter_row &&
         (strcmp(provider, "openai") == 0 || strcmp(provider, "chatgpt") == 0);
}

int cai_client_model_can_estimate_usd(const cai_client *client,
                                      const char *model) {
  const cai_model_catalog_entry *entry;

  if (cai_client_static_model_matches(client, model) &&
      cai_model_can_estimate_usage_usd(model)) {
    return 1;
  }
  entry = cai_client_cached_model_metadata(client, model);
  if (cai_model_catalog_has_token_price(entry)) {
    return strcmp(entry->pricing_currency, "USD") == 0;
  }
  return 0;
}

void cai_client_model_estimate_usage(const cai_client *client,
                                     const char *model,
                                     const cai_token_usage *usage, double *usd,
                                     double *eur) {
  const cai_model_catalog_entry *entry;
  double estimate;

  *usd = 0.0;
  *eur = 0.0;
  if (usage == NULL) {
    return;
  }
  if (cai_client_static_model_matches(client, model) &&
      cai_model_can_estimate_usage_usd(model)) {
    *usd = cai_model_estimate_usage_usd(model, usage->input_tokens,
                                        usage->input_cached_tokens,
                                        usage->output_tokens);
    return;
  }
  entry = cai_client_cached_model_metadata(client, model);
  if (cai_model_catalog_has_token_price(entry)) {
    estimate =
        ((double)(usage->input_tokens > 0LL ? usage->input_tokens : 0LL) *
             entry->input_price +
         (double)(usage->output_tokens > 0LL ? usage->output_tokens : 0LL) *
             entry->output_price) /
        1000000.0;
    if (strcmp(entry->pricing_currency, "EUR") == 0) {
      *eur = estimate;
    } else if (strcmp(entry->pricing_currency, "USD") == 0) {
      *usd = estimate;
    }
  }
}

long long cai_client_model_context_window(const cai_client *client,
                                          const char *model) {
  const cai_model_catalog_entry *entry;

  entry = cai_client_cached_model_metadata(client, model);
  if (entry != NULL && entry->context_window_tokens > 0LL) {
    return entry->context_window_tokens;
  }
  if (cai_client_catalog_policy(client) != NULL) {
    long long seeded =
        cai_catalog_seed_context(cai_client_catalog_policy(client), model);
    if (seeded > 0LL) {
      return seeded;
    }
  }
  return cai_client_static_model_matches(client, model)
             ? cai_model_context_window_tokens(model)
             : 0LL;
}

static void cai_model_catalog_seed_contexts(cai_client *client,
                                            cai_model_catalog *catalog) {
  const cai_catalog_policy *policy = cai_client_catalog_policy(client);
  cai_model_catalog_entry *entries;
  size_t i;

  if (policy == NULL || policy->context_count == 0U) {
    return;
  }
  entries = ((cai_model_catalog_impl *)catalog->impl)->entries;
  for (i = 0U; i < catalog->count; i++) {
    if (entries[i].context_window_tokens == 0LL) {
      entries[i].context_window_tokens =
          cai_catalog_seed_context(policy, entries[i].slug);
    }
  }
}

static int cai_model_catalog_merge_fallback(cai_client *client,
                                            cai_model_catalog *catalog,
                                            cai_error *error) {
  cai_client_impl *impl;
  const cai_model_catalog_entry *fallback;
  cai_model_catalog_entry *entries;
  size_t i;

  impl = CAI_CLIENT_IMPL(client);
  entries = ((cai_model_catalog_impl *)catalog->impl)->entries;
  for (i = 0U; i < catalog->count; i++) {
    fallback = cai_model_catalog_find(impl->fallback_catalog, entries[i].slug);
    if (fallback == NULL) {
      continue;
    }
    if (entries[i].context_window_tokens == 0LL) {
      entries[i].context_window_tokens = fallback->context_window_tokens;
    }
    if (entries[i].auto_compact_token_limit == 0LL) {
      entries[i].auto_compact_token_limit = fallback->auto_compact_token_limit;
    }
    entries[i].capabilities |=
        fallback->capabilities & ~entries[i].capabilities_known;
    entries[i].capabilities_known |= fallback->capabilities_known;
#define CAI_FILL_CATALOG_STRING(field)                                         \
  if (entries[i].field == NULL && fallback->field != NULL) {                   \
    entries[i].field = cai_strdup(&impl->allocator, fallback->field);          \
    if (entries[i].field == NULL) {                                            \
      return cai_set_error(error, CAI_ERR_NOMEM,                               \
                           "failed to merge model metadata");                  \
    }                                                                          \
  }
    CAI_FILL_CATALOG_STRING(display_name)
    CAI_FILL_CATALOG_STRING(description)
    CAI_FILL_CATALOG_STRING(model_type)
    CAI_FILL_CATALOG_STRING(lifecycle_status)
    CAI_FILL_CATALOG_STRING(compaction_compatibility_hash)
#undef CAI_FILL_CATALOG_STRING
    if (!entries[i].has_pricing && fallback->has_pricing) {
      entries[i].pricing_currency =
          cai_strdup(&impl->allocator, fallback->pricing_currency);
      entries[i].pricing_unit =
          cai_strdup(&impl->allocator, fallback->pricing_unit);
      if (entries[i].pricing_currency == NULL ||
          entries[i].pricing_unit == NULL) {
        return cai_set_error(error, CAI_ERR_NOMEM,
                             "failed to merge model pricing");
      }
      entries[i].input_price = fallback->input_price;
      entries[i].output_price = fallback->output_price;
      entries[i].has_pricing = 1;
    }
  }
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

static int cai_model_catalog_install_response(cai_client *client,
                                              const char *json,
                                              int provider_format,
                                              cai_error *error) {
  cai_client_impl *impl = CAI_CLIENT_IMPL(client);
  cai_model_catalog *catalog = NULL;
  int rc =
      provider_format
          ? cai_model_catalog_parse_provider(client, json, 0, &catalog, error)
          : cai_model_catalog_parse(client, json, &catalog, error);
  if (rc == CAI_OK && impl->fallback_catalog != NULL) {
    rc = cai_model_catalog_merge_fallback(client, catalog, error);
  }
  if (rc != CAI_OK) {
    cai_model_catalog_close(catalog);
    return rc;
  }
  cai_model_catalog_seed_contexts(client, catalog);
  cai_model_catalog_close(impl->model_catalog);
  impl->model_catalog = catalog;
  impl->model_catalog_fetched_at = time(NULL);
  return CAI_OK;
}

int cai_model_catalog_import_provider_response(cai_client *client,
                                               const char *json,
                                               cai_error *error) {
  if (client == NULL || client->impl == NULL || json == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "client and provider response are required");
  }
  return cai_model_catalog_install_response(client, json, 1, error);
}

int cai_client_list_models(cai_client *client,
                           cai_model_catalog_refresh_strategy strategy,
                           cai_model_catalog **out, cai_error *error) {
  cai_client_impl *impl;
  char *body;
  char *request_id;
  long http_status;
  time_t now;
  int rc;
  const cai_catalog_policy *policy;

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
  policy = cai_client_catalog_policy(client);
  now = time(NULL);
  if (impl->model_catalog != NULL &&
      (strategy == CAI_MODEL_CATALOG_REFRESH_OFFLINE ||
       (strategy == CAI_MODEL_CATALOG_REFRESH_ONLINE_IF_UNCACHED &&
        now >= impl->model_catalog_fetched_at &&
        now - impl->model_catalog_fetched_at <=
            CAI_MODEL_CATALOG_CACHE_SECONDS))) {
    return cai_model_catalog_clone(impl->model_catalog, out, error);
  }
  if (policy == NULL) {
    if (impl->fallback_catalog != NULL) {
      return cai_model_catalog_clone(impl->fallback_catalog, out, error);
    }
    return cai_set_error(error, CAI_ERR_INVALID,
                         "provider has no model catalog; configure model "
                         "metadata JSON");
  }
  if (strategy == CAI_MODEL_CATALOG_REFRESH_OFFLINE) {
    if (impl->fallback_catalog != NULL) {
      return cai_model_catalog_clone(impl->fallback_catalog, out, error);
    }
    return cai_set_error(error, CAI_ERR_INVALID,
                         "no cached provider model catalog is available");
  }
  body = NULL;
  request_id = NULL;
  http_status = 0L;
  rc = cai_http_json_request(client, "GET", policy->request_path, NULL, &body,
                             &http_status, &request_id, error);
  if (rc == CAI_OK && (http_status < 200L || http_status >= 300L)) {
    rc = cai_set_openai_error(error, http_status, body, request_id);
  }
  if (rc == CAI_OK) {
    rc = cai_model_catalog_install_response(client, body,
                                            policy->provider_format, error);
  }
  cai_free_mem(NULL, body);
  cai_free_mem(NULL, request_id);
  if (rc != CAI_OK) {
    if (impl->fallback_catalog != NULL &&
        strategy == CAI_MODEL_CATALOG_REFRESH_ONLINE_IF_UNCACHED) {
      cai_error_cleanup(error);
      cai_error_init(error);
      return cai_model_catalog_clone(impl->fallback_catalog, out, error);
    }
    return rc;
  }
  return cai_model_catalog_clone(impl->model_catalog, out, error);
}
