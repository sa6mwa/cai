#include "../cai_internal.h"

#include <cai/tools/exec.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <pty.h>
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include <util.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

extern char *realpath(const char *path, char *resolved_path);

#define CAI_EXEC_DEFAULT_TIMEOUT_MS 10000L
#define CAI_EXEC_DEFAULT_MAX_TIMEOUT_MS 60000L
#define CAI_EXEC_DEFAULT_OUTPUT_MEMORY_LIMIT (128U * 1024U)
#define CAI_EXEC_DEFAULT_OUTPUT_MAX_BYTES (1024U * 1024U)
#define CAI_EXEC_DEFAULT_PIDS_MAX 64LL
#define CAI_EXEC_DEFAULT_MEMORY_MAX_BYTES (256LL * 1024LL * 1024LL)
#define CAI_EXEC_DEFAULT_CGROUP_PARENT "/sys/fs/cgroup"

typedef struct cai_exec_context {
  char *root_path;
  char *default_workdir;
  char *shell_path;
  char *bwrap_path;
  char *sandbox_exec_path;
  int allow_network;
  int allow_pty;
  int allow_login_shell;
  int enable_cgroup_limits;
  long timeout_ms;
  long max_timeout_ms;
  long long pids_max;
  long long memory_max_bytes;
  char *cgroup_parent_path;
  size_t output_memory_limit;
  size_t output_max_bytes;
  char *output_spool_dir;
} cai_exec_context;

typedef struct cai_exec_args {
  char *cmd;
  char *workdir;
  char *shell;
  int tty;
  int has_tty;
  int login;
  int has_login;
  long long timeout_ms;
  int has_timeout_ms;
  long long max_output_tokens;
  int has_max_output_tokens;
} cai_exec_args;

typedef struct cai_exec_result {
  double wall_time_seconds;
  long long exit_code;
  int has_exit_code;
  long long signal;
  int has_signal;
  int timed_out;
  char *cwd;
  char *sandbox;
  long long original_byte_count;
  int stdout_truncated;
  int stderr_truncated;
  int output_truncated;
  lonejson_spooled stdout_data;
  lonejson_spooled stderr_data;
  lonejson_spooled output;
} cai_exec_result;

typedef struct cai_exec_capture {
  lonejson_spooled stdout_data;
  lonejson_spooled stderr_data;
  lonejson_spooled output;
  size_t max_bytes;
  size_t stdout_bytes;
  size_t stderr_bytes;
  size_t output_bytes;
  int stdout_truncated;
  int stderr_truncated;
  int output_truncated;
} cai_exec_capture;

typedef struct cai_exec_proc_result {
  int exit_code;
  int has_exit_code;
  int signal_number;
  int has_signal;
  int timed_out;
  long duration_ms;
} cai_exec_proc_result;

typedef struct cai_exec_cgroup {
  int enabled;
  char path[PATH_MAX];
} cai_exec_cgroup;

static const lonejson_field cai_exec_arg_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_exec_args, cmd, "cmd"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_exec_args, workdir, "workdir"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_exec_args, shell, "shell"),
    LONEJSON_FIELD_BOOL_PRESENT_NULLABLE(cai_exec_args, tty, has_tty, "tty"),
    LONEJSON_FIELD_BOOL_PRESENT_NULLABLE(cai_exec_args, login, has_login,
                                         "login"),
    LONEJSON_FIELD_I64_PRESENT_NULLABLE(cai_exec_args, timeout_ms,
                                        has_timeout_ms, "timeout_ms"),
    LONEJSON_FIELD_I64_PRESENT_NULLABLE(cai_exec_args, max_output_tokens,
                                        has_max_output_tokens,
                                        "max_output_tokens")};
LONEJSON_MAP_DEFINE(cai_exec_args_map, cai_exec_args, cai_exec_arg_fields);

static const lonejson_field cai_exec_result_fields[] = {
    LONEJSON_FIELD_F64_REQ(cai_exec_result, wall_time_seconds,
                           "wall_time_seconds"),
    LONEJSON_FIELD_I64_PRESENT(cai_exec_result, exit_code, has_exit_code,
                               "exit_code"),
    LONEJSON_FIELD_I64_PRESENT(cai_exec_result, signal, has_signal, "signal"),
    LONEJSON_FIELD_BOOL_REQ(cai_exec_result, timed_out, "timed_out"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_exec_result, cwd, "cwd"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_exec_result, sandbox, "sandbox"),
    LONEJSON_FIELD_I64_REQ(cai_exec_result, original_byte_count,
                           "original_byte_count"),
    LONEJSON_FIELD_BOOL_REQ(cai_exec_result, stdout_truncated,
                            "stdout_truncated"),
    LONEJSON_FIELD_BOOL_REQ(cai_exec_result, stderr_truncated,
                            "stderr_truncated"),
    LONEJSON_FIELD_BOOL_REQ(cai_exec_result, output_truncated,
                            "output_truncated"),
    LONEJSON_FIELD_STRING_STREAM_REQ(cai_exec_result, stdout_data, "stdout"),
    LONEJSON_FIELD_STRING_STREAM_REQ(cai_exec_result, stderr_data, "stderr"),
    LONEJSON_FIELD_STRING_STREAM_REQ(cai_exec_result, output, "output")};
LONEJSON_MAP_DEFINE(cai_exec_result_map, cai_exec_result,
                    cai_exec_result_fields);

static const char cai_exec_schema_json[] =
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
    "\"cmd\":{\"type\":\"string\"},"
    "\"workdir\":{\"type\":[\"string\",\"null\"]},"
    "\"shell\":{\"type\":[\"string\",\"null\"]},"
    "\"tty\":{\"type\":[\"boolean\",\"null\"]},"
    "\"login\":{\"type\":[\"boolean\",\"null\"]},"
    "\"timeout_ms\":{\"type\":[\"integer\",\"null\"]},"
    "\"max_output_tokens\":{\"type\":[\"integer\",\"null\"]}"
    "},"
    "\"required\":[\"cmd\"],"
    "\"additionalProperties\":false"
    "}";

static const char cai_exec_default_description[] =
    "Runs a sandboxed shell command non-interactively and returns its output. "
    "Linux uses bubblewrap; Darwin uses experimental sandbox-exec support. "
    "Always set workdir when a specific directory matters; do not use cd "
    "unless absolutely necessary. The embedding application controls "
    "sandboxing, writable roots, timeout caps, PTY support, and shell policy.";

static const char *cai_exec_default_string(const char *value,
                                           const char *fallback) {
  return value != NULL && value[0] != '\0' ? value : fallback;
}

static int cai_exec_strdup_field(char **out, const char *value,
                                 const char *message, cai_error *error) {
  *out = cai_strdup(NULL, value);
  if (*out == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, message);
  }
  return CAI_OK;
}

static int cai_exec_strdup_optional(char **out, const char *value,
                                    const char *message, cai_error *error) {
  if (value == NULL || value[0] == '\0') {
    *out = NULL;
    return CAI_OK;
  }
  return cai_exec_strdup_field(out, value, message, error);
}

static void cai_exec_context_cleanup(void *context) {
  cai_exec_context *ctx;

  ctx = (cai_exec_context *)context;
  if (ctx == NULL) {
    return;
  }
  cai_free_mem(NULL, ctx->root_path);
  cai_free_mem(NULL, ctx->default_workdir);
  cai_free_mem(NULL, ctx->shell_path);
  cai_free_mem(NULL, ctx->bwrap_path);
  cai_free_mem(NULL, ctx->sandbox_exec_path);
  cai_free_mem(NULL, ctx->cgroup_parent_path);
  cai_free_mem(NULL, ctx->output_spool_dir);
  cai_free_mem(NULL, ctx);
}

