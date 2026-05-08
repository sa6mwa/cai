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
  void (*context_cleanup)(void *context);
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

typedef enum cai_tool_json_container_kind {
  CAI_TOOL_JSON_OBJECT = 1,
  CAI_TOOL_JSON_ARRAY = 2
} cai_tool_json_container_kind;

typedef struct cai_tool_argument_frame {
  cai_tool_json_container_kind kind;
  const lonejson_map *map;
  const lonejson_map *array_item_map;
  const lonejson_field *pending_field;
} cai_tool_argument_frame;

typedef struct cai_tool_argument_validator {
  const lonejson_map *root_map;
  cai_tool_argument_frame frames[64];
  size_t depth;
  char *key;
  size_t key_length;
  size_t key_capacity;
  int collecting_key;
} cai_tool_argument_validator;

typedef struct cai_tool_typed_property_doc {
  const char *type;
  const char *description;
} cai_tool_typed_property_doc;

typedef struct cai_searxng_context {
  char *base_url;
  char *search_path;
  char *engine;
  char *language;
  long timeout_ms;
  size_t response_memory_limit;
  size_t response_max_bytes;
  char *response_spool_dir;
} cai_searxng_context;

typedef struct cai_searxng_args {
  char *query;
} cai_searxng_args;

typedef struct cai_searxng_result {
  char *query;
  char *engine;
  char *title;
  char *url;
  char *snippet;
  char *source;
  long long result_count;
  long long infobox_count;
} cai_searxng_result;

typedef struct cai_searxng_item_doc {
  char *url;
  char *title;
  char *content;
  char *engine;
  char *infobox;
  char *id;
} cai_searxng_item_doc;

typedef struct cai_searxng_response_doc {
  char *query;
  long long number_of_results;
  int has_number_of_results;
  lonejson_object_array results;
  lonejson_object_array infoboxes;
} cai_searxng_response_doc;

typedef struct cai_searxng_spool_reader {
  lonejson_spooled cursor;
} cai_searxng_spool_reader;

#define CAI_TOOL_SCHEMA_IMPL(schema) ((cai_tool_schema_impl *)((schema)->impl))

static void cai_tool_schema_init_methods(cai_tool_schema *schema);

static const lonejson_field cai_tool_typed_property_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_tool_typed_property_doc, type, "type"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_tool_typed_property_doc,
                                          description, "description")};
LONEJSON_MAP_DEFINE(cai_tool_typed_property_map, cai_tool_typed_property_doc,
                    cai_tool_typed_property_fields);

static const lonejson_field cai_searxng_arg_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_searxng_args, query, "query")};
LONEJSON_MAP_DEFINE(cai_searxng_args_map, cai_searxng_args,
                    cai_searxng_arg_fields);

static const lonejson_field cai_searxng_result_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_searxng_result, query, "query"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_searxng_result, engine, "engine"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_searxng_result, title, "title"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_searxng_result, url, "url"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_searxng_result, snippet, "snippet"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_searxng_result, source, "source"),
    LONEJSON_FIELD_I64_REQ(cai_searxng_result, result_count, "result_count"),
    LONEJSON_FIELD_I64_REQ(cai_searxng_result, infobox_count, "infobox_count")};
LONEJSON_MAP_DEFINE(cai_searxng_result_map, cai_searxng_result,
                    cai_searxng_result_fields);

static const lonejson_field cai_searxng_item_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_searxng_item_doc, url, "url"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_searxng_item_doc, title, "title"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_searxng_item_doc, content,
                                          "content"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_searxng_item_doc, engine,
                                          "engine"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_searxng_item_doc, infobox,
                                          "infobox"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_searxng_item_doc, id, "id")};
LONEJSON_MAP_DEFINE(cai_searxng_item_map, cai_searxng_item_doc,
                    cai_searxng_item_fields);

