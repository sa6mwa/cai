#include "cai_internal.h"

#include <cai/mcp.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct cai_mcp_client_tool_impl {
  cai_mcp_client_tool public_tool;
  char *name;
  char *description;
  char *input_schema_json;
} cai_mcp_client_tool_impl;

typedef struct cai_mcp_client_resource_impl {
  cai_mcp_client_resource public_resource;
  char *uri;
  char *name;
  char *title;
  char *description;
  char *mime_type;
} cai_mcp_client_resource_impl;

typedef struct cai_mcp_client_resource_template_impl {
  cai_mcp_client_resource_template public_resource_template;
  char *uri_template;
  char *name;
  char *title;
  char *description;
  char *mime_type;
} cai_mcp_client_resource_template_impl;

typedef struct cai_mcp_client_prompt_impl {
  cai_mcp_client_prompt public_prompt;
  char *name;
  char *title;
  char *description;
  char *arguments_json;
} cai_mcp_client_prompt_impl;

typedef struct cai_mcp_streamable_http_client_impl {
  cai_mcp_client public_client;
  cai_allocator allocator;
  char *url;
  char *client_name;
  char *client_version;
  char *protocol_version;
  long timeout_ms;
  int insecure_skip_verify;
  char *ca_bundle_path;
  char *ca_path;
  char *session_id;
  int initialized;
  long long next_id;
  cai_mcp_client_tool_impl *tools;
  size_t tool_count;
  size_t tool_capacity;
  cai_mcp_client_resource_impl *resources;
  size_t resource_count;
  size_t resource_capacity;
  cai_mcp_client_resource_template_impl *resource_templates;
  size_t resource_template_count;
  size_t resource_template_capacity;
  cai_mcp_client_prompt_impl *prompts;
  size_t prompt_count;
  size_t prompt_capacity;
} cai_mcp_streamable_http_client_impl;

typedef struct cai_mcp_http_response_capture {
  lonejson_spooled body;
  char *content_type;
  char *session_id;
  long status;
} cai_mcp_http_response_capture;

typedef struct cai_mcp_spooled_upload {
  lonejson_spooled cursor;
  int rewound;
} cai_mcp_spooled_upload;

typedef struct cai_mcp_spooled_reader {
  lonejson_spooled cursor;
} cai_mcp_spooled_reader;

typedef struct cai_mcp_jsonrpc_error_doc {
  int64_t code;
  char *message;
} cai_mcp_jsonrpc_error_doc;

typedef struct cai_mcp_jsonrpc_sink_response_doc {
  lonejson_json_value result;
  cai_mcp_jsonrpc_error_doc error_doc;
} cai_mcp_jsonrpc_sink_response_doc;

typedef struct cai_mcp_list_tool_doc {
  char *name;
  char *description;
  char *title;
  lonejson_json_value input_schema;
} cai_mcp_list_tool_doc;

typedef struct cai_mcp_tools_list_result_doc {
  lonejson_object_array tools;
  char *next_cursor;
} cai_mcp_tools_list_result_doc;

typedef struct cai_mcp_tools_list_response_doc {
  cai_mcp_tools_list_result_doc result;
  cai_mcp_jsonrpc_error_doc error_doc;
} cai_mcp_tools_list_response_doc;

typedef struct cai_mcp_list_resource_doc {
  char *uri;
  char *name;
  char *title;
  char *description;
  char *mime_type;
} cai_mcp_list_resource_doc;

typedef struct cai_mcp_resources_list_result_doc {
  lonejson_object_array resources;
  char *next_cursor;
} cai_mcp_resources_list_result_doc;

typedef struct cai_mcp_resources_list_response_doc {
  cai_mcp_resources_list_result_doc result;
  cai_mcp_jsonrpc_error_doc error_doc;
} cai_mcp_resources_list_response_doc;

typedef struct cai_mcp_list_resource_template_doc {
  char *uri_template;
  char *name;
  char *title;
  char *description;
  char *mime_type;
} cai_mcp_list_resource_template_doc;

typedef struct cai_mcp_resource_templates_list_result_doc {
  lonejson_object_array resource_templates;
  char *next_cursor;
} cai_mcp_resource_templates_list_result_doc;

typedef struct cai_mcp_resource_templates_list_response_doc {
  cai_mcp_resource_templates_list_result_doc result;
  cai_mcp_jsonrpc_error_doc error_doc;
} cai_mcp_resource_templates_list_response_doc;

typedef struct cai_mcp_list_prompt_doc {
  char *name;
  char *title;
  char *description;
  lonejson_json_value arguments;
} cai_mcp_list_prompt_doc;

typedef struct cai_mcp_prompts_list_result_doc {
  lonejson_object_array prompts;
  char *next_cursor;
} cai_mcp_prompts_list_result_doc;

typedef struct cai_mcp_prompts_list_response_doc {
  cai_mcp_prompts_list_result_doc result;
  cai_mcp_jsonrpc_error_doc error_doc;
} cai_mcp_prompts_list_response_doc;

typedef struct cai_mcp_initialize_result_doc {
  char *protocol_version;
} cai_mcp_initialize_result_doc;

typedef struct cai_mcp_initialize_response_doc {
  cai_mcp_initialize_result_doc result;
  cai_mcp_jsonrpc_error_doc error_doc;
} cai_mcp_initialize_response_doc;

typedef struct cai_mcp_registry_tool_context {
  cai_mcp_client *client;
  char *remote_name;
} cai_mcp_registry_tool_context;

static const lonejson_field cai_mcp_jsonrpc_error_fields[] = {
    LONEJSON_FIELD_I64(cai_mcp_jsonrpc_error_doc, code, "code"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_jsonrpc_error_doc, message, "message")};
LONEJSON_MAP_DEFINE(cai_mcp_jsonrpc_error_map, cai_mcp_jsonrpc_error_doc,
                    cai_mcp_jsonrpc_error_fields);

static const lonejson_field cai_mcp_jsonrpc_sink_response_fields[] = {
    LONEJSON_FIELD_JSON_VALUE(cai_mcp_jsonrpc_sink_response_doc, result,
                              "result"),
    LONEJSON_FIELD_OBJECT(cai_mcp_jsonrpc_sink_response_doc, error_doc, "error",
                          &cai_mcp_jsonrpc_error_map)};
LONEJSON_MAP_DEFINE(cai_mcp_jsonrpc_sink_response_map,
                    cai_mcp_jsonrpc_sink_response_doc,
                    cai_mcp_jsonrpc_sink_response_fields);

static const lonejson_field cai_mcp_list_tool_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_list_tool_doc, name, "name"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_list_tool_doc, description,
                                "description"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_list_tool_doc, title, "title"),
    {"inputSchema", LONEJSON__KEY_LEN("inputSchema"),
     LONEJSON__KEY_FIRST("inputSchema"), LONEJSON__KEY_LAST("inputSchema"),
     offsetof(cai_mcp_list_tool_doc, input_schema),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT}};
LONEJSON_MAP_DEFINE(cai_mcp_list_tool_map, cai_mcp_list_tool_doc,
                    cai_mcp_list_tool_fields);

static const lonejson_field cai_mcp_tools_list_result_fields[] = {
    LONEJSON_FIELD_OBJECT_ARRAY(cai_mcp_tools_list_result_doc, tools, "tools",
                                cai_mcp_list_tool_doc, &cai_mcp_list_tool_map,
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_tools_list_result_doc, next_cursor,
                                "nextCursor")};
LONEJSON_MAP_DEFINE(cai_mcp_tools_list_result_map,
                    cai_mcp_tools_list_result_doc,
                    cai_mcp_tools_list_result_fields);

static const lonejson_field cai_mcp_tools_list_response_fields[] = {
    LONEJSON_FIELD_OBJECT(cai_mcp_tools_list_response_doc, result, "result",
                          &cai_mcp_tools_list_result_map),
    LONEJSON_FIELD_OBJECT(cai_mcp_tools_list_response_doc, error_doc, "error",
                          &cai_mcp_jsonrpc_error_map)};
LONEJSON_MAP_DEFINE(cai_mcp_tools_list_response_map,
                    cai_mcp_tools_list_response_doc,
                    cai_mcp_tools_list_response_fields);

static const lonejson_field cai_mcp_list_resource_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_list_resource_doc, uri, "uri"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_list_resource_doc, name, "name"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_list_resource_doc, title, "title"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_list_resource_doc, description,
                                "description"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_list_resource_doc, mime_type,
                                "mimeType")};
LONEJSON_MAP_DEFINE(cai_mcp_list_resource_map, cai_mcp_list_resource_doc,
                    cai_mcp_list_resource_fields);

static const lonejson_field cai_mcp_resources_list_result_fields[] = {
    LONEJSON_FIELD_OBJECT_ARRAY(cai_mcp_resources_list_result_doc, resources,
                                "resources", cai_mcp_list_resource_doc,
                                &cai_mcp_list_resource_map,
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_resources_list_result_doc, next_cursor,
                                "nextCursor")};
LONEJSON_MAP_DEFINE(cai_mcp_resources_list_result_map,
                    cai_mcp_resources_list_result_doc,
                    cai_mcp_resources_list_result_fields);

static const lonejson_field cai_mcp_resources_list_response_fields[] = {
    LONEJSON_FIELD_OBJECT(cai_mcp_resources_list_response_doc, result, "result",
                          &cai_mcp_resources_list_result_map),
    LONEJSON_FIELD_OBJECT(cai_mcp_resources_list_response_doc, error_doc,
                          "error", &cai_mcp_jsonrpc_error_map)};
LONEJSON_MAP_DEFINE(cai_mcp_resources_list_response_map,
                    cai_mcp_resources_list_response_doc,
                    cai_mcp_resources_list_response_fields);

static const lonejson_field cai_mcp_list_resource_template_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_list_resource_template_doc,
                                    uri_template, "uriTemplate"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_list_resource_template_doc, name,
                                    "name"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_list_resource_template_doc, title,
                                "title"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_list_resource_template_doc, description,
                                "description"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_list_resource_template_doc, mime_type,
                                "mimeType")};
LONEJSON_MAP_DEFINE(cai_mcp_list_resource_template_map,
                    cai_mcp_list_resource_template_doc,
                    cai_mcp_list_resource_template_fields);

static const lonejson_field cai_mcp_resource_templates_list_result_fields[] = {
    LONEJSON_FIELD_OBJECT_ARRAY(
        cai_mcp_resource_templates_list_result_doc, resource_templates,
        "resourceTemplates", cai_mcp_list_resource_template_doc,
        &cai_mcp_list_resource_template_map, LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_resource_templates_list_result_doc,
                                next_cursor, "nextCursor")};
LONEJSON_MAP_DEFINE(cai_mcp_resource_templates_list_result_map,
                    cai_mcp_resource_templates_list_result_doc,
                    cai_mcp_resource_templates_list_result_fields);

static const lonejson_field cai_mcp_resource_templates_list_response_fields[] =
    {LONEJSON_FIELD_OBJECT(cai_mcp_resource_templates_list_response_doc, result,
                           "result",
                           &cai_mcp_resource_templates_list_result_map),
     LONEJSON_FIELD_OBJECT(cai_mcp_resource_templates_list_response_doc,
                           error_doc, "error", &cai_mcp_jsonrpc_error_map)};
LONEJSON_MAP_DEFINE(cai_mcp_resource_templates_list_response_map,
                    cai_mcp_resource_templates_list_response_doc,
                    cai_mcp_resource_templates_list_response_fields);

static const lonejson_field cai_mcp_list_prompt_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_list_prompt_doc, name, "name"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_list_prompt_doc, title, "title"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_list_prompt_doc, description,
                                "description"),
    {"arguments", LONEJSON__KEY_LEN("arguments"),
     LONEJSON__KEY_FIRST("arguments"), LONEJSON__KEY_LAST("arguments"),
     offsetof(cai_mcp_list_prompt_doc, arguments),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT}};
LONEJSON_MAP_DEFINE(cai_mcp_list_prompt_map, cai_mcp_list_prompt_doc,
                    cai_mcp_list_prompt_fields);

