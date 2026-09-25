#include "../cli/options.h"

#include <cai/models.h>

#include <stdio.h>
#include <string.h>

int main(void) {
  cai_cli_options options;
  char defaults_raw[][32] = {"cai"};
  char configured_raw[][32] = {"cai",
                               "-N",
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
                            "--developer-instructions",
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
  char seeded_raw[][128] = {
      "cai",         "-Nni", "first turn",    "--instruction",
      "second turn", "-C",   "/tmp/project",  "-I",
      "developer",   "-g",   "finish project"};
  char *seeded[11];
  char review_raw[][128] = {"cai", "--review",  "--base", "trunk",
                            "-o",  "review.md", "-T",     "json"};
  char *review[8];
  char fix_raw[][64] = {"cai", "--review-and-fix"};
  char *fix[2];
  char invalid_review_raw[][64] = {"cai",   "--review", "--base",
                                   "trunk", "-i",       "custom"};
  char *invalid_review[6];
  char invalid_noninteractive_raw[][32] = {"cai", "-n"};
  char invalid_review_subagent_raw[][32] = {"cai", "--review-and-fix",
                                            "--no-review-subagent"};
  char invalid_output_type_raw[][32] = {"cai", "--review", "-T", "xml"};
  char invalid_output_without_review_raw[][32] = {"cai", "-o", "review.md"};
  char invalid_legacy_instructions_raw[][32] = {"cai", "--instructions", "old"};
  char invalid_legacy_workspace_raw[][32] = {"cai", "--workspace", "/tmp"};
  char *invalid_noninteractive[2];
  char *invalid_review_subagent[3];
  char *invalid_output_type[4];
  char *invalid_output_without_review[3];
  char *invalid_legacy_instructions[3];
  char *invalid_legacy_workspace[3];
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
  for (i = 0U; i < sizeof(seeded) / sizeof(seeded[0]); i++)
    seeded[i] = seeded_raw[i];
  for (i = 0U; i < sizeof(review) / sizeof(review[0]); i++)
    review[i] = review_raw[i];
  for (i = 0U; i < sizeof(fix) / sizeof(fix[0]); i++)
    fix[i] = fix_raw[i];
  for (i = 0U; i < sizeof(invalid_review) / sizeof(invalid_review[0]); i++)
    invalid_review[i] = invalid_review_raw[i];
  for (i = 0U;
       i < sizeof(invalid_noninteractive) / sizeof(invalid_noninteractive[0]);
       i++)
    invalid_noninteractive[i] = invalid_noninteractive_raw[i];
  for (i = 0U;
       i < sizeof(invalid_review_subagent) / sizeof(invalid_review_subagent[0]);
       i++)
    invalid_review_subagent[i] = invalid_review_subagent_raw[i];
  for (i = 0U; i < sizeof(invalid_output_type) / sizeof(invalid_output_type[0]);
       i++)
    invalid_output_type[i] = invalid_output_type_raw[i];
  for (i = 0U; i < sizeof(invalid_output_without_review) /
                       sizeof(invalid_output_without_review[0]);
       i++)
    invalid_output_without_review[i] = invalid_output_without_review_raw[i];
  for (i = 0U; i < sizeof(invalid_legacy_instructions) /
                       sizeof(invalid_legacy_instructions[0]);
       i++)
    invalid_legacy_instructions[i] = invalid_legacy_instructions_raw[i];
  for (i = 0U; i < sizeof(invalid_legacy_workspace) /
                       sizeof(invalid_legacy_workspace[0]);
       i++)
    invalid_legacy_workspace[i] = invalid_legacy_workspace_raw[i];
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
      strcmp(options.directory, "/tmp/project") != 0 ||
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
      strcmp(options.developer_instructions, "Be precise") != 0 ||
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
  if (cai_cli_parse_options(11, seeded, &options) != 1 ||
      !options.new_session || !options.non_interactive ||
      options.instruction_count != 2U ||
      strcmp(cai_cli_instruction_at(&options, 0U), "first turn") != 0 ||
      strcmp(cai_cli_instruction_at(&options, 1U), "second turn") != 0 ||
      cai_cli_instruction_at(&options, 2U) != NULL ||
      strcmp(options.directory, "/tmp/project") != 0 ||
      strcmp(options.developer_instructions, "developer") != 0 ||
      strcmp(options.goal, "finish project") != 0)
    return 1;
  if (cai_cli_parse_options(8, review, &options) != 1 || !options.review ||
      strcmp(options.base, "trunk") != 0 ||
      strcmp(options.out, "review.md") != 0 ||
      strcmp(options.output_type, "json") != 0)
    return 1;
  if (cai_cli_parse_options(2, fix, &options) != 1 || !options.review_and_fix ||
      !options.new_session || !options.non_interactive ||
      cai_cli_parse_options(6, invalid_review, &options) != -1)
    return 1;
  if (cai_cli_parse_options(2, invalid_noninteractive, &options) != -1 ||
      cai_cli_parse_options(3, invalid_review_subagent, &options) != -1 ||
      cai_cli_parse_options(4, invalid_output_type, &options) != -1 ||
      cai_cli_parse_options(3, invalid_output_without_review, &options) != -1 ||
      cai_cli_parse_options(3, invalid_legacy_instructions, &options) != -1 ||
      cai_cli_parse_options(3, invalid_legacy_workspace, &options) != -1)
    return 1;
  return 0;
}
