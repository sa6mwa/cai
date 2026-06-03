#include "cai_internal.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct cai_http_buffer {
  char *data;
  size_t length;
  size_t capacity;
  size_t limit;
  int exceeded;
} cai_http_buffer;

typedef struct cai_http_spooled_upload {
  lonejson_spooled cursor;
  int initialized;
} cai_http_spooled_upload;

typedef struct cai_api_error_body {
  char *message;
  char *type;
  char *code;
} cai_api_error_body;

typedef struct cai_api_error_doc {
  cai_api_error_body error;
} cai_api_error_doc;

typedef struct cai_input_token_count_doc {
  char *object;
  long long input_tokens;
} cai_input_token_count_doc;

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

static const lonejson_field cai_input_token_count_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_input_token_count_doc, object, "object"),
    LONEJSON_FIELD_I64(cai_input_token_count_doc, input_tokens,
                       "input_tokens")};
LONEJSON_MAP_DEFINE(cai_input_token_count_map, cai_input_token_count_doc,
                    cai_input_token_count_fields);

static size_t cai_http_write(char *ptr, size_t size, size_t nmemb,
                             void *userdata) {
  cai_http_buffer *buffer;
  size_t count;
  size_t needed;
  char *grown;

  buffer = (cai_http_buffer *)userdata;
  count = size * nmemb;
  if (size != 0U && count / size != nmemb) {
    return 0U;
  }
  if (buffer->limit > 0U && (buffer->length >= buffer->limit ||
                             count > buffer->limit - buffer->length)) {
    buffer->exceeded = 1;
    return 0U;
  }
  needed = buffer->length + count + 1U;
  if (needed < buffer->length || needed < count) {
    return 0U;
  }
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
  result = upload->cursor.read(&upload->cursor, (unsigned char *)ptr, capacity);
  if (result.error_code != 0) {
    return CURL_READFUNC_ABORT;
  }
  return result.bytes_read;
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
  CAI_LJ->init(CAI_LJ, &cai_api_error_map, &doc);
  status =
      CAI_LJ->parse_cstr(CAI_LJ, &cai_api_error_map, &doc, detail, &json_error);
  if (status == LONEJSON_STATUS_OK && doc.error.message != NULL) {
    detail = doc.error.message;
    server_code = doc.error.code != NULL ? doc.error.code : doc.error.type;
  }
  rc = cai_set_error_http(error, CAI_ERR_SERVER, http_status,
                          "OpenAI API request failed", detail, server_code,
                          request_id);
  CAI_LJ->cleanup(CAI_LJ, &cai_api_error_map, &doc);
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
  header = (char *)cai_alloc(&CAI_CLIENT_IMPL(client)->allocator,
                             prefix_len + key_len + 1U);
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
  header = (char *)cai_alloc(&CAI_CLIENT_IMPL(client)->allocator,
                             prefix_len + value_len + 1U);
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
  CAI_LJ->spooled_init(CAI_LJ, &spooled);
  len = strlen(request_json);
  if (spooled.append(&spooled, request_json, len, &json_error) !=
      LONEJSON_STATUS_OK) {
    spooled.cleanup(&spooled);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to spool request JSON",
                                json_error.message);
  }
  {
    int rc = cai_http_json_request_spooled(client, method, path, &spooled, len,
                                           out_json, out_http_status,
                                           out_request_id, error);
    spooled.cleanup(&spooled);
    return rc;
  }
}

