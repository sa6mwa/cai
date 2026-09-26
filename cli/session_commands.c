#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "session_commands.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>

/* Sanitize and clip a UTF-8 field by displayed cells, preserving codepoints. */
static void row_field(char *out, size_t capacity, const char *text, int width) {
  mbstate_t state;
  wchar_t code;
  const char *cursor;
  size_t n;
  size_t used;
  int cells;
  int visible;
  int clipped;
  int pass;
  clipped = 0;
  used = 0U;
  for (pass = 0; pass < 2; pass++) {
    memset(&state, 0, sizeof(state));
    cursor = text;
    used = 0U;
    cells = 0;
    while (*cursor != '\0' && width > 0) {
      n = mbrtowc(&code, cursor, MB_CUR_MAX, &state);
      if (n == (size_t)-1 || n == (size_t)-2) {
        n = 1U;
        code = L'?';
        memset(&state, 0, sizeof(state));
      }
      visible = wcwidth(code);
      if (visible < 0)
        visible = 1;
      if (cells + visible > width || used + n + 1U > capacity)
        break;
      if (pass != 0) {
        if (code == L'?' || wcwidth(code) < 0)
          out[used] = code == L'?' ? '?' : ' ';
        else
          memcpy(out + used, cursor, n);
      }
      used += code == L'?' || wcwidth(code) < 0 ? 1U : n;
      cells += visible;
      cursor += n;
    }
    if (pass == 0 && *cursor != '\0' && width >= 3) {
      clipped = 1;
      width -= 3;
    }
  }
  out[used] = '\0';
  if (clipped && used + 4U <= capacity)
    memcpy(out + used, "...", 4U);
}

static int row_cells(const char *text) {
  mbstate_t state;
  wchar_t code;
  size_t n;
  int cells;
  int width;
  memset(&state, 0, sizeof(state));
  cells = 0;
  while (*text != '\0') {
    n = mbrtowc(&code, text, MB_CUR_MAX, &state);
    if (n == (size_t)-1 || n == (size_t)-2) {
      n = 1U;
      width = 1;
      memset(&state, 0, sizeof(state));
    } else {
      width = wcwidth(code);
    }
    cells += width > 0 ? width : 0;
    text += n;
  }
  return cells;
}

int cai_cli_sessions_print(cai_sink *destination,
                           const cai_cli_session *sessions, size_t count,
                           int numbered, int width, cai_error *error) {
  size_t i;
  char directory[PATH_MAX];
  char prompt[2048];
  char line[PATH_MAX + 2304];
  char clipped[PATH_MAX + 2304];
  char number[32];
  int dir_width;
  int prompt_width;
  int prefix_width;
  if (width <= 0)
    width = 80;
  if (count == 0U &&
      cai_sink_write(destination, "No conversations.\n", 18U, error) != CAI_OK)
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "write session list");
  for (i = 0U; i < count; i++) {
    number[0] = '\0';
    if (numbered)
      snprintf(number, sizeof(number), "%lu  ", (unsigned long)(i + 1U));
    prefix_width = (int)(strlen(number) + strlen(sessions[i].id) + 4U);
    dir_width = width > prefix_width ? (width - prefix_width) / 2 : 0;
    if (dir_width > 32)
      dir_width = 32;
    row_field(directory, sizeof(directory), sessions[i].workspace, dir_width);
    /* A conservative directory allocation keeps a prompt column on every row.
     */
    prompt_width = width - prefix_width - row_cells(directory);
    row_field(prompt, sizeof(prompt), sessions[i].first_prompt, prompt_width);
    snprintf(line, sizeof(line), "%s%s  %s  %s", number, sessions[i].id,
             directory, prompt);
    row_field(clipped, sizeof(clipped), line, width);
    if (cai_sink_write(destination, clipped, strlen(clipped), error) !=
            CAI_OK ||
        cai_sink_write(destination, "\n", 1U, error) != CAI_OK)
      return cai_cli_error(error, CAI_ERR_TRANSPORT, "write session list");
  }
  return CAI_OK;
}

typedef struct export_store {
  cai_cli_pouch *pouch;
  const char *scope;
} export_store;

static int export_load(void *context, const char *scope, const char *id,
                       cai_source **out, unsigned long long *sequence,
                       cai_error *error) {
  export_store *store;
  (void)scope;
  store = (export_store *)context;
  return store->pouch->store.load_id(store->pouch, store->scope, id, out,
                                     sequence, error);
}

static int export_latest(void *context, const char *scope, char *id,
                         size_t capacity, cai_source **out,
                         unsigned long long *sequence, cai_error *error) {
  (void)context;
  (void)scope;
  (void)id;
  (void)capacity;
  (void)sequence;
  (void)error;
  *out = NULL;
  return CAI_OK;
}

