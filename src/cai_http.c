#include "cai_internal.h"

#include <curl/curl.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct cai_http_buffer {
  char *data;
  size_t length;
  size_t capacity;
} cai_http_buffer;

typedef struct cai_http_spooled_upload {
  lonejson_spooled cursor;
  int initialized;
} cai_http_spooled_upload;

typedef struct cai_http_response_params_upload {
  const cai_response_create_params *params;
  int stream;
  int read_fd;
  int write_fd;
  int thread_started;
  pthread_t thread;
  int rc;
  cai_error error;
  lonejson_error json_error;
} cai_http_response_params_upload;

typedef struct cai_api_error_body {
  char *message;
  char *type;
  char *code;
} cai_api_error_body;

typedef struct cai_api_error_doc {
  cai_api_error_body error;
} cai_api_error_doc;

typedef enum cai_http_response_mode {
  CAI_HTTP_RESPONSE_PARSE = 0,
  CAI_HTTP_RESPONSE_IGNORE = 1
} cai_http_response_mode;

static const lonejson_field cai_api_error_body_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_api_error_body, message, "message"),
    LONEJSON_FIELD_STRING_ALLOC(cai_api_error_body, type, "type"),
    LONEJSON_FIELD_STRING_ALLOC(cai_api_error_body, code, "code")};
LONEJSON_MAP_DEFINE(cai_api_error_body_map, cai_api_error_body,
                    cai_api_error_body_fields);

static const lonejson_field cai_api_error_fields[] = {LONEJSON_FIELD_OBJECT(
    cai_api_error_doc, error, "error", &cai_api_error_body_map)};
LONEJSON_MAP_DEFINE(cai_api_error_map, cai_api_error_doc, cai_api_error_fields);

static size_t cai_http_write(char *ptr, size_t size, size_t nmemb,
                             void *userdata) {
  cai_http_buffer *buffer;
  size_t count;
  size_t needed;
  char *grown;

  buffer = (cai_http_buffer *)userdata;
  count = size * nmemb;
  needed = buffer->length + count + 1U;
  if (needed > buffer->capacity) {
    size_t new_capacity;

    new_capacity = buffer->capacity == 0U ? 1024U : buffer->capacity;
    while (new_capacity < needed) {
      new_capacity *= 2U;
    }
    grown = (char *)cai_realloc_mem(NULL, buffer->data, new_capacity);
    if (grown == NULL) {
      return 0U;
    }
    buffer->data = grown;
    buffer->capacity = new_capacity;
  }
  if (count > 0U) {
    memcpy(buffer->data + buffer->length, ptr, count);
    buffer->length += count;
  }
  buffer->data[buffer->length] = '\0';
  return count;
}

static size_t cai_http_header_write(char *ptr, size_t size, size_t nmemb,
                                    void *userdata) {
  static const char request_id_header[] = "x-request-id:";
  char **request_id;
  size_t count;
  char *start;
  char *end;
  char *copy;

  request_id = (char **)userdata;
  count = size * nmemb;
  if (request_id == NULL || *request_id != NULL ||
      count <= sizeof(request_id_header) - 1U) {
    return count;
  }
  if (strncasecmp(ptr, request_id_header, sizeof(request_id_header) - 1U) !=
      0) {
    return count;
  }
  start = ptr + sizeof(request_id_header) - 1U;
  end = ptr + count;
  while (start < end && (*start == ' ' || *start == '\t')) {
    start++;
  }
  while (end > start && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ' ||
                         end[-1] == '\t')) {
    end--;
  }
  if (end <= start) {
    return count;
  }
  copy = cai_strndup(NULL, start, (size_t)(end - start));
  if (copy == NULL) {
    return 0U;
  }
  *request_id = copy;
  return count;
}

static size_t cai_http_spooled_read(char *ptr, size_t size, size_t nmemb,
                                    void *userdata) {
  cai_http_spooled_upload *upload;
  lonejson_read_result result;
  size_t capacity;

  upload = (cai_http_spooled_upload *)userdata;
  capacity = size * nmemb;
  if (upload == NULL || ptr == NULL || capacity == 0U) {
    return 0U;
  }
  result =
      lonejson_spooled_read(&upload->cursor, (unsigned char *)ptr, capacity);
  if (result.error_code != 0) {
    return CURL_READFUNC_ABORT;
  }
  return result.bytes_read;
}

