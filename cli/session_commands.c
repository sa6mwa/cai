#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "session_commands.h"
#include "identity.h"
#include "messages.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>

static int session_select(cai_cli_pouch *pouch, const char *workspace,
                          const char *choice,
                          char out[CAI_AGENT_SESSION_ID_MAX], int deleting,
                          cai_error *error) {
  cai_cli_session *sessions = NULL;
  unsigned long long number;
  size_t count = 0U, i;
  char *end;
  int rc;
  if (choice == NULL || choice[0] == '\0')
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "select a conversation ID or list number");
  if (strspn(choice, "0123456789") != strlen(choice)) {
    if (strlen(choice) >= CAI_AGENT_SESSION_ID_MAX)
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "conversation ID is too long");
    strcpy(out, choice);
    return CAI_OK;
  }
  rc = cai_cli_pouch_list(pouch, workspace, &sessions, &count, error);
  if (rc != CAI_OK)
    goto done;
  for (i = 0U; i < count; i++) {
    if (strcmp(choice, sessions[i].id) == 0) {
      strcpy(out, sessions[i].id);
      free(sessions);
      return CAI_OK;
    }
  }
  if (deleting) {
    rc = cai_cli_sessions_deletable(pouch, sessions, &count, error);
    if (rc != CAI_OK)
      goto done;
  }
  errno = 0;
  number = strtoull(choice, &end, 10);
  if (errno != 0 || *end != '\0' || number == 0U || number > count)
    rc = cai_cli_error(error, CAI_ERR_INVALID,
                       "No conversation with that number in this directory. "
                       "List conversations with --resume or --delete.");
  else
    strcpy(out, sessions[(size_t)number - 1U].id);
done:
  free(sessions);
  return rc;
}

int cai_cli_session_select(cai_cli_pouch *pouch, const char *workspace,
                           const char *choice,
                           char out[CAI_AGENT_SESSION_ID_MAX],
                           cai_error *error) {
  return session_select(pouch, workspace, choice, out, 0, error);
}
int cai_cli_session_select_delete(cai_cli_pouch *pouch, const char *workspace,
                                  const char *choice,
                                  char out[CAI_AGENT_SESSION_ID_MAX],
                                  cai_error *error) {
  return session_select(pouch, workspace, choice, out, 1, error);
}
int cai_cli_sessions_deletable(cai_cli_pouch *pouch, cai_cli_session *sessions,
                               size_t *count, cai_error *error) {
  size_t i, kept = 0U;
  int eligible, rc;
  for (i = 0U; i < *count; i++) {
    rc = cai_cli_pouch_deletable(pouch, sessions[i].id, &eligible, error);
    if (rc != CAI_OK)
      return rc;
    if (eligible)
      sessions[kept++] = sessions[i];
  }
  *count = kept;
  return CAI_OK;
}
static int session_file_id(const char *id) {
  return id != NULL && id[0] != '\0' && strlen(id) < CAI_AGENT_SESSION_ID_MAX &&
         strspn(id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                    "0123456789-_") == strlen(id);
}
int cai_cli_session_history_path(const cai_cli_options *options, const char *id,
                                 char out[PATH_MAX], cai_error *error) {
  if (!session_file_id(id) || options->history_dir == NULL ||
      snprintf(out, PATH_MAX, "%s/%s", options->history_dir, id) >= PATH_MAX)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "invalid conversation prompt-history path");
  return CAI_OK;
}

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

