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

/* Record fixed ASCII rows independently of interleaved feed writes. These
 * bounded snapshots contain data, not test decisions. */
static void row_control(const char *csi, char *snapshot, size_t capacity,
                        int target, int rows, int width, int *row, int *column,
                        int *scroll_bottom) {
  char *end;
  long first = strtol(csi, &end, 10);
  long second = *end == ';' ? strtol(end + 1, NULL, 10) : 1L;
  char command = csi[strlen(csi) - 1U];
  int i;
  if (first < 1L)
    first = 1L;
  if (second < 1L)
    second = 1L;
  if (command == 'H' || command == 'f') {
    *row = (int)(first > rows ? rows : first);
    *column = (int)(second > width ? width : second);
  } else if (command == 'A') {
    *row = first >= *row ? 1 : *row - (int)first;
  } else if (command == 'B') {
    *row = first > rows - *row ? rows : *row + (int)first;
  } else if (command == 'C') {
    *column = first > width - *column ? width : *column + (int)first;
  } else if (command == 'D') {
    *column = first >= *column ? 1 : *column - (int)first;
  } else if (command == 'G') {
    *column = (int)(first > width ? width : first);
  } else if (command == 'r') {
    if (*end != ';')
      second = rows;
    *scroll_bottom = (int)(second > rows ? rows : second);
  } else if ((command == 'K' && *row == target) || command == 'J') {
    /* Default K erases from the cursor; K=1 erases through it. */
    long mode = strtol(csi, NULL, 10);
    int start = mode == 1L || mode == 2L || *row < target ? 0 : *column - 1;
    int stop = mode == 1L && *row == target ? *column : (int)capacity;
    if (mode > 2L || (command == 'J' && ((mode == 1L && *row < target) ||
                                         (mode == 0L && *row > target))))
      return;
    for (i = start; i < stop && i < (int)capacity; i++)
      snapshot[i] = ' ';
  }
}

static int save_rows(const char *directory, const char *name, char rows[][4096],
                     size_t count, size_t width) {
  char path[4096], final[4096];
  FILE *file;
  size_t row, length;
  int rc = 0;
  snprintf(path, sizeof(path), "%s/%s.tmp", directory, name);
  snprintf(final, sizeof(final), "%s/%s", directory, name);
  file = fopen(path, "w");
  if (file == NULL)
    return -1;
  for (row = 0U; row < count; row++) {
    length = width;
    while (length > 0U && rows[row][length - 1U] == ' ')
      length--;
    if (fwrite(rows[row], 1U, length, file) != length ||
        fputc('\n', file) == EOF)
      rc = -1;
  }
  if (fclose(file) != 0)
    rc = -1;
  return rc == 0 ? rename(path, final) : rc;
}

