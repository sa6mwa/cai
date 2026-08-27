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
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct cai_local_session_store {
  char *root_directory;
} cai_local_session_store;

#if defined(CAI_TESTING)
static int cai_session_store_test_fail_scope_parent_sync_enabled;

void cai_session_store_test_set_fail_scope_parent_sync(int enabled) {
  cai_session_store_test_fail_scope_parent_sync_enabled = enabled != 0 ? 1 : 0;
}

static int cai_session_store_test_fail_scope_parent_sync(void) {
  return cai_session_store_test_fail_scope_parent_sync_enabled;
}
#endif

typedef struct cai_local_checkpoint_source {
  FILE *fp;
  long start;
  long end;
} cai_local_checkpoint_source;

typedef struct cai_local_event_doc {
  char *record_type;
  unsigned long long sequence;
  char *type;
  char *data;
} cai_local_event_doc;

static const lonejson_field cai_local_event_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_local_event_doc, record_type,
                                    "record_type"),
    LONEJSON_FIELD_U64_REQ(cai_local_event_doc, sequence, "sequence"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_local_event_doc, type, "type"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_local_event_doc, data, "data")};
LONEJSON_MAP_DEFINE(cai_local_event_map, cai_local_event_doc,
                    cai_local_event_fields);

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
      struct stat st;

      *cursor = '\0';
      if (mkdir(buffer, 0700) != 0) {
        if (errno != EEXIST) {
          return cai_set_error(error, CAI_ERR_TRANSPORT,
                               "failed to create session store directory");
        }
        if (lstat(buffer, &st) != 0 || !S_ISDIR(st.st_mode)) {
          return cai_set_error(
              error, CAI_ERR_TRANSPORT,
              "session store path component is not a directory");
        }
      }
      *cursor = '/';
    }
  }
  if (mkdir(buffer, 0700) != 0) {
    struct stat st;

    if (errno != EEXIST) {
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "failed to create session store directory");
    }
    if (lstat(buffer, &st) != 0 || !S_ISDIR(st.st_mode)) {
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "session store root is not a directory");
    }
  }
  return CAI_OK;
}

/*
 * Persist checkpoint recency in the checkpoint itself.  A journal's mtime
 * cannot represent this: a later event append intentionally advances it
 * without creating resumable state.
 */
static int cai_store_checkpoint_created_at_ns(unsigned long long *out,
                                              cai_error *error) {
  struct timespec now;
  unsigned long long seconds;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "checkpoint timestamp output is required");
  }
  if (clock_gettime(CLOCK_REALTIME, &now) != 0 || now.tv_sec < 0 ||
      now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to obtain checkpoint timestamp");
  }
  seconds = (unsigned long long)now.tv_sec;
  if (seconds > ((unsigned long long)-1 - (unsigned long long)now.tv_nsec) /
                    1000000000U) {
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "checkpoint timestamp is out of range");
  }
  *out = seconds * 1000000000U + (unsigned long long)now.tv_nsec;
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

static int cai_store_validate_private_directory_fd(int fd, cai_error *error) {
  struct stat st;

  if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
      (st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "scoped session directory is not private");
  }
  return CAI_OK;
}

static int cai_store_validate_private_regular_fd(int fd, cai_error *error) {
  struct stat st;

  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
      st.st_uid != geteuid() || (st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "session log is not a private regular file");
  }
  return CAI_OK;
}

