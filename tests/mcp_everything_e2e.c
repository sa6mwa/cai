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

static int e2e_has_required(const cai_mcp_client_schema *schema,
                            const char *name) {
  size_t i;

  if (schema == NULL) {
    return 0;
  }
  for (i = 0U; i < schema->required_count; i++) {
    if (strcmp(schema->required[i], name) == 0) {
      return 1;
    }
  }
  return 0;
}

static const cai_mcp_client_tool *e2e_find_tool(cai_mcp_client *client,
                                                const char *name) {
  const cai_mcp_client_tool *tool;
  size_t i;

  for (i = 0U; i < cai_mcp_client_tool_count(client); i++) {
    tool = cai_mcp_client_tool_at(client, i);
    if (tool != NULL && tool->name != NULL && strcmp(tool->name, name) == 0) {
      return tool;
    }
  }
  return NULL;
}

static const cai_mcp_client_resource *e2e_find_resource(cai_mcp_client *client,
                                                        const char *uri) {
  const cai_mcp_client_resource *resource;
  size_t i;

  for (i = 0U; i < cai_mcp_client_resource_count(client); i++) {
    resource = cai_mcp_client_resource_at(client, i);
    if (resource != NULL && resource->uri != NULL &&
        strcmp(resource->uri, uri) == 0) {
      return resource;
    }
  }
  return NULL;
}

static const cai_mcp_client_resource_template *
e2e_find_resource_template(cai_mcp_client *client, const char *uri_template) {
  const cai_mcp_client_resource_template *resource_template;
  size_t i;

  for (i = 0U; i < cai_mcp_client_resource_template_count(client); i++) {
    resource_template = cai_mcp_client_resource_template_at(client, i);
    if (resource_template != NULL && resource_template->uri_template != NULL &&
        strcmp(resource_template->uri_template, uri_template) == 0) {
      return resource_template;
    }
  }
  return NULL;
}

static const cai_mcp_client_prompt *e2e_find_prompt(cai_mcp_client *client,
                                                    const char *name) {
  const cai_mcp_client_prompt *prompt;
  size_t i;

  for (i = 0U; i < cai_mcp_client_prompt_count(client); i++) {
    prompt = cai_mcp_client_prompt_at(client, i);
    if (prompt != NULL && prompt->name != NULL &&
        strcmp(prompt->name, name) == 0) {
      return prompt;
    }
  }
  return NULL;
}

