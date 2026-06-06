#define CAI_FUZZ_COMMON_SOURCE
#define CAI_FUZZ_COMMON_SINK
#define CAI_FUZZ_COMMON_HEX
#include <cai/cai.h>
#include <cai/mcp.h>

#include "fuzz_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);

typedef struct cai_fuzz_mcp_store {
  int exists;
  char session_id[CAI_MCP_SESSION_ID_MAX];
  cai_mcp_session_state state;
} cai_fuzz_mcp_store;

typedef struct cai_fuzz_mcp_headers {
  const char *accept;
  const char *protocol_version;
  const char *session_id;
  const char *origin;
} cai_fuzz_mcp_headers;

typedef struct cai_fuzz_mcp_response_headers {
  char session_id[CAI_MCP_SESSION_ID_MAX];
  char content_type[64];
} cai_fuzz_mcp_response_headers;

static int cai_fuzz_mcp_echo(void *context,
                             struct lonejson_spooled *arguments_json,
                             cai_sink *output, cai_error *error) {
  char buffer[128];
  size_t size;
  int n;

  (void)context;
  size = arguments_json->size_fn(arguments_json);
  n = snprintf(buffer, sizeof(buffer), "{\"ok\":true,\"size\":%lu}",
               (unsigned long)size);
  if (n < 0 || (size_t)n >= sizeof(buffer)) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "failed to format fuzz MCP tool output");
  }
  return cai_sink_write(output, buffer, (size_t)n, error);
}

static const char *cai_fuzz_mcp_header_get(void *context, const char *name) {
  cai_fuzz_mcp_headers *headers;

  headers = (cai_fuzz_mcp_headers *)context;
  if (strcmp(name, "accept") == 0) {
    return headers->accept;
  }
  if (strcmp(name, "mcp-protocol-version") == 0) {
    return headers->protocol_version;
  }
  if (strcmp(name, "mcp-session-id") == 0) {
    return headers->session_id;
  }
  if (strcmp(name, "origin") == 0) {
    return headers->origin;
  }
  return NULL;
}

static int cai_fuzz_mcp_header_set(void *context, const char *name,
                                   const char *value, cai_error *error) {
  cai_fuzz_mcp_response_headers *headers;

  (void)error;
  headers = (cai_fuzz_mcp_response_headers *)context;
  if (strcmp(name, "mcp-session-id") == 0) {
    snprintf(headers->session_id, sizeof(headers->session_id), "%s", value);
  } else if (strcmp(name, "content-type") == 0) {
    snprintf(headers->content_type, sizeof(headers->content_type), "%s", value);
  }
  return CAI_OK;
}

static int cai_fuzz_mcp_create(void *context,
                               const cai_mcp_session_state *initial_state,
                               char *session_id, size_t session_id_capacity,
                               cai_error *error) {
  cai_fuzz_mcp_store *store;
  const char *id;

  (void)error;
  store = (cai_fuzz_mcp_store *)context;
  id = "sess-fuzz-1";
  if (strlen(id) + 1U > session_id_capacity) {
    return CAI_ERR_INVALID;
  }
  snprintf(store->session_id, sizeof(store->session_id), "%s", id);
  memcpy(&store->state, initial_state, sizeof(store->state));
  store->exists = 1;
  snprintf(session_id, session_id_capacity, "%s", id);
  return CAI_OK;
}

static int cai_fuzz_mcp_load(void *context, const char *session_id,
                             cai_mcp_session_state *state, cai_error *error) {
  cai_fuzz_mcp_store *store;

  (void)error;
  store = (cai_fuzz_mcp_store *)context;
  if (!store->exists || strcmp(store->session_id, session_id) != 0) {
    return CAI_ERR_INVALID;
  }
  memcpy(state, &store->state, sizeof(*state));
  return CAI_OK;
}

