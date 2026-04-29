#include <cai/cai.h>

#include "cai_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct test_state {
  int failures;
} test_state;

typedef struct read_state {
  const char *text;
  size_t offset;
  int closed;
} read_state;

typedef struct write_state {
  char buffer[64];
  size_t length;
  int closed;
} write_state;

static void test_fail(test_state *state, const char *name, const char *msg) {
  state->failures++;
  fprintf(stderr, "FAIL %s: %s\n", name, msg);
}

static void expect_int(test_state *state, const char *name, long actual,
                       long expected) {
  if (actual != expected) {
    char msg[128];
    snprintf(msg, sizeof(msg), "expected %ld got %ld", expected, actual);
    test_fail(state, name, msg);
  }
}

static void expect_str(test_state *state, const char *name, const char *actual,
                       const char *expected) {
  if (actual == NULL || strcmp(actual, expected) != 0) {
    test_fail(state, name, "string mismatch");
  }
}

static void write_file_or_die(const char *path, const char *text) {
  FILE *fp;

  fp = fopen(path, "w");
  if (fp == NULL) {
    perror(path);
    exit(2);
  }
  fputs(text, fp);
  fclose(fp);
}

static void test_model_capabilities(test_state *state) {
  const cai_model_info *info;

  info = cai_model_info_by_id(CAI_MODEL_GPT_5_4_NANO);
  if (info == NULL) {
    test_fail(state, "model_capabilities", "model missing");
    return;
  }
  expect_str(state, "model_capabilities", info->id, CAI_MODEL_GPT_5_4_NANO);
  expect_int(
      state, "model_responses",
      cai_model_supports(CAI_MODEL_GPT_5_4_NANO, CAI_MODEL_CAP_RESPONSES), 1L);
  expect_int(state, "model_realtime",
             cai_model_supports(CAI_MODEL_GPT_5_4_NANO, CAI_MODEL_CAP_REALTIME),
             1L);
  expect_int(state, "model_unknown",
             cai_model_supports("future-model", CAI_MODEL_CAP_RESPONSES), 0L);
}

static void test_env_precedence(test_state *state) {
  char template_dir[] = "/tmp/cai-env-test-XXXXXX";
  char original_cwd[4096];
  char *key;
  cai_error error;

  if (getcwd(original_cwd, sizeof(original_cwd)) == NULL) {
    test_fail(state, "env_precedence", "getcwd failed");
    return;
  }
  if (mkdtemp(template_dir) == NULL) {
    test_fail(state, "env_precedence", "mkdtemp failed");
    return;
  }
  if (chdir(template_dir) != 0) {
    test_fail(state, "env_precedence", "chdir failed");
    return;
  }

  cai_error_init(&error);
  setenv("OPENAI_API_KEY", "env-key", 1);
  key = NULL;
  expect_int(state, "env_fallback",
             cai_resolve_api_key(NULL, NULL, &key, &error), CAI_OK);
  expect_str(state, "env_fallback_value", key, "env-key");
  cai_free_mem(NULL, key);
  cai_error_cleanup(&error);

  write_file_or_die(".env", "OPENAI_API_KEY=dotenv-key\n");
  key = NULL;
  expect_int(state, "dotenv_override",
             cai_resolve_api_key(NULL, NULL, &key, &error), CAI_OK);
  expect_str(state, "dotenv_override_value", key, "dotenv-key");
  cai_free_mem(NULL, key);
  cai_error_cleanup(&error);

  write_file_or_die(".env", "export OPENAI_API_KEY = \"quoted-key\" \n");
  key = NULL;
  expect_int(state, "dotenv_quoted",
             cai_resolve_api_key(NULL, NULL, &key, &error), CAI_OK);
  expect_str(state, "dotenv_quoted_value", key, "quoted-key");
  cai_free_mem(NULL, key);
  cai_error_cleanup(&error);

  write_file_or_die(".env", "OPENAI_API_KEY=dotenv-key\n");
  key = NULL;
  expect_int(state, "explicit_override",
             cai_resolve_api_key(NULL, "explicit-key", &key, &error), CAI_OK);
  expect_str(state, "explicit_override_value", key, "explicit-key");
  cai_free_mem(NULL, key);
  cai_error_cleanup(&error);

  write_file_or_die(".env", "OTHER=value\n");
  key = NULL;
  expect_int(state, "dotenv_missing_key",
             cai_resolve_api_key(NULL, NULL, &key, &error), CAI_ERR_INVALID);
  if (key != NULL) {
    test_fail(state, "dotenv_missing_key", "unexpected key allocated");
    cai_free_mem(NULL, key);
  }
  cai_error_cleanup(&error);

  if (chdir(original_cwd) != 0) {
    test_fail(state, "env_precedence", "restore chdir failed");
  }
}

