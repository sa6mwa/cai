#define _POSIX_C_SOURCE 200809L

#include "login.h"
#include <cai/auth.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

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

int cai_cli_login(const char *auth_json_path) {
  char redirect_uri[256];
  int port = 0;
  int server_fd;
  int client_fd;
  int rc;
  int exit_code = 1;
  char *authorize_url = NULL;
  char *auth_json_display = NULL;
  char request_buffer[16384];
  char bad_request_body[] = "Bad Request\n";
  char callback_failed_body[] = "OAuth callback failed\n";
  char *method;
  char *target;
  int response_owned;
  int callback_completed;
  cai_chatgpt_login_config login_config;
  cai_chatgpt_login_request login_request;
  cai_chatgpt_login_response login_response;
  cai_chatgpt_login *login = NULL;
  cai_error error;

  server_fd = listen_localhost(CAI_CHATGPT_AUTH_DEFAULT_CALLBACK_PORT, &port);
  if (server_fd < 0)
    server_fd =
        listen_localhost(CAI_CHATGPT_AUTH_FALLBACK_CALLBACK_PORT, &port);
  if (server_fd < 0) {
    fprintf(stderr, "cai: cannot listen for ChatGPT login callback: %s\n",
            strerror(errno));
    return 1;
  }
  snprintf(redirect_uri, sizeof(redirect_uri), "http://localhost:%d%s", port,
           CAI_CHATGPT_AUTH_DEFAULT_CALLBACK_PATH);
  cai_error_init(&error);
  cai_chatgpt_login_config_init(&login_config);
  login_config.auth_json_path = auth_json_path;
  login_config.redirect_uri = redirect_uri;
  rc = cai_chatgpt_login_start(&login_config, &login, &authorize_url, &error);
  if (rc != CAI_OK) {
    fprintf(stderr, "cai: start ChatGPT login: %s\n",
            error.message != NULL ? error.message : cai_status_string(rc));
    goto done;
  }
  fprintf(stderr, "\nOpen this URL to authenticate:\n\n%s\n\n", authorize_url);
  if (cai_chatgpt_login_open_browser(authorize_url, &error) != CAI_OK) {
    fprintf(stderr, "cai: browser did not open; use the URL above: %s\n",
            error.message != NULL ? error.message : "opener unavailable");
    cai_error_cleanup(&error);
    cai_error_init(&error);
  }
  fprintf(stderr, "Waiting for OAuth callback on %s\n", redirect_uri);
  for (;;) {
    client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
      if (errno == EINTR)
        continue;
      fprintf(stderr, "cai: login callback: %s\n", strerror(errno));
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
      write_http_response(client_fd, &login_response);
      close(client_fd);
      continue;
    }
    login_request.method = method;
    login_request.target = target;
    rc = login->handle_callback(login, &login_request, &login_response, &error);
    response_owned = rc == CAI_OK && login_response.body != NULL;
    if (rc != CAI_OK) {
      fprintf(stderr, "cai: OAuth callback failed: %s\n",
              error.message != NULL ? error.message : cai_status_string(rc));
      login_response.status = 500;
      login_response.content_type = "text/plain; charset=utf-8";
      login_response.body = callback_failed_body;
      login_response.completed = 1;
    }
    write_http_response(client_fd, &login_response);
    close(client_fd);
    callback_completed = login_response.completed;
    if (response_owned)
      cai_chatgpt_login_response_cleanup(&login_response);
    if (login->completed(login)) {
      if (auth_json_path != NULL && auth_json_path[0] != '\0') {
        fprintf(stderr, "ChatGPT auth saved to %s\n", auth_json_path);
      } else if (cai_chatgpt_auth_default_path(&auth_json_display, &error) ==
                 CAI_OK) {
        fprintf(stderr, "ChatGPT auth saved to %s\n", auth_json_display);
      } else {
        fprintf(stderr, "ChatGPT auth saved to cai state\n");
      }
      exit_code = 0;
      break;
    }
    if (callback_completed)
      break;
  }
done:
  cai_string_destroy(authorize_url);
  cai_string_destroy(auth_json_display);
  if (login != NULL)
    login->close(login);
  cai_error_cleanup(&error);
  close(server_fd);
  return exit_code;
}
