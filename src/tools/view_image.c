#include "../cai_internal.h"

#include <cai/tools/view_image.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/param.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

#define CAI_VIEW_IMAGE_DEFAULT_MAX_BYTES (20U * 1024U * 1024U)

extern char *realpath(const char *path, char *resolved_path);

typedef struct cai_view_image_context {
  char *root_path;
  char *default_workdir;
  size_t max_image_bytes;
  char *pending_data_url;
  char *pending_detail;
} cai_view_image_context;

typedef struct cai_view_image_args {
  char *path;
  char *detail;
} cai_view_image_args;

typedef struct cai_view_image_result {
  char *path;
  char *resolved_path;
  char *mime_type;
  char *detail;
  char *content_hash;
  long long byte_count;
} cai_view_image_result;

static const lonejson_field cai_view_image_arg_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_view_image_args, path, "path"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_view_image_args, detail,
                                          "detail")};
LONEJSON_MAP_DEFINE(cai_view_image_args_map, cai_view_image_args,
                    cai_view_image_arg_fields);

static const lonejson_field cai_view_image_result_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_view_image_result, path, "path"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_view_image_result, resolved_path,
                                    "resolved_path"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_view_image_result, mime_type,
                                    "mime_type"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_view_image_result, detail, "detail"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_view_image_result, content_hash,
                                    "content_hash"),
    LONEJSON_FIELD_I64_REQ(cai_view_image_result, byte_count, "byte_count")};
LONEJSON_MAP_DEFINE(cai_view_image_result_map, cai_view_image_result,
                    cai_view_image_result_fields);

static const char cai_view_image_schema_json[] =
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
    "\"path\":{\"type\":\"string\"},"
    "\"detail\":{\"type\":[\"string\",\"null\"],"
    "\"enum\":[\"high\",\"original\",null]}"
    "},"
    "\"required\":[\"path\"],"
    "\"additionalProperties\":false"
    "}";

static const char cai_view_image_default_description[] =
    "View a local image file from the configured workspace when visual "
    "inspection is needed. Use this only for images already available on "
    "disk. The image is attached to the model as typed image content; it is "
    "not returned as text. detail defaults to high; use original to request "
    "the source resolution. Paths must remain inside the configured root and "
    "must resolve to a private regular image file.";

static int cai_view_image_path_is_under_root(const char *root,
                                             const char *path) {
  size_t root_len;

  if (root == NULL || path == NULL) {
    return 0;
  }
  root_len = strlen(root);
  if (root_len == 1U && root[0] == '/') {
    return path[0] == '/' ? 1 : 0;
  }
  return strncmp(root, path, root_len) == 0 &&
                 (path[root_len] == '\0' || path[root_len] == '/')
             ? 1
             : 0;
}

static int cai_view_image_copy(char **out, const char *value,
                               const char *message, cai_error *error) {
  *out = cai_strdup(NULL, value);
  return *out != NULL ? CAI_OK : cai_set_error(error, CAI_ERR_NOMEM, message);
}

static int cai_view_image_realpath_copy(const char *path, char **out,
                                        cai_error *error) {
  char resolved[PATH_MAX];

  if (path == NULL || path[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "image path is required");
  }
  if (realpath(path, resolved) == NULL) {
    return cai_set_error_detail(error, CAI_ERR_INVALID,
                                "failed to resolve image path",
                                strerror(errno));
  }
  return cai_view_image_copy(out, resolved, "failed to allocate image path",
                             error);
}

static int cai_view_image_require_directory(const char *path,
                                            const char *description,
                                            cai_error *error) {
  struct stat st;

  if (stat(path, &st) != 0) {
    return cai_set_error_detail(error, CAI_ERR_INVALID,
                                "failed to inspect view_image directory",
                                strerror(errno));
  }
  if (!S_ISDIR(st.st_mode)) {
    return cai_set_error(error, CAI_ERR_INVALID, description);
  }
  return CAI_OK;
}

