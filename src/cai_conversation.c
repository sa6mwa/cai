#include "cai_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct cai_conversation_doc {
  char *id;
  char *object;
} cai_conversation_doc;

typedef struct cai_conversation_metadata_doc {
  lonejson_json_value metadata;
} cai_conversation_metadata_doc;

typedef struct cai_conversation_items_request_doc {
  lonejson_json_value items;
} cai_conversation_items_request_doc;

typedef struct cai_conversation_spooled_reader_context {
  lonejson_spooled cursor;
} cai_conversation_spooled_reader_context;

static const lonejson_field cai_conversation_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_conversation_doc, id, "id"),
    LONEJSON_FIELD_STRING_ALLOC(cai_conversation_doc, object, "object")};
LONEJSON_MAP_DEFINE(cai_conversation_map, cai_conversation_doc,
                    cai_conversation_fields);

static const lonejson_field cai_conversation_metadata_fields[] = {
    LONEJSON_FIELD_JSON_VALUE_REQ(cai_conversation_metadata_doc, metadata,
                                  "metadata")};
LONEJSON_MAP_DEFINE(cai_conversation_metadata_map,
                    cai_conversation_metadata_doc,
                    cai_conversation_metadata_fields);

static const lonejson_field cai_conversation_items_request_fields[] = {
    LONEJSON_FIELD_JSON_VALUE_REQ(cai_conversation_items_request_doc, items,
                                  "items")};
LONEJSON_MAP_DEFINE(cai_conversation_items_request_map,
                    cai_conversation_items_request_doc,
                    cai_conversation_items_request_fields);

static lonejson_status cai_conversation_spooled_sink(void *user,
                                                     const void *data,
                                                     size_t len,
                                                     lonejson_error *error) {
  return ((lonejson_spooled *)user)->append((lonejson_spooled *)user, data, len,
                                            error);
}

static lonejson_read_result
cai_conversation_spooled_reader(void *user, unsigned char *buffer,
                                size_t capacity) {
  cai_conversation_spooled_reader_context *context;
  lonejson_read_result result;

  context = (cai_conversation_spooled_reader_context *)user;
  if (context == NULL) {
    result = lonejson_default_read_result();
    result.eof = 1;
    result.error_code = 1;
    return result;
  }
  return context->cursor.read(&context->cursor, buffer, capacity);
}

static void cai_conversation_object_array_init(lonejson_object_array *array,
                                               size_t elem_size) {
  array->items = NULL;
  array->count = 0U;
  array->capacity = 0U;
  array->elem_size = elem_size;
  array->flags = 0U;
}

static int cai_conversation_object_array_grow(const cai_allocator *allocator,
                                              lonejson_object_array *array,
                                              size_t elem_size,
                                              cai_error *error) {
  size_t new_capacity;
  void *grown;

  if (array->count < array->capacity) {
    return CAI_OK;
  }
  new_capacity = array->capacity == 0U ? 2U : array->capacity * 2U;
  grown = cai_realloc_mem(allocator, array->items, new_capacity * elem_size);
  if (grown == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to grow item array");
  }
  array->items = grown;
  array->capacity = new_capacity;
  array->elem_size = elem_size;
  return CAI_OK;
}

static void
cai_conversation_content_part_cleanup(const cai_allocator *allocator,
                                      struct cai_content_part *part) {
  if (part == NULL) {
    return;
  }
  cai_free_mem(allocator, part->type);
  cai_free_mem(allocator, part->text);
  if (part->has_text_spooled) {
    part->text_spooled.cleanup(&part->text_spooled);
    part->has_text_spooled = 0;
  }
  cai_free_mem(allocator, part->image_url);
  cai_free_mem(allocator, part->file_id);
  cai_free_mem(allocator, part->filename);
  cai_free_mem(allocator, part->file_url);
  if (part->has_file_data) {
    part->file_data.cleanup(&part->file_data);
    part->has_file_data = 0;
  }
  cai_free_mem(allocator, part->detail);
}

static void
cai_conversation_input_message_cleanup(const cai_allocator *allocator,
                                       struct cai_input_message *message) {
  struct cai_content_part *parts;
  size_t i;

  if (message == NULL) {
    return;
  }
  cai_free_mem(allocator, message->role);
  parts = (struct cai_content_part *)message->content.items;
  for (i = 0U; i < message->content.count; i++) {
    cai_conversation_content_part_cleanup(allocator, &parts[i]);
  }
  cai_free_mem(allocator, message->content.items);
}