static const lonejson_field cai_searxng_response_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_searxng_response_doc, query,
                                          "query"),
    LONEJSON_FIELD_I64_PRESENT(cai_searxng_response_doc, number_of_results,
                               has_number_of_results, "number_of_results"),
    LONEJSON_FIELD_OBJECT_ARRAY(cai_searxng_response_doc, results, "results",
                                cai_searxng_item_doc, &cai_searxng_item_map,
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_OBJECT_ARRAY(cai_searxng_response_doc, infoboxes,
                                "infoboxes", cai_searxng_item_doc,
                                &cai_searxng_item_map, LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(cai_searxng_response_map, cai_searxng_response_doc,
                    cai_searxng_response_fields);

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
  if (entry->context_cleanup != NULL) {
    entry->context_cleanup(entry->context);
  }
  memset(entry, 0, sizeof(*entry));
}

static int cai_tool_schema_append_comma(cai_json_builder *builder, size_t count,
                                        cai_error *error) {
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

static int cai_tool_schema_grow(cai_tool_schema_impl *impl, cai_error *error) {
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
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate schema JSON");
  }
  cai_free_mem(NULL, *target);
  *target = copy;
  return CAI_OK;
}

static int cai_tool_validate_json_value(const char *json, const char *message,
                                        cai_error *error) {
  lonejson_json_value value;
  lonejson_error json_error;
  int rc;

  if (json == NULL || json[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, message);
  }
  lonejson_json_value_init(&value);
  lonejson_error_init(&json_error);
  if (lonejson_json_value_set_buffer(&value, json, strlen(json), &json_error) !=
      LONEJSON_STATUS_OK) {
    rc = cai_set_error_detail(error, CAI_ERR_INVALID, message,
                              json_error.message);
  } else {
    rc = CAI_OK;
  }
  lonejson_json_value_cleanup(&value);
  return rc;
}

static lonejson_status cai_tool_compact_json_sink(void *user, const void *data,
                                                  size_t len,
                                                  lonejson_error *json_error) {
  cai_json_builder *builder;
  cai_error error;

  builder = (cai_json_builder *)user;
  cai_error_init(&error);
  if (cai_json_builder_append(builder, (const char *)data, len, &error) ==
      CAI_OK) {
    cai_error_cleanup(&error);
    return LONEJSON_STATUS_OK;
  }
  if (json_error != NULL) {
    snprintf(json_error->message, sizeof(json_error->message), "%s",
             error.message != NULL ? error.message : "JSON compact failed");
  }
  cai_error_cleanup(&error);
  return LONEJSON_STATUS_CALLBACK_FAILED;
}

static const lonejson_field *cai_tool_map_find_field(const lonejson_map *map,
                                                     const char *key,
                                                     size_t key_length) {
  size_t i;

  if (map == NULL || key == NULL) {
    return NULL;
  }
  for (i = 0U; i < map->field_count; i++) {
    if (map->fields[i].json_key_len == key_length &&
        memcmp(map->fields[i].json_key, key, key_length) == 0) {
      return &map->fields[i];
    }
  }
  return NULL;
}

static int cai_tool_field_is_scalar(const lonejson_field *field) {
  if (field == NULL) {
    return 0;
  }
  switch (field->kind) {
  case LONEJSON_FIELD_KIND_STRING:
  case LONEJSON_FIELD_KIND_STRING_STREAM:
  case LONEJSON_FIELD_KIND_BASE64_STREAM:
  case LONEJSON_FIELD_KIND_STRING_SOURCE:
  case LONEJSON_FIELD_KIND_BASE64_SOURCE:
  case LONEJSON_FIELD_KIND_JSON_VALUE:
  case LONEJSON_FIELD_KIND_I64:
  case LONEJSON_FIELD_KIND_U64:
  case LONEJSON_FIELD_KIND_F64:
  case LONEJSON_FIELD_KIND_BOOL:
    return 1;
  default:
    return 0;
  }
}

static lonejson_status cai_tool_argument_validator_error(lonejson_error *error,
                                                         const char *message) {
  if (error != NULL) {
    snprintf(error->message, sizeof(error->message), "%s", message);
  }
  return LONEJSON_STATUS_CALLBACK_FAILED;
}

static cai_tool_argument_frame *
cai_tool_argument_validator_current(cai_tool_argument_validator *validator) {
  if (validator == NULL || validator->depth == 0U) {
    return NULL;
  }
  return &validator->frames[validator->depth - 1U];
}

static void cai_tool_argument_validator_consume_scalar(
    cai_tool_argument_validator *validator) {
  cai_tool_argument_frame *frame;

  frame = cai_tool_argument_validator_current(validator);
  if (frame != NULL && frame->kind == CAI_TOOL_JSON_OBJECT &&
      cai_tool_field_is_scalar(frame->pending_field)) {
    frame->pending_field = NULL;
  }
}

static lonejson_status cai_tool_argument_object_begin(void *user,
                                                      lonejson_error *error) {
  cai_tool_argument_validator *validator;
  cai_tool_argument_frame *parent;
  cai_tool_argument_frame *frame;
  const lonejson_map *map;

  validator = (cai_tool_argument_validator *)user;
  if (validator->depth >=
      sizeof(validator->frames) / sizeof(validator->frames[0])) {
    return cai_tool_argument_validator_error(error,
                                             "tool arguments are too deep");
  }
  map = NULL;
  if (validator->depth == 0U) {
    map = validator->root_map;
  } else {
    parent = cai_tool_argument_validator_current(validator);
    if (parent != NULL && parent->kind == CAI_TOOL_JSON_OBJECT &&
        parent->pending_field != NULL &&
        parent->pending_field->kind == LONEJSON_FIELD_KIND_OBJECT) {
      map = parent->pending_field->submap;
      parent->pending_field = NULL;
    } else if (parent != NULL && parent->kind == CAI_TOOL_JSON_ARRAY) {
      map = parent->array_item_map;
    }
  }
  frame = &validator->frames[validator->depth];
  memset(frame, 0, sizeof(*frame));
  frame->kind = CAI_TOOL_JSON_OBJECT;
  frame->map = map;
  validator->depth++;
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_tool_argument_object_end(void *user,
                                                    lonejson_error *error) {
  cai_tool_argument_validator *validator;
  cai_tool_argument_frame *frame;

  (void)error;
  validator = (cai_tool_argument_validator *)user;
  frame = cai_tool_argument_validator_current(validator);
  if (frame != NULL && frame->kind == CAI_TOOL_JSON_OBJECT) {
    validator->depth--;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_tool_argument_array_begin(void *user,
                                                     lonejson_error *error) {
  cai_tool_argument_validator *validator;
  cai_tool_argument_frame *parent;
  cai_tool_argument_frame *frame;
  const lonejson_map *array_item_map;

  validator = (cai_tool_argument_validator *)user;
  if (validator->depth >=
      sizeof(validator->frames) / sizeof(validator->frames[0])) {
    return cai_tool_argument_validator_error(error,
                                             "tool arguments are too deep");
  }
  array_item_map = NULL;
  parent = cai_tool_argument_validator_current(validator);
  if (parent != NULL && parent->kind == CAI_TOOL_JSON_OBJECT &&
      parent->pending_field != NULL) {
    if (parent->pending_field->kind == LONEJSON_FIELD_KIND_OBJECT_ARRAY) {
      array_item_map = parent->pending_field->submap;
    }
    parent->pending_field = NULL;
  }
  frame = &validator->frames[validator->depth];
  memset(frame, 0, sizeof(*frame));
  frame->kind = CAI_TOOL_JSON_ARRAY;
  frame->array_item_map = array_item_map;
  validator->depth++;
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_tool_argument_array_end(void *user,
                                                   lonejson_error *error) {
  cai_tool_argument_validator *validator;
  cai_tool_argument_frame *frame;

  (void)error;
  validator = (cai_tool_argument_validator *)user;
  frame = cai_tool_argument_validator_current(validator);
  if (frame != NULL && frame->kind == CAI_TOOL_JSON_ARRAY) {
    validator->depth--;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_tool_argument_key_begin(void *user,
                                                   lonejson_error *error) {
  cai_tool_argument_validator *validator;

  (void)error;
  validator = (cai_tool_argument_validator *)user;
  validator->collecting_key = 1;
  validator->key_length = 0U;
  if (validator->key != NULL) {
    validator->key[0] = '\0';
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_tool_argument_key_chunk(void *user, const char *data,
                                                   size_t len,
                                                   lonejson_error *error) {
  cai_tool_argument_validator *validator;
  char *grown;
  size_t required;
  size_t capacity;

  validator = (cai_tool_argument_validator *)user;
  if (!validator->collecting_key || data == NULL) {
    return LONEJSON_STATUS_OK;
  }
  required = validator->key_length + len + 1U;
  if (required > validator->key_capacity) {
    capacity = validator->key_capacity == 0U ? 64U : validator->key_capacity;
    while (capacity < required) {
      capacity *= 2U;
    }
    grown = (char *)cai_realloc_mem(NULL, validator->key, capacity);
    if (grown == NULL) {
      return cai_tool_argument_validator_error(error,
                                               "failed to allocate JSON key");
    }
    validator->key = grown;
    validator->key_capacity = capacity;
  }
  memcpy(validator->key + validator->key_length, data, len);
  validator->key_length += len;
  validator->key[validator->key_length] = '\0';
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_tool_argument_key_end(void *user,
                                                 lonejson_error *error) {
  cai_tool_argument_validator *validator;
  cai_tool_argument_frame *frame;
  const lonejson_field *field;

  validator = (cai_tool_argument_validator *)user;
  validator->collecting_key = 0;
  frame = cai_tool_argument_validator_current(validator);
  if (frame == NULL || frame->kind != CAI_TOOL_JSON_OBJECT ||
      frame->map == NULL) {
    return LONEJSON_STATUS_OK;
  }
  field = cai_tool_map_find_field(frame->map, validator->key,
                                  validator->key_length);
  if (field == NULL) {
    return cai_tool_argument_validator_error(
        error, "tool arguments contain an unknown field");
  }
  frame->pending_field = field;
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_tool_argument_scalar_event(void *user,
                                                      lonejson_error *error) {
  (void)error;
  cai_tool_argument_validator_consume_scalar(
      (cai_tool_argument_validator *)user);
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_tool_argument_bool_event(void *user, int value,
                                                    lonejson_error *error) {
  (void)value;
  return cai_tool_argument_scalar_event(user, error);
}

static int cai_tool_validate_arguments_shape(const lonejson_map *map,
                                             const char *json,
                                             cai_error *error) {
  cai_json_builder compact;
  cai_tool_argument_validator validator;
  lonejson_json_value value;
  lonejson_value_visitor visitor;
  lonejson_value_limits limits;
  lonejson_error json_error;
  lonejson_status status;
  int rc;

  memset(&compact, 0, sizeof(compact));
  lonejson_json_value_init(&value);
  lonejson_error_init(&json_error);
  status =
      lonejson_json_value_set_buffer(&value, json, strlen(json), &json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_json_value_write_to_sink(
        &value, cai_tool_compact_json_sink, &compact, &json_error);
  }
  if (status != LONEJSON_STATUS_OK) {
    lonejson_json_value_cleanup(&value);
    cai_free_mem(NULL, compact.data);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "tool arguments failed validation",
                                json_error.message);
  }

  memset(&validator, 0, sizeof(validator));
  validator.root_map = map;
  visitor = lonejson_default_value_visitor();
  visitor.object_begin = cai_tool_argument_object_begin;
  visitor.object_end = cai_tool_argument_object_end;
  visitor.object_key_begin = cai_tool_argument_key_begin;
  visitor.object_key_chunk = cai_tool_argument_key_chunk;
  visitor.object_key_end = cai_tool_argument_key_end;
  visitor.array_begin = cai_tool_argument_array_begin;
  visitor.array_end = cai_tool_argument_array_end;
  visitor.string_begin = cai_tool_argument_scalar_event;
  visitor.number_begin = cai_tool_argument_scalar_event;
  visitor.boolean_value = cai_tool_argument_bool_event;
  visitor.null_value = cai_tool_argument_scalar_event;
  limits = lonejson_default_value_limits();
  limits.max_key_bytes = 4096U;
  lonejson_error_init(&json_error);
  status = lonejson_visit_value_buffer(compact.data, compact.length, &visitor,
                                       &validator, &limits, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    rc = cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                              "tool arguments failed validation",
                              json_error.message);
  } else {
    rc = CAI_OK;
  }
  cai_free_mem(NULL, validator.key);
  lonejson_json_value_cleanup(&value);
  cai_free_mem(NULL, compact.data);
  return rc;
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
    rc = cai_json_builder_lit(&builder, "],\"additionalProperties\":false}",
                              error);
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
  rc = cai_tool_validate_json_value(
      property_json, "property schema must be valid JSON", error);
  if (rc != CAI_OK) {
    return rc;
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
  cai_tool_typed_property_doc doc;
  lonejson_error json_error;
  char *json;
  int rc;

  if (type == NULL || type[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "property type is required");
  }
  doc.type = type;
  doc.description = description;
  lonejson_error_init(&json_error);
  json = lonejson_serialize_alloc(&cai_tool_typed_property_map, &doc, NULL,
                                  NULL, &json_error);
  if (json == NULL) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to serialize typed property schema",
                                json_error.message);
  }
  rc = cai_tool_schema_add_property_json(schema, name, json, required, error);
  cai_free_mem(NULL, json);
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
    return cai_tool_schema_add_typed_property(schema, field->json_key, NULL,
                                              type, required, error);
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
      rc = cai_json_builder_lit(&builder,
                                "{\"type\":\"array\",\"items\":", error);
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
    case LONEJSON_FIELD_KIND_STRING_STREAM:
    case LONEJSON_FIELD_KIND_BASE64_STREAM:
      lonejson_spooled_cleanup((lonejson_spooled *)ptr);
      break;
    case LONEJSON_FIELD_KIND_STRING_SOURCE:
    case LONEJSON_FIELD_KIND_BASE64_SOURCE:
      lonejson_source_cleanup((lonejson_source *)ptr);
      break;
    case LONEJSON_FIELD_KIND_JSON_VALUE:
      lonejson_json_value_cleanup((lonejson_json_value *)ptr);
      break;
    default:
      break;
    }
  }
}

static const lonejson_field *
cai_tool_result_find_field(const lonejson_map *map, const char *field_name) {
  size_t i;

  if (map == NULL || field_name == NULL || field_name[0] == '\0') {
    return NULL;
  }
  for (i = 0U; i < map->field_count; i++) {
    if (strcmp(map->fields[i].json_key, field_name) == 0) {
      return &map->fields[i];
    }
  }
  return NULL;
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

int cai_tool_result_set_source_path(const lonejson_map *result_map,
                                    void *result, const char *field_name,
                                    const char *path, cai_error *error) {
  const lonejson_field *field;
  lonejson_source *source;
  lonejson_error json_error;

  if (result == NULL || path == NULL || path[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "tool result, field name, and source path are "
                         "required");
  }
  field = cai_tool_result_find_field(result_map, field_name);
  if (field == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "tool result field is not registered");
  }
  if (field->kind != LONEJSON_FIELD_KIND_STRING_SOURCE &&
      field->kind != LONEJSON_FIELD_KIND_BASE64_SOURCE) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "tool result field is not a source field");
  }
  source = (lonejson_source *)((unsigned char *)result + field->struct_offset);
  lonejson_error_init(&json_error);
  if (lonejson_source_set_path(source, path, &json_error) !=
      LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to set tool result source path",
                                json_error.message);
  }
  return CAI_OK;
}

int cai_tool_result_set_spooled(const lonejson_map *result_map, void *result,
                                const char *field_name, lonejson_spooled *spool,
                                cai_error *error) {
  const lonejson_field *field;
  lonejson_spooled *target;

  if (result == NULL || spool == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "tool result, field name, and spooled value are "
                         "required");
  }
  field = cai_tool_result_find_field(result_map, field_name);
  if (field == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "tool result field is not registered");
  }
  if (field->kind != LONEJSON_FIELD_KIND_STRING_STREAM &&
      field->kind != LONEJSON_FIELD_KIND_BASE64_STREAM) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "tool result field is not a spooled field");
  }
  target = (lonejson_spooled *)((unsigned char *)result + field->struct_offset);
  lonejson_spooled_cleanup(target);
  *target = *spool;
  memset(spool, 0, sizeof(*spool));
  return CAI_OK;
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
    void (*context_cleanup)(void *context), cai_error *error) {
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
  if (entry->name == NULL ||
      (description != NULL && entry->description == NULL) ||
      entry->schema_json == NULL) {
    cai_tool_entry_cleanup(entry);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate tool registration");
  }
  entry->context = context;
  entry->context_cleanup = context_cleanup;
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
        result_map, callback, NULL, context, NULL, error);
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
      NULL, NULL, callback, context, NULL, error);
}