static int cai_exec_is_dir(const char *path) {
  struct stat st;

  if (path == NULL || stat(path, &st) != 0) {
    return 0;
  }
  return S_ISDIR(st.st_mode) ? 1 : 0;
}

static int cai_exec_path_is_under_root(const char *root, const char *path) {
  size_t root_len;

  if (root == NULL || path == NULL) {
    return 0;
  }
  root_len = strlen(root);
  if (root_len == 1U && root[0] == '/') {
    return path[0] == '/' ? 1 : 0;
  }
  if (strncmp(path, root, root_len) != 0) {
    return 0;
  }
  return path[root_len] == '\0' || path[root_len] == '/' ? 1 : 0;
}

static int cai_exec_realpath_dup(const char *path, char **out,
                                 cai_error *error) {
  char resolved[PATH_MAX];

  if (realpath(path, resolved) == NULL) {
    return cai_set_error_detail(error, CAI_ERR_INVALID,
                                "failed to resolve command execution path",
                                strerror(errno));
  }
  return cai_exec_strdup_field(out, resolved,
                               "failed to allocate command execution path",
                               error);
}

static int cai_exec_context_new(const cai_exec_tool_config *config,
                                cai_exec_context **out, cai_error *error) {
  cai_exec_context *ctx;
  const char *root;
  const char *workdir;
  const char *shell;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "exec context output pointer is required");
  }
  *out = NULL;
  root = config != NULL ? config->root_path : NULL;
  if (root == NULL || root[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "exec tool root_path is required");
  }
  if (!cai_exec_is_dir(root)) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "exec tool root_path must be an existing directory");
  }
  ctx = (cai_exec_context *)cai_alloc(NULL, sizeof(*ctx));
  if (ctx == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate exec tool context");
  }
  memset(ctx, 0, sizeof(*ctx));
  rc = cai_exec_realpath_dup(root, &ctx->root_path, error);
  workdir = cai_exec_default_string(config != NULL ? config->default_workdir
                                                  : NULL,
                                    ctx->root_path);
  if (rc == CAI_OK) {
    rc = cai_exec_realpath_dup(workdir, &ctx->default_workdir, error);
  }
  shell = cai_exec_default_string(config != NULL ? config->shell_path : NULL,
                                  "/bin/sh");
  if (rc == CAI_OK) {
    rc = cai_exec_strdup_field(&ctx->shell_path, shell,
                               "failed to allocate exec shell path", error);
  }
  if (rc == CAI_OK) {
    rc = cai_exec_strdup_optional(
        &ctx->bwrap_path, config != NULL ? config->bwrap_path : NULL,
        "failed to allocate bwrap path", error);
  }
  if (rc == CAI_OK) {
    rc = cai_exec_strdup_optional(
        &ctx->sandbox_exec_path,
        config != NULL ? config->sandbox_exec_path : NULL,
        "failed to allocate sandbox-exec path", error);
  }
  if (rc == CAI_OK) {
    rc = cai_exec_strdup_optional(
        &ctx->output_spool_dir,
        config != NULL ? config->output_spool_dir : NULL,
        "failed to allocate exec output spool directory", error);
  }
  if (rc == CAI_OK) {
    rc = cai_exec_strdup_optional(
        &ctx->cgroup_parent_path,
        config != NULL ? config->cgroup_parent_path : NULL,
        "failed to allocate exec cgroup parent path", error);
  }
  if (rc != CAI_OK) {
    cai_exec_context_cleanup(ctx);
    return rc;
  }
  ctx->allow_network = config != NULL && config->allow_network ? 1 : 0;
  ctx->allow_pty = config != NULL && config->allow_pty ? 1 : 0;
  ctx->allow_login_shell =
      config != NULL && config->allow_login_shell ? 1 : 0;
  ctx->enable_cgroup_limits =
      config != NULL && config->enable_cgroup_limits ? 1 : 0;
  ctx->timeout_ms =
      config != NULL && config->timeout_ms > 0L ? config->timeout_ms
                                                : CAI_EXEC_DEFAULT_TIMEOUT_MS;
  ctx->max_timeout_ms = config != NULL && config->max_timeout_ms > 0L
                            ? config->max_timeout_ms
                            : CAI_EXEC_DEFAULT_MAX_TIMEOUT_MS;
  ctx->pids_max = config != NULL && config->pids_max > 0LL
                      ? config->pids_max
                      : CAI_EXEC_DEFAULT_PIDS_MAX;
  ctx->memory_max_bytes =
      config != NULL && config->memory_max_bytes > 0LL
          ? config->memory_max_bytes
          : CAI_EXEC_DEFAULT_MEMORY_MAX_BYTES;
  if (ctx->timeout_ms > ctx->max_timeout_ms) {
    ctx->timeout_ms = ctx->max_timeout_ms;
  }
  ctx->output_memory_limit =
      config != NULL && config->output_memory_limit != 0U
          ? config->output_memory_limit
          : CAI_EXEC_DEFAULT_OUTPUT_MEMORY_LIMIT;
  ctx->output_max_bytes = config != NULL && config->output_max_bytes != 0U
                              ? config->output_max_bytes
                              : CAI_EXEC_DEFAULT_OUTPUT_MAX_BYTES;
  if (!cai_exec_path_is_under_root(ctx->root_path, ctx->default_workdir)) {
    cai_exec_context_cleanup(ctx);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "exec default_workdir must be under root_path");
  }
  *out = ctx;
  return CAI_OK;
}

static int cai_exec_find_trusted(const char *const *candidates, char *out,
                                 size_t out_size) {
  size_t i;
  size_t len;

  if (candidates == NULL || out == NULL || out_size == 0U) {
    return 0;
  }
  for (i = 0U; candidates[i] != NULL; i++) {
    len = strlen(candidates[i]);
    if (len + 1U <= out_size && access(candidates[i], X_OK) == 0) {
      memcpy(out, candidates[i], len + 1U);
      return 1;
    }
  }
  return 0;
}

static const char *cai_exec_sandbox_name(void) {
#if defined(__linux__)
  return "bwrap";
#elif defined(__APPLE__)
  return "sandbox-exec";
#else
  return "unavailable";
#endif
}

static int cai_exec_resolve_workdir(const cai_exec_context *ctx,
                                    const char *requested, char **out,
                                    cai_error *error) {
  char joined[PATH_MAX];
  char *resolved;
  const char *candidate;

  *out = NULL;
  candidate =
      requested != NULL && requested[0] != '\0' ? requested : ctx->default_workdir;
  if (candidate[0] != '/') {
    if (strlen(ctx->default_workdir) + 1U + strlen(candidate) + 1U >
        sizeof(joined)) {
      return cai_set_error(error, CAI_ERR_INVALID,
                           "exec workdir path is too long");
    }
    strcpy(joined, ctx->default_workdir);
    strcat(joined, "/");
    strcat(joined, candidate);
    candidate = joined;
  }
  resolved = NULL;
  if (cai_exec_realpath_dup(candidate, &resolved, error) != CAI_OK) {
    return error != NULL ? error->code : CAI_ERR_INVALID;
  }
  if (!cai_exec_path_is_under_root(ctx->root_path, resolved)) {
    cai_free_mem(NULL, resolved);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "exec workdir must stay under root_path");
  }
  *out = resolved;
  return CAI_OK;
}

