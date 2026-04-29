#include "cai_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct cai_json_string_doc {
  char *value;
} cai_json_string_doc;

enum { CAI_INPUT_MESSAGE = 0, CAI_INPUT_FUNCTION_CALL_OUTPUT = 1 };

typedef struct cai_response_content_doc {
  char *type;
  char *text;
  char *refusal;
} cai_response_content_doc;

typedef struct cai_response_output_doc {
  char *id;
  char *type;
  char *call_id;
  char *name;
  char *arguments;
  lonejson_object_array content;
} cai_response_output_doc;

typedef struct cai_response_input_tokens_details_doc {
  long long cached_tokens;
} cai_response_input_tokens_details_doc;

typedef struct cai_response_output_tokens_details_doc {
  long long reasoning_tokens;
} cai_response_output_tokens_details_doc;

typedef struct cai_response_usage_doc {
  long long input_tokens;
  long long input_cached_tokens;
  long long output_tokens;
  long long output_reasoning_tokens;
  long long total_tokens;
  cai_response_input_tokens_details_doc input_tokens_details;
  cai_response_output_tokens_details_doc output_tokens_details;
} cai_response_usage_doc;

typedef struct cai_response_error_doc {
  char *code;
  char *message;
} cai_response_error_doc;

typedef struct cai_response_incomplete_doc {
  char *reason;
} cai_response_incomplete_doc;

typedef struct cai_response_conversation_doc {
  char *id;
} cai_response_conversation_doc;

typedef struct cai_response_doc {
  char *id;
  char *status;
  char *model;
  long long created_at;
  cai_response_error_doc error;
  cai_response_incomplete_doc incomplete_details;
  cai_response_conversation_doc conversation;
  cai_response_usage_doc usage;
  lonejson_object_array output;
} cai_response_doc;

static const lonejson_field cai_json_string_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_json_string_doc, value, "value")};
LONEJSON_MAP_DEFINE(cai_json_string_map, cai_json_string_doc,
                    cai_json_string_fields);

static const lonejson_field cai_response_content_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_response_content_doc, type, "type"),
    LONEJSON_FIELD_STRING_ALLOC(cai_response_content_doc, text, "text"),
    LONEJSON_FIELD_STRING_ALLOC(cai_response_content_doc, refusal, "refusal")};
LONEJSON_MAP_DEFINE(cai_response_content_map, cai_response_content_doc,
                    cai_response_content_fields);

static const lonejson_field cai_response_output_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_response_output_doc, id, "id"),
    LONEJSON_FIELD_STRING_ALLOC(cai_response_output_doc, type, "type"),
    LONEJSON_FIELD_STRING_ALLOC(cai_response_output_doc, call_id, "call_id"),
    LONEJSON_FIELD_STRING_ALLOC(cai_response_output_doc, name, "name"),
    LONEJSON_FIELD_STRING_ALLOC(cai_response_output_doc, arguments,
                                "arguments"),
    LONEJSON_FIELD_OBJECT_ARRAY(
        cai_response_output_doc, content, "content", cai_response_content_doc,
        &cai_response_content_map, LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(cai_response_output_map, cai_response_output_doc,
                    cai_response_output_fields);

static const lonejson_field cai_response_input_tokens_details_fields[] = {
    LONEJSON_FIELD_I64(cai_response_input_tokens_details_doc, cached_tokens,
                       "cached_tokens")};
LONEJSON_MAP_DEFINE(cai_response_input_tokens_details_map,
                    cai_response_input_tokens_details_doc,
                    cai_response_input_tokens_details_fields);

static const lonejson_field cai_response_output_tokens_details_fields[] = {
    LONEJSON_FIELD_I64(cai_response_output_tokens_details_doc, reasoning_tokens,
                       "reasoning_tokens")};
LONEJSON_MAP_DEFINE(cai_response_output_tokens_details_map,
                    cai_response_output_tokens_details_doc,
                    cai_response_output_tokens_details_fields);

static const lonejson_field cai_response_usage_fields[] = {
    LONEJSON_FIELD_I64(cai_response_usage_doc, input_tokens, "input_tokens"),
    LONEJSON_FIELD_I64(cai_response_usage_doc, input_cached_tokens,
                       "input_cached_tokens"),
    LONEJSON_FIELD_OBJECT(cai_response_usage_doc, input_tokens_details,
                          "input_tokens_details",
                          &cai_response_input_tokens_details_map),
    LONEJSON_FIELD_I64(cai_response_usage_doc, output_tokens, "output_tokens"),
    LONEJSON_FIELD_I64(cai_response_usage_doc, output_reasoning_tokens,
                       "output_reasoning_tokens"),
    LONEJSON_FIELD_OBJECT(cai_response_usage_doc, output_tokens_details,
                          "output_tokens_details",
                          &cai_response_output_tokens_details_map),
    LONEJSON_FIELD_I64(cai_response_usage_doc, total_tokens, "total_tokens")};
LONEJSON_MAP_DEFINE(cai_response_usage_map, cai_response_usage_doc,
                    cai_response_usage_fields);

static const lonejson_field cai_response_error_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_response_error_doc, code, "code"),
    LONEJSON_FIELD_STRING_ALLOC(cai_response_error_doc, message, "message")};
LONEJSON_MAP_DEFINE(cai_response_error_map, cai_response_error_doc,
                    cai_response_error_fields);

static const lonejson_field cai_response_incomplete_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_response_incomplete_doc, reason, "reason")};
LONEJSON_MAP_DEFINE(cai_response_incomplete_map, cai_response_incomplete_doc,
                    cai_response_incomplete_fields);

static const lonejson_field cai_response_conversation_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_response_conversation_doc, id, "id")};
LONEJSON_MAP_DEFINE(cai_response_conversation_map,
                    cai_response_conversation_doc,
                    cai_response_conversation_fields);

