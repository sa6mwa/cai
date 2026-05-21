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
#define CAI_TODO_FOUND_NONE (-9001)
#define CAI_TODO_INITIAL_STORE "{\"version\":1,\"boards\":[],\"items\":[],\"done\":[]}"
#define CAI_TODO_DESCRIPTION_1                                                \
  "Persistent kanban board tool for planning work. Start with operation=help " \
  "when uncertain. A configured default board always exists and is used when "
#define CAI_TODO_DESCRIPTION_2                                                \
  "board_id/board_key/board_name are omitted. Use list_boards to discover "   \
  "board IDs and keys. add_item creates readable item refs such as DEF-001. "
#define CAI_TODO_DESCRIPTION_3                                                \
  "current_work inspects in_process work; move_item moves todo/in_process; "  \
  "complete_item archives done work; set_wip_limit limits WIP; "              \
  "wip_limit_exceeded is a normal structured result."
#define CAI_TODO_DESCRIPTION                                                  \
  CAI_TODO_DESCRIPTION_1 CAI_TODO_DESCRIPTION_2 CAI_TODO_DESCRIPTION_3
#define CAI_TODO_HELP_TEXT_1                                                  \
  "todo_kanban usage: default board always exists; omit board_id, board_key " \
  "and board_name to use it. "
#define CAI_TODO_HELP_TEXT_2                                                  \
  "list_boards discovers IDs and keys. create_board accepts board_name, "     \
  "optional unique board_key, and wip_limit. "
#define CAI_TODO_HELP_TEXT_3                                                  \
  "add_item assigns item_id like DEF-001; callers cannot set sequence "       \
  "numbers. Refs accept DEF-001, DEF#1, DEF001, or DEF1. "                   \
  "current_work lists in_process; list_board lists active items. "
#define CAI_TODO_HELP_TEXT_4                                                  \
  "move_item/complete_item require item_id. WIP denial is "                   \
  "code=wip_limit_exceeded. Use returned IDs and keys."
#define CAI_TODO_HELP_TEXT                                                    \
  CAI_TODO_HELP_TEXT_1 CAI_TODO_HELP_TEXT_2 CAI_TODO_HELP_TEXT_3             \
      CAI_TODO_HELP_TEXT_4

typedef struct cai_todo_context {
  cai_todo_store_callbacks store;
  void *store_context;
  char *default_board;
  size_t max_title_bytes;
  size_t max_description_bytes;
  size_t max_result_items;
} cai_todo_context;

typedef struct cai_todo_args {
  char *operation;
  char *board_id;
  char *board_key;
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
  char *board_key;
  char *board_name;
  char *title;
  char *status;
} cai_todo_result_item;

typedef struct cai_todo_result_board {
  char *id;
  char *key;
  char *name;
  long long wip_limit;
  int has_wip_limit;
} cai_todo_result_board;

typedef struct cai_todo_result {
  int ok;
  int has_ok;
  char *operation;
  char *code;
  char *message;
  char *board_id;
  char *board_key;
  char *board_name;
  char *item_id;
  char *status;
  long long wip_limit;
  int has_wip_limit;
  long long in_process_count;
  int has_in_process_count;
  long long item_count;
  int has_item_count;
  long long board_count;
  int has_board_count;
  int truncated;
  int has_truncated;
  lonejson_object_array items;
  lonejson_object_array boards;
} cai_todo_result;

typedef struct cai_todo_board {
  char *id;
  char *key;
  char *name;
  long long next_sequence;
  int has_next_sequence;
  long long wip_limit;
  int has_wip_limit;
  long long created_at_unix;
  int has_created_at_unix;
  long long updated_at_unix;
  int has_updated_at_unix;
} cai_todo_board;

typedef struct cai_todo_item {
  char *id;
  char *item_id;
  char *board_id;
  char *board_key;
  char *board_name;
  char *status;
  lonejson_spooled title;
  lonejson_spooled description;
  long long created_at_unix;
  int has_created_at_unix;
  long long updated_at_unix;
  int has_updated_at_unix;
  long long completed_at_unix;
  int has_completed_at_unix;
} cai_todo_item;

typedef struct cai_todo_file_lock {
  int fd;
} cai_todo_file_lock;

typedef struct cai_todo_file_store {
  char *store_path;
  char *lock_path;
} cai_todo_file_store;

typedef struct cai_todo_file_transaction {
  cai_todo_file_store *store;
  cai_todo_file_lock lock;
  char *read_path;
  char *write_path;
  FILE *read_fp;
  FILE *write_fp;
  int owns_read_path;
} cai_todo_file_transaction;

typedef struct cai_todo_find_board_state {
  const char *board_id;
  const char *board_key;
  const char *board_name;
  cai_todo_board *out;
  int found;
} cai_todo_find_board_state;

typedef struct cai_todo_count_items_state {
  const char *board_id;
  long long items;
  long long in_process;
} cai_todo_count_items_state;

typedef struct cai_todo_id_state {
  const char *id;
  int found;
} cai_todo_id_state;

typedef struct cai_todo_board_key_state {
  const char *key;
  const char *allow_board_id;
  int found;
} cai_todo_board_key_state;

typedef struct cai_todo_sequence_state {
  const char *board_key;
  long long max_sequence;
} cai_todo_sequence_state;

typedef struct cai_todo_missing_board_key_state {
  char *name;
  int found;
} cai_todo_missing_board_key_state;

typedef struct cai_todo_list_state {
  cai_todo_result *result;
  const char *board_id;
  const char *board_name;
  int only_current;
  size_t max_items;
  cai_error *error;
  int rc;
} cai_todo_list_state;

typedef struct cai_todo_board_list_state {
  cai_todo_result *result;
  size_t max_items;
  cai_error *error;
  int rc;
  long long count;
} cai_todo_board_list_state;

typedef struct cai_todo_rewrite_state {
  const cai_todo_context *ctx;
  const cai_todo_args *args;
  cai_todo_board board;
  cai_todo_board replacement_board;
  cai_todo_item item;
  cai_todo_item replacement_item;
  cai_todo_item append_item;
  const char *new_status;
  const char *target_board_id;
  const char *old_board_key;
  const char *new_board_key;
  long long next_sequence;
  int found;
  int complete;
  long long now_unix;
} cai_todo_rewrite_state;

static void cai_todo_file_store_destroy(void *context);
static int cai_todo_file_store_begin(void *context, void **transaction,
                                     cai_error *error);
static int cai_todo_file_store_open_read(void *context, void *transaction,
                                         lonejson_reader_fn *reader,
                                         void **reader_context,
                                         cai_error *error);
static void cai_todo_file_store_close_read(void *context, void *transaction,
                                           void *reader_context);
static int cai_todo_file_store_open_write(void *context, void *transaction,
                                          lonejson_sink_fn *sink,
                                          void **sink_context,
                                          cai_error *error);
static int cai_todo_file_store_commit_write(void *context, void *transaction,
                                            cai_error *error);
static int cai_todo_file_store_commit(void *context, void *transaction,
                                      cai_error *error);
static void cai_todo_file_store_rollback(void *context, void *transaction);

static const lonejson_field cai_todo_arg_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_args, operation, "operation"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_todo_args, board_id, "board_id"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_todo_args, board_key,
                                          "board_key"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_todo_args, board_name,
                                          "board_name"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_todo_args, item_id, "item_id"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_todo_args, title, "title"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_todo_args, description,
                                          "description"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_todo_args, status, "status"),
    LONEJSON_FIELD_I64_PRESENT_NULLABLE(cai_todo_args, wip_limit,
                                        has_wip_limit, "wip_limit")};
LONEJSON_MAP_DEFINE(cai_todo_args_map, cai_todo_args, cai_todo_arg_fields);

static const lonejson_field cai_todo_result_item_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_result_item, id, "id"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_result_item, board_id, "board_id"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_result_item, board_key,
                                    "board_key"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_result_item, board_name,
                                    "board_name"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_result_item, title, "title"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_result_item, status, "status")};
LONEJSON_MAP_DEFINE(cai_todo_result_item_map, cai_todo_result_item,
                    cai_todo_result_item_fields);

static const lonejson_field cai_todo_result_board_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_result_board, id, "id"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_result_board, key, "key"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_result_board, name, "name"),
    LONEJSON_FIELD_I64_PRESENT(cai_todo_result_board, wip_limit,
                               has_wip_limit, "wip_limit")};
LONEJSON_MAP_DEFINE(cai_todo_result_board_map, cai_todo_result_board,
                    cai_todo_result_board_fields);

static const lonejson_field cai_todo_result_fields[] = {
    LONEJSON_FIELD_BOOL_PRESENT(cai_todo_result, ok, has_ok, "ok"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_result, operation, "operation"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_result, code, "code"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_result, message, "message"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_todo_result, board_id,
                                          "board_id"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_todo_result, board_key,
                                          "board_key"),
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
    LONEJSON_FIELD_I64_PRESENT(cai_todo_result, board_count, has_board_count,
                               "board_count"),
    LONEJSON_FIELD_BOOL_PRESENT(cai_todo_result, truncated, has_truncated,
                                "truncated"),
    LONEJSON_FIELD_OBJECT_ARRAY_OMIT_EMPTY(cai_todo_result, items, "items",
                                           cai_todo_result_item,
                                           &cai_todo_result_item_map,
                                           LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_OBJECT_ARRAY_OMIT_EMPTY(cai_todo_result, boards, "boards",
                                           cai_todo_result_board,
                                           &cai_todo_result_board_map,
                                           LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(cai_todo_result_map, cai_todo_result,
                    cai_todo_result_fields);

static const lonejson_field cai_todo_board_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_board, id, "id"),
    LONEJSON_FIELD_STRING_ALLOC(cai_todo_board, key, "key"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_board, name, "name"),
    LONEJSON_FIELD_I64_PRESENT(cai_todo_board, next_sequence,
                               has_next_sequence, "next_sequence"),
    LONEJSON_FIELD_I64_PRESENT(cai_todo_board, wip_limit, has_wip_limit,
                               "wip_limit"),
    LONEJSON_FIELD_I64_PRESENT(cai_todo_board, created_at_unix,
                               has_created_at_unix, "created_at_unix"),
    LONEJSON_FIELD_I64_PRESENT(cai_todo_board, updated_at_unix,
                               has_updated_at_unix, "updated_at_unix")};
LONEJSON_MAP_DEFINE(cai_todo_board_map, cai_todo_board,
                    cai_todo_board_fields);

static const lonejson_field cai_todo_item_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_item, id, "id"),
    LONEJSON_FIELD_STRING_ALLOC(cai_todo_item, item_id, "item_id"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_item, board_id, "board_id"),
    LONEJSON_FIELD_STRING_ALLOC(cai_todo_item, board_key, "board_key"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_item, board_name, "board_name"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_todo_item, status, "status"),
    LONEJSON_FIELD_STRING_STREAM_REQ(cai_todo_item, title, "title"),
    LONEJSON_FIELD_STRING_STREAM(cai_todo_item, description, "description"),
    LONEJSON_FIELD_I64_PRESENT(cai_todo_item, created_at_unix,
                               has_created_at_unix, "created_at_unix"),
    LONEJSON_FIELD_I64_PRESENT(cai_todo_item, updated_at_unix,
                               has_updated_at_unix, "updated_at_unix"),
    LONEJSON_FIELD_I64_PRESENT(cai_todo_item, completed_at_unix,
                               has_completed_at_unix, "completed_at_unix")};
LONEJSON_MAP_DEFINE(cai_todo_item_map, cai_todo_item, cai_todo_item_fields);

static int cai_todo_streq(const char *a, const char *b) {
  return a != NULL && b != NULL && strcmp(a, b) == 0;
}

static const char *cai_todo_arg_string(const char *value,
                                       const char *fallback) {
  return value != NULL && value[0] != '\0' ? value : fallback;
}

static int cai_todo_char_is_alnum(unsigned char ch) {
  return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
         (ch >= 'a' && ch <= 'z');
}

static char cai_todo_char_upper(unsigned char ch) {
  if (ch >= 'a' && ch <= 'z') {
    return (char)(ch - ('a' - 'A'));
  }
  return (char)ch;
}