int cai_conversation_parse_json(const char *json, cai_conversation **out,
                                cai_error *error) {
  cai_conversation_doc doc;
  cai_conversation *conversation;
  lonejson_error json_error;
  lonejson_status status;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "conversation output pointer is required");
  }
  *out = NULL;
  if (json == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "conversation JSON is required");
  }
  memset(&doc, 0, sizeof(doc));
  CAI_LJ->init(CAI_LJ, &cai_conversation_map, &doc);
  status = CAI_LJ->parse_cstr(CAI_LJ, &cai_conversation_map, &doc, json,
                              &json_error);
  if (status != LONEJSON_STATUS_OK) {
    lonejson_cleanup(&cai_conversation_map, &doc);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to parse conversation JSON",
                                json_error.message);
  }
  if (doc.id == NULL || doc.object == NULL) {
    lonejson_cleanup(&cai_conversation_map, &doc);
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "conversation JSON is missing id or object");
  }
  conversation = (cai_conversation *)cai_alloc(NULL, sizeof(*conversation));
  if (conversation == NULL) {
    lonejson_cleanup(&cai_conversation_map, &doc);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate conversation");
  }
  conversation->id = cai_strdup(NULL, doc.id);
  conversation->object = cai_strdup(NULL, doc.object);
  if (conversation->id == NULL || conversation->object == NULL) {
    cai_conversation_destroy(conversation);
    lonejson_cleanup(&cai_conversation_map, &doc);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate parsed conversation");
  }
  lonejson_cleanup(&cai_conversation_map, &doc);
  *out = conversation;
  return CAI_OK;
}

int cai_conversation_from_id(const char *conversation_id,
                             cai_conversation **out, cai_error *error) {
  cai_conversation *conversation;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "conversation output pointer is required");
  }
  *out = NULL;
  if (conversation_id == NULL || conversation_id[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "conversation id is required");
  }
  conversation = (cai_conversation *)cai_alloc(NULL, sizeof(*conversation));
  if (conversation == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate conversation");
  }
  conversation->id = cai_strdup(NULL, conversation_id);
  conversation->object = cai_strdup(NULL, "conversation");
  if (conversation->id == NULL || conversation->object == NULL) {
    cai_conversation_destroy(conversation);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate conversation");
  }
  *out = conversation;
  return CAI_OK;
}

static int cai_build_conversation_path(cai_client *client,
                                       const char *conversation_id,
                                       const char *suffix, char **out,
                                       cai_error *error) {
  static const char prefix[] = "conversations/";
  char *path;
  size_t prefix_len;
  size_t id_len;
  size_t suffix_len;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "path output is required");
  }
  *out = NULL;
  if (conversation_id == NULL || conversation_id[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "conversation id is required");
  }
  prefix_len = sizeof(prefix) - 1U;
  id_len = strlen(conversation_id);
  suffix_len = suffix != NULL ? strlen(suffix) : 0U;
  path = (char *)cai_alloc(client != NULL ? &CAI_CLIENT_IMPL(client)->allocator : NULL,
                           prefix_len + id_len + suffix_len + 1U);
  if (path == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate conversation path");
  }
  memcpy(path, prefix, prefix_len);
  memcpy(path + prefix_len, conversation_id, id_len);
  if (suffix_len > 0U) {
    memcpy(path + prefix_len + id_len, suffix, suffix_len);
  }
  path[prefix_len + id_len + suffix_len] = '\0';
  *out = path;
  return CAI_OK;
}

static int cai_build_conversation_item_path(cai_client *client,
                                            const char *conversation_id,
                                            const char *item_id, char **out,
                                            cai_error *error) {
  static const char items_prefix[] = "/items/";
  char *suffix;
  size_t prefix_len;
  size_t id_len;
  int rc;

  if (item_id == NULL || item_id[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "item id is required");
  }
  prefix_len = sizeof(items_prefix) - 1U;
  id_len = strlen(item_id);
  suffix = (char *)cai_alloc(client != NULL ? &CAI_CLIENT_IMPL(client)->allocator : NULL,
                             prefix_len + id_len + 1U);
  if (suffix == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate conversation item path");
  }
  memcpy(suffix, items_prefix, prefix_len);
  memcpy(suffix + prefix_len, item_id, id_len);
  suffix[prefix_len + id_len] = '\0';
  rc = cai_build_conversation_path(client, conversation_id, suffix, out, error);
  cai_free_mem(client != NULL ? &CAI_CLIENT_IMPL(client)->allocator : NULL, suffix);
  return rc;
}

