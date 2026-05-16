#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define RESPONSE_LIMIT 131072U

static int write_all(int fd, const void *data, size_t len) {
  const char *p;
  ssize_t n;

  p = (const char *)data;
  while (len > 0U) {
    n = write(fd, p, len);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (n == 0) {
      return -1;
    }
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

static int read_line(int fd, char *buffer, size_t capacity) {
  size_t len;
  char c;
  ssize_t n;

  len = 0U;
  while (len + 1U < capacity) {
    n = read(fd, &c, 1U);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (n == 0) {
      break;
    }
    if (c == '\n') {
      break;
    }
    buffer[len++] = c;
  }
  buffer[len] = '\0';
  return len > 0U ? 0 : -1;
}

static int connect_loopback(unsigned short port) {
  struct sockaddr_in addr;
  int fd;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int read_response(int fd, char *buffer, size_t capacity) {
  size_t len;
  ssize_t n;

  len = 0U;
  while (len + 1U < capacity) {
    n = read(fd, buffer + len, capacity - len - 1U);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (n == 0) {
      break;
    }
    len += (size_t)n;
  }
  buffer[len] = '\0';
  return len > 0U ? 0 : -1;
}

static int http_post(unsigned short port, const char *body, char *response,
                     size_t response_capacity) {
  char header[1024];
  int fd;
  int rc;

  fd = connect_loopback(port);
  if (fd < 0) {
    perror("connect");
    return -1;
  }
  snprintf(header, sizeof(header),
           "POST /mcp HTTP/1.1\r\n"
           "Host: 127.0.0.1:%u\r\n"
           "Content-Type: application/json\r\n"
           "Accept: application/json, text/event-stream\r\n"
           "MCP-Protocol-Version: 2025-11-25\r\n"
           "Content-Length: %lu\r\n"
           "Connection: close\r\n"
           "\r\n",
           (unsigned int)port, (unsigned long)strlen(body));
  rc = write_all(fd, header, strlen(header));
  if (rc == 0) {
    rc = write_all(fd, body, strlen(body));
  }
  if (rc == 0) {
    rc = read_response(fd, response, response_capacity);
  }
  close(fd);
  return rc;
}

static int expect_contains(const char *name, const char *haystack,
                           const char *needle) {
  if (strstr(haystack, needle) == NULL) {
    fprintf(stderr, "%s missing expected text: %s\nresponse:\n%s\n", name,
            needle, haystack);
    return 1;
  }
  return 0;
}

static pid_t start_server(const char *server_path, const char *todo_store,
                          const char *todo_lock, unsigned short *port_out) {
  int pipefd[2];
  pid_t pid;
  char line[64];

  if (pipe(pipefd) != 0) {
    perror("pipe");
    return -1;
  }
  pid = fork();
  if (pid < 0) {
    perror("fork");
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
  }
  if (pid == 0) {
    setenv("CAI_MCP_EXAMPLE_TODO_STORE", todo_store, 1);
    setenv("CAI_MCP_EXAMPLE_TODO_LOCK", todo_lock, 1);
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);
    execl(server_path, server_path, "--port", "0", "--print-port",
          "--requests", "3", (char *)NULL);
    _exit(127);
  }
  close(pipefd[1]);
  if (read_line(pipefd[0], line, sizeof(line)) != 0) {
    close(pipefd[0]);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    return -1;
  }
  close(pipefd[0]);
  *port_out = (unsigned short)strtoul(line, NULL, 10);
  return pid;
}

int main(int argc, char **argv) {
  char response[RESPONSE_LIMIT];
  char dir_template[] = "/tmp/cai-mcp-example-XXXXXX";
  char todo_store[PATH_MAX];
  char todo_lock[PATH_MAX];
  unsigned short port;
  pid_t pid;
  int status;
  int failures;

  if (argc != 2) {
    fprintf(stderr, "usage: %s /path/to/cai_example_mcp_server\n", argv[0]);
    return 2;
  }
  if (mkdtemp(dir_template) == NULL) {
    perror("mkdtemp");
    return 1;
  }
  snprintf(todo_store, sizeof(todo_store), "%s/todo.json", dir_template);
  snprintf(todo_lock, sizeof(todo_lock), "%s/todo.lock", dir_template);
  failures = 0;
  pid = start_server(argv[1], todo_store, todo_lock, &port);
  if (pid < 0) {
    fprintf(stderr, "failed to start MCP example server\n");
    return 1;
  }
  if (http_post(port,
                "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                "\"params\":{\"protocolVersion\":\"2025-11-25\","
                "\"capabilities\":{},\"clientInfo\":{\"name\":\"example\","
                "\"version\":\"1\"}}}",
                response, sizeof(response)) != 0) {
    failures++;
  } else {
    failures += expect_contains("initialize status", response, "HTTP/1.1 200");
    failures += expect_contains("initialize server", response,
                                "\"name\":\"cai-mcp-example\"");
  }
  if (http_post(port,
                "{\"jsonrpc\":\"2.0\",\"id\":\"list\","
                "\"method\":\"tools/list\"}",
                response, sizeof(response)) != 0) {
    failures++;
  } else {
    failures += expect_contains("tools/list status", response, "HTTP/1.1 200");
    failures += expect_contains("tools/list revgeo", response,
                                "\"name\":\"reverse_geocode\"");
    failures += expect_contains("tools/list todo", response,
                                "\"name\":\"todo_kanban\"");
  }
  if (http_post(port,
                "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
                "\"params\":{\"name\":\"todo_kanban\",\"arguments\":{"
                "\"operation\":\"create_board\",\"board_name\":\"mcp\"}}}",
                response, sizeof(response)) != 0) {
    failures++;
  } else {
    failures += expect_contains("tools/call status", response, "HTTP/1.1 200");
    failures += expect_contains("tools/call todo", response,
                                "\"structuredContent\":{\"ok\":true");
    failures += expect_contains("tools/call board", response,
                                "\"board_name\":\"mcp\"");
  }
  if (waitpid(pid, &status, 0) < 0) {
    perror("waitpid");
    failures++;
  } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    fprintf(stderr, "MCP example server exited badly: %d\n", status);
    failures++;
  }
  unlink(todo_store);
  unlink(todo_lock);
  rmdir(dir_template);
  return failures == 0 ? 0 : 1;
}
