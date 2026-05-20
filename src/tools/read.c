#include "../cai_internal.h"

#include <cai/tools/read.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

extern char *realpath(const char *path, char *resolved_path);

#define CAI_READ_DEFAULT_CONTENT_MEMORY_LIMIT (128U * 1024U)
#define CAI_READ_DEFAULT_CONTENT_MAX_BYTES (1024U * 1024U)

typedef struct cai_read_context {
  char *root_path;
  char *default_workdir;
  size_t content_memory_limit;
  size_t content_max_bytes;
  char *content_spool_dir;
} cai_read_context;

typedef struct cai_read_args {
  char *path;
  long long start_line;
  int has_start_line;
  long long end_line;
  int has_end_line;
  long long max_bytes;
  int has_max_bytes;
} cai_read_args;

typedef struct cai_read_result {
  char *path;
  char *resolved_path;
  long long start_line;
  long long end_line;
  long long byte_count;
  long long file_size;
  int truncated;
  lonejson_spooled content;
} cai_read_result;

static const lonejson_field cai_read_arg_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_read_args, path, "path"),
    LONEJSON_FIELD_I64_PRESENT_NULLABLE(cai_read_args, start_line,
                                        has_start_line, "start_line"),
    LONEJSON_FIELD_I64_PRESENT_NULLABLE(cai_read_args, end_line, has_end_line,
                                        "end_line"),
    LONEJSON_FIELD_I64_PRESENT_NULLABLE(cai_read_args, max_bytes,
                                        has_max_bytes, "max_bytes")};
LONEJSON_MAP_DEFINE(cai_read_args_map, cai_read_args, cai_read_arg_fields);

static const lonejson_field cai_read_result_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_read_result, path, "path"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_read_result, resolved_path,
                                    "resolved_path"),
    LONEJSON_FIELD_I64_REQ(cai_read_result, start_line, "start_line"),
    LONEJSON_FIELD_I64_REQ(cai_read_result, end_line, "end_line"),
    LONEJSON_FIELD_I64_REQ(cai_read_result, byte_count, "byte_count"),
    LONEJSON_FIELD_I64_REQ(cai_read_result, file_size, "file_size"),
    LONEJSON_FIELD_BOOL_REQ(cai_read_result, truncated, "truncated"),
    LONEJSON_FIELD_STRING_STREAM_REQ(cai_read_result, content, "content")};
LONEJSON_MAP_DEFINE(cai_read_result_map, cai_read_result,
                    cai_read_result_fields);

static const char cai_read_schema_json[] =
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
    "\"path\":{\"type\":\"string\"},"
    "\"start_line\":{\"type\":[\"integer\",\"null\"]},"
    "\"end_line\":{\"type\":[\"integer\",\"null\"]},"
    "\"max_bytes\":{\"type\":[\"integer\",\"null\"]}"
    "},"
    "\"required\":[\"path\"],"
    "\"additionalProperties\":false"
    "}";

static const char cai_read_default_description[] =
    "Reads a UTF-8/text file from the configured sandbox root. Use start_line "
    "and end_line for large files. Paths must stay inside the configured root; "
    "symlink and absolute-path escapes are rejected.";

static const char *cai_read_default_string(const char *value,
                                           const char *fallback) {
  return value != NULL && value[0] != '\0' ? value : fallback;
}

static int cai_read_strdup_field(char **out, const char *value,
                                 const char *message, cai_error *error) {
  *out = cai_strdup(NULL, value);
  if (*out == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, message);
  }
  return CAI_OK;
}

static int cai_read_strdup_optional(char **out, const char *value,
                                    const char *message, cai_error *error) {
  if (value == NULL || value[0] == '\0') {
    *out = NULL;
    return CAI_OK;
  }
  return cai_read_strdup_field(out, value, message, error);
}

static void cai_read_context_cleanup(void *context) {
  cai_read_context *ctx;

  ctx = (cai_read_context *)context;
  if (ctx == NULL) {
    return;
  }
  cai_free_mem(NULL, ctx->root_path);
  cai_free_mem(NULL, ctx->default_workdir);
  cai_free_mem(NULL, ctx->content_spool_dir);
  cai_free_mem(NULL, ctx);
}

