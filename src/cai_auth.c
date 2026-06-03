#include "cai_internal.h"

#include <cai/auth.h>

#include <curl/curl.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct cai_auth_tokens_doc {
  char *id_token;
  char *access_token;
  char *refresh_token;
  char *account_id;
} cai_auth_tokens_doc;

typedef struct cai_auth_file_doc {
  char *auth_mode;
  cai_auth_tokens_doc tokens;
  char *last_refresh;
} cai_auth_file_doc;

typedef struct cai_auth_refresh_request_doc {
  char *client_id;
  char *grant_type;
  char *refresh_token;
} cai_auth_refresh_request_doc;

typedef struct cai_auth_refresh_response_doc {
  char *id_token;
  char *access_token;
  char *refresh_token;
} cai_auth_refresh_response_doc;

typedef struct cai_auth_jwt_claims_doc {
  long long exp;
} cai_auth_jwt_claims_doc;

typedef struct cai_auth_buffer {
  char *data;
  size_t length;
  size_t capacity;
} cai_auth_buffer;

struct cai_chatgpt_auth {
  cai_allocator allocator;
  char *auth_json_path;
  char *issuer;
  char *client_id;
  long long refresh_window_seconds;
  struct pslog_logger *logger;
  int logger_disabled;
  char *id_token;
  char *access_token;
  char *refresh_token;
  char *account_id;
};

static const lonejson_field cai_auth_tokens_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_auth_tokens_doc, id_token,
                                          "id_token"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_auth_tokens_doc, access_token,
                                          "access_token"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_auth_tokens_doc, refresh_token,
                                          "refresh_token"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_auth_tokens_doc, account_id,
                                          "account_id")};
LONEJSON_MAP_DEFINE(cai_auth_tokens_map, cai_auth_tokens_doc,
                    cai_auth_tokens_fields);

static const lonejson_field cai_auth_file_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_auth_file_doc, auth_mode,
                                          "auth_mode"),
    LONEJSON_FIELD_OBJECT(cai_auth_file_doc, tokens, "tokens",
                          &cai_auth_tokens_map),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_auth_file_doc, last_refresh,
                                          "last_refresh")};
LONEJSON_MAP_DEFINE(cai_auth_file_map, cai_auth_file_doc, cai_auth_file_fields);

static const lonejson_field cai_auth_refresh_request_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_auth_refresh_request_doc, client_id,
                                "client_id"),
    LONEJSON_FIELD_STRING_ALLOC(cai_auth_refresh_request_doc, grant_type,
                                "grant_type"),
    LONEJSON_FIELD_STRING_ALLOC(cai_auth_refresh_request_doc, refresh_token,
                                "refresh_token")};
LONEJSON_MAP_DEFINE(cai_auth_refresh_request_map, cai_auth_refresh_request_doc,
                    cai_auth_refresh_request_fields);

static const lonejson_field cai_auth_refresh_response_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_auth_refresh_response_doc,
                                          id_token, "id_token"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_auth_refresh_response_doc,
                                          access_token, "access_token"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_auth_refresh_response_doc,
                                          refresh_token, "refresh_token")};
LONEJSON_MAP_DEFINE(cai_auth_refresh_response_map,
                    cai_auth_refresh_response_doc,
                    cai_auth_refresh_response_fields);

static const lonejson_field cai_auth_jwt_claims_fields[] = {
    LONEJSON_FIELD_I64(cai_auth_jwt_claims_doc, exp, "exp")};
LONEJSON_MAP_DEFINE(cai_auth_jwt_claims_map, cai_auth_jwt_claims_doc,
                    cai_auth_jwt_claims_fields);

static void cai_auth_secure_clear(char *value) {
  volatile char *p;
  size_t len;

  if (value == NULL) {
    return;
  }
  len = strlen(value);
  p = (volatile char *)value;
  while (len-- > 0U) {
    *p++ = '\0';
  }
}

static void cai_auth_free_secret(const cai_allocator *allocator, char *value) {
  cai_auth_secure_clear(value);
  cai_free_mem(allocator, value);
}

static int cai_auth_replace_secret(const cai_allocator *allocator, char **slot,
                                   const char *value, cai_error *error) {
  char *copy;

  copy = cai_strdup(allocator, value);
  if (value != NULL && copy == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate auth token");
  }
  cai_auth_free_secret(allocator, *slot);
  *slot = copy;
  return CAI_OK;
}

