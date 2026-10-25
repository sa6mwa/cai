#define _POSIX_C_SOURCE 200809L
#include "../cli/login.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

int cai_login_example_main(int argc, char **argv);
#define main cai_login_example_main
#include "../examples/chatgpt-login/main.c"
#undef main

static void disconnected_browser(void) {
  int sockets[2];
  cai_chatgpt_login_response response;
  char body[] = "Authentication complete.";
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = SIG_DFL;
  sigemptyset(&action.sa_mask);
  assert(sigaction(SIGPIPE, &action, NULL) == 0);
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  close(sockets[1]);
  memset(&response, 0, sizeof(response));
  response.status = 200;
  response.body = body;
  assert(cai_cli_login_write_response(sockets[0], &response) == -1);
  assert(errno == EPIPE || errno == ECONNRESET);
  assert(sigaction(SIGPIPE, NULL, &action) == 0);
  assert(action.sa_handler == SIG_DFL);
  close(sockets[0]);
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  close(sockets[1]);
  write_http_response(sockets[0], &response);
  close(sockets[0]);
  assert(sigaction(SIGPIPE, NULL, &action) == 0);
  assert(action.sa_handler == SIG_DFL);
}

int main(void) {
  cai_chatgpt_login_response response;
  char body[] = "Saved caf\303\251 credentials.\n";
  char received[1024], content_type[600];
  int sockets[2], status;
  size_t used = 0U;
  ssize_t count;
  pid_t child;
  memset(&response, 0, sizeof(response));
  response.status = 200;
  response.body = body;
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  assert(cai_cli_login_write_response(sockets[0], &response) == 0);
  close(sockets[0]);
  while ((count = read(sockets[1], received + used,
                       sizeof(received) - used - 1U)) > 0)
    used += (size_t)count;
  assert(count == 0);
  received[used] = '\0';
  assert(strcmp(received, "HTTP/1.1 200 OK\r\n"
                          "Content-Type: text/plain; charset=utf-8\r\n"
                          "Content-Length: 25\r\nConnection: close\r\n\r\n"
                          "Saved caf\303\251 credentials.\n") == 0);
  close(sockets[1]);
  assert(cai_cli_login_write_response(-1, &response) == -1);
  assert(errno == EBADF);
  memset(content_type, 'x', sizeof(content_type) - 1U);
  content_type[sizeof(content_type) - 1U] = '\0';
  response.content_type = content_type;
  assert(cai_cli_login_write_response(-1, &response) == -1);
  assert(errno == EOVERFLOW);
  child = fork();
  assert(child >= 0);
  if (child == 0) {
    disconnected_browser();
    _exit(0);
  }
  assert(waitpid(child, &status, 0) == child);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    fprintf(stderr, "disconnected OAuth writer failed: wait status %d\n",
            status);
    return 1;
  }
  return 0;
}