static int cai_exec_shell_allowed(const cai_exec_context *ctx,
                                  const char *shell_path) {
  if (shell_path == NULL || shell_path[0] == '\0') {
    return 1;
  }
  if (strcmp(shell_path, ctx->shell_path) == 0) {
    return 1;
  }
  return 0;
}

static int cai_exec_append_spool(lonejson_spooled *spool, const char *data,
                                 size_t len, cai_error *error) {
  lonejson_error json_error;

  if (len == 0U) {
    return CAI_OK;
  }
  lonejson_error_init(&json_error);
  if (lonejson_spooled_append(spool, data, len, &json_error) !=
      LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to append command output",
                                json_error.message);
  }
  return CAI_OK;
}

static int cai_exec_capture_append(cai_exec_capture *capture, int is_stderr,
                                   const char *data, size_t len,
                                   cai_error *error) {
  size_t *stream_bytes;
  int *truncated;
  lonejson_spooled *stream;
  size_t room;
  size_t keep;
  int rc;

  stream = is_stderr ? &capture->stderr_data : &capture->stdout_data;
  stream_bytes = is_stderr ? &capture->stderr_bytes : &capture->stdout_bytes;
  truncated = is_stderr ? &capture->stderr_truncated
                        : &capture->stdout_truncated;
  if (len == 0U) {
    return CAI_OK;
  }
  room = *stream_bytes < capture->max_bytes ? capture->max_bytes - *stream_bytes
                                            : 0U;
  keep = len < room ? len : room;
  if (keep < len) {
    *truncated = 1;
  }
  if (keep > 0U) {
    rc = cai_exec_append_spool(stream, data, keep, error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  room = capture->output_bytes < capture->max_bytes
             ? capture->max_bytes - capture->output_bytes
             : 0U;
  keep = len < room ? len : room;
  if (keep < len) {
    capture->output_truncated = 1;
  }
  if (keep > 0U) {
    rc = cai_exec_append_spool(&capture->output, data, keep, error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  *stream_bytes += len;
  capture->output_bytes += len;
  return CAI_OK;
}

static long cai_exec_now_ms(void) {
  struct timeval tv;

  gettimeofday(&tv, NULL);
  return (long)(tv.tv_sec * 1000L + tv.tv_usec / 1000L);
}

static void cai_exec_close_fd(int *fd) {
  if (fd != NULL && *fd >= 0) {
    close(*fd);
    *fd = -1;
  }
}

static void cai_exec_sleep_ms(long ms) {
  struct timeval tv;

  if (ms <= 0L) {
    return;
  }
  tv.tv_sec = ms / 1000L;
  tv.tv_usec = (ms % 1000L) * 1000L;
  select(0, NULL, NULL, NULL, &tv);
}

static int cai_exec_set_cloexec(int fd) {
  int flags;

  flags = fcntl(fd, F_GETFD, 0);
  if (flags < 0) {
    return -1;
  }
  return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static int cai_exec_write_file_text(const char *path, const char *text,
                                    cai_error *error) {
  int fd;
  size_t len;
  ssize_t nwritten;

  fd = open(path, O_WRONLY);
  if (fd < 0) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to open cgroup control file",
                                strerror(errno));
  }
  len = strlen(text);
  nwritten = write(fd, text, len);
  close(fd);
  if (nwritten != (ssize_t)len) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to write cgroup control file",
                                strerror(errno));
  }
  return CAI_OK;
}

static int cai_exec_cgroup_join(char *out, size_t out_size, const char *dir,
                                const char *name, cai_error *error) {
  if (strlen(dir) + 1U + strlen(name) + 1U > out_size) {
    return cai_set_error(error, CAI_ERR_INVALID, "cgroup path is too long");
  }
  strcpy(out, dir);
  strcat(out, "/");
  strcat(out, name);
  return CAI_OK;
}

static int cai_exec_cgroup_prepare(const cai_exec_context *ctx,
                                   cai_exec_cgroup *cgroup,
                                   cai_error *error) {
#if defined(__linux__)
  const char *parent;
  char control_path[PATH_MAX];
  char value[64];

  memset(cgroup, 0, sizeof(*cgroup));
  if (!ctx->enable_cgroup_limits) {
    return CAI_OK;
  }
  parent = ctx->cgroup_parent_path != NULL ? ctx->cgroup_parent_path
                                           : CAI_EXEC_DEFAULT_CGROUP_PARENT;
  if (snprintf(cgroup->path, sizeof(cgroup->path), "%s/cai-exec-%ld-%ld",
               parent, (long)getpid(), cai_exec_now_ms()) >=
      (int)sizeof(cgroup->path)) {
    return cai_set_error(error, CAI_ERR_INVALID, "cgroup path is too long");
  }
  if (mkdir(cgroup->path, 0700) != 0) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to create exec cgroup",
                                strerror(errno));
  }
  cgroup->enabled = 1;
  if (cai_exec_cgroup_join(control_path, sizeof(control_path), cgroup->path,
                           "pids.max", error) != CAI_OK) {
    return error != NULL ? error->code : CAI_ERR_INVALID;
  }
  snprintf(value, sizeof(value), "%lld", ctx->pids_max);
  if (cai_exec_write_file_text(control_path, value, error) != CAI_OK) {
    return error != NULL ? error->code : CAI_ERR_TRANSPORT;
  }
  if (cai_exec_cgroup_join(control_path, sizeof(control_path), cgroup->path,
                           "memory.max", error) != CAI_OK) {
    return error != NULL ? error->code : CAI_ERR_INVALID;
  }
  snprintf(value, sizeof(value), "%lld", ctx->memory_max_bytes);
  if (cai_exec_write_file_text(control_path, value, error) != CAI_OK) {
    return error != NULL ? error->code : CAI_ERR_TRANSPORT;
  }
  return CAI_OK;
#else
  memset(cgroup, 0, sizeof(*cgroup));
  if (!ctx->enable_cgroup_limits) {
    return CAI_OK;
  }
  return cai_set_error(error, CAI_ERR_INVALID,
                       "exec cgroup limits require Linux cgroup v2");
#endif
}

static int cai_exec_cgroup_add_pid(const cai_exec_cgroup *cgroup, pid_t pid,
                                   cai_error *error) {
  char control_path[PATH_MAX];
  char value[64];

  if (cgroup == NULL || !cgroup->enabled) {
    return CAI_OK;
  }
  if (cai_exec_cgroup_join(control_path, sizeof(control_path), cgroup->path,
                           "cgroup.procs", error) != CAI_OK) {
    return error != NULL ? error->code : CAI_ERR_INVALID;
  }
  snprintf(value, sizeof(value), "%ld", (long)pid);
  return cai_exec_write_file_text(control_path, value, error);
}

static void cai_exec_cgroup_cleanup(cai_exec_cgroup *cgroup) {
  if (cgroup != NULL && cgroup->enabled) {
    rmdir(cgroup->path);
    cgroup->enabled = 0;
    cgroup->path[0] = '\0';
  }
}

static void cai_exec_bwrap_arg(const char **argv, size_t *i, size_t cap,
                               const char *arg) {
  if (*i + 1U < cap) {
    argv[*i] = arg;
    (*i)++;
  }
}

static void cai_exec_bwrap_bind_if_exists(const char **argv, size_t *i,
                                          size_t cap, const char *flag,
                                          const char *path) {
  if (path != NULL && access(path, F_OK) == 0) {
    cai_exec_bwrap_arg(argv, i, cap, flag);
    cai_exec_bwrap_arg(argv, i, cap, path);
    cai_exec_bwrap_arg(argv, i, cap, path);
  }
}

