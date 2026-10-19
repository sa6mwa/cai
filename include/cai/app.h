#ifndef CAI_APP_H
#define CAI_APP_H

#include <cai/config.h>

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

#ifdef __cplusplus
}
#endif
#endif
