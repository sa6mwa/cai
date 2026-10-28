#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "app_internal.h"
#include "identity.h"
#include "status.h"
#include "theme.h"
#include <cai/models.h>
#include <cai/version.h>
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
    S("paths", "home_dir", home_dir, CAI_CONFIG_PATH,
      "Unified application home; unset roots use its config, state, cache, "
      "data and run subdirectories. Individual paths take precedence. "
      "Use --home or the application-prefixed HOME variable to select it "
      "before config discovery; an INI value never reloads configuration."),
    S("paths", "state_dir", state_dir, CAI_CONFIG_PATH,
      "Private state root; empty uses home_dir/state, otherwise the "
      "application "
      "name under XDG_STATE_HOME."),
    S("paths", "cache_dir", cache_dir, CAI_CONFIG_PATH,
      "Private cache root; empty uses home_dir/cache, otherwise the "
      "application "
      "name under XDG_CACHE_HOME."),
    S("paths", "data_dir", data_dir, CAI_CONFIG_PATH,
      "Data root; empty uses home_dir/data, otherwise the application name "
      "under XDG_DATA_HOME."),
    S("paths", "runtime_dir", runtime_dir, CAI_CONFIG_PATH,
      "Runtime files; empty uses home_dir/run, then "
      "XDG_RUNTIME_DIR/program_name, "
      "otherwise cache_dir. Chrome sockets default to runtime_dir/chrome."),
    S("paths", "clipboard_dir", clipboard_dir, CAI_CONFIG_PATH,
      "Pasted images; empty uses cache_dir/clipboard."),
    S("paths", "log_dir", log_dir, CAI_CONFIG_PATH,
      "Interactive logs; empty uses cache_dir."),
    S("store.default", "key_file", pouch_key_file, CAI_CONFIG_PATH,
      "Encryption key; empty uses state_dir/pouch.key."),
    S("storage", "namespace_prefix", namespace_prefix, 0U,
      "Namespace prefix; empty uses program_name."),
    S("storage", "sessions_namespace", sessions_namespace, 0U,
      "Empty uses namespace_prefix.sessions."),
    {"storage", "conversation_retention_days",
     offsetof(cai_cli_options, conversation_retention_days), CAI_CONFIG_INTEGER,
     0U, NULL,
     "Whole conversation collection retention since last append, default 30 "
     "days; 0 disables expiry. Liblockdc maintenance performs deletion, not a "
     "CAI timer. "
     "Individual journal entries never expire separately."},
    S("storage", "auth_namespace", auth_namespace, 0U,
      "Empty uses namespace_prefix.auth."),
    S("storage", "scopes_namespace", scopes_namespace, 0U,
      "Empty uses namespace_prefix.scopes."),
    S("storage", "todo_namespace", todo_namespace, 0U,
      "Empty uses namespace_prefix.todo."),
    S("storage", "messages_namespace", messages_namespace, 0U,
      "Empty uses namespace_prefix.messages."),
    S("messaging", "queue_prefix", queue_prefix, 0U,
      "Single-component prefix for automatic queues; empty uses program_name."),
    S("agent", "service_tier", service_tier, 0U,
      "Provider service tier; empty restores saved routing. Explicit default "
      "requests normal routing; priority requests Fast. Use /tier list."),
    B("agent", "mcp", mcp, "Connect named [mcp.NAME] HTTP servers."),
    {"terminal", "yield_time_ms", offsetof(cai_cli_options, yield_time_ms),
     CAI_CONFIG_INTEGER, 0U, NULL,
     "Command yield interval, 0..30000 ms. Default 10000. Zero selects "
     "foreground mode: wait for exit or cancellation. /background restores "
     "the libcai default when yielding is disabled."},
    B("messaging", "enabled", messaging,
      "Listen for and send conversation messages on the shared Pouch backend."),
    S("messaging", "queue_name", message_queue, 0U,
      "Empty allocates queue_prefix.<directory-basename>, adding the fewest "
      "real "
      "ancestor components needed to distinguish registered workspaces. "
      "The actual hostname is a leading component. Assigned names persist. "
      "An explicit name overrides it. Newest app startup takes ownership of "
      "new deliveries; "
      "already delivered messages stay in their original conversation."),
    B("messaging", "enable_auto_enqueue", enable_auto_enqueue,
      "Automatically approve incoming TUI next-turn prompts; /autoqueue "
      "toggles "
      "this for the current conversation. Default false."),
    {"messaging", "ack_timeout_ms",
     offsetof(cai_cli_options, message_ack_timeout_ms), CAI_CONFIG_INTEGER, 0U,
     NULL,
     "Delivery ACK deadline in milliseconds, independent of human approval. "
     "Default 300000 (five minutes); range 1..86400000."},
    S("paths", "directory", directory, CAI_CONFIG_PATH,
      "Empty uses the current working directory on each invocation; explicit "
      "-C wins."),
    S("paths", "auth_json", auth_json, CAI_CONFIG_PATH,
      "Empty checks ~/.codex/auth.json first, then encrypted application auth. "
      "An explicit file must exist and replaces that discovery."),
    S("paths", "agents_md", agents_md, CAI_CONFIG_PATH,
      "Global AGENTS.md; empty uses config_dir/AGENTS.md, loaded if it "
      "exists."),
    S("paths", "config_dir", config_dir, CAI_CONFIG_PATH,
      "Global instructions root; empty uses home_dir/config, otherwise "
      "${XDG_CONFIG_HOME:-$HOME/.config}/program_name. "
      "Path defaults shown here are resolved at generation; empty keeps "
      "automatic lookup."),
    S("paths", "skills_dir", skills_dir, CAI_CONFIG_PATH,
      "Global skills directory; empty uses config_dir/skills, loaded if it "
      "exists."),
    S("paths", "model_metadata", model_metadata, CAI_CONFIG_PATH,
      "Empty optionally loads config_dir/models.json; built-in/provider "
      "metadata "
      "still applies. An explicit file must exist."),
    S("paths", "export_dir", export_dir, CAI_CONFIG_PATH,
      "Export root; empty uses data_dir/exports."),
    S("store.default", "client_pem", lockd_client_pem, CAI_CONFIG_PATH,
      "Remote lockd mTLS client PEM; empty supplies no client certificate."),
    S("store.default", "endpoint", lockd, 0U,
      "Empty opens a pouch under "
      "state_dir/pouch; "
      "explicit values are lockd endpoint URLs."),
    S("storage", "sessions_store", sessions_store, 0U,
      "Named store for conversations. --temp uses its own private store."),
    B("storage", "disable_compression", disable_compression,
      "Disable streaming zlib compression for Pouch roots. Default false. "
      "Changing an existing root's mode converts it synchronously with "
      "exclusive writer ownership; contention warns and keeps the stored "
      "mode."),
    S("storage", "auth_store", auth_store, 0U,
      "Named authentication store; retained in --temp runs."),
    S("storage", "messaging_store", messaging_store, 0U,
      "Named messaging store; retained in --temp runs."),
    B("ui", "temporary", temporary,
      "Start in a unique cache workspace; histories are deleted on exit. "
      "Workspace deletion asks [y/N]. --yes/--force also deletes the "
      "workspace."),
    {"ui", "replay_turns", offsetof(cai_cli_options, replay_turns),
     CAI_CONFIG_INTEGER, CAI_CONFIG_SIGNED, NULL,
     "Console replay only: newest whole turns and standalone shell entries. "
     "Default 10; 0 hides replay; -1 shows all. Agent context is unchanged."},
    {"ui", "prompt_history_limit",
     offsetof(cai_cli_options, prompt_history_limit), CAI_CONFIG_INTEGER, 0U,
     NULL,
     "Newest prompt-history entries loaded for recall and retained by native "
     "count maintenance, default 200. Pruning is asynchronous; 0 disables "
     "recall and recording without deleting saved prompts."},
    B("ui", "shell_context", shell_context,
      "Share sanitized !COMMAND output; /shell and !sh always stay local."),
    S("ui", "theme", theme, 0U,
      "Softline theme alias or SOFTLINE:LIBMDF. Use --list-themes for names "
      "and default pairings. plain selects plain:ascii (non-colored "
      "Markdown)."),
    {"ui", "theme_offset", offsetof(cai_cli_options, theme_offset),
     CAI_CONFIG_INTEGER, CAI_CONFIG_SIGNED, NULL,
     "Parent libmdf theme rotation, default 0. Signed modulo usable themes; "
     "boring is excluded. Softline and ANSI policy are unchanged."},
    {"subagent", "theme_offset",
     offsetof(cai_cli_options, subagent_theme_offset), CAI_CONFIG_INTEGER,
     CAI_CONFIG_SIGNED, NULL,
     "Ordinary child libmdf rotation relative to the effective parent, default "
     "0."},
    {"review", "theme_offset", offsetof(cai_cli_options, review_theme_offset),
     CAI_CONFIG_INTEGER, CAI_CONFIG_SIGNED, NULL,
     "Reviewer libmdf rotation relative to the effective parent, default 1."},
    S("ui", "status_fields", status_fields, 0U,
      "Colon-separated status order: model, ctx, dir, branch, weekly, crd, "
      "yolo, goal, provider. default expands "
      "model:ctx:dir:branch:weekly:crd:yolo:goal. "
      "Aliases: context=ctx, directory/folder/workspace=dir, w=weekly, "
      "credits=crd. Duplicates keep their first position. Missing values and "
      "YOLO when disabled are hidden. branch is hidden without a Git branch. "
      "weekly includes short quota windows "
      "or the API cost estimate."),
    {"ui", "todo_layout", offsetof(cai_cli_options, todo_layout),
     CAI_CONFIG_STRING, 0U, "board|list",
     "Kanban presentation; board groups cards by column with titles capped at "
     "28 characters. List shows full titles. /todo layout changes this "
     "invocation only."},
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
  if (path[0] == '~' && path[1] == '/') {
    const char *home = getenv("HOME");
    if (home == NULL || home[0] != '/')
      return fail(error, path, "set an absolute HOME to expand this path");
    n = snprintf(out, size, "%s/%s", home, path + 2U);
  } else if (path[0] == '/')
    n = snprintf(out, size, "%s", path);
  else {
    if (getcwd(cwd, sizeof(cwd)) == NULL)
      return fail(error, path, "resolve current directory");
    n = snprintf(out, size, "%s/%s", cwd, path);
  }
  return n < 0 || (size_t)n >= size
             ? fail(error, path, "application path is too long")
             : 1;
}
/* Store every derived value in the owned config: consumers never borrow a
 * temporary path and repeated application runs have no global identity. */
