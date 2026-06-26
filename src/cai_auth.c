#include "cai_internal.h"

#include <cai/auth.h>

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
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
  char *account_id;
} cai_auth_refresh_response_doc;

typedef struct cai_auth_jwt_claims_doc {
  long long exp;
} cai_auth_jwt_claims_doc;

typedef struct cai_auth_buffer {
  char *data;
  size_t length;
  size_t capacity;
} cai_auth_buffer;

typedef struct cai_chatgpt_auth_impl {
  cai_allocator allocator;
  char *auth_json_path;
  char *issuer;
  char *client_id;
  long long refresh_window_seconds;
  long http_timeout_ms;
  int insecure_skip_verify;
  char *ca_bundle_path;
  char *ca_path;
  struct pslog_logger *logger;
  int logger_disabled;
  char *id_token;
  char *access_token;
  char *refresh_token;
  char *account_id;
} cai_chatgpt_auth_impl;

typedef struct cai_chatgpt_login_impl {
  cai_allocator allocator;
  char *auth_json_path;
  char *issuer;
  char *client_id;
  char *redirect_uri;
  char *callback_path;
  char *scopes;
  char *originator;
  char *state;
  char *code_verifier;
  char *authorize_url;
  long http_timeout_ms;
  int insecure_skip_verify;
  char *ca_bundle_path;
  char *ca_path;
  struct pslog_logger *logger;
  int logger_disabled;
  int completed;
} cai_chatgpt_login_impl;

static cai_chatgpt_auth_impl *
cai_chatgpt_auth_impl_from_public(cai_chatgpt_auth *auth) {
  return auth != NULL ? (cai_chatgpt_auth_impl *)auth->impl : NULL;
}

static const cai_chatgpt_login_impl *
cai_chatgpt_login_impl_from_const_public(const cai_chatgpt_login *login) {
  return login != NULL ? (const cai_chatgpt_login_impl *)login->impl : NULL;
}

static cai_chatgpt_login_impl *
cai_chatgpt_login_impl_from_public(cai_chatgpt_login *login) {
  return login != NULL ? (cai_chatgpt_login_impl *)login->impl : NULL;
}

static int cai_auth_allocator_is_empty(const cai_allocator *allocator) {
  return allocator->malloc_fn == NULL && allocator->realloc_fn == NULL &&
         allocator->free_fn == NULL;
}

static int cai_auth_allocator_is_complete(const cai_allocator *allocator) {
  return allocator->malloc_fn != NULL && allocator->realloc_fn != NULL &&
         allocator->free_fn != NULL;
}

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
                                          refresh_token, "refresh_token"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_auth_refresh_response_doc,
                                          account_id, "account_id")};
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

static int cai_auth_secret_equal(const char *a, const char *b) {
  if (a == NULL || b == NULL) {
    return a == b;
  }
  return strcmp(a, b) == 0;
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

static int cai_auth_path_join(char **out, const char *left, const char *right,
                              cai_error *error) {
  char *path;
  size_t left_len;
  size_t right_len;
  size_t need_slash;

  if (out == NULL || left == NULL || left[0] == '\0' || right == NULL ||
      right[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "auth path components are required");
  }
  *out = NULL;
  left_len = strlen(left);
  right_len = strlen(right);
  need_slash = left[left_len - 1U] == '/' ? 0U : 1U;
  if (left_len + need_slash + right_len + 1U < left_len) {
    return cai_set_error(error, CAI_ERR_INVALID, "auth path is too long");
  }
  path = (char *)cai_alloc(NULL, left_len + need_slash + right_len + 1U);
  if (path == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate auth path");
  }
  memcpy(path, left, left_len);
  if (need_slash) {
    path[left_len] = '/';
  }
  memcpy(path + left_len + need_slash, right, right_len);
  path[left_len + need_slash + right_len] = '\0';
  *out = path;
  return CAI_OK;
}

static int cai_auth_is_absolute_path(const char *path) {
  return path != NULL && path[0] == '/';
}

int cai_chatgpt_auth_default_path(char **out, cai_error *error) {
  const char *xdg;
  const char *home;
  char *base;
  char *dir;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "auth path output pointer is required");
  }
  *out = NULL;
  xdg = getenv("XDG_CONFIG_HOME");
  if (cai_auth_is_absolute_path(xdg)) {
    dir = NULL;
    rc = cai_auth_path_join(&dir, xdg, "cai", error);
    if (rc == CAI_OK) {
      rc = cai_auth_path_join(out, dir, "auth.json", error);
    }
    cai_free_mem(NULL, dir);
    return rc;
  }

  home = getenv("HOME");
  if (!cai_auth_is_absolute_path(home)) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "HOME is required for default ChatGPT auth path");
  }
  base = NULL;
  dir = NULL;
  rc = cai_auth_path_join(&base, home, ".config", error);
  if (rc == CAI_OK) {
    rc = cai_auth_path_join(&dir, base, "cai", error);
  }
  if (rc == CAI_OK) {
    rc = cai_auth_path_join(out, dir, "auth.json", error);
  }
  cai_free_mem(NULL, dir);
  cai_free_mem(NULL, base);
  return rc;
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
      if (mkdir(copy, 0700) != 0 && errno != EEXIST) {
        int rc;
        rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                  "failed to create auth directory", copy);
        cai_free_mem(NULL, copy);
        return rc;
      }
      *p = '/';
    }
  }
  cai_free_mem(NULL, copy);
  return CAI_OK;
}

