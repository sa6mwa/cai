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

typedef struct cai_tool_schema_impl {
  char *properties_json;
  char *required_json;
  char *schema_json;
  size_t property_count;
  size_t required_count;
  int strict;
} cai_tool_schema_impl;

#define CAI_TOOL_SCHEMA_IMPL(schema) \
  ((cai_tool_schema_impl *)((schema)->impl))

static void cai_tool_schema_init_methods(cai_tool_schema *schema);

static void cai_tool_entry_cleanup(cai_tool_entry *entry) {
  if (entry == NULL) {
    return;
  }
  cai_free_mem(NULL, entry->name);
  cai_free_mem(NULL, entry->description);
  cai_free_mem(NULL, entry->schema_json);
  memset(entry, 0, sizeof(*entry));
}

static int cai_tool_schema_append_comma(cai_json_builder *builder,
                                        size_t count, cai_error *error) {
  if (count == 0U) {
    return CAI_OK;
  }
  return cai_json_builder_lit(builder, ",", error);
}

static int cai_tool_schema_replace(char **target, cai_json_builder *builder,
                                   cai_error *error) {
  char *copy;

  copy = cai_strdup(NULL, builder->data != NULL ? builder->data : "");
  if (copy == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate schema JSON");
  }
  cai_free_mem(NULL, *target);
  *target = copy;
  return CAI_OK;
}

static int cai_tool_schema_rebuild(cai_tool_schema *schema, cai_error *error) {
  cai_tool_schema_impl *impl;
  cai_json_builder builder;
  int rc;

  impl = CAI_TOOL_SCHEMA_IMPL(schema);
  memset(&builder, 0, sizeof(builder));
  rc = cai_json_builder_lit(&builder, "{\"type\":\"object\",\"properties\":{",
                             error);
  if (rc == CAI_OK && impl->properties_json != NULL) {
    rc = cai_json_builder_lit(&builder, impl->properties_json, error);
  }
  if (rc == CAI_OK) {
    rc = cai_json_builder_lit(&builder, "},\"required\":[", error);
  }
  if (rc == CAI_OK && impl->required_json != NULL) {
    rc = cai_json_builder_lit(&builder, impl->required_json, error);
  }
  if (rc == CAI_OK) {
    rc = cai_json_builder_lit(
        &builder, "],\"additionalProperties\":false}", error);
  }
  if (rc == CAI_OK) {
    rc = cai_tool_schema_replace(&impl->schema_json, &builder, error);
  }
  cai_free_mem(NULL, builder.data);
  return rc;
}

static int cai_tool_schema_add_required(cai_tool_schema *schema,
                                        const char *name, cai_error *error) {
  cai_tool_schema_impl *impl;
  cai_json_builder builder;
  int rc;

  impl = CAI_TOOL_SCHEMA_IMPL(schema);
  memset(&builder, 0, sizeof(builder));
  if (impl->required_json != NULL) {
    rc = cai_json_builder_lit(&builder, impl->required_json, error);
  } else {
    rc = CAI_OK;
  }
  if (rc == CAI_OK) {
    rc = cai_tool_schema_append_comma(&builder, impl->required_count, error);
  }
  if (rc == CAI_OK) {
    rc = cai_json_builder_string(&builder, name, error);
  }
  if (rc == CAI_OK) {
    rc = cai_tool_schema_replace(&impl->required_json, &builder, error);
  }
  if (rc == CAI_OK) {
    impl->required_count++;
  }
  cai_free_mem(NULL, builder.data);
  return rc;
}

