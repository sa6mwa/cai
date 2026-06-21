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

static int e2e_error(const char *message, cai_error *error) {
  fprintf(stderr, "%s", message);
  if (error != NULL && error->message != NULL) {
    fprintf(stderr, ": %s", error->message);
  }
  if (error != NULL && error->detail != NULL) {
    fprintf(stderr, " (%s)", error->detail);
  }
  fprintf(stderr, "\n");
  return 1;
}

int main(int argc, char **argv) {
  cai_mcp_streamable_http_client_config config;
  cai_mcp_client *client;
  const cai_mcp_client_tool *tool;
  cai_sink_callbacks callbacks;
  cai_sink *sink;
  e2e_writer writer;
  lonejson_spooled args;
  lonejson_error json_error;
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

  rc = cai_mcp_client_refresh_tools(client, &error);
  if (rc != CAI_OK) {
    cai_mcp_client_destroy(client);
    return e2e_error("failed to refresh MCP tools", &error);
  }
  for (i = 0U; i < cai_mcp_client_tool_count(client); i++) {
    tool = cai_mcp_client_tool_at(client, i);
    if (tool != NULL && tool->name != NULL &&
        strcmp(tool->name, "echo_message") == 0) {
      size_t j;

      found_echo = 1;
      for (j = 0U;
           tool->input_schema != NULL && j < tool->input_schema->property_count;
           j++) {
        if (strcmp(tool->input_schema->properties[j], "message") == 0) {
          break;
        }
      }
      if (tool->input_schema == NULL ||
          j == tool->input_schema->property_count) {
        cai_mcp_client_destroy(client);
        return e2e_error("echo_message schema did not include message", NULL);
      }
      break;
    }
  }
  if (!found_echo) {
    cai_mcp_client_destroy(client);
    return e2e_error("MCP server did not advertise echo_message", NULL);
  }

  rc = cai_mcp_client_ping(client, &error);
  if (rc != CAI_OK) {
    cai_mcp_client_destroy(client);
    return e2e_error("failed to ping MCP server", &error);
  }

  callbacks.write = e2e_write;
  callbacks.close = e2e_close;
  callbacks.context = &writer;
  rc = cai_sink_from_callbacks(&callbacks, &sink, &error);
  if (rc != CAI_OK) {
    cai_mcp_client_destroy(client);
    return e2e_error("failed to create output sink", &error);
  }
  CAI_LJ->spooled_init(CAI_LJ, &args);
  lonejson_error_init(&json_error);
  if (args.append(&args, "{\"message\":\"cai-mcp-client-e2e-ok\"}",
                  strlen("{\"message\":\"cai-mcp-client-e2e-ok\"}"),
                  &json_error) != LONEJSON_STATUS_OK) {
    args.cleanup(&args);
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    fprintf(stderr, "failed to build tool arguments: %s\n", json_error.message);
    return 1;
  }
  rc = cai_mcp_client_call_tool(client, "echo_message", &args, sink, &error);
  args.cleanup(&args);
  if (rc != CAI_OK) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    return e2e_error("failed to call echo_message", &error);
  }
  cai_sink_close(sink);
  if (writer.data == NULL ||
      strstr(writer.data, "cai-mcp-client-e2e-ok") == NULL) {
    cai_mcp_client_destroy(client);
    free(writer.data);
    return e2e_error("echo_message output did not include e2e marker", NULL);
  }

  cai_mcp_client_destroy(client);
  free(writer.data);
  cai_error_cleanup(&error);
  printf("MCP client e2e passed at %s\n", argv[1]);
  return 0;
}
