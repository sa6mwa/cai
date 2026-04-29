#include "cai_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct cai_conversation_doc {
  char *id;
  char *object;
} cai_conversation_doc;

static const lonejson_field cai_conversation_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_conversation_doc, id, "id"),
    LONEJSON_FIELD_STRING_ALLOC(cai_conversation_doc, object, "object")};
LONEJSON_MAP_DEFINE(cai_conversation_map, cai_conversation_doc,
                    cai_conversation_fields);

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
  lonejson_init(&cai_conversation_map, &doc);
  status =
      lonejson_parse_cstr(&cai_conversation_map, &doc, json, NULL, &json_error);
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

static int cai_build_conversation_path(cai_client *client,
                                       const char *conversation_id, char **out,
                                       cai_error *error) {
  static const char prefix[] = "conversations/";
  char *path;
  size_t prefix_len;
  size_t id_len;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "path output is required");
  }
  *out = NULL;
  if (conversation_id == NULL || conversation_id[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "conversation id is required");
  }
  prefix_len = sizeof(prefix) - 1U;
  id_len = strlen(conversation_id);
  path = (char *)cai_alloc(client != NULL ? &client->allocator : NULL,
                           prefix_len + id_len + 1U);
  if (path == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate conversation path");
  }
  memcpy(path, prefix, prefix_len);
  memcpy(path + prefix_len, conversation_id, id_len);
  path[prefix_len + id_len] = '\0';
  *out = path;
  return CAI_OK;
}

static int cai_conversation_request(cai_client *client, const char *method,
                                    const char *path, const char *body,
                                    cai_conversation **out, cai_error *error) {
  char *json;
  long http_status;
  int rc;

  json = NULL;
  rc = cai_http_json_request(client, method, path, body, &json, &http_status,
                             error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (http_status < 200L || http_status >= 300L) {
    if (error != NULL) {
      error->http_status = http_status;
    }
    rc = cai_set_error_detail(error, CAI_ERR_SERVER,
                              "conversation request returned an error", json);
    free(json);
    return rc;
  }
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

  rc = cai_build_conversation_path(client, conversation_id, &path, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_conversation_request(client, "GET", path, NULL, out, error);
  cai_free_mem(client != NULL ? &client->allocator : NULL, path);
  return rc;
}

int cai_client_delete_conversation(cai_client *client,
                                   const char *conversation_id,
                                   cai_error *error) {
  cai_conversation *conversation;
  char *path;
  int rc;

  path = NULL;
  conversation = NULL;
  rc = cai_build_conversation_path(client, conversation_id, &path, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_conversation_request(client, "DELETE", path, NULL, &conversation,
                                error);
  cai_free_mem(client != NULL ? &client->allocator : NULL, path);
  cai_conversation_destroy(conversation);
  return rc;
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