static const lonejson_field cai_mcp_prompts_list_result_fields[] = {
    LONEJSON_FIELD_OBJECT_ARRAY(cai_mcp_prompts_list_result_doc, prompts,
                                "prompts", cai_mcp_list_prompt_doc,
                                &cai_mcp_list_prompt_map,
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_prompts_list_result_doc, next_cursor,
                                "nextCursor")};
LONEJSON_MAP_DEFINE(cai_mcp_prompts_list_result_map,
                    cai_mcp_prompts_list_result_doc,
                    cai_mcp_prompts_list_result_fields);

static const lonejson_field cai_mcp_prompts_list_response_fields[] = {
    LONEJSON_FIELD_OBJECT(cai_mcp_prompts_list_response_doc, result, "result",
                          &cai_mcp_prompts_list_result_map),
    LONEJSON_FIELD_OBJECT(cai_mcp_prompts_list_response_doc, error_doc, "error",
                          &cai_mcp_jsonrpc_error_map)};
LONEJSON_MAP_DEFINE(cai_mcp_prompts_list_response_map,
                    cai_mcp_prompts_list_response_doc,
                    cai_mcp_prompts_list_response_fields);

static const lonejson_field cai_mcp_initialize_result_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_initialize_result_doc, protocol_version,
                                "protocolVersion")};
LONEJSON_MAP_DEFINE(cai_mcp_initialize_result_map,
                    cai_mcp_initialize_result_doc,
                    cai_mcp_initialize_result_fields);

static const lonejson_field cai_mcp_initialize_response_fields[] = {
    LONEJSON_FIELD_OBJECT(cai_mcp_initialize_response_doc, result, "result",
                          &cai_mcp_initialize_result_map),
    LONEJSON_FIELD_OBJECT(cai_mcp_initialize_response_doc, error_doc, "error",
                          &cai_mcp_jsonrpc_error_map)};
LONEJSON_MAP_DEFINE(cai_mcp_initialize_response_map,
                    cai_mcp_initialize_response_doc,
                    cai_mcp_initialize_response_fields);

static cai_mcp_streamable_http_client_impl *
cai_mcp_streamable_impl(cai_mcp_client *client) {
  return client != NULL ? (cai_mcp_streamable_http_client_impl *)client->impl
                        : NULL;
}

static const cai_mcp_streamable_http_client_impl *
cai_mcp_streamable_const_impl(const cai_mcp_client *client) {
  return client != NULL
             ? (const cai_mcp_streamable_http_client_impl *)client->impl
             : NULL;
}

static int cai_mcp_ascii_ieq_n(const char *a, const char *b, size_t n) {
  size_t i;

  if (a == NULL || b == NULL) {
    return 0;
  }
  for (i = 0U; i < n; i++) {
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
      return 0;
    }
  }
  return 1;
}

static int cai_mcp_header_is(const char *line, const char *name,
                             size_t name_len) {
  return strlen(line) > name_len && line[name_len] == ':' &&
         cai_mcp_ascii_ieq_n(line, name, name_len);
}

static char *cai_mcp_trim_header_value(const cai_allocator *allocator,
                                       const char *value, size_t len) {
  while (len > 0U && (*value == ' ' || *value == '\t')) {
    value++;
    len--;
  }
  while (len > 0U && (value[len - 1U] == '\r' || value[len - 1U] == '\n' ||
                      value[len - 1U] == ' ' || value[len - 1U] == '\t')) {
    len--;
  }
  return cai_strndup(allocator, value, len);
}

static size_t cai_mcp_header_callback(char *buffer, size_t size, size_t nitems,
                                      void *userdata) {
  cai_mcp_http_response_capture *capture;
  size_t len;

  capture = (cai_mcp_http_response_capture *)userdata;
  len = size * nitems;
  if (len == 0U || capture == NULL) {
    return len;
  }
  if (cai_mcp_header_is(buffer, "content-type", strlen("content-type"))) {
    cai_free_mem(NULL, capture->content_type);
    capture->content_type =
        cai_mcp_trim_header_value(NULL, buffer + strlen("content-type") + 1U,
                                  len - strlen("content-type") - 1U);
  } else if (cai_mcp_header_is(buffer, "mcp-session-id",
                               strlen("mcp-session-id"))) {
    cai_free_mem(NULL, capture->session_id);
    capture->session_id =
        cai_mcp_trim_header_value(NULL, buffer + strlen("mcp-session-id") + 1U,
                                  len - strlen("mcp-session-id") - 1U);
  }
  return len;
}

static size_t cai_mcp_response_write(char *ptr, size_t size, size_t nmemb,
                                     void *userdata) {
  cai_mcp_http_response_capture *capture;
  lonejson_error json_error;
  size_t len;

  capture = (cai_mcp_http_response_capture *)userdata;
  len = size * nmemb;
  if (len == 0U) {
    return 0U;
  }
  lonejson_error_init(&json_error);
  if (capture == NULL ||
      capture->body.append(&capture->body, ptr, len, &json_error) !=
          LONEJSON_STATUS_OK) {
    return 0U;
  }
  return len;
}

static size_t cai_mcp_upload_read(char *ptr, size_t size, size_t nmemb,
                                  void *userdata) {
  cai_mcp_spooled_upload *upload;
  lonejson_error json_error;
  lonejson_read_result result;
  size_t capacity;

  upload = (cai_mcp_spooled_upload *)userdata;
  capacity = size * nmemb;
  if (upload == NULL || capacity == 0U) {
    return 0U;
  }
  if (!upload->rewound) {
    lonejson_error_init(&json_error);
    if (upload->cursor.rewind(&upload->cursor, &json_error) !=
        LONEJSON_STATUS_OK) {
      return CURL_READFUNC_ABORT;
    }
    upload->rewound = 1;
  }
  result = upload->cursor.read(&upload->cursor, (unsigned char *)ptr, capacity);
  if (result.error_code != 0) {
    return CURL_READFUNC_ABORT;
  }
  return result.bytes_read;
}

static lonejson_read_result
cai_mcp_spooled_read(void *user, unsigned char *buffer, size_t capacity) {
  cai_mcp_spooled_reader *reader;

  reader = (cai_mcp_spooled_reader *)user;
  return reader->cursor.read(&reader->cursor, buffer, capacity);
}

static lonejson_status cai_mcp_spool_sink(void *user, const void *data,
                                          size_t len,
                                          lonejson_error *json_error) {
  lonejson_spooled *spool;

  spool = (lonejson_spooled *)user;
  return spool->append(spool, data, len, json_error);
}

static lonejson_status cai_mcp_cai_sink_bridge(void *user, const void *data,
                                               size_t len,
                                               lonejson_error *json_error) {
  cai_sink *sink;
  cai_error error;

  sink = (cai_sink *)user;
  cai_error_init(&error);
  if (sink != NULL && cai_sink_write(sink, data, len, &error) == CAI_OK) {
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

static lonejson_status cai_mcp_discard_sink(void *user, const void *data,
                                            size_t len,
                                            lonejson_error *json_error) {
  (void)user;
  (void)data;
  (void)len;
  (void)json_error;
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_mcp_buffer_sink(void *user, const void *data,
                                           size_t len,
                                           lonejson_error *json_error) {
  cai_buffer_builder *builder;
  cai_error error;

  (void)json_error;
  builder = (cai_buffer_builder *)user;
  if (builder == NULL || data == NULL) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  cai_error_init(&error);
  if (cai_buffer_append(builder, (const char *)data, len, &error) != CAI_OK) {
    cai_error_cleanup(&error);
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  cai_error_cleanup(&error);
  return LONEJSON_STATUS_OK;
}

static int cai_mcp_set_json_error(cai_error *error, const char *message,
                                  const lonejson_error *json_error) {
  return cai_set_error_detail(error, CAI_ERR_PROTOCOL, message,
                              json_error != NULL ? json_error->message : NULL);
}

static int cai_mcp_set_rpc_error(cai_error *error,
                                 const cai_mcp_jsonrpc_error_doc *rpc_error) {
  char detail[80];

  if (rpc_error == NULL || rpc_error->message == NULL) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP server returned an error");
  }
  snprintf(detail, sizeof(detail), "JSON-RPC error code %lld",
           (long long)rpc_error->code);
  return cai_set_error_detail(error, CAI_ERR_SERVER, rpc_error->message,
                              detail);
}

static int cai_mcp_response_is_json(const cai_mcp_http_response_capture *res) {
  return res != NULL && res->content_type != NULL &&
         strstr(res->content_type, "application/json") != NULL;
}

static int cai_mcp_response_is_sse(const cai_mcp_http_response_capture *res) {
  return res != NULL && res->content_type != NULL &&
         strstr(res->content_type, "text/event-stream") != NULL;
}

static int cai_mcp_response_ok(const cai_mcp_http_response_capture *res,
                               cai_error *error) {
  if (res == NULL) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "MCP HTTP response is missing");
  }
  if (res->status < 200L || res->status >= 300L) {
    return cai_set_error_http(error, CAI_ERR_SERVER, res->status,
                              "MCP server returned HTTP error", NULL, NULL,
                              NULL);
  }
  return CAI_OK;
}

static int cai_mcp_post(cai_mcp_streamable_http_client_impl *impl,
                        const lonejson_spooled *request, size_t request_len,
                        int is_request, cai_mcp_http_response_capture *response,
                        cai_error *error) {
  CURL *curl;
  CURLcode curl_rc;
  struct curl_slist *headers;
  char protocol_header[128];
  char session_header[192];
  cai_mcp_spooled_upload upload;
  int rc;

  if (impl == NULL || request == NULL || response == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client request is required");
  }
  headers = NULL;
  curl = curl_easy_init();
  if (curl == NULL) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to initialize MCP HTTP request");
  }
  rc = cai_append_header(&headers, "Content-Type: application/json", error);
  if (rc == CAI_OK) {
    rc = cai_append_header(
        &headers, "Accept: application/json, text/event-stream", error);
  }
  if (rc == CAI_OK && impl->initialized && impl->session_id != NULL) {
    snprintf(session_header, sizeof(session_header), "MCP-Session-Id: %s",
             impl->session_id);
    rc = cai_append_header(&headers, session_header, error);
  }
  if (rc == CAI_OK && impl->initialized && impl->protocol_version != NULL) {
    snprintf(protocol_header, sizeof(protocol_header),
             "MCP-Protocol-Version: %s", impl->protocol_version);
    rc = cai_append_header(&headers, protocol_header, error);
  }
  if (rc != CAI_OK) {
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    return rc;
  }

  response->content_type = NULL;
  response->session_id = NULL;
  response->status = 0L;
  CAI_LJ->spooled_init(CAI_LJ, &response->body);

  upload.cursor = *request;
  upload.rewound = 0;
  curl_easy_setopt(curl, CURLOPT_URL, impl->url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_READFUNCTION, cai_mcp_upload_read);
  curl_easy_setopt(curl, CURLOPT_READDATA, &upload);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)request_len);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cai_mcp_response_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, cai_mcp_header_callback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, response);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, impl->timeout_ms);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, impl->timeout_ms);
  cai_configure_curl_tls(curl, impl->insecure_skip_verify, impl->ca_bundle_path,
                         impl->ca_path);
  curl_rc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response->status);
  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);
  if (curl_rc != CURLE_OK) {
    response->body.cleanup(&response->body);
    cai_free_mem(NULL, response->content_type);
    cai_free_mem(NULL, response->session_id);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "MCP HTTP request failed",
                                curl_easy_strerror(curl_rc));
  }
  (void)is_request;
  return cai_mcp_response_ok(response, error);
}

static void
cai_mcp_http_response_capture_cleanup(cai_mcp_http_response_capture *response) {
  if (response != NULL) {
    if (response->body.cleanup != NULL) {
      response->body.cleanup(&response->body);
    }
    cai_free_mem(NULL, response->content_type);
    cai_free_mem(NULL, response->session_id);
    memset(response, 0, sizeof(*response));
  }
}

static void
cai_mcp_streamable_reset_session(cai_mcp_streamable_http_client_impl *impl) {
  if (impl != NULL) {
    cai_free_mem(&impl->allocator, impl->session_id);
    impl->session_id = NULL;
    impl->initialized = 0;
  }
}