static void cai_exec_bwrap_bind_file_if_exists(const char **argv, size_t *i,
                                               size_t cap, const char *path) {
  if (path != NULL && access(path, F_OK) == 0) {
    cai_exec_bwrap_arg(argv, i, cap, "--ro-bind");
    cai_exec_bwrap_arg(argv, i, cap, path);
    cai_exec_bwrap_arg(argv, i, cap, path);
  }
}

static void cai_exec_bwrap_parent_dirs(const char **argv, size_t *i,
                                       size_t cap, const char *path,
                                       char dirs[][PATH_MAX],
                                       size_t *dir_count, size_t dir_cap) {
  char current[PATH_MAX];
  const char *cursor;
  const char *slash;
  size_t len;

  if (path == NULL || path[0] != '/') {
    return;
  }
  cursor = path + 1;
  while (*cursor != '\0') {
    slash = strchr(cursor, '/');
    if (slash == NULL) {
      break;
    }
    len = (size_t)(slash - path);
    if (len == 0U || len >= sizeof(current)) {
      break;
    }
    memcpy(current, path, len);
    current[len] = '\0';
    if (strcmp(current, "/") != 0 && *dir_count < dir_cap) {
      strcpy(dirs[*dir_count], current);
      cai_exec_bwrap_arg(argv, i, cap, "--dir");
      cai_exec_bwrap_arg(argv, i, cap, dirs[*dir_count]);
      (*dir_count)++;
    }
    cursor = slash + 1;
  }
}

static int cai_exec_path_is_under_mount(const char *path) {
  return cai_exec_path_is_under_root("/usr", path) ||
         cai_exec_path_is_under_root("/bin", path) ||
         cai_exec_path_is_under_root("/lib", path) ||
         cai_exec_path_is_under_root("/lib64", path);
}

static int cai_exec_shell_runtime_prefix(const char *shell_path, char *out,
                                         size_t out_size) {
  const char *bin;
  size_t len;

  if (shell_path == NULL || shell_path[0] != '/') {
    return 0;
  }
  if (strncmp(shell_path, "/nix/store/", 11U) == 0) {
    len = strlen("/nix/store");
  } else {
    bin = strstr(shell_path, "/bin/");
    if (bin == NULL || bin == shell_path) {
      return 0;
    }
    len = (size_t)(bin - shell_path);
  }
  if (len == 0U || len >= out_size) {
    return 0;
  }
  memcpy(out, shell_path, len);
  out[len] = '\0';
  return 1;
}

static int cai_exec_shell_bin_dir(const char *shell_path, char *out,
                                  size_t out_size) {
  const char *bin;
  size_t len;

  if (shell_path == NULL || shell_path[0] != '/') {
    return 0;
  }
  bin = strstr(shell_path, "/bin/");
  if (bin == NULL || bin == shell_path) {
    return 0;
  }
  len = (size_t)(bin - shell_path) + 4U;
  if (len == 0U || len >= out_size) {
    return 0;
  }
  memcpy(out, shell_path, len);
  out[len] = '\0';
  return 1;
}

static const char *cai_exec_bwrap_path_env(const cai_exec_context *ctx,
                                           const char *shell_path,
                                           char dirs[][PATH_MAX],
                                           size_t *dir_count,
                                           size_t dir_cap) {
  char bin_dir[PATH_MAX];
  int n;

  if (cai_exec_shell_bin_dir(shell_path, bin_dir, sizeof(bin_dir)) &&
      !cai_exec_path_is_under_root(ctx->root_path, bin_dir) &&
      !cai_exec_path_is_under_mount(bin_dir) && access(bin_dir, F_OK) == 0 &&
      *dir_count < dir_cap) {
    n = snprintf(dirs[*dir_count], PATH_MAX, "%s:/usr/local/bin:/usr/bin:/bin",
                 bin_dir);
    if (n > 0 && (size_t)n < PATH_MAX) {
      (*dir_count)++;
      return dirs[*dir_count - 1U];
    }
  }
  return "/usr/local/bin:/usr/bin:/bin";
}

static int cai_exec_bwrap_bind_custom_shell_prefix(
    const cai_exec_context *ctx, const char **argv, size_t *i, size_t cap,
    const char *shell_path, char dirs[][PATH_MAX], size_t *dir_count,
    size_t dir_cap) {
  char prefix[PATH_MAX];

  if (!cai_exec_shell_runtime_prefix(shell_path, prefix, sizeof(prefix)) ||
      cai_exec_path_is_under_root(ctx->root_path, prefix) ||
      cai_exec_path_is_under_mount(prefix) || access(prefix, F_OK) != 0 ||
      *dir_count >= dir_cap) {
    return 0;
  }
  cai_exec_bwrap_parent_dirs(argv, i, cap, prefix, dirs, dir_count, dir_cap);
  if (*dir_count >= dir_cap) {
    return 0;
  }
  strcpy(dirs[*dir_count], prefix);
  cai_exec_bwrap_arg(argv, i, cap, "--ro-bind");
  cai_exec_bwrap_arg(argv, i, cap, dirs[*dir_count]);
  cai_exec_bwrap_arg(argv, i, cap, dirs[*dir_count]);
  (*dir_count)++;
  return 1;
}

static void cai_exec_bwrap_bind_custom_shell(const cai_exec_context *ctx,
                                             const char **argv, size_t *i,
                                             size_t cap,
                                             const char *shell_path,
                                             char dirs[][PATH_MAX],
                                             size_t *dir_count,
                                             size_t dir_cap) {
  if (shell_path == NULL || shell_path[0] != '/' ||
      cai_exec_path_is_under_root(ctx->root_path, shell_path) ||
      cai_exec_path_is_under_mount(shell_path)) {
    return;
  }
  if (cai_exec_bwrap_bind_custom_shell_prefix(ctx, argv, i, cap, shell_path,
                                              dirs, dir_count, dir_cap)) {
    return;
  }
  cai_exec_bwrap_parent_dirs(argv, i, cap, shell_path, dirs, dir_count,
                             dir_cap);
  cai_exec_bwrap_bind_file_if_exists(argv, i, cap, shell_path);
}

