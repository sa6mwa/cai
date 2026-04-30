#include <cai/cai.h>

#include "cai_internal.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
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

typedef struct tool_weather_args {
  char *city;
  long long days;
} tool_weather_args;

typedef struct raw_tool_state {
  char seen[64];
} raw_tool_state;

static const lonejson_field tool_weather_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(tool_weather_args, city, "city"),
    LONEJSON_FIELD_I64(tool_weather_args, days, "days")};
LONEJSON_MAP_DEFINE(tool_weather_map, tool_weather_args, tool_weather_fields);

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

static char *test_spooled_to_cstr(lonejson_spooled *spool) {
  lonejson_spooled cursor;
  lonejson_error json_error;
  lonejson_read_result chunk;
  unsigned char buffer[256];
  char *out;
  char *grown;
  size_t length;
  size_t capacity;

  out = NULL;
  length = 0U;
  capacity = 0U;
  cursor = *spool;
  lonejson_error_init(&json_error);
  if (lonejson_spooled_rewind(&cursor, &json_error) != LONEJSON_STATUS_OK) {
    return NULL;
  }
  for (;;) {
    chunk = lonejson_spooled_read(&cursor, buffer, sizeof(buffer));
    if (chunk.error_code != 0) {
      free(out);
      return NULL;
    }
    if (length + chunk.bytes_read + 1U > capacity) {
      capacity = capacity == 0U ? 512U : capacity * 2U;
      while (capacity < length + chunk.bytes_read + 1U) {
        capacity *= 2U;
      }
      grown = (char *)realloc(out, capacity);
      if (grown == NULL) {
        free(out);
        return NULL;
      }
      out = grown;
    }
    if (chunk.bytes_read > 0U) {
      memcpy(out + length, buffer, chunk.bytes_read);
      length += chunk.bytes_read;
    }
    if (chunk.eof) {
      break;
    }
  }
  if (out == NULL) {
    out = (char *)malloc(1U);
    if (out == NULL) {
      return NULL;
    }
  }
  out[length] = '\0';
  return out;
}

