#include "../cai_internal.h"

#include <cai/tools/terminal.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <pty.h>
#include <sys/syscall.h>
extern long syscall(long number, ...);
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include <util.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

extern char *realpath(const char *path, char *resolved_path);

#define CAI_TERMINAL_DEFAULT_YIELD_MS 10000L
#define CAI_TERMINAL_MAX_YIELD_MS 30000L
#define CAI_TERMINAL_DEFAULT_WRITE_YIELD_MS 250L
#define CAI_TERMINAL_MAX_WRITE_YIELD_MS 30000L
#define CAI_TERMINAL_MIN_POLL_YIELD_MS 5000L
#define CAI_TERMINAL_DEFAULT_POLL_YIELD_MS 5000L
#define CAI_TERMINAL_MAX_POLL_YIELD_MS 300000L
#define CAI_TERMINAL_DEFAULT_OUTPUT_MAX (3U * 1024U * 1024U)
#define CAI_TERMINAL_STDIN_WRITE_TIMEOUT_MS 1000L
#define CAI_TERMINAL_CLOSE_FD_FALLBACK_LIMIT 1048576

/*
 * The terminal is an explicit host-side capability, but it must not inherit
 * the client process environment. In particular, API credentials commonly
 * live there and tool output is returned to the model. Keep the useful,
 * deterministic shell settings here and derive HOME from the already-scoped
 * workspace below.
 */
static char cai_terminal_shell_flag[] = "-c";
static char cai_terminal_safe_path[] =
    "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
static char cai_terminal_safe_lang[] = "LANG=C.UTF-8";
static char cai_terminal_safe_lc_all[] = "LC_ALL=C.UTF-8";
static char cai_terminal_safe_term[] = "TERM=xterm-256color";
static char cai_terminal_safe_tmpdir[] = "TMPDIR=/tmp";
/* A PTY causes tools such as git to select less; never let its history file
 * become an untracked workspace artifact. */
static char cai_terminal_safe_lesshistfile[] = "LESSHISTFILE=/dev/null";

#if defined(__linux__)
typedef struct cai_terminal_linux_dirent64 {
  unsigned long long inode;
  long long offset;
  unsigned short record_length;
  unsigned char type;
  char name[1];
} cai_terminal_linux_dirent64;

static int cai_terminal_linux_fd_name(const char *name, size_t length,
                                      int *out) {
  unsigned long value;
  size_t index;

  value = 0U;
  if (name == NULL || out == NULL || length == 0U) {
    return 0;
  }
  for (index = 0U; index < length && name[index] != '\0'; index++) {
    if (name[index] < '0' || name[index] > '9' ||
        value > (unsigned long)(INT_MAX - (name[index] - '0')) / 10U) {
      return 0;
    }
    value = value * 10U + (unsigned long)(name[index] - '0');
  }
  if (index == 0U || index == length || name[index] != '\0') {
    return 0;
  }
  *out = (int)value;
  return 1;
}

/* Some supported Linux kernels predate close_range(). Enumerating procfs with
 * raw syscalls remains safe after fork and closes precisely the descriptor
 * snapshot inherited by the child, without a costly RLIMIT_NOFILE sweep. */
static int cai_terminal_linux_close_inherited_fds_proc(void) {
#if defined(SYS_openat) && defined(SYS_getdents64)
  char buffer[4096];
  int directory_fd;
  int rc;

  directory_fd = (int)syscall(SYS_openat, AT_FDCWD, "/proc/self/fd",
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
  if (directory_fd < 0) {
    return -1;
  }
  rc = 0;
  for (;;) {
    ssize_t count;
    size_t cursor;

    count =
        (ssize_t)syscall(SYS_getdents64, directory_fd, buffer, sizeof(buffer));
    if (count == 0) {
      break;
    }
    if (count < 0) {
      rc = -1;
      break;
    }
    cursor = 0U;
    while (cursor < (size_t)count) {
      cai_terminal_linux_dirent64 *entry;
      size_t name_length;
      int fd;

      if ((size_t)count - cursor <
          offsetof(cai_terminal_linux_dirent64, name)) {
        rc = -1;
        break;
      }
      entry = (cai_terminal_linux_dirent64 *)(buffer + cursor);
      if (entry->record_length <
              offsetof(cai_terminal_linux_dirent64, name) + 1U ||
          entry->record_length > (size_t)count - cursor) {
        rc = -1;
        break;
      }
      name_length =
          entry->record_length - offsetof(cai_terminal_linux_dirent64, name);
      if (cai_terminal_linux_fd_name(entry->name, name_length, &fd) &&
          fd >= 3 && fd != directory_fd) {
        (void)close(fd);
      }
      cursor += entry->record_length;
    }
    if (rc != 0) {
      break;
    }
  }
  (void)close(directory_fd);
  return rc;
#else
  return -1;
#endif
}
#endif

/* The terminal child runs model-requested code. Do not leak host sockets,
 * files, pipes, or credentials through descriptors intentionally kept
 * inheritable by the embedding process. This executes only after fork, so it
 * uses direct descriptor syscalls only. */
static void cai_terminal_close_inherited_fds(int fd_limit) {
#if !defined(__APPLE__) && !defined(__FreeBSD__)
  int fd;
#endif
#if defined(__linux__) && defined(SYS_close_range)
  if (syscall(SYS_close_range, 3U, UINT_MAX, 0U) == 0) {
    return;
  }
#endif
#if defined(__linux__)
  if (cai_terminal_linux_close_inherited_fds_proc() == 0) {
    return;
  }
#endif
#if defined(__APPLE__) || defined(__FreeBSD__)
  closefrom(3);
#else
  /* Older Linux kernels lack close_range(). An open descriptor can never be
   * trusted merely because it predates terminal setup. */
  for (fd = 3; fd < fd_limit; fd++) {
    (void)close(fd);
  }
#endif
}

#if defined(CAI_TESTING)
void cai_terminal_test_set_reader_create_failure(int enabled);
void cai_terminal_test_set_child_pre_setsid_hold(int enabled);

static volatile sig_atomic_t cai_terminal_test_reader_create_failure;
static volatile sig_atomic_t cai_terminal_test_child_pre_setsid_hold;

void cai_terminal_test_set_reader_create_failure(int enabled) {
  cai_terminal_test_reader_create_failure = enabled != 0 ? 1 : 0;
}

void cai_terminal_test_set_child_pre_setsid_hold(int enabled) {
  cai_terminal_test_child_pre_setsid_hold = enabled != 0 ? 1 : 0;
}

static void cai_terminal_test_before_setsid(void) {
  if (cai_terminal_test_child_pre_setsid_hold != 0) {
    (void)raise(SIGSTOP);
  }
}

static int cai_terminal_start_reader(pthread_t *reader, void *(*entry)(void *),
                                     void *context) {
  if (cai_terminal_test_reader_create_failure != 0) {
    return EAGAIN;
  }
  return pthread_create(reader, NULL, entry, context);
}
#else
static void cai_terminal_test_before_setsid(void) {}

static int cai_terminal_start_reader(pthread_t *reader, void *(*entry)(void *),
                                     void *context) {
  return pthread_create(reader, NULL, entry, context);
}
#endif

typedef struct cai_terminal_manager {
  pthread_mutex_t lock;
  pthread_cond_t changed;
  int refs;
  char *root_path;
  int root_fd;
  char *default_workdir;
  char *shell_path;
  char *home_environment;
  long default_yield_ms;
  long max_yield_ms;
  long default_write_yield_ms;
  long max_write_yield_ms;
  long default_poll_yield_ms;
  long max_poll_yield_ms;
  size_t output_max_bytes;
  cai_terminal_policy_fn policy;
  void *policy_context;
  cai_terminal_event_fn event_callback;
  void *event_context;
  char terminal_id[48];
  char *command;
  char *workdir;
  unsigned long long command_id;
  int pty_fd;
  pid_t pid;
  pthread_t reader;
  int reader_started;
  int starting;
  int running;
  int completed;
  int child_reaped;
  int pty_eof;
  int termination_requested;
  int completion_event_emitted;
  /* A callback retains exclusive ownership while it collects a result and
   * emits its lifecycle event. This prevents the next command from replacing
   * metadata belonging to a just-completed command. */
  int operation_active;
  int child_status;
  int child_status_known;
  char *output;
  size_t output_length;
  size_t output_capacity;
  size_t delivered_offset;
  size_t total_output_bytes;
  int output_truncated;
  struct timespec command_started_at;
} cai_terminal_manager;

typedef struct cai_terminal_binding {
  cai_terminal_manager *manager;
} cai_terminal_binding;

typedef struct cai_terminal_exec_args {
  char *cmd;
  char *workdir;
  long long yield_time_ms;
  int has_yield_time_ms;
  long long max_output_tokens;
  int has_max_output_tokens;
  int tty;
  int has_tty;
} cai_terminal_exec_args;

typedef struct cai_terminal_write_args {
  char *session_id;
  char *chars;
  long long yield_time_ms;
  int has_yield_time_ms;
  long long max_output_tokens;
  int has_max_output_tokens;
  int terminate;
  int has_terminate;
} cai_terminal_write_args;

typedef struct cai_terminal_result {
  char *session_id;
  char *output;
  int running;
  int completed;
  long long exit_code;
  int has_exit_code;
  long long signal;
  int has_signal;
  long long command_id;
  long long original_byte_count;
  int output_truncated;
  int detached_processes_possible;
  long long duration_ms;
} cai_terminal_result;

typedef struct cai_terminal_event_metadata {
  char terminal_id[48];
  unsigned long long command_id;
  char *command;
  char *workdir;
} cai_terminal_event_metadata;

static const lonejson_field cai_terminal_exec_arg_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_terminal_exec_args, cmd, "cmd"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_terminal_exec_args, workdir,
                                          "workdir"),
    LONEJSON_FIELD_I64_PRESENT(cai_terminal_exec_args, yield_time_ms,
                               has_yield_time_ms, "yield_time_ms"),
    LONEJSON_FIELD_I64_PRESENT(cai_terminal_exec_args, max_output_tokens,
                               has_max_output_tokens, "max_output_tokens"),
    LONEJSON_FIELD_BOOL_PRESENT_NULLABLE(cai_terminal_exec_args, tty, has_tty,
                                         "tty")};