static ssize_t cai_http_socket_write_no_sigpipe(int fd, const char *data,
                                                size_t len) {
#ifdef MSG_NOSIGNAL
  return send(fd, data, len, MSG_NOSIGNAL);
#else
  return write(fd, data, len);
#endif
}

static lonejson_status cai_http_response_params_upload_sink(
    void *user, const void *data, size_t len, lonejson_error *error) {
  cai_http_response_params_upload *upload;
  const char *bytes;
  size_t offset;

  upload = (cai_http_response_params_upload *)user;
  bytes = (const char *)data;
  offset = 0U;
  while (offset < len) {
    ssize_t written;

    written = cai_http_socket_write_no_sigpipe(upload->write_fd,
                                               bytes + offset, len - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (error != NULL) {
        error->code = LONEJSON_STATUS_IO_ERROR;
        error->system_errno = errno;
        snprintf(error->message, sizeof(error->message),
                 "failed to stream request JSON");
      }
      return LONEJSON_STATUS_IO_ERROR;
    }
    if (written == 0) {
      if (error != NULL) {
        error->code = LONEJSON_STATUS_IO_ERROR;
        snprintf(error->message, sizeof(error->message),
                 "request JSON stream closed");
      }
      return LONEJSON_STATUS_IO_ERROR;
    }
    offset += (size_t)written;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_http_count_sink(void *user, const void *data,
                                           size_t len, lonejson_error *error) {
  size_t *count;

  (void)data;
  (void)error;
  count = (size_t *)user;
  *count += len;
  return LONEJSON_STATUS_OK;
}

static void *cai_http_response_params_upload_main(void *arg) {
  cai_http_response_params_upload *upload;

  upload = (cai_http_response_params_upload *)arg;
  upload->rc = cai_response_create_params_write_json_sink(
      upload->params, upload->stream, cai_http_response_params_upload_sink,
      upload, &upload->json_error, NULL, &upload->error);
  if (upload->write_fd >= 0) {
    close(upload->write_fd);
    upload->write_fd = -1;
  }
  return NULL;
}

static int cai_http_response_params_upload_start(
    cai_http_response_params_upload *upload,
    const cai_response_create_params *params, int stream, cai_error *error) {
  int fds[2];

  memset(upload, 0, sizeof(*upload));
  upload->params = params;
  upload->stream = stream;
  upload->read_fd = -1;
  upload->write_fd = -1;
  upload->rc = CAI_OK;
  cai_error_init(&upload->error);
  lonejson_error_init(&upload->json_error);
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to create request upload stream");
  }
#ifdef SO_NOSIGPIPE
  {
    int one;

    one = 1;
    (void)setsockopt(fds[1], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
  }
#endif
  upload->read_fd = fds[0];
  upload->write_fd = fds[1];
  if (pthread_create(&upload->thread, NULL,
                     cai_http_response_params_upload_main, upload) != 0) {
    close(upload->read_fd);
    close(upload->write_fd);
    upload->read_fd = -1;
    upload->write_fd = -1;
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to start request upload stream");
  }
  upload->thread_started = 1;
  return CAI_OK;
}

static size_t cai_http_response_params_upload_read(char *ptr, size_t size,
                                                   size_t nmemb,
                                                   void *userdata) {
  cai_http_response_params_upload *upload;
  size_t capacity;

  upload = (cai_http_response_params_upload *)userdata;
  capacity = size * nmemb;
  if (upload == NULL || ptr == NULL || capacity == 0U ||
      upload->read_fd < 0) {
    return 0U;
  }
  for (;;) {
    ssize_t got;

    got = read(upload->read_fd, ptr, capacity);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      return CURL_READFUNC_ABORT;
    }
    return (size_t)got;
  }
}

static int cai_http_response_params_upload_finish(
    cai_http_response_params_upload *upload, cai_error *error) {
  int rc;

  if (upload->read_fd >= 0) {
    close(upload->read_fd);
    upload->read_fd = -1;
  }
  if (upload->write_fd >= 0) {
    close(upload->write_fd);
    upload->write_fd = -1;
  }
  if (upload->thread_started) {
    pthread_join(upload->thread, NULL);
    upload->thread_started = 0;
  }
  rc = upload->rc;
  if (rc != CAI_OK && error != NULL) {
    (void)cai_set_error_detail(
        error, rc,
        upload->error.message != NULL ? upload->error.message
                                      : "failed to stream request JSON",
        upload->error.detail != NULL ? upload->error.detail
                                     : upload->json_error.message);
  }
  cai_error_cleanup(&upload->error);
  return rc;
}

int cai_set_openai_error(cai_error *error, long http_status, const char *body,
                         const char *request_id) {
  cai_api_error_doc doc;
  lonejson_error json_error;
  lonejson_status status;
  const char *detail;
  const char *server_code;
  int rc;

  detail = body != NULL ? body : "";
  server_code = NULL;
  memset(&doc, 0, sizeof(doc));
  lonejson_init(&cai_api_error_map, &doc);
  status =
      lonejson_parse_cstr(&cai_api_error_map, &doc, detail, NULL, &json_error);
  if (status == LONEJSON_STATUS_OK && doc.error.message != NULL) {
    detail = doc.error.message;
    server_code = doc.error.code != NULL ? doc.error.code : doc.error.type;
  }
  rc = cai_set_error_http(error, CAI_ERR_SERVER, http_status,
                          "OpenAI API request failed", detail, server_code,
                          request_id);
  lonejson_cleanup(&cai_api_error_map, &doc);
  return rc;
}

int cai_build_url(const cai_allocator *allocator, const char *base_url,
                  const char *path, char **out, cai_error *error) {
  size_t base_len;
  size_t path_len;
  size_t slash;
  char *url;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "URL output is required");
  }
  *out = NULL;
  if (base_url == NULL || path == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "base URL and path are required");
  }
  base_len = strlen(base_url);
  path_len = strlen(path);
  slash = base_len > 0U && base_url[base_len - 1U] == '/' ? 0U : 1U;
  url = (char *)cai_alloc(allocator, base_len + slash + path_len + 1U);
  if (url == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate URL");
  }
  memcpy(url, base_url, base_len);
  if (slash != 0U) {
    url[base_len] = '/';
  }
  memcpy(url + base_len + slash, path, path_len);
  url[base_len + slash + path_len] = '\0';
  *out = url;
  return CAI_OK;
}

