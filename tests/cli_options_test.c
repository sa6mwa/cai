#include "../cli/app_internal.h"
#include "../cli/options.h"

#include <cai/models.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every advertised value option must consume its own argv entry, even when
 * that value looks like an instruction flag. Test both long and short forms
 * using the primary parser without unrelated mode/provider validation. */
static int value_option_instructions(void) {
  cai_cli_options options;
  char program[] = "cai", marker[] = "-i", first[] = "actual prompt";
  char next[] = "--instruction", second[] = "second prompt";
  char short_flag[3] = {'-', '\0', '\0'};
  char flag_buffer[128], value_buffer[32];
  char *args[7];
  size_t i, count;
  int form;
  for (i = 0U; i < cai_cli_help_count(); i++) {
    const cai_app_option *option = cai_cli_help_at(i);
    const char *value = marker;
    if (option->type != CAI_APP_STRING && option->type != CAI_APP_INTEGER &&
        option->type != CAI_APP_OPTIONAL_STRING)
      continue;
    if (option->type == CAI_APP_INTEGER)
      value = "7";
    else if (option->type == CAI_APP_OPTIONAL_STRING)
      value = "session-id";
    else if (strstr(option->long_flag, "reasoning-effort") != NULL)
      value = "medium";
    else if (strstr(option->long_flag, "reasoning-summary") != NULL)
      value = "concise";
    else if (strcmp(option->long_flag, "--tool-output") == 0)
      value = "compact";
    else if (strcmp(option->long_flag, "--todo-layout") == 0)
      value = "board";
    for (form = 0; form < (option->short_flag != '\0' ? 2 : 1); form++) {
      short_flag[1] = option->short_flag;
      args[0] = program;
      snprintf(flag_buffer, sizeof(flag_buffer), "%s",
               form == 0 ? option->long_flag : short_flag);
      snprintf(value_buffer, sizeof(value_buffer), "%s", value);
      args[1] = flag_buffer;
      args[2] = value_buffer;
      args[3] = marker;
      args[4] = first;
      args[5] = next;
      args[6] = second;
      count = option->short_flag == 'i' ? 1U : 0U;
      if (cai_cli_read_options(7, args, NULL, &options) != 1 ||
          options.instruction_count != count + 2U ||
          (count != 0U && cai_cli_instruction_at(&options, 0U) != args[2]) ||
          cai_cli_instruction_at(&options, count) != first ||
          cai_cli_instruction_at(&options, count + 1U) != second ||
          cai_cli_instruction_at(&options, count + 2U) != NULL) {
        fprintf(stderr, "incorrect prompt extraction for %s '%s'\n", args[1],
                value);
        return 1;
      }
    }
  }
  return 0;
}

static int config_option_precedence(void) {
  cai_cli_options defaults, options;
  static char program[] = "cai", disable[] = "--no-config", list[] = "--list";
  static char flag[] = "--config", path[] = "explicit.ini";
  char *inherited[] = {program, disable, list};
  char *explicit_first[] = {program, flag, path, disable, list};
  char *explicit_last[] = {program, disable, flag, path, list};
  cai_cli_options_init(&defaults);
  defaults.config_file = "inherited.ini";
  if (cai_cli_read_options(1, inherited, &defaults, &options) != 1 ||
      cai_cli_validate_options(&options) != 1 ||
      options.config_file != defaults.config_file ||
      cai_cli_read_options(3, explicit_first, &defaults, &options) != 1 ||
      cai_cli_validate_options(&options) != 1 || options.config_file != path)
    return 1;
  if (cai_cli_read_options(3, inherited, &defaults, &options) != 1 ||
      cai_cli_validate_options(&options) != 1)
    return 1;
  if (cai_cli_read_options(5, explicit_first, &defaults, &options) != 1 ||
      cai_cli_validate_options(&options) != -1 ||
      cai_cli_read_options(5, explicit_last, &defaults, &options) != 1 ||
      cai_cli_validate_options(&options) != -1)
    return 1;
  return 0;
}

static int short_argument_options(void) {
  static const char *const invalid[] = {"", "-", "--", "-x", "-Nq"};
  char program[] = "cai";
  char *args[2];
  cai_cli_options options;
  size_t i, length;
  int rc;
  args[0] = program;
  for (i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    /* Exact allocations expose lookahead past a terminating NUL to memory
     * checkers, matching the app receiver's copied argv strings. */
    length = strlen(invalid[i]) + 1U;
    args[1] = malloc(length);
    if (args[1] == NULL)
      return 1;
    memcpy(args[1], invalid[i], length);
    rc = cai_cli_parse_options(2, args, &options);
    free(args[1]);
    if (rc != -1 || options.diagnostic[0] == '\0')
      return 1;
  }
  return 0;
}

