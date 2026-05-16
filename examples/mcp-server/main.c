#include <cai/cai.h>
#include <cai/mcp.h>
#include <cai/tools/revgeo.h>
#include <cai/tools/todo.h>

#include <lonejson.h>

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CAI_MCP_EXAMPLE_HEADER_LIMIT 32768U
#define CAI_MCP_EXAMPLE_MAX_HEADERS 32U
#define CAI_MCP_EXAMPLE_DEFAULT_PORT 18766
#define CAI_MCP_EXAMPLE_CLIPBOARD_LIMIT (1024U * 1024U)

typedef struct clipboard_args {
  char *text;
} clipboard_args;

typedef struct clipboard_result {
  int ok;
  int has_ok;
  char *message;
} clipboard_result;

typedef struct http_header {
  char name[64];
  char value[512];
} http_header;

typedef struct http_request_state {
  int fd;
  char method[16];
  char path[128];
  http_header headers[CAI_MCP_EXAMPLE_MAX_HEADERS];
  size_t header_count;
  size_t remaining_body;
} http_request_state;

typedef struct http_response_state {
  int fd;
  cai_mcp_http_response *response;
  http_header headers[CAI_MCP_EXAMPLE_MAX_HEADERS];
  size_t header_count;
  int headers_sent;
} http_response_state;

static const lonejson_field clipboard_arg_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(clipboard_args, text, "text")};
LONEJSON_MAP_DEFINE(clipboard_arg_map, clipboard_args, clipboard_arg_fields);

