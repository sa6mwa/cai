#define _POSIX_C_SOURCE 200809L
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct notice_record {
  char level[16];
  char verbose_level[16];
  char message[1024];
  char verbose_message[1024];
  char operation[256];
} notice_record;
static const lonejson_field notice_fields[] = {
    LONEJSON_FIELD_STRING_FIXED(notice_record, level, "lvl",
                                LONEJSON_OVERFLOW_TRUNCATE),
    LONEJSON_FIELD_STRING_FIXED(notice_record, verbose_level, "level",
                                LONEJSON_OVERFLOW_TRUNCATE),
    LONEJSON_FIELD_STRING_FIXED(notice_record, message, "msg",
                                LONEJSON_OVERFLOW_TRUNCATE),
    LONEJSON_FIELD_STRING_FIXED(notice_record, verbose_message, "message",
                                LONEJSON_OVERFLOW_TRUNCATE),
    LONEJSON_FIELD_STRING_FIXED(notice_record, operation, "operation",
                                LONEJSON_OVERFLOW_TRUNCATE)};
LONEJSON_MAP_DEFINE(notice_map, notice_record, notice_fields);

static void notice(cai_cli_log *log) {
  notice_record record;
  lonejson *json;
  lonejson_error error;
  char plain[4096];
  const char *label;
  const char *cursor;
  size_t used;
  size_t i;
  size_t slot;
  uint64_t signal;
  int escape;
  if (log->wakeup_fd < 0)
    return;
  used = 0U;
  escape = 0;
  for (i = 0U; i < log->prefix_length; i++) {
    unsigned char ch;
    ch = (unsigned char)log->prefix[i];
    if (escape) {
      if (ch >= 64U && ch <= 126U && ch != '[')
        escape = 0;
    } else if (ch == 27U) {
      escape = 1;
    } else if (ch >= 32U && ch != 127U) {
      plain[used++] = (char)ch;
    }
  }
  plain[used] = '\0';
  memset(&record, 0, sizeof(record));
  if (plain[0] == '{') {
    json = lonejson_new(NULL, &error);
    if (json != NULL) {
      (void)lonejson_parse_buffer(json, &notice_map, &record, plain, used,
                                  &error);
      lonejson_free(json);
    }
    if (record.level[0] == '\0')
      strcpy(record.level, record.verbose_level);
    if (record.message[0] == '\0')
      strcpy(record.message, record.verbose_message);
    if (strcmp(record.level, "warn") != 0 &&
        strcmp(record.level, "error") != 0 &&
        strcmp(record.level, "fatal") != 0 &&
        strcmp(record.level, "panic") != 0)
      return;
    label = record.level;
    cursor = record.message[0] != '\0' ? record.message
                                       : "log event (see session log)";
  } else {
    /* Find the first native severity token, before the message. Do not
     * surface an INFO message just because its text happens to say WRN. */
    cursor = plain;
    for (;;) {
      if ((cursor == plain || cursor[-1] == ' ') && strlen(cursor) >= 4U &&
          cursor[3] == ' ' &&
          (strncmp(cursor, "TRC", 3U) == 0 || strncmp(cursor, "DBG", 3U) == 0 ||
           strncmp(cursor, "INF", 3U) == 0 || strncmp(cursor, "WRN", 3U) == 0 ||
           strncmp(cursor, "ERR", 3U) == 0 || strncmp(cursor, "FTL", 3U) == 0 ||
           strncmp(cursor, "PNC", 3U) == 0 || strncmp(cursor, "---", 3U) == 0))
        break;
      if (*cursor == '\0')
        return;
      cursor++;
    }
    if (strncmp(cursor, "WRN", 3U) == 0)
      label = "warn";
    else if (strncmp(cursor, "ERR", 3U) == 0 ||
             strncmp(cursor, "FTL", 3U) == 0 || strncmp(cursor, "PNC", 3U) == 0)
      label = "error";
    else
      return;
    cursor += 4U;
  }
  if (log->count == 32U) {
    log->head = (log->head + 1U) % 32U;
    log->count--;
    log->dropped++;
  }
  slot = (log->head + log->count++) % 32U;
  snprintf(log->notices[slot], sizeof(log->notices[slot]), "[%s] %s%s%s", label,
           record.operation, record.operation[0] != '\0' ? ": " : "", cursor);
  for (i = 0U; log->notices[slot][i] != '\0'; i++) {
    if ((unsigned char)log->notices[slot][i] < 32U ||
        log->notices[slot][i] == 127)
      log->notices[slot][i] = ' ';
  }
  signal = 1U;
  (void)write(log->wakeup_fd, &signal, sizeof(signal));
}