static void cai_mcp_clear_error(cai_error *error) {
  if (error != NULL) {
    cai_error_cleanup(error);
    cai_error_init(error);
  }
}

static void cai_mcp_spooled_cleanup_if_initialized(lonejson_spooled *spool) {
  if (spool != NULL && spool->cleanup != NULL) {
    spool->cleanup(spool);
    memset(spool, 0, sizeof(*spool));
  }
}

static int cai_mcp_write_cstr(lonejson_spooled *spool, const char *text,
                              cai_error *error) {
  lonejson_error json_error;

  lonejson_error_init(&json_error);
  if (spool->append(spool, text, strlen(text), &json_error) !=
      LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to build MCP request",
                                json_error.message);
  }
  return CAI_OK;
}

static int cai_mcp_spool_copy(const lonejson_spooled *src,
                              lonejson_spooled *dst, cai_error *error) {
  lonejson_spooled cursor;
  lonejson_error json_error;
  unsigned char buffer[4096];
  lonejson_read_result chunk;

  CAI_LJ->spooled_init(CAI_LJ, dst);
  cursor = *src;
  lonejson_error_init(&json_error);
  if (cursor.rewind(&cursor, &json_error) != LONEJSON_STATUS_OK) {
    dst->cleanup(dst);
    return cai_mcp_set_json_error(error, "failed to rewind MCP response",
                                  &json_error);
  }
  for (;;) {
    chunk = cursor.read(&cursor, buffer, sizeof(buffer));
    if (chunk.error_code != 0) {
      dst->cleanup(dst);
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "failed to read MCP response body");
    }
    if (chunk.bytes_read != 0U &&
        dst->append(dst, buffer, chunk.bytes_read, &json_error) !=
            LONEJSON_STATUS_OK) {
      dst->cleanup(dst);
      return cai_mcp_set_json_error(error, "failed to copy MCP response",
                                    &json_error);
    }
    if (chunk.eof) {
      break;
    }
  }
  return CAI_OK;
}

static int cai_mcp_sse_line_is_blank(const char *line, size_t len) {
  size_t i;

  for (i = 0U; i < len; i++) {
    if (line[i] != '\r' && line[i] != ' ' && line[i] != '\t') {
      return 0;
    }
  }
  return 1;
}

static void cai_mcp_sse_set_event(char *event, size_t event_size,
                                  const char *value, size_t len) {
  while (len > 0U && (*value == ' ' || *value == '\t')) {
    value++;
    len--;
  }
  while (len > 0U && (value[len - 1U] == '\r' || value[len - 1U] == ' ' ||
                      value[len - 1U] == '\t')) {
    len--;
  }
  if (event_size == 0U) {
    return;
  }
  if (len >= event_size) {
    len = event_size - 1U;
  }
  memcpy(event, value, len);
  event[len] = '\0';
}

static int cai_mcp_sse_data_event_done(const char *event,
                                       const lonejson_spooled *data) {
  return data->size_fn(data) != 0U &&
         (event[0] == '\0' || strcmp(event, "message") == 0);
}

static int cai_mcp_sse_handle_line(lonejson_spooled *data, char *event,
                                   size_t event_size, const char *line,
                                   size_t len, int *done, cai_error *error) {
  lonejson_error json_error;
  const char *value;

  if (cai_mcp_sse_line_is_blank(line, len)) {
    if (cai_mcp_sse_data_event_done(event, data)) {
      *done = 1;
    } else {
      data->reset(data);
      event[0] = '\0';
    }
    return CAI_OK;
  }
  if (len >= 5U && memcmp(line, "data:", 5U) == 0) {
    value = line + 5U;
    len -= 5U;
    if (len > 0U && *value == ' ') {
      value++;
      len--;
    }
    while (len > 0U && value[len - 1U] == '\r') {
      len--;
    }
    if (len == 0U) {
      return CAI_OK;
    }
    if (data->size_fn(data) != 0U) {
      lonejson_error_init(&json_error);
      if (data->append(data, "\n", 1U, &json_error) != LONEJSON_STATUS_OK) {
        return cai_mcp_set_json_error(error, "failed to parse MCP SSE data",
                                      &json_error);
      }
    }
    lonejson_error_init(&json_error);
    if (data->append(data, value, len, &json_error) != LONEJSON_STATUS_OK) {
      return cai_mcp_set_json_error(error, "failed to parse MCP SSE data",
                                    &json_error);
    }
  } else if (len >= 6U && memcmp(line, "event:", 6U) == 0) {
    cai_mcp_sse_set_event(event, event_size, line + 6U, len - 6U);
  }
  return CAI_OK;
}

static int
cai_mcp_sse_response_json_body(const cai_mcp_http_response_capture *response,
                               lonejson_spooled *out, cai_error *error) {
  lonejson_spooled cursor;
  lonejson_error json_error;
  lonejson_read_result chunk;
  cai_buffer_builder line;
  unsigned char buffer[4096];
  char event[64];
  size_t i;
  int done;
  int rc;

  CAI_LJ->spooled_init(CAI_LJ, out);
  cursor = response->body;
  memset(&line, 0, sizeof(line));
  event[0] = '\0';
  done = 0;
  rc = CAI_OK;
  lonejson_error_init(&json_error);
  if (cursor.rewind(&cursor, &json_error) != LONEJSON_STATUS_OK) {
    out->cleanup(out);
    return cai_mcp_set_json_error(error, "failed to rewind MCP SSE response",
                                  &json_error);
  }
  while (!done) {
    chunk = cursor.read(&cursor, buffer, sizeof(buffer));
    if (chunk.error_code != 0) {
      rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to read MCP SSE response");
      break;
    }
    for (i = 0U; i < chunk.bytes_read && !done; i++) {
      if (buffer[i] == '\n') {
        rc = cai_mcp_sse_handle_line(out, event, sizeof(event), line.data,
                                     line.length, &done, error);
        line.length = 0U;
        if (line.data != NULL) {
          line.data[0] = '\0';
        }
      } else {
        rc = cai_buffer_append(&line, (const char *)&buffer[i], 1U, error);
      }
      if (rc != CAI_OK) {
        break;
      }
    }
    if (rc != CAI_OK || chunk.eof) {
      break;
    }
  }
  if (rc == CAI_OK && !done && line.length != 0U) {
    rc = cai_mcp_sse_handle_line(out, event, sizeof(event), line.data,
                                 line.length, &done, error);
  }
  cai_free_mem(NULL, line.data);
  if (rc == CAI_OK && !done && !cai_mcp_sse_data_event_done(event, out)) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "MCP SSE response did not include JSON-RPC message");
  }
  if (rc != CAI_OK) {
    out->cleanup(out);
  }
  return rc;
}

static int
cai_mcp_response_json_body(const cai_mcp_http_response_capture *response,
                           const char *operation, lonejson_spooled *out,
                           cai_error *error) {
  if (cai_mcp_response_is_json(response)) {
    return cai_mcp_spool_copy(&response->body, out, error);
  }
  if (cai_mcp_response_is_sse(response)) {
    return cai_mcp_sse_response_json_body(response, out, error);
  }
  return cai_set_error_detail(error, CAI_ERR_PROTOCOL, operation,
                              "MCP response was neither JSON nor SSE");
}

static int cai_mcp_write_json_string(lonejson_spooled *spool, const char *text,
                                     cai_error *error) {
  lonejson_error json_error;
  lonejson_writer writer;
  lonejson_status status;

  lonejson_error_init(&json_error);
  status = CAI_LJ->writer_init_sink(CAI_LJ, &writer, cai_mcp_spool_sink, spool,
                                    &json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = writer.string(&writer, text != NULL ? text : "",
                           text != NULL ? strlen(text) : 0U, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.finish(&writer, &json_error);
  }
  if (writer.cleanup != NULL) {
    writer.cleanup(&writer);
  }
  if (status != LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to write MCP JSON string",
                                json_error.message);
  }
  return CAI_OK;
}

static int cai_mcp_request_begin(cai_mcp_streamable_http_client_impl *impl,
                                 lonejson_spooled *spool, long long id,
                                 const char *method, cai_error *error) {
  char id_buf[64];
  int rc;

  snprintf(id_buf, sizeof(id_buf), "%lld", id);
  rc = cai_mcp_write_cstr(spool, "{\"jsonrpc\":\"2.0\",\"id\":", error);
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, id_buf, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, ",\"method\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(spool, method, error);
  }
  (void)impl;
  return rc;
}

static int cai_mcp_initialize_request(cai_mcp_streamable_http_client_impl *impl,
                                      lonejson_spooled *spool, size_t *out_len,
                                      cai_error *error) {
  int rc;

  CAI_LJ->spooled_init(CAI_LJ, spool);
  rc = cai_mcp_request_begin(impl, spool, ++impl->next_id, "initialize", error);
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, ",\"params\":{\"protocolVersion\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(spool, impl->protocol_version, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(
        spool, ",\"capabilities\":{},\"clientInfo\":{\"name\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(spool, impl->client_name, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, ",\"version\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(spool, impl->client_version, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, "}}}", error);
  }
  if (out_len != NULL) {
    *out_len = spool->size_fn(spool);
  }
  if (rc != CAI_OK) {
    spool->cleanup(spool);
  }
  return rc;
}

static int cai_mcp_list_request(cai_mcp_streamable_http_client_impl *impl,
                                const char *method, const char *cursor,
                                lonejson_spooled *spool, size_t *out_len,
                                cai_error *error) {
  int rc;

  CAI_LJ->spooled_init(CAI_LJ, spool);
  rc = cai_mcp_request_begin(impl, spool, ++impl->next_id, method, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, ",\"params\":{", error);
  }
  if (rc == CAI_OK && cursor != NULL && cursor[0] != '\0') {
    rc = cai_mcp_write_cstr(spool, "\"cursor\":", error);
    if (rc == CAI_OK) {
      rc = cai_mcp_write_json_string(spool, cursor, error);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, "}}", error);
  }
  if (out_len != NULL) {
    *out_len = spool->size_fn(spool);
  }
  if (rc != CAI_OK) {
    spool->cleanup(spool);
  }
  return rc;
}

static int cai_mcp_initialized_notification(lonejson_spooled *spool,
                                            size_t *out_len, cai_error *error) {
  int rc;

  CAI_LJ->spooled_init(CAI_LJ, spool);
  rc = cai_mcp_write_cstr(
      spool, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}",
      error);
  if (out_len != NULL) {
    *out_len = spool->size_fn(spool);
  }
  if (rc != CAI_OK) {
    spool->cleanup(spool);
  }
  return rc;
}

static int cai_mcp_ping_request(cai_mcp_streamable_http_client_impl *impl,
                                lonejson_spooled *spool, size_t *out_len,
                                cai_error *error) {
  int rc;

  CAI_LJ->spooled_init(CAI_LJ, spool);
  rc = cai_mcp_request_begin(impl, spool, ++impl->next_id, "ping", error);
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, "}", error);
  }
  if (out_len != NULL) {
    *out_len = spool->size_fn(spool);
  }
  if (rc != CAI_OK) {
    spool->cleanup(spool);
  }
  return rc;
}

static int
cai_mcp_resource_read_request(cai_mcp_streamable_http_client_impl *impl,
                              const char *uri, lonejson_spooled *spool,
                              size_t *out_len, cai_error *error) {
  int rc;

  if (uri == NULL || uri[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP resource URI is required");
  }
  CAI_LJ->spooled_init(CAI_LJ, spool);
  rc = cai_mcp_request_begin(impl, spool, ++impl->next_id, "resources/read",
                             error);
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, ",\"params\":{\"uri\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(spool, uri, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, "}}", error);
  }
  if (out_len != NULL) {
    *out_len = spool->size_fn(spool);
  }
  if (rc != CAI_OK) {
    spool->cleanup(spool);
  }
  return rc;
}

