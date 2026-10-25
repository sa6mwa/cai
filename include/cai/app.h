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

/** Host commands are copied into a bounded FIFO and applied on the run thread.
 * Queueing a prompt starts a new turn; steering targets the active turn.
 * Goal replacement starts pursuit with no token budget. */
typedef enum cai_app_control_type {
  CAI_APP_QUEUE_PROMPT = 1,
  CAI_APP_STEER,
  CAI_APP_CANCEL_TURN,
  CAI_APP_STOP,
  CAI_APP_REPLACE_GOAL,
  CAI_APP_PAUSE_GOAL,
  CAI_APP_RESUME_GOAL,
  CAI_APP_CLEAR_GOAL
} cai_app_control_type;

typedef enum cai_app_outcome {
  CAI_APP_NOT_RUN = 0,
  CAI_APP_RUNNING,
  CAI_APP_COMPLETED,
  CAI_APP_STOPPED,
  CAI_APP_CANCELLED,
  CAI_APP_FAILED
} cai_app_outcome;

/** Owned snapshot. Initialize before first use; cleanup after the last use.
 * Getters replace an initialized snapshot atomically on success. Strings and
 * errors remain valid independently of the app, later queries and later runs.
 * Values reflect the most recent owner-thread safe boundary, not live workers.
 * runtime_ready distinguishes startup/utilities from an open agent runtime. */
typedef struct cai_app_status {
  int running;
  int runtime_ready;
  cai_app_outcome outcome;
  int exit_code;
  cai_agent_run_state state;
  const char *session_id;
  cai_agent_runtime_settings effective;
  cai_agent_runtime_settings pending;
  cai_agent_goal_snapshot goal;
  cai_agent_runtime_metrics metrics;
  /** Most recent run error, including provider detail when supplied. May
   * describe a recovered interactive diagnostic; check outcome/exit_code. */
  cai_error error;
  /** Most recently processed host command; IDs restart at one each run. */
  unsigned long long control_id;
  int control_status;
  cai_error control_error;
} cai_app_status;

/** Initialize an empty owned status before its first query. */
void cai_app_status_init(cai_app_status *status);
/** Release owned status strings/errors and reset it. NULL is accepted. */
void cai_app_status_cleanup(cai_app_status *status);

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

/** Initialize before use. NULL paths derive XDG locations from program_name.
 * settings supplies explicit overrides for the complete CAI INI surface,
 * including providers, models, instructions, tools, MCP and storage. Order:
 * CAI defaults < INI/environment < settings < forwarded arguments.
 * [env] retains its normal config_env_has_precedence policy.
 * Runtime defaults match cai; startup text is empty unless the host sets it. */