static void test_model_capabilities(test_state *state) {
  const cai_model_info *info;
  cai_agent_config agent_config;

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
  expect_int(state, "model_context",
             cai_model_context_window_tokens(CAI_MODEL_GPT_5_4_NANO), 400000L);
  expect_int(state, "model_compact_limit",
             cai_model_auto_compact_token_limit(CAI_MODEL_GPT_5_4), 840000L);
  cai_agent_config_init(&agent_config);
  expect_int(state, "agent_config_auto_compact_default",
             agent_config.auto_compact, 0L);
  expect_int(state, "agent_config_compact_limit_default",
             agent_config.auto_compact_token_limit, 0L);
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

static int test_weather_tool(void *context, const void *params,
                             cai_sink *output, cai_error *error) {
  const tool_weather_args *args;
  char text[64];
  int len;

  (void)context;
  args = (const tool_weather_args *)params;
  len = snprintf(text, sizeof(text), "%s:%lld", args->city, args->days);
  if (len <= 0 || (size_t)len >= sizeof(text)) {
    return cai_set_error(error, CAI_ERR_INVALID, "weather output too large");
  }
  return cai_sink_write(output, text, (size_t)len, error);
}

static int test_raw_tool(void *context, const char *arguments_json,
                         cai_sink *output, cai_error *error) {
  raw_tool_state *state;

  state = (raw_tool_state *)context;
  snprintf(state->seen, sizeof(state->seen), "%s", arguments_json);
  return cai_sink_write(output, arguments_json, strlen(arguments_json), error);
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

static void test_tool_registry(test_state *state) {
  static const char schema[] =
      "{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\"},"
      "\"days\":{\"type\":\"integer\"}},\"required\":[\"city\"]}";
  cai_tool_registry *registry;
  cai_response_create_params *params;
  cai_sink_callbacks sink_callbacks;
  cai_sink *sink;
  write_state writer;
  raw_tool_state raw_state;
  cai_error error;
  char *json;

  cai_error_init(&error);
  registry = NULL;
  params = NULL;
  sink = NULL;
  json = NULL;
  writer.buffer[0] = '\0';
  writer.length = 0U;
  writer.closed = 0;
  raw_state.seen[0] = '\0';
  sink_callbacks.write = test_write;
  sink_callbacks.close = test_write_close;
  sink_callbacks.context = &writer;

  expect_int(state, "tool_registry_new",
             cai_tool_registry_new(&registry, &error), CAI_OK);
  expect_int(state, "tool_register_typed",
             cai_tool_registry_register_lonejson(
                 registry, "weather", "Get weather", &tool_weather_map, schema,
                 1, test_weather_tool, NULL, &error),
             CAI_OK);
  expect_int(state, "tool_register_raw",
             cai_tool_registry_register_raw(registry, "raw_echo",
                                            "Echo raw JSON", schema, 0,
                                            test_raw_tool, &raw_state, &error),
             CAI_OK);
  expect_int(state, "tool_sink",
             cai_sink_from_callbacks(&sink_callbacks, &sink, &error), CAI_OK);
  expect_int(state, "tool_run_typed",
             cai_tool_registry_run(registry, "weather",
                                   "{\"city\":\"Malmo\",\"days\":3}", sink,
                                   &error),
             CAI_OK);
  expect_str(state, "tool_run_typed_output", writer.buffer, "Malmo:3");
  writer.buffer[0] = '\0';
  writer.length = 0U;
  expect_int(
      state, "tool_run_raw",
      cai_tool_registry_run(registry, "raw_echo", "{\"x\":1}", sink, &error),
      CAI_OK);
  expect_str(state, "tool_run_raw_output", writer.buffer, "{\"x\":1}");
  expect_str(state, "tool_run_raw_seen", raw_state.seen, "{\"x\":1}");
  cai_sink_close(sink);
  sink = NULL;

  expect_int(state, "tool_params_new",
             cai_response_create_params_new(&params, &error), CAI_OK);
  expect_int(state, "tool_params_model",
             cai_response_create_params_set_model(
                 params, CAI_MODEL_GPT_5_4_NANO, &error),
             CAI_OK);
  expect_int(
      state, "tool_params_input",
      cai_response_create_params_add_text(params, "user", "hello", &error),
      CAI_OK);
  expect_int(state, "tool_params_add_registry",
             cai_tool_registry_add_to_response_params(registry, params, &error),
             CAI_OK);
  expect_int(
      state, "tool_params_serialize",
      cai_response_create_params_serialize_json(params, &json, NULL, &error),
      CAI_OK);
  if (json == NULL) {
    test_fail(state, "tool_params_serialize", "no JSON returned");
  } else {
    if (strstr(json, "\"name\":\"weather\"") == NULL ||
        strstr(json, "\"name\":\"raw_echo\"") == NULL ||
        strstr(json, "\"strict\":true") == NULL ||
        strstr(json, "\"strict\":false") == NULL) {
      test_fail(state, "tool_params_serialize",
                "registry tools missing from JSON");
    }
    free(json);
  }

  cai_response_create_params_destroy(params);
  cai_tool_registry_destroy(registry);
  cai_error_cleanup(&error);
}

static void test_client_open(test_state *state) {
  cai_client_config config;
  cai_client *client;
  struct pslog_logger *logger;
  cai_error error;

  cai_error_init(&error);
  cai_client_config_init(&config);
  logger = (struct pslog_logger *)(void *)&state->failures;
  config.api_key = "test-key";
  config.base_url = "http://example.test/v1";
  config.logger = logger;
  client = NULL;
  expect_int(state, "client_open", cai_client_open(&config, &client, &error),
             CAI_OK);
  if (client == NULL) {
    test_fail(state, "client_open", "client not allocated");
  } else {
    expect_str(state, "client_api_key", client->api_key, "test-key");
    expect_str(state, "client_base_url", client->base_url,
               "http://example.test/v1");
    expect_int(state, "client_http_2_disabled", client->http_2_disabled, 0);
    if (client->logger != logger) {
      test_fail(state, "client_logger", "borrowed logger not preserved");
    }
    expect_int(state, "client_logger_disabled", client->logger_disabled, 0);
    expect_int(state, "client_limit", (long)client->json_response_limit_bytes,
               (long)CAI_DEFAULT_JSON_RESPONSE_LIMIT);
  }
  cai_client_close(client);
  client = NULL;
  config.logger_disabled = 1;
  expect_int(state, "client_open_logger_disabled",
             cai_client_open(&config, &client, &error), CAI_OK);
  if (client == NULL) {
    test_fail(state, "client_open_logger_disabled", "client not allocated");
  } else {
    if (client->logger != NULL) {
      test_fail(state, "client_logger_disabled_null",
                "disabled logger should not be retained");
    }
    expect_int(state, "client_logger_disabled_flag", client->logger_disabled,
               1);
  }
  cai_client_close(client);
  cai_error_cleanup(&error);
}

static void test_response_json(test_state *state) {
  cai_response_create_params *params;
  cai_response *response;
  cai_output *output;
  cai_sink_callbacks sink_callbacks;
  cai_sink *sink;
  cai_error error;
  cai_token_usage usage;
  write_state writer;
  char response_json[1024];
  char *json;
  size_t json_len;

  strcpy(response_json, "{\"id\":\"resp_123\",\"status\":\"completed\",");
  strcat(response_json, "\"model\":\"gpt-5.4-nano\",\"created_at\":123,");
  strcat(response_json, "\"error\":{\"code\":\"rate_limited\",");
  strcat(response_json, "\"message\":\"slow down\"},");
  strcat(response_json, "\"incomplete_details\":{\"reason\":");
  strcat(response_json, "\"max_output_tokens\"},\"conversation\":{\"id\":");
  strcat(response_json, "\"conv_123\"},\"output\":[{\"type\":\"message\",");
  strcat(response_json, "\"content\":[{\"type\":\"output_text\",");
  strcat(response_json, "\"text\":\"hello \"},{\"type\":\"output_text\",");
  strcat(response_json, "\"text\":\"world\"},{\"type\":\"refusal\",");
  strcat(response_json, "\"refusal\":\"cannot comply\"}]},{\"id\":\"fc_1\",");
  strcat(response_json, "\"type\":\"function_call\",\"call_id\":\"call_1\",");
  strcat(response_json, "\"name\":\"weather\",\"arguments\":");
  strcat(response_json, "\"{\\\"city\\\":\\\"Malmo\\\"}\"}],\"usage\":{");
  strcat(response_json, "\"input_tokens\":11,\"input_tokens_details\":{");
  strcat(response_json, "\"cached_tokens\":5},\"output_tokens\":7,");
  strcat(response_json, "\"output_tokens_details\":{");
  strcat(response_json, "\"reasoning_tokens\":3},");
  strcat(response_json, "\"total_tokens\":18}}");

  cai_error_init(&error);
  params = NULL;
  output = NULL;
  json = NULL;
  expect_int(state, "params_new",
             cai_response_create_params_new(&params, &error), CAI_OK);
  expect_int(state, "params_set_model",
             cai_response_create_params_set_model(
                 params, CAI_MODEL_GPT_5_4_NANO, &error),
             CAI_OK);
  expect_int(
      state, "params_set_instructions",
      cai_response_create_params_set_instructions(params, "be brief", &error),
      CAI_OK);
  expect_int(state, "params_set_conversation",
             cai_response_create_params_set_conversation_id(params, "conv_123",
                                                            &error),
             CAI_OK);
  expect_int(
      state, "params_set_max_output_tokens",
      cai_response_create_params_set_max_output_tokens(params, 128, &error),
      CAI_OK);
  expect_int(
      state, "params_set_reasoning",
      cai_response_create_params_set_reasoning(
          params, CAI_REASONING_EFFORT_LOW, CAI_REASONING_SUMMARY_AUTO, &error),
      CAI_OK);
  expect_int(
      state, "params_set_parallel_tools",
      cai_response_create_params_set_parallel_tool_calls(params, 0, &error),
      CAI_OK);
  expect_int(
      state, "params_set_bad_parallel_tools",
      cai_response_create_params_set_parallel_tool_calls(params, 2, &error),
      CAI_ERR_INVALID);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  expect_int(state, "params_set_text_format",
             cai_response_create_params_set_text_format_json_schema(
                 params, "answer", "Answer payload",
                 "{\"type\":\"object\",\"properties\":{\"answer\":{\"type\":"
                 "\"string\"}},\"required\":[\"answer\"],"
                 "\"additionalProperties\":false}",
                 1, &error),
             CAI_OK);
  expect_int(
      state, "params_set_bad_max_output_tokens",
      cai_response_create_params_set_max_output_tokens(params, -1, &error),
      CAI_ERR_INVALID);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  expect_int(
      state, "params_add_text",
      cai_response_create_params_add_text(params, "user", "hello", &error),
      CAI_OK);
  expect_int(
      state, "params_add_image",
      cai_response_create_params_add_image_url(
          params, "user", "https://example.test/image.png", "high", &error),
      CAI_OK);
  expect_int(state, "params_add_tool",
             cai_response_create_params_add_function_tool(
                 params, "get_weather", "Get weather",
                 "{\"type\":\"object\",\"properties\":{\"city\":{\"type\":"
                 "\"string\"}},\"required\":[\"city\"]}",
                 1, &error),
             CAI_OK);
  expect_int(state, "params_serialize",
             cai_response_create_params_serialize_json(params, &json, &json_len,
                                                       &error),
             CAI_OK);
  if (json == NULL) {
    test_fail(state, "params_serialize", "no JSON returned");
  } else {
    if (strstr(json, "\"model\":\"gpt-5.4-nano\"") == NULL) {
      test_fail(state, "params_serialize", "model missing from JSON");
    }
    if (strstr(json, "\"conversation\":\"conv_123\"") == NULL) {
      test_fail(state, "params_serialize", "conversation missing from JSON");
    }
    if (strstr(json, "\"max_output_tokens\":128") == NULL) {
      test_fail(state, "params_serialize",
                "max output tokens missing from JSON");
    }
    if (strstr(json,
               "\"reasoning\":{\"effort\":\"low\",\"summary\":\"auto\"}") ==
        NULL) {
      test_fail(state, "params_serialize", "reasoning missing from JSON");
    }
    if (strstr(json, "\"parallel_tool_calls\":false") == NULL) {
      test_fail(state, "params_serialize",
                "parallel tool calls missing from JSON");
    }
    if (strstr(json, "\"text\":{\"format\":{\"type\":\"json_schema\"") ==
            NULL ||
        strstr(json, "\"name\":\"answer\"") == NULL ||
        strstr(json, "\"description\":\"Answer payload\"") == NULL ||
        strstr(json, "\"schema\":{\"type\":\"object\"") == NULL ||
        strstr(json, "\"strict\":true") == NULL) {
      test_fail(state, "params_serialize", "text format missing from JSON");
    }
    if (strstr(json, "\"type\":\"input_text\"") == NULL) {
      test_fail(state, "params_serialize", "text content missing from JSON");
    }
    if (strstr(json, "\"type\":\"input_image\"") == NULL) {
      test_fail(state, "params_serialize", "image content missing from JSON");
    }
    if (strstr(json, "\"tools\":[") == NULL ||
        strstr(json, "\"name\":\"get_weather\"") == NULL ||
        strstr(json, "\"strict\":true") == NULL ||
        strstr(json, "\"parameters\":{\"type\":\"object\"") == NULL) {
      test_fail(state, "params_serialize", "function tool missing from JSON");
    }
    if (strstr(json, ":null") != NULL) {
      test_fail(state, "params_serialize", "unexpected null field in JSON");
    }
    expect_int(state, "params_serialize_len", (long)strlen(json),
               (long)json_len);
    free(json);
    json = NULL;
  }
  cai_response_create_params_destroy(params);
  params = NULL;

  expect_int(state, "json_object_params_new",
             cai_response_create_params_new(&params, &error), CAI_OK);
  expect_int(state, "json_object_params_model",
             cai_response_create_params_set_model(
                 params, CAI_MODEL_GPT_5_4_NANO, &error),
             CAI_OK);
  expect_int(
      state, "json_object_params_format",
      cai_response_create_params_set_text_format_json_object(params, &error),
      CAI_OK);
  expect_int(
      state, "json_object_params_text",
      cai_response_create_params_add_text(params, "user", "JSON only", &error),
      CAI_OK);
  expect_int(state, "json_object_params_serialize",
             cai_response_create_params_serialize_json(params, &json, &json_len,
                                                       &error),
             CAI_OK);
  if (json == NULL) {
    test_fail(state, "json_object_params_serialize", "no JSON returned");
  } else {
    if (strstr(json, "\"text\":{\"format\":{\"type\":\"json_object\"}}") ==
        NULL) {
      test_fail(state, "json_object_params_serialize",
                "JSON object format missing");
    }
    free(json);
    json = NULL;
  }
  cai_response_create_params_destroy(params);

  response = NULL;
  expect_int(state, "response_parse",
             cai_response_parse_json(response_json, &response, &error), CAI_OK);
  expect_str(state, "response_id", cai_response_id(response), "resp_123");
  expect_str(state, "response_status", cai_response_status(response),
             "completed");
  expect_str(state, "response_model", cai_response_model(response),
             CAI_MODEL_GPT_5_4_NANO);
  expect_str(state, "response_conversation",
             cai_response_conversation_id(response), "conv_123");
  expect_int(state, "response_created_at",
             (long)cai_response_created_at(response), 123L);
  expect_str(state, "response_text", cai_response_output_text(response),
             "hello world");
  expect_str(state, "response_refusal", cai_response_refusal(response),
             "cannot comply");
  writer.length = 0U;
  writer.closed = 0;
  writer.buffer[0] = '\0';
  sink_callbacks.write = test_write;
  sink_callbacks.close = test_write_close;
  sink_callbacks.context = &writer;
  sink = NULL;
  expect_int(state, "response_sink_create",
             cai_sink_from_callbacks(&sink_callbacks, &sink, &error), CAI_OK);
  expect_int(state, "response_write_text",
             cai_response_write_output_text(response, sink, &error), CAI_OK);
  expect_str(state, "response_written_text", writer.buffer, "hello world");
  cai_sink_close(sink);
  writer.length = 0U;
  writer.closed = 0;
  writer.buffer[0] = '\0';
  sink = NULL;
  expect_int(state, "response_refusal_sink_create",
             cai_sink_from_callbacks(&sink_callbacks, &sink, &error), CAI_OK);
  expect_int(state, "response_write_refusal",
             cai_response_write_refusal(response, sink, &error), CAI_OK);
  expect_str(state, "response_written_refusal", writer.buffer, "cannot comply");
  cai_sink_close(sink);
  expect_str(state, "response_error_code", cai_response_error_code(response),
             "rate_limited");
  expect_str(state, "response_error_message",
             cai_response_error_message(response), "slow down");
  expect_str(state, "response_incomplete_reason",
             cai_response_incomplete_reason(response), "max_output_tokens");
  expect_int(state, "response_input_tokens",
             cai_response_input_tokens(response), 11L);
  expect_int(state, "response_input_cached_tokens",
             cai_response_input_cached_tokens(response), 5L);
  expect_int(state, "response_output_tokens",
             cai_response_output_tokens(response), 7L);
  expect_int(state, "response_output_reasoning_tokens",
             cai_response_output_reasoning_tokens(response), 3L);
  expect_int(state, "response_total_tokens",
             cai_response_total_tokens(response), 18L);
  expect_int(state, "response_usage",
             cai_response_usage(response, &usage, &error), CAI_OK);
  expect_int(state, "response_usage_cached", usage.input_cached_tokens, 5L);
  expect_int(state, "response_usage_reasoning", usage.output_reasoning_tokens,
             3L);
  expect_int(state, "response_tool_count",
             (long)cai_response_tool_call_count(response), 1L);
  expect_str(state, "response_tool_id", cai_response_tool_call_id(response, 0U),
             "call_1");
  expect_str(state, "response_tool_name",
             cai_response_tool_call_name(response, 0U), "weather");
  expect_str(state, "response_tool_arguments",
             cai_response_tool_call_arguments(response, 0U),
             "{\"city\":\"Malmo\"}");
  if (strstr(cai_response_raw_json(response), "\"id\":\"resp_123\"") == NULL) {
    test_fail(state, "response_raw_json", "raw JSON missing response id");
  }
  expect_int(state, "output_from_response",
             cai_output_from_response(response, &output, &error), CAI_OK);
  response = NULL;
  expect_str(state, "output_text", cai_output_text(output), "hello world");
  expect_str(state, "output_refusal", cai_output_refusal(output),
             "cannot comply");
  if (cai_output_response(output) == NULL) {
    test_fail(state, "output_response", "response not retained");
  }
  if (strstr(cai_output_raw_json(output), "\"id\":\"resp_123\"") == NULL) {
    test_fail(state, "output_raw_json", "raw JSON missing response id");
  }
  writer.length = 0U;
  writer.closed = 0;
  writer.buffer[0] = '\0';
  sink = NULL;
  expect_int(state, "output_sink_create",
             cai_sink_from_callbacks(&sink_callbacks, &sink, &error), CAI_OK);
  expect_int(state, "output_write_text",
             cai_output_write_text(output, sink, &error), CAI_OK);
  expect_str(state, "output_written_text", writer.buffer, "hello world");
  cai_sink_close(sink);
  cai_output_destroy(output);
  cai_response_destroy(response);
  cai_error_cleanup(&error);
}

static void test_response_spooled_request_fragments(test_state *state) {
  cai_response_create_params *params;
  lonejson_spooled raw_items;
  lonejson_spooled file_data;
  lonejson_spooled tool_file_data;
  lonejson_spooled request_json;
  lonejson_spooled output_items;
  cai_response *response;
  cai_error error;
  lonejson_error json_error;
  char *json;
  size_t json_len;

  static const char raw_fragment[] =
      "{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":"
      "\"output_text\",\"text\":\"remembered\"}]}";
  static const char response_json[] =
      "{\"id\":\"resp_spooled_items\",\"status\":\"completed\",\"output\":["
      "{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":"
      "\"output_text\",\"text\":\"spooled\"}]}],\"usage\":{\"input_tokens\":1,"
      "\"output_tokens\":1,\"total_tokens\":2}}";

  cai_error_init(&error);
  params = NULL;
  response = NULL;
  json = NULL;
  expect_int(state, "spooled_params_new",
             cai_response_create_params_new(&params, &error), CAI_OK);
  expect_int(state, "spooled_params_model",
             cai_response_create_params_set_model(
                 params, CAI_MODEL_GPT_5_4_NANO, &error),
             CAI_OK);
  lonejson_error_init(&json_error);
  lonejson_spooled_init(&raw_items, NULL);
  expect_int(state, "spooled_raw_append",
             lonejson_spooled_append(&raw_items, raw_fragment,
                                     strlen(raw_fragment), &json_error),
             LONEJSON_STATUS_OK);
  expect_int(state, "spooled_raw_set",
             cai_response_create_params_set_raw_input_spooled(params, &raw_items,
                                                              &error),
             CAI_OK);
  expect_int(state, "spooled_typed_add",
             cai_response_create_params_add_text(params, "user", "next",
                                                 &error),
             CAI_OK);
  lonejson_spooled_init(&file_data, NULL);
  expect_int(state, "spooled_file_append",
             lonejson_spooled_append(&file_data, "inline file text",
                                     strlen("inline file text"), &json_error),
             LONEJSON_STATUS_OK);
  expect_int(state, "spooled_file_add",
             cai_response_create_params_add_file_data_spooled(
                 params, "user", "note.txt", &file_data, "low", &error),
             CAI_OK);
  expect_int(state, "spooled_file_id_add",
             cai_response_create_params_add_file_id(params, "user",
                                                    "file_input_123", "high",
                                                    &error),
             CAI_OK);
  expect_int(state, "spooled_tool_text_add",
             cai_response_create_params_add_function_call_output_text(
                 params, "call_content_1", "tool text", &error),
             CAI_OK);
  lonejson_spooled_init(&tool_file_data, NULL);
  expect_int(state, "spooled_tool_file_append",
             lonejson_spooled_append(&tool_file_data, "tool file text",
                                     strlen("tool file text"), &json_error),
             LONEJSON_STATUS_OK);
  expect_int(state, "spooled_tool_file_add",
             cai_response_create_params_add_function_call_output_file_data_spooled(
                 params, "call_content_2", "tool.txt", &tool_file_data, "low",
                 &error),
             CAI_OK);
  expect_int(state, "spooled_request_json",
             cai_response_create_params_spool_json(params, 0, &request_json,
                                                   &json_len, &error),
             CAI_OK);
  json = test_spooled_to_cstr(&request_json);
  if (json == NULL) {
    test_fail(state, "spooled_request_json", "failed to read request spool");
  } else {
    if (strstr(json, "\"input\":[{\"type\":\"message\"") == NULL ||
        strstr(json, "\"text\":\"remembered\"") == NULL ||
        strstr(json, "\"role\":\"user\"") == NULL ||
        strstr(json, "\"text\":\"next\"") == NULL ||
        strstr(json, "\"filename\":\"note.txt\"") == NULL ||
        strstr(json, "\"file_data\":\"inline file text\"") == NULL ||
        strstr(json, "\"file_id\":\"file_input_123\"") == NULL ||
        strstr(json, "\"output\":[{\"type\":\"input_text\",\"text\":\"tool text\"}]") ==
            NULL ||
        strstr(json, "\"filename\":\"tool.txt\"") == NULL ||
        strstr(json, "\"file_data\":\"tool file text\"") == NULL ||
        strstr(json, "\"text\":\"remembered\"") >
            strstr(json, "\"text\":\"next\"")) {
      test_fail(state, "spooled_request_json", "request did not merge fragments");
    }
    expect_int(state, "spooled_request_len", (long)strlen(json),
               (long)json_len);
    free(json);
  }
  lonejson_spooled_cleanup(&request_json);
  cai_response_create_params_destroy(params);

  expect_int(state, "spooled_response_parse",
             cai_response_parse_json(response_json, &response, &error), CAI_OK);
  expect_int(state, "spooled_output_items",
             cai_response_output_items_spool(response, &output_items, &json_len,
                                             &error),
             CAI_OK);
  json = test_spooled_to_cstr(&output_items);
  if (json == NULL) {
    test_fail(state, "spooled_output_items", "failed to read output spool");
  } else {
    if (json[0] == '[' || strstr(json, "\"text\":\"spooled\"") == NULL) {
      test_fail(state, "spooled_output_items",
                "output items were not spooled as array items");
    }
    expect_int(state, "spooled_output_len", (long)strlen(json), (long)json_len);
    free(json);
  }
  lonejson_spooled_cleanup(&output_items);
  cai_response_destroy(response);
  cai_error_cleanup(&error);
}

static int mock_write_all(int fd, const char *data, size_t length) {
  size_t offset;
  ssize_t written;

  offset = 0U;
  while (offset < length) {
    written = write(fd, data + offset, length - offset);
    if (written <= 0) {
      return -1;
    }
    offset += (size_t)written;
  }
  return 0;
}

static int mock_read_request(int fd, char *request, size_t capacity) {
  ssize_t nread;

  nread = read(fd, request, capacity - 1U);
  if (nread <= 0) {
    return -1;
  }
  request[nread] = '\0';
  return 0;
}

static int mock_write_status_json_response(int fd, int status,
                                           const char *status_text,
                                           const char *request_id,
                                           const char *body) {
  char response[1024];
  int response_len;

  response_len = snprintf(
      response, sizeof(response),
      "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
      "%s%s%s"
      "Content-Length: %lu\r\nConnection: close\r\n\r\n%s",
      status, status_text, request_id != NULL ? "x-request-id: " : "",
      request_id != NULL ? request_id : "", request_id != NULL ? "\r\n" : "",
      (unsigned long)strlen(body), body);
  if (response_len <= 0 || (size_t)response_len >= sizeof(response)) {
    return -1;
  }
  return mock_write_all(fd, response, (size_t)response_len);
}

static int mock_write_json_response(int fd, const char *body) {
  return mock_write_status_json_response(fd, 200, "OK", NULL, body);
}

static const char *mock_response_for_request(const char *request) {
  static const char create_body[] =
      "{\"id\":\"resp_mock\",\"status\":\"completed\",\"output\":[{\"type\":"
      "\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"mock "
      "ok\"}]}]}";
  static const char retrieve_body[] =
      "{\"id\":\"resp_get\",\"status\":\"completed\",\"output\":[{\"type\":"
      "\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"get "
      "ok\"}]}]}";
  static const char cancel_body[] =
      "{\"id\":\"resp_cancel\",\"status\":\"cancelled\",\"output\":[{\"type\":"
      "\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"cancel "
      "ok\"}]}]}";
  static const char delete_body[] = "{\"deleted\":true,\"id\":\"resp_get\"}";
  static const char input_items_body[] =
      "{\"object\":\"list\",\"data\":[{\"id\":\"msg_1\",\"type\":\"message\","
      "\"role\":\"user\"},{\"id\":\"msg_2\",\"type\":\"message\",\"role\":"
      "\"assistant\"}],\"first_id\":\"msg_1\",\"last_id\":\"msg_2\","
      "\"has_more\":true}";
  static const char conversation_create_body[] =
      "{\"id\":\"conv_mock\",\"object\":\"conversation\",\"created_at\":1}";
  static const char conversation_get_body[] =
      "{\"id\":\"conv_get\",\"object\":\"conversation\",\"created_at\":1}";
  static const char conversation_update_body[] =
      "{\"id\":\"conv_get\",\"object\":\"conversation\",\"created_at\":1}";
  static const char conversation_delete_body[] =
      "{\"id\":\"conv_get\",\"object\":\"conversation.deleted\","
      "\"deleted\":true}";
  static const char conversation_items_body[] =
      "{\"object\":\"list\",\"data\":[{\"id\":\"conv_msg_1\",\"type\":"
      "\"message\",\"role\":\"user\"}],\"first_id\":\"conv_msg_1\","
      "\"last_id\":\"conv_msg_1\",\"has_more\":false}";
  static const char conversation_items_create_body[] =
      "{\"object\":\"list\",\"data\":[{\"id\":\"conv_msg_new\",\"type\":"
      "\"message\",\"role\":\"user\"}],\"first_id\":\"conv_msg_new\","
      "\"last_id\":\"conv_msg_new\",\"has_more\":false}";
  static const char conversation_item_delete_body[] =
      "{\"id\":\"conv_msg_1\",\"object\":\"conversation.item.deleted\","
      "\"deleted\":true}";
  static const char conversation_item_retrieve_body[] =
      "{\"id\":\"conv_msg_1\",\"type\":\"message\",\"role\":\"user\"}";
  static const char session_first_body[] =
      "{\"id\":\"resp_session_1\",\"status\":\"completed\",\"output\":[{"
      "\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":"
      "\"first turn\"}]}]}";
  static const char session_second_body[] =
      "{\"id\":\"resp_session_2\",\"status\":\"completed\",\"output\":[{"
      "\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":"
      "\"second turn\"}]}]}";
  static const char session_third_body[] =
      "{\"id\":\"resp_session_3\",\"status\":\"completed\",\"output\":[{"
      "\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":"
      "\"third turn\"}]}]}";
  static const char session_image_body[] =
      "{\"id\":\"resp_session_img\",\"status\":\"completed\",\"output\":[{"
      "\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":"
      "\"image turn\"}]}]}";
  static const char session_conversation_body[] =
      "{\"id\":\"resp_session_conv\",\"status\":\"completed\","
      "\"conversation\":{\"id\":\"conv_session\"},\"output\":[{"
      "\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":"
      "\"conversation turn\"}]}]}";
  static const char session_auto_conversation_body[] =
      "{\"id\":\"resp_session_auto_conv\",\"status\":\"completed\","
      "\"conversation\":{\"id\":\"conv_mock\"},\"output\":[{"
      "\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":"
      "\"auto conversation turn\"}]}]}";
  static const char compact_first_body[] =
      "{\"id\":\"resp_compact_1\",\"status\":\"completed\",\"output\":[{"
      "\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":"
      "\"before compact\"}]}],\"usage\":{\"input_tokens\":320000,"
      "\"output_tokens\":1000,\"total_tokens\":321000}}";
  static const char compact_body[] =
      "{\"id\":\"resp_compact_summary\",\"status\":\"completed\",\"output\":[{"
      "\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":"
      "\"compacted summary\"}]}],\"usage\":{\"input_tokens\":100,"
      "\"output_tokens\":20,\"total_tokens\":120}}";
  static const char compact_second_body[] =
      "{\"id\":\"resp_compact_2\",\"status\":\"completed\",\"output\":[{"
      "\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":"
      "\"after compact\"}]}],\"usage\":{\"input_tokens\":200,"
      "\"output_tokens\":30,\"total_tokens\":230}}";
  static const char agent_tool_body[] =
      "{\"id\":\"resp_agent_tool\",\"status\":\"completed\",\"output\":[{"
      "\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":"
      "\"tool ready\"}]}]}";
  static const char auto_tool_call_body[] =
      "{\"id\":\"resp_auto_tool_1\",\"status\":\"completed\",\"output\":[{"
      "\"id\":\"fc_auto_1\",\"type\":\"function_call\",\"call_id\":"
      "\"call_auto_1\",\"name\":\"raw_echo\",\"arguments\":\"{\\\"x\\\":1}\""
      "}]}";
  static const char auto_tool_done_body[] =
      "{\"id\":\"resp_auto_tool_2\",\"status\":\"completed\",\"output\":[{"
      "\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":"
      "\"auto done\"}]}]}";
  static const char manual_tool_call_body[] =
      "{\"id\":\"resp_manual_tool_1\",\"status\":\"completed\",\"output\":[{"
      "\"id\":\"fc_manual_1\",\"type\":\"function_call\",\"call_id\":"
      "\"call_manual_1\",\"name\":\"raw_echo\",\"arguments\":\"{\\\"y\\\":2}\""
      "}]}";
  static const char manual_tool_done_body[] =
      "{\"id\":\"resp_manual_tool_2\",\"status\":\"completed\",\"output\":[{"
      "\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":"
      "\"manual done\"}]}]}";
  static const char stream_body[] =
      "data: {\"type\":\"response.output_text.delta\",\"delta\":\"hel\"}\n\n"
      "data: {\"type\":\"response.output_text.delta\",\"delta\":\"lo\"}\n\n"
      "data: {\"type\":\"response.completed\",\"response\":{\"id\":"
      "\"resp_stream_raw\",\"usage\":{\"input_tokens\":1,"
      "\"input_tokens_details\":{\"cached_tokens\":0},\"output_tokens\":2,"
      "\"output_tokens_details\":{\"reasoning_tokens\":0},"
      "\"total_tokens\":3}}}\n\n";
  static const char stream_session_first_body[] =
      "data: {\"type\":\"response.output_text.delta\",\"delta\":\"one\"}\n\n"
      "data: {\"type\":\"response.completed\",\"response\":{\"id\":"
      "\"resp_stream_session_1\",\"usage\":{\"input_tokens\":10,"
      "\"input_tokens_details\":{\"cached_tokens\":4},\"output_tokens\":3,"
      "\"output_tokens_details\":{\"reasoning_tokens\":1},"
      "\"total_tokens\":13}}}\n\n";
  static const char stream_session_second_body[] =
      "data: {\"type\":\"response.output_text.delta\",\"delta\":\"two\"}\n\n"
      "data: {\"type\":\"response.completed\",\"response\":{\"id\":"
      "\"resp_stream_session_2\",\"usage\":{\"input_tokens\":20,"
      "\"input_tokens_details\":{\"cached_tokens\":8},\"output_tokens\":4,"
      "\"output_tokens_details\":{\"reasoning_tokens\":2},"
      "\"total_tokens\":24}}}\n\n";
  static const char stream_session_source_first_body[] =
      "data: {\"type\":\"response.output_text.delta\",\"delta\":\"src1\"}\n\n"
      "data: {\"type\":\"response.completed\",\"response\":{\"id\":"
      "\"resp_stream_source_session_1\",\"usage\":{\"input_tokens\":30,"
      "\"input_tokens_details\":{\"cached_tokens\":12},\"output_tokens\":5,"
      "\"output_tokens_details\":{\"reasoning_tokens\":3},"
      "\"total_tokens\":35}}}\n\n";
  static const char stream_session_source_second_body[] =
      "data: {\"type\":\"response.output_text.delta\",\"delta\":\"src2\"}\n\n"
      "data: {\"type\":\"response.completed\",\"response\":{\"id\":"
      "\"resp_stream_source_session_2\",\"usage\":{\"input_tokens\":40,"
      "\"input_tokens_details\":{\"cached_tokens\":16},\"output_tokens\":6,"
      "\"output_tokens_details\":{\"reasoning_tokens\":4},"
      "\"total_tokens\":46}}}\n\n";
  static const char stream_history_first_body[] =
      "data: {\"type\":\"response.output_text.delta\",\"delta\":\"hist1\"}\n\n"
      "data: {\"type\":\"response.completed\",\"response\":{\"id\":"
      "\"resp_stream_history_1\",\"usage\":{\"input_tokens\":10,"
      "\"output_tokens\":2,\"total_tokens\":12}}}\n\n";
  static const char stream_history_second_body[] =
      "data: {\"type\":\"response.output_text.delta\",\"delta\":\"hist2\"}\n\n"
      "data: {\"type\":\"response.completed\",\"response\":{\"id\":"
      "\"resp_stream_history_2\",\"usage\":{\"input_tokens\":20,"
      "\"output_tokens\":3,\"total_tokens\":23}}}\n\n";
  static const char stream_history_first_retrieve_body[] =
      "{\n"
      "  \"id\": \"resp_stream_history_1\",\n"
      "  \"status\": \"completed\",\n"
      "  \"output\": [\n"
      "    {\n"
      "      \"type\": \"message\",\n"
      "      \"role\": \"assistant\",\n"
      "      \"content\": [\n"
      "        {\n"
      "          \"type\": \"output_text\",\n"
      "          \"text\": \"history stream answer\"\n"
      "        }\n"
      "      ]\n"
      "    }\n"
      "  ]\n"
      "}";
  static const char stream_history_second_retrieve_body[] =
      "{\"id\":\"resp_stream_history_2\",\"status\":\"completed\","
      "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{"
      "\"type\":\"output_text\",\"text\":\"history stream second answer\"}]}]}";

  if (strstr(request, "POST /v1/responses/compact HTTP/") != NULL) {
    if (strstr(request, "compact first") != NULL &&
        strstr(request, "before compact") != NULL) {
      return compact_body;
    }
    return NULL;
  }
  if (strncmp(request, "POST /v1/responses HTTP/", 24U) == 0) {
    if (strstr(request, "\"stream\":true") != NULL) {
      if (strstr(request, "session stream one") != NULL &&
          strstr(request, "previous_response_id") == NULL) {
        return stream_session_first_body;
      }
      if (strstr(request, "session stream two") != NULL &&
          strstr(request,
                 "\"previous_response_id\":\"resp_stream_session_1\"") !=
              NULL) {
        return stream_session_second_body;
      }
      if (strstr(request, "session source one") != NULL &&
          strstr(request,
                 "\"previous_response_id\":\"resp_stream_session_2\"") !=
              NULL) {
        return stream_session_source_first_body;
      }
      if (strstr(request, "session source two") != NULL &&
          strstr(request, "\"previous_response_id\":"
                          "\"resp_stream_source_session_1\"") != NULL) {
        return stream_session_source_second_body;
      }
      if (strstr(request, "history stream second") != NULL &&
          strstr(request, "history stream answer") != NULL &&
          strstr(request, "previous_response_id") == NULL) {
        return stream_history_second_body;
      }
      if (strstr(request, "history stream first") != NULL &&
          strstr(request, "previous_response_id") == NULL) {
        return stream_history_first_body;
      }
      return stream_body;
    }
    if (strstr(request, "manual tool turn") != NULL &&
        strstr(request, "\"name\":\"raw_echo\"") != NULL) {
      return manual_tool_call_body;
    }
    if (strstr(request, "\"type\":\"function_call_output\"") != NULL &&
        strstr(request, "\"call_id\":\"call_manual_1\"") != NULL &&
        strstr(request, "\"output\":\"{\\\"y\\\":2}\"") != NULL &&
        strstr(request, "\"previous_response_id\":\"resp_manual_tool_1\"") !=
            NULL) {
      return manual_tool_done_body;
    }
    if (strstr(request, "auto tool turn") != NULL &&
        strstr(request, "\"name\":\"raw_echo\"") != NULL) {
      return auto_tool_call_body;
    }
    if (strstr(request, "\"type\":\"function_call_output\"") != NULL &&
        strstr(request, "\"call_id\":\"call_auto_1\"") != NULL &&
        strstr(request, "\"output\":\"{\\\"x\\\":1}\"") != NULL &&
        strstr(request, "\"previous_response_id\":\"resp_auto_tool_1\"") !=
            NULL) {
      return auto_tool_done_body;
    }
    if (strstr(request, "agent tool turn") != NULL &&
        strstr(request, "\"tools\":[") != NULL &&
        strstr(request, "\"name\":\"raw_echo\"") != NULL &&
        strstr(request, "\"parameters\":{\"type\":\"object\"") != NULL) {
      return agent_tool_body;
    }
    if (strstr(request, "session first") != NULL &&
        strstr(request, "\"max_output_tokens\":64") != NULL &&
        strstr(request,
               "\"reasoning\":{\"effort\":\"medium\",\"summary\":\"auto\"}") !=
            NULL &&
        strstr(request, "\"text\":{\"format\":{\"type\":\"json_schema\"") !=
            NULL &&
        strstr(request, "\"parallel_tool_calls\":false") != NULL &&
        strstr(request, "previous_response_id") == NULL) {
      return session_first_body;
    }
    if (strstr(request, "session second") != NULL &&
        strstr(request, "\"previous_response_id\":\"resp_session_1\"") !=
            NULL) {
      return session_second_body;
    }
    if (strstr(request, "incremental turn") != NULL &&
        strstr(request, "\"role\":\"developer\"") != NULL &&
        strstr(request, "\"previous_response_id\":\"resp_session_2\"") !=
            NULL) {
      return session_third_body;
    }
    if (strstr(request, "\"type\":\"input_image\"") != NULL &&
        strstr(request, "https://example.test/session.png") != NULL &&
        strstr(request, "\"detail\":\"high\"") != NULL &&
        strstr(request, "\"previous_response_id\":\"resp_session_3\"") !=
            NULL) {
      return session_image_body;
    }
    if (strstr(request, "conversation turn") != NULL &&
        strstr(request, "\"conversation\":\"conv_session\"") != NULL &&
        strstr(request, "previous_response_id") == NULL) {
      return session_conversation_body;
    }
    if (strstr(request, "auto conversation turn") != NULL &&
        strstr(request, "\"conversation\":\"conv_mock\"") != NULL &&
        strstr(request, "previous_response_id") == NULL) {
      return session_auto_conversation_body;
    }
    if (strstr(request, "compact first") != NULL &&
        strstr(request, "previous_response_id") == NULL) {
      return compact_first_body;
    }
    if (strstr(request, "compact second") != NULL &&
        strstr(request, "compacted summary") != NULL &&
        strstr(request, "previous_response_id") == NULL) {
      return compact_second_body;
    }
    return create_body;
  }
  if (strncmp(request, "GET /v1/responses/resp_get HTTP/", 32U) == 0) {
    return retrieve_body;
  }
  if (strstr(request, "GET /v1/responses/resp_stream_history_1 HTTP/") !=
      NULL) {
    return stream_history_first_retrieve_body;
  }
  if (strstr(request, "GET /v1/responses/resp_stream_history_2 HTTP/") !=
      NULL) {
    return stream_history_second_retrieve_body;
  }
  if (strncmp(request, "POST /v1/responses/resp_get/cancel HTTP/", 40U) == 0) {
    return cancel_body;
  }
  if (strncmp(request, "DELETE /v1/responses/resp_get HTTP/", 35U) == 0) {
    return delete_body;
  }
  if (strstr(request, "GET /v1/responses/resp_get/input_items?") != NULL &&
      strstr(request, "after=msg_0%20x") != NULL &&
      strstr(request, "limit=2") != NULL &&
      strstr(request, "order=asc") != NULL) {
    return input_items_body;
  }
  if (strncmp(request, "POST /v1/conversations HTTP/", 28U) == 0) {
    return conversation_create_body;
  }
  if (strncmp(request, "GET /v1/conversations/conv_get HTTP/", 36U) == 0) {
    return conversation_get_body;
  }
  if (strncmp(request, "POST /v1/conversations/conv_get HTTP/", 37U) == 0 &&
      strstr(request, "\"metadata\":{\"tenant\":\"vectis\"}") != NULL) {
    return conversation_update_body;
  }
	  if (strstr(request, "POST /v1/conversations/conv_get/items HTTP/") != NULL &&
	      strstr(request, "\"items\":[") != NULL &&
	      strstr(request, "\"type\":\"input_text\"") != NULL &&
	      strstr(request, "\"text\":\"conversation item\"") != NULL &&
	      strstr(request, "\"type\":\"input_image\"") != NULL &&
	      strstr(request, "https://example.test/conv.png") != NULL &&
	      strstr(request, "\"type\":\"input_file\"") != NULL &&
	      strstr(request, "\"file_data\":\"conversation file text\"") != NULL) {
	    return conversation_items_create_body;
	  }
  if (strstr(request, "GET /v1/conversations/conv_get/items?") != NULL &&
      strstr(request, "limit=1") != NULL &&
      strstr(request, "order=desc") != NULL) {
    return conversation_items_body;
  }
  if (strstr(request,
             "GET /v1/conversations/conv_get/items/conv_msg_1 HTTP/") != NULL) {
    return conversation_item_retrieve_body;
  }
  if (strstr(request,
             "DELETE /v1/conversations/conv_get/items/conv_msg_1 HTTP/") !=
      NULL) {
    return conversation_item_delete_body;
  }
  if (strncmp(request, "DELETE /v1/conversations/conv_get HTTP/", 39U) == 0) {
    return conversation_delete_body;
  }
  return NULL;
}

static void mock_openai_child(int pipe_fd, int request_count) {
  char request[4096];
  struct sockaddr_in addr;
  socklen_t addr_len;
  int server_fd;
  int client_fd;
  int port;
  int i;
  const char *body;

  server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    _exit(2);
  }
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    _exit(3);
  }
  if (listen(server_fd, 1) != 0) {
    _exit(4);
  }
  addr_len = (socklen_t)sizeof(addr);
  if (getsockname(server_fd, (struct sockaddr *)&addr, &addr_len) != 0) {
    _exit(5);
  }
  port = (int)ntohs(addr.sin_port);
  if (write(pipe_fd, &port, sizeof(port)) != (ssize_t)sizeof(port)) {
    _exit(6);
  }
  close(pipe_fd);
  for (i = 0; i < request_count; i++) {
    client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
      _exit(7);
    }
    if (mock_read_request(client_fd, request, sizeof(request)) != 0) {
      _exit(8);
    }
    if (strstr(request, "\"text\":\"hello\"") != NULL &&
        (strstr(request, "OpenAI-Organization: org_mock") == NULL ||
         strstr(request, "OpenAI-Project: proj_mock") == NULL)) {
      _exit(11);
    }
    if (strstr(request, "GET /v1/responses/resp_error HTTP/") != NULL) {
      if (mock_write_status_json_response(
              client_fd, 400, "Bad Request", "req_mock_error",
              "{\"error\":{\"message\":\"model is required\",\"type\":"
              "\"invalid_request_error\",\"code\":\"missing_required_"
              "parameter\"}}") != 0) {
        _exit(10);
      }
      close(client_fd);
      continue;
    }
    body = mock_response_for_request(request);
    if (body == NULL) {
      _exit(9);
    }
    if (mock_write_json_response(client_fd, body) != 0) {
      _exit(10);
    }
    close(client_fd);
  }
  close(server_fd);
  _exit(0);
}

