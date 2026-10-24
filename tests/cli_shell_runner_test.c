#define _GNU_SOURCE
#include "../cli/shell.h"
#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>

static int budget_fd = -1;
static size_t unread_bytes, read_number;
ssize_t __real_read(int fd, void *bytes, size_t count);
ssize_t __wrap_read(int fd, void *bytes, size_t count);
ssize_t __wrap_read(int fd, void *bytes, size_t count) {
  size_t chunk;
  if (fd != budget_fd || budget_fd < 0)
    return __real_read(fd, bytes, count);
  chunk = read_number++ == 0U ? 1U : 4096U;
  if (chunk > count)
    chunk = count;
  if (chunk > unread_bytes)
    chunk = unread_bytes;
  memset(bytes, 'X', chunk);
  unread_bytes -= chunk;
  return (ssize_t)chunk;
}

typedef struct capture {
  cai_cli_shell *shell;
  char data[131072];
  size_t length;
  int early;
  int fail;
} capture;
static int output(void *context, const char *data, size_t length,
                  cai_error *error) {
  capture *c = context;
  (void)error;
  if (c->fail)
    return CAI_ERR_TRANSPORT;
  assert(length < sizeof(c->data) - c->length);
  memcpy(c->data + c->length, data, length);
  c->length += length;
  c->data[c->length] = '\0';
  if (!c->shell->exited)
    c->early = 1;
  return CAI_OK;
}
static void drain(cai_cli_shell *shell, capture *c, cai_error *error) {
  struct pollfd fds[3];
  unsigned long long started = cai_cli_shell_elapsed(shell);
  while (!shell->exited || !shell->eof) {
    fds[0].fd = shell->output_fd;
    fds[1].fd = shell->exit_fd;
    fds[2].fd = shell->timer_fd;
    fds[0].events = fds[1].events = fds[2].events = POLLIN | POLLHUP;
    assert(poll(fds, 3U, 100) >= 0);
    assert(cai_cli_shell_pump(shell, output, c, error) == CAI_OK);
    assert(cai_cli_shell_elapsed(shell) - started < 5000U);
  }
}
int main(int argc, char **argv) {
  cai_cli_shell shell;
  capture c;
  cai_error error;
  struct itimerspec timer;
  static const char input[] =
      "**plain** #text & <tag>\n\xe2\x82\xac\xf0\x9f\x9a\x80\0\033\377\342";
  size_t split, i;
  assert(argc == 2);
  cai_error_init(&error);
  cai_cli_shell_init(&shell);
  /* Uneven, continuously readable chunks must yield after 64 KiB. The next
   * dispatch must receive all remaining bytes, without dropping new output. */
  memset(&c, 0, sizeof(c));
  c.shell = &shell;
  shell.pid = 1; /* Synthetic descriptors only; no child exists in this case. */
  shell.exited = 1;
  shell.output_fd = budget_fd = 100000;
  unread_bytes = 100000U;
  read_number = 0U;
  assert(cai_cli_shell_pump(&shell, output, &c, &error) == CAI_OK);
  assert(c.length == 65536U && unread_bytes == 34464U && !shell.eof);
  assert(cai_cli_shell_pump(&shell, output, &c, &error) == CAI_OK);
  assert(c.length == 100000U && unread_bytes == 0U && shell.eof);
  budget_fd = -1;
  cai_cli_shell_init(&shell);
  setenv("SHELL", "/bin/sh", 1);
  memset(&c, 0, sizeof(c));
  c.shell = &shell;
  assert(cai_cli_shell_start(&shell,
                             "test ! -t 0 && test ! -t 1 && test ! -t 2 && "
                             "cat; printf ERR >&2; exit 7",
                             argv[1], &error) == CAI_OK);
  assert(cai_cli_shell_start(&shell, "no-second-command", argv[1], &error) ==
         CAI_ERR_INVALID);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  drain(&shell, &c, &error);
  assert(shell.exit_code == 7 && strcmp(c.data, "ERR") == 0);
  cai_cli_shell_close(&shell);
  memset(&c, 0, sizeof(c));
  c.shell = &shell;
  assert(cai_cli_shell_start(&shell, "head -c 100000 /dev/zero | tr '\\000' X",
                             argv[1], &error) == CAI_OK);
  drain(&shell, &c, &error);
  assert(c.length == 100000U && c.early && shell.exit_code == 0);
  cai_cli_shell_close(&shell);
  /* A completed invocation cannot signal a PID that has since been reused.
   * Simulate reuse with another process group owned by this fixture. */
  memset(&c, 0, sizeof(c));
  c.shell = &shell;
  assert(cai_cli_shell_start(&shell, "sleep 60", argv[1], &error) == CAI_OK);
  {
    cai_cli_shell completed = shell;
    struct pollfd fd;
    completed.exited = 1;
    cai_cli_shell_cancel(&completed);
    fd.fd = shell.exit_fd;
    fd.events = POLLIN | POLLHUP;
    fd.revents = 0;
    assert(poll(&fd, 1U, 30) == 0);
  }
  cai_cli_shell_cancel(&shell);
  drain(&shell, &c, &error);
  assert(shell.exit_code == 137);
  cai_cli_shell_close(&shell);
  /* Rearm the same real one-shot timeout FD, without waiting an hour. */
  memset(&c, 0, sizeof(c));
  c.shell = &shell;
  assert(cai_cli_shell_start(&shell, "sleep 60", argv[1], &error) == CAI_OK);
  memset(&timer, 0, sizeof(timer));
  timer.it_value.tv_nsec = 1000000L;
  assert(timerfd_settime(shell.timer_fd, 0, &timer, NULL) == 0);
  drain(&shell, &c, &error);
  assert(shell.exit_code == 137);
  cai_cli_shell_close(&shell);
  memset(&c, 0, sizeof(c));
  c.shell = &shell;
  assert(cai_cli_shell_start(&shell, "printf ignored",
                             "/nonexistent-cai-fixture-directory",
                             &error) == CAI_OK);
  drain(&shell, &c, &error);
  assert(shell.exit_code == 126);
  cai_cli_shell_close(&shell);
  /* Output consumer failure stops the invocation without leaving a child. */
  memset(&c, 0, sizeof(c));
  c.shell = &shell;
  c.fail = 1;
  assert(cai_cli_shell_start(&shell, "printf early; sleep 60", argv[1],
                             &error) == CAI_OK);
  for (i = 0U; i < 1000U; i++) {
    struct pollfd fd;
    fd.fd = shell.output_fd;
    fd.events = POLLIN;
    fd.revents = 0;
    assert(poll(&fd, 1U, 10) >= 0);
    if (cai_cli_shell_pump(&shell, output, &c, &error) == CAI_ERR_TRANSPORT)
      break;
  }
  assert(i < 1000U);
  cai_cli_shell_close(&shell);
  assert(shell.pid == 0);
  for (split = 1U; split <= sizeof(input) - 1U; split++) {
    cai_cli_shell_init(&shell);
    memset(&c, 0, sizeof(c));
    c.shell = &shell;
    for (i = 0U; i < sizeof(input) - 1U; i += split) {
      size_t length = sizeof(input) - 1U - i;
      if (length > split)
        length = split;
      assert(cai_cli_shell_plain(&shell, input + i, length, 0, output, &c,
                                 &error) == CAI_OK);
    }
    assert(cai_cli_shell_plain(&shell, NULL, 0U, 1, output, &c, &error) ==
           CAI_OK);
    assert(strcmp(c.data, "**plain** #text & <tag>\n€🚀\\x00\\x1b\\xff\\xe2") ==
           0);
    assert(shell.pending_length == 0U);
  }
  cai_error_cleanup(&error);
  return 0;
}
