#include "../cai_internal.h"

#include <cai/tools/patch.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

extern char *realpath(const char *path, char *resolved_path);

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define CAI_PATCH_DEFAULT_MAX_PATCH_BYTES (1024U * 1024U)
#define CAI_PATCH_DEFAULT_MAX_FILE_BYTES (8U * 1024U * 1024U)
#define CAI_PATCH_MAX_CHANGES 1024U
#define CAI_PATCH_ADD 1
#define CAI_PATCH_DELETE 2
#define CAI_PATCH_UPDATE 3

typedef struct cai_patch_context {
  char *root_path;
  size_t max_patch_bytes;
  size_t max_file_bytes;
} cai_patch_context;

typedef struct cai_patch_hunk {
  char *old_text;
  size_t old_length;
  char *new_text;
  size_t new_length;
  char *context;
  int end_of_file;
} cai_patch_hunk;

typedef struct cai_patch_change {
  int kind;
  char *path;
  char *move_path;
  char *new_text;
  size_t new_length;
  cai_patch_hunk *hunks;
  size_t hunk_count;
  size_t hunk_capacity;
  char *resolved_path;
  char *resolved_move_path;
  int primary_parent_fd;
  char primary_name[PATH_MAX];
  int move_parent_fd;
  char move_name[PATH_MAX];
  char *before;
  size_t before_length;
  char *after;
  size_t after_length;
  size_t search_offset;
} cai_patch_change;

typedef struct cai_patch_plan {
  cai_patch_change *items;
  size_t count;
  size_t capacity;
} cai_patch_plan;

typedef struct cai_patch_spooled_reader {
  lonejson_spooled cursor;
  unsigned char buffer[4096];
  size_t offset;
  size_t length;
  char *line;
  size_t line_length;
  size_t line_capacity;
} cai_patch_spooled_reader;

static const char cai_patch_default_description[] =
    "The `apply_patch` tool can be used to edit files. This is a FREEFORM "
    "tool, so do not wrap the patch in JSON.";

static const char cai_patch_lark_grammar_first[] =
    "start: begin_patch hunk+ end_patch\n"
    "begin_patch: \"*** Begin Patch\" LF\n"
    "end_patch: \"*** End Patch\" LF?\n\n"
    "hunk: add_hunk | delete_hunk | update_hunk\n"
    "add_hunk: \"*** Add File: \" filename LF add_line+\n"
    "delete_hunk: \"*** Delete File: \" filename LF\n"
    "update_hunk: \"*** Update File: \" filename LF change_move? change?\n\n"
    "filename: /(.+)/\n";

static const char cai_patch_lark_grammar_second[] =
    "add_line: \"+\" /(.*)/ LF -> line\n\n"
    "change_move: \"*** Move to: \" filename LF\n"
    "change: (change_context | change_line)+ eof_line?\n"
    "change_context: (\"@@\" | \"@@ \" /(.+)/) LF\n"
    "change_line: (\"+\" | \"-\" | \" \") /(.*)/ LF\n"
    "eof_line: \"*** End of File\" LF\n\n"
    "%import common.LF\n";

static int cai_patch_size_add(size_t left, size_t right, size_t *out) {
  if (left > SIZE_MAX - right) {
    return 0;
  }
  *out = left + right;
  return 1;
}

static int cai_patch_size_multiply(size_t left, size_t right, size_t *out) {
  if (left != 0U && right > SIZE_MAX / left) {
    return 0;
  }
  *out = left * right;
  return 1;
}

static void cai_patch_grow_capacity(size_t current, size_t required,
                                    size_t initial, size_t *out) {
  size_t capacity;

  capacity = current == 0U ? initial : current;
  while (capacity < required) {
    if (capacity > SIZE_MAX / 2U) {
      capacity = required;
      break;
    }
    capacity *= 2U;
  }
  *out = capacity;
}

static int cai_patch_copy(char **out, const char *value, size_t length,
                          cai_error *error) {
  char *copy;
  size_t allocation_size;

  *out = NULL;
  if (!cai_patch_size_add(length, 1U, &allocation_size)) {
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "patch data exceeds addressable size");
  }
  copy = (char *)cai_alloc(NULL, allocation_size);
  if (copy == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate patch data");
  }
  if (length > 0U) {
    memcpy(copy, value, length);
  }
  copy[length] = '\0';
  *out = copy;
  return CAI_OK;
}

static void cai_patch_change_cleanup(cai_patch_change *change) {
  if (change == NULL) {
    return;
  }
  cai_free_mem(NULL, change->path);
  cai_free_mem(NULL, change->move_path);
  cai_free_mem(NULL, change->new_text);
  if (change->hunks != NULL) {
    size_t i;

    for (i = 0U; i < change->hunk_count; i++) {
      cai_free_mem(NULL, change->hunks[i].old_text);
      cai_free_mem(NULL, change->hunks[i].new_text);
      cai_free_mem(NULL, change->hunks[i].context);
    }
  }
  cai_free_mem(NULL, change->hunks);
  cai_free_mem(NULL, change->resolved_path);
  cai_free_mem(NULL, change->resolved_move_path);
  if (change->primary_parent_fd >= 0) {
    close(change->primary_parent_fd);
  }
  if (change->move_parent_fd >= 0) {
    close(change->move_parent_fd);
  }
  cai_free_mem(NULL, change->before);
  cai_free_mem(NULL, change->after);
  memset(change, 0, sizeof(*change));
}

static int cai_patch_change_append_hunk(cai_patch_change *change,
                                        cai_patch_hunk **out,
                                        cai_error *error) {
  cai_patch_hunk *hunks;
  size_t capacity;
  size_t allocation_size;
  size_t required;

  if (change->hunk_count == change->hunk_capacity) {
    if (!cai_patch_size_add(change->hunk_count, 1U, &required)) {
      return cai_set_error(error, CAI_ERR_LIMIT,
                           "patch hunk count exceeds addressable size");
    }
    cai_patch_grow_capacity(change->hunk_capacity, required, 2U, &capacity);
    if (!cai_patch_size_multiply(capacity, sizeof(*hunks), &allocation_size)) {
      return cai_set_error(error, CAI_ERR_LIMIT,
                           "patch hunk allocation exceeds addressable size");
    }
    hunks = (cai_patch_hunk *)cai_alloc(NULL, allocation_size);
    if (hunks == NULL) {
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate patch hunk");
    }
    if (change->hunk_count > 0U) {
      memcpy(hunks, change->hunks, change->hunk_count * sizeof(*hunks));
    }
    cai_free_mem(NULL, change->hunks);
    change->hunks = hunks;
    change->hunk_capacity = capacity;
  }
  memset(&change->hunks[change->hunk_count], 0,
         sizeof(change->hunks[change->hunk_count]));
  *out = &change->hunks[change->hunk_count++];
  return CAI_OK;
}