static int cai_read_realpath_dup(const char *path, char **out,
                                 cai_error *error) {
  char resolved[PATH_MAX];

  if (path == NULL || path[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "path is required");
  }
  if (realpath(path, resolved) == NULL) {
    return cai_set_error_detail(error, CAI_ERR_INVALID,
                                "failed to resolve path", strerror(errno));
  }
  return cai_read_strdup_field(out, resolved, "failed to allocate path", error);
}

static int cai_read_path_is_under_root(const char *root, const char *path) {
  size_t root_len;

  if (root == NULL || path == NULL) {
    return 0;
  }
  root_len = strlen(root);
  if (strncmp(root, path, root_len) != 0) {
    return 0;
  }
  return path[root_len] == '\0' || path[root_len] == '/' ? 1 : 0;
}

static int cai_read_join(char *out, size_t out_size, const char *base,
                         const char *path, cai_error *error) {
  int n;

  if (base == NULL || path == NULL || path[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "path is required");
  }
  n = snprintf(out, out_size, "%s/%s", base, path);
  if (n < 0 || (size_t)n >= out_size) {
    return cai_set_error(error, CAI_ERR_INVALID, "path is too long");
  }
  return CAI_OK;
}

static int cai_read_resolve_file(const cai_read_context *ctx,
                                 const char *request_path,
                                 char **resolved_path, cai_error *error) {
  char candidate[PATH_MAX];
  char *resolved;
  struct stat st;
  int rc;

  resolved = NULL;
  if (request_path == NULL || request_path[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "read path is required");
  }
  if (request_path[0] == '/') {
    rc = cai_read_realpath_dup(request_path, &resolved, error);
  } else {
    rc = cai_read_join(candidate, sizeof(candidate), ctx->default_workdir,
                       request_path, error);
    if (rc == CAI_OK) {
      rc = cai_read_realpath_dup(candidate, &resolved, error);
    }
  }
  if (rc != CAI_OK) {
    return rc;
  }
  if (!cai_read_path_is_under_root(ctx->root_path, resolved)) {
    cai_free_mem(NULL, resolved);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "read path escapes configured root");
  }
  if (stat(resolved, &st) != 0) {
    cai_free_mem(NULL, resolved);
    return cai_set_error_detail(error, CAI_ERR_INVALID, "failed to stat file",
                                strerror(errno));
  }
  if (!S_ISREG(st.st_mode)) {
    cai_free_mem(NULL, resolved);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "read path must be a regular file");
  }
  *resolved_path = resolved;
  return CAI_OK;
}

static int cai_read_context_new(const cai_read_tool_config *config,
                                cai_read_context **out, cai_error *error) {
  cai_read_context *ctx;
  char *root;
  char *workdir;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "read context output pointer is required");
  }
  *out = NULL;
  if (config == NULL || config->root_path == NULL ||
      config->root_path[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "read tool root_path is required");
  }
  ctx = (cai_read_context *)cai_alloc(NULL, sizeof(*ctx));
  if (ctx == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate read tool context");
  }
  memset(ctx, 0, sizeof(*ctx));
  root = NULL;
  workdir = NULL;
  rc = cai_read_realpath_dup(config->root_path, &root, error);
  if (rc == CAI_OK) {
    if (config->default_workdir != NULL &&
        config->default_workdir[0] != '\0') {
      rc = cai_read_realpath_dup(config->default_workdir, &workdir, error);
    } else {
      rc = cai_read_strdup_field(&workdir, root,
                                 "failed to allocate read default workdir",
                                 error);
    }
  }
  if (rc == CAI_OK && !cai_read_path_is_under_root(root, workdir)) {
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "read tool default_workdir escapes configured root");
  }
  if (rc == CAI_OK) {
    ctx->root_path = root;
    ctx->default_workdir = workdir;
    root = NULL;
    workdir = NULL;
    ctx->content_memory_limit =
        config->content_memory_limit != 0U
            ? config->content_memory_limit
            : CAI_READ_DEFAULT_CONTENT_MEMORY_LIMIT;
    ctx->content_max_bytes = config->content_max_bytes != 0U
                                 ? config->content_max_bytes
                                 : CAI_READ_DEFAULT_CONTENT_MAX_BYTES;
    rc = cai_read_strdup_optional(&ctx->content_spool_dir,
                                  config->content_spool_dir,
                                  "failed to allocate read spool directory",
                                  error);
  }
  cai_free_mem(NULL, root);
  cai_free_mem(NULL, workdir);
  if (rc != CAI_OK) {
    cai_read_context_cleanup(ctx);
    return rc;
  }
  *out = ctx;
  return CAI_OK;
}

