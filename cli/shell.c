#define _GNU_SOURCE
#include "shell.h"
#include "pouch_stream.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static volatile sig_atomic_t handoff_resize;
static void handoff_winch(int signal_number) {
  (void)signal_number;
  handoff_resize = 1;
}

static int handoff_write(int fd, const char *data, size_t size) {
  while (size != 0U) {
    ssize_t n = write(fd, data, size);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return -1;
    data += n;
    size -= (size_t)n;
  }
  return 0;
}

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
  int raw_status;
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
  result = waited < 0           ? -errno
           : waiter->raw_status ? status
           : WIFEXITED(status)  ? WEXITSTATUS(status)
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

static int shell_foreground(pid_t foreground) {
  struct sigaction ignore, previous;
  int rc, saved_errno;
  memset(&ignore, 0, sizeof(ignore));
  ignore.sa_handler = SIG_IGN;
  sigemptyset(&ignore.sa_mask);
  if (sigaction(SIGTTOU, &ignore, &previous) != 0)
    return -1;
  rc = tcsetpgrp(STDIN_FILENO, foreground);
  saved_errno = errno;
  if (sigaction(SIGTTOU, &previous, NULL) != 0 && rc == 0)
    return -1;
  errno = saved_errno;
  return rc;
}

int cai_cli_shell_handoff(const char *path, const char *directory,
                          cai_cli_shell_output_fn output, void *context,
                          int stop_fd, cai_cli_shell_stop_fn stop, int *status,
                          cai_error *error) {
  struct termios saved, raw;
  struct winsize geometry;
  struct sigaction action, previous;
  struct pollfd fds[4];
  shell_waiter *waiter = NULL;
  pthread_t thread;
  char name[256], buffer[4096], input[4096];
  size_t input_length = 0U, input_offset = 0U;
  int master = -1, slave = -1, pipefd[2] = {-1, -1};
  int rc = CAI_OK, failure = 0, raw_set = 0, signal_set = 0;
  int launched = 0, exited = 0, eof = 0, input_open = 1, wait_status, ready;
  pid_t child = -1, foreground, active;
  ssize_t n;
  *status = 127 << 8;
  if (tcgetattr(STDIN_FILENO, &saved) != 0 ||
      ioctl(STDIN_FILENO, TIOCGWINSZ, &geometry) != 0 ||
      (foreground = tcgetpgrp(STDIN_FILENO)) < 0)
    return shell_error(error, "cannot read terminal attributes", errno);
  if (output != NULL) {
    master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
      failure = errno;
      goto done;
    }
    failure = ptsname_r(master, name, sizeof(name));
    if (failure != 0)
      goto done;
    slave = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0 || tcsetattr(slave, TCSANOW, &saved) != 0 ||
        ioctl(slave, TIOCSWINSZ, &geometry) != 0) {
      failure = errno;
      goto done;
    }
  } else {
    eof = 1;
    input_open = 0;
  }
  if (pipe2(pipefd, O_CLOEXEC) != 0) {
    failure = errno;
    goto done;
  }
  waiter = malloc(sizeof(*waiter));
  if (waiter == NULL) {
    rc = cai_cli_error(error, CAI_ERR_NOMEM, "cannot allocate PTY waiter");
    goto done;
  }
  memset(&action, 0, sizeof(action));
  action.sa_handler = SIG_DFL;
  sigemptyset(&action.sa_mask);
  child = fork();
  if (child == 0) {
    sigset_t mask;
    if (master >= 0)
      close(master);
    close(pipefd[0]);
    close(pipefd[1]);
    if (output != NULL) {
      if (setsid() < 0 || ioctl(slave, TIOCSCTTY, 0) < 0 ||
          dup2(slave, STDIN_FILENO) < 0 || dup2(slave, STDOUT_FILENO) < 0 ||
          dup2(slave, STDERR_FILENO) < 0) {
        shell_child_error(slave, "Cannot initialize shell terminal", "", errno);
        _exit(126);
      }
      if (slave > STDERR_FILENO)
        close(slave);
    } else {
      /* Do not allocate or relay another PTY when no capture is requested.
       * Job control, resize and input belong directly to the child shell. */
      action.sa_handler = SIG_IGN;
      (void)sigaction(SIGTTOU, &action, NULL);
      if (setpgid(0, 0) != 0 || tcsetpgrp(STDIN_FILENO, getpid()) != 0) {
        shell_child_error(STDERR_FILENO, "Cannot claim shell terminal", "",
                          errno);
        _exit(126);
      }
      action.sa_handler = SIG_DFL;
      (void)sigaction(SIGTTOU, &action, NULL);
    }
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGQUIT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)sigaction(SIGHUP, &action, NULL);
    (void)sigaction(SIGWINCH, &action, NULL);
    (void)sigaction(SIGPIPE, &action, NULL);
    sigemptyset(&mask);
    (void)sigprocmask(SIG_SETMASK, &mask, NULL);
    if (chdir(directory) != 0) {
      shell_child_error(STDERR_FILENO, "Cannot enter shell directory ",
                        directory, errno);
      _exit(126);
    }
    execl(path, path, "-i", (char *)NULL);
    shell_child_error(STDERR_FILENO, "Cannot execute user shell ", path, errno);
    _exit(127);
  }
  if (child < 0) {
    failure = errno;
    goto done;
  }
  if (slave >= 0)
    close(slave);
  slave = -1;
  waiter->pid = child;
  waiter->fd = pipefd[1];
  waiter->raw_status = 1;
  failure = pthread_create(&thread, NULL, shell_wait, waiter);
  if (failure != 0)
    goto done;
  launched = 1;
  waiter = NULL;
  pipefd[1] = -1;
  if (output != NULL) {
    action.sa_handler = handoff_winch;
    handoff_resize = 0;
    if (sigaction(SIGWINCH, &action, &previous) != 0) {
      failure = errno;
      goto done;
    }
    signal_set = 1;
    raw = saved;
    cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
      failure = errno;
      goto done;
    }
    raw_set = 1;
    if (fcntl(master, F_SETFL, O_NONBLOCK) < 0) {
      failure = errno;
      goto done;
    }
  }
  handoff_resize = output != NULL;
  while (!eof || !exited) {
    if (handoff_resize) {
      handoff_resize = 0;
      if (ioctl(STDIN_FILENO, TIOCGWINSZ, &geometry) == 0)
        (void)ioctl(master, TIOCSWINSZ, &geometry);
    }
    fds[0].fd = eof ? -1 : master;
    fds[0].events = POLLIN | (input_offset < input_length ? POLLOUT : 0);
    fds[1].fd = input_open && !exited && !eof && input_offset == input_length
                    ? STDIN_FILENO
                    : -1;
    fds[1].events = POLLIN;
    fds[2].fd = exited ? -1 : pipefd[0];
    fds[2].events = POLLIN;
    fds[3].fd = stop != NULL ? stop_fd : -1;
    fds[3].events = POLLIN;
    /* A detached background job may retain the slave after its shell exits.
     * Drain queued bytes, then release the handoff instead of waiting for it.
     */
    ready = poll(fds, 4U, exited ? 0 : -1);
    if (ready == 0)
      break;
    if (ready < 0) {
      if (errno == EINTR)
        continue;
      failure = errno;
      break;
    }
    if (fds[3].revents != 0 && stop(context)) {
      rc = cai_cli_error(error, CAI_ERR_CANCELLED,
                         "interactive shell interrupted by host STOP");
      break;
    }
    if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      n = read(master, buffer, sizeof(buffer));
      if (n > 0) {
        if (output != NULL) {
          rc = output(context, buffer, (size_t)n, error);
          if (rc != CAI_OK)
            break;
        }
        if (handoff_write(STDOUT_FILENO, buffer, (size_t)n) != 0) {
          failure = errno;
          break;
        }
      } else if (n == 0 || (n < 0 && errno == EIO))
        eof = 1;
      else if (errno != EINTR && errno != EAGAIN) {
        failure = errno;
        break;
      }
    }
    if ((fds[0].revents & POLLOUT) != 0 && input_offset < input_length) {
      n = write(master, input + input_offset, input_length - input_offset);
      if (n > 0)
        input_offset += (size_t)n;
      else if (n < 0 && errno != EINTR && errno != EAGAIN && errno != EIO) {
        failure = errno;
        break;
      }
    }
    if (fds[2].revents != 0) {
      n = read(pipefd[0], status, sizeof(*status));
      if (n == (ssize_t)sizeof(*status)) {
        exited = 1;
        if (*status < 0) {
          failure = -*status;
          break;
        }
      } else if (n >= 0 || errno != EINTR) {
        failure = EIO;
        break;
      }
    }
    if (fds[1].revents != 0 && !exited && !eof) {
      n = read(STDIN_FILENO, input, sizeof(input));
      if (n == 0) {
        input_open = 0;
        (void)kill(-child, SIGHUP);
      } else if (n > 0) {
        input_offset = 0U;
        input_length = (size_t)n;
      } else if (errno != EINTR) {
        failure = errno;
        break;
      }
    }
  }
