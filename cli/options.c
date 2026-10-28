#include "options.h"
#include <cai/app.h>

#include <cai/models.h>
#include <cai/version.h>

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void cai_cli_option_error(cai_cli_options *options, const char *format,
                                 ...) {
  va_list arguments;
  va_start(arguments, format);
  {
    size_t n =
        (size_t)snprintf(options->diagnostic, sizeof(options->diagnostic),
                         "%s: ", options->program_name);
    if (n < sizeof(options->diagnostic))
      vsnprintf(options->diagnostic + n, sizeof(options->diagnostic) - n,
                format, arguments);
  }
  va_end(arguments);
  options->diagnostic[strcspn(options->diagnostic, "\n")] = '\0';
}

void cai_cli_options_init(cai_cli_options *options) {
  size_t i;
  memset(options, 0, sizeof(*options));
  options->program_name = CAI_APP_DEFAULT_PROGRAM_NAME;
  options->invocation_name = CAI_APP_DEFAULT_PROGRAM_NAME;
  for (i = 0U; options->program_name[i] != '\0'; i++) {
    char c = options->program_name[i];
    options->environment_prefix[i] = c == '-' ? '_'
                                     : c >= 'a' && c <= 'z'
                                         ? (char)(c - 'a' + 'A')
                                         : c;
  }
  options->environment_prefix[i++] = '_';
  options->environment_prefix[i] = '\0';
  snprintf(options->default_api_key_env, sizeof(options->default_api_key_env),
           "%sAPI_KEY", options->environment_prefix);
  options->provider = "chatgpt";
  options->model = CAI_MODEL_GPT_6_LUNA;
  options->reasoning_effort = "medium";
  options->normal_reasoning_effort = "medium";
  options->high_reasoning_effort = "high";
  options->reasoning_summary = "concise";
  options->todo_layout = "board";
  options->theme = "default";
  options->status_fields = CAI_CLI_STATUS_DEFAULT_FIELDS;
  options->sessions_store = options->auth_store = options->messaging_store =
      "default";
  options->image_generation = 1;
  options->terminal = 1;
  options->shell_context = 1;
  options->shell_context_max_lines = 5000U;
  options->shell_context_max_bytes = 4U * 1024U * 1024U;
  options->review_subagent = 1;
  options->subagent_steering = 1;
  options->chrome = 1;
  options->web_search = 1;
  options->mcp = 1;
  options->messaging = 0;
  options->message_ack_timeout_ms = 300000L;
  options->replay_turns = 20L;
  options->prompt_history_limit = 200;
  options->conversation_retention_days = 30L;
  options->read_file = options->list_files = options->apply_patch = 1;
  options->view_image = options->goals = options->skills = 1;
}

static int cai_cli_one_of(const char *value, const char *const *values) {
  size_t i;
  for (i = 0U; values[i] != NULL; i++) {
    if (strcmp(value, values[i]) == 0)
      return 1;
  }
  return 0;
}

/* Value-taking short options may end a group. */
static int cai_cli_short_value_option(const char *flag) {
  size_t length;
  if (flag[0] != '-' || flag[1] == '\0' || flag[1] == '-')
    return 0;
  length = strlen(flag);
  switch (flag[length - 1U]) {
  case 'i':
  case 'g':
  case 't':
  case 'R':
  case 'D':
  case 'c':
    return flag[length - 1U];
  default:
    return 0;
  }
}

/* Instruction selection uses this same parser: there is no second option
 * list that can mistake another flag's value for a startup prompt. */