static const lonejson_field cai_response_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_response_doc, id, "id"),
    LONEJSON_FIELD_STRING_ALLOC(cai_response_doc, status, "status"),
    LONEJSON_FIELD_STRING_ALLOC(cai_response_doc, model, "model"),
    LONEJSON_FIELD_I64(cai_response_doc, created_at, "created_at"),
    LONEJSON_FIELD_OBJECT(cai_response_doc, error, "error",
                          &cai_response_error_map),
    LONEJSON_FIELD_OBJECT(cai_response_doc, incomplete_details,
                          "incomplete_details", &cai_response_incomplete_map),
    LONEJSON_FIELD_OBJECT(cai_response_doc, conversation, "conversation",
                          &cai_response_conversation_map),
    LONEJSON_FIELD_OBJECT(cai_response_doc, usage, "usage",
                          &cai_response_usage_map),
    LONEJSON_FIELD_OBJECT_ARRAY(
        cai_response_doc, output, "output", cai_response_output_doc,
        &cai_response_output_map, LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(cai_response_map, cai_response_doc, cai_response_fields);

static void cai_content_part_cleanup(const cai_allocator *allocator,
                                     struct cai_content_part *part) {
  if (part == NULL) {
    return;
  }
  cai_free_mem(allocator, part->type);
  cai_free_mem(allocator, part->text);
  cai_free_mem(allocator, part->image_url);
  cai_free_mem(allocator, part->detail);
}

static void cai_input_message_cleanup(const cai_allocator *allocator,
                                      struct cai_input_message *message) {
  struct cai_content_part *parts;
  size_t i;

  if (message == NULL) {
    return;
  }
  cai_free_mem(allocator, message->role);
  cai_free_mem(allocator, message->call_id);
  cai_free_mem(allocator, message->output);
  parts = (struct cai_content_part *)message->content.items;
  for (i = 0U; i < message->content.count; i++) {
    cai_content_part_cleanup(allocator, &parts[i]);
  }
  cai_free_mem(allocator, message->content.items);
}

static void cai_function_tool_cleanup(const cai_allocator *allocator,
                                      struct cai_function_tool *tool) {
  if (tool == NULL) {
    return;
  }
  cai_free_mem(allocator, tool->name);
  cai_free_mem(allocator, tool->description);
  cai_free_mem(allocator, tool->parameters_json);
}

static void cai_object_array_init(lonejson_object_array *array,
                                  size_t elem_size) {
  array->items = NULL;
  array->count = 0U;
  array->capacity = 0U;
  array->elem_size = elem_size;
  array->flags = 0U;
}

static int cai_object_array_grow(const cai_allocator *allocator,
                                 lonejson_object_array *array, size_t elem_size,
                                 cai_error *error) {
  size_t new_capacity;
  void *new_items;

  if (array->count < array->capacity) {
    return CAI_OK;
  }
  new_capacity = array->capacity == 0U ? 2U : array->capacity * 2U;
  new_items =
      cai_realloc_mem(allocator, array->items, new_capacity * elem_size);
  if (new_items == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to grow JSON array");
  }
  array->items = new_items;
  array->capacity = new_capacity;
  array->elem_size = elem_size;
  return CAI_OK;
}

static int cai_replace_string(const cai_allocator *allocator, char **slot,
                              const char *value, cai_error *error) {
  char *copy;

  copy = NULL;
  if (value != NULL) {
    copy = cai_strdup(allocator, value);
    if (copy == NULL) {
      return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate string");
    }
  }
  cai_free_mem(allocator, *slot);
  *slot = copy;
  return CAI_OK;
}

static int cai_json_builder_append(cai_json_builder *builder, const char *text,
                                   size_t length, cai_error *error) {
  size_t needed;
  size_t new_capacity;
  char *grown;

  needed = builder->length + length + 1U;
  if (needed > builder->capacity) {
    new_capacity = builder->capacity == 0U ? 256U : builder->capacity;
    while (new_capacity < needed) {
      new_capacity *= 2U;
    }
    grown = (char *)cai_realloc_mem(NULL, builder->data, new_capacity);
    if (grown == NULL) {
      return cai_set_error(error, CAI_ERR_NOMEM, "failed to grow JSON buffer");
    }
    builder->data = grown;
    builder->capacity = new_capacity;
  }
  if (length > 0U) {
    memcpy(builder->data + builder->length, text, length);
    builder->length += length;
  }
  builder->data[builder->length] = '\0';
  return CAI_OK;
}

int cai_json_builder_lit(cai_json_builder *builder, const char *text,
                         cai_error *error) {
  return cai_json_builder_append(builder, text, strlen(text), error);
}

int cai_json_builder_string(cai_json_builder *builder, const char *value,
                            cai_error *error) {
  cai_json_string_doc doc;
  lonejson_error json_error;
  char *json;
  char *colon;
  char *copy;
  size_t len;
  int rc;

  copy = cai_strdup(NULL, value);
  if (copy == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate JSON string");
  }
  doc.value = copy;
  json = lonejson_serialize_alloc(&cai_json_string_map, &doc, NULL, NULL,
                                  &json_error);
  if (json == NULL) {
    cai_free_mem(NULL, copy);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to serialize JSON string",
                                json_error.message);
  }
  colon = strchr(json, ':');
  if (colon == NULL) {
    free(json);
    cai_free_mem(NULL, copy);
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "lonejson produced an unexpected string wrapper");
  }
  colon++;
  len = strlen(colon);
  if (len == 0U || colon[len - 1U] != '}') {
    free(json);
    cai_free_mem(NULL, copy);
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "lonejson produced an unexpected string wrapper");
  }
  len--;
  rc = cai_json_builder_append(builder, colon, len, error);
  free(json);
  cai_free_mem(NULL, copy);
  return rc;
}