static int cai_exec_build_bwrap_argv(const cai_exec_context *ctx,
                                     const char *workdir,
                                     const char *shell_path,
                                     const char *cmd, const char *bwrap_path,
                                     int login_shell,
                                     const char **argv, size_t argv_cap,
                                     char dirs[][PATH_MAX],
                                     size_t *dir_count, size_t dir_cap) {
  size_t i;

  if (argv_cap < 48U) {
    return -1;
  }
  i = 0U;
  *dir_count = 0U;
  cai_exec_bwrap_arg(argv, &i, argv_cap, bwrap_path);
  cai_exec_bwrap_arg(argv, &i, argv_cap, "--die-with-parent");
  cai_exec_bwrap_arg(argv, &i, argv_cap, "--new-session");
  cai_exec_bwrap_arg(argv, &i, argv_cap, "--unshare-pid");
  cai_exec_bwrap_arg(argv, &i, argv_cap, "--unshare-ipc");
  cai_exec_bwrap_arg(argv, &i, argv_cap, "--unshare-uts");
  if (!ctx->allow_network) {
    cai_exec_bwrap_arg(argv, &i, argv_cap, "--unshare-net");
  }
  cai_exec_bwrap_arg(argv, &i, argv_cap, "--clearenv");
  cai_exec_bwrap_arg(argv, &i, argv_cap, "--setenv");
  cai_exec_bwrap_arg(argv, &i, argv_cap, "PATH");
  cai_exec_bwrap_arg(argv, &i, argv_cap,
                     cai_exec_bwrap_path_env(ctx, shell_path, dirs, dir_count,
                                             dir_cap));
  cai_exec_bwrap_arg(argv, &i, argv_cap, "--setenv");
  cai_exec_bwrap_arg(argv, &i, argv_cap, "HOME");
  cai_exec_bwrap_arg(argv, &i, argv_cap, ctx->root_path);
  cai_exec_bwrap_arg(argv, &i, argv_cap, "--setenv");
  cai_exec_bwrap_arg(argv, &i, argv_cap, "TMPDIR");
  cai_exec_bwrap_arg(argv, &i, argv_cap, "/tmp");
  cai_exec_bwrap_arg(argv, &i, argv_cap, "--setenv");
  cai_exec_bwrap_arg(argv, &i, argv_cap, "LANG");
  cai_exec_bwrap_arg(argv, &i, argv_cap, "C");
  cai_exec_bwrap_arg(argv, &i, argv_cap, "--dev");
  cai_exec_bwrap_arg(argv, &i, argv_cap, "/dev");
  cai_exec_bwrap_arg(argv, &i, argv_cap, "--proc");
  cai_exec_bwrap_arg(argv, &i, argv_cap, "/proc");
  cai_exec_bwrap_arg(argv, &i, argv_cap, "--tmpfs");
  cai_exec_bwrap_arg(argv, &i, argv_cap, "/tmp");
  cai_exec_bwrap_arg(argv, &i, argv_cap, "--dir");
  cai_exec_bwrap_arg(argv, &i, argv_cap, "/var");
  cai_exec_bwrap_arg(argv, &i, argv_cap, "--tmpfs");
  cai_exec_bwrap_arg(argv, &i, argv_cap, "/var/tmp");
  cai_exec_bwrap_parent_dirs(argv, &i, argv_cap, ctx->root_path, dirs,
                             dir_count, dir_cap);
  cai_exec_bwrap_arg(argv, &i, argv_cap, "--bind");
  cai_exec_bwrap_arg(argv, &i, argv_cap, ctx->root_path);
  cai_exec_bwrap_arg(argv, &i, argv_cap, ctx->root_path);
  cai_exec_bwrap_bind_if_exists(argv, &i, argv_cap, "--ro-bind", "/usr");
  cai_exec_bwrap_bind_if_exists(argv, &i, argv_cap, "--ro-bind", "/bin");
  cai_exec_bwrap_bind_if_exists(argv, &i, argv_cap, "--ro-bind", "/lib");
  cai_exec_bwrap_bind_if_exists(argv, &i, argv_cap, "--ro-bind", "/lib64");
  cai_exec_bwrap_bind_custom_shell(ctx, argv, &i, argv_cap, shell_path, dirs,
                                   dir_count, dir_cap);
  if (access("/etc/ld.so.cache", F_OK) == 0 || ctx->allow_network) {
    cai_exec_bwrap_arg(argv, &i, argv_cap, "--dir");
    cai_exec_bwrap_arg(argv, &i, argv_cap, "/etc");
  }
  if (access("/etc/ld.so.cache", F_OK) == 0) {
    cai_exec_bwrap_arg(argv, &i, argv_cap, "--ro-bind");
    cai_exec_bwrap_arg(argv, &i, argv_cap, "/etc/ld.so.cache");
    cai_exec_bwrap_arg(argv, &i, argv_cap, "/etc/ld.so.cache");
  }
  if (ctx->allow_network) {
    cai_exec_bwrap_bind_file_if_exists(argv, &i, argv_cap,
                                       "/etc/resolv.conf");
    cai_exec_bwrap_bind_file_if_exists(argv, &i, argv_cap,
                                       "/etc/nsswitch.conf");
    cai_exec_bwrap_bind_file_if_exists(argv, &i, argv_cap, "/etc/hosts");
    cai_exec_bwrap_bind_if_exists(argv, &i, argv_cap, "--ro-bind", "/etc/ssl");
    cai_exec_bwrap_bind_if_exists(argv, &i, argv_cap, "--ro-bind", "/etc/pki");
  }
  cai_exec_bwrap_arg(argv, &i, argv_cap, "--chdir");
  cai_exec_bwrap_arg(argv, &i, argv_cap, workdir);
  cai_exec_bwrap_arg(argv, &i, argv_cap, "--");
  cai_exec_bwrap_arg(argv, &i, argv_cap, shell_path);
  cai_exec_bwrap_arg(argv, &i, argv_cap, login_shell ? "-lc" : "-c");
  cai_exec_bwrap_arg(argv, &i, argv_cap, cmd);
  argv[i] = NULL;
  if (i + 1U >= argv_cap) {
    return -1;
  }
  return 0;
}

#if defined(__APPLE__)
static int cai_exec_profile_append(char *profile, size_t profile_size,
                                   size_t *offset, const char *text) {
  size_t len;

  len = strlen(text);
  if (*offset > profile_size || len >= profile_size - *offset) {
    return -1;
  }
  memcpy(profile + *offset, text, len);
  *offset += len;
  profile[*offset] = '\0';
  return 0;
}

static int cai_exec_profile_append_quoted(char *profile, size_t profile_size,
                                          size_t *offset, const char *text) {
  const char *p;

  if (cai_exec_profile_append(profile, profile_size, offset, "\"") != 0) {
    return -1;
  }
  for (p = text; p != NULL && *p != '\0'; p++) {
    if (*p == '"' || *p == '\\') {
      if (cai_exec_profile_append(profile, profile_size, offset, "\\") != 0) {
        return -1;
      }
    }
    if (*offset + 1U >= profile_size) {
      return -1;
    }
    profile[*offset] = *p;
    (*offset)++;
    profile[*offset] = '\0';
  }
  return cai_exec_profile_append(profile, profile_size, offset, "\"");
}

static int cai_exec_profile_allow_subpath(char *profile, size_t profile_size,
                                          size_t *offset, const char *path) {
  if (cai_exec_profile_append(profile, profile_size, offset, " (subpath ") !=
      0) {
    return -1;
  }
  if (cai_exec_profile_append_quoted(profile, profile_size, offset, path) !=
      0) {
    return -1;
  }
  return cai_exec_profile_append(profile, profile_size, offset, ")");
}

static int cai_exec_profile_allow_literal(char *profile, size_t profile_size,
                                          size_t *offset, const char *path) {
  if (cai_exec_profile_append(profile, profile_size, offset, " (literal ") !=
      0) {
    return -1;
  }
  if (cai_exec_profile_append_quoted(profile, profile_size, offset, path) !=
      0) {
    return -1;
  }
  return cai_exec_profile_append(profile, profile_size, offset, ")");
}

