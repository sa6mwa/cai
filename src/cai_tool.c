#include "cai_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum cai_tool_kind {
  CAI_TOOL_LONEJSON = 1,
  CAI_TOOL_RAW = 2,
  CAI_TOOL_RAW_SPOOLED = 3
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
  cai_tool_raw_spooled_fn raw_spooled_callback;
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

typedef struct cai_tool_spooled_reader {
  lonejson_spooled cursor;
} cai_tool_spooled_reader;

typedef struct cai_tool_buffer_sink_context {
  cai_buffer_builder *builder;
  cai_error *error;
} cai_tool_buffer_sink_context;

typedef struct cai_tool_description_rewrite {
  const char *description;
} cai_tool_description_rewrite;

#define CAI_TOOL_SCHEMA_IMPL(schema) ((cai_tool_schema_impl *)((schema)->impl))

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

static lonejson_read_result cai_tool_spooled_read(void *user,
                                                  unsigned char *buffer,
                                                  size_t capacity) {
  cai_tool_spooled_reader *reader;

  reader = (cai_tool_spooled_reader *)user;
  return lonejson_spooled_read(&reader->cursor, buffer, capacity);
}

static lonejson_status cai_tool_buffer_sink(void *user, const void *data,
                                                  size_t len,
                                                  lonejson_error *json_error) {
  cai_tool_buffer_sink_context *context;

  (void)json_error;
  context = (cai_tool_buffer_sink_context *)user;
  if (context == NULL || context->builder == NULL || data == NULL) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (cai_buffer_append(context->builder, (const char *)data, len,
                              context->error) != CAI_OK) {
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status
cai_tool_writer_key(lonejson_writer *writer, const char *key,
                    lonejson_error *json_error) {
  return lonejson_writer_key(writer, key, strlen(key), json_error);
}

static lonejson_status
cai_tool_writer_string_cstr(lonejson_writer *writer, const char *value,
                            lonejson_error *json_error) {
  if (value == NULL) {
    value = "";
  }
  return lonejson_writer_string(writer, value, strlen(value), json_error);
}

static lonejson_status
cai_tool_writer_raw_json(lonejson_writer *writer, const char *json,
                         lonejson_error *json_error) {
  return lonejson_writer_json_value_buffer(writer, json, strlen(json), NULL,
                                           json_error);
}

static lonejson_status
cai_tool_emit_description(lonejson_writer *writer, void *user,
                          lonejson_error *json_error) {
  cai_tool_description_rewrite *rewrite;

  rewrite = (cai_tool_description_rewrite *)user;
  if (rewrite == NULL) {
    return LONEJSON_STATUS_INVALID_ARGUMENT;
  }
  return cai_tool_writer_string_cstr(writer, rewrite->description, json_error);
}

static int cai_tool_schema_rewrite_description(cai_buffer_builder *builder,
                                               const char *schema_json,
                                               const char *description,
                                               cai_error *error) {
  cai_tool_buffer_sink_context sink_context;
  cai_tool_description_rewrite rewrite;
  lonejson_value_rewrite_selector_options options;
  lonejson_error json_error;
  lonejson_status status;

  sink_context.builder = builder;
  sink_context.error = error;
  rewrite.description = description;
  memset(&options, 0, sizeof(options));
  options.selector = "description";
  options.action = LONEJSON_VALUE_REWRITE_REPLACE;
  options.replacement.emit = cai_tool_emit_description;
  options.replacement.emit_user = &rewrite;
  lonejson_error_init(&json_error);
  status = lonejson_value_rewrite_selector_buffer(
      schema_json, strlen(schema_json), cai_tool_buffer_sink,
      &sink_context, &options, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to rewrite tool schema description",
                                json_error.message);
  }
  return CAI_OK;
}

static int cai_tool_schema_build_array_property(cai_buffer_builder *builder,
                                                const char *item_type,
                                                const char *item_json,
                                                cai_error *error) {
  cai_tool_buffer_sink_context sink_context;
  lonejson_writer writer;
  lonejson_error json_error;
  lonejson_status status;

  sink_context.builder = builder;
  sink_context.error = error;
  lonejson_error_init(&json_error);
  status = lonejson_writer_init_sink(&writer, cai_tool_buffer_sink,
                                     &sink_context, NULL, &json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_tool_writer_key(&writer, "type", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_tool_writer_string_cstr(&writer, "array", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_tool_writer_key(&writer, "items", &json_error);
  }
  if (status == LONEJSON_STATUS_OK && item_json != NULL) {
    status = cai_tool_writer_raw_json(&writer, item_json, &json_error);
  }
  if (status == LONEJSON_STATUS_OK && item_json == NULL) {
    status = lonejson_writer_begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK && item_json == NULL) {
    status = cai_tool_writer_key(&writer, "type", &json_error);
  }
  if (status == LONEJSON_STATUS_OK && item_json == NULL) {
    status = cai_tool_writer_string_cstr(&writer, item_type, &json_error);
  }
  if (status == LONEJSON_STATUS_OK && item_json == NULL) {
    status = lonejson_writer_end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_finish(&writer, &json_error);
  }
  lonejson_writer_cleanup(&writer);
  if (status != LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to serialize array property schema",
                                json_error.message);
  }
  return CAI_OK;
}

static int cai_tool_schema_build_empty_object(cai_buffer_builder *builder,
                                              cai_error *error) {
  cai_tool_buffer_sink_context sink_context;
  lonejson_writer writer;
  lonejson_error json_error;
  lonejson_status status;

  sink_context.builder = builder;
  sink_context.error = error;
  lonejson_error_init(&json_error);
  status = lonejson_writer_init_sink(&writer, cai_tool_buffer_sink,
                                     &sink_context, NULL, &json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_finish(&writer, &json_error);
  }
  lonejson_writer_cleanup(&writer);
  if (status != LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to serialize empty object schema",
                                json_error.message);
  }
  return CAI_OK;
}

static lonejson_status
cai_tool_writer_property(lonejson_writer *writer,
                         const cai_tool_schema_property *prop, int strict,
                         lonejson_error *json_error) {
  lonejson_status status;

  if (strict && !prop->required) {
    status = lonejson_writer_begin_object(writer, json_error);
    if (status == LONEJSON_STATUS_OK) {
      status = cai_tool_writer_key(writer, "anyOf", json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = lonejson_writer_begin_array(writer, json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = cai_tool_writer_raw_json(writer, prop->property_json,
                                        json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = lonejson_writer_begin_object(writer, json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = cai_tool_writer_key(writer, "type", json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = cai_tool_writer_string_cstr(writer, "null", json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = lonejson_writer_end_object(writer, json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = lonejson_writer_end_array(writer, json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = lonejson_writer_end_object(writer, json_error);
    }
    return status;
  }
  return cai_tool_writer_raw_json(writer, prop->property_json, json_error);
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

static int cai_tool_schema_replace(char **target, cai_buffer_builder *builder,
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

static int cai_tool_validate_spooled_json_value(lonejson_spooled *json,
                                                const char *message,
                                                cai_error *error) {
  cai_tool_spooled_reader reader;
  lonejson_value_visitor visitor;
  lonejson_value_limits limits;
  lonejson_error json_error;

  if (json == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, message);
  }
  reader.cursor = *json;
  lonejson_error_init(&json_error);
  if (lonejson_spooled_rewind(&reader.cursor, &json_error) !=
      LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to rewind tool arguments",
                                json_error.message);
  }
  visitor = lonejson_default_value_visitor();
  limits = lonejson_default_value_limits();
  if (lonejson_visit_value_reader(cai_tool_spooled_read, &reader, &visitor,
                                  NULL, &limits, &json_error) !=
      LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_INVALID, message,
                                json_error.message);
  }
  return CAI_OK;
}

static lonejson_status cai_tool_compact_json_sink(void *user, const void *data,
                                                  size_t len,
                                                  lonejson_error *json_error) {
  cai_buffer_builder *builder;
  cai_error error;

  builder = (cai_buffer_builder *)user;
  cai_error_init(&error);
  if (cai_buffer_append(builder, (const char *)data, len, &error) ==
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
  cai_buffer_builder compact;
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
  cai_buffer_builder builder;
  cai_tool_buffer_sink_context sink_context;
  lonejson_writer writer;
  lonejson_error json_error;
  lonejson_status status;
  size_t i;

  impl = CAI_TOOL_SCHEMA_IMPL(schema);
  memset(&builder, 0, sizeof(builder));
  sink_context.builder = &builder;
  sink_context.error = error;
  lonejson_error_init(&json_error);
  status = lonejson_writer_init_sink(&writer, cai_tool_buffer_sink,
                                     &sink_context, NULL, &json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_tool_writer_key(&writer, "type", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_tool_writer_string_cstr(&writer, "object", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_tool_writer_key(&writer, "properties", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_object(&writer, &json_error);
  }
  for (i = 0U; status == LONEJSON_STATUS_OK && i < impl->property_count; i++) {
    status = lonejson_writer_key(&writer, impl->properties[i].name,
                                 strlen(impl->properties[i].name),
                                 &json_error);
    if (status == LONEJSON_STATUS_OK) {
      status = cai_tool_writer_property(&writer, &impl->properties[i],
                                        impl->strict, &json_error);
    }
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_tool_writer_key(&writer, "required", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_array(&writer, &json_error);
  }
  for (i = 0U; status == LONEJSON_STATUS_OK && i < impl->property_count; i++) {
    if (!impl->strict && !impl->properties[i].required) {
      continue;
    }
    status = cai_tool_writer_string_cstr(&writer, impl->properties[i].name,
                                         &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_array(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_tool_writer_key(&writer, "additionalProperties",
                                 &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_bool(&writer, 0, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_finish(&writer, &json_error);
  }
  lonejson_writer_cleanup(&writer);
  if (status != LONEJSON_STATUS_OK) {
    cai_free_mem(NULL, builder.data);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to serialize tool schema",
                                json_error.message);
  }
  if (cai_tool_schema_replace(&impl->schema_json, &builder, error) != CAI_OK) {
    cai_free_mem(NULL, builder.data);
    return error != NULL ? error->code : CAI_ERR_NOMEM;
  }
  cai_free_mem(NULL, builder.data);
  return CAI_OK;
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
  cai_buffer_builder builder;
  cai_tool_buffer_sink_context sink_context;
  lonejson_writer writer;
  lonejson_error json_error;
  lonejson_status status;
  int rc;

  if (type == NULL || type[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "property type is required");
  }
  memset(&builder, 0, sizeof(builder));
  sink_context.builder = &builder;
  sink_context.error = error;
  lonejson_error_init(&json_error);
  status = lonejson_writer_init_sink(&writer, cai_tool_buffer_sink,
                                     &sink_context, NULL, &json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_tool_writer_key(&writer, "type", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_tool_writer_string_cstr(&writer, type, &json_error);
  }
  if (status == LONEJSON_STATUS_OK && description != NULL) {
    status = cai_tool_writer_key(&writer, "description", &json_error);
  }
  if (status == LONEJSON_STATUS_OK && description != NULL) {
    status = cai_tool_writer_string_cstr(&writer, description, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_finish(&writer, &json_error);
  }
  lonejson_writer_cleanup(&writer);
  if (status != LONEJSON_STATUS_OK) {
    cai_free_mem(NULL, builder.data);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to serialize typed property schema",
                                json_error.message);
  }
  rc = cai_tool_schema_add_property_json(
      schema, name, builder.data != NULL ? builder.data : "{}", required,
      error);
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
  cai_buffer_builder builder;
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
    rc = cai_tool_schema_build_array_property(&builder, "string", NULL, error);
    break;
  case LONEJSON_FIELD_KIND_I64_ARRAY:
  case LONEJSON_FIELD_KIND_U64_ARRAY:
    rc = cai_tool_schema_build_array_property(&builder, "integer", NULL,
                                              error);
    break;
  case LONEJSON_FIELD_KIND_F64_ARRAY:
    rc = cai_tool_schema_build_array_property(&builder, "number", NULL, error);
    break;
  case LONEJSON_FIELD_KIND_BOOL_ARRAY:
    rc = cai_tool_schema_build_array_property(&builder, "boolean", NULL,
                                              error);
    break;
  case LONEJSON_FIELD_KIND_OBJECT_ARRAY: {
    cai_tool_schema *nested;

    nested = NULL;
    rc = cai_tool_schema_from_map(field->submap, &nested, error);
    if (rc == CAI_OK) {
      rc = cai_tool_schema_build_array_property(
          &builder, NULL, cai_tool_schema_json(nested), error);
    }
    cai_tool_schema_destroy(nested);
    break;
  }
  case LONEJSON_FIELD_KIND_JSON_VALUE:
    rc = cai_tool_schema_build_empty_object(&builder, error);
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
    case LONEJSON_FIELD_KIND_OBJECT_ARRAY: {
      lonejson_object_array *array;
      size_t j;

      array = (lonejson_object_array *)ptr;
      for (j = 0U; j < array->count; j++) {
        cai_tool_result_cleanup_plain(
            field->submap,
            (unsigned char *)array->items + (j * array->elem_size));
      }
      cai_free_mem(NULL, array->items);
      array->items = NULL;
      array->count = 0U;
      array->capacity = 0U;
      break;
    }
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
    cai_tool_fn lonejson_callback, cai_tool_raw_fn raw_callback,
    cai_tool_raw_spooled_fn raw_spooled_callback, void *context,
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
  if (kind == CAI_TOOL_RAW_SPOOLED && raw_spooled_callback == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "spooled raw tool callback is required");
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
  entry->raw_spooled_callback = raw_spooled_callback;
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
        result_map, callback, NULL, NULL, context, NULL, error);
  }
  cai_tool_schema_destroy(schema);
  return rc;
}

int cai_tool_registry_register_lonejson_owned(
    cai_tool_registry *registry, const char *name, const char *description,
    const lonejson_map *params_map, const lonejson_map *result_map,
    cai_tool_fn callback, void *context, void (*context_cleanup)(void *context),
    cai_error *error) {
  cai_tool_schema *schema;
  int rc;

  schema = NULL;
  rc = cai_tool_schema_from_map(params_map, &schema, error);
  if (rc == CAI_OK) {
    rc = cai_tool_registry_register_common(
        registry, name, description, cai_tool_schema_json(schema),
        cai_tool_schema_strict(schema), CAI_TOOL_LONEJSON, params_map,
        result_map, callback, NULL, NULL, context, context_cleanup, error);
  }
  cai_tool_schema_destroy(schema);
  return rc;
}

int cai_tool_registry_register_lonejson_schema_owned(
    cai_tool_registry *registry, const char *name, const char *description,
    const char *schema_json, int strict, const lonejson_map *params_map,
    const lonejson_map *result_map, cai_tool_fn callback, void *context,
    void (*context_cleanup)(void *context), cai_error *error) {
  return cai_tool_registry_register_common(
      registry, name, description, schema_json, strict, CAI_TOOL_LONEJSON,
      params_map, result_map, callback, NULL, NULL, context, context_cleanup,
      error);
}

int cai_tool_registry_register_raw(cai_tool_registry *registry,
                                   const char *name, const char *description,
                                   const char *schema_json, int strict,
                                   cai_tool_raw_fn callback, void *context,
                                   cai_error *error) {
  return cai_tool_registry_register_common(
      registry, name, description, schema_json, strict, CAI_TOOL_RAW, NULL,
      NULL, NULL, callback, NULL, context, NULL, error);
}

int cai_tool_registry_register_raw_spooled(
    cai_tool_registry *registry, const char *name, const char *description,
    const char *schema_json, int strict, cai_tool_raw_spooled_fn callback,
    void *context, cai_error *error) {
  return cai_tool_registry_register_common(
      registry, name, description, schema_json, strict, CAI_TOOL_RAW_SPOOLED,
      NULL, NULL, NULL, NULL, callback, context, NULL, error);
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

size_t cai_tool_registry_count(const cai_tool_registry *registry) {
  return registry != NULL ? registry->count : 0U;
}

const char *cai_tool_registry_name_at(const cai_tool_registry *registry,
                                      size_t index) {
  if (registry == NULL || index >= registry->count) {
    return NULL;
  }
  return registry->entries[index].name;
}

const char *cai_tool_registry_description_at(const cai_tool_registry *registry,
                                             size_t index) {
  if (registry == NULL || index >= registry->count) {
    return NULL;
  }
  return registry->entries[index].description;
}

const char *cai_tool_registry_schema_at(const cai_tool_registry *registry,
                                        size_t index) {
  if (registry == NULL || index >= registry->count) {
    return NULL;
  }
  return registry->entries[index].schema_json;
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
  if (entry->kind == CAI_TOOL_RAW_SPOOLED) {
    lonejson_spooled spooled;
    lonejson_error spool_error;

    rc = cai_tool_validate_json_value(
        arguments_json, "tool arguments must be valid JSON", error);
    if (rc != CAI_OK) {
      return rc;
    }
    lonejson_spooled_init(&spooled, NULL);
    lonejson_error_init(&spool_error);
    if (lonejson_spooled_append(&spooled, arguments_json,
                                strlen(arguments_json), &spool_error) !=
        LONEJSON_STATUS_OK) {
      lonejson_spooled_cleanup(&spooled);
      return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                  "failed to spool raw tool arguments",
                                  spool_error.message);
    }
    rc = entry->raw_spooled_callback(entry->context, &spooled, output, error);
    lonejson_spooled_cleanup(&spooled);
    return rc;
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

int cai_tool_registry_run_spooled(cai_tool_registry *registry,
                                  const char *name,
                                  lonejson_spooled *arguments_json,
                                  cai_sink *output, cai_error *error) {
  cai_tool_entry *entry;
  cai_tool_spooled_reader reader;
  lonejson_parse_options options;
  lonejson_error json_error;
  lonejson_status status;
  void *params;
  void *result;
  char *raw_json;
  size_t raw_len;
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
    raw_len = lonejson_spooled_size(arguments_json);
    raw_json = (char *)cai_alloc(NULL, raw_len + 1U);
    if (raw_json == NULL) {
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate raw tool arguments");
    }
    reader.cursor = *arguments_json;
    lonejson_error_init(&json_error);
    if (lonejson_spooled_rewind(&reader.cursor, &json_error) !=
        LONEJSON_STATUS_OK) {
      cai_free_mem(NULL, raw_json);
      return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                  "failed to rewind tool arguments",
                                  json_error.message);
    }
    {
      lonejson_read_result chunk;
      size_t offset;

      offset = 0U;
      while (offset < raw_len) {
        chunk = lonejson_spooled_read(&reader.cursor,
                                      (unsigned char *)raw_json + offset,
                                      raw_len - offset);
        if (chunk.error_code != 0) {
          cai_free_mem(NULL, raw_json);
          return cai_set_error(error, CAI_ERR_PROTOCOL,
                               "failed to read tool arguments");
        }
        if (chunk.bytes_read == 0U) {
          break;
        }
        offset += chunk.bytes_read;
      }
      raw_json[offset] = '\0';
    }
    rc = cai_tool_registry_run(registry, name, raw_json, output, error);
    cai_free_mem(NULL, raw_json);
    return rc;
  }
  if (entry->kind == CAI_TOOL_RAW_SPOOLED) {
    rc = cai_tool_validate_spooled_json_value(
        arguments_json, "tool arguments must be valid JSON", error);
    if (rc != CAI_OK) {
      return rc;
    }
    return entry->raw_spooled_callback(entry->context, arguments_json, output,
                                       error);
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
  reader.cursor = *arguments_json;
  lonejson_error_init(&json_error);
  if (lonejson_spooled_rewind(&reader.cursor, &json_error) !=
      LONEJSON_STATUS_OK) {
    lonejson_cleanup(entry->params_map, params);
    lonejson_cleanup(entry->result_map, result);
    cai_free_mem(NULL, result);
    cai_free_mem(NULL, params);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to rewind tool arguments",
                                json_error.message);
  }
  options = lonejson_default_parse_options();
  status = lonejson_parse_reader(entry->params_map, params,
                                 cai_tool_spooled_read, &reader, &options,
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
  return cai_tool_schema_rebuild(schema, error);
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
  cai_buffer_builder builder;
  cai_tool_buffer_sink_context sink_context;
  lonejson_writer writer;
  lonejson_error json_error;
  lonejson_status status;
  size_t i;
  int rc;

  if (values == NULL || value_count == 0U) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "string enum values are required");
  }
  memset(&builder, 0, sizeof(builder));
  sink_context.builder = &builder;
  sink_context.error = error;
  lonejson_error_init(&json_error);
  status = lonejson_writer_init_sink(&writer, cai_tool_buffer_sink,
                                     &sink_context, NULL, &json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_tool_writer_key(&writer, "type", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_tool_writer_string_cstr(&writer, "string", &json_error);
  }
  if (status == LONEJSON_STATUS_OK && description != NULL) {
    status = cai_tool_writer_key(&writer, "description", &json_error);
  }
  if (status == LONEJSON_STATUS_OK && description != NULL) {
    status = cai_tool_writer_string_cstr(&writer, description, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_tool_writer_key(&writer, "enum", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_array(&writer, &json_error);
  }
  for (i = 0U; status == LONEJSON_STATUS_OK && i < value_count; i++) {
    status = cai_tool_writer_string_cstr(&writer, values[i], &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_array(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_finish(&writer, &json_error);
  }
  lonejson_writer_cleanup(&writer);
  if (status != LONEJSON_STATUS_OK) {
    cai_free_mem(NULL, builder.data);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to serialize string enum schema",
                                json_error.message);
  }
  rc = cai_tool_schema_add_property_json(
      schema, name, builder.data != NULL ? builder.data : "{}", required,
      error);
  cai_free_mem(NULL, builder.data);
  return rc;
}

int cai_tool_schema_describe(cai_tool_schema *schema, const char *name,
                             const char *description, cai_error *error) {
  cai_tool_schema_impl *impl;
  cai_tool_schema_property *prop;
  cai_buffer_builder builder;
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
  rc = cai_tool_schema_rewrite_description(&builder, prop->property_json,
                                           description, error);
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
  cai_buffer_builder builder;
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
  rc = cai_tool_schema_rewrite_description(&builder, schema_json, description,
                                           error);
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
