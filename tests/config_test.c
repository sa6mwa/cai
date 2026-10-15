#include <assert.h>
#include <cai/config.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct reader {
  const char *bytes;
  size_t size, pos, chunk;
  int fail;
} reader;
static size_t read_bytes(void *context, void *out, size_t count,
                         cai_error *error) {
  reader *r = context;
  size_t n;
  if (r->fail && r->pos >= 5U) {
    error->code = CAI_ERR_TRANSPORT;
    return 0U;
  }
  n = r->size - r->pos;
  if (n > count)
    n = count;
  if (n > r->chunk)
    n = r->chunk;
  memcpy(out, r->bytes + r->pos, n);
  r->pos += n;
  return n;
}
static int load_n(cai_config *config, const char *text, size_t size,
                  size_t chunk, int fail, cai_error *error) {
  cai_source_callbacks callbacks;
  cai_source *source;
  reader r;
  int rc;
  memset(&callbacks, 0, sizeof(callbacks));
  r.bytes = text;
  r.size = size;
  r.pos = 0U;
  r.chunk = chunk;
  r.fail = fail;
  callbacks.read = read_bytes;
  callbacks.context = &r;
  assert(cai_source_from_callbacks(&callbacks, &source, error) == CAI_OK);
  rc = config->load(config, source, "/configuration/sample.ini", error);
  cai_source_close(source);
  return rc;
}
static int load(cai_config *config, const char *text, size_t chunk, int fail,
                cai_error *error) {
  return load_n(config, text, strlen(text), chunk, fail, error);
}
static void reset(cai_error *error) {
  cai_error_cleanup(error);
  cai_error_init(error);
}
static void uncomment_assignments(char *text) {
  char *read = text, *write = text, *end;
  size_t length;
  int newline;
  while (*read) {
    end = strchr(read, '\n');
    if (end == NULL)
      end = read + strlen(read);
    newline = *end != '\0';
    if (end - read >= 2 && read[0] == '#' && read[1] == ' ' &&
        memchr(read, '=', (size_t)(end - read)) != NULL)
      read += 2;
    length = (size_t)(end - read);
    memmove(write, read, length);
    write += length;
    read = newline ? end + 1 : end;
    if (newline)
      *write++ = '\n';
  }
  *write = '\0';
}
static int reject_write(void *context, const void *bytes, size_t count,
                        cai_error *error) {
  (void)context;
  (void)bytes;
  (void)count;
  error->code = CAI_ERR_TRANSPORT;
  return CAI_ERR_TRANSPORT;
}
static void numeric_locale_test(void) {
  cai_config *config;
  cai_config_field field;
  cai_error error;
  cai_client_config client;
  const char *locale = getenv("CAI_TEST_CONFIG_LOCALE");
  assert(setlocale(LC_NUMERIC, locale) != NULL);
  assert(strcmp(localeconv()->decimal_point, ",") == 0);
  memset(&field, 0, sizeof(field));
  field.section = "host";
  field.key = "amount";
  field.type = CAI_CONFIG_REAL;
  field.default_value = "1.5";
  cai_error_init(&error);
  assert(cai_config_open(&field, 1U, &config, &error) == CAI_OK);
  assert(load(config, "[client_limits]\nmax_spend_usd=1.5\n", 1U, 0, &error) ==
         CAI_OK);
  assert(config->get_real(config, "client_limits", "max_spend_usd", 0.0) ==
         1.5);
  cai_client_config_init(&client);
  assert(config->apply_client(config, &client, &error) == CAI_OK);
  assert(client.usage_limits.max_spend_usd == 1.5);
  assert(config->set(config, "client_limits", "max_spend_usd", "1,5", &error) ==
         CAI_ERR_INVALID);
  reset(&error);
  assert(setlocale(LC_NUMERIC, "C") != NULL);
  assert(config->get_real(config, "client_limits", "max_spend_usd", 0.0) ==
         1.5);
  assert(strcmp(localeconv()->decimal_point, ".") == 0);
  config->close(config);
  cai_error_cleanup(&error);
}
int main(int argc, char **argv) {
  cai_config *config, *again;
  cai_error error;
  cai_client_config client;
  cai_agent_runtime_config runtime;
  cai_agent_config agent;
  cai_terminal_tool_config terminal;
  cai_sink *sink;
  FILE *file;
  char bytes[32768], path[4096], cwd[4096], expected[4096];
  char *oversized;
  size_t n, i, count;
  const char *value;
  const char *const *list;
  struct stat status;
  cai_config_field field;
  cai_config_field selected_fields[2];
  cai_sink_callbacks rejected;
  pid_t children[3];
  int result;
  mode_t mask;
  static const char valid[] =
      "config_env_has_precedence = false\r\n[client]\nprovider=berget\n"
      "timeout_ms=987\napi_protocol=chat_completions\n[agent]\nmodel=custom-"
      "model\n"
      "reasoning_effort=high\nterminal=false\n[review]\nallowed_models="
      "one\nallowed_models=two\n"
      "[response]\nmax_output_tokens=123\n[terminal]\noutput_max_bytes=4096\n"
      "[env]\nCAI_TEST_CONFIG_VALUE=\"secret-DO-NOT-LOG\\nline\"\n";
  static const char *const invalid[] = {
      "[unknown]\nx=y",
      "[client]\ntimeout_ms=-1",
      "[client]\ntimeout_ms=xx",
      "[client]\ntimeout_ms=9223372036854775808",
      "[agent]\nterminal=maybe",
      "[agent]\nmodel=a\nmodel=b",
      "[env]\nINVALID.NAME=secret-DO-NOT-LOG",
      "[env]\nCAI_TEST_CONFIG_VALUE=\"secret-DO-NOT-LOG",
      "[client]\nchat_max_attempts=17",
      "[client]\nchat_retry_max_delay_ms=60001",
      "[client_limits]\nmax_spend_usd=nan",
      "[client_limits]\nmax_spend_usd=-0.01",
      "[client_limits]\nmax_spend_usd=1e999",
      "[client]\nmissing_key=secret-DO-NOT-LOG",
      "[response]\nmax_output_tokens=2147483648",
      "[client]\napi_protocol=unsupported",
      "[client]\napi_protocol=\"\"",
      "[client]\nprovider=\"\"",
      "[response]\nsession_continuity=\"\"",
      "[response]\ncompact_threshold_percent=101",
      "[env]\nCAI_TEST_CONFIG_VALUE=one\nCAI_TEST_CONFIG_VALUE=two"};
  assert(argc == 2);
  if (getenv("CAI_TEST_CONFIG_LOCALE") != NULL) {
    numeric_locale_test();
    return 0;
  }
  cai_error_init(&error);
  assert(cai_config_open(NULL, 0U, &config, &error) == CAI_OK);
  for (i = 1U; i <= 16U; i *= 2U) {
    assert(load(config, valid, i, 0, &error) == CAI_OK);
    assert(strcmp(config->get(config, "agent", "model"), "custom-model") == 0);
    list = cai_config_get_list(config, "review", "allowed_models", &count);
    assert(count == 2U && strcmp(list[0], "one") == 0 &&
           strcmp(list[1], "two") == 0);
  }
  cai_client_config_init(&client);
  assert(config->apply_client(config, &client, &error) == CAI_OK);
  assert(client.timeout_ms == 987L &&
         client.api_protocol == CAI_API_PROTOCOL_CHAT_COMPLETIONS);
  cai_agent_runtime_config_init(&runtime);
  assert(config->apply_runtime(config, &runtime, &error) == CAI_OK);
  assert(runtime.disable_terminal &&
         strcmp(runtime.model, "custom-model") == 0 &&
         runtime.review_allowed_model_count == 2U);
  cai_agent_config_init(&agent);
  assert(config->apply_agent(config, &agent, &error) == CAI_OK &&
         agent.max_output_tokens == 123);
  memset(&terminal, 0, sizeof(terminal));
  assert(config->apply_terminal(config, &terminal, &error) == CAI_OK &&
         terminal.output_max_bytes == 4096U);
  for (i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    assert(load(config, invalid[i], 1U, 0, &error) == CAI_ERR_INVALID);
    assert(strstr(error.message, "sample.ini:") != NULL &&
           strstr(error.message, "secret-DO-NOT-LOG") == NULL);
    reset(&error);
    assert(strcmp(config->get(config, "agent", "model"), "custom-model") == 0);
  }
  assert(load(config, valid, 1U, 1, &error) == CAI_ERR_TRANSPORT);
  reset(&error);
  assert(load(config, valid, 1U, 1, NULL) == CAI_ERR_TRANSPORT);
  assert(strcmp(config->get(config, "agent", "model"), "custom-model") == 0);
  assert(load_n(config, "[agent]\nmodel=a\0b", 18U, 2U, 0, &error) ==
         CAI_ERR_INVALID);
  reset(&error);
  oversized = malloc(1024U * 1024U + 64U);
  assert(oversized != NULL);
  memcpy(oversized, "[agent]\nmodel=", 14U);
  memset(oversized + 14U, 'x', 1024U * 1024U + 48U);
  oversized[1024U * 1024U + 62U] = '\0';
  assert(load(config, oversized, 4096U, 0, &error) == CAI_ERR_INVALID);
  assert(strstr(error.message, "line exceeds limit") != NULL);
  reset(&error);
  assert(config->set(config, "agent", "model", oversized, &error) ==
         CAI_ERR_INVALID);
  assert(strcmp(config->get(config, "agent", "model"), "custom-model") == 0);
  free(oversized);
  reset(&error);
  assert(unsetenv("CAI_TEST_CONFIG_VALUE") == 0);
  assert(config->apply_environment(config, &error) == CAI_OK);
  assert(strcmp(getenv("CAI_TEST_CONFIG_VALUE"), "secret-DO-NOT-LOG\nline") ==
         0);
  assert(setenv("CAI_TEST_CONFIG_VALUE", "", 1) == 0);
  assert(config->apply_environment(config, &error) == CAI_OK &&
         getenv("CAI_TEST_CONFIG_VALUE")[0] == '\0');
  assert(config->set(config, "", "config_env_has_precedence", "true", &error) ==
         CAI_OK);
  assert(config->apply_environment(config, &error) == CAI_OK &&
         strstr(getenv("CAI_TEST_CONFIG_VALUE"), "secret-") != NULL);
  assert(unsetenv("CAI_TEST_CONFIG_VALUE") == 0);
  assert(config->set(config, "client", "ca_bundle_path", "relative.pem",
                     &error) == CAI_OK);
  assert(cai_config_resolve_path(config, "client", "ca_bundle_path", &value,
                                 &error) == CAI_OK);
  assert(strcmp(value, "/configuration/relative.pem") == 0);
  assert(cai_config_resolve_path(config, "agent", "model", &value, &error) ==
         CAI_ERR_INVALID);
  reset(&error);
  file = tmpfile();
  assert(file != NULL);
  assert(cai_sink_file(file, 0, &sink, &error) == CAI_OK);
  assert(config->write_template(config, sink, &error) == CAI_OK);
  cai_sink_close(sink);
  rewind(file);
  n = fread(bytes, 1, sizeof(bytes) - 1U, file);
  bytes[n] = '\0';
  fclose(file);
  assert(strstr(bytes, "secret-DO-NOT-LOG") == NULL &&
         strstr(bytes, "# config_env_has_precedence = false") != NULL);
  assert(strncmp(bytes, "## CAI configuration.", 21U) == 0);
  assert(strstr(bytes, "\n## Config [env]") != NULL);
  assert(strstr(bytes, "\n## Optional literal environment assignments;") !=
         NULL);
  /* Every single-# line is an assignment; documentation uses ##, even when
   * it contains '=' or a newline supplied by a host schema. */
  {
    const char *line = bytes, *end;
    while (*line != '\0') {
      end = strchr(line, '\n');
      if (end == NULL)
        end = line + strlen(line);
      if (line[0] == '#' && line[1] != '#') {
        assert(line[1] == ' ');
        assert(memchr(line, '=', (size_t)(end - line)) != NULL);
      }
      line = *end != '\0' ? end + 1 : end;
    }
  }
  assert(cai_config_open(NULL, 0U, &again, &error) == CAI_OK);
  assert(load(again, bytes, 1U, 0, &error) == CAI_OK &&
         again->get(again, "client", "provider") == NULL);
  snprintf(path, sizeof(path), "%s/config-private.ini", argv[1]);
  unlink(path);
  mask = umask(0222);
  assert(config->create_template_file(config, path, &error) == CAI_OK);
  umask(mask);
  assert(stat(path, &status) == 0 && (status.st_mode & 0777) == 0600);
  file = fopen(path, "wb");
  assert(file != NULL);
  fputs("# retained user file\n", file);
  fclose(file);
  assert(config->create_template_file(config, path, &error) == CAI_OK);
  file = fopen(path, "rb");
  assert(file != NULL);
  assert(fgets(bytes, sizeof(bytes), file) != NULL);
  fclose(file);
  assert(strcmp(bytes, "# retained user file\n") == 0);
  file = fopen(path, "wb");
  assert(file != NULL);
  fputs("[client]\nca_path = certs\n", file);
  fclose(file);
  assert(getcwd(cwd, sizeof(cwd)) != NULL);
  assert(chdir(argv[1]) == 0);
  assert(again->load_file(again, "config-private.ini", &error) == CAI_OK);
  assert(again->resolve_path(again, "client", "ca_path", &value, &error) ==
         CAI_OK);
  snprintf(expected, sizeof(expected), "%s/certs", argv[1]);
  assert(strcmp(value, expected) == 0);
  assert(chdir(cwd) == 0);
  unlink(path);
  assert(mkfifo(path, 0600) == 0);
  assert(config->load_file(config, path, &error) == CAI_ERR_INVALID);
  assert(strstr(error.message, "regular file") != NULL);
  reset(&error);
  unlink(path);
  /* Concurrent first boots must publish a complete file without replacing
   * another process's winner. Fork before any runtime worker threads exist. */
  for (i = 0U; i < 3U; i++) {
    children[i] = fork();
    assert(children[i] >= 0);
    if (children[i] == 0)
      _exit(config->create_template_file(config, path, &error) == CAI_OK ? 0
                                                                         : 1);
  }
  for (i = 0U; i < 3U; i++) {
    assert(waitpid(children[i], &result, 0) == children[i]);
    assert(WIFEXITED(result) && WEXITSTATUS(result) == 0);
  }
  assert(again->load_file(again, path, &error) == CAI_OK);
  unlink(path);
  assert(load(config, "[review]\nallowed_models=\"\"\n", 2U, 0, &error) ==
         CAI_OK);
  cai_agent_runtime_config_init(&runtime);
  assert(config->apply_runtime(config, &runtime, &error) == CAI_OK);
  assert(runtime.review_allowed_model_count == 0U &&
         runtime.review_allowed_models == NULL);
  assert(load(config,
              "[client]\nbase_url=\"\"\napi_key_env=\"\"\nmetadata_provider="
              "\"\"\napi_key=\"\"\n",
              2U, 0, &error) == CAI_OK);
  cai_client_config_init(&client);
  cai_client_config_use_berget(&client);
  client.api_key = "old-explicit-key";
  assert(config->apply_client(config, &client, &error) == CAI_OK);
  assert(strcmp(client.base_url, CAI_BERGET_BASE_URL) == 0);
  assert(strcmp(client.api_key_env, CAI_BERGET_API_KEY_ENV) == 0);
  assert(strcmp(client.metadata_provider, "berget") == 0 &&
         client.api_key == NULL);
  assert(config->field_count(config) > 50U);
  assert(config->field_at(config, config->field_count(config)) == NULL);
  assert(cai_config_field_count(NULL) == 0U);
  assert(cai_config_get_list(NULL, NULL, NULL, &count) == NULL && count == 0U);
  /* Host-selected schema is strict and its descriptions cannot inject
   * active assignments into the commented template. */
  memset(&field, 0, sizeof(field));
  field.section = "host";
  field.key = "token";
  field.type = CAI_CONFIG_STRING;
  field.flags = CAI_CONFIG_SECRET;
  field.default_value = "secret-DO-NOT-LOG";
  field.description = "Line one\nunsafe = active\r[malicious]";
  again->close(again);
  assert(cai_config_open_schema(&field, 1U, &again, &error) == CAI_OK);
  field.description = NULL;
  assert(again->field_count(again) == 1U);
  assert(again->set(again, "client", "timeout_ms", "123", &error) ==
         CAI_ERR_INVALID);
  reset(&error);
  file = tmpfile();
  assert(file != NULL);
  assert(cai_sink_file(file, 0, &sink, &error) == CAI_OK);
  assert(again->write_template(again, sink, &error) == CAI_OK);
  cai_sink_close(sink);
  rewind(file);
  n = fread(bytes, 1, sizeof(bytes) - 1U, file);
  bytes[n] = '\0';
  fclose(file);
  assert(strstr(bytes, "\n## unsafe = active") != NULL);
  assert(strstr(bytes, "\n## [malicious]") != NULL);
  assert(strstr(bytes, "secret-DO-NOT-LOG") == NULL);
  assert(load(again, bytes, 2U, 0, &error) == CAI_OK);
  memset(&rejected, 0, sizeof(rejected));
  rejected.write = reject_write;
  assert(cai_sink_from_callbacks(&rejected, &sink, &error) == CAI_OK);
  assert(again->write_template(again, sink, &error) == CAI_ERR_TRANSPORT);
  cai_sink_close(sink);
  reset(&error);
  again->close(again);
  field.section = "client";
  field.key = "api_key";
  field.flags = 0U;
  assert(cai_config_open(&field, 1U, &again, &error) == CAI_OK);
  for (i = 0U; i < again->field_count(again); i++) {
    const cai_config_field *metadata = again->field_at(again, i);
    if (strcmp(metadata->section, "client") == 0 &&
        strcmp(metadata->key, "api_key") == 0)
      assert((metadata->flags & CAI_CONFIG_SECRET) != 0U);
  }
  again->close(again);
  field.key = "timeout_ms";
  field.default_value = NULL;
  assert(cai_config_open_schema(&field, 1U, &again, &error) == CAI_ERR_INVALID);
  reset(&error);
  field.section = "response";
  field.key = "max_output_tokens";
  field.type = CAI_CONFIG_INTEGER;
  field.default_value = "2147483648";
  assert(cai_config_open_schema(&field, 1U, &again, &error) == CAI_ERR_INVALID);
  reset(&error);
  field.section = "runtime";
  field.key = "event_queue_limit";
  field.flags = CAI_CONFIG_SIGNED;
  field.default_value = "-1";
  assert(cai_config_open_schema(&field, 1U, &again, &error) == CAI_ERR_INVALID);
  reset(&error);
  field.default_value = NULL;
  assert(cai_config_open_schema(&field, 1U, &again, &error) == CAI_OK);
  assert(again->set(again, "runtime", "event_queue_limit", "-1", &error) ==
         CAI_ERR_INVALID);
  reset(&error);
  again->close(again);
  field.section = "client";
  field.key = "api_protocol";
  field.type = CAI_CONFIG_STRING;
  field.flags = 0U;
  field.choices = "responses|invented";
  assert(cai_config_open_schema(&field, 1U, &again, &error) == CAI_ERR_INVALID);
  reset(&error);
  field.choices = NULL;
  assert(cai_config_open_schema(&field, 1U, &again, &error) == CAI_OK);
  assert(strcmp(again->field_at(again, 0U)->choices,
                "responses|chat_completions") == 0);
  assert(again->set(again, "client", "api_protocol", "invented", &error) ==
         CAI_ERR_INVALID);
  reset(&error);
  again->close(again);
  field.choices = "responses";
  assert(cai_config_open_schema(&field, 1U, &again, &error) == CAI_OK);
  assert(again->set(again, "client", "api_protocol", "chat_completions",
                    &error) == CAI_ERR_INVALID);
  reset(&error);
  assert(again->set(again, "client", "api_protocol", "responses", &error) ==
         CAI_OK);
  again->close(again);
  field.section = "";
  field.key = "config_env_has_precedence";
  field.choices = NULL;
  assert(cai_config_open_schema(&field, 1U, &again, &error) == CAI_ERR_INVALID);
  reset(&error);
  field.section = "review";
  field.key = "allowed_models";
  field.type = CAI_CONFIG_BOOL;
  assert(cai_config_open_schema(&field, 1U, &again, &error) == CAI_ERR_INVALID);
  reset(&error);
  field.section = "host";
  field.key = "signed_value";
  field.type = CAI_CONFIG_INTEGER;
  field.flags = CAI_CONFIG_SIGNED;
  field.default_value = "-5";
  assert(cai_config_open_schema(&field, 1U, &again, &error) == CAI_OK);
  assert(again->set(again, "host", "signed_value", "-10", &error) == CAI_OK);
  assert(again->get_integer(again, "host", "signed_value", 0LL) == -10LL);
  again->close(again);
  field.flags = 8U;
  field.default_value = NULL;
  assert(cai_config_open_schema(&field, 1U, &again, &error) == CAI_ERR_INVALID);
  reset(&error);
  memset(selected_fields, 0, sizeof(selected_fields));
  selected_fields[0].section = "host";
  selected_fields[0].key = "demo";
  selected_fields[0].type = CAI_CONFIG_STRING;
  selected_fields[0].default_value = "value";
  selected_fields[1].section = "";
  selected_fields[1].key = "config_env_has_precedence";
  selected_fields[1].type = CAI_CONFIG_BOOL;
  selected_fields[1].default_value = "false";
  assert(cai_config_open_schema(selected_fields, 2U, &again, &error) == CAI_OK);
  file = tmpfile();
  assert(file != NULL);
  assert(cai_sink_file(file, 0, &sink, &error) == CAI_OK);
  assert(again->write_template(again, sink, &error) == CAI_OK);
  cai_sink_close(sink);
  rewind(file);
  n = fread(bytes, 1, sizeof(bytes) - 1U, file);
  bytes[n] = '\0';
  fclose(file);
  assert(strstr(bytes, "# config_env_has_precedence") <
         strstr(bytes, "[host]"));
  uncomment_assignments(bytes);
  assert(load(again, bytes, 1U, 0, &error) == CAI_OK);
  assert(again->get_bool(again, "", "config_env_has_precedence", 1) == 0);
  assert(strcmp(again->get(again, "host", "demo"), "value") == 0);
  again->close(again);
  config->close(config);
  assert(cai_config_open(NULL, 0U, &again, &error) == CAI_OK);
  cai_agent_runtime_config_init(&runtime);
  assert(again->apply_runtime(again, &runtime, &error) == CAI_OK);
  assert(!runtime.disable_web_search && !runtime.disable_chrome &&
         !runtime.enable_todo && !runtime.enable_searxng &&
         !runtime.enable_revgeo);
  assert(load(again,
              "[agent]\nweb_search=false\ntodo=true\nsearxng=true\nrevgeo="
              "true\nread_file=false\nlist_files=false\napply_patch="
              "false\nview_image=false\ngoals=false\nskills=false\n[chrome]"
              "\nenabled=false\ntimeout_ms=150\nsocket=\"/tmp/"
              "browser.sock\"\n[todo]\ndefault_board=work\n[searxng]\nbase_url="
              "http://127.0.0.1:8889\n[revgeo]\nzoom=15\n",
              1U, 0, &error) == CAI_OK);
  assert(again->apply_runtime(again, &runtime, &error) == CAI_OK);
  assert(runtime.disable_web_search && runtime.disable_chrome &&
         runtime.enable_todo && runtime.enable_searxng &&
         runtime.enable_revgeo && runtime.disable_read_file &&
         runtime.disable_list_files && runtime.disable_apply_patch &&
         runtime.disable_view_image && runtime.disable_goals &&
         runtime.disable_skills);
  assert(strcmp(runtime.chrome_config.socket_path, "/tmp/browser.sock") == 0 &&
         runtime.chrome_config.timeout_ms == 150L &&
         strcmp(runtime.todo.default_board, "work") == 0 &&
         strcmp(runtime.searxng.base_url, "http://127.0.0.1:8889") == 0 &&
         runtime.revgeo.zoom == 15);
  again->close(again);
  cai_error_cleanup(&error);
  return 0;
}