static int cai_auth_write_file(const char *path, const char *json,
                               cai_error *error) {
#if defined(__unix__) || defined(__APPLE__)
  char *template_path;
  char *slash;
  char *allocated_dir_path;
  const char *dir_path;
  size_t path_len;
  size_t suffix_len;
  size_t dir_len;
  size_t written;
  size_t json_len;
  ssize_t n;
  int fd;
  int dir_fd;
  int saved_errno;
#else
  FILE *fp;
#endif
  int rc;

  rc = cai_auth_mkdirs_for_file(path, error);
  if (rc != CAI_OK) {
    return rc;
  }
#if defined(__unix__) || defined(__APPLE__)
  path_len = strlen(path);
  suffix_len = sizeof(".tmp.XXXXXX") - 1U;
  template_path = (char *)cai_alloc(NULL, path_len + suffix_len + 1U);
  if (template_path == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate auth temp path");
  }
  memcpy(template_path, path, path_len);
  memcpy(template_path + path_len, ".tmp.XXXXXX", suffix_len + 1U);
  allocated_dir_path = NULL;
  dir_path = NULL;
  fd = mkstemp(template_path);
  if (fd < 0) {
    cai_free_mem(NULL, template_path);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to open temporary auth JSON for writing");
  }
  if (fchmod(fd, 0600) != 0) {
    saved_errno = errno;
    close(fd);
    unlink(template_path);
    cai_free_mem(NULL, template_path);
    errno = saved_errno;
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to chmod temporary auth JSON");
  }
  json_len = strlen(json);
  written = 0U;
  while (written < json_len) {
    n = write(fd, json + written, json_len - written);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      saved_errno = errno;
      close(fd);
      unlink(template_path);
      cai_free_mem(NULL, template_path);
      errno = saved_errno;
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "failed to write temporary auth JSON");
    }
    written += (size_t)n;
  }
  if (fsync(fd) != 0) {
    saved_errno = errno;
    close(fd);
    unlink(template_path);
    cai_free_mem(NULL, template_path);
    errno = saved_errno;
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to sync temporary auth JSON");
  }
  if (close(fd) != 0) {
    saved_errno = errno;
    unlink(template_path);
    cai_free_mem(NULL, template_path);
    errno = saved_errno;
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to close temporary auth JSON");
  }
  slash = strrchr(path, '/');
  if (slash == NULL) {
    dir_path = ".";
  } else if (slash == path) {
    dir_path = "/";
  } else {
    dir_len = (size_t)(slash - path);
    allocated_dir_path = (char *)cai_alloc(NULL, dir_len + 1U);
    if (allocated_dir_path == NULL) {
      cai_free_mem(NULL, template_path);
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate auth directory path");
    }
    memcpy(allocated_dir_path, path, dir_len);
    allocated_dir_path[dir_len] = '\0';
    dir_path = allocated_dir_path;
  }
  if (rename(template_path, path) != 0) {
    saved_errno = errno;
    unlink(template_path);
    cai_free_mem(NULL, allocated_dir_path);
    cai_free_mem(NULL, template_path);
    errno = saved_errno;
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to replace auth JSON");
  }
  dir_fd = open(dir_path, O_RDONLY);
  if (dir_fd >= 0) {
    (void)fsync(dir_fd);
    close(dir_fd);
  }
  cai_free_mem(NULL, allocated_dir_path);
  cai_free_mem(NULL, template_path);
#else
  fp = fopen(path, "wb");
  if (fp == NULL) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to open auth JSON for writing");
  }
  if (fwrite(json, 1U, strlen(json), fp) != strlen(json) || fflush(fp) != 0) {
    fclose(fp);
    return cai_set_error(error, CAI_ERR_TRANSPORT, "failed to write auth JSON");
  }
  fclose(fp);
#endif
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

static int cai_auth_urlsafe_b64_encode(const unsigned char *data, size_t len,
                                       char **out, cai_error *error) {
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  char *encoded;
  size_t out_len;
  size_t rem;
  size_t i;
  size_t j;
  unsigned int value;

  if (data == NULL || out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "base64 input and output are required");
  }
  *out = NULL;
  out_len = ((len + 2U) / 3U) * 4U;
  rem = len % 3U;
  if (rem == 1U) {
    out_len -= 2U;
  } else if (rem == 2U) {
    out_len -= 1U;
  }
  encoded = (char *)cai_alloc(NULL, out_len + 1U);
  if (encoded == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate base64");
  }
  i = 0U;
  j = 0U;
  while (i + 3U <= len) {
    value = ((unsigned int)data[i] << 16) | ((unsigned int)data[i + 1U] << 8) |
            (unsigned int)data[i + 2U];
    encoded[j++] = alphabet[(value >> 18) & 63U];
    encoded[j++] = alphabet[(value >> 12) & 63U];
    encoded[j++] = alphabet[(value >> 6) & 63U];
    encoded[j++] = alphabet[value & 63U];
    i += 3U;
  }
  if (i < len) {
    value = (unsigned int)data[i] << 16;
    if (i + 1U < len) {
      value |= (unsigned int)data[i + 1U] << 8;
    }
    encoded[j++] = alphabet[(value >> 18) & 63U];
    encoded[j++] = alphabet[(value >> 12) & 63U];
    if (i + 1U < len) {
      encoded[j++] = alphabet[(value >> 6) & 63U];
    }
  }
  encoded[j] = '\0';
  *out = encoded;
  return CAI_OK;
}

static int cai_auth_random_bytes(unsigned char *out, size_t len,
                                 cai_error *error) {
  FILE *fp;
  size_t got;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "random output is required");
  }
  fp = fopen("/dev/urandom", "rb");
  if (fp == NULL) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to open system random source");
  }
  got = fread(out, 1U, len, fp);
  fclose(fp);
  if (got != len) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to read system random source");
  }
  return CAI_OK;
}

static int cai_auth_random_b64(size_t byte_count, char **out,
                               cai_error *error) {
  unsigned char bytes[48];
  int rc;

  if (byte_count > sizeof(bytes)) {
    return cai_set_error(error, CAI_ERR_INVALID, "random request too large");
  }
  rc = cai_auth_random_bytes(bytes, byte_count, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_auth_urlsafe_b64_encode(bytes, byte_count, out, error);
  memset(bytes, 0, sizeof(bytes));
  return rc;
}

static int cai_auth_pkce_challenge(const char *verifier, char **out,
                                   cai_error *error) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  int rc;

  if (verifier == NULL || out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "PKCE verifier and output are required");
  }
  SHA256((const unsigned char *)verifier, strlen(verifier), digest);
  rc = cai_auth_urlsafe_b64_encode(digest, sizeof(digest), out, error);
  memset(digest, 0, sizeof(digest));
  return rc;
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

static int cai_chatgpt_auth_parse_file(cai_chatgpt_auth_impl *auth,
                                       cai_auth_file_doc *doc,
                                       cai_error *error) {
  char *json;
  lonejson_error json_error;
  lonejson_status status;
  int rc;

  json = NULL;
  memset(doc, 0, sizeof(*doc));
  CAI_LJ->init(CAI_LJ, &cai_auth_file_map, doc);
  rc = cai_auth_read_file(auth->auth_json_path, &json, error);
  if (rc != CAI_OK) {
    return rc;
  }
  lonejson_error_init(&json_error);
  status =
      CAI_LJ->parse_cstr(CAI_LJ, &cai_auth_file_map, doc, json, &json_error);
  cai_auth_secure_clear(json);
  cai_free_mem(NULL, json);
  if (status != LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to parse auth JSON",
                                json_error.message);
  }
  if (doc->auth_mode == NULL || strcmp(doc->auth_mode, "chatgpt") != 0 ||
      doc->tokens.access_token == NULL || doc->tokens.refresh_token == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "auth JSON does not contain ChatGPT tokens");
  }
  return CAI_OK;
}

