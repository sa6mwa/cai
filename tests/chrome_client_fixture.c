#define _POSIX_C_SOURCE 200809L
#include "../src/cai_chrome_protocol.h"
#include "../src/cai_internal.h"
#include <cai/agent_runtime.h>
#include <cai/chrome.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int control_probe(int argc, char **argv) {
  cai_chrome control;
  cai_error error;
  char *reply = NULL;
  int rc;
  memset(&control, 0, sizeof(control));
  control.fd = -1;
  control.timeout_ms = 1000L;
  control.limit = 32768U;
  cai_error_init(&error);
  rc = cai_chrome_connect_socket(argv[1], &control.fd, &error);
  if (rc == CAI_OK)
    rc = cai_chrome_request(&control, argv[3], argc > 4 ? argv[4] : "{}",
                            &reply, &error);
  if (rc == CAI_OK)
    puts(reply);
  else
    fprintf(stderr, "%s\n", error.message != NULL ? error.message : "failed");
  if (control.fd >= 0)
    close(control.fd);
  cai_chrome_result_free(reply);
  cai_error_cleanup(&error);
  return rc == CAI_OK ? 0 : 1;
}

static int stale_socket(const char *path) {
  struct sockaddr_un address;
  int fd, rc;
  if (strlen(path) >= sizeof(address.sun_path))
    return 2;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  strcpy(address.sun_path, path);
  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return 2;
  rc = bind(fd, (struct sockaddr *)&address, sizeof(address));
  close(fd);
  if (rc == 0)
    rc = chmod(path, 0600);
  return rc == 0 ? 0 : 2;
}

static int hold_browser(cai_chrome *chrome, cai_error *error) {
  char *reply = NULL;
  int tab, rc = CAI_OK;
  for (tab = 7; tab <= 8 && rc == CAI_OK; tab++) {
    char params[160];
    snprintf(params, sizeof(params),
             "{\"session_id\":\"shutdown-fixture\",\"turn_id\":\"1\","
             "\"session_context\":\"live\",\"tabId\":%d}",
             tab);
    rc = cai_chrome_request(chrome, "attach", params, &reply, error);
    cai_chrome_result_free(reply);
    reply = NULL;
  }
  if (rc == CAI_OK) {
    puts("browser-held");
    fflush(stdout);
    (void)getchar(); /* Bash releases this only after removing the host. */
    rc = cai_chrome_request(chrome, "getUserTabs", "{}", &reply, error);
  }
  cai_chrome_result_free(reply);
  return rc;
}

