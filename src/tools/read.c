#include "../cai_internal.h"

#include <cai/tools/read.h>

#include <errno.h>
#include <dirent.h>
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
#define CAI_LIST_FILES_DEFAULT_MAX_ENTRIES 200LL

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

typedef struct cai_read_text_validator {
  unsigned int expected;
  unsigned int codepoint;
  unsigned int min_codepoint;
} cai_read_text_validator;

typedef struct cai_list_files_args {
  char *path;
  int recursive;
  int has_recursive;
  int include_hidden;
  int has_include_hidden;
  long long max_entries;
  int has_max_entries;
} cai_list_files_args;

typedef struct cai_list_files_entry {
  char *path;
  char *resolved_path;
  char *name;
  char *type;
  long long size;
  int has_size;
} cai_list_files_entry;

typedef struct cai_list_files_result {
  char *path;
  char *resolved_path;
  long long entry_count;
  int truncated;
  lonejson_object_array entries;
} cai_list_files_result;

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

static const lonejson_field cai_list_files_arg_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_list_files_args, path, "path"),
    LONEJSON_FIELD_BOOL_PRESENT_NULLABLE(cai_list_files_args, recursive,
                                         has_recursive, "recursive"),
    LONEJSON_FIELD_BOOL_PRESENT_NULLABLE(cai_list_files_args, include_hidden,
                                         has_include_hidden, "include_hidden"),
    LONEJSON_FIELD_I64_PRESENT_NULLABLE(cai_list_files_args, max_entries,
                                        has_max_entries, "max_entries")};
LONEJSON_MAP_DEFINE(cai_list_files_args_map, cai_list_files_args,
                    cai_list_files_arg_fields);

static const lonejson_field cai_list_files_entry_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_list_files_entry, path, "path"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_list_files_entry, resolved_path,
                                    "resolved_path"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_list_files_entry, name, "name"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_list_files_entry, type, "type"),
    LONEJSON_FIELD_I64_PRESENT(cai_list_files_entry, size, has_size, "size")};
LONEJSON_MAP_DEFINE(cai_list_files_entry_map, cai_list_files_entry,
                    cai_list_files_entry_fields);

static const lonejson_field cai_list_files_result_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_list_files_result, path, "path"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_list_files_result, resolved_path,
                                    "resolved_path"),
    LONEJSON_FIELD_I64_REQ(cai_list_files_result, entry_count, "entry_count"),
    LONEJSON_FIELD_BOOL_REQ(cai_list_files_result, truncated, "truncated"),
    LONEJSON_FIELD_OBJECT_ARRAY_OMIT_EMPTY(cai_list_files_result, entries,
                                           "entries", cai_list_files_entry,
                                           &cai_list_files_entry_map,
                                           LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(cai_list_files_result_map, cai_list_files_result,
                    cai_list_files_result_fields);

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

static const char cai_list_files_schema_json[] =
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
    "\"path\":{\"type\":[\"string\",\"null\"]},"
    "\"recursive\":{\"type\":[\"boolean\",\"null\"]},"
    "\"include_hidden\":{\"type\":[\"boolean\",\"null\"]},"
    "\"max_entries\":{\"type\":[\"integer\",\"null\"]}"
    "},"
    "\"required\":[],"
    "\"additionalProperties\":false"
    "}";

static const char cai_list_files_default_description[] =
    "Lists files and directories from the configured sandbox root. Use this "
    "before read_file when you need to discover paths. Paths must stay inside "
    "the configured root; symlink and absolute-path escapes are rejected. "
    "Recursive listing is bounded by max_entries.";

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

static int cai_read_resolve_dir(const cai_read_context *ctx,
                                const char *request_path, char **resolved_path,
                                cai_error *error) {
  char candidate[PATH_MAX];
  char *resolved;
  struct stat st;
  int rc;

  resolved = NULL;
  if (request_path == NULL || request_path[0] == '\0') {
    rc = cai_read_strdup_field(&resolved, ctx->default_workdir,
                               "failed to allocate list path", error);
  } else if (request_path[0] == '/') {
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
                         "list path escapes configured root");
  }
  if (stat(resolved, &st) != 0) {
    cai_free_mem(NULL, resolved);
    return cai_set_error_detail(error, CAI_ERR_INVALID,
                                "failed to stat directory", strerror(errno));
  }
  if (!S_ISDIR(st.st_mode)) {
    cai_free_mem(NULL, resolved);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "list path must be a directory");
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

static void cai_read_text_validator_init(cai_read_text_validator *validator) {
  memset(validator, 0, sizeof(*validator));
}

static int cai_read_text_validator_finish(cai_read_text_validator *validator,
                                          cai_error *error) {
  if (validator->expected != 0U) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "read_file only supports complete UTF-8 text");
  }
  return CAI_OK;
}

