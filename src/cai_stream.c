#include "cai_internal.h"

#include <curl/curl.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct cai_stream_response_doc {
  char *id;
} cai_stream_response_doc;

typedef struct cai_stream_delta_doc {
  char *type;
  char *delta;
  cai_stream_response_doc response;
} cai_stream_delta_doc;

static const lonejson_field cai_stream_response_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_stream_response_doc, id, "id")};
LONEJSON_MAP_DEFINE(cai_stream_response_map, cai_stream_response_doc,
                    cai_stream_response_fields);

static const lonejson_field cai_stream_delta_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_stream_delta_doc, type, "type"),
    LONEJSON_FIELD_STRING_ALLOC(cai_stream_delta_doc, delta, "delta"),
    LONEJSON_FIELD_OBJECT(cai_stream_delta_doc, response, "response",
                          &cai_stream_response_map)};
LONEJSON_MAP_DEFINE(cai_stream_delta_map, cai_stream_delta_doc,
                    cai_stream_delta_fields);

typedef struct cai_sse_state {
  cai_sink *sink;
  char **out_response_id;
  char *line;
  size_t length;
  size_t capacity;
  int failed;
} cai_sse_state;

typedef struct cai_pipe_stream {
  cai_client *client;
  char *request_json;
  int read_fd;
  int write_fd;
  pthread_t thread;
  int thread_started;
} cai_pipe_stream;

static int cai_stream_request_json(const cai_response_create_params *params,
                                   char **out, cai_error *error) {
  char *json;
  char *stream_json;
  size_t length;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "stream JSON output pointer is required");
  }
  *out = NULL;
  json = NULL;
  rc = cai_response_create_params_serialize_json(params, &json, &length, error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (length == 0U || json[length - 1U] != '}') {
    free(json);
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "response request JSON is not an object");
  }
  stream_json = (char *)cai_alloc(NULL, length + 16U);
  if (stream_json == NULL) {
    free(json);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate streaming request JSON");
  }
  memcpy(stream_json, json, length - 1U);
  memcpy(stream_json + length - 1U, ",\"stream\":true}", 16U);
  free(json);
  *out = stream_json;
  return CAI_OK;
}

static int cai_sse_line_reserve(cai_sse_state *state, size_t extra) {
  char *grown;
  size_t next_capacity;

  if (state->length + extra + 1U <= state->capacity) {
    return CAI_OK;
  }
  next_capacity = state->capacity == 0U ? 256U : state->capacity * 2U;
  while (next_capacity < state->length + extra + 1U) {
    next_capacity *= 2U;
  }
  grown = (char *)cai_realloc_mem(NULL, state->line, next_capacity);
  if (grown == NULL) {
    return CAI_ERR_NOMEM;
  }
  state->line = grown;
  state->capacity = next_capacity;
  return CAI_OK;
}

static int cai_sse_emit_data(cai_sse_state *state, const char *data) {
  cai_stream_delta_doc doc;
  lonejson_error json_error;
  lonejson_status status;
  size_t length;
  int rc;

  (void)json_error;
  if (data == NULL || data[0] == '\0') {
    return CAI_OK;
  }
  while (*data == ' ') {
    data++;
  }
  lonejson_init(&cai_stream_delta_map, &doc);
  status =
      lonejson_parse_cstr(&cai_stream_delta_map, &doc, data, NULL, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    lonejson_cleanup(&cai_stream_delta_map, &doc);
    return CAI_OK;
  }
  rc = CAI_OK;
  if (doc.type != NULL && strcmp(doc.type, "response.output_text.delta") == 0 &&
      doc.delta != NULL) {
    length = strlen(doc.delta);
    if (length > 0U) {
      rc = cai_sink_write(state->sink, doc.delta, length, NULL);
    }
  }
  if (rc == CAI_OK && state->out_response_id != NULL &&
      *state->out_response_id == NULL && doc.response.id != NULL) {
    *state->out_response_id = cai_strdup(NULL, doc.response.id);
    if (*state->out_response_id == NULL) {
      rc = CAI_ERR_NOMEM;
    }
  }
  lonejson_cleanup(&cai_stream_delta_map, &doc);
  return rc;
}