static void cai_patch_plan_cleanup(cai_patch_plan *plan) {
  size_t i;

  if (plan == NULL) {
    return;
  }
  for (i = 0U; i < plan->count; i++) {
    cai_patch_change_cleanup(&plan->items[i]);
  }
  cai_free_mem(NULL, plan->items);
  memset(plan, 0, sizeof(*plan));
}

static int cai_patch_plan_append(cai_patch_plan *plan, cai_patch_change **out,
                                 cai_error *error) {
  cai_patch_change *items;
  size_t capacity;
  size_t allocation_size;
  size_t required;

  if (plan->count >= CAI_PATCH_MAX_CHANGES) {
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "patch exceeds the maximum changed-file count");
  }
  if (plan->count == plan->capacity) {
    if (!cai_patch_size_add(plan->count, 1U, &required)) {
      return cai_set_error(error, CAI_ERR_LIMIT,
                           "patch change count exceeds addressable size");
    }
    cai_patch_grow_capacity(plan->capacity, required, 4U, &capacity);
    if (!cai_patch_size_multiply(capacity, sizeof(*items), &allocation_size)) {
      return cai_set_error(error, CAI_ERR_LIMIT,
                           "patch plan allocation exceeds addressable size");
    }
    items = (cai_patch_change *)cai_alloc(NULL, allocation_size);
    if (items == NULL) {
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate patch plan");
    }
    if (plan->count > 0U) {
      memcpy(items, plan->items, plan->count * sizeof(*items));
    }
    cai_free_mem(NULL, plan->items);
    plan->items = items;
    plan->capacity = capacity;
  }
  memset(&plan->items[plan->count], 0, sizeof(plan->items[plan->count]));
  plan->items[plan->count].primary_parent_fd = -1;
  plan->items[plan->count].move_parent_fd = -1;
  *out = &plan->items[plan->count++];
  return CAI_OK;
}

static int cai_patch_under_root(const char *root, const char *path) {
  size_t length;

  if (root == NULL || path == NULL) {
    return 0;
  }
  if (strcmp(root, "/") == 0) {
    return path[0] == '/' ? 1 : 0;
  }
  length = strlen(root);
  return strncmp(root, path, length) == 0 &&
                 (path[length] == '\0' || path[length] == '/')
             ? 1
             : 0;
}

static int cai_patch_realpath_copy(const char *path, char **out,
                                   cai_error *error) {
  char resolved[PATH_MAX];

  if (realpath(path, resolved) == NULL) {
    return cai_set_error_detail(error, CAI_ERR_INVALID,
                                "failed to resolve patch path",
                                strerror(errno));
  }
  return cai_patch_copy(out, resolved, strlen(resolved), error);
}

static int cai_patch_validate_relative_path(const char *path,
                                            cai_error *error) {
  const char *segment;
  const char *next;

  if (path == NULL || path[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "patch path is required");
  }
  if (strncmp(path, "./", 2U) == 0 || strstr(path, "//") != NULL ||
      strstr(path, "/./") != NULL ||
      (strlen(path) >= 2U && strcmp(path + strlen(path) - 2U, "/.") == 0) ||
      strcmp(path, ".") == 0 || strcmp(path, "..") == 0 ||
      strncmp(path, "../", 3U) == 0 || strstr(path, "/../") != NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "patch path contains an unsafe segment");
  }
  segment = path;
  while (segment[0] != '\0') {
    next = strchr(segment, '/');
    if ((next == NULL && strcmp(segment, "..") == 0) ||
        (next != NULL && next == segment + 2U && segment[0] == '.' &&
         segment[1] == '.')) {
      return cai_set_error(error, CAI_ERR_INVALID,
                           "patch path contains an unsafe segment");
    }
    if (next == NULL) {
      break;
    }
    segment = next + 1U;
  }
  return CAI_OK;
}

static int cai_patch_join(char *out, size_t out_size, const char *root,
                          const char *path, cai_error *error) {
  int written;

  if (path[0] == '/') {
    written = snprintf(out, out_size, "%s", path);
  } else {
    if (cai_patch_validate_relative_path(path, error) != CAI_OK) {
      return error != NULL ? error->code : CAI_ERR_INVALID;
    }
    written = snprintf(out, out_size, "%s/%s", root, path);
  }
  if (written < 0 || (size_t)written >= out_size) {
    return cai_set_error(error, CAI_ERR_INVALID, "patch path is too long");
  }
  return CAI_OK;
}

static int cai_patch_resolve_existing(const cai_patch_context *ctx,
                                      const char *path, char **out,
                                      cai_error *error) {
  char candidate[PATH_MAX];
  struct stat st;
  int rc;

  rc =
      cai_patch_join(candidate, sizeof(candidate), ctx->root_path, path, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_patch_realpath_copy(candidate, out, error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (!cai_patch_under_root(ctx->root_path, *out)) {
    cai_free_mem(NULL, *out);
    *out = NULL;
    return cai_set_error(error, CAI_ERR_INVALID,
                         "patch path escapes configured root");
  }
  if (stat(*out, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink > 1) {
    cai_free_mem(NULL, *out);
    *out = NULL;
    return cai_set_error(error, CAI_ERR_INVALID,
                         "patch path must be a single-linked regular file");
  }
  return CAI_OK;
}

static int cai_patch_resolve_new(const cai_patch_context *ctx, const char *path,
                                 char **out, cai_error *error) {
  char candidate[PATH_MAX];
  char parent[PATH_MAX];
  char resolved_parent[PATH_MAX];
  char *slash;
  int rc;

  rc =
      cai_patch_join(candidate, sizeof(candidate), ctx->root_path, path, error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (access(candidate, F_OK) == 0) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "patch add or move target already exists");
  }
  if (errno != ENOENT) {
    return cai_set_error_detail(error, CAI_ERR_INVALID,
                                "failed to inspect patch target",
                                strerror(errno));
  }
  if (snprintf(parent, sizeof(parent), "%s", candidate) >=
      (int)sizeof(parent)) {
    return cai_set_error(error, CAI_ERR_INVALID, "patch path is too long");
  }
  slash = strrchr(parent, '/');
  if (slash == NULL || slash == parent) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "patch target must have a workspace parent");
  }
  *slash = '\0';
  if (realpath(parent, resolved_parent) == NULL ||
      !cai_patch_under_root(ctx->root_path, resolved_parent)) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "patch target parent escapes configured root");
  }
  return cai_patch_copy(out, candidate, strlen(candidate), error);
}