void cai_cli_history_row(char *out, size_t capacity,
                         const cai_agent_history_entry *entry, int width) {
  char line[1200];
  size_t length;
  snprintf(line, sizeof(line), "%lu. %s%s", (unsigned long)entry->remove_count,
           entry->kind == CAI_AGENT_HISTORY_SHELL ? "Shell: " : "",
           entry->summary);
  if (capacity < 2U)
    return;
  row_field(out, capacity - 1U, line, width > 0 ? width : 1);
  length = strlen(out);
  out[length++] = '\n';
  out[length] = '\0';
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

typedef struct export_execution_visit {
  cai_agent_session_event_fn callback;
  void *context;
} export_execution_visit;
static int export_execution_event(void *context,
                                  const cai_agent_session_event *event,
                                  cai_error *error) {
  export_execution_visit *visit = context;
  return strcmp(event->type, "execution") == 0
             ? visit->callback(visit->context, event, error)
             : CAI_OK;
}
static int export_events(void *context, const char *scope, const char *id,
                         unsigned long long after,
                         cai_agent_session_event_fn callback, void *visitor,
                         cai_error *error) {
  export_store *store = context;
  export_execution_visit visit;
  (void)scope;
  /* The Markdown exporter uses durable conversation history from the
   * commit. Recovering pending journal inputs would start work and is
   * forbidden here. */
  visit.callback = callback;
  visit.context = visitor;
  return cai_cli_pouch_replay(store->pouch, store->scope, id, after, 0U, NULL,
                              export_execution_event, &visit, error);
}

static int export_commit(void *context, const char *scope, const char *id,
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
  callbacks->commit = export_commit;
  callbacks->append_event = export_append;
  cai_agent_runtime_config_init(&config);
  config.session_store = callbacks;
  config.session_scope = pouch->program_name;
  if (pouch->config_directory[0] == '\0' &&
      cai_cli_xdg_path(pouch->config_directory, sizeof(pouch->config_directory),
                       "XDG_CONFIG_HOME", ".config", pouch->program_name,
                       error) != CAI_OK)
    return error != NULL ? error->code : CAI_ERR_INVALID;
  config.agent_config_directory = pouch->config_directory;
  config.agent_identity = pouch->program_name;
  config.workspace_directory =
      stat(metadata->workspace, &status) == 0 && S_ISDIR(status.st_mode)
          ? metadata->workspace
          : ".";
  config.resume_session_id = id;
  config.disable_terminal = 1;
  config.disable_review_subagent = 1;
  return cai_agent_runtime_open(*client, &config, runtime, error);
}

static int export_path(const cai_cli_pouch *pouch, char *path, size_t capacity,
                       const char *root, const char *id, cai_error *error) {
  const char *base;
  int n;
  if (root != NULL)
    n = snprintf(path, capacity, "%s/%s", root, id);
  else {
    base = getenv("XDG_DATA_HOME");
    if (base != NULL && base[0] == '/')
      n = snprintf(path, capacity, "%s/%s/exports/%s", base,
                   pouch->program_name, id);
    else {
      base = getenv("HOME");
      if (base == NULL || base[0] != '/')
        return cai_cli_error(
            error, CAI_ERR_INVALID,
            "set HOME or XDG_DATA_HOME for conversation exports");
      n = snprintf(path, capacity, "%s/.local/share/%s/exports/%s", base,
                   pouch->program_name, id);
    }
  }
  if (n < 0 || (size_t)n >= capacity)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "export directory path too long");
  return CAI_OK;
}

static int export_directory(char *path, int require_private, cai_error *error) {
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
  if ((require_private ? lstat(path, &status) : stat(path, &status)) != 0 ||
      !S_ISDIR(status.st_mode))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "export parent is not a directory");
  if (require_private &&
      (status.st_uid != geteuid() || (status.st_mode & 077U) != 0U))
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "session export directory must be private (mode 0700)");
  return CAI_OK;
}

static int export_file(const char *path, char *temporary, size_t capacity,
                       FILE **out, cai_error *error) {
  const char *slash = strrchr(path, '/');
  int fd, n;
  /* A fixed sibling name supports long destination basenames without
   * exceeding the filesystem's per-component limit just for staging. */
  n = slash == NULL ? snprintf(temporary, capacity, ".export-XXXXXX")
                    : snprintf(temporary, capacity, "%.*s/.export-XXXXXX",
                               (int)(slash - path), path);
  if (n < 0 || (size_t)n >= capacity) {
    temporary[0] = '\0';
    return cai_cli_error(error, CAI_ERR_INVALID, "export path too long");
  }
  fd = mkstemp(temporary);
  if (fd < 0) {
    temporary[0] = '\0';
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "create private export file");
  }
  *out = fdopen(fd, "wb");
  if (*out == NULL) {
    close(fd);
    unlink(temporary);
    temporary[0] = '\0';
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "open export output stream");
  }
  return CAI_OK;
}

/* Named destinations are workspace-relative, independent of the managed
 * export root. Only an exact .md/.jsonl suffix selects one projection. */
