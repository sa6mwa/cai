#include "../cai_internal.h"

#include <cai/tools/todo.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define CAI_TODO_VERSION 1
#define CAI_TODO_STATUS_TODO "todo"
#define CAI_TODO_STATUS_IN_PROCESS "in_process"
#define CAI_TODO_REC_BOARD "board"
#define CAI_TODO_REC_ITEM "item"
#define CAI_TODO_REC_DONE "done"
#define CAI_TODO_FOUND_NONE (-9001)

typedef struct cai_todo_context {
  char *active_path;
  char *done_path;
  char *lock_path;
  char *default_board;
  size_t max_title_bytes;
  size_t max_description_bytes;
  size_t max_result_items;
} cai_todo_context;

typedef struct cai_todo_args {
  char *operation;
  char *board_id;
  char *board_name;
  char *item_id;
  char *title;
  char *description;
  char *status;
  long long wip_limit;
  int has_wip_limit;
} cai_todo_args;

typedef struct cai_todo_result_item {
  char *id;
  char *board_id;
  char *board_name;
  char *title;
  char *status;
} cai_todo_result_item;

typedef struct cai_todo_result {
  int ok;
  int has_ok;
  char *operation;
  char *code;
  char *message;
  char *board_id;
  char *board_name;
  char *item_id;
  char *status;
  long long wip_limit;
  int has_wip_limit;
  long long in_process_count;
  int has_in_process_count;
  long long item_count;
  int has_item_count;
  int truncated;
  int has_truncated;
  lonejson_object_array items;
} cai_todo_result;

typedef struct cai_todo_record {
  char *type;
  long long version;
  int has_version;
  char *id;
  char *name;
  char *board_id;
  char *board_name;
  long long wip_limit;
  int has_wip_limit;
  char *status;
  lonejson_spooled title;
  lonejson_spooled description;
  long long created_at_unix;
  int has_created_at_unix;
  long long updated_at_unix;
  int has_updated_at_unix;
  long long completed_at_unix;
  int has_completed_at_unix;
} cai_todo_record;

typedef struct cai_todo_file_lock {
  int fd;
} cai_todo_file_lock;

typedef struct cai_todo_scan_state {
  const cai_todo_context *ctx;
  const cai_todo_args *args;
  cai_todo_result *result;
  FILE *active_out;
  FILE *done_out;
  const char *new_board_id;
  const char *new_item_id;
  const char *target_board_id;
  int found_board;
  int found_item;
  int found_duplicate_id;
  int active_changed;
  int done_changed;
  long long target_wip_limit;
  int target_has_wip_limit;
  long long target_in_process;
  long long target_items;
  long long now_unix;
} cai_todo_scan_state;

static const lonejson_field cai_todo_arg_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_args, operation, "operation"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_todo_args, board_id, "board_id"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_todo_args, board_name,
                                          "board_name"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_todo_args, item_id, "item_id"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_todo_args, title, "title"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_todo_args, description,
                                          "description"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_todo_args, status, "status"),
    LONEJSON_FIELD_I64_PRESENT(cai_todo_args, wip_limit, has_wip_limit,
                               "wip_limit")};
LONEJSON_MAP_DEFINE(cai_todo_args_map, cai_todo_args, cai_todo_arg_fields);

static const lonejson_field cai_todo_result_item_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_result_item, id, "id"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_result_item, board_id, "board_id"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_result_item, board_name,
                                    "board_name"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_result_item, title, "title"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_result_item, status, "status")};
LONEJSON_MAP_DEFINE(cai_todo_result_item_map, cai_todo_result_item,
                    cai_todo_result_item_fields);

static const lonejson_field cai_todo_result_fields[] = {
    LONEJSON_FIELD_BOOL_PRESENT(cai_todo_result, ok, has_ok, "ok"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_result, operation, "operation"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_result, code, "code"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_result, message, "message"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_todo_result, board_id,
                                          "board_id"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_todo_result, board_name,
                                          "board_name"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_todo_result, item_id, "item_id"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_todo_result, status, "status"),
    LONEJSON_FIELD_I64_PRESENT(cai_todo_result, wip_limit, has_wip_limit,
                               "wip_limit"),
    LONEJSON_FIELD_I64_PRESENT(cai_todo_result, in_process_count,
                               has_in_process_count, "in_process_count"),
    LONEJSON_FIELD_I64_PRESENT(cai_todo_result, item_count, has_item_count,
                               "item_count"),
    LONEJSON_FIELD_BOOL_PRESENT(cai_todo_result, truncated, has_truncated,
                                "truncated"),
    LONEJSON_FIELD_OBJECT_ARRAY_OMIT_EMPTY(cai_todo_result, items, "items",
                                           cai_todo_result_item,
                                           &cai_todo_result_item_map,
                                           LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(cai_todo_result_map, cai_todo_result,
                    cai_todo_result_fields);

static const lonejson_field cai_todo_record_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_record, type, "type"),
    LONEJSON_FIELD_I64_PRESENT(cai_todo_record, version, has_version,
                               "version"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_record, id, "id"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_todo_record, name, "name"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_todo_record, board_id,
                                          "board_id"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_todo_record, board_name,
                                          "board_name"),
    LONEJSON_FIELD_I64_PRESENT(cai_todo_record, wip_limit, has_wip_limit,
                               "wip_limit"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_todo_record, status, "status"),
    LONEJSON_FIELD_STRING_STREAM(cai_todo_record, title, "title"),
    LONEJSON_FIELD_STRING_STREAM(cai_todo_record, description, "description"),
    LONEJSON_FIELD_I64_PRESENT(cai_todo_record, created_at_unix,
                               has_created_at_unix, "created_at_unix"),
    LONEJSON_FIELD_I64_PRESENT(cai_todo_record, updated_at_unix,
                               has_updated_at_unix, "updated_at_unix"),
    LONEJSON_FIELD_I64_PRESENT(cai_todo_record, completed_at_unix,
                               has_completed_at_unix, "completed_at_unix")};
LONEJSON_MAP_DEFINE(cai_todo_record_map, cai_todo_record,
                    cai_todo_record_fields);

static int cai_todo_streq(const char *a, const char *b) {
  return a != NULL && b != NULL && strcmp(a, b) == 0;
}

static const char *cai_todo_arg_string(const char *value,
                                       const char *fallback) {
  return value != NULL && value[0] != '\0' ? value : fallback;
}

static int cai_todo_copy_string(char **out, const char *value,
                                cai_error *error) {
  *out = cai_strdup(NULL, value != NULL ? value : "");
  if (*out == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate todo string");
  }
  return CAI_OK;
}

