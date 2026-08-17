#include <cai/session_store.h>

#include "cai_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct cai_local_session_store {
  char *root_directory;
} cai_local_session_store;

typedef struct cai_local_checkpoint_source {
  FILE *fp;
  long start;
  long end;
} cai_local_checkpoint_source;

static int cai_store_make_directory(const char *path, cai_error *error) {
  char buffer[PATH_MAX];
  char *cursor;
  size_t length;

  if (path == NULL || path[0] != '/') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session store path must be absolute");
  }
  length = strlen(path);
  if (length >= sizeof(buffer)) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session store path is too long");
  }
  memcpy(buffer, path, length + 1U);
  for (cursor = buffer + 1; *cursor != '\0'; cursor++) {
    if (*cursor == '/') {
      *cursor = '\0';
      if (mkdir(buffer, 0700) != 0 && errno != EEXIST) {
        return cai_set_error(error, CAI_ERR_TRANSPORT,
                             "failed to create session store directory");
      }
      *cursor = '/';
    }
  }
  if (mkdir(buffer, 0700) != 0 && errno != EEXIST) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to create session store directory");
  }
  return CAI_OK;
}

static int cai_store_directory_fd(const char *path, cai_error *error) {
  struct stat st;
  int fd;

  fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    cai_set_error(error, CAI_ERR_TRANSPORT,
                  "failed to open session store directory");
    return -1;
  }
  if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
    close(fd);
    cai_set_error(error, CAI_ERR_TRANSPORT,
                  "session store root is not a directory");
    return -1;
  }
  return fd;
}

static int cai_store_scope_hash(const char *scope, char output[65],
                                cai_error *error) {
  static const char hex[] = "0123456789abcdef";
  unsigned char digest[SHA256_DIGEST_LENGTH];
  size_t i;

  if (scope == NULL || scope[0] != '/') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session scope must be a canonical absolute path");
  }
  if (SHA256((const unsigned char *)scope, strlen(scope), digest) == NULL) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to hash session scope");
  }
  for (i = 0U; i < sizeof(digest); i++) {
    output[i * 2U] = hex[digest[i] >> 4U];
    output[i * 2U + 1U] = hex[digest[i] & 0x0fU];
  }
  output[64] = '\0';
  return CAI_OK;
}

static int cai_store_session_id_valid(const char *session_id) {
  const unsigned char *cursor;
  size_t length;

  if (session_id == NULL || session_id[0] == '\0') {
    return 0;
  }
  length = strlen(session_id);
  if (length > 128U) {
    return 0;
  }
  for (cursor = (const unsigned char *)session_id; *cursor != '\0'; cursor++) {
    if (!((*cursor >= 'a' && *cursor <= 'z') ||
          (*cursor >= 'A' && *cursor <= 'Z') ||
          (*cursor >= '0' && *cursor <= '9') || *cursor == '-' ||
          *cursor == '_')) {
      return 0;
    }
  }
  return 1;
}

static int cai_store_open_scope(cai_local_session_store *store,
                                const char *scope, int *out, char hash[65],
                                cai_error *error) {
  int root_fd;
  int scope_fd;
  int rc;

  *out = -1;
  rc = cai_store_scope_hash(scope, hash, error);
  if (rc != CAI_OK) {
    return rc;
  }
  root_fd = cai_store_directory_fd(store->root_directory, error);
  if (root_fd < 0) {
    return error != NULL ? error->code : CAI_ERR_TRANSPORT;
  }
  if (mkdirat(root_fd, hash, 0700) != 0 && errno != EEXIST) {
    close(root_fd);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to create scoped session directory");
  }
  scope_fd =
      openat(root_fd, hash, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  close(root_fd);
  if (scope_fd < 0) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to open scoped session directory");
  }
  *out = scope_fd;
  return CAI_OK;
}

static int cai_store_write_all(int fd, const void *data, size_t length,
                               cai_error *error) {
  const unsigned char *cursor;

  cursor = (const unsigned char *)data;
  while (length > 0U) {
    ssize_t written;

    written = write(fd, cursor, length);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "failed to write session checkpoint");
    }
    cursor += (size_t)written;
    length -= (size_t)written;
  }
  return CAI_OK;
}

