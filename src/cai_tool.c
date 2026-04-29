#include "cai_internal.h"

#include <stdlib.h>
#include <string.h>

typedef enum cai_tool_kind {
  CAI_TOOL_LONEJSON = 1,
  CAI_TOOL_RAW = 2
} cai_tool_kind;

typedef struct cai_tool_entry {
  char *name;
  char *description;
  char *schema_json;
  int strict;
  cai_tool_kind kind;
  const lonejson_map *map;
  cai_tool_lonejson_fn lonejson_callback;
  cai_tool_raw_fn raw_callback;
  void *context;
} cai_tool_entry;

struct cai_tool_registry {
  cai_tool_entry *entries;
  size_t count;
  size_t capacity;
};

static void cai_tool_entry_cleanup(cai_tool_entry *entry) {
  if (entry == NULL) {
    return;
  }
  cai_free_mem(NULL, entry->name);
  cai_free_mem(NULL, entry->description);
  cai_free_mem(NULL, entry->schema_json);
  memset(entry, 0, sizeof(*entry));
}

static cai_tool_entry *cai_tool_registry_find(cai_tool_registry *registry,
                                              const char *name) {
  size_t i;

  if (registry == NULL || name == NULL) {
    return NULL;
  }
  for (i = 0U; i < registry->count; i++) {
    if (strcmp(registry->entries[i].name, name) == 0) {
      return &registry->entries[i];
    }
  }
  return NULL;
}

static int cai_tool_registry_grow(cai_tool_registry *registry,
                                  cai_error *error) {
  size_t new_capacity;
  void *grown;

  if (registry->count < registry->capacity) {
    return CAI_OK;
  }
  new_capacity = registry->capacity == 0U ? 4U : registry->capacity * 2U;
  grown = cai_realloc_mem(NULL, registry->entries,
                          new_capacity * sizeof(registry->entries[0]));
  if (grown == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to grow tool registry");
  }
  registry->entries = (cai_tool_entry *)grown;
  registry->capacity = new_capacity;
  return CAI_OK;
}

static int cai_tool_registry_register_common(
    cai_tool_registry *registry, const char *name, const char *description,
    const char *schema_json, int strict, cai_tool_kind kind,
    const lonejson_map *map, cai_tool_lonejson_fn lonejson_callback,
    cai_tool_raw_fn raw_callback, void *context, cai_error *error) {
  cai_tool_entry *entry;
  int rc;

  if (registry == NULL || name == NULL || name[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "tool registry and name are required");
  }
  if (schema_json == NULL || schema_json[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "tool parameter schema JSON is required");
  }
  if (cai_tool_registry_find(registry, name) != NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "tool is already registered");
  }
  if (kind == CAI_TOOL_LONEJSON && (map == NULL || lonejson_callback == NULL)) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "lonejson tool map and callback are required");
  }
  if (kind == CAI_TOOL_RAW && raw_callback == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "raw tool callback is required");
  }
  rc = cai_tool_registry_grow(registry, error);
  if (rc != CAI_OK) {
    return rc;
  }
  entry = &registry->entries[registry->count];
  memset(entry, 0, sizeof(*entry));
  entry->name = cai_strdup(NULL, name);
  entry->description = cai_strdup(NULL, description);
  entry->schema_json = cai_strdup(NULL, schema_json);
  entry->strict = strict ? 1 : 0;
  entry->kind = kind;
  entry->map = map;
  entry->lonejson_callback = lonejson_callback;
  entry->raw_callback = raw_callback;
  entry->context = context;
  if (entry->name == NULL ||
      (description != NULL && entry->description == NULL) ||
      entry->schema_json == NULL) {
    cai_tool_entry_cleanup(entry);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate tool registration");
  }
  registry->count++;
  return CAI_OK;
}

int cai_tool_registry_new(cai_tool_registry **out, cai_error *error) {
  cai_tool_registry *registry;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "tool registry output pointer is required");
  }
  *out = NULL;
  registry = (cai_tool_registry *)cai_alloc(NULL, sizeof(*registry));
  if (registry == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate tool registry");
  }
  registry->entries = NULL;
  registry->count = 0U;
  registry->capacity = 0U;
  *out = registry;
  return CAI_OK;
}

void cai_tool_registry_destroy(cai_tool_registry *registry) {
  size_t i;

  if (registry == NULL) {
    return;
  }
  for (i = 0U; i < registry->count; i++) {
    cai_tool_entry_cleanup(&registry->entries[i]);
  }
  cai_free_mem(NULL, registry->entries);
  cai_free_mem(NULL, registry);
}

int cai_tool_registry_register_lonejson(
    cai_tool_registry *registry, const char *name, const char *description,
    const lonejson_map *map, const char *schema_json, int strict,
    cai_tool_lonejson_fn callback, void *context, cai_error *error) {
  return cai_tool_registry_register_common(
      registry, name, description, schema_json, strict, CAI_TOOL_LONEJSON, map,
      callback, NULL, context, error);
}

int cai_tool_registry_register_raw(cai_tool_registry *registry,
                                   const char *name, const char *description,
                                   const char *schema_json, int strict,
                                   cai_tool_raw_fn callback, void *context,
                                   cai_error *error) {
  return cai_tool_registry_register_common(
      registry, name, description, schema_json, strict, CAI_TOOL_RAW, NULL,
      NULL, callback, context, error);
}

int cai_tool_registry_add_to_response_params(const cai_tool_registry *registry,
                                             cai_response_create_params *params,
                                             cai_error *error) {
  size_t i;
  int rc;

  if (registry == NULL || params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "tool registry and response params are required");
  }
  for (i = 0U; i < registry->count; i++) {
    rc = cai_response_create_params_add_function_tool(
        params, registry->entries[i].name, registry->entries[i].description,
        registry->entries[i].schema_json, registry->entries[i].strict, error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  return CAI_OK;
}

int cai_tool_registry_run(cai_tool_registry *registry, const char *name,
                          const char *arguments_json, cai_sink *output,
                          cai_error *error) {
  cai_tool_entry *entry;
  lonejson_error json_error;
  lonejson_status status;
  void *params;
  int rc;

  if (name == NULL || name[0] == '\0' || arguments_json == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "tool name and arguments JSON are required");
  }
  entry = cai_tool_registry_find(registry, name);
  if (entry == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "tool is not registered");
  }
  if (entry->kind == CAI_TOOL_RAW) {
    return entry->raw_callback(entry->context, arguments_json, output, error);
  }
  params = cai_alloc(NULL, entry->map->struct_size);
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate tool parameters");
  }
  lonejson_init(entry->map, params);
  status = lonejson_parse_cstr(entry->map, params, arguments_json, NULL,
                               &json_error);
  if (status != LONEJSON_STATUS_OK) {
    lonejson_cleanup(entry->map, params);
    cai_free_mem(NULL, params);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to parse tool arguments",
                                json_error.message);
  }
  rc = entry->lonejson_callback(entry->context, params, output, error);
  lonejson_cleanup(entry->map, params);
  cai_free_mem(NULL, params);
  return rc;
}
