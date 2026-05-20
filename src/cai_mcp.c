#include "cai_internal.h"

#include <cai/mcp.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct cai_mcp_jsonrpc_doc {
  lonejson_json_value id;
  char *jsonrpc;
  char *method;
  lonejson_json_value params;
} cai_mcp_jsonrpc_doc;

typedef struct cai_mcp_initialize_client_info_doc {
  char *name;
  char *version;
} cai_mcp_initialize_client_info_doc;

typedef struct cai_mcp_initialize_params_doc {
  char *protocol_version;
  cai_mcp_initialize_client_info_doc client_info;
} cai_mcp_initialize_params_doc;

typedef struct cai_mcp_call_params_doc {
  char *name;
  lonejson_json_value arguments;
} cai_mcp_call_params_doc;

typedef struct cai_mcp_source_reader {
  cai_source *source;
  cai_error *error;
  size_t total;
  size_t limit;
} cai_mcp_source_reader;

typedef struct cai_mcp_spooled_reader {
  lonejson_spooled cursor;
} cai_mcp_spooled_reader;

typedef struct cai_mcp_spool_sink_context {
  lonejson_spooled *spool;
  size_t total;
  size_t limit;
} cai_mcp_spool_sink_context;

typedef struct cai_mcp_tool_stream_context {
  cai_sink *response;
  const lonejson_spooled *id;
  lonejson_writer writer;
  lonejson_writer_value_stream value_stream;
  lonejson_error json_error;
  size_t total;
  int began;
  int value_open;
} cai_mcp_tool_stream_context;

struct cai_mcp_handler {
  char *name;
  char *version;
  cai_tool_registry *tools;
  size_t request_max_bytes;
  size_t response_spool_memory_limit;
  size_t tool_output_max_bytes;
  int stateless;
  int validate_origin;
  char **allowed_origins;
  size_t allowed_origin_count;
  char *protocol_version;
  char *last_session_id;
  int allow_legacy_no_version;
  cai_mcp_session_callbacks session;
  void *session_context;
  void *user_context;
};

static const lonejson_field cai_mcp_jsonrpc_fields[] = {
    LONEJSON_FIELD_JSON_VALUE(cai_mcp_jsonrpc_doc, id, "id"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_jsonrpc_doc, jsonrpc, "jsonrpc"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_jsonrpc_doc, method, "method"),
    LONEJSON_FIELD_JSON_VALUE(cai_mcp_jsonrpc_doc, params, "params")};
LONEJSON_MAP_DEFINE(cai_mcp_jsonrpc_map, cai_mcp_jsonrpc_doc,
                    cai_mcp_jsonrpc_fields);

static const lonejson_field cai_mcp_initialize_client_info_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_initialize_client_info_doc, name,
                                "name"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_initialize_client_info_doc, version,
                                "version")};
LONEJSON_MAP_DEFINE(cai_mcp_initialize_client_info_map,
                    cai_mcp_initialize_client_info_doc,
                    cai_mcp_initialize_client_info_fields);

static const lonejson_field cai_mcp_initialize_params_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_initialize_params_doc,
                                protocol_version, "protocolVersion"),
    LONEJSON_FIELD_OBJECT(cai_mcp_initialize_params_doc, client_info,
                          "clientInfo",
                          &cai_mcp_initialize_client_info_map)};
LONEJSON_MAP_DEFINE(cai_mcp_initialize_params_map,
                    cai_mcp_initialize_params_doc,
                    cai_mcp_initialize_params_fields);

static const lonejson_field cai_mcp_call_params_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_call_params_doc, name, "name"),
    LONEJSON_FIELD_JSON_VALUE_REQ(cai_mcp_call_params_doc, arguments,
                                  "arguments")};
LONEJSON_MAP_DEFINE(cai_mcp_call_params_map, cai_mcp_call_params_doc,
                    cai_mcp_call_params_fields);

static lonejson_status cai_mcp_spool_sink(void *user, const void *data,
                                          size_t len,
                                          lonejson_error *json_error) {
  if (lonejson_spooled_append((lonejson_spooled *)user, data, len,
                              json_error) == LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_OK;
  }
  return LONEJSON_STATUS_CALLBACK_FAILED;
}

static lonejson_read_result cai_mcp_source_read(void *user,
                                                unsigned char *buffer,
                                                size_t capacity) {
  cai_mcp_source_reader *reader;
  lonejson_read_result result;
  size_t got;

  reader = (cai_mcp_source_reader *)user;
  result = lonejson_default_read_result();
  got = cai_source_read(reader->source, buffer, capacity, reader->error);
  if (reader->limit > 0U &&
      (reader->total > reader->limit ||
       got > reader->limit - reader->total)) {
    if (reader->error != NULL) {
      cai_set_error(reader->error, CAI_ERR_INVALID,
                    "MCP request body exceeded configured limit");
    }
    result.error_code = CAI_ERR_INVALID;
    return result;
  }
  reader->total += got;
  result.bytes_read = got;
  result.eof = got == 0U ? 1 : 0;
  if (reader->error != NULL && reader->error->code != CAI_OK) {
    result.error_code = reader->error->code;
  }
  return result;
}

static lonejson_read_result cai_mcp_spooled_read(void *user,
                                                 unsigned char *buffer,
                                                 size_t capacity) {
  cai_mcp_spooled_reader *reader;

  reader = (cai_mcp_spooled_reader *)user;
  return lonejson_spooled_read(&reader->cursor, buffer, capacity);
}