static void test_http_create_response(test_state *state) {
  int pipe_fds[2];
  pid_t pid;
  int port;
  ssize_t nread;
  int child_status;
  char base_url[128];
  cai_client_config config;
  cai_client *client;
  cai_response_create_params *params;
  cai_response *response;
  cai_input_item_list *items;
  cai_list_params list_params;
  cai_error error;

  if (pipe(pipe_fds) != 0) {
    test_fail(state, "http_mock", "pipe failed");
    return;
  }
  pid = fork();
  if (pid < 0) {
    test_fail(state, "http_mock", "fork failed");
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return;
  }
  if (pid == 0) {
    close(pipe_fds[0]);
    mock_openai_child(pipe_fds[1], 5);
  }
  close(pipe_fds[1]);
  nread = read(pipe_fds[0], &port, sizeof(port));
  close(pipe_fds[0]);
  if (nread != (ssize_t)sizeof(port)) {
    test_fail(state, "http_mock", "failed to read mock port");
    waitpid(pid, &child_status, 0);
    return;
  }

  cai_error_init(&error);
  snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);
  cai_client_config_init(&config);
  config.api_key = "mock-key";
  config.base_url = base_url;
  config.organization_id = "org_mock";
  config.project_id = "proj_mock";
  config.http_2_disabled = 1;
  config.timeout_ms = 5000L;
  client = NULL;
  params = NULL;
  response = NULL;
  items = NULL;
  expect_int(state, "http_client_open",
             cai_client_open(&config, &client, &error), CAI_OK);
  expect_int(state, "http_params_new",
             cai_response_create_params_new(&params, &error), CAI_OK);
  expect_int(state, "http_params_model",
             cai_response_create_params_set_model(
                 params, CAI_MODEL_GPT_5_4_NANO, &error),
             CAI_OK);
  expect_int(
      state, "http_params_input",
      cai_response_create_params_add_text(params, "user", "hello", &error),
      CAI_OK);
  expect_int(state, "http_create",
             cai_client_create_response(client, params, &response, &error),
             CAI_OK);
  expect_str(state, "http_response_id", cai_response_id(response), "resp_mock");
  expect_str(state, "http_response_text", cai_response_output_text(response),
             "mock ok");
  cai_response_destroy(response);
  response = NULL;
  expect_int(
      state, "http_retrieve",
      cai_client_retrieve_response(client, "resp_get", &response, &error),
      CAI_OK);
  expect_str(state, "http_retrieve_id", cai_response_id(response), "resp_get");
  expect_str(state, "http_retrieve_text", cai_response_output_text(response),
             "get ok");
  cai_response_destroy(response);
  response = NULL;
  expect_int(state, "http_cancel",
             cai_client_cancel_response(client, "resp_get", &response, &error),
             CAI_OK);
  expect_str(state, "http_cancel_status", cai_response_status(response),
             "cancelled");
  expect_str(state, "http_cancel_text", cai_response_output_text(response),
             "cancel ok");
  cai_response_destroy(response);
  expect_int(state, "http_delete",
             cai_client_delete_response(client, "resp_get", &error), CAI_OK);
  cai_list_params_init(&list_params);
  list_params.after = "msg_0 x";
  list_params.limit = 2;
  list_params.order = "asc";
  expect_int(state, "http_list_input_items",
             cai_client_list_response_input_items(client, "resp_get",
                                                  &list_params, &items, &error),
             CAI_OK);
  expect_int(state, "http_list_input_items_count",
             (long)cai_input_item_list_count(items), 2L);
  expect_int(state, "http_list_input_items_more",
             cai_input_item_list_has_more(items), 1L);
  expect_str(state, "http_list_input_items_first",
             cai_input_item_list_first_id(items), "msg_1");
  expect_str(state, "http_list_input_items_last",
             cai_input_item_list_last_id(items), "msg_2");
  expect_str(state, "http_list_input_items_id", cai_input_item_id(items, 0U),
             "msg_1");
  expect_str(state, "http_list_input_items_type",
             cai_input_item_type(items, 0U), "message");
  expect_str(state, "http_list_input_items_role",
             cai_input_item_role(items, 1U), "assistant");
  cai_input_item_list_destroy(items);
  cai_response_create_params_destroy(params);
  cai_client_close(client);
  cai_error_cleanup(&error);

  if (waitpid(pid, &child_status, 0) != pid) {
    test_fail(state, "http_mock", "waitpid failed");
  } else if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
    test_fail(state, "http_mock", "mock child failed");
  }
}