static int log_write(void *context, const char *data, size_t count,
                     size_t *written) {
  cai_cli_log *log;
  ssize_t n;
  size_t i;
  log = (cai_cli_log *)context;
  pthread_mutex_lock(&log->lock);
  do {
    n = write(log->fd >= 0 ? log->fd : STDERR_FILENO, data, count);
  } while (n < 0 && errno == EINTR);
  if (n < 0) {
    log->failed = errno != 0 ? errno : EIO;
    *written = 0U;
  } else {
    *written = (size_t)n;
    for (i = 0U; i < *written; i++) {
      if (data[i] == '\n') {
        log->prefix[log->prefix_length] = '\0';
        notice(log);
        log->prefix_length = 0U;
      } else if (log->prefix_length + 1U < sizeof(log->prefix)) {
        log->prefix[log->prefix_length++] = data[i];
      }
    }
  }
  pthread_mutex_unlock(&log->lock);
  return n < 0 ? log->failed : 0;
}

static int cache_directory(cai_cli_log *log, cai_error *error) {
  const char *base;
  char *cursor;
  struct stat status;
  int n;
  base = getenv("XDG_CACHE_HOME");
  if (base != NULL && *base != '\0')
    n = snprintf(log->directory, sizeof(log->directory), "%s/cai", base);
  else {
    base = getenv("HOME");
    if (base == NULL || *base == '\0')
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "set HOME or XDG_CACHE_HOME for cai logs");
    n = snprintf(log->directory, sizeof(log->directory), "%s/.cache/cai", base);
  }
  if (n < 0 || (size_t)n >= sizeof(log->directory) || log->directory[0] != '/')
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "cai cache path must be absolute and fit PATH_MAX");
  for (cursor = log->directory + 1;; cursor++) {
    if (*cursor == '/' || *cursor == '\0') {
      char saved;
      saved = *cursor;
      *cursor = '\0';
      if (mkdir(log->directory, 0700) != 0 && errno != EEXIST) {
        *cursor = saved;
        return cai_cli_error(error, CAI_ERR_TRANSPORT,
                             "create cai log directory");
      }
      *cursor = saved;
      if (saved == '\0')
        break;
    }
  }
  if (lstat(log->directory, &status) != 0 || !S_ISDIR(status.st_mode) ||
      status.st_uid != geteuid() || (status.st_mode & 077U) != 0U)
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "cai cache directory must be owned by you with mode 0700");
  return CAI_OK;
}

int cai_cli_log_open(cai_cli_log *log, cai_error *error) {
  pslog_config config;
  memset(log, 0, sizeof(*log));
  log->fd = -1;
  log->wakeup_fd = -1;
  if (pthread_mutex_init(&log->lock, NULL) != 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "initialize logging lock");
  log->initialized = 1;
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.min_level = PSLOG_LEVEL_TRACE;
  config.output.write = log_write;
  config.output.close = NULL;
  config.output.isatty = NULL;
  config.output.userdata = log;
  config.output.owned = 0;
  log->root = pslog_new_from_env("LOG_", &config);
  log->logger = log->root;
  return log->logger != NULL
             ? CAI_OK
             : cai_cli_error(error, CAI_ERR_NOMEM,
                             "create pslog logger from LOG_ environment");
}

int cai_cli_log_interactive(cai_cli_log *log, cai_error *error) {
  int rc;
  rc = cache_directory(log, error);
  if (rc != CAI_OK)
    return rc;
  if (snprintf(log->path, sizeof(log->path), "%s/startup-XXXXXX",
               log->directory) >= (int)sizeof(log->path))
    return cai_cli_error(error, CAI_ERR_INVALID, "session log path too long");
  log->fd = mkstemp(log->path);
  if (log->fd < 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "create private startup log");
  log->bootstrap = 1;
  if (fcntl(log->fd, F_SETFD, FD_CLOEXEC) != 0 ||
      fcntl(log->fd, F_SETFL, O_APPEND) != 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "protect startup log descriptor");
  log->wakeup_fd = eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
  if (log->wakeup_fd < 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "create logging wakeup descriptor");
  return CAI_OK;
}

