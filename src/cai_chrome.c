#define _POSIX_C_SOURCE 200809L
#include "cai_chrome_protocol.h"
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static int chrome_wait(cai_chrome *c, short events, long long deadline,
                       cai_error *error) {
  struct pollfd fd;
  int rc;
  long long remaining;
  fd.fd = c->fd;
  fd.events = events;
  fd.revents = 0;
  for (;;) {
    if (!c->cleanup && cai_stream_thread_cancel_requested())
      return cai_set_error(error, CAI_ERR_CANCELLED,
                           "Chrome request cancelled");
    remaining = deadline - cai_chrome_now_ms();
    if (remaining <= 0)
      return cai_set_error(
          error, CAI_ERR_TRANSPORT,
          "Chrome extension request timed out; the command may have run");
    rc = poll(&fd, 1U, remaining > 50LL ? 50 : (int)remaining);
    if (rc < 0 && errno == EINTR)
      continue;
    if (rc < 0)
      return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                  "Cannot poll Chrome host", strerror(errno));
    if (rc == 0)
      continue;
    if (fd.revents & events)
      return CAI_OK;
    if (fd.revents & (POLLERR | POLLHUP | POLLNVAL))
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "Chrome native host disconnected");
  }
}
static int chrome_bytes(cai_chrome *c, void *data, size_t length, int writing,
                        long long deadline, cai_error *error) {
  size_t done = 0U;
  int rc;
  ssize_t n;
  while (done < length) {
    rc = chrome_wait(c, writing ? POLLOUT : POLLIN, deadline, error);
    if (rc != CAI_OK)
      return rc;
    n = writing ? send(c->fd, (char *)data + done, length - done, MSG_NOSIGNAL)
                : recv(c->fd, (char *)data + done, length - done, 0);
    if (n < 0 && (errno == EINTR || errno == EAGAIN))
      continue;
    if (n <= 0)
      return cai_set_error_detail(
          error, CAI_ERR_TRANSPORT,
          "Chrome native host disconnected during a frame",
          n == 0 ? "EOF" : strerror(errno));
    done += (size_t)n;
  }
  return CAI_OK;
}
static int chrome_frame(cai_chrome *c, char **out, long long deadline,
                        cai_error *error) {
  uint32_t length;
  char *json;
  int rc;
  *out = NULL;
  rc = chrome_bytes(c, &length, sizeof(length), 0, deadline, error);
  if (rc != CAI_OK)
    return rc;
  if (length == 0U || (size_t)length > c->limit)
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "Chrome response frame exceeds the configured limit");
  json = cai_alloc(NULL, (size_t)length + 1U);
  if (json == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "Cannot allocate Chrome response frame");
  rc = chrome_bytes(c, json, length, 0, deadline, error);
  if (rc != CAI_OK) {
    cai_free_mem(NULL, json);
    return rc;
  }
  if (memchr(json, '\0', length) != NULL) {
    cai_free_mem(NULL, json);
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "Chrome frame contains a NUL byte");
  }
  json[length] = '\0';
  *out = json;
  return CAI_OK;
}
int cai_chrome_request(cai_chrome *c, const char *method, const char *params,
                       char **out, cai_error *error) {
  cai_chrome_rpc request;
  cai_chrome_rpc reply;
  char id[80];
  char *json = NULL;
  char *frame = NULL;
  char *reply_id = NULL;
  uint32_t length;
  long long deadline;
  int rc;
  if (out != NULL)
    *out = NULL;
  if (c != NULL && c->fd < 0)
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "Chrome connection is closed; check the ChatGPT "
                         "extension and reopen the browser connection");
  if (c == NULL || method == NULL || method[0] == '\0' || params == NULL ||
      out == NULL || params[strspn(params, " \r\n\t")] != '{')
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Chrome request requires an open connection, method, "
                         "object params and output");
  if (strlen(params) > CAI_CHROME_REQUEST_LIMIT)
    return cai_set_error(error, CAI_ERR_LIMIT, "Chrome request is too large");
  memset(&request, 0, sizeof(request));
  request.jsonrpc = "2.0";
  request.method = method;
  snprintf(id, sizeof(id), "\"cai:%ld:%lu\"", (long)getpid(), ++c->sequence);
  rc = cai_chrome_value_buffer(&request.id, id, error);
  if (rc == CAI_OK)
    rc = cai_chrome_value_buffer(&request.params, params, error);
  if (rc == CAI_OK) {
    json = cai_chrome_serialize(&cai_chrome_request_map, &request, error);
    if (json == NULL)
      rc = error != NULL ? error->code : CAI_ERR_PROTOCOL;
  }
  CAI_LJ_CHROME->json_value_cleanup(CAI_LJ_CHROME, &request.id);
  CAI_LJ_CHROME->json_value_cleanup(CAI_LJ_CHROME, &request.params);
  if (rc != CAI_OK)
    return rc;
  if (strlen(json) > CAI_CHROME_REQUEST_LIMIT) {
    cai_free_mem(NULL, json);
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "Chrome request frame exceeds 1 MiB");
  }
  if (c->logger != NULL)
    c->logger->debugf(c->logger, "Chrome extension request", "method=%s",
                      method);
  deadline = cai_chrome_now_ms() + (c->cleanup ? 2000L : c->timeout_ms);
  length = (uint32_t)strlen(json);
  rc = chrome_bytes(c, &length, sizeof(length), 1, deadline, error);
  if (rc == CAI_OK)
    rc = chrome_bytes(c, json, length, 1, deadline, error);
  cai_free_mem(NULL, json);
  while (rc == CAI_OK) {
    rc = chrome_frame(c, &frame, deadline, error);
    if (rc != CAI_OK)
      break;
    rc = cai_chrome_parse(frame, &reply, error);
    cai_free_mem(NULL, frame);
    frame = NULL;
    if (rc == CAI_OK && reply.method != NULL) {
      /* The native host answers server requests, so these are notifications. */
      if (reply.has_id)
        rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                           "Unexpected request from Chrome native host");
      CAI_LJ_CHROME->cleanup(CAI_LJ_CHROME, &cai_chrome_rpc_map, &reply);
      continue;
    }
    if (rc == CAI_OK) {
      reply_id = cai_chrome_value(&reply.id, error);
      if (reply_id == NULL || strcmp(reply_id, id) != 0)
        rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                           "Chrome reply has an unexpected request ID");
      cai_free_mem(NULL, reply_id);
    }
    if (rc == CAI_OK && reply.error.has_code && reply.error.message != NULL) {
      char detail[96];
      snprintf(detail, sizeof(detail), "Chrome JSON-RPC error %lld; method %s",
               (long long)reply.error.code, method);
      rc = cai_set_error_detail(error, CAI_ERR_SERVER, reply.error.message,
                                detail);
    } else if (rc == CAI_OK &&
               (reply.error.has_code || reply.error.message != NULL ||
                !reply.has_result)) {
      rc = cai_set_error(
          error, CAI_ERR_PROTOCOL,
          "Chrome reply must contain a result or a complete error");
    } else if (rc == CAI_OK) {
      *out = cai_chrome_value(&reply.result, error);
      if (*out == NULL)
        rc = CAI_ERR_NOMEM;
    }
    CAI_LJ_CHROME->cleanup(CAI_LJ_CHROME, &cai_chrome_rpc_map, &reply);
    break;
  }
  if (rc != CAI_OK && rc != CAI_ERR_SERVER) {
    close(c->fd);
    c->fd = -1;
  }
  if (rc != CAI_OK && c->logger != NULL)
    c->logger->warnf(c->logger, "Chrome extension request failed",
                     "method=%s error=%s detail=%s", method,
                     error != NULL ? error->message : "request failed",
                     error != NULL && error->detail != NULL ? error->detail
                                                            : "");
  return rc;
}

