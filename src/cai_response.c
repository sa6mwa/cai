#include "cai_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct cai_json_string_doc {
  char *value;
} cai_json_string_doc;

typedef struct cai_response_content_doc {
  char *type;
  char *text;
} cai_response_content_doc;

typedef struct cai_response_output_doc {
  char *type;
  lonejson_object_array content;
} cai_response_output_doc;

typedef struct cai_response_doc {
  char *id;
  char *status;
  lonejson_object_array output;
} cai_response_doc;

static const lonejson_field cai_json_string_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_json_string_doc, value, "value")};
LONEJSON_MAP_DEFINE(cai_json_string_map, cai_json_string_doc,
                    cai_json_string_fields);

static const lonejson_field cai_response_content_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_response_content_doc, type, "type"),
    LONEJSON_FIELD_STRING_ALLOC(cai_response_content_doc, text, "text")};
LONEJSON_MAP_DEFINE(cai_response_content_map, cai_response_content_doc,
                    cai_response_content_fields);

static const lonejson_field cai_response_output_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_response_output_doc, type, "type"),
    LONEJSON_FIELD_OBJECT_ARRAY(
        cai_response_output_doc, content, "content", cai_response_content_doc,
        &cai_response_content_map, LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(cai_response_output_map, cai_response_output_doc,
                    cai_response_output_fields);

static const lonejson_field cai_response_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_response_doc, id, "id"),
    LONEJSON_FIELD_STRING_ALLOC(cai_response_doc, status, "status"),
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
  parts = (struct cai_content_part *)message->content.items;
  for (i = 0U; i < message->content.count; i++) {
    cai_content_part_cleanup(allocator, &parts[i]);
  }
  cai_free_mem(allocator, message->content.items);
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

typedef struct cai_json_builder {
  char *data;
  size_t length;
  size_t capacity;
} cai_json_builder;

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

static int cai_json_builder_lit(cai_json_builder *builder, const char *text,
                                cai_error *error) {
  return cai_json_builder_append(builder, text, strlen(text), error);
}

static int cai_json_builder_string(cai_json_builder *builder, const char *value,
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

static int cai_json_builder_field_string(cai_json_builder *builder,
                                         const char *name, const char *value,
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
  params->instructions = NULL;
  params->previous_response_id = NULL;
  cai_object_array_init(&params->input, sizeof(struct cai_input_message));
  *out = params;
  return CAI_OK;
}

void cai_response_create_params_destroy(cai_response_create_params *params) {
  struct cai_input_message *messages;
  size_t i;

  if (params == NULL) {
    return;
  }
  cai_free_mem(&params->allocator, params->model);
  cai_free_mem(&params->allocator, params->instructions);
  cai_free_mem(&params->allocator, params->previous_response_id);
  messages = (struct cai_input_message *)params->input.items;
  for (i = 0U; i < params->input.count; i++) {
    cai_input_message_cleanup(&params->allocator, &messages[i]);
  }
  cai_free_mem(&params->allocator, params->input.items);
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

int cai_response_create_params_serialize_json(
    const cai_response_create_params *params, char **out_json, size_t *out_len,
    cai_error *error) {
  cai_json_builder builder;
  struct cai_input_message *messages;
  struct cai_content_part *parts;
  size_t i;
  size_t j;
  int need_comma;
  int part_comma;
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
  if (rc == CAI_OK && need_comma) {
    rc = cai_json_builder_lit(&builder, ",", error);
  }
  if (rc == CAI_OK) {
    rc = cai_json_builder_lit(&builder, "\"input\":[", error);
  }
  messages = (struct cai_input_message *)params->input.items;
  for (i = 0U; rc == CAI_OK && i < params->input.count; i++) {
    if (i > 0U) {
      rc = cai_json_builder_lit(&builder, ",", error);
    }
    if (rc == CAI_OK) {
      rc = cai_json_builder_lit(&builder, "{\"role\":", error);
    }
    if (rc == CAI_OK) {
      rc = cai_json_builder_string(&builder, messages[i].role, error);
    }
    if (rc == CAI_OK) {
      rc = cai_json_builder_lit(&builder, ",\"content\":[", error);
    }
    parts = (struct cai_content_part *)messages[i].content.items;
    for (j = 0U; rc == CAI_OK && j < messages[i].content.count; j++) {
      if (j > 0U) {
        rc = cai_json_builder_lit(&builder, ",", error);
      }
      part_comma = 0;
      if (rc == CAI_OK) {
        rc = cai_json_builder_lit(&builder, "{", error);
      }
      if (rc == CAI_OK) {
        rc = cai_json_builder_field_string(&builder, "type", parts[j].type,
                                           &part_comma, error);
      }
      if (rc == CAI_OK && parts[j].text != NULL) {
        rc = cai_json_builder_field_string(&builder, "text", parts[j].text,
                                           &part_comma, error);
      }
      if (rc == CAI_OK && parts[j].image_url != NULL) {
        rc = cai_json_builder_field_string(
            &builder, "image_url", parts[j].image_url, &part_comma, error);
      }
      if (rc == CAI_OK && parts[j].detail != NULL) {
        rc = cai_json_builder_field_string(&builder, "detail", parts[j].detail,
                                           &part_comma, error);
      }
      if (rc == CAI_OK) {
        rc = cai_json_builder_lit(&builder, "}", error);
      }
    }
    if (rc == CAI_OK) {
      rc = cai_json_builder_lit(&builder, "]}", error);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_json_builder_lit(&builder, "]}", error);
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

int cai_response_parse_json(const char *json, cai_response **out,
                            cai_error *error) {
  cai_response_doc doc;
  cai_response *response;
  lonejson_error json_error;
  lonejson_status status;

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
  response->id = cai_strdup(NULL, doc.id);
  response->status = cai_strdup(NULL, doc.status);
  response->output_text = cai_response_collect_text(&doc);
  if (response->id == NULL || response->status == NULL ||
      response->output_text == NULL) {
    cai_response_destroy(response);
    lonejson_cleanup(&cai_response_map, &doc);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate parsed response");
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

const char *cai_response_output_text(const cai_response *response) {
  return response != NULL ? response->output_text : NULL;
}

void cai_response_destroy(cai_response *response) {
  if (response == NULL) {
    return;
  }
  cai_free_mem(NULL, response->id);
  cai_free_mem(NULL, response->status);
  cai_free_mem(NULL, response->output_text);
  cai_free_mem(NULL, response);
}