LONEJSON_MAP_DEFINE(cai_terminal_exec_args_map, cai_terminal_exec_args,
                    cai_terminal_exec_arg_fields);

static const lonejson_field cai_terminal_write_arg_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_terminal_write_args, session_id,
                                    "session_id"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_terminal_write_args, chars,
                                          "chars"),
    LONEJSON_FIELD_I64_PRESENT(cai_terminal_write_args, yield_time_ms,
                               has_yield_time_ms, "yield_time_ms"),
    LONEJSON_FIELD_I64_PRESENT(cai_terminal_write_args, max_output_tokens,
                               has_max_output_tokens, "max_output_tokens"),
    LONEJSON_FIELD_BOOL_PRESENT_NULLABLE(cai_terminal_write_args, terminate,
                                         has_terminate, "terminate")};
LONEJSON_MAP_DEFINE(cai_terminal_write_args_map, cai_terminal_write_args,
                    cai_terminal_write_arg_fields);

static const lonejson_field cai_terminal_result_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_terminal_result, session_id,
                                    "session_id"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_terminal_result, output, "output"),
    LONEJSON_FIELD_BOOL_REQ(cai_terminal_result, running, "running"),
    LONEJSON_FIELD_BOOL_REQ(cai_terminal_result, completed, "completed"),
    LONEJSON_FIELD_I64_PRESENT(cai_terminal_result, exit_code, has_exit_code,
                               "exit_code"),
    LONEJSON_FIELD_I64_PRESENT(cai_terminal_result, signal, has_signal,
                               "signal"),
    LONEJSON_FIELD_I64_REQ(cai_terminal_result, command_id, "command_id"),
    LONEJSON_FIELD_I64_REQ(cai_terminal_result, original_byte_count,
                           "original_byte_count"),
    LONEJSON_FIELD_BOOL_REQ(cai_terminal_result, output_truncated,
                            "output_truncated"),
    LONEJSON_FIELD_BOOL_REQ(cai_terminal_result, detached_processes_possible,
                            "detached_processes_possible"),
    LONEJSON_FIELD_I64_REQ(cai_terminal_result, duration_ms, "duration_ms")};
LONEJSON_MAP_DEFINE(cai_terminal_result_map, cai_terminal_result,
                    cai_terminal_result_fields);

static const char cai_terminal_exec_schema[] =
    "{\"type\":\"object\",\"properties\":{\"cmd\":{\"type\":\"string\","
    "\"description\":\"Command to run in the single managed terminal.\"},"
    "\"workdir\":{\"type\":[\"string\",\"null\"]},"
    "\"yield_time_ms\":{\"type\":[\"integer\",\"null\"]},"
    "\"max_output_tokens\":{\"type\":[\"integer\",\"null\"]},"
    "\"tty\":{\"type\":[\"boolean\",\"null\"]}},\"required\":[\"cmd\"],"
    "\"additionalProperties\":false}";

static const char cai_terminal_write_schema[] =
    "{\"type\":\"object\",\"properties\":{\"session_id\":{\"type\":\"string\"},"
    "\"chars\":{\"type\":[\"string\",\"null\"]},"
    "\"yield_time_ms\":{\"type\":[\"integer\",\"null\"]},"
    "\"max_output_tokens\":{\"type\":[\"integer\",\"null\"]},"
    "\"terminate\":{\"type\":[\"boolean\",\"null\"]}},"
    "\"required\":[\"session_id\"],\"additionalProperties\":false}";

static const char cai_terminal_exec_description[] =
    "Runs one command in CAI's single managed terminal. If it is still "
    "running after yield_time_ms, use write_stdin with the returned session_id "
    "to send input, wait for output, or terminate it. Only one command can "
    "run at a time. Default configuration waits 10000 ms and caps the wait "
    "at 30000 ms; the host may configure other limits.";
static const char cai_terminal_write_description[] =
    "Writes input to, waits for, or terminates CAI's single managed terminal. "
    "Use the exact session_id returned by exec_command; empty chars polls. "
    "Default configuration gives non-empty writes a 250 ms wait capped at "
    "30000 ms, and empty polls a 5000-300000 ms wait. Termination uses the "
    "non-empty-write limits. The host may configure other limits.";

static void cai_terminal_deadline(struct timespec *deadline, long wait_ms) {
  clock_gettime(CLOCK_REALTIME, deadline);
  deadline->tv_sec += wait_ms / 1000L;
  deadline->tv_nsec += (wait_ms % 1000L) * 1000000L;
  if (deadline->tv_nsec >= 1000000000L) {
    deadline->tv_sec++;
    deadline->tv_nsec -= 1000000000L;
  }
}

