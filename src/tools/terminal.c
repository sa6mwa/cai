#include "../cai_internal.h"

#include <cai/tools/terminal.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
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

#define CAI_TERMINAL_DEFAULT_YIELD_MS 10000L
#define CAI_TERMINAL_MAX_YIELD_MS 30000L
#define CAI_TERMINAL_DEFAULT_OUTPUT_MAX (3U * 1024U * 1024U)

typedef struct cai_terminal_manager {
  pthread_mutex_t lock;
  pthread_cond_t changed;
  int refs;
  char *root_path;
  char *default_workdir;
  char *shell_path;
  long default_yield_ms;
  long max_yield_ms;
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
  int running;
  int completed;
  int child_reaped;
  int pty_eof;
  int termination_requested;
  int completion_event_emitted;
  int child_status;
  char *output;
  size_t output_length;
  size_t output_capacity;
  size_t delivered_offset;
  size_t total_output_bytes;
  int output_truncated;
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
} cai_terminal_result;

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
                            "detached_processes_possible")};
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
    "run at a time.";
static const char cai_terminal_write_description[] =
    "Writes input to, waits for, or terminates CAI's single managed terminal. "
    "Use the exact session_id returned by exec_command; empty chars polls.";

static void cai_terminal_deadline(struct timespec *deadline, long wait_ms) {
  clock_gettime(CLOCK_REALTIME, deadline);
  deadline->tv_sec += wait_ms / 1000L;
  deadline->tv_nsec += (wait_ms % 1000L) * 1000000L;
  if (deadline->tv_nsec >= 1000000000L) {
    deadline->tv_sec++;
    deadline->tv_nsec -= 1000000000L;
  }
}

static void cai_terminal_close_fd(int *fd) {
  if (fd != NULL && *fd >= 0) {
    close(*fd);
    *fd = -1;
  }
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
  length = strlen(root);
  return strncmp(root, path, length) == 0 &&
                 (path[length] == '\0' || path[length] == '/')
             ? 1
             : 0;
}

