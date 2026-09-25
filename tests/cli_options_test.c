#include "../cli/options.h"

#include <cai/models.h>

#include <stdio.h>
#include <string.h>

int main(void) {
  cai_cli_options options;
  char defaults_raw[][32] = {"cai"};
  char configured_raw[][32] = {"cai",
                               "-n",
                               "-C",
                               "/tmp/project",
                               "-m",
                               "gpt-5.6-luna",
                               "-r",
                               "high",
                               "--skills-dir",
                               "/tmp/skills",
                               "--no-image-generation"};
  char conflict_raw[][32] = {"cai", "--new", "--resume", "session-id"};
  char invalid_raw[][32] = {"cai", "--reasoning-effort", "unknown"};
  char runtime_raw[][64] = {"cai",
                            "--resume",
                            "session-id",
                            "-a",
                            "/tmp/auth.json",
                            "--reasoning-summary",
                            "concise",
                            "--review-model",
                            "review-model",
                            "--review-reasoning-effort",
                            "low",
                            "--review-reasoning-summary",
                            "detailed",
                            "--agents-md",
                            "/tmp/AGENTS.md",
                            "--config-dir",
                            "/tmp/config",
                            "--identity",
                            "Smith",
                            "--instructions",
                            "Be precise",
                            "--codex-agents-md",
                            "--no-terminal",
                            "--no-review-subagent",
                            "-vv"};
  char invalid_summary_raw[][64] = {"cai", "--review-reasoning-summary",
                                    "invalid"};
  char *defaults[1];
  char *configured[11];
  char *conflict[4];
  char *invalid[3];
  char *runtime[sizeof(runtime_raw) / sizeof(runtime_raw[0])];
  char *invalid_summary[3];
  char openrouter_raw[][64] = {"cai", "-p", "openrouter"};
  char *openrouter[3];
  char custom_raw[][64] = {
      "cai", "-p",           "custom", "--endpoint", "https://example.test/v1",
      "-m",  "example-model"};
  char *custom[7];
  char custom_env_raw[][64] = {"cai",
                               "--provider",
                               "custom",
                               "--endpoint",
                               "http://localhost:1234/v1",
                               "--api-key-env",
                               "LOCAL_KEY",
                               "--model",
                               "local"};
  char *custom_env[9];
  char missing_model_raw[][64] = {"cai", "-p", "custom", "--endpoint",
                                  "https://example.test/v1"};
  char *missing_model[5];
  char invalid_provider_raw[][64] = {"cai", "-p", "invalid"};
  char *invalid_provider[3];
  char invalid_login_raw[][64] = {"cai", "-p", "openai", "-l"};
  char *invalid_login[4];
  char login_raw[][64] = {"cai", "--login"};
  char *login[2];
  size_t i;

  for (i = 0U; i < sizeof(openrouter) / sizeof(openrouter[0]); i++)
    openrouter[i] = openrouter_raw[i];
  for (i = 0U; i < sizeof(custom) / sizeof(custom[0]); i++)
    custom[i] = custom_raw[i];
  for (i = 0U; i < sizeof(custom_env) / sizeof(custom_env[0]); i++)
    custom_env[i] = custom_env_raw[i];
  for (i = 0U; i < sizeof(missing_model) / sizeof(missing_model[0]); i++)
    missing_model[i] = missing_model_raw[i];
  for (i = 0U; i < sizeof(invalid_provider) / sizeof(invalid_provider[0]); i++)
    invalid_provider[i] = invalid_provider_raw[i];
  for (i = 0U; i < sizeof(invalid_login) / sizeof(invalid_login[0]); i++)
    invalid_login[i] = invalid_login_raw[i];
  for (i = 0U; i < sizeof(login) / sizeof(login[0]); i++)
    login[i] = login_raw[i];
  defaults[0] = defaults_raw[0];
  for (i = 0U; i < 11U; i++)
    configured[i] = configured_raw[i];
  for (i = 0U; i < 4U; i++)
    conflict[i] = conflict_raw[i];
  for (i = 0U; i < 3U; i++)
    invalid[i] = invalid_raw[i];
  for (i = 0U; i < sizeof(runtime) / sizeof(runtime[0]); i++)
    runtime[i] = runtime_raw[i];
  for (i = 0U; i < 3U; i++)
    invalid_summary[i] = invalid_summary_raw[i];

  if (cai_cli_parse_options(1, defaults, &options) != 1 ||
      strcmp(options.model, CAI_MODEL_GPT_6_LUNA) != 0 ||
      strcmp(options.reasoning_effort, "medium") != 0 ||
      strcmp(options.reasoning_summary, "concise") != 0 ||
      options.new_session || !options.image_generation || !options.terminal) {
    fputs("CLI defaults failed\n", stderr);
    return 1;
  }
  if (cai_cli_parse_options(11, configured, &options) != 1 ||
      strcmp(options.workspace, "/tmp/project") != 0 ||
      strcmp(options.model, "gpt-5.6-luna") != 0 ||
      strcmp(options.reasoning_effort, "high") != 0 ||
      strcmp(options.skills_dir, "/tmp/skills") != 0 || !options.new_session ||
      options.image_generation) {
    fputs("CLI configured options failed\n", stderr);
    return 1;
  }
  if (cai_cli_parse_options(4, conflict, &options) != -1 ||
      cai_cli_parse_options(3, invalid, &options) != -1 ||
      cai_cli_parse_options(3, invalid_summary, &options) != -1) {
    fputs("CLI invalid options were accepted\n", stderr);
    return 1;
  }
  if (cai_cli_parse_options((int)(sizeof(runtime) / sizeof(runtime[0])),
                            runtime, &options) != 1 ||
      strcmp(options.resume_id, "session-id") != 0 ||
      strcmp(options.auth_json, "/tmp/auth.json") != 0 ||
      strcmp(options.reasoning_summary, "concise") != 0 ||
      strcmp(options.review_model, "review-model") != 0 ||
      strcmp(options.review_reasoning_effort, "low") != 0 ||
      strcmp(options.review_reasoning_summary, "detailed") != 0 ||
      strcmp(options.agents_md, "/tmp/AGENTS.md") != 0 ||
      strcmp(options.config_dir, "/tmp/config") != 0 ||
      strcmp(options.identity, "Smith") != 0 ||
      strcmp(options.instructions, "Be precise") != 0 ||
      !options.codex_agents_md || options.terminal || options.review_subagent ||
      options.verbosity != 2) {
    fputs("CLI runtime settings failed\n", stderr);
    return 1;
  }
  if (strcmp(options.provider, "chatgpt") != 0 || options.login)
    return 1;
  if (cai_cli_parse_options(3, openrouter, &options) != 1 ||
      strcmp(options.model, CAI_OPENROUTER_MODEL_DEFAULT_RESPONSES) != 0 ||
      strcmp(options.provider, "openrouter") != 0)
    return 1;
  if (cai_cli_parse_options(7, custom, &options) != 1 ||
      strcmp(options.api_key_env, "CAI_API_KEY") != 0 ||
      strcmp(options.model, "example-model") != 0)
    return 1;
  if (cai_cli_parse_options(9, custom_env, &options) != 1 ||
      strcmp(options.api_key_env, "LOCAL_KEY") != 0)
    return 1;
  if (cai_cli_parse_options(2, login, &options) != 1 || !options.login ||
      cai_cli_parse_options(5, missing_model, &options) != -1 ||
      cai_cli_parse_options(3, invalid_provider, &options) != -1 ||
      cai_cli_parse_options(4, invalid_login, &options) != -1) {
    fputs("CLI provider options failed\n", stderr);
    return 1;
  }
  return 0;
}