static int cai_exec_build_sandbox_exec_profile(const cai_exec_context *ctx,
                                               char *profile,
                                               size_t profile_size) {
  size_t offset;

  offset = 0U;
  profile[0] = '\0';
  if (cai_exec_profile_append(
          profile, profile_size, &offset,
          "(version 1)\n"
          "(deny default)\n"
          "(allow process*)\n"
          "(allow sysctl-read)\n"
          "(allow mach-lookup)\n"
          "(allow file-read-metadata)\n"
          "(allow file-read-data") != 0) {
    return -1;
  }
  if (cai_exec_profile_allow_subpath(profile, profile_size, &offset,
                                     ctx->root_path) != 0 ||
      cai_exec_profile_allow_literal(profile, profile_size, &offset,
                                     ctx->shell_path) != 0 ||
      cai_exec_profile_allow_subpath(profile, profile_size, &offset,
                                     "/bin") != 0 ||
      cai_exec_profile_allow_subpath(profile, profile_size, &offset,
                                     "/sbin") != 0 ||
      cai_exec_profile_allow_subpath(profile, profile_size, &offset,
                                     "/usr/bin") != 0 ||
      cai_exec_profile_allow_subpath(profile, profile_size, &offset,
                                     "/usr/sbin") != 0 ||
      cai_exec_profile_allow_subpath(profile, profile_size, &offset,
                                     "/usr/lib") != 0 ||
      cai_exec_profile_allow_subpath(profile, profile_size, &offset,
                                     "/System/Library") != 0 ||
      cai_exec_profile_allow_literal(profile, profile_size, &offset,
                                     "/dev/null") != 0) {
    return -1;
  }
  if (cai_exec_profile_append(profile, profile_size, &offset,
                              ")\n(allow file-write*") != 0 ||
      cai_exec_profile_allow_subpath(profile, profile_size, &offset,
                                     ctx->root_path) != 0 ||
      cai_exec_profile_append(profile, profile_size, &offset, ")\n") != 0) {
    return -1;
  }
  if (ctx->allow_network) {
    if (cai_exec_profile_append(profile, profile_size, &offset,
                                "(allow network*)\n") != 0) {
      return -1;
    }
  }
  return 0;
}

static int cai_exec_build_sandbox_exec_argv(
    const cai_exec_context *ctx, const cai_exec_args *args,
    const char *shell_path, const char *cmd, const char *sandbox_exec_path,
    const char **argv, size_t argv_cap, char *profile, size_t profile_size) {
  size_t i;

  if (argv_cap < 8U ||
      cai_exec_build_sandbox_exec_profile(ctx, profile, profile_size) != 0) {
    return -1;
  }
  i = 0U;
  argv[i++] = sandbox_exec_path;
  argv[i++] = "-p";
  argv[i++] = profile;
  argv[i++] = shell_path;
  if (args->has_login && args->login && ctx->allow_login_shell) {
    argv[i++] = "-lc";
  } else {
    argv[i++] = "-c";
  }
  argv[i++] = cmd;
  argv[i] = NULL;
  return 0;
}
#endif

static void cai_exec_child_exec(const cai_exec_context *ctx,
                                const cai_exec_args *args,
                                const char *workdir, const char *sandbox_path,
                                int use_sandbox) {
  const char *argv[96];
  char dirs[32][PATH_MAX];
#if defined(__APPLE__)
  char sandbox_profile[8192];
#endif
  size_t dir_count;
  const char *shell_path;

  shell_path = args->shell != NULL && args->shell[0] != '\0' ? args->shell
                                                             : ctx->shell_path;
  if (chdir(workdir) != 0) {
    _exit(126);
  }
  if (use_sandbox) {
#if defined(__linux__)
    if (cai_exec_build_bwrap_argv(ctx, workdir, shell_path, args->cmd,
                                  sandbox_path,
                                  args->has_login && args->login &&
                                      ctx->allow_login_shell,
                                  argv,
                                  sizeof(argv) / sizeof(argv[0]), dirs,
                                  &dir_count,
                                  sizeof(dirs) / sizeof(dirs[0])) != 0) {
      _exit(127);
    }
    execv(sandbox_path, (char *const *)(void *)argv);
    _exit(127);
#elif defined(__APPLE__)
    (void)dirs;
    dir_count = 0U;
    if (cai_exec_build_sandbox_exec_argv(
            ctx, args, shell_path, args->cmd, sandbox_path, argv,
            sizeof(argv) / sizeof(argv[0]), sandbox_profile,
            sizeof(sandbox_profile)) != 0) {
      _exit(127);
    }
    (void)dir_count;
    execv(sandbox_path, (char *const *)(void *)argv);
    _exit(127);
#else
    (void)argv;
    (void)dirs;
    (void)dir_count;
    _exit(127);
#endif
  }
  if (args->has_login && args->login && ctx->allow_login_shell) {
    execl(shell_path, shell_path, "-lc", args->cmd, (char *)NULL);
  } else {
    execl(shell_path, shell_path, "-c", args->cmd, (char *)NULL);
  }
  _exit(127);
}

static int cai_exec_spawn(const cai_exec_context *ctx, const cai_exec_args *args,
                          const char *workdir, const char *bwrap_path,
                          int use_sandbox, const cai_exec_cgroup *cgroup,
                          int stdout_fd[2], int stderr_fd[2], int *pty_master,
                          pid_t *pid_out, cai_error *error) {
  int stdin_fd;
  int slave_fd;
  int sync_fd[2];
  pid_t pid;

  stdin_fd = -1;
  slave_fd = -1;
  sync_fd[0] = sync_fd[1] = -1;
  *pty_master = -1;
  stdout_fd[0] = stdout_fd[1] = -1;
  stderr_fd[0] = stderr_fd[1] = -1;
  if (pipe(sync_fd) != 0) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to create command sync pipe",
                                strerror(errno));
  }
  cai_exec_set_cloexec(sync_fd[0]);
  cai_exec_set_cloexec(sync_fd[1]);
  if (args->has_tty && args->tty) {
    if (!ctx->allow_pty) {
      cai_exec_close_fd(&sync_fd[0]);
      cai_exec_close_fd(&sync_fd[1]);
      return cai_set_error(error, CAI_ERR_INVALID,
                           "exec tool PTY use is disabled by config");
    }
    if (openpty(pty_master, &slave_fd, NULL, NULL, NULL) != 0) {
      cai_exec_close_fd(&sync_fd[0]);
      cai_exec_close_fd(&sync_fd[1]);
      return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                  "failed to allocate PTY", strerror(errno));
    }
    cai_exec_set_cloexec(*pty_master);
  } else {
    if (pipe(stdout_fd) != 0 || pipe(stderr_fd) != 0) {
      cai_exec_close_fd(&sync_fd[0]);
      cai_exec_close_fd(&sync_fd[1]);
      cai_exec_close_fd(&stdout_fd[0]);
      cai_exec_close_fd(&stdout_fd[1]);
      cai_exec_close_fd(&stderr_fd[0]);
      cai_exec_close_fd(&stderr_fd[1]);
      return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                  "failed to create command pipes",
                                  strerror(errno));
    }
  }
  stdin_fd = open("/dev/null", O_RDONLY);
  if (stdin_fd < 0) {
    cai_exec_close_fd(&sync_fd[0]);
    cai_exec_close_fd(&sync_fd[1]);
    cai_exec_close_fd(pty_master);
    cai_exec_close_fd(&slave_fd);
    cai_exec_close_fd(&stdout_fd[0]);
    cai_exec_close_fd(&stdout_fd[1]);
    cai_exec_close_fd(&stderr_fd[0]);
    cai_exec_close_fd(&stderr_fd[1]);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to open /dev/null", strerror(errno));
  }
  pid = fork();
  if (pid < 0) {
    close(stdin_fd);
    cai_exec_close_fd(&sync_fd[0]);
    cai_exec_close_fd(&sync_fd[1]);
    cai_exec_close_fd(pty_master);
    cai_exec_close_fd(&slave_fd);
    cai_exec_close_fd(&stdout_fd[0]);
    cai_exec_close_fd(&stdout_fd[1]);
    cai_exec_close_fd(&stderr_fd[0]);
    cai_exec_close_fd(&stderr_fd[1]);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to fork command", strerror(errno));
  }
  if (pid == 0) {
    char start_byte;

    setpgid(0, 0);
    cai_exec_close_fd(&sync_fd[1]);
    if (args->has_tty && args->tty) {
      close(*pty_master);
      dup2(stdin_fd, STDIN_FILENO);
      dup2(slave_fd, STDOUT_FILENO);
      dup2(slave_fd, STDERR_FILENO);
      close(slave_fd);
    } else {
      dup2(stdin_fd, STDIN_FILENO);
      dup2(stdout_fd[1], STDOUT_FILENO);
      dup2(stderr_fd[1], STDERR_FILENO);
      cai_exec_close_fd(&stdout_fd[0]);
      cai_exec_close_fd(&stdout_fd[1]);
      cai_exec_close_fd(&stderr_fd[0]);
      cai_exec_close_fd(&stderr_fd[1]);
    }
    close(stdin_fd);
    if (read(sync_fd[0], &start_byte, 1U) != 1) {
      _exit(126);
    }
    close(sync_fd[0]);
    cai_exec_child_exec(ctx, args, workdir, bwrap_path, use_sandbox);
  }
  setpgid(pid, pid);
  close(stdin_fd);
  cai_exec_close_fd(&sync_fd[0]);
  if (cai_exec_cgroup_add_pid(cgroup, pid, error) != CAI_OK) {
    cai_exec_close_fd(&sync_fd[1]);
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    cai_exec_close_fd(pty_master);
    cai_exec_close_fd(&slave_fd);
    cai_exec_close_fd(&stdout_fd[0]);
    cai_exec_close_fd(&stdout_fd[1]);
    cai_exec_close_fd(&stderr_fd[0]);
    cai_exec_close_fd(&stderr_fd[1]);
    return error != NULL ? error->code : CAI_ERR_TRANSPORT;
  }
  if (write(sync_fd[1], "x", 1U) != 1) {
    cai_exec_close_fd(&sync_fd[1]);
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to start command", strerror(errno));
  }
  cai_exec_close_fd(&sync_fd[1]);
  if (args->has_tty && args->tty) {
    cai_exec_close_fd(&slave_fd);
  } else {
    cai_exec_close_fd(&stdout_fd[1]);
    cai_exec_close_fd(&stderr_fd[1]);
  }
  *pid_out = pid;
  return CAI_OK;
}