static int cai_store_scope_hash(const char *scope, char output[65],
                                cai_error *error) {
  static const char hex[] = "0123456789abcdef";
  unsigned char digest[SHA256_DIGEST_LENGTH];
  size_t i;

  if (scope == NULL || scope[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "session scope is required");
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

static int cai_store_session_filename_valid(const char *filename) {
  const unsigned char *cursor;
  size_t length;
  size_t session_id_length;

  if (filename == NULL) {
    return 0;
  }
  length = strlen(filename);
  if (length <= 6U || strcmp(filename + length - 6U, ".jsonl") != 0) {
    return 0;
  }
  session_id_length = length - 6U;
  if (session_id_length == 0U || session_id_length > 128U) {
    return 0;
  }
  cursor = (const unsigned char *)filename;
  while ((size_t)(cursor - (const unsigned char *)filename) <
         session_id_length) {
    if (!((*cursor >= 'a' && *cursor <= 'z') ||
          (*cursor >= 'A' && *cursor <= 'Z') ||
          (*cursor >= '0' && *cursor <= '9') || *cursor == '-' ||
          *cursor == '_')) {
      return 0;
    }
    cursor++;
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
  if (scope_fd < 0) {
    close(root_fd);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to open scoped session directory");
  }
  /* A scope directory is durable state: syncing only its journal and
   * directory does not persist its entry in the store root after a crash. Do
   * this for both a newly-created directory and one created concurrently. */
#if defined(CAI_TESTING)
  if (cai_session_store_test_fail_scope_parent_sync() != 0) {
    errno = EIO;
    rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                              "failed to sync session store root directory",
                              strerror(errno));
  } else {
    rc = CAI_OK;
  }
#else
  rc = CAI_OK;
#endif
  if (rc == CAI_OK && fsync(root_fd) != 0) {
    rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                              "failed to sync session store root directory",
                              strerror(errno));
  }
  close(root_fd);
  if (rc != CAI_OK) {
    close(scope_fd);
    return rc;
  }
  rc = cai_store_validate_private_directory_fd(scope_fd, error);
  if (rc != CAI_OK) {
    close(scope_fd);
    return rc;
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

/* Find the last complete top-level JSON object before before_end. Checkpoints
 * contain arbitrary JSON state and may therefore span several physical lines;
 * line-oriented recovery would mistake a state newline for a record boundary.
 */
static int cai_local_find_last_record_before(int fd, long before_end,
                                             long *out_start, long *out_end,
                                             long *out_complete_end,
                                             int *out_found, cai_error *error) {
  char buffer[4096];
  off_t end;
  off_t offset;
  long record_start;
  long candidate_end;
  int depth;
  int in_string;
  int escaped;
  int awaiting_newline;

  if (out_start == NULL || out_end == NULL || out_complete_end == NULL ||
      out_found == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session checkpoint record outputs are required");
  }
  *out_found = 0;
  *out_complete_end = 0L;
  end = before_end > 0L ? (off_t)before_end : lseek(fd, 0, SEEK_END);
  if (end < 0) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to seek session checkpoint log");
  }
  offset = 0;
  record_start = -1L;
  candidate_end = 0L;
  depth = 0;
  in_string = 0;
  escaped = 0;
  awaiting_newline = 0;
  while (offset < end) {
    ssize_t nread;
    size_t i;

    size_t amount = end - offset > (off_t)sizeof(buffer)
                        ? sizeof(buffer)
                        : (size_t)(end - offset);
    if (lseek(fd, offset, SEEK_SET) < 0) {
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "failed to seek session checkpoint log");
    }
    nread = read(fd, buffer, amount);
    if (nread != (ssize_t)amount) {
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "failed to read session checkpoint log");
    }
    for (i = 0U; i < amount; i++, offset++) {
      char ch = buffer[i];

      if (awaiting_newline) {
        if (ch == '\n') {
          *out_start = record_start;
          *out_end = candidate_end;
          *out_complete_end = (long)offset + 1L;
          *out_found = 1;
          awaiting_newline = 0;
          record_start = -1L;
          continue;
        }
        awaiting_newline = 0;
        record_start = -1L;
        depth = 0;
        in_string = 0;
        escaped = 0;
      }
      if (record_start < 0L) {
        if (ch == '{') {
          record_start = (long)offset;
          depth = 1;
        }
        continue;
      }
      if (in_string) {
        if (escaped) {
          escaped = 0;
        } else if (ch == '\\') {
          escaped = 1;
        } else if (ch == '"') {
          in_string = 0;
        }
      } else if (ch == '"') {
        in_string = 1;
      } else if (ch == '{') {
        depth++;
      } else if (ch == '}') {
        depth--;
        if (depth == 0) {
          candidate_end = (long)offset + 1L;
          awaiting_newline = 1;
        } else if (depth < 0) {
          record_start = -1L;
          depth = 0;
        }
      }
    }
  }
  return CAI_OK;
}

typedef int (*cai_local_record_fn)(void *context, int fd, long record_start,
                                   long record_end, cai_error *error);