static int cai_sse_process_line(cai_sse_state *state) {
  char *line;
  size_t length;

  line = state->line;
  length = state->length;
  while (length > 0U &&
         (line[length - 1U] == '\r' || line[length - 1U] == '\n')) {
    line[length - 1U] = '\0';
    length--;
  }
  if (strncmp(line, "data:", 5U) == 0) {
    return cai_sse_emit_data(state, line + 5);
  }
  return CAI_OK;
}

static size_t cai_sse_write(void *ptr, size_t size, size_t nmemb, void *user) {
  cai_sse_state *state;
  const char *bytes;
  size_t total;
  size_t i;

  state = (cai_sse_state *)user;
  bytes = (const char *)ptr;
  total = size * nmemb;
  for (i = 0U; i < total; i++) {
    if (cai_sse_line_reserve(state, 1U) != CAI_OK) {
      state->failed = 1;
      return 0U;
    }
    state->line[state->length++] = bytes[i];
    state->line[state->length] = '\0';
    if (bytes[i] == '\n') {
      if (cai_sse_process_line(state) != CAI_OK) {
        state->failed = 1;
        return 0U;
      }
      state->length = 0U;
      state->line[0] = '\0';
    }
  }
  return total;
}

int cai_client_stream_response_text_json_with_id(cai_client *client,
                                                 const char *request_json,
                                                 cai_sink *sink,
                                                 char **out_response_id,
                                                 cai_error *error) {
  CURL *curl;
  CURLcode curl_rc;
  struct curl_slist *headers;
  cai_sse_state state;
  char *url;
  long http_status;
  int rc;

  if (client == NULL || request_json == NULL || sink == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "client, request JSON, and sink are required");
  }
  url = NULL;
  headers = NULL;
  if (out_response_id != NULL) {
    *out_response_id = NULL;
  }
  state.sink = sink;
  state.out_response_id = out_response_id;
  state.line = NULL;
  state.length = 0U;
  state.capacity = 0U;
  state.failed = 0;
  rc = cai_build_url(&client->allocator, client->base_url, "responses", &url,
                     error);
  if (rc == CAI_OK) {
    rc = cai_append_header(&headers, "Content-Type: application/json", error);
  }
  if (rc == CAI_OK) {
    rc = cai_append_header(&headers, "Accept: text/event-stream", error);
  }
  if (rc == CAI_OK) {
    rc = cai_append_bearer_header(client, &headers, error);
  }
  if (rc == CAI_OK) {
    rc = cai_append_prefixed_header(client, &headers, "OpenAI-Organization: ",
                                    client->organization_id, error);
  }
  if (rc == CAI_OK) {
    rc = cai_append_prefixed_header(
        client, &headers, "OpenAI-Project: ", client->project_id, error);
  }
  if (rc != CAI_OK) {
    curl_slist_free_all(headers);
    cai_free_mem(&client->allocator, url);
    return rc;
  }
  curl = curl_easy_init();
  if (curl == NULL) {
    curl_slist_free_all(headers);
    cai_free_mem(&client->allocator, url);
    return cai_set_error(error, CAI_ERR_TRANSPORT, "failed to initialize curl");
  }
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_json);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(request_json));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cai_sse_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  if (client->timeout_ms > 0L) {
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, client->timeout_ms);
  }
  if (client->http_2_disabled) {
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
  } else {
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
  }
  if (client->insecure_skip_verify) {
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  }
  curl_rc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);
  cai_free_mem(&client->allocator, url);
  cai_free_mem(NULL, state.line);
  if (curl_rc != CURLE_OK) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "streaming HTTP request failed",
                                curl_easy_strerror(curl_rc));
  }
  if (state.failed) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "streaming response sink failed");
  }
  if (http_status < 200L || http_status >= 300L) {
    return cai_set_error(error, CAI_ERR_SERVER,
                         "streaming HTTP request failed");
  }
  return CAI_OK;
}

int cai_client_stream_response_text(cai_client *client,
                                    const cai_response_create_params *params,
                                    cai_sink *sink, cai_error *error) {
  return cai_client_stream_response_text_with_id(client, params, sink, NULL,
                                                 error);
}