static int cai_build_response_path(const cai_allocator *allocator,
                                   const char *response_id, const char *suffix,
                                   char **out, cai_error *error) {
  static const char prefix[] = "responses/";
  size_t prefix_len;
  size_t id_len;
  size_t suffix_len;
  char *path;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "path output is required");
  }
  *out = NULL;
  if (response_id == NULL || response_id[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "response id is required");
  }
  prefix_len = sizeof(prefix) - 1U;
  id_len = strlen(response_id);
  suffix_len = suffix != NULL ? strlen(suffix) : 0U;
  path = (char *)cai_alloc(allocator, prefix_len + id_len + suffix_len + 1U);
  if (path == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate response path");
  }
  memcpy(path, prefix, prefix_len);
  memcpy(path + prefix_len, response_id, id_len);
  if (suffix_len > 0U) {
    memcpy(path + prefix_len + id_len, suffix, suffix_len);
  }
  path[prefix_len + id_len + suffix_len] = '\0';
  *out = path;
  return CAI_OK;
}

static int cai_query_is_unreserved(unsigned char ch) {
  return (ch >= (unsigned char)'A' && ch <= (unsigned char)'Z') ||
         (ch >= (unsigned char)'a' && ch <= (unsigned char)'z') ||
         (ch >= (unsigned char)'0' && ch <= (unsigned char)'9') ||
         ch == (unsigned char)'-' || ch == (unsigned char)'_' ||
         ch == (unsigned char)'.' || ch == (unsigned char)'~';
}