static unsigned long long
cai_terminal_elapsed_ms(const struct timespec *started,
                        const struct timespec *finished) {
  time_t seconds;
  long nanoseconds;

  if (started == NULL || finished == NULL ||
      (finished->tv_sec < started->tv_sec) ||
      (finished->tv_sec == started->tv_sec &&
       finished->tv_nsec < started->tv_nsec)) {
    return 0U;
  }
  seconds = finished->tv_sec - started->tv_sec;
  nanoseconds = finished->tv_nsec - started->tv_nsec;
  if (nanoseconds < 0L) {
    seconds--;
    nanoseconds += 1000000000L;
  }
  if ((unsigned long long)seconds > ULLONG_MAX / 1000U ||
      (unsigned long long)seconds * 1000U >
          ULLONG_MAX - (unsigned long long)(nanoseconds / 1000000L)) {
    return ULLONG_MAX;
  }
  return (unsigned long long)seconds * 1000U +
         (unsigned long long)(nanoseconds / 1000000L);
}

static void cai_terminal_close_fd(int *fd) {
  if (fd != NULL && *fd >= 0) {
    close(*fd);
    *fd = -1;
  }
}

static int cai_terminal_operation_begin(cai_terminal_manager *manager,
                                        cai_error *error) {
  int active;

  pthread_mutex_lock(&manager->lock);
  active = manager->operation_active;
  if (!active) {
    manager->operation_active = 1;
  }
  pthread_mutex_unlock(&manager->lock);
  return active
             ? cai_set_error(error, CAI_ERR_INVALID,
                             "single terminal is processing another tool call")
             : CAI_OK;
}

static void cai_terminal_operation_end(cai_terminal_manager *manager) {
  pthread_mutex_lock(&manager->lock);
  manager->operation_active = 0;
  pthread_cond_broadcast(&manager->changed);
  pthread_mutex_unlock(&manager->lock);
}

static int cai_terminal_set_nonblock(int fd) {
  int flags;

  flags = fcntl(fd, F_GETFL, 0);
  return flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0 ? -1 : 0;
}

static int cai_terminal_under_root(const char *root, const char *path) {
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

static int cai_terminal_open_workdir(cai_terminal_manager *manager,
                                     const char *resolved, int *out_fd,
                                     cai_error *error) {
  char relative[PATH_MAX];
  char *component;
  char *next;
  const char *start;
  size_t root_length;
  int fd;

  *out_fd = -1;
  if (manager == NULL || manager->root_fd < 0 || resolved == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "terminal workspace descriptor is unavailable");
  }
  root_length = strlen(manager->root_path);
  if (strcmp(manager->root_path, "/") == 0) {
    start = resolved[0] == '/' ? resolved + 1U : NULL;
  } else if (strncmp(resolved, manager->root_path, root_length) == 0 &&
             (resolved[root_length] == '\0' || resolved[root_length] == '/')) {
    start = resolved + root_length;
    if (*start == '/') {
      start++;
    }
  } else {
    start = NULL;
  }
  if (start == NULL || snprintf(relative, sizeof(relative), "%s", start) >=
                           (int)sizeof(relative)) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "terminal workdir escapes workspace root");
  }
  fd = dup(manager->root_fd);
  if (fd < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
    if (fd >= 0) {
      close(fd);
    }
    return cai_set_error_detail(error, CAI_ERR_INVALID,
                                "failed to pin terminal workdir",
                                strerror(errno));
  }
  component = relative;
  while (component[0] != '\0') {
    int next_fd;
    struct stat st;

    next = strchr(component, '/');
    if (next != NULL) {
      *next = '\0';
    }
    next_fd =
        openat(fd, component, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (next_fd < 0 || fstat(next_fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
      if (next_fd >= 0) {
        close(next_fd);
      }
      close(fd);
      return cai_set_error_detail(error, CAI_ERR_INVALID,
                                  "failed to pin terminal workdir",
                                  strerror(errno));
    }
    close(fd);
    fd = next_fd;
    if (next == NULL) {
      break;
    }
    component = next + 1U;
  }
  *out_fd = fd;
  return CAI_OK;
}

static int cai_terminal_resolve_workdir(cai_terminal_manager *manager,
                                        const char *requested, char **out,
                                        int *out_fd, cai_error *error) {
  char candidate[PATH_MAX];
  char resolved[PATH_MAX];
  const char *base;
  int written;

  *out = NULL;
  *out_fd = -1;
  base = requested != NULL && requested[0] != '\0' ? requested
                                                   : manager->default_workdir;
  if (base == NULL || base[0] == '\0') {
    base = manager->root_path;
  }
  if (base[0] == '/') {
    written = snprintf(candidate, sizeof(candidate), "%s", base);
  } else {
    written = snprintf(candidate, sizeof(candidate), "%s/%s",
                       manager->root_path, base);
  }
  if (written < 0 || (size_t)written >= sizeof(candidate) ||
      realpath(candidate, resolved) == NULL ||
      !cai_terminal_under_root(manager->root_path, resolved)) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "terminal workdir must resolve below workspace root");
  }
  if (cai_terminal_open_workdir(manager, resolved, out_fd, error) != CAI_OK) {
    return error != NULL ? error->code : CAI_ERR_INVALID;
  }
  *out = cai_strdup(NULL, resolved);
  if (*out == NULL) {
    cai_terminal_close_fd(out_fd);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to copy terminal workdir");
  }
  return CAI_OK;
}

static int cai_terminal_output_append(cai_terminal_manager *manager,
                                      const char *data, size_t length) {
  size_t retained;
  size_t needed;
  char *grown;
  size_t i;

  if (length > SIZE_MAX - manager->total_output_bytes) {
    manager->total_output_bytes = SIZE_MAX;
  } else {
    manager->total_output_bytes += length;
  }
  if (manager->output_length >= manager->output_max_bytes) {
    manager->output_truncated = 1;
    return CAI_OK;
  }
  retained = length;
  if (retained > manager->output_max_bytes - manager->output_length) {
    retained = manager->output_max_bytes - manager->output_length;
    manager->output_truncated = 1;
  }
  needed = manager->output_length + retained + 1U;
  if (needed > manager->output_capacity) {
    size_t capacity;

    capacity =
        manager->output_capacity == 0U ? 4096U : manager->output_capacity;
    while (capacity < needed) {
      if (capacity > manager->output_max_bytes / 2U) {
        capacity = manager->output_max_bytes + 1U;
        break;
      }
      capacity *= 2U;
    }
    grown = (char *)cai_realloc_mem(NULL, manager->output, capacity);
    if (grown == NULL) {
      return CAI_ERR_NOMEM;
    }
    manager->output = grown;
    manager->output_capacity = capacity;
  }
  for (i = 0U; i < retained; i++) {
    manager->output[manager->output_length + i] =
        data[i] == '\0' ? '?' : data[i];
  }
  manager->output_length += retained;
  manager->output[manager->output_length] = '\0';
  return CAI_OK;
}

