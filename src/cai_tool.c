#include "cai_internal.h"

#include <stdio.h>
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
  const lonejson_map *params_map;
  const lonejson_map *result_map;
  cai_tool_fn lonejson_callback;
  cai_tool_raw_fn raw_callback;
  void *context;
} cai_tool_entry;

struct cai_tool_registry {
  cai_tool_entry *entries;
  size_t count;
  size_t capacity;
};

typedef struct cai_tool_schema_property {
  char *name;
  char *property_json;
  int required;
} cai_tool_schema_property;

typedef struct cai_tool_schema_impl {
  cai_tool_schema_property *properties;
  size_t property_count;
  size_t property_capacity;
  char *schema_json;
  int strict;
} cai_tool_schema_impl;

#define CAI_TOOL_SCHEMA_IMPL(schema) \
  ((cai_tool_schema_impl *)((schema)->impl))

static void cai_tool_schema_init_methods(cai_tool_schema *schema);

static lonejson_status cai_lonejson_sink_write(void *user, const void *data,
                                               size_t len,
                                               lonejson_error *json_error) {
  cai_sink *sink;
  cai_error error;

  sink = (cai_sink *)user;
  cai_error_init(&error);
  if (cai_sink_write(sink, data, len, &error) == CAI_OK) {
    cai_error_cleanup(&error);
    return LONEJSON_STATUS_OK;
  }
  if (json_error != NULL) {
    snprintf(json_error->message, sizeof(json_error->message), "%s",
             error.message != NULL ? error.message : "sink write failed");
  }
  cai_error_cleanup(&error);
  return LONEJSON_STATUS_CALLBACK_FAILED;
}

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

static void cai_tool_schema_property_cleanup(cai_tool_schema_property *prop) {
  if (prop == NULL) {
    return;
  }
  cai_free_mem(NULL, prop->name);
  cai_free_mem(NULL, prop->property_json);
  memset(prop, 0, sizeof(*prop));
}

static cai_tool_schema_property *
cai_tool_schema_find_property(cai_tool_schema_impl *impl, const char *name) {
  size_t i;

  if (impl == NULL || name == NULL) {
    return NULL;
  }
  for (i = 0U; i < impl->property_count; i++) {
    if (strcmp(impl->properties[i].name, name) == 0) {
      return &impl->properties[i];
    }
  }
  return NULL;
}

