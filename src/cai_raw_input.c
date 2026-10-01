#include "cai_internal.h"
#include <cai/mcp.h>
#include <string.h>

/* Metadata and schemas are small control documents, never tool payloads. */
typedef struct cai_raw_input_doc {
  char *field;
  char *framing;
  cai_custom_tool_format format;
} cai_raw_input_doc;
typedef struct cai_raw_meta_doc {
  cai_raw_input_doc mapping;
} cai_raw_meta_doc;
typedef struct cai_raw_property_doc {
  char *type;
} cai_raw_property_doc;
typedef struct cai_raw_schema_doc {
  char *type;
  cai_raw_property_doc property;
  lonejson_string_array required;
} cai_raw_schema_doc;

static const lonejson_field cai_raw_format_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_custom_tool_format, type, "type"),
    LONEJSON_FIELD_STRING_ALLOC(cai_custom_tool_format, syntax, "syntax"),
    LONEJSON_FIELD_STRING_ALLOC(cai_custom_tool_format, definition,
                                "definition")};
LONEJSON_MAP_DEFINE(cai_raw_format_map, cai_custom_tool_format,
                    cai_raw_format_fields);
static const lonejson_field cai_raw_doc_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_raw_input_doc, field, "field"),
    LONEJSON_FIELD_STRING_ALLOC(cai_raw_input_doc, framing, "framing"),
    LONEJSON_FIELD_OBJECT(cai_raw_input_doc, format, "format",
                          &cai_raw_format_map)};
LONEJSON_MAP_DEFINE(cai_raw_doc_map, cai_raw_input_doc, cai_raw_doc_fields);
static const lonejson_field cai_raw_meta_fields[] = {LONEJSON_FIELD_OBJECT(
    cai_raw_meta_doc, mapping, CAI_MCP_RAW_INPUT_META_KEY, &cai_raw_doc_map)};
LONEJSON_MAP_DEFINE(cai_raw_meta_map, cai_raw_meta_doc, cai_raw_meta_fields);
static const lonejson_field cai_raw_property_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_raw_property_doc, type, "type")};
LONEJSON_MAP_DEFINE(cai_raw_property_map, cai_raw_property_doc,
                    cai_raw_property_fields);
static const lonejson_field cai_raw_schema_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_raw_schema_doc, type, "type"),
    LONEJSON_FIELD_OBJECT_REQ(cai_raw_schema_doc, property, "properties", NULL),
    {"required", 8U, 'r', 'd', offsetof(cai_raw_schema_doc, required),
     LONEJSON_FIELD_KIND_STRING_ARRAY, LONEJSON_STORAGE_DYNAMIC,
     LONEJSON_OVERFLOW_FAIL, 0U, 0U, 0U, NULL, NULL, 0U,
     LONEJSON_SPOOL_CLASS_DEFAULT}};

