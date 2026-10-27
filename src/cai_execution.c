#include "cai_execution.h"
#include "cai_internal.h"
#include <stdio.h>
#include <string.h>

static const lonejson_field execution_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_execution_record, model, "model"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_execution_record, base_url,
                                          "base_url"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_execution_record,
                                          last_response_id, "last_response_id"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_execution_record, kind, "kind"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_execution_record, session_id,
                                    "session_id"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(
        cai_execution_record, parent_session_id, "parent_session_id"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_execution_record, parent_call_id,
                                          "parent_call_id"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_execution_record, subagent,
                                          "subagent"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_execution_record, tool_name,
                                          "tool_name"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_execution_record, call_id,
                                          "call_id"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_execution_record, field, "field"),
    LONEJSON_FIELD_U64_REQ(cai_execution_record, timestamp_ns, "timestamp_ns"),
    LONEJSON_FIELD_U64(cai_execution_record, source_sequence,
                       "source_sequence"),
    LONEJSON_FIELD_U64_REQ(cai_execution_record, offset, "offset"),
    LONEJSON_FIELD_BOOL_REQ(cai_execution_record, final, "final"),
    LONEJSON_FIELD_BOOL_REQ(cai_execution_record, line_start, "line_start"),
    LONEJSON_FIELD_I64(cai_execution_record, code, "code"),
    LONEJSON_FIELD_I64(cai_execution_record, http_status, "http_status"),
    LONEJSON_FIELD_STRING_FIXED_REQ(cai_execution_record, data, "data",
                                    LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(execution_map, cai_execution_record, execution_fields);

void cai_execution_record_init(cai_execution_record *record) {
  if (record != NULL)
    memset(record, 0, sizeof(*record));
}
void cai_execution_record_cleanup(cai_execution_record *record) {
  if (record != NULL) {
    CAI_LJ->cleanup(CAI_LJ, &execution_map, record);
    memset(record, 0, sizeof(*record));
  }
}
typedef struct execution_utf8 {
  unsigned long codepoint, minimum;
  unsigned int remaining;
} execution_utf8;
static lonejson_status execution_text_begin(void *context,
                                            lonejson_error *error) {
  (void)error;
  memset(context, 0, sizeof(execution_utf8));
  return LONEJSON_STATUS_OK;
}
static lonejson_status execution_text_end(void *context,
                                          lonejson_error *error) {
  execution_utf8 *text = context;
  (void)error;
  return text->remaining == 0U ? LONEJSON_STATUS_OK
                               : LONEJSON_STATUS_INVALID_JSON;
}
static lonejson_status execution_text_chunk(void *context, const char *bytes,
                                            size_t count,
                                            lonejson_error *error) {
  execution_utf8 *text = context;
  size_t i;
  (void)error;
  for (i = 0U; i < count; i++) {
    unsigned char c = (unsigned char)bytes[i];
    if (text->remaining == 0U) {
      if (c == 0U)
        return LONEJSON_STATUS_INVALID_JSON;
      if (c < 0x80U)
        continue;
      if (c >= 0xc2U && c <= 0xdfU) {
        text->remaining = 1U;
        text->codepoint = c & 0x1fU;
        text->minimum = 0x80UL;
      } else if (c >= 0xe0U && c <= 0xefU) {
        text->remaining = 2U;
        text->codepoint = c & 0x0fU;
        text->minimum = 0x800UL;
      } else if (c >= 0xf0U && c <= 0xf4U) {
        text->remaining = 3U;
        text->codepoint = c & 0x07U;
        text->minimum = 0x10000UL;
      } else
        return LONEJSON_STATUS_INVALID_JSON;
    } else {
      if ((c & 0xc0U) != 0x80U)
        return LONEJSON_STATUS_INVALID_JSON;
      text->codepoint = (text->codepoint << 6) | (c & 0x3fU);
      if (--text->remaining == 0U &&
          (text->codepoint < text->minimum || text->codepoint > 0x10ffffUL ||
           (text->codepoint >= 0xd800UL && text->codepoint <= 0xdfffUL)))
        return LONEJSON_STATUS_INVALID_JSON;
    }
  }
  return LONEJSON_STATUS_OK;
}
static lonejson_status execution_validate_json(const char *json, size_t length,
                                               lonejson_error *error) {
  lonejson_value_visitor visitor = lonejson_default_value_visitor();
  execution_utf8 text;
  memset(&text, 0, sizeof(text));
  visitor.string_begin = execution_text_begin;
  visitor.string_chunk = execution_text_chunk;
  visitor.string_end = execution_text_end;
  visitor.object_key_begin = execution_text_begin;
  visitor.object_key_chunk = execution_text_chunk;
  visitor.object_key_end = execution_text_end;
  return CAI_LJ->visit_value_buffer(CAI_LJ, json, length, &visitor, &text,
                                    error);
}
int cai_execution_record_parse(const char *json, cai_execution_record *record,
                               cai_error *error) {
  lonejson_error local;
  cai_execution_record parsed;
  size_t length;
  if (json == NULL || record == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "execution record is required");
  for (length = 0U;
       length <= CAI_EXECUTION_RECORD_MAX_BYTES && json[length] != '\0';
       length++) {
  }
  if (length > CAI_EXECUTION_RECORD_MAX_BYTES)
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "execution record exceeds 64 KiB");
  if (execution_validate_json(json, length, &local) != LONEJSON_STATUS_OK)
    return cai_set_error_detail(
        error, CAI_ERR_PROTOCOL,
        "execution record must contain valid JSON text without NUL bytes",
        local.message);
  cai_execution_record_init(&parsed);
  if (CAI_LJ->parse_cstr(CAI_LJ, &execution_map, &parsed, json, &local) !=
      LONEJSON_STATUS_OK) {
    cai_execution_record_cleanup(&parsed);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "invalid execution record", local.message);
  }
  if (parsed.kind[0] == '\0' || parsed.session_id[0] == '\0') {
    cai_execution_record_cleanup(&parsed);
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "execution record has no kind or session");
  }
  cai_execution_record_cleanup(record);
  *record = parsed;
  return CAI_OK;
}
typedef struct execution_json_buffer {
  cai_buffer_builder buffer;
  cai_error *error;
  int rc;
} execution_json_buffer;