/* Visit complete top-level JSON objects in journal order. Checkpoint state is
 * arbitrary JSON and can span physical lines, so only object framing—not a
 * line prefix—can distinguish one journal record from its embedded state. */
static int cai_local_visit_complete_records(int fd,
                                            cai_local_record_fn callback,
                                            void *context, cai_error *error) {
  char buffer[4096];
  off_t end;
  off_t offset;
  long record_start;
  long record_end;
  int depth;
  int in_string;
  int escaped;
  int awaiting_newline;

  if (fd < 0 || callback == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session record visitor arguments are required");
  }
  end = lseek(fd, 0, SEEK_END);
  if (end < 0) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to seek session checkpoint log");
  }
  offset = 0;
  record_start = -1L;
  record_end = 0L;
  depth = 0;
  in_string = 0;
  escaped = 0;
  awaiting_newline = 0;
  while (offset < end) {
    ssize_t nread;
    size_t amount;
    size_t index;

    amount = end - offset > (off_t)sizeof(buffer) ? sizeof(buffer)
                                                  : (size_t)(end - offset);
    if (lseek(fd, offset, SEEK_SET) < 0) {
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "failed to seek session checkpoint log");
    }
    nread = read(fd, buffer, amount);
    if (nread != (ssize_t)amount) {
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "failed to read session checkpoint log");
    }
    for (index = 0U; index < amount; index++, offset++) {
      char ch;

      ch = buffer[index];
      if (awaiting_newline) {
        if (ch == '\n') {
          int rc;

          rc = callback(context, fd, record_start, record_end, error);
          if (rc != CAI_OK) {
            return rc;
          }
          awaiting_newline = 0;
          record_start = -1L;
          continue;
        }
        awaiting_newline = 0;
        record_start = -1L;
        depth = 0;
        in_string = 0;
        escaped = 0;
      }
      if (record_start < 0L) {
        if (ch == '{') {
          record_start = (long)offset;
          depth = 1;
        }
        continue;
      }
      if (in_string) {
        if (escaped) {
          escaped = 0;
        } else if (ch == '\\') {
          escaped = 1;
        } else if (ch == '"') {
          in_string = 0;
        }
      } else if (ch == '"') {
        in_string = 1;
      } else if (ch == '{') {
        depth++;
      } else if (ch == '}') {
        depth--;
        if (depth == 0) {
          record_end = (long)offset + 1L;
          awaiting_newline = 1;
        } else if (depth < 0) {
          record_start = -1L;
          depth = 0;
        }
      }
    }
  }
  return CAI_OK;
}

static int cai_store_repair_incomplete_tail(int fd, cai_error *error) {
  off_t end;
  long start;
  long record_end;
  long complete_end;
  int found;
  int rc;

  end = lseek(fd, 0, SEEK_END);
  if (end < 0) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to seek session checkpoint log");
  }
  rc = cai_local_find_last_record_before(fd, 0L, &start, &record_end,
                                         &complete_end, &found, error);
  if (rc != CAI_OK) {
    return rc;
  }
  if ((off_t)complete_end != end &&
      ftruncate(fd, found ? (off_t)complete_end : 0) != 0) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to remove incomplete session checkpoint");
  }
  if (lseek(fd, 0, SEEK_END) < 0) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to seek repaired session checkpoint log");
  }
  return CAI_OK;
}