static void *cai_terminal_reader(void *value) {
  cai_terminal_manager *manager;
  char buffer[8192];

  manager = (cai_terminal_manager *)value;
  for (;;) {
    ssize_t count;
    int status;
    pid_t waited;

    count = read(manager->pty_fd, buffer, sizeof(buffer));
    if (count > 0) {
      pthread_mutex_lock(&manager->lock);
      if (cai_terminal_output_append(manager, buffer, (size_t)count) !=
          CAI_OK) {
        /* Keep supervising the command, but never report dropped PTY bytes
         * as a complete capture when output retention runs out of memory. */
        manager->output_truncated = 1;
      }
      pthread_cond_broadcast(&manager->changed);
      pthread_mutex_unlock(&manager->lock);
    } else if (count == 0 || (count < 0 && errno == EIO)) {
      pthread_mutex_lock(&manager->lock);
      manager->pty_eof = 1;
      pthread_cond_broadcast(&manager->changed);
      pthread_mutex_unlock(&manager->lock);
    }
    waited = waitpid(manager->pid, &status, WNOHANG);
    if (waited == manager->pid) {
      pthread_mutex_lock(&manager->lock);
      manager->child_reaped = 1;
      manager->child_status = status;
      manager->child_status_known = 1;
      pthread_cond_broadcast(&manager->changed);
      pthread_mutex_unlock(&manager->lock);
    } else if (waited < 0 && errno == ECHILD) {
      /* Hosts may ignore SIGCHLD or reap it in their own handler. The command
       * has still completed, but its exit status is no longer available. */
      pthread_mutex_lock(&manager->lock);
      manager->child_reaped = 1;
      manager->child_status_known = 0;
      pthread_cond_broadcast(&manager->changed);
      pthread_mutex_unlock(&manager->lock);
    }
    pthread_mutex_lock(&manager->lock);
    /*
     * The supervised shell is the command completion authority.  A command
     * may intentionally daemonize a descendant which retains the slave PTY;
     * waiting for EOF in that case would report a completed command as
     * perpetually running.  Close the master once the shell has been reaped,
     * after draining bytes already available in this iteration.  The result
     * explicitly reports that detached descendants may remain.
     */
    if (manager->child_reaped) {
      /*
       * A nonblocking read may have returned one full buffer before waitpid
       * observes a short-lived shell exit. Drain all bytes already queued on
       * the PTY before declaring completion; otherwise a fast command can
       * lose its final output while still being reported as complete.
       */
      pthread_mutex_unlock(&manager->lock);
      for (;;) {
        count = read(manager->pty_fd, buffer, sizeof(buffer));
        if (count > 0) {
          pthread_mutex_lock(&manager->lock);
          if (cai_terminal_output_append(manager, buffer, (size_t)count) !=
              CAI_OK) {
            /* See the main read path above: completion remains valid, but
             * its output must be marked as incomplete. */
            manager->output_truncated = 1;
          }
          pthread_cond_broadcast(&manager->changed);
          pthread_mutex_unlock(&manager->lock);
          continue;
        }
        if (count == 0 || (count < 0 && errno == EIO)) {
          pthread_mutex_lock(&manager->lock);
          manager->pty_eof = 1;
          pthread_mutex_unlock(&manager->lock);
        }
        if (count < 0 && errno == EINTR) {
          continue;
        }
        break;
      }
      pthread_mutex_lock(&manager->lock);
      manager->running = 0;
      manager->completed = 1;
      cai_terminal_close_fd(&manager->pty_fd);
      pthread_cond_broadcast(&manager->changed);
      pthread_mutex_unlock(&manager->lock);
      break;
    }
    pthread_mutex_unlock(&manager->lock);
    if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
        errno != EINTR) {
      pthread_mutex_lock(&manager->lock);
      manager->pty_eof = 1;
      pthread_mutex_unlock(&manager->lock);
    }
    {
      struct timespec pause_time;

      pause_time.tv_sec = 0;
      pause_time.tv_nsec = 10000000L;
      nanosleep(&pause_time, NULL);
    }
  }
  return NULL;
}

static void cai_terminal_manager_destroy(cai_terminal_manager *manager) {
  if (manager == NULL) {
    return;
  }
  pthread_mutex_lock(&manager->lock);
  if (manager->running && manager->pid > 0) {
    /* The parent marks the command running before the child has necessarily
     * completed setsid(). If its process group is not available yet, target
     * the known child directly so reader teardown cannot wait on its shell. */
    if (kill(-manager->pid, SIGKILL) != 0 && errno == ESRCH) {
      (void)kill(manager->pid, SIGKILL);
    }
  }
  pthread_mutex_unlock(&manager->lock);
  if (manager->reader_started) {
    pthread_join(manager->reader, NULL);
  }
  cai_terminal_close_fd(&manager->pty_fd);
  cai_terminal_close_fd(&manager->root_fd);
  cai_free_mem(NULL, manager->root_path);
  cai_free_mem(NULL, manager->default_workdir);
  cai_free_mem(NULL, manager->shell_path);
  cai_free_mem(NULL, manager->home_environment);
  cai_free_mem(NULL, manager->command);
  cai_free_mem(NULL, manager->workdir);
  cai_free_mem(NULL, manager->output);
  pthread_cond_destroy(&manager->changed);
  pthread_mutex_destroy(&manager->lock);
  cai_free_mem(NULL, manager);
}

static void cai_terminal_binding_cleanup(void *value) {
  cai_terminal_binding *binding;
  cai_terminal_manager *manager;
  int destroy;

  binding = (cai_terminal_binding *)value;
  if (binding == NULL) {
    return;
  }
  manager = binding->manager;
  destroy = 0;
  if (manager != NULL) {
    pthread_mutex_lock(&manager->lock);
    manager->refs--;
    destroy = manager->refs == 0;
    pthread_mutex_unlock(&manager->lock);
  }
  cai_free_mem(NULL, binding);
  if (destroy) {
    cai_terminal_manager_destroy(manager);
  }
}