static lonejson_status execution_write(void *context, const void *bytes,
                                       size_t count, lonejson_error *error) {
  execution_json_buffer *buffer = context;
  (void)error;
  if (count > CAI_EXECUTION_RECORD_MAX_BYTES - buffer->buffer.length) {
    buffer->rc = cai_set_error(buffer->error, CAI_ERR_LIMIT,
                               "execution record exceeds 64 KiB");
    return LONEJSON_STATUS_OVERFLOW;
  }
  buffer->rc = cai_buffer_append(&buffer->buffer, bytes, count, buffer->error);
  return buffer->rc == CAI_OK ? LONEJSON_STATUS_OK
                              : LONEJSON_STATUS_ALLOCATION_FAILED;
}
int cai_execution_record_json(const cai_execution_record *record, char **out,
                              cai_error *error) {
  execution_json_buffer buffer;
  lonejson_error local;
  memset(&buffer, 0, sizeof(buffer));
  buffer.error = error;
  *out = NULL;
  if (CAI_LJ->serialize_sink(CAI_LJ, &execution_map, record, execution_write,
                             &buffer, &local) != LONEJSON_STATUS_OK) {
    cai_free_mem(NULL, buffer.buffer.data);
    if (buffer.rc != CAI_OK)
      return buffer.rc;
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "serialize execution record", local.message);
  }
  if (execution_validate_json(buffer.buffer.data, buffer.buffer.length,
                              &local) != LONEJSON_STATUS_OK) {
    cai_free_mem(NULL, buffer.buffer.data);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "execution record is not valid UTF-8 text",
                                local.message);
  }
  *out = buffer.buffer.data;
  return CAI_OK;
}
static int execution_text(cai_sink *sink, const char *text, cai_error *error) {
  return cai_sink_write(sink, text, strlen(text), error);
}
/* Metadata may originate in provider IDs or imported records. Keep control
 * characters out of headings too, without altering the retained record. */