static int cai_tool_schema_add_property_json(cai_tool_schema *schema,
                                             const char *name,
                                             const char *property_json,
                                             int required, cai_error *error) {
  cai_tool_schema_impl *impl;
  cai_json_builder builder;
  int rc;

  if (schema == NULL || name == NULL || name[0] == '\0' ||
      property_json == NULL || property_json[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "schema, property name, and property JSON are "
                         "required");
  }
  impl = CAI_TOOL_SCHEMA_IMPL(schema);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "tool schema is closed");
  }
  memset(&builder, 0, sizeof(builder));
  if (impl->properties_json != NULL) {
    rc = cai_json_builder_lit(&builder, impl->properties_json, error);
  } else {
    rc = CAI_OK;
  }
  if (rc == CAI_OK) {
    rc = cai_tool_schema_append_comma(&builder, impl->property_count, error);
  }
  if (rc == CAI_OK) {
    rc = cai_json_builder_string(&builder, name, error);
  }
  if (rc == CAI_OK) {
    rc = cai_json_builder_lit(&builder, ":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_json_builder_lit(&builder, property_json, error);
  }
  if (rc == CAI_OK) {
    rc = cai_tool_schema_replace(&impl->properties_json, &builder, error);
  }
  cai_free_mem(NULL, builder.data);
  if (rc != CAI_OK) {
    return rc;
  }
  impl->property_count++;
  if (required) {
    rc = cai_tool_schema_add_required(schema, name, error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  return cai_tool_schema_rebuild(schema, error);
}

static int cai_tool_schema_add_typed_property(cai_tool_schema *schema,
                                              const char *name,
                                              const char *description,
                                              const char *type, int required,
                                              cai_error *error) {
  cai_json_builder builder;
  int need_comma;
  int rc;

  if (type == NULL || type[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "property type is required");
  }
  memset(&builder, 0, sizeof(builder));
  need_comma = 0;
  rc = cai_json_builder_lit(&builder, "{", error);
  if (rc == CAI_OK) {
    rc = cai_json_builder_field_string(&builder, "type", type, &need_comma,
                                       error);
  }
  if (rc == CAI_OK && description != NULL) {
    rc = cai_json_builder_field_string(&builder, "description", description,
                                       &need_comma, error);
  }
  if (rc == CAI_OK) {
    rc = cai_json_builder_lit(&builder, "}", error);
  }
  if (rc == CAI_OK) {
    rc = cai_tool_schema_add_property_json(
        schema, name, builder.data != NULL ? builder.data : "{}", required,
        error);
  }
  cai_free_mem(NULL, builder.data);
  return rc;
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

static int cai_tool_schema_set_strict_method(cai_tool_schema *schema,
                                             int strict, cai_error *error) {
  return cai_tool_schema_set_strict(schema, strict, error);
}

static void cai_tool_schema_init_methods(cai_tool_schema *schema) {
  schema->set_strict = cai_tool_schema_set_strict_method;
  schema->string = cai_tool_schema_add_string;
  schema->integer = cai_tool_schema_add_integer;
  schema->number = cai_tool_schema_add_number;
  schema->boolean = cai_tool_schema_add_boolean;
  schema->string_enum = cai_tool_schema_add_string_enum;
  schema->raw_property = cai_tool_schema_add_raw_property;
  schema->json = cai_tool_schema_json;
  schema->strict = cai_tool_schema_strict;
  schema->close = cai_tool_schema_destroy;
}

int cai_tool_schema_new(cai_tool_schema **out, cai_error *error) {
  cai_tool_schema *schema;
  cai_tool_schema_impl *impl;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "tool schema output pointer is required");
  }
  *out = NULL;
  schema = (cai_tool_schema *)cai_alloc(NULL, sizeof(*schema));
  if (schema == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate tool schema");
  }
  memset(schema, 0, sizeof(*schema));
  impl = (cai_tool_schema_impl *)cai_alloc(NULL, sizeof(*impl));
  if (impl == NULL) {
    cai_free_mem(NULL, schema);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate tool schema implementation");
  }
  memset(impl, 0, sizeof(*impl));
  impl->strict = 1;
  schema->impl = impl;
  cai_tool_schema_init_methods(schema);
  rc = cai_tool_schema_rebuild(schema, error);
  if (rc != CAI_OK) {
    cai_tool_schema_destroy(schema);
    return rc;
  }
  *out = schema;
  return CAI_OK;
}

void cai_tool_schema_destroy(cai_tool_schema *schema) {
  cai_tool_schema_impl *impl;

  if (schema == NULL) {
    return;
  }
  impl = CAI_TOOL_SCHEMA_IMPL(schema);
  if (impl != NULL) {
    cai_free_mem(NULL, impl->properties_json);
    cai_free_mem(NULL, impl->required_json);
    cai_free_mem(NULL, impl->schema_json);
    cai_free_mem(NULL, impl);
  }
  schema->impl = NULL;
  cai_free_mem(NULL, schema);
}

int cai_tool_schema_set_strict(cai_tool_schema *schema, int strict,
                               cai_error *error) {
  cai_tool_schema_impl *impl;

  if (schema == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "tool schema is required");
  }
  impl = CAI_TOOL_SCHEMA_IMPL(schema);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "tool schema is closed");
  }
  impl->strict = strict ? 1 : 0;
  return CAI_OK;
}

