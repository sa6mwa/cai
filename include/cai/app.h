#ifndef CAI_APP_H
#define CAI_APP_H

#include <cai/config.h>
#include <cai/tools/todo.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Explicit application setting, using the same sections/keys/values as INI.
 * List settings repeat their key. Strings are copied by cai_app_open. */
typedef struct cai_app_setting {
  const char *section;
  const char *key;
  const char *value;
} cai_app_setting;

/** Argument shape accepted by the shared CAI command-line parser. */
typedef enum cai_app_option_type {
  CAI_APP_FLAG = 0,
  CAI_APP_STRING = 1,
  CAI_APP_INTEGER = 2,
  CAI_APP_OPTIONAL_STRING = 3,
  CAI_APP_COUNTER = 4
} cai_app_option_type;

/** Immutable help entry. long_flag includes --; short_flag is zero if absent.
 * negatable advertises a corresponding --no-NAME spelling. */
typedef struct cai_app_option {
  const char *long_flag;
  char short_flag;
  cai_app_option_type type;
  const char *argument;
  const char *description;
  int repeatable;
  int negatable;
} cai_app_option;

/** Initialize before use. NULL paths select CAI's standard XDG locations.
 * settings supplies explicit overrides for the complete CAI INI surface,
 * including providers, models, instructions, tools, MCP and storage. Order:
 * CAI defaults < INI/environment < settings < forwarded arguments.
 * [env] retains its normal config_env_has_precedence policy.
 * The initialized record selects the same defaults as the cai executable. */
typedef struct cai_app_config {
  const char *program_name;
  const char *banner;
  const char *config_file;
  const char *config_directory;
  int no_config;
  const cai_app_setting *settings;
  size_t setting_count;
  /** Optional already-initialized, thread-safe logger. Borrowed until close;
   * CAI neither closes nor reconfigures it and creates no default log files.
   * Its output must be compatible with terminal ownership during a run. */
  struct pslog_logger *logger;
  /** Optional native tool registration hook, using the same contract as
   * cai_agent_runtime_config.register_agent_tools. Called on the run thread
   * whenever an agent runtime opens, after built-in tools (including messaging)
   * are registered and before workers start. Use cai_agent_register_* here.
   * The agent is borrowed only for this call; context and registered handler
   * contexts remain host-owned and must outlive each run. Handlers execute on
   * worker threads and must not call an owning-thread-only scripting VM.
   * Errors abort runtime startup. Not inherited by subagents and rejected by
   * isolated review. Help and utility invocations do not call this hook. */
  int (*register_agent_tools)(void *context, cai_agent *agent,
                              cai_error *error);
  void *register_agent_tools_context;
} cai_app_config;

typedef struct cai_app cai_app;
/** Synchronous terminal application receiver. Use on the host's main thread.
 * One run owns the process terminal, environment, cwd, locale and signals;
 * hosts must serialize other use of those resources. They are restored before
 * return, including error paths. Repeated runs are supported; concurrent or
 * recursive runs are rejected. Borrowed loggers may receive calls from native
 * workers and must be thread-safe. */
struct cai_app {
  /** argv contains only CAI options (exclude the program name). Empty is valid.
   * Returns CAI status for invocation/restoration errors. On CAI_OK, exit_code
   * receives the application's CLI status: 0 success, 1 work/runtime failure,
   * 2 invalid options/configuration. CLI diagnostics use the configured logger.
   * Arguments are borrowed only during run. This method never calls exit. */
  int (*run)(cai_app *self, int argc, const char *const *argv, int *exit_code,
             cai_error *error);
  size_t (*option_count)(const cai_app *self);
  const cai_app_option *(*option_at)(const cai_app *self, size_t index);
  /** Write default help with the configured program_name to a borrowed sink.
   * No configuration files, logger, banner, auth or terminal are initialized.
   */
  int (*write_help)(const cai_app *self, cai_sink *sink, cai_error *error);
  void (*close)(cai_app *self);
  void *impl;
};

/** Seed CAI defaults, automatic XDG paths and no host overrides. */
void cai_app_config_init(cai_app_config *config);
/** Copy configuration and validate settings without reading files or running
 * the application. Logger remains borrowed. On failure *out is NULL. */
int cai_app_open(const cai_app_config *config, cai_app **out, cai_error *error);
int cai_app_run(cai_app *self, int argc, const char *const *argv,
                int *exit_code, cai_error *error);
size_t cai_app_option_count(const cai_app *self);
const cai_app_option *cai_app_option_at(const cai_app *self, size_t index);
int cai_app_write_help(const cai_app *self, cai_sink *sink, cai_error *error);
/** Close an idle application and copied settings. NULL is accepted. */
void cai_app_close(cai_app *self);

/** Independent shared kanban access for HTTP hosts and other embedders.
 * This opens the same encrypted Pouch store as CAI without running a terminal
 * application or changing environment/cwd. NULL paths use normal CAI defaults.
 * The logger is borrowed through the final receiver close; if absent, a logger
 * is created from LOG_ with warn as its default level. */
typedef struct cai_app_kanban_config {
  /** Absolute CAI state directory containing pouch.key; NULL uses XDG. */
  const char *state_directory;
  /** Same lockd endpoint as sessions; NULL selects state_directory/pouch. */
  const char *lockd;
  /** Optional client PEM for remote lockd mutual TLS. */
  const char *lockd_client_pem;
  /** Initial selection, or NULL for "default". Not shared with other receivers.
   */
  const char *default_board;
  /** Maximum returned cards/boards; zero selects 100. */
  size_t max_result_items;
  /** Borrowed native logger, valid through the final receiver close. */
  struct pslog_logger *logger;
} cai_app_kanban_config;
void cai_app_kanban_config_init(cai_app_kanban_config *config);
/** Returns an owned core receiver with Pouch-backed transaction callbacks.
 * Its retained references own the store; the caller closes the receiver. */
int cai_app_kanban_open(const cai_app_kanban_config *config, cai_todo **out,
                        cai_error *error);

#ifdef __cplusplus
}
#endif
#endif
