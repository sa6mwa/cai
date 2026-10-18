#ifndef CAI_CLI_MCP_H
#define CAI_CLI_MCP_H
#include <cai/config.h>
#include <cai/mcp.h>
#include <pslog.h>

typedef struct cai_cli_mcp_server cai_cli_mcp_server;
typedef struct cai_cli_mcp {
  cai_cli_mcp_server *servers;
  size_t count;
  cai_mcp_client **clients;
  size_t client_count;
} cai_cli_mcp;

/* Config/logger outlive this collection, which outlives every borrowing
 * runtime. Discovery happens once on first registration, with a whole-server
 * budget. */
int cai_cli_mcp_open(cai_cli_mcp *mcp, const cai_config *config, int enabled,
                     pslog_logger *logger, cai_error *error);
void cai_cli_mcp_close(cai_cli_mcp *mcp);
typedef int (*cai_cli_tools_write)(void *, const char *, size_t);
int cai_cli_tools_markdown(const cai_cli_mcp *mcp, cai_agent_runtime *runtime,
                           int detailed, cai_cli_tools_write write,
                           void *context, cai_error *error);
#endif