static int cai_local_session_checkpoint(
    void *context, const char *scope, const char *session_id, cai_source *state,
    unsigned long long applied_event_sequence, cai_error *error) {
  cai_local_session_store *store;
  char hash[65];
  char filename[160];
  char buffer[8192];
  char record_prefix[128];
  unsigned long long created_at_ns;
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
  created_at_ns = 0U;
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
  if (rc == CAI_OK)
    rc = cai_store_validate_private_regular_fd(fd, error);
  if (rc == CAI_OK && flock(fd, LOCK_EX) != 0) {
    rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                       "failed to lock session checkpoint log");
  }
  if (rc == CAI_OK) {
    rc = cai_store_repair_incomplete_tail(fd, error);
  }
  if (rc == CAI_OK) {
    rc = cai_store_checkpoint_created_at_ns(&created_at_ns, error);
  }
  if (rc == CAI_OK &&
      snprintf(
          record_prefix, sizeof(record_prefix),
          "{\"record_type\":\"checkpoint\",\"checkpoint_created_at_ns\":%llu,"
          "\"applied_event_sequence\":%llu,\"state\":",
          created_at_ns,
          applied_event_sequence) >= (int)sizeof(record_prefix)) {
    rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                       "failed to format session checkpoint record");
  }
  if (rc == CAI_OK) {
    rc = cai_store_write_all(fd, record_prefix, strlen(record_prefix), error);
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
    rc = cai_store_write_all(fd, "}\n", 2U, error);
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

static int cai_local_session_append_event(void *context, const char *scope,
                                          const char *session_id,
                                          const cai_agent_session_event *event,
                                          cai_error *error) {
  cai_local_session_store *store;
  cai_buffer_builder builder;
  char hash[65];
  char filename[160];
  int scope_fd;
  int fd;
  int rc;

  if (context == NULL || event == NULL || event->sequence == 0U ||
      event->type == NULL || event->type[0] == '\0' ||
      !cai_store_session_id_valid(session_id)) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "valid session event arguments are required");
  }
  store = (cai_local_session_store *)context;
  memset(&builder, 0, sizeof(builder));
  scope_fd = -1;
  fd = -1;
  rc = cai_buffer_append_cstr(&builder,
                              "{\"record_type\":\"event\","
                              "\"sequence\":",
                              error);
  if (rc == CAI_OK) {
    char number[32];

    (void)snprintf(number, sizeof(number), "%llu", event->sequence);
    rc = cai_buffer_append_cstr(&builder, number, error);
  }
  if (rc == CAI_OK) {
    rc = cai_buffer_append_cstr(&builder, ",\"type\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_buffer_append_json_string(&builder, event->type, error);
  }
  if (rc == CAI_OK) {
    rc = cai_buffer_append_cstr(&builder, ",\"data\":", error);
  }
  if (rc == CAI_OK && event->data != NULL) {
    rc = cai_buffer_append_json_string(&builder, event->data, error);
  }
  if (rc == CAI_OK && event->data == NULL) {
    rc = cai_buffer_append_cstr(&builder, "null", error);
  }
  if (rc == CAI_OK) {
    rc = cai_buffer_append_cstr(&builder, "}\n", error);
  }
  if (rc == CAI_OK) {
    rc = cai_store_open_scope(store, scope, &scope_fd, hash, error);
  }
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
                         "failed to open session event log");
    }
  }
  if (rc == CAI_OK)
    rc = cai_store_validate_private_regular_fd(fd, error);
  if (rc == CAI_OK && flock(fd, LOCK_EX) != 0) {
    rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                       "failed to lock session event log");
  }
  if (rc == CAI_OK) {
    rc = cai_store_repair_incomplete_tail(fd, error);
  }
  if (rc == CAI_OK) {
    rc = cai_store_write_all(fd, builder.data, builder.length, error);
  }
  if (rc == CAI_OK && fsync(fd) != 0) {
    rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                       "failed to sync session event log");
  }
  if (rc == CAI_OK && fsync(scope_fd) != 0) {
    rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                       "failed to sync session event directory");
  }
  if (fd >= 0) {
    (void)flock(fd, LOCK_UN);
    close(fd);
  }
  if (scope_fd >= 0) {
    close(scope_fd);
  }
  cai_free_mem(NULL, builder.data);
  return rc;
}

typedef struct cai_local_event_replay_context {
  unsigned long long after_sequence;
  unsigned long long previous_sequence;
  cai_agent_session_event_fn callback;
  void *callback_context;
} cai_local_event_replay_context;