static int cai_mcp_spool_write(void *context, const void *bytes, size_t count,
                               cai_error *error) {
  cai_mcp_spool_sink_context *sink_context;
  lonejson_error json_error;

  sink_context = (cai_mcp_spool_sink_context *)context;
  if (sink_context->limit > 0U &&
      (sink_context->total > sink_context->limit ||
       count > sink_context->limit - sink_context->total)) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP tool output exceeded configured limit");
  }
  lonejson_error_init(&json_error);
  if (lonejson_spooled_append(sink_context->spool, bytes, count,
                              &json_error) != LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to spool MCP tool output",
                                json_error.message);
  }
  sink_context->total += count;
  return CAI_OK;
}

static int cai_mcp_copy_fixed(char *dst, size_t capacity, const char *src,
                              const char *message, cai_error *error) {
  size_t len;

  if (dst == NULL || capacity == 0U) {
    return cai_set_error(error, CAI_ERR_INVALID, message);
  }
  dst[0] = '\0';
  if (src == NULL || src[0] == '\0') {
    return CAI_OK;
  }
  len = strlen(src);
  if (len >= capacity) {
    return cai_set_error(error, CAI_ERR_INVALID, message);
  }
  memcpy(dst, src, len + 1U);
  return CAI_OK;
}

static int cai_mcp_session_enabled(const cai_mcp_handler *handler) {
  return handler != NULL && !handler->stateless &&
         handler->session.create != NULL && handler->session.load != NULL &&
         handler->session.save != NULL;
}

static long long cai_mcp_now(void) { return (long long)time(NULL); }