static int cai_conversation_request(cai_client *client, const char *method,
                                    const char *path, const char *body,
                                    cai_conversation **out, cai_error *error) {
  char *json;
  char *request_id;
  long http_status;
  int rc;

  json = NULL;
  request_id = NULL;
  rc = cai_http_json_request(client, method, path, body, &json, &http_status,
                             &request_id, error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (http_status < 200L || http_status >= 300L) {
    rc = cai_set_openai_error(error, http_status, json, request_id);
    free(json);
    free(request_id);
    return rc;
  }
  free(request_id);
  rc = cai_conversation_parse_json(json, out, error);
  free(json);
  return rc;
}

static int cai_conversation_request_spooled(cai_client *client,
                                            const char *method,
                                            const char *path,
                                            const lonejson_spooled *body,
                                            size_t body_len,
                                            cai_conversation **out,
                                            cai_error *error) {
  char *json;
  char *request_id;
  long http_status;
  int rc;

  json = NULL;
  request_id = NULL;
  rc = cai_http_json_request_spooled(client, method, path, body, body_len,
                                     &json, &http_status, &request_id, error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (http_status < 200L || http_status >= 300L) {
    rc = cai_set_openai_error(error, http_status, json, request_id);
    free(json);
    free(request_id);
    return rc;
  }
  free(request_id);
  rc = cai_conversation_parse_json(json, out, error);
  free(json);
  return rc;
}

int cai_client_create_conversation(cai_client *client, cai_conversation **out,
                                   cai_error *error) {
  return cai_conversation_request(client, "POST", "conversations", "{}", out,
                                  error);
}

int cai_client_retrieve_conversation(cai_client *client,
                                     const char *conversation_id,
                                     cai_conversation **out, cai_error *error) {
  char *path;
  int rc;

  rc = cai_build_conversation_path(client, conversation_id, NULL, &path, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_conversation_request(client, "GET", path, NULL, out, error);
  cai_free_mem(client != NULL ? &CAI_CLIENT_IMPL(client)->allocator : NULL, path);
  return rc;
}

int cai_client_retrieve_conversation_handle(
    cai_client *client, const cai_conversation *conversation,
    cai_conversation **out, cai_error *error) {
  if (conversation == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "conversation is required");
  }
  return cai_client_retrieve_conversation(client, conversation->id, out, error);
}

int cai_client_update_conversation_metadata(cai_client *client,
                                            const char *conversation_id,
                                            const char *metadata_json,
                                            cai_conversation **out,
                                            cai_error *error) {
  cai_conversation_metadata_doc doc;
  lonejson_spooled body;
  lonejson_error json_error;
  const char *metadata;
  char *path;
  int has_body;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "conversation output pointer is required");
  }
  *out = NULL;
  metadata = metadata_json != NULL ? metadata_json : "{}";
  memset(&doc, 0, sizeof(doc));
  CAI_LJ->json_value_init(CAI_LJ, &doc.metadata);
  memset(&body, 0, sizeof(body));
  has_body = 0;
  path = NULL;
  lonejson_error_init(&json_error);
  if (doc.metadata.methods->set_buffer(&doc.metadata, metadata,
                                     strlen(metadata),
                                     &json_error) != LONEJSON_STATUS_OK) {
    rc = cai_set_error_detail(error, CAI_ERR_INVALID,
                              "conversation metadata must be valid JSON",
                              json_error.message);
  } else {
    rc = CAI_OK;
  }
  if (rc == CAI_OK) {
    rc = cai_build_conversation_path(client, conversation_id, NULL, &path,
                                     error);
  }
  if (rc == CAI_OK) {
    CAI_LJ->spooled_init(CAI_LJ, &body);
    has_body = 1;
    lonejson_error_init(&json_error);
    if (lonejson_serialize_sink(CAI_LJ, &cai_conversation_metadata_map, &doc,
                                cai_conversation_spooled_sink, &body,
                                &json_error) != LONEJSON_STATUS_OK) {
      rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to serialize conversation metadata",
                                json_error.message);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_conversation_request_spooled(client, "POST", path, &body,
                                          body.size_fn(&body), out,
                                          error);
  }
  cai_free_mem(client != NULL ? &CAI_CLIENT_IMPL(client)->allocator : NULL, path);
  if (has_body) {
    body.cleanup(&body);
  }
  CAI_LJ->json_value_cleanup(CAI_LJ, &doc.metadata);
  return rc;
}