static int cai_tool_schema_grow(cai_tool_schema_impl *impl,
                                cai_error *error) {
  size_t new_capacity;
  void *grown;

  if (impl->property_count < impl->property_capacity) {
    return CAI_OK;
  }
  new_capacity =
      impl->property_capacity == 0U ? 8U : impl->property_capacity * 2U;
  grown = cai_realloc_mem(NULL, impl->properties,
                          new_capacity * sizeof(impl->properties[0]));
  if (grown == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to grow tool schema properties");
  }
  impl->properties = (cai_tool_schema_property *)grown;
  impl->property_capacity = new_capacity;
  return CAI_OK;
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
  size_t i;
  size_t required_count;
  int rc;

  impl = CAI_TOOL_SCHEMA_IMPL(schema);
  memset(&builder, 0, sizeof(builder));
  rc = cai_json_builder_lit(&builder, "{\"type\":\"object\",\"properties\":{",
                             error);
  for (i = 0U; rc == CAI_OK && i < impl->property_count; i++) {
    rc = cai_tool_schema_append_comma(&builder, i, error);
    if (rc == CAI_OK) {
      rc = cai_json_builder_string(&builder, impl->properties[i].name, error);
    }
    if (rc == CAI_OK) {
      rc = cai_json_builder_lit(&builder, ":", error);
    }
    if (rc == CAI_OK) {
      rc = cai_json_builder_lit(&builder, impl->properties[i].property_json,
                                error);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_json_builder_lit(&builder, "},\"required\":[", error);
  }
  for (i = 0U, required_count = 0U; rc == CAI_OK && i < impl->property_count;
       i++) {
    if (!impl->properties[i].required) {
      continue;
    }
    rc = cai_tool_schema_append_comma(&builder, required_count, error);
    if (rc == CAI_OK) {
      rc = cai_json_builder_string(&builder, impl->properties[i].name, error);
    }
    if (rc == CAI_OK) {
      required_count++;
    }
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

static int cai_tool_schema_add_property_json(cai_tool_schema *schema,
                                             const char *name,
                                             const char *property_json,
                                             int required, cai_error *error) {
  cai_tool_schema_impl *impl;
  cai_tool_schema_property *prop;
  char *property_copy;
  char *name_copy;
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
  prop = cai_tool_schema_find_property(impl, name);
  property_copy = cai_strdup(NULL, property_json);
  if (property_copy == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate tool schema property");
  }
  if (prop != NULL) {
    cai_free_mem(NULL, prop->property_json);
    prop->property_json = property_copy;
    prop->required = required ? 1 : 0;
  } else {
    rc = cai_tool_schema_grow(impl, error);
    if (rc != CAI_OK) {
      cai_free_mem(NULL, property_copy);
      return rc;
    }
    name_copy = cai_strdup(NULL, name);
    if (name_copy == NULL) {
      cai_free_mem(NULL, property_copy);
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate tool schema property");
    }
    prop = &impl->properties[impl->property_count];
    memset(prop, 0, sizeof(*prop));
    prop->name = name_copy;
    prop->property_json = property_copy;
    prop->required = required ? 1 : 0;
    impl->property_count++;
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

static const char *cai_tool_json_type_for_field(const lonejson_field *field) {
  switch (field->kind) {
  case LONEJSON_FIELD_KIND_STRING:
  case LONEJSON_FIELD_KIND_STRING_STREAM:
  case LONEJSON_FIELD_KIND_BASE64_STREAM:
  case LONEJSON_FIELD_KIND_STRING_SOURCE:
  case LONEJSON_FIELD_KIND_BASE64_SOURCE:
    return "string";
  case LONEJSON_FIELD_KIND_I64:
  case LONEJSON_FIELD_KIND_U64:
    return "integer";
  case LONEJSON_FIELD_KIND_F64:
    return "number";
  case LONEJSON_FIELD_KIND_BOOL:
    return "boolean";
  default:
    return NULL;
  }
}

static int cai_tool_schema_add_field(cai_tool_schema *schema,
                                     const lonejson_field *field,
                                     cai_error *error) {
  cai_json_builder builder;
  const char *type;
  int required;
  int rc;

  required = (field->flags & LONEJSON_FIELD_REQUIRED) != 0U;
  type = cai_tool_json_type_for_field(field);
  if (type != NULL) {
    return cai_tool_schema_add_typed_property(
        schema, field->json_key, NULL, type, required, error);
  }
  if (field->kind == LONEJSON_FIELD_KIND_OBJECT) {
    cai_tool_schema *nested;

    nested = NULL;
    rc = cai_tool_schema_from_map(field->submap, &nested, error);
    if (rc == CAI_OK) {
      rc = cai_tool_schema_add_property_json(schema, field->json_key,
                                             cai_tool_schema_json(nested),
                                             required, error);
    }
    cai_tool_schema_destroy(nested);
    return rc;
  }
  memset(&builder, 0, sizeof(builder));
  switch (field->kind) {
  case LONEJSON_FIELD_KIND_STRING_ARRAY:
    rc = cai_json_builder_lit(
        &builder, "{\"type\":\"array\",\"items\":{\"type\":\"string\"}}",
        error);
    break;
  case LONEJSON_FIELD_KIND_I64_ARRAY:
  case LONEJSON_FIELD_KIND_U64_ARRAY:
    rc = cai_json_builder_lit(
        &builder, "{\"type\":\"array\",\"items\":{\"type\":\"integer\"}}",
        error);
    break;
  case LONEJSON_FIELD_KIND_F64_ARRAY:
    rc = cai_json_builder_lit(
        &builder, "{\"type\":\"array\",\"items\":{\"type\":\"number\"}}",
        error);
    break;
  case LONEJSON_FIELD_KIND_BOOL_ARRAY:
    rc = cai_json_builder_lit(
        &builder, "{\"type\":\"array\",\"items\":{\"type\":\"boolean\"}}",
        error);
    break;
  case LONEJSON_FIELD_KIND_OBJECT_ARRAY: {
    cai_tool_schema *nested;

    nested = NULL;
    rc = cai_tool_schema_from_map(field->submap, &nested, error);
    if (rc == CAI_OK) {
      rc = cai_json_builder_lit(&builder, "{\"type\":\"array\",\"items\":",
                                 error);
    }
    if (rc == CAI_OK) {
      rc = cai_json_builder_lit(&builder, cai_tool_schema_json(nested), error);
    }
    if (rc == CAI_OK) {
      rc = cai_json_builder_lit(&builder, "}", error);
    }
    cai_tool_schema_destroy(nested);
    break;
  }
  case LONEJSON_FIELD_KIND_JSON_VALUE:
    rc = cai_json_builder_lit(&builder, "{}", error);
    break;
  default:
    return cai_set_error(error, CAI_ERR_INVALID,
                         "unsupported lonejson field kind for tool schema");
  }
  if (rc == CAI_OK) {
    rc = cai_tool_schema_add_property_json(
        schema, field->json_key, builder.data != NULL ? builder.data : "{}",
        required, error);
  }
  cai_free_mem(NULL, builder.data);
  return rc;
}

static void cai_tool_result_cleanup_plain(const lonejson_map *map,
                                          void *value) {
  size_t i;

  if (map == NULL || value == NULL) {
    return;
  }
  for (i = 0U; i < map->field_count; i++) {
    const lonejson_field *field;
    void *ptr;

    field = &map->fields[i];
    ptr = (unsigned char *)value + field->struct_offset;
    switch (field->kind) {
    case LONEJSON_FIELD_KIND_STRING:
      if (field->storage == LONEJSON_STORAGE_DYNAMIC) {
        char **p;

        p = (char **)ptr;
        cai_free_mem(NULL, *p);
        *p = NULL;
      }
      break;
    case LONEJSON_FIELD_KIND_OBJECT:
      cai_tool_result_cleanup_plain(field->submap, ptr);
      break;
    default:
      break;
    }
  }
}

char *cai_tool_result_strdup(const char *value, cai_error *error) {
  char *copy;

  copy = cai_strdup(NULL, value);
  if (value != NULL && copy == NULL) {
    (void)cai_set_error(error, CAI_ERR_NOMEM,
                        "failed to allocate tool result string");
  }
  return copy;
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
    const lonejson_map *params_map, const lonejson_map *result_map,
    cai_tool_fn lonejson_callback, cai_tool_raw_fn raw_callback, void *context,
    cai_error *error) {
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
  if (kind == CAI_TOOL_LONEJSON &&
      (params_map == NULL || result_map == NULL || lonejson_callback == NULL)) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "lonejson tool parameter map, result map, and "
                         "callback are required");
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
  entry->params_map = params_map;
  entry->result_map = result_map;
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
    const lonejson_map *params_map, const lonejson_map *result_map,
    cai_tool_fn callback, void *context, cai_error *error) {
  cai_tool_schema *schema;
  int rc;

  schema = NULL;
  rc = cai_tool_schema_from_map(params_map, &schema, error);
  if (rc == CAI_OK) {
    rc = cai_tool_registry_register_common(
        registry, name, description, cai_tool_schema_json(schema),
        cai_tool_schema_strict(schema), CAI_TOOL_LONEJSON, params_map,
        result_map, callback, NULL, context, error);
  }
  cai_tool_schema_destroy(schema);
  return rc;
}

int cai_tool_registry_register_raw(cai_tool_registry *registry,
                                   const char *name, const char *description,
                                   const char *schema_json, int strict,
                                   cai_tool_raw_fn callback, void *context,
                                   cai_error *error) {
  return cai_tool_registry_register_common(
      registry, name, description, schema_json, strict, CAI_TOOL_RAW, NULL,
      NULL, NULL, callback, context, error);
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
  void *result;
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
  params = cai_alloc(NULL, entry->params_map->struct_size);
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate tool parameters");
  }
  result = cai_alloc(NULL, entry->result_map->struct_size);
  if (result == NULL) {
    cai_free_mem(NULL, params);
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate tool result");
  }
  lonejson_init(entry->params_map, params);
  lonejson_init(entry->result_map, result);
  status = lonejson_parse_cstr(entry->params_map, params, arguments_json, NULL,
                               &json_error);
  if (status != LONEJSON_STATUS_OK) {
    lonejson_cleanup(entry->params_map, params);
    lonejson_cleanup(entry->result_map, result);
    cai_free_mem(NULL, result);
    cai_free_mem(NULL, params);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to parse tool arguments",
                                json_error.message);
  }
  rc = entry->lonejson_callback(entry->context, params, result, error);
  if (rc == CAI_OK) {
    lonejson_error_init(&json_error);
    status = lonejson_serialize_sink(entry->result_map, result,
                                     cai_lonejson_sink_write, output, NULL,
                                     &json_error);
    if (status != LONEJSON_STATUS_OK) {
      rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to serialize tool result",
                                json_error.message);
    }
  }
  lonejson_cleanup(entry->params_map, params);
  cai_tool_result_cleanup_plain(entry->result_map, result);
  cai_free_mem(NULL, result);
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
  schema->describe = cai_tool_schema_describe;
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

int cai_tool_schema_from_map(const lonejson_map *map, cai_tool_schema **out,
                             cai_error *error) {
  cai_tool_schema *schema;
  size_t i;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "tool schema output pointer is required");
  }
  *out = NULL;
  if (map == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "lonejson map is required");
  }
  schema = NULL;
  rc = cai_tool_schema_new(&schema, error);
  for (i = 0U; rc == CAI_OK && i < map->field_count; i++) {
    rc = cai_tool_schema_add_field(schema, &map->fields[i], error);
  }
  if (rc != CAI_OK) {
    cai_tool_schema_destroy(schema);
    return rc;
  }
  *out = schema;
  return CAI_OK;
}