static void cai_todo_context_cleanup(void *context) {
  cai_todo_context *ctx;

  ctx = (cai_todo_context *)context;
  if (ctx == NULL) {
    return;
  }
  cai_free_mem(NULL, ctx->active_path);
  cai_free_mem(NULL, ctx->done_path);
  cai_free_mem(NULL, ctx->lock_path);
  cai_free_mem(NULL, ctx->default_board);
  cai_free_mem(NULL, ctx);
}

static int cai_todo_path_join(char **out, const char *dir, const char *file,
                              cai_error *error) {
  size_t dir_len;
  size_t file_len;
  size_t need_slash;
  char *path;

  dir_len = strlen(dir);
  file_len = strlen(file);
  need_slash = dir_len > 0U && dir[dir_len - 1U] == '/' ? 0U : 1U;
  path = (char *)cai_alloc(NULL, dir_len + need_slash + file_len + 1U);
  if (path == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate todo path");
  }
  memcpy(path, dir, dir_len);
  if (need_slash) {
    path[dir_len] = '/';
  }
  memcpy(path + dir_len + need_slash, file, file_len);
  path[dir_len + need_slash + file_len] = '\0';
  *out = path;
  return CAI_OK;
}

static int cai_todo_default_dir(char **out, cai_error *error) {
  const char *xdg;
  const char *home;
  char *base;
  int rc;

  xdg = getenv("XDG_CONFIG_HOME");
  if (xdg != NULL && xdg[0] != '\0') {
    return cai_todo_path_join(out, xdg, "cai", error);
  }
  home = getenv("HOME");
  if (home == NULL || home[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "HOME is required for default todo paths");
  }
  base = NULL;
  rc = cai_todo_path_join(&base, home, ".config", error);
  if (rc == CAI_OK) {
    rc = cai_todo_path_join(out, base, "cai", error);
  }
  cai_free_mem(NULL, base);
  return rc;
}

static int cai_todo_default_path(char **out, const char *file,
                                 cai_error *error) {
  char *dir;
  int rc;

  dir = NULL;
  rc = cai_todo_default_dir(&dir, error);
  if (rc == CAI_OK) {
    rc = cai_todo_path_join(out, dir, file, error);
  }
  cai_free_mem(NULL, dir);
  return rc;
}

static int cai_todo_mkdirs_for_file(const char *path, cai_error *error) {
  char tmp[PATH_MAX];
  size_t i;
  size_t len;

  len = strlen(path);
  if (len >= sizeof(tmp)) {
    return cai_set_error(error, CAI_ERR_INVALID, "todo path is too long");
  }
  memcpy(tmp, path, len + 1U);
  for (i = 1U; i < len; ++i) {
    if (tmp[i] == '/') {
      tmp[i] = '\0';
      if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
        return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                    "failed to create todo directory", tmp);
      }
      tmp[i] = '/';
    }
  }
  return CAI_OK;
}

static int cai_todo_ensure_file(const char *path, cai_error *error) {
  int fd;

  if (cai_todo_mkdirs_for_file(path, error) != CAI_OK) {
    return error != NULL ? error->code : CAI_ERR_TRANSPORT;
  }
  fd = open(path, O_CREAT | O_APPEND | O_WRONLY, 0600);
  if (fd < 0) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to create todo store", path);
  }
  close(fd);
  return CAI_OK;
}

static int cai_todo_context_new(const cai_todo_tool_config *config,
                                cai_todo_context **out, cai_error *error) {
  cai_todo_context *ctx;
  int rc;

  *out = NULL;
  ctx = (cai_todo_context *)cai_alloc(NULL, sizeof(*ctx));
  if (ctx == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate todo tool context");
  }
  memset(ctx, 0, sizeof(*ctx));
  if (config != NULL && config->active_path != NULL &&
      config->active_path[0] != '\0') {
    rc = cai_todo_copy_string(&ctx->active_path, config->active_path, error);
  } else {
    rc = cai_todo_default_path(&ctx->active_path, CAI_TODO_DEFAULT_ACTIVE_FILE,
                               error);
  }
  if (rc == CAI_OK) {
    if (config != NULL && config->done_path != NULL &&
        config->done_path[0] != '\0') {
      rc = cai_todo_copy_string(&ctx->done_path, config->done_path, error);
    } else {
      rc = cai_todo_default_path(&ctx->done_path, CAI_TODO_DEFAULT_DONE_FILE,
                                 error);
    }
  }
  if (rc == CAI_OK) {
    if (config != NULL && config->lock_path != NULL &&
        config->lock_path[0] != '\0') {
      rc = cai_todo_copy_string(&ctx->lock_path, config->lock_path, error);
    } else {
      rc = cai_todo_default_path(&ctx->lock_path, CAI_TODO_DEFAULT_LOCK_FILE,
                                 error);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(
        &ctx->default_board,
        cai_todo_arg_string(config != NULL ? config->default_board : NULL,
                            "default"),
        error);
  }
  if (rc != CAI_OK) {
    cai_todo_context_cleanup(ctx);
    return rc;
  }
  ctx->max_title_bytes =
      config != NULL && config->max_title_bytes != 0U
          ? config->max_title_bytes
          : 4096U;
  ctx->max_description_bytes =
      config != NULL && config->max_description_bytes != 0U
          ? config->max_description_bytes
          : 128U * 1024U;
  ctx->max_result_items =
      config != NULL && config->max_result_items != 0U
          ? config->max_result_items
          : 100U;
  rc = cai_todo_ensure_file(ctx->active_path, error);
  if (rc == CAI_OK) {
    rc = cai_todo_ensure_file(ctx->done_path, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_ensure_file(ctx->lock_path, error);
  }
  if (rc != CAI_OK) {
    cai_todo_context_cleanup(ctx);
    return rc;
  }
  *out = ctx;
  return CAI_OK;
}

static int cai_todo_lock(cai_todo_context *ctx, cai_todo_file_lock *lock,
                         cai_error *error) {
  struct flock fl;

  lock->fd = -1;
  if (cai_todo_ensure_file(ctx->lock_path, error) != CAI_OK) {
    return error != NULL ? error->code : CAI_ERR_TRANSPORT;
  }
  lock->fd = open(ctx->lock_path, O_RDWR, 0600);
  if (lock->fd < 0) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to open todo lockfile",
                                ctx->lock_path);
  }
  memset(&fl, 0, sizeof(fl));
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  if (fcntl(lock->fd, F_SETLKW, &fl) != 0) {
    close(lock->fd);
    lock->fd = -1;
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to lock todo store", ctx->lock_path);
  }
  return CAI_OK;
}

static void cai_todo_unlock(cai_todo_file_lock *lock) {
  struct flock fl;

  if (lock == NULL || lock->fd < 0) {
    return;
  }
  memset(&fl, 0, sizeof(fl));
  fl.l_type = F_UNLCK;
  fl.l_whence = SEEK_SET;
  (void)fcntl(lock->fd, F_SETLK, &fl);
  close(lock->fd);
  lock->fd = -1;
}