static lonejson_status cai_auth_buffer_write(void *user, const void *data,
                                             size_t size,
                                             lonejson_error *error) {
  cai_auth_buffer *buffer;
  char *grown;
  size_t needed;
  size_t new_capacity;

  (void)error;
  buffer = (cai_auth_buffer *)user;
  needed = buffer->length + size + 1U;
  if (needed < buffer->length || needed < size) {
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  if (needed > buffer->capacity) {
    new_capacity = buffer->capacity == 0U ? 512U : buffer->capacity;
    while (new_capacity < needed) {
      new_capacity *= 2U;
    }
    grown = (char *)cai_realloc_mem(NULL, buffer->data, new_capacity);
    if (grown == NULL) {
      return LONEJSON_STATUS_ALLOCATION_FAILED;
    }
    buffer->data = grown;
    buffer->capacity = new_capacity;
  }
  if (size > 0U) {
    memcpy(buffer->data + buffer->length, data, size);
    buffer->length += size;
  }
  buffer->data[buffer->length] = '\0';
  return LONEJSON_STATUS_OK;
}

static size_t cai_auth_curl_write(char *ptr, size_t size, size_t nmemb,
                                  void *userdata) {
  cai_auth_buffer *buffer;
  lonejson_error json_error;
  size_t count;

  buffer = (cai_auth_buffer *)userdata;
  count = size * nmemb;
  if (size != 0U && count / size != nmemb) {
    return 0U;
  }
  lonejson_error_init(&json_error);
  if (cai_auth_buffer_write(buffer, ptr, count, &json_error) !=
      LONEJSON_STATUS_OK) {
    return 0U;
  }
  return count;
}

static int cai_auth_serialize_cstr(const lonejson_map *map, void *doc,
                                   char **out, cai_error *error) {
  cai_auth_buffer buffer;
  lonejson_error json_error;
  lonejson_status status;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "auth JSON output pointer is required");
  }
  *out = NULL;
  memset(&buffer, 0, sizeof(buffer));
  lonejson_error_init(&json_error);
  status = CAI_LJ->serialize_sink(CAI_LJ, map, doc, cai_auth_buffer_write,
                                  &buffer, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    cai_free_mem(NULL, buffer.data);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to serialize auth JSON",
                                json_error.message);
  }
  *out = buffer.data != NULL ? buffer.data : cai_strdup(NULL, "");
  if (*out == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate auth JSON");
  }
  return CAI_OK;
}

static int cai_auth_read_file(const char *path, char **out, cai_error *error) {
  FILE *fp;
  char *data;
  long size;
  size_t read_count;

  if (path == NULL || path[0] == '\0' || out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "auth JSON path and output are required");
  }
  *out = NULL;
  fp = fopen(path, "rb");
  if (fp == NULL) {
    return cai_set_error(error, CAI_ERR_TRANSPORT, "failed to open auth JSON");
  }
  if (fseek(fp, 0L, SEEK_END) != 0) {
    fclose(fp);
    return cai_set_error(error, CAI_ERR_TRANSPORT, "failed to seek auth JSON");
  }
  size = ftell(fp);
  if (size < 0L) {
    fclose(fp);
    return cai_set_error(error, CAI_ERR_TRANSPORT, "failed to size auth JSON");
  }
  if (fseek(fp, 0L, SEEK_SET) != 0) {
    fclose(fp);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to rewind auth JSON");
  }
  data = (char *)cai_alloc(NULL, (size_t)size + 1U);
  if (data == NULL) {
    fclose(fp);
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate auth JSON");
  }
  read_count = fread(data, 1U, (size_t)size, fp);
  fclose(fp);
  if (read_count != (size_t)size) {
    cai_free_mem(NULL, data);
    return cai_set_error(error, CAI_ERR_TRANSPORT, "failed to read auth JSON");
  }
  data[read_count] = '\0';
  *out = data;
  return CAI_OK;
}

static int cai_auth_mkdirs_for_file(const char *path, cai_error *error) {
  char *copy;
  char *p;

  copy = cai_strdup(NULL, path);
  if (copy == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate auth path");
  }
  for (p = copy + 1; *p != '\0'; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(copy, 0700) != 0) {
        /* Existing directories are fine; fopen will report real path errors. */
      }
      *p = '/';
    }
  }
  cai_free_mem(NULL, copy);
  return CAI_OK;
}