static int default_string(cai_config *cfg, const char *section, const char *key,
                          const char *value, const char **target, int document,
                          int is_path, cai_error *error) {
  const char *resolved;
  char absolute[PATH_MAX];
  int rc;
  if (is_path) {
    if (value == NULL || value[0] == '\0')
      return fail(error, key, "path must not be empty");
    if (value[0] != '/') {
      if (absolute_path(absolute, sizeof(absolute), value, error) < 0)
        return -1;
      value = absolute;
    }
  }
  if (document)
    return cai_config_set_default(cfg, section, key, value, error);
  rc = cfg->set(cfg, section, key, value, error);
  if (rc == CAI_OK && is_path)
    rc = cfg->resolve_path(cfg, section, key, &resolved, error);
  else
    resolved = cfg->get(cfg, section, key);
  if (rc == CAI_OK)
    *target = resolved;
  return rc;
}
static int application_defaults(cai_config *cfg, cai_cli_options *o,
                                const cai_app_config *app, int document,
                                cai_error *error) {
  static const char *const root_keys[] = {"config_dir", "state_dir",
                                          "cache_dir", "data_dir"};
  static const char *const variables[] = {"XDG_CONFIG_HOME", "XDG_STATE_HOME",
                                          "XDG_CACHE_HOME", "XDG_DATA_HOME"};
  static const char *const fallbacks[] = {".config", ".local/state", ".cache",
                                          ".local/share"};
  static const char *const home_children[] = {"config", "state", "cache",
                                              "data"};
  const char **roots[4];
  const char *host_roots[4];
  const char **children[7];
  const char *host_children[7];
  const char *parents[7];
  const char **inputs[3];
  static const char *const input_sections[] = {"paths", "store.default",
                                               "chrome"};
  static const char *const input_keys[] = {"auth_json", "client_pem", "socket"};
  static const char *const keys[] = {
      "clipboard_dir", "log_dir",    "pouch_key_file", "export_dir",
      "agents_md",     "skills_dir", "model_metadata"};
  static const char *const suffixes[] = {
      "/clipboard", "",        "/pouch.key",  "/exports",
      "/AGENTS.md", "/skills", "/models.json"};
  const char **namespaces[5];
  static const char *const ns_keys[] = {"sessions_namespace", "auth_namespace",
                                        "scopes_namespace", "todo_namespace",
                                        "messages_namespace"};
  static const char *const ns_suffixes[] = {"sessions", "auth", "scopes",
                                            "todo", "messages"};
  char path[PATH_MAX];
  char home_path[PATH_MAX];
  size_t i;
  int rc, n;
  const char *home = o->home_dir != NULL ? o->home_dir
                     : app != NULL       ? app->home_directory
                                         : NULL;
  if (home != NULL) {
    if (home[0] == '\0')
      return fail(error, "home_dir",
                  "application home must be a nonempty path");
    if (absolute_path(home_path, sizeof(home_path), home, error) < 0)
      return -1;
    rc = default_string(cfg, "paths", "home_dir", home_path, &o->home_dir,
                        document, 1, error);
    if (rc != CAI_OK)
      return rc;
    home = home_path;
  }
  roots[0] = &o->config_dir;
  roots[1] = &o->state_dir;
  roots[2] = &o->cache_dir;
  roots[3] = &o->data_dir;
  host_roots[0] = app != NULL ? app->config_directory : NULL;
  host_roots[1] = app != NULL ? app->state_directory : NULL;
  host_roots[2] = app != NULL ? app->cache_directory : NULL;
  host_roots[3] = app != NULL ? app->data_directory : NULL;
  for (i = 0U; i < 4U; i++) {
    const char *v = *roots[i] != NULL ? *roots[i] : host_roots[i];
    if (v == NULL) {
      if (home != NULL) {
        n = snprintf(path, sizeof(path), "%s/%s", home, home_children[i]);
        rc = n < 0 || (size_t)n >= sizeof(path)
                 ? fail(error, NULL, "application home path too long")
                 : CAI_OK;
      } else
        rc = cai_cli_xdg_path(path, sizeof(path), variables[i], fallbacks[i],
                              o->program_name, error);
      if (rc != CAI_OK)
        return rc;
      v = path;
    }
    rc = default_string(cfg, "paths", root_keys[i], v, roots[i], document, 1,
                        error);
    if (rc != CAI_OK)
      return rc;
  }
  /* For template documentation use the documented root values, not explicit
   * assignments (set_default deliberately does not change runtime settings). */
  if (document) {
    for (i = 0U; i < cfg->field_count(cfg); i++) {
      const cai_config_field *f = cfg->field_at(cfg, i);
      size_t j;
      for (j = 0U; j < 4U; j++)
        if (strcmp(f->section, "paths") == 0 &&
            strcmp(f->key, root_keys[j]) == 0)
          *roots[j] = f->default_value;
    }
  }
  {
    const char *v = o->runtime_dir != NULL ? o->runtime_dir
                    : app != NULL          ? app->runtime_directory
                                           : NULL;
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *chrome = cfg->get(cfg, "chrome", "socket_directory");
    if (v == NULL) {
      if (home != NULL)
        n = snprintf(path, sizeof(path), "%s/run", home);
      else if (runtime != NULL && runtime[0] == '/')
        n = snprintf(path, sizeof(path), "%s/%s", runtime, o->program_name);
      else {
        if (!document && runtime != NULL && runtime[0] != '\0' &&
            (chrome == NULL || chrome[0] == '\0'))
          return fail(error, "XDG_RUNTIME_DIR",
                      "Chrome requires an absolute runtime directory");
        n = snprintf(path, sizeof(path), "%s", o->cache_dir);
      }
      if (n < 0 || (size_t)n >= sizeof(path))
        return fail(error, NULL, "application runtime path too long");
      v = path;
    }
    rc = default_string(cfg, "paths", "runtime_dir", v, &o->runtime_dir,
                        document, 1, error);
    if (rc != CAI_OK)
      return rc;
  }
  children[0] = &o->clipboard_dir;
  children[1] = &o->log_dir;
  children[2] = &o->pouch_key_file;
  children[3] = &o->export_dir;
  children[4] = &o->agents_md;
  children[5] = &o->skills_dir;
  children[6] = &o->model_metadata;
  host_children[0] = app != NULL ? app->clipboard_directory : NULL;
  host_children[1] = app != NULL ? app->log_directory : NULL;
  host_children[2] = app != NULL ? app->pouch_key_file : NULL;
  host_children[3] = host_children[4] = host_children[5] = host_children[6] =
      NULL;
  parents[2] = o->state_dir;
  parents[0] = parents[1] = o->cache_dir;
  parents[3] = o->data_dir;
  parents[4] = parents[5] = parents[6] = o->config_dir;
  for (i = 0U; i < 7U; i++) {
    const char *v = *children[i] != NULL ? *children[i] : host_children[i];
    /* Optional discovery retains its "missing is okay" semantics. */
    if (!document && i >= 4U && v == NULL)
      continue;
    if (v == NULL) {
      n = snprintf(path, sizeof(path), "%s%s", parents[i], suffixes[i]);
      if (n < 0 || (size_t)n >= sizeof(path))
        return fail(error, NULL, "application path too long");
      v = path;
    }
    rc = default_string(cfg, i == 2U ? "store.default" : "paths",
                        i == 2U ? "key_file" : keys[i], v, children[i],
                        document, 1, error);
    if (rc != CAI_OK)
      return rc;
  }
  /* Retained inputs must stay anchored here, before --temp changes cwd.
   * INI paths are already config-relative absolutes; CLI paths use this
   * selected workspace. Unset optional paths retain automatic discovery. */
  inputs[0] = &o->auth_json;
  inputs[1] = &o->lockd_client_pem;
  inputs[2] = &o->chrome_socket;
  for (i = 0U; !document && i < 3U; i++) {
    if (*inputs[i] == NULL)
      continue;
    rc = default_string(cfg, input_sections[i], input_keys[i], *inputs[i],
                        inputs[i], 0, 1, error);
    if (rc != CAI_OK)
      return rc;
  }
  if (o->namespace_prefix == NULL)
    o->namespace_prefix = app != NULL && app->namespace_prefix != NULL
                              ? app->namespace_prefix
                              : o->program_name;
  if (o->queue_prefix == NULL)
    o->queue_prefix = app != NULL && app->queue_prefix != NULL
                          ? app->queue_prefix
                          : o->program_name;
  if (!cai_cli_identity_valid(o->queue_prefix))
    return fail(
        error, NULL,
        "queue_prefix must be a single lowercase identifier (up to 32 bytes)");
  rc = default_string(cfg, "storage", "namespace_prefix", o->namespace_prefix,
                      &o->namespace_prefix, document, 0, error);
  if (rc == CAI_OK)
    rc = default_string(cfg, "messaging", "queue_prefix", o->queue_prefix,
                        &o->queue_prefix, document, 0, error);
  if (rc != CAI_OK)
    return rc;
  namespaces[0] = &o->sessions_namespace;
  namespaces[1] = &o->auth_namespace;
  namespaces[2] = &o->scopes_namespace;
  namespaces[3] = &o->todo_namespace;
  namespaces[4] = &o->messages_namespace;
  for (i = 0U; i < 5U; i++) {
    const char *v = *namespaces[i];
    if (v == NULL) {
      n = snprintf(path, sizeof(path), "%s.%s", o->namespace_prefix,
                   ns_suffixes[i]);
      if (n < 0 || (size_t)n >= sizeof(path))
        return fail(error, NULL, "namespace too long");
      v = path;
    }
    if (v[0] == '\0' || strlen(v) > 128U ||
        strspn(v, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ01234567"
                  "89._-") != strlen(v))
      return fail(error, NULL, "invalid storage namespace");
    rc = default_string(cfg, "storage", ns_keys[i], v, namespaces[i], document,
                        0, error);
    if (rc != CAI_OK)
      return rc;
  }
  if (document) {
    rc = cai_config_set_default(cfg, "agent", "identity", o->program_name,
                                error);
    if (rc == CAI_OK)
      rc = cai_config_set_default(cfg, "client", "api_key_env",
                                  o->default_api_key_env, error);
    if (rc != CAI_OK)
      return rc;
  }
  if (!document) {
    const char *v = cfg->get(cfg, "chrome", "socket_directory");
    if (v == NULL || v[0] == '\0') {
      n = snprintf(path, sizeof(path), "%s/chrome", o->runtime_dir);
      if (n < 0 || (size_t)n >= sizeof(path))
        return fail(error, NULL, "Chrome path too long");
      if (cfg->set(cfg, "chrome", "socket_directory", path, error) != CAI_OK)
        return -1;
    }
    if (o->identity == NULL)
      o->identity = o->program_name;
  }
  return CAI_OK;
}
int cai_cli_app_schema(cai_config **out, cai_error *error) {
  static const cai_config_field stores[] = {
      {"store.*", "endpoint", CAI_CONFIG_STRING, "",
       "pouch:// root or HTTP(S) lockd endpoint. Each selected store opens "
       "once per app context; equal pouch roots share one process client.",
       NULL, 0U},
      {"store.*", "key_file", CAI_CONFIG_STRING, "",
       "Encryption key for this pouch root. Required for named pouch stores; "
       "a new root generates the key, an existing root requires its key.",
       NULL, CAI_CONFIG_PATH},
      {"store.*", "client_pem", CAI_CONFIG_STRING, "",
       "Optional mTLS client PEM for this HTTP(S) endpoint.", NULL,
       CAI_CONFIG_PATH}};
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
  selected = calloc(sdk->field_count(sdk) + sizeof(stores) / sizeof(stores[0]),
                    sizeof(*selected));
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
        strcmp(f->key, "logger_disabled") != 0 &&
        strcmp(f->key, "shell_context_max_lines") != 0 &&
        strcmp(f->key, "shell_context_max_bytes") != 0)
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
  for (i = 0U; i < sizeof(stores) / sizeof(stores[0]); i++)
    selected[count++] = stores[i];
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
  if (rc == CAI_OK)
    rc = cai_config_set_default(*out, "ui", "tool_output", "compact", error);
  if (rc == CAI_OK)
    rc = cai_config_set_default(*out, "ui", "theme", defaults.theme, error);
  if (rc == CAI_OK)
    rc = cai_config_set_default(*out, "ui", "theme_offset", "0", error);
  if (rc == CAI_OK)
    rc = cai_config_set_default(*out, "subagent", "theme_offset", "0", error);
  if (rc == CAI_OK)
    rc = cai_config_set_default(*out, "review", "theme_offset", "1", error);
  if (rc == CAI_OK)
    rc = cai_config_set_default(*out, "ui", "replay_turns", "10", error);
  if (rc == CAI_OK)
    rc = cai_config_set_default(*out, "ui", "prompt_history_limit", "200",
                                error);
  if (rc == CAI_OK)
    rc = cai_config_set_default(*out, "messaging", "ack_timeout_ms", "300000",
                                error);
  if (rc == CAI_OK)
    rc = cai_config_set_default(*out, "storage", "conversation_retention_days",
                                "30", error);
  return rc == CAI_OK ? 1 : -1;
}
static void optional_string(const cai_config *cfg, const char *section,
                            const char *key, const char **out) {
  const char *value = cai_config_get(cfg, section, key);
  if (value != NULL)
    *out = value[0] != '\0' ? value : NULL;
}
/* Config [env] has already been applied. Keep the usual CLI > environment >
 * INI precedence while using the standard parser for validation and storage. */