static int cai_todo_normalize_board_key(const char *input, char *out,
                                        size_t out_size, cai_error *error) {
  size_t i;
  size_t j;
  unsigned char ch;

  if (input == NULL || input[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "board_key is required");
  }
  j = 0U;
  for (i = 0U; input[i] != '\0'; i++) {
    ch = (unsigned char)input[i];
    if (cai_todo_char_is_alnum(ch)) {
      if (j + 1U >= out_size) {
        return cai_set_error(error, CAI_ERR_INVALID, "board_key is too long");
      }
      out[j++] = cai_todo_char_upper(ch);
    } else if (ch == '-' || ch == '_' || ch == '#' || ch == ' ') {
      continue;
    } else {
      return cai_set_error(error, CAI_ERR_INVALID,
                           "board_key must be alphanumeric");
    }
  }
  if (j == 0U) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "board_key must contain alphanumeric characters");
  }
  out[j] = '\0';
  return CAI_OK;
}

static void cai_todo_default_key_candidate(const char *name, char *out,
                                           size_t out_size,
                                           size_t key_length) {
  size_t i;
  size_t j;
  unsigned char ch;

  j = 0U;
  for (i = 0U; name != NULL && name[i] != '\0' && j < key_length &&
              j + 1U < out_size;
       i++) {
    ch = (unsigned char)name[i];
    if (cai_todo_char_is_alnum(ch)) {
      out[j++] = cai_todo_char_upper(ch);
    }
  }
  while (j < key_length && j + 1U < out_size) {
    out[j++] = 'X';
  }
  out[j] = '\0';
}

static int cai_todo_format_item_id(const char *board_key, long long sequence,
                                   char *out, size_t out_size,
                                   cai_error *error) {
  int n;

  if (board_key == NULL || board_key[0] == '\0' || sequence <= 0) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "board key and positive sequence are required");
  }
  if (sequence < 1000LL) {
    n = snprintf(out, out_size, "%s-%03lld", board_key, sequence);
  } else {
    n = snprintf(out, out_size, "%s-%lld", board_key, sequence);
  }
  if (n < 0 || (size_t)n >= out_size) {
    return cai_set_error(error, CAI_ERR_INVALID, "item_id is too long");
  }
  return CAI_OK;
}

static long long cai_todo_parse_item_sequence(const char *item_id,
                                              const char *board_key) {
  const char *p;
  size_t key_len;
  size_t i;
  long long value;

  if (item_id == NULL || board_key == NULL) {
    return 0LL;
  }
  key_len = strlen(board_key);
  if (key_len == 0U) {
    return 0LL;
  }
  for (i = 0U; i < key_len; i++) {
    if (cai_todo_char_upper((unsigned char)item_id[i]) !=
        cai_todo_char_upper((unsigned char)board_key[i])) {
      return 0LL;
    }
  }
  p = item_id + key_len;
  if (*p == '-' || *p == '#') {
    p++;
  }
  if (*p < '0' || *p > '9') {
    return 0LL;
  }
  value = 0LL;
  while (*p >= '0' && *p <= '9') {
    if (value > 922337203685477580LL) {
      return 0LL;
    }
    value = value * 10LL + (long long)(*p - '0');
    p++;
  }
  return *p == '\0' ? value : 0LL;
}

static int cai_todo_item_ref_matches(const cai_todo_item *item,
                                     const char *input) {
  long long stored_sequence;
  long long input_sequence;

  if (item == NULL || input == NULL) {
    return 0;
  }
  if (cai_todo_streq(item->id, input) || cai_todo_streq(item->item_id, input)) {
    return 1;
  }
  if (item->board_key == NULL || item->board_key[0] == '\0') {
    return 0;
  }
  stored_sequence = cai_todo_parse_item_sequence(item->item_id, item->board_key);
  input_sequence = cai_todo_parse_item_sequence(input, item->board_key);
  return stored_sequence > 0LL && input_sequence == stored_sequence;
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

static int cai_todo_replace_string(char **out, const char *value,
                                   cai_error *error) {
  cai_free_mem(NULL, *out);
  *out = NULL;
  return cai_todo_copy_string(out, value, error);
}

static void cai_todo_context_cleanup(void *context) {
  cai_todo_context *ctx;

  ctx = (cai_todo_context *)context;
  if (ctx == NULL) {
    return;
  }
  if (ctx->store.destroy != NULL) {
    ctx->store.destroy(ctx->store_context);
  }
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

static int cai_todo_write_initial_store(const char *path, cai_error *error) {
  FILE *fp;
  size_t len;

  fp = fopen(path, "wb");
  if (fp == NULL) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to create todo store", path);
  }
  len = strlen(CAI_TODO_INITIAL_STORE);
  if (fwrite(CAI_TODO_INITIAL_STORE, 1U, len, fp) != len ||
      fputc('\n', fp) == EOF) {
    fclose(fp);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to write todo store", path);
  }
  if (fclose(fp) != 0) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to close todo store", path);
  }
  return CAI_OK;
}

static int cai_todo_ensure_store(const char *path, cai_error *error) {
  struct stat st;

  if (cai_todo_mkdirs_for_file(path, error) != CAI_OK) {
    return error != NULL ? error->code : CAI_ERR_TRANSPORT;
  }
  if (stat(path, &st) == 0) {
    if (st.st_size == 0) {
      return cai_todo_write_initial_store(path, error);
    }
    return CAI_OK;
  }
  if (errno != ENOENT) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to stat todo store", path);
  }
  return cai_todo_write_initial_store(path, error);
}

static int cai_todo_ensure_file(const char *path, cai_error *error) {
  int fd;

  if (cai_todo_mkdirs_for_file(path, error) != CAI_OK) {
    return error != NULL ? error->code : CAI_ERR_TRANSPORT;
  }
  fd = open(path, O_CREAT | O_APPEND | O_WRONLY, 0600);
  if (fd < 0) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to create todo file", path);
  }
  close(fd);
  return CAI_OK;
}

static int cai_todo_context_new(const cai_todo_tool_config *config,
                                cai_todo_context **out, cai_error *error) {
  cai_todo_context *ctx;
  cai_todo_file_store *file_store;
  int rc;

  *out = NULL;
  file_store = NULL;
  ctx = (cai_todo_context *)cai_alloc(NULL, sizeof(*ctx));
  if (ctx == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate todo tool context");
  }
  memset(ctx, 0, sizeof(*ctx));
  rc = CAI_OK;
  if (config != NULL && config->store != NULL) {
    ctx->store = *config->store;
    ctx->store_context = config->store_context;
  } else {
    file_store = (cai_todo_file_store *)cai_alloc(NULL, sizeof(*file_store));
    if (file_store == NULL) {
      rc = cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate todo file store");
    } else {
      memset(file_store, 0, sizeof(*file_store));
      if (config != NULL && config->store_path != NULL &&
          config->store_path[0] != '\0') {
        rc = cai_todo_copy_string(&file_store->store_path, config->store_path,
                                  error);
      } else {
        rc = cai_todo_default_path(&file_store->store_path,
                                   CAI_TODO_DEFAULT_STORE_FILE, error);
      }
      if (rc == CAI_OK) {
        if (config != NULL && config->lock_path != NULL &&
            config->lock_path[0] != '\0') {
          rc = cai_todo_copy_string(&file_store->lock_path, config->lock_path,
                                    error);
        } else {
          rc = cai_todo_default_path(&file_store->lock_path,
                                     CAI_TODO_DEFAULT_LOCK_FILE, error);
        }
      }
      if (rc == CAI_OK) {
        ctx->store.begin = cai_todo_file_store_begin;
        ctx->store.open_read = cai_todo_file_store_open_read;
        ctx->store.close_read = cai_todo_file_store_close_read;
        ctx->store.open_write = cai_todo_file_store_open_write;
        ctx->store.commit_write = cai_todo_file_store_commit_write;
        ctx->store.commit = cai_todo_file_store_commit;
        ctx->store.rollback = cai_todo_file_store_rollback;
        ctx->store.destroy = cai_todo_file_store_destroy;
        ctx->store_context = file_store;
        file_store = NULL;
      }
    }
  }
  if (rc == CAI_OK) {
    if (ctx->store.begin == NULL || ctx->store.open_read == NULL ||
        ctx->store.close_read == NULL || ctx->store.open_write == NULL ||
        ctx->store.commit_write == NULL || ctx->store.commit == NULL ||
        ctx->store.rollback == NULL) {
      rc = cai_set_error(error, CAI_ERR_INVALID,
                         "todo store callbacks are incomplete");
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
    cai_todo_file_store_destroy(file_store);
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
  *out = ctx;
  return CAI_OK;
}

static int cai_todo_lock_file(const char *lock_path, cai_todo_file_lock *lock,
                              cai_error *error) {
  struct flock fl;

  lock->fd = -1;
  if (cai_todo_ensure_file(lock_path, error) != CAI_OK) {
    return error != NULL ? error->code : CAI_ERR_TRANSPORT;
  }
  lock->fd = open(lock_path, O_RDWR, 0600);
  if (lock->fd < 0) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to open todo lockfile", lock_path);
  }
  memset(&fl, 0, sizeof(fl));
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  if (fcntl(lock->fd, F_SETLKW, &fl) != 0) {
    close(lock->fd);
    lock->fd = -1;
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to lock todo store", lock_path);
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
  struct timeval tv;
  size_t len;
  size_t suffix_len;
  char *tmp;
  static unsigned int counter;

  gettimeofday(&tv, NULL);
  snprintf(suffix, sizeof(suffix), ".tmp.%ld.%ld.%ld.%u", (long)getpid(),
           (long)tv.tv_sec, (long)tv.tv_usec, ++counter);
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

static int cai_todo_commit_temp(const char *tmp_path, const char *target_path,
                                cai_error *error) {
  if (rename(tmp_path, target_path) != 0) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to commit todo temp store",
                                target_path);
  }
  return CAI_OK;
}

static lonejson_read_result cai_todo_file_read(void *user,
                                               unsigned char *buffer,
                                               size_t capacity) {
  FILE *fp;
  lonejson_read_result result;

  result = lonejson_default_read_result();
  fp = (FILE *)user;
  if (capacity == 0U) {
    return result;
  }
  result.bytes_read = fread(buffer, 1U, capacity, fp);
  if (result.bytes_read < capacity) {
    if (ferror(fp)) {
      result.error_code = errno != 0 ? errno : EIO;
    } else if (feof(fp)) {
      result.eof = 1;
    }
  }
  return result;
}

static lonejson_status cai_todo_file_write(void *user, const void *data,
                                           size_t len,
                                           lonejson_error *error) {
  FILE *fp;

  fp = (FILE *)user;
  if (len != 0U && fwrite(data, 1U, len, fp) != len) {
    if (error != NULL) {
      lonejson_error_init(error);
      error->code = LONEJSON_STATUS_IO_ERROR;
      error->system_errno = errno != 0 ? errno : EIO;
      snprintf(error->message, sizeof(error->message),
               "failed to write todo store");
    }
    return LONEJSON_STATUS_IO_ERROR;
  }
  return LONEJSON_STATUS_OK;
}

static void cai_todo_file_store_destroy(void *context) {
  cai_todo_file_store *store;

  store = (cai_todo_file_store *)context;
  if (store == NULL) {
    return;
  }
  cai_free_mem(NULL, store->store_path);
  cai_free_mem(NULL, store->lock_path);
  cai_free_mem(NULL, store);
}

static int cai_todo_file_store_begin(void *context, void **transaction,
                                     cai_error *error) {
  cai_todo_file_store *store;
  cai_todo_file_transaction *txn;
  int rc;

  *transaction = NULL;
  store = (cai_todo_file_store *)context;
  if (store == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "todo file store is required");
  }
  rc = cai_todo_ensure_store(store->store_path, error);
  if (rc == CAI_OK) {
    rc = cai_todo_ensure_file(store->lock_path, error);
  }
  if (rc != CAI_OK) {
    return rc;
  }
  txn = (cai_todo_file_transaction *)cai_alloc(NULL, sizeof(*txn));
  if (txn == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate todo store transaction");
  }
  memset(txn, 0, sizeof(*txn));
  txn->store = store;
  txn->lock.fd = -1;
  rc = cai_todo_lock_file(store->lock_path, &txn->lock, error);
  if (rc != CAI_OK) {
    cai_free_mem(NULL, txn);
    return rc;
  }
  rc = cai_todo_copy_string(&txn->read_path, store->store_path, error);
  if (rc != CAI_OK) {
    cai_todo_unlock(&txn->lock);
    cai_free_mem(NULL, txn);
    return rc;
  }
  *transaction = txn;
  return CAI_OK;
}

