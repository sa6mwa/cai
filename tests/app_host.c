#include <assert.h>
#include <cai/app.h>
#include <fcntl.h>
#include <pslog.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
static void host_interrupt(int signo) { (void)signo; }
int main(int argc, char **argv) {
  cai_app_config config;
  cai_app_setting setting;
  cai_app *app = NULL;
  cai_error error;
  pslog_config logging;
  pslog_logger *logger = NULL;
  FILE *log_file = NULL;
  int code = 1, rc;
  struct sigaction action, after;
  struct termios terminal, restored;
  int flags = fcntl(STDIN_FILENO, F_GETFL);
  int has_terminal = tcgetattr(STDIN_FILENO, &terminal) == 0;
  memset(&action, 0, sizeof(action));
  action.sa_handler = host_interrupt;
  sigemptyset(&action.sa_mask);
  assert(sigaction(SIGINT, &action, NULL) == 0);
  cai_app_config_init(&config);
  config.program_name = "vectis -a smith --";
  config.banner = getenv("CAI_TEST_APP_BANNER");
  if (getenv("CAI_TEST_APP_LOG_PATH") != NULL) {
    log_file = fopen(getenv("CAI_TEST_APP_LOG_PATH"), "a");
    assert(log_file != NULL);
    pslog_default_config(&logging);
    logging.mode = PSLOG_MODE_JSON;
    logging.min_level = PSLOG_LEVEL_TRACE;
    logging.output = pslog_output_from_fp(log_file, 0);
    logger = pslog_new(&logging);
    assert(logger != NULL);
    config.logger = logger;
  }
  if (getenv("CAI_TEST_APP_MODEL") != NULL) {
    setting.section = "agent";
    setting.key = "model";
    setting.value = getenv("CAI_TEST_APP_MODEL");
    config.settings = &setting;
    config.setting_count = 1U;
  }
  cai_error_init(&error);
  rc = cai_app_open(&config, &app, &error);
  if (rc == CAI_OK)
    rc =
        app->run(app, argc - 1, (const char *const *)(argv + 1), &code, &error);
  if (rc == CAI_OK && getenv("CAI_TEST_APP_REPEAT") != NULL)
    rc =
        app->run(app, argc - 1, (const char *const *)(argv + 1), &code, &error);
  assert(sigaction(SIGINT, NULL, &after) == 0 &&
         after.sa_handler == host_interrupt);
  assert(fcntl(STDIN_FILENO, F_GETFL) == flags);
  if (has_terminal) {
    assert(tcgetattr(STDIN_FILENO, &restored) == 0);
    assert(terminal.c_lflag == restored.c_lflag &&
           terminal.c_iflag == restored.c_iflag);
  }
  if (rc != CAI_OK)
    fprintf(stderr, "%s\n",
            error.message != NULL ? error.message : "app failed");
  cai_app_close(app);
  if (logger != NULL) {
    logger->info(logger, "HOST_LOGGER_STILL_OPEN", NULL, 0U);
    logger->destroy(logger);
    assert(fclose(log_file) == 0);
  }
  cai_error_cleanup(&error);
  return rc == CAI_OK ? code : 1;
}