static void test_http_error_details(test_state *state) {
  int pipe_fds[2];
  pid_t pid;
  int port;
  ssize_t nread;
  int child_status;
  char base_url[128];
  cai_client_config config;
  cai_client *client;
  cai_response *response;
  cai_error error;

  if (pipe(pipe_fds) != 0) {
    test_fail(state, "http_error_mock", "pipe failed");
    return;
  }
  pid = fork();
  if (pid < 0) {
    test_fail(state, "http_error_mock", "fork failed");
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return;
  }
  if (pid == 0) {
    close(pipe_fds[0]);
    mock_openai_child(pipe_fds[1], 1);
  }
  close(pipe_fds[1]);
  nread = read(pipe_fds[0], &port, sizeof(port));
  close(pipe_fds[0]);
  if (nread != (ssize_t)sizeof(port)) {
    test_fail(state, "http_error_mock", "failed to read mock port");
    waitpid(pid, &child_status, 0);
    return;
  }

  cai_error_init(&error);
  snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);
  cai_client_config_init(&config);
  config.api_key = "mock-key";
  config.base_url = base_url;
  config.http_2_disabled = 1;
  config.timeout_ms = 5000L;
  client = NULL;
  response = NULL;

  expect_int(state, "http_error_client_open",
             cai_client_open(&config, &client, &error), CAI_OK);
  expect_int(
      state, "http_error_retrieve",
      cai_client_retrieve_response(client, "resp_error", &response, &error),
      CAI_ERR_SERVER);
  expect_int(state, "http_error_status", error.http_status, 400L);
  expect_str(state, "http_error_message", error.message,
             "OpenAI API request failed");
  expect_str(state, "http_error_detail", error.detail, "model is required");
  expect_str(state, "http_error_code", error.server_code,
             "missing_required_parameter");
  expect_str(state, "http_error_request_id", error.request_id,
             "req_mock_error");

  cai_response_destroy(response);
  cai_client_close(client);
  cai_error_cleanup(&error);

  if (waitpid(pid, &child_status, 0) != pid) {
    test_fail(state, "http_error_mock", "waitpid failed");
  } else if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
    test_fail(state, "http_error_mock", "mock child failed");
  }
}

