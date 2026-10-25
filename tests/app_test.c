#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <cai/app.h>
#include <fcntl.h>
#include <locale.h>
#include <pslog.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct log_state {
  int records, closed, recursion;
  cai_app *app;
} log_state;
static int log_write(void *context, const char *bytes, size_t count,
                     size_t *written) {
  log_state *state = context;
  (void)bytes;
  (void)count;
  state->records++;
  *written = count;
  if (state->app != NULL && !state->recursion) {
    cai_error error;
    int code;
    const char *args[] = {"--help"};
    cai_error_init(&error);
    state->recursion = 1;
    assert(state->app->run(state->app, 1, args, &code, &error) ==
           CAI_ERR_INVALID);
    cai_error_cleanup(&error);
  }
  return 0;
}
static int log_close(void *context) {
  ((log_state *)context)->closed++;
  return 0;
}
static int help_write(void *context, const void *bytes, size_t count,
                      cai_error *error) {
  char *buffer = context;
  (void)error;
  assert(strlen(buffer) + count < 16384U);
  strncat(buffer, bytes, count);
  return CAI_OK;
}
static void handler(int signo) { (void)signo; }
int main(int argc, char **argv) {
  cai_app_config config;
  cai_app *app = NULL;
  cai_error error;
  cai_sink *sink = NULL;
  cai_sink_callbacks callbacks;
  pslog_config logging;
  pslog_logger *logger;
  log_state logs;
  char root[4096], work[4096], pouch[4096], cwd[4096], current[4096];
  char help[16384];
  char program[] = "vectis -a smith --";
  const char *bad[] = {"--not-a-cai-option"};
  const char *list[] = {"--list", "-C", NULL};
  const char *help_args[] = {"-h"};
  const char *bad_dir[] = {"-C", "/nonexistent/cai-embedding-workspace",
                           "--list"};
  cai_app_setting settings[3];
  struct sigaction previous, action, after;
  size_t i;
  int code, saw_model = 0, before_flags;
  assert(argc == 2);
  snprintf(root, sizeof(root), "%s/app-test-XXXXXX", argv[1]);
  assert(mkdtemp(root) != NULL);
  assert(snprintf(work, sizeof(work), "%s/work", root) < (int)sizeof(work));
  assert(snprintf(pouch, sizeof(pouch), "pouch://%s/pouch", root) <
         (int)sizeof(pouch));
  assert(mkdir(work, 0700) == 0);
  assert(getcwd(cwd, sizeof(cwd)) != NULL);
  assert(setenv("CAI_APP_RESTORE_TEST", "host", 1) == 0);
  assert(unsetenv("CAI_APP_NEW_VARIABLE") == 0);
  memset(&action, 0, sizeof(action));
  action.sa_handler = handler;
  sigemptyset(&action.sa_mask);
  assert(sigaction(SIGINT, &action, &previous) == 0);
  before_flags = fcntl(STDIN_FILENO, F_GETFL);
  memset(&logs, 0, sizeof(logs));
  pslog_default_config(&logging);
  logging.mode = PSLOG_MODE_JSON;
  logging.min_level = PSLOG_LEVEL_TRACE;
  logging.output.userdata = &logs;
  logging.output.write = log_write;
  logging.output.close = log_close;
  logging.output.owned = 1;
  logger = pslog_new(&logging);
  assert(logger != NULL);
  settings[0].section = "store.default";
  settings[0].key = "endpoint";
  settings[0].value = pouch;
  settings[1].section = "env";
  settings[1].key = "CAI_APP_NEW_VARIABLE";
  settings[1].value = "temporary";
  settings[2].section = "paths";
  settings[2].key = "directory";
  settings[2].value = work;
  cai_app_config_init(&config);
  assert(strcmp(config.program_name, "cai") == 0 && config.logger == NULL);
  assert(config.banner == NULL && config.introduction_markdown == NULL);
  assert(config.introduction_margin_left == 0 &&
         config.introduction_margin_right == 0);
  cai_error_init(&error);
  config.introduction_margin_left = -1;
  assert(cai_app_open(&config, &app, &error) == CAI_ERR_INVALID && app == NULL);
  config.introduction_margin_left = 0;
  config.introduction_margin_right = -1;
  assert(cai_app_open(&config, &app, &error) == CAI_ERR_INVALID && app == NULL);
  config.introduction_margin_right = 0;
  cai_error_cleanup(&error);
  cai_error_init(&error);
  assert(config.register_agent_tools == NULL &&
         config.register_agent_tools_context == NULL);
  config.invocation_name = program;
  config.banner = "BANNER_MUST_NOT_APPEAR_IN_HELP";
  config.introduction_markdown = "INTRODUCTION_MUST_NOT_APPEAR_IN_HELP";
  config.no_config = 1;
  config.settings = settings;
  config.setting_count = 3U;
  config.logger = logger;
  cai_error_init(&error);
  assert(cai_app_open(&config, &app, &error) == CAI_OK);
  {
    cai_app_status status;
    unsigned long long id = 123U;
    cai_app_status_init(&status);
    assert(app->get_status(app, &status, &error) == CAI_OK);
    assert(status.outcome == CAI_APP_NOT_RUN && !status.running);
    assert(app->control(app, CAI_APP_QUEUE_PROMPT, NULL, &id, &error) ==
           CAI_ERR_INVALID);
    assert(id == 0U);
    cai_error_cleanup(&error);
    assert(app->control(app, CAI_APP_STOP, "extra", &id, &error) ==
           CAI_ERR_INVALID);
    cai_error_cleanup(&error);
    assert(app->control(app, (cai_app_control_type)999, NULL, &id, &error) ==
           CAI_ERR_INVALID);
    cai_error_cleanup(&error);
    assert(app->get_status(app, NULL, &error) == CAI_ERR_INVALID);
    cai_error_cleanup(&error);
    cai_app_status_cleanup(&status);
  }
  logs.app = app;
  program[0] = 'X';
  assert(app->option_count(app) > 50U);
  for (i = 0U; i < app->option_count(app); i++) {
    const cai_app_option *o = app->option_at(app, i);
    assert(o != NULL && o->description != NULL &&
           strncmp(o->long_flag, "--", 2U) == 0);
    if (strcmp(o->long_flag, "--model") == 0) {
      assert(o->short_flag == 'm' && o->type == CAI_APP_STRING);
      saw_model = 1;
    }
  }
  assert(saw_model && app->option_at(app, i) == NULL);
  memset(&callbacks, 0, sizeof(callbacks));
  help[0] = '\0';
  callbacks.context = help;
  callbacks.write = help_write;
  assert(cai_sink_from_callbacks(&callbacks, &sink, &error) == CAI_OK);
  assert(app->write_help(app, sink, &error) == CAI_OK);
  assert(strstr(help, "Usage: vectis -a smith -- [options]") != NULL);
  assert(strstr(help, config.banner) == NULL && logs.records == 0);
  cai_sink_close(sink);
  assert(app->run(app, 1, help_args, &code, &error) == CAI_OK && code == 0);
  assert(app->run(app, -1, NULL, &code, &error) == CAI_ERR_INVALID);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  assert(app->run(app, 1, bad, &code, &error) == CAI_OK && code == 2);
  {
    cai_app_status status;
    cai_app_status_init(&status);
    assert(app->get_status(app, &status, &error) == CAI_OK);
    assert(status.outcome == CAI_APP_FAILED && status.exit_code == 2 &&
           !status.running);
    assert(app->run(app, 1, help_args, &code, &error) == CAI_OK && code == 0);
    assert(app->get_status(app, &status, &error) == CAI_OK);
    assert(status.outcome == CAI_APP_COMPLETED && status.error.code == CAI_OK);
    assert(status.session_id == NULL && status.control_id == 0U);
    cai_app_status_cleanup(&status);
  }
  assert(logs.records > 0 && logs.recursion && logs.closed == 0);
  assert(app->run(app, 3, bad_dir, &code, &error) == CAI_OK && code == 2);
  list[2] = work;
  assert(app->run(app, 3, list, &code, &error) == CAI_OK && code == 0);
  assert(app->run(app, 3, list, &code, &error) == CAI_OK && code == 0);
  assert(getcwd(current, sizeof(current)) != NULL && strcmp(current, cwd) == 0);
  assert(strcmp(getenv("CAI_APP_RESTORE_TEST"), "host") == 0);
  assert(getenv("CAI_APP_NEW_VARIABLE") == NULL);
  assert(sigaction(SIGINT, NULL, &after) == 0 && after.sa_handler == handler);
  assert(fcntl(STDIN_FILENO, F_GETFL) == before_flags);
  assert(strcmp(setlocale(LC_CTYPE, NULL), "C") == 0);
  logs.app = NULL;
  app->close(app);
  config.program_name = CAI_APP_DEFAULT_PROGRAM_NAME;
  config.config_file = "/nonexistent/cai-embedding-config.ini";
  config.no_config = 0;
  assert(cai_app_open(&config, &app, &error) == CAI_OK);
  {
    const char *skip[] = {"--no-config", "--list"};
    assert(app->run(app, 2, skip, &code, &error) == CAI_OK && code == 0);
  }
  app->close(app);
  logger->info(logger, "HOST_AFTER_APP_CLOSE", NULL, 0U);
  assert(logs.closed == 0);
  logger->destroy(logger);
  config.logger = NULL;
  assert(logs.closed == 1);
  config.setting_count = 1U;
  settings[0].section = "agent";
  settings[0].key = "unknown_setting";
  assert(cai_app_open(&config, &app, &error) == CAI_ERR_INVALID && app == NULL);
  assert(sigaction(SIGINT, &previous, NULL) == 0);
  cai_error_cleanup(&error);
  /* CTest owns this isolated directory and removes it in the shell test. */
  return 0;
}