static int cai_append_query_piece(const cai_allocator *allocator, char **path,
                                  size_t *length, size_t *capacity,
                                  const char *key, const char *value,
                                  int *has_query, cai_error *error) {
  static const char hex[] = "0123456789ABCDEF";
  size_t key_len;
  size_t value_len;
  size_t extra;
  size_t needed;
  size_t i;
  char *grown;
  char *cursor;
  unsigned char ch;

  key_len = strlen(key);
  value_len = strlen(value);
  extra = 1U + key_len + 1U;
  for (i = 0U; i < value_len; i++) {
    ch = (unsigned char)value[i];
    extra += cai_query_is_unreserved(ch) ? 1U : 3U;
  }
  needed = *length + extra + 1U;
  if (needed > *capacity) {
    size_t new_capacity;

    new_capacity = *capacity == 0U ? 128U : *capacity;
    while (new_capacity < needed) {
      new_capacity *= 2U;
    }
    grown = (char *)cai_realloc_mem(allocator, *path, new_capacity);
    if (grown == NULL) {
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate query path");
    }
    *path = grown;
    *capacity = new_capacity;
  }
  cursor = *path + *length;
  *cursor++ = *has_query ? '&' : '?';
  *has_query = 1;
  memcpy(cursor, key, key_len);
  cursor += key_len;
  *cursor++ = '=';
  for (i = 0U; i < value_len; i++) {
    ch = (unsigned char)value[i];
    if (cai_query_is_unreserved(ch)) {
      *cursor++ = (char)ch;
    } else {
      *cursor++ = '%';
      *cursor++ = hex[(ch >> 4U) & 0x0FU];
      *cursor++ = hex[ch & 0x0FU];
    }
  }
  *cursor = '\0';
  *length = (size_t)(cursor - *path);
  return CAI_OK;
}