static int cai_exec_drain(cai_exec_capture *capture, int out_fd, int err_fd,
                          int pty_fd, pid_t pid, long timeout_ms,
                          cai_exec_proc_result *result, cai_error *error) {
  char buffer[8192];
  long start_ms;
  long now_ms;
  int status;
  int process_done;
  int rc;

  memset(result, 0, sizeof(*result));
  start_ms = cai_exec_now_ms();
  process_done = 0;
  status = 0;
  while (out_fd >= 0 || err_fd >= 0 || pty_fd >= 0 || !process_done) {
    fd_set readfds;
    int maxfd;
    struct timeval tv;

    now_ms = cai_exec_now_ms();
    if (!process_done && timeout_ms > 0L && now_ms - start_ms >= timeout_ms) {
      result->timed_out = 1;
      kill(-pid, SIGKILL);
      kill(pid, SIGKILL);
    }
    if (!process_done) {
      rc = waitpid(pid, &status, WNOHANG);
      if (rc == pid) {
        process_done = 1;
      } else if (rc < 0 && errno != EINTR) {
        process_done = 1;
      }
    }
    FD_ZERO(&readfds);
    maxfd = -1;
    if (out_fd >= 0) {
      FD_SET(out_fd, &readfds);
      if (out_fd > maxfd) {
        maxfd = out_fd;
      }
    }
    if (err_fd >= 0) {
      FD_SET(err_fd, &readfds);
      if (err_fd > maxfd) {
        maxfd = err_fd;
      }
    }
    if (pty_fd >= 0) {
      FD_SET(pty_fd, &readfds);
      if (pty_fd > maxfd) {
        maxfd = pty_fd;
      }
    }
    if (maxfd < 0) {
      if (process_done) {
        break;
      }
      cai_exec_sleep_ms(10L);
      continue;
    }
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    rc = select(maxfd + 1, &readfds, NULL, NULL, &tv);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                  "failed to read command output",
                                  strerror(errno));
    }
    if (out_fd >= 0 && FD_ISSET(out_fd, &readfds)) {
      ssize_t nread;

      nread = read(out_fd, buffer, sizeof(buffer));
      if (nread > 0) {
        rc = cai_exec_capture_append(capture, 0, buffer, (size_t)nread, error);
        if (rc != CAI_OK) {
          return rc;
        }
      } else if (nread == 0 || errno != EINTR) {
        close(out_fd);
        out_fd = -1;
      }
    }
    if (err_fd >= 0 && FD_ISSET(err_fd, &readfds)) {
      ssize_t nread;

      nread = read(err_fd, buffer, sizeof(buffer));
      if (nread > 0) {
        rc = cai_exec_capture_append(capture, 1, buffer, (size_t)nread, error);
        if (rc != CAI_OK) {
          return rc;
        }
      } else if (nread == 0 || errno != EINTR) {
        close(err_fd);
        err_fd = -1;
      }
    }
    if (pty_fd >= 0 && FD_ISSET(pty_fd, &readfds)) {
      ssize_t nread;

      nread = read(pty_fd, buffer, sizeof(buffer));
      if (nread > 0) {
        rc = cai_exec_capture_append(capture, 0, buffer, (size_t)nread, error);
        if (rc != CAI_OK) {
          return rc;
        }
      } else if (nread == 0 || errno != EINTR) {
        close(pty_fd);
        pty_fd = -1;
      }
    }
  }
  if (!process_done) {
    waitpid(pid, &status, 0);
  }
  result->duration_ms = cai_exec_now_ms() - start_ms;
  if (WIFEXITED(status)) {
    result->has_exit_code = 1;
    result->exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result->has_signal = 1;
    result->signal_number = WTERMSIG(status);
  }
  return CAI_OK;
}

static int cai_exec_run_process(const cai_exec_context *ctx,
                                const cai_exec_args *args, const char *workdir,
                                const char *bwrap_path, int use_sandbox,
                                cai_exec_capture *capture,
                                cai_exec_proc_result *result,
                                cai_error *error) {
  int stdout_fd[2];
  int stderr_fd[2];
  int pty_master;
  cai_exec_cgroup cgroup;
  pid_t pid;
  long timeout_ms;
  int rc;

  memset(&cgroup, 0, sizeof(cgroup));
  timeout_ms = ctx->timeout_ms;
  if (args->has_timeout_ms && args->timeout_ms > 0) {
    timeout_ms = (long)args->timeout_ms;
  }
  if (timeout_ms > ctx->max_timeout_ms) {
    timeout_ms = ctx->max_timeout_ms;
  }
  rc = cai_exec_cgroup_prepare(ctx, &cgroup, error);
  if (rc != CAI_OK) {
    cai_exec_cgroup_cleanup(&cgroup);
    return rc;
  }
  rc = cai_exec_spawn(ctx, args, workdir, bwrap_path, use_sandbox, &cgroup,
                      stdout_fd, stderr_fd, &pty_master, &pid, error);
  if (rc != CAI_OK) {
    cai_exec_cgroup_cleanup(&cgroup);
    return rc;
  }
  rc = cai_exec_drain(capture, stdout_fd[0], stderr_fd[0], pty_master, pid,
                      timeout_ms, result, error);
  if (rc != CAI_OK) {
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
  }
  cai_exec_cgroup_cleanup(&cgroup);
  return rc;
}