static int grouped_value_options(void) {
  char flags[][16] = {"-g", "-Ng", "-Nng", "-vng",   "--goal",
                      "-t", "-Nt", "-Nnt", "--theme"};
  char values[][16] = {"-i", "--instruction", "-Nni", "-g", "-Nng", "-"};
  char program[] = "cai", instruction[] = "-i";
  char first[] = "actual prompt", second_flag[] = "--instruction";
  char second[] = "second prompt";
  char *args[7];
  cai_cli_options options;
  size_t i, j;
  args[0] = program;
  args[3] = instruction;
  args[4] = first;
  args[5] = second_flag;
  args[6] = second;
  for (i = 0U; i < sizeof(flags) / sizeof(flags[0]); i++) {
    args[1] = flags[i];
    for (j = 0U; j < sizeof(values) / sizeof(values[0]); j++) {
      args[2] = values[j];
      if (cai_cli_parse_options(7, args, &options) != 1 ||
          (i < 5U ? options.goal : options.theme) != args[2] ||
          options.instruction_count != 2U ||
          cai_cli_instruction_at(&options, 0U) != first ||
          cai_cli_instruction_at(&options, 1U) != second ||
          cai_cli_instruction_at(&options, 2U) != NULL) {
        fprintf(stderr, "incorrect prompt extraction for %s '%s'\n", args[1],
                args[2]);
        return 1;
      }
    }
  }
  return 0;
}

static int replay_options(void) {
  char values[][24] = {"cai", "--replay-turns", "2", "-i", "first",
                       "-i",  "second"};
  char resume[] = "--resume";
  char *args[7];
  cai_cli_options options;
  size_t i;
  for (i = 0U; i < 7U; i++)
    args[i] = values[i];
  if (cai_cli_parse_options(7, args, &options) != 1 ||
      options.replay_turns != 2L ||
      strcmp(cai_cli_instruction_at(&options, 0U), "first") != 0 ||
      strcmp(cai_cli_instruction_at(&options, 1U), "second") != 0)
    return 1;
  /* --resume has an optional value; the next -i must remain an instruction. */
  args[1] = resume;
  args[2] = args[3];
  args[3] = args[4];
  if (cai_cli_read_options(4, args, NULL, &options) != 1 ||
      !options.resume_list || options.instruction_count != 1U ||
      strcmp(cai_cli_instruction_at(&options, 0U), "first") != 0)
    return 1;
  return 0;
}

static int resume_alias_options(void) {
  char cases[][6][32] = {{"cai", "-R"},
                         {"cai", "-vR"},
                         {"cai", "-R", "session-id", "-i", "actual prompt"},
                         {"cai", "-vnR", "session-id", "-i", "actual prompt"},
                         {"cai", "-R", "-i", "actual prompt"},
                         {"cai", "-vR", "-i", "actual prompt"},
                         {"cai", "-R", "--directory", "/tmp/project"},
                         {"cai", "-NR", "session-id"},
                         {"cai", "--new", "-R"},
                         {"cai", "-R", "--list"}};
  int lengths[] = {2, 2, 5, 5, 4, 4, 4, 3, 3, 3};
  cai_cli_options options;
  char *args[6];
  size_t i, j;
  int with_id, with_prompt;
  for (i = 0U; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
    for (j = 0U; j < (size_t)lengths[i]; j++)
      args[j] = cases[i][j];
    if (cai_cli_read_options(lengths[i], args, NULL, &options) != 1) {
      fprintf(stderr, "resume alias case %lu failed to parse: %s\n",
              (unsigned long)i, options.diagnostic);
      return 1;
    }
    with_id = i == 2U || i == 3U;
    with_prompt = i >= 2U && i <= 5U;
    if (i < 7U && (options.resume_list != !with_id ||
                   (with_id ? options.resume_id == NULL ||
                                  strcmp(options.resume_id, "session-id") != 0
                            : options.resume_id != NULL) ||
                   options.instruction_count != (with_prompt ? 1U : 0U) ||
                   (with_prompt && strcmp(cai_cli_instruction_at(&options, 0U),
                                          "actual prompt") != 0))) {
      fprintf(stderr, "resume alias case %lu has incorrect values\n",
              (unsigned long)i);
      return 1;
    }
    /* Listing cannot submit work, but parsing must still leave -i intact. */
    if (cai_cli_validate_options(&options) != (i < 4U || i == 6U ? 1 : -1)) {
      fprintf(stderr, "resume alias case %lu has incorrect validation: %s\n",
              (unsigned long)i, options.diagnostic);
      return 1;
    }
  }
  return 0;
}

