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

static void test_response_json(test_state *state) {
  static const char response_json[] =
      "{\"id\":\"resp_123\",\"status\":\"completed\",\"output\":[{\"type\":"
      "\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"hello "
      "\"},{\"type\":\"output_text\",\"text\":\"world\"}]}]}";
  cai_response_create_params *params;
  cai_response *response;
  cai_error error;
  char *json;
  size_t json_len;

  cai_error_init(&error);
  params = NULL;
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
  expect_int(
      state, "params_add_text",
      cai_response_create_params_add_text(params, "user", "hello", &error),
      CAI_OK);
  expect_int(
      state, "params_add_image",
      cai_response_create_params_add_image_url(
          params, "user", "https://example.test/image.png", "high", &error),
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
    if (strstr(json, "\"type\":\"input_text\"") == NULL) {
      test_fail(state, "params_serialize", "text content missing from JSON");
    }
    if (strstr(json, "\"type\":\"input_image\"") == NULL) {
      test_fail(state, "params_serialize", "image content missing from JSON");
    }
    if (strstr(json, ":null") != NULL) {
      test_fail(state, "params_serialize", "unexpected null field in JSON");
    }
    expect_int(state, "params_serialize_len", (long)strlen(json),
               (long)json_len);
    free(json);
  }
  cai_response_create_params_destroy(params);

  response = NULL;
  expect_int(state, "response_parse",
             cai_response_parse_json(response_json, &response, &error), CAI_OK);
  expect_str(state, "response_id", cai_response_id(response), "resp_123");
  expect_str(state, "response_status", cai_response_status(response),
             "completed");
  expect_str(state, "response_text", cai_response_output_text(response),
             "hello world");
  if (strstr(cai_response_raw_json(response), "\"id\":\"resp_123\"") == NULL) {
    test_fail(state, "response_raw_json", "raw JSON missing response id");
  }
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

  if (strncmp(request, "POST /v1/responses HTTP/", 24U) == 0) {
    if (strstr(request, "session first") != NULL &&
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
    return create_body;
  }
  if (strncmp(request, "GET /v1/responses/resp_get HTTP/", 32U) == 0) {
    return retrieve_body;
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
  if (strstr(request, "POST /v1/conversations/conv_get/items HTTP/") != NULL &&
      strstr(request, "\"items\":[") != NULL &&
      strstr(request, "\"type\":\"input_text\"") != NULL &&
      strstr(request, "\"text\":\"conversation item\"") != NULL &&
      strstr(request, "\"type\":\"input_image\"") != NULL &&
      strstr(request, "https://example.test/conv.png") != NULL) {
    return conversation_items_create_body;
  }
  if (strstr(request, "GET /v1/conversations/conv_get/items?") != NULL &&
      strstr(request, "limit=1") != NULL &&
      strstr(request, "order=desc") != NULL) {
    return conversation_items_body;
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
  config.prefer_http_2 = 0;
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
  config.prefer_http_2 = 0;
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
  cai_input_item_list *items;
  cai_conversation_items_params *item_params;
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
    mock_openai_child(pipe_fds[1], 6);
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
  config.prefer_http_2 = 0;
  config.timeout_ms = 5000L;
  client = NULL;
  conversation = NULL;
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
  expect_int(state, "conversation_create_items",
             cai_client_create_conversation_items(client, "conv_get",
                                                  item_params, &items, &error),
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
             cai_client_list_conversation_items(client, "conv_get",
                                                &list_params, &items, &error),
             CAI_OK);
  expect_int(state, "conversation_list_items_count",
             (long)cai_input_item_list_count(items), 1L);
  expect_str(state, "conversation_list_items_id", cai_input_item_id(items, 0U),
             "conv_msg_1");
  expect_str(state, "conversation_list_items_role",
             cai_input_item_role(items, 0U), "user");
  cai_input_item_list_destroy(items);
  expect_int(state, "conversation_delete_item",
             cai_client_delete_conversation_item(client, "conv_get",
                                                 "conv_msg_1", &error),
             CAI_OK);
  expect_int(state, "conversation_delete",
             cai_client_delete_conversation(client, "conv_get", &error),
             CAI_OK);
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
  cai_response *response;
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
    mock_openai_child(pipe_fds[1], 4);
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
  client_config.prefer_http_2 = 0;
  client_config.timeout_ms = 5000L;
  cai_agent_config_init(&agent_config);
  agent_config.model = CAI_MODEL_GPT_5_4_NANO;
  agent_config.instructions = "answer tersely";
  client = NULL;
  agent = NULL;
  session = NULL;
  response = NULL;

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

int main(void) {
  test_state state;

  state.failures = 0;
  test_model_capabilities(&state);
  test_env_precedence(&state);
  test_source_sink(&state);
  test_client_open(&state);
  test_response_json(&state);
  test_http_create_response(&state);
  test_http_error_details(&state);
  test_agent_session(&state);
  test_conversations(&state);
  if (state.failures != 0) {
    fprintf(stderr, "%d test(s) failed\n", state.failures);
    return 1;
  }
  return 0;
}