static int cai_store_repair_incomplete_tail(int fd, cai_error *error) {
  char buffer[4096];
  off_t end;
  off_t cursor;
  off_t complete_end;

  end = lseek(fd, 0, SEEK_END);
  if (end < 0) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to seek session checkpoint log");
  }
  if (end == 0) {
    return CAI_OK;
  }
  cursor = end;
  complete_end = -1;
  while (cursor > 0 && complete_end < 0) {
    ssize_t nread;
    size_t amount;
    size_t i;

    amount = cursor > (off_t)sizeof(buffer) ? sizeof(buffer) : (size_t)cursor;
    cursor -= (off_t)amount;
    if (lseek(fd, cursor, SEEK_SET) < 0) {
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "failed to seek session checkpoint log");
    }
    nread = read(fd, buffer, amount);
    if (nread != (ssize_t)amount) {
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "failed to read session checkpoint log");
    }
    for (i = amount; i > 0U; i--) {
      if (buffer[i - 1U] == '\n') {
        complete_end = cursor + (off_t)i;
        break;
      }
    }
  }
  if (complete_end != end &&
      ftruncate(fd, complete_end < 0 ? 0 : complete_end) != 0) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to remove incomplete session checkpoint");
  }
  if (lseek(fd, 0, SEEK_END) < 0) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to seek repaired session checkpoint log");
  }
  return CAI_OK;
}

static int cai_local_session_checkpoint(void *context, const char *scope,
                                        const char *session_id,
                                        cai_source *state, cai_error *error) {
  cai_local_session_store *store;
  char hash[65];
  char filename[160];
  char buffer[8192];
  int scope_fd;
  int fd;
  int rc;

  if (context == NULL || state == NULL ||
      !cai_store_session_id_valid(session_id)) {
    return cai_set_error(
        error, CAI_ERR_INVALID,
        "valid session store checkpoint arguments are required");
  }
  store = (cai_local_session_store *)context;
  scope_fd = -1;
  fd = -1;
  rc = cai_store_open_scope(store, scope, &scope_fd, hash, error);
  if (rc == CAI_OK && snprintf(filename, sizeof(filename), "%s.jsonl",
                               session_id) >= (int)sizeof(filename)) {
    rc =
        cai_set_error(error, CAI_ERR_INVALID, "session identifier is too long");
  }
  if (rc == CAI_OK) {
    fd = openat(scope_fd, filename,
                O_RDWR | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
      rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to open session checkpoint log");
    }
  }
  if (rc == CAI_OK) {
    struct stat st;

    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1) {
      rc =
          cai_set_error(error, CAI_ERR_TRANSPORT,
                        "session checkpoint log is not a private regular file");
    }
  }
  if (rc == CAI_OK && flock(fd, LOCK_EX) != 0) {
    rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                       "failed to lock session checkpoint log");
  }
  if (rc == CAI_OK) {
    rc = cai_store_repair_incomplete_tail(fd, error);
  }
  while (rc == CAI_OK) {
    size_t nread;

    nread = cai_source_read(state, buffer, sizeof(buffer), error);
    if (nread == 0U) {
      break;
    }
    rc = cai_store_write_all(fd, buffer, nread, error);
  }
  if (rc == CAI_OK && error != NULL && error->code != CAI_OK) {
    rc = error->code;
  }
  if (rc == CAI_OK) {
    rc = cai_store_write_all(fd, "\n", 1U, error);
  }
  if (rc == CAI_OK && fsync(fd) != 0) {
    rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                       "failed to sync session checkpoint log");
  }
  if (rc == CAI_OK && fsync(scope_fd) != 0) {
    rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                       "failed to sync session checkpoint directory");
  }
  if (fd >= 0) {
    (void)flock(fd, LOCK_UN);
    close(fd);
  }
  if (scope_fd >= 0) {
    close(scope_fd);
  }
  return rc;
}

static size_t cai_local_checkpoint_read(void *context, void *buffer,
                                        size_t count, cai_error *error) {
  cai_local_checkpoint_source *source;
  long remaining;
  size_t nread;

  source = (cai_local_checkpoint_source *)context;
  if (source == NULL || source->fp == NULL || buffer == NULL || count == 0U) {
    return 0U;
  }
  remaining = source->end - ftell(source->fp);
  if (remaining <= 0L) {
    return 0U;
  }
  if ((unsigned long)remaining < count) {
    count = (size_t)remaining;
  }
  nread = fread(buffer, 1U, count, source->fp);
  if (nread == 0U && ferror(source->fp)) {
    cai_set_error(error, CAI_ERR_TRANSPORT,
                  "failed to read session checkpoint log");
  }
  return nread;
}