static int cai_patch_read_file(const char *path, size_t maximum, char **out,
                               size_t *out_length, cai_error *error) {
  struct stat st;
  char *data;
  size_t offset;
  size_t allocation_size;
  ssize_t nread;
  int fd;

  *out = NULL;
  *out_length = 0U;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
      (size_t)st.st_size > maximum) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "patch file is not a permitted size regular file");
  }
  if (!cai_patch_size_add((size_t)st.st_size, 1U, &allocation_size)) {
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "patch file exceeds addressable size");
  }
  data = (char *)cai_alloc(NULL, allocation_size);
  if (data == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate patch file");
  }
  fd = open(path, O_RDONLY);
  if (fd < 0) {
    cai_free_mem(NULL, data);
    return cai_set_error_detail(error, CAI_ERR_INVALID,
                                "failed to open patch file", strerror(errno));
  }
  offset = 0U;
  while (offset < (size_t)st.st_size) {
    nread = read(fd, data + offset, (size_t)st.st_size - offset);
    if (nread <= 0) {
      close(fd);
      cai_free_mem(NULL, data);
      return cai_set_error_detail(error, CAI_ERR_INVALID,
                                  "failed to read patch file", strerror(errno));
    }
    offset += (size_t)nread;
  }
  close(fd);
  data[offset] = '\0';
  *out = data;
  *out_length = offset;
  return CAI_OK;
}

/* Keep an opened parent directory from preflight through publication so an
 * attacker cannot redirect a later path-based write by swapping a directory
 * for a symlink. */
static int cai_patch_open_parent(const char *path, int *out_fd, char *name,
                                 size_t name_size, cai_error *error) {
  char parent[PATH_MAX];
  char *slash;
  size_t name_length;
  int fd;

  *out_fd = -1;
  if (path == NULL || name == NULL || name_size == 0U ||
      snprintf(parent, sizeof(parent), "%s", path) >= (int)sizeof(parent)) {
    return cai_set_error(error, CAI_ERR_INVALID, "patch path is too long");
  }
  slash = strrchr(parent, '/');
  if (slash == NULL || slash[1] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "patch target must have a file name");
  }
  name_length = strlen(slash + 1);
  if (name_length + 1U > name_size) {
    return cai_set_error(error, CAI_ERR_INVALID, "patch file name is too long");
  }
  memcpy(name, slash + 1, name_length + 1U);
  if (slash == parent) {
    slash[1] = '\0';
  } else {
    *slash = '\0';
  }
  fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return cai_set_error_detail(error, CAI_ERR_INVALID,
                                "failed to open patch parent directory",
                                strerror(errno));
  }
  *out_fd = fd;
  return CAI_OK;
}

static char *cai_patch_next_line(char **cursor) {
  char *line;
  char *newline;

  if (cursor == NULL || *cursor == NULL || (*cursor)[0] == '\0') {
    return NULL;
  }
  line = *cursor;
  newline = strchr(line, '\n');
  if (newline != NULL) {
    *newline = '\0';
    *cursor = newline + 1U;
  } else {
    *cursor = line + strlen(line);
  }
  if (strlen(line) > 0U && line[strlen(line) - 1U] == '\r') {
    line[strlen(line) - 1U] = '\0';
  }
  return line;
}

static int cai_patch_append_line(cai_buffer_builder *builder, const char *line,
                                 cai_error *error) {
  int rc;

  rc = cai_buffer_append_cstr(builder, line, error);
  if (rc == CAI_OK) {
    rc = cai_buffer_append(builder, "\n", 1U, error);
  }
  return rc;
}

static void cai_patch_spooled_reader_cleanup(cai_patch_spooled_reader *reader) {
  if (reader != NULL) {
    cai_free_mem(NULL, reader->line);
    memset(reader, 0, sizeof(*reader));
  }
}

static int cai_patch_spooled_reader_append(cai_patch_spooled_reader *reader,
                                           const unsigned char *data,
                                           size_t length, cai_error *error) {
  char *line;
  size_t capacity;
  size_t needed;

  if (length > 0U && memchr(data, '\0', length) != NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "streamed patch input contains a NUL byte");
  }
  if (!cai_patch_size_add(reader->line_length, length, &needed) ||
      !cai_patch_size_add(needed, 1U, &needed)) {
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "patch line exceeds addressable size");
  }
  if (needed > reader->line_capacity) {
    cai_patch_grow_capacity(reader->line_capacity, needed, 128U, &capacity);
    if (capacity < needed) {
      return cai_set_error(error, CAI_ERR_LIMIT,
                           "patch line exceeds addressable size");
    }
    line = (char *)cai_alloc(NULL, capacity);
    if (line == NULL) {
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate patch line");
    }
    if (reader->line_length > 0U) {
      memcpy(line, reader->line, reader->line_length);
    }
    cai_free_mem(NULL, reader->line);
    reader->line = line;
    reader->line_capacity = capacity;
  }
  if (length > 0U) {
    memcpy(reader->line + reader->line_length, data, length);
    reader->line_length += length;
  }
  reader->line[reader->line_length] = '\0';
  return CAI_OK;
}

/* Returns 1 with one complete line, 0 at EOF, or -CAI_ERR_* on failure. */
static int cai_patch_spooled_reader_next(cai_patch_spooled_reader *reader,
                                         char **out, cai_error *error) {
  lonejson_read_result chunk;
  size_t start;
  size_t i;
  int rc;

  *out = NULL;
  reader->line_length = 0U;
  if (reader->line != NULL) {
    reader->line[0] = '\0';
  }
  for (;;) {
    if (reader->offset == reader->length) {
      chunk = reader->cursor.read(&reader->cursor, reader->buffer,
                                  sizeof(reader->buffer));
      if (chunk.error_code != 0) {
        return -cai_set_error(error, CAI_ERR_PROTOCOL,
                              "failed to read streamed patch input");
      }
      if (chunk.bytes_read == 0U) {
        if (reader->line_length == 0U) {
          return CAI_OK;
        }
        break;
      }
      reader->offset = 0U;
      reader->length = chunk.bytes_read;
    }
    start = reader->offset;
    for (i = start; i < reader->length && reader->buffer[i] != '\n'; i++) {
    }
    rc = cai_patch_spooled_reader_append(reader, reader->buffer + start,
                                         i - start, error);
    if (rc != CAI_OK) {
      return -rc;
    }
    reader->offset = i;
    if (i < reader->length) {
      reader->offset++;
      break;
    }
  }
  if (reader->line_length > 0U &&
      reader->line[reader->line_length - 1U] == '\r') {
    reader->line[--reader->line_length] = '\0';
  }
  if (reader->line == NULL) {
    rc = cai_patch_spooled_reader_append(reader, NULL, 0U, error);
    if (rc != CAI_OK) {
      return -rc;
    }
  }
  *out = reader->line;
  return 1;
}

