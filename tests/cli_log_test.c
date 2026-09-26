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
  CHECK(strstr(observed.text[0], "[warn] worker warning") != NULL);
  CHECK(strstr(observed.text[1], "fixture operation: host error") != NULL);
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
  CHECK(cai_cli_response_append(&response, root, "latest ", 7U, &error) ==
        CAI_OK);
  CHECK(cai_cli_response_append(&response, root, "response", 8U, &error) ==
        CAI_OK);
  CHECK(cai_cli_response_source(&response, &source, &error) == CAI_OK);
  CHECK(cai_source_read(source, output, sizeof(output), &error) == 15U &&
        memcmp(output, "latest response", 15U) == 0);
  CHECK(cai_source_read(source, output, sizeof(output), &error) == 0U);
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
