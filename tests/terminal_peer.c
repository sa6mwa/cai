/* A PTY data bridge for shell-owned CLI tests. No test decisions live here. */
#define _XOPEN_SOURCE 600
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static volatile sig_atomic_t resized;
static volatile sig_atomic_t stopped;
static void stop_signal(int signo) {
  (void)signo;
  stopped = 1;
}
static void resize_signal(int signo) {
  (void)signo;
  resized = 1;
}
static int write_all(int fd, const char *data, size_t size) {
  ssize_t n;
  while (size) {
    n = write(fd, data, size);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return -1;
    data += n;
    size -= (size_t)n;
  }
  return 0;
}
int main(int argc, char **argv) {
  int master, slave, raw, text, diagnostics, status_fd, status, state = 0;
  int alive = 1, input = 1, control, foreground, width;
  pid_t child;
  char path[4096], state_path[4096], data[8192], plain[8192], *name;
  ssize_t count;
  size_t i, used;
  fd_set reads;
  struct timeval timeout;
  struct winsize window;
  struct termios attributes;
  FILE *settings;
  if (argc < 4) {
    fprintf(stderr, "usage: terminal_peer OUTPUT_DIR WIDTH COMMAND [ARG...]\n");
    return 2;
  }
  width = atoi(argv[2]);
  master = posix_openpt(O_RDWR | O_NOCTTY);
  if (master < 0 || grantpt(master) || unlockpt(master)) {
    perror("open PTY");
    return 2;
  }
  name = ptsname(master);
  if (!name || (slave = open(name, O_RDWR | O_NOCTTY)) < 0) {
    perror("PTY slave");
    return 2;
  }
  memset(&window, 0, sizeof(window));
  window.ws_row = 30;
  window.ws_col = (unsigned short)width;
  if (ioctl(slave, TIOCSWINSZ, &window))
    return 2;
#define OPEN_FILE(var, suffix)                                                 \
  do {                                                                         \
    if (snprintf(path, sizeof(path), "%s/%s", argv[1], suffix) >=              \
        (int)sizeof(path))                                                     \
      return 2;                                                                \
    var = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);                      \
    if (var < 0) {                                                             \
      perror(path);                                                            \
      return 2;                                                                \
    }                                                                          \
  } while (0)
  OPEN_FILE(raw, "raw");
  OPEN_FILE(text, "text");
  OPEN_FILE(diagnostics, "stderr");
  child = fork();
  if (child < 0)
    return 2;
  if (child == 0) {
    close(master);
    if (setsid() < 0 || ioctl(slave, TIOCSCTTY, 0) < 0 || dup2(slave, 0) < 0 ||
        dup2(slave, 1) < 0 ||
        dup2(getenv("CAI_TEST_TERMINAL_STDERR") != NULL ? slave : diagnostics,
             2) < 0)
      _exit(126);
    if (slave > 2)
      close(slave);
    close(raw);
    close(text);
    close(diagnostics);
    execvp(argv[3], argv + 3);
    perror(argv[3]);
    _exit(127);
  }
  close(slave);
  close(diagnostics);
  signal(SIGUSR1, resize_signal);
  signal(SIGTERM, stop_signal);
  signal(SIGINT, stop_signal);
  while (alive && !stopped) {
    if (resized) {
      resized = 0;
      snprintf(path, sizeof(path), "%s/width", argv[1]);
      settings = fopen(path, "r");
      if (settings) {
        if (fscanf(settings, "%d", &width) == 1) {
          window.ws_col = (unsigned short)width;
          ioctl(master, TIOCSWINSZ, &window);
          kill(child, SIGWINCH);
        }
        fclose(settings);
      }
    }
    FD_ZERO(&reads);
    FD_SET(master, &reads);
    if (input)
      FD_SET(0, &reads);
    timeout.tv_sec = 0;
    timeout.tv_usec = 50000;
    control = select(master + 1, &reads, NULL, NULL, &timeout);
    if (control < 0 && errno != EINTR)
      break;
    if (control > 0 && input && FD_ISSET(0, &reads)) {
      count = read(0, data, sizeof(data));
      if (count <= 0)
        input = 0;
      else if (write_all(master, data, (size_t)count))
        break;
    }
    if (control > 0 && FD_ISSET(master, &reads)) {
      count = read(master, data, sizeof(data));
      if (count <= 0)
        break;
      if (write_all(raw, data, (size_t)count))
        break;
      used = 0;
      for (i = 0; i < (size_t)count; i++) {
        unsigned char ch = (unsigned char)data[i];
        if (state == 1) {
          state = ch == '[' ? 2 : ch == ']' ? 3 : 0;
          continue;
        }
        if (state == 2) {
          if (ch >= 0x40 && ch <= 0x7e)
            state = 0;
          continue;
        }
        if (state == 3) {
          if (ch == 7)
            state = 0;
          else if (ch == 27)
            state = 4;
          continue;
        }
        if (state == 4) {
          state = ch == '\\' ? 0 : 3;
          continue;
        }
        if (ch == 27)
          state = 1;
        else if (ch != '\r')
          plain[used++] = (char)ch;
      }
      if (write_all(text, plain, used))
        break;
    }
    snprintf(path, sizeof(path), "%s/tty.tmp", argv[1]);
    settings = fopen(path, "w");
    if (settings) {
      foreground = (int)tcgetpgrp(master);
      control = tcgetattr(master, &attributes) == 0 &&
                (attributes.c_lflag & ICANON) && (attributes.c_lflag & ECHO);
      fprintf(settings, "%d %d %d\n", control, foreground, (int)child);
      fclose(settings);
      snprintf(state_path, sizeof(state_path), "%s/tty", argv[1]);
      if (rename(path, state_path) != 0) {
        stopped = 1;
      }
    }
    /* Reap after draining the PTY: closure is reported by read, including EIO.
     */
  }
  if (stopped)
    kill(-child, SIGKILL);
  close(master);
  close(raw);
  close(text);
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR)
      return 2;
  }
  status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  OPEN_FILE(status_fd, "status");
  snprintf(data, sizeof(data), "%d\n", status);
  write_all(status_fd, data, strlen(data));
  close(status_fd);
  return status;
}