static int cai_mcp_prompt_get_request(cai_mcp_streamable_http_client_impl *impl,
                                      const char *name,
                                      lonejson_spooled *arguments_json,
                                      lonejson_spooled *spool, size_t *out_len,
                                      cai_error *error) {
  lonejson_error json_error;
  lonejson_writer writer;
  lonejson_status status;
  int rc;

  if (name == NULL || name[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP prompt name is required");
  }
  CAI_LJ->spooled_init(CAI_LJ, spool);
  rc =
      cai_mcp_request_begin(impl, spool, ++impl->next_id, "prompts/get", error);
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, ",\"params\":{\"name\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(spool, name, error);
  }
  if (rc == CAI_OK && arguments_json != NULL) {
    rc = cai_mcp_write_cstr(spool, ",\"arguments\":", error);
    if (rc == CAI_OK) {
      lonejson_error_init(&json_error);
      status = CAI_LJ->writer_init_sink(CAI_LJ, &writer, cai_mcp_spool_sink,
                                        spool, &json_error);
      if (status == LONEJSON_STATUS_OK) {
        status =
            writer.json_value_spooled(&writer, arguments_json, &json_error);
      }
      if (writer.cleanup != NULL) {
        writer.cleanup(&writer);
      }
      if (status != LONEJSON_STATUS_OK) {
        rc = cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                  "failed to write MCP prompt arguments",
                                  json_error.message);
      }
    }
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, "}}", error);
  }
  if (out_len != NULL) {
    *out_len = spool->size_fn(spool);
  }
  if (rc != CAI_OK) {
    spool->cleanup(spool);
  }
  return rc;
}

static int cai_mcp_completion_request(
    cai_mcp_streamable_http_client_impl *impl, const char *ref_type,
    const char *ref_value, const char *argument_name,
    const char *argument_value, lonejson_spooled *context_arguments_json,
    lonejson_spooled *spool, size_t *out_len, cai_error *error) {
  lonejson_error json_error;
  lonejson_writer writer;
  lonejson_status status;
  int rc;

  if (ref_type == NULL || ref_type[0] == '\0' || ref_value == NULL ||
      ref_value[0] == '\0' || argument_name == NULL ||
      argument_name[0] == '\0') {
    return cai_set_error(
        error, CAI_ERR_INVALID,
        "MCP completion reference and argument name are required");
  }
  CAI_LJ->spooled_init(CAI_LJ, spool);
  rc = cai_mcp_request_begin(impl, spool, ++impl->next_id,
                             "completion/complete", error);
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, ",\"params\":{\"ref\":{\"type\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(spool, ref_type, error);
  }
  if (rc == CAI_OK) {
    rc = strcmp(ref_type, "ref/resource") == 0
             ? cai_mcp_write_cstr(spool, ",\"uri\":", error)
             : cai_mcp_write_cstr(spool, ",\"name\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(spool, ref_value, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, "},\"argument\":{\"name\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(spool, argument_name, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, ",\"value\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(
        spool, argument_value != NULL ? argument_value : "", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, "}", error);
  }
  if (rc == CAI_OK && context_arguments_json != NULL) {
    rc = cai_mcp_write_cstr(spool, ",\"context\":{\"arguments\":", error);
    if (rc == CAI_OK) {
      lonejson_error_init(&json_error);
      status = CAI_LJ->writer_init_sink(CAI_LJ, &writer, cai_mcp_spool_sink,
                                        spool, &json_error);
      if (status == LONEJSON_STATUS_OK) {
        status = writer.json_value_spooled(&writer, context_arguments_json,
                                           &json_error);
      }
      if (writer.cleanup != NULL) {
        writer.cleanup(&writer);
      }
      if (status != LONEJSON_STATUS_OK) {
        rc = cai_set_error_detail(
            error, CAI_ERR_PROTOCOL,
            "failed to write MCP completion context arguments",
            json_error.message);
      }
    }
    if (rc == CAI_OK) {
      rc = cai_mcp_write_cstr(spool, "}", error);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, "}}", error);
  }
  if (out_len != NULL) {
    *out_len = spool->size_fn(spool);
  }
  if (rc != CAI_OK) {
    spool->cleanup(spool);
  }
  return rc;
}

static int cai_mcp_call_request(cai_mcp_streamable_http_client_impl *impl,
                                const char *name,
                                lonejson_spooled *arguments_json,
                                lonejson_spooled *spool, size_t *out_len,
                                cai_error *error) {
  lonejson_error json_error;
  lonejson_writer writer;
  lonejson_status status;
  int rc;

  if (name == NULL || name[0] == '\0' || arguments_json == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP tool name and arguments are required");
  }
  CAI_LJ->spooled_init(CAI_LJ, spool);
  rc = cai_mcp_request_begin(impl, spool, ++impl->next_id, "tools/call", error);
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, ",\"params\":{\"name\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(spool, name, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, ",\"arguments\":", error);
  }
  if (rc == CAI_OK) {
    lonejson_error_init(&json_error);
    status = CAI_LJ->writer_init_sink(CAI_LJ, &writer, cai_mcp_spool_sink,
                                      spool, &json_error);
    if (status == LONEJSON_STATUS_OK) {
      status = writer.json_value_spooled(&writer, arguments_json, &json_error);
    }
    if (writer.cleanup != NULL) {
      writer.cleanup(&writer);
    }
    if (status != LONEJSON_STATUS_OK) {
      rc = cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to write MCP tool arguments",
                                json_error.message);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, "}}", error);
  }
  if (out_len != NULL) {
    *out_len = spool->size_fn(spool);
  }
  if (rc != CAI_OK) {
    spool->cleanup(spool);
  }
  return rc;
}

static int
cai_mcp_parse_initialize_response(cai_mcp_streamable_http_client_impl *impl,
                                  const cai_mcp_http_response_capture *response,
                                  cai_error *error) {
  cai_mcp_initialize_response_doc doc;
  lonejson_spooled json_body;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  int rc;

  rc = cai_mcp_response_json_body(response, "MCP initialize response",
                                  &json_body, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&doc, 0, sizeof(doc));
  reader.cursor = json_body;
  lonejson_error_init(&json_error);
  if (reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to rewind MCP response",
                                  &json_error);
  }
  status = CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_initialize_response_map, &doc,
                                cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_initialize_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to parse MCP initialize",
                                  &json_error);
  }
  if (doc.error_doc.message != NULL) {
    rc = cai_mcp_set_rpc_error(error, &doc.error_doc);
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_initialize_response_map, &doc);
    json_body.cleanup(&json_body);
    return rc;
  }
  if (doc.result.protocol_version == NULL ||
      strcmp(doc.result.protocol_version, impl->protocol_version) != 0) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_initialize_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP server negotiated unsupported protocol version");
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_initialize_response_map, &doc);
  json_body.cleanup(&json_body);
  return CAI_OK;
}

static void cai_mcp_client_tool_impl_cleanup(const cai_allocator *allocator,
                                             cai_mcp_client_tool_impl *tool) {
  if (tool != NULL) {
    cai_free_mem(allocator, tool->name);
    cai_free_mem(allocator, tool->description);
    cai_free_mem(allocator, tool->input_schema_json);
    memset(tool, 0, sizeof(*tool));
  }
}

static void
cai_mcp_client_resource_impl_cleanup(const cai_allocator *allocator,
                                     cai_mcp_client_resource_impl *resource) {
  if (resource != NULL) {
    cai_free_mem(allocator, resource->uri);
    cai_free_mem(allocator, resource->name);
    cai_free_mem(allocator, resource->title);
    cai_free_mem(allocator, resource->description);
    cai_free_mem(allocator, resource->mime_type);
    memset(resource, 0, sizeof(*resource));
  }
}

static void cai_mcp_client_resource_template_impl_cleanup(
    const cai_allocator *allocator,
    cai_mcp_client_resource_template_impl *resource_template) {
  if (resource_template != NULL) {
    cai_free_mem(allocator, resource_template->uri_template);
    cai_free_mem(allocator, resource_template->name);
    cai_free_mem(allocator, resource_template->title);
    cai_free_mem(allocator, resource_template->description);
    cai_free_mem(allocator, resource_template->mime_type);
    memset(resource_template, 0, sizeof(*resource_template));
  }
}

static void
cai_mcp_client_prompt_impl_cleanup(const cai_allocator *allocator,
                                   cai_mcp_client_prompt_impl *prompt) {
  if (prompt != NULL) {
    cai_free_mem(allocator, prompt->name);
    cai_free_mem(allocator, prompt->title);
    cai_free_mem(allocator, prompt->description);
    cai_free_mem(allocator, prompt->arguments_json);
    memset(prompt, 0, sizeof(*prompt));
  }
}

static void
cai_mcp_client_clear_tools(cai_mcp_streamable_http_client_impl *impl) {
  size_t i;

  if (impl == NULL) {
    return;
  }
  for (i = 0U; i < impl->tool_count; i++) {
    cai_mcp_client_tool_impl_cleanup(&impl->allocator, &impl->tools[i]);
  }
  impl->tool_count = 0U;
}

static void
cai_mcp_client_clear_resources(cai_mcp_streamable_http_client_impl *impl) {
  size_t i;

  if (impl == NULL) {
    return;
  }
  for (i = 0U; i < impl->resource_count; i++) {
    cai_mcp_client_resource_impl_cleanup(&impl->allocator, &impl->resources[i]);
  }
  impl->resource_count = 0U;
}

static void cai_mcp_client_clear_resource_templates(
    cai_mcp_streamable_http_client_impl *impl) {
  size_t i;

  if (impl == NULL) {
    return;
  }
  for (i = 0U; i < impl->resource_template_count; i++) {
    cai_mcp_client_resource_template_impl_cleanup(&impl->allocator,
                                                  &impl->resource_templates[i]);
  }
  impl->resource_template_count = 0U;
}

static void
cai_mcp_client_clear_prompts(cai_mcp_streamable_http_client_impl *impl) {
  size_t i;

  if (impl == NULL) {
    return;
  }
  for (i = 0U; i < impl->prompt_count; i++) {
    cai_mcp_client_prompt_impl_cleanup(&impl->allocator, &impl->prompts[i]);
  }
  impl->prompt_count = 0U;
}

static int
cai_mcp_client_reserve_tools(cai_mcp_streamable_http_client_impl *impl,
                             size_t count, cai_error *error) {
  cai_mcp_client_tool_impl *tools;

  if (count <= impl->tool_capacity) {
    return CAI_OK;
  }
  tools = (cai_mcp_client_tool_impl *)cai_realloc_mem(
      &impl->allocator, impl->tools, count * sizeof(*tools));
  if (tools == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate MCP tool cache");
  }
  memset(tools + impl->tool_capacity, 0,
         (count - impl->tool_capacity) * sizeof(*tools));
  impl->tools = tools;
  impl->tool_capacity = count;
  return CAI_OK;
}

static int
cai_mcp_client_reserve_resources(cai_mcp_streamable_http_client_impl *impl,
                                 size_t count, cai_error *error) {
  cai_mcp_client_resource_impl *resources;

  if (count <= impl->resource_capacity) {
    return CAI_OK;
  }
  resources = (cai_mcp_client_resource_impl *)cai_realloc_mem(
      &impl->allocator, impl->resources, count * sizeof(*resources));
  if (resources == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate MCP resource cache");
  }
  memset(resources + impl->resource_capacity, 0,
         (count - impl->resource_capacity) * sizeof(*resources));
  impl->resources = resources;
  impl->resource_capacity = count;
  return CAI_OK;
}

static int cai_mcp_client_reserve_resource_templates(
    cai_mcp_streamable_http_client_impl *impl, size_t count, cai_error *error) {
  cai_mcp_client_resource_template_impl *resource_templates;

  if (count <= impl->resource_template_capacity) {
    return CAI_OK;
  }
  resource_templates = (cai_mcp_client_resource_template_impl *)cai_realloc_mem(
      &impl->allocator, impl->resource_templates,
      count * sizeof(*resource_templates));
  if (resource_templates == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate MCP resource template cache");
  }
  memset(resource_templates + impl->resource_template_capacity, 0,
         (count - impl->resource_template_capacity) *
             sizeof(*resource_templates));
  impl->resource_templates = resource_templates;
  impl->resource_template_capacity = count;
  return CAI_OK;
}

