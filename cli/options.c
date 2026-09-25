#include "options.h"

#include <cai/models.h>
#include <cai/version.h>

#include <stdio.h>
#include <string.h>

void cai_cli_options_init(cai_cli_options *options) {
  memset(options, 0, sizeof(*options));
  options->model = CAI_MODEL_GPT_6_LUNA;
  options->reasoning_effort = "medium";
  options->reasoning_summary = "auto";
  options->image_generation = 1;
  options->terminal = 1;
  options->review_subagent = 1;
}

static void cai_cli_help(void) {
  fputs(
      "Usage: cai [options]\n\n"
      "Start Cai Smith in the workspace directory. The newest session resumes\n"
      "automatically; use --new to start a fresh one. In the prompt, /resume\n"
      "lists sessions for this directory and /resume N opens entry N.\n\n",
      stdout);
  fputs("  -n, --new                    Start a new session\n"
        "      --resume ID              Resume a session by ID\n"
        "  -C, --workspace DIR          Use DIR as the workspace\n"
        "  -a, --auth-json PATH         ChatGPT auth file (default "
        "~/.codex/auth.json)\n"
        "  -m, --model ID               Model (default gpt-6-luna)\n"
        "  -r, --reasoning-effort LEVEL none|low|medium|high|xhigh|max\n"
        "      --reasoning-summary MODE none|auto|concise|detailed\n",
        stdout);
  fputs("      --review-model ID        Model for review children\n"
        "      --review-reasoning-effort LEVEL\n"
        "      --review-reasoning-summary MODE\n"
        "      --agents-md PATH        Global AGENTS.md path\n"
        "      --config-dir DIR        Global agent config directory\n"
        "      --skills-dir DIR        Global skills directory\n"
        "      --identity TEXT         Visible agent identity\n"
        "      --instructions TEXT     Append developer instructions\n",
        stdout);
  fputs("      --codex-agents-md       Discover ancestor AGENTS.md files\n"
        "      --no-image-generation   Disable hosted image generation\n"
        "      --no-terminal           Disable terminal tools\n"
        "      --no-review-subagent    Disable the built-in reviewer\n"
        "  -v, --verbose                Print runtime events (repeat for "
        "sequences)\n"
        "  -h, --help                   Show this help\n"
        "      --version                Show version\n",
        stdout);
}

static int cai_cli_one_of(const char *value, const char *const *values) {
  size_t i;
  for (i = 0U; values[i] != NULL; i++) {
    if (strcmp(value, values[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

int cai_cli_parse_options(int argc, char *const *argv,
                          cai_cli_options *options) {
  static const char *const efforts[] = {"none",  "low", "medium", "high",
                                        "xhigh", "max", NULL};
  static const char *const summaries[] = {"none", "auto", "concise", "detailed",
                                          NULL};
  int i;

  if (options == NULL) {
    return -1;
  }
  cai_cli_options_init(options);
  for (i = 1; i < argc; i++) {
    const char *flag;
    const char *value;
    const char **field;

    flag = argv[i];
    if (strcmp(flag, "-h") == 0 || strcmp(flag, "--help") == 0) {
      cai_cli_help();
      return 0;
    }
    if (strcmp(flag, "--version") == 0) {
      puts(CAI_VERSION_STRING);
      return 0;
    }
    if (strcmp(flag, "-n") == 0 || strcmp(flag, "--new") == 0) {
      options->new_session = 1;
      continue;
    }
    if (strcmp(flag, "--no-image-generation") == 0) {
      options->image_generation = 0;
      continue;
    }
    if (strcmp(flag, "--no-terminal") == 0) {
      options->terminal = 0;
      continue;
    }
    if (strcmp(flag, "--no-review-subagent") == 0) {
      options->review_subagent = 0;
      continue;
    }
    if (strcmp(flag, "--codex-agents-md") == 0) {
      options->codex_agents_md = 1;
      continue;
    }
    if (strcmp(flag, "-v") == 0 || strcmp(flag, "--verbose") == 0) {
      options->verbosity++;
      continue;
    }
    if (strcmp(flag, "-vv") == 0) {
      options->verbosity += 2;
      continue;
    }
    field = NULL;
    if (strcmp(flag, "--resume") == 0)
      field = &options->resume_id;
    else if (strcmp(flag, "-C") == 0 || strcmp(flag, "--workspace") == 0)
      field = &options->workspace;
    else if (strcmp(flag, "-a") == 0 || strcmp(flag, "--auth-json") == 0)
      field = &options->auth_json;
    else if (strcmp(flag, "-m") == 0 || strcmp(flag, "--model") == 0)
      field = &options->model;
    else if (strcmp(flag, "-r") == 0 || strcmp(flag, "--reasoning-effort") == 0)
      field = &options->reasoning_effort;
    else if (strcmp(flag, "--reasoning-summary") == 0)
      field = &options->reasoning_summary;
    else if (strcmp(flag, "--review-model") == 0)
      field = &options->review_model;
    else if (strcmp(flag, "--review-reasoning-effort") == 0)
      field = &options->review_reasoning_effort;
    else if (strcmp(flag, "--review-reasoning-summary") == 0)
      field = &options->review_reasoning_summary;
    else if (strcmp(flag, "--agents-md") == 0)
      field = &options->agents_md;
    else if (strcmp(flag, "--config-dir") == 0)
      field = &options->config_dir;
    else if (strcmp(flag, "--skills-dir") == 0)
      field = &options->skills_dir;
    else if (strcmp(flag, "--identity") == 0)
      field = &options->identity;
    else if (strcmp(flag, "--instructions") == 0)
      field = &options->instructions;
    if (field == NULL) {
      fprintf(stderr, "cai: unknown option: %s\n", flag);
      return -1;
    }
    if (++i >= argc || argv[i][0] == '\0') {
      fprintf(stderr, "cai: %s requires a value\n", flag);
      return -1;
    }
    value = argv[i];
    *field = value;
    if ((field == &options->reasoning_effort ||
         field == &options->review_reasoning_effort) &&
        !cai_cli_one_of(value, efforts)) {
      fprintf(stderr, "cai: invalid reasoning effort: %s\n", value);
      return -1;
    }
    if ((field == &options->reasoning_summary ||
         field == &options->review_reasoning_summary) &&
        !cai_cli_one_of(value, summaries)) {
      fprintf(stderr, "cai: invalid reasoning summary: %s\n", value);
      return -1;
    }
  }
  if (options->new_session && options->resume_id != NULL) {
    fputs("cai: --new and --resume cannot be combined\n", stderr);
    return -1;
  }
  return 1;
}