static int execution_label(cai_sink *sink, const char *text, cai_error *error) {
  size_t i, begin = 0U;
  int rc = CAI_OK;
  char escaped[8];
  for (i = 0U; text[i] != '\0' && rc == CAI_OK; i++) {
    unsigned int control = (unsigned char)text[i];
    if (control < 32U || control == 127U ||
        (control == 0xc2U && (unsigned char)text[i + 1U] >= 0x80U &&
         (unsigned char)text[i + 1U] <= 0x9fU)) {
      if (i > begin)
        rc = cai_sink_write(sink, text + begin, i - begin, error);
      if (control == 0xc2U)
        control = (unsigned char)text[++i];
      snprintf(escaped, sizeof(escaped), "\\u%04x", control);
      if (rc == CAI_OK)
        rc = execution_text(sink, escaped, error);
      begin = i + 1U;
    }
  }
  if (rc == CAI_OK && i > begin)
    rc = cai_sink_write(sink, text + begin, i - begin, error);
  return rc;
}
int cai_execution_record_write_markdown(const cai_execution_record *r,
                                        int compact, cai_sink *sink,
                                        cai_error *error) {
  size_t i, begin;
  int rc, line_start;
  int failed;
  const char *label;
  char status[96];
  if (r == NULL || r->kind == NULL || sink == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "execution record and sink are required");
  failed = strstr(r->kind, "failed") != NULL;
  if (compact &&
      (strncmp(r->kind, "turn.", 5U) == 0 ||
       strncmp(r->kind, "response.", 9U) == 0) &&
      !failed)
    return CAI_OK;
  if (compact && !failed && r->field != NULL &&
      strcmp(r->kind, "hosted.tool") != 0)
    return CAI_OK;
  label = strcmp(r->kind, "tool.started") == 0           ? "Tool called"
          : strcmp(r->kind, "tool.arguments") == 0       ? "Tool arguments"
          : strcmp(r->kind, "tool.output") == 0          ? "Tool result"
          : strcmp(r->kind, "tool.partial_output") == 0  ? "Partial tool result"
          : strcmp(r->kind, "tool.completed") == 0       ? "Tool completed"
          : strcmp(r->kind, "tool.failed") == 0          ? "Tool failed"
          : strcmp(r->kind, "turn.failed") == 0          ? "Turn failed"
          : strcmp(r->kind, "compaction.failed") == 0    ? "Compaction failed"
          : strcmp(r->kind, "request.failed") == 0       ? "Request failed"
          : strcmp(r->kind, "turn.started") == 0         ? "Turn started"
          : strcmp(r->kind, "turn.completed") == 0       ? "Turn completed"
          : strcmp(r->kind, "turn.cancelled") == 0       ? "Turn cancelled"
          : strcmp(r->kind, "response.completed") == 0   ? "Response completed"
          : strcmp(r->kind, "compaction.started") == 0   ? "Compaction started"
          : strcmp(r->kind, "compaction.completed") == 0 ? "Context compacted"
          : strcmp(r->kind, "goal.changed") == 0         ? "Goal status changed"
          : strcmp(r->kind, "subagent.input") == 0 ? "Subagent instruction"
          : strcmp(r->kind, "hosted.tool") == 0    ? "Hosted tool"
                                                   : r->kind;
  rc = CAI_OK;
  if (r->offset == 0U) {
    rc = execution_text(sink, "\n\n**", error);
    if (rc == CAI_OK)
      rc = execution_label(sink, label, error);
    if (rc == CAI_OK && r->tool_name != NULL) {
      rc = execution_text(sink, ": ", error);
      if (rc == CAI_OK)
        rc = execution_label(sink, r->tool_name, error);
    }
    if (rc == CAI_OK)
      rc = execution_text(sink, "**", error);
    if (rc == CAI_OK && r->field == NULL && !compact && r->call_id != NULL) {
      rc = execution_text(sink, " — call `", error);
      if (rc == CAI_OK)
        rc = execution_label(sink, r->call_id, error);
      if (rc == CAI_OK)
        rc = execution_text(sink, "`", error);
    }
    if (rc == CAI_OK && r->field == NULL &&
        (r->code != 0LL || r->http_status != 0LL)) {
      snprintf(status, sizeof(status), " (code %lld; HTTP %lld)", r->code,
               r->http_status);
      rc = execution_text(sink, status, error);
    }
    if (rc == CAI_OK && r->field != NULL) {
      rc = execution_text(sink, " — ", error);
      if (rc == CAI_OK)
        rc = execution_label(sink, r->field, error);
    }
    if (rc == CAI_OK)
      rc = execution_text(sink, "\n\n", error);
  }
  line_start = r->line_start;
  if (compact && !failed)
    return rc;
  begin = 0U;
  for (i = 0U; r->data[i] != '\0' && rc == CAI_OK; i++) {
    if (line_start) {
      rc = execution_text(sink, "    ", error);
      line_start = 0;
    }
    if (((unsigned char)r->data[i] < 32U && r->data[i] != '\n' &&
         r->data[i] != '\t') ||
        (unsigned char)r->data[i] == 127U ||
        ((unsigned char)r->data[i] == 0xc2U &&
         (unsigned char)r->data[i + 1U] >= 0x80U &&
         (unsigned char)r->data[i + 1U] <= 0x9fU)) {
      unsigned int control = (unsigned char)r->data[i];
      if (i > begin && rc == CAI_OK)
        rc = cai_sink_write(sink, r->data + begin, i - begin, error);
      if (control == 0xc2U)
        control = (unsigned char)r->data[++i];
      snprintf(status, sizeof(status), "\\u%04x", control);
      if (rc == CAI_OK)
        rc = execution_text(sink, status, error);
      begin = i + 1U;
      continue;
    }
    if (r->data[i] == '\n') {
      if (rc == CAI_OK)
        rc = cai_sink_write(sink, r->data + begin, i + 1U - begin, error);
      begin = i + 1U;
      line_start = 1;
    }
  }
  if (rc == CAI_OK && i > begin)
    rc = cai_sink_write(sink, r->data + begin, i - begin, error);
  if (rc == CAI_OK && r->final && !line_start)
    rc = execution_text(sink, "\n", error);
  return rc;
}