int cai_cli_log_session(cai_cli_log *log, const char *id, cai_error *error) {
  char path[4096];
  char bytes[8192];
  int fd;
  ssize_t n = 0;
  struct stat status;
  ssize_t done;
  ssize_t written;
  int rc;
  const char *cursor;
  if (log->fd < 0)
    return CAI_OK;
  for (cursor = id; *cursor != '\0'; cursor++) {
    if (!((*cursor >= 'a' && *cursor <= 'z') ||
          (*cursor >= 'A' && *cursor <= 'Z') ||
          (*cursor >= '0' && *cursor <= '9') || *cursor == '-' ||
          *cursor == '_'))
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "unsafe session ID for log path");
  }
  if (snprintf(path, sizeof(path), "%s/%s.log", log->directory, id) >=
      (int)sizeof(path))
    return cai_cli_error(error, CAI_ERR_INVALID, "session log path too long");
  if (strcmp(path, log->path) == 0)
    return CAI_OK;
  fd = open(path, O_CREAT | O_APPEND | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "open session log");
  if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_uid != geteuid() || (status.st_mode & 077U) != 0U) {
    close(fd);
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "session log must be a private file owned by you");
  }
  pthread_mutex_lock(&log->lock);
  rc = CAI_OK;
  /* Bootstrap records belong to the first selected session. When switching
   * sessions, the previous session's file is left intact. */
  if (log->bootstrap) {
    if (lseek(log->fd, 0, SEEK_SET) < 0)
      rc = CAI_ERR_TRANSPORT;
    while (rc == CAI_OK && (n = read(log->fd, bytes, sizeof(bytes))) > 0) {
      done = 0;
      while (done < n) {
        written = write(fd, bytes + done, (size_t)(n - done));
        if (written <= 0) {
          rc = CAI_ERR_TRANSPORT;
          break;
        }
        done += written;
      }
    }
    if (rc == CAI_OK && n < 0)
      rc = CAI_ERR_TRANSPORT;
    if (rc == CAI_OK)
      unlink(log->path);
  }
  if (rc == CAI_OK) {
    close(log->fd);
    log->fd = fd;
    strcpy(log->path, path);
    log->bootstrap = 0;
  } else {
    close(fd);
  }
  pthread_mutex_unlock(&log->lock);
  return rc == CAI_OK ? rc
                      : cai_cli_error(error, rc,
                                      "move bootstrap records to session log");
}

int cai_cli_log_notices(cai_cli_log *log, int (*visit)(void *, const char *),
                        void *context) {
  char text[1200];
  uint64_t signal;
  if (!log->initialized)
    return 0;
  if (log->wakeup_fd >= 0)
    (void)read(log->wakeup_fd, &signal, sizeof(signal));
  for (;;) {
    pthread_mutex_lock(&log->lock);
    if (log->dropped != 0UL) {
      snprintf(text, sizeof(text),
               "[warn] %lu additional notices are in the session log",
               log->dropped);
      log->dropped = 0UL;
    } else if (log->count != 0U) {
      strcpy(text, log->notices[log->head]);
      log->head = (log->head + 1U) % 32U;
      log->count--;
    } else {
      pthread_mutex_unlock(&log->lock);
      break;
    }
    pthread_mutex_unlock(&log->lock);
    if (visit(context, text) != 0)
      return -1;
  }
  return 0;
}

int cai_cli_log_close(cai_cli_log *log) {
  int rc;
  rc = log->failed;
  if (log->logger != NULL && log->logger != log->root)
    log->logger->destroy(log->logger);
  if (log->root != NULL) {
    if (log->root->close(log->root) != 0)
      rc = EIO;
    log->root->destroy(log->root);
  }
  if (log->fd >= 0 && close(log->fd) != 0)
    rc = EIO;
  if (log->wakeup_fd >= 0)
    close(log->wakeup_fd);
  if (log->initialized)
    pthread_mutex_destroy(&log->lock);
  return rc;
}

void cai_cli_log_message(pslog_logger *logger, pslog_level level,
                         const char *event, const char *role, const char *bytes,
                         size_t count, const pslog_field *extra,
                         size_t extra_count) {
  char message[4097];
  pslog_field fields[24];
  size_t offset;
  size_t n;
  size_t i;
  size_t field_count;
  if (logger == NULL)
    return;
  field_count = 0U;
  fields[field_count++] = pslog_str("event", event);
  fields[field_count++] = pslog_str("role", role);
  for (i = 0U; i < extra_count && field_count < 23U; i++)
    fields[field_count++] = extra[i];
  offset = 0U;
  do {
    n = count - offset;
    if (n > sizeof(message) - 1U) {
      n = sizeof(message) - 1U;
      while (n > 0U && ((unsigned char)bytes[offset + n] & 0xc0U) == 0x80U)
        n--;
      if (n == 0U)
        n = sizeof(message) - 1U;
    }
    if (n > 0U)
      memcpy(message, bytes + offset, n);
    for (i = 0U; i < n; i++) {
      if (message[i] == '\0')
        message[i] = '?';
    }
    message[n] = '\0';
    fields[field_count] = pslog_u64("offset", (pslog_uint64)offset);
    logger->log(logger, level, message, fields, field_count + 1U);
    offset += n;
  } while (offset < count);
}

void cai_cli_log_error(pslog_logger *logger, const char *operation,
                       const cai_error *error) {
  pslog_field fields[5];
  size_t count;
  const char *message;
  count = 0U;
  fields[count++] = pslog_str("operation", operation);
  if (error != NULL) {
    fields[count++] = pslog_i64("http_status", error->http_status);
    fields[count++] = pslog_str("provider_code", error->server_code);
    fields[count++] = pslog_str("request_id", error->request_id);
    fields[count++] = pslog_str("detail", error->detail);
  }
  message = error != NULL && error->message != NULL ? error->message
                                                    : "operation failed";
  cai_cli_log_message(logger, PSLOG_LEVEL_ERROR, "error", "diagnostic", message,
                      strlen(message), fields, count);
}