static int cai_patch_parse_change(cai_patch_plan *plan, char **cursor,
                                  cai_error *error) {
  cai_patch_change *change;
  cai_buffer_builder old_text;
  cai_buffer_builder new_text;
  cai_patch_hunk *hunk;
  char *line;
  char *path;
  int seen_hunk;
  int rc;

  line = cai_patch_next_line(cursor);
  if (line == NULL || strncmp(line, "*** ", 4U) != 0) {
    return cai_set_error(error, CAI_ERR_INVALID, "expected patch file header");
  }
  rc = cai_patch_plan_append(plan, &change, error);
  if (rc != CAI_OK) {
    return rc;
  }
  path = NULL;
  if (strncmp(line, "*** Add File: ", 14U) == 0) {
    change->kind = CAI_PATCH_ADD;
    path = line + 14U;
  } else if (strncmp(line, "*** Delete File: ", 17U) == 0) {
    change->kind = CAI_PATCH_DELETE;
    path = line + 17U;
  } else if (strncmp(line, "*** Update File: ", 17U) == 0) {
    change->kind = CAI_PATCH_UPDATE;
    path = line + 17U;
  } else {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "unsupported patch file header");
  }
  rc = cai_patch_copy(&change->path, path, strlen(path), error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&old_text, 0, sizeof(old_text));
  memset(&new_text, 0, sizeof(new_text));
  hunk = NULL;
  seen_hunk = 0;
  while ((*cursor)[0] != '\0') {
    if (strncmp(*cursor, "*** End Patch", 13U) == 0 ||
        strncmp(*cursor, "*** Add File: ", 14U) == 0 ||
        strncmp(*cursor, "*** Delete File: ", 17U) == 0 ||
        strncmp(*cursor, "*** Update File: ", 17U) == 0) {
      break;
    }
    line = cai_patch_next_line(cursor);
    if (change->kind == CAI_PATCH_DELETE) {
      cai_free_mem(NULL, old_text.data);
      cai_free_mem(NULL, new_text.data);
      return cai_set_error(error, CAI_ERR_INVALID,
                           "delete patch must not contain hunk content");
    }
    if (change->kind == CAI_PATCH_UPDATE &&
        strncmp(line, "*** Move to: ", 13U) == 0) {
      if (change->move_path != NULL || seen_hunk) {
        cai_free_mem(NULL, old_text.data);
        cai_free_mem(NULL, new_text.data);
        return cai_set_error(error, CAI_ERR_INVALID,
                             "invalid patch move header");
      }
      rc = cai_patch_copy(&change->move_path, line + 13U, strlen(line + 13U),
                          error);
      if (rc != CAI_OK) {
        cai_free_mem(NULL, old_text.data);
        cai_free_mem(NULL, new_text.data);
        return rc;
      }
      continue;
    }
    if (change->kind == CAI_PATCH_ADD) {
      if (line[0] != '+') {
        cai_free_mem(NULL, old_text.data);
        cai_free_mem(NULL, new_text.data);
        return cai_set_error(error, CAI_ERR_INVALID,
                             "add patch content must begin with +");
      }
      rc = cai_patch_append_line(&new_text, line + 1U, error);
      if (rc != CAI_OK) {
        cai_free_mem(NULL, old_text.data);
        cai_free_mem(NULL, new_text.data);
        return rc;
      }
      continue;
    }
    if (strncmp(line, "@@", 2U) == 0 && (line[2] == '\0' || line[2] == ' ')) {
      if (hunk != NULL) {
        hunk->old_text = old_text.data;
        hunk->old_length = old_text.length;
        hunk->new_text = new_text.data;
        hunk->new_length = new_text.length;
        memset(&old_text, 0, sizeof(old_text));
        memset(&new_text, 0, sizeof(new_text));
      }
      rc = cai_patch_change_append_hunk(change, &hunk, error);
      if (rc != CAI_OK) {
        cai_free_mem(NULL, old_text.data);
        cai_free_mem(NULL, new_text.data);
        return rc;
      }
      if (line[2] == ' ') {
        rc =
            cai_patch_copy(&hunk->context, line + 3U, strlen(line + 3U), error);
        if (rc != CAI_OK) {
          cai_free_mem(NULL, old_text.data);
          cai_free_mem(NULL, new_text.data);
          return rc;
        }
      }
      seen_hunk = 1;
      continue;
    }
    if (change->kind == CAI_PATCH_UPDATE && seen_hunk &&
        strcmp(line, "*** End of File") == 0) {
      hunk->end_of_file = 1;
      break;
    }
    if (!seen_hunk || (line[0] != ' ' && line[0] != '+' && line[0] != '-')) {
      cai_free_mem(NULL, old_text.data);
      cai_free_mem(NULL, new_text.data);
      return cai_set_error(error, CAI_ERR_INVALID, "invalid update hunk line");
    }
    if (line[0] != '+') {
      rc = cai_patch_append_line(&old_text, line + 1U, error);
    } else {
      rc = CAI_OK;
    }
    if (rc == CAI_OK && line[0] != '-') {
      rc = cai_patch_append_line(&new_text, line + 1U, error);
    }
    if (rc != CAI_OK) {
      cai_free_mem(NULL, old_text.data);
      cai_free_mem(NULL, new_text.data);
      return rc;
    }
  }
  if (change->kind == CAI_PATCH_UPDATE && !seen_hunk &&
      change->move_path == NULL) {
    cai_free_mem(NULL, old_text.data);
    cai_free_mem(NULL, new_text.data);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "update patch requires a hunk");
  }
  if (hunk != NULL) {
    hunk->old_text = old_text.data;
    hunk->old_length = old_text.length;
    hunk->new_text = new_text.data;
    hunk->new_length = new_text.length;
  } else if (change->kind == CAI_PATCH_ADD) {
    change->new_text = new_text.data;
    change->new_length = new_text.length;
  } else {
    cai_free_mem(NULL, old_text.data);
    cai_free_mem(NULL, new_text.data);
  }
  return CAI_OK;
}

static int cai_patch_parse(const cai_patch_context *ctx, const char *patch,
                           cai_patch_plan *plan, cai_error *error) {
  char *copy;
  char *cursor;
  char *line;
  int rc;

  if (patch == NULL || patch[0] == '\0' ||
      strlen(patch) > ctx->max_patch_bytes) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "patch is empty or exceeds the configured size limit");
  }
  rc = cai_patch_copy(&copy, patch, strlen(patch), error);
  if (rc != CAI_OK) {
    return rc;
  }
  cursor = copy;
  line = cai_patch_next_line(&cursor);
  if (line == NULL || strcmp(line, "*** Begin Patch") != 0) {
    cai_free_mem(NULL, copy);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "patch must begin with *** Begin Patch");
  }
  while (cursor[0] != '\0') {
    if (strncmp(cursor, "*** End Patch", 13U) == 0) {
      line = cai_patch_next_line(&cursor);
      if (line == NULL || strcmp(line, "*** End Patch") != 0 ||
          cursor[0] != '\0') {
        cai_free_mem(NULL, copy);
        return cai_set_error(error, CAI_ERR_INVALID, "invalid patch ending");
      }
      cai_free_mem(NULL, copy);
      return plan->count > 0U ? CAI_OK
                              : cai_set_error(error, CAI_ERR_INVALID,
                                              "patch has no file changes");
    }
    rc = cai_patch_parse_change(plan, &cursor, error);
    if (rc != CAI_OK) {
      cai_free_mem(NULL, copy);
      return rc;
    }
    if (cursor != NULL && strncmp(cursor, "*** ", 4U) != 0) {
      cai_free_mem(NULL, copy);
      return cai_set_error(error, CAI_ERR_INVALID, "invalid patch body");
    }
  }
  cai_free_mem(NULL, copy);
  return cai_set_error(error, CAI_ERR_INVALID,
                       "patch must end with *** End Patch");
}