static const lonejson_field clipboard_result_fields[] = {
    LONEJSON_FIELD_BOOL_PRESENT(clipboard_result, ok, has_ok, "ok"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(clipboard_result, message, "message")};
LONEJSON_MAP_DEFINE(clipboard_result_map, clipboard_result,
                    clipboard_result_fields);

static void copy_cstr(char *dst, size_t dst_size, const char *src) {
  size_t len;

  if (dst_size == 0U) {
    return;
  }
  len = strlen(src);
  if (len >= dst_size) {
    len = dst_size - 1U;
  }
  memcpy(dst, src, len);
  dst[len] = '\0';
}

static void lower_ascii(char *s) {
  while (*s != '\0') {
    if (*s >= 'A' && *s <= 'Z') {
      *s = (char)(*s - 'A' + 'a');
    }
    s++;
  }
}

static void trim_ascii(char *s) {
  char *start;
  char *end;

  start = s;
  while (*start == ' ' || *start == '\t' || *start == '\r' ||
         *start == '\n') {
    start++;
  }
  if (start != s) {
    memmove(s, start, strlen(start) + 1U);
  }
  end = s + strlen(s);
  while (end > s &&
         (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
          end[-1] == '\n')) {
    end--;
  }
  *end = '\0';
}

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

static int write_cstr(int fd, const char *s) {
  return write_all(fd, s, strlen(s));
}

static int parse_request_line(http_request_state *request, char *line) {
  char *method;
  char *path;
  char *version;

  method = strtok(line, " ");
  path = strtok(NULL, " ");
  version = strtok(NULL, " ");
  if (method == NULL || path == NULL || version == NULL) {
    return -1;
  }
  copy_cstr(request->method, sizeof(request->method), method);
  copy_cstr(request->path, sizeof(request->path), path);
  return 0;
}

static int add_header(http_request_state *request, const char *line) {
  char temp[1024];
  char *colon;
  http_header *header;

  if (request->header_count >= CAI_MCP_EXAMPLE_MAX_HEADERS) {
    return -1;
  }
  copy_cstr(temp, sizeof(temp), line);
  colon = strchr(temp, ':');
  if (colon == NULL) {
    return -1;
  }
  *colon = '\0';
  header = &request->headers[request->header_count];
  copy_cstr(header->name, sizeof(header->name), temp);
  copy_cstr(header->value, sizeof(header->value), colon + 1);
  trim_ascii(header->name);
  trim_ascii(header->value);
  lower_ascii(header->name);
  request->header_count++;
  if (strcmp(header->name, "content-length") == 0) {
    request->remaining_body = (size_t)strtoul(header->value, NULL, 10);
  }
  return 0;
}

static int read_headers(http_request_state *request) {
  char buffer[CAI_MCP_EXAMPLE_HEADER_LIMIT + 1U];
  size_t len;
  char c;
  ssize_t n;
  char *line;
  char *save;
  int fd;

  len = 0U;
  fd = request->fd;
  memset(request, 0, sizeof(*request));
  request->fd = fd;
  while (len < CAI_MCP_EXAMPLE_HEADER_LIMIT) {
    n = read(request->fd, &c, 1U);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (n == 0) {
      return -1;
    }
    buffer[len++] = c;
    if (len >= 4U && buffer[len - 4U] == '\r' &&
        buffer[len - 3U] == '\n' && buffer[len - 2U] == '\r' &&
        buffer[len - 1U] == '\n') {
      break;
    }
  }
  if (len >= CAI_MCP_EXAMPLE_HEADER_LIMIT) {
    return -1;
  }
  buffer[len] = '\0';
  save = NULL;
  line = strtok_r(buffer, "\r\n", &save);
  if (line == NULL || parse_request_line(request, line) != 0) {
    return -1;
  }
  while ((line = strtok_r(NULL, "\r\n", &save)) != NULL) {
    if (line[0] != '\0' && add_header(request, line) != 0) {
      return -1;
    }
  }
  return 0;
}

static const char *request_header_get(void *context, const char *name) {
  http_request_state *request;
  size_t i;

  request = (http_request_state *)context;
  for (i = 0U; i < request->header_count; i++) {
    if (strcmp(request->headers[i].name, name) == 0) {
      return request->headers[i].value;
    }
  }
  return NULL;
}

static int response_header_set(void *context, const char *name,
                               const char *value, cai_error *error) {
  http_response_state *response;
  http_header *header;

  (void)error;
  response = (http_response_state *)context;
  if (response->headers_sent ||
      response->header_count >= CAI_MCP_EXAMPLE_MAX_HEADERS) {
    return CAI_ERR_INVALID;
  }
  header = &response->headers[response->header_count];
  copy_cstr(header->name, sizeof(header->name), name);
  copy_cstr(header->value, sizeof(header->value), value);
  response->header_count++;
  return CAI_OK;
}

static const char *status_text(int status) {
  if (status == 200) {
    return "OK";
  }
  if (status == 202) {
    return "Accepted";
  }
  if (status == 400) {
    return "Bad Request";
  }
  if (status == 403) {
    return "Forbidden";
  }
  if (status == 405) {
    return "Method Not Allowed";
  }
  if (status == 406) {
    return "Not Acceptable";
  }
  if (status == 415) {
    return "Unsupported Media Type";
  }
  return "OK";
}

static int send_response_headers(http_response_state *response,
                                 cai_error *error) {
  char line[128];
  size_t i;
  int status;

  (void)error;
  if (response->headers_sent) {
    return CAI_OK;
  }
  status = response->response->status != 0 ? response->response->status : 200;
  snprintf(line, sizeof(line), "HTTP/1.1 %d %s\r\n", status,
           status_text(status));
  if (write_cstr(response->fd, line) != 0) {
    return CAI_ERR_TRANSPORT;
  }
  for (i = 0U; i < response->header_count; i++) {
    if (write_cstr(response->fd, response->headers[i].name) != 0 ||
        write_cstr(response->fd, ": ") != 0 ||
        write_cstr(response->fd, response->headers[i].value) != 0 ||
        write_cstr(response->fd, "\r\n") != 0) {
      return CAI_ERR_TRANSPORT;
    }
  }
  if (write_cstr(response->fd, "Connection: close\r\n\r\n") != 0) {
    return CAI_ERR_TRANSPORT;
  }
  response->headers_sent = 1;
  return CAI_OK;
}

static size_t body_source_read(void *context, void *buffer, size_t count,
                               cai_error *error) {
  http_request_state *request;
  ssize_t n;

  (void)error;
  request = (http_request_state *)context;
  if (request->remaining_body == 0U) {
    return 0U;
  }
  if (count > request->remaining_body) {
    count = request->remaining_body;
  }
  for (;;) {
    n = read(request->fd, buffer, count);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
  if (n <= 0) {
    return 0U;
  }
  request->remaining_body -= (size_t)n;
  return (size_t)n;
}

static int body_source_reset(void *context, cai_error *error) {
  (void)context;
  (void)error;
  return CAI_ERR_INVALID;
}

static int body_sink_write(void *context, const void *bytes, size_t count,
                           cai_error *error) {
  http_response_state *response;
  int rc;

  response = (http_response_state *)context;
  rc = send_response_headers(response, error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (write_all(response->fd, bytes, count) != 0) {
    return CAI_ERR_TRANSPORT;
  }
  return CAI_OK;
}

static int find_in_path(const char *name, char *out, size_t out_size) {
  const char *path;
  const char *segment;
  const char *end;
  size_t dir_len;
  size_t name_len;

  path = getenv("PATH");
  name_len = strlen(name);
  if (path == NULL || out_size == 0U) {
    return 0;
  }
  segment = path;
  while (*segment != '\0') {
    end = strchr(segment, ':');
    if (end == NULL) {
      end = segment + strlen(segment);
    }
    dir_len = (size_t)(end - segment);
    if (dir_len > 0U && dir_len + 1U + name_len < out_size) {
      memcpy(out, segment, dir_len);
      out[dir_len] = '/';
      memcpy(out + dir_len + 1U, name, name_len + 1U);
      if (access(out, X_OK) == 0) {
        return 1;
      }
    }
    segment = *end == ':' ? end + 1 : end;
  }
  out[0] = '\0';
  return 0;
}

static int clipboard_tool(void *context, const void *params, void *result,
                          cai_error *error) {
  const char *xclip_path;
  const clipboard_args *args;
  clipboard_result *out;
  int pipe_fds[2];
  pid_t pid;
  int status;

  xclip_path = (const char *)context;
  args = (const clipboard_args *)params;
  out = (clipboard_result *)result;
  out->ok = 0;
  out->has_ok = 1;
  if (strlen(args->text) > CAI_MCP_EXAMPLE_CLIPBOARD_LIMIT) {
    out->message = cai_tool_result_strdup("clipboard input exceeds 1 MiB",
                                          error);
    return out->message != NULL ? CAI_OK : CAI_ERR_NOMEM;
  }
  if (pipe(pipe_fds) != 0) {
    return CAI_ERR_TRANSPORT;
  }
  pid = fork();
  if (pid < 0) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return CAI_ERR_TRANSPORT;
  }
  if (pid == 0) {
    close(pipe_fds[1]);
    dup2(pipe_fds[0], STDIN_FILENO);
    close(pipe_fds[0]);
    execl(xclip_path, xclip_path, "-selection", "clipboard", (char *)NULL);
    _exit(127);
  }
  close(pipe_fds[0]);
  if (write_all(pipe_fds[1], args->text, strlen(args->text)) != 0) {
    close(pipe_fds[1]);
    waitpid(pid, NULL, 0);
    return CAI_ERR_TRANSPORT;
  }
  close(pipe_fds[1]);
  if (waitpid(pid, &status, 0) < 0) {
    return CAI_ERR_TRANSPORT;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    out->message = cai_tool_result_strdup("xclip failed", error);
  } else {
    out->ok = 1;
    out->message = cai_tool_result_strdup("copied text to X11 clipboard",
                                          error);
  }
  return out->message != NULL ? CAI_OK : CAI_ERR_NOMEM;
}

static int handle_connection(int fd, cai_mcp_handler *handler) {
  http_request_state request_state;
  http_response_state response_state;
  cai_source_callbacks source_callbacks;
  cai_sink_callbacks sink_callbacks;
  cai_source *source;
  cai_sink *sink;
  cai_mcp_http_request request;
  cai_mcp_http_response response;
  cai_error error;
  int rc;

  source = NULL;
  sink = NULL;
  cai_error_init(&error);
  request_state.fd = fd;
  if (read_headers(&request_state) != 0) {
    write_cstr(fd, "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n");
    return 1;
  }
  if (strcmp(request_state.path, "/mcp") != 0) {
    write_cstr(fd, "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n");
    return 1;
  }
  source_callbacks.read = body_source_read;
  source_callbacks.reset = body_source_reset;
  source_callbacks.close = NULL;
  source_callbacks.context = &request_state;
  rc = cai_source_from_callbacks(&source_callbacks, &source, &error);
  if (rc != CAI_OK) {
    goto done;
  }
  memset(&response_state, 0, sizeof(response_state));
  response_state.fd = fd;
  sink_callbacks.write = body_sink_write;
  sink_callbacks.close = NULL;
  sink_callbacks.context = &response_state;
  rc = cai_sink_from_callbacks(&sink_callbacks, &sink, &error);
  if (rc != CAI_OK) {
    goto done;
  }
  memset(&request, 0, sizeof(request));
  memset(&response, 0, sizeof(response));
  response_state.response = &response;
  request.method = request_state.method;
  request.body = source;
  request.header = request_header_get;
  request.header_context = &request_state;
  response.body = sink;
  response.set_header = response_header_set;
  response.header_context = &response_state;
  rc = cai_mcp_handler_handle_http(handler, &request, &response, &error);
  if (rc == CAI_OK && !response_state.headers_sent) {
    rc = send_response_headers(&response_state, &error);
  }

done:
  cai_sink_close(sink);
  cai_source_close(source);
  if (rc != CAI_OK) {
    fprintf(stderr, "mcp request failed: %s\n",
            error.message != NULL ? error.message : cai_status_string(rc));
  }
  cai_error_cleanup(&error);
  return rc == CAI_OK ? 0 : 1;
}

static int create_listener(unsigned short port, unsigned short *actual_port) {
  struct sockaddr_in addr;
  socklen_t addr_len;
  int fd;
  int yes;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  if (listen(fd, 16) != 0) {
    close(fd);
    return -1;
  }
  addr_len = (socklen_t)sizeof(addr);
  if (getsockname(fd, (struct sockaddr *)&addr, &addr_len) != 0) {
    close(fd);
    return -1;
  }
  *actual_port = ntohs(addr.sin_port);
  return fd;
}

static void usage(const char *argv0) {
  fprintf(stderr,
          "usage: %s [--port N] [--print-port] [--requests N]\n"
          "default port: %d\n",
          argv0, CAI_MCP_EXAMPLE_DEFAULT_PORT);
}

int main(int argc, char **argv) {
  cai_tool_registry *registry;
  cai_mcp_handler_config mcp_config;
  cai_todo_tool_config todo_config;
  cai_mcp_handler *handler;
  cai_error error;
  unsigned short port;
  unsigned short actual_port;
  long request_limit;
  long handled;
  int print_port;
  int listener;
  int client;
  int i;
  int rc;
  char xclip_path[512];

  registry = NULL;
  handler = NULL;
  port = CAI_MCP_EXAMPLE_DEFAULT_PORT;
  actual_port = 0U;
  request_limit = 0L;
  handled = 0L;
  print_port = 0;
  cai_error_init(&error);
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      i++;
      port = (unsigned short)strtoul(argv[i], NULL, 10);
    } else if (strcmp(argv[i], "--print-port") == 0) {
      print_port = 1;
    } else if (strcmp(argv[i], "--requests") == 0 && i + 1 < argc) {
      i++;
      request_limit = strtol(argv[i], NULL, 10);
    } else {
      usage(argv[0]);
      return 2;
    }
  }
  rc = cai_tool_registry_new(&registry, &error);
  if (rc == CAI_OK) {
    rc = cai_tool_registry_register_revgeo_tool(registry, NULL, &error);
  }
  if (rc == CAI_OK) {
    memset(&todo_config, 0, sizeof(todo_config));
    todo_config.store_path = getenv("CAI_MCP_EXAMPLE_TODO_STORE");
    todo_config.lock_path = getenv("CAI_MCP_EXAMPLE_TODO_LOCK");
    rc = cai_tool_registry_register_todo_tool(registry, &todo_config, &error);
  }
  if (rc == CAI_OK && find_in_path("xclip", xclip_path, sizeof(xclip_path))) {
    rc = cai_tool_registry_register_lonejson(
        registry, "copy_to_clipboard",
        "Copy text to the local X11 clipboard with xclip when available.",
        &clipboard_arg_map, &clipboard_result_map, clipboard_tool, xclip_path,
        &error);
  }
  if (rc != CAI_OK) {
    fprintf(stderr, "tool registry setup failed: %s\n",
            error.message != NULL ? error.message : cai_status_string(rc));
    cai_tool_registry_destroy(registry);
    cai_error_cleanup(&error);
    return 1;
  }
  cai_mcp_handler_config_init(&mcp_config);
  mcp_config.name = "cai-mcp-example";
  mcp_config.version = "0.0.0";
  mcp_config.tools = registry;
  rc = cai_mcp_handler_new(&mcp_config, &handler, &error);
  if (rc != CAI_OK) {
    fprintf(stderr, "MCP handler setup failed: %s\n",
            error.message != NULL ? error.message : cai_status_string(rc));
    cai_tool_registry_destroy(registry);
    cai_error_cleanup(&error);
    return 1;
  }
  listener = create_listener(port, &actual_port);
  if (listener < 0) {
    perror("listen");
    cai_mcp_handler_destroy(handler);
    cai_tool_registry_destroy(registry);
    cai_error_cleanup(&error);
    return 1;
  }
  if (print_port) {
    printf("%u\n", (unsigned int)actual_port);
    fflush(stdout);
  }
  for (;;) {
    if (request_limit > 0L && handled >= request_limit) {
      break;
    }
    client = accept(listener, NULL, NULL);
    if (client < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("accept");
      break;
    }
    handle_connection(client, handler);
    handled++;
    close(client);
  }
  close(listener);
  cai_mcp_handler_destroy(handler);
  cai_tool_registry_destroy(registry);
  cai_error_cleanup(&error);
  return 0;
}
