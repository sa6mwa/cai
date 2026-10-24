#include <assert.h>
#include <cai/agent_runtime.h>
#include <pthread.h>
#include <string.h>

typedef struct tools {
  int reasoning, other;
} tools;
static int visit(void *context, const cai_agent_tool_info *tool,
                 cai_error *error) {
  tools *found = context;
  (void)error;
  if (strcmp(tool->name, "set_reasoning") == 0) {
    found->reasoning++;
    assert(strstr(tool->description, "explicitly instructed") != NULL);
    assert(strstr(tool->description, "continue the task") != NULL);
  } else
    found->other++;
  return CAI_OK;
}
static void *wrong_thread(void *context) {
  cai_reasoning_tool_config config;
  cai_error error;
  cai_error_init(&error);
  assert(cai_agent_runtime_get_reasoning_tool(context, &config, &error) ==
         CAI_ERR_INVALID);
  cai_error_cleanup(&error);
  return NULL;
}
int main(int argc, char **argv) {
  cai_client_config client_config;
  cai_agent_runtime_config config;
  cai_reasoning_tool_config settings;
  cai_agent_runtime_settings effective;
  cai_client *client = NULL;
  cai_agent_runtime *runtime = NULL;
  cai_error error;
  tools before, after;
  char normal[] = "low", high[] = "xhigh";
  pthread_t thread;
  assert(argc == 2);
  cai_error_init(&error);
  cai_client_config_init(&client_config);
  client_config.api_key = "fixture";
  client_config.base_url = "http://127.0.0.1:1/v1";
  client_config.metadata_provider = "custom";
  client_config.model_metadata_json =
      "{\"models\":[{\"provider\":\"custom\",\"id\":\"fixture\","
      "\"reasoning_efforts\":[\"low\",\"medium\",\"high\",\"xhigh\"]}]}";
  assert(cai_client_open(&client_config, &client, &error) == CAI_OK);
  cai_agent_runtime_config_init(&config);
  assert(!config.reasoning_tool.enabled);
  assert(strcmp(config.reasoning_tool.normal_effort, "medium") == 0);
  assert(strcmp(config.reasoning_tool.high_effort, "high") == 0);
  config.workspace_directory = argv[1];
  config.model = "fixture";
  config.disable_terminal = config.disable_chrome = 1;
  config.disable_default_session_store = config.disable_skills = 1;
  assert(cai_agent_runtime_open(client, &config, &runtime, &error) == CAI_OK);
  memset(&before, 0, sizeof(before));
  assert(cai_agent_runtime_visit_tools(runtime, visit, &before, &error) ==
         CAI_OK);
  assert(before.reasoning == 0);
  assert(cai_agent_runtime_get_reasoning_tool(runtime, &settings, &error) ==
         CAI_OK);
  settings.enabled = 1;
  settings.normal_effort = normal;
  settings.high_effort = high;
  assert(cai_agent_runtime_set_reasoning_tool(runtime, &settings, &error) ==
         CAI_OK);
  normal[0] = high[0] = 'X';
  assert(cai_agent_runtime_get_reasoning_tool(runtime, &settings, &error) ==
         CAI_OK);
  assert(settings.enabled && strcmp(settings.normal_effort, "low") == 0 &&
         strcmp(settings.high_effort, "xhigh") == 0);
  memset(&after, 0, sizeof(after));
  assert(cai_agent_runtime_visit_tools(runtime, visit, &after, &error) ==
         CAI_OK);
  assert(after.reasoning == 1 && after.other == before.other);
  assert(cai_agent_runtime_get_settings(runtime, &effective, NULL, &error) ==
         CAI_OK);
  assert(strcmp(effective.reasoning_effort, "medium") == 0);
  settings.high_effort = "invalid";
  assert(cai_agent_runtime_set_reasoning_tool(runtime, &settings, &error) ==
         CAI_ERR_INVALID);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  assert(cai_agent_runtime_get_reasoning_tool(runtime, &settings, &error) ==
         CAI_OK);
  assert(strcmp(settings.high_effort, "xhigh") == 0);
  settings.enabled = 0;
  assert(cai_agent_runtime_set_reasoning_tool(runtime, &settings, &error) ==
         CAI_OK);
  memset(&after, 0, sizeof(after));
  assert(cai_agent_runtime_visit_tools(runtime, visit, &after, &error) ==
         CAI_OK);
  assert(after.reasoning == 0 && after.other == before.other);
  assert(pthread_create(&thread, NULL, wrong_thread, runtime) == 0);
  assert(pthread_join(thread, NULL) == 0);
  cai_agent_runtime_close(runtime);
  cai_client_close(client);
  cai_error_cleanup(&error);
  return 0;
}