static int export_targets(const cai_cli_pouch *pouch, const char *root,
                          const char *id, const char *basename,
                          char paths[2][PATH_MAX], cai_error *error) {
  static const char *const suffixes[] = {".md", ".jsonl"};
  char directory[PATH_MAX];
  char *slash;
  size_t length, i;
  int selected = -1, n, rc;
  struct stat status;
  memset(paths, 0, 2U * PATH_MAX);
  if (basename == NULL) {
    rc = export_path(pouch, directory, sizeof(directory), root, id, error);
    if (rc == CAI_OK)
      rc = export_directory(directory, 1, error);
    if (rc != CAI_OK)
      return rc;
    for (i = 0U; i < 2U; i++) {
      n = snprintf(paths[i], PATH_MAX, "%s/%s%s", directory, id, suffixes[i]);
      if (n < 0 || n >= PATH_MAX)
        return cai_cli_error(error, CAI_ERR_INVALID,
                             "export file path too long");
    }
  } else {
    length = strlen(basename);
    slash = strrchr(basename, '/');
    if (length == 0U || length >= PATH_MAX || basename[length - 1U] == '/' ||
        strcmp(slash != NULL ? slash + 1U : basename, ".") == 0 ||
        strcmp(slash != NULL ? slash + 1U : basename, "..") == 0)
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "export basename must name a file");
    for (i = 0U; i < 2U; i++) {
      size_t suffix_length = strlen(suffixes[i]);
      if (length >= suffix_length &&
          strcmp(basename + length - suffix_length, suffixes[i]) == 0)
        selected = (int)i;
    }
    for (i = 0U; i < 2U; i++) {
      if (selected != -1 && selected != (int)i)
        continue;
      n = snprintf(paths[i], PATH_MAX, "%s%s", basename,
                   selected == -1 ? suffixes[i] : "");
      if (n < 0 || n >= PATH_MAX)
        return cai_cli_error(error, CAI_ERR_INVALID,
                             "export file path too long");
    }
    strcpy(directory, basename);
    slash = strrchr(directory, '/');
    if (slash == directory)
      slash[1] = '\0';
    else if (slash != NULL)
      *slash = '\0';
    else
      strcpy(directory, ".");
    rc = export_directory(directory, 0, error);
    if (rc != CAI_OK)
      return rc;
  }
  /* Catch invalid destinations before publishing either projection. Existing
   * regular files are replaced via rename; their contents are never truncated
   * while an export is still being generated. */
  for (i = 0U; i < 2U; i++) {
    if (paths[i][0] == '\0')
      continue;
    if (lstat(paths[i], &status) == 0) {
      if (!S_ISREG(status.st_mode) && !S_ISLNK(status.st_mode))
        return cai_cli_error(error, CAI_ERR_INVALID,
                             "export destination must be a file");
    } else if (errno != ENOENT)
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "cannot inspect export destination");
  }
  return CAI_OK;
}

int cai_cli_session_export(cai_cli_pouch *pouch, const char *id,
                           const char *export_root, const char *basename,
                           cai_agent_runtime *active_runtime,
                           cai_sink *destination, cai_error *error) {
  cai_cli_session metadata;
  export_store storage;
  cai_agent_session_store callbacks;
  cai_client *client;
  cai_agent_runtime *runtime;
  cai_sink *sink;
  char paths[2][PATH_MAX];
  char temporary[2][PATH_MAX];
  struct cai_cli_session_guard *use = NULL;
  const char *cursor;
  FILE *file;
  lc_lease *lease;
  int rc;
  size_t i;
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
  memset(temporary, 0, sizeof(temporary));
  lease = NULL;
  /* Hold one document lease across both exports so their commit and
   * journal cannot change between the Markdown and JSONL reads. */
  rc = cai_cli_pouch_begin_read(pouch, id, &use, error);
  if (rc == CAI_OK)
    rc = cai_cli_pouch_lock(pouch, id, &lease, error);
  if (rc == CAI_OK)
    rc = cai_cli_pouch_session(pouch, id, &metadata, error);
  if (rc == CAI_OK && active_runtime == NULL)
    rc = offline_runtime(pouch, id, &metadata, &storage, &callbacks, &client,
                         &runtime, error);
  if (rc == CAI_OK)
    rc = export_targets(pouch, export_root, id, basename, paths, error);
  /* Finish every selected stream before publishing any output. */
  for (i = 0U; rc == CAI_OK && i < 2U; i++) {
    if (paths[i][0] == '\0')
      continue;
    rc =
        export_file(paths[i], temporary[i], sizeof(temporary[i]), &file, error);
    if (rc == CAI_OK)
      rc = cai_sink_file(file, 0, &sink, error);
    if (rc == CAI_OK && i == 0U)
      rc = cai_agent_runtime_export_markdown(runtime, sink, error);
    if (rc == CAI_OK && i == 0U)
      rc = cai_agent_runtime_export_execution_markdown(runtime, sink, error);
    if (rc == CAI_OK && i == 1U)
      rc = cai_cli_pouch_export(pouch, id, runtime, sink, error);
    cai_sink_close(sink);
    sink = NULL;
    if (file != NULL) {
      if (rc == CAI_OK && (fflush(file) != 0 || fsync(fileno(file)) != 0))
        rc =
            cai_cli_error(error, CAI_ERR_TRANSPORT, "sync conversation export");
      if (fclose(file) != 0 && rc == CAI_OK)
        rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "close conversation export");
      file = NULL;
    }
  }
  for (i = 0U; rc == CAI_OK && i < 2U; i++)
    if (paths[i][0] != '\0' && rename(temporary[i], paths[i]) != 0)
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "publish conversation export");
  for (i = 0U; i < 2U; i++)
    if (temporary[i][0] != '\0')
      unlink(temporary[i]);
  if (active_runtime == NULL && runtime != NULL)
    cai_agent_runtime_close(runtime);
  if (client != NULL)
    client->close(client);
  rc = cai_cli_pouch_unlock(lease, rc, error);
  /* Preserve the CLI utility's JSONL-then-Markdown listing order. */
  for (i = 2U; rc == CAI_OK && i > 0U; i--) {
    const char *path = paths[i - 1U];
    if (path[0] != '\0') {
      rc = cai_sink_write(destination, path, strlen(path), error);
      if (rc == CAI_OK)
        rc = cai_sink_write(destination, "\n", 1U, error);
    }
  }
  cai_cli_pouch_end_read(use);
  return rc;
}