static int cai_chatgpt_auth_apply_doc(cai_chatgpt_auth_impl *auth,
                                      const cai_auth_file_doc *doc,
                                      cai_error *error) {
  int rc;

  rc = cai_auth_replace_secret(&auth->allocator, &auth->id_token,
                               doc->tokens.id_token, error);
  if (rc == CAI_OK) {
    rc = cai_auth_replace_secret(&auth->allocator, &auth->access_token,
                                 doc->tokens.access_token, error);
  }
  if (rc == CAI_OK) {
    rc = cai_auth_replace_secret(&auth->allocator, &auth->refresh_token,
                                 doc->tokens.refresh_token, error);
  }
  if (rc == CAI_OK) {
    rc = cai_auth_replace_secret(&auth->allocator, &auth->account_id,
                                 doc->tokens.account_id, error);
  }
  return rc;
}

static int cai_chatgpt_auth_load(cai_chatgpt_auth_impl *auth,
                                 cai_error *error) {
  cai_auth_file_doc doc;
  int rc;

  rc = cai_chatgpt_auth_parse_file(auth, &doc, error);
  if (rc == CAI_OK) {
    rc = cai_chatgpt_auth_apply_doc(auth, &doc, error);
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_auth_file_map, &doc);
  return rc;
}

static int cai_chatgpt_auth_reload_changed(cai_chatgpt_auth_impl *auth,
                                           int *out_changed, cai_error *error) {
  cai_auth_file_doc doc;
  int changed;
  int rc;

  if (out_changed == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "reload changed output is required");
  }
  *out_changed = 0;
  rc = cai_chatgpt_auth_parse_file(auth, &doc, error);
  if (rc != CAI_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_auth_file_map, &doc);
    return rc;
  }
  changed =
      !cai_auth_secret_equal(auth->id_token, doc.tokens.id_token) ||
      !cai_auth_secret_equal(auth->access_token, doc.tokens.access_token) ||
      !cai_auth_secret_equal(auth->refresh_token, doc.tokens.refresh_token) ||
      !cai_auth_secret_equal(auth->account_id, doc.tokens.account_id);
  if (changed && auth->account_id != NULL &&
      (doc.tokens.account_id == NULL ||
       strcmp(auth->account_id, doc.tokens.account_id) != 0)) {
    CAI_LJ->cleanup(CAI_LJ, &cai_auth_file_map, &doc);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "auth JSON changed to a different ChatGPT account");
  }
  if (changed) {
    rc = cai_chatgpt_auth_apply_doc(auth, &doc, error);
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_auth_file_map, &doc);
  if (rc == CAI_OK) {
    *out_changed = changed;
  }
  return rc;
}

int cai_chatgpt_auth_open(const cai_chatgpt_auth_config *config,
                          cai_chatgpt_auth **out, cai_error *error) {
  cai_chatgpt_auth_config defaults;
  const cai_chatgpt_auth_config *effective;
  cai_chatgpt_auth *auth;
  cai_chatgpt_auth_impl *impl;
  char *default_auth_json_path;
  const char *auth_json_path;
  int rc;

  default_auth_json_path = NULL;
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
  if (!cai_auth_allocator_is_empty(&effective->allocator) &&
      !cai_auth_allocator_is_complete(&effective->allocator)) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "custom allocator requires malloc, realloc, and free");
  }
  auth_json_path = effective->auth_json_path;
  if (auth_json_path == NULL || auth_json_path[0] == '\0') {
    rc = cai_chatgpt_auth_default_path(&default_auth_json_path, error);
    if (rc != CAI_OK) {
      return rc;
    }
    auth_json_path = default_auth_json_path;
  }
  auth = (cai_chatgpt_auth *)cai_alloc(&effective->allocator, sizeof(*auth));
  if (auth == NULL) {
    cai_free_mem(NULL, default_auth_json_path);
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate auth");
  }
  memset(auth, 0, sizeof(*auth));
  auth->allocator = effective->allocator;
  auth->access_token = cai_chatgpt_auth_access_token;
  auth->refresh = cai_chatgpt_auth_refresh;
  auth->close = cai_chatgpt_auth_close;
  impl =
      (cai_chatgpt_auth_impl *)cai_alloc(&effective->allocator, sizeof(*impl));
  if (impl == NULL) {
    cai_free_mem(&effective->allocator, auth);
    cai_free_mem(NULL, default_auth_json_path);
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate auth");
  }
  memset(impl, 0, sizeof(*impl));
  auth->impl = impl;
  impl->allocator = effective->allocator;
  impl->auth_json_path = cai_strdup(&impl->allocator, auth_json_path);
  cai_free_mem(NULL, default_auth_json_path);
  impl->issuer =
      cai_strdup(&impl->allocator, effective->issuer != NULL
                                       ? effective->issuer
                                       : CAI_CHATGPT_AUTH_DEFAULT_ISSUER);
  impl->client_id =
      cai_strdup(&impl->allocator, effective->client_id != NULL
                                       ? effective->client_id
                                       : CAI_CHATGPT_AUTH_DEFAULT_CLIENT_ID);
  impl->refresh_window_seconds =
      effective->refresh_window_seconds > 0
          ? effective->refresh_window_seconds
          : CAI_CHATGPT_AUTH_DEFAULT_REFRESH_WINDOW_SECONDS;
  impl->http_timeout_ms = effective->http_timeout_ms > 0
                              ? effective->http_timeout_ms
                              : CAI_CHATGPT_AUTH_DEFAULT_HTTP_TIMEOUT_MS;
  impl->insecure_skip_verify = effective->insecure_skip_verify;
  impl->ca_bundle_path =
      cai_strdup(&impl->allocator, effective->ca_bundle_path);
  impl->ca_path = cai_strdup(&impl->allocator, effective->ca_path);
  impl->logger = effective->logger;
  impl->logger_disabled = effective->logger_disabled;
  if (impl->auth_json_path == NULL || impl->issuer == NULL ||
      impl->client_id == NULL ||
      (effective->ca_bundle_path != NULL && impl->ca_bundle_path == NULL) ||
      (effective->ca_path != NULL && impl->ca_path == NULL)) {
    auth->close(auth);
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate auth");
  }
  rc = cai_chatgpt_auth_load(impl, error);
  if (rc != CAI_OK) {
    auth->close(auth);
    return rc;
  }
  *out = auth;
  return CAI_OK;
}