int main(int argc, char **argv) {
  int master, slave, raw, text, feed, diagnostics, status_fd, status, state = 0;
  int alive = 1, input = 1, control, foreground, width;
  int events = -1, frame_changed;
  pid_t child;
  char path[4096], state_path[4096], data[8192], plain[8192], feed_bytes[8192],
      *name;
  ssize_t count;
  size_t i, used, feed_used;
  int row;
  fd_set reads;
  struct timeval timeout;
  struct winsize window;
  struct termios attributes;
  FILE *settings;
  char screen[30][4096], csi[64];
  size_t csi_length = 0U, row_length;
  int cursor_row = 1, cursor_column = 1, scroll_bottom = 30;
  memset(screen, ' ', sizeof(screen));
  if (argc < 4) {
    fprintf(stderr, "usage: terminal_peer OUTPUT_DIR WIDTH COMMAND [ARG...]\n");
    return 2;
  }
  width = atoi(argv[2]);
  if (getenv("CAI_TEST_TERMINAL_EVENT_FD") != NULL) {
    events = atoi(getenv("CAI_TEST_TERMINAL_EVENT_FD"));
    if (events < 3 || fcntl(events, F_SETFL, O_NONBLOCK) < 0)
      return 2;
  }
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
  OPEN_FILE(feed, "feed");
  OPEN_FILE(diagnostics, "stderr");
  child = fork();
  if (child < 0)
    return 2;
  if (child == 0) {
    if (events >= 0)
      close(events);
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
    close(feed);
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
    frame_changed = 0;
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
      frame_changed = 1;
      count = read(master, data, sizeof(data));
      if (count <= 0)
        break;
      if (write_all(raw, data, (size_t)count))
        break;
      used = 0;
      feed_used = 0U;
      for (i = 0; i < (size_t)count; i++) {
        unsigned char ch = (unsigned char)data[i];
        if (state == 1) {
          state = ch == '[' ? 2 : ch == ']' ? 3 : 0;
          csi_length = 0U;
          continue;
        }
        if (state == 2) {
          if (csi_length + 1U < sizeof(csi))
            csi[csi_length++] = (char)ch;
          if (ch >= 0x40 && ch <= 0x7e) {
            csi[csi_length] = '\0';
            /* Answer the standard DSR query as a terminal does, rather than
             * making every editor open/handoff wait for its probe timeout. */
            if (strcmp(csi, "6n") == 0) {
              char reply[64];
              int length = snprintf(reply, sizeof(reply), "\033[%d;%dR",
                                    cursor_row, cursor_column);
              if (length < 0 || (size_t)length >= sizeof(reply) ||
                  write_all(master, reply, (size_t)length) != 0)
                stopped = 1;
            }
            for (row = 1; row < window.ws_row; row++) {
              int r = cursor_row, column = cursor_column;
              int bottom = scroll_bottom;
              row_control(csi, screen[row - 1], sizeof(screen[0]), row,
                          window.ws_row, width, &r, &column, &bottom);
            }
            row_control(csi, screen[window.ws_row - 1], sizeof(screen[0]),
                        window.ws_row, window.ws_row, width, &cursor_row,
                        &cursor_column, &scroll_bottom);
            state = 0;
          }
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
        else {
          /* Softline's explicit scroll region owns feed output. Sticky and
           * input rows are separate even when writes interleave mid-word. */
          if (ch != '\r' && scroll_bottom < window.ws_row &&
              cursor_row <= scroll_bottom)
            feed_bytes[feed_used++] = (char)ch;
          if (ch == '\r')
            cursor_column = 1;
          else if (ch == '\n') {
            if (cursor_row != scroll_bottom && cursor_row < window.ws_row)
              cursor_row++;
          } else if (ch == '\b') {
            if (cursor_column > 1)
              cursor_column--;
          } else if (ch >= 32U && ch < 127U) {
            if (cursor_row > 0 && cursor_row <= window.ws_row &&
                cursor_column > 0 && cursor_column <= (int)sizeof(screen[0]))
              screen[cursor_row - 1][cursor_column - 1] = (char)ch;
            if (cursor_column < width)
              cursor_column++;
          }
          if (ch != '\r')
            plain[used++] = (char)ch;
        }
      }
      row_length =
          (size_t)width < sizeof(screen[0]) ? (size_t)width : sizeof(screen[0]);
      if (save_rows(argv[1], "prompt", &screen[window.ws_row - 1], 1U,
                    row_length) != 0 ||
          save_rows(argv[1], "message", &screen[scroll_bottom],
                    scroll_bottom < window.ws_row
                        ? (size_t)(window.ws_row - scroll_bottom - 1)
                        : 0U,
                    row_length) != 0)
        stopped = 1;
      /* A newly visible feed fragment must not expose the previous frame's
       * fixed rows to a reader. Publish snapshots before append-only bytes. */
      if (write_all(text, plain, used) ||
          write_all(feed, feed_bytes, feed_used))
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
    /* Wake the Bash test after publishing a frame, without polling delays.
     * This is only a readiness hint: a full pipe already means work is ready.
     */
    if (frame_changed && events >= 0 && write(events, ".", 1U) < 0 &&
        errno != EAGAIN && errno != EINTR)
      break;
  }
  if (stopped)
    kill(-child, SIGKILL);
  close(master);
  close(raw);
  close(text);
  close(feed);
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR)
      return 2;
  }
  status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  OPEN_FILE(status_fd, "status");
  snprintf(data, sizeof(data), "%d\n", status);
  write_all(status_fd, data, strlen(data));
  close(status_fd);
  if (events >= 0) {
    (void)write(events, ".", 1U);
    close(events);
  }
  return status;
}