static int cai_terminal_manager_new(const cai_terminal_tool_config *config,
                                    cai_terminal_manager **out,
                                    cai_error *error) {
  cai_terminal_manager *manager;
  char resolved[PATH_MAX];
  size_t home_length;

  *out = NULL;
  if (config == NULL || config->root_path == NULL ||
      config->root_path[0] == '\0' ||
      realpath(config->root_path, resolved) == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "terminal tools require a valid workspace root");
  }
  manager = (cai_terminal_manager *)cai_alloc(NULL, sizeof(*manager));
  if (manager == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate terminal manager");
  }
  memset(manager, 0, sizeof(*manager));
  manager->pty_fd = -1;
  manager->root_fd = -1;
  manager->root_path = cai_strdup(NULL, resolved);
  if (manager->root_path != NULL) {
    manager->root_fd =
        open(resolved, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  }
  manager->default_workdir = config->default_workdir != NULL
                                 ? cai_strdup(NULL, config->default_workdir)
                                 : NULL;
  manager->shell_path = cai_strdup(
      NULL, config->shell_path != NULL ? config->shell_path : "/bin/sh");
  home_length = strlen("HOME=") + strlen(resolved) + 1U;
  manager->home_environment = (char *)cai_alloc(NULL, home_length);
  if (manager->home_environment != NULL) {
    (void)snprintf(manager->home_environment, home_length, "HOME=%s", resolved);
  }
  manager->default_yield_ms = config->default_yield_time_ms > 0L
                                  ? config->default_yield_time_ms
                                  : CAI_TERMINAL_DEFAULT_YIELD_MS;
  manager->max_yield_ms = config->max_yield_time_ms > 0L
                              ? config->max_yield_time_ms
                              : CAI_TERMINAL_MAX_YIELD_MS;
  manager->default_write_yield_ms = config->default_write_yield_time_ms > 0L
                                        ? config->default_write_yield_time_ms
                                        : CAI_TERMINAL_DEFAULT_WRITE_YIELD_MS;
  manager->max_write_yield_ms = config->max_write_yield_time_ms > 0L
                                    ? config->max_write_yield_time_ms
                                    : CAI_TERMINAL_MAX_WRITE_YIELD_MS;
  manager->default_poll_yield_ms = config->default_poll_yield_time_ms > 0L
                                       ? config->default_poll_yield_time_ms
                                       : CAI_TERMINAL_DEFAULT_POLL_YIELD_MS;
  manager->max_poll_yield_ms = config->max_poll_yield_time_ms > 0L
                                   ? config->max_poll_yield_time_ms
                                   : CAI_TERMINAL_MAX_POLL_YIELD_MS;
  if (manager->max_poll_yield_ms < CAI_TERMINAL_MIN_POLL_YIELD_MS) {
    manager->max_poll_yield_ms = CAI_TERMINAL_MIN_POLL_YIELD_MS;
  }
  manager->output_max_bytes = config->output_max_bytes != 0U
                                  ? config->output_max_bytes
                                  : CAI_TERMINAL_DEFAULT_OUTPUT_MAX;
  manager->policy = config->policy;
  manager->policy_context = config->policy_context;
  manager->event_callback = config->event_callback;
  manager->event_context = config->event_context;
  snprintf(manager->terminal_id, sizeof(manager->terminal_id), "terminal-1");
  if (manager->root_path == NULL || manager->root_fd < 0 ||
      manager->shell_path == NULL || manager->home_environment == NULL ||
      manager->max_yield_ms <= 0L || manager->max_write_yield_ms <= 0L ||
      manager->max_poll_yield_ms <= 0L || manager->output_max_bytes == 0U) {
    cai_free_mem(NULL, manager->root_path);
    cai_terminal_close_fd(&manager->root_fd);
    cai_free_mem(NULL, manager->default_workdir);
    cai_free_mem(NULL, manager->shell_path);
    cai_free_mem(NULL, manager->home_environment);
    cai_free_mem(NULL, manager);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate terminal manager");
  }
  if (pthread_mutex_init(&manager->lock, NULL) != 0) {
    cai_free_mem(NULL, manager->root_path);
    cai_terminal_close_fd(&manager->root_fd);
    cai_free_mem(NULL, manager->default_workdir);
    cai_free_mem(NULL, manager->shell_path);
    cai_free_mem(NULL, manager->home_environment);
    cai_free_mem(NULL, manager);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to initialize terminal manager lock");
  }
  if (pthread_cond_init(&manager->changed, NULL) != 0) {
    pthread_mutex_destroy(&manager->lock);
    cai_free_mem(NULL, manager->root_path);
    cai_terminal_close_fd(&manager->root_fd);
    cai_free_mem(NULL, manager->default_workdir);
    cai_free_mem(NULL, manager->shell_path);
    cai_free_mem(NULL, manager->home_environment);
    cai_free_mem(NULL, manager);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to initialize terminal manager condition");
  }
  *out = manager;
  return CAI_OK;
}

static int cai_terminal_wait(cai_terminal_manager *manager, size_t initial,
                             long wait_ms) {
  struct timespec deadline;

  cai_terminal_deadline(&deadline, wait_ms);
  pthread_mutex_lock(&manager->lock);
  while (manager->running && manager->output_length == initial) {
    if (pthread_cond_timedwait(&manager->changed, &manager->lock, &deadline) !=
        0) {
      break;
    }
  }
  pthread_mutex_unlock(&manager->lock);
  return CAI_OK;
}

static long cai_terminal_clamp_yield(long long requested, int has_requested,
                                     long default_yield_ms, long max_yield_ms,
                                     long min_yield_ms) {
  long long value;

  value = has_requested ? (requested < 0LL ? 0LL : requested)
                        : (long long)default_yield_ms;
  if (value < (long long)min_yield_ms) {
    value = (long long)min_yield_ms;
  }
  if (value > (long long)max_yield_ms) {
    value = (long long)max_yield_ms;
  }
  return (long)value;
}

static int cai_terminal_start(cai_terminal_manager *manager, const char *cmd,
                              const char *workdir, int workdir_fd, int tty,
                              cai_error *error) {
  int master;
  int slave;
  int join_reader;
  int inherited_fd_limit;
  pid_t pid;
  pthread_t reader;
  char *command_copy;
  char *workdir_copy;

  (void)tty;
  master = -1;
  slave = -1;
  inherited_fd_limit = CAI_TERMINAL_CLOSE_FD_FALLBACK_LIMIT;
  memset(&reader, 0, sizeof(reader));
  command_copy = cai_strdup(NULL, cmd);
  workdir_copy = cai_strdup(NULL, workdir);
  if (command_copy == NULL || workdir_copy == NULL) {
    cai_free_mem(NULL, command_copy);
    cai_free_mem(NULL, workdir_copy);
    cai_terminal_close_fd(&workdir_fd);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to copy terminal command metadata");
  }
  if (workdir_fd < 0) {
    cai_free_mem(NULL, command_copy);
    cai_free_mem(NULL, workdir_copy);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "terminal workdir descriptor is required");
  }
  pthread_mutex_lock(&manager->lock);
  if (manager->starting || manager->running) {
    pthread_mutex_unlock(&manager->lock);
    cai_free_mem(NULL, command_copy);
    cai_free_mem(NULL, workdir_copy);
    cai_terminal_close_fd(&workdir_fd);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "single terminal already has a running command");
  }
  join_reader = manager->reader_started;
  if (join_reader) {
    reader = manager->reader;
  }
  /* Reserve the only slot before opening a PTY or forking.  Calls can arrive
   * concurrently through independently dispatched tools. */
  manager->starting = 1;
  pthread_mutex_unlock(&manager->lock);
  if (join_reader) {
    pthread_join(reader, NULL);
    pthread_mutex_lock(&manager->lock);
    manager->reader_started = 0;
    pthread_mutex_unlock(&manager->lock);
  }
  if (openpty(&master, &slave, NULL, NULL, NULL) != 0 ||
      cai_terminal_set_nonblock(master) != 0) {
    cai_terminal_close_fd(&master);
    cai_terminal_close_fd(&slave);
    pthread_mutex_lock(&manager->lock);
    manager->starting = 0;
    pthread_cond_broadcast(&manager->changed);
    pthread_mutex_unlock(&manager->lock);
    cai_free_mem(NULL, command_copy);
    cai_free_mem(NULL, workdir_copy);
    cai_terminal_close_fd(&workdir_fd);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to create terminal PTY",
                                strerror(errno));
  }
  {
    struct rlimit descriptor_limit;

    if (getrlimit(RLIMIT_NOFILE, &descriptor_limit) == 0 &&
        descriptor_limit.rlim_cur < (rlim_t)INT_MAX) {
      inherited_fd_limit = (int)descriptor_limit.rlim_cur;
    }
  }
  pid = fork();
  if (pid < 0) {
    cai_terminal_close_fd(&master);
    cai_terminal_close_fd(&slave);
    pthread_mutex_lock(&manager->lock);
    manager->starting = 0;
    pthread_cond_broadcast(&manager->changed);
    pthread_mutex_unlock(&manager->lock);
    cai_free_mem(NULL, command_copy);
    cai_free_mem(NULL, workdir_copy);
    cai_terminal_close_fd(&workdir_fd);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to fork terminal command",
                                strerror(errno));
  }
  if (pid == 0) {
    char *argv[4];
    char *environment[8];

    argv[0] = manager->shell_path;
    argv[1] = cai_terminal_shell_flag;
    argv[2] = command_copy;
    argv[3] = NULL;
    environment[0] = manager->home_environment;
    environment[1] = cai_terminal_safe_path;
    environment[2] = cai_terminal_safe_lang;
    environment[3] = cai_terminal_safe_lc_all;
    environment[4] = cai_terminal_safe_term;
    environment[5] = cai_terminal_safe_tmpdir;
    environment[6] = cai_terminal_safe_lesshistfile;
    environment[7] = NULL;

    cai_terminal_test_before_setsid();
    (void)setsid();
    (void)ioctl(slave, TIOCSCTTY, 0);
    dup2(slave, STDIN_FILENO);
    dup2(slave, STDOUT_FILENO);
    dup2(slave, STDERR_FILENO);
    close(master);
    if (slave > STDERR_FILENO) {
      close(slave);
    }
    if (fchdir(workdir_fd) != 0) {
      _exit(126);
    }
    close(workdir_fd);
    cai_terminal_close_inherited_fds(inherited_fd_limit);
    execve(manager->shell_path, argv, environment);
    _exit(127);
  }
  close(workdir_fd);
  close(slave);
  pthread_mutex_lock(&manager->lock);
  manager->pty_fd = master;
  manager->pid = pid;
  cai_free_mem(NULL, manager->command);
  cai_free_mem(NULL, manager->workdir);
  manager->command = command_copy;
  manager->workdir = workdir_copy;
  manager->command_id++;
  manager->running = 1;
  manager->completed = 0;
  manager->child_reaped = 0;
  manager->pty_eof = 0;
  manager->child_status = 0;
  manager->child_status_known = 0;
  manager->termination_requested = 0;
  manager->completion_event_emitted = 0;
  manager->output_length = 0U;
  manager->delivered_offset = 0U;
  manager->total_output_bytes = 0U;
  manager->output_truncated = 0;
  (void)clock_gettime(CLOCK_MONOTONIC, &manager->command_started_at);
  if (manager->output != NULL) {
    manager->output[0] = '\0';
  }
  pthread_mutex_unlock(&manager->lock);
  if (cai_terminal_start_reader(&manager->reader, cai_terminal_reader,
                                manager) != 0) {
    /* Child setup has not necessarily reached setsid(), so its process group
     * may not exist yet. Kill the known child in that case before waitpid. */
    if (kill(-pid, SIGKILL) != 0 && errno == ESRCH) {
      (void)kill(pid, SIGKILL);
    }
    (void)waitpid(pid, NULL, 0);
    pthread_mutex_lock(&manager->lock);
    cai_terminal_close_fd(&manager->pty_fd);
    manager->running = 0;
    manager->starting = 0;
    pthread_cond_broadcast(&manager->changed);
    pthread_mutex_unlock(&manager->lock);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to start terminal output reader");
  }
  pthread_mutex_lock(&manager->lock);
  manager->reader_started = 1;
  manager->starting = 0;
  pthread_cond_broadcast(&manager->changed);
  pthread_mutex_unlock(&manager->lock);
  return CAI_OK;
}

