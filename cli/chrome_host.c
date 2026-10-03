#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include "../src/cai_chrome_protocol.h"
#include "chrome.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define HOST_QUEUE_LIMIT (32U * 1024U * 1024U)
#define EXTENSION_ORIGIN "chrome-extension://hehggadaopoacecdllhhajmbjkdcmajg/"

typedef struct host_frame {
  unsigned char header[4];
  size_t header_used;
  uint32_t length;
  char *data;
  size_t used;
  long long deadline;
} host_frame;
typedef struct host_queue {
  char *data;
  size_t length;
  size_t sent;
  long long deadline;
} host_queue;
typedef struct chrome_host {
  int client;
  host_frame extension_frame, client_frame;
  host_queue extension_queue, client_queue;
  char *session, *turn, *pending_id;
  int pending_end;
  int abandoned;
  long long pending_deadline;
  int cleanup_pending;
  pslog_logger *logger;
} chrome_host;
typedef struct host_identity {
  const char *session;
  const char *turn;
  const char *context;
} host_identity;
static const lonejson_field identity_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(host_identity, session, "session_id"),
    LONEJSON_FIELD_STRING_ALLOC(host_identity, turn, "turn_id"),
    LONEJSON_FIELD_STRING_ALLOC(host_identity, context, "session_context")};
LONEJSON_MAP_DEFINE(identity_map, host_identity, identity_fields);
static const char *const browser_methods[] = {
    "getInfo",     "getUserTabs", "getTabs",    "createTab", "claimUserTab",
    "attach",      "detach",      "executeCdp", "markTab",   "finalizeTabs",
    "nameSession", "turnEnded",   NULL};
static int allowed_method(const char *method) {
  size_t i;
  for (i = 0U; browser_methods[i] != NULL; i++)
    if (strcmp(method, browser_methods[i]) == 0)
      return 1;
  return 0;
}
static void frame_clear(host_frame *f) {
  free(f->data);
  memset(f, 0, sizeof(*f));
}
static void queue_clear(host_queue *q) {
  free(q->data);
  memset(q, 0, sizeof(*q));
}
static int enqueue(host_queue *q, const char *json, cai_error *error) {
  uint32_t n;
  size_t length = strlen(json), live = q->length - q->sent;
  char *replacement;
  if (length == 0U || length > CAI_CHROME_RESPONSE_LIMIT ||
      live > HOST_QUEUE_LIMIT - length - 4U)
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "Chrome host output queue exceeded its bounded limit");
  if (q->sent != 0U)
    memmove(q->data, q->data + q->sent, live);
  q->sent = 0U;
  q->length = live;
  replacement = realloc(q->data, live + length + 4U);
  if (replacement == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "Cannot allocate Chrome host output queue");
  q->data = replacement;
  n = (uint32_t)length;
  memcpy(q->data + live, &n, 4U);
  memcpy(q->data + live + 4U, json, length);
  q->sent = 0U;
  q->length = live + length + 4U;
  if (q->deadline == 0LL)
    q->deadline = cai_chrome_now_ms() + 5000LL;
  return CAI_OK;
}
static int flush_queue(host_queue *q, int fd, int socket_fd, cai_error *error) {
  ssize_t n;
  if (q->sent == q->length)
    return CAI_OK;
  n = socket_fd ? send(fd, q->data + q->sent, q->length - q->sent, MSG_NOSIGNAL)
                : write(fd, q->data + q->sent, q->length - q->sent);
  if (n < 0 && (errno == EAGAIN || errno == EINTR))
    return CAI_OK;
  if (n <= 0)
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "Chrome host output disconnected",
                                strerror(errno));
  q->sent += (size_t)n;
  if (q->sent == q->length)
    queue_clear(q);
  return CAI_OK;
}
/* Incremental framing keeps heartbeat handling live during fragmented input.
 * Exactly one bounded frame is materialized for JSON dispatch. */