static int cai_chatgpt_auth_should_refresh(cai_chatgpt_auth_impl *auth,
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

static int cai_auth_refresh_endpoint(cai_chatgpt_auth_impl *auth, char **out,
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

static int cai_auth_issuer_url(const cai_allocator *allocator,
                               const char *issuer, const char *suffix,
                               char **out, cai_error *error) {
  size_t issuer_len;
  size_t suffix_len;
  char *url;

  if (issuer == NULL || suffix == NULL || out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "issuer, suffix, and output are required");
  }
  issuer_len = strlen(issuer);
  while (issuer_len > 0U && issuer[issuer_len - 1U] == '/') {
    issuer_len--;
  }
  suffix_len = strlen(suffix);
  url = (char *)cai_alloc(allocator, issuer_len + suffix_len + 1U);
  if (url == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate auth URL");
  }
  memcpy(url, issuer, issuer_len);
  memcpy(url + issuer_len, suffix, suffix_len);
  url[issuer_len + suffix_len] = '\0';
  *out = url;
  return CAI_OK;
}

static int cai_auth_buffer_append(cai_auth_buffer *buffer, const char *text,
                                  cai_error *error) {
  lonejson_error json_error;

  if (text == NULL) {
    return CAI_OK;
  }
  lonejson_error_init(&json_error);
  if (cai_auth_buffer_write(buffer, text, strlen(text), &json_error) !=
      LONEJSON_STATUS_OK) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate auth URL");
  }
  return CAI_OK;
}

static int cai_auth_hex_value(char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'A' && ch <= 'F') {
    return 10 + ch - 'A';
  }
  if (ch >= 'a' && ch <= 'f') {
    return 10 + ch - 'a';
  }
  return -1;
}

static int cai_auth_url_encode(const char *value, char **out,
                               cai_error *error) {
  static const char hex[] = "0123456789ABCDEF";
  const unsigned char *src;
  char *encoded;
  size_t len;
  size_t capacity;
  size_t i;
  size_t j;
  unsigned char ch;

  if (value == NULL || out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "URL encode input and output are required");
  }
  *out = NULL;
  len = strlen(value);
  capacity = len * 3U + 1U;
  encoded = (char *)cai_alloc(NULL, capacity);
  if (encoded == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate URL value");
  }
  src = (const unsigned char *)value;
  j = 0U;
  for (i = 0U; i < len; i++) {
    ch = src[i];
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' ||
        ch == '~') {
      encoded[j++] = (char)ch;
    } else {
      encoded[j++] = '%';
      encoded[j++] = hex[(ch >> 4) & 15U];
      encoded[j++] = hex[ch & 15U];
    }
  }
  encoded[j] = '\0';
  *out = encoded;
  return CAI_OK;
}

static int cai_auth_url_decode(const char *value, size_t len, char **out,
                               cai_error *error) {
  char *decoded;
  size_t i;
  size_t j;
  int hi;
  int lo;
  char ch;

  if (value == NULL || out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "URL decode input and output are required");
  }
  *out = NULL;
  decoded = (char *)cai_alloc(NULL, len + 1U);
  if (decoded == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate URL value");
  }
  i = 0U;
  j = 0U;
  while (i < len) {
    ch = value[i++];
    if (ch == '+') {
      decoded[j++] = ' ';
    } else if (ch == '%' && i + 1U < len) {
      hi = cai_auth_hex_value(value[i]);
      lo = cai_auth_hex_value(value[i + 1U]);
      if (hi < 0 || lo < 0) {
        cai_free_mem(NULL, decoded);
        return cai_set_error(error, CAI_ERR_PROTOCOL,
                             "invalid URL escape in OAuth callback");
      }
      decoded[j++] = (char)((hi << 4) | lo);
      i += 2U;
    } else if (ch == '%') {
      cai_free_mem(NULL, decoded);
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "truncated URL escape in OAuth callback");
    } else {
      decoded[j++] = ch;
    }
  }
  decoded[j] = '\0';
  *out = decoded;
  return CAI_OK;
}

static int cai_auth_append_query_param(cai_auth_buffer *buffer,
                                       const char *name, const char *value,
                                       int *first, cai_error *error) {
  char *encoded;
  int rc;

  if (value == NULL || value[0] == '\0') {
    return CAI_OK;
  }
  encoded = NULL;
  rc = cai_auth_url_encode(value, &encoded, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_auth_buffer_append(buffer, *first ? "?" : "&", error);
  if (rc == CAI_OK) {
    rc = cai_auth_buffer_append(buffer, name, error);
  }
  if (rc == CAI_OK) {
    rc = cai_auth_buffer_append(buffer, "=", error);
  }
  if (rc == CAI_OK) {
    rc = cai_auth_buffer_append(buffer, encoded, error);
  }
  cai_free_mem(NULL, encoded);
  *first = 0;
  return rc;
}

static int cai_auth_query_param(const char *target, const char *name,
                                char **out, cai_error *error) {
  const char *query;
  const char *p;
  const char *key;
  const char *value;
  size_t name_len;
  size_t key_len;
  size_t value_len;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "query output pointer is required");
  }
  *out = NULL;
  if (target == NULL || name == NULL) {
    return CAI_OK;
  }
  query = strchr(target, '?');
  if (query == NULL) {
    return CAI_OK;
  }
  query++;
  name_len = strlen(name);
  p = query;
  while (*p != '\0') {
    key = p;
    while (*p != '\0' && *p != '=' && *p != '&') {
      p++;
    }
    key_len = (size_t)(p - key);
    if (*p == '=') {
      p++;
      value = p;
      while (*p != '\0' && *p != '&') {
        p++;
      }
      value_len = (size_t)(p - value);
    } else {
      value = p;
      value_len = 0U;
    }
    if (key_len == name_len && strncmp(key, name, name_len) == 0) {
      return cai_auth_url_decode(value, value_len, out, error);
    }
    if (*p == '&') {
      p++;
    }
  }
  return CAI_OK;
}

static int cai_auth_http_post(cai_allocator *allocator, const char *url,
                              const char *content_type, const char *body_text,
                              long timeout_ms, int insecure_skip_verify,
                              const char *ca_bundle_path, const char *ca_path,
                              char **out_json, long *out_status,
                              cai_error *error) {
  CURL *curl;
  CURLcode curl_rc;
  struct curl_slist *headers;
  cai_auth_buffer body;
  char content_header[128];

  if (out_json == NULL || out_status == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "HTTP response outputs are required");
  }
  *out_json = NULL;
  *out_status = 0L;
  headers = NULL;
  memset(&body, 0, sizeof(body));
  if (content_type == NULL) {
    content_type = "application/json";
  }
  if (snprintf(content_header, sizeof(content_header), "Content-Type: %s",
               content_type) < 0 ||
      strlen(content_header) >= sizeof(content_header)) {
    return cai_set_error(error, CAI_ERR_INVALID, "content type is too long");
  }
  if (cai_append_header(&headers, content_header, error) != CAI_OK) {
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
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_text);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                   (curl_off_t)strlen(body_text));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cai_auth_curl_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  if (timeout_ms <= 0L) {
    timeout_ms = CAI_CHATGPT_AUTH_DEFAULT_HTTP_TIMEOUT_MS;
  }
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
  cai_configure_curl_tls(curl, insecure_skip_verify, ca_bundle_path, ca_path);
  curl_rc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, out_status);
  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);
  if (curl_rc != CURLE_OK) {
    cai_free_mem(NULL, body.data);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "auth HTTP request failed",
                                curl_easy_strerror(curl_rc));
  }
  (void)allocator;
  *out_json = body.data != NULL ? body.data : cai_strdup(NULL, "");
  if (*out_json == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate auth HTTP body");
  }
  return CAI_OK;
}