static int cai_terminal_fill_result(cai_terminal_manager *manager,
                                    size_t output_limit,
                                    cai_terminal_result *result,
                                    cai_error *error) {
  size_t available;
  size_t count;
  char *output;
  struct timespec now;

  memset(result, 0, sizeof(*result));
  pthread_mutex_lock(&manager->lock);
  available = manager->output_length - manager->delivered_offset;
  count = available;
  if (output_limit > 0U && count > output_limit) {
    count = output_limit;
  }
  output = (char *)cai_alloc(NULL, count + 1U);
  if (output != NULL && count > 0U) {
    memcpy(output, manager->output + manager->delivered_offset, count);
  }
  if (output != NULL) {
    output[count] = '\0';
  }
  result->session_id = cai_tool_result_strdup(manager->terminal_id, error);
  result->output = output;
  result->running = manager->running;
  result->completed = manager->completed;
  result->command_id = (long long)manager->command_id;
  result->original_byte_count = manager->total_output_bytes > (size_t)LLONG_MAX
                                    ? LLONG_MAX
                                    : (long long)manager->total_output_bytes;
  result->output_truncated = manager->output_truncated || count < available;
  /* CAI supervises the shell process, not arbitrary descendants that may
   * have escaped its process group.  Never promise those descendants exited. */
  result->detached_processes_possible = manager->completed;
  (void)clock_gettime(CLOCK_MONOTONIC, &now);
  {
    unsigned long long duration;

    duration = cai_terminal_elapsed_ms(&manager->command_started_at, &now);
    result->duration_ms = duration > (unsigned long long)LLONG_MAX
                              ? LLONG_MAX
                              : (long long)duration;
  }
  if (manager->completed && manager->child_status_known &&
      WIFEXITED(manager->child_status)) {
    result->exit_code = WEXITSTATUS(manager->child_status);
    result->has_exit_code = 1;
  } else if (manager->completed && manager->child_status_known &&
             WIFSIGNALED(manager->child_status)) {
    result->signal = WTERMSIG(manager->child_status);
    result->has_signal = 1;
  }
  /* Do not acknowledge output until the complete result is durable for the
   * caller. A later poll must be able to retry after an allocation failure. */
  if (result->session_id != NULL && result->output != NULL) {
    manager->delivered_offset += count;
  }
  pthread_mutex_unlock(&manager->lock);
  if (result->session_id == NULL || result->output == NULL) {
    cai_free_mem(NULL, result->session_id);
    cai_free_mem(NULL, result->output);
    memset(result, 0, sizeof(*result));
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to copy terminal command result");
  }
  return CAI_OK;
}

static int cai_terminal_emit(cai_terminal_manager *manager, int type,
                             const cai_terminal_result *result,
                             cai_error *error) {
  cai_terminal_event event;

  if (manager->event_callback == NULL) {
    return CAI_OK;
  }
  memset(&event, 0, sizeof(event));
  event.type = type;
  event.terminal_id = manager->terminal_id;
  event.command_id = manager->command_id;
  event.command = manager->command;
  event.workdir = manager->workdir;
  if (result != NULL) {
    event.output = result->output;
    event.output_length = result->output != NULL ? strlen(result->output) : 0U;
    event.has_exit_code = result->has_exit_code;
    event.exit_code = result->exit_code;
    event.has_signal = result->has_signal;
    event.signal = result->signal;
    event.duration_ms = (unsigned long long)result->duration_ms;
    event.total_output_bytes = (unsigned long long)result->original_byte_count;
    event.output_truncated = result->output_truncated;
    event.detached_processes_possible = result->detached_processes_possible;
  }
  return manager->event_callback(manager->event_context, &event, error);
}

static void
cai_terminal_event_metadata_cleanup(cai_terminal_event_metadata *metadata) {
  if (metadata == NULL) {
    return;
  }
  cai_free_mem(NULL, metadata->command);
  cai_free_mem(NULL, metadata->workdir);
  memset(metadata, 0, sizeof(*metadata));
}

static int cai_terminal_event_metadata_capture_locked(
    const cai_terminal_manager *manager, cai_terminal_event_metadata *metadata,
    cai_error *error) {
  memset(metadata, 0, sizeof(*metadata));
  (void)snprintf(metadata->terminal_id, sizeof(metadata->terminal_id), "%s",
                 manager->terminal_id);
  metadata->command_id = manager->command_id;
  metadata->command =
      cai_strdup(NULL, manager->command != NULL ? manager->command : "");
  metadata->workdir =
      cai_strdup(NULL, manager->workdir != NULL ? manager->workdir : "");
  if (metadata->command == NULL || metadata->workdir == NULL) {
    cai_terminal_event_metadata_cleanup(metadata);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to snapshot terminal command metadata");
  }
  return CAI_OK;
}