int cai_client_update_conversation_metadata_handle(
    cai_client *client, const cai_conversation *conversation,
    const char *metadata_json, cai_conversation **out, cai_error *error) {
  if (conversation == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "conversation is required");
  }
  return cai_client_update_conversation_metadata(client, conversation->id,
                                                 metadata_json, out, error);
}

int cai_client_delete_conversation(cai_client *client,
                                   const char *conversation_id,
                                   cai_error *error) {
  cai_conversation *conversation;
  char *path;
  int rc;

  path = NULL;
  conversation = NULL;
  rc = cai_build_conversation_path(client, conversation_id, NULL, &path, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_conversation_request(client, "DELETE", path, NULL, &conversation,
                                error);
  cai_free_mem(client != NULL ? &CAI_CLIENT_IMPL(client)->allocator : NULL, path);
  cai_conversation_destroy(conversation);
  return rc;
}

int cai_client_delete_conversation_handle(cai_client *client,
                                          const cai_conversation *conversation,
                                          cai_error *error) {
  if (conversation == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "conversation is required");
  }
  return cai_client_delete_conversation(client, conversation->id, error);
}

int cai_client_list_conversation_items(cai_client *client,
                                       const char *conversation_id,
                                       const cai_list_params *params,
                                       cai_input_item_list **out,
                                       cai_error *error) {
  char *path;
  char *json;
  char *request_id;
  long http_status;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input item list output pointer is required");
  }
  *out = NULL;
  path = NULL;
  json = NULL;
  request_id = NULL;
  rc = cai_build_conversation_path(client, conversation_id, "/items", &path,
                                   error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_append_list_query_params(client != NULL ? &CAI_CLIENT_IMPL(client)->allocator : NULL,
                                    &path, params, error);
  if (rc != CAI_OK) {
    cai_free_mem(client != NULL ? &CAI_CLIENT_IMPL(client)->allocator : NULL, path);
    return rc;
  }
  rc = cai_http_json_request(client, "GET", path, NULL, &json, &http_status,
                             &request_id, error);
  cai_free_mem(client != NULL ? &CAI_CLIENT_IMPL(client)->allocator : NULL, path);
  if (rc != CAI_OK) {
    return rc;
  }
  if (http_status < 200L || http_status >= 300L) {
    rc = cai_set_openai_error(error, http_status, json, request_id);
    free(json);
    free(request_id);
    return rc;
  }
  rc = cai_input_item_list_parse_json(json, out, error);
  free(json);
  free(request_id);
  return rc;
}

int cai_client_list_conversation_items_handle(
    cai_client *client, const cai_conversation *conversation,
    const cai_list_params *params, cai_input_item_list **out,
    cai_error *error) {
  if (conversation == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "conversation is required");
  }
  return cai_client_list_conversation_items(client, conversation->id, params,
                                            out, error);
}

int cai_client_delete_conversation_item(cai_client *client,
                                        const char *conversation_id,
                                        const char *item_id, cai_error *error) {
  cai_conversation *conversation;
  char *path;
  int rc;

  path = NULL;
  conversation = NULL;
  rc = cai_build_conversation_item_path(client, conversation_id, item_id, &path,
                                        error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_conversation_request(client, "DELETE", path, NULL, &conversation,
                                error);
  cai_free_mem(client != NULL ? &CAI_CLIENT_IMPL(client)->allocator : NULL, path);
  cai_conversation_destroy(conversation);
  return rc;
}

int cai_client_delete_conversation_item_handle(
    cai_client *client, const cai_conversation *conversation,
    const char *item_id, cai_error *error) {
  if (conversation == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "conversation is required");
  }
  return cai_client_delete_conversation_item(client, conversation->id, item_id,
                                             error);
}

