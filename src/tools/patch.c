#include "../cai_internal.h"

#include <cai/tools/patch.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
#define CAI_PATCH_ADD 1
#define CAI_PATCH_DELETE 2
#define CAI_PATCH_UPDATE 3

typedef struct cai_patch_context {
  char *root_path;
  size_t max_patch_bytes;
  size_t max_file_bytes;
} cai_patch_context;

typedef struct cai_patch_args {
  char *patch;
} cai_patch_args;

typedef struct cai_patch_result {
  int applied;
  char *summary;
} cai_patch_result;

typedef struct cai_patch_hunk {
  char *old_text;
  size_t old_length;
  char *new_text;
  size_t new_length;
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
  char *before;
  size_t before_length;
  char *after;
  size_t after_length;
} cai_patch_change;

typedef struct cai_patch_plan {
  cai_patch_change *items;
  size_t count;
  size_t capacity;
} cai_patch_plan;

static const lonejson_field cai_patch_arg_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_patch_args, patch, "patch")};
LONEJSON_MAP_DEFINE(cai_patch_args_map, cai_patch_args, cai_patch_arg_fields);

static const lonejson_field cai_patch_result_fields[] = {
    LONEJSON_FIELD_BOOL_REQ(cai_patch_result, applied, "applied"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_patch_result, summary, "summary")};
LONEJSON_MAP_DEFINE(cai_patch_result_map, cai_patch_result,
                    cai_patch_result_fields);

static const char cai_patch_schema_json[] =
    "{\"type\":\"object\",\"properties\":{\"patch\":{\"type\":\"string\","
    "\"description\":\"Codex apply_patch body beginning with *** Begin Patch "
    "and ending with *** End Patch.\"}},\"required\":[\"patch\"],"
    "\"additionalProperties\":false}";

static const char cai_patch_default_description[] =
    "Applies a Codex-style patch inside the configured workspace. The patch "
    "must begin with *** Begin Patch and end with *** End Patch. It supports "
    "*** Add File, *** Delete File, and *** Update File with exact @@ "
    "hunks and optional *** Move to. All paths are validated before writes; "
    "this tool never invokes a shell.";

static int cai_patch_copy(char **out, const char *value, size_t length,
                          cai_error *error) {
  char *copy;

  *out = NULL;
  copy = (char *)cai_alloc(NULL, length + 1U);
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
    }
  }
  cai_free_mem(NULL, change->hunks);
  cai_free_mem(NULL, change->resolved_path);
  cai_free_mem(NULL, change->resolved_move_path);
  cai_free_mem(NULL, change->before);
  cai_free_mem(NULL, change->after);
  memset(change, 0, sizeof(*change));
}

static int cai_patch_change_append_hunk(cai_patch_change *change,
                                        cai_patch_hunk **out,
                                        cai_error *error) {
  cai_patch_hunk *hunks;
  size_t capacity;

  if (change->hunk_count == change->hunk_capacity) {
    capacity = change->hunk_capacity == 0U ? 2U : change->hunk_capacity * 2U;
    hunks = (cai_patch_hunk *)cai_alloc(NULL, capacity * sizeof(*hunks));
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

  if (plan->count == plan->capacity) {
    capacity = plan->capacity == 0U ? 4U : plan->capacity * 2U;
    items = (cai_patch_change *)cai_alloc(NULL, capacity * sizeof(*items));
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
  if (strstr(path, "//") != NULL || strstr(path, "/./") != NULL ||
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
  ssize_t nread;
  int fd;

  *out = NULL;
  *out_length = 0U;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
      (size_t)st.st_size > maximum) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "patch file is not a permitted size regular file");
  }
  data = (char *)cai_alloc(NULL, (size_t)st.st_size + 1U);
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
    if (strcmp(line, "@@") == 0) {
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
      seen_hunk = 1;
      continue;
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

static int cai_patch_apply_hunk(cai_patch_change *change,
                                const cai_patch_hunk *hunk, cai_error *error) {
  char *match;
  char *replacement;
  size_t prefix;
  size_t length;

  if (hunk->old_length == 0U) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "update hunk must include existing context");
  }
  match = strstr(change->after, hunk->old_text);
  if (match == NULL || strstr(match + 1U, hunk->old_text) != NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "update patch context does not match exactly once");
  }
  prefix = (size_t)(match - change->after);
  length = prefix + hunk->new_length +
           (change->after_length - prefix - hunk->old_length);
  replacement = (char *)cai_alloc(NULL, length + 1U);
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
      continue;
    }
    rc = cai_patch_resolve_existing(ctx, change->path, &change->resolved_path,
                                    error);
    if (rc != CAI_OK) {
      return rc;
    }
    rc = cai_patch_read_file(change->resolved_path, ctx->max_file_bytes,
                             &change->before, &change->before_length, error);
    if (rc != CAI_OK) {
      return rc;
    }
    if (change->kind == CAI_PATCH_DELETE) {
      continue;
    }
    if (change->move_path != NULL) {
      rc = cai_patch_resolve_new(ctx, change->move_path,
                                 &change->resolved_move_path, error);
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
  }
  return CAI_OK;
}

