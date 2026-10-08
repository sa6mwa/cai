#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include <cai/models.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct cli_field {
  const char *section, *key;
  size_t offset;
  cai_config_type type;
  unsigned int flags;
  const char *choices, *description;
} cli_field;
#define S(s, k, m, f, d)                                                       \
  {s, k, offsetof(cai_cli_options, m), CAI_CONFIG_STRING, f, NULL, d}
#define B(s, k, m, d)                                                          \
  {s, k, offsetof(cai_cli_options, m), CAI_CONFIG_BOOL, 0U, NULL, d}
static const cli_field fields[] = {
    S("paths", "directory", directory, CAI_CONFIG_PATH,
      "Default workspace; explicit -C wins."),
    S("paths", "auth_json", auth_json, CAI_CONFIG_PATH,
      "Optional auth file; automatic uses Codex first, then encrypted cai "
      "auth."),
    S("paths", "agents_md", agents_md, CAI_CONFIG_PATH,
      "Global AGENTS.md override."),
    S("paths", "config_dir", config_dir, CAI_CONFIG_PATH,
      "Global agent configuration/instructions root."),
    S("paths", "skills_dir", skills_dir, CAI_CONFIG_PATH,
      "Global skills directory; empty is automatic."),
    S("paths", "model_metadata", model_metadata, CAI_CONFIG_PATH,
      "Provider fallback catalog JSON file."),
    S("paths", "export_dir", export_dir, CAI_CONFIG_PATH,
      "Export root; automatic uses XDG_DATA_HOME/cai/exports."),
    S("paths", "lockd_client_pem", lockd_client_pem, CAI_CONFIG_PATH,
      "Remote lockd mTLS client PEM."),
    S("storage", "lockd", lockd, 0U,
      "Storage endpoint; automatic uses state/cai/pouch."),
    S("chrome", "socket", chrome_socket, CAI_CONFIG_PATH,
      "Chrome host socket; empty discovers hosts."),
    B("chrome", "enabled", chrome, "Enable the Chrome tool."),
    B("ui", "shell_context", shell_context,
      "Share captured !COMMAND output; /shell remains local."),
    {"ui", "tool_output", offsetof(cai_cli_options, tool_output),
     CAI_CONFIG_STRING, 0U, "compact|full",
     "Tool previews; empty selects compact."}};
#undef S
#undef B
static const char *const agent_keys[] = {"model", "reasoning_effort",
                                         "reasoning_summary", "identity",
                                         "developer_instructions"};
static const size_t agent_offsets[] = {
    offsetof(cai_cli_options, model),
    offsetof(cai_cli_options, reasoning_effort),
    offsetof(cai_cli_options, reasoning_summary),
    offsetof(cai_cli_options, identity),
    offsetof(cai_cli_options, developer_instructions)};
static const char *const review_keys[] = {"model", "reasoning_effort",
                                          "reasoning_summary"};
static const size_t review_offsets[] = {
    offsetof(cai_cli_options, review_model),
    offsetof(cai_cli_options, review_reasoning_effort),
    offsetof(cai_cli_options, review_reasoning_summary)};