static int storage_options(void) {
  cai_cli_options options;
  char arguments[][64] = {"cai",
                          "--lockd",
                          "https://lockd.example.test",
                          "--lockd-client-pem",
                          "client.pem",
                          "--export",
                          "session",
                          "--export-dir",
                          "exports"};
  char cases[][7][64] = {{"cai", "-l"},
                         {"cai", "--resume"},
                         {"cai", "--resume", "--lockd", "pouch:///tmp/pouch"},
                         {"cai", "--import", "conversation.jsonl"},
                         {"cai", "--list", "--import", "conversation.jsonl"},
                         {"cai", "--export", "id", "-i", "work"},
                         {"cai", "--import", "conversation.jsonl", "--new"},
                         {"cai", "--list", "--lockd", "file:///tmp/store"},
                         {"cai", "--login", "--auth-json", "auth.json"},
                         {"cai", "--export"}};
  int lengths[] = {2, 2, 4, 3, 4, 5, 4, 4, 4, 2};
  char *argv[9];
  size_t i;
  size_t j;
  for (i = 0U; i < 9U; i++)
    argv[i] = arguments[i];
  if (cai_cli_parse_options(9, argv, &options) != 1 ||
      strcmp(options.lockd, arguments[2]) != 0 ||
      strcmp(options.lockd_client_pem, "client.pem") != 0 ||
      strcmp(options.export_id, "session") != 0 ||
      strcmp(options.export_dir, "exports") != 0)
    return 1;
  for (i = 0U; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
    for (j = 0U; j < (size_t)lengths[i]; j++)
      argv[j] = cases[i][j];
    if (cai_cli_parse_options(lengths[i], argv, &options) != (i < 4U ? 1 : -1))
      return 1;
    if ((i == 0U && (!options.list || options.login)) ||
        ((i == 1U || i == 2U) && !options.resume_list) ||
        (i == 3U && strcmp(options.import_file, "conversation.jsonl") != 0))
      return 1;
  }
  return 0;
}

static int messaging_options(void) {
  cai_cli_options options;
  char first[] = "cai", flag[] = "--message-ack-timeout-ms";
  char off[] = "--no-messaging", on[] = "--autoqueue";
  char valid[] = "7", invalid[] = "86400001";
  char queue_flag[] = "--message-queue", queue[] = "custom-mailbox";
  char *args[7];
  args[0] = first;
  args[1] = off;
  args[2] = on;
  args[3] = flag;
  args[4] = valid;
  args[5] = queue_flag;
  args[6] = queue;
  if (cai_cli_parse_options(1, args, &options) != 1 || options.messaging ||
      options.enable_auto_enqueue || options.message_ack_timeout_ms != 300000L)
    return 1;
  if (cai_cli_parse_options(5, args, &options) != 1 || options.messaging ||
      !options.enable_auto_enqueue || options.message_ack_timeout_ms != 7L)
    return 1;
  if (cai_cli_parse_options(7, args, &options) != 1 ||
      options.message_queue == NULL ||
      strcmp(options.message_queue, queue) != 0)
    return 1;
  args[4] = invalid;
  if (cai_cli_parse_options(5, args, &options) != -1 ||
      strstr(options.diagnostic, "1..86400000") == NULL ||
      cai_cli_parse_options(4, args, &options) != -1 ||
      strstr(options.diagnostic, "requires") == NULL)
    return 1;
  return 0;
}

static int help_file_write(void *context, const void *bytes, size_t count,
                           cai_error *error) {
  (void)error;
  return fwrite(bytes, 1U, count, context) == count ? CAI_OK
                                                    : CAI_ERR_TRANSPORT;
}
static int help_columns(void) {
  FILE *stream = tmpfile();
  char line[512];
  size_t rows = 0U;
  int failed = 0;
  cai_sink *sink = NULL;
  cai_sink_callbacks callbacks;
  if (stream == NULL)
    return 1;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.context = stream;
  callbacks.write = help_file_write;
  if (cai_sink_from_callbacks(&callbacks, &sink, NULL) != CAI_OK ||
      cai_cli_help_write("cai", "cai", "CAI_", sink, NULL) != CAI_OK)
    failed = 1;
  cai_sink_close(sink);
  rewind(stream);
  if (fgets(line, sizeof(line), stream) == NULL ||
      strcmp(line, "cai and libcai Copyright (C) 2026 C89 Systems AB "
                   "https://c89.systems\n") != 0 ||
      fgets(line, sizeof(line), stream) == NULL || strcmp(line, "\n") != 0 ||
      fgets(line, sizeof(line), stream) == NULL ||
      strcmp(line, "Usage: cai [options]\n") != 0)
    failed = 1;
  while (fgets(line, sizeof(line), stream) != NULL) {
    if (strncmp(line, "  ", 2U) != 0)
      continue;
    rows++;
    if (strlen(line) <= 40U || line[38] != ' ' || line[39] == ' ' ||
        line[39] == '\n')
      failed = 1;
  }
  fclose(stream);
  return failed || rows < 60U;
}