static int cai_local_replay_complete_event(void *context, int fd,
                                           long record_start, long record_end,
                                           cai_error *error) {
  static const char event_prefix[] = "{\"record_type\":\"event\",";
  cai_local_event_replay_context *replay;
  cai_local_event_doc doc;
  cai_agent_session_event event;
  lonejson_error json_error;
  lonejson_status status;
  char prefix[sizeof(event_prefix) - 1U];
  char *record;
  size_t length;
  size_t offset;
  ssize_t nread;
  int rc;

  if (context == NULL || record_start < 0L || record_end <= record_start) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "invalid session event record bounds");
  }
  replay = (cai_local_event_replay_context *)context;
  length = (size_t)(record_end - record_start);
  if ((long)length != record_end - record_start) {
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "session event record is too large");
  }
  if (length < sizeof(prefix)) {
    return CAI_OK;
  }
  if (lseek(fd, (off_t)record_start, SEEK_SET) < 0) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to seek session event record");
  }
  offset = 0U;
  while (offset < sizeof(prefix)) {
    nread = read(fd, prefix + offset, sizeof(prefix) - offset);
    if (nread <= 0) {
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "failed to read session event record");
    }
    offset += (size_t)nread;
  }
  if (memcmp(prefix, event_prefix, sizeof(prefix)) != 0) {
    return CAI_OK;
  }
  if (length == (size_t)-1) {
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "session event record is too large");
  }
  record = (char *)malloc(length + 1U);
  if (record == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate session event record");
  }
  if (lseek(fd, (off_t)record_start, SEEK_SET) < 0) {
    free(record);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to seek session event record");
  }
  offset = 0U;
  while (offset < length) {
    nread = read(fd, record + offset, length - offset);
    if (nread <= 0) {
      free(record);
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "failed to read session event record");
    }
    offset += (size_t)nread;
  }
  record[length] = '\0';
  memset(&doc, 0, sizeof(doc));
  CAI_LJ->init(CAI_LJ, &cai_local_event_map, &doc);
  lonejson_error_init(&json_error);
  status = CAI_LJ->parse_buffer(CAI_LJ, &cai_local_event_map, &doc, record,
                                length, &json_error);
  free(record);
  if (status != LONEJSON_STATUS_OK || strcmp(doc.record_type, "event") != 0) {
    CAI_LJ->cleanup(CAI_LJ, &cai_local_event_map, &doc);
    return cai_set_error_detail(error, CAI_ERR_INVALID,
                                "invalid session event record",
                                json_error.message);
  }
  if (doc.sequence == 0U || doc.sequence <= replay->previous_sequence) {
    CAI_LJ->cleanup(CAI_LJ, &cai_local_event_map, &doc);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session event sequences must be strictly increasing");
  }
  replay->previous_sequence = doc.sequence;
  rc = CAI_OK;
  if (doc.sequence > replay->after_sequence) {
    event.sequence = doc.sequence;
    event.type = doc.type;
    event.data = doc.data;
    rc = replay->callback(replay->callback_context, &event, error);
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_local_event_map, &doc);
  return rc;
}

static int cai_local_session_load_events_after(
    void *context, const char *scope, const char *session_id,
    unsigned long long after_sequence, cai_agent_session_event_fn callback,
    void *callback_context, cai_error *error) {
  cai_local_session_store *store;
  cai_local_event_replay_context replay;
  char hash[65];
  char filename[160];
  int scope_fd;
  int fd;
  int rc;

  if (context == NULL || callback == NULL ||
      !cai_store_session_id_valid(session_id)) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "valid session event replay arguments are required");
  }
  store = (cai_local_session_store *)context;
  memset(&replay, 0, sizeof(replay));
  replay.after_sequence = after_sequence;
  replay.callback = callback;
  replay.callback_context = callback_context;
  scope_fd = -1;
  fd = -1;
  rc = cai_store_open_scope(store, scope, &scope_fd, hash, error);
  if (rc == CAI_OK && snprintf(filename, sizeof(filename), "%s.jsonl",
                               session_id) >= (int)sizeof(filename)) {
    rc =
        cai_set_error(error, CAI_ERR_INVALID, "session identifier is too long");
  }
  if (rc == CAI_OK) {
    fd = openat(scope_fd, filename, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 && errno == ENOENT) {
      rc = CAI_OK;
      goto done;
    }
    if (fd < 0) {
      rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to open session event log");
    }
  }
  if (rc == CAI_OK)
    rc = cai_store_validate_private_regular_fd(fd, error);
  if (rc == CAI_OK && flock(fd, LOCK_SH) != 0) {
    rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                       "failed to lock session event log");
  }
  if (rc == CAI_OK) {
    rc = cai_local_visit_complete_records(fd, cai_local_replay_complete_event,
                                          &replay, error);
  }
done:
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