static int cai_auth_http_post_json(cai_chatgpt_auth_impl *auth, const char *url,
                                   const char *request_json, char **out_json,
                                   long *out_status, cai_error *error) {
  return cai_auth_http_post(&auth->allocator, url, "application/json",
                            request_json, auth->http_timeout_ms,
                            auth->insecure_skip_verify, auth->ca_bundle_path,
                            auth->ca_path, out_json, out_status, error);
}

static int cai_chatgpt_auth_save(cai_chatgpt_auth_impl *auth,
                                 cai_error *error) {
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
  cai_chatgpt_auth_impl *impl;
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
  int reloaded;

  impl = cai_chatgpt_auth_impl_from_public(auth);
  if (impl == NULL || impl->refresh_token == NULL ||
      impl->refresh_token[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "ChatGPT refresh token is required");
  }
  rc = cai_chatgpt_auth_reload_changed(impl, &reloaded, error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (reloaded && !cai_chatgpt_auth_should_refresh(impl, error)) {
    return CAI_OK;
  }
  grant_type = cai_strdup(NULL, "refresh_token");
  if (grant_type == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate refresh grant type");
  }
  memset(&request, 0, sizeof(request));
  request.client_id = impl->client_id;
  request.grant_type = grant_type;
  request.refresh_token = impl->refresh_token;
  request_json = NULL;
  response_json = NULL;
  url = NULL;
  rc = cai_auth_serialize_cstr(&cai_auth_refresh_request_map, &request,
                               &request_json, error);
  cai_free_mem(NULL, grant_type);
  if (rc == CAI_OK) {
    rc = cai_auth_refresh_endpoint(impl, &url, error);
  }
  if (rc == CAI_OK) {
    rc = cai_auth_http_post_json(impl, url, request_json, &response_json,
                                 &status, error);
  }
  cai_auth_secure_clear(request_json);
  cai_free_mem(NULL, request_json);
  cai_free_mem(&impl->allocator, url);
  if (rc != CAI_OK) {
    return rc;
  }
  if (status < 200L || status >= 300L) {
    cai_auth_secure_clear(response_json);
    cai_free_mem(NULL, response_json);
    return cai_set_error_http(
        error,
        (status == 400L || status == 401L || status == 403L)
            ? CAI_ERR_INVALID
            : CAI_ERR_TRANSPORT,
        status,
        (status == 400L || status == 401L || status == 403L)
            ? "ChatGPT token refresh failed; login again with chatgpt-login"
            : "ChatGPT token refresh failed",
        NULL, NULL, NULL);
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
    rc = cai_auth_replace_secret(&impl->allocator, &impl->id_token,
                                 response.id_token, error);
  }
  if (rc == CAI_OK && response.access_token != NULL) {
    rc = cai_auth_replace_secret(&impl->allocator, &impl->access_token,
                                 response.access_token, error);
  }
  if (rc == CAI_OK && response.refresh_token != NULL) {
    rc = cai_auth_replace_secret(&impl->allocator, &impl->refresh_token,
                                 response.refresh_token, error);
  }
  if (rc == CAI_OK && response.account_id != NULL) {
    rc = cai_auth_replace_secret(&impl->allocator, &impl->account_id,
                                 response.account_id, error);
  }
  if (rc == CAI_OK && impl->access_token == NULL) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "refresh response omitted access token");
  }
  if (rc == CAI_OK) {
    rc = cai_chatgpt_auth_save(impl, error);
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_auth_refresh_response_map, &response);
  return rc;
}

int cai_chatgpt_auth_access_token(cai_chatgpt_auth *auth, char **out,
                                  cai_error *error) {
  cai_chatgpt_auth_impl *impl;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "access token output pointer is required");
  }
  *out = NULL;
  impl = cai_chatgpt_auth_impl_from_public(auth);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "auth is required");
  }
  if (cai_chatgpt_auth_should_refresh(impl, error)) {
    rc = auth->refresh(auth, error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  *out = cai_strdup(NULL, impl->access_token);
  if (*out == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate access token");
  }
  return CAI_OK;
}

void cai_chatgpt_login_config_init(cai_chatgpt_login_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
}

void cai_chatgpt_login_browser_config_init(
    cai_chatgpt_login_browser_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
}

static const char *cai_chatgpt_login_default_browser_command(void) {
#if defined(__APPLE__)
  return "open";
#else
  return "xdg-open";
#endif
}

int cai_chatgpt_login_browser_command(char *out, size_t out_size,
                                      cai_error *error) {
  const char *command;
  size_t length;

  if (out == NULL || out_size == 0U) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "browser command output buffer is required");
  }
  command = cai_chatgpt_login_default_browser_command();
  length = strlen(command);
  if (length + 1U > out_size) {
    out[0] = '\0';
    return cai_set_error(error, CAI_ERR_INVALID,
                         "browser command output buffer is too small");
  }
  memcpy(out, command, length + 1U);
  return CAI_OK;
}

int cai_chatgpt_login_open_browser_with_config(
    const cai_chatgpt_login_browser_config *config, const char *authorize_url,
    cai_error *error) {
  const char *command;
  ssize_t count;
  ssize_t ignored_write;
  int child_errno;
  int child_status;
  int pipe_fds[2];
  pid_t pid;
  pid_t opener_pid;

  if (authorize_url == NULL || authorize_url[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "authorization URL is required");
  }
  command = config != NULL ? config->command : NULL;
  if (command == NULL || command[0] == '\0') {
    command = cai_chatgpt_login_default_browser_command();
  }
  if (pipe(pipe_fds) != 0) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to create browser opener pipe",
                                strerror(errno));
  }
  if (fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC) != 0) {
    child_errno = errno;
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to configure browser opener pipe",
                                strerror(child_errno));
  }
  pid = fork();
  if (pid < 0) {
    child_errno = errno;
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to launch browser opener",
                                strerror(child_errno));
  }
  if (pid == 0) {
    close(pipe_fds[0]);
    opener_pid = fork();
    if (opener_pid < 0) {
      child_errno = errno;
      ignored_write = write(pipe_fds[1], &child_errno, sizeof(child_errno));
      (void)ignored_write;
      _exit(127);
    }
    if (opener_pid > 0) {
      close(pipe_fds[1]);
      _exit(0);
    }
    unsetenv("LD_PRELOAD");
    execlp(command, command, authorize_url, (char *)NULL);
    child_errno = errno;
    ignored_write = write(pipe_fds[1], &child_errno, sizeof(child_errno));
    (void)ignored_write;
    _exit(127);
  }
  close(pipe_fds[1]);
  child_errno = 0;
  count = read(pipe_fds[0], &child_errno, sizeof(child_errno));
  close(pipe_fds[0]);
  while (waitpid(pid, &child_status, 0) < 0 && errno == EINTR) {
  }
  if (count > 0) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to execute browser opener",
                                strerror(child_errno));
  }
  if (count < 0) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to observe browser opener launch",
                                strerror(errno));
  }
  return CAI_OK;
}