static int cai_auth_write_file(const char *path, const char *json,
                               cai_error *error) {
  FILE *fp;
  int rc;
#if defined(__unix__) || defined(__APPLE__)
  int fd;
#endif

  rc = cai_auth_mkdirs_for_file(path, error);
  if (rc != CAI_OK) {
    return rc;
  }
#if defined(__unix__) || defined(__APPLE__)
  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to open auth JSON for writing");
  }
  (void)fchmod(fd, 0600);
  fp = fdopen(fd, "wb");
  if (fp == NULL) {
    close(fd);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to open auth JSON stream");
  }
#else
  fp = fopen(path, "wb");
  if (fp == NULL) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to open auth JSON for writing");
  }
#endif
  if (fwrite(json, 1U, strlen(json), fp) != strlen(json) || fflush(fp) != 0) {
    fclose(fp);
    return cai_set_error(error, CAI_ERR_TRANSPORT, "failed to write auth JSON");
  }
  fclose(fp);
  return CAI_OK;
}

static int cai_auth_urlsafe_b64_decode(const char *text, unsigned char **out,
                                       size_t *out_len, cai_error *error) {
  unsigned char *decoded;
  size_t len;
  size_t capacity;
  size_t i;
  size_t j;
  int bits;
  unsigned int buffer;
  unsigned int value;
  char ch;

  if (text == NULL || out == NULL || out_len == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "base64 input and output are required");
  }
  *out = NULL;
  *out_len = 0U;
  len = strlen(text);
  capacity = (len * 3U) / 4U + 4U;
  decoded = (unsigned char *)cai_alloc(NULL, capacity);
  if (decoded == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate base64");
  }
  buffer = 0U;
  bits = 0;
  j = 0U;
  for (i = 0U; i < len; i++) {
    ch = text[i];
    if (ch >= 'A' && ch <= 'Z') {
      value = (unsigned int)(ch - 'A');
    } else if (ch >= 'a' && ch <= 'z') {
      value = 26U + (unsigned int)(ch - 'a');
    } else if (ch >= '0' && ch <= '9') {
      value = 52U + (unsigned int)(ch - '0');
    } else if (ch == '-') {
      value = 62U;
    } else if (ch == '_') {
      value = 63U;
    } else {
      cai_free_mem(NULL, decoded);
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "invalid base64url character");
    }
    buffer = (buffer << 6) | value;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      decoded[j++] = (unsigned char)((buffer >> bits) & 0xFFU);
    }
  }
  *out = decoded;
  *out_len = j;
  return CAI_OK;
}

static int cai_auth_jwt_payload(const char *jwt, char **out, cai_error *error) {
  const char *first;
  const char *second;
  char *payload_b64;
  unsigned char *payload;
  size_t payload_b64_len;
  size_t payload_len;
  int rc;

  if (jwt == NULL || out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "JWT and payload output are required");
  }
  *out = NULL;
  first = strchr(jwt, '.');
  second = first != NULL ? strchr(first + 1, '.') : NULL;
  if (first == NULL || second == NULL || first[1] == '\0') {
    return cai_set_error(error, CAI_ERR_PROTOCOL, "invalid JWT format");
  }
  payload_b64_len = (size_t)(second - first - 1);
  payload_b64 = cai_strndup(NULL, first + 1, payload_b64_len);
  if (payload_b64 == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate JWT payload");
  }
  payload = NULL;
  payload_len = 0U;
  rc = cai_auth_urlsafe_b64_decode(payload_b64, &payload, &payload_len, error);
  cai_free_mem(NULL, payload_b64);
  if (rc != CAI_OK) {
    return rc;
  }
  *out = (char *)payload;
  (*out)[payload_len] = '\0';
  return CAI_OK;
}

static int cai_auth_jwt_exp(const char *jwt, long long *out_exp,
                            cai_error *error) {
  cai_auth_jwt_claims_doc claims;
  lonejson_error json_error;
  char *payload;
  lonejson_status status;

  if (out_exp == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "JWT expiry output is required");
  }
  *out_exp = 0;
  payload = NULL;
  if (cai_auth_jwt_payload(jwt, &payload, error) != CAI_OK) {
    return error != NULL ? error->code : CAI_ERR_PROTOCOL;
  }
  memset(&claims, 0, sizeof(claims));
  CAI_LJ->init(CAI_LJ, &cai_auth_jwt_claims_map, &claims);
  lonejson_error_init(&json_error);
  status = CAI_LJ->parse_cstr(CAI_LJ, &cai_auth_jwt_claims_map, &claims,
                              payload, &json_error);
  cai_free_mem(NULL, payload);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_auth_jwt_claims_map, &claims);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to parse JWT claims",
                                json_error.message);
  }
  *out_exp = claims.exp;
  CAI_LJ->cleanup(CAI_LJ, &cai_auth_jwt_claims_map, &claims);
  return CAI_OK;
}