static const char *app_environment(const char *prefix, const char *suffix) {
  char name[384];
  if (snprintf(name, sizeof(name), "%s%s", prefix, suffix) >= (int)sizeof(name))
    return NULL;
  return getenv(name);
}
static int environment_error(const char *prefix, const char *suffix,
                             cai_error *error) {
  char name[384];
  snprintf(name, sizeof(name), "%s%s", prefix, suffix);
  return fail(error, name,
              error != NULL && error->message != NULL
                  ? error->message
                  : "invalid environment setting");
}
static int tool_environment(cai_config *cfg, const char *prefix,
                            cai_error *error) {
  static const char *const env[] = {"WEB_SEARCH",
                                    "CHROME",
                                    "TODO",
                                    "SEARXNG",
                                    "REVGEO",
                                    "TERMINAL",
                                    "IMAGE_GENERATION",
                                    "REVIEW_SUBAGENT",
                                    "CODEX_AGENTS_MD",
                                    "SEARXNG_URL",
                                    "REVGEO_URL",
                                    "CHROME_SOCKET",
                                    "READ_FILE",
                                    "LIST_FILES",
                                    "APPLY_PATCH",
                                    "VIEW_IMAGE",
                                    "GOALS",
                                    "SKILLS",
                                    "SHELL_CONTEXT",
                                    "SHELL_CONTEXT_MAX_LINES",
                                    "SHELL_CONTEXT_MAX_BYTES",
                                    "REASONING_TOOL",
                                    "NORMAL_REASONING_EFFORT",
                                    "HIGH_REASONING_EFFORT",
                                    "SUBAGENT_STEERING",
                                    "YOLO",
                                    "CAPACITY_RETRY",
                                    "REVIEW_AND_FIX_TOOL"};
  static const char *const sections[] = {
      "agent", "chrome", "agent",   "agent",  "agent",  "agent",   "agent",
      "agent", "agent",  "searxng", "revgeo", "chrome", "agent",   "agent",
      "agent", "agent",  "agent",   "agent",  "ui",     "runtime", "runtime",
      "agent", "agent",  "agent",   "agent",  "agent",  "agent",   "agent"};
  static const char *const keys[] = {"web_search",
                                     "enabled",
                                     "todo",
                                     "searxng",
                                     "revgeo",
                                     "terminal",
                                     "image_generation",
                                     "review_subagent",
                                     "codex_agents_md",
                                     "base_url",
                                     "base_url",
                                     "socket",
                                     "read_file",
                                     "list_files",
                                     "apply_patch",
                                     "view_image",
                                     "goals",
                                     "skills",
                                     "shell_context",
                                     "shell_context_max_lines",
                                     "shell_context_max_bytes",
                                     "reasoning_tool",
                                     "normal_reasoning_effort",
                                     "high_reasoning_effort",
                                     "subagent_steering",
                                     "yolo",
                                     "capacity_retry",
                                     "review_and_fix_tool"};
  size_t i;
  {
    static const char *const aliases[][3] = {
        {"MODEL", "agent", "model"},
        {"THEME", "ui", "theme"},
        {"THEME_OFFSET", "ui", "theme_offset"},
        {"SUBAGENT_THEME_OFFSET", "subagent", "theme_offset"},
        {"REVIEW_THEME_OFFSET", "review", "theme_offset"},
        {"STATUS_FIELDS", "ui", "status_fields"},
        {"PROVIDER", "client", "provider"},
        {"REASONING_EFFORT", "agent", "reasoning_effort"},
        {"API_KEY_ENV", "client", "api_key_env"},
        {"ENDPOINT", "client", "base_url"},
        {"HOME", "paths", "home_dir"},
        {"CONFIG_DIR", "paths", "config_dir"},
        {"STATE_DIR", "paths", "state_dir"},
        {"CACHE_DIR", "paths", "cache_dir"},
        {"DATA_DIR", "paths", "data_dir"},
        {"RUNTIME_DIR", "paths", "runtime_dir"},
        {"CLIPBOARD_DIR", "paths", "clipboard_dir"},
        {"LOG_DIR", "paths", "log_dir"},
        {"POUCH_KEY_FILE", "store.default", "key_file"},
        {"DISABLE_COMPRESSION", "storage", "disable_compression"},
        {"EXPORT_DIR", "paths", "export_dir"},
        {"AGENTS_MD", "paths", "agents_md"},
        {"SKILLS_DIR", "paths", "skills_dir"},
        {"MODEL_METADATA", "paths", "model_metadata"},
        {"AUTH_JSON", "paths", "auth_json"},
        {"LOCKD", "store.default", "endpoint"},
        {"SESSIONS_STORE", "storage", "sessions_store"},
        {"AUTH_STORE", "storage", "auth_store"},
        {"MESSAGING_STORE", "storage", "messaging_store"},
        {"TEMP", "ui", "temporary"},
        {"REPLAY_TURNS", "ui", "replay_turns"},
        {"PROMPT_HISTORY_LIMIT", "ui", "prompt_history_limit"},
        {"CONVERSATION_RETENTION_DAYS", "storage",
         "conversation_retention_days"},
        {"NAMESPACE_PREFIX", "storage", "namespace_prefix"},
        {"QUEUE_PREFIX", "messaging", "queue_prefix"},
        {"TODO_LAYOUT", "ui", "todo_layout"},
        {"FOREGROUND", "terminal", "foreground"},
        {"YIELD_TIME_MS", "terminal", "yield_time_ms"}};
    for (i = 0U; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
      const char *v = app_environment(prefix, aliases[i][0]);
      char home[PATH_MAX];
      if (v != NULL && strcmp(aliases[i][0], "HOME") == 0) {
        if (absolute_path(home, sizeof(home), v, error) < 0)
          return environment_error(prefix, aliases[i][0], error);
        v = home;
      }
      if (v != NULL &&
          cfg->set(cfg, aliases[i][1], aliases[i][2], v, error) != CAI_OK)
        return environment_error(prefix, aliases[i][0], error);
    }
  }
  for (i = 0U; i < cfg->field_count(cfg); i++) {
    const cai_config_field *f = cfg->field_at(cfg, i);
    char key[256];
    char home[PATH_MAX];
    size_t j;
    const char *v;
    if (f->section[0] == '\0' || strcmp(f->section, "env") == 0 ||
        strchr(f->section, '*') != NULL || strchr(f->section, '.') != NULL)
      continue;
    if (snprintf(key, sizeof(key), "%s_%s", f->section, f->key) >=
        (int)sizeof(key))
      return fail(error, NULL, "environment setting name too long");
    for (j = 0U; key[j] != '\0'; j++)
      if (key[j] >= 'a' && key[j] <= 'z')
        key[j] = (char)(key[j] - 'a' + 'A');
    v = app_environment(prefix, key);
    if (v != NULL && strcmp(key, "PATHS_HOME_DIR") == 0) {
      if (absolute_path(home, sizeof(home), v, error) < 0)
        return environment_error(prefix, key, error);
      v = home;
    }
    if (v != NULL && cfg->set(cfg, f->section, f->key, v, error) != CAI_OK)
      return -1;
  }
  if (app_environment(prefix, "BOARD") != NULL &&
      cai_config_set(cfg, "todo", "default_board",
                     app_environment(prefix, "BOARD"), error) != CAI_OK)
    return environment_error(prefix, "BOARD", error);
  if (app_environment(prefix, "MESSAGING") != NULL &&
      cai_config_set(cfg, "messaging", "enabled",
                     app_environment(prefix, "MESSAGING"), error) != CAI_OK)
    return environment_error(prefix, "MESSAGING", error);
  if (app_environment(prefix, "MESSAGE_QUEUE") != NULL &&
      cai_config_set(cfg, "messaging", "queue_name",
                     app_environment(prefix, "MESSAGE_QUEUE"), error) != CAI_OK)
    return environment_error(prefix, "MESSAGE_QUEUE", error);
  if (app_environment(prefix, "ENABLE_AUTO_ENQUEUE") != NULL &&
      cai_config_set(cfg, "messaging", "enable_auto_enqueue",
                     app_environment(prefix, "ENABLE_AUTO_ENQUEUE"),
                     error) != CAI_OK)
    return environment_error(prefix, "ENABLE_AUTO_ENQUEUE", error);
  if (app_environment(prefix, "MESSAGE_ACK_TIMEOUT_MS") != NULL &&
      cai_config_set(cfg, "messaging", "ack_timeout_ms",
                     app_environment(prefix, "MESSAGE_ACK_TIMEOUT_MS"),
                     error) != CAI_OK)
    return environment_error(prefix, "MESSAGE_ACK_TIMEOUT_MS", error);
  if (app_environment(prefix, "MCP") != NULL &&
      cai_config_set(cfg, "agent", "mcp", app_environment(prefix, "MCP"),
                     error) != CAI_OK)
    return environment_error(prefix, "MCP", error);
  if (app_environment(prefix, "SERVICE_TIER") != NULL &&
      cai_config_set(cfg, "agent", "service_tier",
                     app_environment(prefix, "SERVICE_TIER"), error) != CAI_OK)
    return environment_error(prefix, "SERVICE_TIER", error);
  for (i = 0U; i < sizeof(env) / sizeof(env[0]); i++) {
    const char *value = app_environment(prefix, env[i]);
    if (value != NULL &&
        cai_config_set(cfg, sections[i], keys[i], value, error) != CAI_OK)
      return environment_error(prefix, env[i], error);
  }
  return 1;
}
int cai_cli_configure(int argc, char *const *argv, cai_cli_options *options,
                      cai_config **configuration,
                      const cai_app_config *application, cai_error *error) {
  cai_cli_options early, defaults, documented;
  cai_config *cfg = NULL;
  const char *value, *resolved;
  char selected[PATH_MAX];
  char config_root[PATH_MAX];
  char home_root[PATH_MAX];
  size_t i;
  int rc, n, config_dir_flag, home_flag;
  *configuration = NULL;
  cai_cli_options_init(&defaults);
  defaults.program_name =
      application != NULL && application->program_name != NULL
          ? application->program_name
          : CAI_APP_DEFAULT_PROGRAM_NAME;
  defaults.invocation_name =
      application != NULL && application->invocation_name != NULL
          ? application->invocation_name
          : defaults.program_name;
  if (cai_cli_environment_prefix(application, defaults.environment_prefix,
                                 error) != CAI_OK)
    return -1;
  snprintf(defaults.default_api_key_env, sizeof(defaults.default_api_key_env),
           "%sAPI_KEY", defaults.environment_prefix);
  defaults.config_file = app_environment(defaults.environment_prefix, "CONFIG");
  defaults.home_dir =
      app_environment(defaults.environment_prefix, "PATHS_HOME_DIR");
  if (defaults.home_dir == NULL)
    defaults.home_dir = app_environment(defaults.environment_prefix, "HOME");
  defaults.config_dir =
      app_environment(defaults.environment_prefix, "CONFIG_DIR");
  rc = cai_cli_read_options(argc, argv, &defaults, &early);
  config_dir_flag = early.config_dir != defaults.config_dir;
  home_flag = early.home_dir != defaults.home_dir;
  if (rc < 0 || (rc == 0 && (!early.dump_config || early.help ||
                             early.version || early.list_themes))) {
    *options = early;
    if (early.help)
      return cai_cli_help_print(defaults.program_name, defaults.invocation_name,
                                defaults.environment_prefix) == CAI_OK
                 ? 0
                 : -1;
    if (early.version)
      puts(CAI_VERSION_STRING);
    if (early.list_themes) {
      cai_sink *sink = NULL;
      int written = cai_sink_stdout(&sink, error);
      if (written == CAI_OK)
        written = cai_cli_theme_list(sink, error);
      cai_sink_close(sink);
      return written == CAI_OK ? 0 : -1;
    }
    return rc;
  }
  if (application != NULL) {
    if (early.config_file == NULL && !early.no_config) {
      early.config_file = application->config_file;
      early.no_config = application->no_config;
    }
    if (early.config_dir == NULL)
      early.config_dir = application->config_directory;
    if (early.home_dir == NULL)
      early.home_dir = application->home_directory;
  }
  if (early.directory != NULL && chdir(early.directory) != 0)
    return fail(error, early.directory, "cannot change workspace");
  config_root[0] = '\0';
  if (early.home_dir != NULL) {
    if (early.home_dir[0] == '\0')
      return fail(error, "home_dir",
                  "application home must be a nonempty path");
    if (absolute_path(home_root, sizeof(home_root), early.home_dir, error) < 0)
      return -1;
    early.home_dir = home_root;
  }
  if (early.config_dir != NULL &&
      absolute_path(config_root, sizeof(config_root), early.config_dir, error) <
          0)
    return -1;
  if (early.config_dir == NULL) {
    if (early.home_dir != NULL) {
      n = snprintf(config_root, sizeof(config_root), "%s/config",
                   early.home_dir);
      if (n < 0 || (size_t)n >= sizeof(config_root))
        return fail(error, NULL, "application home path too long");
    } else if (cai_cli_xdg_path(config_root, sizeof(config_root),
                                "XDG_CONFIG_HOME", ".config",
                                defaults.program_name, error) != CAI_OK)
      return -1;
  }
  rc = cai_cli_app_schema(&cfg, error);
  if (rc < 0) {
    cai_config_close(cfg);
    return -1;
  }
  *configuration = cfg;
  if (early.dump_config) {
    cai_sink *sink = NULL;
    FILE *file = stdout;
    documented = early;
    documented.config_dir = config_root;
    *options = early;
    if (application_defaults(cfg, &documented, application, 1, error) != CAI_OK)
      return -1;
    if (early.dump_config_file != NULL) {
      file = fopen(early.dump_config_file, "wb");
      if (file == NULL)
        return fail(error, early.dump_config_file,
                    "cannot open configuration template output");
    }
    rc = cai_sink_file(file, 0, &sink, error);
    if (rc == CAI_OK)
      rc = cfg->write_template(cfg, sink, error);
    cai_sink_close(sink);
    if (fflush(file) != 0 && rc == CAI_OK)
      rc = fail(error, early.dump_config_file,
                "cannot flush configuration template output");
    if (file != stdout && fclose(file) != 0 && rc == CAI_OK)
      rc = fail(error, early.dump_config_file,
                "cannot close configuration template output");
    return rc == CAI_OK ? 0 : -1;
  }
  if (!early.no_config) {
    if (early.config_file != NULL)
      rc = absolute_path(selected, sizeof(selected), early.config_file, error);
    else {
      n = snprintf(selected, sizeof(selected), "%s/config.ini", config_root);
      rc = n < 0 || (size_t)n >= sizeof(selected)
               ? fail(error, NULL, "configuration path too long")
               : 1;
    }
    if (rc < 0)
      return -1;
    documented = early;
    documented.config_dir = config_root;
    if (application_defaults(cfg, &documented, application, 1, error) != CAI_OK)
      return -1;
    if (cfg->create_template_file(cfg, selected, error) != CAI_OK ||
        cai_config_load_file(cfg, selected, error) != CAI_OK)
      return -1;
  }
  /* Host environment entries participate in the normal explicit env policy. */
  for (i = 0U; application != NULL && i < application->setting_count; i++) {
    const cai_app_setting *s = &application->settings[i];
    if ((strcmp(s->section, "env") == 0 || s->section[0] == '\0') &&
        cfg->set(cfg, s->section, s->key, s->value, error) != CAI_OK)
      return -1;
  }
  if (cfg->apply_environment(cfg, error) != CAI_OK)
    return -1;
  if (tool_environment(cfg, defaults.environment_prefix, error) < 0)
    return -1;
  if (app_environment(defaults.environment_prefix, "CONFIG_DIR") != NULL &&
      cfg->set(cfg, "paths", "config_dir", config_root, error) != CAI_OK)
    return -1;
  for (i = 0U; application != NULL && i < application->setting_count; i++) {
    const cai_app_setting *s = &application->settings[i];
    if (strcmp(s->section, "env") != 0 && s->section[0] != '\0' &&
        cfg->set(cfg, s->section, s->key, s->value, error) != CAI_OK)
      return -1;
  }
  defaults.config_file = early.config_file;
  {
    long long replay = cai_config_get_integer(cfg, "ui", "replay_turns", 10LL);
    if (replay < -1LL || replay > LONG_MAX)
      return fail(error, NULL,
                  "ui.replay_turns requires -1, 0 or a positive count");
    defaults.replay_turns = (long)replay;
  }
  {
    long long limit =
        cai_config_get_integer(cfg, "ui", "prompt_history_limit", 200LL);
    if (limit < 0LL || limit > INT_MAX)
      return fail(
          error, NULL,
          "ui.prompt_history_limit requires a nonnegative count up to INT_MAX");
    defaults.prompt_history_limit = (int)limit;
  }
  defaults.no_config = early.no_config;
  {
    long long days = cai_config_get_integer(
        cfg, "storage", "conversation_retention_days", 30LL);
    if (days < 0LL || days > LONG_MAX / 86400L)
      return fail(error, NULL,
                  "storage.conversation_retention_days exceeds the supported "
                  "day range");
    defaults.conversation_retention_days = (long)days;
  }
  defaults.shell_context_max_lines = (size_t)cai_config_get_integer(
      cfg, "runtime", "shell_context_max_lines", 5000LL);
  defaults.shell_context_max_bytes = (size_t)cai_config_get_integer(
      cfg, "runtime", "shell_context_max_bytes", 4194304LL);
  if (defaults.shell_context_max_lines == 0U)
    defaults.shell_context_max_lines = 5000U;
  if (defaults.shell_context_max_bytes == 0U)
    defaults.shell_context_max_bytes = 4194304U;
  if (defaults.shell_context_max_bytes < 4U)
    return fail(error, NULL, "shell_context_max_bytes must be at least 4");
  optional_string(cfg, "client", "provider", &defaults.provider);
  defaults.theme_offset =
      cai_config_get_integer(cfg, "ui", "theme_offset", 0LL);
  defaults.subagent_theme_offset =
      cai_config_get_integer(cfg, "subagent", "theme_offset", 0LL);
  defaults.review_theme_offset =
      cai_config_get_integer(cfg, "review", "theme_offset", 1LL);
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
  defaults.foreground = cai_config_get_bool(cfg, "terminal", "foreground", 0);
  {
    long long interval =
        cai_config_get_integer(cfg, "terminal", "default_yield_time_ms",
                               CAI_TERMINAL_DEFAULT_YIELD_TIME_MS);
    if (interval == 0LL)
      interval = CAI_TERMINAL_DEFAULT_YIELD_TIME_MS;
    interval =
        cai_config_get_integer(cfg, "terminal", "yield_time_ms", interval);
    if (interval < 0LL || interval > 30000LL)
      return fail(error, NULL, "yield_time_ms must be 0..30000");
    defaults.yield_time_ms = (long)interval;
  }
  if (defaults.yield_time_ms == 0L) {
    defaults.foreground = 1;
    defaults.yield_time_ms = CAI_TERMINAL_DEFAULT_YIELD_TIME_MS;
  }
  defaults.yolo = cai_config_get_bool(cfg, "agent", "yolo", defaults.yolo);
  defaults.review_subagent = cai_config_get_bool(
      cfg, "agent", "review_subagent", defaults.review_subagent);
  defaults.review_and_fix_tool = cai_config_get_bool(
      cfg, "agent", "review_and_fix_tool", defaults.review_and_fix_tool);
  defaults.subagent_steering = cai_config_get_bool(
      cfg, "agent", "subagent_steering", defaults.subagent_steering);
  defaults.capacity_retry = cai_config_get_bool(cfg, "agent", "capacity_retry",
                                                defaults.capacity_retry);
  defaults.codex_agents_md = cai_config_get_bool(
      cfg, "agent", "codex_agents_md", defaults.codex_agents_md);
  defaults.web_search =
      cai_config_get_bool(cfg, "agent", "web_search", defaults.web_search);
  defaults.chrome =
      cai_config_get_bool(cfg, "chrome", "enabled", defaults.chrome);
  defaults.todo = cai_config_get_bool(cfg, "agent", "todo", defaults.todo);
  defaults.disable_compression = cai_config_get_bool(
      cfg, "storage", "disable_compression", defaults.disable_compression);
  optional_string(cfg, "todo", "default_board", &defaults.board);
  defaults.messaging =
      cai_config_get_bool(cfg, "messaging", "enabled", defaults.messaging);
  defaults.enable_auto_enqueue =
      cai_config_get_bool(cfg, "messaging", "enable_auto_enqueue", 0);
  {
    long long timeout =
        cai_config_get_integer(cfg, "messaging", "ack_timeout_ms", 300000LL);
    if (timeout <= 0 || timeout > 86400000LL)
      return fail(error, NULL,
                  "messaging.ack_timeout_ms must be between 1 and 86400000");
    defaults.message_ack_timeout_ms = (long)timeout;
  }
  defaults.searxng =
      cai_config_get_bool(cfg, "agent", "searxng", defaults.searxng);
  defaults.revgeo =
      cai_config_get_bool(cfg, "agent", "revgeo", defaults.revgeo);
  defaults.read_file =
      cai_config_get_bool(cfg, "agent", "read_file", defaults.read_file);
  defaults.list_files =
      cai_config_get_bool(cfg, "agent", "list_files", defaults.list_files);
  defaults.apply_patch =
      cai_config_get_bool(cfg, "agent", "apply_patch", defaults.apply_patch);
  defaults.view_image =
      cai_config_get_bool(cfg, "agent", "view_image", defaults.view_image);
  defaults.goals = cai_config_get_bool(cfg, "agent", "goals", defaults.goals);
  defaults.reasoning_tool = cai_config_get_bool(cfg, "agent", "reasoning_tool",
                                                defaults.reasoning_tool);
  optional_string(cfg, "agent", "normal_reasoning_effort",
                  &defaults.normal_reasoning_effort);
  optional_string(cfg, "agent", "high_reasoning_effort",
                  &defaults.high_reasoning_effort);
  defaults.skills =
      cai_config_get_bool(cfg, "agent", "skills", defaults.skills);
  optional_string(cfg, "searxng", "base_url", &defaults.searxng_url);
  optional_string(cfg, "revgeo", "base_url", &defaults.revgeo_url);
  if (cai_config_resolve_path(cfg, "chrome", "socket", &resolved, error) !=
      CAI_OK)
    return -1;
  defaults.chrome_socket = resolved;
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
    static const char *const sections[] = {"client",   "client",   "terminal",
                                           "terminal", "terminal", "chrome",
                                           "searxng",  "revgeo"};
    static const char *const keys[] = {
        "ca_bundle_path",     "ca_path",           "root_path",
        "default_workdir",    "shell_path",        "socket_directory",
        "response_spool_dir", "response_spool_dir"};
    for (i = 0U; i < sizeof(keys) / sizeof(keys[0]); i++)
      if (cai_config_resolve_path(cfg, sections[i], keys[i], &resolved,
                                  error) != CAI_OK)
        return -1;
  }
  rc = cai_cli_read_options(argc, argv, &defaults, options);
  if (rc <= 0)
    return rc;
  if (home_flag || (options->home_dir == NULL && early.home_dir != NULL)) {
    if (default_string(cfg, "paths", "home_dir", early.home_dir,
                       &options->home_dir, 0, 1, error) != CAI_OK)
      return -1;
  }
  if (config_dir_flag ||
      (options->config_dir == NULL && early.config_dir != NULL)) {
    if (cfg->set(cfg, "paths", "config_dir", config_root, error) != CAI_OK)
      return -1;
    options->config_dir = cfg->get(cfg, "paths", "config_dir");
  }
  if (early.directory == NULL && options->directory != NULL &&
      chdir(options->directory) != 0)
    return fail(error, options->directory,
                "cannot change configured workspace");
  if (application_defaults(cfg, options, application, 0, error) != CAI_OK)
    return -1;
  if (cai_cli_validate_options(options) < 0)
    return -1;
  {
    cai_cli_theme theme;
    const char *reason = cai_cli_theme_parse(options->theme, &theme);
    if (reason != NULL)
      return fail(error, options->theme, reason);
  }
  {
    cai_cli_status_fields selection;
    const char *reason =
        cai_cli_status_fields_parse(options->status_fields, &selection);
    if (reason != NULL)
      return fail(error, "ui.status_fields", reason);
  }
  return 1;
}