int cai_client_retrieve_conversation_item(cai_client *client,
                                          const char *conversation_id,
                                          const char *item_id,
                                          cai_conversation_item **out,
                                          cai_error *error) {
  char *path;
  char *json;
  char *request_id;
  long http_status;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "conversation item output pointer is required");
  }
  *out = NULL;
  path = NULL;
  json = NULL;
  request_id = NULL;
  rc = cai_build_conversation_item_path(client, conversation_id, item_id, &path,
                                        error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_http_json_request(client, "GET", path, NULL, &json, &http_status,
                             &request_id, error);
  cai_free_mem(client != NULL ? &CAI_CLIENT_IMPL(client)->allocator : NULL, path);
  if (rc != CAI_OK) {
    return rc;
  }
  if (http_status < 200L || http_status >= 300L) {
    rc = cai_set_openai_error(error, http_status, json, request_id);
    free(json);
    free(request_id);
    return rc;
  }
  rc = cai_conversation_item_parse_json(json, out, error);
  free(json);
  free(request_id);
  return rc;
}

int cai_client_retrieve_conversation_item_handle(
    cai_client *client, const cai_conversation *conversation,
    const char *item_id, cai_conversation_item **out, cai_error *error) {
  if (conversation == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "conversation is required");
  }
  return cai_client_retrieve_conversation_item(client, conversation->id,
                                               item_id, out, error);
}

int cai_conversation_items_params_new(cai_conversation_items_params **out,
                                      cai_error *error) {
  cai_conversation_items_params *params;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "conversation items params output is required");
  }
  *out = NULL;
  params = (cai_conversation_items_params *)cai_alloc(NULL, sizeof(*params));
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate conversation items params");
  }
  params->allocator.malloc_fn = NULL;
  params->allocator.realloc_fn = NULL;
  params->allocator.free_fn = NULL;
  params->allocator.context = NULL;
  cai_conversation_object_array_init(&params->items,
                                     sizeof(struct cai_input_message));
  *out = params;
  return CAI_OK;
}

void cai_conversation_items_params_destroy(
    cai_conversation_items_params *params) {
  struct cai_input_message *messages;
  size_t i;

  if (params == NULL) {
    return;
  }
  messages = (struct cai_input_message *)params->items.items;
  for (i = 0U; i < params->items.count; i++) {
    cai_conversation_input_message_cleanup(&params->allocator, &messages[i]);
  }
  cai_free_mem(&params->allocator, params->items.items);
  cai_free_mem(&params->allocator, params);
}

static int cai_conversation_items_params_add_part(
    cai_conversation_items_params *params, const char *role,
    struct cai_content_part *part, cai_error *error) {
  struct cai_input_message *messages;
  struct cai_input_message *message;
  struct cai_content_part *parts;
  int rc;

  if (params == NULL || role == NULL || role[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "role is required");
  }
  rc = cai_conversation_object_array_grow(&params->allocator, &params->items,
                                          sizeof(struct cai_input_message),
                                          error);
  if (rc != CAI_OK) {
    return rc;
  }
  messages = (struct cai_input_message *)params->items.items;
  message = &messages[params->items.count];
  memset(message, 0, sizeof(*message));
  message->role = NULL;
  cai_conversation_object_array_init(&message->content,
                                     sizeof(struct cai_content_part));
  message->role = cai_strdup(&params->allocator, role);
  if (message->role == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate role");
  }
  rc = cai_conversation_object_array_grow(&params->allocator, &message->content,
                                          sizeof(struct cai_content_part),
                                          error);
  if (rc != CAI_OK) {
    cai_conversation_input_message_cleanup(&params->allocator, message);
    return rc;
  }
  parts = (struct cai_content_part *)message->content.items;
  parts[0] = *part;
  message->content.count = 1U;
  params->items.count++;
  return CAI_OK;
}

