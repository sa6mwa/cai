#include <cai/cai.h>
#include <cai/mcp.h>

#include "cai_lj.h"

#include <lonejson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct e2e_writer {
  char *data;
  size_t length;
  size_t capacity;
} e2e_writer;

static int e2e_write(void *context, const void *bytes, size_t count,
                     cai_error *error) {
  e2e_writer *writer;
  char *grown;
  size_t next_capacity;

  (void)error;
  writer = (e2e_writer *)context;
  if (writer == NULL || bytes == NULL) {
    return CAI_ERR_INVALID;
  }
  if (writer->length + count + 1U > writer->capacity) {
    next_capacity = writer->capacity == 0U ? 256U : writer->capacity * 2U;
    while (next_capacity < writer->length + count + 1U) {
      next_capacity *= 2U;
    }
    grown = (char *)realloc(writer->data, next_capacity);
    if (grown == NULL) {
      return CAI_ERR_NOMEM;
    }
    writer->data = grown;
    writer->capacity = next_capacity;
  }
  memcpy(writer->data + writer->length, bytes, count);
  writer->length += count;
  writer->data[writer->length] = '\0';
  return CAI_OK;
}

static void e2e_close(void *context) { (void)context; }

static void e2e_writer_reset(e2e_writer *writer) {
  if (writer != NULL) {
    writer->length = 0U;
    if (writer->data != NULL) {
      writer->data[0] = '\0';
    }
  }
}

static int e2e_error(const char *message, cai_error *error) {
  fprintf(stderr, "%s", message);
  if (error != NULL && error->message != NULL) {
    fprintf(stderr, ": %s", error->message);
  }
  if (error != NULL && error->detail != NULL) {
    fprintf(stderr, " (%s)", error->detail);
  }
  fprintf(stderr, "\n");
  if (error != NULL) {
    cai_error_cleanup(error);
  }
  return 1;
}

static int e2e_spool(lonejson_spooled *spool, const char *json) {
  lonejson_error json_error;

  CAI_LJ->spooled_init(CAI_LJ, spool);
  lonejson_error_init(&json_error);
  if (spool->append(spool, json, strlen(json), &json_error) !=
      LONEJSON_STATUS_OK) {
    fprintf(stderr, "failed to build JSON payload: %s\n", json_error.message);
    spool->cleanup(spool);
    return 1;
  }
  return 0;
}

