#include "cai_internal.h"

#include <cai/mcp.h>

#include <stdio.h>
#include <string.h>

typedef struct cai_mcp_jsonrpc_doc {
  lonejson_json_value id;
  char *jsonrpc;
  char *method;
  lonejson_json_value params;
} cai_mcp_jsonrpc_doc;

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

static int cai_mcp_write(cai_sink *sink, const char *text, cai_error *error);
static int cai_mcp_write_bytes(cai_sink *sink, const void *data, size_t len,
                               cai_error *error);
static int cai_mcp_write_json_string(cai_sink *sink, const char *value,
                                     cai_error *error);
static int cai_mcp_write_result_begin(cai_sink *sink,
                                      const lonejson_spooled *id,
                                      cai_error *error);

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
  int allow_legacy_no_version;
  void *user_context;
};

static const lonejson_field cai_mcp_jsonrpc_fields[] = {
    LONEJSON_FIELD_JSON_VALUE(cai_mcp_jsonrpc_doc, id, "id"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_jsonrpc_doc, jsonrpc, "jsonrpc"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_jsonrpc_doc, method, "method"),
    LONEJSON_FIELD_JSON_VALUE(cai_mcp_jsonrpc_doc, params, "params")};
LONEJSON_MAP_DEFINE(cai_mcp_jsonrpc_map, cai_mcp_jsonrpc_doc,
                    cai_mcp_jsonrpc_fields);

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

static int cai_mcp_write_tool_success_prefix(cai_sink *sink,
                                             cai_error *error) {
  int rc;

  rc = cai_mcp_write(sink, "{\"content\":[{\"type\":\"text\",\"text\":",
                     error);
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(sink, "structured JSON result", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write(sink, "}],\"structuredContent\":", error);
  }
  return rc;
}

static int cai_mcp_write_tool_success_suffix(cai_sink *sink,
                                             cai_error *error) {
  return cai_mcp_write(sink, ",\"isError\":false}}", error);
}

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