static int cai_view_image_context_new(const cai_view_image_tool_config *config,
                                      cai_view_image_context **out,
                                      cai_error *error) {
  cai_view_image_context *context;
  const char *workdir;
  int rc;

  if (out == NULL || config == NULL || config->root_path == NULL ||
      config->root_path[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "view_image sandbox root is required");
  }
  *out = NULL;
  context = (cai_view_image_context *)cai_alloc(NULL, sizeof(*context));
  if (context == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate view_image context");
  }
  memset(context, 0, sizeof(*context));
  rc = cai_view_image_realpath_copy(config->root_path, &context->root_path,
                                    error);
  if (rc == CAI_OK) {
    rc = cai_view_image_require_directory(
        context->root_path, "view_image sandbox root must be a directory",
        error);
  }
  workdir = config->default_workdir != NULL ? config->default_workdir
                                            : config->root_path;
  if (rc == CAI_OK) {
    rc =
        cai_view_image_realpath_copy(workdir, &context->default_workdir, error);
  }
  if (rc == CAI_OK) {
    rc = cai_view_image_require_directory(
        context->default_workdir,
        "view_image working directory must be a directory", error);
  }
  if (rc == CAI_OK && !cai_view_image_path_is_under_root(
                          context->root_path, context->default_workdir)) {
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "view_image working directory escapes configured root");
  }
  if (rc != CAI_OK) {
    cai_free_mem(NULL, context->root_path);
    cai_free_mem(NULL, context->default_workdir);
    cai_free_mem(NULL, context);
    return rc;
  }
  context->max_image_bytes = config->max_image_bytes != 0U
                                 ? config->max_image_bytes
                                 : CAI_VIEW_IMAGE_DEFAULT_MAX_BYTES;
  *out = context;
  return CAI_OK;
}

static void cai_view_image_context_cleanup(void *value) {
  cai_view_image_context *context;

  context = (cai_view_image_context *)value;
  if (context == NULL) {
    return;
  }
  cai_free_mem(NULL, context->root_path);
  cai_free_mem(NULL, context->default_workdir);
  cai_free_mem(NULL, context->pending_data_url);
  cai_free_mem(NULL, context->pending_detail);
  cai_free_mem(NULL, context);
}

static int cai_view_image_opened_path(int fd, char **out, cai_error *error) {
  char path[PATH_MAX];

#if defined(__linux__)
  {
    char link_path[64];
    ssize_t length;

    (void)snprintf(link_path, sizeof(link_path), "/proc/self/fd/%d", fd);
    length = readlink(link_path, path, sizeof(path) - 1U);
    if (length < 0 || (size_t)length >= sizeof(path)) {
      return cai_set_error_detail(
          error, CAI_ERR_INVALID, "failed to verify opened image path",
          length < 0 ? strerror(errno) : "path too long");
    }
    path[length] = '\0';
  }
#elif defined(__APPLE__)
  if (fcntl(fd, F_GETPATH, path) != 0) {
    return cai_set_error_detail(error, CAI_ERR_INVALID,
                                "failed to verify opened image path",
                                strerror(errno));
  }
#else
  (void)fd;
  return cai_set_error(error, CAI_ERR_INVALID,
                       "opened image path verification is not supported");
#endif
  return cai_view_image_copy(out, path, "failed to allocate opened image path",
                             error);
}