int cai_chatgpt_login_open_browser(const char *authorize_url,
                                   cai_error *error) {
  return cai_chatgpt_login_open_browser_with_config(NULL, authorize_url, error);
}

static int cai_chatgpt_login_build_authorize_url(cai_chatgpt_login_impl *login,
                                                 char **out, cai_error *error) {
  cai_auth_buffer url;
  char *base;
  char *challenge;
  int first;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "authorize URL output pointer is required");
  }
  *out = NULL;
  base = NULL;
  challenge = NULL;
  memset(&url, 0, sizeof(url));
  rc = cai_auth_issuer_url(&login->allocator, login->issuer, "/oauth/authorize",
                           &base, error);
  if (rc == CAI_OK) {
    rc = cai_auth_pkce_challenge(login->code_verifier, &challenge, error);
  }
  if (rc == CAI_OK) {
    rc = cai_auth_buffer_append(&url, base, error);
  }
  first = 1;
  if (rc == CAI_OK) {
    rc = cai_auth_append_query_param(&url, "client_id", login->client_id,
                                     &first, error);
  }
  if (rc == CAI_OK) {
    rc = cai_auth_append_query_param(&url, "response_type", "code", &first,
                                     error);
  }
  if (rc == CAI_OK) {
    rc = cai_auth_append_query_param(&url, "redirect_uri", login->redirect_uri,
                                     &first, error);
  }
  if (rc == CAI_OK) {
    rc = cai_auth_append_query_param(&url, "scope", login->scopes, &first,
                                     error);
  }
  if (rc == CAI_OK) {
    rc = cai_auth_append_query_param(&url, "code_challenge", challenge, &first,
                                     error);
  }
  if (rc == CAI_OK) {
    rc = cai_auth_append_query_param(&url, "code_challenge_method", "S256",
                                     &first, error);
  }
  if (rc == CAI_OK) {
    rc =
        cai_auth_append_query_param(&url, "state", login->state, &first, error);
  }
  if (rc == CAI_OK) {
    rc = cai_auth_append_query_param(&url, "id_token_add_organizations", "true",
                                     &first, error);
  }
  if (rc == CAI_OK) {
    rc = cai_auth_append_query_param(&url, "codex_cli_simplified_flow", "true",
                                     &first, error);
  }
  if (rc == CAI_OK) {
    rc = cai_auth_append_query_param(&url, "originator", login->originator,
                                     &first, error);
  }
  cai_free_mem(&login->allocator, base);
  cai_free_mem(NULL, challenge);
  if (rc != CAI_OK) {
    cai_free_mem(NULL, url.data);
    return rc;
  }
  *out = url.data;
  return CAI_OK;
}

int cai_chatgpt_login_start(const cai_chatgpt_login_config *config,
                            cai_chatgpt_login **out, char **out_authorize_url,
                            cai_error *error) {
  cai_chatgpt_login_config defaults;
  const cai_chatgpt_login_config *effective;
  cai_chatgpt_login *login;
  cai_chatgpt_login_impl *impl;
  char *default_auth_json_path;
  const char *auth_json_path;
  char *generated_state;
  char *generated_verifier;
  int rc;

  default_auth_json_path = NULL;
  if (out == NULL || out_authorize_url == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "login and authorize URL outputs are required");
  }
  *out = NULL;
  *out_authorize_url = NULL;
  if (config == NULL) {
    cai_chatgpt_login_config_init(&defaults);
    effective = &defaults;
  } else {
    effective = config;
  }
  if (!cai_auth_allocator_is_empty(&effective->allocator) &&
      !cai_auth_allocator_is_complete(&effective->allocator)) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "custom allocator requires malloc, realloc, and free");
  }
  auth_json_path = effective->auth_json_path;
  if (auth_json_path == NULL || auth_json_path[0] == '\0') {
    rc = cai_chatgpt_auth_default_path(&default_auth_json_path, error);
    if (rc != CAI_OK) {
      return rc;
    }
    auth_json_path = default_auth_json_path;
  }
  if (effective->redirect_uri == NULL || effective->redirect_uri[0] == '\0') {
    cai_free_mem(NULL, default_auth_json_path);
    return cai_set_error(error, CAI_ERR_INVALID, "redirect URI is required");
  }
  generated_state = NULL;
  generated_verifier = NULL;
  login = (cai_chatgpt_login *)cai_alloc(&effective->allocator, sizeof(*login));
  if (login == NULL) {
    cai_free_mem(NULL, default_auth_json_path);
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate login");
  }
  memset(login, 0, sizeof(*login));
  login->allocator = effective->allocator;
  login->handle_callback = cai_chatgpt_login_handle_callback;
  login->completed = cai_chatgpt_login_completed;
  login->close = cai_chatgpt_login_close;
  impl =
      (cai_chatgpt_login_impl *)cai_alloc(&effective->allocator, sizeof(*impl));
  if (impl == NULL) {
    cai_free_mem(&effective->allocator, login);
    cai_free_mem(NULL, default_auth_json_path);
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate login");
  }
  memset(impl, 0, sizeof(*impl));
  login->impl = impl;
  impl->allocator = effective->allocator;
  rc = CAI_OK;
  if (effective->state == NULL) {
    rc = cai_auth_random_b64(24U, &generated_state, error);
  }
  if (rc == CAI_OK && effective->code_verifier == NULL) {
    rc = cai_auth_random_b64(32U, &generated_verifier, error);
  }
  if (rc == CAI_OK) {
    impl->auth_json_path = cai_strdup(&impl->allocator, auth_json_path);
    impl->issuer =
        cai_strdup(&impl->allocator, effective->issuer != NULL
                                         ? effective->issuer
                                         : CAI_CHATGPT_AUTH_DEFAULT_ISSUER);
    impl->client_id =
        cai_strdup(&impl->allocator, effective->client_id != NULL
                                         ? effective->client_id
                                         : CAI_CHATGPT_AUTH_DEFAULT_CLIENT_ID);
    impl->redirect_uri = cai_strdup(&impl->allocator, effective->redirect_uri);
    impl->callback_path = cai_strdup(
        &impl->allocator, effective->callback_path != NULL
                              ? effective->callback_path
                              : CAI_CHATGPT_AUTH_DEFAULT_CALLBACK_PATH);
    impl->scopes =
        cai_strdup(&impl->allocator, effective->scopes != NULL
                                         ? effective->scopes
                                         : CAI_CHATGPT_AUTH_DEFAULT_SCOPES);
    impl->originator =
        cai_strdup(&impl->allocator, effective->originator != NULL
                                         ? effective->originator
                                         : CAI_CHATGPT_AUTH_DEFAULT_ORIGINATOR);
    impl->state = cai_strdup(&impl->allocator, effective->state != NULL
                                                   ? effective->state
                                                   : generated_state);
    impl->code_verifier =
        cai_strdup(&impl->allocator, effective->code_verifier != NULL
                                         ? effective->code_verifier
                                         : generated_verifier);
    impl->http_timeout_ms = effective->http_timeout_ms > 0
                                ? effective->http_timeout_ms
                                : CAI_CHATGPT_AUTH_DEFAULT_HTTP_TIMEOUT_MS;
    impl->insecure_skip_verify = effective->insecure_skip_verify;
    impl->ca_bundle_path =
        cai_strdup(&impl->allocator, effective->ca_bundle_path);
    impl->ca_path = cai_strdup(&impl->allocator, effective->ca_path);
    impl->logger = effective->logger;
    impl->logger_disabled = effective->logger_disabled;
    if (impl->auth_json_path == NULL || impl->issuer == NULL ||
        impl->client_id == NULL || impl->redirect_uri == NULL ||
        impl->callback_path == NULL || impl->scopes == NULL ||
        impl->originator == NULL || impl->state == NULL ||
        impl->code_verifier == NULL ||
        (effective->ca_bundle_path != NULL && impl->ca_bundle_path == NULL) ||
        (effective->ca_path != NULL && impl->ca_path == NULL)) {
      rc = cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate login");
    }
  }
  cai_auth_secure_clear(generated_verifier);
  cai_free_mem(NULL, generated_verifier);
  cai_free_mem(NULL, generated_state);
  cai_free_mem(NULL, default_auth_json_path);
  if (rc == CAI_OK) {
    rc = cai_chatgpt_login_build_authorize_url(impl, &impl->authorize_url,
                                               error);
  }
  if (rc != CAI_OK) {
    login->close(login);
    return rc;
  }
  *out_authorize_url = cai_strdup(NULL, impl->authorize_url);
  if (*out_authorize_url == NULL) {
    login->close(login);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate authorize URL");
  }
  *out = login;
  return CAI_OK;
}