static int cai_local_checkpoint_reset(void *context, cai_error *error) {
  cai_local_checkpoint_source *source;

  source = (cai_local_checkpoint_source *)context;
  if (source == NULL || source->fp == NULL ||
      fseek(source->fp, source->start, SEEK_SET) != 0) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to rewind session checkpoint");
  }
  clearerr(source->fp);
  return CAI_OK;
}

static void cai_local_checkpoint_close(void *context) {
  cai_local_checkpoint_source *source;

  source = (cai_local_checkpoint_source *)context;
  if (source == NULL) {
    return;
  }
  if (source->fp != NULL) {
    (void)flock(fileno(source->fp), LOCK_UN);
    fclose(source->fp);
  }
  cai_free_mem(NULL, source);
}

static int cai_local_checkpoint_source_open(int fd, long start, long end,
                                            cai_source **out,
                                            cai_error *error) {
  cai_local_checkpoint_source *context;
  cai_source_callbacks callbacks;

  context = (cai_local_checkpoint_source *)cai_alloc(NULL, sizeof(*context));
  if (context == NULL) {
    close(fd);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate session checkpoint source");
  }
  context->fp = fdopen(fd, "rb");
  context->start = start;
  context->end = end;
  if (context->fp == NULL) {
    close(fd);
    cai_free_mem(NULL, context);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to open session checkpoint source");
  }
  if (fseek(context->fp, start, SEEK_SET) != 0) {
    cai_local_checkpoint_close(context);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to seek session checkpoint source");
  }
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.read = cai_local_checkpoint_read;
  callbacks.reset = cai_local_checkpoint_reset;
  callbacks.close = cai_local_checkpoint_close;
  callbacks.context = context;
  if (cai_source_from_callbacks(&callbacks, out, error) != CAI_OK) {
    cai_local_checkpoint_close(context);
    return error != NULL ? error->code : CAI_ERR_NOMEM;
  }
  return CAI_OK;
}

static int cai_local_find_last_line(int fd, long *out_start, long *out_end,
                                    cai_error *error) {
  char buffer[4096];
  off_t end;
  off_t cursor;
  off_t record_end;

  end = lseek(fd, 0, SEEK_END);
  if (end <= 0) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session checkpoint log is empty");
  }
  cursor = end;
  record_end = -1;
  while (cursor > 0) {
    ssize_t nread;
    size_t i;
    size_t amount;

    amount = cursor > (off_t)sizeof(buffer) ? sizeof(buffer) : (size_t)cursor;
    cursor -= (off_t)amount;
    if (lseek(fd, cursor, SEEK_SET) < 0) {
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "failed to seek session checkpoint log");
    }
    nread = read(fd, buffer, amount);
    if (nread != (ssize_t)amount) {
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "failed to read session checkpoint log");
    }
    for (i = amount; i > 0U; i--) {
      if (buffer[i - 1U] == '\n' && record_end < 0) {
        record_end = cursor + (off_t)i;
      } else if (buffer[i - 1U] == '\n') {
        *out_start = (long)(cursor + (off_t)i);
        *out_end = (long)record_end;
        return CAI_OK;
      }
    }
  }
  if (record_end < 0) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session checkpoint log has no complete record");
  }
  *out_start = 0L;
  *out_end = (long)record_end;
  return CAI_OK;
}