int cai_http_json_request_spooled(cai_client *client, const char *method,
                                  const char *path,
                                  const lonejson_spooled *request_json,
                                  size_t request_json_len, char **out_json,
                                  long *out_http_status, char **out_request_id,
                                  cai_error *error) {
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
  int retried_auth;

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
  retried_auth = 0;
retry_request:
  url = NULL;
  headers = NULL;
  body.data = NULL;
  body.length = 0U;
  body.capacity = 0U;
  body.limit = CAI_CLIENT_IMPL(client)->json_response_limit_bytes;
  body.exceeded = 0;
  request_id = NULL;
  upload.initialized = 0;

  rc = cai_build_url(&CAI_CLIENT_IMPL(client)->allocator,
                     CAI_CLIENT_IMPL(client)->base_url, path, &url, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_append_header(&headers, "Content-Type: application/json", error);
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
                                    CAI_CLIENT_IMPL(client)->project_id, error);
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
    if (request_json == NULL) {
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)0);
    }
  } else {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
  }
  if (request_json != NULL) {
    upload.cursor = *request_json;
    upload.initialized = 1;
    lonejson_error_init(&json_error);
    if (upload.cursor.rewind(&upload.cursor, &json_error) !=
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
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                     CAI_CLIENT_IMPL(client)->timeout_ms);
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

  cai_log_http_request_start(CAI_CLIENT_IMPL(client), method, path, 0,
                             request_json != NULL ? request_json_len : 0U);
  curl_rc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
  if (curl_rc == CURLE_OK) {
    cai_log_http_request_done(CAI_CLIENT_IMPL(client), method, path,
                              http_status, body.length, request_id);
  }
  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);
  cai_free_mem(&CAI_CLIENT_IMPL(client)->allocator, url);

  if (out_http_status != NULL) {
    *out_http_status = http_status;
  }
  if (curl_rc == CURLE_OK && !retried_auth &&
      (http_status == 401L || http_status == 403L) &&
      CAI_CLIENT_IMPL(client)->chatgpt_auth != NULL) {
    cai_free_mem(NULL, body.data);
    cai_free_mem(NULL, request_id);
    rc = cai_client_refresh_chatgpt_auth_after_http(client, http_status, error);
    if (rc != CAI_OK) {
      return rc;
    }
    retried_auth = 1;
    goto retry_request;
  }
  if (curl_rc != CURLE_OK) {
    cai_free_mem(NULL, body.data);
    cai_free_mem(NULL, request_id);
    if (body.exceeded) {
      cai_log_http_response_limit(CAI_CLIENT_IMPL(client), method, path,
                                  body.limit);
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "HTTP response exceeded configured JSON response "
                           "limit");
    }
    cai_log_http_transport_error(CAI_CLIENT_IMPL(client), method, path,
                                 curl_easy_strerror(curl_rc));
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
  rc = cai_response_parse_json_with_allocator(
      &CAI_CLIENT_IMPL(client)->allocator, body != NULL ? body : "", out,
      error);
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
  cai_response_request_upload *upload;
  char *url;
  char *request_id;
  long http_status;
  int rc;
  int retried_auth;

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
  retried_auth = 0;
retry_request:
  url = NULL;
  headers = NULL;
  body.data = NULL;
  body.length = 0U;
  body.capacity = 0U;
  body.limit = CAI_CLIENT_IMPL(client)->json_response_limit_bytes;
  body.exceeded = 0;
  request_id = NULL;
  upload = NULL;

  rc = cai_response_request_upload_open(params, stream, &upload, error);
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
                                    CAI_CLIENT_IMPL(client)->project_id, error);
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
    cai_response_request_upload_close(upload);
    return cai_set_error(error, CAI_ERR_TRANSPORT, "failed to initialize curl");
  }
  if (rc == CAI_OK) {
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION,
                     cai_response_request_upload_read);
    curl_easy_setopt(curl, CURLOPT_READDATA, upload);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                     cai_response_request_upload_size(upload));
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
      curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
    } else {
      curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,
                       (long)CURL_HTTP_VERSION_2TLS);
    }
    if (CAI_CLIENT_IMPL(client)->insecure_skip_verify) {
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    cai_log_http_request_start(
        CAI_CLIENT_IMPL(client), "POST", path, stream,
        (size_t)cai_response_request_upload_size(upload));
    curl_rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    if (curl_rc == CURLE_OK) {
      cai_log_http_request_done(CAI_CLIENT_IMPL(client), "POST", path,
                                http_status, body.length, request_id);
    }
  } else {
    curl_rc = CURLE_OK;
    http_status = 0L;
  }
  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);
  cai_free_mem(&CAI_CLIENT_IMPL(client)->allocator, url);
  cai_response_request_upload_close(upload);
  if (out_http_status != NULL) {
    *out_http_status = http_status;
  }
  if (rc != CAI_OK) {
    cai_free_mem(NULL, body.data);
    cai_free_mem(NULL, request_id);
    return rc;
  }
  if (curl_rc == CURLE_OK && !retried_auth &&
      (http_status == 401L || http_status == 403L) &&
      CAI_CLIENT_IMPL(client)->chatgpt_auth != NULL) {
    cai_free_mem(NULL, body.data);
    cai_free_mem(NULL, request_id);
    rc = cai_client_refresh_chatgpt_auth_after_http(client, http_status, error);
    if (rc != CAI_OK) {
      return rc;
    }
    retried_auth = 1;
    goto retry_request;
  }
  if (curl_rc != CURLE_OK) {
    cai_free_mem(NULL, body.data);
    cai_free_mem(NULL, request_id);
    if (body.exceeded) {
      cai_log_http_response_limit(CAI_CLIENT_IMPL(client), "POST", path,
                                  body.limit);
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "HTTP response exceeded configured JSON response "
                           "limit");
    }
    cai_log_http_transport_error(CAI_CLIENT_IMPL(client), "POST", path,
                                 curl_easy_strerror(curl_rc));
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
      rc = cai_response_parse_json_with_allocator(
          &CAI_CLIENT_IMPL(client)->allocator, body != NULL ? body : "", out,
          error);
    }
    cai_free_mem(NULL, body);
    cai_free_mem(NULL, request_id);
  }
  return rc;
}