static int cai_terminal_resolve_workdir(cai_terminal_manager *manager,
                                        const char *requested, char **out,
                                        cai_error *error) {
  char candidate[PATH_MAX];
  char resolved[PATH_MAX];
  const char *base;
  int written;

  *out = NULL;
  base = requested != NULL && requested[0] != '\0' ? requested
                                                     : manager->default_workdir;
  if (base == NULL || base[0] == '\0') {
    base = manager->root_path;
  }
  if (base[0] == '/') {
    written = snprintf(candidate, sizeof(candidate), "%s", base);
  } else {
    written = snprintf(candidate, sizeof(candidate), "%s/%s", manager->root_path,
                       base);
  }
  if (written < 0 || (size_t)written >= sizeof(candidate) ||
      realpath(candidate, resolved) == NULL ||
      !cai_terminal_under_root(manager->root_path, resolved)) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "terminal workdir must resolve below workspace root");
  }
  *out = cai_strdup(NULL, resolved);
  return *out != NULL
             ? CAI_OK
             : cai_set_error(error, CAI_ERR_NOMEM,
                             "failed to copy terminal workdir");
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

    capacity = manager->output_capacity == 0U ? 4096U : manager->output_capacity;
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
      (void)cai_terminal_output_append(manager, buffer, (size_t)count);
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
      manager->running = 0;
      manager->completed = 1;
      cai_terminal_close_fd(&manager->pty_fd);
      pthread_cond_broadcast(&manager->changed);
      pthread_mutex_unlock(&manager->lock);
      break;
    }
    pthread_mutex_unlock(&manager->lock);
    if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
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
    (void)kill(-manager->pid, SIGKILL);
  }
  pthread_mutex_unlock(&manager->lock);
  if (manager->reader_started) {
    pthread_join(manager->reader, NULL);
  }
  cai_terminal_close_fd(&manager->pty_fd);
  cai_free_mem(NULL, manager->root_path);
  cai_free_mem(NULL, manager->default_workdir);
  cai_free_mem(NULL, manager->shell_path);
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

  *out = NULL;
  if (config == NULL || config->root_path == NULL || config->root_path[0] == '\0' ||
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
  manager->root_path = cai_strdup(NULL, resolved);
  manager->default_workdir = config->default_workdir != NULL
                                 ? cai_strdup(NULL, config->default_workdir)
                                 : NULL;
  manager->shell_path = cai_strdup(NULL, config->shell_path != NULL ? config->shell_path
                                                                      : "/bin/sh");
  manager->default_yield_ms = config->default_yield_time_ms > 0L
                                  ? config->default_yield_time_ms
                                  : CAI_TERMINAL_DEFAULT_YIELD_MS;
  manager->max_yield_ms = config->max_yield_time_ms > 0L
                              ? config->max_yield_time_ms
                              : CAI_TERMINAL_MAX_YIELD_MS;
  manager->output_max_bytes = config->output_max_bytes != 0U
                                  ? config->output_max_bytes
                                  : CAI_TERMINAL_DEFAULT_OUTPUT_MAX;
  manager->policy = config->policy;
  manager->policy_context = config->policy_context;
  manager->event_callback = config->event_callback;
  manager->event_context = config->event_context;
  snprintf(manager->terminal_id, sizeof(manager->terminal_id), "terminal-1");
  if (manager->root_path == NULL || manager->shell_path == NULL ||
      manager->max_yield_ms <= 0L || manager->output_max_bytes == 0U) {
    cai_free_mem(NULL, manager->root_path);
    cai_free_mem(NULL, manager->default_workdir);
    cai_free_mem(NULL, manager->shell_path);
    cai_free_mem(NULL, manager);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate terminal manager");
  }
  if (pthread_mutex_init(&manager->lock, NULL) != 0) {
    cai_free_mem(NULL, manager->root_path);
    cai_free_mem(NULL, manager->default_workdir);
    cai_free_mem(NULL, manager->shell_path);
    cai_free_mem(NULL, manager);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to initialize terminal manager lock");
  }
  if (pthread_cond_init(&manager->changed, NULL) != 0) {
    pthread_mutex_destroy(&manager->lock);
    cai_free_mem(NULL, manager->root_path);
    cai_free_mem(NULL, manager->default_workdir);
    cai_free_mem(NULL, manager->shell_path);
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

static long cai_terminal_clamp_yield(const cai_terminal_manager *manager,
                                     long long requested, int has_requested) {
  long value;

  value = has_requested ? (requested < 0LL ? 0L : (long)requested)
                        : manager->default_yield_ms;
  return value > manager->max_yield_ms ? manager->max_yield_ms : value;
}

static int cai_terminal_start(cai_terminal_manager *manager, const char *cmd,
                              const char *workdir, int tty, cai_error *error) {
  int master;
  int slave;
  pid_t pid;
  char *command_copy;
  char *workdir_copy;

  (void)tty;
  master = -1;
  slave = -1;
  command_copy = cai_strdup(NULL, cmd);
  workdir_copy = cai_strdup(NULL, workdir);
  if (command_copy == NULL || workdir_copy == NULL) {
    cai_free_mem(NULL, command_copy);
    cai_free_mem(NULL, workdir_copy);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to copy terminal command metadata");
  }
  if (manager->reader_started) {
    pthread_mutex_lock(&manager->lock);
    if (manager->running) {
      pthread_mutex_unlock(&manager->lock);
      cai_free_mem(NULL, command_copy);
      cai_free_mem(NULL, workdir_copy);
      return cai_set_error(error, CAI_ERR_INVALID,
                           "single terminal already has a running command");
    }
    pthread_mutex_unlock(&manager->lock);
    pthread_join(manager->reader, NULL);
    manager->reader_started = 0;
  }
  if (openpty(&master, &slave, NULL, NULL, NULL) != 0 ||
      cai_terminal_set_nonblock(master) != 0) {
    cai_terminal_close_fd(&master);
    cai_terminal_close_fd(&slave);
    cai_free_mem(NULL, command_copy);
    cai_free_mem(NULL, workdir_copy);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to create terminal PTY", strerror(errno));
  }
  pid = fork();
  if (pid < 0) {
    cai_terminal_close_fd(&master);
    cai_terminal_close_fd(&slave);
    cai_free_mem(NULL, command_copy);
    cai_free_mem(NULL, workdir_copy);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to fork terminal command", strerror(errno));
  }
  if (pid == 0) {
    (void)setsid();
    (void)ioctl(slave, TIOCSCTTY, 0);
    dup2(slave, STDIN_FILENO);
    dup2(slave, STDOUT_FILENO);
    dup2(slave, STDERR_FILENO);
    close(master);
    if (slave > STDERR_FILENO) {
      close(slave);
    }
    if (chdir(workdir) != 0) {
      _exit(126);
    }
    execl(manager->shell_path, manager->shell_path, "-c", cmd, (char *)NULL);
    _exit(127);
  }
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
  manager->termination_requested = 0;
  manager->completion_event_emitted = 0;
  manager->output_length = 0U;
  manager->delivered_offset = 0U;
  manager->total_output_bytes = 0U;
  manager->output_truncated = 0;
  if (manager->output != NULL) {
    manager->output[0] = '\0';
  }
  pthread_mutex_unlock(&manager->lock);
  if (pthread_create(&manager->reader, NULL, cai_terminal_reader, manager) != 0) {
    (void)kill(-pid, SIGKILL);
    (void)waitpid(pid, NULL, 0);
    pthread_mutex_lock(&manager->lock);
    cai_terminal_close_fd(&manager->pty_fd);
    manager->running = 0;
    pthread_mutex_unlock(&manager->lock);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to start terminal output reader");
  }
  manager->reader_started = 1;
  return CAI_OK;
}

static int cai_terminal_fill_result(cai_terminal_manager *manager,
                                    size_t output_limit,
                                    cai_terminal_result *result,
                                    cai_error *error) {
  size_t available;
  size_t count;
  char *output;

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
  manager->delivered_offset += count;
  result->session_id = cai_tool_result_strdup(manager->terminal_id, error);
  result->output = output;
  result->running = manager->running;
  result->completed = manager->completed;
  result->command_id = (long long)manager->command_id;
  result->original_byte_count = (long long)manager->total_output_bytes;
  result->output_truncated = manager->output_truncated || count < available;
  /* CAI supervises the shell process, not arbitrary descendants that may
   * have escaped its process group.  Never promise those descendants exited. */
  result->detached_processes_possible = manager->completed;
  if (manager->completed && WIFEXITED(manager->child_status)) {
    result->exit_code = WEXITSTATUS(manager->child_status);
    result->has_exit_code = 1;
  } else if (manager->completed && WIFSIGNALED(manager->child_status)) {
    result->signal = WTERMSIG(manager->child_status);
    result->has_signal = 1;
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
  }
  return manager->event_callback(manager->event_context, &event, error);
}

static int cai_terminal_emit_completion_once(cai_terminal_manager *manager,
                                             const cai_terminal_result *result,
                                             cai_error *error) {
  int type;
  int emit;

  emit = 0;
  type = CAI_TERMINAL_EVENT_COMMAND_COMPLETED;
  pthread_mutex_lock(&manager->lock);
  if (manager->completed && !manager->completion_event_emitted) {
    manager->completion_event_emitted = 1;
    emit = 1;
    if (manager->termination_requested) {
      type = CAI_TERMINAL_EVENT_COMMAND_CANCELLED;
    }
  }
  pthread_mutex_unlock(&manager->lock);
  return emit ? cai_terminal_emit(manager, type, result, error) : CAI_OK;
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

  binding = (cai_terminal_binding *)value;
  args = (const cai_terminal_exec_args *)params;
  result = (cai_terminal_result *)out;
  if (binding == NULL || binding->manager == NULL || args == NULL ||
      args->cmd == NULL || args->cmd[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "terminal exec command is required");
  }
  workdir = NULL;
  rc = cai_terminal_resolve_workdir(binding->manager, args->workdir, &workdir,
                                    error);
  if (rc == CAI_OK && binding->manager->policy != NULL) {
    rc = binding->manager->policy(binding->manager->policy_context, args->cmd,
                                  binding->manager->root_path, workdir,
                                  args->has_tty && args->tty, error);
  }
  if (rc == CAI_OK) {
    rc = cai_terminal_start(binding->manager, args->cmd, workdir,
                            args->has_tty && args->tty, error);
  }
  cai_free_mem(NULL, workdir);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_terminal_emit(binding->manager, CAI_TERMINAL_EVENT_COMMAND_STARTED,
                         NULL, error);
  if (rc != CAI_OK) {
    return rc;
  }
  wait_ms = cai_terminal_clamp_yield(binding->manager, args->yield_time_ms,
                                     args->has_yield_time_ms);
  output_limit = args->has_max_output_tokens && args->max_output_tokens > 0LL
                     ? (size_t)args->max_output_tokens
                     : 0U;
  (void)cai_terminal_wait(binding->manager, 0U, wait_ms);
  rc = cai_terminal_fill_result(binding->manager, output_limit, result, error);
  if (rc == CAI_OK) {
    rc = result->completed
             ? cai_terminal_emit_completion_once(binding->manager, result, error)
             : cai_terminal_emit(binding->manager,
                                 result->output[0] != '\0'
                                     ? CAI_TERMINAL_EVENT_OUTPUT
                                     : CAI_TERMINAL_EVENT_WAITING,
                                 result, error);
  }
  return rc;
}

static void cai_terminal_send_signal(cai_terminal_manager *manager, int signal) {
  pthread_mutex_lock(&manager->lock);
  if (manager->running && manager->pid > 0) {
    (void)kill(-manager->pid, signal);
  }
  pthread_mutex_unlock(&manager->lock);
}

static int cai_terminal_write_all(int fd, const char *data, cai_error *error) {
  size_t offset;
  size_t length;

  offset = 0U;
  length = strlen(data);
  while (offset < length) {
    ssize_t written;

    written = write(fd, data + offset, length - offset);
    if (written > 0) {
      offset += (size_t)written;
    } else if (written < 0 && errno == EINTR) {
      continue;
    } else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      struct timespec pause_time;

      pause_time.tv_sec = 0;
      pause_time.tv_nsec = 1000000L;
      nanosleep(&pause_time, NULL);
    } else {
      return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                  "failed to write terminal stdin", strerror(errno));
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
    return cai_set_error(error, CAI_ERR_INVALID,
                         "write_stdin session_id does not match the single terminal");
  }
  pthread_mutex_lock(&binding->manager->lock);
  if (!binding->manager->running) {
    int completed;

    completed = binding->manager->completed;
    pthread_mutex_unlock(&binding->manager->lock);
    if (completed) {
      output_limit = args->has_max_output_tokens && args->max_output_tokens > 0LL
                         ? (size_t)args->max_output_tokens
                         : 0U;
      rc = cai_terminal_fill_result(binding->manager, output_limit, result,
                                    error);
      if (rc == CAI_OK) {
        rc = cai_terminal_emit_completion_once(binding->manager, result, error);
      }
      return rc;
    }
    return cai_set_error(error, CAI_ERR_INVALID,
                         "single terminal has no running command");
  }
  initial = binding->manager->output_length;
  fd = binding->manager->pty_fd;
  pthread_mutex_unlock(&binding->manager->lock);
  if (args->chars != NULL && args->chars[0] != '\0') {
    rc = cai_terminal_write_all(fd, args->chars, error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  wait_ms = cai_terminal_clamp_yield(binding->manager, args->yield_time_ms,
                                     args->has_yield_time_ms);
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
    rc = result->completed
             ? cai_terminal_emit_completion_once(binding->manager, result, error)
             : cai_terminal_emit(binding->manager,
                                 result->output[0] != '\0'
                                     ? CAI_TERMINAL_EVENT_OUTPUT
                                     : CAI_TERMINAL_EVENT_WAITING,
                                 result, error);
  }
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
  int rc;

  if (registry == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "terminal tool registry is required");
  }
  manager = NULL;
  exec_binding = NULL;
  write_binding = NULL;
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
          registry, CAI_TERMINAL_WRITE_TOOL_NAME, cai_terminal_write_description,
          cai_terminal_write_schema, 0, &cai_terminal_write_args_map,
          &cai_terminal_result_map, cai_terminal_write_callback, write_binding,
          cai_terminal_binding_cleanup, error);
      if (rc == CAI_OK) {
        write_binding = NULL;
      }
    }
  }
  if (exec_binding != NULL) {
    cai_terminal_binding_cleanup(exec_binding);
  }
  if (write_binding != NULL) {
    cai_terminal_binding_cleanup(write_binding);
  }
  if (manager != NULL && manager->refs == 0) {
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