static int cai_fuzz_mcp_save(void *context, const char *session_id,
                             const cai_mcp_session_state *state,
                             cai_error *error) {
  cai_fuzz_mcp_store *store;

  (void)error;
  store = (cai_fuzz_mcp_store *)context;
  if (!store->exists || strcmp(store->session_id, session_id) != 0) {
    return CAI_ERR_INVALID;
  }
  memcpy(&store->state, state, sizeof(store->state));
  return CAI_OK;
}

static int cai_fuzz_mcp_destroy(void *context, const char *session_id,
                                cai_error *error) {
  cai_fuzz_mcp_store *store;

  (void)error;
  store = (cai_fuzz_mcp_store *)context;
  if (store->exists && strcmp(store->session_id, session_id) == 0) {
    memset(store, 0, sizeof(*store));
  }
  return CAI_OK;
}

static int cai_fuzz_mcp_request(cai_mcp_handler *handler, const char *method,
                                const char *accept, const char *session_id,
                                const unsigned char *body, size_t body_size,
                                size_t max_chunk) {
  cai_mcp_http_request request;
  cai_mcp_http_response response;
  cai_fuzz_mcp_headers headers;
  cai_fuzz_mcp_response_headers response_headers;
  cai_fuzz_noop_sink_context sink_context;
  cai_source *source;
  cai_sink *sink;
  cai_error error;

  memset(&request, 0, sizeof(request));
  memset(&response, 0, sizeof(response));
  memset(&headers, 0, sizeof(headers));
  memset(&response_headers, 0, sizeof(response_headers));
  memset(&sink_context, 0, sizeof(sink_context));
  cai_error_init(&error);
  source = NULL;
  sink = NULL;
  headers.accept = accept;
  headers.protocol_version = CAI_MCP_PROTOCOL_VERSION;
  headers.session_id = session_id;
  headers.origin = "http://127.0.0.1";
  if (body != NULL && cai_fuzz_source_new(body, body_size, max_chunk, &source,
                                          &error) != CAI_OK) {
    cai_error_cleanup(&error);
    return 0;
  }
  if (cai_fuzz_sink_new(&sink_context, &sink, &error) != CAI_OK) {
    cai_source_close(source);
    cai_error_cleanup(&error);
    return 0;
  }
  request.method = method;
  request.body = source;
  request.header = cai_fuzz_mcp_header_get;
  request.header_context = &headers;
  response.body = sink;
  response.set_header = cai_fuzz_mcp_header_set;
  response.header_context = &response_headers;
  (void)handler->handle_http(handler, &request, &response, &error);
  cai_sink_close(sink);
  cai_source_close(source);
  cai_error_cleanup(&error);
  return 0;
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
  static const cai_mcp_session_callbacks session_callbacks = {
      cai_fuzz_mcp_create, cai_fuzz_mcp_load, cai_fuzz_mcp_save,
      cai_fuzz_mcp_destroy, NULL};
  static const char schema_json[] =
      "{\"type\":\"object\",\"properties\":{},\"required\":[],"
      "\"additionalProperties\":true}";
  cai_tool_registry *registry;
  cai_mcp_handler *handler;
  cai_mcp_handler_config config;
  cai_fuzz_mcp_store store;
  char *payload_hex;
  char *initialize_json;
  char *tool_json;
  char *response_message_json;
  size_t payload_len;
  size_t max_chunk;
  cai_error error;

  registry = NULL;
  handler = NULL;
  initialize_json = NULL;
  tool_json = NULL;
  response_message_json = NULL;
  memset(&store, 0, sizeof(store));
  cai_error_init(&error);
  if (cai_tool_registry_new(&registry, &error) != CAI_OK) {
    cai_error_cleanup(&error);
    return 0;
  }
  if (cai_tool_registry_register_raw_spooled(
          registry, "echo", "Echo fuzz MCP arguments", schema_json, 0,
          cai_fuzz_mcp_echo, NULL, &error) != CAI_OK) {
    cai_tool_registry_destroy(registry);
    cai_error_cleanup(&error);
    return 0;
  }
  cai_mcp_handler_config_init(&config);
  config.name = "fuzz-mcp";
  config.version = "0";
  config.tools = registry;
  config.enable_sessions = 1;
  config.session = &session_callbacks;
  config.session_context = &store;
  config.response_spool_memory_limit = 32U;
  config.tool_output_max_bytes = 256U;
  if (cai_mcp_handler_new(&config, &handler, &error) != CAI_OK) {
    cai_tool_registry_destroy(registry);
    cai_error_cleanup(&error);
    return 0;
  }

  max_chunk = size == 0U ? 1U : (size_t)(1U + (data[0] % 23U));
  (void)cai_fuzz_mcp_request(handler, "POST", "application/json", NULL, data,
                             size, max_chunk);
  (void)cai_fuzz_mcp_request(handler, "POST", "text/event-stream", NULL, data,
                             size, max_chunk);
  (void)cai_fuzz_mcp_request(handler, "GET", "text/event-stream", NULL, NULL,
                             0U, 1U);

  payload_hex = cai_fuzz_hex_string(data, size, 2048U);
  if (payload_hex != NULL) {
    payload_len = strlen(payload_hex);
    initialize_json = (char *)malloc(payload_len + 256U);
    tool_json = (char *)malloc(payload_len + 256U);
    response_message_json = (char *)malloc(96U);
    if (initialize_json != NULL && tool_json != NULL &&
        response_message_json != NULL) {
      snprintf(initialize_json, payload_len + 256U,
               "{\"jsonrpc\":\"2.0\",\"id\":\"init\",\"method\":"
               "\"initialize\",\"params\":{\"protocolVersion\":\"%s\","
               "\"clientInfo\":{\"name\":\"%s\",\"version\":\"1\"},"
               "\"capabilities\":{}}}",
               CAI_MCP_PROTOCOL_VERSION, payload_hex);
      (void)cai_fuzz_mcp_request(handler, "POST", "application/json", NULL,
                                 (const unsigned char *)initialize_json,
                                 strlen(initialize_json), max_chunk);
      (void)cai_fuzz_mcp_request(
          handler, "POST", "application/json", "sess-fuzz-1",
          (const unsigned char *)"{\"jsonrpc\":\"2.0\",\"id\":\"list\","
                                 "\"method\":\"tools/list\",\"params\":{}}",
          strlen("{\"jsonrpc\":\"2.0\",\"id\":\"list\","
                 "\"method\":\"tools/list\",\"params\":{}}"),
          max_chunk);
      snprintf(tool_json, payload_len + 256U,
               "{\"jsonrpc\":\"2.0\",\"id\":\"call\",\"method\":"
               "\"tools/call\",\"params\":{\"name\":\"echo\","
               "\"arguments\":{\"payload\":\"%s\"}}}",
               payload_hex);
      (void)cai_fuzz_mcp_request(
          handler, "POST", "text/event-stream", "sess-fuzz-1",
          (const unsigned char *)tool_json, strlen(tool_json), max_chunk);
      (void)cai_fuzz_mcp_request(
          handler, "POST", "application/json", "sess-fuzz-1",
          (const unsigned char
               *)"{\"jsonrpc\":\"2.0\",\"id\":\"ping\",\"method\":\"ping\","
                 "\"params\":{}}",
          strlen("{\"jsonrpc\":\"2.0\",\"id\":\"ping\",\"method\":\"ping\","
                 "\"params\":{}}"),
          max_chunk);
      (void)cai_fuzz_mcp_request(handler, "GET", "text/event-stream",
                                 "sess-fuzz-1", NULL, 0U, 1U);
      snprintf(response_message_json, 96U,
               "{\"jsonrpc\":\"2.0\",\"id\":\"noop\",\"result\":{}}");
      (void)cai_fuzz_mcp_request(handler, "POST", "application/json",
                                 "sess-fuzz-1",
                                 (const unsigned char *)response_message_json,
                                 strlen(response_message_json), max_chunk);
    }
    free(response_message_json);
    free(tool_json);
    free(initialize_json);
    free(payload_hex);
  }

  handler->destroy(handler);
  cai_tool_registry_destroy(registry);
  cai_error_cleanup(&error);
  return 0;
}