int cai_json_builder_field_string(cai_json_builder *builder, const char *name,
                                  const char *value, int *need_comma,
                                  cai_error *error) {
  int rc;

  if (*need_comma) {
    rc = cai_json_builder_lit(builder, ",", error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  rc = cai_json_builder_string(builder, name, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_json_builder_lit(builder, ":", error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_json_builder_string(builder, value, error);
  if (rc == CAI_OK) {
    *need_comma = 1;
  }
  return rc;
}

static int cai_json_builder_field_int(cai_json_builder *builder,
                                      const char *name, int value,
                                      int *need_comma, cai_error *error) {
  char buffer[32];
  int rc;

  if (*need_comma) {
    rc = cai_json_builder_lit(builder, ",", error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  rc = cai_json_builder_string(builder, name, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_json_builder_lit(builder, ":", error);
  if (rc != CAI_OK) {
    return rc;
  }
  snprintf(buffer, sizeof(buffer), "%d", value);
  rc = cai_json_builder_lit(builder, buffer, error);
  if (rc == CAI_OK) {
    *need_comma = 1;
  }
  return rc;
}

static int cai_json_builder_field_bool(cai_json_builder *builder,
                                       const char *name, int value,
                                       int *need_comma, cai_error *error) {
  int rc;

  if (*need_comma) {
    rc = cai_json_builder_lit(builder, ",", error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  rc = cai_json_builder_string(builder, name, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_json_builder_lit(builder, value ? ":true" : ":false", error);
  if (rc == CAI_OK) {
    *need_comma = 1;
  }
  return rc;
}

static int
cai_serialize_reasoning_json(cai_json_builder *builder,
                             const cai_response_create_params *params,
                             int *need_comma, cai_error *error) {
  int reasoning_comma;
  int rc;

  if (params->reasoning_effort == NULL && params->reasoning_summary == NULL) {
    return CAI_OK;
  }
  if (*need_comma) {
    rc = cai_json_builder_lit(builder, ",", error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  rc = cai_json_builder_string(builder, "reasoning", error);
  if (rc == CAI_OK) {
    rc = cai_json_builder_lit(builder, ":{", error);
  }
  reasoning_comma = 0;
  if (rc == CAI_OK && params->reasoning_effort != NULL) {
    rc = cai_json_builder_field_string(
        builder, "effort", params->reasoning_effort, &reasoning_comma, error);
  }
  if (rc == CAI_OK && params->reasoning_summary != NULL) {
    rc = cai_json_builder_field_string(
        builder, "summary", params->reasoning_summary, &reasoning_comma, error);
  }
  if (rc == CAI_OK) {
    rc = cai_json_builder_lit(builder, "}", error);
  }
  if (rc == CAI_OK) {
    *need_comma = 1;
  }
  return rc;
}

static int
cai_serialize_text_format_json(cai_json_builder *builder,
                               const cai_response_create_params *params,
                               int *need_comma, cai_error *error) {
  int format_comma;
  int rc;

  if (params->text_format_type == NULL) {
    return CAI_OK;
  }
  if (*need_comma) {
    rc = cai_json_builder_lit(builder, ",", error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  rc = cai_json_builder_lit(builder, "\"text\":{\"format\":{", error);
  format_comma = 0;
  if (rc == CAI_OK) {
    rc = cai_json_builder_field_string(
        builder, "type", params->text_format_type, &format_comma, error);
  }
  if (rc == CAI_OK && params->text_format_name != NULL) {
    rc = cai_json_builder_field_string(
        builder, "name", params->text_format_name, &format_comma, error);
  }
  if (rc == CAI_OK && params->text_format_description != NULL) {
    rc = cai_json_builder_field_string(builder, "description",
                                       params->text_format_description,
                                       &format_comma, error);
  }
  if (rc == CAI_OK && params->text_format_schema_json != NULL) {
    if (format_comma) {
      rc = cai_json_builder_lit(builder, ",", error);
    }
    if (rc == CAI_OK) {
      rc = cai_json_builder_lit(builder, "\"schema\":", error);
    }
    if (rc == CAI_OK) {
      rc =
          cai_json_builder_lit(builder, params->text_format_schema_json, error);
    }
    format_comma = 1;
  }
  if (rc == CAI_OK && params->text_format_schema_json != NULL) {
    rc = cai_json_builder_field_bool(
        builder, "strict", params->text_format_strict, &format_comma, error);
  }
  if (rc == CAI_OK) {
    rc = cai_json_builder_lit(builder, "}}", error);
  }
  if (rc == CAI_OK) {
    *need_comma = 1;
  }
  return rc;
}

int cai_response_create_params_new(cai_response_create_params **out,
                                   cai_error *error) {
  cai_response_create_params *params;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params output pointer is required");
  }
  *out = NULL;
  params = (cai_response_create_params *)cai_alloc(NULL, sizeof(*params));
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate response params");
  }
  params->allocator.malloc_fn = NULL;
  params->allocator.realloc_fn = NULL;
  params->allocator.free_fn = NULL;
  params->allocator.context = NULL;
  params->model = NULL;
  params->conversation_id = NULL;
  params->instructions = NULL;
  params->previous_response_id = NULL;
  params->reasoning_effort = NULL;
  params->reasoning_summary = NULL;
  params->text_format_type = NULL;
  params->text_format_name = NULL;
  params->text_format_description = NULL;
  params->text_format_schema_json = NULL;
  params->text_format_strict = 0;
  params->max_output_tokens = 0;
  params->parallel_tool_calls = -1;
  cai_object_array_init(&params->input, sizeof(struct cai_input_message));
  cai_object_array_init(&params->tools, sizeof(struct cai_function_tool));
  *out = params;
  return CAI_OK;
}

void cai_response_create_params_destroy(cai_response_create_params *params) {
  struct cai_input_message *messages;
  struct cai_function_tool *tools;
  size_t i;

  if (params == NULL) {
    return;
  }
  cai_free_mem(&params->allocator, params->model);
  cai_free_mem(&params->allocator, params->conversation_id);
  cai_free_mem(&params->allocator, params->instructions);
  cai_free_mem(&params->allocator, params->previous_response_id);
  cai_free_mem(&params->allocator, params->reasoning_effort);
  cai_free_mem(&params->allocator, params->reasoning_summary);
  cai_free_mem(&params->allocator, params->text_format_type);
  cai_free_mem(&params->allocator, params->text_format_name);
  cai_free_mem(&params->allocator, params->text_format_description);
  cai_free_mem(&params->allocator, params->text_format_schema_json);
  messages = (struct cai_input_message *)params->input.items;
  for (i = 0U; i < params->input.count; i++) {
    cai_input_message_cleanup(&params->allocator, &messages[i]);
  }
  tools = (struct cai_function_tool *)params->tools.items;
  for (i = 0U; i < params->tools.count; i++) {
    cai_function_tool_cleanup(&params->allocator, &tools[i]);
  }
  cai_free_mem(&params->allocator, params->input.items);
  cai_free_mem(&params->allocator, params->tools.items);
  cai_free_mem(&params->allocator, params);
}

int cai_response_create_params_set_model(cai_response_create_params *params,
                                         const char *model, cai_error *error) {
  if (params == NULL || model == NULL || model[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "model is required");
  }
  return cai_replace_string(&params->allocator, &params->model, model, error);
}

int cai_response_create_params_set_instructions(
    cai_response_create_params *params, const char *instructions,
    cai_error *error) {
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  return cai_replace_string(&params->allocator, &params->instructions,
                            instructions, error);
}

int cai_response_create_params_set_previous_response_id(
    cai_response_create_params *params, const char *response_id,
    cai_error *error) {
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  return cai_replace_string(&params->allocator, &params->previous_response_id,
                            response_id, error);
}

int cai_response_create_params_set_conversation_id(
    cai_response_create_params *params, const char *conversation_id,
    cai_error *error) {
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  return cai_replace_string(&params->allocator, &params->conversation_id,
                            conversation_id, error);
}

int cai_response_create_params_set_max_output_tokens(
    cai_response_create_params *params, int max_output_tokens,
    cai_error *error) {
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  if (max_output_tokens < 0) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "max output tokens cannot be negative");
  }
  params->max_output_tokens = max_output_tokens;
  return CAI_OK;
}

int cai_response_create_params_set_reasoning(cai_response_create_params *params,
                                             const char *effort,
                                             const char *summary,
                                             cai_error *error) {
  int rc;

  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  rc = cai_replace_string(&params->allocator, &params->reasoning_effort, effort,
                          error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_replace_string(&params->allocator, &params->reasoning_summary,
                            summary, error);
}

int cai_response_create_params_set_parallel_tool_calls(
    cai_response_create_params *params, int enabled, cai_error *error) {
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  if (enabled != 0 && enabled != 1) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "parallel tool calls must be 0 or 1");
  }
  params->parallel_tool_calls = enabled;
  return CAI_OK;
}

static int
cai_response_params_set_text_format_type(cai_response_create_params *params,
                                         const char *type, cai_error *error) {
  int rc;

  rc = cai_replace_string(&params->allocator, &params->text_format_type, type,
                          error);
  if (rc == CAI_OK && strcmp(type, "json_object") == 0) {
    cai_free_mem(&params->allocator, params->text_format_name);
    cai_free_mem(&params->allocator, params->text_format_description);
    cai_free_mem(&params->allocator, params->text_format_schema_json);
    params->text_format_name = NULL;
    params->text_format_description = NULL;
    params->text_format_schema_json = NULL;
    params->text_format_strict = 0;
  }
  return rc;
}

int cai_response_create_params_set_text_format_json_object(
    cai_response_create_params *params, cai_error *error) {
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  return cai_response_params_set_text_format_type(params, "json_object", error);
}

int cai_response_create_params_set_text_format_json_schema(
    cai_response_create_params *params, const char *name,
    const char *description, const char *schema_json, int strict,
    cai_error *error) {
  int rc;

  if (params == NULL || name == NULL || name[0] == '\0' ||
      schema_json == NULL || schema_json[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "text format name and schema are required");
  }
  rc = cai_response_params_set_text_format_type(params, "json_schema", error);
  if (rc == CAI_OK) {
    rc = cai_replace_string(&params->allocator, &params->text_format_name, name,
                            error);
  }
  if (rc == CAI_OK) {
    rc =
        cai_replace_string(&params->allocator, &params->text_format_description,
                           description, error);
  }
  if (rc == CAI_OK) {
    rc =
        cai_replace_string(&params->allocator, &params->text_format_schema_json,
                           schema_json, error);
  }
  if (rc == CAI_OK) {
    params->text_format_strict = strict ? 1 : 0;
  }
  return rc;
}

static int cai_response_params_add_part(cai_response_create_params *params,
                                        const char *role,
                                        struct cai_content_part *part,
                                        cai_error *error) {
  struct cai_input_message *messages;
  struct cai_input_message *message;
  struct cai_content_part *parts;
  int rc;

  if (params == NULL || role == NULL || role[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "role is required");
  }
  rc = cai_object_array_grow(&params->allocator, &params->input,
                             sizeof(struct cai_input_message), error);
  if (rc != CAI_OK) {
    return rc;
  }
  messages = (struct cai_input_message *)params->input.items;
  message = &messages[params->input.count];
  memset(message, 0, sizeof(*message));
  message->kind = CAI_INPUT_MESSAGE;
  message->role = NULL;
  cai_object_array_init(&message->content, sizeof(struct cai_content_part));
  message->role = cai_strdup(&params->allocator, role);
  if (message->role == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate role");
  }
  rc = cai_object_array_grow(&params->allocator, &message->content,
                             sizeof(struct cai_content_part), error);
  if (rc != CAI_OK) {
    cai_input_message_cleanup(&params->allocator, message);
    return rc;
  }
  parts = (struct cai_content_part *)message->content.items;
  parts[0] = *part;
  message->content.count = 1U;
  params->input.count++;
  return CAI_OK;
}

int cai_response_create_params_add_text(cai_response_create_params *params,
                                        const char *role, const char *text,
                                        cai_error *error) {
  struct cai_content_part part;
  int rc;

  if (text == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "text is required");
  }
  part.type =
      cai_strdup(params != NULL ? &params->allocator : NULL, "input_text");
  part.text = cai_strdup(params != NULL ? &params->allocator : NULL, text);
  part.image_url = NULL;
  part.detail = NULL;
  if (part.type == NULL || part.text == NULL) {
    cai_content_part_cleanup(params != NULL ? &params->allocator : NULL, &part);
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate text input");
  }
  rc = cai_response_params_add_part(params, role, &part, error);
  if (rc != CAI_OK) {
    cai_content_part_cleanup(params != NULL ? &params->allocator : NULL, &part);
  }
  return rc;
}

int cai_response_create_params_add_image_url(cai_response_create_params *params,
                                             const char *role, const char *url,
                                             const char *detail,
                                             cai_error *error) {
  struct cai_content_part part;
  const cai_allocator *allocator;
  int rc;

  if (url == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "image URL is required");
  }
  allocator = params != NULL ? &params->allocator : NULL;
  part.type = cai_strdup(allocator, "input_image");
  part.text = NULL;
  part.image_url = cai_strdup(allocator, url);
  part.detail = cai_strdup(allocator, detail);
  if (part.type == NULL || part.image_url == NULL ||
      (detail != NULL && part.detail == NULL)) {
    cai_content_part_cleanup(allocator, &part);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate image input");
  }
  rc = cai_response_params_add_part(params, role, &part, error);
  if (rc != CAI_OK) {
    cai_content_part_cleanup(allocator, &part);
  }
  return rc;
}

int cai_response_create_params_add_function_tool(
    cai_response_create_params *params, const char *name,
    const char *description, const char *parameters_json, int strict,
    cai_error *error) {
  struct cai_function_tool *tools;
  struct cai_function_tool *tool;
  int rc;

  if (params == NULL || name == NULL || name[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "tool name is required");
  }
  rc = cai_object_array_grow(&params->allocator, &params->tools,
                             sizeof(struct cai_function_tool), error);
  if (rc != CAI_OK) {
    return rc;
  }
  tools = (struct cai_function_tool *)params->tools.items;
  tool = &tools[params->tools.count];
  tool->name = cai_strdup(&params->allocator, name);
  tool->description = cai_strdup(&params->allocator, description);
  tool->parameters_json = cai_strdup(
      &params->allocator, parameters_json != NULL ? parameters_json : "{}");
  tool->strict = strict ? 1 : 0;
  if (tool->name == NULL ||
      (description != NULL && tool->description == NULL) ||
      tool->parameters_json == NULL) {
    cai_function_tool_cleanup(&params->allocator, tool);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate function tool");
  }
  params->tools.count++;
  return CAI_OK;
}

int cai_response_create_params_add_function_call_output(
    cai_response_create_params *params, const char *call_id, const char *output,
    cai_error *error) {
  struct cai_input_message *messages;
  struct cai_input_message *message;
  int rc;

  if (params == NULL || call_id == NULL || call_id[0] == '\0' ||
      output == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "function call id and output are required");
  }
  rc = cai_object_array_grow(&params->allocator, &params->input,
                             sizeof(struct cai_input_message), error);
  if (rc != CAI_OK) {
    return rc;
  }
  messages = (struct cai_input_message *)params->input.items;
  message = &messages[params->input.count];
  memset(message, 0, sizeof(*message));
  message->kind = CAI_INPUT_FUNCTION_CALL_OUTPUT;
  cai_object_array_init(&message->content, sizeof(struct cai_content_part));
  message->call_id = cai_strdup(&params->allocator, call_id);
  message->output = cai_strdup(&params->allocator, output);
  if (message->call_id == NULL || message->output == NULL) {
    cai_input_message_cleanup(&params->allocator, message);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate function call output");
  }
  params->input.count++;
  return CAI_OK;
}

static int cai_serialize_function_tools_json(cai_json_builder *builder,
                                             const lonejson_object_array *array,
                                             cai_error *error) {
  struct cai_function_tool *tools;
  size_t i;
  int need_comma;
  int rc;

  rc = cai_json_builder_lit(builder, "\"tools\":[", error);
  tools = (struct cai_function_tool *)array->items;
  for (i = 0U; rc == CAI_OK && i < array->count; i++) {
    if (i > 0U) {
      rc = cai_json_builder_lit(builder, ",", error);
    }
    need_comma = 0;
    if (rc == CAI_OK) {
      rc = cai_json_builder_lit(builder, "{", error);
    }
    if (rc == CAI_OK) {
      rc = cai_json_builder_field_string(builder, "type", "function",
                                         &need_comma, error);
    }
    if (rc == CAI_OK) {
      rc = cai_json_builder_field_string(builder, "name", tools[i].name,
                                         &need_comma, error);
    }
    if (rc == CAI_OK && tools[i].description != NULL) {
      rc = cai_json_builder_field_string(
          builder, "description", tools[i].description, &need_comma, error);
    }
    if (rc == CAI_OK && need_comma) {
      rc = cai_json_builder_lit(builder, ",", error);
    }
    if (rc == CAI_OK) {
      rc = cai_json_builder_lit(builder, "\"parameters\":", error);
    }
    if (rc == CAI_OK) {
      rc = cai_json_builder_lit(builder, tools[i].parameters_json, error);
    }
    if (rc == CAI_OK) {
      rc = cai_json_builder_lit(
          builder, tools[i].strict ? ",\"strict\":true" : ",\"strict\":false",
          error);
    }
    if (rc == CAI_OK) {
      rc = cai_json_builder_lit(builder, "}", error);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_json_builder_lit(builder, "]", error);
  }
  return rc;
}

int cai_serialize_input_messages_json(cai_json_builder *builder,
                                      const char *field_name,
                                      const lonejson_object_array *input,
                                      cai_error *error) {
  struct cai_input_message *messages;
  struct cai_content_part *parts;
  size_t i;
  size_t j;
  int part_comma;
  int rc;

  if (input == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "input messages are required");
  }
  rc = cai_json_builder_string(builder, field_name, error);
  if (rc == CAI_OK) {
    rc = cai_json_builder_lit(builder, ":[", error);
  }
  messages = (struct cai_input_message *)input->items;
  for (i = 0U; rc == CAI_OK && i < input->count; i++) {
    if (i > 0U) {
      rc = cai_json_builder_lit(builder, ",", error);
    }
    if (messages[i].kind == CAI_INPUT_FUNCTION_CALL_OUTPUT) {
      int need_comma;

      need_comma = 0;
      if (rc == CAI_OK) {
        rc = cai_json_builder_lit(builder, "{", error);
      }
      if (rc == CAI_OK) {
        rc = cai_json_builder_field_string(
            builder, "type", "function_call_output", &need_comma, error);
      }
      if (rc == CAI_OK) {
        rc = cai_json_builder_field_string(
            builder, "call_id", messages[i].call_id, &need_comma, error);
      }
      if (rc == CAI_OK) {
        rc = cai_json_builder_field_string(
            builder, "output", messages[i].output, &need_comma, error);
      }
      if (rc == CAI_OK) {
        rc = cai_json_builder_lit(builder, "}", error);
      }
      continue;
    }
    if (rc == CAI_OK) {
      rc = cai_json_builder_lit(builder, "{\"role\":", error);
    }
    if (rc == CAI_OK) {
      rc = cai_json_builder_string(builder, messages[i].role, error);
    }
    if (rc == CAI_OK) {
      rc = cai_json_builder_lit(builder, ",\"content\":[", error);
    }
    parts = (struct cai_content_part *)messages[i].content.items;
    for (j = 0U; rc == CAI_OK && j < messages[i].content.count; j++) {
      if (j > 0U) {
        rc = cai_json_builder_lit(builder, ",", error);
      }
      part_comma = 0;
      if (rc == CAI_OK) {
        rc = cai_json_builder_lit(builder, "{", error);
      }
      if (rc == CAI_OK) {
        rc = cai_json_builder_field_string(builder, "type", parts[j].type,
                                           &part_comma, error);
      }
      if (rc == CAI_OK && parts[j].text != NULL) {
        rc = cai_json_builder_field_string(builder, "text", parts[j].text,
                                           &part_comma, error);
      }
      if (rc == CAI_OK && parts[j].image_url != NULL) {
        rc = cai_json_builder_field_string(
            builder, "image_url", parts[j].image_url, &part_comma, error);
      }
      if (rc == CAI_OK && parts[j].detail != NULL) {
        rc = cai_json_builder_field_string(builder, "detail", parts[j].detail,
                                           &part_comma, error);
      }
      if (rc == CAI_OK) {
        rc = cai_json_builder_lit(builder, "}", error);
      }
    }
    if (rc == CAI_OK) {
      rc = cai_json_builder_lit(builder, "]}", error);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_json_builder_lit(builder, "]", error);
  }
  return rc;
}

int cai_response_create_params_serialize_json(
    const cai_response_create_params *params, char **out_json, size_t *out_len,
    cai_error *error) {
  cai_json_builder builder;
  int need_comma;
  int rc;

  if (out_json == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "JSON output pointer is required");
  }
  *out_json = NULL;
  if (params == NULL || params->model == NULL || params->input.count == 0U) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "model and at least one input message are required");
  }
  builder.data = NULL;
  builder.length = 0U;
  builder.capacity = 0U;
  need_comma = 0;
  rc = cai_json_builder_lit(&builder, "{", error);
  if (rc == CAI_OK) {
    rc = cai_json_builder_field_string(&builder, "model", params->model,
                                       &need_comma, error);
  }
  if (rc == CAI_OK && params->instructions != NULL) {
    rc = cai_json_builder_field_string(
        &builder, "instructions", params->instructions, &need_comma, error);
  }
  if (rc == CAI_OK && params->previous_response_id != NULL) {
    rc = cai_json_builder_field_string(&builder, "previous_response_id",
                                       params->previous_response_id,
                                       &need_comma, error);
  }
  if (rc == CAI_OK && params->conversation_id != NULL) {
    rc = cai_json_builder_field_string(
        &builder, "conversation", params->conversation_id, &need_comma, error);
  }
  if (rc == CAI_OK && params->max_output_tokens > 0) {
    rc = cai_json_builder_field_int(&builder, "max_output_tokens",
                                    params->max_output_tokens, &need_comma,
                                    error);
  }
  if (rc == CAI_OK) {
    rc = cai_serialize_reasoning_json(&builder, params, &need_comma, error);
  }
  if (rc == CAI_OK) {
    rc = cai_serialize_text_format_json(&builder, params, &need_comma, error);
  }
  if (rc == CAI_OK && params->parallel_tool_calls >= 0) {
    rc = cai_json_builder_field_bool(&builder, "parallel_tool_calls",
                                     params->parallel_tool_calls, &need_comma,
                                     error);
  }
  if (rc == CAI_OK && need_comma) {
    rc = cai_json_builder_lit(&builder, ",", error);
  }
  rc = cai_serialize_input_messages_json(&builder, "input", &params->input,
                                         error);
  if (rc == CAI_OK && params->tools.count > 0U) {
    rc = cai_json_builder_lit(&builder, ",", error);
  }
  if (rc == CAI_OK && params->tools.count > 0U) {
    rc = cai_serialize_function_tools_json(&builder, &params->tools, error);
  }
  if (rc == CAI_OK) {
    rc = cai_json_builder_lit(&builder, "}", error);
  }
  if (rc != CAI_OK) {
    cai_free_mem(NULL, builder.data);
    return rc;
  }
  if (out_len != NULL) {
    *out_len = builder.length;
  }
  *out_json = builder.data;
  return CAI_OK;
}

static char *cai_response_collect_text(cai_response_doc *doc) {
  cai_response_output_doc *outputs;
  cai_response_content_doc *content;
  size_t total;
  size_t i;
  size_t j;
  char *text;
  char *cursor;
  size_t len;

  total = 0U;
  outputs = (cai_response_output_doc *)doc->output.items;
  for (i = 0U; i < doc->output.count; i++) {
    content = (cai_response_content_doc *)outputs[i].content.items;
    for (j = 0U; j < outputs[i].content.count; j++) {
      if (content[j].text != NULL) {
        total += strlen(content[j].text);
      }
    }
  }
  text = (char *)cai_alloc(NULL, total + 1U);
  if (text == NULL) {
    return NULL;
  }
  cursor = text;
  for (i = 0U; i < doc->output.count; i++) {
    content = (cai_response_content_doc *)outputs[i].content.items;
    for (j = 0U; j < outputs[i].content.count; j++) {
      if (content[j].text != NULL) {
        len = strlen(content[j].text);
        if (len > 0U) {
          memcpy(cursor, content[j].text, len);
          cursor += len;
        }
      }
    }
  }
  *cursor = '\0';
  return text;
}

static char *cai_response_collect_refusal(cai_response_doc *doc, int *present) {
  cai_response_output_doc *outputs;
  cai_response_content_doc *content;
  size_t total;
  size_t i;
  size_t j;
  char *text;
  char *cursor;
  size_t len;

  *present = 0;
  total = 0U;
  outputs = (cai_response_output_doc *)doc->output.items;
  for (i = 0U; i < doc->output.count; i++) {
    content = (cai_response_content_doc *)outputs[i].content.items;
    for (j = 0U; j < outputs[i].content.count; j++) {
      if (content[j].refusal != NULL) {
        *present = 1;
        total += strlen(content[j].refusal);
      }
    }
  }
  if (!*present) {
    return NULL;
  }
  text = (char *)cai_alloc(NULL, total + 1U);
  if (text == NULL) {
    return NULL;
  }
  cursor = text;
  for (i = 0U; i < doc->output.count; i++) {
    content = (cai_response_content_doc *)outputs[i].content.items;
    for (j = 0U; j < outputs[i].content.count; j++) {
      if (content[j].refusal != NULL) {
        len = strlen(content[j].refusal);
        if (len > 0U) {
          memcpy(cursor, content[j].refusal, len);
          cursor += len;
        }
      }
    }
  }
  *cursor = '\0';
  return text;
}

static size_t cai_response_count_tool_calls(cai_response_doc *doc) {
  cai_response_output_doc *outputs;
  size_t count;
  size_t i;

  count = 0U;
  outputs = (cai_response_output_doc *)doc->output.items;
  for (i = 0U; i < doc->output.count; i++) {
    if (outputs[i].type != NULL &&
        strcmp(outputs[i].type, "function_call") == 0) {
      count++;
    }
  }
  return count;
}

static int cai_response_copy_tool_calls(cai_response *response,
                                        cai_response_doc *doc,
                                        cai_error *error) {
  cai_response_output_doc *outputs;
  size_t index;
  size_t i;

  response->tool_call_count = cai_response_count_tool_calls(doc);
  response->tool_calls = NULL;
  if (response->tool_call_count == 0U) {
    return CAI_OK;
  }
  response->tool_calls = (cai_response_tool_call *)cai_alloc(
      NULL, response->tool_call_count * sizeof(response->tool_calls[0]));
  if (response->tool_calls == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate response tool calls");
  }
  memset(response->tool_calls, 0,
         response->tool_call_count * sizeof(response->tool_calls[0]));
  outputs = (cai_response_output_doc *)doc->output.items;
  index = 0U;
  for (i = 0U; i < doc->output.count; i++) {
    if (outputs[i].type == NULL ||
        strcmp(outputs[i].type, "function_call") != 0) {
      continue;
    }
    response->tool_calls[index].id = cai_strdup(NULL, outputs[i].id);
    response->tool_calls[index].call_id = cai_strdup(NULL, outputs[i].call_id);
    response->tool_calls[index].name = cai_strdup(NULL, outputs[i].name);
    response->tool_calls[index].arguments =
        cai_strdup(NULL, outputs[i].arguments);
    if ((outputs[i].id != NULL && response->tool_calls[index].id == NULL) ||
        (outputs[i].call_id != NULL &&
         response->tool_calls[index].call_id == NULL) ||
        (outputs[i].name != NULL && response->tool_calls[index].name == NULL) ||
        (outputs[i].arguments != NULL &&
         response->tool_calls[index].arguments == NULL)) {
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate response tool call fields");
    }
    index++;
  }
  return CAI_OK;
}

int cai_response_parse_json(const char *json, cai_response **out,
                            cai_error *error) {
  cai_response_doc doc;
  cai_response *response;
  lonejson_error json_error;
  lonejson_status status;
  int refusal_present;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response output pointer is required");
  }
  *out = NULL;
  if (json == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "response JSON is required");
  }
  lonejson_init(&cai_response_map, &doc);
  status =
      lonejson_parse_cstr(&cai_response_map, &doc, json, NULL, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    lonejson_cleanup(&cai_response_map, &doc);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to parse response JSON",
                                json_error.message);
  }
  if (doc.id == NULL || doc.status == NULL) {
    lonejson_cleanup(&cai_response_map, &doc);
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "response JSON is missing id or status");
  }
  response = (cai_response *)cai_alloc(NULL, sizeof(*response));
  if (response == NULL) {
    lonejson_cleanup(&cai_response_map, &doc);
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate response");
  }
  response->raw_json = NULL;
  response->id = cai_strdup(NULL, doc.id);
  response->status = cai_strdup(NULL, doc.status);
  response->model = cai_strdup(NULL, doc.model);
  response->conversation_id = cai_strdup(NULL, doc.conversation.id);
  response->output_text = cai_response_collect_text(&doc);
  response->refusal = cai_response_collect_refusal(&doc, &refusal_present);
  response->raw_json = cai_strdup(NULL, json);
  response->error_code = cai_strdup(NULL, doc.error.code);
  response->error_message = cai_strdup(NULL, doc.error.message);
  response->incomplete_reason = cai_strdup(NULL, doc.incomplete_details.reason);
  response->created_at = doc.created_at;
  response->input_tokens = doc.usage.input_tokens;
  response->input_cached_tokens =
      doc.usage.input_cached_tokens != 0LL
          ? doc.usage.input_cached_tokens
          : doc.usage.input_tokens_details.cached_tokens;
  response->output_tokens = doc.usage.output_tokens;
  response->output_reasoning_tokens =
      doc.usage.output_reasoning_tokens != 0LL
          ? doc.usage.output_reasoning_tokens
          : doc.usage.output_tokens_details.reasoning_tokens;
  response->total_tokens = doc.usage.total_tokens;
  response->tool_calls = NULL;
  response->tool_call_count = 0U;
  if (response->id == NULL || response->status == NULL ||
      (doc.model != NULL && response->model == NULL) ||
      (doc.conversation.id != NULL && response->conversation_id == NULL) ||
      response->output_text == NULL || response->raw_json == NULL ||
      (refusal_present && response->refusal == NULL) ||
      (doc.error.code != NULL && response->error_code == NULL) ||
      (doc.error.message != NULL && response->error_message == NULL) ||
      (doc.incomplete_details.reason != NULL &&
       response->incomplete_reason == NULL)) {
    cai_response_destroy(response);
    lonejson_cleanup(&cai_response_map, &doc);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate parsed response");
  }
  if (cai_response_copy_tool_calls(response, &doc, error) != CAI_OK) {
    cai_response_destroy(response);
    lonejson_cleanup(&cai_response_map, &doc);
    return error != NULL ? error->code : CAI_ERR_NOMEM;
  }
  lonejson_cleanup(&cai_response_map, &doc);
  *out = response;
  return CAI_OK;
}