static int capture_options(void) {
  char flag0[] = "cai", flag1[] = "--shell-context-max-lines";
  char flag2[] = "--shell-context-max-bytes", prompt[] = "-i";
  char invalid[][32] = {"0", "-1", "+2", "2x", "", "184467440737095516160"};
  char value[] = "17", bytes[] = "1024", instruction[] = "real instruction";
  char *args[7];
  cai_cli_options options;
  size_t i;
  args[0] = flag0;
  args[1] = flag1;
  args[2] = value;
  args[3] = flag2;
  args[4] = bytes;
  args[5] = prompt;
  args[6] = instruction;
  if (cai_cli_parse_options(7, args, &options) != 1 ||
      options.shell_context_max_lines != 17U ||
      options.shell_context_max_bytes != 1024U ||
      strcmp(cai_cli_instruction_at(&options, 0U), instruction) != 0 ||
      cai_cli_instruction_at(&options, 1U) != NULL)
    return 1;
  for (i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    args[2] = invalid[i];
    if (cai_cli_parse_options(3, args, &options) != -1)
      return 1;
  }
  return 0;
}

static int tool_options(void) {
  cai_cli_options options;
  char arg0[] = "cai", arg1[] = "-i", arg2[] = "actual prompt";
  char searxng[] = "--searxng-url", revgeo[] = "--revgeo-url";
  char socket[] = "--chrome-socket";
  char *args[5];
  char *flags[3];
  size_t i;
  flags[0] = searxng;
  flags[1] = revgeo;
  flags[2] = socket;
  args[0] = arg0;
  if (cai_cli_parse_options(1, args, &options) != 1 || !options.web_search ||
      !options.chrome || options.todo || options.searxng || options.revgeo)
    return 1;
  for (i = 0U; i < 3U; i++) {
    args[1] = flags[i];
    args[2] = arg1;
    args[3] = arg1;
    args[4] = arg2;
    if (cai_cli_parse_options(5, args, &options) != 1 ||
        options.instruction_count != 1U ||
        strcmp(cai_cli_instruction_at(&options, 0U), arg2) != 0 ||
        cai_cli_instruction_at(&options, 1U) != NULL)
      return 1;
  }
  return 0;
}
static int shell_options(void) {
  cai_cli_options options;
  char arg0[] = "cai", arg1[] = "--no-shell-context", arg2[] = "--no-terminal";
  char *args[3];
  args[0] = arg0;
  args[1] = arg1;
  args[2] = arg2;
  return cai_cli_parse_options(1, args, &options) != 1 ||
         !options.shell_context || options.model_explicit ||
         options.reasoning_effort_explicit ||
         options.reasoning_summary_explicit ||
         cai_cli_parse_options(3, args, &options) != 1 ||
         options.shell_context || options.terminal;
}

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
  char berget_raw[][80] = {"cai", "-p", "berget"};
  char *berget[3];
  char metadata_raw[][80] = {"cai", "--model-metadata", "/tmp/models.json",
                             "-i", "write a test"};
  char *metadata[5];
  char berget_override_raw[][80] = {"cai",         "-p", "berget", "-m",
                                    "other-model", "-r", "high"};
  char *berget_override[7];
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
  char invalid_login_raw[][64] = {"cai", "-p", "openai", "--login"};
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
  for (i = 0U; i < sizeof(berget) / sizeof(berget[0]); i++)
    berget[i] = berget_raw[i];
  for (i = 0U; i < sizeof(metadata) / sizeof(metadata[0]); i++)
    metadata[i] = metadata_raw[i];
  for (i = 0U; i < sizeof(berget_override) / sizeof(berget_override[0]); i++)
    berget_override[i] = berget_override_raw[i];
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
      !options.model_explicit || !options.reasoning_effort_explicit ||
      options.reasoning_summary_explicit ||
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
      !options.reasoning_summary_explicit ||
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
  if (cai_cli_parse_options(3, berget, &options) != 1 ||
      strcmp(options.model, "zai-org/GLM-5.3-Flash") != 0 ||
      strcmp(options.reasoning_effort, "medium") != 0 ||
      options.image_generation)
    return 1;
  if (cai_cli_parse_options(5, metadata, &options) != 1 ||
      strcmp(options.model_metadata, "/tmp/models.json") != 0 ||
      options.instruction_count != 1U ||
      strcmp(cai_cli_instruction_at(&options, 0U), "write a test") != 0)
    return 1;
  if (cai_cli_parse_options(7, berget_override, &options) != 1 ||
      strcmp(options.model, "other-model") != 0 ||
      strcmp(options.reasoning_effort, "high") != 0)
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
  return value_option_instructions() || config_option_precedence() ||
         replay_options() || resume_alias_options() ||
         short_argument_options() || grouped_value_options() ||
         help_columns() || shell_options() || storage_options() ||
         tool_options() || capture_options() || messaging_options();
}
