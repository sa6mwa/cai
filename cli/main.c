/* Minimal C embedding example: all CAI behavior lives in libcai_app. */
#include <cai/app.h>
#include <pslog.h>
#include <stdio.h>

int main(int argc, char **argv) {
  cai_app_config config;
  cai_app *app = NULL;
  cai_error error;
  int code = 1;
  int rc;
  cai_app_config_init(&config);
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
