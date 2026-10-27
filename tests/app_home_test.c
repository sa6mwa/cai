#define _POSIX_C_SOURCE 200809L
#include "../cli/config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Observe the production path resolver without database opens or inference. */
static cai_cli_options options;
static cai_config *cfg;
static cai_error error;
static char original[4096];
static char *arguments[24];

static void free_arguments(void) {
  size_t i;
  for (i = 0U; i < sizeof(arguments) / sizeof(arguments[0]); i++) {
    free(arguments[i]);
    arguments[i] = NULL;
  }
}

static void load(const cai_app_config *app, const char *const *flags,
                 size_t count, int expected) {
  size_t i;
  int rc;
  assert(count + 2U <= sizeof(arguments) / sizeof(arguments[0]));
  cai_config_close(cfg);
  cfg = NULL;
  free_arguments();
  cai_error_cleanup(&error);
  assert(chdir(original) == 0);
  arguments[0] = strdup("host");
  for (i = 0U; i < count; i++) {
    arguments[i + 1U] = strdup(flags[i]);
    assert(arguments[i + 1U] != NULL);
  }
  arguments[count + 1U] = strdup("--list");
  assert(arguments[0] != NULL && arguments[count + 1U] != NULL);
  rc =
      cai_cli_configure((int)count + 2, arguments, &options, &cfg, app, &error);
  if (rc != expected)
    fprintf(stderr, "home configuration: %s (%s)\n",
            error.message != NULL ? error.message : options.diagnostic,
            count != 0U ? flags[0] : "host defaults");
  assert(rc == expected);
}

static void equal(const char *actual, const char *expected) {
  if (actual == NULL || strcmp(actual, expected) != 0)
    fprintf(stderr, "path mismatch: %s != %s\n",
            actual != NULL ? actual : "(null)", expected);
  assert(actual != NULL && strcmp(actual, expected) == 0);
}

static void under(const char *actual, const char *root, const char *child) {
  char path[8192];
  snprintf(path, sizeof(path), "%s/%s", root, child);
  equal(actual, path);
}

static void roots(const char *root) {
  equal(options.home_dir, root);
  under(options.config_dir, root, "config");
  under(options.state_dir, root, "state");
  under(options.cache_dir, root, "cache");
  under(options.data_dir, root, "data");
  under(options.runtime_dir, root, "run");
  under(options.history_dir, root, "state/history");
  under(options.pouch_key_file, root, "state/pouch.key");
  under(options.clipboard_dir, root, "cache/clipboard");
  under(options.log_dir, root, "cache");
  under(options.export_dir, root, "data/exports");
  under(cfg->get(cfg, "chrome", "socket_directory"), root, "run/chrome");
}

static void write_ini(const char *path, const char *body) {
  FILE *file = fopen(path, "w");
  assert(file != NULL);
  assert(fputs(body, file) >= 0 && fclose(file) == 0);
}