int cai_conversation_items_params_add_text(
    cai_conversation_items_params *params, const char *role, const char *text,
    cai_error *error) {
  struct cai_content_part part;
  const cai_allocator *allocator;
  int rc;

  if (text == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "text is required");
  }
  allocator = params != NULL ? &params->allocator : NULL;
  memset(&part, 0, sizeof(part));
  part.type = cai_strdup(allocator, "input_text");
  part.text = cai_strdup(allocator, text);
  if (part.type == NULL || part.text == NULL) {
    cai_conversation_content_part_cleanup(allocator, &part);
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate text input");
  }
  rc = cai_conversation_items_params_add_part(params, role, &part, error);
  if (rc != CAI_OK) {
    cai_conversation_content_part_cleanup(allocator, &part);
  }
  return rc;
}

int cai_conversation_items_params_add_text_spooled(
    cai_conversation_items_params *params, const char *role,
    lonejson_spooled *text, cai_error *error) {
  struct cai_content_part part;
  const cai_allocator *allocator;
  int rc;

  if (text == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "text spool is required");
  }
  allocator = params != NULL ? &params->allocator : NULL;
  memset(&part, 0, sizeof(part));
  part.type = cai_strdup(allocator, "input_text");
  part.text_spooled = *text;
  part.has_text_spooled = 1;
  memset(text, 0, sizeof(*text));
  if (part.type == NULL) {
    cai_conversation_content_part_cleanup(allocator, &part);
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate text input");
  }
  rc = cai_conversation_items_params_add_part(params, role, &part, error);
  if (rc != CAI_OK) {
    cai_conversation_content_part_cleanup(allocator, &part);
  }
  return rc;
}

static int cai_conversation_source_to_spooled(cai_source *source,
                                              lonejson_spooled *out,
                                              cai_error *error) {
  lonejson_error json_error;
  unsigned char buffer[4096];
  size_t nread;
  int previous_error_code;

  if (source == NULL || out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "source and spool output are required");
  }
  memset(out, 0, sizeof(*out));
  CAI_LJ->spooled_init(CAI_LJ, out);
  for (;;) {
    previous_error_code = error != NULL ? error->code : CAI_OK;
    nread = cai_source_read(source, buffer, sizeof(buffer), error);
    if (nread == 0U && error != NULL && error->code != previous_error_code &&
        error->code != CAI_OK) {
      out->cleanup(out);
      return error->code;
    }
    if (nread == 0U) {
      break;
    }
    lonejson_error_init(&json_error);
    if (out->append(out, buffer, nread, &json_error) !=
        LONEJSON_STATUS_OK) {
      out->cleanup(out);
      return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                  "failed to spool conversation text input",
                                  json_error.message);
    }
  }
  return CAI_OK;
}

int cai_conversation_items_params_add_text_source(
    cai_conversation_items_params *params, const char *role, cai_source *source,
    cai_error *error) {
  lonejson_spooled text;
  int rc;

  if (source == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "text source is required");
  }
  memset(&text, 0, sizeof(text));
  rc = cai_conversation_source_to_spooled(source, &text, error);
  if (rc == CAI_OK) {
    rc = cai_conversation_items_params_add_text_spooled(params, role, &text,
                                                       error);
  }
  if (rc != CAI_OK) {
    text.cleanup(&text);
  }
  return rc;
}

int cai_conversation_items_params_add_image_url(
    cai_conversation_items_params *params, const char *role, const char *url,
    const char *detail, cai_error *error) {
  struct cai_content_part part;
  const cai_allocator *allocator;
  int rc;

  if (url == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "image URL is required");
  }
  allocator = params != NULL ? &params->allocator : NULL;
  memset(&part, 0, sizeof(part));
  part.type = cai_strdup(allocator, "input_image");
  part.image_url = cai_strdup(allocator, url);
  part.detail = cai_strdup(allocator, detail);
  if (part.type == NULL || part.image_url == NULL ||
      (detail != NULL && part.detail == NULL)) {
    cai_conversation_content_part_cleanup(allocator, &part);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate image input");
  }
  rc = cai_conversation_items_params_add_part(params, role, &part, error);
  if (rc != CAI_OK) {
    cai_conversation_content_part_cleanup(allocator, &part);
  }
  return rc;
}

