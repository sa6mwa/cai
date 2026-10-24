#define _POSIX_C_SOURCE 200809L
#include "mcp.h"
#include "pouch_stream.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct server_tool_storage {
  cai_mcp_client_schema schema;
  cai_tool_raw_input raw;
  char **strings;
  size_t count;
  const char **properties, **required;
  int failed;
} server_tool_storage;

struct cai_cli_mcp_server {
  cai_mcp_client client;
  cai_mcp_client *remote;
  const char *name;
  char *prefix;
  cai_mcp_client_tool *tools;
  server_tool_storage *storage;
  size_t count;
  int enabled, attempted;
  cai_error error;
  pslog_logger *logger;
};
static void server_warn(cai_cli_mcp_server *server) {
  if (server->logger != NULL)
    server->logger->warnf(server->logger, "MCP server tools unavailable",
                          "server=%s error=%s", server->name,
                          server->error.message != NULL ? server->error.message
                                                        : "discovery failed");
}

static void server_tools_clear(cai_cli_mcp_server *server) {
  size_t i, j;
  for (i = 0U; server->storage != NULL && i < server->count; i++) {
    server_tool_storage *owned = &server->storage[i];
    for (j = 0U; j < owned->count; j++)
      free(owned->strings[j]);
    free(owned->strings);
    free(owned->properties);
    free(owned->required);
  }
  free(server->storage);
  free(server->tools);
  server->storage = NULL;
  server->tools = NULL;
  server->count = 0U;
}
static const char *tool_text(server_tool_storage *owned, const char *text) {
  char **strings;
  char *copy;
  if (text == NULL || owned->failed)
    return NULL;
  strings = realloc(owned->strings, (owned->count + 1U) * sizeof(*strings));
  if (strings == NULL) {
    owned->failed = 1;
    return NULL;
  }
  owned->strings = strings;
  copy = strdup(text);
  if (copy == NULL) {
    owned->failed = 1;
    return NULL;
  }
  owned->strings[owned->count++] = copy;
  return copy;
}

/* Tool registration borrows a complete input contract. Keep that contract
 * alive across transport cache invalidation and subsequent session switches. */
