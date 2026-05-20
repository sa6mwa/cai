#ifndef CAI_TOOLS_EXEC_H
#define CAI_TOOLS_EXEC_H

#include <cai/cai.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAI_EXEC_DEFAULT_TOOL_NAME "exec_command"
#define CAI_EXEC_SANDBOX_REQUIRED 0
#define CAI_EXEC_SANDBOX_BEST_EFFORT 1
#define CAI_EXEC_SANDBOX_DISABLED 2

typedef struct cai_exec_tool_config {
  const char *name;
  const char *description;
  const char *root_path;
  const char *default_workdir;
  const char *shell_path;
  const char *bwrap_path;
  int sandbox_mode;
  int allow_network;
  int allow_pty;
  int allow_login_shell;
  long timeout_ms;
  long max_timeout_ms;
  size_t output_memory_limit;
  size_t output_max_bytes;
  const char *output_spool_dir;
} cai_exec_tool_config;

int cai_tool_registry_register_exec_tool(cai_tool_registry *registry,
                                         const cai_exec_tool_config *config,
                                         cai_error *error);
int cai_agent_register_exec_tool(cai_agent *agent,
                                 const cai_exec_tool_config *config,
                                 cai_error *error);

#ifdef __cplusplus
}
#endif

#endif