static int cai_patch_parse_change_spooled(cai_patch_plan *plan,
                                          cai_patch_spooled_reader *reader,
                                          char **line_io, cai_error *error) {
  cai_patch_change *change;
  cai_buffer_builder old_text;
  cai_buffer_builder new_text;
  cai_patch_hunk *hunk;
  char *line;
  char *path;
  int seen_hunk;
  int end_of_file_seen;
  int line_rc;
  int rc;

  line = *line_io;
  if (line == NULL || strncmp(line, "*** ", 4U) != 0) {
    return cai_set_error(error, CAI_ERR_INVALID, "expected patch file header");
  }
  rc = cai_patch_plan_append(plan, &change, error);
  if (rc != CAI_OK) {
    return rc;
  }
  path = NULL;
  if (strncmp(line, "*** Add File: ", 14U) == 0) {
    change->kind = CAI_PATCH_ADD;
    path = line + 14U;
  } else if (strncmp(line, "*** Delete File: ", 17U) == 0) {
    change->kind = CAI_PATCH_DELETE;
    path = line + 17U;
  } else if (strncmp(line, "*** Update File: ", 17U) == 0) {
    change->kind = CAI_PATCH_UPDATE;
    path = line + 17U;
  } else {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "unsupported patch file header");
  }
  rc = cai_patch_copy(&change->path, path, strlen(path), error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&old_text, 0, sizeof(old_text));
  memset(&new_text, 0, sizeof(new_text));
  hunk = NULL;
  seen_hunk = 0;
  end_of_file_seen = 0;
  *line_io = NULL;
  for (;;) {
    line_rc = cai_patch_spooled_reader_next(reader, &line, error);
    if (line_rc < 0) {
      rc = -line_rc;
      goto fail;
    }
    if (line_rc == 0) {
      break;
    }
    if (strncmp(line, "*** End Patch", 13U) == 0 ||
        strncmp(line, "*** Add File: ", 14U) == 0 ||
        strncmp(line, "*** Delete File: ", 17U) == 0 ||
        strncmp(line, "*** Update File: ", 17U) == 0) {
      *line_io = line;
      break;
    }
    if (end_of_file_seen) {
      rc = cai_set_error(error, CAI_ERR_INVALID,
                         "end-of-file marker must end the update hunk");
      goto fail;
    }
    if (change->kind == CAI_PATCH_DELETE) {
      rc = cai_set_error(error, CAI_ERR_INVALID,
                         "delete patch must not contain hunk content");
      goto fail;
    }
    if (change->kind == CAI_PATCH_UPDATE &&
        strncmp(line, "*** Move to: ", 13U) == 0) {
      if (change->move_path != NULL || seen_hunk) {
        rc = cai_set_error(error, CAI_ERR_INVALID, "invalid patch move header");
        goto fail;
      }
      rc = cai_patch_copy(&change->move_path, line + 13U, strlen(line + 13U),
                          error);
      if (rc != CAI_OK) {
        goto fail;
      }
      continue;
    }
    if (change->kind == CAI_PATCH_ADD) {
      if (line[0] != '+') {
        rc = cai_set_error(error, CAI_ERR_INVALID,
                           "add patch content must begin with +");
        goto fail;
      }
      rc = cai_patch_append_line(&new_text, line + 1U, error);
      if (rc != CAI_OK) {
        goto fail;
      }
      continue;
    }
    if (strncmp(line, "@@", 2U) == 0 && (line[2] == '\0' || line[2] == ' ')) {
      if (hunk != NULL) {
        hunk->old_text = old_text.data;
        hunk->old_length = old_text.length;
        hunk->new_text = new_text.data;
        hunk->new_length = new_text.length;
        memset(&old_text, 0, sizeof(old_text));
        memset(&new_text, 0, sizeof(new_text));
      }
      rc = cai_patch_change_append_hunk(change, &hunk, error);
      if (rc != CAI_OK) {
        goto fail;
      }
      if (line[2] == ' ') {
        rc =
            cai_patch_copy(&hunk->context, line + 3U, strlen(line + 3U), error);
        if (rc != CAI_OK) {
          goto fail;
        }
      }
      seen_hunk = 1;
      continue;
    }
    if (change->kind == CAI_PATCH_UPDATE && seen_hunk &&
        strcmp(line, "*** End of File") == 0) {
      hunk->end_of_file = 1;
      end_of_file_seen = 1;
      continue;
    }
    if (!seen_hunk || (line[0] != ' ' && line[0] != '+' && line[0] != '-')) {
      rc = cai_set_error(error, CAI_ERR_INVALID, "invalid update hunk line");
      goto fail;
    }
    rc = line[0] != '+' ? cai_patch_append_line(&old_text, line + 1U, error)
                        : CAI_OK;
    if (rc == CAI_OK && line[0] != '-') {
      rc = cai_patch_append_line(&new_text, line + 1U, error);
    }
    if (rc != CAI_OK) {
      goto fail;
    }
  }
  if (change->kind == CAI_PATCH_UPDATE && !seen_hunk &&
      change->move_path == NULL) {
    rc = cai_set_error(error, CAI_ERR_INVALID, "update patch requires a hunk");
    goto fail;
  }
  if (hunk != NULL) {
    hunk->old_text = old_text.data;
    hunk->old_length = old_text.length;
    hunk->new_text = new_text.data;
    hunk->new_length = new_text.length;
  } else if (change->kind == CAI_PATCH_ADD) {
    change->new_text = new_text.data;
    change->new_length = new_text.length;
  } else {
    cai_free_mem(NULL, old_text.data);
    cai_free_mem(NULL, new_text.data);
  }
  return CAI_OK;

fail:
  cai_free_mem(NULL, old_text.data);
  cai_free_mem(NULL, new_text.data);
  return rc;
}

static int cai_patch_parse_spooled(const cai_patch_context *ctx,
                                   lonejson_spooled *input,
                                   cai_patch_plan *plan, cai_error *error) {
  cai_patch_spooled_reader reader;
  lonejson_error json_error;
  char *line;
  int rc;

  if (input == NULL || input->size_fn(input) == 0U ||
      input->size_fn(input) > ctx->max_patch_bytes) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "patch is empty or exceeds the configured size limit");
  }
  memset(&reader, 0, sizeof(reader));
  reader.cursor = *input;
  lonejson_error_init(&json_error);
  if (reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to rewind streamed patch input",
                                json_error.message);
  }
  rc = cai_patch_spooled_reader_next(&reader, &line, error);
  if (rc < 0) {
    rc = -rc;
  } else if (rc == 1 && strcmp(line, "*** Begin Patch") == 0) {
    rc = cai_patch_spooled_reader_next(&reader, &line, error);
    if (rc < 0) {
      rc = -rc;
    } else if (rc == 1) {
      rc = CAI_OK;
    }
    while (rc == CAI_OK && line != NULL && strcmp(line, "*** End Patch") != 0) {
      rc = cai_patch_parse_change_spooled(plan, &reader, &line, error);
    }
    if (rc == CAI_OK) {
      if (line == NULL || strcmp(line, "*** End Patch") != 0) {
        rc = cai_set_error(error, CAI_ERR_INVALID, "invalid patch ending");
      } else {
        rc = cai_patch_spooled_reader_next(&reader, &line, error);
        if (rc < 0) {
          rc = -rc;
        } else if (rc == 1) {
          rc = cai_set_error(error, CAI_ERR_INVALID, "invalid patch ending");
        } else if (rc == CAI_OK && plan->count == 0U) {
          rc = cai_set_error(error, CAI_ERR_INVALID,
                             "patch has no file changes");
        }
      }
    }
  } else if (rc == CAI_OK) {
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "patch must begin with *** Begin Patch");
  } else if (rc == 1) {
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "patch must begin with *** Begin Patch");
  }
  cai_patch_spooled_reader_cleanup(&reader);
  return rc;
}