int cai_client_stream_response_text_with_id(
    cai_client *client, const cai_response_create_params *params,
    cai_sink *sink, char **out_response_id, cai_error *error) {
  char *request_json;
  int rc;

  if (out_response_id != NULL) {
    *out_response_id = NULL;
  }
  request_json = NULL;
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "params are required");
  }
  rc = cai_stream_request_json(params, &request_json, error);
  if (rc == CAI_OK) {
    rc = cai_client_stream_response_text_json_with_id(
        client, request_json, sink, out_response_id, error);
  }
  cai_free_mem(NULL, request_json);
  return rc;
}

static int cai_pipe_sink_write(void *context, const void *bytes, size_t count,
                               cai_error *error) {
  int fd;
  const char *data;
  size_t offset;
  ssize_t written;

  (void)error;
  fd = *(int *)context;
  data = (const char *)bytes;
  offset = 0U;
  while (offset < count) {
    written = write(fd, data + offset, count - offset);
    if (written <= 0) {
      return CAI_ERR_TRANSPORT;
    }
    offset += (size_t)written;
  }
  return CAI_OK;
}

static void *cai_pipe_stream_main(void *arg) {
  cai_pipe_stream *stream;
  cai_sink_callbacks callbacks;
  cai_sink *sink;
  cai_error error;

  stream = (cai_pipe_stream *)arg;
  sink = NULL;
  cai_error_init(&error);
  callbacks.write = cai_pipe_sink_write;
  callbacks.close = NULL;
  callbacks.context = &stream->write_fd;
  if (cai_sink_from_callbacks(&callbacks, &sink, &error) == CAI_OK) {
    (void)cai_client_stream_response_text_json_with_id(
        stream->client, stream->request_json, sink, NULL, &error);
  }
  cai_sink_close(sink);
  close(stream->write_fd);
  stream->write_fd = -1;
  cai_error_cleanup(&error);
  return NULL;
}

static size_t cai_pipe_source_read(void *context, void *buffer, size_t count,
                                   cai_error *error) {
  cai_pipe_stream *stream;
  ssize_t got;

  (void)error;
  stream = (cai_pipe_stream *)context;
  if (stream == NULL || stream->read_fd < 0) {
    return 0U;
  }
  got = read(stream->read_fd, buffer, count);
  if (got <= 0) {
    return 0U;
  }
  return (size_t)got;
}

static void cai_pipe_source_close(void *context) {
  cai_pipe_stream *stream;

  stream = (cai_pipe_stream *)context;
  if (stream == NULL) {
    return;
  }
  if (stream->read_fd >= 0) {
    close(stream->read_fd);
    stream->read_fd = -1;
  }
  if (stream->thread_started) {
    pthread_join(stream->thread, NULL);
  }
  if (stream->write_fd >= 0) {
    close(stream->write_fd);
  }
  cai_free_mem(NULL, stream->request_json);
  cai_free_mem(NULL, stream);
}

int cai_client_open_response_text_source(
    cai_client *client, const cai_response_create_params *params,
    cai_source **out, cai_error *error) {
  cai_pipe_stream *stream;
  cai_source_callbacks callbacks;
  int fds[2];
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "source output pointer is required");
  }
  *out = NULL;
  if (client == NULL || params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "client and params are required");
  }
  if (pipe(fds) != 0) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to create streaming pipe");
  }
  stream = (cai_pipe_stream *)cai_alloc(NULL, sizeof(*stream));
  if (stream == NULL) {
    close(fds[0]);
    close(fds[1]);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate streaming source");
  }
  stream->client = client;
  stream->request_json = NULL;
  stream->read_fd = fds[0];
  stream->write_fd = fds[1];
  stream->thread_started = 0;
  rc = cai_stream_request_json(params, &stream->request_json, error);
  if (rc != CAI_OK) {
    cai_pipe_source_close(stream);
    return rc;
  }
  if (pthread_create(&stream->thread, NULL, cai_pipe_stream_main, stream) !=
      0) {
    cai_pipe_source_close(stream);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to start streaming thread");
  }
  stream->thread_started = 1;
  callbacks.read = cai_pipe_source_read;
  callbacks.reset = NULL;
  callbacks.close = cai_pipe_source_close;
  callbacks.context = stream;
  rc = cai_source_from_callbacks(&callbacks, out, error);
  if (rc != CAI_OK) {
    cai_pipe_source_close(stream);
  }
  return rc;
}