static int export_events(void *context, const char *scope, const char *id,
                         unsigned long long after,
                         cai_agent_session_event_fn callback, void *visitor,
                         cai_error *error) {
  (void)context;
  (void)scope;
  (void)id;
  (void)after;
  (void)callback;
  (void)visitor;
  (void)error;
  /* The Markdown exporter uses durable conversation history from the
   * checkpoint. Recovering pending journal inputs would start work and is
   * forbidden here. */
  return CAI_OK;
}

static int export_checkpoint(void *context, const char *scope, const char *id,
                             cai_source *source, unsigned long long sequence,
                             cai_error *error) {
  (void)context;
  (void)scope;
  (void)id;
  (void)source;
  (void)sequence;
  (void)error;
  return CAI_OK;
}

static int export_append(void *context, const char *scope, const char *id,
                         const cai_agent_session_event *event,
                         cai_error *error) {
  (void)context;
  (void)scope;
  (void)id;
  (void)event;
  (void)error;
  return CAI_OK;
}

static int offline_runtime(cai_cli_pouch *pouch, const char *id,
                           const cai_cli_session *metadata,
                           export_store *storage,
                           cai_agent_session_store *callbacks,
                           cai_client **client, cai_agent_runtime **runtime,
                           cai_error *error) {
  cai_client_config client_config;
  cai_agent_runtime_config config;
  struct stat status;
  int rc;
  cai_client_config_init(&client_config);
  client_config.logger = pouch->logger;
  client_config.api_key = "offline-export";
  client_config.base_url = "http://127.0.0.1:1/v1";
  rc = cai_client_open(&client_config, client, error);
  if (rc != CAI_OK)
    return rc;
  memset(callbacks, 0, sizeof(*callbacks));
  storage->pouch = pouch;
  storage->scope = metadata->workspace;
  callbacks->context = storage;
  callbacks->load_id = export_load;
  callbacks->load_latest = export_latest;
  callbacks->load_events_after = export_events;
  callbacks->checkpoint = export_checkpoint;
  callbacks->append_event = export_append;
  cai_agent_runtime_config_init(&config);
  config.session_store = callbacks;
  config.session_scope = "cai-export";
  config.workspace_directory =
      stat(metadata->workspace, &status) == 0 && S_ISDIR(status.st_mode)
          ? metadata->workspace
          : ".";
  config.resume_session_id = id;
  config.disable_terminal = 1;
  config.disable_review_subagent = 1;
  return cai_agent_runtime_open(*client, &config, runtime, error);
}

static int export_path(char *path, size_t capacity, const char *root,
                       const char *id, cai_error *error) {
  const char *base;
  int n;
  if (root != NULL)
    n = snprintf(path, capacity, "%s/%s", root, id);
  else {
    base = getenv("XDG_DATA_HOME");
    if (base != NULL && base[0] == '/')
      n = snprintf(path, capacity, "%s/cai/exports/%s", base, id);
    else {
      base = getenv("HOME");
      if (base == NULL || base[0] != '/')
        return cai_cli_error(
            error, CAI_ERR_INVALID,
            "set HOME or XDG_DATA_HOME for conversation exports");
      n = snprintf(path, capacity, "%s/.local/share/cai/exports/%s", base, id);
    }
  }
  if (n < 0 || (size_t)n >= capacity)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "export directory path too long");
  return CAI_OK;
}

static int export_directory(char *path, cai_error *error) {
  char *cursor;
  struct stat status;
  for (cursor = path + 1;; cursor++) {
    if (*cursor != '/' && *cursor != '\0')
      continue;
    if (*cursor == '/') {
      *cursor = '\0';
      if (mkdir(path, 0700) != 0 && errno != EEXIST) {
        *cursor = '/';
        return cai_cli_error(error, CAI_ERR_TRANSPORT,
                             "create export directory");
      }
      *cursor = '/';
    } else {
      if (mkdir(path, 0700) != 0 && errno != EEXIST)
        return cai_cli_error(error, CAI_ERR_TRANSPORT,
                             "create session export directory");
      break;
    }
  }
  if (lstat(path, &status) != 0 || !S_ISDIR(status.st_mode) ||
      status.st_uid != geteuid() || (status.st_mode & 077U) != 0U)
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "session export directory must be private (mode 0700)");
  return CAI_OK;
}

static int export_file(const char *directory, const char *id,
                       const char *suffix, char *temporary, size_t capacity,
                       FILE **out, cai_error *error) {
  int fd;
  if (snprintf(temporary, capacity, "%s/%s.%s.tmp.XXXXXX", directory, id,
               suffix) >= (int)capacity)
    return cai_cli_error(error, CAI_ERR_INVALID, "export path too long");
  fd = mkstemp(temporary);
  if (fd < 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "create private export file");
  *out = fdopen(fd, "wb");
  if (*out == NULL) {
    close(fd);
    unlink(temporary);
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "open export output stream");
  }
  return CAI_OK;
}