static lonejson_status cai_mcp_lonejson_sink(void *user, const void *data,
                                             size_t len,
                                             lonejson_error *json_error) {
  cai_sink *sink;
  cai_error error;

  (void)json_error;
  sink = (cai_sink *)user;
  cai_error_init(&error);
  if (cai_sink_write(sink, data, len, &error) != CAI_OK) {
    cai_error_cleanup(&error);
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status
cai_mcp_writer_key(lonejson_writer *writer, const char *key,
                   lonejson_error *json_error) {
  return lonejson_writer_key(writer, key, strlen(key), json_error);
}

static lonejson_status
cai_mcp_writer_string_cstr(lonejson_writer *writer, const char *value,
                           lonejson_error *json_error) {
  if (value == NULL) {
    value = "";
  }
  return lonejson_writer_string(writer, value, strlen(value), json_error);
}

static lonejson_status
cai_mcp_writer_id(lonejson_writer *writer, const lonejson_spooled *id,
                  lonejson_error *json_error) {
  if (id != NULL && lonejson_spooled_size(id) > 0U) {
    return lonejson_writer_json_value_spooled(writer, id, NULL, json_error);
  }
  return lonejson_writer_null(writer, json_error);
}

static int cai_mcp_writer_error(lonejson_status status,
                                const lonejson_error *json_error,
                                const char *message, cai_error *error) {
  if (status == LONEJSON_STATUS_OK) {
    return CAI_OK;
  }
  return cai_set_error_detail(error, CAI_ERR_TRANSPORT, message,
                              json_error != NULL ? json_error->message : NULL);
}

static lonejson_status
cai_mcp_writer_begin_jsonrpc(lonejson_writer *writer,
                             const lonejson_spooled *id,
                             lonejson_error *json_error) {
  lonejson_status status;

  status = lonejson_writer_begin_object(writer, json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_key(writer, "jsonrpc", json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_string(writer, "2.0", 3U, json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_key(writer, "id", json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_id(writer, id, json_error);
  }
  return status;
}

static lonejson_status
cai_mcp_writer_begin_result(lonejson_writer *writer,
                            const lonejson_spooled *id,
                            lonejson_error *json_error) {
  lonejson_status status;

  status = cai_mcp_writer_begin_jsonrpc(writer, id, json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_key(writer, "result", json_error);
  }
  return status;
}

static lonejson_status
cai_mcp_writer_tool_content(lonejson_writer *writer, const char *text,
                            lonejson_error *json_error) {
  lonejson_status status;

  status = cai_mcp_writer_key(writer, "content", json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_array(writer, json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_object(writer, json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_key(writer, "type", json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_string(writer, "text", 4U, json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_key(writer, "text", json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_string_cstr(writer, text, json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_object(writer, json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_array(writer, json_error);
  }
  return status;
}

static lonejson_status
cai_mcp_writer_tool_success_begin(lonejson_writer *writer,
                                  lonejson_error *json_error) {
  lonejson_status status;

  status = lonejson_writer_begin_object(writer, json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_tool_content(writer, "structured JSON result",
                                         json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_key(writer, "structuredContent", json_error);
  }
  return status;
}

static lonejson_status
cai_mcp_writer_tool_success_end(lonejson_writer *writer,
                                lonejson_error *json_error) {
  lonejson_status status;

  status = cai_mcp_writer_key(writer, "isError", json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_bool(writer, 0, json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_object(writer, json_error);
  }
  return status;
}

static int cai_mcp_write_tool_result(cai_sink *sink,
                                     const lonejson_spooled *id,
                                     const lonejson_spooled *structured,
                                     const char *error_text, int is_error,
                                     cai_error *error) {
  lonejson_writer writer;
  lonejson_error json_error;
  lonejson_status status;

  lonejson_error_init(&json_error);
  status =
      lonejson_writer_init_sink(&writer, cai_mcp_lonejson_sink, sink, NULL,
                                &json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_begin_result(&writer, id, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_tool_content(
        &writer, is_error ? error_text : "structured JSON result",
        &json_error);
  }
  if (status == LONEJSON_STATUS_OK && !is_error) {
    status = cai_mcp_writer_key(&writer, "structuredContent", &json_error);
  }
  if (status == LONEJSON_STATUS_OK && !is_error) {
    if (structured != NULL && lonejson_spooled_size(structured) > 0U) {
      status = lonejson_writer_json_value_spooled(&writer, structured, NULL,
                                                  &json_error);
    } else {
      status = lonejson_writer_null(&writer, &json_error);
    }
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_key(&writer, "isError", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_bool(&writer, is_error ? 1 : 0, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_finish(&writer, &json_error);
  }
  lonejson_writer_cleanup(&writer);
  return cai_mcp_writer_error(status, &json_error,
                              "failed to write MCP tool result", error);
}

static int cai_mcp_tool_stream_begin(cai_mcp_tool_stream_context *stream,
                                     cai_error *error) {
  lonejson_status status;

  lonejson_error_init(&stream->json_error);
  status = lonejson_writer_init_sink(&stream->writer, cai_mcp_lonejson_sink,
                                     stream->response, NULL,
                                     &stream->json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_begin_result(&stream->writer, stream->id,
                                         &stream->json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_tool_success_begin(&stream->writer,
                                               &stream->json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_value_stream_open(
        &stream->value_stream, &stream->writer, NULL, &stream->json_error);
  }
  if (status != LONEJSON_STATUS_OK) {
    lonejson_writer_cleanup(&stream->writer);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to stream MCP tool response",
                                stream->json_error.message);
  }
  stream->began = 1;
  stream->value_open = 1;
  return CAI_OK;
}

static int cai_mcp_tool_stream_finish(cai_mcp_tool_stream_context *stream,
                                      cai_error *error) {
  lonejson_status status;

  if (!stream->began) {
    return CAI_OK;
  }
  status = LONEJSON_STATUS_OK;
  if (stream->value_open) {
    status = lonejson_writer_value_stream_close(&stream->value_stream,
                                                &stream->json_error);
    stream->value_open = 0;
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_tool_success_end(&stream->writer,
                                             &stream->json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_object(&stream->writer, &stream->json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_finish(&stream->writer, &stream->json_error);
  }
  lonejson_writer_value_stream_cleanup(&stream->value_stream);
  lonejson_writer_cleanup(&stream->writer);
  if (status != LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to finish MCP tool response",
                                stream->json_error.message);
  }
  return CAI_OK;
}

static int cai_mcp_tool_stream_write(void *context, const void *bytes,
                                     size_t count, cai_error *error) {
  cai_mcp_tool_stream_context *stream;
  lonejson_status status;

  stream = (cai_mcp_tool_stream_context *)context;
  if (!stream->began) {
    if (cai_mcp_tool_stream_begin(stream, error) != CAI_OK) {
      return error != NULL ? error->code : CAI_ERR_TRANSPORT;
    }
  }
  if (count == 0U) {
    return CAI_OK;
  }
  if (stream->total > (size_t)-1 - count) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP tool output byte count overflow");
  }
  status = lonejson_writer_value_stream_push(&stream->value_stream, bytes,
                                             count, &stream->json_error);
  if (status == LONEJSON_STATUS_OK) {
    stream->total += count;
    return CAI_OK;
  }
  return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                              "failed to stream MCP tool output",
                              stream->json_error.message);
}

static int cai_mcp_set_header(cai_mcp_http_response *response,
                              const char *name, const char *value,
                              cai_error *error) {
  if (response->set_header == NULL) {
    return CAI_OK;
  }
  return response->set_header(response->header_context, name, value, error);
}

static const char *cai_mcp_header(const cai_mcp_http_request *request,
                                  const char *name) {
  if (request == NULL || request->header == NULL) {
    return NULL;
  }
  return request->header(request->header_context, name);
}

static int cai_mcp_header_contains(const char *value, const char *needle) {
  const char *p;
  size_t needle_len;

  if (value == NULL || needle == NULL) {
    return 0;
  }
  needle_len = strlen(needle);
  p = value;
  while (*p != '\0') {
    if (strncmp(p, needle, needle_len) == 0) {
      return 1;
    }
    p++;
  }
  return 0;
}

static int cai_mcp_origin_allowed(cai_mcp_handler *handler,
                                  const char *origin) {
  size_t i;

  if (origin == NULL || origin[0] == '\0') {
    return 1;
  }
  if (handler->allowed_origin_count == 0U) {
    return 1;
  }
  for (i = 0U; i < handler->allowed_origin_count; i++) {
    if (handler->allowed_origins[i] != NULL &&
        strcmp(handler->allowed_origins[i], origin) == 0) {
      return 1;
    }
  }
  return 0;
}

static int cai_mcp_write_error(cai_sink *sink, const lonejson_spooled *id,
                               int code, const char *message,
                               cai_error *error) {
  lonejson_writer writer;
  lonejson_error json_error;
  lonejson_status status;

  lonejson_error_init(&json_error);
  status =
      lonejson_writer_init_sink(&writer, cai_mcp_lonejson_sink, sink, NULL,
                                &json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_begin_jsonrpc(&writer, id, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_key(&writer, "error", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_key(&writer, "code", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_i64(&writer, (long long)code, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_key(&writer, "message", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_string_cstr(&writer, message, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_finish(&writer, &json_error);
  }
  lonejson_writer_cleanup(&writer);
  return cai_mcp_writer_error(status, &json_error,
                              "failed to write MCP error", error);
}

static int cai_mcp_write_initialize(cai_mcp_handler *handler,
                                    cai_sink *sink,
                                    const lonejson_spooled *id,
                                    cai_error *error) {
  lonejson_writer writer;
  lonejson_error json_error;
  lonejson_status status;

  lonejson_error_init(&json_error);
  status =
      lonejson_writer_init_sink(&writer, cai_mcp_lonejson_sink, sink, NULL,
                                &json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_begin_result(&writer, id, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_key(&writer, "protocolVersion", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status =
        cai_mcp_writer_string_cstr(&writer, handler->protocol_version,
                                   &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_key(&writer, "capabilities", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_key(&writer, "tools", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_key(&writer, "listChanged", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_bool(&writer, 0, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_key(&writer, "serverInfo", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_key(&writer, "name", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_string_cstr(&writer, handler->name, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_key(&writer, "version", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status =
        cai_mcp_writer_string_cstr(&writer, handler->version, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_finish(&writer, &json_error);
  }
  lonejson_writer_cleanup(&writer);
  return cai_mcp_writer_error(status, &json_error,
                              "failed to write MCP initialize response",
                              error);
}

static int cai_mcp_write_ping(cai_sink *sink, const lonejson_spooled *id,
                              cai_error *error) {
  lonejson_writer writer;
  lonejson_error json_error;
  lonejson_status status;

  lonejson_error_init(&json_error);
  status =
      lonejson_writer_init_sink(&writer, cai_mcp_lonejson_sink, sink, NULL,
                                &json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_begin_result(&writer, id, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_finish(&writer, &json_error);
  }
  lonejson_writer_cleanup(&writer);
  return cai_mcp_writer_error(status, &json_error,
                              "failed to write MCP ping response", error);
}

static int cai_mcp_write_tools_list(cai_mcp_handler *handler, cai_sink *sink,
                                    const lonejson_spooled *id,
                                    cai_error *error) {
  size_t i;
  size_t count;
  lonejson_writer writer;
  lonejson_error json_error;
  lonejson_status status;

  lonejson_error_init(&json_error);
  status =
      lonejson_writer_init_sink(&writer, cai_mcp_lonejson_sink, sink, NULL,
                                &json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_begin_result(&writer, id, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_key(&writer, "tools", &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_array(&writer, &json_error);
  }
  count = cai_tool_registry_count(handler->tools);
  for (i = 0U; status == LONEJSON_STATUS_OK && i < count; i++) {
    status = lonejson_writer_begin_object(&writer, &json_error);
    if (status == LONEJSON_STATUS_OK) {
      status = cai_mcp_writer_key(&writer, "name", &json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = cai_mcp_writer_string_cstr(
          &writer, cai_tool_registry_name_at(handler->tools, i), &json_error);
    }
    if (status == LONEJSON_STATUS_OK &&
        cai_tool_registry_description_at(handler->tools, i) != NULL) {
      status = cai_mcp_writer_key(&writer, "description", &json_error);
      if (status == LONEJSON_STATUS_OK) {
        status = cai_mcp_writer_string_cstr(
            &writer, cai_tool_registry_description_at(handler->tools, i),
            &json_error);
      }
    }
    if (status == LONEJSON_STATUS_OK) {
      status = cai_mcp_writer_key(&writer, "inputSchema", &json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = lonejson_writer_json_value_buffer(
          &writer, cai_tool_registry_schema_at(handler->tools, i),
          strlen(cai_tool_registry_schema_at(handler->tools, i)), NULL,
          &json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = lonejson_writer_end_object(&writer, &json_error);
    }
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_array(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_finish(&writer, &json_error);
  }
  lonejson_writer_cleanup(&writer);
  return cai_mcp_writer_error(status, &json_error,
                              "failed to write MCP tools list", error);
}

static int cai_mcp_parse_call_params(const lonejson_spooled *params_spool,
                                     lonejson_spooled *arguments,
                                     char **out_name, cai_error *error) {
  cai_mcp_call_params_doc params;
  cai_mcp_spooled_reader reader;
  lonejson_parse_options options;
  lonejson_error json_error;

  memset(&params, 0, sizeof(params));
  lonejson_init(&cai_mcp_call_params_map, &params);
  lonejson_error_init(&json_error);
  lonejson_spooled_init(arguments, NULL);
  if (lonejson_json_value_set_parse_sink(&params.arguments,
                                         cai_mcp_spool_sink, arguments,
                                         &json_error) != LONEJSON_STATUS_OK) {
    lonejson_cleanup(&cai_mcp_call_params_map, &params);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to configure MCP arguments parser",
                                json_error.message);
  }
  reader.cursor = *params_spool;
  if (lonejson_spooled_rewind(&reader.cursor, &json_error) !=
      LONEJSON_STATUS_OK) {
    lonejson_cleanup(&cai_mcp_call_params_map, &params);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to rewind MCP params",
                                json_error.message);
  }
  options = lonejson_default_parse_options();
  options.clear_destination = 0;
  if (lonejson_parse_reader(&cai_mcp_call_params_map, &params,
                            cai_mcp_spooled_read, &reader, &options,
                            &json_error) != LONEJSON_STATUS_OK) {
    lonejson_cleanup(&cai_mcp_call_params_map, &params);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to parse MCP tool call params",
                                json_error.message);
  }
  *out_name = cai_strdup(NULL, params.name);
  if (*out_name == NULL) {
    lonejson_cleanup(&cai_mcp_call_params_map, &params);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate MCP tool name");
  }
  lonejson_cleanup(&cai_mcp_call_params_map, &params);
  return CAI_OK;
}

static int cai_mcp_run_tool(cai_mcp_handler *handler, cai_sink *sink,
                            const lonejson_spooled *id,
                            const lonejson_spooled *params_spool,
                            cai_error *error) {
  lonejson_spooled arguments;
  lonejson_spooled output;
  lonejson_spool_options spool_options;
  cai_sink_callbacks callbacks;
  cai_mcp_spool_sink_context spool_sink_context;
  cai_mcp_tool_stream_context stream_context;
  cai_sink *output_sink;
  char *name;
  int rc;

  name = NULL;
  output_sink = NULL;
  memset(&arguments, 0, sizeof(arguments));
  memset(&output, 0, sizeof(output));
  rc = cai_mcp_parse_call_params(params_spool, &arguments, &name, error);
  if (rc != CAI_OK) {
    lonejson_spooled_cleanup(&arguments);
    return cai_mcp_write_error(sink, id, -32602,
                               "Invalid tool call parameters", error);
  }

  if (handler->tool_output_max_bytes == 0U) {
    memset(&stream_context, 0, sizeof(stream_context));
    stream_context.response = sink;
    stream_context.id = id;
    callbacks.write = cai_mcp_tool_stream_write;
    callbacks.close = NULL;
    callbacks.context = &stream_context;
    rc = cai_sink_from_callbacks(&callbacks, &output_sink, error);
    if (rc == CAI_OK) {
      rc = cai_tool_registry_run_spooled(handler->tools, name, &arguments,
                                         output_sink, error);
    }
    cai_sink_close(output_sink);
    if (rc != CAI_OK) {
      if (stream_context.began) {
        lonejson_writer_value_stream_cleanup(&stream_context.value_stream);
        lonejson_writer_cleanup(&stream_context.writer);
        cai_free_mem(NULL, name);
        lonejson_spooled_cleanup(&arguments);
        return rc;
      }
      rc = cai_mcp_write_tool_result(
          sink, id, NULL,
          error != NULL && error->message != NULL ? error->message
                                                  : "tool failed",
          1, error);
    } else {
      if (!stream_context.began) {
        rc = cai_mcp_write_tool_result(sink, id, NULL, NULL, 0, error);
      }
      if (stream_context.began) {
        rc = cai_mcp_tool_stream_finish(&stream_context, error);
      }
    }
    cai_free_mem(NULL, name);
    lonejson_spooled_cleanup(&arguments);
    return rc;
  }

  spool_options = lonejson_default_spool_options();
  spool_options.memory_limit = handler->response_spool_memory_limit;
  lonejson_spooled_init(&output, &spool_options);
  memset(&spool_sink_context, 0, sizeof(spool_sink_context));
  spool_sink_context.spool = &output;
  spool_sink_context.limit = handler->tool_output_max_bytes;
  callbacks.write = cai_mcp_spool_write;
  callbacks.close = NULL;
  callbacks.context = &spool_sink_context;
  rc = cai_sink_from_callbacks(&callbacks, &output_sink, error);
  if (rc == CAI_OK) {
    rc = cai_tool_registry_run_spooled(handler->tools, name, &arguments,
                                       output_sink, error);
  }
  cai_sink_close(output_sink);
  if (rc != CAI_OK) {
    rc = cai_mcp_write_tool_result(
        sink, id, NULL,
        error != NULL && error->message != NULL ? error->message
                                                : "tool failed",
        1, error);
  } else {
    rc = cai_mcp_write_tool_result(sink, id, &output, NULL, 0, error);
  }
  cai_free_mem(NULL, name);
  lonejson_spooled_cleanup(&arguments);
  lonejson_spooled_cleanup(&output);
  return rc;
}

static int cai_mcp_parse_request(cai_source *source, size_t limit,
                                 cai_mcp_jsonrpc_doc *doc,
                                 lonejson_spooled *id_spool,
                                 lonejson_spooled *params_spool,
                                 cai_error *error) {
  cai_mcp_source_reader reader;
  lonejson_parse_options options;
  lonejson_error json_error;

  lonejson_init(&cai_mcp_jsonrpc_map, doc);
  lonejson_error_init(&json_error);
  if (lonejson_json_value_set_parse_sink(&doc->id, cai_mcp_spool_sink,
                                         id_spool, &json_error) !=
      LONEJSON_STATUS_OK ||
      lonejson_json_value_set_parse_sink(&doc->params, cai_mcp_spool_sink,
                                         params_spool, &json_error) !=
          LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to configure MCP request parser",
                                json_error.message);
  }
  reader.source = source;
  reader.error = error;
  reader.total = 0U;
  reader.limit = limit;
  options = lonejson_default_parse_options();
  options.clear_destination = 0;
  if (lonejson_parse_reader(&cai_mcp_jsonrpc_map, doc, cai_mcp_source_read,
                            &reader, &options, &json_error) !=
      LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to parse MCP request",
                                json_error.message);
  }
  return CAI_OK;
}

static int cai_mcp_parse_initialize_params(
    const lonejson_spooled *params_spool, cai_mcp_initialize_params_doc *params,
    cai_error *error) {
  cai_mcp_spooled_reader reader;
  lonejson_parse_options options;
  lonejson_error json_error;

  memset(params, 0, sizeof(*params));
  lonejson_init(&cai_mcp_initialize_params_map, params);
  if (params_spool == NULL || lonejson_spooled_size(params_spool) == 0U) {
    return CAI_OK;
  }
  reader.cursor = *params_spool;
  lonejson_error_init(&json_error);
  if (lonejson_spooled_rewind(&reader.cursor, &json_error) !=
      LONEJSON_STATUS_OK) {
    lonejson_cleanup(&cai_mcp_initialize_params_map, params);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to rewind MCP initialize params",
                                json_error.message);
  }
  options = lonejson_default_parse_options();
  if (lonejson_parse_reader(&cai_mcp_initialize_params_map, params,
                            cai_mcp_spooled_read, &reader, &options,
                            &json_error) != LONEJSON_STATUS_OK) {
    lonejson_cleanup(&cai_mcp_initialize_params_map, params);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to parse MCP initialize params",
                                json_error.message);
  }
  return CAI_OK;
}

void cai_mcp_handler_config_init(cai_mcp_handler_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->name = "cai";
  config->version = CAI_VERSION_STRING;
  config->request_max_bytes = 1024U * 1024U;
  config->response_spool_memory_limit = 128U * 1024U;
  config->stateless = 1;
  config->validate_origin = 1;
  config->protocol_version = CAI_MCP_PROTOCOL_VERSION;
  config->allow_legacy_no_version = 1;
}

int cai_mcp_handler_new(const cai_mcp_handler_config *config,
                        cai_mcp_handler **out, cai_error *error) {
  cai_mcp_handler_config defaults;
  cai_mcp_handler *handler;
  size_t i;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP handler output pointer is required");
  }
  *out = NULL;
  if (config == NULL) {
    cai_mcp_handler_config_init(&defaults);
    config = &defaults;
  }
  if (config->tools == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP handler requires a tool registry");
  }
  if (!config->stateless &&
      (config->session == NULL || config->session->create == NULL ||
       config->session->load == NULL || config->session->save == NULL)) {
    return cai_set_error(
        error, CAI_ERR_INVALID,
        "stateful MCP handler requires create, load, and save callbacks");
  }
  handler = (cai_mcp_handler *)cai_alloc(NULL, sizeof(*handler));
  if (handler == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate MCP handler");
  }
  memset(handler, 0, sizeof(*handler));
  handler->name = cai_strdup(NULL, config->name != NULL ? config->name : "cai");
  handler->version =
      cai_strdup(NULL, config->version != NULL ? config->version
                                               : CAI_VERSION_STRING);
  handler->protocol_version = cai_strdup(
      NULL, config->protocol_version != NULL ? config->protocol_version
                                             : CAI_MCP_PROTOCOL_VERSION);
  if (handler->name == NULL || handler->version == NULL ||
      handler->protocol_version == NULL) {
    cai_mcp_handler_destroy(handler);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate MCP handler strings");
  }
  handler->tools = config->tools;
  handler->request_max_bytes = config->request_max_bytes;
  handler->response_spool_memory_limit = config->response_spool_memory_limit;
  handler->tool_output_max_bytes = config->tool_output_max_bytes;
  handler->stateless = config->stateless ? 1 : 0;
  handler->validate_origin = config->validate_origin ? 1 : 0;
  handler->allow_legacy_no_version = config->allow_legacy_no_version ? 1 : 0;
  if (config->session != NULL) {
    handler->session = *config->session;
  }
  handler->session_context = config->session_context;
  handler->user_context = config->user_context;
  if (config->allowed_origin_count > 0U) {
    handler->allowed_origins = (char **)cai_alloc(
        NULL, config->allowed_origin_count * sizeof(handler->allowed_origins[0]));
    if (handler->allowed_origins == NULL) {
      cai_mcp_handler_destroy(handler);
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate MCP origins");
    }
    handler->allowed_origin_count = config->allowed_origin_count;
    for (i = 0U; i < config->allowed_origin_count; i++) {
      handler->allowed_origins[i] =
          cai_strdup(NULL, config->allowed_origins[i]);
      if (handler->allowed_origins[i] == NULL) {
        cai_mcp_handler_destroy(handler);
        return cai_set_error(error, CAI_ERR_NOMEM,
                             "failed to allocate MCP origin");
      }
    }
  }
  *out = handler;
  return CAI_OK;
}

int cai_mcp_handler_handle_http(cai_mcp_handler *handler,
                                const cai_mcp_http_request *request,
                                cai_mcp_http_response *response,
                                cai_error *error) {
  cai_mcp_jsonrpc_doc doc;
  cai_mcp_session_state session_state;
  lonejson_spooled id_spool;
  lonejson_spooled params_spool;
  const char *accept;
  const char *content_type;
  const char *origin;
  const char *session_id;
  const char *version;
  char created_session_id[CAI_MCP_SESSION_ID_MAX];
  int have_session;
  int rc;

  if (handler == NULL || request == NULL || response == NULL ||
      response->body == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP handler, request, and response body are required");
  }
  response->status = 200;
  rc = cai_mcp_set_header(response, "mcp-protocol-version",
                          handler->protocol_version, error);
  if (rc != CAI_OK) {
    return rc;
  }
  session_id = cai_mcp_header(request, "mcp-session-id");
  if (request->method != NULL && strcmp(request->method, "DELETE") == 0) {
    if (!cai_mcp_session_enabled(handler)) {
      response->status = 405;
      cai_mcp_set_header(response, "content-type", "application/json", error);
      return cai_mcp_write_error(response->body, NULL, -32600,
                                 "MCP handler only supports POST", error);
    }
    if (session_id == NULL || session_id[0] == '\0') {
      response->status = 400;
      cai_mcp_set_header(response, "content-type", "application/json", error);
      return cai_mcp_write_error(response->body, NULL, -32600,
                                 "Missing MCP session id", error);
    }
    if (handler->session.destroy != NULL) {
      rc = handler->session.destroy(handler->session_context, session_id,
                                    error);
      if (rc != CAI_OK) {
        response->status = 404;
        cai_mcp_set_header(response, "content-type", "application/json",
                           error);
        return cai_mcp_write_error(response->body, NULL, -32001,
                                   "MCP session not found", error);
      }
    }
    response->status = 202;
    return CAI_OK;
  }
  origin = cai_mcp_header(request, "origin");
  if (handler->validate_origin && !cai_mcp_origin_allowed(handler, origin)) {
    response->status = 403;
    cai_mcp_set_header(response, "content-type", "application/json", error);
    return cai_mcp_write_error(response->body, NULL, -32000,
                               "Forbidden origin", error);
  }
  if (request->method == NULL || strcmp(request->method, "POST") != 0) {
    response->status = 405;
    cai_mcp_set_header(response, "content-type", "application/json", error);
    return cai_mcp_write_error(response->body, NULL, -32600,
                               "MCP handler only supports POST", error);
  }
  content_type = cai_mcp_header(request, "content-type");
  if (content_type != NULL &&
      !cai_mcp_header_contains(content_type, "application/json")) {
    response->status = 415;
    cai_mcp_set_header(response, "content-type", "application/json", error);
    return cai_mcp_write_error(response->body, NULL, -32600,
                               "MCP POST requires application/json", error);
  }
  accept = cai_mcp_header(request, "accept");
  if (accept != NULL &&
      !cai_mcp_header_contains(accept, "application/json") &&
      !cai_mcp_header_contains(accept, "text/event-stream")) {
    response->status = 406;
    cai_mcp_set_header(response, "content-type", "application/json", error);
    return cai_mcp_write_error(response->body, NULL, -32600,
                               "MCP Accept must allow JSON or SSE", error);
  }
  version = cai_mcp_header(request, "mcp-protocol-version");
  if ((version == NULL || version[0] == '\0') &&
      !handler->allow_legacy_no_version) {
    response->status = 400;
    cai_mcp_set_header(response, "content-type", "application/json", error);
    return cai_mcp_write_error(response->body, NULL, -32600,
                               "Missing MCP protocol version", error);
  }
  if (version != NULL && version[0] != '\0' &&
      strcmp(version, handler->protocol_version) != 0 &&
      strcmp(version, "2025-03-26") != 0) {
    response->status = 400;
    cai_mcp_set_header(response, "content-type", "application/json", error);
    return cai_mcp_write_error(response->body, NULL, -32600,
                               "Unsupported MCP protocol version", error);
  }
  if (request->body == NULL) {
    response->status = 400;
    cai_mcp_set_header(response, "content-type", "application/json", error);
    return cai_mcp_write_error(response->body, NULL, -32700,
                               "Missing MCP request body", error);
  }
  memset(&doc, 0, sizeof(doc));
  memset(&session_state, 0, sizeof(session_state));
  lonejson_spooled_init(&id_spool, NULL);
  lonejson_spooled_init(&params_spool, NULL);
  created_session_id[0] = '\0';
  have_session = 0;
  rc = cai_mcp_parse_request(request->body, handler->request_max_bytes, &doc,
                             &id_spool, &params_spool, error);
  if (rc != CAI_OK) {
    response->status = 400;
    cai_mcp_set_header(response, "content-type", "application/json", error);
    rc = cai_mcp_write_error(response->body, NULL, -32700, "Parse error",
                             error);
    goto done;
  }
  if (doc.jsonrpc == NULL || strcmp(doc.jsonrpc, "2.0") != 0) {
    response->status = 400;
    cai_mcp_set_header(response, "content-type", "application/json", error);
    rc = cai_mcp_write_error(response->body, &id_spool, -32600,
                             "Invalid JSON-RPC version", error);
    goto done;
  }
  if (cai_mcp_session_enabled(handler) && strcmp(doc.method, "initialize") != 0) {
    if (session_id == NULL || session_id[0] == '\0') {
      response->status = 400;
      cai_mcp_set_header(response, "content-type", "application/json", error);
      rc = cai_mcp_write_error(response->body, &id_spool, -32001,
                               "Missing MCP session id", error);
      goto done;
    }
    rc = handler->session.load(handler->session_context, session_id,
                               &session_state, error);
    if (rc != CAI_OK) {
      response->status = 404;
      cai_mcp_set_header(response, "content-type", "application/json", error);
      rc = cai_mcp_write_error(response->body, &id_spool, -32001,
                               "MCP session not found", error);
      goto done;
    }
    have_session = 1;
    session_state.last_seen_at = cai_mcp_now();
  }
  if (strcmp(doc.method, "notifications/initialized") == 0) {
    if (have_session) {
      session_state.initialized = 1;
      rc = handler->session.save(handler->session_context, session_id,
                                 &session_state, error);
      if (rc != CAI_OK) {
        response->status = 500;
        cai_mcp_set_header(response, "content-type", "application/json",
                           error);
        rc = cai_mcp_write_error(response->body, &id_spool, -32000,
                                 "Failed to save MCP session", error);
        goto done;
      }
    }
    response->status = 202;
    rc = CAI_OK;
    goto done;
  }
  cai_mcp_set_header(response, "content-type", "application/json", error);
  if (lonejson_spooled_size(&id_spool) == 0U) {
    response->status = 202;
    rc = CAI_OK;
    goto done;
  }
  if (strcmp(doc.method, "initialize") == 0) {
    if (cai_mcp_session_enabled(handler)) {
      cai_mcp_initialize_params_doc init_params;

      rc = cai_mcp_parse_initialize_params(&params_spool, &init_params, error);
      if (rc != CAI_OK) {
        response->status = 400;
        cai_mcp_set_header(response, "content-type", "application/json",
                           error);
        rc = cai_mcp_write_error(response->body, &id_spool, -32602,
                                 "Invalid initialize params", error);
        goto done;
      }
      memset(&session_state, 0, sizeof(session_state));
      session_state.initialized = 0;
      session_state.created_at = cai_mcp_now();
      session_state.last_seen_at = session_state.created_at;
      rc = cai_mcp_copy_fixed(
          session_state.protocol_version,
          sizeof(session_state.protocol_version),
          init_params.protocol_version != NULL ? init_params.protocol_version
                                               : handler->protocol_version,
          "MCP protocol version is too large", error);
      if (rc == CAI_OK) {
        rc = cai_mcp_copy_fixed(session_state.client_name,
                                sizeof(session_state.client_name),
                                init_params.client_info.name,
                                "MCP client name is too large", error);
      }
      if (rc == CAI_OK) {
        rc = cai_mcp_copy_fixed(session_state.client_version,
                                sizeof(session_state.client_version),
                                init_params.client_info.version,
                                "MCP client version is too large", error);
      }
      if (rc == CAI_OK) {
        rc = handler->session.create(handler->session_context, &session_state,
                                     created_session_id,
                                     sizeof(created_session_id), error);
      }
      lonejson_cleanup(&cai_mcp_initialize_params_map, &init_params);
      if (rc != CAI_OK) {
        response->status = 500;
        cai_mcp_set_header(response, "content-type", "application/json",
                           error);
        rc = cai_mcp_write_error(response->body, &id_spool, -32000,
                                 "Failed to create MCP session", error);
        goto done;
      }
      if (created_session_id[0] == '\0') {
        response->status = 500;
        cai_mcp_set_header(response, "content-type", "application/json",
                           error);
        rc = cai_mcp_write_error(response->body, &id_spool, -32000,
                                 "MCP session id is empty", error);
        goto done;
      }
      cai_free_mem(NULL, handler->last_session_id);
      handler->last_session_id = cai_strdup(NULL, created_session_id);
      if (handler->last_session_id == NULL) {
        response->status = 500;
        cai_mcp_set_header(response, "content-type", "application/json",
                           error);
        rc = cai_mcp_write_error(response->body, &id_spool, -32000,
                                 "Failed to allocate MCP session id", error);
        goto done;
      }
      rc = cai_mcp_set_header(response, "mcp-session-id",
                              handler->last_session_id, error);
      if (rc != CAI_OK) {
        goto done;
      }
    }
    rc = cai_mcp_write_initialize(handler, response->body, &id_spool, error);
  } else if (strcmp(doc.method, "ping") == 0) {
    rc = cai_mcp_write_ping(response->body, &id_spool, error);
  } else if (strcmp(doc.method, "tools/list") == 0) {
    rc = cai_mcp_write_tools_list(handler, response->body, &id_spool, error);
  } else if (strcmp(doc.method, "tools/call") == 0) {
    rc = cai_mcp_run_tool(handler, response->body, &id_spool, &params_spool,
                          error);
  } else {
    rc = cai_mcp_write_error(response->body, &id_spool, -32601,
                             "Method not found", error);
  }
  if (rc == CAI_OK && have_session) {
    rc = handler->session.save(handler->session_context, session_id,
                               &session_state, error);
  }

done:
  lonejson_cleanup(&cai_mcp_jsonrpc_map, &doc);
  lonejson_spooled_cleanup(&id_spool);
  lonejson_spooled_cleanup(&params_spool);
  return rc;
}

void cai_mcp_handler_destroy(cai_mcp_handler *handler) {
  size_t i;

  if (handler == NULL) {
    return;
  }
  cai_free_mem(NULL, handler->name);
  cai_free_mem(NULL, handler->version);
  cai_free_mem(NULL, handler->protocol_version);
  cai_free_mem(NULL, handler->last_session_id);
  if (handler->allowed_origins != NULL) {
    for (i = 0U; i < handler->allowed_origin_count; i++) {
      cai_free_mem(NULL, handler->allowed_origins[i]);
    }
    cai_free_mem(NULL, handler->allowed_origins);
  }
  if (handler->session.cleanup != NULL) {
    handler->session.cleanup(handler->session_context);
  }
  cai_free_mem(NULL, handler);
}