static int cai_read_validate_text_chunk(cai_read_text_validator *validator,
                                        const char *data, size_t len,
                                        cai_error *error) {
  size_t i;

  for (i = 0U; i < len; i++) {
    unsigned char b;

    b = (unsigned char)data[i];
    if (b == 0U) {
      return cai_set_error(error, CAI_ERR_INVALID,
                           "read_file only supports text files; NUL byte "
                           "found");
    }
    if (validator->expected == 0U) {
      if (b < 0x80U) {
        continue;
      }
      if (b >= 0xC2U && b <= 0xDFU) {
        validator->expected = 1U;
        validator->codepoint = (unsigned int)(b & 0x1FU);
        validator->min_codepoint = 0x80U;
        continue;
      }
      if (b >= 0xE0U && b <= 0xEFU) {
        validator->expected = 2U;
        validator->codepoint = (unsigned int)(b & 0x0FU);
        validator->min_codepoint = 0x800U;
        continue;
      }
      if (b >= 0xF0U && b <= 0xF4U) {
        validator->expected = 3U;
        validator->codepoint = (unsigned int)(b & 0x07U);
        validator->min_codepoint = 0x10000U;
        continue;
      }
      return cai_set_error(error, CAI_ERR_INVALID,
                           "read_file only supports UTF-8 text files");
    }
    if ((b & 0xC0U) != 0x80U) {
      return cai_set_error(error, CAI_ERR_INVALID,
                           "read_file only supports UTF-8 text files");
    }
    validator->codepoint =
        (validator->codepoint << 6U) | (unsigned int)(b & 0x3FU);
    validator->expected--;
    if (validator->expected == 0U) {
      if (validator->codepoint < validator->min_codepoint ||
          (validator->codepoint >= 0xD800U &&
           validator->codepoint <= 0xDFFFU) ||
          validator->codepoint > 0x10FFFFU) {
        return cai_set_error(error, CAI_ERR_INVALID,
                             "read_file only supports UTF-8 text files");
      }
      validator->codepoint = 0U;
      validator->min_codepoint = 0U;
    }
  }
  return CAI_OK;
}

static int cai_list_array_grow(lonejson_object_array *array, size_t elem_size,
                               cai_error *error) {
  size_t new_capacity;
  void *new_items;

  if (array->count < array->capacity) {
    return CAI_OK;
  }
  new_capacity = array->capacity == 0U ? 16U : array->capacity * 2U;
  new_items = cai_realloc_mem(NULL, array->items, new_capacity * elem_size);
  if (new_items == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to grow list_files entries");
  }
  array->items = new_items;
  array->capacity = new_capacity;
  array->elem_size = elem_size;
  return CAI_OK;
}

static const char *cai_list_type_from_mode(mode_t mode) {
  if (S_ISREG(mode)) {
    return "file";
  }
  if (S_ISDIR(mode)) {
    return "directory";
  }
  if (S_ISLNK(mode)) {
    return "symlink";
  }
  return "other";
}

static const char *cai_list_relative_path(const cai_read_context *ctx,
                                          const char *resolved_path) {
  size_t root_len;

  root_len = strlen(ctx->root_path);
  if (strncmp(ctx->root_path, resolved_path, root_len) != 0) {
    return resolved_path;
  }
  if (resolved_path[root_len] == '\0') {
    return ".";
  }
  if (resolved_path[root_len] == '/') {
    return resolved_path + root_len + 1U;
  }
  return resolved_path;
}

static int cai_list_add_entry(const cai_read_context *ctx,
                              cai_list_files_result *out, const char *name,
                              const char *resolved_path, const struct stat *st,
                              long long max_entries, cai_error *error) {
  cai_list_files_entry *entry;
  int rc;

  if (out->entries.count >= (size_t)max_entries) {
    out->truncated = 1;
    return CAI_OK;
  }
  rc = cai_list_array_grow(&out->entries, sizeof(*entry), error);
  if (rc != CAI_OK) {
    return rc;
  }
  entry = (cai_list_files_entry *)out->entries.items + out->entries.count;
  memset(entry, 0, sizeof(*entry));
  rc = cai_read_strdup_field(&entry->path,
                             cai_list_relative_path(ctx, resolved_path),
                             "failed to allocate list entry path", error);
  if (rc == CAI_OK) {
    rc = cai_read_strdup_field(&entry->resolved_path, resolved_path,
                               "failed to allocate list resolved path", error);
  }
  if (rc == CAI_OK) {
    rc = cai_read_strdup_field(&entry->name, name,
                               "failed to allocate list entry name", error);
  }
  if (rc == CAI_OK) {
    rc = cai_read_strdup_field(&entry->type, cai_list_type_from_mode(st->st_mode),
                               "failed to allocate list entry type", error);
  }
  if (rc == CAI_OK && S_ISREG(st->st_mode)) {
    entry->size = (long long)st->st_size;
    entry->has_size = 1;
  }
  if (rc == CAI_OK) {
    out->entries.count++;
    out->entry_count = (long long)out->entries.count;
  }
  return rc;
}

