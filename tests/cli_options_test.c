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
  size_t i;

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
      strcmp(options.reasoning_effort, "medium") != 0 || options.new_session ||
      !options.image_generation || !options.terminal) {
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
  return 0;
}
