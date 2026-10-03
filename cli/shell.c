#define _GNU_SOURCE
#include "shell.h"
#include "pouch_stream.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

static int shell_error(cai_error *error, const char *operation, int code) {
  char message[512];
  snprintf(message, sizeof(message), "%s: %s (errno %d)", operation,
           strerror(code), code);
  return cai_cli_error(error, CAI_ERR_TRANSPORT, message);
}

/* After fork in a threaded host, use only async-signal-safe operations. */
static void shell_child_error(int fd, const char *operation, const char *path,
                              int code) {
  char number[16];
  size_t count = sizeof(number);
  unsigned int value = (unsigned int)code;
  do {
    number[--count] = (char)('0' + value % 10U);
    value /= 10U;
  } while (value != 0U);
  (void)write(fd, operation, strlen(operation));
  (void)write(fd, path, strlen(path));
  (void)write(fd, " (errno ", 8U);
  (void)write(fd, number + count, sizeof(number) - count);
  (void)write(fd, ")\n", 2U);
}

typedef struct shell_waiter {
  pid_t pid;
  int fd;
} shell_waiter;

/* A dedicated wait owns exactly this child. The completion pipe keeps the UI
 * event-driven without process-global SIGCHLD handlers or a pidfd kernel floor.
 */
static void *shell_wait(void *context) {
  shell_waiter *waiter = context;
  int status, result;
  pid_t waited;
  do {
    waited = waitpid(waiter->pid, &status, 0);
  } while (waited < 0 && errno == EINTR);
  result = waited < 0          ? -errno
           : WIFEXITED(status) ? WEXITSTATUS(status)
                               : 128 + WTERMSIG(status);
  if (waited > 0)
    (void)kill(-waiter->pid, SIGKILL);
  while (write(waiter->fd, &result, sizeof(result)) < 0 && errno == EINTR) {
  }
  close(waiter->fd);
  free(waiter);
  return NULL;
}

void cai_cli_shell_init(cai_cli_shell *shell) {
  memset(shell, 0, sizeof(*shell));
  shell->output_fd = shell->exit_fd = shell->timer_fd = -1;
}

void cai_cli_shell_cancel(cai_cli_shell *shell) {
  if (shell->pid > 0 && !shell->exited)
    (void)kill(-shell->pid, SIGKILL);
}

unsigned long long cai_cli_shell_elapsed(const cai_cli_shell *shell) {
  struct timespec now;
  long long ms;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return 0U;
  ms = (long long)(now.tv_sec - shell->started.tv_sec) * 1000LL +
       (long long)(now.tv_nsec - shell->started.tv_nsec) / 1000000LL;
  return ms > 0LL ? (unsigned long long)ms : 0U;
}

void cai_cli_shell_close(cai_cli_shell *shell) {
  int status;
  if (shell->waiter_started) {
    cai_cli_shell_cancel(shell);
    pthread_join(shell->waiter, NULL);
  } else if (shell->pid > 0 && !shell->exited) {
    cai_cli_shell_cancel(shell);
    while (waitpid(shell->pid, &status, 0) < 0 && errno == EINTR) {
    }
  }
  if (shell->output_fd >= 0)
    close(shell->output_fd);
  if (shell->exit_fd >= 0)
    close(shell->exit_fd);
  if (shell->timer_fd >= 0)
    close(shell->timer_fd);
  cai_cli_shell_init(shell);
}

int cai_cli_shell_start(cai_cli_shell *shell, const char *command,
                        const char *directory, cai_error *error) {
  int pipes[2], notification[2], input, saved;
  shell_waiter *waiter;
  struct itimerspec timeout;
  struct sigaction action;
  const char *path = getenv("SHELL");
  pid_t pid;
  if (shell->pid > 0)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "a user shell command is running");
  if (path == NULL || path[0] != '/')
    path = "/bin/sh";
  if (pipe2(pipes, O_CLOEXEC) != 0)
    return shell_error(error, "cannot create shell output pipe", errno);
  input = open("/dev/null", O_RDONLY | O_CLOEXEC);
  if (input < 0) {
    saved = errno;
    close(pipes[0]);
    close(pipes[1]);
    return shell_error(error, "cannot open shell stdin", saved);
  }
  memset(&action, 0, sizeof(action));
  action.sa_handler = SIG_DFL;
  sigemptyset(&action.sa_mask);
  pid = fork();
  if (pid == 0) {
    sigset_t mask;
    close(pipes[0]);
    (void)setpgid(0, 0);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)sigaction(SIGPIPE, &action, NULL);
    sigemptyset(&mask);
    (void)sigprocmask(SIG_SETMASK, &mask, NULL);
    if (chdir(directory) != 0) {
      shell_child_error(pipes[1], "Cannot enter shell directory ", directory,
                        errno);
      _exit(126);
    }
    if (dup2(input, STDIN_FILENO) < 0 || dup2(pipes[1], STDOUT_FILENO) < 0 ||
        dup2(pipes[1], STDERR_FILENO) < 0) {
      shell_child_error(pipes[1], "Cannot connect shell pipes", "", errno);
      _exit(126);
    }
    close(input);
    close(pipes[1]);
    execl(path, path, "-c", command, (char *)NULL);
    shell_child_error(STDERR_FILENO, "Cannot execute user shell ", path, errno);
    _exit(127);
  }
  saved = errno;
  close(input);
  close(pipes[1]);
  if (pid < 0) {
    close(pipes[0]);
    return shell_error(error, "cannot fork user shell", saved);
  }
  shell->pid = pid;
  shell->output_fd = pipes[0];
  (void)setpgid(pid, pid);
  (void)clock_gettime(CLOCK_MONOTONIC, &shell->started);
  if (pipe2(notification, O_CLOEXEC) != 0) {
    saved = errno;
    cai_cli_shell_close(shell);
    return shell_error(error, "cannot create shell completion pipe", saved);
  }
  shell->exit_fd = notification[0];
  shell->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
  memset(&timeout, 0, sizeof(timeout));
  timeout.it_value.tv_sec = 3600;
  if (shell->timer_fd < 0 || fcntl(shell->exit_fd, F_SETFL, O_NONBLOCK) < 0 ||
      fcntl(shell->output_fd, F_SETFL, O_NONBLOCK) < 0 ||
      timerfd_settime(shell->timer_fd, 0, &timeout, NULL) < 0) {
    saved = errno;
    close(notification[1]);
    cai_cli_shell_close(shell);
    return shell_error(error, "cannot configure shell descriptors", saved);
  }
  waiter = malloc(sizeof(*waiter));
  if (waiter == NULL) {
    close(notification[1]);
    cai_cli_shell_close(shell);
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "cannot allocate shell completion waiter");
  }
  waiter->pid = pid;
  waiter->fd = notification[1];
  saved = pthread_create(&shell->waiter, NULL, shell_wait, waiter);
  if (saved != 0) {
    free(waiter);
    close(notification[1]);
    cai_cli_shell_close(shell);
    return shell_error(error, "cannot start shell completion waiter", saved);
  }
  shell->waiter_started = 1;
  return CAI_OK;
}