static void cai_searxng_context_cleanup(void *context) {
  cai_searxng_context *ctx;

  ctx = (cai_searxng_context *)context;
  if (ctx == NULL) {
    return;
  }
  cai_free_mem(NULL, ctx->base_url);
  cai_free_mem(NULL, ctx->search_path);
  cai_free_mem(NULL, ctx->engine);
  cai_free_mem(NULL, ctx->language);
  cai_free_mem(NULL, ctx->response_spool_dir);
  cai_free_mem(NULL, ctx);
}

static const char *cai_searxng_default_string(const char *value,
                                              const char *fallback) {
  return value != NULL && value[0] != '\0' ? value : fallback;
}

static int cai_searxng_context_copy_string(char **out, const char *value,
                                           cai_error *error) {
  *out = cai_strdup(NULL, value);
  if (*out == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate SearXNG tool config");
  }
  return CAI_OK;
}

static int cai_searxng_context_new(const cai_searxng_tool_config *config,
                                   cai_searxng_context **out,
                                   cai_error *error) {
  const char *base_url;
  const char *search_path;
  const char *engine;
  const char *language;
  cai_searxng_context *ctx;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "SearXNG context output pointer is required");
  }
  *out = NULL;
  base_url = cai_searxng_default_string(
      config != NULL ? config->base_url : NULL, CAI_SEARXNG_DEFAULT_BASE_URL);
  search_path =
      cai_searxng_default_string(config != NULL ? config->search_path : NULL,
                                 CAI_SEARXNG_DEFAULT_SEARCH_PATH);
  engine = cai_searxng_default_string(config != NULL ? config->engine : NULL,
                                      CAI_SEARXNG_DEFAULT_ENGINE);
  language = cai_searxng_default_string(
      config != NULL ? config->language : NULL, "en");
  ctx = (cai_searxng_context *)cai_alloc(NULL, sizeof(*ctx));
  if (ctx == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate SearXNG tool context");
  }
  memset(ctx, 0, sizeof(*ctx));
  rc = cai_searxng_context_copy_string(&ctx->base_url, base_url, error);
  if (rc == CAI_OK) {
    rc = cai_searxng_context_copy_string(&ctx->search_path, search_path, error);
  }
  if (rc == CAI_OK) {
    rc = cai_searxng_context_copy_string(&ctx->engine, engine, error);
  }
  if (rc == CAI_OK) {
    rc = cai_searxng_context_copy_string(&ctx->language, language, error);
  }
  if (rc == CAI_OK && config != NULL && config->response_spool_dir != NULL &&
      config->response_spool_dir[0] != '\0') {
    rc = cai_searxng_context_copy_string(&ctx->response_spool_dir,
                                         config->response_spool_dir, error);
  }
  if (rc != CAI_OK) {
    cai_searxng_context_cleanup(ctx);
    return rc;
  }
  ctx->timeout_ms =
      config != NULL && config->timeout_ms > 0L ? config->timeout_ms : 10000L;
  ctx->response_memory_limit =
      config != NULL && config->response_memory_limit != 0U
          ? config->response_memory_limit
          : 128U * 1024U;
  ctx->response_max_bytes = config != NULL && config->response_max_bytes != 0U
                                ? config->response_max_bytes
                                : 1024U * 1024U;
  *out = ctx;
  return CAI_OK;
}