static int cai_list_scan_dir(const cai_read_context *ctx,
                             cai_list_files_result *out,
                             const char *resolved_dir, int recursive,
                             int include_hidden, long long max_entries,
                             cai_error *error) {
  DIR *dir;
  struct dirent *entry;
  char child_path[PATH_MAX];
  char real_child[PATH_MAX];
  struct stat st;
  int rc;
  int n;

  dir = opendir(resolved_dir);
  if (dir == NULL) {
    return cai_set_error_detail(error, CAI_ERR_INVALID,
                                "failed to open directory", strerror(errno));
  }
  rc = CAI_OK;
  while (rc == CAI_OK && (entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    if (!include_hidden && entry->d_name[0] == '.') {
      continue;
    }
    if (out->entries.count >= (size_t)max_entries) {
      out->truncated = 1;
      break;
    }
    n = snprintf(child_path, sizeof(child_path), "%s/%s", resolved_dir,
                 entry->d_name);
    if (n < 0 || (size_t)n >= sizeof(child_path)) {
      rc = cai_set_error(error, CAI_ERR_INVALID, "list entry path is too long");
      break;
    }
    if (lstat(child_path, &st) != 0) {
      rc = cai_set_error_detail(error, CAI_ERR_INVALID,
                                "failed to stat list entry", strerror(errno));
      break;
    }
    if (S_ISLNK(st.st_mode)) {
      rc = cai_list_add_entry(ctx, out, entry->d_name, child_path, &st,
                              max_entries, error);
      continue;
    }
    if (realpath(child_path, real_child) == NULL) {
      rc = cai_set_error_detail(error, CAI_ERR_INVALID,
                                "failed to resolve list entry",
                                strerror(errno));
      break;
    }
    if (!cai_read_path_is_under_root(ctx->root_path, real_child)) {
      continue;
    }
    rc = cai_list_add_entry(ctx, out, entry->d_name, real_child, &st,
                            max_entries, error);
    if (rc == CAI_OK && recursive && S_ISDIR(st.st_mode) &&
        out->entries.count < (size_t)max_entries) {
      rc = cai_list_scan_dir(ctx, out, real_child, recursive, include_hidden,
                             max_entries, error);
    }
  }
  closedir(dir);
  return rc;
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
  cai_read_text_validator validator;
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
  cai_read_text_validator_init(&validator);
  while (!stop && (nread = fread(buffer, 1U, sizeof(buffer), fp)) > 0U) {
    rc = cai_read_validate_text_chunk(&validator, buffer, nread, error);
    if (rc != CAI_OK) {
      break;
    }
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
  if (rc == CAI_OK) {
    rc = cai_read_text_validator_finish(&validator, error);
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

static int cai_list_files_callback(void *context, const void *params,
                                   void *result, cai_error *error) {
  const cai_read_context *ctx;
  const cai_list_files_args *args;
  cai_list_files_result *out;
  char *resolved_path;
  long long max_entries;
  int recursive;
  int include_hidden;
  int rc;

  ctx = (const cai_read_context *)context;
  args = (const cai_list_files_args *)params;
  out = (cai_list_files_result *)result;
  if (ctx == NULL || args == NULL || out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "list_files callback received invalid state");
  }
  resolved_path = NULL;
  max_entries = CAI_LIST_FILES_DEFAULT_MAX_ENTRIES;
  if (args->has_max_entries && args->max_entries > 0LL) {
    max_entries = args->max_entries;
    if (max_entries > CAI_LIST_FILES_DEFAULT_MAX_ENTRIES) {
      max_entries = CAI_LIST_FILES_DEFAULT_MAX_ENTRIES;
    }
  }
  recursive = args->has_recursive && args->recursive ? 1 : 0;
  include_hidden = args->has_include_hidden && args->include_hidden ? 1 : 0;
  rc = cai_read_resolve_dir(ctx, args->path, &resolved_path, error);
  if (rc == CAI_OK) {
    out->path = cai_strdup(NULL, args->path != NULL ? args->path : ".");
    out->resolved_path = cai_strdup(NULL, resolved_path);
    if (out->path == NULL || out->resolved_path == NULL) {
      rc = cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate list_files result metadata");
    }
  }
  if (rc == CAI_OK) {
    rc = cai_list_scan_dir(ctx, out, resolved_path, recursive, include_hidden,
                           max_entries, error);
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

int cai_tool_registry_register_list_files_tool(
    cai_tool_registry *registry, const cai_read_tool_config *config,
    cai_error *error) {
  cai_read_context *ctx;
  int rc;

  ctx = NULL;
  rc = cai_read_context_new(config, &ctx, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_tool_registry_register_lonejson_schema_owned(
      registry, CAI_LIST_FILES_DEFAULT_TOOL_NAME,
      cai_list_files_default_description,
      cai_list_files_schema_json, 0, &cai_list_files_args_map,
      &cai_list_files_result_map, cai_list_files_callback, ctx,
      cai_read_context_cleanup, error);
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

int cai_agent_register_list_files_tool(cai_agent *agent,
                                       const cai_read_tool_config *config,
                                       cai_error *error) {
  cai_agent_impl *impl;

  if (agent == NULL || agent->impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  impl = CAI_AGENT_IMPL(agent);
  return cai_tool_registry_register_list_files_tool(impl->tools, config, error);
}
