#define _POSIX_C_SOURCE 200809L
#include "../cli/log.h"
#include "../cli/response.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(test)                                                            \
  do {                                                                         \
    if (!(test)) {                                                             \
      fprintf(stderr, "logging check failed at line %d: %s\n", __LINE__,       \
              #test);                                                          \
      return 1;                                                                \
    }                                                                          \
  } while (0)

typedef struct notices {
  size_t count;
  char text[40][1200];
} notices;

static int collect(void *context, const char *text) {
  notices *out = (notices *)context;
  if (out->count >= 40U)
    return -1;
  snprintf(out->text[out->count++], sizeof(out->text[0]), "%s", text);
  return 0;
}

static char *read_log(const char *path) {
  FILE *file;
  char *bytes;
  long length;
  file = fopen(path, "rb");
  if (file == NULL)
    return NULL;
  if (fseek(file, 0L, SEEK_END) != 0 || (length = ftell(file)) < 0L ||
      fseek(file, 0L, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }
  bytes = (char *)malloc((size_t)length + 1U);
  if (bytes == NULL) {
    fclose(file);
    return NULL;
  }
  if (fread(bytes, 1U, (size_t)length, file) != (size_t)length) {
    free(bytes);
    fclose(file);
    return NULL;
  }
  bytes[length] = '\0';
  fclose(file);
  return bytes;
}

static void *background_warning(void *context) {
  pslog_logger *logger = (pslog_logger *)context;
  logger->warn(logger, "worker warning", NULL, 0U);
  return NULL;
}

static int structured_notices(cai_cli_log *log) {
  notices observed;
  pslog_field fields[6];
  char long_detail[6000];
  log->logger->errorf(log->logger, "cai.http.transport_error",
                      "method=%s path=%s error=%s", "POST", "/responses",
                      "Connection refused: localhost:12345");
  log->logger->warnf(log->logger, "cai.models.catalog",
                     "source=%s models=%lu code=%d error=%s detail=%s",
                     "fallback", 0UL, 5, "catalog unavailable",
                     "HTTP 503: model discovery is temporarily unavailable");
  fields[0] = pslog_str("detail", "quota exhausted\nTry again later");
  fields[1] = pslog_i64("http_status", 429);
  fields[2] = pslog_str("provider_code", "rate_limit_exceeded");
  fields[3] = pslog_str("request_id", "req_notice_429");
  fields[4] = pslog_str("backend", "fixture");
  fields[5] = pslog_bool("retryable", 1);
  log->logger->error(log->logger, "provider rejected the request", fields, 6U);
  memset(&observed, 0, sizeof(observed));
  CHECK(cai_cli_log_notices(log, collect, &observed) == 0 &&
        observed.count == 3U);
  CHECK(strstr(observed.text[0], "HTTP transport failed") != NULL);
  CHECK(strncmp(observed.text[0], "* ERROR: ", 9U) == 0);
  CHECK(strstr(observed.text[0], "Connection refused: localhost:12345") !=
        NULL);
  CHECK(strstr(observed.text[0], "method: POST") != NULL &&
        strstr(observed.text[0], "path: /responses") != NULL);
  CHECK(strstr(observed.text[1], "Model catalog unavailable") != NULL);
  CHECK(strncmp(observed.text[1], "* WARN: ", 8U) == 0);
  CHECK(strstr(observed.text[1], "catalog unavailable") != NULL &&
        strstr(observed.text[1], "HTTP 503") != NULL &&
        strstr(observed.text[1], "source: fallback") != NULL);
  CHECK(strstr(observed.text[2], "quota exhausted Try again later") != NULL);
  CHECK(strstr(observed.text[2], "http_status: 429") != NULL &&
        strstr(observed.text[2], "req_notice_429") != NULL &&
        strstr(observed.text[2], "rate_limit_exceeded") != NULL);
  CHECK(strstr(observed.text[2], "backend: fixture") != NULL &&
        strstr(observed.text[2], "retryable: true") != NULL);
  CHECK(strchr(observed.text[2], '\n') == NULL);

  fields[0] = pslog_str("tool", "apply_patch");
  fields[1] = pslog_str("call_id", "call_failure");
  log->logger->warn(log->logger, "cai.agent.runtime.tool.error", fields, 2U);
  fields[1] = pslog_str("tool_call_id", "call_failure");
  fields[2] = pslog_str("event", "tool_call_failed");
  log->logger->warn(log->logger, "Patch context does not match", fields, 3U);
  memset(&observed, 0, sizeof(observed));
  CHECK(cai_cli_log_notices(log, collect, &observed) == 0 &&
        observed.count == 0U);
  {
    char *stored = read_log(log->path);
    CHECK(stored != NULL);
    CHECK(strstr(stored, "cai.agent.runtime.tool.error") != NULL &&
          strstr(stored, "Patch context does not match") != NULL);
    free(stored);
  }
  /* A coincidentally named warning without tool attribution stays visible. */
  log->logger->warn(log->logger, "cai.agent.runtime.tool.error", NULL, 0U);
  fields[0] = pslog_str("tool", "apply_patch");
  fields[1] = pslog_str("event", "tool_call_failed");
  log->logger->warn(log->logger, "Unattributed failure", fields, 2U);
  memset(&observed, 0, sizeof(observed));
  CHECK(cai_cli_log_notices(log, collect, &observed) == 0 &&
        observed.count == 2U);

  log->logger->errorf(log->logger, "cai.response.provider_error",
                      "provider_code=%s detail=%s request_id=%s",
                      "cyber_policy", "Provider cybersecurity notice",
                      "req_cyber");
  memset(&observed, 0, sizeof(observed));
  CHECK(cai_cli_log_notices(log, collect, &observed) == 0 &&
        observed.count == 1U);
  CHECK(strstr(observed.text[0], "* ERROR: Provider rejected the response") !=
        NULL);
  CHECK(strstr(observed.text[0], "cyber_policy") != NULL &&
        strstr(observed.text[0], "Provider cybersecurity notice") != NULL &&
        strstr(observed.text[0], "req_cyber") != NULL);
  log->logger->errorf(log->logger, "cai.agent.policy_diagnostic", "error=%s",
                      "diagnostic also blocked");
  memset(&observed, 0, sizeof(observed));
  CHECK(cai_cli_log_notices(log, collect, &observed) == 0 &&
        observed.count == 1U);
  CHECK(strstr(observed.text[0], "Policy explanation unavailable") != NULL &&
        strstr(observed.text[0], "diagnostic also blocked") != NULL);

  fields[0] = pslog_str("sys", "storage.pouch");
  fields[1] = pslog_str("ns", "cai.sessions");
  fields[2] = pslog_str("error", "pouch query-index manifest is not readable");
  fields[3] = pslog_i64("code", 1);
  log->logger->warn(log->logger, "cache.load.index.error", fields, 4U);
  memset(&observed, 0, sizeof(observed));
  CHECK(cai_cli_log_notices(log, collect, &observed) == 0 &&
        observed.count == 1U);
  CHECK(strstr(observed.text[0], "indexed queries will attempt recovery") !=
        NULL);
  CHECK(strstr(observed.text[0], "from stored records") != NULL);
  CHECK(strstr(observed.text[0], "ns: cai.sessions") != NULL);
  CHECK(strstr(observed.text[0], "manifest is not readable") != NULL);
  memset(long_detail, 'x', sizeof(long_detail) - 1U);
  memcpy(long_detail, "failure detail: ", 16U);
  long_detail[sizeof(long_detail) - 1U] = '\0';
  fields[0] = pslog_str("detail", long_detail);
  log->logger->error(log->logger, "cai.http.transport_error", fields, 1U);
  memset(&observed, 0, sizeof(observed));
  CHECK(cai_cli_log_notices(log, collect, &observed) == 0 &&
        observed.count == 1U);
  CHECK(strstr(observed.text[0], "failure detail: ") != NULL);
  CHECK(strstr(observed.text[0], "more detail in the session log") != NULL);
  return 0;
}

int main(int argc, char **argv) {
  cai_cli_log log;
  cai_cli_response response;
  cai_error error;
  cai_source *source;
  pslog_field field;
  pslog_logger *child;
  pthread_t worker;
  notices observed;
  struct stat status;
  char root[4096];
  char first_path[4096];
  char extra_path[4128];
  char tee_output[4140];
  char startup_path[4096];
  char input[30001];
  char output[8192];
  char *record;
  char *cursor;
  size_t length;
  size_t offset;
  size_t i;
  size_t n;
  DIR *directory;
  struct dirent *entry;

  CHECK(argc == 2);
  CHECK(snprintf(root, sizeof(root), "%s/startup-logging-XXXXXX", argv[1]) <
        (int)sizeof(root));
  CHECK(mkdtemp(root) != NULL);
  CHECK(setenv("XDG_CACHE_HOME", root, 1) == 0);
  CHECK(setenv("LOG_MODE", "json", 1) == 0);
  CHECK(setenv("LOG_NO_COLOR", "true", 1) == 0);
  CHECK(setenv("LOG_DISABLE_TIMESTAMP", "true", 1) == 0);
  CHECK(unsetenv("LOG_OUTPUT") == 0);
  CHECK(unsetenv("LOG_LEVEL") == 0);
  CHECK(unsetenv("LOG_FORCE_COLOR") == 0);
  CHECK(unsetenv("LOG_VERBOSE_FIELDS") == 0);
  cai_error_init(&error);
  CHECK(cai_cli_log_open(&log, PSLOG_LEVEL_TRACE, 1, &error) == CAI_OK);
  CHECK(cai_cli_log_interactive(&log, &error) == CAI_OK);
  strcpy(startup_path, log.path);
  log.logger->debug(log.logger, "startup diagnostic", NULL, 0U);
  field = pslog_str("sys", "native.fixture");
  child = log.logger->with(log.logger, &field, 1U);
  CHECK(child != NULL);
  child->info(child, "native child", NULL, 0U);
  child->destroy(child);
  CHECK(cai_cli_log_session(&log, "first", &error) == CAI_OK);
  CHECK(access(startup_path, F_OK) != 0);
  strcpy(first_path, log.path);
  CHECK(fstat(log.fd, &status) == 0 && (status.st_mode & 0777U) == 0600U);
  CHECK(stat(log.directory, &status) == 0 && (status.st_mode & 0777U) == 0700U);
  CHECK((fcntl(log.fd, F_GETFD) & FD_CLOEXEC) != 0);
  for (i = 0U; i < sizeof(input) - 1U; i += 3U) {
    input[i] = (char)0xe2;
    input[i + 1U] = (char)0x98;
    input[i + 2U] = (char)0x83;
  }
  input[sizeof(input) - 1U] = '\0';
  field = pslog_str("prompt_kind", "normal");
  cai_cli_log_message(log.logger, PSLOG_LEVEL_INFO, "assistant_text_delta",
                      "assistant", input, sizeof(input) - 1U, &field, 1U);
  log.logger->info(log.logger, "contains WRN and ERR text", NULL, 0U);
  memset(&observed, 0, sizeof(observed));
  CHECK(cai_cli_log_notices(&log, collect, &observed) == 0 &&
        observed.count == 0U);
  CHECK(pthread_create(&worker, NULL, background_warning, log.logger) == 0);
  CHECK(pthread_join(worker, NULL) == 0);
  CHECK(cai_cli_error(&error, CAI_ERR_TRANSPORT, "host error") ==
        CAI_ERR_TRANSPORT);
  cai_cli_log_error(log.logger, "fixture operation", &error);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  CHECK(cai_cli_log_notices(&log, collect, &observed) == 0 &&
        observed.count == 2U);
  CHECK(strstr(observed.text[0], "* WARN: worker warning") != NULL);
  CHECK(strstr(observed.text[1], "fixture operation: host error") != NULL);
  CHECK(structured_notices(&log) == 0);
  record = read_log(first_path);
  CHECK(record != NULL && strstr(record, "startup diagnostic") != NULL);
  CHECK(strstr(record, "native.fixture") != NULL);
  CHECK(strstr(record, "\"lvl\":\"info\"") != NULL);
  CHECK(strstr(record, "\"role\":\"assistant\"") != NULL);
  CHECK(strstr(record, "\"prompt_kind\":\"normal\"") != NULL);
  CHECK(strstr(record, "\"app\"") == NULL && strchr(record, 27) == NULL);
  length = 0U;
  cursor = record;
  while ((cursor = strstr(cursor, "\xe2\x98\x83")) != NULL) {
    length += 3U;
    cursor += 3;
  }
  CHECK(length == sizeof(input) - 1U);
  free(record);
  CHECK(cai_cli_log_session(&log, "../escape", &error) == CAI_ERR_INVALID);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  CHECK(cai_cli_log_session(&log, "second", &error) == CAI_OK);
  log.logger->info(log.logger, "second session", NULL, 0U);
  record = read_log(log.path);
  CHECK(record != NULL && strstr(record, "second session") != NULL &&
        strstr(record, "startup diagnostic") == NULL);
  free(record);
  for (i = 0U; i < 45U; i++)
    log.logger->warn(log.logger, "queued warning", NULL, 0U);
  memset(&observed, 0, sizeof(observed));
  CHECK(cai_cli_log_notices(&log, collect, &observed) == 0 &&
        observed.count == 33U);
  CHECK(strstr(observed.text[0], "13 additional notices") != NULL);
  CHECK(cai_cli_log_close(&log) == 0);

  CHECK(setenv("LOG_MODE", "console", 1) == 0);
  CHECK(setenv("LOG_NO_COLOR", "false", 1) == 0);
  CHECK(setenv("LOG_FORCE_COLOR", "true", 1) == 0);
  CHECK(cai_cli_log_open(&log, PSLOG_LEVEL_TRACE, 1, &error) == CAI_OK);
  CHECK(cai_cli_log_interactive(&log, &error) == CAI_OK);
  log.logger->info(log.logger, "message says WRN fake warning", NULL, 0U);
  log.logger->warn(log.logger, "real console warning", NULL, 0U);
  memset(&observed, 0, sizeof(observed));
  CHECK(cai_cli_log_notices(&log, collect, &observed) == 0 &&
        observed.count == 1U);
  CHECK(strstr(observed.text[0], "real console warning") != NULL &&
        strchr(observed.text[0], 27) == NULL);
  log.logger->warnf(log.logger, "cai.models.catalog", "error=%s detail=%s",
                    "catalog unavailable", "HTTP 503");
  memset(&observed, 0, sizeof(observed));
  CHECK(cai_cli_log_notices(&log, collect, &observed) == 0 &&
        observed.count == 1U);
  CHECK(strstr(observed.text[0], "catalog unavailable") != NULL &&
        strstr(observed.text[0], "HTTP 503") != NULL);
  CHECK(cai_cli_log_close(&log) == 0);

  CHECK(setenv("LOG_MODE", "json", 1) == 0);
  CHECK(setenv("LOG_NO_COLOR", "true", 1) == 0);
  CHECK(setenv("LOG_VERBOSE_FIELDS", "true", 1) == 0);
  CHECK(setenv("LOG_LEVEL", "warn", 1) == 0);
  CHECK(cai_cli_log_open(&log, PSLOG_LEVEL_TRACE, 1, &error) == CAI_OK);
  CHECK(cai_cli_log_interactive(&log, &error) == CAI_OK);
  log.logger->info(log.logger, "filtered info", NULL, 0U);
  log.logger->warn(log.logger, "verbose warning", NULL, 0U);
  memset(&observed, 0, sizeof(observed));
  CHECK(cai_cli_log_notices(&log, collect, &observed) == 0 &&
        observed.count == 1U);
  CHECK(strstr(observed.text[0], "verbose warning") != NULL);
  CHECK(structured_notices(&log) == 0);
  record = read_log(log.path);
  CHECK(record != NULL && strstr(record, "filtered info") == NULL &&
        strstr(record, "\"level\":\"warn\"") != NULL);
  free(record);
  CHECK(cai_cli_log_close(&log) == 0);

  CHECK(snprintf(extra_path, sizeof(extra_path), "%s/extra.log", root) <
        (int)sizeof(extra_path));
  CHECK(snprintf(tee_output, sizeof(tee_output), "default+%s", extra_path) <
        (int)sizeof(tee_output));
  CHECK(setenv("LOG_OUTPUT", tee_output, 1) == 0);
  CHECK(cai_cli_log_open(&log, PSLOG_LEVEL_TRACE, 1, &error) == CAI_OK);
  CHECK(cai_cli_log_interactive(&log, &error) == CAI_OK);
  CHECK(cai_cli_log_session(&log, "tee", &error) == CAI_OK);
  log.logger->warn(log.logger, "tee warning", NULL, 0U);
  memset(&observed, 0, sizeof(observed));
  CHECK(cai_cli_log_notices(&log, collect, &observed) == 0 &&
        observed.count == 1U);
  CHECK(strstr(observed.text[0], "tee warning") != NULL);
  CHECK(cai_cli_log_close(&log) == 0);
  record = read_log(extra_path);
  CHECK(record != NULL && strstr(record, "tee warning") != NULL);
  free(record);
  CHECK(unsetenv("LOG_OUTPUT") == 0);
  CHECK(setenv("HOME", root, 1) == 0);
  CHECK(unsetenv("XDG_CACHE_HOME") == 0);
  CHECK(cai_cli_log_open(&log, PSLOG_LEVEL_TRACE, 1, &error) == CAI_OK);
  CHECK(cai_cli_log_interactive(&log, &error) == CAI_OK);
  CHECK(strstr(log.directory, "/.cache/cai") != NULL);
  CHECK(cai_cli_log_close(&log) == 0);

  memset(&response, 0, sizeof(response));
  source = NULL;
  CHECK(cai_cli_response_source(&response, &source, &error) == CAI_OK &&
        source == NULL);
  CHECK(cai_cli_response_append(&response, root, input, sizeof(input) - 1U,
                                &error) == CAI_OK);
  cai_cli_response_boundary(&response);
  CHECK(cai_cli_response_append(&response, root, "", 0U, &error) == CAI_OK);
  CHECK(cai_cli_response_source(&response, &source, &error) == CAI_OK);
  offset = 0U;
  while ((n = cai_source_read(source, output, sizeof(output), &error)) > 0U) {
    CHECK(offset + n <= sizeof(input) - 1U &&
          memcmp(output, input + offset, n) == 0);
    offset += n;
  }
  CHECK(offset == sizeof(input) - 1U && error.code == CAI_OK);
  cai_source_close(source);
  cai_cli_response_boundary(&response);
  CHECK(cai_cli_response_source(&response, &source, &error) == CAI_OK &&
        source == NULL);
  cai_cli_response_boundary(&response);
  CHECK(cai_cli_response_source(&response, &source, &error) == CAI_OK &&
        source == NULL);
  CHECK(cai_cli_response_append(&response, root, "latest ", 7U, &error) ==
        CAI_OK);
  CHECK(cai_cli_response_append(&response, root, "response", 8U, &error) ==
        CAI_OK);
  CHECK(cai_cli_response_source(&response, &source, &error) == CAI_OK);
  CHECK(cai_source_read(source, output, sizeof(output), &error) == 15U &&
        memcmp(output, "latest response", 15U) == 0);
  CHECK(cai_source_read(source, output, sizeof(output), &error) == 0U);
  cai_source_close(source);
  cai_cli_response_boundary(&response);
  CHECK(cai_cli_response_source(&response, &source, &error) == CAI_OK &&
        source != NULL);
  CHECK(cai_source_read(source, output, sizeof(output), &error) == 15U &&
        memcmp(output, "latest response", 15U) == 0);
  cai_source_close(source);
  CHECK(fstat(fileno(response.file), &status) == 0 && status.st_nlink == 0 &&
        (status.st_mode & 0777U) == 0600U);
  directory = opendir(root);
  CHECK(directory != NULL);
  while ((entry = readdir(directory)) != NULL)
    CHECK(strncmp(entry->d_name, ".response-", 10U) != 0);
  closedir(directory);
  cai_cli_response_close(&response);
  cai_error_cleanup(&error);
  return 0;
}