const char *cai_response_id(const cai_response *response) {
  return response != NULL ? response->id : NULL;
}

const char *cai_response_status(const cai_response *response) {
  return response != NULL ? response->status : NULL;
}

const char *cai_response_model(const cai_response *response) {
  return response != NULL ? response->model : NULL;
}

const char *cai_response_conversation_id(const cai_response *response) {
  return response != NULL ? response->conversation_id : NULL;
}

long long cai_response_created_at(const cai_response *response) {
  return response != NULL ? response->created_at : 0LL;
}

const char *cai_response_output_text(const cai_response *response) {
  return response != NULL ? response->output_text : NULL;
}

const char *cai_response_refusal(const cai_response *response) {
  return response != NULL ? response->refusal : NULL;
}

int cai_response_write_output_text(const cai_response *response, cai_sink *sink,
                                   cai_error *error) {
  size_t length;

  if (response == NULL || sink == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response and sink are required");
  }
  if (response->output_text == NULL) {
    return CAI_OK;
  }
  length = strlen(response->output_text);
  if (length == 0U) {
    return CAI_OK;
  }
  return cai_sink_write(sink, response->output_text, length, error);
}

int cai_response_write_refusal(const cai_response *response, cai_sink *sink,
                               cai_error *error) {
  size_t length;

  if (response == NULL || sink == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response and sink are required");
  }
  if (response->refusal == NULL) {
    return CAI_OK;
  }
  length = strlen(response->refusal);
  if (length == 0U) {
    return CAI_OK;
  }
  return cai_sink_write(sink, response->refusal, length, error);
}