static int read_frame(host_frame *f, int fd, size_t limit, int *ready,
                      cai_error *error) {
  ssize_t n;
  *ready = 0;
  if (f->header_used < 4U) {
    n = read(fd, f->header + f->header_used, 4U - f->header_used);
    if (n < 0 && (errno == EAGAIN || errno == EINTR))
      return CAI_OK;
    if (n <= 0)
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "Chrome host input disconnected");
    if (f->deadline == 0LL)
      f->deadline = cai_chrome_now_ms() + 5000LL;
    f->header_used += (size_t)n;
    if (f->header_used < 4U)
      return CAI_OK;
    memcpy(&f->length, f->header, 4U);
    if (f->length == 0U || (size_t)f->length > limit)
      return cai_set_error(error, CAI_ERR_LIMIT,
                           "Chrome host received an oversized or empty frame");
    f->data = malloc((size_t)f->length + 1U);
    if (f->data == NULL)
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "Cannot allocate Chrome host input frame");
  }
  n = read(fd, f->data + f->used,
           (size_t)f->length - f->used > 32768U ? 32768U
                                                : (size_t)f->length - f->used);
  if (n < 0 && (errno == EAGAIN || errno == EINTR))
    return CAI_OK;
  if (n <= 0)
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "Chrome host disconnected during a frame");
  f->used += (size_t)n;
  if (f->used == f->length) {
    if (memchr(f->data, '\0', f->used) != NULL)
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "Chrome host frame contains a NUL byte");
    f->data[f->used] = '\0';
    *ready = 1;
  }
  return CAI_OK;
}
static void clear_identity(chrome_host *h) {
  free(h->session);
  free(h->turn);
  h->session = NULL;
  h->turn = NULL;
}
static void drop_client(chrome_host *h) {
  if (h->client >= 0)
    close(h->client);
  h->client = -1;
  frame_clear(&h->client_frame);
  queue_clear(&h->client_queue);
  if (h->session != NULL)
    h->abandoned = 1;
}
static int response_queue(host_queue *q, cai_chrome_rpc *rpc, int success,
                          cai_error *error) {
  char *json = cai_chrome_serialize(
      success ? &cai_chrome_success_map : &cai_chrome_failure_map, rpc, error);
  int rc;
  if (json == NULL)
    return CAI_ERR_PROTOCOL;
  rc = enqueue(q, json, error);
  free(json);
  return rc;
}
static int host_cleanup(chrome_host *h, cai_error *error) {
  cai_chrome_rpc rpc;
  host_identity identity;
  char *params;
  int rc;
  if (!h->abandoned || h->pending_id != NULL || h->session == NULL)
    return CAI_OK;
  memset(&rpc, 0, sizeof(rpc));
  identity.session = h->session;
  identity.turn = h->turn;
  identity.context = "cached";
  params = cai_chrome_serialize(&identity_map, &identity, error);
  if (params == NULL)
    return CAI_ERR_NOMEM;
  rpc.jsonrpc = "2.0";
  rpc.method = "turnEnded";
  rc = cai_chrome_value_buffer(&rpc.id, "\"cai-cleanup\"", error);
  if (rc == CAI_OK)
    rc = cai_chrome_value_buffer(&rpc.params, params, error);
  if (rc == CAI_OK) {
    char *json = cai_chrome_serialize(&cai_chrome_request_map, &rpc, error);
    if (json == NULL)
      rc = CAI_ERR_PROTOCOL;
    else {
      rc = enqueue(&h->extension_queue, json, error);
      free(json);
    }
  }
  CAI_LJ_CHROME->json_value_cleanup(CAI_LJ_CHROME, &rpc.id);
  CAI_LJ_CHROME->json_value_cleanup(CAI_LJ_CHROME, &rpc.params);
  free(params);
  if (rc == CAI_OK) {
    h->pending_id = cai_strdup(NULL, "\"cai-cleanup\"");
    if (h->pending_id == NULL)
      return cai_set_error(error, CAI_ERR_NOMEM, "Cannot track Chrome cleanup");
    h->pending_deadline = cai_chrome_now_ms() + 5000LL;
    h->cleanup_pending = 1;
  }
  return rc;
}
static int dispatch_client(chrome_host *h, const char *json, cai_error *error) {
  cai_chrome_rpc rpc;
  host_identity identity;
  char *params = NULL, *id = NULL;
  lonejson_error je;
  int rc;
  memset(&identity, 0, sizeof(identity));
  lonejson_error_init(&je);
  rc = cai_chrome_parse(json, &rpc, error);
  if (rc != CAI_OK)
    goto done;
  if (rpc.method == NULL || !rpc.has_id || h->pending_id != NULL) {
    rc = cai_set_error(
        error, CAI_ERR_PROTOCOL,
        "Chrome client must send one identified request at a time");
    goto done;
  }
  if (!allowed_method(rpc.method)) {
    cai_chrome_rpc response;
    memset(&response, 0, sizeof(response));
    response.jsonrpc = "2.0";
    response.id = rpc.id;
    response.error.has_code = 1;
    response.error.code = -32601;
    response.error.message = "This CAI host supports browser control only";
    rc = response_queue(&h->client_queue, &response, 0, error);
    goto done;
  }
  params = cai_chrome_value(&rpc.params, error);
  if (params == NULL || params[0] != '{') {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "Chrome request params must be an object");
    goto done;
  }
  if (CAI_LJ_CHROME->parse_cstr(CAI_LJ_CHROME, &identity_map, &identity, params,
                                &je) != LONEJSON_STATUS_OK) {
    rc = cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                              "Invalid Chrome session identity", je.message);
    goto done;
  }
  if (strcmp(rpc.method, "getInfo") != 0 &&
      strcmp(rpc.method, "getUserTabs") != 0) {
    if (identity.session == NULL || identity.turn == NULL ||
        identity.session[0] == '\0' || identity.turn[0] == '\0' ||
        strlen(identity.session) > 128U || strlen(identity.turn) > 128U ||
        (h->session != NULL && (strcmp(h->session, identity.session) != 0 ||
                                strcmp(h->turn, identity.turn) != 0))) {
      rc = cai_set_error(
          error, CAI_ERR_PROTOCOL,
          "Chrome request changed or omitted its active session/turn identity");
      goto done;
    }
    if (h->session == NULL) {
      h->session = cai_strdup(NULL, identity.session);
      h->turn = cai_strdup(NULL, identity.turn);
      if (h->session == NULL || h->turn == NULL) {
        rc = CAI_ERR_NOMEM;
        goto done;
      }
    }
  }
  id = cai_chrome_value(&rpc.id, error);
  if (id == NULL ||
      (id[0] != '"' && id[0] != '-' && (id[0] < '0' || id[0] > '9'))) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "Chrome request ID must be a string or number");
    goto done;
  }
  rc = enqueue(&h->extension_queue, json, error);
  if (rc == CAI_OK) {
    h->pending_id = id;
    id = NULL;
    h->pending_end = strcmp(rpc.method, "turnEnded") == 0;
    h->pending_deadline = cai_chrome_now_ms() + 60000LL;
  }
