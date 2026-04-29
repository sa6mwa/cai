#include "cai_internal.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct cai_http_buffer {
  char *data;
  size_t length;
  size_t capacity;
} cai_http_buffer;

typedef enum cai_http_response_mode {
  CAI_HTTP_RESPONSE_PARSE = 0,
  CAI_HTTP_RESPONSE_IGNORE = 1
} cai_http_response_mode;

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

static int cai_append_header(struct curl_slist **headers, const char *header,
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

static int cai_append_bearer_header(cai_client *client,
                                    struct curl_slist **headers,
                                    cai_error *error) {
  static const char prefix[] = "Authorization: Bearer ";
  size_t prefix_len;
  size_t key_len;
  char *header;
  int rc;

  prefix_len = sizeof(prefix) - 1U;
  key_len = strlen(client->api_key);
  header = (char *)cai_alloc(&client->allocator, prefix_len + key_len + 1U);
  if (header == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate authorization header");
  }
  memcpy(header, prefix, prefix_len);
  memcpy(header + prefix_len, client->api_key, key_len);
  header[prefix_len + key_len] = '\0';
  rc = cai_append_header(headers, header, error);
  cai_free_mem(&client->allocator, header);
  return rc;
}

int cai_http_json_request(cai_client *client, const char *method,
                          const char *path, const char *request_json,
                          char **out_json, long *out_http_status,
                          cai_error *error) {
  CURL *curl;
  CURLcode curl_rc;
  struct curl_slist *headers;
  cai_http_buffer body;
  char *url;
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
  if (client == NULL || method == NULL || path == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "client, method, and path are required");
  }
  url = NULL;
  headers = NULL;
  body.data = NULL;
  body.length = 0U;
  body.capacity = 0U;

  rc = cai_build_url(&client->allocator, client->base_url, path, &url, error);
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
  if (rc != CAI_OK) {
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
  if (strcmp(method, "POST") == 0) {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
  } else {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
  }
  if (request_json != NULL) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_json);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(request_json));
  }
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cai_http_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  if (client->timeout_ms > 0L) {
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, client->timeout_ms);
  }
  if (!client->prefer_http_2) {
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

  if (out_http_status != NULL) {
    *out_http_status = http_status;
  }
  if (curl_rc != CURLE_OK) {
    cai_free_mem(NULL, body.data);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "HTTP request transport failed",
                                curl_easy_strerror(curl_rc));
  }
  *out_json = body.data != NULL ? body.data : cai_strdup(NULL, "");
  if (*out_json == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate empty response body");
  }
  return CAI_OK;
}

static int cai_http_response_request(cai_client *client, const char *method,
                                     const char *path, const char *request_json,
                                     cai_http_response_mode mode,
                                     cai_response **out, cai_error *error) {
  char *body;
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
  rc = cai_http_json_request(client, method, path, request_json, &body,
                             &http_status, error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (http_status < 200L || http_status >= 300L) {
    if (error != NULL) {
      error->http_status = http_status;
    }
    rc = cai_set_error_detail(error, CAI_ERR_SERVER,
                              "response request returned an error",
                              body != NULL ? body : "");
    cai_free_mem(NULL, body);
    return rc;
  }
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

int cai_client_create_response(cai_client *client,
                               const cai_response_create_params *params,
                               cai_response **out, cai_error *error) {
  char *request_json;
  int rc;

  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  request_json = NULL;
  rc = cai_response_create_params_serialize_json(params, &request_json, NULL,
                                                 error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_http_response_request(client, "POST", "responses", request_json,
                                 CAI_HTTP_RESPONSE_PARSE, out, error);
  free(request_json);
  return rc;
}

int cai_client_retrieve_response(cai_client *client, const char *response_id,
                                 cai_response **out, cai_error *error) {
  char *path;
  int rc;

  path = NULL;
  rc = cai_build_response_path(client != NULL ? &client->allocator : NULL,
                               response_id, NULL, &path, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_http_response_request(client, "GET", path, NULL,
                                 CAI_HTTP_RESPONSE_PARSE, out, error);
  cai_free_mem(client != NULL ? &client->allocator : NULL, path);
  return rc;
}

int cai_client_cancel_response(cai_client *client, const char *response_id,
                               cai_response **out, cai_error *error) {
  char *path;
  int rc;

  path = NULL;
  rc = cai_build_response_path(client != NULL ? &client->allocator : NULL,
                               response_id, "/cancel", &path, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_http_response_request(client, "POST", path, NULL,
                                 CAI_HTTP_RESPONSE_PARSE, out, error);
  cai_free_mem(client != NULL ? &client->allocator : NULL, path);
  return rc;
}

int cai_client_delete_response(cai_client *client, const char *response_id,
                               cai_error *error) {
  char *path;
  int rc;

  path = NULL;
  rc = cai_build_response_path(client != NULL ? &client->allocator : NULL,
                               response_id, NULL, &path, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_http_response_request(client, "DELETE", path, NULL,
                                 CAI_HTTP_RESPONSE_IGNORE, NULL, error);
  cai_free_mem(client != NULL ? &client->allocator : NULL, path);
  return rc;
}