int cai_conversation_items_params_add_image_file_id(
    cai_conversation_items_params *params, const char *role,
    const char *file_id, const char *detail, cai_error *error) {
  struct cai_content_part part;
  const cai_allocator *allocator;
  int rc;

  if (file_id == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "image file id is required");
  }
  allocator = params != NULL ? &params->allocator : NULL;
  memset(&part, 0, sizeof(part));
  part.type = cai_strdup(allocator, "input_image");
  part.file_id = cai_strdup(allocator, file_id);
  part.detail = cai_strdup(allocator, detail);
  if (part.type == NULL || part.file_id == NULL ||
      (detail != NULL && part.detail == NULL)) {
    cai_conversation_content_part_cleanup(allocator, &part);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate image file input");
  }
  rc = cai_conversation_items_params_add_part(params, role, &part, error);
  if (rc != CAI_OK) {
    cai_conversation_content_part_cleanup(allocator, &part);
  }
  return rc;
}

int cai_conversation_items_params_add_file_id(
    cai_conversation_items_params *params, const char *role,
    const char *file_id, const char *detail, cai_error *error) {
  struct cai_content_part part;
  const cai_allocator *allocator;
  int rc;

  if (file_id == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "file id is required");
  }
  allocator = params != NULL ? &params->allocator : NULL;
  memset(&part, 0, sizeof(part));
  part.type = cai_strdup(allocator, "input_file");
  part.file_id = cai_strdup(allocator, file_id);
  part.detail = cai_strdup(allocator, detail);
  if (part.type == NULL || part.file_id == NULL ||
      (detail != NULL && part.detail == NULL)) {
    cai_conversation_content_part_cleanup(allocator, &part);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate file input");
  }
  rc = cai_conversation_items_params_add_part(params, role, &part, error);
  if (rc != CAI_OK) {
    cai_conversation_content_part_cleanup(allocator, &part);
  }
  return rc;
}

int cai_conversation_items_params_add_file_url(
    cai_conversation_items_params *params, const char *role,
    const char *file_url, const char *detail, cai_error *error) {
  struct cai_content_part part;
  const cai_allocator *allocator;
  int rc;

  if (file_url == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "file URL is required");
  }
  allocator = params != NULL ? &params->allocator : NULL;
  memset(&part, 0, sizeof(part));
  part.type = cai_strdup(allocator, "input_file");
  part.file_url = cai_strdup(allocator, file_url);
  part.detail = cai_strdup(allocator, detail);
  if (part.type == NULL || part.file_url == NULL ||
      (detail != NULL && part.detail == NULL)) {
    cai_conversation_content_part_cleanup(allocator, &part);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate file URL input");
  }
  rc = cai_conversation_items_params_add_part(params, role, &part, error);
  if (rc != CAI_OK) {
    cai_conversation_content_part_cleanup(allocator, &part);
  }
  return rc;
}

int cai_conversation_items_params_add_file_data_spooled(
    cai_conversation_items_params *params, const char *role,
    const char *filename, struct lonejson_spooled *file_data,
    const char *detail, cai_error *error) {
  struct cai_content_part part;
  const cai_allocator *allocator;
  int rc;

  if (file_data == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "file data spool is required");
  }
  allocator = params != NULL ? &params->allocator : NULL;
  memset(&part, 0, sizeof(part));
  part.type = cai_strdup(allocator, "input_file");
  part.filename = cai_strdup(allocator, filename);
  part.detail = cai_strdup(allocator, detail);
  part.file_data = *file_data;
  part.has_file_data = 1;
  memset(file_data, 0, sizeof(*file_data));
  if (part.type == NULL || (filename != NULL && part.filename == NULL) ||
      (detail != NULL && part.detail == NULL)) {
    cai_conversation_content_part_cleanup(allocator, &part);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate file data input");
  }
  rc = cai_conversation_items_params_add_part(params, role, &part, error);
  if (rc != CAI_OK) {
    cai_conversation_content_part_cleanup(allocator, &part);
  }
  return rc;
}