static int cai_patch_context_line_matches(const char *line,
                                          const char *line_end,
                                          const char *context) {
  const char *context_end;

  while (line < line_end && (*line == ' ' || *line == '\t' || *line == '\r')) {
    line++;
  }
  while (line_end > line && (line_end[-1] == ' ' || line_end[-1] == '\t' ||
                             line_end[-1] == '\r')) {
    line_end--;
  }
  context_end = context + strlen(context);
  while (*context == ' ' || *context == '\t' || *context == '\r') {
    context++;
  }
  while (context_end > context &&
         (context_end[-1] == ' ' || context_end[-1] == '\t' ||
          context_end[-1] == '\r')) {
    context_end--;
  }
  return (size_t)(line_end - line) == (size_t)(context_end - context) &&
         memcmp(line, context, (size_t)(context_end - context)) == 0;
}

static char *cai_patch_find_context_line(char *text, size_t offset,
                                         const char *context) {
  char *line;
  char *line_end;

  line = text + offset;
  for (;;) {
    line_end = strchr(line, '\n');
    if (line_end == NULL) {
      return cai_patch_context_line_matches(line, line + strlen(line), context)
                 ? line
                 : NULL;
    }
    if (cai_patch_context_line_matches(line, line_end, context)) {
      return line;
    }
    line = line_end + 1U;
  }
}

static int cai_patch_apply_hunk(cai_patch_change *change,
                                const cai_patch_hunk *hunk, cai_error *error) {
  char *match;
  char *next_match;
  char *anchor;
  char *anchor_end;
  char *replacement;
  size_t prefix;
  size_t suffix;
  size_t length;
  size_t allocation_size;

  if (hunk->old_length == 0U) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "update hunk must include existing context");
  }
  if (hunk->context != NULL && hunk->context[0] != '\0') {
    anchor = cai_patch_find_context_line(change->after, change->search_offset,
                                         hunk->context);
    if (anchor == NULL) {
      return cai_set_error(error, CAI_ERR_INVALID,
                           "update hunk context was not found");
    }
    anchor_end = strchr(anchor, '\n');
    match =
        strstr(anchor_end != NULL ? anchor_end + 1U : anchor + strlen(anchor),
               hunk->old_text);
  } else if (hunk->end_of_file && change->after_length >= hunk->old_length) {
    match = change->after + change->after_length - hunk->old_length;
  } else {
    match = strstr(change->after + change->search_offset, hunk->old_text);
  }
  if (match == NULL || memcmp(match, hunk->old_text, hunk->old_length) != 0 ||
      (hunk->end_of_file &&
       (size_t)(match - change->after) + hunk->old_length !=
           change->after_length)) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "update patch context does not match exactly once");
  }
  if (!hunk->end_of_file &&
      (hunk->context == NULL || hunk->context[0] == '\0')) {
    next_match = strstr(match + 1U, hunk->old_text);
    if (next_match != NULL) {
      return cai_set_error(error, CAI_ERR_INVALID,
                           "update patch context is ambiguous");
    }
  }
  prefix = (size_t)(match - change->after);
  suffix = change->after_length - prefix - hunk->old_length;
  if (!cai_patch_size_add(prefix, hunk->new_length, &length) ||
      !cai_patch_size_add(length, suffix, &length) ||
      !cai_patch_size_add(length, 1U, &allocation_size)) {
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "patched file exceeds addressable size");
  }
  replacement = (char *)cai_alloc(NULL, allocation_size);
  if (replacement == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate patched file");
  }
  memcpy(replacement, change->after, prefix);
  memcpy(replacement + prefix, hunk->new_text, hunk->new_length);
  memcpy(replacement + prefix + hunk->new_length, match + hunk->old_length,
         change->after_length - prefix - hunk->old_length);
  replacement[length] = '\0';
  cai_free_mem(NULL, change->after);
  change->after = replacement;
  change->after_length = length;
  change->search_offset = prefix + hunk->new_length;
  return CAI_OK;
}

static int cai_patch_validate_destinations(const cai_patch_plan *plan,
                                           size_t index, cai_error *error) {
  const cai_patch_change *change;
  const char *target;
  size_t j;

  change = &plan->items[index];
  target = change->resolved_move_path != NULL ? change->resolved_move_path
                                              : change->resolved_path;
  for (j = 0U; j < index; j++) {
    const cai_patch_change *previous;
    const char *previous_target;

    previous = &plan->items[j];
    previous_target = previous->resolved_move_path != NULL
                          ? previous->resolved_move_path
                          : previous->resolved_path;
    if (strcmp(target, previous_target) == 0 ||
        strcmp(target, previous->resolved_path) == 0 ||
        (change->resolved_move_path != NULL &&
         strcmp(change->resolved_path, previous_target) == 0)) {
      return cai_set_error(error, CAI_ERR_INVALID,
                           "patch has colliding destination paths");
    }
  }
  return CAI_OK;
}