static int cai_local_checkpoint_record_bounds(
    int fd, long record_start, long record_end, long *out_state_start,
    long *out_state_end, unsigned long long *out_applied_event_sequence,
    unsigned long long *out_created_at_ns, int *out_has_created_at_ns,
    cai_error *error) {
  static const char prefix[] = "{\"record_type\":\"checkpoint\",";
  static const char timestamp_prefix[] = "\"checkpoint_created_at_ns\":";
  static const char sequence_prefix[] = "\"applied_event_sequence\":";
  char header[192];
  char *cursor;
  char *end;
  ssize_t nread;
  unsigned long long created_at_ns;
  unsigned long long sequence;

  if (out_state_start == NULL || out_state_end == NULL ||
      out_applied_event_sequence == NULL || out_created_at_ns == NULL ||
      out_has_created_at_ns == NULL || record_end <= record_start) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session checkpoint record bounds are required");
  }
  nread = pread(fd, header, sizeof(header) - 1U, (off_t)record_start);
  if (nread <= 0) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to read session checkpoint record");
  }
  header[nread] = '\0';
  if (strncmp(header, prefix, sizeof(prefix) - 1U) != 0) {
    *out_state_start = record_start;
    *out_state_end = record_end;
    *out_applied_event_sequence = 0U;
    *out_created_at_ns = 0U;
    *out_has_created_at_ns = 0;
    return CAI_OK;
  }
  cursor = header + sizeof(prefix) - 1U;
  created_at_ns = 0U;
  *out_has_created_at_ns = 0;
  if (strncmp(cursor, timestamp_prefix, sizeof(timestamp_prefix) - 1U) == 0) {
    cursor += sizeof(timestamp_prefix) - 1U;
    end = cursor;
    while (*end >= '0' && *end <= '9') {
      unsigned long long digit;

      digit = (unsigned long long)(*end - '0');
      if (created_at_ns > ((unsigned long long)-1 - digit) / 10U) {
        return cai_set_error(error, CAI_ERR_INVALID,
                             "invalid session checkpoint record");
      }
      created_at_ns = created_at_ns * 10U + digit;
      end++;
    }
    if (end == cursor || strncmp(end, ",", 1U) != 0) {
      return cai_set_error(error, CAI_ERR_INVALID,
                           "invalid session checkpoint record");
    }
    cursor = end + 1;
    *out_has_created_at_ns = 1;
  }
  if (strncmp(cursor, sequence_prefix, sizeof(sequence_prefix) - 1U) != 0) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "invalid session checkpoint record");
  }
  cursor += sizeof(sequence_prefix) - 1U;
  sequence = 0U;
  end = cursor;
  while (*end >= '0' && *end <= '9') {
    unsigned long long digit;

    digit = (unsigned long long)(*end - '0');
    if (sequence > ((unsigned long long)-1 - digit) / 10U) {
      return cai_set_error(error, CAI_ERR_INVALID,
                           "invalid session checkpoint record");
    }
    sequence = sequence * 10U + digit;
    end++;
  }
  if (end == cursor || strncmp(end, ",\"state\":", 9U) != 0 ||
      record_end - record_start < (long)(end - header) + 11L) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "invalid session checkpoint record");
  }
  *out_state_start = record_start + (long)(end - header) + 9L;
  *out_state_end = record_end - 1L;
  *out_applied_event_sequence = sequence;
  *out_created_at_ns = created_at_ns;
  return CAI_OK;
}

/*
 * Find the newest complete checkpoint record in one journal.  Journal events
 * are deliberately allowed to precede the first checkpoint: such a journal
 * records durable intent, but cannot itself resume an agent session.
 */