static int cai_searxng_append_url_part(cai_json_builder *url,
                                       const char *base_url,
                                       const char *search_path,
                                       cai_error *error) {
  size_t base_len;
  int rc;

  base_len = strlen(base_url);
  rc = cai_json_builder_lit(url, base_url, error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (base_len > 0U && base_url[base_len - 1U] == '/' &&
      search_path[0] == '/') {
    return cai_json_builder_lit(url, search_path + 1, error);
  }
  if ((base_len == 0U || base_url[base_len - 1U] != '/') &&
      search_path[0] != '/') {
    rc = cai_json_builder_lit(url, "/", error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  return cai_json_builder_lit(url, search_path, error);
}

static int cai_searxng_append_query_param(CURL *curl, cai_json_builder *url,
                                          const char *name, const char *value,
                                          int *need_amp, cai_error *error) {
  char *escaped;
  int rc;

  escaped = curl_easy_escape(curl, value != NULL ? value : "", 0);
  if (escaped == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to URL-encode SearXNG query parameter");
  }
  rc = cai_json_builder_lit(url, *need_amp ? "&" : "?", error);
  if (rc == CAI_OK) {
    rc = cai_json_builder_lit(url, name, error);
  }
  if (rc == CAI_OK) {
    rc = cai_json_builder_lit(url, "=", error);
  }
  if (rc == CAI_OK) {
    rc = cai_json_builder_lit(url, escaped, error);
  }
  curl_free(escaped);
  *need_amp = 1;
  return rc;
}

static int cai_searxng_build_url(CURL *curl, const cai_searxng_context *ctx,
                                 const char *query, char **out,
                                 cai_error *error) {
  cai_json_builder url;
  int need_amp;
  int rc;

  memset(&url, 0, sizeof(url));
  need_amp = 0;
  rc =
      cai_searxng_append_url_part(&url, ctx->base_url, ctx->search_path, error);
  if (rc == CAI_OK) {
    rc = cai_searxng_append_query_param(curl, &url, "q", query, &need_amp,
                                        error);
  }
  if (rc == CAI_OK) {
    rc = cai_searxng_append_query_param(curl, &url, "format", "json", &need_amp,
                                        error);
  }
  if (rc == CAI_OK) {
    rc = cai_searxng_append_query_param(curl, &url, "engines", ctx->engine,
                                        &need_amp, error);
  }
  if (rc == CAI_OK) {
    rc = cai_searxng_append_query_param(curl, &url, "language", ctx->language,
                                        &need_amp, error);
  }
  if (rc == CAI_OK) {
    rc = cai_searxng_append_query_param(curl, &url, "safesearch", "0",
                                        &need_amp, error);
  }
  if (rc == CAI_OK) {
    *out = url.data;
    return CAI_OK;
  }
  cai_free_mem(NULL, url.data);
  return rc;
}

static size_t cai_searxng_write_spool(char *ptr, size_t size, size_t nmemb,
                                      void *userdata) {
  lonejson_spooled *spool;
  lonejson_error json_error;
  size_t len;

  spool = (lonejson_spooled *)userdata;
  len = size * nmemb;
  lonejson_error_init(&json_error);
  if (lonejson_spooled_append(spool, ptr, len, &json_error) !=
      LONEJSON_STATUS_OK) {
    return 0U;
  }
  return len;
}

static int cai_searxng_fetch(const cai_searxng_context *ctx, const char *query,
                             lonejson_spooled *out, cai_error *error) {
  CURL *curl;
  CURLcode code;
  long http_status;
  char *url;
  lonejson_spool_options spool_options;
  int rc;

  url = NULL;
  curl = curl_easy_init();
  if (curl == NULL) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to initialize curl for SearXNG");
  }
  rc = cai_searxng_build_url(curl, ctx, query, &url, error);
  if (rc != CAI_OK) {
    curl_easy_cleanup(curl);
    return rc;
  }
  spool_options = lonejson_default_spool_options();
  spool_options.memory_limit = ctx->response_memory_limit;
  spool_options.max_bytes = ctx->response_max_bytes;
  spool_options.temp_dir = ctx->response_spool_dir;
  lonejson_spooled_init(out, &spool_options);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cai_searxng_write_spool);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "cai-searxng-tool/1");
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, ctx->timeout_ms);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  code = curl_easy_perform(curl);
  http_status = 0L;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
  curl_easy_cleanup(curl);
  cai_free_mem(NULL, url);
  if (code != CURLE_OK) {
    lonejson_spooled_cleanup(out);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "SearXNG request failed",
                                curl_easy_strerror(code));
  }
  if (http_status < 200L || http_status >= 300L) {
    lonejson_spooled_cleanup(out);
    return cai_set_error_http(error, CAI_ERR_SERVER, http_status,
                              "SearXNG request failed", NULL, NULL, NULL);
  }
  return CAI_OK;
}