static int cai_todo_tmp_path(char **out, const char *path, cai_error *error) {
  char suffix[64];
  size_t len;
  size_t suffix_len;
  char *tmp;

  snprintf(suffix, sizeof(suffix), ".tmp.%ld.%ld", (long)getpid(),
           (long)time(NULL));
  len = strlen(path);
  suffix_len = strlen(suffix);
  tmp = (char *)cai_alloc(NULL, len + suffix_len + 1U);
  if (tmp == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate temp path");
  }
  memcpy(tmp, path, len);
  memcpy(tmp + len, suffix, suffix_len + 1U);
  *out = tmp;
  return CAI_OK;
}

static lonejson_status cai_todo_file_sink(void *user, const void *data,
                                          size_t len, lonejson_error *error) {
  FILE *fp;

  (void)error;
  fp = (FILE *)user;
  if (len != 0U && fwrite(data, 1U, len, fp) != len) {
    return LONEJSON_STATUS_IO_ERROR;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_todo_spool_sink(void *user, const void *data,
                                           size_t len, lonejson_error *error) {
  return lonejson_spooled_append((lonejson_spooled *)user, data, len, error);
}

static int cai_todo_write_record(FILE *fp, const cai_todo_record *record,
                                 cai_error *error) {
  lonejson_error json_error;

  lonejson_error_init(&json_error);
  if (lonejson_serialize_sink(&cai_todo_record_map, record,
                              cai_todo_file_sink, fp, NULL, &json_error) !=
      LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to serialize todo record",
                                json_error.message);
  }
  if (fputc('\n', fp) == EOF) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to write todo record");
  }
  return CAI_OK;
}

static int cai_todo_spool_string(lonejson_spooled *spool, const char *value,
                                 cai_error *error) {
  lonejson_error json_error;

  lonejson_error_init(&json_error);
  lonejson_spooled_cleanup(spool);
  lonejson_spooled_init(spool, NULL);
  if (value != NULL &&
      lonejson_spooled_append(spool, value, strlen(value), &json_error) !=
          LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to spool todo string",
                                json_error.message);
  }
  return CAI_OK;
}

static int cai_todo_spool_clone(lonejson_spooled *dst,
                                const lonejson_spooled *src,
                                cai_error *error) {
  lonejson_error json_error;

  lonejson_spooled_cleanup(dst);
  lonejson_spooled_init(dst, NULL);
  lonejson_error_init(&json_error);
  if (lonejson_spooled_write_to_sink(src, cai_todo_spool_sink, dst,
                                     &json_error) != LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to clone todo spooled field",
                                json_error.message);
  }
  return CAI_OK;
}

static int cai_todo_spool_to_string(const lonejson_spooled *spool,
                                    size_t max_bytes, char **out,
                                    cai_error *error) {
  lonejson_spooled cursor;
  lonejson_error json_error;
  lonejson_read_result chunk;
  char *data;
  size_t size;
  size_t offset;

  size = lonejson_spooled_size(spool);
  if (size > max_bytes) {
    size = max_bytes;
  }
  data = (char *)cai_alloc(NULL, size + 1U);
  if (data == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate todo result text");
  }
  cursor = *spool;
  lonejson_error_init(&json_error);
  if (lonejson_spooled_rewind(&cursor, &json_error) != LONEJSON_STATUS_OK) {
    cai_free_mem(NULL, data);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to rewind todo string",
                                json_error.message);
  }
  offset = 0U;
  while (offset < size) {
    chunk = lonejson_spooled_read(&cursor, (unsigned char *)data + offset,
                                  size - offset);
    if (chunk.error_code != 0) {
      cai_free_mem(NULL, data);
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "failed to read todo string");
    }
    if (chunk.bytes_read == 0U) {
      break;
    }
    offset += chunk.bytes_read;
  }
  data[offset] = '\0';
  *out = data;
  return CAI_OK;
}

static int cai_todo_set_result(cai_todo_result *result, const char *operation,
                               int ok, const char *code, const char *message,
                               cai_error *error) {
  int rc;

  result->ok = ok;
  result->has_ok = 1;
  cai_free_mem(NULL, result->operation);
  cai_free_mem(NULL, result->code);
  cai_free_mem(NULL, result->message);
  result->operation = NULL;
  result->code = NULL;
  result->message = NULL;
  rc = cai_todo_copy_string(&result->operation, operation, error);
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&result->code, code, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&result->message, message, error);
  }
  return rc;
}

static int cai_todo_array_grow(lonejson_object_array *array, size_t elem_size,
                               cai_error *error) {
  size_t new_capacity;
  void *new_items;

  if (array->count < array->capacity) {
    return CAI_OK;
  }
  new_capacity = array->capacity == 0U ? 4U : array->capacity * 2U;
  new_items = cai_realloc_mem(NULL, array->items, new_capacity * elem_size);
  if (new_items == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to grow todo result items");
  }
  array->items = new_items;
  array->capacity = new_capacity;
  array->elem_size = elem_size;
  return CAI_OK;
}

static int cai_todo_add_result_item(cai_todo_result *result,
                                    const cai_todo_record *record,
                                    const char *board_name,
                                    size_t max_items, cai_error *error) {
  cai_todo_result_item *item;
  int rc;

  if (result->items.count >= max_items) {
    result->truncated = 1;
    result->has_truncated = 1;
    return CAI_OK;
  }
  rc = cai_todo_array_grow(&result->items, sizeof(*item), error);
  if (rc != CAI_OK) {
    return rc;
  }
  item = (cai_todo_result_item *)result->items.items + result->items.count;
  memset(item, 0, sizeof(*item));
  rc = cai_todo_copy_string(&item->id, record->id, error);
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&item->board_id, record->board_id, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&item->board_name,
                              board_name != NULL ? board_name : "", error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&item->status,
                              record->status != NULL ? record->status : "",
                              error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_spool_to_string(&record->title, 4096U, &item->title, error);
  }
  if (rc == CAI_OK) {
    result->items.count++;
  }
  return rc;
}

static int cai_todo_add_result_board(cai_todo_result *result,
                                     const cai_todo_record *record,
                                     size_t max_items, cai_error *error) {
  cai_todo_result_item *item;
  int rc;

  if (result->items.count >= max_items) {
    result->truncated = 1;
    result->has_truncated = 1;
    return CAI_OK;
  }
  rc = cai_todo_array_grow(&result->items, sizeof(*item), error);
  if (rc != CAI_OK) {
    return rc;
  }
  item = (cai_todo_result_item *)result->items.items + result->items.count;
  memset(item, 0, sizeof(*item));
  rc = cai_todo_copy_string(&item->id, record->id, error);
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&item->board_id, record->id, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&item->board_name,
                              record->name != NULL ? record->name : "",
                              error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&item->title,
                              record->name != NULL ? record->name : "",
                              error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&item->status, "board", error);
  }
  if (rc == CAI_OK) {
    result->items.count++;
  }
  return rc;
}