static int cai_local_find_latest_checkpoint(
    int fd, long *out_state_start, long *out_state_end,
    unsigned long long *out_applied_event_sequence,
    unsigned long long *out_created_at_ns, int *out_has_created_at_ns,
    int *out_found, cai_error *error) {
  static const char event_prefix[] = "{\"record_type\":\"event\",";
  char record_prefix[sizeof(event_prefix)];
  long start;
  long end;
  long complete_end;
  long before_end;
  ssize_t nread;
  int found_record;
  int rc;

  if (fd < 0 || out_state_start == NULL || out_state_end == NULL ||
      out_applied_event_sequence == NULL || out_created_at_ns == NULL ||
      out_has_created_at_ns == NULL || out_found == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session checkpoint search outputs are required");
  }
  *out_state_start = 0L;
  *out_state_end = 0L;
  *out_applied_event_sequence = 0U;
  *out_created_at_ns = 0U;
  *out_has_created_at_ns = 0;
  *out_found = 0;
  before_end = 0L;
  for (;;) {
    rc = cai_local_find_last_record_before(fd, before_end, &start, &end,
                                           &complete_end, &found_record, error);
    if (rc != CAI_OK) {
      return rc;
    }
    if (!found_record) {
      return CAI_OK;
    }
    nread = pread(fd, record_prefix, sizeof(record_prefix) - 1U, (off_t)start);
    if (nread < 0) {
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "failed to read session checkpoint record");
    }
    if (nread > 0) {
      record_prefix[nread] = '\0';
    }
    if (nread > 0 &&
        strncmp(record_prefix, event_prefix, sizeof(event_prefix) - 1U) == 0) {
      if (start == 0L) {
        return CAI_OK;
      }
      before_end = start;
      continue;
    }
    rc = cai_local_checkpoint_record_bounds(
        fd, start, end, out_state_start, out_state_end,
        out_applied_event_sequence, out_created_at_ns, out_has_created_at_ns,
        error);
    if (rc == CAI_OK) {
      *out_found = 1;
    }
    return rc;
  }
}

static unsigned long long cai_store_stat_mtime_ns(const struct stat *st) {
  unsigned long long seconds;
  long nanoseconds;

#ifdef __APPLE__
  if (st->st_mtimespec.tv_sec < 0 || st->st_mtimespec.tv_nsec < 0) {
    return 0U;
  }
  seconds = (unsigned long long)st->st_mtimespec.tv_sec;
  nanoseconds = st->st_mtimespec.tv_nsec;
#else
  if (st->st_mtim.tv_sec < 0 || st->st_mtim.tv_nsec < 0) {
    return 0U;
  }
  seconds = (unsigned long long)st->st_mtim.tv_sec;
  nanoseconds = st->st_mtim.tv_nsec;
#endif
  if (nanoseconds >= 1000000000L ||
      seconds > ((unsigned long long)-1 - (unsigned long long)nanoseconds) /
                    1000000000U) {
    return (unsigned long long)-1;
  }
  return seconds * 1000000000U + (unsigned long long)nanoseconds;
}

static int cai_store_candidate_is_newer(
    unsigned long long candidate_created_at_ns, const char *candidate_name,
    unsigned long long current_created_at_ns, const char *current_name) {
  if (candidate_created_at_ns != current_created_at_ns) {
    return candidate_created_at_ns > current_created_at_ns;
  }
  /* Generated XIDs sort chronologically, so this deterministic fallback also
   * selects the newer generated session on filesystems with tied timestamps. */
  return strcmp(candidate_name, current_name) > 0;
}

static unsigned long long
cai_store_checkpoint_order(unsigned long long created_at_ns,
                           int has_created_at_ns,
                           const struct stat *journal_stat) {
  if (has_created_at_ns) {
    return created_at_ns;
  }
  /* Journals written by older CAI versions have no checkpoint timestamp. */
  return cai_store_stat_mtime_ns(journal_stat);
}