static int cai_local_session_load_latest(void *context, const char *scope,
                                         char *session_id,
                                         size_t session_id_capacity,
                                         cai_source **out, cai_error *error) {
  cai_local_session_store *store;
  char hash[65];
  DIR *directory;
  struct dirent *entry;
  struct stat candidate_stat;
  char candidate[160];
  int scope_fd;
  int fd;
  int rc;

  if (out == NULL || session_id == NULL || session_id_capacity == 0U) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session checkpoint outputs are required");
  }
  *out = NULL;
  session_id[0] = '\0';
  if (context == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "session store is required");
  }
  store = (cai_local_session_store *)context;
  scope_fd = -1;
  candidate[0] = '\0';
  memset(&candidate_stat, 0, sizeof(candidate_stat));
  rc = cai_store_open_scope(store, scope, &scope_fd, hash, error);
  if (rc != CAI_OK) {
    return rc;
  }
  directory = fdopendir(dup(scope_fd));
  if (directory == NULL) {
    close(scope_fd);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to enumerate session checkpoint logs");
  }
  while ((entry = readdir(directory)) != NULL) {
    struct stat st;
    size_t length;

    length = strlen(entry->d_name);
    if (length <= 6U || strcmp(entry->d_name + length - 6U, ".jsonl") != 0 ||
        fstatat(scope_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(st.st_mode) || st.st_nlink != 1) {
      continue;
    }
    if (candidate[0] == '\0' ||
        st.st_mtim.tv_sec > candidate_stat.st_mtim.tv_sec ||
        (st.st_mtim.tv_sec == candidate_stat.st_mtim.tv_sec &&
         st.st_mtim.tv_nsec > candidate_stat.st_mtim.tv_nsec)) {
      if (length >= sizeof(candidate)) {
        continue;
      }
      memcpy(candidate, entry->d_name, length + 1U);
      candidate_stat = st;
    }
  }
  closedir(directory);
  if (candidate[0] == '\0') {
    close(scope_fd);
    return CAI_OK;
  }
  {
    size_t length;

    length = strlen(candidate) - 6U;
    if (length + 1U > session_id_capacity) {
      close(scope_fd);
      return cai_set_error(error, CAI_ERR_INVALID,
                           "latest session identifier is too long");
    }
    memcpy(session_id, candidate, length);
    session_id[length] = '\0';
  }
  fd = openat(scope_fd, candidate, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  close(scope_fd);
  if (fd < 0) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to open latest session checkpoint log");
  }
  if (flock(fd, LOCK_SH) != 0) {
    close(fd);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to lock latest session checkpoint log");
  }
  {
    long start;
    long end;

    rc = cai_local_find_last_line(fd, &start, &end, error);
    if (rc == CAI_OK) {
      rc = cai_local_checkpoint_source_open(fd, start, end, out, error);
      fd = -1;
    }
  }
  if (fd >= 0) {
    (void)flock(fd, LOCK_UN);
    close(fd);
  }
  if (rc != CAI_OK) {
    session_id[0] = '\0';
  }
  return rc;
}

static void cai_local_session_store_destroy(void *context) {
  cai_local_session_store *store;

  store = (cai_local_session_store *)context;
  if (store == NULL) {
    return;
  }
  cai_free_mem(NULL, store->root_directory);
  cai_free_mem(NULL, store);
}

void cai_agent_local_session_store_config_init(
    cai_agent_local_session_store_config *config) {
  if (config != NULL) {
    memset(config, 0, sizeof(*config));
  }
}

int cai_agent_local_session_store_open(
    const cai_agent_local_session_store_config *config,
    cai_agent_session_store *out, cai_error *error) {
  cai_local_session_store *store;
  const char *root;
  const char *xdg_state;
  const char *home;
  char default_path[PATH_MAX];
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session store output is required");
  }
  memset(out, 0, sizeof(*out));
  root = config != NULL ? config->root_directory : NULL;
  if (root == NULL || root[0] == '\0') {
    xdg_state = getenv("XDG_STATE_HOME");
    home = getenv("HOME");
    if (xdg_state != NULL && xdg_state[0] != '\0') {
      (void)snprintf(default_path, sizeof(default_path), "%s/cai/sessions",
                     xdg_state);
    } else if (home != NULL && home[0] != '\0') {
      (void)snprintf(default_path, sizeof(default_path),
                     "%s/.local/state/cai/sessions", home);
    } else {
      return cai_set_error(
          error, CAI_ERR_INVALID,
          "XDG_STATE_HOME or HOME is required for local sessions");
    }
    root = default_path;
  }
  rc = cai_store_make_directory(root, error);
  if (rc != CAI_OK) {
    return rc;
  }
  store = (cai_local_session_store *)cai_alloc(NULL, sizeof(*store));
  if (store == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate local session store");
  }
  memset(store, 0, sizeof(*store));
  store->root_directory = cai_strdup(NULL, root);
  if (store->root_directory == NULL) {
    cai_local_session_store_destroy(store);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to copy local session store path");
  }
  out->checkpoint = cai_local_session_checkpoint;
  out->load_latest = cai_local_session_load_latest;
  out->context = store;
  return CAI_OK;
}

void cai_agent_local_session_store_close(cai_agent_session_store *store) {
  if (store == NULL) {
    return;
  }
  if (store->checkpoint == cai_local_session_checkpoint &&
      store->load_latest == cai_local_session_load_latest) {
    cai_local_session_store_destroy(store->context);
  }
  memset(store, 0, sizeof(*store));
}
