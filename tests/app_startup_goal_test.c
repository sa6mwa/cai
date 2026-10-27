#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <cai/app.h>
#include <stdlib.h>
#include <string.h>

typedef struct goal_host {
  cai_app *app;
  const char *mode;
  int changes, starts, acks, rejected;
} goal_host;
static void control(goal_host *host, cai_app_control_type type,
                    const char *text) {
  cai_error error;
  unsigned long long id;
  cai_error_init(&error);
  assert(host->app->control(host->app, type, text, &id, &error) == CAI_OK);
  assert(id != 0U);
  cai_error_cleanup(&error);
}
static int event(void *context, const cai_agent_runtime_event *e,
                 cai_error *error) {
  goal_host *host = context;
  if (e->type == CAI_AGENT_EVENT_RUN_STARTED)
    host->starts++;
  if (e->type != CAI_AGENT_EVENT_GOAL_CHANGED)
    return CAI_OK;
  host->changes++;
  if (host->changes == 2 && strcmp(host->mode, "pause_resume") == 0)
    control(host, CAI_APP_RESUME_GOAL, NULL);
  if (host->changes != 1)
    return CAI_OK;
  if (strcmp(host->mode, "error") == 0) {
    error->code = CAI_ERR_PROTOCOL;
    error->message = strdup("HOST STARTUP GOAL OBSERVER FAILED");
    assert(error->message != NULL);
    return CAI_ERR_PROTOCOL;
  }
  if (strcmp(host->mode, "clear") == 0)
    control(host, CAI_APP_CLEAR_GOAL, NULL);
  else if (strcmp(host->mode, "pause") == 0 ||
           strcmp(host->mode, "pause_resume") == 0)
    control(host, CAI_APP_PAUSE_GOAL, NULL);
  else if (strcmp(host->mode, "stop") == 0)
    control(host, CAI_APP_STOP, NULL);
  else if (strcmp(host->mode, "resume_active") == 0)
    control(host, CAI_APP_RESUME_GOAL, NULL);
  else {
    if (strcmp(host->mode, "clear_replace") == 0)
      control(host, CAI_APP_CLEAR_GOAL, NULL);
    control(host, CAI_APP_REPLACE_GOAL,
            strcmp(host->mode, "same") == 0 ? "INITIAL STARTUP GOAL"
                                            : "HOST REPLACEMENT");
    if (strcmp(host->mode, "replace_clear") == 0)
      control(host, CAI_APP_CLEAR_GOAL, NULL);
  }
  return CAI_OK;
}
static void ack(void *context, unsigned long long id, int status,
                const cai_error *error) {
  goal_host *host = context;
  (void)error;
  assert(id != 0U);
  host->acks++;
  if (status != CAI_OK)
    host->rejected++;
}
int main(int argc, char **argv) {
  goal_host host;
  cai_app_config config;
  cai_app_status status;
  cai_error error;
  int code = 1, work;
  memset(&host, 0, sizeof(host));
  host.mode = getenv("CAI_TEST_STARTUP_GOAL_MODE");
  assert(host.mode != NULL);
  cai_app_config_init(&config);
  config.event_callback = event;
  config.event_context = &host;
  config.control_callback = ack;
  config.control_context = &host;
  cai_error_init(&error);
  cai_app_status_init(&status);
  assert(cai_app_open(&config, &host.app, &error) == CAI_OK);
  assert(host.app->run(host.app, argc - 1, (const char *const *)(argv + 1),
                       &code, &error) == CAI_OK);
  assert(host.app->get_status(host.app, &status, &error) == CAI_OK);
  assert(!status.running && !status.runtime_ready);
  work = strcmp(host.mode, "replace") == 0 || strcmp(host.mode, "same") == 0 ||
         strcmp(host.mode, "clear_replace") == 0 ||
         strcmp(host.mode, "pause_resume") == 0 ||
         strcmp(host.mode, "resume_active") == 0;
  if (strcmp(host.mode, "error") == 0) {
    assert(code == 1 && status.outcome == CAI_APP_FAILED && host.acks == 0);
  } else {
    assert(code == 0);
    assert(host.acks == ((strcmp(host.mode, "clear_replace") == 0 ||
                          strcmp(host.mode, "replace_clear") == 0 ||
                          strcmp(host.mode, "pause_resume") == 0)
                             ? 2
                             : 1));
    assert(host.rejected == 0);
  }
  assert(host.starts == (work ? 1 : 0));
  if (work) {
    assert(status.goal.has_goal && strcmp(status.goal.status, "complete") == 0);
    assert(strcmp(status.goal.objective,
                  strcmp(host.mode, "replace") == 0 ||
                          strcmp(host.mode, "clear_replace") == 0
                      ? "HOST REPLACEMENT"
                      : "INITIAL STARTUP GOAL") == 0);
  } else if (strcmp(host.mode, "clear") == 0 ||
             strcmp(host.mode, "replace_clear") == 0)
    assert(!status.goal.has_goal);
  else if (strcmp(host.mode, "pause") == 0)
    assert(status.goal.has_goal && strcmp(status.goal.status, "paused") == 0);
  else if (strcmp(host.mode, "stop") == 0)
    assert(status.outcome == CAI_APP_STOPPED);
  host.app->close(host.app);
  cai_app_status_cleanup(&status);
  cai_error_cleanup(&error);
  return 0;
}