void cai_tool_schema_destroy(cai_tool_schema *schema) {
  cai_tool_schema_impl *impl;
  size_t i;

  if (schema == NULL) {
    return;
  }
  impl = CAI_TOOL_SCHEMA_IMPL(schema);
  if (impl != NULL) {
    for (i = 0U; i < impl->property_count; i++) {
      cai_tool_schema_property_cleanup(&impl->properties[i]);
    }
    cai_free_mem(NULL, impl->properties);
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

int cai_tool_schema_describe(cai_tool_schema *schema, const char *name,
                             const char *description, cai_error *error) {
  cai_tool_schema_impl *impl;
  cai_tool_schema_property *prop;
  cai_json_builder builder;
  int required;
  int rc;

  if (schema == NULL || name == NULL || name[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "tool schema and property name are required");
  }
  if (description == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "description is required");
  }
  impl = CAI_TOOL_SCHEMA_IMPL(schema);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "tool schema is closed");
  }
  prop = cai_tool_schema_find_property(impl, name);
  if (prop == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "tool schema property is not registered");
  }
  if (prop->property_json == NULL || prop->property_json[0] != '{') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "tool schema property must be a JSON object");
  }
  memset(&builder, 0, sizeof(builder));
  required = prop->required;
  rc = cai_json_builder_lit(&builder, "{", error);
  if (rc == CAI_OK) {
    rc = cai_json_builder_lit(&builder, prop->property_json + 1, error);
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
        schema, name, builder.data != NULL ? builder.data : prop->property_json,
        required, error);
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