static int cai_conversation_items_params_spool_json(
    const cai_conversation_items_params *params, lonejson_spooled *out,
    size_t *out_len, cai_error *error) {
  cai_conversation_items_request_doc doc;
  cai_conversation_spooled_reader_context reader_context;
  lonejson_spooled items;
  lonejson_error json_error;
  int has_items;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "JSON spool output pointer is required");
  }
  if (params == NULL || params->items.count == 0U) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "at least one conversation item is required");
  }
  memset(&doc, 0, sizeof(doc));
  memset(&items, 0, sizeof(items));
  memset(&reader_context, 0, sizeof(reader_context));
  has_items = 0;
  rc = cai_input_messages_spool_json_array(&params->items, &items, NULL, error);
  if (rc == CAI_OK) {
    has_items = 1;
    reader_context.cursor = items;
    lonejson_error_init(&json_error);
    if (reader_context.cursor.rewind(&reader_context.cursor, &json_error) !=
        LONEJSON_STATUS_OK) {
      rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to rewind conversation items JSON",
                                json_error.message);
    }
  }
  if (rc == CAI_OK) {
    CAI_LJ->json_value_init(CAI_LJ, &doc.items);
    lonejson_error_init(&json_error);
    if (doc.items.methods->set_reader(&doc.items,
                                       cai_conversation_spooled_reader,
                                       &reader_context,
                                       &json_error) != LONEJSON_STATUS_OK) {
      rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to prepare conversation items JSON",
                                json_error.message);
    }
  }
  if (rc == CAI_OK) {
    CAI_LJ->spooled_init(CAI_LJ, out);
    lonejson_error_init(&json_error);
    if (lonejson_serialize_sink(CAI_LJ, &cai_conversation_items_request_map,
                                &doc, cai_conversation_spooled_sink, out,
                                &json_error) != LONEJSON_STATUS_OK) {
      out->cleanup(out);
      rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to serialize conversation items JSON",
                                json_error.message);
    }
  }
  if (rc != CAI_OK) {
    if (has_items) {
      items.cleanup(&items);
    }
    CAI_LJ->json_value_cleanup(CAI_LJ, &doc.items);
    return rc;
  }
  if (out_len != NULL) {
    *out_len = out->size_fn(out);
  }
  items.cleanup(&items);
  CAI_LJ->json_value_cleanup(CAI_LJ, &doc.items);
  return CAI_OK;
}

int cai_client_create_conversation_items(
    cai_client *client, const char *conversation_id,
    const cai_conversation_items_params *params, cai_input_item_list **out,
    cai_error *error) {
  char *path;
  lonejson_spooled body;
  size_t body_len;
  int has_body;
  char *json;
  char *request_id;
  long http_status;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input item list output pointer is required");
  }
  *out = NULL;
  path = NULL;
  memset(&body, 0, sizeof(body));
  body_len = 0U;
  has_body = 0;
  json = NULL;
  request_id = NULL;
  rc = cai_conversation_items_params_spool_json(params, &body, &body_len,
                                               error);
  if (rc != CAI_OK) {
    return rc;
  }
  has_body = 1;
  rc = cai_build_conversation_path(client, conversation_id, "/items", &path,
                                   error);
  if (rc != CAI_OK) {
    body.cleanup(&body);
    return rc;
  }
  rc = cai_http_json_request_spooled(client, "POST", path, &body, body_len,
                                     &json, &http_status, &request_id, error);
  cai_free_mem(client != NULL ? &CAI_CLIENT_IMPL(client)->allocator : NULL, path);
  if (has_body) {
    body.cleanup(&body);
  }
  if (rc != CAI_OK) {
    return rc;
  }
  if (http_status < 200L || http_status >= 300L) {
    rc = cai_set_openai_error(error, http_status, json, request_id);
    free(json);
    free(request_id);
    return rc;
  }
  rc = cai_input_item_list_parse_json(json, out, error);
  free(json);
  free(request_id);
  return rc;
}

int cai_client_create_conversation_items_handle(
    cai_client *client, const cai_conversation *conversation,
    const cai_conversation_items_params *params, cai_input_item_list **out,
    cai_error *error) {
  if (conversation == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "conversation is required");
  }
  return cai_client_create_conversation_items(client, conversation->id, params,
                                              out, error);
}

const char *cai_conversation_id(const cai_conversation *conversation) {
  return conversation != NULL ? conversation->id : NULL;
}

const char *cai_conversation_object(const cai_conversation *conversation) {
  return conversation != NULL ? conversation->object : NULL;
}

void cai_conversation_destroy(cai_conversation *conversation) {
  if (conversation == NULL) {
    return;
  }
  cai_free_mem(NULL, conversation->id);
  cai_free_mem(NULL, conversation->object);
  cai_free_mem(NULL, conversation);
}