static int e2e_prompt_has_argument(const cai_mcp_client_prompt *prompt,
                                   const char *name, int required) {
  size_t i;

  if (prompt == NULL) {
    return 0;
  }
  for (i = 0U; i < prompt->argument_count; i++) {
    if (strcmp(prompt->arguments[i].name, name) == 0 &&
        prompt->arguments[i].has_required &&
        prompt->arguments[i].required == required) {
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

static int e2e_call_tool(cai_mcp_client *client, const char *name,
                         const char *args_json, cai_sink *sink,
                         e2e_writer *writer, cai_error *error) {
  lonejson_spooled args;
  int rc;

  e2e_writer_reset(writer);
  if (e2e_spool(&args, args_json) != 0) {
    return CAI_ERR_INVALID;
  }
  rc = cai_mcp_client_call_tool(client, name, &args, sink, error);
  args.cleanup(&args);
  return rc;
}

static int e2e_get_prompt(cai_mcp_client *client, const char *name,
                          const char *args_json, cai_sink *sink,
                          e2e_writer *writer, cai_error *error) {
  lonejson_spooled args;
  lonejson_spooled *args_ptr;
  int rc;

  e2e_writer_reset(writer);
  args_ptr = NULL;
  if (args_json != NULL) {
    if (e2e_spool(&args, args_json) != 0) {
      return CAI_ERR_INVALID;
    }
    args_ptr = &args;
  }
  rc = cai_mcp_client_get_prompt(client, name, args_ptr, sink, error);
  if (args_ptr != NULL) {
    args.cleanup(&args);
  }
  return rc;
}

static int e2e_complete(cai_mcp_client *client, const char *ref_type,
                        const char *ref_value, const char *argument_name,
                        const char *argument_value,
                        const char *context_arguments_json, cai_sink *sink,
                        e2e_writer *writer, cai_error *error) {
  lonejson_spooled context_arguments;
  lonejson_spooled *context_ptr;
  int rc;

  e2e_writer_reset(writer);
  context_ptr = NULL;
  if (context_arguments_json != NULL) {
    if (e2e_spool(&context_arguments, context_arguments_json) != 0) {
      return CAI_ERR_INVALID;
    }
    context_ptr = &context_arguments;
  }
  rc = cai_mcp_client_complete(client, ref_type, ref_value, argument_name,
                               argument_value, context_ptr, sink, error);
  if (context_ptr != NULL) {
    context_arguments.cleanup(&context_arguments);
  }
  return rc;
}

int main(int argc, char **argv) {
  cai_mcp_streamable_http_client_config config;
  cai_mcp_client *client;
  const cai_mcp_client_tool *tool;
  const cai_mcp_client_resource *resource;
  const cai_mcp_client_resource_template *resource_template;
  const cai_mcp_client_prompt *prompt;
  cai_sink_callbacks callbacks;
  cai_sink *sink;
  e2e_writer writer;
  cai_error error;
  int rc;

  if (argc != 2) {
    fprintf(stderr, "usage: %s MCP_STREAMABLE_HTTP_URL\n", argv[0]);
    return 2;
  }

  client = NULL;
  sink = NULL;
  memset(&writer, 0, sizeof(writer));
  memset(&callbacks, 0, sizeof(callbacks));
  cai_error_init(&error);

  cai_mcp_streamable_http_client_config_init(&config);
  config.url = argv[1];
  config.client_name = "cai-mcp-everything-e2e";
  config.timeout_ms = 5000L;
  rc = cai_mcp_streamable_http_client_open(&config, &client, &error);
  if (rc != CAI_OK) {
    return e2e_error("failed to open MCP client", &error);
  }
  rc = cai_mcp_client_ping(client, &error);
  if (rc != CAI_OK) {
    cai_mcp_client_destroy(client);
    return e2e_error("failed to ping MCP Everything", &error);
  }

  rc = cai_mcp_client_refresh_tools(client, &error);
  if (rc != CAI_OK) {
    cai_mcp_client_destroy(client);
    return e2e_error("failed to refresh MCP tools", &error);
  }
  if (cai_mcp_client_tool_count(client) < 12U) {
    cai_mcp_client_destroy(client);
    return e2e_error("MCP Everything advertised too few tools", NULL);
  }
  tool = e2e_find_tool(client, "echo");
  if (tool == NULL || strcmp(tool->title, "Echo Tool") != 0 ||
      !e2e_has_property(tool->input_schema, "message") ||
      !e2e_has_required(tool->input_schema, "message") ||
      tool->task_support != CAI_MCP_CLIENT_TOOL_TASK_SUPPORT_FORBIDDEN) {
    cai_mcp_client_destroy(client);
    return e2e_error("echo tool metadata was incomplete", NULL);
  }
  tool = e2e_find_tool(client, "get-structured-content");
  if (tool == NULL || tool->output_schema == NULL ||
      !e2e_has_property(tool->output_schema, "temperature") ||
      !e2e_has_required(tool->output_schema, "humidity")) {
    cai_mcp_client_destroy(client);
    return e2e_error("structured-content tool schema was incomplete", NULL);
  }
  if (e2e_find_tool(client, "get-sum") == NULL ||
      e2e_find_tool(client, "get-resource-reference") == NULL ||
      e2e_find_tool(client, "trigger-long-running-operation") == NULL) {
    cai_mcp_client_destroy(client);
    return e2e_error("expected MCP Everything tools were missing", NULL);
  }

  rc = cai_mcp_client_refresh_resources(client, &error);
  if (rc != CAI_OK) {
    cai_mcp_client_destroy(client);
    return e2e_error("failed to refresh MCP resources", &error);
  }
  if (cai_mcp_client_resource_count(client) < 7U) {
    cai_mcp_client_destroy(client);
    return e2e_error("MCP Everything advertised too few resources", NULL);
  }
  resource = e2e_find_resource(
      client, "demo://resource/static/document/architecture.md");
  if (resource == NULL || strcmp(resource->name, "architecture.md") != 0 ||
      strcmp(resource->mime_type, "text/markdown") != 0) {
    cai_mcp_client_destroy(client);
    return e2e_error("architecture resource metadata was incomplete", NULL);
  }
  if (e2e_find_resource(
          client, "demo://resource/static/document/features.md") == NULL) {
    cai_mcp_client_destroy(client);
    return e2e_error("features resource was missing", NULL);
  }

  rc = cai_mcp_client_refresh_resource_templates(client, &error);
  if (rc != CAI_OK) {
    cai_mcp_client_destroy(client);
    return e2e_error("failed to refresh MCP resource templates", &error);
  }
  if (cai_mcp_client_resource_template_count(client) < 2U) {
    cai_mcp_client_destroy(client);
    return e2e_error("MCP Everything advertised too few resource templates",
                     NULL);
  }
  resource_template = e2e_find_resource_template(
      client, "demo://resource/dynamic/text/{resourceId}");
  if (resource_template == NULL ||
      strcmp(resource_template->name, "Dynamic Text Resource") != 0 ||
      strcmp(resource_template->mime_type, "text/plain") != 0) {
    cai_mcp_client_destroy(client);
    return e2e_error("dynamic text resource template was incomplete", NULL);
  }
  if (e2e_find_resource_template(
          client, "demo://resource/dynamic/blob/{resourceId}") == NULL) {
    cai_mcp_client_destroy(client);
    return e2e_error("dynamic blob resource template was missing", NULL);
  }

  rc = cai_mcp_client_refresh_prompts(client, &error);
  if (rc != CAI_OK) {
    cai_mcp_client_destroy(client);
    return e2e_error("failed to refresh MCP prompts", &error);
  }
  if (cai_mcp_client_prompt_count(client) < 4U) {
    cai_mcp_client_destroy(client);
    return e2e_error("MCP Everything advertised too few prompts", NULL);
  }
  prompt = e2e_find_prompt(client, "args-prompt");
  if (prompt == NULL || strcmp(prompt->title, "Arguments Prompt") != 0 ||
      !e2e_prompt_has_argument(prompt, "city", 1) ||
      !e2e_prompt_has_argument(prompt, "state", 0)) {
    cai_mcp_client_destroy(client);
    return e2e_error("args-prompt metadata was incomplete", NULL);
  }
  prompt = e2e_find_prompt(client, "completable-prompt");
  if (prompt == NULL || !e2e_prompt_has_argument(prompt, "department", 1) ||
      !e2e_prompt_has_argument(prompt, "name", 1) ||
      e2e_find_prompt(client, "resource-prompt") == NULL) {
    cai_mcp_client_destroy(client);
    return e2e_error("expected MCP Everything prompts were missing", NULL);
  }

  callbacks.write = e2e_write;
  callbacks.close = e2e_close;
  callbacks.context = &writer;
  rc = cai_sink_from_callbacks(&callbacks, &sink, &error);
  if (rc != CAI_OK) {
    cai_mcp_client_destroy(client);
    return e2e_error("failed to create output sink", &error);
  }

  e2e_writer_reset(&writer);
  rc =
      cai_mcp_client_send_request(client, "unknown/method", NULL, sink, &error);
  if (e2e_expect_error("Everything unknown method", rc, &error, CAI_ERR_SERVER,
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
    return e2e_error("Everything unknown method wrote result output", NULL);
  }
  cai_error_cleanup(&error);
  cai_error_init(&error);
  rc = e2e_call_tool(client, "echo", "{}", sink, &writer, &error);
  if (rc != CAI_OK ||
      e2e_expect_contains("Everything invalid echo args", writer.data,
                          "\"isError\":true") != 0 ||
      e2e_expect_contains("Everything invalid echo args", writer.data,
                          "Invalid arguments for tool echo") != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    return rc == CAI_OK ? 1
                        : e2e_error("failed invalid echo args call", &error);
  }
  cai_error_cleanup(&error);
  cai_error_init(&error);
  e2e_writer_reset(&writer);
  rc = cai_mcp_client_read_resource(
      client, "demo://resource/static/document/missing.md", sink, &error);
  if (e2e_expect_error("Everything missing resource", rc, &error,
                       CAI_ERR_SERVER, "not found") != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    cai_error_cleanup(&error);
    return 1;
  }
  cai_error_cleanup(&error);
  cai_error_init(&error);
  rc = e2e_get_prompt(client, "missing-prompt", NULL, sink, &writer, &error);
  if (e2e_expect_error("Everything missing prompt", rc, &error, CAI_ERR_SERVER,
                       "not found") != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    cai_error_cleanup(&error);
    return 1;
  }
  cai_error_cleanup(&error);
  cai_error_init(&error);
  rc = e2e_complete(client, "ref/prompt", "completable-prompt", "name", "",
                    "{\"department\":\"NoSuchDepartment\"}", sink, &writer,
                    &error);
  if (rc != CAI_OK ||
      e2e_expect_contains("unknown department completion", writer.data,
                          "\"completion\"") != 0 ||
      e2e_expect_contains("unknown department completion", writer.data,
                          "\"values\":[]") != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    return rc == CAI_OK
               ? 1
               : e2e_error("failed unknown department completion", &error);
  }

  rc = e2e_call_tool(client, "echo",
                     "{\"message\":\"cai-mcp-everything-e2e-ok\"}", sink,
                     &writer, &error);
  if (rc != CAI_OK || e2e_expect_contains("echo output", writer.data,
                                          "cai-mcp-everything-e2e-ok") != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    return rc == CAI_OK ? 1 : e2e_error("failed to call echo tool", &error);
  }
  rc = e2e_call_tool(client, "get-sum", "{\"a\":7,\"b\":35}", sink, &writer,
                     &error);
  if (rc != CAI_OK || e2e_expect_contains("sum output", writer.data,
                                          "The sum of 7 and 35 is 42") != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    return rc == CAI_OK ? 1 : e2e_error("failed to call get-sum", &error);
  }
  rc = e2e_call_tool(client, "get-structured-content",
                     "{\"location\":\"Chicago\"}", sink, &writer, &error);
  if (rc != CAI_OK ||
      e2e_expect_contains("structured output", writer.data,
                          "\"structuredContent\"") != 0 ||
      e2e_expect_contains("structured output", writer.data,
                          "\"temperature\"") != 0 ||
      e2e_expect_contains("structured output", writer.data,
                          "Light rain / drizzle") != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    return rc == CAI_OK ? 1
                        : e2e_error("failed to call structured tool", &error);
  }
  e2e_writer_reset(&writer);
  rc = cai_mcp_client_read_resource(
      client, "demo://resource/static/document/architecture.md", sink, &error);
  if (rc != CAI_OK ||
      e2e_expect_contains("static resource output", writer.data,
                          "\"contents\"") != 0 ||
      e2e_expect_contains("static resource output", writer.data,
                          "demo://resource/static/document/architecture.md") !=
          0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    return rc == CAI_OK ? 1
                        : e2e_error("failed to read static resource", &error);
  }
  e2e_writer_reset(&writer);
  rc = cai_mcp_client_read_resource(client, "demo://resource/dynamic/text/7",
                                    sink, &error);
  if (rc != CAI_OK ||
      e2e_expect_contains("dynamic resource output", writer.data,
                          "Resource 7:") != 0 ||
      e2e_expect_contains("dynamic resource output", writer.data,
                          "\"mimeType\":\"text/plain\"") != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    return rc == CAI_OK ? 1
                        : e2e_error("failed to read dynamic resource", &error);
  }
  rc = e2e_get_prompt(client, "simple-prompt", NULL, sink, &writer, &error);
  if (rc != CAI_OK ||
      e2e_expect_contains("simple prompt output", writer.data,
                          "simple prompt without arguments") != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    return rc == CAI_OK ? 1 : e2e_error("failed to get simple prompt", &error);
  }
  rc = e2e_get_prompt(client, "args-prompt",
                      "{\"city\":\"Gothenburg\",\"state\":\"Vastra Gotaland\"}",
                      sink, &writer, &error);
  if (rc != CAI_OK ||
      e2e_expect_contains("args prompt output", writer.data, "Gothenburg") !=
          0 ||
      e2e_expect_contains("args prompt output", writer.data,
                          "Vastra Gotaland") != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    return rc == CAI_OK ? 1 : e2e_error("failed to get args prompt", &error);
  }
  rc = e2e_get_prompt(client, "resource-prompt",
                      "{\"resourceType\":\"Text\",\"resourceId\":\"3\"}", sink,
                      &writer, &error);
  if (rc != CAI_OK ||
      e2e_expect_contains("resource prompt output", writer.data,
                          "\"type\":\"resource\"") != 0 ||
      e2e_expect_contains("resource prompt output", writer.data,
                          "demo://resource/dynamic/text/3") != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    return rc == CAI_OK ? 1
                        : e2e_error("failed to get resource prompt", &error);
  }
  rc = e2e_complete(client, "ref/prompt", "completable-prompt", "department",
                    "", NULL, sink, &writer, &error);
  if (rc != CAI_OK ||
      e2e_expect_contains("department completion", writer.data,
                          "Engineering") != 0 ||
      e2e_expect_contains("department completion", writer.data,
                          "\"completion\"") != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    return rc == CAI_OK ? 1
                        : e2e_error("failed to complete department", &error);
  }
  rc = e2e_complete(client, "ref/prompt", "completable-prompt", "name", "",
                    "{\"department\":\"Engineering\"}", sink, &writer, &error);
  if (rc != CAI_OK ||
      e2e_expect_contains("name completion", writer.data, "Alice") != 0 ||
      e2e_expect_contains("name completion", writer.data, "\"total\":3") != 0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    return rc == CAI_OK ? 1 : e2e_error("failed to complete name", &error);
  }
  rc = e2e_complete(client, "ref/resource",
                    "demo://resource/dynamic/text/{resourceId}", "resourceId",
                    "", NULL, sink, &writer, &error);
  if (rc != CAI_OK ||
      e2e_expect_contains("resource completion", writer.data,
                          "\"completion\"") != 0 ||
      e2e_expect_contains("resource completion", writer.data, "\"values\"") !=
          0) {
    cai_sink_close(sink);
    cai_mcp_client_destroy(client);
    free(writer.data);
    return rc == CAI_OK ? 1
                        : e2e_error("failed to complete resource id", &error);
  }

  cai_sink_close(sink);
  cai_mcp_client_destroy(client);
  printf("MCP Everything e2e matrix passed at %s\n", argv[1]);
  free(writer.data);
  cai_error_cleanup(&error);
  return 0;
}