int cai_cli_shell_pump(cai_cli_shell *shell, cai_cli_shell_output_fn output,
                       void *context, cai_error *error) {
  char bytes[4096];
  size_t budget = 65536U;
  ssize_t count;
  int status, rc;
  unsigned long long timer;
  if (shell->pid <= 0)
    return CAI_OK;
  if (read(shell->timer_fd, &timer, sizeof(timer)) > 0)
    cai_cli_shell_cancel(shell);
  if (!shell->exited) {
    do {
      count = read(shell->exit_fd, &status, sizeof(status));
    } while (count < 0 && errno == EINTR);
    if (count == (ssize_t)sizeof(status)) {
      if (status < 0)
        return shell_error(error, "cannot collect shell exit status", -status);
      shell->exited = 1;
      shell->exit_code = status;
    } else if (count >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "shell completion pipe failed");
    }
  }
  while (!shell->eof && budget != 0U) {
    count = read(shell->output_fd, bytes, sizeof(bytes));
    if (count == 0) {
      shell->eof = 1;
      break;
    }
    if (count < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break;
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "cannot read shell output");
    }
    rc = output(context, bytes, (size_t)count, error);
    if (rc != CAI_OK)
      return rc;
    budget -= (size_t)count;
  }
  return CAI_OK;
}

int cai_cli_shell_plain(cai_cli_shell *shell, const char *bytes, size_t length,
                        int final, cai_cli_shell_output_fn output,
                        void *context, cai_error *error) {
  static const char hex[] = "0123456789abcdef";
  char rendered[512];
  size_t at = 0U, used = 0U, count, i;
  unsigned long scalar;
  int rc;
  while (at < length || shell->pending_length != 0U) {
    unsigned char ch;
    if (at < length && shell->pending_length < sizeof(shell->pending))
      shell->pending[shell->pending_length++] = (unsigned char)bytes[at++];
    ch = shell->pending[0];
    count = ch < 128U                  ? 1U
            : ch >= 194U && ch <= 223U ? 2U
            : ch >= 224U && ch <= 239U ? 3U
            : ch >= 240U && ch <= 244U ? 4U
                                       : 0U;
    scalar = count > 1U ? ch & ((1U << (7U - count)) - 1U) : ch;
    for (i = 1U; i < count && i < shell->pending_length; i++) {
      if ((shell->pending[i] & 192U) != 128U) {
        count = 0U;
        break;
      }
      scalar = (scalar << 6) | (shell->pending[i] & 63U);
    }
    if (count > shell->pending_length) {
      if (at < length)
        continue;
      if (!final)
        break;
      count = 0U;
    }
    if ((count == 2U && scalar < 128UL) || (count == 3U && scalar < 2048UL) ||
        (count == 4U && scalar < 65536UL) || scalar > 1114111UL ||
        (scalar >= 55296UL && scalar <= 57343UL) ||
        (count == 1U && ((ch < 32U && ch != '\n' && ch != '\t') || ch == 127U)))
      count = 0U;
    if (sizeof(rendered) - used < 4U) {
      rc = output(context, rendered, used, error);
      if (rc != CAI_OK)
        return rc;
      used = 0U;
    }
    if (count == 0U) {
      rendered[used++] = '\\';
      rendered[used++] = 'x';
      rendered[used++] = hex[ch >> 4];
      rendered[used++] = hex[ch & 15U];
      count = 1U;
    } else {
      memcpy(rendered + used, shell->pending, count);
      used += count;
    }
    shell->pending_length -= count;
    memmove(shell->pending, shell->pending + count, shell->pending_length);
  }
  return used != 0U ? output(context, rendered, used, error) : CAI_OK;
}