static int cai_mcp_tool_stream_begin(cai_mcp_tool_stream_context *stream,
                                     cai_error *error) {
  lonejson_status status;

  lonejson_error_init(&stream->json_error);
  status = lonejson_writer_init_sink(&stream->writer, cai_mcp_lonejson_sink,
                                     stream->response, NULL,
                                     &stream->json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_object(&stream->writer,
                                          &stream->json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_key(&stream->writer, "jsonrpc",
                                &stream->json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_string(&stream->writer, "2.0", 3U,
                                    &stream->json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_key(&stream->writer, "id", &stream->json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    if (stream->id != NULL && lonejson_spooled_size(stream->id) > 0U) {
      status = lonejson_writer_json_value_spooled(
          &stream->writer, stream->id, NULL, &stream->json_error);
    } else {
      status = lonejson_writer_null(&stream->writer, &stream->json_error);
    }
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_key(&stream->writer, "result",
                                &stream->json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_object(&stream->writer,
                                          &stream->json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_key(&stream->writer, "content",
                                &stream->json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_array(&stream->writer, &stream->json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_object(&stream->writer,
                                          &stream->json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_key(&stream->writer, "type", &stream->json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_string(&stream->writer, "text", 4U,
                                    &stream->json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_key(&stream->writer, "text", &stream->json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_string(&stream->writer, "structured JSON result",
                                    strlen("structured JSON result"),
                                    &stream->json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_object(&stream->writer, &stream->json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_array(&stream->writer, &stream->json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_mcp_writer_key(&stream->writer, "structuredContent",
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
    status = cai_mcp_writer_key(&stream->writer, "isError",
                                &stream->json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_bool(&stream->writer, 0, &stream->json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_object(&stream->writer, &stream->json_error);
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

static int cai_mcp_write(cai_sink *sink, const char *text, cai_error *error) {
  return cai_sink_write(sink, text, strlen(text), error);
}

static int cai_mcp_write_bytes(cai_sink *sink, const void *data, size_t len,
                               cai_error *error) {
  return cai_sink_write(sink, data, len, error);
}

static int cai_mcp_write_json_string(cai_sink *sink, const char *value,
                                     cai_error *error) {
  cai_json_builder builder;
  int rc;

  memset(&builder, 0, sizeof(builder));
  rc = cai_json_builder_string(&builder, value != NULL ? value : "", error);
  if (rc == CAI_OK) {
    rc = cai_sink_write(sink, builder.data, builder.length, error);
  }
  cai_free_mem(NULL, builder.data);
  return rc;
}

static int cai_mcp_write_spooled(cai_sink *sink,
                                 const lonejson_spooled *spool,
                                 cai_error *error) {
  lonejson_spooled cursor;
  lonejson_read_result chunk;
  unsigned char buffer[4096];
  lonejson_error json_error;

  cursor = *spool;
  lonejson_error_init(&json_error);
  if (lonejson_spooled_rewind(&cursor, &json_error) != LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to rewind JSON spool",
                                json_error.message);
  }
  for (;;) {
    chunk = lonejson_spooled_read(&cursor, buffer, sizeof(buffer));
    if (chunk.error_code != 0) {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "failed to read JSON spool");
    }
    if (chunk.bytes_read == 0U) {
      break;
    }
    if (cai_mcp_write_bytes(sink, buffer, chunk.bytes_read, error) !=
        CAI_OK) {
      return error != NULL ? error->code : CAI_ERR_TRANSPORT;
    }
  }
  return CAI_OK;
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
  int rc;

  rc = cai_mcp_write(sink, "{\"jsonrpc\":\"2.0\",\"id\":", error);
  if (rc == CAI_OK) {
    if (id != NULL && lonejson_spooled_size(id) > 0U) {
      rc = cai_mcp_write_spooled(sink, id, error);
    } else {
      rc = cai_mcp_write(sink, "null", error);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write(sink, ",\"error\":{\"code\":", error);
  }
  if (rc == CAI_OK) {
    char number[32];
    snprintf(number, sizeof(number), "%d", code);
    rc = cai_mcp_write(sink, number, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write(sink, ",\"message\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(sink, message, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write(sink, "}}", error);
  }
  return rc;
}

static int cai_mcp_write_result_begin(cai_sink *sink,
                                      const lonejson_spooled *id,
                                      cai_error *error) {
  int rc;

  rc = cai_mcp_write(sink, "{\"jsonrpc\":\"2.0\",\"id\":", error);
  if (rc == CAI_OK) {
    rc = cai_mcp_write_spooled(sink, id, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write(sink, ",\"result\":", error);
  }
  return rc;
}

static int cai_mcp_write_initialize(cai_mcp_handler *handler,
                                    cai_sink *sink,
                                    const lonejson_spooled *id,
                                    cai_error *error) {
  int rc;

  rc = cai_mcp_write_result_begin(sink, id, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_write(sink, "{\"protocolVersion\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(sink, handler->protocol_version, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write(sink, ",\"capabilities\":{\"tools\":{"
                            "\"listChanged\":false}},\"serverInfo\":{"
                            "\"name\":",
                       error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(sink, handler->name, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write(sink, ",\"version\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(sink, handler->version, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write(sink, "}}}", error);
  }
  return rc;
}

static int cai_mcp_write_ping(cai_sink *sink, const lonejson_spooled *id,
                              cai_error *error) {
  int rc;

  rc = cai_mcp_write_result_begin(sink, id, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_write(sink, "{}", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write(sink, "}", error);
  }
  return rc;
}

static int cai_mcp_write_tools_list(cai_mcp_handler *handler, cai_sink *sink,
                                    const lonejson_spooled *id,
                                    cai_error *error) {
  size_t i;
  size_t count;
  int rc;

  rc = cai_mcp_write_result_begin(sink, id, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_write(sink, "{\"tools\":[", error);
  }
  count = cai_tool_registry_count(handler->tools);
  for (i = 0U; rc == CAI_OK && i < count; i++) {
    if (i != 0U) {
      rc = cai_mcp_write(sink, ",", error);
    }
    if (rc == CAI_OK) {
      rc = cai_mcp_write(sink, "{\"name\":", error);
    }
    if (rc == CAI_OK) {
      rc = cai_mcp_write_json_string(
          sink, cai_tool_registry_name_at(handler->tools, i), error);
    }
    if (rc == CAI_OK &&
        cai_tool_registry_description_at(handler->tools, i) != NULL) {
      rc = cai_mcp_write(sink, ",\"description\":", error);
      if (rc == CAI_OK) {
        rc = cai_mcp_write_json_string(
            sink, cai_tool_registry_description_at(handler->tools, i), error);
      }
    }
    if (rc == CAI_OK) {
      rc = cai_mcp_write(sink, ",\"inputSchema\":", error);
    }
    if (rc == CAI_OK) {
      rc = cai_mcp_write(sink, cai_tool_registry_schema_at(handler->tools, i),
                         error);
    }
    if (rc == CAI_OK) {
      rc = cai_mcp_write(sink, "}", error);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write(sink, "]}}", error);
  }
  return rc;
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
      rc = cai_mcp_write_result_begin(sink, id, error);
      if (rc == CAI_OK) {
        rc = cai_mcp_write(sink,
                           "{\"content\":[{\"type\":\"text\",\"text\":",
                           error);
      }
      if (rc == CAI_OK) {
        rc = cai_mcp_write_json_string(
            sink, error != NULL && error->message != NULL ? error->message
                                                          : "tool failed",
            error);
      }
      if (rc == CAI_OK) {
        rc = cai_mcp_write(sink, "}],\"isError\":true}}", error);
      }
    } else {
      if (!stream_context.began) {
        rc = cai_mcp_write_result_begin(sink, id, error);
        if (rc == CAI_OK) {
          rc = cai_mcp_write_tool_success_prefix(sink, error);
        }
        if (rc == CAI_OK) {
          rc = cai_mcp_write(sink, "null", error);
        }
      }
      if (stream_context.began) {
        rc = cai_mcp_tool_stream_finish(&stream_context, error);
      } else if (rc == CAI_OK) {
        rc = cai_mcp_write_tool_success_suffix(sink, error);
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
    rc = cai_mcp_write_result_begin(sink, id, error);
    if (rc == CAI_OK) {
      rc = cai_mcp_write(sink,
                         "{\"content\":[{\"type\":\"text\",\"text\":",
                         error);
    }
    if (rc == CAI_OK) {
      rc = cai_mcp_write_json_string(
          sink, error != NULL && error->message != NULL ? error->message
                                                        : "tool failed",
          error);
    }
    if (rc == CAI_OK) {
      rc = cai_mcp_write(sink, "}],\"isError\":true}}", error);
    }
  } else {
    rc = cai_mcp_write_result_begin(sink, id, error);
    if (rc == CAI_OK) {
      rc = cai_mcp_write_tool_success_prefix(sink, error);
    }
    if (rc == CAI_OK) {
      rc = cai_mcp_write_spooled(sink, &output, error);
    }
    if (rc == CAI_OK) {
      rc = cai_mcp_write_tool_success_suffix(sink, error);
    }
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
  lonejson_spooled id_spool;
  lonejson_spooled params_spool;
  const char *accept;
  const char *content_type;
  const char *origin;
  const char *version;
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
  lonejson_spooled_init(&id_spool, NULL);
  lonejson_spooled_init(&params_spool, NULL);
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
  if (strcmp(doc.method, "notifications/initialized") == 0) {
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
  if (handler->allowed_origins != NULL) {
    for (i = 0U; i < handler->allowed_origin_count; i++) {
      cai_free_mem(NULL, handler->allowed_origins[i]);
    }
    cai_free_mem(NULL, handler->allowed_origins);
  }
  cai_free_mem(NULL, handler);
}