static void cai_todo_record_init(cai_todo_record *record) {
  memset(record, 0, sizeof(*record));
  lonejson_spooled_init(&record->title, NULL);
  lonejson_spooled_init(&record->description, NULL);
  lonejson_init(&cai_todo_record_map, record);
}

static void cai_todo_record_cleanup(cai_todo_record *record) {
  if (record == NULL) {
    return;
  }
  cai_free_mem(NULL, record->type);
  cai_free_mem(NULL, record->id);
  cai_free_mem(NULL, record->name);
  cai_free_mem(NULL, record->board_id);
  cai_free_mem(NULL, record->board_name);
  cai_free_mem(NULL, record->status);
  lonejson_spooled_cleanup(&record->title);
  lonejson_spooled_cleanup(&record->description);
  memset(record, 0, sizeof(*record));
}

static void cai_todo_record_parse_init(cai_todo_record *record) {
  memset(record, 0, sizeof(*record));
}

static void cai_todo_record_parse_cleanup(cai_todo_record *record) {
  lonejson_cleanup(&cai_todo_record_map, record);
}

static int cai_todo_open_stream(const char *path, FILE **fp_out,
                                lonejson_stream **stream_out,
                                cai_error *error) {
  FILE *fp;
  lonejson_error json_error;

  *fp_out = NULL;
  *stream_out = NULL;
  if (cai_todo_ensure_file(path, error) != CAI_OK) {
    return error != NULL ? error->code : CAI_ERR_TRANSPORT;
  }
  fp = fopen(path, "rb");
  if (fp == NULL) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to open todo store", path);
  }
  lonejson_error_init(&json_error);
  *stream_out =
      lonejson_stream_open_filep(&cai_todo_record_map, fp, NULL, &json_error);
  if (*stream_out == NULL) {
    fclose(fp);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to open todo record stream",
                                json_error.message);
  }
  *fp_out = fp;
  return CAI_OK;
}

static int cai_todo_stream_next(lonejson_stream *stream,
                                cai_todo_record *record, int *has_record,
                                cai_error *error) {
  lonejson_error json_error;
  lonejson_stream_result result;
  const lonejson_error *stream_error;

  lonejson_error_init(&json_error);
  result = lonejson_stream_next(stream, record, &json_error);
  if (result == LONEJSON_STREAM_OBJECT) {
    *has_record = 1;
    return CAI_OK;
  }
  if (result == LONEJSON_STREAM_EOF) {
    *has_record = 0;
    return CAI_OK;
  }
  stream_error = lonejson_stream_error(stream);
  return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                              "failed to parse todo store",
                              stream_error != NULL ? stream_error->message
                                                   : json_error.message);
}

static int cai_todo_record_is_board(const cai_todo_record *record) {
  return cai_todo_streq(record->type, CAI_TODO_REC_BOARD);
}

static int cai_todo_record_is_item(const cai_todo_record *record) {
  return cai_todo_streq(record->type, CAI_TODO_REC_ITEM);
}

static int cai_todo_board_matches(const cai_todo_record *record,
                                  const char *board_id,
                                  const char *board_name) {
  if (!cai_todo_record_is_board(record)) {
    return 0;
  }
  if (board_id != NULL && board_id[0] != '\0') {
    return cai_todo_streq(record->id, board_id);
  }
  if (board_name != NULL && board_name[0] != '\0') {
    return cai_todo_streq(record->name, board_name);
  }
  return 0;
}

static int cai_todo_id_exists(const cai_todo_context *ctx, const char *id,
                              cai_error *error) {
  const char *paths[2];
  FILE *fp;
  lonejson_stream *stream;
  cai_todo_record record;
  int has_record;
  int exists;
  size_t i;
  int rc;

  paths[0] = ctx->active_path;
  paths[1] = ctx->done_path;
  exists = 0;
  for (i = 0U; i < 2U && !exists; ++i) {
    rc = cai_todo_open_stream(paths[i], &fp, &stream, error);
    if (rc != CAI_OK) {
      return rc;
    }
    for (;;) {
      cai_todo_record_parse_init(&record);
      rc = cai_todo_stream_next(stream, &record, &has_record, error);
      if (rc != CAI_OK || !has_record) {
        cai_todo_record_parse_cleanup(&record);
        break;
      }
      if (cai_todo_streq(record.id, id)) {
        exists = 1;
      }
      cai_todo_record_parse_cleanup(&record);
      if (exists) {
        break;
      }
    }
    lonejson_stream_close(stream);
    fclose(fp);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  return exists;
}

static void cai_todo_base36(unsigned long long value, char *out,
                            size_t out_len) {
  static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  char tmp[32];
  size_t count;
  size_t i;

  count = 0U;
  if (value == 0ULL) {
    tmp[count++] = '0';
  } else {
    while (value != 0ULL && count < sizeof(tmp)) {
      tmp[count++] = alphabet[value % 36ULL];
      value /= 36ULL;
    }
  }
  i = 0U;
  while (i + 1U < out_len && count > 0U) {
    out[i++] = tmp[--count];
  }
  out[i] = '\0';
}

static int cai_todo_generate_id(const cai_todo_context *ctx, char out[32],
                                cai_error *error) {
  struct timeval tv;
  unsigned long long random_part;
  unsigned long long value;
  unsigned int counter;
  int fd;
  int tries;

  random_part = 0ULL;
  fd = open("/dev/urandom", O_RDONLY);
  if (fd >= 0) {
    ssize_t bytes_read;

    bytes_read = read(fd, &random_part, sizeof(random_part));
    if (bytes_read < 0) {
      random_part = 0ULL;
    }
    close(fd);
  }
  for (tries = 0; tries < 16; ++tries) {
    static unsigned int local_counter;
    counter = ++local_counter;
    gettimeofday(&tv, NULL);
    value = ((unsigned long long)tv.tv_sec * 1000000ULL +
             (unsigned long long)tv.tv_usec) ^
            ((unsigned long long)getpid() << 16) ^
            ((unsigned long long)counter << 8) ^ random_part;
    cai_todo_base36(value, out, 32U);
    if (!cai_todo_id_exists(ctx, out, error)) {
      return CAI_OK;
    }
    random_part += 0x9e3779b97f4a7c15ULL;
  }
  return cai_set_error(error, CAI_ERR_INVALID,
                       "failed to generate unique todo id");
}

static int cai_todo_record_set_board(cai_todo_record *record, const char *id,
                                     const char *name, long long wip_limit,
                                     int has_wip_limit, cai_error *error) {
  int rc;

  cai_todo_record_init(record);
  rc = cai_todo_copy_string(&record->type, CAI_TODO_REC_BOARD, error);
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&record->id, id, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&record->name, name, error);
  }
  record->version = CAI_TODO_VERSION;
  record->has_version = 1;
  record->wip_limit = has_wip_limit ? wip_limit : -1;
  record->has_wip_limit = 1;
  return rc;
}

