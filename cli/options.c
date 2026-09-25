#include "options.h"

#include <cai/models.h>
#include <cai/version.h>

#include <stdio.h>
#include <string.h>

void cai_cli_options_init(cai_cli_options *options) {
  memset(options, 0, sizeof(*options));
  options->provider = "chatgpt";
  options->model = CAI_MODEL_GPT_6_LUNA;
  options->reasoning_effort = "medium";
  options->reasoning_summary = "concise";
  options->image_generation = 1;
  options->terminal = 1;
  options->review_subagent = 1;
}

static void cai_cli_help(void) {
  fputs("cai and libcai Copyright (C) 2026 C89 Systems AB "
        "https://c89.systems\n\n",
        stdout);
  fputs(
      "Usage: cai [options]\n\n"
      "Start Cai Smith in the workspace directory. The newest session resumes\n"
      "automatically; use --new to start a fresh one. In the prompt, /resume\n"
      "lists sessions for this directory and /resume N opens entry N.\n\n",
      stdout);
  fputs("  -N, --new                    Start a new session\n"
        "  -n, --non-interactive        Run supplied work and exit\n"
        "      --resume ID              Resume a session by ID\n"
        "  -C, --directory DIR          Change to DIR before starting\n"
        "  -i, --instruction TEXT       Submit a prompt (repeatable)\n"
        "  -g, --goal TEXT              Start or replace a durable goal\n"
        "      --review                 Run one isolated review and exit\n"
        "      --review-and-fix         Review and fix in a new goal session\n",
        stdout);
  fputs("      --base REF               Review changes against REF\n"
        "  -o, --out FILE               Write review findings to FILE\n"
        "  -T, --output-type TYPE       Review findings: markdown|json\n"
        "  -a, --auth-json PATH         ChatGPT auth file (default "
        "~/.codex/auth.json, then cai state)\n"
        "  -l, --login                  Log in to ChatGPT and exit\n"
        "  -p, --provider NAME          chatgpt|openai|openrouter|custom\n",
        stdout);
  fputs("      --endpoint URL           Custom provider API base URL\n"
        "      --api-key-env NAME       Custom API key variable (default "
        "CAI_API_KEY)\n",
        stdout);
  fputs("  -m, --model ID               Model (provider default if omitted)\n"
        "  -r, --reasoning-effort LEVEL none|low|medium|high|xhigh|max\n"
        "      --reasoning-summary MODE none|auto|concise|detailed (default "
        "concise)\n",
        stdout);
  fputs("      --review-model ID        Model for review children\n"
        "      --review-reasoning-effort LEVEL\n"
        "      --review-reasoning-summary MODE\n"
        "      --agents-md PATH        Global AGENTS.md path\n"
        "      --config-dir DIR        Global agent config directory\n"
        "      --skills-dir DIR        Global skills directory\n"
        "      --identity TEXT         Visible agent identity\n"
        "  -I, --developer-instructions TEXT\n"
        "                               Append developer instructions\n",
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
  static const char *const providers[] = {"chatgpt", "openai", "openrouter",
                                          "custom", NULL};
  int model_explicit = 0;
  int i;
  size_t j;

  if (options == NULL) {
    return -1;
  }
  cai_cli_options_init(options);
  options->argc = argc;
  options->argv = argv;
  for (i = 1; i < argc; i++) {
    const char *flag;
    const char *value;
    const char **field;
    char short_flag[3];

    flag = argv[i];
    if (flag[0] == '-' && flag[1] != '-' && flag[2] != '\0' &&
        strcmp(flag, "-vv") != 0) {
      for (j = 1U; flag[j] != '\0'; j++) {
        if (flag[j] == 'N')
          options->new_session = 1;
        else if (flag[j] == 'n')
          options->non_interactive = 1;
        else if (flag[j] == 'v')
          options->verbosity++;
        else if (flag[j + 1U] == '\0' && (flag[j] == 'i' || flag[j] == 'g')) {
          short_flag[0] = '-';
          short_flag[1] = flag[j];
          short_flag[2] = '\0';
          flag = short_flag;
          break;
        } else {
          fprintf(stderr, "cai: invalid short option group: %s\n", argv[i]);
          return -1;
        }
      }
      if (flag != short_flag)
        continue;
    }
    if (strcmp(flag, "-h") == 0 || strcmp(flag, "--help") == 0) {
      cai_cli_help();
      return 0;
    }
    if (strcmp(flag, "--version") == 0) {
      puts(CAI_VERSION_STRING);
      return 0;
    }
    if (strcmp(flag, "-N") == 0 || strcmp(flag, "--new") == 0) {
      options->new_session = 1;
      continue;
    }
    if (strcmp(flag, "-n") == 0 || strcmp(flag, "--non-interactive") == 0) {
      options->non_interactive = 1;
      continue;
    }
    if (strcmp(flag, "--review") == 0) {
      options->review = 1;
      continue;
    }
    if (strcmp(flag, "--review-and-fix") == 0) {
      options->review_and_fix = 1;
      options->new_session = 1;
      options->non_interactive = 1;
      continue;
    }
    if (strcmp(flag, "-l") == 0 || strcmp(flag, "--login") == 0) {
      options->login = 1;
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
    if (strcmp(flag, "-i") == 0 || strcmp(flag, "--instruction") == 0) {
      if (++i >= argc || argv[i][0] == '\0') {
        fprintf(stderr, "cai: %s requires a value\n", flag);
        return -1;
      }
      options->instruction_count++;
      continue;
    }
    if (strcmp(flag, "--resume") == 0)
      field = &options->resume_id;
    else if (strcmp(flag, "-g") == 0 || strcmp(flag, "--goal") == 0)
      field = &options->goal;
    else if (strcmp(flag, "--base") == 0)
      field = &options->base;
    else if (strcmp(flag, "-o") == 0 || strcmp(flag, "--out") == 0)
      field = &options->out;
    else if (strcmp(flag, "-T") == 0 || strcmp(flag, "--output-type") == 0)
      field = &options->output_type;
    else if (strcmp(flag, "-p") == 0 || strcmp(flag, "--provider") == 0)
      field = &options->provider;
    else if (strcmp(flag, "--endpoint") == 0)
      field = &options->endpoint;
    else if (strcmp(flag, "--api-key-env") == 0)
      field = &options->api_key_env;
    else if (strcmp(flag, "-C") == 0 || strcmp(flag, "--directory") == 0)
      field = &options->directory;
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
    else if (strcmp(flag, "-I") == 0 ||
             strcmp(flag, "--developer-instructions") == 0)
      field = &options->developer_instructions;
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
    if (field == &options->model)
      model_explicit = 1;
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
  if (options->review && options->review_and_fix) {
    fputs("cai: --review and --review-and-fix cannot be combined\n", stderr);
    return -1;
  }
  if (options->review &&
      (options->new_session || options->resume_id != NULL ||
       options->goal != NULL || options->instruction_count > 1U ||
       (options->base != NULL && options->instruction_count != 0U))) {
    fputs("cai: --review accepts one of --base or one -i, without session or "
          "goal flags\n",
          stderr);
    return -1;
  }
  if (options->review_and_fix &&
      (options->goal != NULL || options->instruction_count != 0U)) {
    fputs("cai: --review-and-fix supplies its own goal and prompt\n", stderr);
    return -1;
  }
  if (options->review_and_fix && !options->review_subagent) {
    fputs("cai: --review-and-fix requires the review subagent\n", stderr);
    return -1;
  }
  if (options->base != NULL && !options->review && !options->review_and_fix) {
    fputs("cai: --base requires --review or --review-and-fix\n", stderr);
    return -1;
  }
  if ((options->out != NULL || options->output_type != NULL) &&
      !options->review) {
    fputs("cai: --out and --output-type require --review\n", stderr);
    return -1;
  }
  if (options->output_type != NULL &&
      strcmp(options->output_type, "markdown") != 0 &&
      strcmp(options->output_type, "json") != 0) {
    fputs("cai: --output-type must be markdown or json\n", stderr);
    return -1;
  }
  if (options->output_type == NULL)
    options->output_type = "markdown";
  if (options->non_interactive && !options->review &&
      !options->review_and_fix && options->instruction_count == 0U &&
      options->goal == NULL) {
    fputs("cai: --non-interactive requires -i, --goal, or a review mode\n",
          stderr);
    return -1;
  }
  if (!cai_cli_one_of(options->provider, providers)) {
    fprintf(stderr, "cai: invalid provider: %s\n", options->provider);
    return -1;
  }
  if (strcmp(options->provider, "chatgpt") != 0 &&
      (options->auth_json != NULL || options->login)) {
    fputs("cai: --auth-json and --login require --provider chatgpt\n", stderr);
    return -1;
  }
  if (options->login &&
      (options->new_session || options->resume_id != NULL ||
       options->non_interactive || options->review || options->review_and_fix ||
       options->goal != NULL || options->instruction_count != 0U)) {
    fputs("cai: --login cannot be combined with session work\n", stderr);
    return -1;
  }
  if (strcmp(options->provider, "custom") == 0) {
    if (options->endpoint == NULL || !model_explicit) {
      fputs("cai: custom provider requires --endpoint and --model\n", stderr);
      return -1;
    }
    if (strncmp(options->endpoint, "https://", 8U) != 0 &&
        strncmp(options->endpoint, "http://", 7U) != 0) {
      fputs("cai: --endpoint must start with https:// or http://\n", stderr);
      return -1;
    }
    if (options->api_key_env == NULL)
      options->api_key_env = "CAI_API_KEY";
  } else if (options->endpoint != NULL || options->api_key_env != NULL) {
    fputs("cai: --endpoint and --api-key-env require --provider custom\n",
          stderr);
    return -1;
  }
  if (strcmp(options->provider, "openrouter") == 0 && !model_explicit)
    options->model = CAI_OPENROUTER_MODEL_DEFAULT_RESPONSES;
  return 1;
}

const char *cai_cli_instruction_at(const cai_cli_options *options,
                                   size_t index) {
  size_t found;
  int i;
  const char *flag;
  int has_value;
  int instruction;

  if (options == NULL || options->argv == NULL)
    return NULL;
  found = 0U;
  for (i = 1; i < options->argc; i++) {
    flag = options->argv[i];
    instruction = strcmp(flag, "-i") == 0 ||
                  strcmp(flag, "--instruction") == 0 ||
                  (flag[0] == '-' && flag[1] != '-' && strlen(flag) > 2U &&
                   flag[strlen(flag) - 1U] == 'i');
    has_value =
        instruction || strcmp(flag, "--resume") == 0 ||
        strcmp(flag, "-g") == 0 || strcmp(flag, "--goal") == 0 ||
        strcmp(flag, "--base") == 0 || strcmp(flag, "-o") == 0 ||
        strcmp(flag, "--out") == 0 || strcmp(flag, "-T") == 0 ||
        strcmp(flag, "--output-type") == 0 || strcmp(flag, "-p") == 0 ||
        strcmp(flag, "--provider") == 0 || strcmp(flag, "--endpoint") == 0 ||
        strcmp(flag, "--api-key-env") == 0 || strcmp(flag, "-C") == 0 ||
        strcmp(flag, "--directory") == 0 || strcmp(flag, "-a") == 0 ||
        strcmp(flag, "--auth-json") == 0 || strcmp(flag, "-m") == 0 ||
        strcmp(flag, "--model") == 0 || strcmp(flag, "-r") == 0 ||
        strcmp(flag, "--reasoning-effort") == 0 ||
        strcmp(flag, "--reasoning-summary") == 0 ||
        strcmp(flag, "--review-model") == 0 ||
        strcmp(flag, "--review-reasoning-effort") == 0 ||
        strcmp(flag, "--review-reasoning-summary") == 0 ||
        strcmp(flag, "--agents-md") == 0 || strcmp(flag, "--config-dir") == 0 ||
        strcmp(flag, "--skills-dir") == 0 || strcmp(flag, "--identity") == 0 ||
        strcmp(flag, "-I") == 0 ||
        strcmp(flag, "--developer-instructions") == 0;
    if (has_value && i + 1 < options->argc) {
      if (instruction && found++ == index)
        return options->argv[i + 1];
      i++;
    }
  }
  return NULL;
}