const char *cai_response_raw_json(const cai_response *response) {
  return response != NULL ? response->raw_json : NULL;
}

const char *cai_response_error_code(const cai_response *response) {
  return response != NULL ? response->error_code : NULL;
}

const char *cai_response_error_message(const cai_response *response) {
  return response != NULL ? response->error_message : NULL;
}

const char *cai_response_incomplete_reason(const cai_response *response) {
  return response != NULL ? response->incomplete_reason : NULL;
}

long long cai_response_input_tokens(const cai_response *response) {
  return response != NULL ? response->input_tokens : 0LL;
}

long long cai_response_input_cached_tokens(const cai_response *response) {
  return response != NULL ? response->input_cached_tokens : 0LL;
}

long long cai_response_output_tokens(const cai_response *response) {
  return response != NULL ? response->output_tokens : 0LL;
}

long long cai_response_output_reasoning_tokens(const cai_response *response) {
  return response != NULL ? response->output_reasoning_tokens : 0LL;
}

long long cai_response_total_tokens(const cai_response *response) {
  return response != NULL ? response->total_tokens : 0LL;
}

int cai_response_usage(const cai_response *response, cai_token_usage *out,
                       cai_error *error) {
  if (response == NULL || out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response and usage output are required");
  }
  out->input_tokens = response->input_tokens;
  out->input_cached_tokens = response->input_cached_tokens;
  out->output_tokens = response->output_tokens;
  out->output_reasoning_tokens = response->output_reasoning_tokens;
  out->total_tokens = response->total_tokens;
  return CAI_OK;
}