static lonejson_read_result
cai_searxng_spool_read(void *user, unsigned char *buffer, size_t capacity) {
  cai_searxng_spool_reader *reader;

  reader = (cai_searxng_spool_reader *)user;
  return lonejson_spooled_read(&reader->cursor, buffer, capacity);
}

static int cai_searxng_parse(lonejson_spooled *json,
                             cai_searxng_response_doc *doc, cai_error *error) {
  cai_searxng_spool_reader reader;
  lonejson_error json_error;

  memset(doc, 0, sizeof(*doc));
  lonejson_init(&cai_searxng_response_map, doc);
  reader.cursor = *json;
  lonejson_error_init(&json_error);
  if (lonejson_spooled_rewind(&reader.cursor, &json_error) !=
      LONEJSON_STATUS_OK) {
    lonejson_cleanup(&cai_searxng_response_map, doc);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to rewind SearXNG response",
                                json_error.message);
  }
  lonejson_error_init(&json_error);
  if (lonejson_parse_reader(&cai_searxng_response_map, doc,
                            cai_searxng_spool_read, &reader, NULL,
                            &json_error) != LONEJSON_STATUS_OK) {
    lonejson_cleanup(&cai_searxng_response_map, doc);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to parse SearXNG response JSON",
                                json_error.message);
  }
  return CAI_OK;
}