static int fail(cai_error *error, const char *path, const char *reason) {
  char message[1024];
  snprintf(message, sizeof(message), "%s: %s",
           path != NULL ? path : "configuration", reason);
  if (error != NULL) {
    cai_error_cleanup(error);
    error->code = CAI_ERR_INVALID;
    error->message = strdup(message);
  }
  return -1;
}
static int absolute_path(char *out, size_t size, const char *path,
                         cai_error *error) {
  char cwd[PATH_MAX];
  int n;
  if (path[0] == '/')
    n = snprintf(out, size, "%s", path);
  else {
    if (getcwd(cwd, sizeof(cwd)) == NULL)
      return fail(error, path, "resolve current directory");
    n = snprintf(out, size, "%s/%s", cwd, path);
  }
  return n < 0 || (size_t)n >= size
             ? fail(error, path, "configuration filename too long")
             : 1;
}
static int open_schema(cai_config **out, cai_error *error) {
  cai_config_field extension[sizeof(fields) / sizeof(fields[0])];
  cai_config *sdk = NULL;
  cai_config_field *selected;
  cai_cli_options defaults;
  size_t i, count = 0U;
  int rc;
  cai_cli_options_init(&defaults);
  for (i = 0U; i < sizeof(fields) / sizeof(fields[0]); i++) {
    const cli_field *f = &fields[i];
    const unsigned char *p = (const unsigned char *)&defaults + f->offset;
    extension[i].section = f->section;
    extension[i].key = f->key;
    extension[i].type = f->type;
    extension[i].flags = f->flags;
    extension[i].choices = f->choices;
    extension[i].description = f->description;
    extension[i].default_value =
        f->type == CAI_CONFIG_STRING ? *(const char *const *)p
        : f->type == CAI_CONFIG_BOOL ? (*(const int *)p ? "true" : "false")
                                     : "0";
  }
  rc = cai_config_open(extension, sizeof(extension) / sizeof(extension[0]),
                       &sdk, error);
  if (rc != CAI_OK)
    return -1;
  selected = calloc(sdk->field_count(sdk), sizeof(*selected));
  if (selected == NULL) {
    cai_config_close(sdk);
    return fail(error, NULL,
                "cannot allocate application configuration schema");
  }
  for (i = 0U; i < sdk->field_count(sdk); i++) {
    const cai_config_field *f = sdk->field_at(sdk, i);
    if (strcmp(f->section, "response") == 0 ||
        strcmp(f->section, "session_limits") == 0)
      continue;
    if (strcmp(f->section, "runtime") == 0 &&
        strcmp(f->key, "event_queue_limit") != 0 &&
        strcmp(f->key, "steering_queue_limit") != 0 &&
        strcmp(f->key, "turn_queue_limit") != 0 &&
        strcmp(f->key, "logger_disabled") != 0)
      continue;
    selected[count] = *f;
    if (strcmp(f->section, "agent") == 0 && strcmp(f->key, "preset") == 0) {
      selected[count].choices = "smith";
      selected[count].default_value = "smith";
      selected[count].description =
          "Smith agent preset; --review selects isolated review mode.";
    }
    count++;
  }
  rc = cai_config_open_schema(selected, count, out, error);
  free(selected);
  cai_config_close(sdk);
  if (rc != CAI_OK)
    return -1;
  rc = cai_config_set_default(*out, "client", "provider", defaults.provider,
                              error);
  for (i = 0U; rc == CAI_OK && i < sizeof(agent_keys) / sizeof(agent_keys[0]);
       i++)
    rc = cai_config_set_default(
        *out, "agent", agent_keys[i],
        *(const char *const *)((const unsigned char *)&defaults +
                               agent_offsets[i]),
        error);
  if (rc == CAI_OK)
    rc = cai_config_set_default(*out, "agent", "image_generation",
                                defaults.image_generation ? "true" : "false",
                                error);
  return rc == CAI_OK ? 1 : -1;
}
static void optional_string(const cai_config *cfg, const char *section,
                            const char *key, const char **out) {
  const char *value = cai_config_get(cfg, section, key);
  if (value != NULL)
    *out = value[0] != '\0' ? value : NULL;
}
int cai_cli_configure(int argc, char *const *argv, cai_cli_options *options,
                      cai_config **configuration, cai_error *error) {
  cai_cli_options early, defaults;
  cai_config *cfg = NULL;
  const char *base, *value, *resolved;
  char selected[PATH_MAX];
  char config_root[PATH_MAX];
  size_t i;
  int rc, n;
  *configuration = NULL;
  rc = cai_cli_read_options(argc, argv, NULL, &early);
  if (rc <= 0) {
    *options = early;
    return rc;
  }
  if (early.directory != NULL && chdir(early.directory) != 0)
    return fail(error, early.directory, "cannot change workspace");
  config_root[0] = '\0';
  if (early.config_dir != NULL &&
      absolute_path(config_root, sizeof(config_root), early.config_dir, error) <
          0)
    return -1;
  rc = open_schema(&cfg, error);
  if (rc < 0) {
    cai_config_close(cfg);
    return -1;
  }
  *configuration = cfg;
  if (!early.no_config) {
    if (early.config_file != NULL)
      rc = absolute_path(selected, sizeof(selected), early.config_file, error);
    else {
      if (early.config_dir != NULL) {
        n = snprintf(selected, sizeof(selected), "%s/config.ini", config_root);
      } else {
        base = getenv("XDG_CONFIG_HOME");
        if (base != NULL && base[0] == '/')
          n = snprintf(selected, sizeof(selected), "%s/cai/config.ini", base);
        else {
          base = getenv("HOME");
          if (base == NULL || base[0] != '/')
            return fail(error, NULL, "set HOME or absolute XDG_CONFIG_HOME");
          n = snprintf(selected, sizeof(selected), "%s/.config/cai/config.ini",
                       base);
        }
      }
      rc = n < 0 || (size_t)n >= sizeof(selected)
               ? fail(error, NULL, "configuration path too long")
               : 1;
    }
    if (rc < 0)
      return -1;
    if (cfg->create_template_file(cfg, selected, error) != CAI_OK ||
        cai_config_load_file(cfg, selected, error) != CAI_OK)
      return -1;
    if (cfg->apply_environment(cfg, error) != CAI_OK)
      return -1;
  }
  cai_cli_options_init(&defaults);
  optional_string(cfg, "client", "provider", &defaults.provider);
  for (i = 0U; i < sizeof(agent_keys) / sizeof(agent_keys[0]); i++)
    if (i >= 3U ||
        ((value = cai_config_get(cfg, "agent", agent_keys[i])) != NULL &&
         value[0] != '\0'))
      optional_string(
          cfg, "agent", agent_keys[i],
          (const char **)((unsigned char *)&defaults + agent_offsets[i]));
  for (i = 0U; i < sizeof(review_keys) / sizeof(review_keys[0]); i++)
    optional_string(
        cfg, "review", review_keys[i],
        (const char **)((unsigned char *)&defaults + review_offsets[i]));
  value = cai_config_get(cfg, "agent", "model");
  defaults.model_configured = value != NULL && value[0] != '\0';
  defaults.image_generation = cai_config_get_bool(
      cfg, "agent", "image_generation", defaults.image_generation);
  defaults.terminal =
      cai_config_get_bool(cfg, "agent", "terminal", defaults.terminal);
  defaults.review_subagent = cai_config_get_bool(
      cfg, "agent", "review_subagent", defaults.review_subagent);
  defaults.codex_agents_md = cai_config_get_bool(
      cfg, "agent", "codex_agents_md", defaults.codex_agents_md);
  for (i = 0U; i < sizeof(fields) / sizeof(fields[0]); i++) {
    const cli_field *f = &fields[i];
    unsigned char *p = (unsigned char *)&defaults + f->offset;
    if (f->type == CAI_CONFIG_STRING) {
      if ((f->flags & CAI_CONFIG_PATH) != 0U) {
        if (cai_config_resolve_path(cfg, f->section, f->key, &resolved,
                                    error) != CAI_OK)
          return -1;
        if (cai_config_get(cfg, f->section, f->key) != NULL)
          *(const char **)p = resolved;
      } else
        optional_string(cfg, f->section, f->key, (const char **)p);
    } else if (f->type == CAI_CONFIG_BOOL)
      *(int *)p = cai_config_get_bool(cfg, f->section, f->key, *(int *)p);
  }
  if (strcmp(early.provider_explicit ? early.provider : defaults.provider,
             "custom") == 0) {
    optional_string(cfg, "client", "base_url", &defaults.endpoint);
    optional_string(cfg, "client", "api_key_env", &defaults.api_key_env);
  }
  {
    static const char *const sections[] = {"client", "client", "terminal",
                                           "terminal", "terminal"};
    static const char *const keys[] = {"ca_bundle_path", "ca_path", "root_path",
                                       "default_workdir", "shell_path"};
    for (i = 0U; i < sizeof(keys) / sizeof(keys[0]); i++)
      if (cai_config_resolve_path(cfg, sections[i], keys[i], &resolved,
                                  error) != CAI_OK)
        return -1;
  }
  rc = cai_cli_read_options(argc, argv, &defaults, options);
  if (rc <= 0)
    return rc;
  if (config_root[0] != '\0') {
    if (cfg->set(cfg, "paths", "config_dir", config_root, error) != CAI_OK)
      return -1;
    options->config_dir = cfg->get(cfg, "paths", "config_dir");
  }
  if (early.directory == NULL && options->directory != NULL &&
      chdir(options->directory) != 0)
    return fail(error, options->directory,
                "cannot change configured workspace");
  if (cai_cli_validate_options(options) < 0)
    return -1;
  return 1;
}