void cai_chatgpt_auth_config_init(cai_chatgpt_auth_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
}

static int cai_chatgpt_auth_load(cai_chatgpt_auth *auth, cai_error *error) {
  cai_auth_file_doc doc;
  char *json;
  lonejson_error json_error;
  lonejson_status status;
  int rc;

  json = NULL;
  rc = cai_auth_read_file(auth->auth_json_path, &json, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&doc, 0, sizeof(doc));
  CAI_LJ->init(CAI_LJ, &cai_auth_file_map, &doc);
  lonejson_error_init(&json_error);
  status =
      CAI_LJ->parse_cstr(CAI_LJ, &cai_auth_file_map, &doc, json, &json_error);
  cai_auth_secure_clear(json);
  cai_free_mem(NULL, json);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_auth_file_map, &doc);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to parse auth JSON",
                                json_error.message);
  }
  if (doc.auth_mode == NULL || strcmp(doc.auth_mode, "chatgpt") != 0 ||
      doc.tokens.access_token == NULL || doc.tokens.refresh_token == NULL) {
    CAI_LJ->cleanup(CAI_LJ, &cai_auth_file_map, &doc);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "auth JSON does not contain ChatGPT tokens");
  }
  rc = cai_auth_replace_secret(&auth->allocator, &auth->id_token,
                               doc.tokens.id_token, error);
  if (rc == CAI_OK) {
    rc = cai_auth_replace_secret(&auth->allocator, &auth->access_token,
                                 doc.tokens.access_token, error);
  }
  if (rc == CAI_OK) {
    rc = cai_auth_replace_secret(&auth->allocator, &auth->refresh_token,
                                 doc.tokens.refresh_token, error);
  }
  if (rc == CAI_OK) {
    rc = cai_auth_replace_secret(&auth->allocator, &auth->account_id,
                                 doc.tokens.account_id, error);
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_auth_file_map, &doc);
  return rc;
}

int cai_chatgpt_auth_open(const cai_chatgpt_auth_config *config,
                          cai_chatgpt_auth **out, cai_error *error) {
  cai_chatgpt_auth_config defaults;
  const cai_chatgpt_auth_config *effective;
  cai_chatgpt_auth *auth;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "auth output pointer is required");
  }
  *out = NULL;
  if (config == NULL) {
    cai_chatgpt_auth_config_init(&defaults);
    effective = &defaults;
  } else {
    effective = config;
  }
  if (effective->auth_json_path == NULL ||
      effective->auth_json_path[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "auth JSON path is required");
  }
  auth = (cai_chatgpt_auth *)cai_alloc(&effective->allocator, sizeof(*auth));
  if (auth == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate auth");
  }
  memset(auth, 0, sizeof(*auth));
  auth->allocator = effective->allocator;
  auth->auth_json_path =
      cai_strdup(&auth->allocator, effective->auth_json_path);
  auth->issuer =
      cai_strdup(&auth->allocator, effective->issuer != NULL
                                       ? effective->issuer
                                       : CAI_CHATGPT_AUTH_DEFAULT_ISSUER);
  auth->client_id =
      cai_strdup(&auth->allocator, effective->client_id != NULL
                                       ? effective->client_id
                                       : CAI_CHATGPT_AUTH_DEFAULT_CLIENT_ID);
  auth->refresh_window_seconds =
      effective->refresh_window_seconds > 0
          ? effective->refresh_window_seconds
          : CAI_CHATGPT_AUTH_DEFAULT_REFRESH_WINDOW_SECONDS;
  auth->logger = effective->logger;
  auth->logger_disabled = effective->logger_disabled;
  if (auth->auth_json_path == NULL || auth->issuer == NULL ||
      auth->client_id == NULL) {
    cai_chatgpt_auth_close(auth);
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate auth");
  }
  rc = cai_chatgpt_auth_load(auth, error);
  if (rc != CAI_OK) {
    cai_chatgpt_auth_close(auth);
    return rc;
  }
  *out = auth;
  return CAI_OK;
}