static int
cai_mcp_client_reserve_prompts(cai_mcp_streamable_http_client_impl *impl,
                               size_t count, cai_error *error) {
  cai_mcp_client_prompt_impl *prompts;

  if (count <= impl->prompt_capacity) {
    return CAI_OK;
  }
  prompts = (cai_mcp_client_prompt_impl *)cai_realloc_mem(
      &impl->allocator, impl->prompts, count * sizeof(*prompts));
  if (prompts == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate MCP prompt cache");
  }
  memset(prompts + impl->prompt_capacity, 0,
         (count - impl->prompt_capacity) * sizeof(*prompts));
  impl->prompts = prompts;
  impl->prompt_capacity = count;
  return CAI_OK;
}

static char *cai_mcp_json_value_to_cstr(const lonejson_json_value *value,
                                        cai_error *error) {
  cai_buffer_builder builder;
  lonejson_error json_error;

  memset(&builder, 0, sizeof(builder));
  lonejson_error_init(&json_error);
  if (value == NULL ||
      value->methods->write_to_sink(value, cai_mcp_buffer_sink, &builder,
                                    &json_error) != LONEJSON_STATUS_OK) {
    cai_free_mem(NULL, builder.data);
    return NULL;
  }
  if (cai_buffer_append(&builder, "", 1U, error) != CAI_OK) {
    cai_free_mem(NULL, builder.data);
    return NULL;
  }
  return builder.data;
}

static int
cai_mcp_parse_tools_list_response(cai_mcp_streamable_http_client_impl *impl,
                                  const cai_mcp_http_response_capture *response,
                                  char **next_cursor, cai_error *error) {
  cai_mcp_tools_list_response_doc doc;
  lonejson_spooled json_body;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  cai_mcp_list_tool_doc *src_tools;
  size_t base_count;
  size_t i;
  int rc;

  if (next_cursor != NULL) {
    *next_cursor = NULL;
  }
  rc = cai_mcp_response_json_body(response, "MCP tools/list response",
                                  &json_body, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&doc, 0, sizeof(doc));
  reader.cursor = json_body;
  lonejson_error_init(&json_error);
  if (reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to rewind MCP response",
                                  &json_error);
  }
  status = CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_tools_list_response_map, &doc,
                                cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tools_list_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to parse MCP tools/list",
                                  &json_error);
  }
  if (doc.error_doc.message != NULL) {
    rc = cai_mcp_set_rpc_error(error, &doc.error_doc);
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tools_list_response_map, &doc);
    json_body.cleanup(&json_body);
    return rc;
  }
  base_count = impl->tool_count;
  rc = cai_mcp_client_reserve_tools(impl, base_count + doc.result.tools.count,
                                    error);
  if (rc == CAI_OK) {
    src_tools = (cai_mcp_list_tool_doc *)doc.result.tools.items;
    for (i = 0U; i < doc.result.tools.count; i++) {
      cai_mcp_client_tool_impl *dst = &impl->tools[base_count + i];
      dst->name = cai_strdup(&impl->allocator, src_tools[i].name);
      dst->description = cai_strdup(
          &impl->allocator,
          src_tools[i].description != NULL
              ? src_tools[i].description
              : (src_tools[i].title != NULL ? src_tools[i].title : ""));
      dst->input_schema_json =
          cai_mcp_json_value_to_cstr(&src_tools[i].input_schema, error);
      if (dst->name == NULL || dst->description == NULL ||
          dst->input_schema_json == NULL) {
        rc = error != NULL && error->code != CAI_OK
                 ? error->code
                 : cai_set_error(error, CAI_ERR_NOMEM,
                                 "failed to copy MCP tool metadata");
        cai_mcp_client_tool_impl_cleanup(&impl->allocator, dst);
        break;
      }
      dst->public_tool.name = dst->name;
      dst->public_tool.description = dst->description;
      dst->public_tool.input_schema_json = dst->input_schema_json;
      impl->tool_count++;
    }
  }
  if (rc == CAI_OK && next_cursor != NULL && doc.result.next_cursor != NULL) {
    *next_cursor = cai_strdup(&impl->allocator, doc.result.next_cursor);
    if (*next_cursor == NULL) {
      rc = cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to copy MCP tools/list cursor");
    }
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tools_list_response_map, &doc);
  if (rc != CAI_OK) {
    cai_mcp_client_clear_tools(impl);
    if (next_cursor != NULL) {
      cai_free_mem(&impl->allocator, *next_cursor);
      *next_cursor = NULL;
    }
  }
  json_body.cleanup(&json_body);
  return rc;
}

static int cai_mcp_parse_resources_list_response(
    cai_mcp_streamable_http_client_impl *impl,
    const cai_mcp_http_response_capture *response, char **next_cursor,
    cai_error *error) {
  cai_mcp_resources_list_response_doc doc;
  lonejson_spooled json_body;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  cai_mcp_list_resource_doc *src_resources;
  size_t base_count;
  size_t i;
  int rc;

  if (next_cursor != NULL) {
    *next_cursor = NULL;
  }
  rc = cai_mcp_response_json_body(response, "MCP resources/list response",
                                  &json_body, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&doc, 0, sizeof(doc));
  reader.cursor = json_body;
  lonejson_error_init(&json_error);
  if (reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to rewind MCP response",
                                  &json_error);
  }
  status =
      CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_resources_list_response_map, &doc,
                           cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_resources_list_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to parse MCP resources/list",
                                  &json_error);
  }
  if (doc.error_doc.message != NULL) {
    rc = cai_mcp_set_rpc_error(error, &doc.error_doc);
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_resources_list_response_map, &doc);
    json_body.cleanup(&json_body);
    return rc;
  }
  base_count = impl->resource_count;
  rc = cai_mcp_client_reserve_resources(
      impl, base_count + doc.result.resources.count, error);
  if (rc == CAI_OK) {
    src_resources = (cai_mcp_list_resource_doc *)doc.result.resources.items;
    for (i = 0U; i < doc.result.resources.count; i++) {
      cai_mcp_client_resource_impl *dst = &impl->resources[base_count + i];
      dst->uri = cai_strdup(&impl->allocator, src_resources[i].uri);
      dst->name = cai_strdup(&impl->allocator, src_resources[i].name);
      dst->title = cai_strdup(&impl->allocator,
                              src_resources[i].title != NULL
                                  ? src_resources[i].title
                                  : (src_resources[i].description != NULL
                                         ? src_resources[i].description
                                         : ""));
      dst->description =
          cai_strdup(&impl->allocator, src_resources[i].description != NULL
                                           ? src_resources[i].description
                                           : "");
      dst->mime_type = cai_strdup(
          &impl->allocator,
          src_resources[i].mime_type != NULL ? src_resources[i].mime_type : "");
      if (dst->uri == NULL || dst->name == NULL || dst->title == NULL ||
          dst->description == NULL || dst->mime_type == NULL) {
        rc = cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to copy MCP resource metadata");
        cai_mcp_client_resource_impl_cleanup(&impl->allocator, dst);
        break;
      }
      dst->public_resource.uri = dst->uri;
      dst->public_resource.name = dst->name;
      dst->public_resource.title = dst->title;
      dst->public_resource.description = dst->description;
      dst->public_resource.mime_type = dst->mime_type;
      impl->resource_count++;
    }
  }
  if (rc == CAI_OK && next_cursor != NULL && doc.result.next_cursor != NULL) {
    *next_cursor = cai_strdup(&impl->allocator, doc.result.next_cursor);
    if (*next_cursor == NULL) {
      rc = cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to copy MCP resources/list cursor");
    }
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_resources_list_response_map, &doc);
  if (rc != CAI_OK) {
    cai_mcp_client_clear_resources(impl);
    if (next_cursor != NULL) {
      cai_free_mem(&impl->allocator, *next_cursor);
      *next_cursor = NULL;
    }
  }
  json_body.cleanup(&json_body);
  return rc;
}

static int cai_mcp_parse_resource_templates_list_response(
    cai_mcp_streamable_http_client_impl *impl,
    const cai_mcp_http_response_capture *response, char **next_cursor,
    cai_error *error) {
  cai_mcp_resource_templates_list_response_doc doc;
  lonejson_spooled json_body;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  cai_mcp_list_resource_template_doc *src_resource_templates;
  size_t base_count;
  size_t i;
  int rc;

  if (next_cursor != NULL) {
    *next_cursor = NULL;
  }
  rc = cai_mcp_response_json_body(
      response, "MCP resources/templates/list response", &json_body, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&doc, 0, sizeof(doc));
  reader.cursor = json_body;
  lonejson_error_init(&json_error);
  if (reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to rewind MCP response",
                                  &json_error);
  }
  status = CAI_LJ->parse_reader(
      CAI_LJ, &cai_mcp_resource_templates_list_response_map, &doc,
      cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_resource_templates_list_response_map,
                    &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(
        error, "failed to parse MCP resources/templates/list", &json_error);
  }
  if (doc.error_doc.message != NULL) {
    rc = cai_mcp_set_rpc_error(error, &doc.error_doc);
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_resource_templates_list_response_map,
                    &doc);
    json_body.cleanup(&json_body);
    return rc;
  }
  base_count = impl->resource_template_count;
  rc = cai_mcp_client_reserve_resource_templates(
      impl, base_count + doc.result.resource_templates.count, error);
  if (rc == CAI_OK) {
    src_resource_templates = (cai_mcp_list_resource_template_doc *)
                                 doc.result.resource_templates.items;
    for (i = 0U; i < doc.result.resource_templates.count; i++) {
      cai_mcp_client_resource_template_impl *dst =
          &impl->resource_templates[base_count + i];
      dst->uri_template =
          cai_strdup(&impl->allocator, src_resource_templates[i].uri_template);
      dst->name = cai_strdup(&impl->allocator, src_resource_templates[i].name);
      dst->title = cai_strdup(
          &impl->allocator, src_resource_templates[i].title != NULL
                                ? src_resource_templates[i].title
                                : (src_resource_templates[i].description != NULL
                                       ? src_resource_templates[i].description
                                       : ""));
      dst->description = cai_strdup(
          &impl->allocator, src_resource_templates[i].description != NULL
                                ? src_resource_templates[i].description
                                : "");
      dst->mime_type = cai_strdup(&impl->allocator,
                                  src_resource_templates[i].mime_type != NULL
                                      ? src_resource_templates[i].mime_type
                                      : "");
      if (dst->uri_template == NULL || dst->name == NULL ||
          dst->title == NULL || dst->description == NULL ||
          dst->mime_type == NULL) {
        rc = cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to copy MCP resource template metadata");
        cai_mcp_client_resource_template_impl_cleanup(&impl->allocator, dst);
        break;
      }
      dst->public_resource_template.uri_template = dst->uri_template;
      dst->public_resource_template.name = dst->name;
      dst->public_resource_template.title = dst->title;
      dst->public_resource_template.description = dst->description;
      dst->public_resource_template.mime_type = dst->mime_type;
      impl->resource_template_count++;
    }
  }
  if (rc == CAI_OK && next_cursor != NULL && doc.result.next_cursor != NULL) {
    *next_cursor = cai_strdup(&impl->allocator, doc.result.next_cursor);
    if (*next_cursor == NULL) {
      rc = cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to copy MCP resources/templates/list cursor");
    }
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_resource_templates_list_response_map, &doc);
  if (rc != CAI_OK) {
    cai_mcp_client_clear_resource_templates(impl);
    if (next_cursor != NULL) {
      cai_free_mem(&impl->allocator, *next_cursor);
      *next_cursor = NULL;
    }
  }
  json_body.cleanup(&json_body);
  return rc;
}