int cai_custom_tool_format_validate(const cai_custom_tool_format *format,
                                    cai_error *error) {
  if (format == NULL || (format->type == NULL && format->syntax == NULL &&
                         format->definition == NULL))
    return CAI_OK;
  if (format->type == NULL || strcmp(format->type, "grammar") != 0 ||
      format->syntax == NULL ||
      (strcmp(format->syntax, "lark") != 0 &&
       strcmp(format->syntax, "regex") != 0) ||
      format->definition == NULL || format->definition[0] == '\0')
    return cai_set_error(
        error, CAI_ERR_INVALID,
        "custom tool format must be absent or a non-empty lark/regex grammar");
  return CAI_OK;
}
void cai_raw_input_cleanup(const cai_allocator *allocator,
                           cai_tool_raw_input *mapping) {
  const char *strings[4];
  size_t i;
  if (mapping == NULL)
    return;
  strings[0] = mapping->field;
  strings[1] = mapping->format.type;
  strings[2] = mapping->format.syntax;
  strings[3] = mapping->format.definition;
  for (i = 0U; i < 4U; i++) {
    void *owned;
    /* This internal descriptor owns these strings; its public view is const. */
    memcpy(&owned, &strings[i], sizeof(owned));
    cai_free_mem(allocator, owned);
  }
  memset(mapping, 0, sizeof(*mapping));
}
int cai_raw_input_copy(const cai_allocator *allocator, cai_tool_raw_input *out,
                       const cai_tool_raw_input *mapping, cai_error *error) {
  int rc;
  memset(out, 0, sizeof(*out));
  if (mapping == NULL || mapping->field == NULL || mapping->field[0] == '\0' ||
      (mapping->framing != CAI_TOOL_RAW_INPUT_TEXT &&
       mapping->framing != CAI_TOOL_RAW_INPUT_JSON_HEADER))
    return cai_set_error(
        error, CAI_ERR_INVALID,
        "raw-input mapping requires a string field and valid framing");
  rc = cai_custom_tool_format_validate(&mapping->format, error);
  if (rc != CAI_OK)
    return rc;
  out->field = cai_strdup(allocator, mapping->field);
  out->framing = mapping->framing;
  out->format.type = cai_strdup(allocator, mapping->format.type);
  out->format.syntax = cai_strdup(allocator, mapping->format.syntax);
  out->format.definition = cai_strdup(allocator, mapping->format.definition);
  if (out->field == NULL ||
      (mapping->format.type != NULL && out->format.type == NULL) ||
      (mapping->format.syntax != NULL && out->format.syntax == NULL) ||
      (mapping->format.definition != NULL && out->format.definition == NULL)) {
    cai_raw_input_cleanup(allocator, out);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to copy raw-input mapping");
  }
  return CAI_OK;
}
int cai_raw_input_validate(const char *schema_json,
                           const cai_tool_raw_input *mapping,
                           cai_error *error) {
  static const lonejson_field property_template = LONEJSON_FIELD_OBJECT_REQ(
      cai_raw_schema_doc, property, "field", &cai_raw_property_map);
  lonejson_field property_field;
  lonejson_field fields[3];
  lonejson_map properties;
  lonejson_map schema;
  cai_raw_schema_doc doc;
  lonejson_error json_error;
  int rc = CAI_OK;
  size_t i;
  if (schema_json == NULL || mapping == NULL || mapping->field == NULL ||
      mapping->field[0] == '\0')
    return cai_set_error(error, CAI_ERR_INVALID,
                         "raw-input schema and field are required");
  if (mapping->framing != CAI_TOOL_RAW_INPUT_TEXT &&
      mapping->framing != CAI_TOOL_RAW_INPUT_JSON_HEADER)
    return cai_set_error(error, CAI_ERR_INVALID, "invalid raw-input framing");
  rc = cai_custom_tool_format_validate(&mapping->format, error);
  if (rc != CAI_OK)
    return rc;
  property_field = property_template;
  memset(&properties, 0, sizeof(properties));
  properties.name = "raw-properties";
  properties.struct_size = sizeof(cai_raw_schema_doc);
  properties.fields = &property_field;
  properties.field_count = 1U;
  memset(&schema, 0, sizeof(schema));
  schema.name = "raw-schema";
  schema.struct_size = sizeof(cai_raw_schema_doc);
  schema.fields = fields;
  schema.field_count = 3U;
  property_field.json_key = mapping->field;
  property_field.json_key_len = strlen(mapping->field);
  property_field.json_key_first = (unsigned char)mapping->field[0];
  property_field.json_key_last =
      (unsigned char)mapping->field[property_field.json_key_len - 1U];
  memcpy(fields, cai_raw_schema_fields, sizeof(fields));
  /* The nested properties object has the same layout, but only property is
   * populated. */
  fields[1].struct_offset = 0U;
  fields[1].submap = &properties;
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
  if (CAI_LJ->parse_cstr(CAI_LJ, &schema, &doc, schema_json, &json_error) !=
      LONEJSON_STATUS_OK) {
    rc = cai_set_error_detail(
        error, CAI_ERR_INVALID,
        "raw-input field must name a declared string property",
        json_error.message);
  } else if (doc.type == NULL || doc.property.type == NULL ||
             strcmp(doc.type, "object") != 0 ||
             strcmp(doc.property.type, "string") != 0) {
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "raw-input field must name a declared string property "
                       "in an object schema");
  } else if (mapping->framing == CAI_TOOL_RAW_INPUT_TEXT) {
    for (i = 0U; i < doc.required.count; i++) {
      if (doc.required.items[i] == NULL ||
          strcmp(doc.required.items[i], mapping->field) != 0) {
        rc = cai_set_error(error, CAI_ERR_INVALID,
                           "raw-input text cannot supply other required "
                           "arguments; use json-header framing");
        break;
      }
    }
  }
  CAI_LJ->cleanup(CAI_LJ, &schema, &doc);
  return rc;
}
typedef struct cai_raw_meta_presence {
  char key[sizeof(CAI_MCP_RAW_INPUT_META_KEY)];
  size_t length;
  int advertised;
} cai_raw_meta_presence;
static lonejson_status cai_raw_meta_key_begin(void *user,
                                              const lonejson_value_path *path,
                                              lonejson_error *error) {
  cai_raw_meta_presence *presence = (cai_raw_meta_presence *)user;
  (void)error;
  if (path->segment_count == 0U)
    presence->length = 0U;
  return LONEJSON_STATUS_OK;
}
static lonejson_status cai_raw_meta_key_chunk(void *user,
                                              const lonejson_value_path *path,
                                              const char *data, size_t len,
                                              lonejson_error *error) {
  cai_raw_meta_presence *presence = (cai_raw_meta_presence *)user;
  (void)error;
  if (path->segment_count == 0U) {
    size_t copy = sizeof(presence->key) > presence->length
                      ? sizeof(presence->key) - presence->length
                      : 0U;
    if (copy > len)
      copy = len;
    if (copy != 0U)
      memcpy(presence->key + presence->length, data, copy);
    presence->length += len;
  }
  return LONEJSON_STATUS_OK;
}
static lonejson_status cai_raw_meta_key_end(void *user,
                                            const lonejson_value_path *path,
                                            lonejson_error *error) {
  cai_raw_meta_presence *presence = (cai_raw_meta_presence *)user;
  (void)error;
  if (path->segment_count == 0U &&
      presence->length == sizeof(CAI_MCP_RAW_INPUT_META_KEY) - 1U &&
      memcmp(presence->key, CAI_MCP_RAW_INPUT_META_KEY, presence->length) ==
          0) {
    if (presence->advertised)
      return LONEJSON_STATUS_DUPLICATE_FIELD;
    presence->advertised = 1;
  }
  return LONEJSON_STATUS_OK;
}

