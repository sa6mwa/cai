#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define SMOKE_RESPONSE_LIMIT 65536U

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

static int http_request(unsigned short port, const char *method,
                        const char *accept, const char *body, char *response,
                        size_t response_capacity) {
  char header[1024];
  int fd;
  int rc;
  size_t body_len;

  fd = connect_loopback(port);
  if (fd < 0) {
    perror("connect");
    return -1;
  }
  body_len = body != NULL ? strlen(body) : 0U;
  snprintf(header, sizeof(header),
           "%s /mcp HTTP/1.1\r\n"
           "Host: 127.0.0.1:%u\r\n"
           "Accept: %s\r\n"
           "MCP-Protocol-Version: 2025-11-25\r\n"
           "%s"
           "Content-Length: %lu\r\n"
           "Connection: close\r\n"
           "\r\n",
           method != NULL ? method : "POST", (unsigned int)port,
           accept != NULL ? accept : "application/json, text/event-stream",
           body_len > 0U ? "Content-Type: application/json\r\n" : "",
           (unsigned long)body_len);
  rc = write_all(fd, header, strlen(header));
  if (rc == 0 && body_len > 0U) {
    rc = write_all(fd, body, body_len);
  }
  if (rc == 0) {
    rc = read_response(fd, response, response_capacity);
  }
  close(fd);
  return rc;
}

static int http_post(unsigned short port, const char *body, char *response,
                     size_t response_capacity) {
  return http_request(port, "POST", "application/json, text/event-stream", body,
                      response, response_capacity);
}

static int http_post_sse(unsigned short port, const char *body, char *response,
                         size_t response_capacity) {
  return http_request(port, "POST", "text/event-stream", body, response,
                      response_capacity);
}

static int http_get_sse(unsigned short port, char *response,
                        size_t response_capacity) {
  return http_request(port, "GET", "text/event-stream", NULL, response,
                      response_capacity);
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

static pid_t start_server(const char *server_path, unsigned short *port_out) {
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
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);
    execl(server_path, server_path, "--port", "0", "--print-port", "--requests",
          "5", (char *)NULL);
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
  char response[SMOKE_RESPONSE_LIMIT];
  unsigned short port;
  pid_t pid;
  int status;
  int failures;

  if (argc != 2) {
    fprintf(stderr, "usage: %s /path/to/cai_mcp_http_server\n", argv[0]);
    return 2;
  }
  failures = 0;
  pid = start_server(argv[1], &port);
  if (pid < 0) {
    fprintf(stderr, "failed to start MCP HTTP server\n");
    return 1;
  }
  if (http_post(port,
                "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                "\"params\":{\"protocolVersion\":\"2025-11-25\","
                "\"capabilities\":{},\"clientInfo\":{\"name\":\"smoke\","
                "\"version\":\"1\"}}}",
                response, sizeof(response)) != 0) {
    failures++;
  } else {
    failures += expect_contains("initialize status", response, "HTTP/1.1 200");
    failures += expect_contains("initialize protocol", response,
                                "\"protocolVersion\":\"2025-11-25\"");
    failures += expect_contains("initialize server", response,
                                "\"name\":\"cai-mcp-http-test\"");
  }
  if (http_post(port,
                "{\"jsonrpc\":\"2.0\",\"id\":\"list\","
                "\"method\":\"tools/list\"}",
                response, sizeof(response)) != 0) {
    failures++;
  } else {
    failures += expect_contains("tools/list status", response, "HTTP/1.1 200");
    failures += expect_contains("tools/list tool", response,
                                "\"name\":\"echo_message\"");
    failures +=
        expect_contains("tools/list schema", response, "\"inputSchema\"");
  }
  if (http_post(port,
                "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
                "\"params\":{\"name\":\"echo_message\",\"arguments\":{"
                "\"message\":\"streamed through http\"}}}",
                response, sizeof(response)) != 0) {
    failures++;
  } else {
    failures += expect_contains("tools/call status", response, "HTTP/1.1 200");
    failures += expect_contains("tools/call result", response,
                                "\"structuredContent\":{\"echo\":"
                                "\"streamed through http\"}");
  }
  if (http_post_sse(port,
                    "{\"jsonrpc\":\"2.0\",\"id\":\"sse\","
                    "\"method\":\"ping\"}",
                    response, sizeof(response)) != 0) {
    failures++;
  } else {
    failures += expect_contains("sse ping status", response, "HTTP/1.1 200");
    failures += expect_contains("sse ping content-type", response,
                                "content-type: text/event-stream");
    failures += expect_contains("sse ping data prefix", response,
                                "\r\ndata: {\"jsonrpc\":\"2.0\"");
    failures += expect_contains("sse ping result", response,
                                "\"id\":\"sse\",\"result\":{}");
  }
  if (http_get_sse(port, response, sizeof(response)) != 0) {
    failures++;
  } else {
    failures += expect_contains("get sse status", response, "HTTP/1.1 200");
    failures += expect_contains("get sse content-type", response,
                                "content-type: text/event-stream");
    failures += expect_contains("get sse body", response,
                                "\r\n\r\n: cai-mcp-stream\n\n");
  }
  if (waitpid(pid, &status, 0) < 0) {
    perror("waitpid");
    failures++;
  } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    fprintf(stderr, "MCP HTTP server exited badly: %d\n", status);
    failures++;
  }
  return failures == 0 ? 0 : 1;
}