int cai_client_count_response_input_tokens(
    cai_client *client, const cai_response_create_params *params,
    cai_token_usage *out, cai_error *error) {
  cai_response_create_params *count_params;
  char *body;
  char *request_id;
  long http_status;
  int rc;
  cai_input_token_count_doc doc;
  lonejson_error json_error;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "token usage output pointer is required");
  }
  memset(out, 0, sizeof(*out));
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  count_params = NULL;
  body = NULL;
  request_id = NULL;
  rc = cai_response_create_params_clone(params, &count_params, error);
  if (rc != CAI_OK) {
    return rc;
  }
  /*
   * /responses/input_tokens counts rendered input. The live endpoint accepts
   * input-shaping fields such as tools and tool_choice, but rejects
   * generation/execution controls that are valid for /responses.
   */
  count_params->max_output_tokens = 0;
  count_params->max_tool_calls = 0;
  rc = cai_http_response_params_request(client, "responses/input_tokens",
                                        count_params, 0, &body, &http_status,
                                        &request_id, error);
  if (rc == CAI_OK && (http_status < 200L || http_status >= 300L)) {
    rc = cai_set_openai_error(error, http_status, body, request_id);
  }
  if (rc == CAI_OK) {
    memset(&doc, 0, sizeof(doc));
    lonejson_error_init(&json_error);
    if (CAI_LJ->parse_buffer(CAI_LJ, &cai_input_token_count_map, &doc,
                             body != NULL ? body : "",
                             body != NULL ? strlen(body) : 0U,
                             &json_error) != LONEJSON_STATUS_OK) {
      rc = cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to parse input token count response",
                                json_error.message);
    } else {
      out->input_tokens = doc.input_tokens;
      out->total_tokens = doc.input_tokens;
    }
    CAI_LJ->cleanup(CAI_LJ, &cai_input_token_count_map, &doc);
  }
  cai_free_mem(NULL, body);
  cai_free_mem(NULL, request_id);
  cai_response_create_params_destroy(count_params);
  return rc;
}

int cai_client_retrieve_response(cai_client *client, const char *response_id,
                                 cai_response **out, cai_error *error) {
  char *path;
  int rc;

  path = NULL;
  rc = cai_build_response_path(
      client != NULL ? &CAI_CLIENT_IMPL(client)->allocator : NULL, response_id,
      NULL, &path, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_http_response_request(client, "GET", path, NULL,
                                 CAI_HTTP_RESPONSE_PARSE, out, error);
  cai_free_mem(client != NULL ? &CAI_CLIENT_IMPL(client)->allocator : NULL,
               path);
  return rc;
}

int cai_client_cancel_response(cai_client *client, const char *response_id,
                               cai_response **out, cai_error *error) {
  char *path;
  int rc;

  path = NULL;
  rc = cai_build_response_path(
      client != NULL ? &CAI_CLIENT_IMPL(client)->allocator : NULL, response_id,
      "/cancel", &path, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_http_response_request(client, "POST", path, NULL,
                                 CAI_HTTP_RESPONSE_PARSE, out, error);
  cai_free_mem(client != NULL ? &CAI_CLIENT_IMPL(client)->allocator : NULL,
               path);
  return rc;
}

int cai_client_delete_response(cai_client *client, const char *response_id,
                               cai_error *error) {
  char *path;
  int rc;

  path = NULL;
  rc = cai_build_response_path(
      client != NULL ? &CAI_CLIENT_IMPL(client)->allocator : NULL, response_id,
      NULL, &path, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_http_response_request(client, "DELETE", path, NULL,
                                 CAI_HTTP_RESPONSE_IGNORE, NULL, error);
  cai_free_mem(client != NULL ? &CAI_CLIENT_IMPL(client)->allocator : NULL,
               path);
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
  rc = cai_build_response_input_items_path(
      client != NULL ? &CAI_CLIENT_IMPL(client)->allocator : NULL, response_id,
      params, &path, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_http_json_request(client, "GET", path, NULL, &body, &http_status,
                             &request_id, error);
  cai_free_mem(client != NULL ? &CAI_CLIENT_IMPL(client)->allocator : NULL,
               path);
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
