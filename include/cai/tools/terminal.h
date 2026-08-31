/** @file cai/tools/terminal.h
 *  One-slot asynchronous PTY terminal tools for CAI agent mode.
 */
#ifndef CAI_TOOLS_TERMINAL_H
#define CAI_TOOLS_TERMINAL_H

#include <cai/cai.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Stable model-facing name for starting the one managed terminal command. */
#define CAI_TERMINAL_EXEC_TOOL_NAME "exec_command"
/** Stable model-facing name for interacting with that same command. */
#define CAI_TERMINAL_WRITE_TOOL_NAME "write_stdin"

/** A managed terminal command began. */
#define CAI_TERMINAL_EVENT_COMMAND_STARTED 1
/** New PTY output is available. */
#define CAI_TERMINAL_EVENT_OUTPUT 2
/** A requested wait ended while the command remains active. */
#define CAI_TERMINAL_EVENT_WAITING 3
/** The command exited and CAI drained its PTY. */
#define CAI_TERMINAL_EVENT_COMMAND_COMPLETED 4
/** CAI requested and observed terminal-command cancellation. */
#define CAI_TERMINAL_EVENT_COMMAND_CANCELLED 5

/** Borrowed terminal lifecycle fact emitted on the agent tool-dispatch thread.
 */
typedef struct cai_terminal_event {
  /** CAI_TERMINAL_EVENT_* discriminator. */
  int type;
  /** Stable one-slot terminal session identifier. */
  const char *terminal_id;
  /** Monotonic command identifier within terminal_id. */
  unsigned long long command_id;
  /** Original command text; borrowed for the callback duration. */
  const char *command;
  /** Resolved working directory; borrowed for the callback duration. */
  const char *workdir;
  /** New PTY output bytes; borrowed and not NUL-terminated. */
  const char *output;
  /** Number of bytes in output. */
  size_t output_length;
  /** Non-zero when exit_code is supplied on a final event. */
  int has_exit_code;
  /** Observed command exit status when has_exit_code is non-zero. */
  long long exit_code;
  /** Non-zero when signal is supplied on a final event. */
  int has_signal;
  /** Observed terminating signal when has_signal is non-zero. */
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

/**
 * Observe a terminal lifecycle event on the tool-dispatch thread. Every event
 * pointer is borrowed for the callback duration. Return non-OK to fail the
 * invoking tool call.
 */
typedef int (*cai_terminal_event_fn)(void *context,
                                     const cai_terminal_event *event,
                                     cai_error *error);

/**
 * Host authorization decision for a requested terminal command. Return CAI_OK
 * to allow it; return a CAI error after setting error to deny it. command,
 * workspace, and workdir are borrowed for the callback duration.
 */
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
  /**
   * Optional host authorization callback, invoked before every command. The
   * callback/context remain borrowed for the registered tools' lifetime.
   */
  cai_terminal_policy_fn policy;
  /** Context passed to policy. */
  void *policy_context;
  /**
   * Optional lifecycle observer; never called on the PTY reader thread. The
   * callback/context remain borrowed for the registered tools' lifetime.
   */
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

/**
 * Register the coupled one-slot exec_command and write_stdin tools. The
 * registry takes ownership of internal terminal state; it copies path/limit
 * settings but borrows callback contexts until the registry is destroyed.
 */
int cai_tool_registry_register_terminal_tools(
    cai_tool_registry *registry, const cai_terminal_tool_config *config,
    cai_error *error);
/** Register the coupled one-slot tools on an agent with the same lifetime
 * rules. */
int cai_agent_register_terminal_tools(cai_agent *agent,
                                      const cai_terminal_tool_config *config,
                                      cai_error *error);

#ifdef __cplusplus
}
#endif

#endif
