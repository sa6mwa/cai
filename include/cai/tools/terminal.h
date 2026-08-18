/** @file cai/tools/terminal.h
 *  One-slot asynchronous PTY terminal tools for CAI agent mode.
 */
#ifndef CAI_TOOLS_TERMINAL_H
#define CAI_TOOLS_TERMINAL_H

#include <cai/cai.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAI_TERMINAL_EXEC_TOOL_NAME "exec_command"
#define CAI_TERMINAL_WRITE_TOOL_NAME "write_stdin"

/** Host decision for a requested terminal command. */
typedef int (*cai_terminal_policy_fn)(void *context, const char *command,
                                      const char *workspace,
                                      const char *workdir, int tty,
                                      cai_error *error);

/** Configuration for CAI's one-slot PTY terminal tool pair. */
typedef struct cai_terminal_tool_config {
  /** Required workspace root. Command working directories stay below it. */
  const char *root_path;
  /** Initial working directory below root; NULL selects root. */
  const char *default_workdir;
  /** Shell used for command execution; NULL selects /bin/sh. */
  const char *shell_path;
  /** Default initial/poll wait in milliseconds; zero selects 10000. */
  long default_yield_time_ms;
  /** Maximum accepted initial/poll wait; zero selects 30000. */
  long max_yield_time_ms;
  /** Maximum retained terminal output per command; zero selects 3 MiB. */
  size_t output_max_bytes;
  /** Optional host authorization callback, invoked before every command. */
  cai_terminal_policy_fn policy;
  /** Context passed to policy. */
  void *policy_context;
} cai_terminal_tool_config;

/** Register the coupled one-slot exec_command and write_stdin tools. */
int cai_tool_registry_register_terminal_tools(
    cai_tool_registry *registry, const cai_terminal_tool_config *config,
    cai_error *error);
/** Register the coupled one-slot tools on an agent. */
int cai_agent_register_terminal_tools(cai_agent *agent,
                                      const cai_terminal_tool_config *config,
                                      cai_error *error);

#ifdef __cplusplus
}
#endif

#endif