static int cai_read_append(lonejson_spooled *spool, const char *data,
                           size_t len, cai_error *error) {
  lonejson_error json_error;

  if (len == 0U) {
    return CAI_OK;
  }
  lonejson_error_init(&json_error);
  if (lonejson_spooled_append(spool, data, len, &json_error) !=
      LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to spool read content",
                                json_error.message);
  }
  return CAI_OK;
}

static int cai_read_stream_file(const cai_read_context *ctx,
                                const cai_read_args *args,
                                const char *resolved_path,
                                lonejson_spooled *content,
                                long long *byte_count,
                                long long *end_line_out, int *truncated,
                                long long *file_size, cai_error *error) {
  lonejson_spool_options spool_options;
  struct stat st;
  FILE *fp;
  char buffer[4096];
  long long start_line;
  long long end_line;
  long long max_bytes;
  long long current_line;
  long long last_line;
  long long written;
  size_t nread;
  size_t i;
  size_t start;
  int include;
  int stop;
  int saw_any;
  int rc;

  if (stat(resolved_path, &st) != 0) {
    return cai_set_error_detail(error, CAI_ERR_INVALID, "failed to stat file",
                                strerror(errno));
  }
  *file_size = (long long)st.st_size;
  start_line = args->has_start_line ? args->start_line : 1LL;
  if (start_line < 1LL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "start_line must be greater than zero");
  }
  end_line = args->has_end_line ? args->end_line : 0LL;
  if (end_line != 0LL && end_line < start_line) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "end_line must be greater than or equal to start_line");
  }
  max_bytes = ctx->content_max_bytes;
  if (args->has_max_bytes && args->max_bytes > 0LL) {
    max_bytes = args->max_bytes;
    if ((unsigned long long)max_bytes >
        (unsigned long long)ctx->content_max_bytes) {
      max_bytes = (long long)ctx->content_max_bytes;
    }
  }
  if (max_bytes <= 0LL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "read content byte limit is invalid");
  }
  spool_options = lonejson_default_spool_options();
  spool_options.memory_limit = ctx->content_memory_limit;
  spool_options.max_bytes = (size_t)max_bytes;
  spool_options.temp_dir = ctx->content_spool_dir;
  lonejson_spooled_init(content, &spool_options);
  fp = fopen(resolved_path, "rb");
  if (fp == NULL) {
    lonejson_spooled_cleanup(content);
    return cai_set_error_detail(error, CAI_ERR_INVALID, "failed to open file",
                                strerror(errno));
  }
  current_line = 1LL;
  last_line = start_line;
  written = 0LL;
  stop = 0;
  saw_any = 0;
  rc = CAI_OK;
  while (!stop && (nread = fread(buffer, 1U, sizeof(buffer), fp)) > 0U) {
    start = 0U;
    for (i = 0U; i < nread && !stop; i++) {
      include = current_line >= start_line &&
                (end_line == 0LL || current_line <= end_line);
      if (buffer[i] == '\n') {
        if (include) {
          size_t len;
          len = i + 1U - start;
          if (written + (long long)len > max_bytes) {
            len = (size_t)(max_bytes - written);
            *truncated = 1;
            stop = 1;
          }
          rc = cai_read_append(content, buffer + start, len, error);
          if (rc != CAI_OK) {
            stop = 1;
            break;
          }
          written += (long long)len;
          saw_any = 1;
          last_line = current_line;
          if (written >= max_bytes) {
            *truncated = 1;
            stop = 1;
          }
        }
        current_line++;
        start = i + 1U;
        if (end_line != 0LL && current_line > end_line) {
          stop = 1;
        }
      }
    }
    if (rc == CAI_OK && !stop && start < nread) {
      include = current_line >= start_line &&
                (end_line == 0LL || current_line <= end_line);
      if (include) {
        size_t len;
        len = nread - start;
        if (written + (long long)len > max_bytes) {
          len = (size_t)(max_bytes - written);
          *truncated = 1;
          stop = 1;
        }
        rc = cai_read_append(content, buffer + start, len, error);
        if (rc != CAI_OK) {
          stop = 1;
        } else {
          written += (long long)len;
          saw_any = 1;
          last_line = current_line;
          if (written >= max_bytes) {
            *truncated = 1;
            stop = 1;
          }
        }
      }
    }
  }
  if (rc == CAI_OK && ferror(fp)) {
    rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                              "failed while reading file", strerror(errno));
  }
  fclose(fp);
  if (rc != CAI_OK) {
    lonejson_spooled_cleanup(content);
    return rc;
  }
  *byte_count = written;
  *end_line_out = saw_any ? last_line : start_line;
  return CAI_OK;
}