static void test_conversations(test_state *state) {
  int pipe_fds[2];
  pid_t pid;
  int port;
  ssize_t nread;
  int child_status;
  char base_url[128];
  cai_client_config config;
  cai_client *client;
  cai_conversation *conversation;
  cai_conversation *conversation_ref;
  cai_conversation_item *item;
  cai_input_item_list *items;
  cai_conversation_items_params *item_params;
  lonejson_spooled conversation_file_data;
  lonejson_error json_error;
  cai_list_params list_params;
  cai_error error;

  if (pipe(pipe_fds) != 0) {
    test_fail(state, "conversation_mock", "pipe failed");
    return;
  }
  pid = fork();
  if (pid < 0) {
    test_fail(state, "conversation_mock", "fork failed");
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return;
  }
  if (pid == 0) {
    close(pipe_fds[0]);
    mock_openai_child(pipe_fds[1], 8);
  }
  close(pipe_fds[1]);
  nread = read(pipe_fds[0], &port, sizeof(port));
  close(pipe_fds[0]);
  if (nread != (ssize_t)sizeof(port)) {
    test_fail(state, "conversation_mock", "failed to read mock port");
    waitpid(pid, &child_status, 0);
    return;
  }

  cai_error_init(&error);
  snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);
  cai_client_config_init(&config);
  config.api_key = "mock-key";
  config.base_url = base_url;
  config.http_2_disabled = 1;
  config.timeout_ms = 5000L;
  client = NULL;
  conversation = NULL;
  conversation_ref = NULL;
  item = NULL;
  items = NULL;
  item_params = NULL;
  expect_int(state, "conversation_client_open",
             cai_client_open(&config, &client, &error), CAI_OK);
  expect_int(state, "conversation_create",
             cai_client_create_conversation(client, &conversation, &error),
             CAI_OK);
  expect_str(state, "conversation_create_id", cai_conversation_id(conversation),
             "conv_mock");
  expect_str(state, "conversation_create_object",
             cai_conversation_object(conversation), "conversation");
  cai_conversation_destroy(conversation);
  conversation = NULL;
  expect_int(state, "conversation_retrieve",
             cai_client_retrieve_conversation(client, "conv_get", &conversation,
                                              &error),
             CAI_OK);
  expect_str(state, "conversation_retrieve_id",
             cai_conversation_id(conversation), "conv_get");
  cai_conversation_destroy(conversation);
  conversation = NULL;
  expect_int(state, "conversation_from_id",
             cai_conversation_from_id("conv_get", &conversation_ref, &error),
             CAI_OK);
  expect_str(state, "conversation_from_id_value",
             cai_conversation_id(conversation_ref), "conv_get");
  expect_int(state, "conversation_update_metadata",
             cai_client_update_conversation_metadata_handle(
                 client, conversation_ref, "{\"tenant\":\"vectis\"}",
                 &conversation, &error),
             CAI_OK);
  expect_str(state, "conversation_update_metadata_id",
             cai_conversation_id(conversation), "conv_get");
  cai_conversation_destroy(conversation);
  conversation = NULL;
  expect_int(state, "conversation_items_params_new",
             cai_conversation_items_params_new(&item_params, &error), CAI_OK);
  expect_int(state, "conversation_items_add_text",
             cai_conversation_items_params_add_text(
                 item_params, "user", "conversation item", &error),
             CAI_OK);
  expect_int(
      state, "conversation_items_add_image",
      cai_conversation_items_params_add_image_url(
          item_params, "user", "https://example.test/conv.png", "low", &error),
      CAI_OK);
  lonejson_error_init(&json_error);
  lonejson_spooled_init(&conversation_file_data, NULL);
  expect_int(state, "conversation_items_file_append",
             lonejson_spooled_append(&conversation_file_data,
                                     "conversation file text",
                                     strlen("conversation file text"),
                                     &json_error),
             LONEJSON_STATUS_OK);
  expect_int(state, "conversation_items_add_file",
             cai_conversation_items_params_add_file_data_spooled(
                 item_params, "user", "conv.txt", &conversation_file_data,
                 "low", &error),
             CAI_OK);
  expect_int(state, "conversation_create_items",
             cai_client_create_conversation_items_handle(
                 client, conversation_ref, item_params, &items, &error),
             CAI_OK);
  expect_str(state, "conversation_create_items_id",
             cai_input_item_id(items, 0U), "conv_msg_new");
  cai_input_item_list_destroy(items);
  items = NULL;
  cai_conversation_items_params_destroy(item_params);
  cai_list_params_init(&list_params);
  list_params.limit = 1;
  list_params.order = "desc";
  expect_int(state, "conversation_list_items",
             cai_client_list_conversation_items_handle(
                 client, conversation_ref, &list_params, &items, &error),
             CAI_OK);
  expect_int(state, "conversation_list_items_count",
             (long)cai_input_item_list_count(items), 1L);
  expect_str(state, "conversation_list_items_id", cai_input_item_id(items, 0U),
             "conv_msg_1");
  expect_str(state, "conversation_list_items_role",
             cai_input_item_role(items, 0U), "user");
  cai_input_item_list_destroy(items);
  items = NULL;
  expect_int(state, "conversation_retrieve_item",
             cai_client_retrieve_conversation_item_handle(
                 client, conversation_ref, "conv_msg_1", &item, &error),
             CAI_OK);
  expect_str(state, "conversation_retrieve_item_id",
             cai_conversation_item_id(item), "conv_msg_1");
  expect_str(state, "conversation_retrieve_item_type",
             cai_conversation_item_type(item), "message");
  expect_str(state, "conversation_retrieve_item_role",
             cai_conversation_item_role(item), "user");
  cai_conversation_item_destroy(item);
  item = NULL;
  expect_int(state, "conversation_delete_item",
             cai_client_delete_conversation_item_handle(
                 client, conversation_ref, "conv_msg_1", &error),
             CAI_OK);
  expect_int(
      state, "conversation_delete",
      cai_client_delete_conversation_handle(client, conversation_ref, &error),
      CAI_OK);
  cai_conversation_destroy(conversation_ref);
  cai_client_close(client);
  cai_error_cleanup(&error);

  if (waitpid(pid, &child_status, 0) != pid) {
    test_fail(state, "conversation_mock", "waitpid failed");
  } else if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
    test_fail(state, "conversation_mock", "mock child failed");
  }
}

