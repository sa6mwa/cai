#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void exact_read(int fd, char *data, size_t size) {
  fd_set reads;
  struct timeval timeout;
  ssize_t n;
  while (size) {
    FD_ZERO(&reads);
    FD_SET(fd, &reads);
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    assert(select(fd + 1, &reads, NULL, NULL, &timeout) == 1);
    n = read(fd, data, size);
    assert(n > 0);
    data += n;
    size -= (size_t)n;
  }
}
static void message(int fd, const char *text, int fragmented) {
  unsigned int size = (unsigned int)strlen(text);
  size_t i;
  if (fragmented) {
    for (i = 0; i < sizeof(size); i++)
      assert(write(fd, ((char *)&size) + i, 1U) == 1);
    for (i = 0; i < size; i++)
      assert(write(fd, text + i, 1U) == 1);
  } else {
    assert(write(fd, &size, sizeof(size)) == (ssize_t)sizeof(size));
    assert(write(fd, text, size) == (ssize_t)size);
  }
}
static void response(int fd, const char *needle) {
  unsigned int size;
  char text[4096];
  exact_read(fd, (char *)&size, sizeof(size));
  assert(size < sizeof(text));
  exact_read(fd, text, size);
  text[size] = '\0';
  assert(strstr(text, needle) != NULL);
}
int main(int argc, char **argv) {
  int input[2], output[2], status, mode;
  pid_t child;
  unsigned int length;
  if (argc != 2)
    return 2;
  for (mode = 0; mode < 5; mode++) {
    assert(pipe(input) == 0 && pipe(output) == 0);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
      close(input[1]);
      close(output[0]);
      assert(dup2(input[0], 0) >= 0 && dup2(output[1], 1) >= 0);
      close(input[0]);
      close(output[1]);
      execl(argv[1], argv[1],
            "chrome-extension://hehggadaopoacecdllhhajmbjkdcmajg/",
            (char *)NULL);
      _exit(127);
    }
    close(input[0]);
    close(output[1]);
    if (mode == 0) {
      message(
          input[1],
          "{\"jsonrpc\":\"2.0\",\"id\":100,\"method\":\"ping\",\"params\":{}}",
          1);
      response(output[0], "\"pong\"");
      message(input[1],
              "{\"jsonrpc\":\"2.0\",\"id\":101,\"method\":\"unsupported\","
              "\"params\":{}}",
              0);
      response(output[0], "-32601");
    } else if (mode == 1) {
      length = 0xffffffffU;
      assert(write(input[1], &length, sizeof(length)) ==
             (ssize_t)sizeof(length));
    } else if (mode == 2) {
      message(input[1], "{{", 0);
    } else if (mode == 3) {
      length = 10U;
      assert(write(input[1], &length, sizeof(length)) ==
             (ssize_t)sizeof(length));
      assert(write(input[1], "short", 5U) == 5);
    } else {
      /* No pending method: an unsolicited ID-only frame is not a void reply. */
      message(input[1], "{\"jsonrpc\":\"2.0\",\"id\":\"unsolicited\"}", 0);
    }
    close(input[1]);
    while (waitpid(child, &status, 0) < 0)
      assert(errno == EINTR);
    close(output[0]);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == (mode == 0 ? 0 : 1));
  }
  return 0;
}