size_t cai_response_tool_call_count(const cai_response *response) {
  return response != NULL ? response->tool_call_count : 0U;
}

const char *cai_response_tool_call_id(const cai_response *response,
                                      size_t index) {
  if (response == NULL || index >= response->tool_call_count) {
    return NULL;
  }
  return response->tool_calls[index].call_id != NULL
             ? response->tool_calls[index].call_id
             : response->tool_calls[index].id;
}

const char *cai_response_tool_call_name(const cai_response *response,
                                        size_t index) {
  if (response == NULL || index >= response->tool_call_count) {
    return NULL;
  }
  return response->tool_calls[index].name;
}

const char *cai_response_tool_call_arguments(const cai_response *response,
                                             size_t index) {
  if (response == NULL || index >= response->tool_call_count) {
    return NULL;
  }
  return response->tool_calls[index].arguments;
}

void cai_response_destroy(cai_response *response) {
  size_t i;

  if (response == NULL) {
    return;
  }
  for (i = 0U; i < response->tool_call_count; i++) {
    cai_free_mem(NULL, response->tool_calls[i].id);
    cai_free_mem(NULL, response->tool_calls[i].call_id);
    cai_free_mem(NULL, response->tool_calls[i].name);
    cai_free_mem(NULL, response->tool_calls[i].arguments);
  }
  cai_free_mem(NULL, response->tool_calls);
  cai_free_mem(NULL, response->id);
  cai_free_mem(NULL, response->status);
  cai_free_mem(NULL, response->model);
  cai_free_mem(NULL, response->conversation_id);
  cai_free_mem(NULL, response->output_text);
  cai_free_mem(NULL, response->refusal);
  cai_free_mem(NULL, response->raw_json);
  cai_free_mem(NULL, response->error_code);
  cai_free_mem(NULL, response->error_message);
  cai_free_mem(NULL, response->incomplete_reason);
  cai_free_mem(NULL, response);
}