int cai_append_list_query_params(const cai_allocator *allocator, char **path,
                                 const cai_list_params *params,
                                 cai_error *error) {
  char limit_text[32];
  size_t length;
  size_t capacity;
  int has_query;
  int rc;

  if (path == NULL || *path == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "path is required");
  }
  if (params == NULL) {
    return CAI_OK;
  }
  if (params->limit < 0 || params->limit > 100) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "list limit must be between 1 and 100");
  }
  if (params->order != NULL && strcmp(params->order, "asc") != 0 &&
      strcmp(params->order, "desc") != 0) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "list order must be asc or desc");
  }
  length = strlen(*path);
  capacity = length + 1U;
  has_query = strchr(*path, '?') != NULL;
  if (params->after != NULL) {
    rc = cai_append_query_piece(allocator, path, &length, &capacity, "after",
                                params->after, &has_query, error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  if (params->limit > 0) {
    snprintf(limit_text, sizeof(limit_text), "%d", params->limit);
    rc = cai_append_query_piece(allocator, path, &length, &capacity, "limit",
                                limit_text, &has_query, error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  if (params->order != NULL) {
    rc = cai_append_query_piece(allocator, path, &length, &capacity, "order",
                                params->order, &has_query, error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  return CAI_OK;
}

static int cai_build_response_input_items_path(const cai_allocator *allocator,
                                               const char *response_id,
                                               const cai_list_params *params,
                                               char **out, cai_error *error) {
  int rc;

  rc = cai_build_response_path(allocator, response_id, "/input_items", out,
                               error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_append_list_query_params(allocator, out, params, error);
  if (rc != CAI_OK) {
    cai_free_mem(allocator, *out);
    *out = NULL;
    return rc;
  }
  return CAI_OK;
}

int cai_append_header(struct curl_slist **headers, const char *header,
                      cai_error *error) {
  struct curl_slist *next;

  next = curl_slist_append(*headers, header);
  if (next == NULL) {
    curl_slist_free_all(*headers);
    *headers = NULL;
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate HTTP header");
  }
  *headers = next;
  return CAI_OK;
}

int cai_append_bearer_header(cai_client *client, struct curl_slist **headers,
                             cai_error *error) {
  static const char prefix[] = "Authorization: Bearer ";
  size_t prefix_len;
  size_t key_len;
  char *header;
  int rc;

  prefix_len = sizeof(prefix) - 1U;
  key_len = strlen(CAI_CLIENT_IMPL(client)->api_key);
  header = (char *)cai_alloc(&CAI_CLIENT_IMPL(client)->allocator, prefix_len + key_len + 1U);
  if (header == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate authorization header");
  }
  memcpy(header, prefix, prefix_len);
  memcpy(header + prefix_len, CAI_CLIENT_IMPL(client)->api_key, key_len);
  header[prefix_len + key_len] = '\0';
  rc = cai_append_header(headers, header, error);
  cai_free_mem(&CAI_CLIENT_IMPL(client)->allocator, header);
  return rc;
}

int cai_append_prefixed_header(cai_client *client, struct curl_slist **headers,
                               const char *prefix, const char *value,
                               cai_error *error) {
  size_t prefix_len;
  size_t value_len;
  char *header;
  int rc;

  if (value == NULL) {
    return CAI_OK;
  }
  prefix_len = strlen(prefix);
  value_len = strlen(value);
  header = (char *)cai_alloc(&CAI_CLIENT_IMPL(client)->allocator, prefix_len + value_len + 1U);
  if (header == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate HTTP header");
  }
  memcpy(header, prefix, prefix_len);
  memcpy(header + prefix_len, value, value_len);
  header[prefix_len + value_len] = '\0';
  rc = cai_append_header(headers, header, error);
  cai_free_mem(&CAI_CLIENT_IMPL(client)->allocator, header);
  return rc;
}

int cai_http_json_request(cai_client *client, const char *method,
                          const char *path, const char *request_json,
                          char **out_json, long *out_http_status,
                          char **out_request_id, cai_error *error) {
  lonejson_spooled spooled;
  lonejson_error json_error;
  size_t len;

  if (request_json == NULL) {
    return cai_http_json_request_spooled(client, method, path, NULL, 0U,
                                         out_json, out_http_status,
                                         out_request_id, error);
  }
  lonejson_error_init(&json_error);
  lonejson_spooled_init(&spooled, NULL);
  len = strlen(request_json);
  if (lonejson_spooled_append(&spooled, request_json, len, &json_error) !=
      LONEJSON_STATUS_OK) {
    lonejson_spooled_cleanup(&spooled);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to spool request JSON",
                                json_error.message);
  }
  {
    int rc = cai_http_json_request_spooled(
        client, method, path, &spooled, len, out_json, out_http_status,
        out_request_id, error);
    lonejson_spooled_cleanup(&spooled);
    return rc;
  }
}

int cai_http_json_request_spooled(cai_client *client, const char *method,
                                  const char *path,
                                  const lonejson_spooled *request_json,
                                  size_t request_json_len, char **out_json,
                                  long *out_http_status,
                                  char **out_request_id, cai_error *error) {
  CURL *curl;
  CURLcode curl_rc;
  struct curl_slist *headers;
  cai_http_buffer body;
  cai_http_spooled_upload upload;
  lonejson_error json_error;
  char *url;
  char *request_id;
  long http_status;
  int rc;

  if (out_json == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "JSON output pointer is required");
  }
  *out_json = NULL;
  if (out_http_status != NULL) {
    *out_http_status = 0L;
  }
  if (out_request_id != NULL) {
    *out_request_id = NULL;
  }
  if (client == NULL || method == NULL || path == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "client, method, and path are required");
  }
  url = NULL;
  headers = NULL;
  body.data = NULL;
  body.length = 0U;
  body.capacity = 0U;
  request_id = NULL;
  upload.initialized = 0;

  rc = cai_build_url(&CAI_CLIENT_IMPL(client)->allocator, CAI_CLIENT_IMPL(client)->base_url, path, &url, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_append_header(&headers, "Content-Type: application/json", error);
  if (rc == CAI_OK) {
    rc = cai_append_header(&headers, "Accept: application/json", error);
  }
  if (rc == CAI_OK) {
    rc = cai_append_bearer_header(client, &headers, error);
  }
  if (rc == CAI_OK) {
    rc = cai_append_prefixed_header(client, &headers, "OpenAI-Organization: ",
                                    CAI_CLIENT_IMPL(client)->organization_id, error);
  }
  if (rc == CAI_OK) {
    rc = cai_append_prefixed_header(
        client, &headers, "OpenAI-Project: ", CAI_CLIENT_IMPL(client)->project_id, error);
  }
  if (rc != CAI_OK) {
    cai_free_mem(&CAI_CLIENT_IMPL(client)->allocator, url);
    return rc;
  }

  curl = curl_easy_init();
  if (curl == NULL) {
    curl_slist_free_all(headers);
    cai_free_mem(&CAI_CLIENT_IMPL(client)->allocator, url);
    return cai_set_error(error, CAI_ERR_TRANSPORT, "failed to initialize curl");
  }
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  if (strcmp(method, "POST") == 0) {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
  } else {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
  }
  if (request_json != NULL) {
    upload.cursor = *request_json;
    upload.initialized = 1;
    lonejson_error_init(&json_error);
    if (lonejson_spooled_rewind(&upload.cursor, &json_error) !=
        LONEJSON_STATUS_OK) {
      curl_easy_cleanup(curl);
      curl_slist_free_all(headers);
      cai_free_mem(&CAI_CLIENT_IMPL(client)->allocator, url);
      return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                  "failed to rewind request JSON",
                                  json_error.message);
    }
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, cai_http_spooled_read);
    curl_easy_setopt(curl, CURLOPT_READDATA, &upload);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                     (curl_off_t)request_json_len);
    if (strcmp(method, "POST") != 0) {
      curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    }
  }
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cai_http_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, cai_http_header_write);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &request_id);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  if (CAI_CLIENT_IMPL(client)->timeout_ms > 0L) {
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, CAI_CLIENT_IMPL(client)->timeout_ms);
  }
  if (CAI_CLIENT_IMPL(client)->http_2_disabled) {
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
  } else {
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
  }
  if (CAI_CLIENT_IMPL(client)->insecure_skip_verify) {
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  }

  curl_rc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);
  cai_free_mem(&CAI_CLIENT_IMPL(client)->allocator, url);

  if (out_http_status != NULL) {
    *out_http_status = http_status;
  }
  if (curl_rc != CURLE_OK) {
    cai_free_mem(NULL, body.data);
    cai_free_mem(NULL, request_id);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "HTTP request transport failed",
                                curl_easy_strerror(curl_rc));
  }
  *out_json = body.data != NULL ? body.data : cai_strdup(NULL, "");
  if (*out_json == NULL) {
    cai_free_mem(NULL, request_id);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate empty response body");
  }
  if (out_request_id != NULL) {
    *out_request_id = request_id;
  } else {
    cai_free_mem(NULL, request_id);
  }
  return CAI_OK;
}