static int e2e_has_property(const cai_mcp_client_schema *schema,
                            const char *name) {
  size_t i;

  if (schema == NULL) {
    return 0;
  }
  for (i = 0U; i < schema->property_count; i++) {
    if (strcmp(schema->properties[i], name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int e2e_expect_contains(const char *name, const char *text,
                               const char *needle) {
  if (text == NULL || strstr(text, needle) == NULL) {
    fprintf(stderr, "%s missing expected fragment: %s\n", name, needle);
    return 1;
  }
  return 0;
}

static int e2e_expect_error(const char *name, int rc, const cai_error *error,
                            int expected_code, const char *expected_message) {
  if (rc != expected_code) {
    fprintf(stderr, "%s returned %d, expected %d\n", name, rc, expected_code);
    return 1;
  }
  if (expected_message != NULL &&
      (error == NULL || error->message == NULL ||
       strstr(error->message, expected_message) == NULL)) {
    fprintf(stderr, "%s error missing expected fragment: %s\n", name,
            expected_message);
    return 1;
  }
  return 0;
}

static int e2e_expect_empty_output(const char *name, const e2e_writer *writer) {
  if (writer != NULL && writer->data != NULL && writer->data[0] != '\0') {
    fprintf(stderr, "%s unexpectedly wrote output: %s\n", name, writer->data);
    return 1;
  }
  return 0;
}

int main(int argc, char **argv) {
  cai_mcp_streamable_http_client_config config;
  cai_mcp_client *client;
  const cai_mcp_client_tool *tool;
  cai_sink_callbacks callbacks;
  cai_sink *sink;
  e2e_writer writer;
  lonejson_spooled args;
  cai_error error;
  size_t i;
  int found_echo;
  int rc;

  if (argc != 2) {
    fprintf(stderr, "usage: %s MCP_STREAMABLE_HTTP_URL\n", argv[0]);
    return 2;
  }

  client = NULL;
  sink = NULL;
  found_echo = 0;
  memset(&writer, 0, sizeof(writer));
  memset(&callbacks, 0, sizeof(callbacks));
  cai_error_init(&error);

  cai_mcp_streamable_http_client_config_init(&config);
  config.url = argv[1];
  config.client_name = "cai-mcp-client-e2e";
  config.timeout_ms = 5000L;
  rc = cai_mcp_streamable_http_client_open(&config, &client, &error);
  if (rc != CAI_OK) {
    return e2e_error("failed to open MCP client", &error);
  }
  rc = cai_mcp_client_initialize(client, &error);
  if (rc != CAI_OK) {
    cai_mcp_client_destroy(client);
    return e2e_error("failed to initialize MCP client", &error);
  }
  rc = cai_mcp_client_ping(client, &error);
  if (rc != CAI_OK) {
    cai_mcp_client_destroy(client);
    return e2e_error("failed to ping MCP server", &error);
  }

  rc = cai_mcp_client_refresh_tools(client, &error);
  if (rc != CAI_OK) {
    cai_mcp_client_destroy(client);
    return e2e_error("failed to refresh MCP tools", &error);
  }
  if (cai_mcp_client_tool_count(client) != 1U) {
    cai_mcp_client_destroy(client);
    return e2e_error("CAI MCP test server tool count changed", NULL);
  }
  for (i = 0U; i < cai_mcp_client_tool_count(client); i++) {
    tool = cai_mcp_client_tool_at(client, i);
    if (tool != NULL && tool->name != NULL &&
        strcmp(tool->name, "echo_message") == 0) {
      found_echo = 1;
      if (strcmp(tool->title, "echo_message") != 0 ||
          strcmp(tool->description,
                 "Echo a message through the MCP test server") != 0 ||
          !e2e_has_property(tool->input_schema, "message")) {
        cai_mcp_client_destroy(client);
        return e2e_error("echo_message metadata was incomplete", NULL);
      }
      break;
    }
  }
  if (!found_echo) {
    cai_mcp_client_destroy(client);
    return e2e_error("MCP server did not advertise echo_message", NULL);
  }

  callbacks.write = e2e_write;
  callbacks.close = e2e_close;
  callbacks.context = &writer;
  rc = cai_sink_from_callbacks(&callbacks, &sink, &error);
  if (rc != CAI_OK) {
    cai_mcp_client_destroy(client);
    return e2e_error("failed to create output sink", &error);
  }
  rc = cai_mcp_client_send_request(client, "", NULL, sink, &error);
  if (e2e_expect_error("empty request method", rc, &error, CAI_ERR_INVALID,
                       "MCP request method is required") != 0 ||
      e2e_expect_empty_output("empty request method", &writer) != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    cai_error_cleanup(&error);
    return 1;
  }
  cai_error_cleanup(&error);
  cai_error_init(&error);
  rc = cai_mcp_client_send_notification(client, "", NULL, &error);
  if (e2e_expect_error("empty notification method", rc, &error, CAI_ERR_INVALID,
                       "MCP notification method is required") != 0 ||
      e2e_expect_empty_output("empty notification method", &writer) != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    cai_error_cleanup(&error);
    return 1;
  }
  cai_error_cleanup(&error);
  cai_error_init(&error);
  rc = cai_mcp_client_call_tool(client, "", NULL, sink, &error);
  if (e2e_expect_error("empty tool name", rc, &error, CAI_ERR_INVALID,
                       "MCP tool name is required") != 0 ||
      e2e_expect_empty_output("empty tool name", &writer) != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    cai_error_cleanup(&error);
    return 1;
  }
  cai_error_cleanup(&error);
  cai_error_init(&error);
  if (e2e_spool(&args, "[]") != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    return 1;
  }
  rc = cai_mcp_client_call_tool(client, "echo_message", &args, sink, &error);
  args.cleanup(&args);
  if (e2e_expect_error("array tool arguments", rc, &error, CAI_ERR_PROTOCOL,
                       "MCP tool arguments must be an object") != 0 ||
      e2e_expect_empty_output("array tool arguments", &writer) != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    cai_error_cleanup(&error);
    return 1;
  }
  cai_error_cleanup(&error);
  cai_error_init(&error);
  rc = cai_mcp_client_read_resource(client, "", sink, &error);
  if (e2e_expect_error("empty resource URI", rc, &error, CAI_ERR_INVALID,
                       "MCP resource URI is required") != 0 ||
      e2e_expect_empty_output("empty resource URI", &writer) != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    cai_error_cleanup(&error);
    return 1;
  }
  cai_error_cleanup(&error);
  cai_error_init(&error);
  rc = cai_mcp_client_get_prompt(client, "", NULL, sink, &error);
  if (e2e_expect_error("empty prompt name", rc, &error, CAI_ERR_INVALID,
                       "MCP prompt name is required") != 0 ||
      e2e_expect_empty_output("empty prompt name", &writer) != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    cai_error_cleanup(&error);
    return 1;
  }
  cai_error_cleanup(&error);
  cai_error_init(&error);
  rc = cai_mcp_client_complete(client, "ref/unknown", "value", "argument", "",
                               NULL, sink, &error);
  if (e2e_expect_error("invalid completion ref type", rc, &error,
                       CAI_ERR_INVALID,
                       "MCP completion reference type is invalid") != 0 ||
      e2e_expect_empty_output("invalid completion ref type", &writer) != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    cai_error_cleanup(&error);
    return 1;
  }
  cai_error_cleanup(&error);
  cai_error_init(&error);
  if (e2e_spool(&args, "[]") != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    return 1;
  }
  rc = cai_mcp_client_complete(client, "ref/prompt", "completable-prompt",
                               "name", "", &args, sink, &error);
  args.cleanup(&args);
  if (e2e_expect_error("array completion context", rc, &error, CAI_ERR_PROTOCOL,
                       "MCP completion context arguments must be an object") !=
          0 ||
      e2e_expect_empty_output("array completion context", &writer) != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    cai_error_cleanup(&error);
    return 1;
  }
  cai_error_cleanup(&error);
  cai_error_init(&error);
  rc = cai_mcp_client_send_request(client, "ping", NULL, sink, &error);
  if (rc != CAI_OK ||
      e2e_expect_contains("generic ping result", writer.data, "{}") != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    return rc == CAI_OK ? 1 : e2e_error("failed to send generic ping", &error);
  }
  e2e_writer_reset(&writer);
  rc =
      cai_mcp_client_send_request(client, "unknown/method", NULL, sink, &error);
  if (e2e_expect_error("unknown method", rc, &error, CAI_ERR_SERVER,
                       "Method not found") != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    cai_error_cleanup(&error);
    return 1;
  }
  if (writer.data != NULL && writer.data[0] != '\0') {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    cai_error_cleanup(&error);
    return e2e_error("unknown method wrote result output", NULL);
  }
  cai_error_cleanup(&error);
  cai_error_init(&error);
  e2e_writer_reset(&writer);
  if (e2e_spool(&args, "{}") != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    return 1;
  }
  rc = cai_mcp_client_send_request(client, "tools/call", &args, sink, &error);
  args.cleanup(&args);
  if (e2e_expect_error("invalid tools/call params", rc, &error, CAI_ERR_SERVER,
                       "Invalid tool call parameters") != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    cai_error_cleanup(&error);
    return 1;
  }
  if (writer.data != NULL && writer.data[0] != '\0') {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    cai_error_cleanup(&error);
    return e2e_error("invalid tools/call wrote result output", NULL);
  }
  cai_error_cleanup(&error);
  cai_error_init(&error);
  e2e_writer_reset(&writer);
  rc = cai_mcp_client_refresh_resources(client, &error);
  if (e2e_expect_error("unsupported resources/list", rc, &error, CAI_ERR_SERVER,
                       "Method not found") != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    cai_error_cleanup(&error);
    return 1;
  }
  cai_error_cleanup(&error);
  cai_error_init(&error);
  e2e_writer_reset(&writer);
  rc = cai_mcp_client_read_resource(client, "demo://missing", sink, &error);
  if (e2e_expect_error("unsupported resources/read", rc, &error, CAI_ERR_SERVER,
                       "Method not found") != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    cai_error_cleanup(&error);
    return 1;
  }
  cai_error_cleanup(&error);
  cai_error_init(&error);
  rc = cai_mcp_client_send_notification(client, "notifications/initialized",
                                        NULL, &error);
  if (rc != CAI_OK) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    return e2e_error("failed to send initialized notification", &error);
  }
  e2e_writer_reset(&writer);
  if (e2e_spool(&args, "{\"message\":\"cai-mcp-client-e2e-ok\"}") != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    return 1;
  }
  rc = cai_mcp_client_call_tool(client, "echo_message", &args, sink, &error);
  args.cleanup(&args);
  if (rc != CAI_OK ||
      e2e_expect_contains("echo_message output", writer.data,
                          "cai-mcp-client-e2e-ok") != 0 ||
      e2e_expect_contains("echo_message output", writer.data,
                          "\"structuredContent\"") != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    return rc == CAI_OK ? 1 : e2e_error("failed to call echo_message", &error);
  }
  e2e_writer_reset(&writer);
  if (e2e_spool(&args, "{}") != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    return 1;
  }
  rc = cai_mcp_client_call_tool(client, "missing_tool", &args, sink, &error);
  args.cleanup(&args);
  if (rc != CAI_OK || e2e_expect_contains("missing tool output", writer.data,
                                          "\"isError\":true") != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    return rc == CAI_OK ? 1 : e2e_error("failed to call missing tool", &error);
  }
  cai_sink_close(sink);

  cai_mcp_client_destroy(client);
  free(writer.data);
  cai_error_cleanup(&error);
  printf("MCP client e2e passed at %s\n", argv[1]);
  return 0;
}