static void test_agent_session(test_state *state) {
  int pipe_fds[2];
  pid_t pid;
  int port;
  ssize_t nread;
  int child_status;
  char base_url[128];
  cai_client_config client_config;
  cai_agent_config agent_config;
  cai_client *client;
  cai_agent *agent;
  cai_session *session;
  cai_conversation *conversation;
  cai_response *response;
  cai_output *output;
  const cai_response *output_response;
  cai_error error;

  if (pipe(pipe_fds) != 0) {
    test_fail(state, "agent_mock", "pipe failed");
    return;
  }
  pid = fork();
  if (pid < 0) {
    test_fail(state, "agent_mock", "fork failed");
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return;
  }
  if (pid == 0) {
    close(pipe_fds[0]);
    mock_openai_child(pipe_fds[1], 7);
  }
  close(pipe_fds[1]);
  nread = read(pipe_fds[0], &port, sizeof(port));
  close(pipe_fds[0]);
  if (nread != (ssize_t)sizeof(port)) {
    test_fail(state, "agent_mock", "failed to read mock port");
    waitpid(pid, &child_status, 0);
    return;
  }

  cai_error_init(&error);
  snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);
  cai_client_config_init(&client_config);
  client_config.api_key = "mock-key";
  client_config.base_url = base_url;
  client_config.http_2_disabled = 1;
  client_config.timeout_ms = 5000L;
  cai_agent_config_init(&agent_config);
  agent_config.model = CAI_MODEL_GPT_5_4_NANO;
  agent_config.instructions = "answer tersely";
  agent_config.reasoning_effort = CAI_REASONING_EFFORT_MEDIUM;
  agent_config.reasoning_summary = CAI_REASONING_SUMMARY_AUTO;
  agent_config.text_format_name = "agent_answer";
  agent_config.text_format_description = "Agent answer payload";
  agent_config.text_format_schema_json =
      "{\"type\":\"object\",\"properties\":{\"answer\":{\"type\":\"string\"}},"
      "\"required\":[\"answer\"],\"additionalProperties\":false}";
  agent_config.text_format_strict = 1;
  agent_config.max_output_tokens = 64;
  agent_config.parallel_tool_calls = 0;
  client = NULL;
  agent = NULL;
  session = NULL;
  conversation = NULL;
  response = NULL;
  output = NULL;
  output_response = NULL;

  expect_int(state, "agent_client_open",
             cai_client_open(&client_config, &client, &error), CAI_OK);
  expect_int(state, "agent_new",
             cai_client_new_agent(client, &agent_config, &agent, &error),
             CAI_OK);
  expect_int(state, "agent_session_new",
             cai_agent_new_session(agent, &session, &error), CAI_OK);
  expect_int(state, "agent_first",
             cai_session_send_text(session, "session first", &response, &error),
             CAI_OK);
  expect_str(state, "agent_first_id", cai_response_id(response),
             "resp_session_1");
  expect_str(state, "agent_first_text", cai_response_output_text(response),
             "first turn");
  cai_response_destroy(response);
  response = NULL;
  expect_int(
      state, "agent_second",
      cai_session_send_text(session, "session second", &response, &error),
      CAI_OK);
  expect_str(state, "agent_second_id", cai_response_id(response),
             "resp_session_2");
  expect_str(state, "agent_second_text", cai_response_output_text(response),
             "second turn");
  cai_response_destroy(response);
  response = NULL;
  expect_int(
      state, "agent_add_developer",
      cai_session_add_text(session, "developer", "internal note", &error),
      CAI_OK);
  expect_int(state, "agent_add_user",
             cai_session_add_text(session, "user", "incremental turn", &error),
             CAI_OK);
  expect_int(state, "agent_run", cai_session_run(session, &response, &error),
             CAI_OK);
  expect_str(state, "agent_third_id", cai_response_id(response),
             "resp_session_3");
  expect_str(state, "agent_third_text", cai_response_output_text(response),
             "third turn");
  cai_response_destroy(response);
  response = NULL;
  expect_int(state, "agent_add_image",
             cai_session_add_image_url(session, "user",
                                       "https://example.test/session.png",
                                       "high", &error),
             CAI_OK);
  expect_int(state, "agent_image_run",
             cai_session_run(session, &response, &error), CAI_OK);
  expect_str(state, "agent_image_id", cai_response_id(response),
             "resp_session_img");
  expect_str(state, "agent_image_text", cai_response_output_text(response),
             "image turn");
  cai_response_destroy(response);
  response = NULL;
  expect_int(state, "agent_session_conversation",
             cai_conversation_from_id("conv_session", &conversation, &error),
             CAI_OK);
  expect_int(state, "agent_session_set_conversation",
             cai_session_set_conversation(session, conversation, &error),
             CAI_OK);
  expect_str(state, "agent_session_conversation_id",
             cai_session_conversation_id(session), "conv_session");
  expect_int(
      state, "agent_conversation_turn",
      cai_session_send_text(session, "conversation turn", &response, &error),
      CAI_OK);
  expect_str(state, "agent_conversation_response_id", cai_response_id(response),
             "resp_session_conv");
  expect_str(state, "agent_conversation_response_conversation",
             cai_response_conversation_id(response), "conv_session");
  expect_str(state, "agent_conversation_text",
             cai_response_output_text(response), "conversation turn");
  cai_response_destroy(response);
  cai_session_destroy(session);
  session = NULL;
  response = NULL;
  cai_conversation_destroy(conversation);
  conversation = NULL;
  expect_int(state, "agent_conversation_from_id",
             cai_conversation_from_id("conv_session", &conversation, &error),
             CAI_OK);
  expect_int(state, "agent_existing_conversation_session",
             cai_agent_new_session_for_conversation(agent, conversation,
                                                    &session, &error),
             CAI_OK);
  expect_str(state, "agent_existing_conversation_session_id",
             cai_session_conversation_id(session), "conv_session");
  cai_session_destroy(session);
  session = NULL;
  cai_conversation_destroy(conversation);
  conversation = NULL;
  expect_int(state, "agent_auto_conversation_session",
             cai_agent_new_conversation_session(agent, &session, &error),
             CAI_OK);
  expect_str(state, "agent_auto_conversation_id",
             cai_session_conversation_id(session), "conv_mock");
  expect_int(
      state, "agent_auto_conversation_add_text",
      cai_session_add_text(session, "user", "auto conversation turn", &error),
      CAI_OK);
  expect_int(state, "agent_auto_conversation_turn",
             cai_session_run_output(session, &output, &error), CAI_OK);
  output_response = cai_output_response(output);
  expect_str(state, "agent_auto_conversation_response_id",
             cai_response_id(output_response), "resp_session_auto_conv");
  expect_str(state, "agent_auto_conversation_response_conversation",
             cai_response_conversation_id(output_response), "conv_mock");
  expect_str(state, "agent_auto_conversation_text", cai_output_text(output),
             "auto conversation turn");
  cai_output_destroy(output);
  cai_session_destroy(session);
  cai_agent_destroy(agent);
  cai_client_close(client);
  cai_error_cleanup(&error);

  if (waitpid(pid, &child_status, 0) != pid) {
    test_fail(state, "agent_mock", "waitpid failed");
  } else if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
    test_fail(state, "agent_mock", "mock child failed");
  }
}

static void test_agent_tool_declarations(test_state *state) {
  static const char schema[] = "{\"type\":\"object\",\"properties\":{}}";
  int pipe_fds[2];
  pid_t pid;
  int port;
  ssize_t nread;
  int child_status;
  char base_url[128];
  cai_client_config client_config;
  cai_agent_config agent_config;
  cai_client *client;
  cai_agent *agent;
  cai_session *session;
  cai_response *response;
  raw_tool_state raw_state;
  cai_error error;

  if (pipe(pipe_fds) != 0) {
    test_fail(state, "agent_tool_mock", "pipe failed");
    return;
  }
  pid = fork();
  if (pid < 0) {
    test_fail(state, "agent_tool_mock", "fork failed");
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return;
  }
  if (pid == 0) {
    close(pipe_fds[0]);
    mock_openai_child(pipe_fds[1], 1);
  }
  close(pipe_fds[1]);
  nread = read(pipe_fds[0], &port, sizeof(port));
  close(pipe_fds[0]);
  if (nread != (ssize_t)sizeof(port)) {
    test_fail(state, "agent_tool_mock", "failed to read mock port");
    waitpid(pid, &child_status, 0);
    return;
  }

  cai_error_init(&error);
  snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);
  cai_client_config_init(&client_config);
  client_config.api_key = "mock-key";
  client_config.base_url = base_url;
  client_config.http_2_disabled = 1;
  client_config.timeout_ms = 5000L;
  cai_agent_config_init(&agent_config);
  agent_config.model = CAI_MODEL_GPT_5_4_NANO;
  client = NULL;
  agent = NULL;
  session = NULL;
  response = NULL;
  raw_state.seen[0] = '\0';

  expect_int(state, "agent_tool_client_open",
             cai_client_open(&client_config, &client, &error), CAI_OK);
  expect_int(state, "agent_tool_new",
             cai_client_new_agent(client, &agent_config, &agent, &error),
             CAI_OK);
  expect_int(state, "agent_tool_register",
             cai_agent_register_raw_tool(agent, "raw_echo", "Echo raw JSON",
                                         schema, 0, test_raw_tool, &raw_state,
                                         &error),
             CAI_OK);
  expect_int(state, "agent_tool_session",
             cai_agent_new_session(agent, &session, &error), CAI_OK);
  expect_int(
      state, "agent_tool_send",
      cai_session_send_text(session, "agent tool turn", &response, &error),
      CAI_OK);
  expect_str(state, "agent_tool_response", cai_response_output_text(response),
             "tool ready");
  cai_response_destroy(response);
  cai_session_destroy(session);
  cai_agent_destroy(agent);
  cai_client_close(client);
  cai_error_cleanup(&error);

  if (waitpid(pid, &child_status, 0) != pid) {
    test_fail(state, "agent_tool_mock", "waitpid failed");
  } else if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
    test_fail(state, "agent_tool_mock", "mock child failed");
  }
}