static int cai_terminal_emit_metadata(
    cai_terminal_manager *manager, int type, const cai_terminal_result *result,
    const cai_terminal_event_metadata *metadata, cai_error *error) {
  cai_terminal_event event;

  if (manager->event_callback == NULL) {
    return CAI_OK;
  }
  memset(&event, 0, sizeof(event));
  event.type = type;
  event.terminal_id = metadata->terminal_id;
  event.command_id = metadata->command_id;
  event.command = metadata->command;
  event.workdir = metadata->workdir;
  if (result != NULL) {
    event.output = result->output;
    event.output_length = result->output != NULL ? strlen(result->output) : 0U;
    event.has_exit_code = result->has_exit_code;
    event.exit_code = result->exit_code;
    event.has_signal = result->has_signal;
    event.signal = result->signal;
    event.duration_ms = (unsigned long long)result->duration_ms;
    event.total_output_bytes = (unsigned long long)result->original_byte_count;
    event.output_truncated = result->output_truncated;
    event.detached_processes_possible = result->detached_processes_possible;
  }
  return manager->event_callback(manager->event_context, &event, error);
}

static int cai_terminal_emit_completion_once(cai_terminal_manager *manager,
                                             const cai_terminal_result *result,
                                             cai_error *error) {
  cai_terminal_event_metadata metadata;
  int type;
  int emit;
  int rc;

  emit = 0;
  memset(&metadata, 0, sizeof(metadata));
  type = CAI_TERMINAL_EVENT_COMMAND_COMPLETED;
  pthread_mutex_lock(&manager->lock);
  if (manager->completed && !manager->completion_event_emitted) {
    rc = cai_terminal_event_metadata_capture_locked(manager, &metadata, error);
    if (rc == CAI_OK) {
      manager->completion_event_emitted = 1;
      emit = 1;
      if (manager->termination_requested) {
        type = CAI_TERMINAL_EVENT_COMMAND_CANCELLED;
      }
    }
  } else {
    rc = CAI_OK;
  }
  pthread_mutex_unlock(&manager->lock);
  if (rc != CAI_OK) {
    return rc;
  }
  if (emit) {
    rc = cai_terminal_emit_metadata(manager, type, result, &metadata, error);
  }
  cai_terminal_event_metadata_cleanup(&metadata);
  return rc;
}

static int cai_terminal_exec_callback(void *value, const void *params,
                                      void *out, cai_error *error) {
  cai_terminal_binding *binding;
  const cai_terminal_exec_args *args;
  cai_terminal_result *result;
  char *workdir;
  long wait_ms;
  size_t output_limit;
  int rc;
  int workdir_fd;
  int operation_started;

  binding = (cai_terminal_binding *)value;
  args = (const cai_terminal_exec_args *)params;
  result = (cai_terminal_result *)out;
  if (binding == NULL || binding->manager == NULL || args == NULL ||
      args->cmd == NULL || args->cmd[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "terminal exec command is required");
  }
  workdir = NULL;
  workdir_fd = -1;
  operation_started = 0;
  rc = cai_terminal_resolve_workdir(binding->manager, args->workdir, &workdir,
                                    &workdir_fd, error);
  if (rc == CAI_OK && binding->manager->policy != NULL) {
    rc = binding->manager->policy(binding->manager->policy_context, args->cmd,
                                  binding->manager->root_path, workdir,
                                  args->has_tty && args->tty, error);
  }
  if (rc == CAI_OK) {
    rc = cai_terminal_operation_begin(binding->manager, error);
    operation_started = rc == CAI_OK;
  }
  if (rc == CAI_OK) {
    rc = cai_terminal_start(binding->manager, args->cmd, workdir, workdir_fd,
                            args->has_tty && args->tty, error);
    workdir_fd = -1;
  }
  cai_terminal_close_fd(&workdir_fd);
  cai_free_mem(NULL, workdir);
  if (rc != CAI_OK) {
    if (operation_started) {
      cai_terminal_operation_end(binding->manager);
    }
    return rc;
  }
  rc = cai_terminal_emit(binding->manager, CAI_TERMINAL_EVENT_COMMAND_STARTED,
                         NULL, error);
  if (rc == CAI_OK) {
    wait_ms = cai_terminal_clamp_yield(
        args->yield_time_ms, args->has_yield_time_ms,
        binding->manager->default_yield_ms, binding->manager->max_yield_ms, 0L);
    output_limit = args->has_max_output_tokens && args->max_output_tokens > 0LL
                       ? (size_t)args->max_output_tokens
                       : 0U;
    (void)cai_terminal_wait(binding->manager, 0U, wait_ms);
    rc =
        cai_terminal_fill_result(binding->manager, output_limit, result, error);
    if (rc == CAI_OK) {
      rc = result->completed
               ? cai_terminal_emit_completion_once(binding->manager, result,
                                                   error)
               : cai_terminal_emit(binding->manager,
                                   result->output[0] != '\0'
                                       ? CAI_TERMINAL_EVENT_OUTPUT
                                       : CAI_TERMINAL_EVENT_WAITING,
                                   result, error);
    }
  }
  cai_terminal_operation_end(binding->manager);
  return rc;
}

static void cai_terminal_send_signal(cai_terminal_manager *manager,
                                     int signal) {
  pthread_mutex_lock(&manager->lock);
  if (manager->running && manager->pid > 0) {
    if (kill(-manager->pid, signal) != 0 && errno == ESRCH) {
      /* The child publishes only after fork, but it may not yet have run
       * setsid(). In that short window its future process group does not
       * exist, so signal the child directly before it can exec the shell. */
      (void)kill(manager->pid, signal);
    }
  }
  pthread_mutex_unlock(&manager->lock);
}

static int cai_terminal_write_all(int fd, const char *data, cai_error *error) {
  size_t offset;
  size_t length;
  struct timespec started_at;

  offset = 0U;
  length = strlen(data);
  (void)clock_gettime(CLOCK_MONOTONIC, &started_at);
  while (offset < length) {
    ssize_t written;

    written = write(fd, data + offset, length - offset);
    if (written > 0) {
      offset += (size_t)written;
    } else if (written < 0 && errno == EINTR) {
      continue;
    } else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      struct pollfd poll_fd;
      struct timespec now;
      long long elapsed_ms;
      int poll_rc;

      (void)clock_gettime(CLOCK_MONOTONIC, &now);
      elapsed_ms = (long long)(now.tv_sec - started_at.tv_sec) * 1000LL +
                   (long long)(now.tv_nsec - started_at.tv_nsec) / 1000000LL;
      if (elapsed_ms >= CAI_TERMINAL_STDIN_WRITE_TIMEOUT_MS) {
        return cai_set_error(error, CAI_ERR_TRANSPORT,
                             "terminal stdin stopped accepting input");
      }
      poll_fd.fd = fd;
      poll_fd.events = POLLOUT;
      poll_fd.revents = 0;
      poll_rc = poll(&poll_fd, 1U,
                     (int)(CAI_TERMINAL_STDIN_WRITE_TIMEOUT_MS - elapsed_ms));
      if (poll_rc < 0 && errno != EINTR) {
        return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                    "failed to wait for terminal stdin",
                                    strerror(errno));
      }
    } else {
      return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                  "failed to write terminal stdin",
                                  strerror(errno));
    }
  }
  return CAI_OK;
}