static int cai_searxng_result_copy(char **target, const char *value,
                                   cai_error *error) {
  *target = cai_tool_result_strdup(value != NULL ? value : "", error);
  return *target != NULL ? CAI_OK : CAI_ERR_NOMEM;
}

static int cai_searxng_fill_result(const cai_searxng_context *ctx,
                                   const cai_searxng_args *args,
                                   const cai_searxng_response_doc *doc,
                                   cai_searxng_result *out, cai_error *error) {
  cai_searxng_item_doc *item;
  const char *title;
  const char *url;
  const char *snippet;
  const char *source;
  int rc;

  item = NULL;
  if (doc->results.count > 0U) {
    item = (cai_searxng_item_doc *)doc->results.items;
    title = item[0].title;
    url = item[0].url;
    snippet = item[0].content;
    source = "result";
  } else if (doc->infoboxes.count > 0U) {
    item = (cai_searxng_item_doc *)doc->infoboxes.items;
    title = item[0].infobox != NULL ? item[0].infobox : item[0].title;
    url = item[0].id != NULL ? item[0].id : item[0].url;
    snippet = item[0].content;
    source = "infobox";
  } else {
    title = "";
    url = "";
    snippet = "";
    source = "none";
  }
  rc = cai_searxng_result_copy(
      &out->query, doc->query != NULL ? doc->query : args->query, error);
  if (rc == CAI_OK) {
    rc = cai_searxng_result_copy(&out->engine, ctx->engine, error);
  }
  if (rc == CAI_OK) {
    rc = cai_searxng_result_copy(&out->title, title, error);
  }
  if (rc == CAI_OK) {
    rc = cai_searxng_result_copy(&out->url, url, error);
  }
  if (rc == CAI_OK) {
    rc = cai_searxng_result_copy(&out->snippet, snippet, error);
  }
  if (rc == CAI_OK) {
    rc = cai_searxng_result_copy(&out->source, source, error);
  }
  out->result_count = (long long)doc->results.count;
  out->infobox_count = (long long)doc->infoboxes.count;
  return rc;
}