static void test_agent_tool_auto_run(test_state *state) {
  static const char schema[] = "{\"type\":\"object\",\"properties\":{}}";
  char spool_dir[] = "/tmp/cai-tool-spool-XXXXXX";
  int pipe_fds[2];
  pid_t pid;
  int port;
  ssize_t nread;
  int child_status;
  char base_url[128];
  cai_client_config client_config;
  cai_agent_config agent_config;
  cai_run_options run_options;
  cai_client *client;
  cai_agent *agent;
  cai_session *session;
  cai_response *response;
  raw_tool_state raw_state;
  cai_error error;

  if (pipe(pipe_fds) != 0) {
    test_fail(state, "agent_auto_mock", "pipe failed");
    return;
  }
  pid = fork();
  if (pid < 0) {
    test_fail(state, "agent_auto_mock", "fork failed");
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return;
  }
  if (pid == 0) {
    close(pipe_fds[0]);
    mock_openai_child(pipe_fds[1], 2);
  }
  close(pipe_fds[1]);
  nread = read(pipe_fds[0], &port, sizeof(port));
  close(pipe_fds[0]);
  if (nread != (ssize_t)sizeof(port)) {
    test_fail(state, "agent_auto_mock", "failed to read mock port");
    waitpid(pid, &child_status, 0);
    return;
  }

  cai_error_init(&error);
  snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);
  cai_client_config_init(&client_config);
  client_config.api_key = "mock-key";
  client_config.base_url = base_url;
  client_config.http_2_disabled = 1;
  client_config.timeout_ms = 5000L;
  cai_agent_config_init(&agent_config);
  agent_config.model = CAI_MODEL_GPT_5_4_NANO;
  cai_run_options_init(&run_options);
  if (mkdtemp(spool_dir) == NULL) {
    test_fail(state, "agent_auto_spool", "mkdtemp failed");
    waitpid(pid, &child_status, 0);
    return;
  }
  run_options.tool_output_memory_limit = 4U;
  run_options.tool_spool_dir = spool_dir;
  client = NULL;
  agent = NULL;
  session = NULL;
  response = NULL;
  raw_state.seen[0] = '\0';

  expect_int(state, "agent_auto_client_open",
             cai_client_open(&client_config, &client, &error), CAI_OK);
  expect_int(state, "agent_auto_new",
             cai_client_new_agent(client, &agent_config, &agent, &error),
             CAI_OK);
  expect_int(state, "agent_auto_register",
             cai_agent_register_raw_tool(agent, "raw_echo", "Echo raw JSON",
                                         schema, 0, test_raw_tool, &raw_state,
                                         &error),
             CAI_OK);
  expect_int(state, "agent_auto_session",
             cai_agent_new_session(agent, &session, &error), CAI_OK);
  expect_int(state, "agent_auto_add",
             cai_session_add_text(session, "user", "auto tool turn", &error),
             CAI_OK);
  expect_int(state, "agent_auto_run",
             cai_session_run_auto(session, &run_options, &response, &error),
             CAI_OK);
  expect_str(state, "agent_auto_response", cai_response_output_text(response),
             "auto done");
  expect_str(state, "agent_auto_seen", raw_state.seen, "{\"x\":1}");
  cai_response_destroy(response);
  cai_session_destroy(session);
  cai_agent_destroy(agent);
  cai_client_close(client);
  cai_error_cleanup(&error);
  if (rmdir(spool_dir) != 0) {
    test_fail(state, "agent_auto_spool", "spool dir not empty");
  }

  if (waitpid(pid, &child_status, 0) != pid) {
    test_fail(state, "agent_auto_mock", "waitpid failed");
  } else if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
    test_fail(state, "agent_auto_mock", "mock child failed");
  }
}

static void test_agent_tool_auto_round_limit(test_state *state) {
  static const char schema[] = "{\"type\":\"object\",\"properties\":{}}";
  int pipe_fds[2];
  pid_t pid;
  int port;
  ssize_t nread;
  int child_status;
  char base_url[128];
  cai_client_config client_config;
  cai_agent_config agent_config;
  cai_run_options run_options;
  cai_client *client;
  cai_agent *agent;
  cai_session *session;
  cai_response *response;
  raw_tool_state raw_state;
  cai_error error;

  if (pipe(pipe_fds) != 0) {
    test_fail(state, "agent_limit_mock", "pipe failed");
    return;
  }
  pid = fork();
  if (pid < 0) {
    test_fail(state, "agent_limit_mock", "fork failed");
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return;
  }
  if (pid == 0) {
    close(pipe_fds[0]);
    mock_openai_child(pipe_fds[1], 1);
  }
  close(pipe_fds[1]);
  nread = read(pipe_fds[0], &port, sizeof(port));
  close(pipe_fds[0]);
  if (nread != (ssize_t)sizeof(port)) {
    test_fail(state, "agent_limit_mock", "failed to read mock port");
    waitpid(pid, &child_status, 0);
    return;
  }

  cai_error_init(&error);
  snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);
  cai_client_config_init(&client_config);
  client_config.api_key = "mock-key";
  client_config.base_url = base_url;
  client_config.http_2_disabled = 1;
  client_config.timeout_ms = 5000L;
  cai_agent_config_init(&agent_config);
  agent_config.model = CAI_MODEL_GPT_5_4_NANO;
  cai_run_options_init(&run_options);
  run_options.max_tool_rounds = 0;
  client = NULL;
  agent = NULL;
  session = NULL;
  response = NULL;
  raw_state.seen[0] = '\0';

  expect_int(state, "agent_limit_client_open",
             cai_client_open(&client_config, &client, &error), CAI_OK);
  expect_int(state, "agent_limit_new",
             cai_client_new_agent(client, &agent_config, &agent, &error),
             CAI_OK);
  expect_int(state, "agent_limit_register",
             cai_agent_register_raw_tool(agent, "raw_echo", "Echo raw JSON",
                                         schema, 0, test_raw_tool, &raw_state,
                                         &error),
             CAI_OK);
  expect_int(state, "agent_limit_session",
             cai_agent_new_session(agent, &session, &error), CAI_OK);
  expect_int(state, "agent_limit_add",
             cai_session_add_text(session, "user", "auto tool turn", &error),
             CAI_OK);
  expect_int(state, "agent_limit_run",
             cai_session_run_auto(session, &run_options, &response, &error),
             CAI_ERR_CANCELLED);
  expect_str(state, "agent_limit_error", error.message,
             "tool auto-run exhausted max tool rounds");
  cai_response_destroy(response);
  cai_session_destroy(session);
  cai_agent_destroy(agent);
  cai_client_close(client);
  cai_error_cleanup(&error);

  if (waitpid(pid, &child_status, 0) != pid) {
    test_fail(state, "agent_limit_mock", "waitpid failed");
  } else if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
    test_fail(state, "agent_limit_mock", "mock child failed");
  }
}

static void test_agent_tool_manual_step(test_state *state) {
  static const char schema[] = "{\"type\":\"object\",\"properties\":{}}";
  int pipe_fds[2];
  pid_t pid;
  int port;
  ssize_t nread;
  int child_status;
  char base_url[128];
  cai_client_config client_config;
  cai_agent_config agent_config;
  cai_client *client;
  cai_agent *agent;
  cai_session *session;
  cai_response *response;
  cai_error error;

  if (pipe(pipe_fds) != 0) {
    test_fail(state, "agent_manual_mock", "pipe failed");
    return;
  }
  pid = fork();
  if (pid < 0) {
    test_fail(state, "agent_manual_mock", "fork failed");
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return;
  }
  if (pid == 0) {
    close(pipe_fds[0]);
    mock_openai_child(pipe_fds[1], 2);
  }
  close(pipe_fds[1]);
  nread = read(pipe_fds[0], &port, sizeof(port));
  close(pipe_fds[0]);
  if (nread != (ssize_t)sizeof(port)) {
    test_fail(state, "agent_manual_mock", "failed to read mock port");
    waitpid(pid, &child_status, 0);
    return;
  }

  cai_error_init(&error);
  snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);
  cai_client_config_init(&client_config);
  client_config.api_key = "mock-key";
  client_config.base_url = base_url;
  client_config.http_2_disabled = 1;
  client_config.timeout_ms = 5000L;
  cai_agent_config_init(&agent_config);
  agent_config.model = CAI_MODEL_GPT_5_4_NANO;
  client = NULL;
  agent = NULL;
  session = NULL;
  response = NULL;

  expect_int(state, "agent_manual_client_open",
             cai_client_open(&client_config, &client, &error), CAI_OK);
  expect_int(state, "agent_manual_new",
             cai_client_new_agent(client, &agent_config, &agent, &error),
             CAI_OK);
  expect_int(state, "agent_manual_register",
             cai_agent_register_raw_tool(agent, "raw_echo", "Echo raw JSON",
                                         schema, 0, test_raw_tool, NULL,
                                         &error),
             CAI_OK);
  expect_int(state, "agent_manual_session",
             cai_agent_new_session(agent, &session, &error), CAI_OK);
  expect_int(state, "agent_manual_add",
             cai_session_add_text(session, "user", "manual tool turn", &error),
             CAI_OK);
  expect_int(state, "agent_manual_run_first",
             cai_session_run(session, &response, &error), CAI_OK);
  expect_int(state, "agent_manual_tool_count",
             (long)cai_response_tool_call_count(response), 1L);
  expect_str(state, "agent_manual_tool_id",
             cai_response_tool_call_id(response, 0U), "call_manual_1");
  cai_response_destroy(response);
  response = NULL;
  expect_int(state, "agent_manual_add_output",
             cai_session_add_function_call_output(session, "call_manual_1",
                                                  "{\"y\":2}", &error),
             CAI_OK);
  expect_int(state, "agent_manual_run_second",
             cai_session_run(session, &response, &error), CAI_OK);
  expect_str(state, "agent_manual_response", cai_response_output_text(response),
             "manual done");
  cai_response_destroy(response);
  cai_session_destroy(session);
  cai_agent_destroy(agent);
  cai_client_close(client);
  cai_error_cleanup(&error);

  if (waitpid(pid, &child_status, 0) != pid) {
    test_fail(state, "agent_manual_mock", "waitpid failed");
  } else if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
    test_fail(state, "agent_manual_mock", "mock child failed");
  }
}

static void test_agent_auto_compaction(test_state *state) {
  int pipe_fds[2];
  pid_t pid;
  int port;
  ssize_t nread;
  int child_status;
  char base_url[128];
  cai_client_config client_config;
  cai_agent_config agent_config;
  cai_client *client;
  cai_agent *agent;
  cai_session *session;
  cai_response *response;
  cai_token_usage usage;
  cai_error error;
  double percent;

  if (pipe(pipe_fds) != 0) {
    test_fail(state, "agent_compact_mock", "pipe failed");
    return;
  }
  pid = fork();
  if (pid < 0) {
    test_fail(state, "agent_compact_mock", "fork failed");
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return;
  }
  if (pid == 0) {
    close(pipe_fds[0]);
    mock_openai_child(pipe_fds[1], 3);
  }
  close(pipe_fds[1]);
  nread = read(pipe_fds[0], &port, sizeof(port));
  close(pipe_fds[0]);
  if (nread != (ssize_t)sizeof(port)) {
    test_fail(state, "agent_compact_mock", "failed to read mock port");
    waitpid(pid, &child_status, 0);
    return;
  }

  cai_error_init(&error);
  snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);
  cai_client_config_init(&client_config);
  client_config.api_key = "mock-key";
  client_config.base_url = base_url;
  client_config.http_2_disabled = 1;
  client_config.timeout_ms = 5000L;
  cai_agent_config_init(&agent_config);
  agent_config.model = CAI_MODEL_GPT_5_4_NANO;
  agent_config.auto_compact = 1;
  agent_config.history_memory_limit = 16U;
  agent_config.history_memory_limit = 16U;
  client = NULL;
  agent = NULL;
  session = NULL;
  response = NULL;

  expect_int(state, "agent_compact_client_open",
             cai_client_open(&client_config, &client, &error), CAI_OK);
  expect_int(state, "agent_compact_new",
             cai_client_new_agent(client, &agent_config, &agent, &error),
             CAI_OK);
  expect_int(state, "agent_compact_session",
             cai_agent_new_session(agent, &session, &error), CAI_OK);
  expect_int(state, "agent_compact_auto_limit",
             cai_session_auto_compact_token_limit(session), 320000L);
  expect_int(state, "agent_compact_add",
             cai_session_add_text(session, "user", "compact first", &error),
             CAI_OK);
  expect_int(state, "agent_compact_run",
             cai_session_run(session, &response, &error), CAI_OK);
  expect_str(state, "agent_compact_text", cai_response_output_text(response),
             "before compact");
  expect_int(state, "agent_compact_spilled",
             cai_session_history_spilled(session), 1L);
  expect_int(state, "agent_compact_usage",
             cai_session_last_usage(session, &usage, &error), CAI_OK);
  expect_int(state, "agent_compact_usage_total", usage.total_tokens, 120L);
  expect_int(state, "agent_compact_window",
             cai_session_context_window_tokens(session), 400000L);
  expect_int(state, "agent_compact_percent",
             cai_session_context_percent(session, &percent, &error), CAI_OK);
  if (percent <= 0.0) {
    test_fail(state, "agent_compact_percent_value", "percent not positive");
  }
  cai_response_destroy(response);
  response = NULL;
  expect_int(state, "agent_compact_add_second",
             cai_session_add_text(session, "user", "compact second", &error),
             CAI_OK);
  expect_int(state, "agent_compact_run_second",
             cai_session_run(session, &response, &error), CAI_OK);
  expect_str(state, "agent_compact_text_second",
             cai_response_output_text(response), "after compact");
  cai_response_destroy(response);
  cai_session_destroy(session);
  cai_agent_destroy(agent);
  cai_client_close(client);
  cai_error_cleanup(&error);

  if (waitpid(pid, &child_status, 0) != pid) {
    test_fail(state, "agent_compact_mock", "waitpid failed");
  } else if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
    test_fail(state, "agent_compact_mock", "mock child failed");
  }
}