static int cai_local_session_load_latest(
    void *context, const char *scope, char *session_id,
    size_t session_id_capacity, cai_source **out,
    unsigned long long *out_applied_event_sequence, cai_error *error) {
  cai_local_session_store *store;
  char hash[65];
  DIR *directory;
  struct dirent *entry;
  unsigned long long candidate_created_at_ns;
  char candidate[160];
  int scope_fd;
  int fd;
  int rc;

  if (out == NULL || session_id == NULL || session_id_capacity == 0U ||
      out_applied_event_sequence == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session checkpoint outputs are required");
  }
  *out = NULL;
  *out_applied_event_sequence = 0U;
  session_id[0] = '\0';
  if (context == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "session store is required");
  }
  store = (cai_local_session_store *)context;
  scope_fd = -1;
  candidate[0] = '\0';
  candidate_created_at_ns = 0U;
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
    long state_start;
    long state_end;
    unsigned long long applied_event_sequence;
    unsigned long long created_at_ns;
    unsigned long long checkpoint_order;
    int journal_fd;
    int found_checkpoint;
    int has_created_at_ns;
    size_t length;

    length = strlen(entry->d_name);
    if (!cai_store_session_filename_valid(entry->d_name) ||
        fstatat(scope_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != geteuid() ||
        (st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
      continue;
    }
    if (length >= sizeof(candidate)) {
      continue;
    }
    journal_fd =
        openat(scope_fd, entry->d_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (journal_fd < 0) {
      rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to open session checkpoint log");
      break;
    }
    rc = cai_store_validate_private_regular_fd(journal_fd, error);
    if (rc != CAI_OK) {
      close(journal_fd);
      break;
    }
    if (flock(journal_fd, LOCK_SH) != 0) {
      close(journal_fd);
      rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to lock session checkpoint log");
      break;
    }
    rc = cai_local_find_latest_checkpoint(
        journal_fd, &state_start, &state_end, &applied_event_sequence,
        &created_at_ns, &has_created_at_ns, &found_checkpoint, error);
    (void)flock(journal_fd, LOCK_UN);
    close(journal_fd);
    if (rc != CAI_OK) {
      break;
    }
    if (!found_checkpoint) {
      continue;
    }
    checkpoint_order =
        cai_store_checkpoint_order(created_at_ns, has_created_at_ns, &st);
    if (candidate[0] == '\0' ||
        cai_store_candidate_is_newer(checkpoint_order, entry->d_name,
                                     candidate_created_at_ns, candidate)) {
      memcpy(candidate, entry->d_name, length + 1U);
      candidate_created_at_ns = checkpoint_order;
    }
  }
  closedir(directory);
  if (rc != CAI_OK) {
    close(scope_fd);
    session_id[0] = '\0';
    return rc;
  }
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
  rc = cai_store_validate_private_regular_fd(fd, error);
  if (rc != CAI_OK) {
    close(fd);
    session_id[0] = '\0';
    return rc;
  }
  if (flock(fd, LOCK_SH) != 0) {
    close(fd);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to lock latest session checkpoint log");
  }
  {
    long state_start;
    long state_end;
    unsigned long long created_at_ns;
    int found_checkpoint;
    int has_created_at_ns;

    rc = cai_local_find_latest_checkpoint(
        fd, &state_start, &state_end, out_applied_event_sequence,
        &created_at_ns, &has_created_at_ns, &found_checkpoint, error);
    if (rc == CAI_OK && found_checkpoint) {
      rc = cai_local_checkpoint_source_open(fd, state_start, state_end, out,
                                            error);
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
  int written;
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
      written = snprintf(default_path, sizeof(default_path), "%s/cai/sessions",
                         xdg_state);
    } else if (home != NULL && home[0] != '\0') {
      written = snprintf(default_path, sizeof(default_path),
                         "%s/.local/state/cai/sessions", home);
    } else {
      return cai_set_error(
          error, CAI_ERR_INVALID,
          "XDG_STATE_HOME or HOME is required for local sessions");
    }
    if (written < 0 || (size_t)written >= sizeof(default_path)) {
      return cai_set_error(error, CAI_ERR_INVALID,
                           "default session store path is too long");
    }
    root = default_path;
  }
  rc = cai_store_make_directory(root, error);
  if (rc != CAI_OK) {
    return rc;
  }
  {
    int root_fd;

    root_fd = cai_store_directory_fd(root, error);
    if (root_fd < 0) {
      return error != NULL ? error->code : CAI_ERR_TRANSPORT;
    }
    rc = cai_store_validate_private_directory_fd(root_fd, error);
    (void)close(root_fd);
    if (rc != CAI_OK) {
      return rc;
    }
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
  out->append_event = cai_local_session_append_event;
  out->load_events_after = cai_local_session_load_events_after;
  out->context = store;
  return CAI_OK;
}

void cai_agent_local_session_store_close(cai_agent_session_store *store) {
  if (store == NULL) {
    return;
  }
  if (store->checkpoint == cai_local_session_checkpoint &&
      store->load_latest == cai_local_session_load_latest &&
      store->append_event == cai_local_session_append_event &&
      store->load_events_after == cai_local_session_load_events_after) {
    cai_local_session_store_destroy(store->context);
  }
  memset(store, 0, sizeof(*store));
}