static int cai_chatgpt_login_target_matches(cai_chatgpt_login_impl *login,
                                            const char *target) {
  const char *query;
  size_t path_len;

  if (login == NULL || target == NULL) {
    return 0;
  }
  query = strchr(target, '?');
  path_len = query != NULL ? (size_t)(query - target) : strlen(target);
  return strlen(login->callback_path) == path_len &&
         strncmp(target, login->callback_path, path_len) == 0;
}

static int cai_chatgpt_login_set_response(cai_chatgpt_login_response *response,
                                          int status, const char *body,
                                          int completed, cai_error *error) {
  if (response == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "login response output is required");
  }
  memset(response, 0, sizeof(*response));
  response->status = status;
  response->content_type = "text/html; charset=utf-8";
  response->body = cai_strdup(NULL, body != NULL ? body : "");
  if (response->body == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate login response body");
  }
  response->completed = completed;
  return CAI_OK;
}

static int cai_auth_append_form_param(cai_auth_buffer *buffer, const char *name,
                                      const char *value, int *first,
                                      cai_error *error) {
  char *encoded;
  int rc;

  encoded = NULL;
  rc = cai_auth_url_encode(value != NULL ? value : "", &encoded, error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (!*first) {
    rc = cai_auth_buffer_append(buffer, "&", error);
  }
  if (rc == CAI_OK) {
    rc = cai_auth_buffer_append(buffer, name, error);
  }
  if (rc == CAI_OK) {
    rc = cai_auth_buffer_append(buffer, "=", error);
  }
  if (rc == CAI_OK) {
    rc = cai_auth_buffer_append(buffer, encoded, error);
  }
  cai_free_mem(NULL, encoded);
  *first = 0;
  return rc;
}

static int cai_chatgpt_login_exchange_code(cai_chatgpt_login_impl *login,
                                           const char *code, cai_error *error) {
  cai_auth_refresh_response_doc response;
  cai_chatgpt_auth_impl auth_view;
  cai_auth_buffer form;
  char *url;
  char *response_json;
  long status;
  lonejson_error json_error;
  lonejson_status json_status;
  int first;
  int rc;

  memset(&form, 0, sizeof(form));
  url = NULL;
  response_json = NULL;
  first = 1;
  rc = cai_auth_append_form_param(&form, "grant_type", "authorization_code",
                                  &first, error);
  if (rc == CAI_OK) {
    rc = cai_auth_append_form_param(&form, "code", code, &first, error);
  }
  if (rc == CAI_OK) {
    rc = cai_auth_append_form_param(&form, "redirect_uri", login->redirect_uri,
                                    &first, error);
  }
  if (rc == CAI_OK) {
    rc = cai_auth_append_form_param(&form, "client_id", login->client_id,
                                    &first, error);
  }
  if (rc == CAI_OK) {
    rc = cai_auth_append_form_param(&form, "code_verifier",
                                    login->code_verifier, &first, error);
  }
  if (rc == CAI_OK) {
    rc = cai_auth_issuer_url(&login->allocator, login->issuer, "/oauth/token",
                             &url, error);
  }
  if (rc == CAI_OK) {
    rc = cai_auth_http_post(
        &login->allocator, url, "application/x-www-form-urlencoded", form.data,
        login->http_timeout_ms, login->insecure_skip_verify,
        login->ca_bundle_path, login->ca_path, &response_json, &status, error);
  }
  cai_auth_secure_clear(form.data);
  cai_free_mem(NULL, form.data);
  cai_free_mem(&login->allocator, url);
  if (rc != CAI_OK) {
    return rc;
  }
  if (status < 200L || status >= 300L) {
    cai_auth_secure_clear(response_json);
    cai_free_mem(NULL, response_json);
    return cai_set_error_http(error, CAI_ERR_TRANSPORT, status,
                              "ChatGPT authorization-code exchange failed",
                              NULL, NULL, NULL);
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
                                "failed to parse code exchange response",
                                json_error.message);
  }
  if (response.access_token == NULL || response.refresh_token == NULL) {
    CAI_LJ->cleanup(CAI_LJ, &cai_auth_refresh_response_map, &response);
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "code exchange response omitted required tokens");
  }
  memset(&auth_view, 0, sizeof(auth_view));
  auth_view.allocator = login->allocator;
  auth_view.auth_json_path = login->auth_json_path;
  auth_view.id_token = response.id_token;
  auth_view.access_token = response.access_token;
  auth_view.refresh_token = response.refresh_token;
  auth_view.account_id = response.account_id;
  rc = cai_chatgpt_auth_save(&auth_view, error);
  CAI_LJ->cleanup(CAI_LJ, &cai_auth_refresh_response_map, &response);
  return rc;
}

