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

#define CAI_TERMINAL_EVENT_COMMAND_STARTED 1
#define CAI_TERMINAL_EVENT_OUTPUT 2
#define CAI_TERMINAL_EVENT_WAITING 3
#define CAI_TERMINAL_EVENT_COMMAND_COMPLETED 4
#define CAI_TERMINAL_EVENT_COMMAND_CANCELLED 5

/** Borrowed terminal lifecycle fact emitted on the agent tool-dispatch thread.
 */
typedef struct cai_terminal_event {
  int type;
  const char *terminal_id;
  unsigned long long command_id;
  const char *command;
  const char *workdir;
  const char *output;
  size_t output_length;
  int has_exit_code;
  long long exit_code;
  int has_signal;
  long long signal;
  /** Elapsed wall-clock time for the supervised shell command. */
  unsigned long long duration_ms;
  /** Total PTY bytes observed for this command, including omitted bytes. */
  unsigned long long total_output_bytes;
  /** Non-zero when CAI omitted terminal bytes from its bounded retention. */
  int output_truncated;
  /**
   * Non-zero when descendants may have escaped the supervised shell. CAI
   * never claims arbitrary detached processes have exited.
   */
  int detached_processes_possible;
} cai_terminal_event;

typedef int (*cai_terminal_event_fn)(void *context,
                                     const cai_terminal_event *event,
                                     cai_error *error);

/** Host decision for a requested terminal command. */
typedef int (*cai_terminal_policy_fn)(void *context, const char *command,
                                      const char *workspace,
                                      const char *workdir, int tty,
                                      cai_error *error);

/**
 * Configuration for CAI's one-slot PTY terminal tool pair. Child commands
 * receive a fixed minimal environment: HOME is the configured workspace and
 * PATH, LANG, LC_ALL, TERM, TMPDIR, and LESSHISTFILE are CAI-controlled. Host
 * process environment variables, including credentials, are never inherited.
 */
typedef struct cai_terminal_tool_config {
  /** Required workspace root. Command working directories stay below it. */
  const char *root_path;
  /** Initial working directory below root; NULL selects root. */
  const char *default_workdir;
  /** Shell used for command execution; NULL selects /bin/sh. */
  const char *shell_path;
  /** Default initial exec_command wait in milliseconds; zero selects 10000. */
  long default_yield_time_ms;
  /** Maximum initial exec_command wait; zero selects 30000. */
  long max_yield_time_ms;
  /** Maximum retained terminal output per command; zero selects 3 MiB. */
  size_t output_max_bytes;
  /** Optional host authorization callback, invoked before every command. */
  cai_terminal_policy_fn policy;
  /** Context passed to policy. */
  void *policy_context;
  /** Optional lifecycle observer; never called on the PTY reader thread. */
  cai_terminal_event_fn event_callback;
  /** Context passed to event_callback. */
  void *event_context;
  /**
   * Default wait after non-empty write_stdin input; zero selects 250.
   * The requested/default wait is capped by max_write_yield_time_ms.
   */
  long default_write_yield_time_ms;
  /** Maximum non-empty write_stdin wait; zero selects 30000. */
  long max_write_yield_time_ms;
  /**
   * Default wait for an empty write_stdin poll; zero selects 5000.
   * A non-terminating empty poll always waits at least 5000 milliseconds
   * unless output or completion arrives first.
   */
  long default_poll_yield_time_ms;
  /**
   * Maximum empty write_stdin poll wait; zero selects 300000 (five minutes).
   * Values below 5000 select the 5000 millisecond minimum.
   */
  long max_poll_yield_time_ms;
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