static int cai_chatgpt_auth_should_refresh(cai_chatgpt_auth *auth,
                                           cai_error *error) {
  long long exp;
  time_t now;

  if (auth == NULL || auth->access_token == NULL) {
    return 1;
  }
  if (cai_auth_jwt_exp(auth->access_token, &exp, error) != CAI_OK) {
    cai_error_cleanup(error);
    return 1;
  }
  now = time(NULL);
  return exp <= (long long)now + auth->refresh_window_seconds;
}

static int cai_auth_refresh_endpoint(cai_chatgpt_auth *auth, char **out,
                                     cai_error *error) {
  static const char suffix[] = "/oauth/token";
  size_t issuer_len;
  size_t suffix_len;
  char *url;

  issuer_len = strlen(auth->issuer);
  while (issuer_len > 0U && auth->issuer[issuer_len - 1U] == '/') {
    issuer_len--;
  }
  suffix_len = sizeof(suffix) - 1U;
  url = (char *)cai_alloc(&auth->allocator, issuer_len + suffix_len + 1U);
  if (url == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate refresh endpoint");
  }
  memcpy(url, auth->issuer, issuer_len);
  memcpy(url + issuer_len, suffix, suffix_len);
  url[issuer_len + suffix_len] = '\0';
  *out = url;
  return CAI_OK;
}

static int cai_auth_http_post_json(cai_chatgpt_auth *auth, const char *url,
                                   const char *request_json, char **out_json,
                                   long *out_status, cai_error *error) {
  CURL *curl;
  CURLcode curl_rc;
  struct curl_slist *headers;
  cai_auth_buffer body;

  if (out_json == NULL || out_status == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "HTTP response outputs are required");
  }
  *out_json = NULL;
  *out_status = 0L;
  headers = NULL;
  memset(&body, 0, sizeof(body));
  if (cai_append_header(&headers, "Content-Type: application/json", error) !=
      CAI_OK) {
    return error != NULL ? error->code : CAI_ERR_NOMEM;
  }
  curl = curl_easy_init();
  if (curl == NULL) {
    curl_slist_free_all(headers);
    return cai_set_error(error, CAI_ERR_TRANSPORT, "failed to initialize curl");
  }
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_json);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                   (curl_off_t)strlen(request_json));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cai_auth_curl_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_rc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, out_status);
  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);
  (void)auth;
  if (curl_rc != CURLE_OK) {
    cai_free_mem(NULL, body.data);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "auth HTTP request failed",
                                curl_easy_strerror(curl_rc));
  }
  *out_json = body.data != NULL ? body.data : cai_strdup(NULL, "");
  if (*out_json == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate auth HTTP body");
  }
  return CAI_OK;
}

static int cai_chatgpt_auth_save(cai_chatgpt_auth *auth, cai_error *error) {
  cai_auth_file_doc doc;
  char refreshed_at[64];
  char *json;
  char *mode;
  time_t now;
  struct tm *tm_utc;
  int rc;

  memset(&doc, 0, sizeof(doc));
  mode = cai_strdup(NULL, "chatgpt");
  if (mode == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate auth mode");
  }
  now = time(NULL);
  tm_utc = gmtime(&now);
  if (tm_utc == NULL || strftime(refreshed_at, sizeof(refreshed_at),
                                 "%Y-%m-%dT%H:%M:%SZ", tm_utc) == 0U) {
    strcpy(refreshed_at, "1970-01-01T00:00:00Z");
  }
  doc.auth_mode = mode;
  doc.tokens.id_token = auth->id_token;
  doc.tokens.access_token = auth->access_token;
  doc.tokens.refresh_token = auth->refresh_token;
  doc.tokens.account_id = auth->account_id;
  doc.last_refresh = refreshed_at;
  json = NULL;
  rc = cai_auth_serialize_cstr(&cai_auth_file_map, &doc, &json, error);
  if (rc == CAI_OK) {
    rc = cai_auth_write_file(auth->auth_json_path, json, error);
  }
  cai_auth_secure_clear(json);
  cai_free_mem(NULL, json);
  cai_free_mem(NULL, mode);
  return rc;
}

