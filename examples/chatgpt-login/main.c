#include <cai/auth.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static int print_error(const char *operation, int rc, const cai_error *error) {
  fprintf(stderr, "%s failed: %s\n", operation,
          error->message != NULL ? error->message : cai_status_string(rc));
  if (error->detail != NULL) {
    fprintf(stderr, "detail: %s\n", error->detail);
  }
  return 1;
}

static void print_help(const char *program) {
  fprintf(stderr,
          "usage: %s [--auth-json <path>] [--port <port>] [--issuer <url>] "
          "[--browser-command <cmd>] [--no-open-browser]\n\n",
          program != NULL ? program : "cai_example_chatgpt_login");
  fprintf(stderr,
          "  --auth-json <path>    Codex-style auth.json path to write; "
          "default is cai's XDG auth path.\n"
          "  --port <port>         Local callback port, default 1455.\n"
          "  --issuer <url>        OAuth issuer, default "
          "https://auth.openai.com.\n");
  fprintf(stderr,
          "  --browser-command <cmd>\n"
          "                         Browser opener command; default is "
          "platform selected.\n"
          "  --no-open-browser     Print the URL without launching a "
          "browser.\n\n"
          "CAI_CHATGPT_AUTH_JSON can override the default auth-json path.\n");
}

static int parse_port(const char *text, int *out) {
  char *end;
  long value;

  errno = 0;
  value = strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value <= 0L ||
      value > 65535L) {
    return 0;
  }
  *out = (int)value;
  return 1;
}

static int parse_args(int argc, char **argv, const char **auth_json,
                      const char **issuer, int *port,
                      const char **browser_command, int *open_browser) {
  int i;

  *auth_json = getenv("CAI_CHATGPT_AUTH_JSON");
  *issuer = NULL;
  *port = CAI_CHATGPT_AUTH_DEFAULT_CALLBACK_PORT;
  *browser_command = NULL;
  *open_browser = 1;
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--auth-json") == 0) {
      if (i + 1 >= argc || argv[i + 1][0] == '\0') {
        fprintf(stderr, "--auth-json requires a path\n");
        return 0;
      }
      *auth_json = argv[++i];
      continue;
    }
    if (strcmp(argv[i], "--issuer") == 0) {
      if (i + 1 >= argc || argv[i + 1][0] == '\0') {
        fprintf(stderr, "--issuer requires a URL\n");
        return 0;
      }
      *issuer = argv[++i];
      continue;
    }
    if (strcmp(argv[i], "--port") == 0) {
      if (i + 1 >= argc || !parse_port(argv[i + 1], port)) {
        fprintf(stderr, "--port requires a valid TCP port\n");
        return 0;
      }
      i++;
      continue;
    }
    if (strcmp(argv[i], "--browser-command") == 0) {
      if (i + 1 >= argc || argv[i + 1][0] == '\0') {
        fprintf(stderr, "--browser-command requires a command\n");
        return 0;
      }
      *browser_command = argv[++i];
      continue;
    }
    if (strcmp(argv[i], "--no-open-browser") == 0) {
      *open_browser = 0;
      continue;
    }
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      print_help(argv[0]);
      return -1;
    }
    fprintf(stderr, "unknown argument: %s\n", argv[i]);
    print_help(argv[0]);
    return 0;
  }
  return 1;
}

static int listen_localhost(int requested_port, int *out_port) {
  struct sockaddr_in addr;
  socklen_t addr_len;
  int fd;
  int one;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  one = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons((unsigned short)requested_port);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  if (listen(fd, 4) != 0) {
    close(fd);
    return -1;
  }
  addr_len = (socklen_t)sizeof(addr);
  if (getsockname(fd, (struct sockaddr *)&addr, &addr_len) != 0) {
    close(fd);
    return -1;
  }
  *out_port = (int)ntohs(addr.sin_port);
  return fd;
}

static int read_http_request(int fd, char *buffer, size_t capacity) {
  size_t length;
  ssize_t nread;

  length = 0U;
  while (length + 1U < capacity) {
    nread = read(fd, buffer + length, capacity - length - 1U);
    if (nread < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (nread == 0) {
      break;
    }
    length += (size_t)nread;
    buffer[length] = '\0';
    if (strstr(buffer, "\r\n\r\n") != NULL || strstr(buffer, "\n\n") != NULL) {
      return 0;
    }
  }
  buffer[length] = '\0';
  return 0;
}

static int parse_request_line(char *request, char **method, char **target) {
  char *sp1;
  char *sp2;

  sp1 = strchr(request, ' ');
  if (sp1 == NULL) {
    return 0;
  }
  *sp1 = '\0';
  sp2 = strchr(sp1 + 1, ' ');
  if (sp2 == NULL) {
    return 0;
  }
  *sp2 = '\0';
  *method = request;
  *target = sp1 + 1;
  return 1;
}

static void write_all_ignore_errors(int fd, const char *data, size_t length) {
  size_t written;
  ssize_t count;

  written = 0U;
  while (written < length) {
    count = write(fd, data + written, length - written);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;
    }
    if (count == 0) {
      return;
    }
    written += (size_t)count;
  }
}

