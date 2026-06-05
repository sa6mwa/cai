#ifndef CAI_TOOLS_EXEC_H
#define CAI_TOOLS_EXEC_H

#include <cai/cai.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Default registered name for the sandboxed command execution tool. */
#define CAI_EXEC_DEFAULT_TOOL_NAME "exec_command"

/** Configuration for the opt-in `exec_command` tool preset. */
typedef struct cai_exec_tool_config {
  /** Optional override for the model-facing tool name. */
  const char *name;
  /** Optional override for the model-facing tool description. */
  const char *description;
  /** Required sandbox root path; commands cannot operate outside it. */
  const char *root_path;
  /** Optional initial working directory inside `root_path`. */
  const char *default_workdir;
  /** Optional shell path used to run commands; defaults to a POSIX shell. */
  const char *shell_path;
  /** Optional bubblewrap executable path; defaults to `bwrap`. */
  const char *bwrap_path;
  /** Optional Darwin sandbox-exec path; defaults to `sandbox-exec`. */
  const char *sandbox_exec_path;
  /** Non-zero to allow network access inside the sandbox. */
  int allow_network;
  /** Non-zero to allow PTY output mode when requested by tool arguments. */
  int allow_pty;
  /** Non-zero to allow login-shell execution when requested. */
  int allow_login_shell;
  /** Non-zero to apply configured Linux cgroup v2 limits. */
  int enable_cgroup_limits;
  /** Default command timeout in milliseconds; zero uses 120 seconds. */
  long timeout_ms;
  /** Maximum accepted per-call timeout in milliseconds; zero uses 3 hours. */
  long max_timeout_ms;
  /** Optional cgroup pids.max limit; zero leaves it unset. */
  long long pids_max;
  /** Optional cgroup memory.max limit in bytes; zero leaves it unset. */
  long long memory_max_bytes;
  /** Optional cgroup parent directory for per-command cgroups. */
  const char *cgroup_parent_path;
  /** In-memory bytes retained before command output spills to disk. */
  size_t output_memory_limit;
  /** Maximum total command-output bytes retained for one command result.
   * Combined `output` is the authoritative merged view and `stdout`/`stderr`
   * mirror the subset of bytes that fit within that same capture budget. */
  size_t output_max_bytes;
  /** Optional directory for output spool spill files. */
  const char *output_spool_dir;
} cai_exec_tool_config;

/** Register the sandboxed `exec_command` preset on a tool registry. */
int cai_tool_registry_register_exec_tool(cai_tool_registry *registry,
                                         const cai_exec_tool_config *config,
                                         cai_error *error);
/** Register the sandboxed `exec_command` preset on an agent. */
int cai_agent_register_exec_tool(cai_agent *agent,
                                 const cai_exec_tool_config *config,
                                 cai_error *error);

#ifdef __cplusplus
}
#endif

#endif