static int cai_view_image_open(const cai_view_image_context *context,
                               const char *path, int *out_fd,
                               char **out_resolved, long long *out_size,
                               cai_error *error) {
  char candidate[PATH_MAX];
  const char *open_path;
  char *resolved;
  char *opened_path;
  struct stat st;
  int fd;
  int length;

  if (context == NULL || out_fd == NULL || out_resolved == NULL ||
      out_size == NULL || path == NULL || path[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "view_image path and output state are required");
  }
  *out_fd = -1;
  *out_resolved = NULL;
  *out_size = 0LL;
  if (path[0] == '/') {
    open_path = path;
  } else {
    length = snprintf(candidate, sizeof(candidate), "%s/%s",
                      context->default_workdir, path);
    if (length < 0 || (size_t)length >= sizeof(candidate)) {
      return cai_set_error(error, CAI_ERR_INVALID, "image path is too long");
    }
    open_path = candidate;
  }
  resolved = NULL;
  opened_path = NULL;
  if (cai_view_image_realpath_copy(open_path, &resolved, error) != CAI_OK) {
    return error != NULL ? error->code : CAI_ERR_INVALID;
  }
  if (!cai_view_image_path_is_under_root(context->root_path, resolved)) {
    cai_free_mem(NULL, resolved);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "image path escapes configured root");
  }
  fd = open(resolved, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) {
    cai_free_mem(NULL, resolved);
    return cai_set_error_detail(error, CAI_ERR_INVALID,
                                "failed to open image file", strerror(errno));
  }
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1) {
    close(fd);
    cai_free_mem(NULL, resolved);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "image path must be a private regular file");
  }
  if (cai_view_image_opened_path(fd, &opened_path, error) != CAI_OK) {
    close(fd);
    cai_free_mem(NULL, opened_path);
    cai_free_mem(NULL, resolved);
    return error != NULL ? error->code : CAI_ERR_INVALID;
  }
  if (!cai_view_image_path_is_under_root(context->root_path, opened_path)) {
    close(fd);
    cai_free_mem(NULL, opened_path);
    cai_free_mem(NULL, resolved);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "opened image path escapes configured root");
  }
  cai_free_mem(NULL, opened_path);
  if (st.st_size < 0 || (unsigned long long)st.st_size >
                            (unsigned long long)context->max_image_bytes) {
    close(fd);
    cai_free_mem(NULL, resolved);
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "image exceeds configured byte limit");
  }
  *out_fd = fd;
  *out_resolved = resolved;
  *out_size = (long long)st.st_size;
  return CAI_OK;
}

static const char *cai_view_image_mime_type(const unsigned char *data,
                                            size_t length) {
  if (length >= 33U && memcmp(data, "\x89PNG\r\n\x1a\n", 8U) == 0 &&
      memcmp(data + 12U, "IHDR", 4U) == 0) {
    return "image/png";
  }
  if (length >= 4U && data[0] == 0xffU && data[1] == 0xd8U &&
      data[2] == 0xffU && data[3] != 0x00U && data[3] != 0xffU) {
    return "image/jpeg";
  }
  if (length >= 13U &&
      (memcmp(data, "GIF87a", 6U) == 0 || memcmp(data, "GIF89a", 6U) == 0)) {
    return "image/gif";
  }
  if (length >= 12U && memcmp(data, "RIFF", 4U) == 0 &&
      memcmp(data + 8U, "WEBP", 4U) == 0) {
    return "image/webp";
  }
  return NULL;
}

static int cai_view_image_read_all(int fd, long long file_size,
                                   size_t max_bytes, unsigned char **out,
                                   size_t *out_size, cai_error *error) {
  unsigned char *data;
  size_t capacity;
  size_t offset;

  if (file_size < 0 ||
      (unsigned long long)file_size > (unsigned long long)max_bytes) {
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "image exceeds configured byte limit");
  }
  capacity = (size_t)file_size + 1U;
  data = (unsigned char *)cai_alloc(NULL, capacity);
  if (data == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate image data");
  }
  offset = 0U;
  while (offset < capacity) {
    ssize_t nread;

    nread = read(fd, data + offset, capacity - offset);
    if (nread < 0 && errno == EINTR) {
      continue;
    }
    if (nread < 0) {
      cai_free_mem(NULL, data);
      return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                  "failed to read image file", strerror(errno));
    }
    if (nread == 0) {
      break;
    }
    offset += (size_t)nread;
  }
  if (offset == capacity) {
    cai_free_mem(NULL, data);
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "image changed beyond configured byte limit");
  }
  *out = data;
  *out_size = offset;
  return CAI_OK;
}