int main(void) {
  cai_app_config app;
  char home[8192], selected[8192], cwd[8192];
  const char *flags[12];
  const char *process_home = getenv("HOME");
  assert(getcwd(original, sizeof(original)) != NULL);
  assert(process_home != NULL);
  snprintf(home, sizeof(home), "%s/unified", original);
  snprintf(selected, sizeof(selected), "%s/selected.ini", original);
  cai_app_config_init(&app);
  cai_error_init(&error);
  assert(app.home_directory == NULL && app.runtime_directory == NULL);
  flags[0] = "--home";
  flags[1] = home;
  load(&app, flags, 2U, 1);
  roots(home);
  equal(getenv("HOME"), process_home);
  snprintf(cwd, sizeof(cwd), "%s/unified/config/config.ini", original);
  write_ini(cwd, "[agent]\nidentity = home-config-loaded\n");
  load(&app, flags, 2U, 1);
  equal(options.identity, "home-config-loaded");

  /* A relative root stays anchored before an INI workspace change. */
  write_ini(selected, "[paths]\ndirectory = moved\n");
  assert(mkdir("moved", 0700) == 0);
  flags[0] = "-c";
  flags[1] = selected;
  flags[2] = "--home";
  flags[3] = "unified";
  load(&app, flags, 4U, 1);
  roots(home);
  assert(getcwd(cwd, sizeof(cwd)) != NULL);
  under(cwd, original, "moved");

  /* INI root paths are config-relative. Loading this file does not redirect
   * configuration discovery or generate another config.ini. */
  write_ini(selected, "[paths]\nhome_dir = ini-home\n"
                      "[agent]\nidentity = selected-file\n");
  load(&app, flags, 2U, 1);
  snprintf(home, sizeof(home), "%s/ini-home", original);
  roots(home);
  equal(options.identity, "selected-file");
  snprintf(cwd, sizeof(cwd), "%s/ini-home/config/config.ini", original);
  assert(access(cwd, F_OK) != 0);

  /* Root precedence is independent of explicit per-path precedence. */
  assert(setenv("CAI_HOME", "environment-home", 1) == 0);
  assert(setenv("CAI_CACHE_DIR", "/specific/cache", 1) == 0);
  write_ini(selected, "[paths]\nhome_dir = ini-home\n"
                      "state_dir = /specific/state\n"
                      "history_dir = /specific/history\n");
  flags[4] = "--runtime-dir";
  flags[5] = "/specific/runtime";
  app.data_directory = "/specific/data";
  app.config_directory = "/specific/config";
  load(&app, flags, 6U, 1);
  under(options.home_dir, original, "unified");
  equal(options.config_dir, app.config_directory);
  equal(options.state_dir, "/specific/state");
  equal(options.cache_dir, "/specific/cache");
  equal(options.data_dir, app.data_directory);
  equal(options.runtime_dir, "/specific/runtime");
  equal(options.history_dir, "/specific/history");
  equal(options.clipboard_dir, "/specific/cache/clipboard");
  equal(options.export_dir, "/specific/data/exports");
  equal(cfg->get(cfg, "chrome", "socket_directory"),
        "/specific/runtime/chrome");
  assert(unsetenv("CAI_CACHE_DIR") == 0);
  cai_app_config_init(&app);
  write_ini(selected, "[paths]\nhome_dir = ini-home\n");
  load(&app, flags, 2U, 1);
  snprintf(home, sizeof(home), "%s/environment-home", original);
  roots(home);
  assert(unsetenv("CAI_HOME") == 0);

  /* Environment entries follow the existing environment precedence policy. */
  write_ini(selected, "config_env_has_precedence = true\n"
                      "[env]\nCAI_HOME = env-section\n");
  assert(setenv("CAI_HOME", "external-home", 1) == 0);
  load(&app, flags, 2U, 1);
  snprintf(home, sizeof(home), "%s/env-section", original);
  roots(home);
  assert(unsetenv("CAI_HOME") == 0);

  /* A renamed app uses its own prefix and takes no extra name suffix. */
  app.program_name = "smith";
  app.home_directory = "host-home";
  flags[0] = "--no-config";
  load(&app, flags, 1U, 1);
  snprintf(home, sizeof(home), "%s/host-home", original);
  roots(home);
  assert(setenv("CAI_HOME", "/wrong-app", 1) == 0);
  assert(setenv("SMITH_HOME", "smith-home", 1) == 0);
  load(&app, flags, 1U, 1);
  snprintf(home, sizeof(home), "%s/smith-home", original);
  roots(home);
  assert(unsetenv("SMITH_HOME") == 0 && unsetenv("CAI_HOME") == 0);
  app.environment_prefix = "HOST_";
  assert(setenv("HOST_HOME", "custom-prefix", 1) == 0);
  load(&app, flags, 1U, 1);
  snprintf(home, sizeof(home), "%s/custom-prefix", original);
  roots(home);
  assert(unsetenv("HOST_HOME") == 0);
  assert(setenv("HOST_PATHS_HOME_DIR", "full-name", 1) == 0);
  load(&app, flags, 1U, 1);
  snprintf(home, sizeof(home), "%s/full-name", original);
  roots(home);
  assert(unsetenv("HOST_PATHS_HOME_DIR") == 0);

  /* No root preserves XDG lookup, and ~/. expansion uses the actual HOME. */
  cai_app_config_init(&app);
  load(&app, flags, 1U, 1);
  assert(options.home_dir == NULL);
  under(options.config_dir, getenv("XDG_CONFIG_HOME"), "cai");
  flags[1] = "--home";
  flags[2] = "~/portable";
  load(&app, flags, 3U, 1);
  snprintf(home, sizeof(home), "%s/portable", process_home);
  roots(home);
  flags[2] = "";
  load(&app, flags, 3U, -1);
  flags[1] = "-c";
  flags[2] = selected;
  load(&app, flags, 3U, -1);
  cai_config_close(cfg);
  free_arguments();
  cai_error_cleanup(&error);
  assert(chdir(original) == 0);
  return 0;
}
