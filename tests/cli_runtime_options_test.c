#define _POSIX_C_SOURCE 200809L
#include "../cli/config.h"
#include "../cli/runtime_options.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Exercise the production config/INI/env/flag translation, without opening a
 * database or asking an agent to complete a turn just to observe a setting. */
static int resolve(const char *file, const char *flag, int expected) {
  static const char *const fixed[] = {"cai",
                                      "--config",
                                      NULL,
                                      "-p",
                                      "custom",
                                      "--endpoint",
                                      "http://127.0.0.1:1/v1",
                                      "-m",
                                      "gpt-6-luna"};
  char *args[12] = {NULL};
  cai_cli_options options;
  cai_agent_runtime_config runtime;
  cai_agent_preset preset;
  cai_skill_config skills;
  cai_config *configuration = NULL;
  cai_error error;
  int result, actual = 0, count = 9;
  int i;
  for (i = 0; i < count; i++)
    args[i] = strdup(i == 2 ? file : fixed[i]);
  if (flag != NULL)
    args[count++] = strdup(flag);
  cai_error_init(&error);
  result =
      cai_cli_configure(count, args, &options, &configuration, NULL, &error);
  if (expected < 0) {
    result = result < 0 && error.message != NULL &&
                     strstr(error.message, "CAI_TODO") != NULL
                 ? 0
                 : 1;
  } else if (result != 1) {
    result = 1;
  } else {
    cai_agent_runtime_config_init(&runtime);
    result = configuration->apply_runtime(configuration, &runtime, &error);
    cai_cli_apply_runtime_options(&options, &runtime, &preset, &skills);
    if (!runtime.disable_chrome)
      actual |= 1;
    if (!runtime.disable_web_search)
      actual |= 2;
    if (runtime.enable_todo)
      actual |= 4;
    if (runtime.enable_searxng)
      actual |= 8;
    if (runtime.enable_revgeo)
      actual |= 16;
    if (actual != expected || runtime.preset_descriptor != &preset ||
        strcmp(runtime.model, "gpt-6-luna") != 0 ||
        !runtime.codex_compat_agents_md ||
        runtime.agent_config_directory == NULL ||
        strcmp(runtime.agent_config_directory, "/fixture/config") != 0 ||
        runtime.global_agents_md_path == NULL ||
        strcmp(runtime.global_agents_md_path, "/fixture/AGENTS.md") != 0 ||
        strcmp(runtime.service_tier, "priority") != 0 ||
        strcmp(runtime.searxng.base_url, "http://fixture/search") != 0 ||
        strcmp(runtime.revgeo.base_url, "http://fixture/geo") != 0)
      result = 1;
  }
  if (result != 0)
    fprintf(stderr, "runtime settings: flag=%s expected=%d actual=%d: %s\n",
            flag != NULL ? flag : "(none)", expected, actual,
            error.message != NULL ? error.message : "mismatch");
  cai_config_close(configuration);
  cai_error_cleanup(&error);
  for (i = 0; i < count; i++)
    free(args[i]);
  return result;
}

static int write_config(const char *path, int precedence) {
  FILE *stream = fopen(path, "w");
  if (stream == NULL)
    return 1;
  fprintf(
      stream,
      "config_env_has_precedence = %s\n"
      "[agent]\ntodo = false\nsearxng = false\nrevgeo = false\n"
      "web_search = false\ncodex_agents_md = true\nservice_tier = priority\n"
      "[chrome]\nenabled = false\n"
      "[paths]\nconfig_dir = /fixture/config\nagents_md = /fixture/AGENTS.md\n"
      "[searxng]\nbase_url = http://fixture/search\n"
      "[revgeo]\nbase_url = http://fixture/geo\n"
      "[env]\nCAI_TODO = true\n",
      precedence ? "true" : "false");
  return fclose(stream) != 0;
}