done:
  CAI_LJ_CHROME->cleanup(CAI_LJ_CHROME, &identity_map, &identity);
  CAI_LJ_CHROME->cleanup(CAI_LJ_CHROME, &cai_chrome_rpc_map, &rpc);
  free(params);
  free(id);
  return rc;
}
static int dispatch_extension(chrome_host *h, const char *json,
                              cai_error *error) {
  cai_chrome_rpc rpc;
  int rc = cai_chrome_parse(json, &rpc, error);
  if (rc != CAI_OK)
    goto done;
  if (rpc.method != NULL && rpc.has_id) {
    cai_chrome_rpc response;
    memset(&response, 0, sizeof(response));
    response.jsonrpc = "2.0";
    response.id = rpc.id;
    if (strcmp(rpc.method, "ping") == 0) {
      rc = cai_chrome_value_buffer(&response.result, "\"pong\"", error);
      if (rc == CAI_OK)
        rc = response_queue(&h->extension_queue, &response, 1, error);
      CAI_LJ_CHROME->json_value_cleanup(CAI_LJ_CHROME, &response.result);
    } else {
      response.error.has_code = 1;
      response.error.code = -32601;
      response.error.message = "CAI implements browser control; OpenAI "
                               "app-server functions are unavailable";
      rc = response_queue(&h->extension_queue, &response, 0, error);
    }
  } else if (rpc.method != NULL) {
    if (h->client >= 0)
      rc = enqueue(&h->client_queue, json, error);
  } else {
    char *id = cai_chrome_value(&rpc.id, error);
    if (id == NULL || h->pending_id == NULL || strcmp(id, h->pending_id) != 0)
      rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                         "Chrome extension returned an unexpected request ID");
    else {
      if (h->client >= 0 && !h->cleanup_pending)
        rc = enqueue(&h->client_queue, json, error);
      if (h->pending_end || h->cleanup_pending) {
        if (rpc.error.has_code) {
          rc = cai_set_error_detail(error, CAI_ERR_SERVER,
                                    "Chrome could not release browser control",
                                    rpc.error.message);
        } else
          clear_identity(h);
      }
      free(h->pending_id);
      h->pending_id = NULL;
      h->pending_end = 0;
      h->cleanup_pending = 0;
      if (h->session == NULL)
        h->abandoned = 0;
    }
    free(id);
  }