static int cai_todo_record_set_item(cai_todo_record *record, const char *id,
                                    const char *board_id,
                                    const char *board_name, const char *title,
                                    const char *description,
                                    const char *status, long long now_unix,
                                    cai_error *error) {
  int rc;

  cai_todo_record_init(record);
  rc = cai_todo_copy_string(&record->type, CAI_TODO_REC_ITEM, error);
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&record->id, id, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&record->board_id, board_id, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&record->board_name, board_name, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&record->status, status, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_spool_string(&record->title, title, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_spool_string(&record->description,
                               description != NULL ? description : "", error);
  }
  record->version = CAI_TODO_VERSION;
  record->has_version = 1;
  record->created_at_unix = now_unix;
  record->has_created_at_unix = 1;
  record->updated_at_unix = now_unix;
  record->has_updated_at_unix = 1;
  return rc;
}

static int cai_todo_make_done_from_item(cai_todo_record *done,
                                        const cai_todo_record *item,
                                        long long now_unix,
                                        cai_error *error) {
  int rc;

  cai_todo_record_init(done);
  rc = cai_todo_copy_string(&done->type, CAI_TODO_REC_DONE, error);
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&done->id, item->id, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&done->board_id, item->board_id, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&done->board_name,
                              item->board_name != NULL ? item->board_name : "",
                              error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_spool_clone(&done->title, &item->title, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_spool_clone(&done->description, &item->description, error);
  }
  done->version = CAI_TODO_VERSION;
  done->has_version = 1;
  done->created_at_unix = item->created_at_unix;
  done->has_created_at_unix = item->has_created_at_unix;
  done->updated_at_unix = now_unix;
  done->has_updated_at_unix = 1;
  done->completed_at_unix = now_unix;
  done->has_completed_at_unix = 1;
  return rc;
}

static int cai_todo_copy_store_with_append(const char *src_path,
                                           const char *dst_path,
                                           const cai_todo_record *append,
                                           cai_error *error) {
  FILE *src;
  FILE *dst;
  lonejson_stream *stream;
  cai_todo_record record;
  int has_record;
  int rc;

  rc = cai_todo_open_stream(src_path, &src, &stream, error);
  if (rc != CAI_OK) {
    return rc;
  }
  dst = fopen(dst_path, "wb");
  if (dst == NULL) {
    lonejson_stream_close(stream);
    fclose(src);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to write todo temp store", dst_path);
  }
  for (;;) {
    cai_todo_record_parse_init(&record);
    rc = cai_todo_stream_next(stream, &record, &has_record, error);
    if (rc != CAI_OK || !has_record) {
      cai_todo_record_parse_cleanup(&record);
      break;
    }
    rc = cai_todo_write_record(dst, &record, error);
    cai_todo_record_parse_cleanup(&record);
    if (rc != CAI_OK) {
      break;
    }
  }
  if (rc == CAI_OK && append != NULL) {
    rc = cai_todo_write_record(dst, append, error);
  }
  lonejson_stream_close(stream);
  fclose(src);
  if (fflush(dst) != 0) {
    rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                       "failed to flush todo temp store");
  }
  if (fclose(dst) != 0 && rc == CAI_OK) {
    rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                       "failed to close todo temp store");
  }
  return rc;
}

static int cai_todo_commit_temp(const char *tmp_path, const char *target_path,
                                cai_error *error) {
  if (rename(tmp_path, target_path) != 0) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to commit todo temp store",
                                target_path);
  }
  return CAI_OK;
}

