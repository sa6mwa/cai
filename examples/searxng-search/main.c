#include <cai/cai.h>
#include <cai/tools/searxng.h>

#include "../common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *example_model(void) {
  const char *model;

  model = getenv("CAI_EXAMPLE_MODEL");
  if (model == NULL || model[0] == '\0') {
    return CAI_MODEL_GPT_5_NANO;
  }
  return model;
}

static const char *example_searxng_base_url(void) {
  const char *base_url;

  base_url = getenv("CAI_SEARXNG_BASE_URL");
  if (base_url == NULL || base_url[0] == '\0') {
    return CAI_SEARXNG_DEFAULT_BASE_URL;
  }
  return base_url;
}

static int print_error(const char *operation, int rc, const cai_error *error) {
  fprintf(stderr, "%s failed: %s\n", operation,
          error->message != NULL ? error->message : cai_status_string(rc));
  if (error->detail != NULL) {
    fprintf(stderr, "detail: %s\n", error->detail);
  }
  return 1;
}

static void build_prompt(char *buffer, size_t capacity, const char *query) {
  snprintf(buffer, capacity,
           "Search the web for: %s. Use the searxng_search tool if current or "
           "external information is useful. Answer with a concise summary and "
           "include the source URL from the tool result.",
           query);
}

int main(int argc, char **argv) {
  const char *query;
  const char *engine;
  char prompt[1024];
  cai_client_config client_config;
  cai_agent_config agent_config;
  cai_run_options run_options;
  cai_searxng_tool_config searxng_config;
  cai_client *client;
  cai_agent *agent;
  cai_output *output;
  cai_error error;
  char *dotenv_api_key;
  int rc;
  int exit_code;

  query = argc > 1 ? argv[1] : "OpenAI Responses API";
  engine = getenv("CAI_SEARXNG_ENGINE");
  build_prompt(prompt, sizeof(prompt), query);

  cai_error_init(&error);
  cai_client_config_init(&client_config);
  cai_agent_config_init(&agent_config);
  cai_run_options_init(&run_options);
  memset(&searxng_config, 0, sizeof(searxng_config));

  agent_config.model = example_model();
  agent_config.developer_instructions =
      "You are a concise web-search assistant. For web search, call "
      "searxng_search and base search facts on the tool result.";
  agent_config.reasoning_effort = CAI_REASONING_EFFORT_MINIMAL;
  agent_config.max_output_tokens = 512;
  agent_config.prompt_cache_key = "cai:example:searxng-search:v1";
  run_options.max_tool_rounds = 6;

  searxng_config.base_url = example_searxng_base_url();
  if (engine != NULL && engine[0] != '\0') {
    searxng_config.engine = engine;
  }

  client = NULL;
  agent = NULL;
  output = NULL;
  dotenv_api_key = NULL;
  exit_code = 1;

  rc = cai_example_load_dotenv_api_key(&client_config, &dotenv_api_key, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_load_dotenv_api_key", rc, &error);
    goto done;
  }
  rc = cai_client_open(&client_config, &client, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_client_open", rc, &error);
    goto done;
  }
  rc = client->new_agent(client, &agent_config, &agent, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_client_new_agent", rc, &error);
    goto done;
  }
  rc = cai_agent_register_searxng_tool(agent, &searxng_config, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_agent_register_searxng_tool", rc, &error);
    goto done;
  }
  rc = agent->add_user_text(agent, prompt, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_agent_add_user_text", rc, &error);
    goto done;
  }
  rc = agent->run_auto_output(agent, &run_options, &output, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_agent_run_auto_output", rc, &error);
    goto done;
  }
  printf("%s\n", cai_output_text(output) != NULL ? cai_output_text(output) : "");
  exit_code = 0;

done:
  cai_output_destroy(output);
  if (agent != NULL) {
    agent->close(agent);
  }
  if (client != NULL) {
    client->close(client);
  }
  cai_string_destroy(dotenv_api_key);
  cai_error_cleanup(&error);
  return exit_code;
}
