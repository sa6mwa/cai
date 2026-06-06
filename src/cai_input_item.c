#include "cai_internal.h"

#include <string.h>

typedef struct cai_input_item_doc {
  char *id;
  char *type;
  char *role;
} cai_input_item_doc;

typedef struct cai_input_item_list_doc {
  char *object;
  lonejson_object_array data;
  char *first_id;
  char *last_id;
  int has_more;
} cai_input_item_list_doc;

static const lonejson_field cai_input_item_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_input_item_doc, id, "id"),
    LONEJSON_FIELD_STRING_ALLOC(cai_input_item_doc, type, "type"),
    LONEJSON_FIELD_STRING_ALLOC(cai_input_item_doc, role, "role")};
LONEJSON_MAP_DEFINE(cai_input_item_map, cai_input_item_doc,
                    cai_input_item_fields);

static const lonejson_field cai_input_item_list_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_input_item_list_doc, object, "object"),
    LONEJSON_FIELD_OBJECT_ARRAY(cai_input_item_list_doc, data, "data",
                                cai_input_item_doc, &cai_input_item_map,
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_ALLOC(cai_input_item_list_doc, first_id, "first_id"),
    LONEJSON_FIELD_STRING_ALLOC(cai_input_item_list_doc, last_id, "last_id"),
    LONEJSON_FIELD_BOOL(cai_input_item_list_doc, has_more, "has_more")};
LONEJSON_MAP_DEFINE(cai_input_item_list_map, cai_input_item_list_doc,
                    cai_input_item_list_fields);

static void cai_input_item_list_init_methods(cai_input_item_list *list) {
  list->count = cai_input_item_list_count;
  list->has_more = cai_input_item_list_has_more;
  list->first_id = cai_input_item_list_first_id;
  list->last_id = cai_input_item_list_last_id;
  list->raw_json = cai_input_item_list_raw_json;
  list->item_id = cai_input_item_id;
  list->item_type = cai_input_item_type;
  list->item_role = cai_input_item_role;
  list->close = cai_input_item_list_destroy;
}

static void cai_conversation_item_init_methods(cai_conversation_item *item) {
  item->id = cai_conversation_item_id;
  item->type = cai_conversation_item_type;
  item->role = cai_conversation_item_role;
  item->raw_json = cai_conversation_item_raw_json;
  item->close = cai_conversation_item_destroy;
}

static int cai_input_item_list_copy_items(cai_input_item_list *list,
                                          const cai_input_item_list_doc *doc,
                                          cai_error *error) {
  cai_input_item_doc *items;
  size_t i;

  list->items = NULL;
  list->count_value = doc->data.count;
  if (list->count_value == 0U) {
    return CAI_OK;
  }
  list->items = (cai_input_item *)cai_alloc(NULL, list->count_value *
                                                      sizeof(*list->items));
  if (list->items == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate input item list");
  }
  memset(list->items, 0, list->count_value * sizeof(*list->items));
  items = (cai_input_item_doc *)doc->data.items;
  for (i = 0U; i < list->count_value; i++) {
    list->items[i].id = cai_strdup(NULL, items[i].id);
    list->items[i].type = cai_strdup(NULL, items[i].type);
    list->items[i].role = cai_strdup(NULL, items[i].role);
    if ((items[i].id != NULL && list->items[i].id == NULL) ||
        (items[i].type != NULL && list->items[i].type == NULL) ||
        (items[i].role != NULL && list->items[i].role == NULL)) {
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate input item fields");
    }
  }
  return CAI_OK;
}

void cai_list_params_init(cai_list_params *params) {
  if (params == NULL) {
    return;
  }
  params->after = NULL;
  params->limit = 0;
  params->order = NULL;
}

int cai_input_item_list_parse_json(const char *json, cai_input_item_list **out,
                                   cai_error *error) {
  cai_input_item_list_doc doc;
  cai_input_item_list *list;
  lonejson_error json_error;
  lonejson_status status;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input item list output pointer is required");
  }
  *out = NULL;
  if (json == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input item list JSON is required");
  }
  memset(&doc, 0, sizeof(doc));
  CAI_LJ->init(CAI_LJ, &cai_input_item_list_map, &doc);
  status = CAI_LJ->parse_cstr(CAI_LJ, &cai_input_item_list_map, &doc, json,
                              &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_input_item_list_map, &doc);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to parse input item list JSON",
                                json_error.message);
  }
  list = (cai_input_item_list *)cai_alloc(NULL, sizeof(*list));
  if (list == NULL) {
    CAI_LJ->cleanup(CAI_LJ, &cai_input_item_list_map, &doc);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate input item list");
  }
  memset(list, 0, sizeof(*list));
  cai_input_item_list_init_methods(list);
  list->object = cai_strdup(NULL, doc.object);
  list->first_id_value = cai_strdup(NULL, doc.first_id);
  list->last_id_value = cai_strdup(NULL, doc.last_id);
  list->raw_json_value = cai_strdup(NULL, json);
  list->has_more_value = doc.has_more;
  if ((doc.object != NULL && list->object == NULL) ||
      (doc.first_id != NULL && list->first_id_value == NULL) ||
      (doc.last_id != NULL && list->last_id_value == NULL) ||
      list->raw_json_value == NULL) {
    cai_input_item_list_destroy(list);
    CAI_LJ->cleanup(CAI_LJ, &cai_input_item_list_map, &doc);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate input item list fields");
  }
  rc = cai_input_item_list_copy_items(list, &doc, error);
  if (rc != CAI_OK) {
    cai_input_item_list_destroy(list);
    CAI_LJ->cleanup(CAI_LJ, &cai_input_item_list_map, &doc);
    return rc;
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_input_item_list_map, &doc);
  *out = list;
  return CAI_OK;
}