static void test_stream_response_text(test_state *state) {
  int pipe_fds[2];
  pid_t pid;
  int port;
  ssize_t nread;
  int child_status;
  char base_url[128];
  char read_buffer[16];
  size_t got;
  cai_client_config config;
  cai_agent_config agent_config;
  cai_response_create_params *params;
  cai_client *client;
  cai_agent *agent;
  cai_session *session;
  cai_source *source;
  cai_sink_callbacks sink_callbacks;
  cai_sink *sink;
  cai_token_usage usage;
  write_state writer;
  cai_error error;

  if (pipe(pipe_fds) != 0) {
    test_fail(state, "stream_mock", "pipe failed");
    return;
  }
  pid = fork();
  if (pid < 0) {
    test_fail(state, "stream_mock", "fork failed");
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return;
  }
  if (pid == 0) {
    close(pipe_fds[0]);
    mock_openai_child(pipe_fds[1], 6);
  }
  close(pipe_fds[1]);
  nread = read(pipe_fds[0], &port, sizeof(port));
  close(pipe_fds[0]);
  if (nread != (ssize_t)sizeof(port)) {
    test_fail(state, "stream_mock", "failed to read mock port");
    waitpid(pid, &child_status, 0);
    return;
  }

  cai_error_init(&error);
  snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);
  cai_client_config_init(&config);
  cai_agent_config_init(&agent_config);
  agent_config.model = CAI_MODEL_GPT_5_4_NANO;
  config.api_key = "mock-key";
  config.base_url = base_url;
  config.http_2_disabled = 1;
  config.timeout_ms = 5000L;
  client = NULL;
  agent = NULL;
  session = NULL;
  params = NULL;
  sink = NULL;
  source = NULL;
  writer.length = 0U;
  writer.closed = 0;
  writer.buffer[0] = '\0';
  sink_callbacks.write = test_write;
  sink_callbacks.close = test_write_close;
  sink_callbacks.context = &writer;

  expect_int(state, "stream_client_open",
             cai_client_open(&config, &client, &error), CAI_OK);
  expect_int(state, "stream_params_new",
             cai_response_create_params_new(&params, &error), CAI_OK);
  expect_int(state, "stream_params_model",
             cai_response_create_params_set_model(
                 params, CAI_MODEL_GPT_5_4_NANO, &error),
             CAI_OK);
  expect_int(
      state, "stream_params_text",
      cai_response_create_params_add_text(params, "user", "stream", &error),
      CAI_OK);
  expect_int(state, "stream_sink_create",
             cai_sink_from_callbacks(&sink_callbacks, &sink, &error), CAI_OK);
  expect_int(state, "stream_to_sink",
             cai_client_stream_response_text(client, params, sink, &error),
             CAI_OK);
  expect_str(state, "stream_sink_value", writer.buffer, "hello");
  cai_sink_close(sink);
  sink = NULL;

  expect_int(
      state, "stream_source_open",
      cai_client_open_response_text_source(client, params, &source, &error),
      CAI_OK);
  got = 0U;
  while (got < 5U) {
    nread =
        (ssize_t)cai_source_read(source, read_buffer + got, 5U - got, &error);
    if (nread <= 0) {
      break;
    }
    got += (size_t)nread;
  }
  read_buffer[got] = '\0';
  expect_str(state, "stream_source_value", read_buffer, "hello");
  cai_source_close(source);
  source = NULL;
  expect_int(state, "stream_agent_new",
             cai_client_new_agent(client, &agent_config, &agent, &error),
             CAI_OK);
  expect_int(state, "stream_session_new",
             cai_agent_new_session(agent, &session, &error), CAI_OK);
  writer.length = 0U;
  writer.closed = 0;
  writer.buffer[0] = '\0';
  sink_callbacks.context = &writer;
  expect_int(
      state, "stream_session_add",
      cai_session_add_text(session, "user", "session stream one", &error),
      CAI_OK);
  expect_int(state, "stream_session_sink_create",
             cai_sink_from_callbacks(&sink_callbacks, &sink, &error), CAI_OK);
  expect_int(state, "stream_session_to_sink",
             cai_session_stream_text(session, sink, &error), CAI_OK);
  expect_str(state, "stream_session_sink_value", writer.buffer, "one");
  expect_int(state, "stream_session_usage",
             cai_session_last_usage(session, &usage, &error), CAI_OK);
  expect_int(state, "stream_session_usage_cached", usage.input_cached_tokens,
             4L);
  expect_int(state, "stream_session_usage_reasoning",
             usage.output_reasoning_tokens, 1L);
  cai_sink_close(sink);
  sink = NULL;
  writer.length = 0U;
  writer.closed = 0;
  writer.buffer[0] = '\0';
  expect_int(
      state, "stream_session_add_second",
      cai_session_add_text(session, "user", "session stream two", &error),
      CAI_OK);
  expect_int(state, "stream_session_sink_create_second",
             cai_sink_from_callbacks(&sink_callbacks, &sink, &error), CAI_OK);
  expect_int(state, "stream_session_to_sink_second",
             cai_session_stream_text(session, sink, &error), CAI_OK);
  expect_str(state, "stream_session_sink_value_second", writer.buffer, "two");
  cai_sink_close(sink);
  sink = NULL;

  expect_int(
      state, "stream_session_source_add",
      cai_session_add_text(session, "user", "session source one", &error),
      CAI_OK);
  expect_int(state, "stream_session_source_open",
             cai_session_open_text_source(session, &source, &error), CAI_OK);
  got = 0U;
  while (got < 4U) {
    nread =
        (ssize_t)cai_source_read(source, read_buffer + got, 4U - got, &error);
    if (nread <= 0) {
      break;
    }
    got += (size_t)nread;
  }
  read_buffer[got] = '\0';
  expect_str(state, "stream_session_source_value", read_buffer, "src1");
  cai_source_close(source);
  source = NULL;
  expect_int(state, "stream_session_source_usage",
             cai_session_last_usage(session, &usage, &error), CAI_OK);
  expect_int(state, "stream_session_source_usage_cached",
             usage.input_cached_tokens, 12L);
  expect_int(state, "stream_session_source_usage_reasoning",
             usage.output_reasoning_tokens, 3L);

  expect_int(
      state, "stream_session_source_add_second",
      cai_session_add_text(session, "user", "session source two", &error),
      CAI_OK);
  expect_int(state, "stream_session_source_open_second",
             cai_session_open_text_source(session, &source, &error), CAI_OK);
  got = 0U;
  while (got < 4U) {
    nread =
        (ssize_t)cai_source_read(source, read_buffer + got, 4U - got, &error);
    if (nread <= 0) {
      break;
    }
    got += (size_t)nread;
  }
  read_buffer[got] = '\0';
  expect_str(state, "stream_session_source_value_second", read_buffer, "src2");
  cai_source_close(source);
  source = NULL;
  cai_response_create_params_destroy(params);
  cai_session_destroy(session);
  cai_agent_destroy(agent);
  cai_client_close(client);
  cai_error_cleanup(&error);

  if (waitpid(pid, &child_status, 0) != pid) {
    test_fail(state, "stream_mock", "waitpid failed");
  } else if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
    test_fail(state, "stream_mock", "mock child failed");
  }
}

static void test_stream_history_preserves_pretty_json(test_state *state) {
  int pipe_fds[2];
  pid_t pid;
  int port;
  ssize_t nread;
  int child_status;
  char base_url[128];
  cai_client_config config;
  cai_agent_config agent_config;
  cai_client *client;
  cai_agent *agent;
  cai_session *session;
  cai_sink_callbacks sink_callbacks;
  cai_sink *sink;
  write_state writer;
  cai_error error;

  if (pipe(pipe_fds) != 0) {
    test_fail(state, "stream_history_mock", "pipe failed");
    return;
  }
  pid = fork();
  if (pid < 0) {
    test_fail(state, "stream_history_mock", "fork failed");
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return;
  }
  if (pid == 0) {
    close(pipe_fds[0]);
    mock_openai_child(pipe_fds[1], 4);
  }
  close(pipe_fds[1]);
  nread = read(pipe_fds[0], &port, sizeof(port));
  close(pipe_fds[0]);
  if (nread != (ssize_t)sizeof(port)) {
    test_fail(state, "stream_history_mock", "failed to read mock port");
    waitpid(pid, &child_status, 0);
    return;
  }

  cai_error_init(&error);
  snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);
  cai_client_config_init(&config);
  cai_agent_config_init(&agent_config);
  agent_config.model = CAI_MODEL_GPT_5_4_NANO;
  agent_config.auto_compact = 1;
  agent_config.history_memory_limit = 16U;
  config.api_key = "mock-key";
  config.base_url = base_url;
  config.http_2_disabled = 1;
  config.timeout_ms = 5000L;
  client = NULL;
  agent = NULL;
  session = NULL;
  sink = NULL;
  sink_callbacks.write = test_write;
  sink_callbacks.close = test_write_close;

  expect_int(state, "stream_history_client_open",
             cai_client_open(&config, &client, &error), CAI_OK);
  expect_int(state, "stream_history_agent_new",
             cai_client_new_agent(client, &agent_config, &agent, &error),
             CAI_OK);
  expect_int(state, "stream_history_session_new",
             cai_agent_new_session(agent, &session, &error), CAI_OK);
  writer.length = 0U;
  writer.closed = 0;
  writer.buffer[0] = '\0';
  sink_callbacks.context = &writer;
  expect_int(
      state, "stream_history_add",
      cai_session_add_text(session, "user", "history stream first", &error),
      CAI_OK);
  expect_int(state, "stream_history_sink_create",
             cai_sink_from_callbacks(&sink_callbacks, &sink, &error), CAI_OK);
  expect_int(state, "stream_history_first",
             cai_session_stream_text(session, sink, &error), CAI_OK);
  expect_str(state, "stream_history_first_value", writer.buffer, "hist1");
  expect_int(state, "stream_history_spilled",
             cai_session_history_spilled(session), 1L);
  cai_sink_close(sink);
  sink = NULL;

  writer.length = 0U;
  writer.closed = 0;
  writer.buffer[0] = '\0';
  expect_int(
      state, "stream_history_add_second",
      cai_session_add_text(session, "user", "history stream second", &error),
      CAI_OK);
  expect_int(state, "stream_history_sink_create_second",
             cai_sink_from_callbacks(&sink_callbacks, &sink, &error), CAI_OK);
  expect_int(state, "stream_history_second",
             cai_session_stream_text(session, sink, &error), CAI_OK);
  expect_str(state, "stream_history_second_value", writer.buffer, "hist2");
  cai_sink_close(sink);
  cai_session_destroy(session);
  cai_agent_destroy(agent);
  cai_client_close(client);
  cai_error_cleanup(&error);

  if (waitpid(pid, &child_status, 0) != pid) {
    test_fail(state, "stream_history_mock", "waitpid failed");
  } else if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
    test_fail(state, "stream_history_mock", "mock child failed");
  }
}

int main(void) {
  test_state state;

  state.failures = 0;
  test_model_capabilities(&state);
  test_env_precedence(&state);
  test_source_sink(&state);
  test_tool_registry(&state);
  test_client_open(&state);
  test_response_json(&state);
  test_response_spooled_request_fragments(&state);
  test_http_create_response(&state);
  test_http_error_details(&state);
  test_agent_session(&state);
  test_agent_tool_declarations(&state);
  test_agent_tool_manual_step(&state);
  test_agent_auto_compaction(&state);
  test_agent_tool_auto_run(&state);
  test_agent_tool_auto_round_limit(&state);
  test_conversations(&state);
  test_stream_response_text(&state);
  test_stream_history_preserves_pretty_json(&state);
  if (state.failures != 0) {
    fprintf(stderr, "%d test(s) failed\n", state.failures);
    return 1;
  }
  return 0;
}