static int cai_mcp_parse_prompts_list_response(
    cai_mcp_streamable_http_client_impl *impl,
    const cai_mcp_http_response_capture *response, char **next_cursor,
    cai_error *error) {
  cai_mcp_prompts_list_response_doc doc;
  lonejson_spooled json_body;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  cai_mcp_list_prompt_doc *src_prompts;
  size_t base_count;
  size_t i;
  int rc;

  if (next_cursor != NULL) {
    *next_cursor = NULL;
  }
  rc = cai_mcp_response_json_body(response, "MCP prompts/list response",
                                  &json_body, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&doc, 0, sizeof(doc));
  reader.cursor = json_body;
  lonejson_error_init(&json_error);
  if (reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to rewind MCP response",
                                  &json_error);
  }
  status =
      CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_prompts_list_response_map, &doc,
                           cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_prompts_list_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to parse MCP prompts/list",
                                  &json_error);
  }
  if (doc.error_doc.message != NULL) {
    rc = cai_mcp_set_rpc_error(error, &doc.error_doc);
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_prompts_list_response_map, &doc);
    json_body.cleanup(&json_body);
    return rc;
  }
  base_count = impl->prompt_count;
  rc = cai_mcp_client_reserve_prompts(
      impl, base_count + doc.result.prompts.count, error);
  if (rc == CAI_OK) {
    src_prompts = (cai_mcp_list_prompt_doc *)doc.result.prompts.items;
    for (i = 0U; i < doc.result.prompts.count; i++) {
      cai_mcp_client_prompt_impl *dst = &impl->prompts[base_count + i];
      dst->name = cai_strdup(&impl->allocator, src_prompts[i].name);
      dst->title =
          cai_strdup(&impl->allocator, src_prompts[i].title != NULL
                                           ? src_prompts[i].title
                                           : (src_prompts[i].description != NULL
                                                  ? src_prompts[i].description
                                                  : ""));
      dst->description = cai_strdup(
          &impl->allocator,
          src_prompts[i].description != NULL ? src_prompts[i].description : "");
      dst->arguments_json =
          src_prompts[i].arguments.methods != NULL
              ? cai_mcp_json_value_to_cstr(&src_prompts[i].arguments, error)
              : cai_strdup(&impl->allocator, "[]");
      if (dst->name == NULL || dst->title == NULL || dst->description == NULL ||
          dst->arguments_json == NULL) {
        rc = error != NULL && error->code != CAI_OK
                 ? error->code
                 : cai_set_error(error, CAI_ERR_NOMEM,
                                 "failed to copy MCP prompt metadata");
        cai_mcp_client_prompt_impl_cleanup(&impl->allocator, dst);
        break;
      }
      dst->public_prompt.name = dst->name;
      dst->public_prompt.title = dst->title;
      dst->public_prompt.description = dst->description;
      dst->public_prompt.arguments_json = dst->arguments_json;
      impl->prompt_count++;
    }
  }
  if (rc == CAI_OK && next_cursor != NULL && doc.result.next_cursor != NULL) {
    *next_cursor = cai_strdup(&impl->allocator, doc.result.next_cursor);
    if (*next_cursor == NULL) {
      rc = cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to copy MCP prompts/list cursor");
    }
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_prompts_list_response_map, &doc);
  if (rc != CAI_OK) {
    cai_mcp_client_clear_prompts(impl);
    if (next_cursor != NULL) {
      cai_free_mem(&impl->allocator, *next_cursor);
      *next_cursor = NULL;
    }
  }
  json_body.cleanup(&json_body);
  return rc;
}

static int
cai_mcp_parse_result_response(const cai_mcp_http_response_capture *response,
                              const char *response_name, const char *parse_name,
                              cai_sink *output, cai_error *error) {
  cai_mcp_jsonrpc_sink_response_doc doc;
  lonejson_spooled json_body;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  int rc;

  rc = cai_mcp_response_json_body(response, response_name, &json_body, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
  CAI_LJ_PRESERVE->json_value_init(CAI_LJ_PRESERVE, &doc.result);
  if (doc.result.methods->set_parse_sink(&doc.result, cai_mcp_cai_sink_bridge,
                                         output,
                                         &json_error) != LONEJSON_STATUS_OK) {
    CAI_LJ_PRESERVE->cleanup(CAI_LJ_PRESERVE,
                             &cai_mcp_jsonrpc_sink_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to prepare MCP result sink",
                                  &json_error);
  }
  reader.cursor = json_body;
  if (reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    CAI_LJ_PRESERVE->cleanup(CAI_LJ_PRESERVE,
                             &cai_mcp_jsonrpc_sink_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to rewind MCP response",
                                  &json_error);
  }
  status = CAI_LJ_PRESERVE->parse_reader(
      CAI_LJ_PRESERVE, &cai_mcp_jsonrpc_sink_response_map, &doc,
      cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ_PRESERVE->cleanup(CAI_LJ_PRESERVE,
                             &cai_mcp_jsonrpc_sink_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, parse_name, &json_error);
  }
  if (doc.error_doc.message != NULL) {
    rc = cai_mcp_set_rpc_error(error, &doc.error_doc);
    CAI_LJ_PRESERVE->cleanup(CAI_LJ_PRESERVE,
                             &cai_mcp_jsonrpc_sink_response_map, &doc);
    json_body.cleanup(&json_body);
    return rc;
  }
  CAI_LJ_PRESERVE->cleanup(CAI_LJ_PRESERVE, &cai_mcp_jsonrpc_sink_response_map,
                           &doc);
  json_body.cleanup(&json_body);
  return CAI_OK;
}

static int cai_mcp_parse_empty_result_response(
    const cai_mcp_http_response_capture *response, const char *response_name,
    const char *parse_name, cai_error *error) {
  cai_mcp_jsonrpc_sink_response_doc doc;
  lonejson_spooled json_body;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  int rc;

  rc = cai_mcp_response_json_body(response, response_name, &json_body, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
  CAI_LJ_PRESERVE->json_value_init(CAI_LJ_PRESERVE, &doc.result);
  if (doc.result.methods->set_parse_sink(&doc.result, cai_mcp_discard_sink,
                                         NULL,
                                         &json_error) != LONEJSON_STATUS_OK) {
    CAI_LJ_PRESERVE->cleanup(CAI_LJ_PRESERVE,
                             &cai_mcp_jsonrpc_sink_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to prepare MCP result sink",
                                  &json_error);
  }
  reader.cursor = json_body;
  if (reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    CAI_LJ_PRESERVE->cleanup(CAI_LJ_PRESERVE,
                             &cai_mcp_jsonrpc_sink_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to rewind MCP response",
                                  &json_error);
  }
  status = CAI_LJ_PRESERVE->parse_reader(
      CAI_LJ_PRESERVE, &cai_mcp_jsonrpc_sink_response_map, &doc,
      cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ_PRESERVE->cleanup(CAI_LJ_PRESERVE,
                             &cai_mcp_jsonrpc_sink_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, parse_name, &json_error);
  }
  if (doc.error_doc.message != NULL) {
    rc = cai_mcp_set_rpc_error(error, &doc.error_doc);
    CAI_LJ_PRESERVE->cleanup(CAI_LJ_PRESERVE,
                             &cai_mcp_jsonrpc_sink_response_map, &doc);
    json_body.cleanup(&json_body);
    return rc;
  }
  CAI_LJ_PRESERVE->cleanup(CAI_LJ_PRESERVE, &cai_mcp_jsonrpc_sink_response_map,
                           &doc);
  json_body.cleanup(&json_body);
  return CAI_OK;
}

static int
cai_mcp_parse_ping_response(const cai_mcp_http_response_capture *response,
                            cai_error *error) {
  return cai_mcp_parse_empty_result_response(response, "MCP ping response",
                                             "failed to parse MCP ping", error);
}

static int
cai_mcp_parse_call_response(const cai_mcp_http_response_capture *response,
                            cai_sink *output, cai_error *error) {
  return cai_mcp_parse_result_response(response, "MCP tools/call response",
                                       "failed to parse MCP tools/call", output,
                                       error);
}

static int cai_mcp_parse_resource_read_response(
    const cai_mcp_http_response_capture *response, cai_sink *output,
    cai_error *error) {
  return cai_mcp_parse_result_response(response, "MCP resources/read response",
                                       "failed to parse MCP resources/read",
                                       output, error);
}

static int
cai_mcp_parse_prompt_get_response(const cai_mcp_http_response_capture *response,
                                  cai_sink *output, cai_error *error) {
  return cai_mcp_parse_result_response(response, "MCP prompts/get response",
                                       "failed to parse MCP prompts/get",
                                       output, error);
}

static int
cai_mcp_parse_completion_response(const cai_mcp_http_response_capture *response,
                                  cai_sink *output, cai_error *error) {
  return cai_mcp_parse_result_response(
      response, "MCP completion/complete response",
      "failed to parse MCP completion/complete", output, error);
}

static int cai_mcp_streamable_initialize(cai_mcp_client *client,
                                         cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_mcp_http_response_capture response;
  lonejson_spooled request;
  size_t request_len;
  int rc;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP client is required");
  }
  if (impl->initialized) {
    return CAI_OK;
  }
  memset(&response, 0, sizeof(response));
  rc = cai_mcp_initialize_request(impl, &request, &request_len, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_post(impl, &request, request_len, 1, &response, error);
  }
  cai_mcp_spooled_cleanup_if_initialized(&request);
  if (rc == CAI_OK) {
    rc = cai_mcp_parse_initialize_response(impl, &response, error);
  }
  if (rc == CAI_OK && response.session_id != NULL) {
    impl->session_id = cai_strdup(&impl->allocator, response.session_id);
    if (impl->session_id == NULL) {
      rc =
          cai_set_error(error, CAI_ERR_NOMEM, "failed to store MCP session id");
    }
  }
  cai_mcp_http_response_capture_cleanup(&response);
  if (rc == CAI_OK) {
    lonejson_spooled notification;
    size_t notification_len;

    impl->initialized = 1;
    rc = cai_mcp_initialized_notification(&notification, &notification_len,
                                          error);
    if (rc == CAI_OK) {
      rc = cai_mcp_post(impl, &notification, notification_len, 0, &response,
                        error);
      cai_mcp_http_response_capture_cleanup(&response);
    }
    cai_mcp_spooled_cleanup_if_initialized(&notification);
  }
  return rc;
}

static int cai_mcp_post_request_with_session_recovery(
    cai_mcp_streamable_http_client_impl *impl, const lonejson_spooled *request,
    size_t request_len, cai_mcp_http_response_capture *response,
    cai_error *error) {
  int had_session;
  int rc;

  had_session = impl != NULL && impl->initialized && impl->session_id != NULL;
  rc = cai_mcp_post(impl, request, request_len, 1, response, error);
  if (rc == CAI_OK || !had_session || response == NULL ||
      response->status != 404L) {
    return rc;
  }

  cai_mcp_http_response_capture_cleanup(response);
  cai_mcp_clear_error(error);
  cai_mcp_streamable_reset_session(impl);
  rc = cai_mcp_streamable_initialize(&impl->public_client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_mcp_post(impl, request, request_len, 1, response, error);
}

static int cai_mcp_streamable_ping(cai_mcp_client *client, cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_mcp_http_response_capture response;
  lonejson_spooled request;
  size_t request_len;
  int rc;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP client is required");
  }
  rc = cai_mcp_client_initialize(client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&response, 0, sizeof(response));
  rc = cai_mcp_ping_request(impl, &request, &request_len, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_post_request_with_session_recovery(impl, &request, request_len,
                                                    &response, error);
  }
  cai_mcp_spooled_cleanup_if_initialized(&request);
  if (rc == CAI_OK) {
    rc = cai_mcp_parse_ping_response(&response, error);
  }
  cai_mcp_http_response_capture_cleanup(&response);
  return rc;
}

static int cai_mcp_streamable_refresh_tools(cai_mcp_client *client,
                                            cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_mcp_http_response_capture response;
  lonejson_spooled request;
  char *cursor;
  char *next_cursor;
  size_t request_len;
  int rc;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP client is required");
  }
  rc = cai_mcp_client_initialize(client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  cursor = NULL;
  cai_mcp_client_clear_tools(impl);
  do {
    next_cursor = NULL;
    memset(&response, 0, sizeof(response));
    rc = cai_mcp_list_request(impl, "tools/list", cursor, &request,
                              &request_len, error);
    if (rc == CAI_OK) {
      rc = cai_mcp_post_request_with_session_recovery(
          impl, &request, request_len, &response, error);
    }
    cai_mcp_spooled_cleanup_if_initialized(&request);
    if (rc == CAI_OK) {
      rc = cai_mcp_parse_tools_list_response(impl, &response, &next_cursor,
                                             error);
    }
    cai_mcp_http_response_capture_cleanup(&response);
    cai_free_mem(&impl->allocator, cursor);
    cursor = next_cursor;
  } while (rc == CAI_OK && cursor != NULL && cursor[0] != '\0');
  cai_free_mem(&impl->allocator, cursor);
  if (rc != CAI_OK) {
    cai_mcp_client_clear_tools(impl);
  }
  return rc;
}

static size_t cai_mcp_streamable_tool_count(const cai_mcp_client *client) {
  const cai_mcp_streamable_http_client_impl *impl;

  impl = cai_mcp_streamable_const_impl(client);
  return impl != NULL ? impl->tool_count : 0U;
}

static const cai_mcp_client_tool *
cai_mcp_streamable_tool_at(const cai_mcp_client *client, size_t index) {
  const cai_mcp_streamable_http_client_impl *impl;

  impl = cai_mcp_streamable_const_impl(client);
  if (impl == NULL || index >= impl->tool_count) {
    return NULL;
  }
  return &impl->tools[index].public_tool;
}

static int cai_mcp_streamable_call_tool(cai_mcp_client *client,
                                        const char *name,
                                        lonejson_spooled *arguments_json,
                                        cai_sink *output, cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_mcp_http_response_capture response;
  lonejson_spooled request;
  size_t request_len;
  int rc;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL || output == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client and output sink are required");
  }
  rc = cai_mcp_client_initialize(client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&response, 0, sizeof(response));
  rc = cai_mcp_call_request(impl, name, arguments_json, &request, &request_len,
                            error);
  if (rc == CAI_OK) {
    rc = cai_mcp_post_request_with_session_recovery(impl, &request, request_len,
                                                    &response, error);
  }
  cai_mcp_spooled_cleanup_if_initialized(&request);
  if (rc == CAI_OK) {
    rc = cai_mcp_parse_call_response(&response, output, error);
  }
  cai_mcp_http_response_capture_cleanup(&response);
  return rc;
}