int cai_tool_schema_add_string(cai_tool_schema *schema, const char *name,
                               const char *description, int required,
                               cai_error *error) {
  return cai_tool_schema_add_typed_property(schema, name, description, "string",
                                            required, error);
}

int cai_tool_schema_add_integer(cai_tool_schema *schema, const char *name,
                                const char *description, int required,
                                cai_error *error) {
  return cai_tool_schema_add_typed_property(
      schema, name, description, "integer", required, error);
}

int cai_tool_schema_add_number(cai_tool_schema *schema, const char *name,
                               const char *description, int required,
                               cai_error *error) {
  return cai_tool_schema_add_typed_property(schema, name, description, "number",
                                            required, error);
}

int cai_tool_schema_add_boolean(cai_tool_schema *schema, const char *name,
                                const char *description, int required,
                                cai_error *error) {
  return cai_tool_schema_add_typed_property(
      schema, name, description, "boolean", required, error);
}

int cai_tool_schema_add_string_enum(cai_tool_schema *schema, const char *name,
                                    const char *description,
                                    const char *const *values,
                                    size_t value_count, int required,
                                    cai_error *error) {
  cai_json_builder builder;
  int need_comma;
  size_t i;
  int rc;

  if (values == NULL || value_count == 0U) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "string enum values are required");
  }
  memset(&builder, 0, sizeof(builder));
  need_comma = 0;
  rc = cai_json_builder_lit(&builder, "{", error);
  if (rc == CAI_OK) {
    rc = cai_json_builder_field_string(&builder, "type", "string",
                                       &need_comma, error);
  }
  if (rc == CAI_OK && description != NULL) {
    rc = cai_json_builder_field_string(&builder, "description", description,
                                       &need_comma, error);
  }
  if (rc == CAI_OK) {
    rc = cai_json_builder_lit(&builder, ",\"enum\":[", error);
  }
  for (i = 0U; rc == CAI_OK && i < value_count; i++) {
    if (i > 0U) {
      rc = cai_json_builder_lit(&builder, ",", error);
    }
    if (rc == CAI_OK) {
      rc = cai_json_builder_string(&builder, values[i], error);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_json_builder_lit(&builder, "]}", error);
  }
  if (rc == CAI_OK) {
    rc = cai_tool_schema_add_property_json(
        schema, name, builder.data != NULL ? builder.data : "{}", required,
        error);
  }
  cai_free_mem(NULL, builder.data);
  return rc;
}

int cai_tool_schema_add_raw_property(cai_tool_schema *schema, const char *name,
                                     const char *description,
                                     const char *schema_json, int required,
                                     cai_error *error) {
  cai_json_builder builder;
  int rc;

  if (schema_json == NULL || schema_json[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "raw property schema JSON is required");
  }
  if (description == NULL) {
    return cai_tool_schema_add_property_json(schema, name, schema_json,
                                             required, error);
  }
  if (schema_json[0] != '{') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "raw property schema with description must be a JSON "
                         "object");
  }
  memset(&builder, 0, sizeof(builder));
  rc = cai_json_builder_lit(&builder, "{", error);
  if (rc == CAI_OK) {
    rc = cai_json_builder_lit(&builder, schema_json + 1, error);
  }
  if (rc == CAI_OK) {
    if (builder.length > 1U && builder.data[builder.length - 1U] == '}') {
      builder.length--;
      builder.data[builder.length] = '\0';
    }
    if (builder.length > 1U) {
      rc = cai_json_builder_lit(&builder, ",", error);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_json_builder_lit(&builder, "\"description\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_json_builder_string(&builder, description, error);
  }
  if (rc == CAI_OK) {
    rc = cai_json_builder_lit(&builder, "}", error);
  }
  if (rc == CAI_OK) {
    rc = cai_tool_schema_add_property_json(
        schema, name, builder.data != NULL ? builder.data : schema_json,
        required, error);
  }
  cai_free_mem(NULL, builder.data);
  return rc;
}

const char *cai_tool_schema_json(const cai_tool_schema *schema) {
  const cai_tool_schema_impl *impl;

  if (schema == NULL) {
    return NULL;
  }
  impl = CAI_TOOL_SCHEMA_IMPL(schema);
  return impl != NULL ? impl->schema_json : NULL;
}

int cai_tool_schema_strict(const cai_tool_schema *schema) {
  const cai_tool_schema_impl *impl;

  if (schema == NULL) {
    return 0;
  }
  impl = CAI_TOOL_SCHEMA_IMPL(schema);
  return impl != NULL ? impl->strict : 0;
}