static int server_tool_copy(cai_cli_mcp_server *server, size_t index,
                            const cai_mcp_client_tool *remote,
                            cai_error *error) {
  cai_mcp_client_tool *tool = &server->tools[index];
  server_tool_storage *owned = &server->storage[index];
  const cai_mcp_client_schema *schema = remote->input_schema;
  size_t i, length = strlen(server->prefix) + strlen(remote->name) + 1U;
  char *name = malloc(length);
  if (name == NULL || schema == NULL) {
    free(name);
    return cai_cli_error(error,
                         schema == NULL ? CAI_ERR_PROTOCOL : CAI_ERR_NOMEM,
                         "cannot copy MCP tool input contract");
  }
  snprintf(name, length, "%s%s", server->prefix, remote->name);
  tool->name = tool_text(owned, name);
  free(name);
  tool->title = tool_text(owned, remote->title);
  tool->description = tool_text(owned, remote->description);
  tool->input_schema_json = tool_text(owned, remote->input_schema_json);
  owned->schema = *schema;
  owned->schema.schema_uri = tool_text(owned, schema->schema_uri);
  owned->schema.type = tool_text(owned, schema->type);
  owned->properties =
      calloc(schema->property_count + 1U, sizeof(*owned->properties));
  owned->required =
      calloc(schema->required_count + 1U, sizeof(*owned->required));
  if (owned->properties == NULL || owned->required == NULL)
    owned->failed = 1;
  for (i = 0U; !owned->failed && i < schema->property_count; i++)
    owned->properties[i] = tool_text(owned, schema->properties[i]);
  for (i = 0U; !owned->failed && i < schema->required_count; i++)
    owned->required[i] = tool_text(owned, schema->required[i]);
  owned->schema.properties = owned->properties;
  owned->schema.required = owned->required;
  tool->input_schema = &owned->schema;
  if (remote->raw_input != NULL) {
    owned->raw = *remote->raw_input;
    owned->raw.field = tool_text(owned, remote->raw_input->field);
    owned->raw.format.type = tool_text(owned, remote->raw_input->format.type);
    owned->raw.format.syntax =
        tool_text(owned, remote->raw_input->format.syntax);
    owned->raw.format.definition =
        tool_text(owned, remote->raw_input->format.definition);
    tool->raw_input = &owned->raw;
  }
  return owned->failed
             ? cai_cli_error(error, CAI_ERR_NOMEM, "copy MCP tool catalog")
             : CAI_OK;
}
static int tool_name_valid(const char *name) {
  const unsigned char *p = (const unsigned char *)name;
  if (p == NULL || *p == '\0')
    return 0;
  for (; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
      return 0;
  return 1;
}
static int server_refresh(cai_mcp_client *client, cai_error *error) {
  cai_cli_mcp_server *server = client->impl;
  size_t i, prefix_length;
  int rc;
  (void)error;
  if (server->attempted)
    return CAI_OK;
  server->attempted = 1;
  rc = cai_mcp_client_refresh_tools(server->remote, &server->error);
  prefix_length = strlen(server->prefix);
  if (rc == CAI_OK) {
    server->count = cai_mcp_client_tool_count(server->remote);
    if (server->count != 0U) {
      server->tools = calloc(server->count, sizeof(*server->tools));
      server->storage = calloc(server->count, sizeof(*server->storage));
      if (server->tools == NULL || server->storage == NULL) {
        rc = cai_cli_error(&server->error, CAI_ERR_NOMEM,
                           "allocate MCP tool catalog");
      }
    }
    for (i = 0U; rc == CAI_OK && i < server->count; i++) {
      const cai_mcp_client_tool *remote =
          cai_mcp_client_tool_at(server->remote, i);
      size_t j;
      if (remote == NULL || !tool_name_valid(remote->name) ||
          prefix_length + strlen(remote->name) > 64U) {
        rc = cai_cli_error(&server->error, CAI_ERR_INVALID,
                           "MCP tool name must use letters, digits, _ or - "
                           "and fit 64 bytes including its server prefix");
        break;
      }
      for (j = 0U; j < i; j++)
        if (strcmp(server->tools[j].name + prefix_length, remote->name) == 0) {
          rc = cai_cli_error(&server->error, CAI_ERR_INVALID,
                             "MCP server advertised a duplicate tool name");
          break;
        }
      if (rc != CAI_OK)
        break;
      rc = server_tool_copy(server, i, remote, &server->error);
    }
  }
  if (rc != CAI_OK) {
    server_tools_clear(server);
    server_warn(server);
  } else if (server->logger != NULL)
    server->logger->debugf(server->logger, "cai.mcp.discovered",
                           "server=%s tools=%lu", server->name,
                           (unsigned long)server->count);
  /* An unavailable optional server contributes no tools; other servers and
   * the agent remain usable. Its diagnostic is retained for /tools. */
  return CAI_OK;
}
static size_t server_count(const cai_mcp_client *client) {
  return ((const cai_cli_mcp_server *)client->impl)->count;
}
static const cai_mcp_client_tool *server_at(const cai_mcp_client *client,
                                            size_t index) {
  const cai_cli_mcp_server *server = client->impl;
  return index < server->count ? &server->tools[index] : NULL;
}
static const char *remote_name(cai_cli_mcp_server *server, const char *name) {
  size_t length = strlen(server->prefix);
  return name != NULL && strncmp(name, server->prefix, length) == 0
             ? name + length
             : NULL;
}
static int server_call(cai_mcp_client *client, const char *name,
                       struct lonejson_spooled *arguments, cai_sink *output,
                       cai_error *error) {
  cai_cli_mcp_server *server = client->impl;
  const char *remote = remote_name(server, name);
  if (remote == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID, "invalid MCP tool alias");
  return cai_mcp_client_call_tool(server->remote, remote, arguments, output,
                                  error);
}
static int server_call_raw(cai_mcp_client *client, const char *name,
                           const cai_tool_raw_input *mapping,
                           struct lonejson_spooled *input, cai_sink *output,
                           cai_error *error) {
  cai_cli_mcp_server *server = client->impl;
  const char *remote = remote_name(server, name);
  if (remote == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID, "invalid MCP tool alias");
  return cai_mcp_client_call_tool_raw(server->remote, remote, mapping, input,
                                      output, error);
}
void cai_cli_mcp_close(cai_cli_mcp *mcp) {
  size_t i;
  for (i = 0U; i < mcp->count; i++) {
    cai_cli_mcp_server *server = &mcp->servers[i];
    server_tools_clear(server);
    cai_mcp_client_destroy(server->remote);
    free(server->prefix);
    cai_error_cleanup(&server->error);
  }
  free(mcp->servers);
  free(mcp->clients);
  memset(mcp, 0, sizeof(*mcp));
}
int cai_cli_mcp_open(cai_cli_mcp *mcp, const cai_config *config, int enabled,
                     pslog_logger *logger, cai_error *error) {
  size_t i, count = cai_config_section_count(config);
  memset(mcp, 0, sizeof(*mcp));
  if (count == 0U)
    return CAI_OK;
  mcp->servers = calloc(count, sizeof(*mcp->servers));
  mcp->clients = calloc(count, sizeof(*mcp->clients));
  if (mcp->servers == NULL || mcp->clients == NULL) {
    cai_cli_mcp_close(mcp);
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate MCP servers");
  }
  for (i = 0U; i < count; i++) {
    const char *section = cai_config_section_at(config, i);
    const char *endpoint, *key_env, *key;
    long long timeout, discovery;
    cai_mcp_streamable_http_client_config client_config;
    cai_cli_mcp_server *server;
    size_t length;
    int rc, prefix_length;
    if (strncmp(section, "mcp.", 4U) != 0)
      continue;
    server = &mcp->servers[mcp->count++];
    server->name = section + 4U;
    server->logger = logger;
    cai_error_init(&server->error);
    server->enabled =
        enabled && cai_config_get_bool(config, section, "enabled", 1);
    if (!server->enabled)
      continue;
    endpoint = cai_config_get(config, section, "endpoint");
    timeout = cai_config_get_integer(config, section, "timeout_ms", 30000);
    discovery =
        cai_config_get_integer(config, section, "discovery_timeout_ms", 3000);
    if (endpoint == NULL || endpoint[0] == '\0' || timeout <= 0 ||
        discovery <= 0 || timeout > LONG_MAX || discovery > LONG_MAX) {
      char message[256];
      snprintf(message, sizeof(message),
               "[%s] requires an endpoint and positive HTTP/discovery timeouts",
               section);
      cai_cli_mcp_close(mcp);
      return cai_cli_error(error, CAI_ERR_INVALID, message);
    }
    /* Both names permit underscores. Include the server-name length so that
     * a_b/c and a/b_c cannot produce the same model-facing alias. */
    prefix_length = snprintf(NULL, 0, "mcp_%lu_%s_",
                             (unsigned long)strlen(server->name), server->name);
    if (prefix_length < 0) {
      cai_cli_mcp_close(mcp);
      return cai_cli_error(error, CAI_ERR_INVALID, "format MCP tool prefix");
    }
    length = (size_t)prefix_length + 1U;
    server->prefix = malloc(length);
    if (server->prefix == NULL) {
      cai_cli_mcp_close(mcp);
      return cai_cli_error(error, CAI_ERR_NOMEM, "allocate MCP tool prefix");
    }
    snprintf(server->prefix, length, "mcp_%lu_%s_",
             (unsigned long)strlen(server->name), server->name);
    cai_mcp_streamable_http_client_config_init(&client_config);
    client_config.url = endpoint;
    client_config.timeout_ms = (long)timeout;
    client_config.discovery_timeout_ms = (long)discovery;
    client_config.logger = logger;
    key_env = cai_config_get(config, section, "api_key_env");
    key = key_env != NULL && key_env[0] != '\0' ? getenv(key_env) : NULL;
    if (key_env != NULL && key_env[0] != '\0' &&
        (key == NULL || key[0] == '\0')) {
      cai_cli_error(&server->error, CAI_ERR_INVALID,
                    "MCP API-key environment variable is unset");
      server_warn(server);
      server->attempted = 1;
      continue;
    }
    client_config.api_key = key;
    rc = cai_mcp_streamable_http_client_open(&client_config, &server->remote,
                                             &server->error);
    if (rc != CAI_OK) {
      server_warn(server);
      server->attempted = 1;
      continue;
    }
    server->client.impl = server;
    server->client.initialize = server_refresh;
    server->client.refresh_tools = server_refresh;
    server->client.tool_count = server_count;
    server->client.tool_at = server_at;
    server->client.call_tool = server_call;
    server->client.call_tool_raw = server_call_raw;
    mcp->clients[mcp->client_count++] = &server->client;
  }
  return CAI_OK;
}

typedef struct inventory_output {
  const cai_cli_mcp *mcp;
  cai_cli_tools_write write;
  void *context;
  int detailed;
  int names_written;
  size_t counts[3];
} inventory_output;
static int literal(inventory_output *out, const char *text, cai_error *error) {
  return out->write(out->context, text, strlen(text)) == 0
             ? CAI_OK
             : cai_cli_error(error, CAI_ERR_TRANSPORT, "write tool inventory");
}
static int cell(inventory_output *out, const char *text, cai_error *error) {
  char buffer[256];
  size_t used = 0U;
  int rc = CAI_OK;
  const unsigned char *p = (const unsigned char *)(text != NULL ? text : "");
  for (; rc == CAI_OK && *p; p++) {
    if (used + 3U >= sizeof(buffer)) {
      rc =
          out->write(out->context, buffer, used) == 0
              ? CAI_OK
              : cai_cli_error(error, CAI_ERR_TRANSPORT, "write tool inventory");
      used = 0U;
    }
    if (strchr("\\|`*~[]<>#", *p) != NULL)
      buffer[used++] = '\\';
    buffer[used++] = *p < 32U || *p == 127U ? ' ' : (char)*p;
  }
  if (rc == CAI_OK && used != 0U && out->write(out->context, buffer, used) != 0)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "write tool inventory");
  return rc;
}
/* Identifiers come from the registered tools/named-section grammar. Code
 * spans preserve underscores instead of rendering them as Markdown emphasis. */