static void write_http_response(int fd,
                                const cai_chatgpt_login_response *response) {
  const char *body;
  const char *content_type;
  const char *status_text;
  char header[512];
  int status;
  int header_len;

  status = response != NULL && response->status != 0 ? response->status : 500;
  status_text = status == 200   ? "OK"
                : status == 400 ? "Bad Request"
                : status == 404 ? "Not Found"
                : status == 405 ? "Method Not Allowed"
                                : "Internal Server Error";
  body = response != NULL && response->body != NULL ? response->body : "";
  content_type = response != NULL && response->content_type != NULL
                     ? response->content_type
                     : "text/plain; charset=utf-8";
  header_len =
      snprintf(header, sizeof(header),
               "HTTP/1.1 %d %s\r\n"
               "Content-Type: %s\r\n"
               "Content-Length: %lu\r\n"
               "Connection: close\r\n\r\n",
               status, status_text, content_type, (unsigned long)strlen(body));
  if (header_len > 0 && (size_t)header_len < sizeof(header)) {
    write_all_ignore_errors(fd, header, (size_t)header_len);
  }
  write_all_ignore_errors(fd, body, strlen(body));
}

int main(int argc, char **argv) {
  const char *auth_json;
  const char *issuer;
  const char *browser_command;
  char redirect_uri[256];
  int requested_port;
  int port;
  int open_browser_flag;
  int server_fd;
  int client_fd;
  int rc;
  int exit_code;
  char *authorize_url;
  char *auth_json_display;
  char request_buffer[16384];
  char bad_request_body[] = "Bad Request\n";
  char callback_failed_body[] = "OAuth callback failed\n";
  char *method;
  char *target;
  int response_owned;
  cai_chatgpt_login_config login_config;
  cai_chatgpt_login_browser_config browser_config;
  cai_chatgpt_login_request login_request;
  cai_chatgpt_login_response login_response;
  cai_chatgpt_login *login;
  cai_error error;

  rc = parse_args(argc, argv, &auth_json, &issuer, &requested_port,
                  &browser_command, &open_browser_flag);
  if (rc < 0) {
    return 0;
  }
  if (rc == 0) {
    return 2;
  }
  port = 0;
  server_fd = listen_localhost(requested_port, &port);
  if (server_fd < 0 &&
      requested_port == CAI_CHATGPT_AUTH_DEFAULT_CALLBACK_PORT) {
    server_fd =
        listen_localhost(CAI_CHATGPT_AUTH_FALLBACK_CALLBACK_PORT, &port);
  }
  if (server_fd < 0) {
    perror("listen");
    return 1;
  }
  snprintf(redirect_uri, sizeof(redirect_uri), "http://localhost:%d%s", port,
           CAI_CHATGPT_AUTH_DEFAULT_CALLBACK_PATH);

  cai_error_init(&error);
  cai_chatgpt_login_config_init(&login_config);
  cai_chatgpt_login_browser_config_init(&browser_config);
  login_config.auth_json_path = auth_json;
  login_config.redirect_uri = redirect_uri;
  login_config.issuer = issuer;
  browser_config.command = browser_command;
  login = NULL;
  authorize_url = NULL;
  auth_json_display = NULL;
  exit_code = 1;
  rc = cai_chatgpt_login_start(&login_config, &login, &authorize_url, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_chatgpt_login_start", rc, &error);
    goto done;
  }
  fprintf(stderr, "Open this URL to authenticate:\n%s\n\n", authorize_url);
  fprintf(stderr, "Waiting for OAuth callback on %s\n", redirect_uri);
  if (open_browser_flag &&
      cai_chatgpt_login_open_browser_with_config(&browser_config, authorize_url,
                                                 &error) != CAI_OK) {
    fprintf(stderr, "Could not launch browser; open the URL manually.\n");
    cai_error_cleanup(&error);
    cai_error_init(&error);
  }

  for (;;) {
    client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("accept");
      break;
    }
    memset(request_buffer, 0, sizeof(request_buffer));
    memset(&login_response, 0, sizeof(login_response));
    response_owned = 0;
    if (read_http_request(client_fd, request_buffer, sizeof(request_buffer)) !=
            0 ||
        !parse_request_line(request_buffer, &method, &target)) {
      login_response.status = 400;
      login_response.content_type = "text/plain; charset=utf-8";
      login_response.body = bad_request_body;
      login_response.completed = 0;
      write_http_response(client_fd, &login_response);
      close(client_fd);
      continue;
    }
    login_request.method = method;
    login_request.target = target;
    rc = login->handle_callback(login, &login_request, &login_response, &error);
    response_owned = rc == CAI_OK && login_response.body != NULL;
    if (rc != CAI_OK) {
      fprintf(stderr, "OAuth callback failed: %s\n",
              error.message != NULL ? error.message : cai_status_string(rc));
      login_response.status = 500;
      login_response.content_type = "text/plain; charset=utf-8";
      login_response.body = callback_failed_body;
      login_response.completed = 1;
    }
    write_http_response(client_fd, &login_response);
    close(client_fd);
    if (response_owned) {
      cai_chatgpt_login_response_cleanup(&login_response);
    }
    if (login->completed(login)) {
      if (auth_json != NULL && auth_json[0] != '\0') {
        fprintf(stderr, "ChatGPT auth saved to %s\n", auth_json);
      } else if (cai_chatgpt_auth_default_path(&auth_json_display, &error) ==
                 CAI_OK) {
        fprintf(stderr, "ChatGPT auth saved to %s\n", auth_json_display);
      } else {
        cai_error_cleanup(&error);
        cai_error_init(&error);
        fprintf(stderr, "ChatGPT auth saved to default auth path\n");
      }
      exit_code = 0;
      break;
    }
    if (login_response.completed) {
      exit_code = 1;
      break;
    }
  }

done:
  cai_string_destroy(authorize_url);
  cai_string_destroy(auth_json_display);
  if (login != NULL) {
    login->close(login);
  }
  cai_error_cleanup(&error);
  close(server_fd);
  return exit_code;
}