static int cli_read_options(int argc, char *const *argv,
                            const cai_cli_options *defaults,
                            cai_cli_options *options, size_t instruction_index,
                            const char **instruction) {
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
  if (instruction != NULL)
    *instruction = NULL;
  if (defaults != NULL) {
    *options = *defaults;
    if (defaults->api_key_env == defaults->default_api_key_env)
      options->api_key_env = options->default_api_key_env;
  } else
    cai_cli_options_init(options);
  options->argc = argc;
  options->argv = argv;
  for (i = 1; i < argc; i++) {
    const char *flag;
    const char *value;
    const char **field;
    char short_flag[3];

    flag = argv[i];
    if (strcmp(flag, "uncommitted") == 0 &&
        (options->review || options->review_and_fix) &&
        !options->review_uncommitted) {
      options->review_uncommitted = 1;
      continue;
    }
    if (strcmp(flag, "--no-config") == 0) {
      options->no_config = 1;
      continue;
    }
    if (strcmp(flag, "--yes") == 0 || strcmp(flag, "--force") == 0) {
      options->force = 1;
      continue;
    }
    if (strcmp(flag, "--temp") == 0) {
      options->temporary = 1;
      continue;
    }
    if (strcmp(flag, "--no-temp") == 0) {
      options->temporary = 0;
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
      static const char *const names[] = {"web-search",
                                          "todo",
                                          "searxng",
                                          "revgeo",
                                          "read-file",
                                          "list-files",
                                          "apply-patch",
                                          "view-image",
                                          "goals",
                                          "skills",
                                          "terminal",
                                          "image-generation",
                                          "review-subagent",
                                          "codex-agents-md",
                                          "mcp",
                                          "reasoning-tool",
                                          "subagent-steering",
                                          "yolo"};
      int *values[18];
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
      values[14] = &options->mcp;
      values[15] = &options->reasoning_tool;
      values[16] = &options->subagent_steering;
      values[17] = &options->yolo;
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
    if (strcmp(flag, "--disable-compression") == 0 ||
        strcmp(flag, "--no-disable-compression") == 0) {
      options->disable_compression = strcmp(flag, "--disable-compression") == 0;
      continue;
    }
    if (strcmp(flag, "--messaging") == 0 ||
        strcmp(flag, "--no-messaging") == 0) {
      options->messaging = strcmp(flag, "--messaging") == 0;
      continue;
    }
    if (strcmp(flag, "--autoqueue") == 0 ||
        strcmp(flag, "--no-autoqueue") == 0) {
      options->enable_auto_enqueue = strcmp(flag, "--autoqueue") == 0;
      continue;
    }
    if (strcmp(flag, "--conversation-retention-days") == 0) {
      char *end;
      long number;
      if (++i >= argc) {
        cai_cli_option_error(options, "%s requires a nonnegative day count",
                             flag);
        return -1;
      }
      errno = 0;
      number = strtol(argv[i], &end, 10);
      if (errno != 0 || end == argv[i] || *end != '\0' || number < 0L ||
          number > LONG_MAX / 86400L) {
        cai_cli_option_error(options, "%s requires days in 0..%ld", flag,
                             LONG_MAX / 86400L);
        return -1;
      }
      options->conversation_retention_days = number;
      continue;
    }
    if (strcmp(flag, "--prompt-history-limit") == 0) {
      char *end;
      long number;
      if (++i >= argc) {
        cai_cli_option_error(options, "%s requires a nonnegative count", flag);
        return -1;
      }
      errno = 0;
      number = strtol(argv[i], &end, 10);
      if (errno != 0 || end == argv[i] || *end != '\0' || number < 0L ||
          number > INT_MAX) {
        cai_cli_option_error(
            options, "%s requires a nonnegative count up to %d", flag, INT_MAX);
        return -1;
      }
      options->prompt_history_limit = (int)number;
      continue;
    }
    if (strcmp(flag, "--replay-turns") == 0) {
      char *end;
      long number;
      if (++i >= argc) {
        cai_cli_option_error(options, "%s requires -1, 0 or a positive count",
                             flag);
        return -1;
      }
      errno = 0;
      number = strtol(argv[i], &end, 10);
      if (errno != 0 || end == argv[i] || *end != '\0' || number < -1L) {
        cai_cli_option_error(options, "%s requires -1, 0 or a positive count",
                             flag);
        return -1;
      }
      options->replay_turns = number;
      continue;
    }
    if (strcmp(flag, "--message-ack-timeout-ms") == 0) {
      char *end;
      long timeout;
      if (++i >= argc) {
        cai_cli_option_error(options, "%s requires milliseconds (1..86400000)",
                             flag);
        return -1;
      }
      errno = 0;
      timeout = strtol(argv[i], &end, 10);
      if (errno != 0 || *end != '\0' || timeout <= 0 || timeout > 86400000L) {
        cai_cli_option_error(options, "%s requires milliseconds (1..86400000)",
                             flag);
        return -1;
      }
      options->message_ack_timeout_ms = timeout;
      continue;
    }
    if (strcmp(flag, "--chrome-remove") == 0) {
      options->chrome_remove = 1;
      continue;
    }

    if (flag[0] == '-' && flag[1] != '\0' && flag[1] != '-' &&
        flag[2] != '\0' && strcmp(flag, "-vv") != 0) {
      for (j = 1U; flag[j] != '\0'; j++) {
        if (flag[j] == 'N')
          options->new_session = 1;
        else if (flag[j] == 'n')
          options->non_interactive = 1;
        else if (flag[j] == 'v')
          options->verbosity++;
        else if (flag[j] == 'l')
          options->list = 1;
        else if (flag[j + 1U] == '\0' &&
                 cai_cli_short_value_option(flag) != 0) {
          short_flag[0] = '-';
          short_flag[1] = flag[j];
          short_flag[2] = '\0';
          flag = short_flag;
          break;
        } else {
          cai_cli_option_error(options, "invalid short option group: %s\n",
                               argv[i]);
          return -1;
        }
      }
      if (flag != short_flag)
        continue;
    }
    if (strcmp(flag, "-h") == 0 || strcmp(flag, "--help") == 0) {
      options->help = 1;
      return 0;
    }
    if (strcmp(flag, "--version") == 0) {
      options->version = 1;
      return 0;
    }
    if (strcmp(flag, "--list-themes") == 0) {
      options->list_themes = 1;
      return 0;
    }
    if (strcmp(flag, "--dump-config") == 0) {
      options->dump_config = 1;
      if (i + 1 < argc && argv[i + 1][0] != '-')
        options->dump_config_file = argv[++i];
      continue;
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
    if ((strcmp(flag, "--resume") == 0 || strcmp(flag, "-R") == 0) &&
        (i + 1 >= argc || argv[i + 1][0] == '-')) {
      options->resume_list = 1;
      continue;
    }
    if ((strcmp(flag, "--delete") == 0 || strcmp(flag, "-D") == 0) &&
        (i + 1 >= argc || argv[i + 1][0] == '-')) {
      options->delete_list = 1;
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
        cai_cli_option_error(options, "%s requires a positive integer", flag);
        return -1;
      }
      errno = 0;
      number = strtoul(argv[i], &end, 10);
      if (errno != 0 || *end != '\0' || number > (size_t)-1 ||
          number < (lines ? 1U : 4U)) {
        cai_cli_option_error(options, "invalid value for %s", flag);
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
        cai_cli_option_error(options, "%s requires a value\n", flag);
        return -1;
      }
      if (instruction != NULL &&
          options->instruction_count == instruction_index)
        *instruction = argv[i];
      options->instruction_count++;
      continue;
    }
    if (strcmp(flag, "-t") == 0 || strcmp(flag, "--theme") == 0)
      field = &options->theme;
    if (strcmp(flag, "--status-fields") == 0)
      field = &options->status_fields;
    else if (strcmp(flag, "--resume") == 0 || strcmp(flag, "-R") == 0)
      field = &options->resume_id;
    else if (strcmp(flag, "--delete") == 0 || strcmp(flag, "-D") == 0)
      field = &options->delete_id;
    else if (strcmp(flag, "--export") == 0)
      field = &options->export_id;
    else if (strcmp(flag, "--export-out") == 0)
      field = &options->export_out;
    else if (strcmp(flag, "--export-dir") == 0)
      field = &options->export_dir;
    else if (strcmp(flag, "--state-dir") == 0)
      field = &options->state_dir;
    else if (strcmp(flag, "--home") == 0)
      field = &options->home_dir;
    else if (strcmp(flag, "--runtime-dir") == 0)
      field = &options->runtime_dir;
    else if (strcmp(flag, "--cache-dir") == 0)
      field = &options->cache_dir;
    else if (strcmp(flag, "--data-dir") == 0)
      field = &options->data_dir;
    else if (strcmp(flag, "--clipboard-dir") == 0)
      field = &options->clipboard_dir;
    else if (strcmp(flag, "--log-dir") == 0)
      field = &options->log_dir;
    else if (strcmp(flag, "--pouch-key-file") == 0)
      field = &options->pouch_key_file;
    else if (strcmp(flag, "--namespace-prefix") == 0)
      field = &options->namespace_prefix;
    else if (strcmp(flag, "--sessions-namespace") == 0)
      field = &options->sessions_namespace;
    else if (strcmp(flag, "--auth-namespace") == 0)
      field = &options->auth_namespace;
    else if (strcmp(flag, "--scopes-namespace") == 0)
      field = &options->scopes_namespace;
    else if (strcmp(flag, "--todo-namespace") == 0)
      field = &options->todo_namespace;
    else if (strcmp(flag, "--messages-namespace") == 0)
      field = &options->messages_namespace;
    else if (strcmp(flag, "--queue-prefix") == 0)
      field = &options->queue_prefix;
    else if (strcmp(flag, "--import") == 0)
      field = &options->import_file;
    else if (strcmp(flag, "--lockd") == 0)
      field = &options->lockd;
    else if (strcmp(flag, "--sessions-store") == 0)
      field = &options->sessions_store;
    else if (strcmp(flag, "--auth-store") == 0)
      field = &options->auth_store;
    else if (strcmp(flag, "--messaging-store") == 0)
      field = &options->messaging_store;
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
    else if (strcmp(flag, "--todo-layout") == 0)
      field = &options->todo_layout;
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
    else if (strcmp(flag, "--normal-reasoning-effort") == 0)
      field = &options->normal_reasoning_effort;
    else if (strcmp(flag, "--high-reasoning-effort") == 0)
      field = &options->high_reasoning_effort;
    else if (strcmp(flag, "--service-tier") == 0)
      field = &options->service_tier;
    else if (strcmp(flag, "--message-queue") == 0)
      field = &options->message_queue;
    else if (strcmp(flag, "--queue") == 0 || strcmp(flag, "-q") == 0)
      field = &options->send_queue;
    else if (strcmp(flag, "--message") == 0 || strcmp(flag, "-M") == 0)
      field = &options->message;
    else if (strcmp(flag, "--board") == 0) {
      field = &options->board;
      options->board_explicit = 1;
    } else if (strcmp(flag, "--review-model") == 0)
      field = &options->review_model;
    else if (strcmp(flag, "--review-reasoning-effort") == 0)
      field = &options->review_reasoning_effort;
    else if (strcmp(flag, "--review-reasoning-summary") == 0)
      field = &options->review_reasoning_summary;
    else if (strcmp(flag, "--agents-md") == 0)
      field = &options->agents_md;
    else if (strcmp(flag, "--config") == 0 || strcmp(flag, "-c") == 0)
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
      cai_cli_option_error(options, "unknown option: %s\n", flag);
      return -1;
    }
    if (++i >= argc || (argv[i][0] == '\0' && field != &options->export_out)) {
      cai_cli_option_error(options, "%s requires a value\n", flag);
      return -1;
    }
    value = argv[i];
    *field = value;
    if (field == &options->export_out && value[0] == '\0')
      *field = NULL;
    if (field == &options->config_file)
      options->config_explicit = 1;
    if (field == &options->todo_layout && strcmp(value, "board") != 0 &&
        strcmp(value, "list") != 0) {
      cai_cli_option_error(options, "--todo-layout must be board or list");
      return -1;
    }
    if (field == &options->tool_output && strcmp(value, "compact") != 0 &&
        strcmp(value, "full") != 0) {
      cai_cli_option_error(options, "--tool-output must be compact or full\n");
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
         field == &options->normal_reasoning_effort ||
         field == &options->high_reasoning_effort ||
         field == &options->review_reasoning_effort) &&
        !cai_cli_one_of(value, efforts)) {
      cai_cli_option_error(options, "invalid reasoning effort: %s\n", value);
      return -1;
    }
    if ((field == &options->reasoning_summary ||
         field == &options->review_reasoning_summary) &&
        !cai_cli_one_of(value, summaries)) {
      cai_cli_option_error(options, "invalid reasoning summary: %s\n", value);
      return -1;
    }
  }
  return options->dump_config ? 0 : 1;
}