int cai_raw_input_parse_meta(const lonejson_json_value *meta,
                             const cai_allocator *allocator,
                             cai_tool_raw_input *out, cai_error *error) {
  cai_raw_meta_doc doc;
  cai_raw_meta_presence presence;
  lonejson_path_value_visitor visitor;
  cai_tool_raw_input mapping;
  lonejson_error json_error;
  int rc = CAI_OK;
  memset(out, 0, sizeof(*out));
  if (meta == NULL || meta->kind == LONEJSON_JSON_VALUE_NULL)
    return CAI_OK;
  if (meta->kind != LONEJSON_JSON_VALUE_BUFFER)
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP raw-input metadata must be captured JSON");
  memset(&presence, 0, sizeof(presence));
  visitor = lonejson_default_path_value_visitor();
  visitor.object_key_begin = cai_raw_meta_key_begin;
  visitor.object_key_chunk = cai_raw_meta_key_chunk;
  visitor.object_key_end = cai_raw_meta_key_end;
  lonejson_error_init(&json_error);
  if (CAI_LJ->visit_path_value_buffer(CAI_LJ, meta->json, meta->len, &visitor,
                                      &presence,
                                      &json_error) != LONEJSON_STATUS_OK)
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "invalid MCP raw-input metadata",
                                json_error.message);
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
  if (CAI_LJ->parse_buffer(CAI_LJ, &cai_raw_meta_map, &doc, meta->json,
                           meta->len, &json_error) != LONEJSON_STATUS_OK) {
    rc = cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                              "invalid MCP raw-input metadata",
                              json_error.message);
  } else if (presence.advertised && doc.mapping.field == NULL) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "MCP raw-input metadata requires a nonempty field");
  } else if (doc.mapping.field != NULL) {
    memset(&mapping, 0, sizeof(mapping));
    mapping.field = doc.mapping.field;
    mapping.format = doc.mapping.format;
    if (doc.mapping.framing != NULL &&
        strcmp(doc.mapping.framing, "text") != 0) {
      if (strcmp(doc.mapping.framing, "json-header") != 0)
        rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                           "unknown MCP raw-input framing");
      else
        mapping.framing = CAI_TOOL_RAW_INPUT_JSON_HEADER;
    }
    if (rc == CAI_OK)
      rc = cai_raw_input_copy(allocator, out, &mapping, error);
  }
  if (rc == CAI_ERR_INVALID) {
    rc = CAI_ERR_PROTOCOL;
    if (error != NULL)
      error->code = rc;
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_raw_meta_map, &doc);
  return rc;
}
lonejson_status cai_raw_input_write_meta(lonejson_writer *writer,
                                         const cai_tool_raw_input *mapping,
                                         lonejson_error *error) {
  lonejson_status status;
  status = writer->begin_object(writer, error);
  if (status == LONEJSON_STATUS_OK)
    status = writer->key(writer, "field", 5U, error);
  if (status == LONEJSON_STATUS_OK)
    status =
        writer->string(writer, mapping->field, strlen(mapping->field), error);
  if (status == LONEJSON_STATUS_OK)
    status = writer->key(writer, "framing", 7U, error);
  if (status == LONEJSON_STATUS_OK) {
    const char *framing = mapping->framing == CAI_TOOL_RAW_INPUT_JSON_HEADER
                              ? "json-header"
                              : "text";
    status = writer->string(writer, framing, strlen(framing), error);
  }
  if (status == LONEJSON_STATUS_OK && mapping->format.type != NULL) {
    status = writer->key(writer, "format", 6U, error);
    if (status == LONEJSON_STATUS_OK)
      status =
          writer->mapped(writer, &cai_raw_format_map, &mapping->format, error);
  }
  if (status == LONEJSON_STATUS_OK)
    status = writer->end_object(writer, error);
  return status;
}