static int cai_patch_preflight(const cai_patch_context *ctx,
                               cai_patch_plan *plan, cai_error *error) {
  size_t i;
  size_t j;
  int rc;

  for (i = 0U; i < plan->count; i++) {
    cai_patch_change *change;

    change = &plan->items[i];
    for (j = 0U; j < i; j++) {
      if (strcmp(change->path, plan->items[j].path) == 0) {
        return cai_set_error(error, CAI_ERR_INVALID,
                             "patch changes the same path more than once");
      }
    }
    if (change->kind == CAI_PATCH_ADD) {
      rc = cai_patch_resolve_new(ctx, change->path, &change->resolved_path,
                                 error);
      change->after = change->new_text;
      change->after_length = change->new_length;
      change->new_text = NULL;
      if (rc != CAI_OK || change->after_length > ctx->max_file_bytes) {
        return rc != CAI_OK ? rc
                            : cai_set_error(error, CAI_ERR_INVALID,
                                            "added file exceeds size limit");
      }
      rc = cai_patch_open_parent(change->resolved_path, &change->primary_parent_fd,
                                 change->primary_name,
                                 sizeof(change->primary_name), error);
      if (rc != CAI_OK) {
        return rc;
      }
      rc = cai_patch_validate_destinations(plan, i, error);
      if (rc != CAI_OK) {
        return rc;
      }
      continue;
    }
    rc = cai_patch_resolve_existing(ctx, change->path, &change->resolved_path,
                                    error);
    if (rc != CAI_OK) {
      return rc;
    }
    rc = cai_patch_open_parent(change->resolved_path, &change->primary_parent_fd,
                               change->primary_name,
                               sizeof(change->primary_name), error);
    if (rc != CAI_OK) {
      return rc;
    }
    rc = cai_patch_read_file(change->resolved_path, ctx->max_file_bytes,
                             &change->before, &change->before_length, error);
    if (rc != CAI_OK) {
      return rc;
    }
    if (change->kind == CAI_PATCH_DELETE) {
      rc = cai_patch_validate_destinations(plan, i, error);
      if (rc != CAI_OK) {
        return rc;
      }
      continue;
    }
    if (change->move_path != NULL) {
      rc = cai_patch_resolve_new(ctx, change->move_path,
                                 &change->resolved_move_path, error);
      if (rc != CAI_OK) {
        return rc;
      }
      rc = cai_patch_open_parent(change->resolved_move_path,
                                 &change->move_parent_fd, change->move_name,
                                 sizeof(change->move_name), error);
      if (rc != CAI_OK) {
        return rc;
      }
    }
    rc = cai_patch_copy(&change->after, change->before, change->before_length,
                        error);
    if (rc == CAI_OK) {
      change->after_length = change->before_length;
      for (j = 0U; j < change->hunk_count; j++) {
        rc = cai_patch_apply_hunk(change, &change->hunks[j], error);
        if (rc != CAI_OK) {
          break;
        }
      }
    }
    if (rc != CAI_OK || change->after_length > ctx->max_file_bytes) {
      return rc != CAI_OK ? rc
                          : cai_set_error(error, CAI_ERR_INVALID,
                                          "patched file exceeds size limit");
    }
    rc = cai_patch_validate_destinations(plan, i, error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  return CAI_OK;
}

static int cai_patch_write_atomic(int parent_fd, const char *name,
                                  const char *data,
                                  size_t length, cai_error *error) {
  char temporary[64];
  size_t offset;
  ssize_t nwritten;
  struct stat st;
  mode_t mode;
  int fd;
  unsigned int attempt;

  if (parent_fd < 0 || name == NULL || name[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "patch destination directory is required");
  }
  fd = -1;
  temporary[0] = '\0';
  for (attempt = 0U; attempt < 128U; attempt++) {
    if (snprintf(temporary, sizeof(temporary), ".cai-patch-%ld-%u",
                 (long)getpid(), attempt) >= (int)sizeof(temporary)) {
      return cai_set_error(error, CAI_ERR_INVALID,
                           "patch temporary name is too long");
    }
    fd = openat(parent_fd, temporary,
                O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0 || errno != EEXIST) {
      break;
    }
  }
  if (fd < 0) {
    return cai_set_error_detail(error, CAI_ERR_INVALID,
                                "failed to create patch temporary file",
                                strerror(errno));
  }
  mode = 0644;
  if (fstatat(parent_fd, name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
    mode = st.st_mode & 0777;
  }
  if (fchmod(fd, mode) != 0) {
    close(fd);
    unlinkat(parent_fd, temporary, 0);
    return cai_set_error_detail(error, CAI_ERR_INVALID,
                                "failed to set patch file mode",
                                strerror(errno));
  }
  offset = 0U;
  while (offset < length) {
    nwritten = write(fd, data + offset, length - offset);
    if (nwritten <= 0) {
      close(fd);
      unlinkat(parent_fd, temporary, 0);
      return cai_set_error_detail(error, CAI_ERR_INVALID,
                                  "failed to write patch file",
                                  strerror(errno));
    }
    offset += (size_t)nwritten;
  }
  if (fsync(fd) != 0 || close(fd) != 0 ||
      renameat(parent_fd, temporary, parent_fd, name) != 0) {
    unlinkat(parent_fd, temporary, 0);
    return cai_set_error_detail(error, CAI_ERR_INVALID,
                                "failed to publish patched file",
                                strerror(errno));
  }
  return CAI_OK;
}

static int cai_patch_commit(cai_patch_plan *plan, cai_error *error) {
  size_t i;
  int rc;

  for (i = 0U; i < plan->count; i++) {
    cai_patch_change *change;
    int target_parent_fd;
    const char *target_name;

    change = &plan->items[i];
    if (change->kind == CAI_PATCH_DELETE) {
      continue;
    }
    target_parent_fd = change->resolved_move_path != NULL
                           ? change->move_parent_fd
                           : change->primary_parent_fd;
    target_name = change->resolved_move_path != NULL ? change->move_name
                                                      : change->primary_name;
    rc = cai_patch_write_atomic(target_parent_fd, target_name, change->after,
                                change->after_length, error);
    if (rc != CAI_OK) {
      goto rollback;
    }
  }
  for (i = 0U; i < plan->count; i++) {
    cai_patch_change *change;

    change = &plan->items[i];
    if (change->kind == CAI_PATCH_DELETE ||
        change->resolved_move_path != NULL) {
      if (unlinkat(change->primary_parent_fd, change->primary_name, 0) != 0) {
        rc = cai_set_error_detail(error, CAI_ERR_INVALID,
                                  "failed to remove patched file",
                                  strerror(errno));
        goto rollback;
      }
    }
  }
  return CAI_OK;

rollback:
  for (i = 0U; i < plan->count; i++) {
    cai_patch_change *change;

    change = &plan->items[i];
    if (change->kind == CAI_PATCH_ADD) {
      (void)unlinkat(change->primary_parent_fd, change->primary_name, 0);
      continue;
    }
    if (change->before != NULL) {
      cai_error ignored;

      cai_error_init(&ignored);
      (void)cai_patch_write_atomic(change->primary_parent_fd,
                                   change->primary_name, change->before,
                                   change->before_length, &ignored);
      cai_error_cleanup(&ignored);
    }
    if (change->resolved_move_path != NULL) {
      (void)unlinkat(change->move_parent_fd, change->move_name, 0);
    }
  }
  return rc;
}

static int cai_patch_write_result(cai_sink *result, const cai_patch_plan *plan,
                                  cai_error *error) {
  cai_buffer_builder builder;
  size_t i;
  int rc;

  if (result == NULL) {
    return CAI_OK;
  }
  memset(&builder, 0, sizeof(builder));
  rc = cai_buffer_append_cstr(&builder,
                              "Success. Updated the following files:\n", error);
  for (i = 0U; rc == CAI_OK && i < plan->count; i++) {
    if (plan->items[i].kind == CAI_PATCH_ADD) {
      rc = cai_buffer_append_cstr(&builder, "A ", error);
      if (rc == CAI_OK) {
        rc = cai_buffer_append_cstr(&builder, plan->items[i].path, error);
      }
      if (rc == CAI_OK) {
        rc = cai_buffer_append_cstr(&builder, "\n", error);
      }
    }
  }
  for (i = 0U; rc == CAI_OK && i < plan->count; i++) {
    if (plan->items[i].kind == CAI_PATCH_UPDATE) {
      rc = cai_buffer_append_cstr(&builder, "M ", error);
      if (rc == CAI_OK) {
        rc = cai_buffer_append_cstr(&builder,
                                    plan->items[i].move_path != NULL
                                        ? plan->items[i].move_path
                                        : plan->items[i].path,
                                    error);
      }
      if (rc == CAI_OK) {
        rc = cai_buffer_append_cstr(&builder, "\n", error);
      }
    }
  }
  for (i = 0U; rc == CAI_OK && i < plan->count; i++) {
    if (plan->items[i].kind == CAI_PATCH_DELETE) {
      rc = cai_buffer_append_cstr(&builder, "D ", error);
      if (rc == CAI_OK) {
        rc = cai_buffer_append_cstr(&builder, plan->items[i].path, error);
      }
      if (rc == CAI_OK) {
        rc = cai_buffer_append_cstr(&builder, "\n", error);
      }
    }
  }
  if (rc == CAI_OK) {
    rc = cai_sink_write(result, builder.data, builder.length, error);
  }
  cai_free_mem(NULL, builder.data);
  return rc;
}

static int cai_patch_context_new(const cai_patch_tool_config *config,
                                 cai_patch_context **out, cai_error *error) {
  cai_patch_context *ctx;
  char *root;
  int rc;

  *out = NULL;
  if (config == NULL || config->root_path == NULL ||
      config->root_path[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "patch tool root_path is required");
  }
  root = NULL;
  rc = cai_patch_realpath_copy(config->root_path, &root, error);
  if (rc != CAI_OK) {
    return rc;
  }
  ctx = (cai_patch_context *)cai_alloc(NULL, sizeof(*ctx));
  if (ctx == NULL) {
    cai_free_mem(NULL, root);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate patch context");
  }
  memset(ctx, 0, sizeof(*ctx));
  ctx->root_path = root;
  ctx->max_patch_bytes = config->max_patch_bytes != 0U
                             ? config->max_patch_bytes
                             : CAI_PATCH_DEFAULT_MAX_PATCH_BYTES;
  ctx->max_file_bytes = config->max_file_bytes != 0U
                            ? config->max_file_bytes
                            : CAI_PATCH_DEFAULT_MAX_FILE_BYTES;
  *out = ctx;
  return CAI_OK;
}

static void cai_patch_context_cleanup(void *context) {
  cai_patch_context *ctx;

  ctx = (cai_patch_context *)context;
  if (ctx == NULL) {
    return;
  }
  cai_free_mem(NULL, ctx->root_path);
  cai_free_mem(NULL, ctx);
}

static int cai_patch_apply_context(const cai_patch_context *ctx,
                                   const char *patch, cai_sink *result,
                                   cai_error *error) {
  cai_patch_plan plan;
  int rc;

  memset(&plan, 0, sizeof(plan));
  rc = cai_patch_parse(ctx, patch, &plan, error);
  if (rc == CAI_OK) {
    rc = cai_patch_preflight(ctx, &plan, error);
  }
  if (rc == CAI_OK) {
    rc = cai_patch_commit(&plan, error);
  }
  if (rc == CAI_OK) {
    rc = cai_patch_write_result(result, &plan, error);
  }
  cai_patch_plan_cleanup(&plan);
  return rc;
}

int cai_apply_patch(const cai_patch_tool_config *config, const char *patch,
                    cai_sink *result, cai_error *error) {
  cai_patch_context *ctx;
  int rc;

  ctx = NULL;
  rc = cai_patch_context_new(config, &ctx, error);
  if (rc == CAI_OK) {
    rc = cai_patch_apply_context(ctx, patch, result, error);
  }
  cai_patch_context_cleanup(ctx);
  return rc;
}

static int cai_patch_tool_spooled_callback(void *context,
                                           lonejson_spooled *input,
                                           cai_sink *result, cai_error *error) {
  cai_patch_context *ctx;
  cai_patch_plan plan;
  int rc;

  ctx = (cai_patch_context *)context;
  if (ctx == NULL || input == NULL || result == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "apply_patch received invalid state");
  }
  memset(&plan, 0, sizeof(plan));
  rc = cai_patch_parse_spooled(ctx, input, &plan, error);
  if (rc == CAI_OK) {
    rc = cai_patch_preflight(ctx, &plan, error);
  }
  if (rc == CAI_OK) {
    rc = cai_patch_commit(&plan, error);
  }
  if (rc == CAI_OK) {
    rc = cai_patch_write_result(result, &plan, error);
  }
  cai_patch_plan_cleanup(&plan);
  return rc;
}

int cai_tool_registry_register_patch_tool(cai_tool_registry *registry,
                                          const cai_patch_tool_config *config,
                                          cai_error *error) {
  cai_patch_context *ctx;
  cai_custom_tool_format format;
  char *grammar;
  size_t grammar_length;
  const char *name;
  const char *description;
  int rc;

  ctx = NULL;
  rc = cai_patch_context_new(config, &ctx, error);
  if (rc != CAI_OK) {
    return rc;
  }
  name = config->name != NULL && config->name[0] != '\0'
             ? config->name
             : CAI_PATCH_DEFAULT_TOOL_NAME;
  description = config->description != NULL && config->description[0] != '\0'
                    ? config->description
                    : cai_patch_default_description;
  format.type = "grammar";
  format.syntax = "lark";
  grammar_length = strlen(cai_patch_lark_grammar_first) +
                   strlen(cai_patch_lark_grammar_second);
  grammar = (char *)cai_alloc(NULL, grammar_length + 1U);
  if (grammar == NULL) {
    cai_patch_context_cleanup(ctx);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate apply_patch grammar");
  }
  memcpy(grammar, cai_patch_lark_grammar_first,
         strlen(cai_patch_lark_grammar_first));
  memcpy(grammar + strlen(cai_patch_lark_grammar_first),
         cai_patch_lark_grammar_second, strlen(cai_patch_lark_grammar_second));
  grammar[grammar_length] = '\0';
  format.definition = grammar;
  rc = cai_tool_registry_register_custom_spooled_owned(
      registry, name, description, &format, cai_patch_tool_spooled_callback,
      ctx, cai_patch_context_cleanup, error);
  cai_free_mem(NULL, grammar);
  if (rc != CAI_OK) {
    cai_patch_context_cleanup(ctx);
  }
  return rc;
}

int cai_agent_register_patch_tool(cai_agent *agent,
                                  const cai_patch_tool_config *config,
                                  cai_error *error) {
  cai_agent_impl *impl;

  if (agent == NULL || agent->impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  impl = CAI_AGENT_IMPL(agent);
  return cai_tool_registry_register_patch_tool(impl->tools, config, error);
}
