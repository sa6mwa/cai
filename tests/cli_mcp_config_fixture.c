/* Observe the production CLI's public MCP configuration at client creation.
 * The real client still performs every request against the HTTP fixture. */
#include "../cli/mcp.h"
#include <stdio.h>
#include <stdlib.h>

int __real_cai_mcp_streamable_http_client_open(
    const cai_mcp_streamable_http_client_config *, cai_mcp_client **,
    cai_error *);
int __wrap_cai_mcp_streamable_http_client_open(
    const cai_mcp_streamable_http_client_config *, cai_mcp_client **,
    cai_error *);

int __wrap_cai_mcp_streamable_http_client_open(
    const cai_mcp_streamable_http_client_config *config, cai_mcp_client **out,
    cai_error *error) {
  const char *path = getenv("CAI_TEST_MCP_CONFIG_TRACE");
  FILE *file;
  if (path != NULL) {
    file = fopen(path, "a");
    if (file == NULL)
      abort();
    if (fprintf(file, "%s timeout=%ld discovery=%ld\n", config->url,
                config->timeout_ms, config->discovery_timeout_ms) < 0 ||
        fclose(file) != 0)
      abort();
  }
  return __real_cai_mcp_streamable_http_client_open(config, out, error);
}