static int identifier(inventory_output *out, const char *text,
                      cai_error *error) {
  int rc = literal(out, "`", error);
  if (rc == CAI_OK)
    rc = literal(out, text, error);
  return rc == CAI_OK ? literal(out, "`", error) : rc;
}
static int inventory_tool(void *context, const cai_agent_tool_info *tool,
                          cai_error *error) {
  inventory_output *out = context;
  const char *source = tool->source == CAI_AGENT_TOOL_HOSTED ? "hosted"
                       : tool->source == CAI_AGENT_TOOL_MCP  ? "MCP"
                                                             : "local";
  int rc;
  out->counts[tool->source]++;
  if (!out->detailed)
    return CAI_OK;
  rc = literal(out, "| ", error);
  if (rc == CAI_OK)
    rc = identifier(out, tool->name, error);
  if (rc == CAI_OK)
    rc = literal(out, " | ", error);
  if (rc == CAI_OK)
    rc = cell(out, source, error);
  if (rc == CAI_OK && tool->source == CAI_AGENT_TOOL_MCP &&
      tool->mcp_client_index < out->mcp->client_count) {
    const cai_cli_mcp_server *server =
        out->mcp->clients[tool->mcp_client_index]->impl;
    rc = literal(out, ": ", error);
    if (rc == CAI_OK)
      rc = identifier(out, server->name, error);
  }
  if (rc == CAI_OK)
    rc = literal(out, " | ", error);
  if (rc == CAI_OK)
    rc = cell(out, tool->input_format, error);
  if (rc == CAI_OK)
    rc = literal(out, " | ", error);
  if (rc == CAI_OK)
    rc = cell(out, tool->description, error);
  return rc == CAI_OK ? literal(out, " |\n", error) : rc;
}
static int inventory_names(void *context, const cai_agent_tool_info *tool,
                           cai_error *error) {
  inventory_output *out = context;
  int rc = CAI_OK;
  if (tool->source == CAI_AGENT_TOOL_MCP)
    return CAI_OK;
  if (out->names_written++)
    rc = literal(out, ", ", error);
  return rc == CAI_OK ? identifier(out, tool->name, error) : rc;
}
int cai_cli_tools_markdown(const cai_cli_mcp *mcp, cai_agent_runtime *runtime,
                           int detailed, cai_cli_tools_write write,
                           void *context, cai_error *error) {
  inventory_output out;
  char row[512];
  size_t i, ready = 0U, failed = 0U, disabled = 0U;
  int rc;
  memset(&out, 0, sizeof(out));
  out.mcp = mcp;
  out.write = write;
  out.context = context;
  out.detailed = detailed;
  rc = CAI_OK;
  if (detailed)
    rc = literal(&out,
                 "## Tools\n\n| Tool | Source | Input | Description "
                 "|\n| --- | --- | --- | --- |\n",
                 error);
  if (rc == CAI_OK)
    rc = cai_agent_runtime_visit_tools(runtime, inventory_tool, &out, error);
  for (i = 0U; i < mcp->count; i++) {
    if (!mcp->servers[i].enabled)
      disabled++;
    else if (mcp->servers[i].error.code != CAI_OK)
      failed++;
    else
      ready++;
  }
  if (rc == CAI_OK && !detailed) {
    snprintf(row, sizeof(row),
             "| Local tools | %lu |\n"
             "| Hosted tools | %lu |\n| MCP tools | %lu |\n"
             "| MCP servers | %lu ready, %lu unavailable, %lu disabled |\n\n"
             "Use `/tools` for the full inventory.\n",
             (unsigned long)out.counts[0], (unsigned long)out.counts[1],
             (unsigned long)out.counts[2], (unsigned long)ready,
             (unsigned long)failed, (unsigned long)disabled);
    rc = literal(&out, row, error);
    if (rc == CAI_OK)
      rc = literal(&out, "\nAgent tools: ", error);
    if (rc == CAI_OK)
      rc = cai_agent_runtime_visit_tools(runtime, inventory_names, &out, error);
    if (rc == CAI_OK)
      rc = literal(&out, ".\n", error);
    if (rc == CAI_OK && mcp->count != 0U)
      rc = literal(&out, "\nMCP servers: ", error);
    for (i = 0U; rc == CAI_OK && i < mcp->count; i++) {
      const cai_cli_mcp_server *server = &mcp->servers[i];
      if (i != 0U)
        rc = literal(&out, ", ", error);
      if (rc == CAI_OK)
        rc = identifier(&out, server->name, error);
      if (!server->enabled)
        snprintf(row, sizeof(row), " (disabled)");
      else if (server->error.code != CAI_OK)
        snprintf(row, sizeof(row), " (unavailable)");
      else
        snprintf(row, sizeof(row), " (%lu tools)",
                 (unsigned long)server->count);
      if (rc == CAI_OK)
        rc = literal(&out, row, error);
    }
    if (rc == CAI_OK && mcp->count != 0U)
      rc = literal(&out, ".\n", error);
  }
  if (rc == CAI_OK && detailed) {
    rc = literal(&out,
                 "\n### MCP servers\n\n| Server | Status | Tools |\n| --- | "
                 "--- | --- |\n",
                 error);
    for (i = 0U; rc == CAI_OK && i < mcp->count; i++) {
      const cai_cli_mcp_server *server = &mcp->servers[i];
      const char *status = !server->enabled ? "disabled"
                           : server->error.code != CAI_OK
                               ? server->error.message
                               : "ready";
      rc = literal(&out, "| ", error);
      if (rc == CAI_OK)
        rc = identifier(&out, server->name, error);
      if (rc == CAI_OK)
        rc = literal(&out, " | ", error);
      if (rc == CAI_OK)
        rc = cell(&out, status, error);
      snprintf(row, sizeof(row), " | %lu |\n", (unsigned long)server->count);
      if (rc == CAI_OK)
        rc = literal(&out, row, error);
    }
    if (rc == CAI_OK && mcp->count == 0U)
      rc = literal(&out, "| — | No MCP servers configured | 0 |\n", error);
  }
  return rc;
}
