/* Minimal C embedding example: all CAI behavior lives in libcai_app. */
#include <cai/app.h>
#include <pslog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  cai_app_config config;
  cai_app *app = NULL;
  cai_error error;
  int code = 1;
  int rc;
  cai_app_config_init(&config);
  config.banner =
      isatty(STDOUT_FILENO) && getenv("NO_COLOR") == NULL &&
              (getenv("TERM") == NULL || strcmp(getenv("TERM"), "dumb") != 0)
          ? "\n\033[97mC\033[94mA\033[97mI\033[0m Smith (C) 2026 C89 Systems "
            "AB https://c89.systems"
          : "\nCAI Smith (C) 2026 C89 Systems AB https://c89.systems";
  config.introduction_markdown = "**Enter** sends your task. `/help` for "
                                 "commands; `cai -h` for options.\n";
  cai_error_init(&error);
  rc = cai_app_open(&config, &app, &error);
  if (rc == CAI_OK)
    rc =
        app->run(app, argc - 1, (const char *const *)(argv + 1), &code, &error);
  if (rc != CAI_OK) {
    pslog_config logging;
    pslog_logger *logger;
    pslog_default_config(&logging);
    logging.mode = PSLOG_MODE_JSON;
    logging.color = PSLOG_COLOR_AUTO;
    logging.min_level = PSLOG_LEVEL_WARN;
    logger = pslog_new_from_env("LOG_", &logging);
    if (logger != NULL) {
      logger->error(
          logger, error.message != NULL ? error.message : cai_status_string(rc),
          NULL, 0U);
      logger->destroy(logger);
    } else
      fputs("cai: cannot initialize application or diagnostic logger\n",
            stderr);
  }
  if (app != NULL)
    app->close(app);
  cai_error_cleanup(&error);
  return rc == CAI_OK ? code : 1;
}