done:
  if (child > 0 && !exited) {
    /* A direct shell may have handed the TTY to a foreground job in another
     * process group. Stop that job as well before reclaiming the terminal. */
    if (output == NULL && (active = tcgetpgrp(STDIN_FILENO)) > 0 &&
        active != foreground && active != getpgrp())
      (void)kill(-active, SIGKILL);
    if (kill(-child, SIGKILL) != 0)
      (void)kill(child, SIGKILL); /* Child may not have set its group yet. */
  }
  if (launched)
    pthread_join(thread, NULL);
  else if (child > 0) {
    (void)kill(child, SIGKILL);
    while (waitpid(child, &wait_status, 0) < 0 && errno == EINTR) {
    }
  }
  if (launched && !exited) {
    do {
      n = read(pipefd[0], status, sizeof(*status));
    } while (n < 0 && errno == EINTR);
    if (n != (ssize_t)sizeof(*status) || *status < 0)
      failure = n == (ssize_t)sizeof(*status) ? -*status : EIO;
  }
  free(waiter);
  if (master >= 0)
    close(master);
  if (slave >= 0)
    close(slave);
  if (pipefd[0] >= 0)
    close(pipefd[0]);
  if (pipefd[1] >= 0)
    close(pipefd[1]);
  if (output == NULL && child > 0 && shell_foreground(foreground) != 0 &&
      failure == 0)
    failure = errno;
  if ((raw_set || (output == NULL && child > 0)) &&
      tcsetattr(STDIN_FILENO, TCSANOW, &saved) != 0 && failure == 0)
    failure = errno;
  if (signal_set && sigaction(SIGWINCH, &previous, NULL) != 0 && failure == 0)
    failure = errno;
  return failure != 0 ? shell_error(error, "interactive shell handoff", failure)
                      : rc;
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
  waiter->raw_status = 0;
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