static int cai_todo_file_store_open_read(void *context, void *transaction,
                                         lonejson_reader_fn *reader,
                                         void **reader_context,
                                         cai_error *error) {
  cai_todo_file_transaction *txn;

  (void)context;
  txn = (cai_todo_file_transaction *)transaction;
  if (txn == NULL || reader == NULL || reader_context == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "todo read transaction is required");
  }
  if (txn->read_fp != NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "todo read stream is already open");
  }
  txn->read_fp = fopen(txn->read_path, "rb");
  if (txn->read_fp == NULL) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to open todo store for reading",
                                txn->read_path);
  }
  *reader = cai_todo_file_read;
  *reader_context = txn->read_fp;
  return CAI_OK;
}

static void cai_todo_file_store_close_read(void *context, void *transaction,
                                           void *reader_context) {
  cai_todo_file_transaction *txn;

  (void)context;
  (void)reader_context;
  txn = (cai_todo_file_transaction *)transaction;
  if (txn != NULL && txn->read_fp != NULL) {
    fclose(txn->read_fp);
    txn->read_fp = NULL;
  }
}

static int cai_todo_file_store_open_write(void *context, void *transaction,
                                          lonejson_sink_fn *sink,
                                          void **sink_context,
                                          cai_error *error) {
  cai_todo_file_transaction *txn;
  int rc;

  (void)context;
  txn = (cai_todo_file_transaction *)transaction;
  if (txn == NULL || sink == NULL || sink_context == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "todo write transaction is required");
  }
  if (txn->write_fp != NULL || txn->write_path != NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "todo write stream is already open");
  }
  rc = cai_todo_tmp_path(&txn->write_path, txn->store->store_path, error);
  if (rc != CAI_OK) {
    return rc;
  }
  txn->write_fp = fopen(txn->write_path, "wb");
  if (txn->write_fp == NULL) {
    rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                              "failed to open todo temp store",
                              txn->write_path);
    cai_free_mem(NULL, txn->write_path);
    txn->write_path = NULL;
    return rc;
  }
  *sink = cai_todo_file_write;
  *sink_context = txn->write_fp;
  return CAI_OK;
}

static int cai_todo_file_store_commit_write(void *context, void *transaction,
                                            cai_error *error) {
  cai_todo_file_transaction *txn;

  (void)context;
  txn = (cai_todo_file_transaction *)transaction;
  if (txn == NULL || txn->write_fp == NULL || txn->write_path == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "todo write stream is not open");
  }
  if (fflush(txn->write_fp) != 0 || fclose(txn->write_fp) != 0) {
    txn->write_fp = NULL;
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to close todo temp store",
                                txn->write_path);
  }
  txn->write_fp = NULL;
  if (txn->owns_read_path && txn->read_path != NULL) {
    unlink(txn->read_path);
  }
  cai_free_mem(NULL, txn->read_path);
  txn->read_path = txn->write_path;
  txn->write_path = NULL;
  txn->owns_read_path = 1;
  return CAI_OK;
}

static int cai_todo_file_store_commit(void *context, void *transaction,
                                      cai_error *error) {
  cai_todo_file_transaction *txn;
  int rc;

  (void)context;
  txn = (cai_todo_file_transaction *)transaction;
  if (txn == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "todo transaction is required");
  }
  rc = CAI_OK;
  if (txn->write_fp != NULL) {
    rc = cai_todo_file_store_commit_write(context, transaction, error);
  }
  if (rc == CAI_OK && txn->owns_read_path && txn->read_path != NULL) {
    rc = cai_todo_commit_temp(txn->read_path, txn->store->store_path, error);
    if (rc == CAI_OK) {
      cai_free_mem(NULL, txn->read_path);
      txn->read_path = NULL;
      txn->owns_read_path = 0;
    }
  }
  if (txn->write_fp != NULL) {
    fclose(txn->write_fp);
    txn->write_fp = NULL;
  }
  if (txn->read_fp != NULL) {
    fclose(txn->read_fp);
    txn->read_fp = NULL;
  }
  if (txn->write_path != NULL) {
    unlink(txn->write_path);
  }
  cai_free_mem(NULL, txn->write_path);
  if (txn->owns_read_path && txn->read_path != NULL) {
    unlink(txn->read_path);
  }
  cai_free_mem(NULL, txn->read_path);
  cai_todo_unlock(&txn->lock);
  cai_free_mem(NULL, txn);
  return rc;
}

static void cai_todo_file_store_rollback(void *context, void *transaction) {
  cai_todo_file_transaction *txn;

  (void)context;
  txn = (cai_todo_file_transaction *)transaction;
  if (txn == NULL) {
    return;
  }
  if (txn->read_fp != NULL) {
    fclose(txn->read_fp);
  }
  if (txn->write_fp != NULL) {
    fclose(txn->write_fp);
  }
  if (txn->write_path != NULL) {
    unlink(txn->write_path);
  }
  if (txn->owns_read_path && txn->read_path != NULL) {
    unlink(txn->read_path);
  }
  cai_free_mem(NULL, txn->write_path);
  cai_free_mem(NULL, txn->read_path);
  cai_todo_unlock(&txn->lock);
  cai_free_mem(NULL, txn);
}

static int cai_todo_store_begin(const cai_todo_context *ctx, void **txn,
                                cai_error *error) {
  if (ctx->store.begin == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "todo store begin callback is required");
  }
  return ctx->store.begin(ctx->store_context, txn, error);
}

static void cai_todo_store_rollback(const cai_todo_context *ctx, void *txn) {
  if (ctx != NULL && ctx->store.rollback != NULL && txn != NULL) {
    ctx->store.rollback(ctx->store_context, txn);
  }
}

static int cai_todo_store_commit(const cai_todo_context *ctx, void *txn,
                                 cai_error *error) {
  if (ctx->store.commit == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "todo store commit callback is required");
  }
  return ctx->store.commit(ctx->store_context, txn, error);
}

static int cai_todo_rewrite(const cai_todo_context *ctx, void *txn,
                            const char *selector,
                            const lonejson_array_rewrite_options *options,
                            cai_error *error) {
  lonejson_error json_error;
  lonejson_reader_fn reader;
  lonejson_sink_fn sink;
  void *reader_context;
  void *sink_context;
  int rc;

  if (ctx->store.open_read == NULL || ctx->store.open_write == NULL ||
      ctx->store.commit_write == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "todo store streaming callbacks are required");
  }
  reader = NULL;
  sink = NULL;
  reader_context = NULL;
  sink_context = NULL;
  rc = ctx->store.open_read(ctx->store_context, txn, &reader, &reader_context,
                            error);
  if (rc == CAI_OK) {
    rc = ctx->store.open_write(ctx->store_context, txn, &sink, &sink_context,
                               error);
  }
  lonejson_error_init(&json_error);
  if (rc == CAI_OK &&
      lonejson_array_rewrite_reader(selector, reader, reader_context, sink,
                                    sink_context, options,
                                    &json_error) != LONEJSON_STATUS_OK) {
    rc = cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                              "failed to rewrite todo store",
                              json_error.message);
  }
  if (ctx->store.close_read != NULL && reader_context != NULL) {
    ctx->store.close_read(ctx->store_context, txn, reader_context);
  }
  if (rc == CAI_OK) {
    rc = ctx->store.commit_write(ctx->store_context, txn, error);
  }
  return rc;
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

static lonejson_status cai_todo_spool_sink(void *user, const void *data,
                                           size_t len, lonejson_error *error) {
  return lonejson_spooled_append((lonejson_spooled *)user, data, len, error);
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

static int cai_todo_item_init(cai_todo_item *item) {
  memset(item, 0, sizeof(*item));
  lonejson_spooled_init(&item->title, NULL);
  lonejson_spooled_init(&item->description, NULL);
  lonejson_init(&cai_todo_item_map, item);
  return CAI_OK;
}

static void cai_todo_item_cleanup(cai_todo_item *item) {
  if (item == NULL) {
    return;
  }
  cai_free_mem(NULL, item->id);
  cai_free_mem(NULL, item->item_id);
  cai_free_mem(NULL, item->board_id);
  cai_free_mem(NULL, item->board_key);
  cai_free_mem(NULL, item->board_name);
  cai_free_mem(NULL, item->status);
  lonejson_spooled_cleanup(&item->title);
  lonejson_spooled_cleanup(&item->description);
  memset(item, 0, sizeof(*item));
}

static void cai_todo_item_parse_init(cai_todo_item *item) {
  memset(item, 0, sizeof(*item));
}

static void cai_todo_item_parse_cleanup(cai_todo_item *item) {
  lonejson_cleanup(&cai_todo_item_map, item);
}

static void cai_todo_board_init(cai_todo_board *board) {
  memset(board, 0, sizeof(*board));
  lonejson_init(&cai_todo_board_map, board);
}

static void cai_todo_board_cleanup(cai_todo_board *board) {
  if (board == NULL) {
    return;
  }
  cai_free_mem(NULL, board->id);
  cai_free_mem(NULL, board->key);
  cai_free_mem(NULL, board->name);
  memset(board, 0, sizeof(*board));
}

static void cai_todo_board_parse_init(cai_todo_board *board) {
  memset(board, 0, sizeof(*board));
}

static void cai_todo_board_parse_cleanup(cai_todo_board *board) {
  lonejson_cleanup(&cai_todo_board_map, board);
}

static int cai_todo_add_result_item(cai_todo_result *result,
                                    const cai_todo_item *record,
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
  rc = cai_todo_copy_string(&item->id,
                            record->item_id != NULL ? record->item_id
                                                    : record->id,
                            error);
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&item->board_id, record->board_id, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&item->board_key,
                              record->board_key != NULL ? record->board_key
                                                        : "",
                              error);
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
                                     const cai_todo_board *board,
                                     size_t max_items, cai_error *error) {
  cai_todo_result_board *result_board;
  int rc;

  if (result->boards.count >= max_items) {
    result->truncated = 1;
    result->has_truncated = 1;
    return CAI_OK;
  }
  rc = cai_todo_array_grow(&result->boards, sizeof(*result_board), error);
  if (rc != CAI_OK) {
    return rc;
  }
  result_board = (cai_todo_result_board *)result->boards.items +
                 result->boards.count;
  memset(result_board, 0, sizeof(*result_board));
  rc = cai_todo_copy_string(&result_board->id, board->id, error);
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&result_board->key,
                              board->key != NULL ? board->key : "", error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&result_board->name,
                              board->name != NULL ? board->name : "", error);
  }
  if (rc == CAI_OK && board->has_wip_limit && board->wip_limit >= 0) {
    result_board->wip_limit = board->wip_limit;
    result_board->has_wip_limit = 1;
  }
  if (rc == CAI_OK) {
    result->boards.count++;
  }
  return rc;
}

static int cai_todo_copy_board(cai_todo_board *dst, const cai_todo_board *src,
                               cai_error *error) {
  int rc;

  cai_todo_board_cleanup(dst);
  cai_todo_board_init(dst);
  rc = cai_todo_copy_string(&dst->id, src->id, error);
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&dst->key, src->key != NULL ? src->key : "",
                              error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&dst->name, src->name, error);
  }
  dst->next_sequence = src->has_next_sequence ? src->next_sequence : 1LL;
  dst->has_next_sequence = 1;
  dst->wip_limit = src->wip_limit;
  dst->has_wip_limit = src->has_wip_limit;
  dst->created_at_unix = src->created_at_unix;
  dst->has_created_at_unix = src->has_created_at_unix;
  dst->updated_at_unix = src->updated_at_unix;
  dst->has_updated_at_unix = src->has_updated_at_unix;
  return rc;
}

static int cai_todo_copy_item(cai_todo_item *dst, const cai_todo_item *src,
                              cai_error *error) {
  int rc;

  cai_todo_item_cleanup(dst);
  cai_todo_item_init(dst);
  rc = cai_todo_copy_string(&dst->id, src->id, error);
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&dst->item_id,
                              src->item_id != NULL ? src->item_id : src->id,
                              error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&dst->board_id, src->board_id, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&dst->board_key,
                              src->board_key != NULL ? src->board_key : "",
                              error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&dst->board_name, src->board_name, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&dst->status, src->status, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_spool_clone(&dst->title, &src->title, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_spool_clone(&dst->description, &src->description, error);
  }
  dst->created_at_unix = src->created_at_unix;
  dst->has_created_at_unix = src->has_created_at_unix;
  dst->updated_at_unix = src->updated_at_unix;
  dst->has_updated_at_unix = src->has_updated_at_unix;
  dst->completed_at_unix = src->completed_at_unix;
  dst->has_completed_at_unix = src->has_completed_at_unix;
  return rc;
}