int cai_chatgpt_auth_refresh(cai_chatgpt_auth *auth, cai_error *error) {
  cai_auth_refresh_request_doc request;
  cai_auth_refresh_response_doc response;
  char *request_json;
  char *response_json;
  char *url;
  long status;
  lonejson_error json_error;
  lonejson_status json_status;
  int rc;
  char *grant_type;

  if (auth == NULL || auth->refresh_token == NULL ||
      auth->refresh_token[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "ChatGPT refresh token is required");
  }
  grant_type = cai_strdup(NULL, "refresh_token");
  if (grant_type == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate refresh grant type");
  }
  memset(&request, 0, sizeof(request));
  request.client_id = auth->client_id;
  request.grant_type = grant_type;
  request.refresh_token = auth->refresh_token;
  request_json = NULL;
  response_json = NULL;
  url = NULL;
  rc = cai_auth_serialize_cstr(&cai_auth_refresh_request_map, &request,
                               &request_json, error);
  cai_free_mem(NULL, grant_type);
  if (rc == CAI_OK) {
    rc = cai_auth_refresh_endpoint(auth, &url, error);
  }
  if (rc == CAI_OK) {
    rc = cai_auth_http_post_json(auth, url, request_json, &response_json,
                                 &status, error);
  }
  cai_auth_secure_clear(request_json);
  cai_free_mem(NULL, request_json);
  cai_free_mem(&auth->allocator, url);
  if (rc != CAI_OK) {
    return rc;
  }
  if (status < 200L || status >= 300L) {
    cai_auth_secure_clear(response_json);
    cai_free_mem(NULL, response_json);
    return cai_set_error_http(
        error, status == 401L ? CAI_ERR_INVALID : CAI_ERR_TRANSPORT, status,
        "ChatGPT token refresh failed", NULL, NULL, NULL);
  }
  memset(&response, 0, sizeof(response));
  CAI_LJ->init(CAI_LJ, &cai_auth_refresh_response_map, &response);
  lonejson_error_init(&json_error);
  json_status = CAI_LJ->parse_cstr(CAI_LJ, &cai_auth_refresh_response_map,
                                   &response, response_json, &json_error);
  cai_auth_secure_clear(response_json);
  cai_free_mem(NULL, response_json);
  if (json_status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_auth_refresh_response_map, &response);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to parse refresh response",
                                json_error.message);
  }
  if (response.id_token != NULL) {
    rc = cai_auth_replace_secret(&auth->allocator, &auth->id_token,
                                 response.id_token, error);
  }
  if (rc == CAI_OK && response.access_token != NULL) {
    rc = cai_auth_replace_secret(&auth->allocator, &auth->access_token,
                                 response.access_token, error);
  }
  if (rc == CAI_OK && response.refresh_token != NULL) {
    rc = cai_auth_replace_secret(&auth->allocator, &auth->refresh_token,
                                 response.refresh_token, error);
  }
  if (rc == CAI_OK && auth->access_token == NULL) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "refresh response omitted access token");
  }
  if (rc == CAI_OK) {
    rc = cai_chatgpt_auth_save(auth, error);
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_auth_refresh_response_map, &response);
  return rc;
}

int cai_chatgpt_auth_access_token(cai_chatgpt_auth *auth, char **out,
                                  cai_error *error) {
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "access token output pointer is required");
  }
  *out = NULL;
  if (auth == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "auth is required");
  }
  if (cai_chatgpt_auth_should_refresh(auth, error)) {
    rc = cai_chatgpt_auth_refresh(auth, error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  *out = cai_strdup(NULL, auth->access_token);
  if (*out == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate access token");
  }
  return CAI_OK;
}

void cai_chatgpt_auth_close(cai_chatgpt_auth *auth) {
  cai_allocator allocator;

  if (auth == NULL) {
    return;
  }
  allocator = auth->allocator;
  cai_free_mem(&allocator, auth->auth_json_path);
  cai_free_mem(&allocator, auth->issuer);
  cai_free_mem(&allocator, auth->client_id);
  cai_auth_free_secret(&allocator, auth->id_token);
  cai_auth_free_secret(&allocator, auth->access_token);
  cai_auth_free_secret(&allocator, auth->refresh_token);
  cai_auth_free_secret(&allocator, auth->account_id);
  memset(auth, 0, sizeof(*auth));
  cai_free_mem(&allocator, auth);
}