static int cai_patch_write_atomic(const char *path, const char *data,
                                  size_t length, cai_error *error) {
  char temporary[PATH_MAX];
  size_t offset;
  ssize_t nwritten;
  struct stat st;
  mode_t mode;
  int fd;

  if (snprintf(temporary, sizeof(temporary), "%s.cai-patch-XXXXXX", path) >=
      (int)sizeof(temporary)) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "patch temporary path is too long");
  }
  fd = mkstemp(temporary);
  if (fd < 0) {
    return cai_set_error_detail(error, CAI_ERR_INVALID,
                                "failed to create patch temporary file",
                                strerror(errno));
  }
  mode = 0644;
  if (stat(path, &st) == 0) {
    mode = st.st_mode & 0777;
  }
  if (fchmod(fd, mode) != 0) {
    close(fd);
    unlink(temporary);
    return cai_set_error_detail(error, CAI_ERR_INVALID,
                                "failed to set patch file mode",
                                strerror(errno));
  }
  offset = 0U;
  while (offset < length) {
    nwritten = write(fd, data + offset, length - offset);
    if (nwritten <= 0) {
      close(fd);
      unlink(temporary);
      return cai_set_error_detail(error, CAI_ERR_INVALID,
                                  "failed to write patch file",
                                  strerror(errno));
    }
    offset += (size_t)nwritten;
  }
  if (fsync(fd) != 0 || close(fd) != 0 || rename(temporary, path) != 0) {
    unlink(temporary);
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
    const char *target;

    change = &plan->items[i];
    if (change->kind == CAI_PATCH_DELETE) {
      continue;
    }
    target = change->resolved_move_path != NULL ? change->resolved_move_path
                                                : change->resolved_path;
    rc = cai_patch_write_atomic(target, change->after, change->after_length,
                                error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  for (i = 0U; i < plan->count; i++) {
    cai_patch_change *change;

    change = &plan->items[i];
    if (change->kind == CAI_PATCH_DELETE ||
        change->resolved_move_path != NULL) {
      if (unlink(change->resolved_path) != 0) {
        return cai_set_error_detail(error, CAI_ERR_INVALID,
                                    "failed to remove patched file",
                                    strerror(errno));
      }
    }
  }
  return CAI_OK;
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
  rc = cai_buffer_append_cstr(&builder, "{\"applied\":true,\"files\":[", error);
  for (i = 0U; rc == CAI_OK && i < plan->count; i++) {
    if (i > 0U) {
      rc = cai_buffer_append_cstr(&builder, ",", error);
    }
    if (rc == CAI_OK) {
      rc = cai_buffer_append_json_string(&builder, plan->items[i].path, error);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_buffer_append_cstr(&builder, "]}", error);
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

static int cai_patch_tool_callback(void *context, const void *params,
                                   void *result, cai_error *error) {
  const cai_patch_args *args;
  cai_patch_result *out;
  cai_patch_context *ctx;
  cai_patch_plan plan;
  int rc;

  ctx = (cai_patch_context *)context;
  args = (const cai_patch_args *)params;
  out = (cai_patch_result *)result;
  if (ctx == NULL || args == NULL || out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "apply_patch received invalid state");
  }
  memset(&plan, 0, sizeof(plan));
  rc = cai_patch_parse(ctx, args->patch, &plan, error);
  if (rc == CAI_OK) {
    rc = cai_patch_preflight(ctx, &plan, error);
  }
  if (rc == CAI_OK) {
    rc = cai_patch_commit(&plan, error);
  }
  if (rc == CAI_OK) {
    char summary[128];

    snprintf(summary, sizeof(summary), "Applied patch to %lu file(s)",
             (unsigned long)plan.count);
    out->summary = cai_strdup(NULL, summary);
    if (out->summary == NULL) {
      rc = cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate patch summary");
    } else {
      out->applied = 1;
    }
  }
  cai_patch_plan_cleanup(&plan);
  return rc;
}

int cai_tool_registry_register_patch_tool(cai_tool_registry *registry,
                                          const cai_patch_tool_config *config,
                                          cai_error *error) {
  cai_patch_context *ctx;
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
  rc = cai_tool_registry_register_lonejson_schema_owned(
      registry, name, description, cai_patch_schema_json, 0,
      &cai_patch_args_map, &cai_patch_result_map, cai_patch_tool_callback, ctx,
      cai_patch_context_cleanup, error);
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
