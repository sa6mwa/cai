#define _GNU_SOURCE
#include "app_internal.h"
#include "pouch_stream.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

extern char **environ;
static pthread_mutex_t application_lock = PTHREAD_MUTEX_INITIALIZER;
static const int signals[] = {SIGINT,  SIGTERM, SIGHUP,  SIGQUIT, SIGWINCH,
                              SIGTSTP, SIGCONT, SIGPIPE, SIGTTOU, SIGTTIN};
typedef struct app_impl {
  cai_app shell;
  cai_app_config config;
  cai_app_setting *settings;
  char **strings;
  int running;
} app_impl;
typedef struct process_state {
  char **environment;
  size_t count;
  char *locale;
  int directory;
  int flags[3];
  struct sigaction actions[sizeof(signals) / sizeof(signals[0])];
  sigset_t mask;
  struct termios terminal;
  int has_terminal;
} process_state;

void cai_app_config_init(cai_app_config *config) {
  if (config != NULL) {
    memset(config, 0, sizeof(*config));
    config->program_name = "cai";
  }
}
static void process_free(process_state *saved) {
  size_t i;
  for (i = 0U; i < saved->count; i++)
    free(saved->environment[i]);
  free(saved->environment);
  free(saved->locale);
  if (saved->directory >= 0)
    close(saved->directory);
}
static int process_save(process_state *saved, cai_error *error) {
  size_t i;
  memset(saved, 0, sizeof(*saved));
  saved->directory = -1;
  while (environ != NULL && environ[saved->count] != NULL)
    saved->count++;
  saved->environment = calloc(saved->count + 1U, sizeof(char *));
  if (saved->environment == NULL) {
    saved->count = 0U;
    return cai_cli_error(error, CAI_ERR_NOMEM, "snapshot host environment");
  }
  for (i = 0U; i < saved->count; i++) {
    saved->environment[i] = strdup(environ[i]);
    if (saved->environment[i] == NULL)
      return cai_cli_error(error, CAI_ERR_NOMEM, "snapshot host environment");
  }
  saved->locale = strdup(setlocale(LC_CTYPE, NULL));
  if (saved->locale == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "snapshot host locale");
  saved->directory = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (saved->directory < 0 ||
      pthread_sigmask(SIG_SETMASK, NULL, &saved->mask) != 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "snapshot host process state");
  for (i = 0U; i < sizeof(signals) / sizeof(signals[0]); i++)
    if (sigaction(signals[i], NULL, &saved->actions[i]) != 0)
      return cai_cli_error(error, CAI_ERR_TRANSPORT, "snapshot host signals");
  for (i = 0U; i < 3U; i++)
    saved->flags[i] = fcntl((int)i, F_GETFL);
  saved->has_terminal = tcgetattr(STDIN_FILENO, &saved->terminal) == 0;
  return CAI_OK;
}
static int process_restore(process_state *saved, cai_error *error) {
  size_t i;
  int failed = 0;
  if (fchdir(saved->directory) != 0)
    failed = 1;
  if (clearenv() != 0)
    failed = 1;
  for (i = 0U; i < saved->count; i++) {
    char *equal = strchr(saved->environment[i], '=');
    if (equal != NULL) {
      *equal = '\0';
      if (setenv(saved->environment[i], equal + 1, 1) != 0)
        failed = 1;
      *equal = '=';
    }
  }
  if (setlocale(LC_CTYPE, saved->locale) == NULL)
    failed = 1;
  for (i = 0U; i < sizeof(signals) / sizeof(signals[0]); i++)
    if (sigaction(signals[i], &saved->actions[i], NULL) != 0)
      failed = 1;
  if (pthread_sigmask(SIG_SETMASK, &saved->mask, NULL) != 0)
    failed = 1;
  if (saved->has_terminal &&
      tcsetattr(STDIN_FILENO, TCSANOW, &saved->terminal) != 0)
    failed = 1;
  for (i = 0U; i < 3U; i++)
    if (saved->flags[i] >= 0 && fcntl((int)i, F_SETFL, saved->flags[i]) != 0)
      failed = 1;
  return failed ? cai_cli_error(error, CAI_ERR_TRANSPORT,
                                "could not restore host process state")
                : CAI_OK;
}
int cai_app_run(cai_app *self, int argc, const char *const *argv,
                int *exit_code, cai_error *error) {
  app_impl *app;
  process_state saved;
  char **arguments;
  int i, rc;
  if (self == NULL || exit_code == NULL || argc < 0 || argc == INT_MAX ||
      (argc > 0 && argv == NULL))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "invalid application arguments");
  *exit_code = 1;
  app = self->impl;
  if (pthread_mutex_trylock(&application_lock) != 0)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "a CAI application is already running");
  arguments = calloc((size_t)argc + 2U, sizeof(*arguments));
  rc = arguments != NULL ? CAI_OK : CAI_ERR_NOMEM;
  for (i = 0; rc == CAI_OK && i <= argc; i++) {
    const char *value = i == 0 ? app->config.program_name : argv[i - 1];
    if (value == NULL)
      rc = CAI_ERR_INVALID;
    else if ((arguments[i] = strdup(value)) == NULL)
      rc = CAI_ERR_NOMEM;
  }
  if (rc != CAI_OK) {
    cai_cli_error(error, rc, "copy application arguments");
  } else {
    rc = process_save(&saved, error);
    if (rc == CAI_OK) {
      app->running = 1;
      *exit_code = cai_cli_app_main(argc + 1, arguments, &app->config);
      app->running = 0;
      rc = process_restore(&saved, error);
    }
    process_free(&saved);
  }
  if (arguments != NULL) {
    for (i = 0; i <= argc; i++)
      free(arguments[i]);
    free(arguments);
  }
  pthread_mutex_unlock(&application_lock);
  return rc;
}
size_t cai_app_option_count(const cai_app *self) {
  return self != NULL ? cai_cli_help_count() : 0U;
}
const cai_app_option *cai_app_option_at(const cai_app *self, size_t index) {
  return self != NULL ? cai_cli_help_at(index) : NULL;
}
int cai_app_write_help(const cai_app *self, cai_sink *sink, cai_error *error) {
  const app_impl *app;
  if (self == NULL || sink == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "help requires application and sink");
  app = self->impl;
  return cai_cli_help_write(app->config.program_name, sink, error);
}
void cai_app_close(cai_app *self) {
  app_impl *app;
  size_t i;
  if (self == NULL)
    return;
  app = self->impl;
  if (app->running)
    return;
  for (i = 0U; app->strings != NULL && i < 4U + 3U * app->config.setting_count;
       i++)
    free(app->strings[i]);
  free(app->strings);
  free(app->settings);
  free(app);
}
int cai_app_open(const cai_app_config *config, cai_app **out,
                 cai_error *error) {
  cai_app_config defaults;
  cai_config *schema = NULL;
  app_impl *app;
  size_t i;
  int rc;
  if (out == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "application output is required");
  *out = NULL;
  cai_app_config_init(&defaults);
  if (config == NULL)
    config = &defaults;
  if ((config->setting_count != 0U && config->settings == NULL) ||
      config->setting_count > ((size_t)-1 / sizeof(cai_app_setting) - 4U) / 3U)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "invalid application settings");
  app = calloc(1U, sizeof(*app));
  if (app == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate application");
  app->shell.impl = app;
  app->shell.run = cai_app_run;
  app->shell.option_count = cai_app_option_count;
  app->shell.option_at = cai_app_option_at;
  app->shell.write_help = cai_app_write_help;
  app->shell.close = cai_app_close;
  app->config.setting_count = config->setting_count;
  app->strings = calloc(4U + 3U * config->setting_count, sizeof(char *));
  if (app->strings == NULL) {
    cai_app_close(&app->shell);
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "copy application configuration");
  }
  app->config.program_name = app->strings[0] =
      strdup(config->program_name != NULL ? config->program_name : "cai");
  app->config.banner = app->strings[1] =
      config->banner != NULL ? strdup(config->banner) : NULL;
  app->config.config_file = app->strings[2] =
      config->config_file != NULL ? strdup(config->config_file) : NULL;
  app->config.config_directory = app->strings[3] =
      config->config_directory != NULL ? strdup(config->config_directory)
                                       : NULL;
  app->config.no_config = config->no_config;
  app->config.logger = config->logger;
  app->config.register_agent_tools = config->register_agent_tools;
  app->config.register_agent_tools_context =
      config->register_agent_tools_context;
  app->config.setting_count = config->setting_count;
  if (app->config.program_name == NULL ||
      (config->banner != NULL && app->config.banner == NULL) ||
      (config->config_file != NULL && app->config.config_file == NULL) ||
      (config->config_directory != NULL &&
       app->config.config_directory == NULL)) {
    cai_app_close(&app->shell);
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "copy application configuration");
  }
  if (app->config.program_name[0] == '\0' ||
      strlen(app->config.program_name) > 512U ||
      strpbrk(app->config.program_name, "\r\n\t") != NULL) {
    cai_app_close(&app->shell);
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "invalid application program name");
  }
  rc = cai_cli_app_schema(&schema, error) < 0 ? CAI_ERR_INVALID : CAI_OK;
  if (config->setting_count != 0U) {
    app->settings = calloc(config->setting_count, sizeof(*app->settings));
    if (app->settings == NULL)
      rc = cai_cli_error(error, CAI_ERR_NOMEM, "copy application settings");
  }
  for (i = 0U; rc == CAI_OK && i < config->setting_count; i++) {
    const cai_app_setting *s = &config->settings[i];
    rc = cai_config_set(schema, s->section, s->key, s->value, error);
    if (rc == CAI_OK) {
      app->settings[i].section = app->strings[4U + i * 3U] = strdup(s->section);
      app->settings[i].key = app->strings[5U + i * 3U] = strdup(s->key);
      app->settings[i].value = app->strings[6U + i * 3U] = strdup(s->value);
      if (app->settings[i].section == NULL || app->settings[i].key == NULL ||
          app->settings[i].value == NULL)
        rc = cai_cli_error(error, CAI_ERR_NOMEM, "copy application setting");
    }
  }
  cai_config_close(schema);
  app->config.settings = app->settings;
  if (rc != CAI_OK) {
    cai_app_close(&app->shell);
    return rc;
  }
  *out = &app->shell;
  return CAI_OK;
}