static int cai_view_image_make_data_url(const char *mime_type,
                                        const unsigned char *data,
                                        size_t data_size, char **out,
                                        cai_error *error) {
  static const char prefix[] = "data:";
  static const char suffix[] = ";base64,";
  size_t encoded_size;
  size_t total_size;
  char *url;

  if (data_size > (size_t)INT_MAX || data_size > (SIZE_MAX - 2U) / 4U) {
    return cai_set_error(error, CAI_ERR_LIMIT, "image is too large to encode");
  }
  encoded_size = ((data_size + 2U) / 3U) * 4U;
  if (strlen(prefix) > SIZE_MAX - strlen(mime_type) ||
      strlen(prefix) + strlen(mime_type) > SIZE_MAX - strlen(suffix) ||
      strlen(prefix) + strlen(mime_type) + strlen(suffix) >
          SIZE_MAX - encoded_size - 1U) {
    return cai_set_error(error, CAI_ERR_LIMIT, "image data URL is too large");
  }
  total_size =
      strlen(prefix) + strlen(mime_type) + strlen(suffix) + encoded_size;
  url = (char *)cai_alloc(NULL, total_size + 1U);
  if (url == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate image data URL");
  }
  (void)snprintf(url, total_size + 1U, "%s%s%s", prefix, mime_type, suffix);
  if (EVP_EncodeBlock((unsigned char *)url + strlen(url), data,
                      (int)data_size) < 0) {
    cai_free_mem(NULL, url);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to encode image data URL");
  }
  *out = url;
  return CAI_OK;
}

static int cai_view_image_sha256(const unsigned char *data, size_t data_size,
                                 char **out, cai_error *error) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_size;
  char text[EVP_MAX_MD_SIZE * 2U + 1U];
  size_t i;

  digest_size = 0U;
  if (EVP_Digest(data, data_size, digest, &digest_size, EVP_sha256(), NULL) !=
          1 ||
      digest_size != 32U) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to hash image content");
  }
  for (i = 0U; i < (size_t)digest_size; i++) {
    (void)snprintf(text + i * 2U, sizeof(text) - i * 2U, "%02x", digest[i]);
  }
  return cai_view_image_copy(out, text, "failed to allocate image hash", error);
}

static int cai_view_image_callback(void *value, const void *params, void *out,
                                   cai_error *error) {
  cai_view_image_context *context;
  const cai_view_image_args *args;
  cai_view_image_result *result;
  const char *detail;
  const char *mime_type;
  unsigned char *data;
  char *data_url;
  char *resolved_path;
  long long file_size;
  size_t data_size;
  int fd;
  int rc;

  context = (cai_view_image_context *)value;
  args = (const cai_view_image_args *)params;
  result = (cai_view_image_result *)out;
  if (context == NULL || args == NULL || result == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "invalid view_image callback state");
  }
  detail = args->detail != NULL ? args->detail : "high";
  if (strcmp(detail, "high") != 0 && strcmp(detail, "original") != 0) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "view_image detail must be high or original");
  }
  cai_free_mem(NULL, context->pending_data_url);
  cai_free_mem(NULL, context->pending_detail);
  context->pending_data_url = NULL;
  context->pending_detail = NULL;
  data = NULL;
  data_url = NULL;
  resolved_path = NULL;
  file_size = 0LL;
  data_size = 0U;
  fd = -1;
  rc = cai_view_image_open(context, args->path, &fd, &resolved_path, &file_size,
                           error);
  if (rc == CAI_OK) {
    rc = cai_view_image_read_all(fd, file_size, context->max_image_bytes, &data,
                                 &data_size, error);
  }
  if (rc == CAI_OK) {
    mime_type = cai_view_image_mime_type(data, data_size);
    if (mime_type == NULL) {
      rc = cai_set_error(error, CAI_ERR_INVALID,
                         "image data is invalid or unsupported");
    }
  } else {
    mime_type = NULL;
  }
  if (rc == CAI_OK) {
    rc = cai_view_image_make_data_url(mime_type, data, data_size, &data_url,
                                      error);
  }
  if (rc == CAI_OK) {
    rc = cai_view_image_sha256(data, data_size, &result->content_hash, error);
  }
  if (rc == CAI_OK) {
    result->path = cai_tool_result_strdup(args->path, error);
    result->resolved_path = cai_tool_result_strdup(resolved_path, error);
    result->mime_type = cai_tool_result_strdup(mime_type, error);
    result->detail = cai_tool_result_strdup(detail, error);
    result->byte_count = (long long)data_size;
    if (result->path == NULL || result->resolved_path == NULL ||
        result->mime_type == NULL || result->detail == NULL ||
        result->content_hash == NULL) {
      rc = error != NULL ? error->code : CAI_ERR_NOMEM;
    }
  }
  if (rc == CAI_OK) {
    context->pending_data_url = data_url;
    data_url = NULL;
    context->pending_detail = cai_strdup(NULL, detail);
    if (context->pending_detail == NULL) {
      cai_free_mem(NULL, context->pending_data_url);
      context->pending_data_url = NULL;
      rc = cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to preserve image detail");
    }
  }
  if (fd >= 0) {
    close(fd);
  }
  cai_free_mem(NULL, data);
  cai_free_mem(NULL, data_url);
  cai_free_mem(NULL, resolved_path);
  return rc;
}