#define CAI_APP_DEFAULT_PROGRAM_NAME "cai"
typedef struct cai_app_config {
  /** Application identity: lowercase letter followed by lowercase letters,
   * digits, '_' or '-', up to 32 bytes. NULL selects "cai". This seeds all
   * application defaults; INI files and argv cannot change the identity. */
  const char *program_name;
  /** Optional usage label, e.g. "vectis -a smith --". */
  const char *invocation_name;
  /** NULL derives uppercase(program_name) + '_'. Provider-standard API key
   * variables and pslog's LOG_ remain independent. */
  const char *environment_prefix;
  /** NULL derives program_name. Individual namespaces may be overridden in
   * [storage]; queue_name in [messaging] overrides the complete queue name. */
  const char *namespace_prefix;
  const char *queue_prefix;
  /** Independent root/child defaults. NULL derives the XDG roots from
   * program_name, history/key from state, and clipboard/logs from cache.
   * Explicit INI/environment, settings and argv override these defaults. */
  const char *state_directory;
  const char *cache_directory;
  const char *data_directory;
  const char *history_directory;
  const char *clipboard_directory;
  const char *log_directory;
  const char *pouch_key_file;
  /** Optional raw interactive banner, written verbatim before terminal
   * renderers start. Defaults to NULL; the host supplies all newlines and ANSI
   * escapes. */
  const char *banner;
  /** Optional interactive introduction rendered through libmdf before session
   * replay. Defaults to NULL. Neither startup text is shown by help, utilities,
   * reviews or non-interactive runs. Both strings are copied by open. */
  const char *introduction_markdown;
  /** Independent libmdf introduction margins, in columns. Both default to zero
   * and must be nonnegative, leaving at least three content columns at runtime.
   * These do not change conversation rendering. */
  int introduction_margin_left;
  int introduction_margin_right;
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
  /** Optional owner-thread observers, alongside normal rendering and logging.
   * Events are borrowed only for the call. Return CAI_OK, or an error to abort
   * the run. Observers may call control/get_status, but must not reenter run,
   * close the app, or access the runtime. Contexts outlive each run. Replay
   * observations never execute tools or trigger inference. */
  cai_agent_runtime_event_fn event_callback;
  void *event_context;
  cai_agent_session_event_fn replay_callback;
  void *replay_context;
  /** Owner-thread acknowledgement for every accepted command, including those
   * rejected or abandoned during shutdown. error is borrowed for the call;
   * acceptance is distinct from successful application. May call get_status
   * or queue another control. Do not reenter run or close the app here, even
   * when the final status is no longer running. NULL is valid; status retains
   * the last result. */
  void (*control_callback)(void *context, unsigned long long id, int status,
                           const cai_error *error);
  void *control_context;
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
  /** Number of immutable parser/help option entries; no startup effects. */
  size_t (*option_count)(const cai_app *self);
  /** Borrow one zero-based option entry, or NULL when index is out of range. */
  const cai_app_option *(*option_at)(const cai_app *self, size_t index);
  /** Write help with the configured identity and invocation_name to a borrowed
   * sink. No configuration files, logger, banner, auth or terminal are
   * initialized.
   */
  int (*write_help)(const cai_app *self, cai_sink *sink, cai_error *error);
  /** Thread-safe while self remains alive. Requires a ready runtime (STOP is
   * also accepted during startup and utilities). text is required only for
   * prompt, steering and goal replacement, and is copied before return.
   * Interactive prompts enter softline's approval/edit queue; batch prompts
   * enter the runtime FIFO. CAI_OK means accepted, with a nonzero id; inspect
   * the acknowledgement for application errors. Queue capacity is 64; a full
   * queue rejects explicitly. STOP requests orderly shutdown, cancelling active
   * work and waiting for workers; it does not terminate the host process. */
  int (*control)(cai_app *self, cai_app_control_type type, const char *text,
                 unsigned long long *id, cai_error *error);
  /** Thread-safe copied status and final run result; no file/log parsing.
   * After run returns, running is false and outcome/exit_code/error describe
   * that invocation. A later run resets the result. */
  int (*get_status)(const cai_app *self, cai_app_status *out, cai_error *error);
  void (*close)(cai_app *self);
  void *impl;
};

/** Seed CAI defaults, automatic XDG paths and no host overrides. */
void cai_app_config_init(cai_app_config *config);
/** Copy configuration and validate settings without reading files or running
 * the application. Logger remains borrowed. On failure *out is NULL. */
int cai_app_open(const cai_app_config *config, cai_app **out, cai_error *error);
/** Run synchronously with options only, excluding argv[0]. See cai_app.run. */
int cai_app_run(cai_app *self, int argc, const char *const *argv,
                int *exit_code, cai_error *error);
/** Return the option count without loading configuration or starting UI. */
size_t cai_app_option_count(const cai_app *self);
/** Borrow a zero-based help entry; NULL self/out-of-range returns NULL. */
const cai_app_option *cai_app_option_at(const cai_app *self, size_t index);
/** Write identity-aware help to a borrowed sink without startup side effects.
 */
int cai_app_write_help(const cai_app *self, cai_sink *sink, cai_error *error);
/** Thread-safe command acceptance, not completion. See cai_app.control for
 * runtime requirements, acknowledgement, copied text and STOP behavior. */
int cai_app_control(cai_app *self, cai_app_control_type type, const char *text,
                    unsigned long long *id, cai_error *error);
/** Copy the most recent safe status. Initialize out first; success replaces
 * its owned values and failure leaves them unchanged. Thread-safe. */
int cai_app_get_status(const cai_app *self, cai_app_status *out,
                       cai_error *error);
/** Close after run has returned, outside its callbacks. Calls made during run
 * are ignored. NULL is accepted. Must not race other receiver calls. */
void cai_app_close(cai_app *self);

/** Independent shared kanban access for HTTP hosts and other embedders.
 * This opens the same encrypted Pouch store as CAI without running a terminal
 * application or changing environment/cwd. NULL paths use normal CAI defaults.
 * The logger is borrowed through the final receiver close; if absent, a logger
 * is created from LOG_ with warn as its default level. */
typedef struct cai_app_kanban_config {
  const char *program_name;
  const char *namespace_prefix;
  const char *todo_namespace;
  const char *pouch_key_file;
  /** Absolute state directory containing pouch.key; NULL derives XDG/name. */
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
/** Initialize automatic CAI paths, the default board and bounded listing. */
void cai_app_kanban_config_init(cai_app_kanban_config *config);
/** Returns an owned core receiver with Pouch-backed transaction callbacks.
 * Its retained references own the store; the caller closes the receiver. */
int cai_app_kanban_open(const cai_app_kanban_config *config, cai_todo **out,
                        cai_error *error);

#ifdef __cplusplus
}
#endif
#endif
