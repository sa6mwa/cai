#include "options.h"

#include <cai/models.h>
#include <cai/version.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void cai_cli_option_error(cai_cli_options *options, const char *format,
                                 ...) {
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(options->diagnostic, sizeof(options->diagnostic), format,
            arguments);
  va_end(arguments);
  options->diagnostic[strcspn(options->diagnostic, "\n")] = '\0';
}

void cai_cli_options_init(cai_cli_options *options) {
  memset(options, 0, sizeof(*options));
  options->provider = "chatgpt";
  options->model = CAI_MODEL_GPT_6_LUNA;
  options->reasoning_effort = "medium";
  options->reasoning_summary = "concise";
  options->image_generation = 1;
  options->terminal = 1;
  options->shell_context = 1;
  options->shell_context_max_lines = 5000U;
  options->shell_context_max_bytes = 4U * 1024U * 1024U;
  options->review_subagent = 1;
  options->chrome = 1;
  options->web_search = 1;
  options->read_file = options->list_files = options->apply_patch = 1;
  options->view_image = options->goals = options->skills = 1;
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
  fputs(
      "  -N, --new                    Start a new session\n"
      "  -n, --non-interactive        Run supplied work and exit\n"
      "      --resume [ID]            List directory sessions or resume ID\n"
      "  -l, --list                   List conversations in all directories\n"
      "      --export ID              Export resumable JSONL and Markdown\n"
      "      --import FILE            Import JSONL as a new session and exit\n"
      "      --export-dir DIR         Export root (default "
      "$XDG_DATA_HOME/cai/exports)\n",
      stdout);
  fputs("  -C, --directory DIR          Change to DIR before starting\n"
        "      --config FILE            Select config.ini\n"
        "      --no-config              Skip configuration and creation\n"
        "  -i, --instruction TEXT       Submit a prompt (repeatable)\n"
        "  -g, --goal TEXT              Start or replace a durable goal\n"
        "      --review                 Run one isolated review and exit\n"
        "      --review-and-fix         Review and fix in a new goal session\n",
        stdout);
  fputs(
      "      --base REF               Review changes against REF\n"
      "  -o, --out FILE               Write review findings to FILE\n"
      "  -T, --output-type TYPE       Review findings: markdown|json\n"
      "      --tool-output MODE       Tool previews: compact|full (interactive "
      "default compact)\n"
      "  -a, --auth-json PATH         ChatGPT auth file (default "
      "~/.codex/auth.json, then encrypted cai store)\n"
      "      --login                  Log in to ChatGPT and exit\n"
      "      --lockd ENDPOINT         Storage endpoint (default "
      "state/cai/pouch)\n",
      stdout);
  fputs("      --lockd-client-pem FILE  Remote lockd mTLS PEM bundle\n"
        "  -p, --provider NAME          "
        "chatgpt|openai|openrouter|berget|custom\n",
        stdout);
  fputs("      --endpoint URL           Custom provider API base URL\n"
        "      --api-key-env NAME       Custom API key variable (default "
        "CAI_API_KEY)\n",
        stdout);
  fputs(
      "      --chrome / --no-chrome   Chrome extension tools (default on)\n"
      "      --web-search / --no-web-search  Hosted Responses search (default "
      "on)\n"
      "      --todo / --no-todo       Encrypted kanban tool (default off)\n"
      "      --searxng / --no-searxng  Local search tool (default off)\n"
      "      --searxng-url URL        Search base (default "
      "http://127.0.0.1:8888)\n"
      "      --revgeo / --no-revgeo    Reverse geocoding (default off)\n"
      "      --revgeo-url URL         Reverse geocoding provider base URL\n",
      stdout);
  fputs("      --[no-]read-file         File reading (default on)\n"
        "      --[no-]list-files        File listing (default on)\n"
        "      --[no-]apply-patch       Workspace patching (default on)\n"
        "      --[no-]view-image        Image inspection (default on)\n"
        "      --[no-]goals             Goal tools (default on)\n"
        "      --[no-]skills            Skill discovery and reading (default "
        "on)\n",
        stdout);
  fputs(
      "      --chrome-socket PATH    Select a connected CAI Chrome host\n"
      "      --chrome-setup          Register CAI as Chrome native host and "
      "exit\n"
      "      --chrome-remove         Remove CAI's own registration and exit\n"
      "      --chrome-manifest FILE  Alternate native-host registration path\n",
      stdout);
  fputs(
      "      --[no-]shell-context     Share !COMMAND results (not /shell, "
      "!sh)\n"
      "      --shell-context-max-lines N  Retain last N lines (default 5000)\n"
      "      --shell-context-max-bytes N  Bound retained text (default "
      "4194304)\n",
      stdout);
  fputs("      --model-metadata FILE    Provider model metadata JSON\n",
        stdout);
  fputs("  -m, --model ID               Model (provider default if omitted)\n"
        "  -r, --reasoning-effort LEVEL "
        "none|minimal|low|medium|high|xhigh|max|ultra\n"
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
  fputs("      --install-skill DIR     Install a SKILL.md package and exit\n"
        "      --replace               Explicitly replace an installed skill\n",
        stdout);
  fputs(
      "      --codex-agents-md       Load Codex global and ancestor AGENTS.md\n"
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

int cai_cli_read_options(int argc, char *const *argv,
                         const cai_cli_options *defaults,
                         cai_cli_options *options) {
  static const char *const efforts[] = {"none",   "minimal", "low",
                                        "medium", "high",    "xhigh",
                                        "max",    "ultra",   NULL};
  static const char *const summaries[] = {"none", "auto", "concise", "detailed",
                                          NULL};
  int i;
  size_t j;

  if (options == NULL) {
    return -1;
  }
  if (defaults != NULL)
    *options = *defaults;
  else
    cai_cli_options_init(options);
  options->argc = argc;
  options->argv = argv;
  for (i = 1; i < argc; i++) {
    const char *flag;
    const char *value;
    const char **field;
    char short_flag[3];

    flag = argv[i];
    if (strcmp(flag, "--no-config") == 0) {
      options->no_config = 1;
      continue;
    }
    if (strcmp(flag, "--replace") == 0) {
      options->replace_skill = 1;
      continue;
    }
    if (strcmp(flag, "--chrome") == 0) {
      options->chrome = 1;
      options->chrome_explicit = 1;
      continue;
    }
    if (strcmp(flag, "--no-chrome") == 0) {
      options->chrome = 0;
      options->chrome_explicit = 1;
      continue;
    }
    if (strncmp(flag, "--", 2U) == 0) {
      static const char *const names[] = {
          "web-search",      "todo",           "searxng",
          "revgeo",          "read-file",      "list-files",
          "apply-patch",     "view-image",     "goals",
          "skills",          "terminal",       "image-generation",
          "review-subagent", "codex-agents-md"};
      int *values[14];
      size_t k;
      int enabled = strncmp(flag, "--no-", 5U) != 0;
      const char *name = flag + (enabled ? 2U : 5U);
      values[0] = &options->web_search;
      values[1] = &options->todo;
      values[2] = &options->searxng;
      values[3] = &options->revgeo;
      values[4] = &options->read_file;
      values[5] = &options->list_files;
      values[6] = &options->apply_patch;
      values[7] = &options->view_image;
      values[8] = &options->goals;
      values[9] = &options->skills;
      values[10] = &options->terminal;
      values[11] = &options->image_generation;
      values[12] = &options->review_subagent;
      values[13] = &options->codex_agents_md;
      for (k = 0U; k < sizeof(names) / sizeof(names[0]); k++)
        if (strncmp(flag, "--", 2U) == 0 && strcmp(name, names[k]) == 0) {
          *values[k] = enabled;
          break;
        }
      if (k != sizeof(names) / sizeof(names[0]))
        continue;
    }
    if (strcmp(flag, "--chrome-setup") == 0) {
      options->chrome_setup = 1;
      continue;
    }
    if (strcmp(flag, "--chrome-remove") == 0) {
      options->chrome_remove = 1;
      continue;
    }

    if (flag[0] == '-' && flag[1] != '-' && flag[2] != '\0' &&
        strcmp(flag, "-vv") != 0) {
      for (j = 1U; flag[j] != '\0'; j++) {
        if (flag[j] == 'N')
          options->new_session = 1;
        else if (flag[j] == 'n')
          options->non_interactive = 1;
        else if (flag[j] == 'v')
          options->verbosity++;
        else if (flag[j] == 'l')
          options->list = 1;
        else if (flag[j + 1U] == '\0' && (flag[j] == 'i' || flag[j] == 'g')) {
          short_flag[0] = '-';
          short_flag[1] = flag[j];
          short_flag[2] = '\0';
          flag = short_flag;
          break;
        } else {
          cai_cli_option_error(options, "cai: invalid short option group: %s\n",
                               argv[i]);
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
    if (strcmp(flag, "--login") == 0) {
      options->login = 1;
      continue;
    }
    if (strcmp(flag, "-l") == 0 || strcmp(flag, "--list") == 0) {
      options->list = 1;
      continue;
    }
    if (strcmp(flag, "--resume") == 0 &&
        (i + 1 >= argc || argv[i + 1][0] == '-')) {
      options->resume_list = 1;
      continue;
    }
    if (strcmp(flag, "--no-shell-context") == 0) {
      options->shell_context = 0;
      continue;
    }
    if (strcmp(flag, "--shell-context") == 0) {
      options->shell_context = 1;
      continue;
    }
    if (strcmp(flag, "--shell-context-max-lines") == 0 ||
        strcmp(flag, "--shell-context-max-bytes") == 0) {
      unsigned long number;
      char *end;
      int lines = strcmp(flag, "--shell-context-max-lines") == 0;
      if (++i >= argc || argv[i][0] < '0' || argv[i][0] > '9') {
        cai_cli_option_error(options, "cai: %s requires a positive integer",
                             flag);
        return -1;
      }
      errno = 0;
      number = strtoul(argv[i], &end, 10);
      if (errno != 0 || *end != '\0' || number > (size_t)-1 ||
          number < (lines ? 1U : 4U)) {
        cai_cli_option_error(options, "cai: invalid value for %s", flag);
        return -1;
      }
      if (lines)
        options->shell_context_max_lines = (size_t)number;
      else
        options->shell_context_max_bytes = (size_t)number;
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
        cai_cli_option_error(options, "cai: %s requires a value\n", flag);
        return -1;
      }
      options->instruction_count++;
      continue;
    }
    if (strcmp(flag, "--resume") == 0)
      field = &options->resume_id;
    else if (strcmp(flag, "--export") == 0)
      field = &options->export_id;
    else if (strcmp(flag, "--export-dir") == 0)
      field = &options->export_dir;
    else if (strcmp(flag, "--import") == 0)
      field = &options->import_file;
    else if (strcmp(flag, "--lockd") == 0)
      field = &options->lockd;
    else if (strcmp(flag, "--lockd-client-pem") == 0)
      field = &options->lockd_client_pem;
    else if (strcmp(flag, "-g") == 0 || strcmp(flag, "--goal") == 0)
      field = &options->goal;
    else if (strcmp(flag, "--base") == 0)
      field = &options->base;
    else if (strcmp(flag, "-o") == 0 || strcmp(flag, "--out") == 0)
      field = &options->out;
    else if (strcmp(flag, "-T") == 0 || strcmp(flag, "--output-type") == 0)
      field = &options->output_type;
    else if (strcmp(flag, "--tool-output") == 0)
      field = &options->tool_output;
    else if (strcmp(flag, "-p") == 0 || strcmp(flag, "--provider") == 0)
      field = &options->provider;
    else if (strcmp(flag, "--chrome-socket") == 0) {
      field = &options->chrome_socket;
      options->chrome = 1;
      options->chrome_explicit = 1;
    } else if (strcmp(flag, "--searxng-url") == 0)
      field = &options->searxng_url;
    else if (strcmp(flag, "--revgeo-url") == 0)
      field = &options->revgeo_url;
    else if (strcmp(flag, "--chrome-manifest") == 0)
      field = &options->chrome_manifest;
    else if (strcmp(flag, "--endpoint") == 0)
      field = &options->endpoint;
    else if (strcmp(flag, "--api-key-env") == 0)
      field = &options->api_key_env;
    else if (strcmp(flag, "--model-metadata") == 0)
      field = &options->model_metadata;
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
    else if (strcmp(flag, "--config") == 0)
      field = &options->config_file;
    else if (strcmp(flag, "--config-dir") == 0)
      field = &options->config_dir;
    else if (strcmp(flag, "--skills-dir") == 0)
      field = &options->skills_dir;
    else if (strcmp(flag, "--install-skill") == 0)
      field = &options->install_skill;
    else if (strcmp(flag, "--identity") == 0)
      field = &options->identity;
    else if (strcmp(flag, "-I") == 0 ||
             strcmp(flag, "--developer-instructions") == 0)
      field = &options->developer_instructions;
    if (field == NULL) {
      cai_cli_option_error(options, "cai: unknown option: %s\n", flag);
      return -1;
    }
    if (++i >= argc || argv[i][0] == '\0') {
      cai_cli_option_error(options, "cai: %s requires a value\n", flag);
      return -1;
    }
    value = argv[i];
    *field = value;
    if (field == &options->tool_output && strcmp(value, "compact") != 0 &&
        strcmp(value, "full") != 0) {
      cai_cli_option_error(options,
                           "cai: --tool-output must be compact or full\n");
      return -1;
    }
    if (field == &options->model)
      options->model_explicit = 1;
    if (field == &options->provider)
      options->provider_explicit = 1;
    if (field == &options->reasoning_effort)
      options->reasoning_effort_explicit = 1;
    if (field == &options->reasoning_summary)
      options->reasoning_summary_explicit = 1;
    if ((field == &options->reasoning_effort ||
         field == &options->review_reasoning_effort) &&
        !cai_cli_one_of(value, efforts)) {
      cai_cli_option_error(options, "cai: invalid reasoning effort: %s\n",
                           value);
      return -1;
    }
    if ((field == &options->reasoning_summary ||
         field == &options->review_reasoning_summary) &&
        !cai_cli_one_of(value, summaries)) {
      cai_cli_option_error(options, "cai: invalid reasoning summary: %s\n",
                           value);
      return -1;
    }
  }
  return 1;
}

int cai_cli_validate_options(cai_cli_options *options) {
  static const char *const providers[] = {"chatgpt", "openai", "openrouter",
                                          "berget",  "custom", NULL};
  if (options->replace_skill && options->install_skill == NULL) {
    cai_cli_option_error(options, "%s",
                         "cai: --replace requires --install-skill");
    return -1;
  }

  if (options->config_file != NULL && options->no_config) {
    cai_cli_option_error(options,
                         "--config and --no-config are mutually exclusive");
    return -1;
  }
  if (options->chrome_manifest != NULL && !options->chrome_setup &&
      !options->chrome_remove) {
    cai_cli_option_error(
        options,
        "cai: --chrome-manifest requires --chrome-setup or --chrome-remove");
    return -1;
  }
  if ((options->chrome_setup || options->chrome_remove) &&
      ((options->chrome_explicit && options->chrome) || options->new_session ||
       options->resume_id != NULL || options->review ||
       options->review_and_fix || options->goal != NULL ||
       options->instruction_count != 0U || options->non_interactive)) {
    cai_cli_option_error(
        options,
        "cai: Chrome setup/removal cannot be combined with agent work");
    return -1;
  }
  if (options->review && options->chrome && options->chrome_explicit) {
    cai_cli_option_error(
        options, "cai: isolated --review does not support Chrome tools");
    return -1;
  }
  if ((options->list + options->resume_list + options->login +
       options->chrome_setup + options->chrome_remove +
       (options->export_id != NULL) + (options->import_file != NULL) +
       (options->install_skill != NULL)) > 1) {
    cai_cli_option_error(
        options, "%s",
        "cai: select only one of --list, --resume without ID, --login, "
        "--export, --import, or --install-skill\n");
    return -1;
  }
  if ((options->list || options->resume_list || options->export_id != NULL ||
       options->import_file != NULL || options->install_skill != NULL) &&
      (options->new_session || options->resume_id != NULL || options->review ||
       options->review_and_fix || options->goal != NULL ||
       options->instruction_count != 0U || options->non_interactive)) {
    cai_cli_option_error(
        options, "%s",
        "cai: storage commands cannot be combined with session work\n");
    return -1;
  }
  if (options->lockd != NULL && strncmp(options->lockd, "pouch://", 8U) != 0 &&
      strncmp(options->lockd, "http://", 7U) != 0 &&
      strncmp(options->lockd, "https://", 8U) != 0) {
    cai_cli_option_error(
        options, "%s",
        "cai: --lockd requires a pouch://, http://, or https:// endpoint\n");
    return -1;
  }
  if (options->new_session &&
      (options->resume_id != NULL || options->resume_list)) {
    cai_cli_option_error(options, "%s",
                         "cai: --new and --resume cannot be combined\n");
    return -1;
  }
  if (options->review && options->review_and_fix) {
    cai_cli_option_error(
        options, "%s",
        "cai: --review and --review-and-fix cannot be combined\n");
    return -1;
  }
  if (options->review &&
      (options->new_session || options->resume_id != NULL ||
       options->goal != NULL || options->instruction_count > 1U ||
       (options->base != NULL && options->instruction_count != 0U))) {
    cai_cli_option_error(
        options, "%s",
        "cai: --review accepts one of --base or one -i, without session or "
        "goal flags\n");
    return -1;
  }
  if (options->review_and_fix &&
      (options->goal != NULL || options->instruction_count != 0U)) {
    cai_cli_option_error(
        options, "%s",
        "cai: --review-and-fix supplies its own goal and prompt\n");
    return -1;
  }
  if (options->review_and_fix && !options->review_subagent) {
    cai_cli_option_error(
        options, "%s", "cai: --review-and-fix requires the review subagent\n");
    return -1;
  }
  if (options->base != NULL && !options->review && !options->review_and_fix) {
    cai_cli_option_error(options, "%s",
                         "cai: --base requires --review or --review-and-fix\n");
    return -1;
  }
  if ((options->out != NULL || options->output_type != NULL) &&
      !options->review) {
    cai_cli_option_error(options, "%s",
                         "cai: --out and --output-type require --review\n");
    return -1;
  }
  if (options->output_type != NULL &&
      strcmp(options->output_type, "markdown") != 0 &&
      strcmp(options->output_type, "json") != 0) {
    cai_cli_option_error(options, "%s",
                         "cai: --output-type must be markdown or json\n");
    return -1;
  }
  if (options->output_type == NULL)
    options->output_type = "markdown";
  if (options->non_interactive && !options->review &&
      !options->review_and_fix && options->instruction_count == 0U &&
      options->goal == NULL) {
    cai_cli_option_error(
        options, "%s",
        "cai: --non-interactive requires -i, --goal, or a review mode\n");
    return -1;
  }
  if (!cai_cli_one_of(options->provider, providers)) {
    cai_cli_option_error(options, "cai: invalid provider: %s\n",
                         options->provider);
    return -1;
  }
  if (strcmp(options->provider, "chatgpt") != 0 &&
      (options->auth_json != NULL || options->login)) {
    cai_cli_option_error(
        options, "%s",
        "cai: --auth-json and --login require --provider chatgpt\n");
    return -1;
  }
  if (options->login &&
      (options->new_session || options->resume_id != NULL ||
       options->non_interactive || options->review || options->review_and_fix ||
       options->goal != NULL || options->instruction_count != 0U ||
       options->auth_json != NULL)) {
    cai_cli_option_error(options, "%s",
                         "cai: --login cannot be combined with session work\n");
    return -1;
  }
  if (options->list || options->resume_list || options->export_id != NULL ||
      options->import_file != NULL || options->install_skill != NULL)
    return 1;
  if (strcmp(options->provider, "custom") == 0) {
    if (options->endpoint == NULL ||
        (!options->model_explicit && !options->model_configured)) {
      cai_cli_option_error(
          options, "%s",
          "cai: custom provider requires --endpoint and --model\n");
      return -1;
    }
    if (strncmp(options->endpoint, "https://", 8U) != 0 &&
        strncmp(options->endpoint, "http://", 7U) != 0) {
      cai_cli_option_error(
          options, "%s",
          "cai: --endpoint must start with https:// or http://\n");
      return -1;
    }
    if (options->api_key_env == NULL)
      options->api_key_env = "CAI_API_KEY";
  } else if (options->endpoint != NULL || options->api_key_env != NULL) {
    cai_cli_option_error(
        options, "%s",
        "cai: --endpoint and --api-key-env require --provider custom\n");
    return -1;
  }
  if (strcmp(options->provider, "openrouter") == 0 &&
      !options->model_explicit && !options->model_configured)
    options->model = CAI_OPENROUTER_MODEL_DEFAULT_RESPONSES;
  if (strcmp(options->provider, "berget") == 0) {
    if (!options->model_explicit && !options->model_configured)
      options->model = "zai-org/GLM-5.3-Flash";
    options->image_generation = 0;
  }
  return 1;
}

int cai_cli_parse_options(int argc, char *const *argv,
                          cai_cli_options *options) {
  int result = cai_cli_read_options(argc, argv, NULL, options);
  return result > 0 ? cai_cli_validate_options(options) : result;
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
        strcmp(flag, "--lockd") == 0 ||
        strcmp(flag, "--lockd-client-pem") == 0 ||
        strcmp(flag, "--export") == 0 || strcmp(flag, "--export-dir") == 0 ||
        strcmp(flag, "--import") == 0 || strcmp(flag, "-g") == 0 ||
        strcmp(flag, "--goal") == 0 || strcmp(flag, "--base") == 0 ||
        strcmp(flag, "-o") == 0 || strcmp(flag, "--out") == 0 ||
        strcmp(flag, "-T") == 0 || strcmp(flag, "--output-type") == 0 ||
        strcmp(flag, "--tool-output") == 0 || strcmp(flag, "-p") == 0 ||
        strcmp(flag, "--provider") == 0 || strcmp(flag, "--endpoint") == 0 ||
        strcmp(flag, "--api-key-env") == 0 ||
        strcmp(flag, "--model-metadata") == 0 || strcmp(flag, "-C") == 0 ||
        strcmp(flag, "--searxng-url") == 0 ||
        strcmp(flag, "--revgeo-url") == 0 ||
        strcmp(flag, "--chrome-socket") == 0 ||
        strcmp(flag, "--chrome-manifest") == 0 ||
        strcmp(flag, "--shell-context-max-lines") == 0 ||
        strcmp(flag, "--shell-context-max-bytes") == 0 ||
        strcmp(flag, "--directory") == 0 || strcmp(flag, "-a") == 0 ||
        strcmp(flag, "--auth-json") == 0 || strcmp(flag, "-m") == 0 ||
        strcmp(flag, "--model") == 0 || strcmp(flag, "-r") == 0 ||
        strcmp(flag, "--reasoning-effort") == 0 ||
        strcmp(flag, "--reasoning-summary") == 0 ||
        strcmp(flag, "--review-model") == 0 ||
        strcmp(flag, "--review-reasoning-effort") == 0 ||
        strcmp(flag, "--review-reasoning-summary") == 0 ||
        strcmp(flag, "--agents-md") == 0 || strcmp(flag, "--config-dir") == 0 ||
        strcmp(flag, "--config") == 0 || strcmp(flag, "--skills-dir") == 0 ||
        strcmp(flag, "--install-skill") == 0 ||
        strcmp(flag, "--identity") == 0 || strcmp(flag, "-I") == 0 ||
        strcmp(flag, "--developer-instructions") == 0;
    if (has_value && i + 1 < options->argc) {
      if (instruction && found++ == index)
        return options->argv[i + 1];
      i++;
    }
  }
  return NULL;
}