static int messaging_settings(const char *path) {
  char *args[] = {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
  const char *fixed[] = {"cai",         "--config",
                         NULL,          "--no-messaging",
                         "--autoqueue", "--message-ack-timeout-ms",
                         "7",           "--message-queue",
                         "flag-mailbox"};
  cai_cli_options options;
  cai_config *configuration = NULL;
  cai_error error;
  FILE *stream;
  int i, failed = 0;
  stream = fopen(path, "w");
  if (stream == NULL)
    return 1;
  fputs("[messaging]\nenabled = false\nenable_auto_enqueue = "
        "true\nack_timeout_ms = 123\nqueue_name = ini-mailbox\n",
        stream);
  if (fclose(stream) != 0)
    return 1;
  for (i = 0; i < 9; i++)
    args[i] = strdup(i == 2 ? path : fixed[i]);
  cai_error_init(&error);
  unsetenv("CAI_MESSAGING");
  unsetenv("CAI_ENABLE_AUTO_ENQUEUE");
  unsetenv("CAI_MESSAGE_ACK_TIMEOUT_MS");
  unsetenv("CAI_MESSAGE_QUEUE");
  for (i = 0; i < 3; i++) {
    int rc = cai_cli_configure(i == 2 ? 9 : 3, args, &options, &configuration,
                               NULL, &error);
    if (rc != 1 || options.message_queue == NULL ||
        strcmp(options.message_queue, i == 0   ? "ini-mailbox"
                                      : i == 1 ? "env-mailbox"
                                               : "flag-mailbox") != 0 ||
        options.messaging != (i == 1) ||
        options.enable_auto_enqueue != (i != 1) ||
        options.message_ack_timeout_ms != (i == 0   ? 123L
                                           : i == 1 ? 456L
                                                    : 7L))
      failed = 1;
    cai_config_close(configuration);
    configuration = NULL;
    setenv("CAI_MESSAGING", "true", 1);
    setenv("CAI_ENABLE_AUTO_ENQUEUE", "false", 1);
    setenv("CAI_MESSAGE_ACK_TIMEOUT_MS", "456", 1);
    setenv("CAI_MESSAGE_QUEUE", "env-mailbox", 1);
  }
  setenv("CAI_MESSAGE_ACK_TIMEOUT_MS", "0", 1);
  if (cai_cli_configure(3, args, &options, &configuration, NULL, &error) !=
          -1 ||
      error.message == NULL)
    failed = 1;
  cai_config_close(configuration);
  cai_error_cleanup(&error);
  for (i = 0; i < 9; i++)
    free(args[i]);
  unsetenv("CAI_MESSAGING");
  unsetenv("CAI_ENABLE_AUTO_ENQUEUE");
  unsetenv("CAI_MESSAGE_ACK_TIMEOUT_MS");
  unsetenv("CAI_MESSAGE_QUEUE");
  return failed;
}

/* Exercise every configuration source without storage or model requests. */
static int history_settings(const char *path, int prompts) {
  static char program[] = "cai", config_flag[] = "--config", list[] = "--list";
  static char replay_flag[] = "--replay-turns";
  static char prompt_flag[] = "--prompt-history-limit";
  static char retention_flag[] = "--conversation-retention-days";
  static char values[][32] = {"-2", "-1",  "0",
                              "20", "bad", "999999999999999999999999"};
  char *args[6];
  cai_app_config app;
  cai_app_setting setting;
  cai_cli_options options;
  cai_config *configuration;
  cai_error error;
  FILE *file;
  size_t i;
  int source, rc, valid, failed = 0;
  const char *key = prompts == 2 ? "conversation_retention_days"
                    : prompts    ? "prompt_history_limit"
                                 : "replay_turns";
  const char *environment = prompts == 2 ? "CAI_CONVERSATION_RETENTION_DAYS"
                            : prompts    ? "CAI_PROMPT_HISTORY_LIMIT"
                                         : "CAI_REPLAY_TURNS";
  args[0] = program;
  args[1] = config_flag;
  args[2] = strdup(path);
  args[3] = list;
  args[4] = prompts == 2 ? retention_flag : prompts ? prompt_flag : replay_flag;
  if (args[2] == NULL)
    return 1;
  setting.section = prompts == 2 ? "storage" : "ui";
  setting.key = key;
  for (source = 0; source < 4; source++) {
    for (i = 0U; i < sizeof(values) / sizeof(values[0]); i++) {
      file = fopen(path, "w");
      if (file == NULL) {
        free(args[2]);
        return 1;
      }
      if (source == 0)
        fprintf(file, "[%s]\n%s = %s\n", setting.section, key, values[i]);
      if (fclose(file) != 0) {
        free(args[2]);
        return 1;
      }
      unsetenv(environment);
      if (source == 1)
        setenv(environment, values[i], 1);
      memset(&app, 0, sizeof(app));
      app.program_name = CAI_APP_DEFAULT_PROGRAM_NAME;
      setting.value = values[i];
      app.settings = &setting;
      app.setting_count = source == 2 ? 1U : 0U;
      args[5] = values[i];
      configuration = NULL;
      cai_cli_options_init(&options);
      cai_error_init(&error);
      rc = cai_cli_configure(source == 3 ? 6 : 4, args, &options,
                             &configuration, &app, &error);
      valid = i >= (prompts ? 2U : 1U) && i <= 3U;
      if ((valid &&
           (rc != 1 || (prompts == 2 ? options.conversation_retention_days
                        : prompts    ? options.prompt_history_limit
                                     : options.replay_turns) !=
                           strtol(values[i], NULL, 10))) ||
          (!valid && (rc != -1 || (error.message == NULL &&
                                   options.diagnostic[0] == '\0')))) {
        fprintf(stderr, "%s source=%d value=%s rc=%d: %s\n", key, source,
                values[i], rc,
                error.message != NULL ? error.message : options.diagnostic);
        failed = 1;
      }
      cai_config_close(configuration);
      cai_error_cleanup(&error);
    }
  }
  unsetenv(environment);
  free(args[2]);
  return failed;
}

static int retained_paths(const char *file, const char *workspace) {
  static const char *const flags[] = {"--agents-md",        "--skills-dir",
                                      "--model-metadata",   "--auth-json",
                                      "--lockd-client-pem", "--chrome-socket"};
  static const char *const values[] = {"policy.md",   "skills",
                                       "models.json", "auth.json",
                                       "client.pem",  "chrome.sock"};
  const char **resolved[6];
  cai_cli_options options;
  cai_config *configuration = NULL;
  cai_error error;
  char original[4096], selected[4096], expected[8192];
  char *args[17];
  FILE *stream;
  size_t i;
  int rc, source, failed = 0;
  if (getcwd(original, sizeof(original)) == NULL)
    return 1;
  args[0] = strdup("cai");
  args[1] = strdup("--config");
  args[2] = strdup(file);
  args[3] = strdup("-C");
  args[4] = strdup(workspace);
  for (i = 0U; i < 6U; i++) {
    args[5U + 2U * i] = strdup(flags[i]);
    args[6U + 2U * i] = strdup(values[i]);
  }
  cai_error_init(&error);
  for (source = 0; source < 2; source++) {
    stream = fopen(file, "w");
    if (stream == NULL)
      break;
    if (source == 1) {
      fputs("[paths]\nagents_md=policy.md\nskills_dir=skills\n"
            "model_metadata=models.json\nauth_json=auth.json\n"
            "[store.default]\nclient_pem=client.pem\n"
            "[chrome]\nsocket=chrome.sock\n",
            stream);
      free(args[4]);
      args[4] = strdup(original);
    }
    if (fclose(stream) != 0)
      break;
    rc = cai_cli_configure(source == 0 ? 17 : 5, args, &options, &configuration,
                           NULL, &error);
    if (rc != 1 || getcwd(selected, sizeof(selected)) == NULL)
      failed = 1;
    else {
      resolved[0] = &options.agents_md;
      resolved[1] = &options.skills_dir;
      resolved[2] = &options.model_metadata;
      resolved[3] = &options.auth_json;
      resolved[4] = &options.lockd_client_pem;
      resolved[5] = &options.chrome_socket;
      for (i = 0U; i < 6U; i++) {
        snprintf(expected, sizeof(expected), "%s/%s",
                 source == 0 ? selected : workspace, values[i]);
        if (*resolved[i] == NULL || strcmp(*resolved[i], expected) != 0) {
          fprintf(stderr, "%s lost its path origin (source=%d): %s\n", flags[i],
                  source, *resolved[i] != NULL ? *resolved[i] : "(null)");
          failed = 1;
        }
      }
    }
    cai_config_close(configuration);
    configuration = NULL;
  }
  if (source != 2)
    failed = 1;
  if (chdir(original) != 0)
    failed = 1;
  cai_config_close(configuration);
  cai_error_cleanup(&error);
  for (i = 0U; i < 17U; i++)
    free(args[i]);
  return failed;
}

static int chrome_directory_settings(const char *file, const char *workspace) {
  static char program[] = "cai", config_flag[] = "--config", list[] = "--list";
  static const char *const names[] = {"cai", "smith"};
  char *args[4];
  const char *runtime = getenv("XDG_RUNTIME_DIR");
  char *saved = runtime != NULL ? strdup(runtime) : NULL;
  char directory[4096], expected[8192];
  cai_app_config app;
  cai_cli_options options;
  cai_config *cfg;
  cai_error error;
  FILE *stream;
  size_t name;
  int mode, rc, failed = 0;
  args[0] = program;
  args[1] = config_flag;
  args[2] = strdup(file);
  args[3] = list;
  snprintf(directory, sizeof(directory), "%s/runtime", workspace);
  for (name = 0U; name < 2U; name++) {
    memset(&app, 0, sizeof(app));
    app.program_name = names[name];
    for (mode = 0; mode < 6; mode++) {
      stream = fopen(file, "w");
      if (stream == NULL) {
        failed = 1;
        break;
      }
      if (mode >= 4)
        fputs("[chrome]\nsocket_directory = override\n", stream);
      if (fclose(stream) != 0) {
        failed = 1;
        break;
      }
      if (mode == 0)
        unsetenv("XDG_RUNTIME_DIR");
      else
        setenv("XDG_RUNTIME_DIR",
               mode == 1                ? ""
               : mode == 3 || mode == 5 ? "relative"
                                        : directory,
               1);
      cfg = NULL;
      cai_error_init(&error);
      rc = cai_cli_configure(4, args, &options, &cfg, &app, &error);
      if (mode == 3) {
        if (rc != -1 || error.message == NULL)
          failed = 1;
      } else if (rc != 1)
        failed = 1;
      else {
        if (mode >= 4)
          snprintf(expected, sizeof(expected), "%s/override", workspace);
        else if (mode == 2)
          snprintf(expected, sizeof(expected), "%s/%s/chrome", directory,
                   names[name]);
        else
          snprintf(expected, sizeof(expected), "%s/chrome", options.cache_dir);
        if (strcmp(cfg->get(cfg, "chrome", "socket_directory"), expected) !=
            0) {
          fprintf(stderr, "Chrome directory mismatch: name=%s mode=%d\n",
                  names[name], mode);
          failed = 1;
        }
      }
      cai_config_close(cfg);
      cai_error_cleanup(&error);
    }
  }
  if (saved != NULL)
    setenv("XDG_RUNTIME_DIR", saved, 1);
  else
    unsetenv("XDG_RUNTIME_DIR");
  free(saved);
  free(args[2]);
  return failed;
}

static int yolo_settings(const char *path) {
  char *args[5];
  cai_cli_options options;
  cai_agent_runtime_config runtime;
  cai_agent_preset preset;
  cai_skill_config skills;
  cai_config *configuration = NULL;
  cai_error error;
  FILE *stream;
  int i, failed = 0;
  stream = fopen(path, "w");
  if (stream == NULL)
    return 1;
  fputs("[agent]\nyolo = true\n", stream);
  if (fclose(stream) != 0)
    return 1;
  args[0] = strdup("cai");
  args[1] = strdup("--config");
  args[2] = strdup(path);
  args[3] = strdup("--yolo");
  args[4] = NULL;
  cai_error_init(&error);
  unsetenv("CAI_YOLO");
  for (i = 0; i < 3; i++) {
    if (i == 1)
      setenv("CAI_YOLO", "false", 1);
    if (cai_cli_configure(i == 2 ? 4 : 3, args, &options, &configuration, NULL,
                          &error) != 1) {
      fprintf(stderr, "yolo settings case %d: %s\n", i,
              error.message != NULL ? error.message : "configuration failed");
      failed = 1;
    } else {
      cai_agent_runtime_config_init(&runtime);
      cai_cli_apply_runtime_options(&options, &runtime, &preset, &skills);
      if (runtime.yolo != (i != 1)) {
        fprintf(stderr, "yolo settings case %d: expected %d got %d\n", i,
                i != 1, runtime.yolo);
        failed = 1;
      }
    }
    cai_config_close(configuration);
    configuration = NULL;
  }
  setenv("CAI_YOLO", "invalid", 1);
  if (cai_cli_configure(3, args, &options, &configuration, NULL, &error) !=
      -1) {
    fprintf(stderr, "invalid YOLO environment accepted\n");
    failed = 1;
  }
  cai_config_close(configuration);
  configuration = NULL;
  unsetenv("CAI_YOLO");
  cai_error_cleanup(&error);
  cai_error_init(&error);
  free(args[3]);
  args[3] = strdup("--no-yolo");
  if (cai_cli_configure(4, args, &options, &configuration, NULL, &error) != 1 ||
      options.yolo) {
    fprintf(stderr, "--no-yolo failed: %s\n",
            error.message != NULL ? error.message : "value mismatch");
    failed = 1;
  }
  cai_config_close(configuration);
  cai_error_cleanup(&error);
  for (i = 0; i < 4; i++)
    free(args[i]);
  return failed;
}

static int status_fields_settings(char *path) {
  cai_cli_options options;
  cai_config *configuration = NULL;
  cai_app_config app;
  cai_app_setting setting;
  cai_error error;
  static char program[] = "cai", config_flag[] = "--config";
  static char status_flag[] = "--status-fields",
              selection[] = "provider:default";
  char *args[] = {program, config_flag, NULL, status_flag, selection};
  const char *expected[] = {"dir:model",    "ctx:model", "provider:default",
                            "goal:default", "dir:model", NULL};
  int i, rc, failed = 0;
  FILE *file = fopen(path, "w");
  if (file == NULL)
    return 1;
  fputs("[ui]\nstatus_fields = dir:model\n", file);
  fclose(file);
  args[2] = path;
  cai_error_init(&error);
  memset(&app, 0, sizeof(app));
  setting.section = "ui";
  setting.key = "status_fields";
  setting.value = "goal:default";
  app.settings = &setting;
  app.setting_count = 1U;
  unsetenv("CAI_STATUS_FIELDS");
  unsetenv("CAI_UI_STATUS_FIELDS");
  for (i = 0; i < 6; i++) {
    if (i == 1)
      setenv("CAI_STATUS_FIELDS", "ctx:model", 1);
    if (i == 4)
      unsetenv("CAI_STATUS_FIELDS");
    if (i == 5)
      setenv("CAI_STATUS_FIELDS", "model:bogus", 1);
    rc = cai_cli_configure(i == 2 ? 5 : 3, args, &options, &configuration,
                           i == 3 ? &app : NULL, &error);
    if (i == 5 ? rc != -1 || error.message == NULL ||
                     strstr(error.message, "status_fields") == NULL
               : rc != 1 || options.status_fields == NULL ||
                     strcmp(options.status_fields, expected[i]) != 0) {
      fprintf(stderr, "status fields precedence case %d: %s\n", i,
              error.message != NULL ? error.message : "unexpected value");
      failed = 1;
    }
    cai_config_close(configuration);
    configuration = NULL;
    cai_error_cleanup(&error);
  }
  unsetenv("CAI_STATUS_FIELDS");
  return failed;
}

static int review_fix_settings(const char *path) {
  char raw[][32] = {"cai", "--config", "--no-review-and-fix-tool"};
  char *args[4];
  cai_cli_options options;
  cai_agent_runtime_config runtime;
  cai_agent_preset preset;
  cai_skill_config skills;
  cai_config *cfg = NULL;
  cai_error error;
  FILE *stream;
  int i, failed = 0;
  args[0] = raw[0];
  args[1] = raw[1];
  args[3] = raw[2];
  args[2] = strdup(path);
  if (args[2] == NULL)
    return 1;
  cai_error_init(&error);
  unsetenv("CAI_REVIEW_AND_FIX_TOOL");
  unsetenv("CAI_AGENT_REVIEW_AND_FIX_TOOL");
  for (i = 0; i < 5; i++) {
    stream = fopen(path, "w");
    if (stream == NULL)
      return 1;
    fputs(i == 0 ? "" : "[agent]\nreview_and_fix_tool=true\n", stream);
    if (fclose(stream) != 0)
      return 1;
    if (i == 2 || i == 3)
      setenv("CAI_REVIEW_AND_FIX_TOOL", "false", 1);
    if (i == 4) {
      unsetenv("CAI_REVIEW_AND_FIX_TOOL");
      setenv("CAI_AGENT_REVIEW_AND_FIX_TOOL", "true", 1);
    }
    strcpy(raw[2],
           i == 3 ? "--review-and-fix-tool" : "--no-review-and-fix-tool");
    if (cai_cli_configure(i >= 3 ? 4 : 3, args, &options, &cfg, NULL, &error) !=
        1)
      failed = 1;
    else {
      cai_agent_runtime_config_init(&runtime);
      if (cfg->apply_runtime(cfg, &runtime, &error) != CAI_OK)
        failed = 1;
      cai_cli_apply_runtime_options(&options, &runtime, &preset, &skills);
      if (runtime.enable_review_and_fix != (i == 1 || i == 3) ||
          runtime.disable_review_subagent)
        failed = 1;
    }
    cai_config_close(cfg);
    cfg = NULL;
  }
  unsetenv("CAI_REVIEW_AND_FIX_TOOL");
  unsetenv("CAI_AGENT_REVIEW_AND_FIX_TOOL");
  cai_error_cleanup(&error);
  free(args[2]);
  return failed;
}

int main(int argc, char **argv) {
  static const char *const env[] = {"CAI_TODO",       "CAI_CHROME",
                                    "CAI_WEB_SEARCH", "CAI_SEARXNG",
                                    "CAI_REVGEO",     NULL};
  char path[4096];
  int fd, failed = 0;
  size_t i;
  if (argc != 2 || snprintf(path, sizeof(path), "%s/runtime-options-XXXXXX",
                            argv[1]) >= (int)sizeof(path))
    return 1;
  fd = mkstemp(path);
  if (fd < 0)
    return 1;
  close(fd);
  for (i = 0U; env[i] != NULL; i++)
    unsetenv(env[i]);
  failed |= write_config(path, 0);
  failed |= resolve(path, NULL, 4);
  failed |= resolve(path, "--no-todo", 0);
  failed |= resolve(path, "--chrome", 5);
  failed |= resolve(path, "--web-search", 6);
  failed |= resolve(path, "--searxng", 12);
  failed |= resolve(path, "--revgeo", 20);
  for (i = 0U; env[i] != NULL; i++)
    setenv(env[i], "true", 1);
  failed |= resolve(path, NULL, 31);
  failed |= resolve(path, "--no-todo", 27);
  failed |= resolve(path, "--no-chrome", 30);
  failed |= resolve(path, "--no-web-search", 29);
  failed |= resolve(path, "--no-searxng", 23);
  failed |= resolve(path, "--no-revgeo", 15);
  setenv("CAI_TODO", "invalid", 1);
  failed |= resolve(path, NULL, -1);
  for (i = 0U; env[i] != NULL; i++)
    unsetenv(env[i]);
  setenv("CAI_TODO", "false", 1);
  failed |= resolve(path, NULL, 0);
  failed |= write_config(path, 1);
  failed |= resolve(path, NULL, 4);
  failed |= resolve(path, "--no-todo", 0);
  failed |= messaging_settings(path);
  failed |= history_settings(path, 0);
  failed |= history_settings(path, 1);
  failed |= history_settings(path, 2);
  failed |= retained_paths(path, argv[1]);
  failed |= chrome_directory_settings(path, argv[1]);
  failed |= yolo_settings(path);
  failed |= status_fields_settings(path);
  failed |= review_fix_settings(path);
  unlink(path);
  return failed != 0;
}