static int cai_http_response_request(cai_client *client, const char *method,
                                     const char *path, const char *request_json,
                                     cai_http_response_mode mode,
                                     cai_response **out, cai_error *error) {
  char *body;
  char *request_id;
  long http_status;
  int rc;

  if (out != NULL) {
    *out = NULL;
  }
  if (client == NULL || method == NULL || path == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "client, method, and path are required");
  }
  body = NULL;
  request_id = NULL;
  rc = cai_http_json_request(client, method, path, request_json, &body,
                             &http_status, &request_id, error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (http_status < 200L || http_status >= 300L) {
    rc = cai_set_openai_error(error, http_status, body, request_id);
    cai_free_mem(NULL, body);
    cai_free_mem(NULL, request_id);
    return rc;
  }
  cai_free_mem(NULL, request_id);
  if (mode == CAI_HTTP_RESPONSE_IGNORE) {
    cai_free_mem(NULL, body);
    return CAI_OK;
  }
  if (out == NULL) {
    cai_free_mem(NULL, body);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response output pointer is required");
  }
  rc = cai_response_parse_json(body != NULL ? body : "", out, error);
  cai_free_mem(NULL, body);
  return rc;
}

int cai_http_response_params_request(cai_client *client, const char *path,
                                     const cai_response_create_params *params,
                                     int stream, char **out_json,
                                     long *out_http_status,
                                     char **out_request_id, cai_error *error) {
  CURL *curl;
  CURLcode curl_rc;
  struct curl_slist *headers;
  cai_http_buffer body;
  cai_http_response_params_upload upload;
  char *url;
  char *request_id;
  long http_status;
  int upload_started;
  int rc;
  size_t request_len;
  lonejson_error json_error;

  if (out_json == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "JSON output pointer is required");
  }
  *out_json = NULL;
  if (out_http_status != NULL) {
    *out_http_status = 0L;
  }
  if (out_request_id != NULL) {
    *out_request_id = NULL;
  }
  if (client == NULL || path == NULL || params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "client, path, and response params are required");
  }
  url = NULL;
  headers = NULL;
  body.data = NULL;
  body.length = 0U;
  body.capacity = 0U;
  request_id = NULL;
  upload_started = 0;
  request_len = 0U;
  lonejson_error_init(&json_error);
  memset(&upload, 0, sizeof(upload));

  rc = cai_response_create_params_write_json_sink(
      params, stream, cai_http_count_sink, &request_len, &json_error, NULL,
      error);
  if (rc == CAI_OK) {
    rc = cai_build_url(&CAI_CLIENT_IMPL(client)->allocator,
                       CAI_CLIENT_IMPL(client)->base_url, path, &url, error);
  }
  if (rc == CAI_OK) {
    rc = cai_append_header(&headers, "Content-Type: application/json", error);
  }
  if (rc == CAI_OK) {
    rc = cai_append_header(&headers, "Accept: application/json", error);
  }
  if (rc == CAI_OK) {
    rc = cai_append_header(&headers, "Expect:", error);
  }
  if (rc == CAI_OK) {
    rc = cai_append_bearer_header(client, &headers, error);
  }
  if (rc == CAI_OK) {
    rc = cai_append_prefixed_header(client, &headers, "OpenAI-Organization: ",
                                    CAI_CLIENT_IMPL(client)->organization_id,
                                    error);
  }
  if (rc == CAI_OK) {
    rc = cai_append_prefixed_header(client, &headers, "OpenAI-Project: ",
                                    CAI_CLIENT_IMPL(client)->project_id,
                                    error);
  }
  if (rc != CAI_OK) {
    curl_slist_free_all(headers);
    cai_free_mem(&CAI_CLIENT_IMPL(client)->allocator, url);
    return rc;
  }
  curl = curl_easy_init();
  if (curl == NULL) {
    curl_slist_free_all(headers);
    cai_free_mem(&CAI_CLIENT_IMPL(client)->allocator, url);
    return cai_set_error(error, CAI_ERR_TRANSPORT, "failed to initialize curl");
  }
  rc = cai_http_response_params_upload_start(&upload, params, stream, error);
  if (rc == CAI_OK) {
    upload_started = 1;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION,
                     cai_http_response_params_upload_read);
    curl_easy_setopt(curl, CURLOPT_READDATA, &upload);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                     (curl_off_t)request_len);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cai_http_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, cai_http_header_write);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &request_id);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (CAI_CLIENT_IMPL(client)->timeout_ms > 0L) {
      curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                       CAI_CLIENT_IMPL(client)->timeout_ms);
    }
    if (CAI_CLIENT_IMPL(client)->http_2_disabled) {
      curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,
                       (long)CURL_HTTP_VERSION_1_1);
    } else {
      curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,
                       (long)CURL_HTTP_VERSION_2TLS);
    }
    if (CAI_CLIENT_IMPL(client)->insecure_skip_verify) {
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    curl_rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
  } else {
    curl_rc = CURLE_OK;
    http_status = 0L;
  }
  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);
  cai_free_mem(&CAI_CLIENT_IMPL(client)->allocator, url);
  if (upload_started) {
    int upload_rc;

    upload_rc = cai_http_response_params_upload_finish(&upload, error);
    if (rc == CAI_OK && upload_rc != CAI_OK) {
      rc = upload_rc;
    }
  }
  if (out_http_status != NULL) {
    *out_http_status = http_status;
  }
  if (rc != CAI_OK) {
    cai_free_mem(NULL, body.data);
    cai_free_mem(NULL, request_id);
    return rc;
  }
  if (curl_rc != CURLE_OK) {
    cai_free_mem(NULL, body.data);
    cai_free_mem(NULL, request_id);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "HTTP request transport failed",
                                curl_easy_strerror(curl_rc));
  }
  *out_json = body.data != NULL ? body.data : cai_strdup(NULL, "");
  if (*out_json == NULL) {
    cai_free_mem(NULL, request_id);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate empty response body");
  }
  if (out_request_id != NULL) {
    *out_request_id = request_id;
  } else {
    cai_free_mem(NULL, request_id);
  }
  return CAI_OK;
}