typedef struct chrome_info {
  char *type;
  char *family;
} chrome_info;
static const lonejson_field info_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(chrome_info, type, "type"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(chrome_info, family, "family")};
LONEJSON_MAP_DEFINE(info_map, chrome_info, info_fields);

int cai_chrome_open(const cai_chrome_config *config, cai_chrome **out,
                    cai_error *error) {
  cai_chrome *c;
  DIR *directory;
  struct dirent *entry;
  char path[4096];
  char selected[4096];
  char *json = NULL;
  lonejson_error je;
  chrome_info info;
  int rc;
  int probe;
  size_t candidates = 0U;
  if (out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID, "Chrome output is required");
  *out = NULL;
  if (config == NULL || config->timeout_ms < 0L ||
      config->timeout_ms > 60000L ||
      config->max_response_bytes > 64U * 1024U * 1024U)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Invalid Chrome configuration");
  selected[0] = '\0';
  if (config->socket_path != NULL) {
    if (strlen(config->socket_path) >= sizeof(selected))
      return cai_set_error(error, CAI_ERR_INVALID,
                           "Chrome socket path is too long");
    strcpy(selected, config->socket_path);
  } else {
    rc = cai_chrome_private_directory(config->socket_directory, error);
    if (rc != CAI_OK)
      return rc;
    directory = opendir(config->socket_directory);
    if (directory == NULL)
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "Cannot list Chrome sockets");
    while ((entry = readdir(directory)) != NULL) {
      size_t n = strlen(entry->d_name);
      if (n < 6U || strcmp(entry->d_name + n - 5U, ".sock") != 0)
        continue;
      if (snprintf(path, sizeof(path), "%s/%s", config->socket_directory,
                   entry->d_name) >= (int)sizeof(path))
        continue;
      /* Ignore stale files, but do not choose arbitrarily between live hosts.
       */
      {
        cai_error ignored;
        cai_error_init(&ignored);
        rc = cai_chrome_connect_socket(path, &probe, &ignored);
        cai_error_cleanup(&ignored);
      }
      if (rc != CAI_OK)
        continue;
      close(probe);
      candidates++;
      strcpy(selected, path);
    }
    closedir(directory);
    if (candidates != 1U)
      return cai_set_error(
          error, CAI_ERR_INVALID,
          candidates == 0U
              ? "No cai Chrome host is connected. Run cai --chrome-setup, then "
                "open the ChatGPT extension in Chrome."
              : "Multiple Chrome hosts are connected; select one with "
                "--chrome-socket PATH");
  }
  c = cai_alloc(NULL, sizeof(*c));
  if (c == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "Cannot allocate Chrome connection");
  memset(c, 0, sizeof(*c));
  c->fd = -1;
  c->timeout_ms = config->timeout_ms != 0L ? config->timeout_ms : 30000L;
  c->limit = config->max_response_bytes != 0U ? config->max_response_bytes
                                              : CAI_CHROME_RESPONSE_LIMIT;
  c->logger = config->logger;
  rc = cai_chrome_connect_socket(selected, &c->fd, error);
  if (rc == CAI_OK)
    rc = cai_chrome_request(c, "getInfo", "{}", &json, error);
  memset(&info, 0, sizeof(info));
  lonejson_error_init(&je);
  if (rc == CAI_OK &&
      CAI_LJ_CHROME->parse_cstr(CAI_LJ_CHROME, &info_map, &info, json, &je) !=
          LONEJSON_STATUS_OK)
    rc = cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                              "Invalid Chrome extension identity", je.message);
  if (rc == CAI_OK && (info.type == NULL || info.family == NULL ||
                       strcmp(info.type, "extension") != 0 ||
                       strcmp(info.family, "chrome") != 0))
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "The selected host is not a Chrome extension backend");
  CAI_LJ_CHROME->cleanup(CAI_LJ_CHROME, &info_map, &info);
  cai_free_mem(NULL, json);
  if (rc != CAI_OK) {
    cai_chrome_close(c);
    return rc;
  }
  *out = c;
  return CAI_OK;
}
int cai_chrome_begin_turn(cai_chrome *c, const char *session, const char *turn,
                          cai_error *error) {
  char *s, *t;
  if (c == NULL || session == NULL || session[0] == '\0' || turn == NULL ||
      turn[0] == '\0' || c->used)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Chrome requires a session and turn identity, with "
                         "the previous turn released");
  s = cai_strdup(NULL, session);
  t = cai_strdup(NULL, turn);
  if (s == NULL || t == NULL) {
    cai_free_mem(NULL, s);
    cai_free_mem(NULL, t);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "Cannot allocate Chrome turn identity");
  }
  cai_free_mem(NULL, c->session_id);
  cai_free_mem(NULL, c->turn_id);
  c->session_id = s;
  c->turn_id = t;
  return CAI_OK;
}
/* Shared with the chrome tool for lifecycle requests. */
typedef struct chrome_turn {
  const char *session;
  const char *turn;
  const char *context;
} chrome_turn;
static const lonejson_field turn_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(chrome_turn, session, "session_id"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(chrome_turn, turn, "turn_id"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(chrome_turn, context, "session_context")};
LONEJSON_MAP_DEFINE(turn_map, chrome_turn, turn_fields);
int cai_chrome_end_turn(cai_chrome *c, cai_error *error) {
  chrome_turn doc;
  char *params, *result = NULL;
  int rc;
  if (c == NULL || !c->used)
    return CAI_OK;
  c->used = 0;
  cai_free_mem(NULL, c->pending_image);
  c->pending_image = NULL;
  if (c->fd < 0)
    return CAI_OK; /* Native host releases leases on disconnect. */
  doc.session = c->session_id;
  doc.turn = c->turn_id;
  doc.context = "cached";
  params = cai_chrome_serialize(&turn_map, &doc, error);
  if (params == NULL)
    return CAI_ERR_NOMEM;
  c->cleanup = 1;
  rc = cai_chrome_request(c, "turnEnded", params, &result, error);
  c->cleanup = 0;
  if (rc != CAI_OK && c->fd >= 0) {
    /* A rejected cleanup leaves ownership uncertain. Disconnect so the host
     * releases it; do not start another turn on that connection. */
    close(c->fd);
    c->fd = -1;
  }
  cai_free_mem(NULL, params);
  cai_free_mem(NULL, result);
  return rc;
}
void cai_chrome_close(cai_chrome *c) {
  cai_error error;
  if (c == NULL)
    return;
  cai_error_init(&error);
  (void)cai_chrome_end_turn(c, &error);
  cai_error_cleanup(&error);
  if (c->fd >= 0)
    close(c->fd);
  cai_free_mem(NULL, c->session_id);
  cai_free_mem(NULL, c->turn_id);
  cai_free_mem(NULL, c->pending_image);
  cai_free_mem(NULL, c);
}

void cai_chrome_result_free(char *json) { cai_free_mem(NULL, json); }