static int cai_view_image_deliver(void *value, const char *call_id,
                                  cai_response_create_params *params,
                                  const lonejson_spooled *output_json,
                                  int *out_delivered, cai_error *error) {
  cai_view_image_context *context;
  int rc;

  (void)output_json;
  context = (cai_view_image_context *)value;
  if (out_delivered == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "view_image delivery output is required");
  }
  *out_delivered = 0;
  if (context == NULL || context->pending_data_url == NULL ||
      context->pending_detail == NULL) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "view_image has no pending image result");
  }
  rc = cai_response_create_params_add_function_call_output_image_url(
      params, call_id, context->pending_data_url, context->pending_detail,
      error);
  if (rc == CAI_OK) {
    cai_free_mem(NULL, context->pending_data_url);
    cai_free_mem(NULL, context->pending_detail);
    context->pending_data_url = NULL;
    context->pending_detail = NULL;
    *out_delivered = 1;
  }
  return rc;
}

int cai_tool_registry_register_view_image_tool(
    cai_tool_registry *registry, const cai_view_image_tool_config *config,
    cai_error *error) {
  cai_view_image_context *context;
  const char *name;
  const char *description;
  int registered;
  int rc;

  context = NULL;
  registered = 0;
  rc = cai_view_image_context_new(config, &context, error);
  if (rc != CAI_OK) {
    return rc;
  }
  name = config->name != NULL && config->name[0] != '\0'
             ? config->name
             : CAI_VIEW_IMAGE_DEFAULT_TOOL_NAME;
  description = config->description != NULL && config->description[0] != '\0'
                    ? config->description
                    : cai_view_image_default_description;
  rc = cai_tool_registry_register_lonejson_schema_owned(
      registry, name, description, cai_view_image_schema_json, 0,
      &cai_view_image_args_map, &cai_view_image_result_map,
      cai_view_image_callback, context, cai_view_image_context_cleanup, error);
  if (rc == CAI_OK) {
    registered = 1;
  }
  if (rc == CAI_OK) {
    rc = cai_tool_registry_set_result_delivery(registry, name,
                                               cai_view_image_deliver, error);
  }
  if (rc != CAI_OK && !registered) {
    cai_view_image_context_cleanup(context);
  }
  return rc;
}

int cai_agent_register_view_image_tool(cai_agent *agent,
                                       const cai_view_image_tool_config *config,
                                       cai_error *error) {
  cai_agent_impl *impl;

  if (agent == NULL || agent->impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  impl = CAI_AGENT_IMPL(agent);
  return cai_tool_registry_register_view_image_tool(impl->tools, config, error);
}