int cai_client_create_response(cai_client *client,
                               const cai_response_create_params *params,
                               cai_response **out, cai_error *error) {
  int rc;

  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  {
    char *body;
    char *request_id;
    long http_status;

    body = NULL;
    request_id = NULL;
    rc = cai_http_response_params_request(client, "responses", params, 0, &body,
                                          &http_status, &request_id, error);
    if (rc == CAI_OK && (http_status < 200L || http_status >= 300L)) {
      rc = cai_set_openai_error(error, http_status, body, request_id);
    }
    if (rc == CAI_OK) {
      rc = cai_response_parse_json(body != NULL ? body : "", out, error);
    }
    cai_free_mem(NULL, body);
    cai_free_mem(NULL, request_id);
  }
  return rc;
}

int cai_client_retrieve_response(cai_client *client, const char *response_id,
                                 cai_response **out, cai_error *error) {
  char *path;
  int rc;

  path = NULL;
  rc = cai_build_response_path(client != NULL ? &CAI_CLIENT_IMPL(client)->allocator : NULL,
                               response_id, NULL, &path, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_http_response_request(client, "GET", path, NULL,
                                 CAI_HTTP_RESPONSE_PARSE, out, error);
  cai_free_mem(client != NULL ? &CAI_CLIENT_IMPL(client)->allocator : NULL, path);
  return rc;
}

int cai_client_cancel_response(cai_client *client, const char *response_id,
                               cai_response **out, cai_error *error) {
  char *path;
  int rc;

  path = NULL;
  rc = cai_build_response_path(client != NULL ? &CAI_CLIENT_IMPL(client)->allocator : NULL,
                               response_id, "/cancel", &path, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_http_response_request(client, "POST", path, NULL,
                                 CAI_HTTP_RESPONSE_PARSE, out, error);
  cai_free_mem(client != NULL ? &CAI_CLIENT_IMPL(client)->allocator : NULL, path);
  return rc;
}

int cai_client_delete_response(cai_client *client, const char *response_id,
                               cai_error *error) {
  char *path;
  int rc;

  path = NULL;
  rc = cai_build_response_path(client != NULL ? &CAI_CLIENT_IMPL(client)->allocator : NULL,
                               response_id, NULL, &path, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_http_response_request(client, "DELETE", path, NULL,
                                 CAI_HTTP_RESPONSE_IGNORE, NULL, error);
  cai_free_mem(client != NULL ? &CAI_CLIENT_IMPL(client)->allocator : NULL, path);
  return rc;
}

int cai_client_list_response_input_items(cai_client *client,
                                         const char *response_id,
                                         const cai_list_params *params,
                                         cai_input_item_list **out,
                                         cai_error *error) {
  char *path;
  char *body;
  char *request_id;
  long http_status;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input item list output pointer is required");
  }
  *out = NULL;
  path = NULL;
  body = NULL;
  request_id = NULL;
  rc = cai_build_response_input_items_path(client != NULL ? &CAI_CLIENT_IMPL(client)->allocator
                                                          : NULL,
                                           response_id, params, &path, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_http_json_request(client, "GET", path, NULL, &body, &http_status,
                             &request_id, error);
  cai_free_mem(client != NULL ? &CAI_CLIENT_IMPL(client)->allocator : NULL, path);
  if (rc != CAI_OK) {
    return rc;
  }
  if (http_status < 200L || http_status >= 300L) {
    rc = cai_set_openai_error(error, http_status, body, request_id);
    cai_free_mem(NULL, body);
    cai_free_mem(NULL, request_id);
    return rc;
  }
  rc = cai_input_item_list_parse_json(body, out, error);
  cai_free_mem(NULL, body);
  cai_free_mem(NULL, request_id);
  return rc;
}