static int cai_todo_stream_boards(const cai_todo_context *ctx, void *txn,
                                  lonejson_array_stream_item_fn callback,
                                  void *user, cai_error *error) {
  lonejson_array_stream *stream;
  lonejson_array_stream_result result;
  lonejson_error json_error;
  lonejson_reader_fn reader;
  void *reader_context;
  void *local_txn;
  cai_todo_board board;
  int rc;

  local_txn = NULL;
  reader = NULL;
  reader_context = NULL;
  if (txn == NULL) {
    rc = cai_todo_store_begin(ctx, &local_txn, error);
    if (rc != CAI_OK) {
      return rc;
    }
    txn = local_txn;
  }
  if (ctx->store.open_read == NULL) {
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "todo store open_read callback is required");
    cai_todo_store_rollback(ctx, local_txn);
    return rc;
  }
  rc = ctx->store.open_read(ctx->store_context, txn, &reader, &reader_context,
                            error);
  if (rc != CAI_OK) {
    cai_todo_store_rollback(ctx, local_txn);
    return rc;
  }
  lonejson_error_init(&json_error);
  stream = lonejson_array_stream_open_reader("boards", reader, reader_context,
                                             NULL, &json_error);
  if (stream == NULL) {
    if (ctx->store.close_read != NULL) {
      ctx->store.close_read(ctx->store_context, txn, reader_context);
    }
    cai_todo_store_rollback(ctx, local_txn);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to open todo boards",
                                json_error.message);
  }
  rc = CAI_OK;
  for (;;) {
    cai_todo_board_parse_init(&board);
    result = lonejson_array_stream_next(stream, &cai_todo_board_map, &board,
                                        &json_error);
    if (result == LONEJSON_ARRAY_STREAM_ITEM) {
      if (callback(user, &board) != LONEJSON_STATUS_OK) {
        rc = error != NULL && error->code != 0 ? error->code : CAI_ERR_PROTOCOL;
      }
      cai_todo_board_parse_cleanup(&board);
      if (rc != CAI_OK) {
        break;
      }
      continue;
    }
    cai_todo_board_parse_cleanup(&board);
    if (result == LONEJSON_ARRAY_STREAM_EOF) {
      break;
    }
    rc = cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                              "failed to parse todo boards",
                              lonejson_array_stream_error(stream) != NULL
                                  ? lonejson_array_stream_error(stream)->message
                                  : json_error.message);
    break;
  }
  lonejson_array_stream_close(stream);
  if (ctx->store.close_read != NULL) {
    ctx->store.close_read(ctx->store_context, txn, reader_context);
  }
  if (local_txn != NULL) {
    if (rc == CAI_OK) {
      rc = cai_todo_store_commit(ctx, local_txn, error);
    } else {
      cai_todo_store_rollback(ctx, local_txn);
    }
  }
  return rc;
}

static int cai_todo_stream_items_path(const cai_todo_context *ctx,
                                      void *txn, const char *path,
                                      lonejson_array_stream_item_fn callback,
                                      void *user, cai_error *error) {
  lonejson_array_stream *stream;
  lonejson_array_stream_result result;
  lonejson_error json_error;
  lonejson_reader_fn reader;
  void *reader_context;
  void *local_txn;
  cai_todo_item item;
  int rc;

  local_txn = NULL;
  reader = NULL;
  reader_context = NULL;
  if (txn == NULL) {
    rc = cai_todo_store_begin(ctx, &local_txn, error);
    if (rc != CAI_OK) {
      return rc;
    }
    txn = local_txn;
  }
  if (ctx->store.open_read == NULL) {
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "todo store open_read callback is required");
    cai_todo_store_rollback(ctx, local_txn);
    return rc;
  }
  rc = ctx->store.open_read(ctx->store_context, txn, &reader, &reader_context,
                            error);
  if (rc != CAI_OK) {
    cai_todo_store_rollback(ctx, local_txn);
    return rc;
  }
  lonejson_error_init(&json_error);
  stream = lonejson_array_stream_open_reader(path, reader, reader_context, NULL,
                                             &json_error);
  if (stream == NULL) {
    if (ctx->store.close_read != NULL) {
      ctx->store.close_read(ctx->store_context, txn, reader_context);
    }
    cai_todo_store_rollback(ctx, local_txn);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to open todo items",
                                json_error.message);
  }
  rc = CAI_OK;
  for (;;) {
    cai_todo_item_parse_init(&item);
    result = lonejson_array_stream_next(stream, &cai_todo_item_map, &item,
                                        &json_error);
    if (result == LONEJSON_ARRAY_STREAM_ITEM) {
      if (callback(user, &item) != LONEJSON_STATUS_OK) {
        rc = error != NULL && error->code != 0 ? error->code : CAI_ERR_PROTOCOL;
      }
      cai_todo_item_parse_cleanup(&item);
      if (rc != CAI_OK) {
        break;
      }
      continue;
    }
    cai_todo_item_parse_cleanup(&item);
    if (result == LONEJSON_ARRAY_STREAM_EOF) {
      break;
    }
    rc = cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                              "failed to parse todo items",
                              lonejson_array_stream_error(stream) != NULL
                                  ? lonejson_array_stream_error(stream)->message
                                  : json_error.message);
    break;
  }
  lonejson_array_stream_close(stream);
  if (ctx->store.close_read != NULL) {
    ctx->store.close_read(ctx->store_context, txn, reader_context);
  }
  if (local_txn != NULL) {
    if (rc == CAI_OK) {
      rc = cai_todo_store_commit(ctx, local_txn, error);
    } else {
      cai_todo_store_rollback(ctx, local_txn);
    }
  }
  return rc;
}

static lonejson_status cai_todo_find_board_cb(void *user, void *dst) {
  cai_todo_find_board_state *state;
  cai_todo_board *board;

  state = (cai_todo_find_board_state *)user;
  board = (cai_todo_board *)dst;
  if (state->found) {
    return LONEJSON_STATUS_OK;
  }
  if ((state->board_id != NULL && state->board_id[0] != '\0' &&
       cai_todo_streq(board->id, state->board_id)) ||
      ((state->board_id == NULL || state->board_id[0] == '\0') &&
       state->board_key != NULL && state->board_key[0] != '\0' &&
       cai_todo_streq(board->key, state->board_key)) ||
      ((state->board_id == NULL || state->board_id[0] == '\0') &&
       (state->board_key == NULL || state->board_key[0] == '\0') &&
       state->board_name != NULL && state->board_name[0] != '\0' &&
       cai_todo_streq(board->name, state->board_name))) {
    state->found = 1;
    if (cai_todo_copy_board(state->out, board, NULL) != CAI_OK) {
      return LONEJSON_STATUS_ALLOCATION_FAILED;
    }
  }
  return LONEJSON_STATUS_OK;
}

static int cai_todo_find_board(cai_todo_context *ctx, void *txn,
                               const char *board_id,
                               const char *board_key,
                               const char *board_name, cai_todo_board *out,
                               long long *in_process, long long *items,
                               cai_error *error);

static lonejson_status cai_todo_count_items_cb(void *user, void *dst) {
  cai_todo_count_items_state *state;
  cai_todo_item *item;

  state = (cai_todo_count_items_state *)user;
  item = (cai_todo_item *)dst;
  if (cai_todo_streq(item->board_id, state->board_id)) {
    state->items++;
    if (cai_todo_streq(item->status, CAI_TODO_STATUS_IN_PROCESS)) {
      state->in_process++;
    }
  }
  return LONEJSON_STATUS_OK;
}