static int cai_searxng_tool_callback(void *context, const void *params,
                                     void *result, cai_error *error) {
  const cai_searxng_context *ctx;
  const cai_searxng_args *args;
  cai_searxng_result *out;
  cai_searxng_response_doc doc;
  lonejson_spooled body;
  int rc;

  ctx = (const cai_searxng_context *)context;
  args = (const cai_searxng_args *)params;
  out = (cai_searxng_result *)result;
  if (ctx == NULL || args == NULL || args->query == NULL ||
      args->query[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "SearXNG query is required");
  }
  rc = cai_searxng_fetch(ctx, args->query, &body, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_searxng_parse(&body, &doc, error);
  lonejson_spooled_cleanup(&body);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_searxng_fill_result(ctx, args, &doc, out, error);
  lonejson_cleanup(&cai_searxng_response_map, &doc);
  return rc;
}

int cai_agent_register_searxng_tool(cai_agent *agent,
                                    const cai_searxng_tool_config *config,
                                    cai_error *error) {
  const char *name;
  const char *description;
  cai_searxng_context *ctx;
  cai_agent_impl *impl;
  cai_tool_schema *schema;
  int rc;

  if (agent == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  impl = CAI_AGENT_IMPL(agent);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is closed");
  }
  name = cai_searxng_default_string(config != NULL ? config->name : NULL,
                                    "searxng_search");
  description = cai_searxng_default_string(
      config != NULL ? config->description : NULL,
      "Search through the configured SearXNG endpoint using the fixed "
      "configured engine.");
  rc = cai_searxng_context_new(config, &ctx, error);
  if (rc != CAI_OK) {
    return rc;
  }
  schema = NULL;
  rc = cai_tool_schema_from_map(&cai_searxng_args_map, &schema, error);
  if (rc == CAI_OK) {
    rc = cai_tool_schema_describe(schema, "query", "Search query", error);
  }
  if (rc == CAI_OK) {
    rc = cai_tool_registry_register_common(
        impl->tools, name, description, cai_tool_schema_json(schema),
        cai_tool_schema_strict(schema), CAI_TOOL_LONEJSON,
        &cai_searxng_args_map, &cai_searxng_result_map,
        cai_searxng_tool_callback, NULL, ctx, cai_searxng_context_cleanup,
        error);
  }
  cai_tool_schema_destroy(schema);
  if (rc != CAI_OK) {
    cai_searxng_context_cleanup(ctx);
  }
  return rc;
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
    rc = cai_tool_validate_json_value(
        arguments_json, "tool arguments must be valid JSON", error);
    if (rc != CAI_OK) {
      return rc;
    }
    return entry->raw_callback(entry->context, arguments_json, output, error);
  }
  rc = cai_tool_validate_arguments_shape(entry->params_map, arguments_json,
                                         error);
  if (rc != CAI_OK) {
    return rc;
  }
  params = cai_alloc(NULL, entry->params_map->struct_size);
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate tool parameters");
  }
  result = cai_alloc(NULL, entry->result_map->struct_size);
  if (result == NULL) {
    cai_free_mem(NULL, params);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate tool result");
  }
  memset(params, 0, entry->params_map->struct_size);
  memset(result, 0, entry->result_map->struct_size);
  lonejson_init(entry->params_map, params);
  lonejson_init(entry->result_map, result);
  lonejson_error_init(&json_error);
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
  return cai_tool_schema_add_typed_property(schema, name, description,
                                            "integer", required, error);
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
  return cai_tool_schema_add_typed_property(schema, name, description,
                                            "boolean", required, error);
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
    rc = cai_json_builder_field_string(&builder, "type", "string", &need_comma,
                                       error);
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
  rc = cai_tool_validate_json_value(
      schema_json, "raw property schema must be valid JSON", error);
  if (rc != CAI_OK) {
    return rc;
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