int cai_chatgpt_login_handle_callback(cai_chatgpt_login *login,
                                      const cai_chatgpt_login_request *request,
                                      cai_chatgpt_login_response *response,
                                      cai_error *error) {
  cai_chatgpt_login_impl *impl;
  char *state;
  char *code;
  char *oauth_error;
  int rc;

  if (response != NULL) {
    memset(response, 0, sizeof(*response));
  }
  impl = cai_chatgpt_login_impl_from_public(login);
  if (impl == NULL || request == NULL || response == NULL ||
      request->method == NULL || request->target == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "login, request, and response are required");
  }
  if (strcmp(request->method, "GET") != 0) {
    return cai_chatgpt_login_set_response(
        response, 405,
        "<!doctype html><title>Method Not Allowed</title>"
        "<h1>Method Not Allowed</h1>",
        0, error);
  }
  if (!cai_chatgpt_login_target_matches(impl, request->target)) {
    return cai_chatgpt_login_set_response(
        response, 404,
        "<!doctype html><title>Not Found</title><h1>Not Found</h1>", 0, error);
  }
  state = NULL;
  code = NULL;
  oauth_error = NULL;
  rc = cai_auth_query_param(request->target, "error", &oauth_error, error);
  if (rc == CAI_OK && oauth_error != NULL) {
    rc = cai_chatgpt_login_set_response(
        response, 400,
        "<!doctype html><title>ChatGPT login failed</title>"
        "<h1>ChatGPT login failed</h1><p>The OAuth issuer returned an "
        "error.</p>",
        1, error);
  }
  if (rc == CAI_OK && oauth_error == NULL) {
    rc = cai_auth_query_param(request->target, "state", &state, error);
  }
  if (rc == CAI_OK && oauth_error == NULL &&
      (state == NULL || strcmp(state, impl->state) != 0)) {
    rc = cai_chatgpt_login_set_response(
        response, 400,
        "<!doctype html><title>ChatGPT login failed</title>"
        "<h1>ChatGPT login failed</h1><p>OAuth state did not match.</p>",
        1, error);
  }
  if (rc == CAI_OK && oauth_error == NULL && response->body == NULL) {
    rc = cai_auth_query_param(request->target, "code", &code, error);
  }
  if (rc == CAI_OK && oauth_error == NULL && response->body == NULL &&
      (code == NULL || code[0] == '\0')) {
    rc = cai_chatgpt_login_set_response(
        response, 400,
        "<!doctype html><title>ChatGPT login failed</title>"
        "<h1>ChatGPT login failed</h1><p>OAuth callback omitted code.</p>",
        1, error);
  }
  if (rc == CAI_OK && oauth_error == NULL && response->body == NULL) {
    rc = cai_chatgpt_login_exchange_code(impl, code, error);
    if (rc == CAI_OK) {
      impl->completed = 1;
      rc = cai_chatgpt_login_set_response(
          response, 200,
          "<!doctype html><title>ChatGPT login complete</title>"
          "<h1>ChatGPT login complete</h1>"
          "<p>You can close this browser tab and return to the terminal.</p>",
          1, error);
    }
  }
  cai_auth_secure_clear(code);
  cai_free_mem(NULL, code);
  cai_free_mem(NULL, state);
  cai_free_mem(NULL, oauth_error);
  return rc;
}

void cai_chatgpt_login_response_cleanup(cai_chatgpt_login_response *response) {
  if (response == NULL) {
    return;
  }
  cai_auth_secure_clear(response->body);
  cai_free_mem(NULL, response->body);
  memset(response, 0, sizeof(*response));
}

int cai_chatgpt_login_completed(const cai_chatgpt_login *login) {
  const cai_chatgpt_login_impl *impl;

  impl = cai_chatgpt_login_impl_from_const_public(login);
  return impl != NULL ? impl->completed : 0;
}

void cai_chatgpt_login_close(cai_chatgpt_login *login) {
  cai_chatgpt_login_impl *impl;
  cai_allocator allocator;

  if (login == NULL) {
    return;
  }
  allocator = login->allocator;
  impl = cai_chatgpt_login_impl_from_public(login);
  if (impl != NULL) {
    cai_free_mem(&impl->allocator, impl->auth_json_path);
    cai_free_mem(&impl->allocator, impl->issuer);
    cai_free_mem(&impl->allocator, impl->client_id);
    cai_free_mem(&impl->allocator, impl->redirect_uri);
    cai_free_mem(&impl->allocator, impl->callback_path);
    cai_free_mem(&impl->allocator, impl->scopes);
    cai_free_mem(&impl->allocator, impl->originator);
    cai_free_mem(&impl->allocator, impl->state);
    cai_auth_free_secret(&impl->allocator, impl->code_verifier);
    cai_free_mem(&impl->allocator, impl->authorize_url);
    cai_free_mem(&impl->allocator, impl->ca_bundle_path);
    cai_free_mem(&impl->allocator, impl->ca_path);
    memset(impl, 0, sizeof(*impl));
    cai_free_mem(&allocator, impl);
  }
  memset(login, 0, sizeof(*login));
  cai_free_mem(&allocator, login);
}

void cai_chatgpt_auth_close(cai_chatgpt_auth *auth) {
  cai_chatgpt_auth_impl *impl;
  cai_allocator allocator;

  if (auth == NULL) {
    return;
  }
  allocator = auth->allocator;
  impl = cai_chatgpt_auth_impl_from_public(auth);
  if (impl != NULL) {
    cai_free_mem(&impl->allocator, impl->auth_json_path);
    cai_free_mem(&impl->allocator, impl->issuer);
    cai_free_mem(&impl->allocator, impl->client_id);
    cai_free_mem(&impl->allocator, impl->ca_bundle_path);
    cai_free_mem(&impl->allocator, impl->ca_path);
    cai_auth_free_secret(&impl->allocator, impl->id_token);
    cai_auth_free_secret(&impl->allocator, impl->access_token);
    cai_auth_free_secret(&impl->allocator, impl->refresh_token);
    cai_auth_free_secret(&impl->allocator, impl->account_id);
    memset(impl, 0, sizeof(*impl));
    cai_free_mem(&allocator, impl);
  }
  memset(auth, 0, sizeof(*auth));
  cai_free_mem(&allocator, auth);
}