static int cai_mcp_streamable_refresh_resources(cai_mcp_client *client,
                                                cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_mcp_http_response_capture response;
  lonejson_spooled request;
  char *cursor;
  char *next_cursor;
  size_t request_len;
  int rc;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP client is required");
  }
  rc = cai_mcp_client_initialize(client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  cursor = NULL;
  cai_mcp_client_clear_resources(impl);
  do {
    next_cursor = NULL;
    memset(&response, 0, sizeof(response));
    rc = cai_mcp_list_request(impl, "resources/list", cursor, &request,
                              &request_len, error);
    if (rc == CAI_OK) {
      rc = cai_mcp_post_request_with_session_recovery(
          impl, &request, request_len, &response, error);
    }
    cai_mcp_spooled_cleanup_if_initialized(&request);
    if (rc == CAI_OK) {
      rc = cai_mcp_parse_resources_list_response(impl, &response, &next_cursor,
                                                 error);
    }
    cai_mcp_http_response_capture_cleanup(&response);
    cai_free_mem(&impl->allocator, cursor);
    cursor = next_cursor;
  } while (rc == CAI_OK && cursor != NULL && cursor[0] != '\0');
  cai_free_mem(&impl->allocator, cursor);
  if (rc != CAI_OK) {
    cai_mcp_client_clear_resources(impl);
  }
  return rc;
}

static size_t cai_mcp_streamable_resource_count(const cai_mcp_client *client) {
  const cai_mcp_streamable_http_client_impl *impl;

  impl = cai_mcp_streamable_const_impl(client);
  return impl != NULL ? impl->resource_count : 0U;
}

static const cai_mcp_client_resource *
cai_mcp_streamable_resource_at(const cai_mcp_client *client, size_t index) {
  const cai_mcp_streamable_http_client_impl *impl;

  impl = cai_mcp_streamable_const_impl(client);
  if (impl == NULL || index >= impl->resource_count) {
    return NULL;
  }
  return &impl->resources[index].public_resource;
}

static int cai_mcp_streamable_read_resource(cai_mcp_client *client,
                                            const char *uri, cai_sink *output,
                                            cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_mcp_http_response_capture response;
  lonejson_spooled request;
  size_t request_len;
  int rc;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL || output == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client and output sink are required");
  }
  rc = cai_mcp_client_initialize(client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&response, 0, sizeof(response));
  rc = cai_mcp_resource_read_request(impl, uri, &request, &request_len, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_post_request_with_session_recovery(impl, &request, request_len,
                                                    &response, error);
  }
  cai_mcp_spooled_cleanup_if_initialized(&request);
  if (rc == CAI_OK) {
    rc = cai_mcp_parse_resource_read_response(&response, output, error);
  }
  cai_mcp_http_response_capture_cleanup(&response);
  return rc;
}

static int cai_mcp_streamable_refresh_resource_templates(cai_mcp_client *client,
                                                         cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_mcp_http_response_capture response;
  lonejson_spooled request;
  char *cursor;
  char *next_cursor;
  size_t request_len;
  int rc;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP client is required");
  }
  rc = cai_mcp_client_initialize(client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  cursor = NULL;
  cai_mcp_client_clear_resource_templates(impl);
  do {
    next_cursor = NULL;
    memset(&response, 0, sizeof(response));
    rc = cai_mcp_list_request(impl, "resources/templates/list", cursor,
                              &request, &request_len, error);
    if (rc == CAI_OK) {
      rc = cai_mcp_post_request_with_session_recovery(
          impl, &request, request_len, &response, error);
    }
    cai_mcp_spooled_cleanup_if_initialized(&request);
    if (rc == CAI_OK) {
      rc = cai_mcp_parse_resource_templates_list_response(impl, &response,
                                                          &next_cursor, error);
    }
    cai_mcp_http_response_capture_cleanup(&response);
    cai_free_mem(&impl->allocator, cursor);
    cursor = next_cursor;
  } while (rc == CAI_OK && cursor != NULL && cursor[0] != '\0');
  cai_free_mem(&impl->allocator, cursor);
  if (rc != CAI_OK) {
    cai_mcp_client_clear_resource_templates(impl);
  }
  return rc;
}

static size_t
cai_mcp_streamable_resource_template_count(const cai_mcp_client *client) {
  const cai_mcp_streamable_http_client_impl *impl;

  impl = cai_mcp_streamable_const_impl(client);
  return impl != NULL ? impl->resource_template_count : 0U;
}

static const cai_mcp_client_resource_template *
cai_mcp_streamable_resource_template_at(const cai_mcp_client *client,
                                        size_t index) {
  const cai_mcp_streamable_http_client_impl *impl;

  impl = cai_mcp_streamable_const_impl(client);
  if (impl == NULL || index >= impl->resource_template_count) {
    return NULL;
  }
  return &impl->resource_templates[index].public_resource_template;
}

static int cai_mcp_streamable_refresh_prompts(cai_mcp_client *client,
                                              cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_mcp_http_response_capture response;
  lonejson_spooled request;
  char *cursor;
  char *next_cursor;
  size_t request_len;
  int rc;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP client is required");
  }
  rc = cai_mcp_client_initialize(client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  cursor = NULL;
  cai_mcp_client_clear_prompts(impl);
  do {
    next_cursor = NULL;
    memset(&response, 0, sizeof(response));
    rc = cai_mcp_list_request(impl, "prompts/list", cursor, &request,
                              &request_len, error);
    if (rc == CAI_OK) {
      rc = cai_mcp_post_request_with_session_recovery(
          impl, &request, request_len, &response, error);
    }
    cai_mcp_spooled_cleanup_if_initialized(&request);
    if (rc == CAI_OK) {
      rc = cai_mcp_parse_prompts_list_response(impl, &response, &next_cursor,
                                               error);
    }
    cai_mcp_http_response_capture_cleanup(&response);
    cai_free_mem(&impl->allocator, cursor);
    cursor = next_cursor;
  } while (rc == CAI_OK && cursor != NULL && cursor[0] != '\0');
  cai_free_mem(&impl->allocator, cursor);
  if (rc != CAI_OK) {
    cai_mcp_client_clear_prompts(impl);
  }
  return rc;
}

static size_t cai_mcp_streamable_prompt_count(const cai_mcp_client *client) {
  const cai_mcp_streamable_http_client_impl *impl;

  impl = cai_mcp_streamable_const_impl(client);
  return impl != NULL ? impl->prompt_count : 0U;
}

static const cai_mcp_client_prompt *
cai_mcp_streamable_prompt_at(const cai_mcp_client *client, size_t index) {
  const cai_mcp_streamable_http_client_impl *impl;

  impl = cai_mcp_streamable_const_impl(client);
  if (impl == NULL || index >= impl->prompt_count) {
    return NULL;
  }
  return &impl->prompts[index].public_prompt;
}

static int cai_mcp_streamable_get_prompt(cai_mcp_client *client,
                                         const char *name,
                                         lonejson_spooled *arguments_json,
                                         cai_sink *output, cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_mcp_http_response_capture response;
  lonejson_spooled request;
  size_t request_len;
  int rc;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL || output == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client and output sink are required");
  }
  rc = cai_mcp_client_initialize(client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&response, 0, sizeof(response));
  rc = cai_mcp_prompt_get_request(impl, name, arguments_json, &request,
                                  &request_len, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_post_request_with_session_recovery(impl, &request, request_len,
                                                    &response, error);
  }
  cai_mcp_spooled_cleanup_if_initialized(&request);
  if (rc == CAI_OK) {
    rc = cai_mcp_parse_prompt_get_response(&response, output, error);
  }
  cai_mcp_http_response_capture_cleanup(&response);
  return rc;
}

static int cai_mcp_streamable_complete(cai_mcp_client *client,
                                       const char *ref_type,
                                       const char *ref_value,
                                       const char *argument_name,
                                       const char *argument_value,
                                       lonejson_spooled *context_arguments_json,
                                       cai_sink *output, cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_mcp_http_response_capture response;
  lonejson_spooled request;
  size_t request_len;
  int rc;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL || output == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client and output sink are required");
  }
  rc = cai_mcp_client_initialize(client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&response, 0, sizeof(response));
  rc = cai_mcp_completion_request(impl, ref_type, ref_value, argument_name,
                                  argument_value, context_arguments_json,
                                  &request, &request_len, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_post_request_with_session_recovery(impl, &request, request_len,
                                                    &response, error);
  }
  cai_mcp_spooled_cleanup_if_initialized(&request);
  if (rc == CAI_OK) {
    rc = cai_mcp_parse_completion_response(&response, output, error);
  }
  cai_mcp_http_response_capture_cleanup(&response);
  return rc;
}

static void cai_mcp_streamable_destroy(cai_mcp_client *client) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_allocator allocator;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL) {
    return;
  }
  allocator = impl->allocator;
  cai_mcp_client_clear_tools(impl);
  cai_mcp_client_clear_resources(impl);
  cai_mcp_client_clear_resource_templates(impl);
  cai_mcp_client_clear_prompts(impl);
  cai_free_mem(&allocator, impl->tools);
  cai_free_mem(&allocator, impl->resources);
  cai_free_mem(&allocator, impl->resource_templates);
  cai_free_mem(&allocator, impl->prompts);
  cai_free_mem(&allocator, impl->url);
  cai_free_mem(&allocator, impl->client_name);
  cai_free_mem(&allocator, impl->client_version);
  cai_free_mem(&allocator, impl->protocol_version);
  cai_free_mem(&allocator, impl->ca_bundle_path);
  cai_free_mem(&allocator, impl->ca_path);
  cai_free_mem(&allocator, impl->session_id);
  memset(impl, 0, sizeof(*impl));
  cai_free_mem(&allocator, impl);
}

void cai_mcp_streamable_http_client_config_init(
    cai_mcp_streamable_http_client_config *config) {
  if (config != NULL) {
    memset(config, 0, sizeof(*config));
  }
}