static int cai_exec_prepare_sandbox(const cai_exec_context *ctx, char *buffer,
                                    size_t buffer_size, int *use_sandbox,
                                    cai_error *error) {
#if defined(__linux__)
  static const char *const bwrap_candidates[] = {
      "/usr/bin/bwrap", "/bin/bwrap", "/usr/local/bin/bwrap", NULL};
  const char *path;

  *use_sandbox = 0;
  path = ctx->bwrap_path;
  if (path != NULL && path[0] != '\0') {
    if (access(path, X_OK) == 0) {
      snprintf(buffer, buffer_size, "%s", path);
      *use_sandbox = 1;
      return CAI_OK;
    }
  } else if (cai_exec_find_trusted(bwrap_candidates, buffer, buffer_size)) {
    *use_sandbox = 1;
    return CAI_OK;
  }
  return cai_set_error(error, CAI_ERR_INVALID,
                       "exec sandbox requires bubblewrap (bwrap) on Linux");
#elif defined(__APPLE__)
  static const char *const sandbox_exec_candidates[] = {
      "/usr/bin/sandbox-exec", NULL};
  const char *path;

  *use_sandbox = 0;
  path = ctx->sandbox_exec_path;
  if (path != NULL && path[0] != '\0') {
    if (access(path, X_OK) == 0) {
      snprintf(buffer, buffer_size, "%s", path);
      *use_sandbox = 1;
      return CAI_OK;
    }
  } else if (cai_exec_find_trusted(sandbox_exec_candidates, buffer,
                                   buffer_size)) {
    *use_sandbox = 1;
    return CAI_OK;
  }
  return cai_set_error(
      error, CAI_ERR_INVALID,
      "exec sandbox requires sandbox-exec on Darwin (experimental)");
#else
  *use_sandbox = 0;
  (void)buffer;
  (void)buffer_size;
  (void)ctx;
  return cai_set_error(error, CAI_ERR_INVALID,
                       "exec sandbox is not implemented on this platform");
#endif
}

static int cai_exec_callback(void *context, const void *params, void *result,
                             cai_error *error) {
  const cai_exec_context *ctx;
  const cai_exec_args *args;
  cai_exec_result *out;
  cai_exec_capture capture;
  lonejson_spool_options spool_options;
  cai_exec_proc_result proc;
  char *workdir;
  char sandbox_path[PATH_MAX];
  size_t output_max_bytes;
  int use_sandbox;
  int rc;

  ctx = (const cai_exec_context *)context;
  args = (const cai_exec_args *)params;
  out = (cai_exec_result *)result;
  if (ctx == NULL || args == NULL || out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "exec tool callback received invalid state");
  }
  if (args->cmd == NULL || args->cmd[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "exec command must not be empty");
  }
  if (!cai_exec_shell_allowed(ctx, args->shell)) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "exec shell must be /bin/sh or bash");
  }
  workdir = NULL;
  rc = cai_exec_resolve_workdir(ctx, args->workdir, &workdir, error);
  if (rc != CAI_OK) {
    return rc;
  }
  use_sandbox = 0;
  sandbox_path[0] = '\0';
  rc = cai_exec_prepare_sandbox(ctx, sandbox_path, sizeof(sandbox_path),
                                &use_sandbox, error);
  if (rc != CAI_OK) {
    cai_free_mem(NULL, workdir);
    return rc;
  }
  memset(&capture, 0, sizeof(capture));
  output_max_bytes = ctx->output_max_bytes;
  if (args->has_max_output_tokens && args->max_output_tokens > 0LL &&
      (unsigned long long)args->max_output_tokens <
          (unsigned long long)output_max_bytes) {
    output_max_bytes = (size_t)args->max_output_tokens;
  }
  spool_options = lonejson_default_spool_options();
  spool_options.memory_limit = ctx->output_memory_limit;
  spool_options.max_bytes = output_max_bytes;
  spool_options.temp_dir = ctx->output_spool_dir;
  capture.max_bytes = output_max_bytes;
  lonejson_spooled_init(&capture.stdout_data, &spool_options);
  lonejson_spooled_init(&capture.stderr_data, &spool_options);
  lonejson_spooled_init(&capture.output, &spool_options);
  rc = cai_exec_run_process(ctx, args, workdir, sandbox_path, use_sandbox,
                            &capture, &proc, error);
  if (rc == CAI_OK) {
    out->wall_time_seconds = (double)proc.duration_ms / 1000.0;
    out->timed_out = proc.timed_out;
    out->stdout_truncated = capture.stdout_truncated;
    out->stderr_truncated = capture.stderr_truncated;
    out->output_truncated = capture.output_truncated;
    out->original_byte_count =
        (long long)(capture.stdout_bytes + capture.stderr_bytes);
    out->cwd = cai_strdup(NULL, workdir);
    out->sandbox = cai_strdup(NULL, use_sandbox ? cai_exec_sandbox_name()
                                                : "unavailable");
    if (out->cwd == NULL || out->sandbox == NULL) {
      rc = cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate exec result metadata");
    }
    if (rc == CAI_OK && proc.has_exit_code) {
      out->exit_code = proc.exit_code;
      out->has_exit_code = 1;
    }
    if (rc == CAI_OK && proc.has_signal) {
      out->signal = proc.signal_number;
      out->has_signal = 1;
    }
    if (rc == CAI_OK) {
      rc = cai_tool_result_set_spooled(&cai_exec_result_map, out, "stdout",
                                       &capture.stdout_data, error);
    }
    if (rc == CAI_OK) {
      rc = cai_tool_result_set_spooled(&cai_exec_result_map, out, "stderr",
                                       &capture.stderr_data, error);
    }
    if (rc == CAI_OK) {
      rc = cai_tool_result_set_spooled(&cai_exec_result_map, out, "output",
                                       &capture.output, error);
    }
  }
  lonejson_spooled_cleanup(&capture.stdout_data);
  lonejson_spooled_cleanup(&capture.stderr_data);
  lonejson_spooled_cleanup(&capture.output);
  cai_free_mem(NULL, workdir);
  return rc;
}

int cai_tool_registry_register_exec_tool(cai_tool_registry *registry,
                                         const cai_exec_tool_config *config,
                                         cai_error *error) {
  cai_exec_context *ctx;
  const char *name;
  const char *description;
  int rc;

  ctx = NULL;
  rc = cai_exec_context_new(config, &ctx, error);
  if (rc != CAI_OK) {
    return rc;
  }
  name = cai_exec_default_string(config != NULL ? config->name : NULL,
                                 CAI_EXEC_DEFAULT_TOOL_NAME);
  description = cai_exec_default_string(
      config != NULL ? config->description : NULL, cai_exec_default_description);
  rc = cai_tool_registry_register_lonejson_schema_owned(
      registry, name, description, cai_exec_schema_json, 0, &cai_exec_args_map,
      &cai_exec_result_map, cai_exec_callback, ctx, cai_exec_context_cleanup,
      error);
  if (rc != CAI_OK) {
    cai_exec_context_cleanup(ctx);
  }
  return rc;
}

int cai_agent_register_exec_tool(cai_agent *agent,
                                 const cai_exec_tool_config *config,
                                 cai_error *error) {
  cai_agent_impl *impl;

  if (agent == NULL || agent->impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  impl = CAI_AGENT_IMPL(agent);
  return cai_tool_registry_register_exec_tool(impl->tools, config, error);
}
