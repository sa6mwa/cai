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
  unlink(path);
  return failed != 0;
}