int cai_mcp_streamable_http_client_open(
    const cai_mcp_streamable_http_client_config *config, cai_mcp_client **out,
    cai_error *error) {
  cai_mcp_streamable_http_client_config defaults;
  const cai_mcp_streamable_http_client_config *effective;
  cai_mcp_streamable_http_client_impl *impl;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client output pointer is required");
  }
  *out = NULL;
  cai_mcp_streamable_http_client_config_init(&defaults);
  effective = config != NULL ? config : &defaults;
  if (effective->url == NULL || effective->url[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP Streamable HTTP URL is required");
  }
  impl = (cai_mcp_streamable_http_client_impl *)cai_alloc(&effective->allocator,
                                                          sizeof(*impl));
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate MCP client");
  }
  memset(impl, 0, sizeof(*impl));
  impl->allocator = effective->allocator;
  impl->url = cai_strdup(&impl->allocator, effective->url);
  impl->client_name = cai_strdup(
      &impl->allocator,
      effective->client_name != NULL ? effective->client_name : "cai");
  impl->client_version =
      cai_strdup(&impl->allocator, effective->client_version != NULL
                                       ? effective->client_version
                                       : CAI_VERSION_STRING);
  impl->protocol_version =
      cai_strdup(&impl->allocator, effective->protocol_version != NULL
                                       ? effective->protocol_version
                                       : CAI_MCP_PROTOCOL_VERSION);
  impl->timeout_ms = effective->timeout_ms > 0L ? effective->timeout_ms
                                                : CAI_DEFAULT_HTTP_TIMEOUT_MS;
  impl->insecure_skip_verify = effective->insecure_skip_verify;
  impl->ca_bundle_path =
      cai_strdup(&impl->allocator, effective->ca_bundle_path);
  impl->ca_path = cai_strdup(&impl->allocator, effective->ca_path);
  if (impl->url == NULL || impl->client_name == NULL ||
      impl->client_version == NULL || impl->protocol_version == NULL ||
      (effective->ca_bundle_path != NULL && impl->ca_bundle_path == NULL) ||
      (effective->ca_path != NULL && impl->ca_path == NULL)) {
    cai_mcp_streamable_destroy(&impl->public_client);
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to copy MCP config");
  }
  impl->public_client.initialize = cai_mcp_streamable_initialize;
  impl->public_client.ping = cai_mcp_streamable_ping;
  impl->public_client.refresh_tools = cai_mcp_streamable_refresh_tools;
  impl->public_client.tool_count = cai_mcp_streamable_tool_count;
  impl->public_client.tool_at = cai_mcp_streamable_tool_at;
  impl->public_client.call_tool = cai_mcp_streamable_call_tool;
  impl->public_client.refresh_resources = cai_mcp_streamable_refresh_resources;
  impl->public_client.resource_count = cai_mcp_streamable_resource_count;
  impl->public_client.resource_at = cai_mcp_streamable_resource_at;
  impl->public_client.read_resource = cai_mcp_streamable_read_resource;
  impl->public_client.refresh_resource_templates =
      cai_mcp_streamable_refresh_resource_templates;
  impl->public_client.resource_template_count =
      cai_mcp_streamable_resource_template_count;
  impl->public_client.resource_template_at =
      cai_mcp_streamable_resource_template_at;
  impl->public_client.refresh_prompts = cai_mcp_streamable_refresh_prompts;
  impl->public_client.prompt_count = cai_mcp_streamable_prompt_count;
  impl->public_client.prompt_at = cai_mcp_streamable_prompt_at;
  impl->public_client.get_prompt = cai_mcp_streamable_get_prompt;
  impl->public_client.complete = cai_mcp_streamable_complete;
  impl->public_client.destroy = cai_mcp_streamable_destroy;
  impl->public_client.impl = impl;
  *out = &impl->public_client;
  return CAI_OK;
}

int cai_mcp_client_initialize(cai_mcp_client *client, cai_error *error) {
  if (client == NULL || client->initialize == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client initialize receiver is required");
  }
  return client->initialize(client, error);
}

int cai_mcp_client_ping(cai_mcp_client *client, cai_error *error) {
  if (client == NULL || client->ping == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client ping receiver is required");
  }
  return client->ping(client, error);
}

int cai_mcp_client_refresh_tools(cai_mcp_client *client, cai_error *error) {
  if (client == NULL || client->refresh_tools == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client refresh_tools receiver is required");
  }
  return client->refresh_tools(client, error);
}

size_t cai_mcp_client_tool_count(const cai_mcp_client *client) {
  return client != NULL && client->tool_count != NULL
             ? client->tool_count(client)
             : 0U;
}

const cai_mcp_client_tool *cai_mcp_client_tool_at(const cai_mcp_client *client,
                                                  size_t index) {
  return client != NULL && client->tool_at != NULL
             ? client->tool_at(client, index)
             : NULL;
}

int cai_mcp_client_call_tool(cai_mcp_client *client, const char *name,
                             lonejson_spooled *arguments_json, cai_sink *output,
                             cai_error *error) {
  if (client == NULL || client->call_tool == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client call_tool receiver is required");
  }
  return client->call_tool(client, name, arguments_json, output, error);
}

int cai_mcp_client_refresh_resources(cai_mcp_client *client, cai_error *error) {
  if (client == NULL || client->refresh_resources == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client refresh_resources receiver is required");
  }
  return client->refresh_resources(client, error);
}

size_t cai_mcp_client_resource_count(const cai_mcp_client *client) {
  return client != NULL && client->resource_count != NULL
             ? client->resource_count(client)
             : 0U;
}

const cai_mcp_client_resource *
cai_mcp_client_resource_at(const cai_mcp_client *client, size_t index) {
  return client != NULL && client->resource_at != NULL
             ? client->resource_at(client, index)
             : NULL;
}

int cai_mcp_client_read_resource(cai_mcp_client *client, const char *uri,
                                 cai_sink *output, cai_error *error) {
  if (client == NULL || client->read_resource == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client read_resource receiver is required");
  }
  return client->read_resource(client, uri, output, error);
}

int cai_mcp_client_refresh_resource_templates(cai_mcp_client *client,
                                              cai_error *error) {
  if (client == NULL || client->refresh_resource_templates == NULL) {
    return cai_set_error(
        error, CAI_ERR_INVALID,
        "MCP client refresh_resource_templates receiver is required");
  }
  return client->refresh_resource_templates(client, error);
}

size_t cai_mcp_client_resource_template_count(const cai_mcp_client *client) {
  return client != NULL && client->resource_template_count != NULL
             ? client->resource_template_count(client)
             : 0U;
}

const cai_mcp_client_resource_template *
cai_mcp_client_resource_template_at(const cai_mcp_client *client,
                                    size_t index) {
  return client != NULL && client->resource_template_at != NULL
             ? client->resource_template_at(client, index)
             : NULL;
}

int cai_mcp_client_refresh_prompts(cai_mcp_client *client, cai_error *error) {
  if (client == NULL || client->refresh_prompts == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client refresh_prompts receiver is required");
  }
  return client->refresh_prompts(client, error);
}

size_t cai_mcp_client_prompt_count(const cai_mcp_client *client) {
  return client != NULL && client->prompt_count != NULL
             ? client->prompt_count(client)
             : 0U;
}

const cai_mcp_client_prompt *
cai_mcp_client_prompt_at(const cai_mcp_client *client, size_t index) {
  return client != NULL && client->prompt_at != NULL
             ? client->prompt_at(client, index)
             : NULL;
}

int cai_mcp_client_get_prompt(cai_mcp_client *client, const char *name,
                              lonejson_spooled *arguments_json,
                              cai_sink *output, cai_error *error) {
  if (client == NULL || client->get_prompt == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client get_prompt receiver is required");
  }
  return client->get_prompt(client, name, arguments_json, output, error);
}

int cai_mcp_client_complete(cai_mcp_client *client, const char *ref_type,
                            const char *ref_value, const char *argument_name,
                            const char *argument_value,
                            lonejson_spooled *context_arguments_json,
                            cai_sink *output, cai_error *error) {
  if (client == NULL || client->complete == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client complete receiver is required");
  }
  return client->complete(client, ref_type, ref_value, argument_name,
                          argument_value, context_arguments_json, output,
                          error);
}

static int cai_mcp_registered_tool_callback(void *context,
                                            lonejson_spooled *arguments_json,
                                            cai_sink *output,
                                            cai_error *error) {
  cai_mcp_registry_tool_context *tool_context;

  tool_context = (cai_mcp_registry_tool_context *)context;
  if (tool_context == NULL || tool_context->client == NULL ||
      tool_context->remote_name == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP registered tool context is invalid");
  }
  return cai_mcp_client_call_tool(tool_context->client,
                                  tool_context->remote_name, arguments_json,
                                  output, error);
}

static void cai_mcp_registered_tool_context_cleanup(void *context) {
  cai_mcp_registry_tool_context *tool_context;

  tool_context = (cai_mcp_registry_tool_context *)context;
  if (tool_context != NULL) {
    cai_free_mem(NULL, tool_context->remote_name);
    cai_free_mem(NULL, tool_context);
  }
}

int cai_mcp_client_register_tools(
    cai_mcp_client *client, cai_tool_registry *registry,
    const cai_mcp_tool_registration_config *config, cai_error *error) {
  cai_mcp_tool_registration_config defaults;
  const cai_mcp_tool_registration_config *effective;
  const cai_mcp_client_tool *tool;
  cai_mcp_registry_tool_context *context;
  cai_buffer_builder name_builder;
  size_t i;
  int rc;

  if (client == NULL || registry == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client and tool registry are required");
  }
  memset(&defaults, 0, sizeof(defaults));
  effective = config != NULL ? config : &defaults;
  rc = cai_mcp_client_refresh_tools(client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  for (i = 0U; i < cai_mcp_client_tool_count(client); i++) {
    tool = cai_mcp_client_tool_at(client, i);
    if (tool == NULL || tool->name == NULL || tool->input_schema_json == NULL) {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP tool metadata is incomplete");
    }
    memset(&name_builder, 0, sizeof(name_builder));
    if (effective->name_prefix != NULL) {
      rc = cai_buffer_append_cstr(&name_builder, effective->name_prefix, error);
      if (rc != CAI_OK) {
        cai_free_mem(NULL, name_builder.data);
        return rc;
      }
    }
    rc = cai_buffer_append_cstr(&name_builder, tool->name, error);
    if (rc == CAI_OK) {
      rc = cai_buffer_append(&name_builder, "", 1U, error);
    }
    if (rc != CAI_OK) {
      cai_free_mem(NULL, name_builder.data);
      return rc;
    }
    context =
        (cai_mcp_registry_tool_context *)cai_alloc(NULL, sizeof(*context));
    if (context == NULL) {
      cai_free_mem(NULL, name_builder.data);
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate MCP tool context");
    }
    memset(context, 0, sizeof(*context));
    context->client = client;
    context->remote_name = cai_strdup(NULL, tool->name);
    if (context->remote_name == NULL) {
      cai_free_mem(NULL, context);
      cai_free_mem(NULL, name_builder.data);
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate MCP tool name");
    }
    rc = cai_tool_registry_register_raw_spooled_owned(
        registry, name_builder.data, tool->description, tool->input_schema_json,
        effective->strict, cai_mcp_registered_tool_callback, context,
        cai_mcp_registered_tool_context_cleanup, error);
    cai_free_mem(NULL, name_builder.data);
    if (rc != CAI_OK) {
      cai_mcp_registered_tool_context_cleanup(context);
      return rc;
    }
  }
  return CAI_OK;
}

int cai_agent_register_mcp_client_tools(
    cai_agent *agent, cai_mcp_client *client,
    const cai_mcp_tool_registration_config *config, cai_error *error) {
  cai_agent_impl *impl;

  if (agent == NULL || client == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "agent and MCP client are required");
  }
  impl = CAI_AGENT_IMPL(agent);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is closed");
  }
  return cai_mcp_client_register_tools(client, impl->tools, config, error);
}

void cai_mcp_client_destroy(cai_mcp_client *client) {
  if (client != NULL && client->destroy != NULL) {
    client->destroy(client);
  }
}