int cai_cli_session_export(cai_cli_pouch *pouch, const char *id,
                           const char *export_root,
                           cai_agent_runtime *active_runtime,
                           cai_sink *destination, cai_error *error) {
  cai_cli_session metadata;
  export_store storage;
  cai_agent_session_store callbacks;
  cai_client *client;
  cai_agent_runtime *runtime;
  cai_sink *sink;
  char directory[PATH_MAX];
  char json_path[PATH_MAX];
  char md_path[PATH_MAX];
  char json_tmp[PATH_MAX];
  char md_tmp[PATH_MAX];
  const char *cursor;
  FILE *file;
  lc_lease *lease;
  int rc;
  for (cursor = id; *cursor != '\0'; cursor++) {
    if (!((*cursor >= 'a' && *cursor <= 'z') ||
          (*cursor >= 'A' && *cursor <= 'Z') ||
          (*cursor >= '0' && *cursor <= '9') || *cursor == '-' ||
          *cursor == '_'))
      return cai_cli_error(
          error, CAI_ERR_INVALID,
          "export ID must contain only letters, digits, '-' or '_'");
  }
  if (id[0] == '\0')
    return cai_cli_error(error, CAI_ERR_INVALID, "export session ID is empty");
  client = NULL;
  runtime = active_runtime;
  sink = NULL;
  file = NULL;
  json_tmp[0] = '\0';
  md_tmp[0] = '\0';
  lease = NULL;
  /* Hold one document lease across both exports so their checkpoint and
   * journal cannot change between the Markdown and JSONL reads. */
  rc = cai_cli_pouch_lock(pouch, id, &lease, error);
  if (rc == CAI_OK)
    rc = cai_cli_pouch_session(pouch, id, &metadata, error);
  if (rc == CAI_OK && active_runtime == NULL)
    rc = offline_runtime(pouch, id, &metadata, &storage, &callbacks, &client,
                         &runtime, error);
  if (rc == CAI_OK) {
    rc = export_path(directory, sizeof(directory), export_root, id, error);
    if (rc == CAI_OK)
      rc = export_directory(directory, error);
  }
  if (rc == CAI_OK && (snprintf(json_path, sizeof(json_path), "%s/%s.jsonl",
                                directory, id) >= (int)sizeof(json_path) ||
                       snprintf(md_path, sizeof(md_path), "%s/%s.md", directory,
                                id) >= (int)sizeof(md_path)))
    rc = cai_cli_error(error, CAI_ERR_INVALID, "export file path too long");
  /* Complete both temporary streams before replacing either published export.
   */
  if (rc == CAI_OK)
    rc = export_file(directory, id, "md", md_tmp, sizeof(md_tmp), &file, error);
  if (rc == CAI_OK)
    rc = cai_sink_file(file, 0, &sink, error);
  if (rc == CAI_OK)
    rc = cai_agent_runtime_export_markdown(runtime, sink, error);
  cai_sink_close(sink);
  sink = NULL;
  if (file != NULL) {
    if (fflush(file) != 0 || fsync(fileno(file)) != 0)
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "sync Markdown export");
    if (fclose(file) != 0 && rc == CAI_OK)
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "close Markdown export");
    file = NULL;
  }
  if (rc == CAI_OK)
    rc = export_file(directory, id, "jsonl", json_tmp, sizeof(json_tmp), &file,
                     error);
  if (rc == CAI_OK)
    rc = cai_sink_file(file, 0, &sink, error);
  if (rc == CAI_OK)
    rc = cai_cli_pouch_export(pouch, id, sink, error);
  cai_sink_close(sink);
  if (file != NULL) {
    if (fflush(file) != 0 || fsync(fileno(file)) != 0)
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "sync JSONL export");
    if (fclose(file) != 0 && rc == CAI_OK)
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "close JSONL export");
  }
  if (rc == CAI_OK &&
      (rename(json_tmp, json_path) != 0 || rename(md_tmp, md_path) != 0))
    rc =
        cai_cli_error(error, CAI_ERR_TRANSPORT, "publish conversation exports");
  if (json_tmp[0] != '\0')
    unlink(json_tmp);
  if (md_tmp[0] != '\0')
    unlink(md_tmp);
  if (active_runtime == NULL && runtime != NULL)
    cai_agent_runtime_close(runtime);
  if (client != NULL)
    client->close(client);
  rc = cai_cli_pouch_unlock(lease, rc, error);
  if (rc == CAI_OK)
    rc = cai_sink_write(destination, json_path, strlen(json_path), error);
  if (rc == CAI_OK)
    rc = cai_sink_write(destination, "\n", 1U, error);
  if (rc == CAI_OK)
    rc = cai_sink_write(destination, md_path, strlen(md_path), error);
  if (rc == CAI_OK)
    rc = cai_sink_write(destination, "\n", 1U, error);
  return rc;
}
