#include <cai/cai.h>
#include <cai/mcp.h>

#include "cai_lj.h"

#include <lonejson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct smoke_writer {
  char *data;
  size_t length;
  size_t capacity;
} smoke_writer;

static int smoke_write(void *context, const void *bytes, size_t count,
                       cai_error *error) {
  smoke_writer *writer;
  char *grown;
  size_t next_capacity;

  (void)error;
  writer = (smoke_writer *)context;
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

static void smoke_close(void *context) { (void)context; }

static int smoke_error(const char *message, cai_error *error) {
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
  const cai_mcp_client_resource *resource;
  const cai_mcp_client_prompt *prompt;
  cai_sink_callbacks callbacks;
  cai_sink *sink;
  smoke_writer writer;
  lonejson_spooled args;
  lonejson_error json_error;
  cai_error error;
  const char *args_json;
  size_t i;
  int found_echo;
  int found_resource;
  int found_prompt;
  int rc;

  if (argc != 2) {
    fprintf(stderr, "usage: %s MCP_STREAMABLE_HTTP_URL\n", argv[0]);
    return 2;
  }

  client = NULL;
  sink = NULL;
  found_echo = 0;
  found_resource = 0;
  found_prompt = 0;
  memset(&writer, 0, sizeof(writer));
  memset(&callbacks, 0, sizeof(callbacks));
  cai_error_init(&error);

  cai_mcp_streamable_http_client_config_init(&config);
  config.url = argv[1];
  config.client_name = "cai-mcp-client-smoke";
  config.timeout_ms = 5000L;
  rc = cai_mcp_streamable_http_client_open(&config, &client, &error);
  if (rc != CAI_OK) {
    return smoke_error("failed to open MCP client", &error);
  }
  rc = client->refresh_tools(client, &error);
  if (rc != CAI_OK) {
    cai_mcp_client_destroy(client);
    return smoke_error("failed to refresh MCP tools", &error);
  }
  for (i = 0U; i < client->tool_count(client); i++) {
    tool = client->tool_at(client, i);
    if (tool != NULL && tool->name != NULL && strcmp(tool->name, "echo") == 0) {
      found_echo = 1;
      if (tool->input_schema_json == NULL ||
          strstr(tool->input_schema_json, "\"message\"") == NULL) {
        cai_mcp_client_destroy(client);
        return smoke_error("echo tool schema did not include message", NULL);
      }
      break;
    }
  }
  if (!found_echo) {
    cai_mcp_client_destroy(client);
    return smoke_error("MCP server did not advertise echo tool", NULL);
  }
  rc = client->refresh_resources(client, &error);
  if (rc != CAI_OK) {
    cai_mcp_client_destroy(client);
    return smoke_error("failed to refresh MCP resources", &error);
  }
  for (i = 0U; i < client->resource_count(client); i++) {
    resource = client->resource_at(client, i);
    if (resource != NULL && resource->uri != NULL &&
        strcmp(resource->uri,
               "demo://resource/static/document/architecture.md") == 0) {
      found_resource = 1;
      if (resource->mime_type == NULL ||
          strcmp(resource->mime_type, "text/markdown") != 0) {
        cai_mcp_client_destroy(client);
        return smoke_error("architecture resource MIME type was unexpected",
                           NULL);
      }
      break;
    }
  }
  if (!found_resource) {
    cai_mcp_client_destroy(client);
    return smoke_error("MCP server did not advertise architecture resource",
                       NULL);
  }
  rc = client->refresh_prompts(client, &error);
  if (rc != CAI_OK) {
    cai_mcp_client_destroy(client);
    return smoke_error("failed to refresh MCP prompts", &error);
  }
  for (i = 0U; i < client->prompt_count(client); i++) {
    prompt = client->prompt_at(client, i);
    if (prompt != NULL && prompt->name != NULL &&
        strcmp(prompt->name, "args-prompt") == 0) {
      found_prompt = 1;
      if (prompt->arguments_json == NULL ||
          strstr(prompt->arguments_json, "\"city\"") == NULL) {
        cai_mcp_client_destroy(client);
        return smoke_error("args-prompt metadata did not include city", NULL);
      }
      break;
    }
  }
  if (!found_prompt) {
    cai_mcp_client_destroy(client);
    return smoke_error("MCP server did not advertise args-prompt", NULL);
  }

  callbacks.write = smoke_write;
  callbacks.close = smoke_close;
  callbacks.context = &writer;
  rc = cai_sink_from_callbacks(&callbacks, &sink, &error);
  if (rc != CAI_OK) {
    cai_mcp_client_destroy(client);
    return smoke_error("failed to create output sink", &error);
  }
  CAI_LJ->spooled_init(CAI_LJ, &args);
  lonejson_error_init(&json_error);
  args_json = "{\"message\":\"cai-mcp-client-smoke-ok\"}";
  if (args.append(&args, args_json, strlen(args_json), &json_error) !=
      LONEJSON_STATUS_OK) {
    args.cleanup(&args);
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    fprintf(stderr, "failed to build tool arguments: %s\n", json_error.message);
    return 1;
  }
  rc = client->call_tool(client, "echo", &args, sink, &error);
  args.cleanup(&args);
  if (rc != CAI_OK) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    return smoke_error("failed to call echo tool", &error);
  }
  if (writer.data == NULL ||
      strstr(writer.data, "cai-mcp-client-smoke-ok") == NULL) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    return smoke_error("echo tool output did not include smoke marker", NULL);
  }
  writer.length = 0U;
  writer.data[0] = '\0';
  rc = client->read_resource(
      client, "demo://resource/static/document/architecture.md", sink, &error);
  if (rc != CAI_OK) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    return smoke_error("failed to read architecture resource", &error);
  }
  if (writer.data == NULL ||
      strstr(writer.data, "demo://resource/static/document/architecture.md") ==
          NULL ||
      strstr(writer.data, "\"contents\"") == NULL) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    return smoke_error("architecture resource output was unexpected", NULL);
  }
  writer.length = 0U;
  writer.data[0] = '\0';
  CAI_LJ->spooled_init(CAI_LJ, &args);
  lonejson_error_init(&json_error);
  args_json = "{\"city\":\"Gothenburg\",\"state\":\"Vastra Gotaland\"}";
  if (args.append(&args, args_json, strlen(args_json), &json_error) !=
      LONEJSON_STATUS_OK) {
    args.cleanup(&args);
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    fprintf(stderr, "failed to build prompt arguments: %s\n",
            json_error.message);
    return 1;
  }
  rc = client->get_prompt(client, "args-prompt", &args, sink, &error);
  args.cleanup(&args);
  cai_sink_close(sink);
  if (rc != CAI_OK) {
    cai_mcp_client_destroy(client);
    free(writer.data);
    return smoke_error("failed to get args-prompt", &error);
  }
  if (writer.data == NULL || strstr(writer.data, "Gothenburg") == NULL ||
      strstr(writer.data, "Vastra Gotaland") == NULL) {
    cai_mcp_client_destroy(client);
    free(writer.data);
    return smoke_error("args-prompt output was unexpected", NULL);
  }
  writer.length = 0U;
  writer.data[0] = '\0';
  callbacks.context = &writer;
  rc = cai_sink_from_callbacks(&callbacks, &sink, &error);
  if (rc != CAI_OK) {
    cai_mcp_client_destroy(client);
    free(writer.data);
    return smoke_error("failed to recreate output sink", &error);
  }
  rc = client->complete(client, "ref/prompt", "completable-prompt",
                        "department", "", NULL, sink, &error);
  cai_sink_close(sink);
  if (rc != CAI_OK) {
    cai_mcp_client_destroy(client);
    free(writer.data);
    return smoke_error("failed to complete prompt argument", &error);
  }
  if (writer.data == NULL || strstr(writer.data, "Engineering") == NULL ||
      strstr(writer.data, "\"completion\"") == NULL) {
    cai_mcp_client_destroy(client);
    free(writer.data);
    return smoke_error("completion output was unexpected", NULL);
  }
  cai_mcp_client_destroy(client);
  printf("MCP client smoke passed at %s\n", argv[1]);
  free(writer.data);
  cai_error_cleanup(&error);
  return 0;
}