static int cai_terminal_write_callback(void *value, const void *params,
                                       void *out, cai_error *error) {
  cai_terminal_binding *binding;
  const cai_terminal_write_args *args;
  cai_terminal_result *result;
  long wait_ms;
  size_t output_limit;
  size_t initial;
  int fd;
  int rc;

  binding = (cai_terminal_binding *)value;
  args = (const cai_terminal_write_args *)params;
  result = (cai_terminal_result *)out;
  if (binding == NULL || binding->manager == NULL || args == NULL ||
      args->session_id == NULL ||
      strcmp(args->session_id, binding->manager->terminal_id) != 0) {
    return cai_set_error(
        error, CAI_ERR_INVALID,
        "write_stdin session_id does not match the single terminal");
  }
  rc = cai_terminal_operation_begin(binding->manager, error);
  if (rc != CAI_OK) {
    return rc;
  }
  pthread_mutex_lock(&binding->manager->lock);
  if (!binding->manager->running) {
    int completed;

    completed = binding->manager->completed;
    pthread_mutex_unlock(&binding->manager->lock);
    if (completed) {
      output_limit =
          args->has_max_output_tokens && args->max_output_tokens > 0LL
              ? (size_t)args->max_output_tokens
              : 0U;
      rc = cai_terminal_fill_result(binding->manager, output_limit, result,
                                    error);
      if (rc == CAI_OK) {
        rc = cai_terminal_emit_completion_once(binding->manager, result, error);
      }
      cai_terminal_operation_end(binding->manager);
      return rc;
    }
    cai_terminal_operation_end(binding->manager);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "single terminal has no running command");
  }
  initial = binding->manager->output_length;
  fd = fcntl(binding->manager->pty_fd, F_DUPFD_CLOEXEC, 3);
  if (fd < 0) {
    pthread_mutex_unlock(&binding->manager->lock);
    cai_terminal_operation_end(binding->manager);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to pin terminal stdin",
                                strerror(errno));
  }
  pthread_mutex_unlock(&binding->manager->lock);
  if (args->chars != NULL && args->chars[0] != '\0') {
    rc = cai_terminal_write_all(fd, args->chars, error);
    if (rc != CAI_OK) {
      cai_terminal_close_fd(&fd);
      cai_terminal_operation_end(binding->manager);
      return rc;
    }
  }
  cai_terminal_close_fd(&fd);
  if ((args->has_terminate && args->terminate) ||
      (args->chars != NULL && args->chars[0] != '\0')) {
    wait_ms =
        cai_terminal_clamp_yield(args->yield_time_ms, args->has_yield_time_ms,
                                 binding->manager->default_write_yield_ms,
                                 binding->manager->max_write_yield_ms, 0L);
  } else {
    wait_ms = cai_terminal_clamp_yield(
        args->yield_time_ms, args->has_yield_time_ms,
        binding->manager->default_poll_yield_ms,
        binding->manager->max_poll_yield_ms, CAI_TERMINAL_MIN_POLL_YIELD_MS);
  }
  if (args->has_terminate && args->terminate) {
    pthread_mutex_lock(&binding->manager->lock);
    binding->manager->termination_requested = 1;
    pthread_mutex_unlock(&binding->manager->lock);
    cai_terminal_send_signal(binding->manager, SIGINT);
  }
  (void)cai_terminal_wait(binding->manager, initial, wait_ms);
  if (args->has_terminate && args->terminate) {
    pthread_mutex_lock(&binding->manager->lock);
    if (binding->manager->running) {
      pthread_mutex_unlock(&binding->manager->lock);
      cai_terminal_send_signal(binding->manager, SIGTERM);
      (void)cai_terminal_wait(binding->manager, initial, 250L);
      pthread_mutex_lock(&binding->manager->lock);
      if (binding->manager->running) {
        pthread_mutex_unlock(&binding->manager->lock);
        cai_terminal_send_signal(binding->manager, SIGKILL);
        (void)cai_terminal_wait(binding->manager, initial, 250L);
      } else {
        pthread_mutex_unlock(&binding->manager->lock);
      }
    } else {
      pthread_mutex_unlock(&binding->manager->lock);
    }
  }
  output_limit = args->has_max_output_tokens && args->max_output_tokens > 0LL
                     ? (size_t)args->max_output_tokens
                     : 0U;
  rc = cai_terminal_fill_result(binding->manager, output_limit, result, error);
  if (rc == CAI_OK) {
    rc = result->completed ? cai_terminal_emit_completion_once(binding->manager,
                                                               result, error)
                           : cai_terminal_emit(binding->manager,
                                               result->output[0] != '\0'
                                                   ? CAI_TERMINAL_EVENT_OUTPUT
                                                   : CAI_TERMINAL_EVENT_WAITING,
                                               result, error);
  }
  cai_terminal_operation_end(binding->manager);
  return rc;
}

static int cai_terminal_binding_new(cai_terminal_manager *manager,
                                    cai_terminal_binding **out,
                                    cai_error *error) {
  cai_terminal_binding *binding;

  binding = (cai_terminal_binding *)cai_alloc(NULL, sizeof(*binding));
  if (binding == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate terminal tool binding");
  }
  binding->manager = manager;
  pthread_mutex_lock(&manager->lock);
  manager->refs++;
  pthread_mutex_unlock(&manager->lock);
  *out = binding;
  return CAI_OK;
}

int cai_tool_registry_register_terminal_tools(
    cai_tool_registry *registry, const cai_terminal_tool_config *config,
    cai_error *error) {
  cai_terminal_manager *manager;
  cai_terminal_binding *exec_binding;
  cai_terminal_binding *write_binding;
  size_t registry_count;
  int released_local_binding;
  int rc;

  if (registry == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "terminal tool registry is required");
  }
  manager = NULL;
  exec_binding = NULL;
  write_binding = NULL;
  registry_count = cai_tool_registry_count(registry);
  released_local_binding = 0;
  rc = cai_terminal_manager_new(config, &manager, error);
  if (rc == CAI_OK) {
    rc = cai_terminal_binding_new(manager, &exec_binding, error);
  }
  if (rc == CAI_OK) {
    rc = cai_terminal_binding_new(manager, &write_binding, error);
  }
  if (rc == CAI_OK) {
    rc = cai_tool_registry_register_lonejson_schema_owned(
        registry, CAI_TERMINAL_EXEC_TOOL_NAME, cai_terminal_exec_description,
        cai_terminal_exec_schema, 0, &cai_terminal_exec_args_map,
        &cai_terminal_result_map, cai_terminal_exec_callback, exec_binding,
        cai_terminal_binding_cleanup, error);
    if (rc == CAI_OK) {
      exec_binding = NULL;
      rc = cai_tool_registry_register_lonejson_schema_owned(
          registry, CAI_TERMINAL_WRITE_TOOL_NAME,
          cai_terminal_write_description, cai_terminal_write_schema, 0,
          &cai_terminal_write_args_map, &cai_terminal_result_map,
          cai_terminal_write_callback, write_binding,
          cai_terminal_binding_cleanup, error);
      if (rc == CAI_OK) {
        write_binding = NULL;
      } else {
        /* Terminal tools are a contract pair. Roll back exec_command so a
         * caller can correct the conflicting write_stdin registration and
         * retry without a stale half-registration. */
        cai_tool_registry_truncate(registry, registry_count);
      }
    }
  }
  if (exec_binding != NULL) {
    cai_terminal_binding_cleanup(exec_binding);
    released_local_binding = 1;
  }
  if (write_binding != NULL) {
    cai_terminal_binding_cleanup(write_binding);
    released_local_binding = 1;
  }
  if (manager != NULL && !released_local_binding && manager->refs == 0) {
    cai_terminal_manager_destroy(manager);
  }
  return rc;
}

int cai_agent_register_terminal_tools(cai_agent *agent,
                                      const cai_terminal_tool_config *config,
                                      cai_error *error) {
  if (agent == NULL || agent->impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  return cai_tool_registry_register_terminal_tools(CAI_AGENT_IMPL(agent)->tools,
                                                   config, error);
}