static int cai_todo_create_board(cai_todo_context *ctx,
                                 const cai_todo_args *args,
                                 cai_todo_result *result, cai_error *error) {
  cai_todo_file_lock lock;
  cai_todo_record board;
  char id[32];
  char *tmp;
  const char *name;
  int rc;

  lock.fd = -1;
  tmp = NULL;
  rc = cai_todo_lock(ctx, &lock, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_todo_generate_id(ctx, id, error);
  name = cai_todo_arg_string(args->board_name, ctx->default_board);
  if (rc == CAI_OK) {
    rc = cai_todo_record_set_board(&board, id, name, args->wip_limit,
                                   args->has_wip_limit, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_tmp_path(&tmp, ctx->active_path, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_store_with_append(ctx->active_path, tmp, &board, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_commit_temp(tmp, ctx->active_path, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_set_result(result, args->operation, 1, "ok", "board created",
                             error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&result->board_id, id, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&result->board_name, name, error);
  }
  if (rc == CAI_OK && args->has_wip_limit) {
    result->wip_limit = args->wip_limit;
    result->has_wip_limit = 1;
  }
  if (tmp != NULL && rc != CAI_OK) {
    unlink(tmp);
  }
  cai_free_mem(NULL, tmp);
  cai_todo_record_cleanup(&board);
  cai_todo_unlock(&lock);
  return rc;
}

static int cai_todo_find_board(cai_todo_context *ctx, const char *board_id,
                               const char *board_name, char **out_id,
                               char **out_name, long long *wip_limit,
                               int *has_wip_limit, long long *in_process,
                               long long *items, cai_error *error) {
  FILE *fp;
  lonejson_stream *stream;
  cai_todo_record record;
  int has_record;
  int found;
  int rc;

  *out_id = NULL;
  *out_name = NULL;
  *has_wip_limit = 0;
  *in_process = 0;
  *items = 0;
  found = 0;
  rc = cai_todo_open_stream(ctx->active_path, &fp, &stream, error);
  if (rc != CAI_OK) {
    return rc;
  }
  for (;;) {
    cai_todo_record_parse_init(&record);
    rc = cai_todo_stream_next(stream, &record, &has_record, error);
    if (rc != CAI_OK || !has_record) {
      cai_todo_record_parse_cleanup(&record);
      break;
    }
    if (!found && cai_todo_board_matches(&record, board_id, board_name)) {
      found = 1;
      rc = cai_todo_copy_string(out_id, record.id, error);
      if (rc == CAI_OK) {
        rc = cai_todo_copy_string(out_name, record.name, error);
      }
      *wip_limit = record.wip_limit;
      *has_wip_limit = record.has_wip_limit && record.wip_limit >= 0;
    } else if (found && cai_todo_record_is_item(&record) &&
               cai_todo_streq(record.board_id, *out_id)) {
      (*items)++;
      if (cai_todo_streq(record.status, CAI_TODO_STATUS_IN_PROCESS)) {
        (*in_process)++;
      }
    }
    cai_todo_record_parse_cleanup(&record);
    if (rc != CAI_OK) {
      break;
    }
  }
  lonejson_stream_close(stream);
  fclose(fp);
  return rc == CAI_OK ? (found ? CAI_OK : CAI_TODO_FOUND_NONE) : rc;
}

static int cai_todo_add_item(cai_todo_context *ctx, const cai_todo_args *args,
                             cai_todo_result *result, cai_error *error) {
  cai_todo_file_lock lock;
  cai_todo_record item;
  char id[32];
  char *board_id;
  char *board_name;
  char *tmp;
  long long wip_limit;
  long long in_process;
  long long items;
  int has_wip_limit;
  const char *status;
  int rc;

  if (args->title == NULL || args->title[0] == '\0') {
    return cai_todo_set_result(result, args->operation, 0, "invalid_request",
                               "title is required", error);
  }
  if (strlen(args->title) > ctx->max_title_bytes) {
    return cai_todo_set_result(result, args->operation, 0, "title_too_large",
                               "title exceeds configured limit", error);
  }
  if (args->description != NULL &&
      strlen(args->description) > ctx->max_description_bytes) {
    return cai_todo_set_result(result, args->operation, 0,
                               "description_too_large",
                               "description exceeds configured limit", error);
  }
  lock.fd = -1;
  board_id = NULL;
  board_name = NULL;
  tmp = NULL;
  rc = cai_todo_lock(ctx, &lock, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_todo_find_board(ctx, args->board_id,
                           cai_todo_arg_string(args->board_name,
                                               ctx->default_board),
                           &board_id, &board_name, &wip_limit, &has_wip_limit,
                           &in_process, &items, error);
  if (rc == CAI_TODO_FOUND_NONE) {
    rc = cai_todo_set_result(result, args->operation, 0, "board_not_found",
                             "board was not found", error);
  }
  status = cai_todo_arg_string(args->status, CAI_TODO_STATUS_TODO);
  if (rc == CAI_OK && !cai_todo_streq(status, CAI_TODO_STATUS_TODO) &&
      !cai_todo_streq(status, CAI_TODO_STATUS_IN_PROCESS)) {
    rc = cai_todo_set_result(result, args->operation, 0, "invalid_status",
                             "status must be todo or in_process", error);
  }
  if (rc == CAI_OK && cai_todo_streq(status, CAI_TODO_STATUS_IN_PROCESS) &&
      has_wip_limit && in_process >= wip_limit) {
    rc = cai_todo_set_result(result, args->operation, 0, "wip_limit_exceeded",
                             "board WIP limit would be exceeded", error);
  }
  if (rc == CAI_OK && !(result->has_ok && !result->ok)) {
    rc = cai_todo_generate_id(ctx, id, error);
  }
  if (rc == CAI_OK && !(result->has_ok && !result->ok)) {
    rc = cai_todo_record_set_item(&item, id, board_id, board_name, args->title,
                                  args->description, status,
                                  (long long)time(NULL), error);
  }
  if (rc == CAI_OK && !(result->has_ok && !result->ok)) {
    rc = cai_todo_tmp_path(&tmp, ctx->active_path, error);
  }
  if (rc == CAI_OK && !(result->has_ok && !result->ok)) {
    rc = cai_todo_copy_store_with_append(ctx->active_path, tmp, &item, error);
  }
  if (rc == CAI_OK && !(result->has_ok && !result->ok)) {
    rc = cai_todo_commit_temp(tmp, ctx->active_path, error);
  }
  if (rc == CAI_OK && !(result->has_ok && !result->ok)) {
    rc = cai_todo_set_result(result, args->operation, 1, "ok", "item added",
                             error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&result->board_id, board_id, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&result->board_name, board_name, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&result->item_id, id, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&result->status, status, error);
  }
  result->in_process_count =
      in_process + (cai_todo_streq(status, CAI_TODO_STATUS_IN_PROCESS) ? 1 : 0);
  result->has_in_process_count = 1;
  result->item_count = items + 1;
  result->has_item_count = 1;
  if (tmp != NULL && rc != CAI_OK) {
    unlink(tmp);
  }
  cai_free_mem(NULL, tmp);
  cai_free_mem(NULL, board_id);
  cai_free_mem(NULL, board_name);
  if (rc != CAI_OK || result->ok) {
    cai_todo_record_cleanup(&item);
  }
  cai_todo_unlock(&lock);
  return rc;
}

static int cai_todo_list(cai_todo_context *ctx, const cai_todo_args *args,
                         cai_todo_result *result, int only_current,
                         cai_error *error) {
  FILE *fp;
  lonejson_stream *stream;
  cai_todo_record record;
  int has_record;
  char *target_id;
  char *target_name;
  long long wip_limit;
  long long in_process;
  long long items;
  int has_wip_limit;
  int rc;

  target_id = NULL;
  target_name = NULL;
  rc = cai_todo_set_result(result, args->operation, 1, "ok", "items listed",
                           error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_todo_find_board(ctx, args->board_id,
                           cai_todo_arg_string(args->board_name,
                                               ctx->default_board),
                           &target_id, &target_name, &wip_limit,
                           &has_wip_limit, &in_process, &items, error);
  if (rc == CAI_TODO_FOUND_NONE) {
    cai_free_mem(NULL, target_id);
    cai_free_mem(NULL, target_name);
    return cai_todo_set_result(result, args->operation, 0, "board_not_found",
                               "board was not found", error);
  }
  if (rc != CAI_OK) {
    cai_free_mem(NULL, target_id);
    cai_free_mem(NULL, target_name);
    return rc;
  }
  rc = cai_todo_copy_string(&result->board_id, target_id, error);
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&result->board_name, target_name, error);
  }
  result->wip_limit = wip_limit;
  result->has_wip_limit = has_wip_limit;
  result->in_process_count = in_process;
  result->has_in_process_count = 1;
  result->item_count = items;
  result->has_item_count = 1;
  if (rc == CAI_OK) {
    rc = cai_todo_open_stream(ctx->active_path, &fp, &stream, error);
  }
  if (rc != CAI_OK) {
    cai_free_mem(NULL, target_id);
    cai_free_mem(NULL, target_name);
    return rc;
  }
  for (;;) {
    cai_todo_record_parse_init(&record);
    rc = cai_todo_stream_next(stream, &record, &has_record, error);
    if (rc != CAI_OK || !has_record) {
      cai_todo_record_parse_cleanup(&record);
      break;
    }
    if (cai_todo_record_is_item(&record) &&
        cai_todo_streq(record.board_id, target_id) &&
        (!only_current ||
         cai_todo_streq(record.status, CAI_TODO_STATUS_IN_PROCESS))) {
      rc = cai_todo_add_result_item(result, &record, target_name,
                                    ctx->max_result_items, error);
    }
    cai_todo_record_parse_cleanup(&record);
    if (rc != CAI_OK) {
      break;
    }
  }
  lonejson_stream_close(stream);
  fclose(fp);
  cai_free_mem(NULL, target_id);
  cai_free_mem(NULL, target_name);
  return rc;
}

static int cai_todo_list_boards(cai_todo_context *ctx,
                                const cai_todo_args *args,
                                cai_todo_result *result, cai_error *error) {
  FILE *fp;
  lonejson_stream *stream;
  cai_todo_record record;
  int has_record;
  long long boards;
  int rc;

  boards = 0;
  rc = cai_todo_set_result(result, args->operation, 1, "ok", "boards listed",
                           error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_todo_open_stream(ctx->active_path, &fp, &stream, error);
  if (rc != CAI_OK) {
    return rc;
  }
  for (;;) {
    cai_todo_record_parse_init(&record);
    rc = cai_todo_stream_next(stream, &record, &has_record, error);
    if (rc != CAI_OK || !has_record) {
      cai_todo_record_parse_cleanup(&record);
      break;
    }
    if (cai_todo_record_is_board(&record)) {
      boards++;
      rc = cai_todo_add_result_board(result, &record, ctx->max_result_items,
                                     error);
    }
    cai_todo_record_parse_cleanup(&record);
    if (rc != CAI_OK) {
      break;
    }
  }
  lonejson_stream_close(stream);
  fclose(fp);
  result->item_count = boards;
  result->has_item_count = 1;
  return rc;
}

static int cai_todo_rewrite_move(cai_todo_context *ctx,
                                 const cai_todo_args *args,
                                 cai_todo_result *result, int complete,
                                 cai_error *error) {
  cai_todo_file_lock lock;
  FILE *src;
  FILE *active_dst;
  FILE *done_dst;
  lonejson_stream *stream;
  cai_todo_record record;
  cai_todo_record done;
  int has_record;
  char *active_tmp;
  char *done_tmp;
  char *target_board_id;
  char *target_board_name;
  long long wip_limit;
  long long in_process;
  long long items;
  int has_wip_limit;
  const char *new_status;
  int found_item;
  int rc;

  if (args->item_id == NULL || args->item_id[0] == '\0') {
    return cai_todo_set_result(result, args->operation, 0, "invalid_request",
                               "item_id is required", error);
  }
  new_status = cai_todo_arg_string(args->status, CAI_TODO_STATUS_TODO);
  if (!complete && !cai_todo_streq(new_status, CAI_TODO_STATUS_TODO) &&
      !cai_todo_streq(new_status, CAI_TODO_STATUS_IN_PROCESS)) {
    return cai_todo_set_result(result, args->operation, 0, "invalid_status",
                               "status must be todo or in_process", error);
  }
  lock.fd = -1;
  src = NULL;
  active_dst = NULL;
  done_dst = NULL;
  stream = NULL;
  active_tmp = NULL;
  done_tmp = NULL;
  target_board_id = NULL;
  target_board_name = NULL;
  found_item = 0;
  rc = cai_todo_lock(ctx, &lock, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_todo_find_board(ctx, args->board_id,
                           cai_todo_arg_string(args->board_name,
                                               ctx->default_board),
                           &target_board_id, &target_board_name, &wip_limit,
                           &has_wip_limit, &in_process, &items, error);
  if (rc == CAI_TODO_FOUND_NONE) {
    rc = CAI_OK;
  }
  if (rc == CAI_OK && !complete &&
      cai_todo_streq(new_status, CAI_TODO_STATUS_IN_PROCESS) && has_wip_limit &&
      in_process >= wip_limit) {
    rc = cai_todo_set_result(result, args->operation, 0, "wip_limit_exceeded",
                             "board WIP limit would be exceeded", error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_tmp_path(&active_tmp, ctx->active_path, error);
  }
  if (rc == CAI_OK && complete) {
    rc = cai_todo_tmp_path(&done_tmp, ctx->done_path, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_open_stream(ctx->active_path, &src, &stream, error);
  }
  if (rc == CAI_OK) {
    active_dst = fopen(active_tmp, "wb");
    if (active_dst == NULL) {
      rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to open active todo temp store",
                                active_tmp);
    }
  }
  if (rc == CAI_OK && complete) {
    rc = cai_todo_copy_store_with_append(ctx->done_path, done_tmp, NULL, error);
    if (rc == CAI_OK) {
      done_dst = fopen(done_tmp, "ab");
      if (done_dst == NULL) {
        rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                  "failed to open done todo temp store",
                                  done_tmp);
      }
    }
  }
  while (rc == CAI_OK) {
    cai_todo_record_parse_init(&record);
    rc = cai_todo_stream_next(stream, &record, &has_record, error);
    if (rc != CAI_OK || !has_record) {
      cai_todo_record_parse_cleanup(&record);
      break;
    }
    if (cai_todo_record_is_item(&record) &&
        cai_todo_streq(record.id, args->item_id)) {
      found_item = 1;
      if (complete) {
        rc = cai_todo_make_done_from_item(&done, &record, (long long)time(NULL),
                                          error);
        if (rc == CAI_OK) {
          rc = cai_todo_write_record(done_dst, &done, error);
        }
        cai_todo_record_cleanup(&done);
      } else {
        cai_free_mem(NULL, record.status);
        record.status = NULL;
        rc = cai_todo_copy_string(&record.status, new_status, error);
        record.updated_at_unix = (long long)time(NULL);
        record.has_updated_at_unix = 1;
        if (rc == CAI_OK) {
          rc = cai_todo_write_record(active_dst, &record, error);
        }
      }
    } else {
      rc = cai_todo_write_record(active_dst, &record, error);
    }
    cai_todo_record_parse_cleanup(&record);
  }
  if (stream != NULL) {
    lonejson_stream_close(stream);
  }
  if (src != NULL) {
    fclose(src);
  }
  if (active_dst != NULL && fclose(active_dst) != 0 && rc == CAI_OK) {
    rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                       "failed to close active todo temp store");
  }
  if (done_dst != NULL && fclose(done_dst) != 0 && rc == CAI_OK) {
    rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                       "failed to close done todo temp store");
  }
  if (rc == CAI_OK && !found_item) {
    rc = cai_todo_set_result(result, args->operation, 0, "item_not_found",
                             "item was not found", error);
  }
  if (rc == CAI_OK && found_item) {
    rc = cai_todo_commit_temp(active_tmp, ctx->active_path, error);
  }
  if (rc == CAI_OK && found_item && complete) {
    rc = cai_todo_commit_temp(done_tmp, ctx->done_path, error);
  }
  if (rc == CAI_OK && found_item) {
    rc = cai_todo_set_result(result, args->operation, 1, "ok",
                             complete ? "item completed" : "item moved",
                             error);
  }
  if (rc == CAI_OK && found_item) {
    rc = cai_todo_copy_string(&result->item_id, args->item_id, error);
  }
  if (rc == CAI_OK && found_item && !complete) {
    rc = cai_todo_copy_string(&result->status, new_status, error);
  }
  if (active_tmp != NULL && rc != CAI_OK) {
    unlink(active_tmp);
  }
  if (done_tmp != NULL && rc != CAI_OK) {
    unlink(done_tmp);
  }
  cai_free_mem(NULL, active_tmp);
  cai_free_mem(NULL, done_tmp);
  cai_free_mem(NULL, target_board_id);
  cai_free_mem(NULL, target_board_name);
  cai_todo_unlock(&lock);
  return rc;
}

static int cai_todo_set_wip_limit(cai_todo_context *ctx,
                                  const cai_todo_args *args,
                                  cai_todo_result *result, cai_error *error) {
  cai_todo_file_lock lock;
  FILE *src;
  FILE *dst;
  lonejson_stream *stream;
  cai_todo_record record;
  char *tmp;
  int has_record;
  int found;
  int rc;

  if (!args->has_wip_limit || args->wip_limit < 0) {
    return cai_todo_set_result(result, args->operation, 0, "invalid_request",
                               "non-negative wip_limit is required", error);
  }
  lock.fd = -1;
  src = NULL;
  dst = NULL;
  stream = NULL;
  tmp = NULL;
  found = 0;
  rc = cai_todo_lock(ctx, &lock, error);
  if (rc == CAI_OK) {
    rc = cai_todo_tmp_path(&tmp, ctx->active_path, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_open_stream(ctx->active_path, &src, &stream, error);
  }
  if (rc == CAI_OK) {
    dst = fopen(tmp, "wb");
    if (dst == NULL) {
      rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to open todo temp store", tmp);
    }
  }
  while (rc == CAI_OK) {
    cai_todo_record_parse_init(&record);
    rc = cai_todo_stream_next(stream, &record, &has_record, error);
    if (rc != CAI_OK || !has_record) {
      cai_todo_record_parse_cleanup(&record);
      break;
    }
    if (cai_todo_board_matches(&record, args->board_id,
                               cai_todo_arg_string(args->board_name,
                                                   ctx->default_board))) {
      found = 1;
      record.wip_limit = args->wip_limit;
      record.has_wip_limit = 1;
    }
    if (rc == CAI_OK) {
      rc = cai_todo_write_record(dst, &record, error);
    }
    cai_todo_record_parse_cleanup(&record);
  }
  if (stream != NULL) {
    lonejson_stream_close(stream);
  }
  if (src != NULL) {
    fclose(src);
  }
  if (dst != NULL && fclose(dst) != 0 && rc == CAI_OK) {
    rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                       "failed to close todo temp store");
  }
  if (rc == CAI_OK && !found) {
    rc = cai_todo_set_result(result, args->operation, 0, "board_not_found",
                             "board was not found", error);
  }
  if (rc == CAI_OK && found) {
    rc = cai_todo_commit_temp(tmp, ctx->active_path, error);
  }
  if (rc == CAI_OK && found) {
    rc = cai_todo_set_result(result, args->operation, 1, "ok",
                             "WIP limit updated", error);
  }
  if (rc == CAI_OK && found) {
    result->wip_limit = args->wip_limit;
    result->has_wip_limit = 1;
  }
  if (tmp != NULL && rc != CAI_OK) {
    unlink(tmp);
  }
  cai_free_mem(NULL, tmp);
  cai_todo_unlock(&lock);
  return rc;
}

static int cai_todo_run(void *context, const void *params, void *out,
                        cai_error *error) {
  cai_todo_context *ctx;
  const cai_todo_args *args;
  cai_todo_result *result;

  ctx = (cai_todo_context *)context;
  args = (const cai_todo_args *)params;
  result = (cai_todo_result *)out;
  if (cai_todo_streq(args->operation, "create_board")) {
    return cai_todo_create_board(ctx, args, result, error);
  }
  if (cai_todo_streq(args->operation, "add_item")) {
    return cai_todo_add_item(ctx, args, result, error);
  }
  if (cai_todo_streq(args->operation, "list_board")) {
    return cai_todo_list(ctx, args, result, 0, error);
  }
  if (cai_todo_streq(args->operation, "list_boards")) {
    return cai_todo_list_boards(ctx, args, result, error);
  }
  if (cai_todo_streq(args->operation, "current_work")) {
    return cai_todo_list(ctx, args, result, 1, error);
  }
  if (cai_todo_streq(args->operation, "move_item")) {
    return cai_todo_rewrite_move(ctx, args, result, 0, error);
  }
  if (cai_todo_streq(args->operation, "complete_item")) {
    return cai_todo_rewrite_move(ctx, args, result, 1, error);
  }
  if (cai_todo_streq(args->operation, "set_wip_limit")) {
    return cai_todo_set_wip_limit(ctx, args, result, error);
  }
  return cai_todo_set_result(result, args->operation, 0, "unknown_operation",
                             "unknown todo operation", error);
}

int cai_tool_registry_register_todo_tool(cai_tool_registry *registry,
                                         const cai_todo_tool_config *config,
                                         cai_error *error) {
  cai_todo_context *ctx;
  const char *name;
  const char *description;
  int rc;

  if (registry == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "tool registry is required");
  }
  ctx = NULL;
  rc = cai_todo_context_new(config, &ctx, error);
  if (rc != CAI_OK) {
    return rc;
  }
  name = cai_todo_arg_string(config != NULL ? config->name : NULL,
                             CAI_TODO_DEFAULT_TOOL_NAME);
  description = cai_todo_arg_string(
      config != NULL ? config->description : NULL,
      "Manage persisted kanban todo boards. Use operation create_board, "
      "list_boards, add_item, list_board, current_work, move_item, "
      "complete_item, or set_wip_limit.");
  rc = cai_tool_registry_register_lonejson_owned(
      registry, name, description, &cai_todo_args_map, &cai_todo_result_map,
      cai_todo_run, ctx, cai_todo_context_cleanup, error);
  if (rc != CAI_OK) {
    cai_todo_context_cleanup(ctx);
  }
  return rc;
}

int cai_agent_register_todo_tool(cai_agent *agent,
                                 const cai_todo_tool_config *config,
                                 cai_error *error) {
  cai_agent_impl *impl;

  if (agent == NULL || agent->impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  impl = CAI_AGENT_IMPL(agent);
  return cai_tool_registry_register_todo_tool(impl->tools, config, error);
}
