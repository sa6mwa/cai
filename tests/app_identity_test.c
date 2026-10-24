#define _POSIX_C_SOURCE 200809L
#include "../cli/config.h"
#include "../cli/identity.h"
#include "../cli/pouch.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void ends(const char *value, const char *suffix) {
  assert(value != NULL && strlen(value) >= strlen(suffix));
  assert(strcmp(value + strlen(value) - strlen(suffix), suffix) == 0);
}
static void configure(cai_app_config *app, cai_cli_options *options,
                      cai_config **cfg, cai_error *error) {
  char *args[3];
  char program[] = "host", list[] = "--list";
  args[0] = program;
  args[1] = list;
  args[2] = NULL;
  assert(cai_cli_configure(2, args, options, cfg, app, error) == 1);
}
static size_t source_read(void *context, void *bytes, size_t capacity,
                          cai_error *error) {
  size_t *position = context, n = 2U - *position;
  (void)error;
  if (n > capacity)
    n = capacity;
  memcpy(bytes, "{}" + *position, n);
  *position += n;
  return n;
}
static cai_source *source(cai_error *error, size_t *position) {
  cai_source_callbacks callbacks;
  cai_source *out = NULL;
  memset(&callbacks, 0, sizeof(callbacks));
  *position = 0U;
  callbacks.read = source_read;
  callbacks.context = position;
  assert(cai_source_from_callbacks(&callbacks, &out, error) == CAI_OK);
  return out;
}
int main(void) {
  cai_app_config app;
  cai_cli_options o, other;
  cai_config *cfg = NULL, *second = NULL;
  cai_cli_pouch first, isolated, shared;
  cai_source *input = NULL, *loaded = NULL;
  cai_error error;
  char endpoint[4096], key[4096], state[4096], path[4096];
  size_t position;
  unsigned long long sequence;
  char id[129];
  cai_app *receiver = NULL;
  cai_app_setting settings[2];
  cai_error_init(&error);
  cai_app_config_init(&app);
  app.program_name = "smith";
  assert(setenv("CAI_MODEL", "wrong-model", 1) == 0);
  assert(setenv("SMITH_MODEL", "right-model", 1) == 0);
  configure(&app, &o, &cfg, &error);
  assert(strcmp(o.program_name, "smith") == 0);
  assert(strcmp(o.environment_prefix, "SMITH_") == 0);
  assert(strcmp(o.default_api_key_env, "SMITH_API_KEY") == 0);
  assert(strcmp(o.model, "right-model") == 0);
  assert(strcmp(o.queue_prefix, "smith") == 0);
  assert(strcmp(o.sessions_namespace, "smith.sessions") == 0);
  assert(strcmp(o.auth_namespace, "smith.auth") == 0);
  assert(strcmp(o.todo_namespace, "smith.todo") == 0);
  assert(strcmp(o.scopes_namespace, "smith.scopes") == 0);
  assert(strcmp(o.messages_namespace, "smith.messages") == 0);
  ends(o.config_dir, "/config/smith");
  ends(o.state_dir, "/state/smith");
  ends(o.cache_dir, "/cache/smith");
  ends(o.data_dir, "/data/smith");
  ends(o.history_dir, "/state/smith/history");
  ends(o.clipboard_dir, "/cache/smith/clipboard");
  ends(o.log_dir, "/cache/smith");
  ends(o.export_dir, "/data/smith/exports");
  ends(o.pouch_key_file, "/state/smith/pouch.key");
  ends(cfg->get(cfg, "chrome", "socket_directory"), "/cache/smith/chrome");
  assert(cai_cli_pouch_open_options(&first, &o, NULL, &error) == CAI_OK);
  input = source(&error, &position);
  assert(first.credentials.replace(first.credentials.context, "probe", input,
                                   &error) == CAI_OK);
  cai_source_close(input);
  input = source(&error, &position);
  assert(first.store.checkpoint(&first, "/workspace", "first", input, 1U,
                                &error) == CAI_OK);
  cai_source_close(input);
  snprintf(endpoint, sizeof(endpoint), "pouch://%s/pouch",
           first.state_directory);
  snprintf(key, sizeof(key), "%s", first.key_path);
  snprintf(state, sizeof(state), "%s/other", getenv("XDG_STATE_HOME"));
  cai_app_config_init(&app);
  app.program_name = "other";
  app.no_config = 1;
  configure(&app, &other, &second, &error);
  other.lockd = endpoint;
  other.pouch_key_file = key;
  assert(cai_cli_pouch_open_options(&isolated, &other, NULL, &error) == CAI_OK);
  assert(isolated.credentials.load(isolated.credentials.context, "probe",
                                   &loaded, &error) == CAI_OK &&
         loaded == NULL);
  assert(isolated.store.load_latest(&isolated, "/workspace", id, sizeof(id),
                                    &loaded, &sequence, &error) == CAI_OK &&
         loaded == NULL);
  cai_cli_pouch_close(&isolated);
  other.auth_namespace = o.auth_namespace;
  other.sessions_namespace = o.sessions_namespace;
  other.scopes_namespace = o.scopes_namespace;
  assert(cai_cli_pouch_open_options(&shared, &other, NULL, &error) == CAI_OK);
  assert(shared.credentials.load(shared.credentials.context, "probe", &loaded,
                                 &error) == CAI_OK &&
         loaded != NULL);
  cai_source_close(loaded);
  loaded = NULL;
  assert(shared.store.load_latest(&shared, "/workspace", id, sizeof(id),
                                  &loaded, &sequence, &error) == CAI_OK &&
         loaded != NULL);
  assert(strcmp(id, "first") == 0);
  cai_source_close(loaded);
  cai_cli_pouch_close(&shared);
  cai_cli_pouch_close(&first);
  cai_config_close(second);
  cai_config_close(cfg);
  /* Host-selected roots and independently overridden child paths. */
  snprintf(path, sizeof(path), "%s/vectis/cai.ini", getenv("XDG_CONFIG_HOME"));
  cai_app_config_init(&app);
  app.program_name = "smith";
  app.config_file = path;
  app.state_directory = state;
  app.environment_prefix = "BOT_";
  app.namespace_prefix = "vectis.smith";
  app.queue_prefix = "team";
  assert(setenv("BOT_MODEL", "bot-model", 1) == 0);
  assert(setenv("BOT_CACHE_DIR", getenv("XDG_CACHE_HOME"), 1) == 0);
  settings[0].section = "storage";
  settings[0].key = "auth_namespace";
  settings[0].value = "shared.auth";
  settings[1].section = "paths";
  settings[1].key = "log_dir";
  settings[1].value = state;
  app.settings = settings;
  app.setting_count = 2U;
  configure(&app, &o, &cfg, &error);
  assert(access(path, F_OK) == 0 && strcmp(o.state_dir, state) == 0);
  assert(strcmp(o.model, "bot-model") == 0 &&
         strcmp(o.queue_prefix, "team") == 0);
  assert(strcmp(o.sessions_namespace, "vectis.smith.sessions") == 0);
  assert(strcmp(o.auth_namespace, "shared.auth") == 0);
  assert(strcmp(o.log_dir, state) == 0 &&
         strcmp(o.cache_dir, getenv("XDG_CACHE_HOME")) == 0);
  ends(o.clipboard_dir, "/cache/clipboard");
  ends(o.history_dir, "/state/other/history");
  cai_config_close(cfg);
  /* Reject unusable identities before creating any configuration or store. */
  app.program_name = "../outside";
  assert(cai_app_open(&app, &receiver, &error) == CAI_ERR_INVALID &&
         receiver == NULL);
  cai_error_cleanup(&error);
  app.program_name = "smith";
  app.environment_prefix = "1BAD_";
  assert(cai_app_open(&app, &receiver, &error) == CAI_ERR_INVALID &&
         receiver == NULL);
  cai_error_cleanup(&error);
  return 0;
}