int cai_conversation_item_parse_json(const char *json,
                                     cai_conversation_item **out,
                                     cai_error *error) {
  cai_input_item_doc doc;
  cai_conversation_item *item;
  lonejson_error json_error;
  lonejson_status status;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "conversation item output pointer is required");
  }
  *out = NULL;
  if (json == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "conversation item JSON is required");
  }
  memset(&doc, 0, sizeof(doc));
  CAI_LJ->init(CAI_LJ, &cai_input_item_map, &doc);
  status =
      CAI_LJ->parse_cstr(CAI_LJ, &cai_input_item_map, &doc, json, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_input_item_map, &doc);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to parse conversation item JSON",
                                json_error.message);
  }
  item = (cai_conversation_item *)cai_alloc(NULL, sizeof(*item));
  if (item == NULL) {
    CAI_LJ->cleanup(CAI_LJ, &cai_input_item_map, &doc);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate conversation item");
  }
  memset(item, 0, sizeof(*item));
  cai_conversation_item_init_methods(item);
  item->id_value = cai_strdup(NULL, doc.id);
  item->type_value = cai_strdup(NULL, doc.type);
  item->role_value = cai_strdup(NULL, doc.role);
  item->raw_json_value = cai_strdup(NULL, json);
  if ((doc.id != NULL && item->id_value == NULL) ||
      (doc.type != NULL && item->type_value == NULL) ||
      (doc.role != NULL && item->role_value == NULL) ||
      item->raw_json_value == NULL) {
    cai_conversation_item_destroy(item);
    CAI_LJ->cleanup(CAI_LJ, &cai_input_item_map, &doc);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate conversation item fields");
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_input_item_map, &doc);
  *out = item;
  return CAI_OK;
}

size_t cai_input_item_list_count(const cai_input_item_list *list) {
  return list != NULL ? list->count_value : 0U;
}

int cai_input_item_list_has_more(const cai_input_item_list *list) {
  return list != NULL ? list->has_more_value : 0;
}

const char *cai_input_item_list_first_id(const cai_input_item_list *list) {
  return list != NULL ? list->first_id_value : NULL;
}

const char *cai_input_item_list_last_id(const cai_input_item_list *list) {
  return list != NULL ? list->last_id_value : NULL;
}

const char *cai_input_item_list_raw_json(const cai_input_item_list *list) {
  return list != NULL ? list->raw_json_value : NULL;
}

const char *cai_input_item_id(const cai_input_item_list *list, size_t index) {
  if (list == NULL || index >= list->count_value) {
    return NULL;
  }
  return list->items[index].id;
}

const char *cai_input_item_type(const cai_input_item_list *list, size_t index) {
  if (list == NULL || index >= list->count_value) {
    return NULL;
  }
  return list->items[index].type;
}

const char *cai_input_item_role(const cai_input_item_list *list, size_t index) {
  if (list == NULL || index >= list->count_value) {
    return NULL;
  }
  return list->items[index].role;
}

void cai_input_item_list_destroy(cai_input_item_list *list) {
  size_t i;

  if (list == NULL) {
    return;
  }
  for (i = 0U; i < list->count_value; i++) {
    cai_free_mem(NULL, list->items[i].id);
    cai_free_mem(NULL, list->items[i].type);
    cai_free_mem(NULL, list->items[i].role);
  }
  cai_free_mem(NULL, list->items);
  cai_free_mem(NULL, list->object);
  cai_free_mem(NULL, list->first_id_value);
  cai_free_mem(NULL, list->last_id_value);
  cai_free_mem(NULL, list->raw_json_value);
  cai_free_mem(NULL, list);
}

const char *cai_conversation_item_id(const cai_conversation_item *item) {
  return item != NULL ? item->id_value : NULL;
}

const char *cai_conversation_item_type(const cai_conversation_item *item) {
  return item != NULL ? item->type_value : NULL;
}

const char *cai_conversation_item_role(const cai_conversation_item *item) {
  return item != NULL ? item->role_value : NULL;
}

const char *cai_conversation_item_raw_json(const cai_conversation_item *item) {
  return item != NULL ? item->raw_json_value : NULL;
}

void cai_conversation_item_destroy(cai_conversation_item *item) {
  if (item == NULL) {
    return;
  }
  cai_free_mem(NULL, item->id_value);
  cai_free_mem(NULL, item->type_value);
  cai_free_mem(NULL, item->role_value);
  cai_free_mem(NULL, item->raw_json_value);
  cai_free_mem(NULL, item);
}
