#include <assert.h>
#include <cai/app.h>
#include <fcntl.h>
#include <pslog.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
static void host_interrupt(int signo) { (void)signo; }
typedef struct host_tools {
  pthread_t owner;
  pthread_mutex_t lock;
  int registrations, calls, reject;
} host_tools;
static int host_tool(void *context, const char *arguments, cai_sink *output,
                     cai_error *error) {
  static const char result[] = "{\"host\":\"HOST_TOOL_RESULT\"}";
  host_tools *tools = context;
  assert(!pthread_equal(tools->owner, pthread_self()));
  assert(arguments != NULL && arguments[0] != '\0');
  assert(pthread_mutex_lock(&tools->lock) == 0);
  tools->calls++;
  assert(pthread_mutex_unlock(&tools->lock) == 0);
  return cai_sink_write(output, result, sizeof(result) - 1U, error);
}
static int register_host_tools(void *context, cai_agent *agent,
                               cai_error *error) {
  host_tools *tools = context;
  cai_custom_tool_format format;
  int rc;
  assert(pthread_equal(tools->owner, pthread_self()));
  tools->registrations++;
  rc = cai_agent_register_raw_tool(agent, tools->reject ? NULL : "host_fact",
                                   "Host fact", "{\"type\":\"object\"}", 0,
                                   host_tool, tools, error);
  memset(&format, 0, sizeof(format));
  format.type = "grammar";
  format.syntax = "lark";
  format.definition = "start: /./+";
  if (rc == CAI_OK)
    rc = cai_agent_register_custom_tool(agent, "host_raw", "Host raw tool",
                                        &format, host_tool, tools, error);
  return rc;
}
int main(int argc, char **argv) {
  cai_app_config config;
  cai_app_setting setting;
  cai_app *app = NULL;
  cai_error error;
  pslog_config logging;
  pslog_logger *logger = NULL;
  FILE *log_file = NULL;
  host_tools tools;
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
  memset(&tools, 0, sizeof(tools));
  tools.owner = pthread_self();
  assert(pthread_mutex_init(&tools.lock, NULL) == 0);
  if (getenv("CAI_TEST_APP_TOOLS") != NULL) {
    tools.reject = strcmp(getenv("CAI_TEST_APP_TOOLS"), "reject") == 0;
    config.register_agent_tools = register_host_tools;
    config.register_agent_tools_context = &tools;
  }
  config.invocation_name = "vectis -a smith --";
  if (getenv("CAI_TEST_APP_NAME") != NULL)
    config.program_name = getenv("CAI_TEST_APP_NAME");
  config.environment_prefix = getenv("CAI_TEST_APP_ENVIRONMENT_PREFIX");
  config.namespace_prefix = getenv("CAI_TEST_APP_NAMESPACE_PREFIX");
  config.queue_prefix = getenv("CAI_TEST_APP_QUEUE_PREFIX");
  config.config_file = getenv("CAI_TEST_APP_CONFIG_FILE");
  config.config_directory = getenv("CAI_TEST_APP_CONFIG_DIRECTORY");
  config.state_directory = getenv("CAI_TEST_APP_STATE_DIRECTORY");
  config.cache_directory = getenv("CAI_TEST_APP_CACHE_DIRECTORY");
  config.data_directory = getenv("CAI_TEST_APP_DATA_DIRECTORY");
  config.banner = getenv("CAI_TEST_APP_BANNER");
  config.introduction_markdown = getenv("CAI_TEST_APP_INTRODUCTION");
  if (getenv("CAI_TEST_APP_INTRODUCTION_LEFT") != NULL)
    config.introduction_margin_left =
        atoi(getenv("CAI_TEST_APP_INTRODUCTION_LEFT"));
  if (getenv("CAI_TEST_APP_INTRODUCTION_RIGHT") != NULL)
    config.introduction_margin_right =
        atoi(getenv("CAI_TEST_APP_INTRODUCTION_RIGHT"));
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
  config.register_agent_tools = NULL;
  config.register_agent_tools_context = NULL;
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
  if (getenv("CAI_TEST_APP_TOOL_REPORT") != NULL) {
    FILE *report = fopen(getenv("CAI_TEST_APP_TOOL_REPORT"), "w");
    assert(report != NULL);
    fprintf(report, "{\"registrations\":%d,\"calls\":%d}\n",
            tools.registrations, tools.calls);
    assert(fclose(report) == 0);
  }
  assert(pthread_mutex_destroy(&tools.lock) == 0);
  if (logger != NULL) {
    logger->info(logger, "HOST_LOGGER_STILL_OPEN", NULL, 0U);
    logger->destroy(logger);
    assert(fclose(log_file) == 0);
  }
  cai_error_cleanup(&error);
  return rc == CAI_OK ? code : 1;
}