done:
  CAI_LJ_CHROME->cleanup(CAI_LJ_CHROME, &cai_chrome_rpc_map, &rpc);
  return rc;
}
int cai_cli_chrome_host(const char *origin, pslog_logger *logger,
                        cai_error *error) {
  chrome_host h;
  char directory[4096], path[4096];
  struct sockaddr_un address;
  struct pollfd fds[4];
  int listener = -1, rc, ready, accept_fd, old_in, old_out;
  mode_t old_mask;
  long long now;
  memset(&h, 0, sizeof(h));
  h.client = -1;
  h.logger = logger;
  if (origin == NULL || strcmp(origin, EXTENSION_ORIGIN) != 0)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "CAI native host accepts only the official ChatGPT "
                         "Chrome extension origin");
  rc = cai_cli_chrome_directory(directory, sizeof(directory), 1, error);
  if (rc != CAI_OK)
    return rc;
  if (snprintf(path, sizeof(path), "%s/%ld.sock", directory, (long)getpid()) >=
      (int)sizeof(address.sun_path))
    return cai_set_error(
        error, CAI_ERR_INVALID,
        "Chrome socket path exceeds the Unix socket path limit");
  listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (listener < 0)
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "Cannot create Chrome host listener",
                                strerror(errno));
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  strcpy(address.sun_path, path);
  old_mask = umask(0077);
  rc = bind(listener, (struct sockaddr *)&address, sizeof(address));
  umask(old_mask);
  if (rc != 0 || listen(listener, 8) != 0) {
    int saved = errno;
    if (rc == 0)
      (void)unlink(path);
    close(listener);
    errno = saved;
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "Cannot bind Chrome host socket",
                                strerror(errno));
  }
  old_in = fcntl(STDIN_FILENO, F_GETFL);
  old_out = fcntl(STDOUT_FILENO, F_GETFL);
  if (old_in < 0 || old_out < 0 ||
      fcntl(STDIN_FILENO, F_SETFL, old_in | O_NONBLOCK) < 0 ||
      fcntl(STDOUT_FILENO, F_SETFL, old_out | O_NONBLOCK) < 0) {
    rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                       "Cannot configure Chrome native messaging pipes");
    goto done;
  }
  /* This is a dedicated host process; stdout contains framed JSON only. */
  (void)signal(SIGPIPE, SIG_IGN);
  if (logger != NULL)
    logger->infof(logger, "CAI Chrome native host ready", "socket=%s", path);
  rc = CAI_OK;
  for (;;) {
    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLIN;
    fds[1].fd = STDOUT_FILENO;
    fds[1].events = h.extension_queue.length != 0U ? POLLOUT : 0;
    fds[2].fd = h.client;
    fds[2].events = POLLIN | (h.client_queue.length != 0U ? POLLOUT : 0);
    fds[3].fd = listener;
    fds[3].events = POLLIN;
    if (poll(fds, 4U, 100) < 0) {
      if (errno == EINTR)
        continue;
      rc = cai_set_error(error, CAI_ERR_TRANSPORT, "Chrome host poll failed");
      break;
    }
    if (fds[0].revents & POLLIN) {
      rc = read_frame(&h.extension_frame, STDIN_FILENO,
                      CAI_CHROME_RESPONSE_LIMIT, &ready, error);
      if (rc != CAI_OK)
        break;
      if (ready) {
        rc = dispatch_extension(&h, h.extension_frame.data, error);
        frame_clear(&h.extension_frame);
        if (rc != CAI_OK)
          break;
      }
    } else if (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
      /* Chrome closing its pipe is normal host termination. */
      rc = h.extension_frame.header_used != 0U
               ? cai_set_error(error, CAI_ERR_PROTOCOL,
                               "Chrome closed a partial native message")
               : CAI_OK;
      break;
    }
    if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
      rc = CAI_OK;
      break;
    }
    if (fds[1].revents & POLLOUT) {
      rc = flush_queue(&h.extension_queue, STDOUT_FILENO, 0, error);
      if (rc != CAI_OK)
        break;
    }
    if (h.client >= 0 && (fds[2].revents & POLLIN)) {
      rc = read_frame(&h.client_frame, h.client, CAI_CHROME_REQUEST_LIMIT,
                      &ready, error);
      if (rc == CAI_OK && ready) {
        rc = dispatch_client(&h, h.client_frame.data, error);
        frame_clear(&h.client_frame);
      }
      if (rc != CAI_OK) {
        if (logger != NULL)
          logger->warnf(logger,
                        "Chrome client disconnected or sent an invalid frame",
                        "error=%s", error->message);
        drop_client(&h);
        cai_error_cleanup(error);
        cai_error_init(error);
        rc = CAI_OK;
      }
    }
    if (h.client >= 0 && (fds[2].revents & POLLOUT)) {
      rc = flush_queue(&h.client_queue, h.client, 1, error);
      if (rc != CAI_OK) {
        drop_client(&h);
        cai_error_cleanup(error);
        cai_error_init(error);
        rc = CAI_OK;
      }
    }
    if (h.client >= 0 && (fds[2].revents & (POLLHUP | POLLERR | POLLNVAL)))
      drop_client(&h);
    if (fds[3].revents & POLLIN) {
      accept_fd = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (accept_fd >= 0) {
        struct ucred peer;
        socklen_t size = sizeof(peer);
        if (h.client >= 0 || h.abandoned ||
            getsockopt(accept_fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 ||
            peer.uid != geteuid())
          close(accept_fd);
        else
          h.client = accept_fd;
      }
    }
    rc = host_cleanup(&h, error);
    if (rc != CAI_OK)
      break;
    now = cai_chrome_now_ms();
    if ((h.client_frame.deadline && now > h.client_frame.deadline) ||
        (h.client_queue.deadline && now > h.client_queue.deadline))
      drop_client(&h);
    if ((h.extension_frame.deadline && now > h.extension_frame.deadline) ||
        (h.extension_queue.deadline && now > h.extension_queue.deadline) ||
        (h.pending_id != NULL && now > h.pending_deadline)) {
      rc = cai_set_error(
          error, CAI_ERR_TRANSPORT,
          "Chrome native messaging or browser operation timed out");
      break;
    }
  }
done:
  if (old_in >= 0)
    (void)fcntl(STDIN_FILENO, F_SETFL, old_in);
  if (old_out >= 0)
    (void)fcntl(STDOUT_FILENO, F_SETFL, old_out);
  drop_client(&h);
  frame_clear(&h.extension_frame);
  queue_clear(&h.extension_queue);
  clear_identity(&h);
  free(h.pending_id);
  close(listener);
  (void)unlink(path);
  return rc;
}