static size_t test_read(void *context, void *buffer, size_t count,
                        cai_error *error) {
  read_state *state;
  size_t remaining;
  size_t n;

  (void)error;
  state = (read_state *)context;
  remaining = strlen(state->text) - state->offset;
  n = remaining < count ? remaining : count;
  if (n > 0U) {
    memcpy(buffer, state->text + state->offset, n);
    state->offset += n;
  }
  return n;
}

static int test_reset(void *context, cai_error *error) {
  read_state *state;

  (void)error;
  state = (read_state *)context;
  state->offset = 0U;
  return CAI_OK;
}

static void test_read_close(void *context) {
  read_state *state;

  state = (read_state *)context;
  state->closed = 1;
}

static int test_write(void *context, const void *bytes, size_t count,
                      cai_error *error) {
  write_state *state;

  (void)error;
  state = (write_state *)context;
  if (state->length + count >= sizeof(state->buffer)) {
    return CAI_ERR_INVALID;
  }
  memcpy(state->buffer + state->length, bytes, count);
  state->length += count;
  state->buffer[state->length] = '\0';
  return CAI_OK;
}

static void test_write_close(void *context) {
  write_state *state;

  state = (write_state *)context;
  state->closed = 1;
}

static void test_source_sink(test_state *state) {
  read_state reader;
  write_state writer;
  cai_source_callbacks source_callbacks;
  cai_sink_callbacks sink_callbacks;
  cai_source *source;
  cai_sink *sink;
  cai_error error;
  char buffer[8];

  cai_error_init(&error);
  reader.text = "abcdef";
  reader.offset = 0U;
  reader.closed = 0;
  source_callbacks.read = test_read;
  source_callbacks.reset = test_reset;
  source_callbacks.close = test_read_close;
  source_callbacks.context = &reader;
  source = NULL;
  expect_int(state, "source_create",
             cai_source_from_callbacks(&source_callbacks, &source, &error),
             CAI_OK);
  expect_int(state, "source_read_1",
             (long)cai_source_read(source, buffer, 3U, &error), 3L);
  buffer[3] = '\0';
  expect_str(state, "source_read_1_value", buffer, "abc");
  expect_int(state, "source_reset", cai_source_reset(source, &error), CAI_OK);
  expect_int(state, "source_read_2",
             (long)cai_source_read(source, buffer, 6U, &error), 6L);
  buffer[6] = '\0';
  expect_str(state, "source_read_2_value", buffer, "abcdef");
  cai_source_close(source);
  expect_int(state, "source_closed", reader.closed, 1L);

  writer.length = 0U;
  writer.closed = 0;
  writer.buffer[0] = '\0';
  sink_callbacks.write = test_write;
  sink_callbacks.close = test_write_close;
  sink_callbacks.context = &writer;
  sink = NULL;
  expect_int(state, "sink_create",
             cai_sink_from_callbacks(&sink_callbacks, &sink, &error), CAI_OK);
  expect_int(state, "sink_write", cai_sink_write(sink, "xyz", 3U, &error),
             CAI_OK);
  expect_str(state, "sink_write_value", writer.buffer, "xyz");
  cai_sink_close(sink);
  expect_int(state, "sink_closed", writer.closed, 1L);
  cai_error_cleanup(&error);
}

static void test_client_open(test_state *state) {
  cai_client_config config;
  cai_client *client;
  cai_error error;

  cai_error_init(&error);
  cai_client_config_init(&config);
  config.api_key = "test-key";
  config.base_url = "http://example.test/v1";
  client = NULL;
  expect_int(state, "client_open", cai_client_open(&config, &client, &error),
             CAI_OK);
  if (client == NULL) {
    test_fail(state, "client_open", "client not allocated");
  } else {
    expect_str(state, "client_api_key", client->api_key, "test-key");
    expect_str(state, "client_base_url", client->base_url,
               "http://example.test/v1");
    expect_int(state, "client_limit", (long)client->json_response_limit_bytes,
               (long)CAI_DEFAULT_JSON_RESPONSE_LIMIT);
  }
  cai_client_close(client);
  cai_error_cleanup(&error);
}

int main(void) {
  test_state state;

  state.failures = 0;
  test_model_capabilities(&state);
  test_env_precedence(&state);
  test_source_sink(&state);
  test_client_open(&state);
  if (state.failures != 0) {
    fprintf(stderr, "%d test(s) failed\n", state.failures);
    return 1;
  }
  return 0;
}