static int cai_read_callback(void *context, const void *params, void *result,
                             cai_error *error) {
  const cai_read_context *ctx;
  const cai_read_args *args;
  cai_read_result *out;
  char *resolved_path;
  lonejson_spooled content;
  long long byte_count;
  long long end_line;
  long long file_size;
  int truncated;
  int has_content;
  int rc;

  ctx = (const cai_read_context *)context;
  args = (const cai_read_args *)params;
  out = (cai_read_result *)result;
  if (ctx == NULL || args == NULL || out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "read tool callback received invalid state");
  }
  resolved_path = NULL;
  truncated = 0;
  byte_count = 0LL;
  end_line = 0LL;
  file_size = 0LL;
  has_content = 0;
  rc = cai_read_resolve_file(ctx, args->path, &resolved_path, error);
  if (rc == CAI_OK) {
    rc = cai_read_stream_file(ctx, args, resolved_path, &content, &byte_count,
                              &end_line, &truncated, &file_size, error);
    if (rc == CAI_OK) {
      has_content = 1;
    }
  }
  if (rc == CAI_OK) {
    out->path = cai_strdup(NULL, args->path);
    out->resolved_path = cai_strdup(NULL, resolved_path);
    if (out->path == NULL || out->resolved_path == NULL) {
      rc = cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate read result metadata");
    }
  }
  if (rc == CAI_OK) {
    out->start_line = args->has_start_line ? args->start_line : 1LL;
    out->end_line = end_line;
    out->byte_count = byte_count;
    out->file_size = file_size;
    out->truncated = truncated;
    rc = cai_tool_result_set_spooled(&cai_read_result_map, out, "content",
                                     &content, error);
  }
  if (rc == CAI_OK) {
    lonejson_spooled_cleanup(&content);
    has_content = 0;
  }
  if (has_content) {
    lonejson_spooled_cleanup(&content);
  }
  cai_free_mem(NULL, resolved_path);
  return rc;
}

int cai_tool_registry_register_read_tool(cai_tool_registry *registry,
                                         const cai_read_tool_config *config,
                                         cai_error *error) {
  cai_read_context *ctx;
  const char *name;
  const char *description;
  int rc;

  ctx = NULL;
  rc = cai_read_context_new(config, &ctx, error);
  if (rc != CAI_OK) {
    return rc;
  }
  name = cai_read_default_string(config != NULL ? config->name : NULL,
                                 CAI_READ_DEFAULT_TOOL_NAME);
  description = cai_read_default_string(
      config != NULL ? config->description : NULL, cai_read_default_description);
  rc = cai_tool_registry_register_lonejson_schema_owned(
      registry, name, description, cai_read_schema_json, 0, &cai_read_args_map,
      &cai_read_result_map, cai_read_callback, ctx, cai_read_context_cleanup,
      error);
  if (rc != CAI_OK) {
    cai_read_context_cleanup(ctx);
  }
  return rc;
}

int cai_agent_register_read_tool(cai_agent *agent,
                                 const cai_read_tool_config *config,
                                 cai_error *error) {
  cai_agent_impl *impl;

  if (agent == NULL || agent->impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  impl = CAI_AGENT_IMPL(agent);
  return cai_tool_registry_register_read_tool(impl->tools, config, error);
}