typedef struct session_cleanup {
  cai_cli_pouch *pouch;
  cai_cli_pouch *messaging;
  const cai_cli_options *options;
} session_cleanup;

static int remove_session_child(const char *directory, const char *name,
                                int tree, cai_error *error) {
  int fd, rc, saved;
  char message[PATH_MAX + 256];
  if (directory == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "conversation cleanup directory is not configured");
  fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0 && errno == ENOENT)
    return CAI_OK;
  if (fd < 0)
    rc = -1;
  else {
    rc = tree ? cai_cli_remove_at(fd, name) : unlinkat(fd, name, 0);
    if (rc != 0 && errno == ENOENT)
      rc = 0;
    saved = errno;
    close(fd);
    errno = saved;
  }
  if (rc == 0)
    return CAI_OK;
  snprintf(message, sizeof(message), "remove conversation data %s/%s: %s",
           directory, name, strerror(errno));
  return cai_cli_error(error, CAI_ERR_TRANSPORT, message);
}
static int cleanup_session_files(void *context, const char *id,
                                 cai_error *error) {
  session_cleanup *cleanup = context;
  char name[CAI_AGENT_SESSION_ID_MAX + 8U], exports[PATH_MAX], *slash;
  int rc;
  if (!session_file_id(id))
    return cai_cli_error(error, CAI_ERR_INVALID, "unsafe conversation file ID");
  rc = cai_cli_messages_delete_session(cleanup->messaging, id, error);
  if (rc == CAI_OK)
    rc = remove_session_child(cleanup->options->history_dir, id, 1, error);
  if (rc == CAI_OK) {
    snprintf(name, sizeof(name), "%s.log", id);
    rc = remove_session_child(cleanup->options->log_dir, name, 0, error);
  }
  if (rc == CAI_OK)
    rc = export_path(cleanup->pouch, exports, sizeof(exports),
                     cleanup->options->export_dir, id, error);
  if (rc == CAI_OK) {
    slash = strrchr(exports, '/');
    *slash = '\0';
    rc = remove_session_child(exports, id, 1, error);
  }
  return rc;
}
int cai_cli_session_delete(cai_cli_pouch *pouch, cai_cli_pouch *messaging,
                           const cai_cli_options *options,
                           const char *workspace, const char *choice,
                           char id[CAI_AGENT_SESSION_ID_MAX],
                           cai_error *error) {
  session_cleanup cleanup;
  int rc = cai_cli_session_select_delete(pouch, workspace, choice, id, error);
  cleanup.pouch = pouch;
  cleanup.messaging = messaging;
  cleanup.options = options;
  return rc == CAI_OK
             ? cai_cli_pouch_delete(pouch, workspace, id, cleanup_session_files,
                                    &cleanup, error)
             : rc;
}