static int cai_todo_find_board(cai_todo_context *ctx, void *txn,
                               const char *board_id,
                               const char *board_key,
                               const char *board_name, cai_todo_board *out,
                               long long *in_process, long long *items,
                               cai_error *error) {
  cai_todo_find_board_state board_state;
  cai_todo_count_items_state item_state;
  int rc;

  cai_todo_board_init(out);
  memset(&board_state, 0, sizeof(board_state));
  board_state.board_id = board_id;
  board_state.board_key = board_key;
  board_state.board_name = board_name;
  board_state.out = out;
  rc = cai_todo_stream_boards(ctx, txn, cai_todo_find_board_cb, &board_state,
                              error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (!board_state.found) {
    cai_todo_board_cleanup(out);
    return CAI_TODO_FOUND_NONE;
  }
  memset(&item_state, 0, sizeof(item_state));
  item_state.board_id = out->id;
  rc = cai_todo_stream_items_path(ctx, txn, "items", cai_todo_count_items_cb,
                                  &item_state, error);
  if (rc != CAI_OK) {
    cai_todo_board_cleanup(out);
    return rc;
  }
  *items = item_state.items;
  *in_process = item_state.in_process;
  return CAI_OK;
}

static lonejson_status cai_todo_id_board_cb(void *user, void *dst) {
  cai_todo_id_state *state;
  cai_todo_board *board;

  state = (cai_todo_id_state *)user;
  board = (cai_todo_board *)dst;
  if (cai_todo_streq(board->id, state->id)) {
    state->found = 1;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_todo_board_key_cb(void *user, void *dst) {
  cai_todo_board_key_state *state;
  cai_todo_board *board;

  state = (cai_todo_board_key_state *)user;
  board = (cai_todo_board *)dst;
  if (board->key != NULL && cai_todo_streq(board->key, state->key) &&
      (state->allow_board_id == NULL ||
       !cai_todo_streq(board->id, state->allow_board_id))) {
    state->found = 1;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_todo_id_item_cb(void *user, void *dst) {
  cai_todo_id_state *state;
  cai_todo_item *item;

  state = (cai_todo_id_state *)user;
  item = (cai_todo_item *)dst;
  if (cai_todo_item_ref_matches(item, state->id)) {
    state->found = 1;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_todo_sequence_cb(void *user, void *dst) {
  cai_todo_sequence_state *state;
  cai_todo_item *item;
  long long sequence;

  state = (cai_todo_sequence_state *)user;
  item = (cai_todo_item *)dst;
  sequence = cai_todo_parse_item_sequence(item->item_id, state->board_key);
  if (sequence > state->max_sequence) {
    state->max_sequence = sequence;
  }
  return LONEJSON_STATUS_OK;
}

static int cai_todo_board_key_exists(const cai_todo_context *ctx, void *txn,
                                     const char *key,
                                     const char *allow_board_id,
                                     cai_error *error) {
  cai_todo_board_key_state state;
  int rc;

  memset(&state, 0, sizeof(state));
  state.key = key;
  state.allow_board_id = allow_board_id;
  rc = cai_todo_stream_boards(ctx, txn, cai_todo_board_key_cb, &state, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return state.found ? 1 : 0;
}

static int cai_todo_generate_board_key(const cai_todo_context *ctx, void *txn,
                                       const char *name,
                                       const char *requested_key,
                                       const char *allow_board_id,
                                       char out[16], cai_error *error) {
  char candidate[16];
  size_t key_len;
  int exists;
  int suffix;
  int n;

  if (requested_key != NULL && requested_key[0] != '\0') {
    if (cai_todo_normalize_board_key(requested_key, candidate,
                                     sizeof(candidate), error) != CAI_OK) {
      return error != NULL ? error->code : CAI_ERR_INVALID;
    }
    if (cai_todo_streq(candidate, "DEF") &&
        !cai_todo_streq(name, ctx->default_board)) {
      return cai_set_error(error, CAI_ERR_INVALID,
                           "board_key DEF is reserved for the default board");
    }
    exists = cai_todo_board_key_exists(ctx, txn, candidate, allow_board_id,
                                       error);
    if (exists != 0) {
      return exists > 0 ? cai_set_error(error, CAI_ERR_INVALID,
                                        "board_key must be unique")
                        : exists;
    }
    strcpy(out, candidate);
    return CAI_OK;
  }
  for (key_len = 3U; key_len < sizeof(candidate) - 1U; key_len++) {
    cai_todo_default_key_candidate(name, candidate, sizeof(candidate),
                                   key_len);
    if (cai_todo_streq(candidate, "DEF") &&
        !cai_todo_streq(name, ctx->default_board)) {
      continue;
    }
    exists = cai_todo_board_key_exists(ctx, txn, candidate, allow_board_id,
                                       error);
    if (exists == 0) {
      strcpy(out, candidate);
      return CAI_OK;
    }
    if (exists < 0) {
      return exists;
    }
  }
  cai_todo_default_key_candidate(name, candidate, sizeof(candidate), 3U);
  for (suffix = 2; suffix < 10000; suffix++) {
    n = snprintf(out, 16U, "%s%d", candidate, suffix);
    if (n < 0 || n >= 16) {
      return cai_set_error(error, CAI_ERR_INVALID, "board_key is too long");
    }
    exists = cai_todo_board_key_exists(ctx, txn, out, allow_board_id, error);
    if (exists == 0) {
      return CAI_OK;
    }
    if (exists < 0) {
      return exists;
    }
  }
  return cai_set_error(error, CAI_ERR_INVALID,
                       "failed to generate unique board_key");
}

static int cai_todo_next_item_sequence(const cai_todo_context *ctx, void *txn,
                                       const cai_todo_board *board,
                                       long long *sequence,
                                       cai_error *error) {
  cai_todo_sequence_state state;
  int rc;

  memset(&state, 0, sizeof(state));
  state.board_key = board->key;
  state.max_sequence = 0LL;
  rc = cai_todo_stream_items_path(ctx, txn, "items", cai_todo_sequence_cb,
                                  &state, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_todo_stream_items_path(ctx, txn, "done", cai_todo_sequence_cb,
                                  &state, error);
  if (rc != CAI_OK) {
    return rc;
  }
  *sequence = board->has_next_sequence && board->next_sequence > 0
                  ? board->next_sequence
                  : 1LL;
  if (*sequence <= state.max_sequence) {
    *sequence = state.max_sequence + 1LL;
  }
  return CAI_OK;
}

static int cai_todo_id_exists(const cai_todo_context *ctx, void *txn,
                              const char *id, cai_error *error) {
  cai_todo_id_state state;
  int rc;

  memset(&state, 0, sizeof(state));
  state.id = id;
  rc = cai_todo_stream_boards(ctx, txn, cai_todo_id_board_cb, &state, error);
  if (rc != CAI_OK || state.found) {
    return rc == CAI_OK ? 1 : rc;
  }
  rc = cai_todo_stream_items_path(ctx, txn, "items", cai_todo_id_item_cb, &state,
                                  error);
  if (rc != CAI_OK || state.found) {
    return rc == CAI_OK ? 1 : rc;
  }
  rc = cai_todo_stream_items_path(ctx, txn, "done", cai_todo_id_item_cb, &state,
                                  error);
  if (rc != CAI_OK) {
    return rc;
  }
  return state.found ? 1 : 0;
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

static int cai_todo_generate_id(const cai_todo_context *ctx, void *txn,
                                char out[32], cai_error *error) {
  struct timeval tv;
  unsigned long long random_part;
  unsigned long long value;
  unsigned int counter;
  int fd;
  int tries;
  int exists;

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
    exists = cai_todo_id_exists(ctx, txn, out, error);
    if (exists == 0) {
      return CAI_OK;
    }
    if (exists < 0) {
      return exists;
    }
    random_part += 0x9e3779b97f4a7c15ULL;
  }
  return cai_set_error(error, CAI_ERR_INVALID,
                       "failed to generate unique todo id");
}

static int cai_todo_set_board(cai_todo_board *board, const char *id,
                              const char *key, const char *name,
                              long long wip_limit, int has_wip_limit,
                              long long now_unix, cai_error *error) {
  int rc;

  cai_todo_board_cleanup(board);
  cai_todo_board_init(board);
  rc = cai_todo_copy_string(&board->id, id, error);
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&board->key, key, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&board->name, name, error);
  }
  board->next_sequence = 1LL;
  board->has_next_sequence = 1;
  board->wip_limit = has_wip_limit ? wip_limit : -1;
  board->has_wip_limit = 1;
  board->created_at_unix = now_unix;
  board->has_created_at_unix = 1;
  board->updated_at_unix = now_unix;
  board->has_updated_at_unix = 1;
  return rc;
}

static int cai_todo_set_item(cai_todo_item *item, const char *id,
                             const char *item_id, const char *board_id,
                             const char *board_key, const char *board_name,
                             const char *title, const char *description,
                             const char *status, long long now_unix,
                             cai_error *error) {
  int rc;

  cai_todo_item_cleanup(item);
  cai_todo_item_init(item);
  rc = cai_todo_copy_string(&item->id, id, error);
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&item->item_id, item_id, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&item->board_id, board_id, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&item->board_key, board_key, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&item->board_name, board_name, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&item->status, status, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_spool_string(&item->title, title, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_spool_string(&item->description,
                               description != NULL ? description : "", error);
  }
  item->created_at_unix = now_unix;
  item->has_created_at_unix = 1;
  item->updated_at_unix = now_unix;
  item->has_updated_at_unix = 1;
  return rc;
}

static lonejson_status cai_todo_update_board_cb(
    void *user, const lonejson_array_rewrite_context *context, void *item,
    lonejson_array_rewrite_result *result, lonejson_error *error);

static int cai_todo_rewrite_items_board_key(cai_todo_context *ctx, void *txn,
                                            const char *path,
                                            const char *board_id,
                                            const char *old_key,
                                            const char *new_key,
                                            long long *next_sequence,
                                            cai_error *error);

static lonejson_status cai_todo_append_board_cb(
    void *user, const lonejson_array_rewrite_context *context,
    lonejson_array_rewrite_emit_fn emit, void *emit_user,
    lonejson_error *error) {
  cai_todo_rewrite_state *state;
  lonejson_array_rewrite_source source;

  (void)context;
  state = (cai_todo_rewrite_state *)user;
  memset(&source, 0, sizeof(source));
  source.map = &cai_todo_board_map;
  source.src = &state->board;
  return emit(emit_user, &source, error);
}

static int cai_todo_board_arg_is_default(cai_todo_context *ctx,
                                         const cai_todo_args *args) {
  const char *board_name;

  if (ctx == NULL || args == NULL ||
      (args->board_id != NULL && args->board_id[0] != '\0') ||
      (args->board_key != NULL && args->board_key[0] != '\0')) {
    return 0;
  }
  board_name = cai_todo_arg_string(args->board_name, ctx->default_board);
  return cai_todo_streq(board_name, ctx->default_board);
}

static int cai_todo_ensure_board(cai_todo_context *ctx, void *txn,
                                 const char *name, cai_todo_board *out,
                                 long long *in_process, long long *items,
                                 long long wip_limit, int has_wip_limit,
                                 const char *requested_key,
                                 int *created, cai_error *error) {
  cai_todo_rewrite_state state;
  lonejson_array_rewrite_options options;
  char id[32];
  char key[16];
  long long next_sequence;
  int rc;

  if (created != NULL) {
    *created = 0;
  }
  rc = cai_todo_find_board(ctx, txn, NULL, NULL, name, out, in_process, items,
                           error);
  if (rc == CAI_OK && (out->key == NULL || out->key[0] == '\0')) {
    rc = cai_todo_generate_board_key(
        ctx, txn, name,
        requested_key != NULL && requested_key[0] != '\0'
            ? requested_key
            : (cai_todo_streq(name, ctx->default_board) ? "DEF" : NULL),
        out->id, key, error);
  }
  if (rc == CAI_OK && (out->key == NULL || out->key[0] == '\0')) {
    next_sequence =
        out->has_next_sequence && out->next_sequence > 0
            ? out->next_sequence
            : 1LL;
    rc = cai_todo_rewrite_items_board_key(ctx, txn, "items", out->id,
                                          out->key, key, &next_sequence,
                                          error);
    if (rc == CAI_OK) {
      rc = cai_todo_rewrite_items_board_key(ctx, txn, "done", out->id,
                                            out->key, key, &next_sequence,
                                            error);
    }
  }
  if (rc == CAI_OK && (out->key == NULL || out->key[0] == '\0')) {
    cai_todo_args update_args;

    memset(&update_args, 0, sizeof(update_args));
    update_args.board_id = out->id;
    memset(&state, 0, sizeof(state));
    memset(&options, 0, sizeof(options));
    state.ctx = ctx;
    state.args = &update_args;
    state.new_board_key = key;
    state.next_sequence = next_sequence;
    state.now_unix = (long long)time(NULL);
    options.item_map = &cai_todo_board_map;
    options.item_dst = &state.board;
    options.item = cai_todo_update_board_cb;
    options.user = &state;
    rc = cai_todo_rewrite(ctx, txn, "boards", &options, error);
    cai_todo_board_cleanup(&state.replacement_board);
    if (rc == CAI_OK) {
      rc = cai_todo_replace_string(&out->key, key, error);
    }
  }
  if (rc != CAI_TODO_FOUND_NONE) {
    return rc;
  }

  memset(&state, 0, sizeof(state));
  memset(&options, 0, sizeof(options));
  cai_todo_board_init(&state.board);
  rc = cai_todo_generate_id(ctx, txn, id, error);
  if (rc == CAI_OK) {
    rc = cai_todo_generate_board_key(
        ctx, txn, name,
        requested_key != NULL && requested_key[0] != '\0'
            ? requested_key
            : (cai_todo_streq(name, ctx->default_board) ? "DEF" : NULL),
        NULL, key, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_set_board(&state.board, id, key, name, wip_limit,
                            has_wip_limit, (long long)time(NULL), error);
  }
  if (rc == CAI_OK) {
    options.append = cai_todo_append_board_cb;
    options.user = &state;
    rc = cai_todo_rewrite(ctx, txn, "boards", &options, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_board(out, &state.board, error);
  }
  if (rc == CAI_OK) {
    *in_process = 0;
    *items = 0;
    if (created != NULL) {
      *created = 1;
    }
  }
  cai_todo_board_cleanup(&state.board);
  return rc;
}

static int cai_todo_ensure_default_board_committed(cai_todo_context *ctx,
                                                   cai_error *error) {
  cai_todo_board board;
  cai_todo_rewrite_state update_state;
  lonejson_array_rewrite_options update_options;
  long long in_process;
  long long items;
  void *txn;
  int created;
  int rc;

  cai_todo_board_init(&board);
  memset(&update_state, 0, sizeof(update_state));
  memset(&update_options, 0, sizeof(update_options));
  in_process = 0;
  items = 0;
  txn = NULL;
  rc = cai_todo_store_begin(ctx, &txn, error);
  if (rc == CAI_OK) {
    rc = cai_todo_ensure_board(ctx, txn, ctx->default_board, &board,
                               &in_process, &items, -1, 0, NULL, &created,
                               error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_store_commit(ctx, txn, error);
    txn = NULL;
  }
  if (txn != NULL) {
    cai_todo_store_rollback(ctx, txn);
  }
  cai_todo_board_cleanup(&board);
  return rc;
}

static lonejson_status cai_todo_missing_board_key_cb(void *user, void *dst) {
  cai_todo_missing_board_key_state *state;
  cai_todo_board *board;

  state = (cai_todo_missing_board_key_state *)user;
  board = (cai_todo_board *)dst;
  if (state->found ||
      (board->key != NULL && board->key[0] != '\0') ||
      board->name == NULL || board->name[0] == '\0') {
    return LONEJSON_STATUS_OK;
  }
  if (cai_todo_copy_string(&state->name, board->name, NULL) != CAI_OK) {
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  state->found = 1;
  return LONEJSON_STATUS_OK;
}

static int cai_todo_backfill_named_board_key(cai_todo_context *ctx,
                                             const char *name,
                                             cai_error *error) {
  cai_todo_board board;
  long long in_process;
  long long items;
  void *txn;
  int created;
  int rc;

  cai_todo_board_init(&board);
  in_process = 0;
  items = 0;
  txn = NULL;
  created = 0;
  rc = cai_todo_store_begin(ctx, &txn, error);
  if (rc == CAI_OK) {
    rc = cai_todo_ensure_board(ctx, txn, name, &board, &in_process, &items, -1,
                               0, NULL, &created, error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_store_commit(ctx, txn, error);
    txn = NULL;
  }
  if (txn != NULL) {
    cai_todo_store_rollback(ctx, txn);
  }
  cai_todo_board_cleanup(&board);
  return rc;
}

static int cai_todo_backfill_board_keys(cai_todo_context *ctx,
                                        cai_error *error) {
  cai_todo_missing_board_key_state state;
  int rc;
  int guard;

  for (guard = 0; guard < 10000; guard++) {
    memset(&state, 0, sizeof(state));
    rc = cai_todo_stream_boards(ctx, NULL, cai_todo_missing_board_key_cb,
                                &state, error);
    if (rc == CAI_OK && !state.found) {
      cai_free_mem(NULL, state.name);
      return CAI_OK;
    }
    if (rc == CAI_OK) {
      rc = cai_todo_backfill_named_board_key(ctx, state.name, error);
    }
    cai_free_mem(NULL, state.name);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  return cai_set_error(error, CAI_ERR_INVALID,
                       "too many todo boards require key migration");
}

static lonejson_status cai_todo_append_item_cb(
    void *user, const lonejson_array_rewrite_context *context,
    lonejson_array_rewrite_emit_fn emit, void *emit_user,
    lonejson_error *error) {
  cai_todo_rewrite_state *state;
  lonejson_array_rewrite_source source;

  (void)context;
  state = (cai_todo_rewrite_state *)user;
  memset(&source, 0, sizeof(source));
  source.map = &cai_todo_item_map;
  source.src = &state->item;
  return emit(emit_user, &source, error);
}

static lonejson_status cai_todo_append_done_cb(
    void *user, const lonejson_array_rewrite_context *context,
    lonejson_array_rewrite_emit_fn emit, void *emit_user,
    lonejson_error *error) {
  cai_todo_rewrite_state *state;
  lonejson_array_rewrite_source source;

  (void)context;
  state = (cai_todo_rewrite_state *)user;
  if (!state->found) {
    return LONEJSON_STATUS_OK;
  }
  memset(&source, 0, sizeof(source));
  source.map = &cai_todo_item_map;
  source.src = &state->append_item;
  return emit(emit_user, &source, error);
}

static lonejson_status cai_todo_update_board_cb(
    void *user, const lonejson_array_rewrite_context *context, void *item,
    lonejson_array_rewrite_result *result, lonejson_error *error) {
  cai_todo_rewrite_state *state;
  cai_todo_board *board;

  (void)context;
  (void)error;
  state = (cai_todo_rewrite_state *)user;
  board = (cai_todo_board *)item;
  if ((state->args->board_id != NULL && state->args->board_id[0] != '\0' &&
       cai_todo_streq(board->id, state->args->board_id)) ||
      ((state->args->board_id == NULL || state->args->board_id[0] == '\0') &&
       state->args->board_key != NULL && state->args->board_key[0] != '\0' &&
       cai_todo_streq(board->key, state->args->board_key)) ||
      ((state->args->board_id == NULL || state->args->board_id[0] == '\0') &&
       (state->args->board_key == NULL ||
        state->args->board_key[0] == '\0') &&
       cai_todo_streq(board->name,
                      cai_todo_arg_string(state->args->board_name,
                                          state->ctx->default_board)))) {
    state->found = 1;
    cai_todo_board_init(&state->replacement_board);
    if (cai_todo_copy_board(&state->replacement_board, board, NULL) !=
        CAI_OK) {
      return LONEJSON_STATUS_ALLOCATION_FAILED;
    }
    if (state->new_board_key != NULL && state->new_board_key[0] != '\0') {
      if (cai_todo_replace_string(&state->replacement_board.key,
                                  state->new_board_key, NULL) != CAI_OK) {
        return LONEJSON_STATUS_ALLOCATION_FAILED;
      }
    }
    if (state->args->has_wip_limit) {
      state->replacement_board.wip_limit = state->args->wip_limit;
      state->replacement_board.has_wip_limit = 1;
    }
    if (state->next_sequence > 0) {
      state->replacement_board.next_sequence = state->next_sequence;
      state->replacement_board.has_next_sequence = 1;
    }
    if (!state->replacement_board.has_next_sequence ||
        state->replacement_board.next_sequence <= 0) {
      state->replacement_board.next_sequence = 1LL;
      state->replacement_board.has_next_sequence = 1;
    }
    state->replacement_board.updated_at_unix = state->now_unix;
    state->replacement_board.has_updated_at_unix = 1;
    result->action = LONEJSON_ARRAY_REWRITE_REPLACE;
    result->replacement.map = &cai_todo_board_map;
    result->replacement.src = &state->replacement_board;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_todo_move_item_cb(
    void *user, const lonejson_array_rewrite_context *context, void *item,
    lonejson_array_rewrite_result *result, lonejson_error *error) {
  cai_todo_rewrite_state *state;
  cai_todo_item *src;

  (void)context;
  state = (cai_todo_rewrite_state *)user;
  src = (cai_todo_item *)item;
  if (!cai_todo_item_ref_matches(src, state->args->item_id)) {
    return LONEJSON_STATUS_OK;
  }
  state->found = 1;
  if (state->complete) {
    if (cai_todo_copy_item(&state->append_item, src, NULL) != CAI_OK) {
      (void)error;
      return LONEJSON_STATUS_ALLOCATION_FAILED;
    }
    state->append_item.completed_at_unix = state->now_unix;
    state->append_item.has_completed_at_unix = 1;
    state->append_item.updated_at_unix = state->now_unix;
    state->append_item.has_updated_at_unix = 1;
    result->action = LONEJSON_ARRAY_REWRITE_DROP;
  } else {
    if (cai_todo_copy_item(&state->replacement_item, src, NULL) != CAI_OK) {
      return LONEJSON_STATUS_ALLOCATION_FAILED;
    }
    if (cai_todo_replace_string(&state->replacement_item.status,
                                state->new_status, NULL) !=
        CAI_OK) {
      return LONEJSON_STATUS_ALLOCATION_FAILED;
    }
    state->replacement_item.updated_at_unix = state->now_unix;
    state->replacement_item.has_updated_at_unix = 1;
    result->action = LONEJSON_ARRAY_REWRITE_REPLACE;
    result->replacement.map = &cai_todo_item_map;
    result->replacement.src = &state->replacement_item;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_todo_update_item_board_key_cb(
    void *user, const lonejson_array_rewrite_context *context, void *item,
    lonejson_array_rewrite_result *result, lonejson_error *error) {
  cai_todo_rewrite_state *state;
  cai_todo_item *src;
  long long sequence;
  char item_ref[32];

  (void)context;
  state = (cai_todo_rewrite_state *)user;
  src = (cai_todo_item *)item;
  if (!cai_todo_streq(src->board_id, state->target_board_id)) {
    return LONEJSON_STATUS_OK;
  }
  cai_todo_item_cleanup(&state->replacement_item);
  if (cai_todo_copy_item(&state->replacement_item, src, NULL) != CAI_OK) {
    (void)error;
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  if (cai_todo_replace_string(&state->replacement_item.board_key,
                              state->new_board_key, NULL) != CAI_OK) {
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  sequence = cai_todo_parse_item_sequence(src->item_id, state->old_board_key);
  if (sequence <= 0) {
    sequence = cai_todo_parse_item_sequence(src->item_id, src->board_key);
  }
  if (sequence <= 0) {
    sequence = cai_todo_parse_item_sequence(src->item_id, state->new_board_key);
  }
  if (sequence <= 0) {
    sequence = state->next_sequence > 0 ? state->next_sequence : 1LL;
  }
  if (sequence > 0) {
    if (cai_todo_format_item_id(state->new_board_key, sequence, item_ref,
                                sizeof(item_ref), NULL) != CAI_OK ||
        cai_todo_replace_string(&state->replacement_item.item_id, item_ref,
                                NULL) != CAI_OK) {
      return LONEJSON_STATUS_ALLOCATION_FAILED;
    }
    if (state->next_sequence <= sequence) {
      state->next_sequence = sequence + 1LL;
    }
  }
  result->action = LONEJSON_ARRAY_REWRITE_REPLACE;
  result->replacement.map = &cai_todo_item_map;
  result->replacement.src = &state->replacement_item;
  return LONEJSON_STATUS_OK;
}

static int cai_todo_rewrite_items_board_key(cai_todo_context *ctx, void *txn,
                                            const char *path,
                                            const char *board_id,
                                            const char *old_key,
                                            const char *new_key,
                                            long long *next_sequence,
                                            cai_error *error) {
  cai_todo_rewrite_state state;
  lonejson_array_rewrite_options options;
  int rc;

  memset(&state, 0, sizeof(state));
  memset(&options, 0, sizeof(options));
  cai_todo_item_init(&state.replacement_item);
  state.target_board_id = board_id;
  state.old_board_key = old_key;
  state.new_board_key = new_key;
  state.next_sequence = next_sequence != NULL ? *next_sequence : 1LL;
  options.item_map = &cai_todo_item_map;
  options.item_dst = &state.item;
  options.item = cai_todo_update_item_board_key_cb;
  options.user = &state;
  rc = cai_todo_rewrite(ctx, txn, path, &options, error);
  if (rc == CAI_OK && next_sequence != NULL) {
    *next_sequence = state.next_sequence;
  }
  cai_todo_item_cleanup(&state.replacement_item);
  return rc;
}

static int cai_todo_create_board(cai_todo_context *ctx,
                                 const cai_todo_args *args,
                                 cai_todo_result *result, cai_error *error) {
  cai_todo_board board;
  cai_todo_rewrite_state update_state;
  lonejson_array_rewrite_options update_options;
  cai_todo_args update_args;
  long long in_process;
  long long items;
  long long next_sequence;
  const char *name;
  char new_key[16];
  int has_new_key;
  void *txn;
  int created;
  int targeted;
  int rc;

  cai_todo_board_init(&board);
  memset(&update_state, 0, sizeof(update_state));
  memset(&update_options, 0, sizeof(update_options));
  memset(&update_args, 0, sizeof(update_args));
  in_process = 0;
  items = 0;
  next_sequence = 1LL;
  txn = NULL;
  created = 0;
  targeted = 0;
  has_new_key = 0;
  new_key[0] = '\0';
  rc = cai_todo_store_begin(ctx, &txn, error);
  if (rc != CAI_OK) {
    cai_todo_board_cleanup(&board);
    return rc;
  }
  name = args->board_name != NULL && args->board_name[0] != '\0'
             ? args->board_name
             : NULL;
  targeted = (args->board_id != NULL && args->board_id[0] != '\0') ||
             (name == NULL && args->board_key != NULL &&
              args->board_key[0] != '\0');
  if (rc == CAI_OK && targeted) {
    rc = cai_todo_find_board(ctx, txn, args->board_id,
                             args->board_id != NULL &&
                                     args->board_id[0] != '\0'
                                 ? NULL
                                 : args->board_key,
                             NULL, &board, &in_process, &items, error);
    if (rc == CAI_TODO_FOUND_NONE) {
      rc = cai_todo_set_result(result, args->operation, 0, "board_not_found",
                               "board was not found", error);
    }
    if (rc == CAI_OK && board.name != NULL) {
      name = board.name;
    }
  } else if (rc == CAI_OK) {
    name = cai_todo_arg_string(args->board_name, ctx->default_board);
    rc = cai_todo_ensure_board(ctx, txn, name, &board, &in_process, &items,
                               args->wip_limit, args->has_wip_limit,
                               args->board_key, &created, error);
  }
  if (rc == CAI_OK && !(result->has_ok && !result->ok) && !created &&
      args->board_key != NULL &&
      args->board_key[0] != '\0') {
    if ((args->board_id != NULL && args->board_id[0] != '\0') || !targeted) {
      rc = cai_todo_generate_board_key(ctx, txn, name, args->board_key,
                                       board.id, new_key, error);
      if (rc == CAI_OK && !cai_todo_streq(board.key, new_key)) {
        has_new_key = 1;
      }
    }
  }
  if (rc == CAI_OK && !(result->has_ok && !result->ok) && !created &&
      (args->has_wip_limit || has_new_key)) {
    if (has_new_key) {
      rc = cai_todo_next_item_sequence(ctx, txn, &board, &next_sequence,
                                       error);
    }
    if (rc == CAI_OK && has_new_key) {
      rc = cai_todo_rewrite_items_board_key(ctx, txn, "items", board.id,
                                            board.key, new_key,
                                            &next_sequence, error);
    }
    if (rc == CAI_OK && has_new_key) {
      rc = cai_todo_rewrite_items_board_key(ctx, txn, "done", board.id,
                                            board.key, new_key,
                                            &next_sequence, error);
    }
    if (rc == CAI_OK) {
      update_args = *args;
      update_args.board_id = board.id;
      update_args.board_key = NULL;
      update_state.ctx = ctx;
      update_state.args = &update_args;
      update_state.new_board_key = has_new_key ? new_key : NULL;
      update_state.next_sequence = has_new_key ? next_sequence : 0LL;
      update_state.now_unix = (long long)time(NULL);
      update_options.item_map = &cai_todo_board_map;
      update_options.item_dst = &update_state.board;
      update_options.item = cai_todo_update_board_cb;
      update_options.user = &update_state;
      rc = cai_todo_rewrite(ctx, txn, "boards", &update_options, error);
      cai_todo_board_cleanup(&update_state.replacement_board);
    }
    if (rc == CAI_OK && update_state.found) {
      if (has_new_key) {
        rc = cai_todo_replace_string(&board.key, new_key, error);
        board.next_sequence = next_sequence;
        board.has_next_sequence = 1;
      }
      if (rc == CAI_OK && args->has_wip_limit) {
        board.wip_limit = args->wip_limit;
        board.has_wip_limit = 1;
      }
    }
  }
  if (rc == CAI_OK) {
    rc = cai_todo_store_commit(ctx, txn, error);
    txn = NULL;
  }
  if (rc == CAI_OK && !(result->has_ok && !result->ok)) {
    rc = cai_todo_set_result(result, args->operation, 1, "ok",
                             created ? "board created"
                                     : "board already exists",
                             error);
  }
  if (rc == CAI_OK && !(result->has_ok && !result->ok)) {
    rc = cai_todo_copy_string(&result->board_id, board.id, error);
  }
  if (rc == CAI_OK && !(result->has_ok && !result->ok)) {
    rc = cai_todo_copy_string(&result->board_key,
                              board.key != NULL ? board.key : "", error);
  }
  if (rc == CAI_OK && !(result->has_ok && !result->ok)) {
    rc = cai_todo_copy_string(&result->board_name, name, error);
  }
  if (rc == CAI_OK && !(result->has_ok && !result->ok) &&
      board.has_wip_limit && board.wip_limit >= 0) {
    result->wip_limit = board.wip_limit;
    result->has_wip_limit = 1;
  }
  result->item_count = items;
  result->has_item_count = 1;
  result->in_process_count = in_process;
  result->has_in_process_count = 1;
  if (txn != NULL) {
    cai_todo_store_rollback(ctx, txn);
  }
  cai_todo_board_cleanup(&board);
  return rc;
}

static int cai_todo_add_item(cai_todo_context *ctx, const cai_todo_args *args,
                             cai_todo_result *result, cai_error *error) {
  cai_todo_rewrite_state state;
  lonejson_array_rewrite_options options;
  cai_todo_board board;
  char id[32];
  char item_ref[32];
  long long sequence;
  long long in_process;
  long long items;
  const char *status;
  void *txn;
  int board_found;
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
  memset(&state, 0, sizeof(state));
  memset(&options, 0, sizeof(options));
  cai_todo_item_init(&state.item);
  cai_todo_board_init(&board);
  in_process = 0;
  items = 0;
  sequence = 0LL;
  item_ref[0] = '\0';
  board_found = 0;
  txn = NULL;
  rc = cai_todo_store_begin(ctx, &txn, error);
  if (rc != CAI_OK) {
    cai_todo_item_cleanup(&state.item);
    cai_todo_board_cleanup(&board);
    return rc;
  }
  if (cai_todo_board_arg_is_default(ctx, args)) {
    rc = cai_todo_ensure_board(ctx, txn, ctx->default_board, &board,
                               &in_process, &items, -1, 0, NULL, NULL, error);
  } else {
    rc = cai_todo_find_board(ctx, txn, args->board_id, args->board_key,
                             cai_todo_arg_string(args->board_name,
                                                 ctx->default_board),
                             &board, &in_process, &items, error);
  }
  if (rc == CAI_TODO_FOUND_NONE) {
    rc = cai_todo_set_result(result, args->operation, 0, "board_not_found",
                             "board was not found", error);
  } else if (rc == CAI_OK) {
    board_found = 1;
  }
  status = cai_todo_arg_string(args->status, CAI_TODO_STATUS_TODO);
  if (rc == CAI_OK && !cai_todo_streq(status, CAI_TODO_STATUS_TODO) &&
      !cai_todo_streq(status, CAI_TODO_STATUS_IN_PROCESS)) {
    rc = cai_todo_set_result(result, args->operation, 0, "invalid_status",
                             "status must be todo or in_process", error);
  }
  if (rc == CAI_OK && cai_todo_streq(status, CAI_TODO_STATUS_IN_PROCESS) &&
      board.has_wip_limit && board.wip_limit >= 0 &&
      in_process >= board.wip_limit) {
    rc = cai_todo_set_result(result, args->operation, 0, "wip_limit_exceeded",
                             "board WIP limit would be exceeded", error);
  }
  if (rc == CAI_OK && !(result->has_ok && !result->ok)) {
    rc = cai_todo_generate_id(ctx, txn, id, error);
  }
  if (rc == CAI_OK && !(result->has_ok && !result->ok)) {
    rc = cai_todo_next_item_sequence(ctx, txn, &board, &sequence, error);
  }
  if (rc == CAI_OK && !(result->has_ok && !result->ok)) {
    rc = cai_todo_format_item_id(board.key, sequence, item_ref,
                                 sizeof(item_ref), error);
  }
  if (rc == CAI_OK && !(result->has_ok && !result->ok)) {
    rc = cai_todo_set_item(&state.item, id, item_ref, board.id, board.key,
                           board.name, args->title, args->description, status,
                           (long long)time(NULL), error);
  }
  if (rc == CAI_OK && !(result->has_ok && !result->ok)) {
    options.append = cai_todo_append_item_cb;
    options.user = &state;
    rc = cai_todo_rewrite(ctx, txn, "items", &options, error);
  }
  if (rc == CAI_OK && !(result->has_ok && !result->ok)) {
    memset(&options, 0, sizeof(options));
    state.ctx = ctx;
    state.args = args;
    state.next_sequence = sequence + 1LL;
    state.now_unix = (long long)time(NULL);
    options.item_map = &cai_todo_board_map;
    options.item_dst = &state.board;
    options.item = cai_todo_update_board_cb;
    options.user = &state;
    rc = cai_todo_rewrite(ctx, txn, "boards", &options, error);
    cai_todo_board_cleanup(&state.replacement_board);
  }
  if (rc == CAI_OK && !(result->has_ok && !result->ok)) {
    rc = cai_todo_store_commit(ctx, txn, error);
    txn = NULL;
  }
  if (rc == CAI_OK && !(result->has_ok && !result->ok)) {
    rc = cai_todo_set_result(result, args->operation, 1, "ok", "item added",
                             error);
  }
  if (rc == CAI_OK && board_found) {
    rc = cai_todo_copy_string(&result->board_id, board.id, error);
  }
  if (rc == CAI_OK && board_found) {
    rc = cai_todo_copy_string(&result->board_key,
                              board.key != NULL ? board.key : "", error);
  }
  if (rc == CAI_OK && board_found) {
    rc = cai_todo_copy_string(&result->board_name, board.name, error);
  }
  if (rc == CAI_OK && result->ok) {
    rc = cai_todo_copy_string(&result->item_id, item_ref, error);
  }
  if (rc == CAI_OK && board_found) {
    rc = cai_todo_copy_string(&result->status, status, error);
  }
  if (board_found) {
    result->in_process_count =
        in_process +
        (result->ok && cai_todo_streq(status, CAI_TODO_STATUS_IN_PROCESS) ? 1
                                                                          : 0);
    result->has_in_process_count = 1;
    result->item_count = items + (result->ok ? 1 : 0);
    result->has_item_count = 1;
  }
  if (txn != NULL) {
    if (result->has_ok && !result->ok && rc == CAI_OK) {
      rc = cai_todo_store_commit(ctx, txn, error);
      txn = NULL;
    } else {
      cai_todo_store_rollback(ctx, txn);
    }
  }
  cai_todo_item_cleanup(&state.item);
  cai_todo_board_cleanup(&board);
  return rc;
}

static lonejson_status cai_todo_list_item_cb(void *user, void *dst) {
  cai_todo_list_state *state;
  cai_todo_item *item;

  state = (cai_todo_list_state *)user;
  item = (cai_todo_item *)dst;
  if (state->rc != CAI_OK) {
    return LONEJSON_STATUS_OK;
  }
  if (cai_todo_streq(item->board_id, state->board_id) &&
      (!state->only_current ||
       cai_todo_streq(item->status, CAI_TODO_STATUS_IN_PROCESS))) {
    state->rc = cai_todo_add_result_item(state->result, item,
                                         state->board_name, state->max_items,
                                         state->error);
    if (state->rc != CAI_OK) {
      return LONEJSON_STATUS_ALLOCATION_FAILED;
    }
  }
  return LONEJSON_STATUS_OK;
}

static int cai_todo_list(cai_todo_context *ctx, const cai_todo_args *args,
                         cai_todo_result *result, int only_current,
                         cai_error *error) {
  cai_todo_board board;
  cai_todo_list_state list_state;
  long long in_process;
  long long items;
  int rc;

  cai_todo_board_init(&board);
  if (cai_todo_board_arg_is_default(ctx, args)) {
    rc = cai_todo_ensure_default_board_committed(ctx, error);
    if (rc != CAI_OK) {
      cai_todo_board_cleanup(&board);
      return rc;
    }
  }
  rc = cai_todo_set_result(result, args->operation, 1, "ok", "items listed",
                           error);
  if (rc != CAI_OK) {
    cai_todo_board_cleanup(&board);
    return rc;
  }
  rc = cai_todo_find_board(ctx, NULL, args->board_id, args->board_key,
                           cai_todo_arg_string(args->board_name,
                                               ctx->default_board),
                           &board, &in_process, &items, error);
  if (rc == CAI_TODO_FOUND_NONE) {
    cai_todo_board_cleanup(&board);
    return cai_todo_set_result(result, args->operation, 0, "board_not_found",
                               "board was not found", error);
  }
  if (rc != CAI_OK) {
    cai_todo_board_cleanup(&board);
    return rc;
  }
  rc = cai_todo_copy_string(&result->board_id, board.id, error);
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&result->board_key,
                              board.key != NULL ? board.key : "", error);
  }
  if (rc == CAI_OK) {
    rc = cai_todo_copy_string(&result->board_name, board.name, error);
  }
  result->wip_limit = board.wip_limit;
  result->has_wip_limit = board.has_wip_limit && board.wip_limit >= 0;
  result->in_process_count = in_process;
  result->has_in_process_count = 1;
  result->item_count = items;
  result->has_item_count = 1;
  if (rc == CAI_OK) {
    memset(&list_state, 0, sizeof(list_state));
    list_state.result = result;
    list_state.board_id = board.id;
    list_state.board_name = board.name;
    list_state.only_current = only_current;
    list_state.max_items = ctx->max_result_items;
    list_state.error = error;
    list_state.rc = CAI_OK;
    rc = cai_todo_stream_items_path(ctx, NULL, "items", cai_todo_list_item_cb,
                                    &list_state, error);
    if (rc == CAI_OK) {
      rc = list_state.rc;
    }
  }
  cai_todo_board_cleanup(&board);
  return rc;
}

static lonejson_status cai_todo_list_board_cb(void *user, void *dst) {
  cai_todo_board_list_state *state;
  cai_todo_board *board;

  state = (cai_todo_board_list_state *)user;
  board = (cai_todo_board *)dst;
  state->count++;
  if (state->rc == CAI_OK) {
    state->rc = cai_todo_add_result_board(state->result, board,
                                          state->max_items, state->error);
    if (state->rc != CAI_OK) {
      return LONEJSON_STATUS_ALLOCATION_FAILED;
    }
  }
  return LONEJSON_STATUS_OK;
}

static int cai_todo_list_boards(cai_todo_context *ctx,
                                const cai_todo_args *args,
                                cai_todo_result *result, cai_error *error) {
  cai_todo_board_list_state state;
  int rc;

  memset(&state, 0, sizeof(state));
  state.result = result;
  state.max_items = ctx->max_result_items;
  state.error = error;
  state.rc = CAI_OK;
  rc = cai_todo_ensure_default_board_committed(ctx, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_todo_set_result(result, args->operation, 1, "ok", "boards listed",
                           error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_todo_stream_boards(ctx, NULL, cai_todo_list_board_cb, &state, error);
  if (rc == CAI_OK) {
    rc = state.rc;
  }
  result->board_count = state.count;
  result->has_board_count = 1;
  return rc;
}

static int cai_todo_rewrite_move(cai_todo_context *ctx,
                                 const cai_todo_args *args,
                                 cai_todo_result *result, int complete,
                                 cai_error *error) {
  cai_todo_rewrite_state state;
  lonejson_array_rewrite_options options;
  cai_todo_board board;
  long long in_process;
  long long items;
  const char *new_status;
  void *txn;
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
  memset(&state, 0, sizeof(state));
  memset(&options, 0, sizeof(options));
  cai_todo_item_init(&state.append_item);
  cai_todo_item_init(&state.replacement_item);
  cai_todo_board_init(&board);
  txn = NULL;
  rc = cai_todo_store_begin(ctx, &txn, error);
  if (rc != CAI_OK) {
    cai_todo_item_cleanup(&state.append_item);
    cai_todo_item_cleanup(&state.replacement_item);
    cai_todo_board_cleanup(&board);
    return rc;
  }
  rc = cai_todo_find_board(ctx, txn, args->board_id, args->board_key,
                           cai_todo_arg_string(args->board_name,
                                               ctx->default_board),
                           &board, &in_process, &items, error);
  if (rc == CAI_TODO_FOUND_NONE) {
    rc = CAI_OK;
  }
  if (rc == CAI_OK && !complete &&
      cai_todo_streq(new_status, CAI_TODO_STATUS_IN_PROCESS) &&
      board.has_wip_limit && board.wip_limit >= 0 &&
      in_process >= board.wip_limit) {
    rc = cai_todo_set_result(result, args->operation, 0, "wip_limit_exceeded",
                             "board WIP limit would be exceeded", error);
  }
  if (rc == CAI_OK && !(result->has_ok && !result->ok)) {
    state.ctx = ctx;
    state.args = args;
    state.new_status = new_status;
    state.complete = complete;
    state.now_unix = (long long)time(NULL);
    options.item_map = &cai_todo_item_map;
    options.item_dst = &state.item;
    options.item = cai_todo_move_item_cb;
    options.user = &state;
    rc = cai_todo_rewrite(ctx, txn, "items", &options, error);
  }
  if (rc == CAI_OK && !(result->has_ok && !result->ok) && !state.found) {
    rc = cai_todo_set_result(result, args->operation, 0, "item_not_found",
                             "item was not found", error);
  }
  if (rc == CAI_OK && state.found && complete) {
    memset(&options, 0, sizeof(options));
    options.append = cai_todo_append_done_cb;
    options.user = &state;
    rc = cai_todo_rewrite(ctx, txn, "done", &options, error);
  }
  if (rc == CAI_OK && state.found) {
    rc = cai_todo_store_commit(ctx, txn, error);
    txn = NULL;
  }
  if (rc == CAI_OK && state.found) {
    rc = cai_todo_set_result(result, args->operation, 1, "ok",
                             complete ? "item completed" : "item moved",
                             error);
  }
  if (rc == CAI_OK && state.found) {
    rc = cai_todo_copy_string(
        &result->item_id,
        complete && state.append_item.item_id != NULL
            ? state.append_item.item_id
            : (!complete && state.replacement_item.item_id != NULL
                   ? state.replacement_item.item_id
                   : args->item_id),
        error);
  }
  if (rc == CAI_OK && state.found && board.id != NULL) {
    rc = cai_todo_copy_string(&result->board_id, board.id, error);
  }
  if (rc == CAI_OK && state.found && board.key != NULL) {
    rc = cai_todo_copy_string(&result->board_key, board.key, error);
  }
  if (rc == CAI_OK && state.found && !complete) {
    rc = cai_todo_copy_string(&result->status, new_status, error);
  }
  if (txn != NULL) {
    if (result->has_ok && !result->ok && rc == CAI_OK) {
      rc = cai_todo_store_commit(ctx, txn, error);
      txn = NULL;
    } else {
      cai_todo_store_rollback(ctx, txn);
    }
  }
  cai_todo_item_cleanup(&state.replacement_item);
  cai_todo_item_cleanup(&state.append_item);
  cai_todo_board_cleanup(&board);
  return rc;
}

static int cai_todo_set_wip_limit(cai_todo_context *ctx,
                                  const cai_todo_args *args,
                                  cai_todo_result *result, cai_error *error) {
  cai_todo_rewrite_state state;
  lonejson_array_rewrite_options options;
  void *txn;
  int rc;

  if (!args->has_wip_limit || args->wip_limit < 0) {
    return cai_todo_set_result(result, args->operation, 0, "invalid_request",
                               "non-negative wip_limit is required", error);
  }
  memset(&state, 0, sizeof(state));
  memset(&options, 0, sizeof(options));
  txn = NULL;
  rc = cai_todo_store_begin(ctx, &txn, error);
  if (rc == CAI_OK && cai_todo_board_arg_is_default(ctx, args)) {
    long long in_process;
    long long items;

    in_process = 0;
    items = 0;
    cai_todo_board_init(&state.board);
    rc = cai_todo_ensure_board(ctx, txn, ctx->default_board, &state.board,
                               &in_process, &items, -1, 0, NULL, NULL, error);
    cai_todo_board_cleanup(&state.board);
  }
  if (rc == CAI_OK) {
    state.ctx = ctx;
    state.args = args;
    state.now_unix = (long long)time(NULL);
    options.item_map = &cai_todo_board_map;
    options.item_dst = &state.board;
    options.item = cai_todo_update_board_cb;
    options.user = &state;
    rc = cai_todo_rewrite(ctx, txn, "boards", &options, error);
    cai_todo_board_cleanup(&state.replacement_board);
  }
  if (rc == CAI_OK && !state.found) {
    rc = cai_todo_set_result(result, args->operation, 0, "board_not_found",
                             "board was not found", error);
  }
  if (rc == CAI_OK && state.found) {
    rc = cai_todo_store_commit(ctx, txn, error);
    txn = NULL;
  }
  if (rc == CAI_OK && state.found) {
    rc = cai_todo_set_result(result, args->operation, 1, "ok",
                             "WIP limit updated", error);
  }
  if (rc == CAI_OK && state.found) {
    result->wip_limit = args->wip_limit;
    result->has_wip_limit = 1;
  }
  if (txn != NULL) {
    if (result->has_ok && !result->ok && rc == CAI_OK) {
      rc = cai_todo_store_commit(ctx, txn, error);
      txn = NULL;
    } else {
      cai_todo_store_rollback(ctx, txn);
    }
  }
  return rc;
}

static int cai_todo_help(const cai_todo_args *args, cai_todo_result *result,
                         cai_error *error) {
  int rc;

  rc = cai_todo_set_result(result, args->operation, 1, "ok",
                           CAI_TODO_HELP_TEXT, error);
  if (rc == CAI_OK) {
    result->item_count = 0;
    result->has_item_count = 1;
  }
  return rc;
}

static int cai_todo_schema_new(cai_tool_schema **out, cai_error *error) {
  static const char *const operations[] = {
      "help",        "create_board", "list_boards",  "set_wip_limit",
      "add_item",    "list_board",   "current_work", "move_item",
      "complete_item"};
  cai_tool_schema *schema;
  int rc;

  schema = NULL;
  rc = cai_tool_schema_from_map(&cai_todo_args_map, &schema, error);
  if (rc == CAI_OK) {
    rc = schema->string_enum(
        schema, "operation",
        "Required operation. Use help first when unsure. help returns a usage "
        "guide. The configured default board always exists and is used when "
        "board_id/board_key/board_name are omitted. create_board creates or "
        "returns a board. list_boards discovers boards, IDs, and keys. "
        "set_wip_limit "
        "configures in_process concurrency. add_item adds active work. "
        "list_board lists all active work. current_work lists only "
        "in_process work. move_item moves an item between todo and "
        "in_process. complete_item archives an item to done.",
        operations, sizeof(operations) / sizeof(operations[0]), 1, error);
  }
  if (rc == CAI_OK) {
    rc = schema->describe(
        schema, "board_id",
        "Opaque board ID returned by create_board or list_boards. Prefer this "
        "over board_name for exact follow-up board calls.",
        error);
  }
  if (rc == CAI_OK) {
    rc = schema->describe(
        schema, "board_key",
        "Short unique board key such as DEF or OPS. Optional for "
        "create_board; cai derives a unique key when omitted. The default "
        "board key is DEF. Can be changed by calling create_board for an "
        "existing board with a new unique board_key. Item sequence numbers "
        "cannot be set by callers.",
        error);
  }
  if (rc == CAI_OK) {
    rc = schema->describe(
        schema, "board_name",
        "Human board name. Used to create or find a board when board_id is not "
        "available. Omit board_id, board_key, and board_name to use the "
        "configured default board; cai creates it lazily if needed.",
        error);
  }
  if (rc == CAI_OK) {
    rc = schema->describe(
        schema, "item_id",
        "Readable board-key sequence reference such as DEF-001 returned by "
        "add_item, list_board, or current_work. Required for move_item and "
        "complete_item. Input is lenient: DEF-001, DEF#1, DEF001, and DEF1 "
        "refer to the same item.",
        error);
  }
  if (rc == CAI_OK) {
    rc = schema->describe(schema, "title",
                          "Short task title. Required for add_item.", error);
  }
  if (rc == CAI_OK) {
    rc = schema->describe(
        schema, "description",
        "Optional task detail. Keep concise unless the work item needs enough "
        "context to be useful later.",
        error);
  }
  if (rc == CAI_OK) {
    rc = schema->raw_property(
        schema, "status",
        "Active lane status. For add_item and move_item use todo or "
        "in_process. complete_item does not use status; it moves the item to "
        "the done archive.",
        "{\"type\":\"string\",\"enum\":[\"todo\",\"in_process\"]}", 0, error);
  }
  if (rc == CAI_OK) {
    rc = schema->raw_property(
        schema, "wip_limit",
        "Non-negative limit for the board's in_process lane. Required for "
        "set_wip_limit; optional for create_board. When reached, moving or "
        "adding more in_process work returns ok=false with "
        "code=wip_limit_exceeded.",
        "{\"type\":\"integer\"}", 0, error);
  }
  if (rc != CAI_OK) {
    cai_tool_schema_destroy(schema);
    return rc;
  }
  *out = schema;
  return CAI_OK;
}

static int cai_todo_run(void *context, const void *params, void *out,
                        cai_error *error) {
  cai_todo_context *ctx;
  const cai_todo_args *args;
  cai_todo_args normalized_args;
  cai_todo_result *result;
  char normalized_board_key[16];
  int rc;

  ctx = (cai_todo_context *)context;
  args = (const cai_todo_args *)params;
  result = (cai_todo_result *)out;
  if (args->board_key != NULL && args->board_key[0] != '\0') {
    rc = cai_todo_normalize_board_key(args->board_key, normalized_board_key,
                                      sizeof(normalized_board_key), error);
    if (rc != CAI_OK) {
      return rc;
    }
    normalized_args = *args;
    normalized_args.board_key = normalized_board_key;
    args = &normalized_args;
  }
  if (cai_todo_streq(args->operation, "help")) {
    return cai_todo_help(args, result, error);
  }
  rc = cai_todo_backfill_board_keys(ctx, error);
  if (rc != CAI_OK) {
    return rc;
  }
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
  cai_tool_schema *schema;
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
      config != NULL ? config->description : NULL, CAI_TODO_DESCRIPTION);
  schema = NULL;
  rc = cai_todo_schema_new(&schema, error);
  if (rc == CAI_OK) {
    rc = cai_tool_registry_register_lonejson_schema_owned(
        registry, name, description, cai_tool_schema_json(schema),
        cai_tool_schema_strict(schema), &cai_todo_args_map,
        &cai_todo_result_map, cai_todo_run, ctx, cai_todo_context_cleanup,
        error);
  }
  cai_tool_schema_destroy(schema);
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