int cai_cli_read_options(int argc, char *const *argv,
                         const cai_cli_options *defaults,
                         cai_cli_options *options) {
  return cli_read_options(argc, argv, defaults, options, 0U, NULL);
}

int cai_cli_validate_options(cai_cli_options *options) {
  static const char *const providers[] = {"chatgpt", "openai", "openrouter",
                                          "berget",  "custom", NULL};
  if (options->force && !options->temporary && options->install_skill == NULL) {
    cai_cli_option_error(options, "%s",
                         "--yes/--force requires --temp or --install-skill");
    return -1;
  }
  if (options->export_out != NULL && options->export_id == NULL) {
    cai_cli_option_error(options, "--export-out requires --export ID");
    return -1;
  }
  if (options->temporary &&
      (options->resume_id != NULL || options->resume_list || options->list ||
       options->delete_id != NULL || options->delete_list || options->login ||
       options->chrome_setup || options->chrome_remove ||
       options->export_id != NULL || options->import_file != NULL ||
       options->install_skill != NULL || options->message != NULL)) {
    cai_cli_option_error(options, "%s",
                         "--temp starts a new conversation; it cannot be "
                         "combined with resume or utility commands");
    return -1;
  }

  if ((options->send_queue != NULL) != (options->message != NULL) ||
      (options->message != NULL &&
       (options->message[0] == '\0' || options->send_queue[0] == '\0'))) {
    cai_cli_option_error(
        options, "--queue NAME and --message TEXT are required together");
    return -1;
  }
  if (options->config_explicit && options->no_config) {
    cai_cli_option_error(options,
                         "--config and --no-config are mutually exclusive");
    return -1;
  }
  if (options->chrome_manifest != NULL && !options->chrome_setup &&
      !options->chrome_remove) {
    cai_cli_option_error(
        options,
        "--chrome-manifest requires --chrome-setup or --chrome-remove");
    return -1;
  }
  if ((options->chrome_setup || options->chrome_remove) &&
      ((options->chrome_explicit && options->chrome) || options->new_session ||
       options->resume_id != NULL || options->review ||
       options->review_and_fix || options->goal != NULL ||
       options->instruction_count != 0U || options->non_interactive)) {
    cai_cli_option_error(
        options, "Chrome setup/removal cannot be combined with agent work");
    return -1;
  }
  if (options->review && options->chrome && options->chrome_explicit) {
    cai_cli_option_error(options,
                         "isolated --review does not support Chrome tools");
    return -1;
  }
  if ((options->list + options->resume_list + options->delete_list +
       (options->delete_id != NULL) + options->login + options->chrome_setup +
       options->chrome_remove + (options->export_id != NULL) +
       (options->import_file != NULL) + (options->install_skill != NULL) +
       (options->message != NULL)) > 1) {
    cai_cli_option_error(
        options, "%s",
        "select only one of --list, --resume without ID, --delete, --login, "
        "--export, --import, --install-skill, or --message\n");
    return -1;
  }
  if ((options->list || options->resume_list || options->delete_list ||
       options->delete_id != NULL || options->export_id != NULL ||
       options->import_file != NULL || options->install_skill != NULL ||
       options->message != NULL) &&
      (options->new_session || options->resume_id != NULL || options->review ||
       options->review_and_fix || options->goal != NULL ||
       options->instruction_count != 0U || options->non_interactive)) {
    cai_cli_option_error(
        options, "%s",
        "storage commands cannot be combined with session work\n");
    return -1;
  }
  if (options->lockd != NULL && strncmp(options->lockd, "pouch://", 8U) != 0 &&
      strncmp(options->lockd, "http://", 7U) != 0 &&
      strncmp(options->lockd, "https://", 8U) != 0) {
    cai_cli_option_error(
        options, "%s",
        "--lockd requires a pouch://, http://, or https:// endpoint\n");
    return -1;
  }
  if (options->new_session &&
      (options->resume_id != NULL || options->resume_list)) {
    cai_cli_option_error(options, "%s",
                         "--new and --resume cannot be combined\n");
    return -1;
  }
  if (options->review && options->review_and_fix) {
    cai_cli_option_error(options, "%s",
                         "--review and --review-and-fix cannot be combined\n");
    return -1;
  }
  if (options->review_uncommitted &&
      (options->base != NULL || options->instruction_count != 0U)) {
    cai_cli_option_error(options, "%s",
                         "uncommitted cannot be combined with --base or -i\n");
    return -1;
  }
  if (options->review &&
      (options->new_session || options->resume_id != NULL ||
       options->goal != NULL || options->instruction_count > 1U ||
       (options->base != NULL && options->instruction_count != 0U))) {
    cai_cli_option_error(
        options, "%s",
        "--review accepts one of --base or one -i, without session or "
        "goal flags\n");
    return -1;
  }
  if (options->review_and_fix &&
      (options->goal != NULL || options->instruction_count != 0U)) {
    cai_cli_option_error(options, "%s",
                         "--review-and-fix supplies its own goal and prompt\n");
    return -1;
  }
  if (options->review_and_fix && !options->review_subagent) {
    cai_cli_option_error(options, "%s",
                         "--review-and-fix requires the review subagent\n");
    return -1;
  }
  if (!options->goals && (options->goal != NULL || options->review_and_fix)) {
    cai_cli_option_error(options, "%s",
                         "--goal and --review-and-fix require goal tools; "
                         "enable --goals or [agent] goals=true\n");
    return -1;
  }
  if (options->base != NULL && !options->review && !options->review_and_fix) {
    cai_cli_option_error(options, "%s",
                         "--base requires --review or --review-and-fix\n");
    return -1;
  }
  if ((options->out != NULL || options->output_type != NULL) &&
      !options->review) {
    cai_cli_option_error(options, "%s",
                         "--out and --output-type require --review\n");
    return -1;
  }
  if (options->output_type != NULL &&
      strcmp(options->output_type, "markdown") != 0 &&
      strcmp(options->output_type, "json") != 0) {
    cai_cli_option_error(options, "%s",
                         "--output-type must be markdown or json\n");
    return -1;
  }
  if (options->output_type == NULL)
    options->output_type = "markdown";
  if (options->non_interactive && !options->review &&
      !options->review_and_fix && options->instruction_count == 0U &&
      options->goal == NULL) {
    cai_cli_option_error(
        options, "%s",
        "--non-interactive requires -i, --goal, or a review mode\n");
    return -1;
  }
  if (!cai_cli_one_of(options->provider, providers)) {
    cai_cli_option_error(options, "invalid provider: %s\n", options->provider);
    return -1;
  }
  if (strcmp(options->provider, "chatgpt") != 0 &&
      (options->auth_json != NULL || options->login)) {
    cai_cli_option_error(
        options, "%s", "--auth-json and --login require --provider chatgpt\n");
    return -1;
  }
  if (options->login &&
      (options->new_session || options->resume_id != NULL ||
       options->non_interactive || options->review || options->review_and_fix ||
       options->goal != NULL || options->instruction_count != 0U ||
       options->auth_json != NULL)) {
    cai_cli_option_error(options, "%s",
                         "--login cannot be combined with session work\n");
    return -1;
  }
  if (options->list || options->resume_list || options->export_id != NULL ||
      options->import_file != NULL || options->install_skill != NULL ||
      options->message != NULL)
    return 1;
  if (strcmp(options->provider, "custom") == 0) {
    if (options->endpoint == NULL ||
        (!options->model_explicit && !options->model_configured)) {
      cai_cli_option_error(options, "%s",
                           "custom provider requires --endpoint and --model\n");
      return -1;
    }
    if (strncmp(options->endpoint, "https://", 8U) != 0 &&
        strncmp(options->endpoint, "http://", 7U) != 0) {
      cai_cli_option_error(options, "%s",
                           "--endpoint must start with https:// or http://\n");
      return -1;
    }
    if (options->api_key_env == NULL)
      options->api_key_env = options->default_api_key_env;
  } else if (options->endpoint != NULL || options->api_key_env != NULL) {
    cai_cli_option_error(
        options, "%s",
        "--endpoint and --api-key-env require --provider custom\n");
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
  cai_cli_options parsed;
  const char *instruction;

  if (options == NULL || options->argv == NULL)
    return NULL;
  return cli_read_options(options->argc, options->argv, NULL, &parsed, index,
                          &instruction) > 0
             ? instruction
             : NULL;
}