typedef struct cancel_state {
  struct timespec start;
} cancel_state;
static int cancel_after(void *context) {
  cancel_state *s = context;
  struct timespec now;
  long long elapsed;
  clock_gettime(CLOCK_MONOTONIC, &now);
  elapsed = (long long)(now.tv_sec - s->start.tv_sec) * 1000LL +
            (long long)(now.tv_nsec - s->start.tv_nsec) / 1000000LL;
  return elapsed > 100LL;
}
typedef struct runtime_state {
  int finished;
  int failed;
  int tool_started;
} runtime_state;
static int runtime_event(void *context, const cai_agent_runtime_event *event,
                         cai_error *error) {
  runtime_state *state = context;
  (void)error;
  if (event->type == CAI_AGENT_EVENT_TEXT_DELTA)
    fwrite(event->data, 1U, event->data_length, stdout);
  if (event->type == CAI_AGENT_EVENT_TOOL_CALL_COMPLETED)
    state->tool_started = 1;
  if (event->type == CAI_AGENT_EVENT_RUN_COMPLETED ||
      event->type == CAI_AGENT_EVENT_RUN_FAILED ||
      event->type == CAI_AGENT_EVENT_RUN_CANCELLED) {
    state->finished = 1;
    state->failed = event->type != CAI_AGENT_EVENT_RUN_COMPLETED;
  }
  return CAI_OK;
}
static int cleanup_tabs(cai_chrome *chrome, int explicit_detach,
                        cai_error *error) {
  const int tabs[] = {7, 8, 7};
  const char *session = explicit_detach ? "explicit" : "multi";
  char params[160], *reply = NULL;
  size_t i;
  int rc = CAI_OK;
  for (i = 0U; rc == CAI_OK && i < sizeof(tabs) / sizeof(tabs[0]); i++) {
    snprintf(params, sizeof(params),
             "{\"session_id\":\"%s\",\"turn_id\":\"1\","
             "\"session_context\":\"live\",\"tabId\":%d}",
             session, tabs[i]);
    rc = cai_chrome_request(chrome, "attach", params, &reply, error);
    cai_chrome_result_free(reply);
    reply = NULL;
  }
  if (rc == CAI_OK && explicit_detach) {
    rc = cai_chrome_request(chrome, "detach", params, &reply, error);
    cai_chrome_result_free(reply);
    reply = NULL;
  }
  if (rc == CAI_OK) {
    snprintf(params, sizeof(params),
             "{\"session_id\":\"%s\",\"turn_id\":\"1\","
             "\"session_context\":\"cached\"}",
             session);
    rc = cai_chrome_request(chrome, "turnEnded", params, &reply, error);
    cai_chrome_result_free(reply);
  }
  return rc;
}
static int run_runtime(cai_chrome *chrome, const char *endpoint,
                       const char *workspace, const char *mode,
                       cai_error *error) {
  cai_client_config cc;
  cai_agent_runtime_config config;
  cai_client *client = NULL;
  cai_agent_runtime *runtime = NULL;
  runtime_state state;
  struct timespec start, now;
  int rc, cancelled = 0;
  memset(&state, 0, sizeof(state));
  cai_client_config_init(&cc);
  cc.api_key = "fixture";
  cc.base_url = endpoint;
  cc.logger_disabled = 1;
  if (strcmp(mode, "runtime-chat") == 0)
    cc.api_protocol = CAI_API_PROTOCOL_CHAT_COMPLETIONS;
  cai_agent_runtime_config_init(&config);
  config.chrome = chrome;
  config.workspace_directory = workspace;
  config.agent_config_directory = workspace;
  config.disable_default_session_store = 1;
  config.disable_terminal = 1;
  config.disable_review_subagent = 1;
  config.logger_disabled = 1;
  config.event_callback = runtime_event;
  config.event_context = &state;
  rc = cai_client_open(&cc, &client, error);
  if (rc == CAI_OK)
    rc = cai_agent_runtime_open(client, &config, &runtime, error);
  if (rc == CAI_OK)
    rc = cai_agent_runtime_submit(runtime, "Inspect the browser", error);
  clock_gettime(CLOCK_MONOTONIC, &start);
  while (rc == CAI_OK && !state.finished) {
    rc = cai_agent_runtime_pump(runtime, 50L, error);
    if (rc == CAI_OK && !cancelled && state.tool_started &&
        strcmp(mode, "runtime-cancel") == 0) {
      rc = cai_agent_runtime_cancel_turn(runtime, error);
      cancelled = 1;
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - start.tv_sec > 10)
      rc = cai_set_error(error, CAI_ERR_LIMIT, "fixture runtime timed out");
  }
  cai_agent_runtime_close(runtime);
  if (client != NULL)
    client->close(client);
  if (rc == CAI_OK && state.failed)
    rc = cai_set_error(error, CAI_ERR_SERVER,
                       "fixture runtime failed or was cancelled");
  return rc;
}
int main(int argc, char **argv) {
  cai_chrome_config config;
  cai_chrome *chrome = NULL;
  cai_error error;
  cai_client_config client_config;
  cai_agent_config agent_config;
  cai_client *client = NULL;
  cai_agent *agent = NULL;
  cai_sink *sink = NULL;
  cai_stream_cancel_scope cancellation;
  cancel_state state;
  char *result = NULL;
  int rc;
  if (argc == 3 && strcmp(argv[2], "stale") == 0)
    return stale_socket(argv[1]);
  if (argc >= 4 && strcmp(argv[2], "control") == 0)
    return control_probe(argc, argv);
  if (argc < 3)
    return 2;
  memset(&config, 0, sizeof(config));
  cai_error_init(&error);
  if (strcmp(argv[2], "discover") == 0)
    config.socket_directory = argv[1];
  else if (strcmp(argv[2], "default") != 0)
    config.socket_path = argv[1];
  config.timeout_ms = 400L;
  if (strcmp(argv[2], "limit") == 0)
    config.max_response_bytes = 200U;
  rc = strcmp(argv[2], "default") == 0
           ? cai_chrome_open_lazy(&config, &chrome, &error)
           : cai_chrome_open(&config, &chrome, &error);
  if (rc == CAI_OK && strcmp(argv[2], "hold") == 0)
    rc = hold_browser(chrome, &error);
  else if (rc == CAI_OK && argc > 4 && strncmp(argv[2], "runtime-", 8U) == 0)
    rc = run_runtime(chrome, argv[3], argv[4], argv[2], &error);
  else if (rc == CAI_OK && (strcmp(argv[2], "cleanup-multi") == 0 ||
                            strcmp(argv[2], "cleanup-explicit") == 0))
    rc = cleanup_tabs(chrome, strcmp(argv[2], "cleanup-explicit") == 0, &error);
  else if (rc == CAI_OK && strcmp(argv[2], "abandon") == 0) {
    /* A raw API caller vanishes without turnEnded: the host owns cleanup. */
    rc = cai_chrome_request(chrome, "attach",
                            "{\"session_id\":\"abandoned\",\"turn_id\":\"1\","
                            "\"session_context\":\"live\",\"tabId\":7}",
                            &result, &error);
  } else if (rc == CAI_OK && argc == 4 && strcmp(argv[2], "method") == 0) {
    rc = cai_chrome_request(chrome, argv[3], "{}", &result, &error);
    if (rc == CAI_OK)
      printf("%s\n", result);
  } else if (rc == CAI_OK && argc > 3) {
    rc = cai_chrome_begin_turn(chrome, "fixture", "1", &error);
    cai_client_config_init(&client_config);
    client_config.api_key = "fixture";
    cai_agent_config_init(&agent_config);
    agent_config.model = "gpt-5.6-luna";
    if (rc == CAI_OK)
      rc = cai_client_open(&client_config, &client, &error);
    if (rc == CAI_OK)
      rc = cai_client_new_agent(client, &agent_config, &agent, &error);
    if (rc == CAI_OK)
      rc = cai_agent_register_chrome_tool(agent, chrome, &error);
    if (rc == CAI_OK)
      rc = cai_sink_stdout(&sink, &error);
    if (rc == CAI_OK)
      rc = cai_tool_registry_run(CAI_AGENT_IMPL(agent)->tools, "chrome",
                                 argv[3], sink, &error);
  } else if (rc == CAI_OK && strcmp(argv[2], "discover") != 0 &&
             strcmp(argv[2], "open") != 0) {
    if (strcmp(argv[2], "cancel") == 0) {
      clock_gettime(CLOCK_MONOTONIC, &state.start);
      cancellation.check = cancel_after;
      cancellation.context = &state;
      cai_stream_set_thread_cancel_scope(&cancellation);
    }
    rc = cai_chrome_request(chrome, "getUserTabs", "{}", &result, &error);
    cai_stream_set_thread_cancel_scope(NULL);
    if (rc == CAI_OK)
      printf("%s\n", result);
  }
  if (rc != CAI_OK)
    fprintf(stderr, "status=%d %s %s\n", rc,
            error.message != NULL ? error.message : "failed",
            error.detail != NULL ? error.detail : "");
  cai_sink_close(sink);
  if (agent != NULL)
    agent->close(agent);
  if (client != NULL)
    client->close(client);
  cai_chrome_close(chrome);
  cai_chrome_result_free(result);
  cai_error_cleanup(&error);
  return rc == CAI_OK ? 0 : 1;
}
