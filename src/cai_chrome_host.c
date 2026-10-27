#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include "cai_chrome_host.h"
#include "cai_chrome_protocol.h"
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
typedef struct host_tab {
  struct host_tab *next;
  int64_t id;
} host_tab;
typedef struct chrome_host {
  int client;
  int control;
  host_frame control_frame;
  host_queue control_queue;
  char *control_id;
  int stopping;
  long long stop_deadline;
  host_frame extension_frame, client_frame;
  host_queue extension_queue, client_queue;
  char *session, *turn, *pending_id;
  const char *pending_method;
  int pending_has_tab;
  int64_t pending_tab;
  host_tab *tabs;
  char *end_request, *end_id;
  int finalizing;
  long long cleanup_deadline;
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
  int has_tab;
  int64_t tab;
} host_identity;
static const lonejson_field identity_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(host_identity, session, "session_id"),
    LONEJSON_FIELD_STRING_ALLOC(host_identity, turn, "turn_id"),
    LONEJSON_FIELD_STRING_ALLOC(host_identity, context, "session_context"),
    LONEJSON_FIELD_I64_PRESENT(host_identity, tab, has_tab, "tabId")};
LONEJSON_MAP_DEFINE(identity_map, host_identity, identity_fields);
static const char *const browser_methods[] = {
    "getInfo",     "getUserTabs", "getTabs",    "createTab", "claimUserTab",
    "attach",      "detach",      "executeCdp", "markTab",   "finalizeTabs",
    "nameSession", "turnEnded",   NULL};
static const char *allowed_method(const char *method) {
  size_t i;
  for (i = 0U; browser_methods[i] != NULL; i++)
    if (strcmp(method, browser_methods[i]) == 0)
      return browser_methods[i];
  return NULL;
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
static void remove_tab(chrome_host *h, int64_t id) {
  host_tab **link = &h->tabs, *tab;
  while (*link != NULL) {
    tab = *link;
    if (tab->id == id) {
      *link = tab->next;
      free(tab);
      return;
    }
    link = &tab->next;
  }
}
static int remember_tab(chrome_host *h, int64_t id, cai_error *error) {
  host_tab *tab;
  for (tab = h->tabs; tab != NULL; tab = tab->next)
    if (tab->id == id)
      return CAI_OK;
  tab = calloc(1U, sizeof(*tab));
  if (tab == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "Cannot track attached Chrome tab");
  tab->id = id;
  tab->next = h->tabs;
  h->tabs = tab;
  return CAI_OK;
}
static void clear_identity(chrome_host *h) {
  free(h->session);
  free(h->turn);
  h->session = NULL;
  h->turn = NULL;
  while (h->tabs != NULL)
    remove_tab(h, h->tabs->id);
  free(h->end_request);
  free(h->end_id);
  h->end_request = h->end_id = NULL;
  h->finalizing = 0;
  h->cleanup_deadline = 0LL;
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
static void control_close(chrome_host *h) {
  if (h->control >= 0)
    close(h->control);
  h->control = -1;
  frame_clear(&h->control_frame);
  queue_clear(&h->control_queue);
}
static int control_reply(chrome_host *h, const char *result,
                         const cai_error *failure, cai_error *error) {
  cai_chrome_rpc reply;
  char message[1536];
  int rc;
  memset(&reply, 0, sizeof(reply));
  reply.jsonrpc = "2.0";
  rc = cai_chrome_value_buffer(&reply.id, h->control_id, error);
  if (rc == CAI_OK && failure == NULL)
    rc = cai_chrome_value_buffer(&reply.result, result, error);
  if (failure != NULL) {
    snprintf(message, sizeof(message), "%s%s%s",
             failure->message != NULL ? failure->message
                                      : "Chrome cleanup failed",
             failure->detail != NULL ? ": " : "",
             failure->detail != NULL ? failure->detail : "");
    reply.error.has_code = 1;
    reply.error.code = -32000;
    reply.error.message = message;
  }
  if (rc == CAI_OK)
    rc = response_queue(&h->control_queue, &reply, failure == NULL, error);
  CAI_LJ_CHROME->json_value_cleanup(CAI_LJ_CHROME, &reply.id);
  CAI_LJ_CHROME->json_value_cleanup(CAI_LJ_CHROME, &reply.result);
  return rc;
}
static int control_read(chrome_host *h, const char *executable,
                        cai_error *error) {
  cai_chrome_rpc rpc;
  cai_chrome_stop_request request;
  lonejson_error je;
  char *params = NULL;
  int ready, rc;
  memset(&request, 0, sizeof(request));
  rc = read_frame(&h->control_frame, h->control, 32768U, &ready, error);
  if (rc != CAI_OK || !ready)
    return rc;
  rc = cai_chrome_parse(h->control_frame.data, NULL, &rpc, error);
  if (rc != CAI_OK)
    goto done;
  if (rpc.method == NULL || strcmp(rpc.method, "shutdown") != 0 ||
      !rpc.has_id || !rpc.has_params) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "Chrome control endpoint accepts shutdown only");
    goto done;
  }
  params = cai_chrome_value(&rpc.params, error);
  lonejson_error_init(&je);
  if (params == NULL ||
      CAI_LJ_CHROME->parse_cstr(CAI_LJ_CHROME, &cai_chrome_stop_map, &request,
                                params, &je) != LONEJSON_STATUS_OK ||
      request.executable == NULL) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "Chrome shutdown requires an executable identity");
    goto done;
  }
  free(h->control_id);
  h->control_id = cai_chrome_value(&rpc.id, error);
  if (h->control_id == NULL) {
    rc = CAI_ERR_NOMEM;
    goto done;
  }
  if (strcmp(request.executable, executable) != 0)
    rc = control_reply(h, "\"different-executable\"", NULL, error);
  else {
    h->stopping = 1;
    h->stop_deadline = cai_chrome_now_ms() + CAI_CHROME_CLEANUP_TIMEOUT_MS;
    if (h->logger != NULL)
      h->logger->infof(h->logger, "Chrome host shutdown requested",
                       "session_id=%s", h->session != NULL ? h->session : "");
    drop_client(h);
  }
done:
  CAI_LJ_CHROME->cleanup(CAI_LJ_CHROME, &cai_chrome_stop_map, &request);
  CAI_LJ_CHROME->cleanup(CAI_LJ_CHROME, &cai_chrome_rpc_map, &rpc);
  free(params);
  frame_clear(&h->control_frame);
  return rc;
}

static int host_listener(const char *path, cai_error *error) {
  struct sockaddr_un address;
  mode_t mask;
  int fd, rc, saved;
  fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) {
    cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                         "Cannot create Chrome host listener", strerror(errno));
    return -1;
  }
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  strcpy(address.sun_path, path);
  mask = umask(0077);
  rc = bind(fd, (struct sockaddr *)&address, sizeof(address));
  umask(mask);
  if (rc != 0 || listen(fd, 8) != 0) {
    saved = errno;
    if (rc == 0)
      (void)unlink(path);
    close(fd);
    cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                         "Cannot bind Chrome host socket", strerror(saved));
    return -1;
  }
  return fd;
}
static int host_cleanup(chrome_host *h, cai_error *error) {
  cai_chrome_rpc rpc;
  host_identity identity;
  char *params;
  int rc;
  if ((!h->abandoned && !h->finalizing) || h->pending_id != NULL ||
      h->session == NULL)
    return CAI_OK;
  if (!h->finalizing) {
    h->finalizing = 1;
    h->cleanup_deadline = cai_chrome_now_ms() + CAI_CHROME_CLEANUP_TIMEOUT_MS;
  }
  /* Explicit turn completion and orphan cleanup share one detach sequence.
   * Preserve the caller's original request/ID until all tabs are released. */
  if (h->tabs == NULL && h->end_request != NULL) {
    rc = enqueue(&h->extension_queue, h->end_request, error);
    if (rc == CAI_OK) {
      free(h->end_request);
      h->end_request = NULL;
      h->pending_id = h->end_id;
      h->end_id = NULL;
      h->pending_method = "turnEnded";
      h->pending_deadline = h->cleanup_deadline;
      h->pending_end = 1;
    }
    return rc;
  }
  memset(&rpc, 0, sizeof(rpc));
  memset(&identity, 0, sizeof(identity));
  identity.session = h->session;
  identity.turn = h->turn;
  identity.context = "cached";
  if (h->tabs != NULL) {
    identity.has_tab = 1;
    identity.tab = h->tabs->id;
  }
  params = cai_chrome_serialize(&identity_map, &identity, error);
  if (params == NULL)
    return CAI_ERR_NOMEM;
  rpc.jsonrpc = "2.0";
  rpc.method = h->tabs != NULL ? "detach" : "turnEnded";
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
    h->pending_deadline = h->cleanup_deadline;
    h->pending_method = rpc.method;
    h->pending_has_tab = identity.has_tab;
    h->pending_tab = identity.tab;
    h->pending_end = h->tabs == NULL;
    h->cleanup_pending = 1;
  }
  return rc;
}
static int dispatch_client(chrome_host *h, const char *json, cai_error *error) {
  cai_chrome_rpc rpc;
  host_identity identity;
  char *params = NULL, *id = NULL;
  const char *method;
  lonejson_error je;
  int rc;
  memset(&identity, 0, sizeof(identity));
  lonejson_error_init(&je);
  rc = cai_chrome_parse(json, NULL, &rpc, error);
  if (rc != CAI_OK)
    goto done;
  if (rpc.method == NULL || !rpc.has_id || h->pending_id != NULL ||
      h->finalizing) {
    rc = cai_set_error(
        error, CAI_ERR_PROTOCOL,
        "Chrome client must send one identified request at a time");
    goto done;
  }
  method = allowed_method(rpc.method);
  if (method == NULL) {
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
  if ((strcmp(method, "attach") == 0 || strcmp(method, "detach") == 0) &&
      (!identity.has_tab || identity.tab < 0LL ||
       identity.tab > 2147483647LL)) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "Chrome attach/detach requires a valid tabId");
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
  if (strcmp(method, "turnEnded") == 0) {
    h->end_request = cai_strdup(NULL, json);
    if (h->end_request == NULL) {
      rc = cai_set_error(error, CAI_ERR_NOMEM,
                         "Cannot retain Chrome turn cleanup request");
      goto done;
    }
    h->end_id = id;
    id = NULL;
    h->finalizing = 1;
    h->cleanup_deadline = cai_chrome_now_ms() + CAI_CHROME_CLEANUP_TIMEOUT_MS;
    goto done;
  }
  rc = enqueue(&h->extension_queue, json, error);
  if (rc == CAI_OK) {
    h->pending_id = id;
    h->pending_method = method;
    h->pending_has_tab = identity.has_tab;
    h->pending_tab = identity.tab;
    id = NULL;
    h->pending_end = 0;
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
  int rc = cai_chrome_parse(json, h->pending_method, &rpc, error);
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
      if (h->pending_has_tab && strcmp(h->pending_method, "attach") == 0 &&
          !rpc.error.has_code)
        rc = remember_tab(h, h->pending_tab, error);
      if (rc == CAI_OK && h->client >= 0 && !h->cleanup_pending)
        rc = enqueue(&h->client_queue, json, error);
      if (h->pending_has_tab && strcmp(h->pending_method, "detach") == 0 &&
          (!rpc.error.has_code || h->cleanup_pending)) {
        /* A user may already have cancelled debugging. Do not let one stale
         * tab prevent the remaining detaches or the final turnEnded call. */
        remove_tab(h, h->pending_tab);
        if (rpc.error.has_code && h->logger != NULL)
          h->logger->warnf(h->logger, "Chrome tab detach failed during cleanup",
                           "tab_id=%lld error=%s", (long long)h->pending_tab,
                           rpc.error.message);
      }
      if (h->pending_end) {
        if (rpc.error.has_code) {
          rc = cai_set_error_detail(error, CAI_ERR_SERVER,
                                    "Chrome could not release browser control",
                                    rpc.error.message);
        } else
          clear_identity(h);
      }
      free(h->pending_id);
      h->pending_id = NULL;
      h->pending_method = NULL;
      h->pending_has_tab = 0;
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
int cai_chrome_native_host_run(const char *origin, pslog_logger *logger,
                               cai_error *error) {
  return cai_chrome_native_host_run_at(origin, NULL, logger, error);
}
int cai_chrome_native_host_run_at(const char *origin,
                                  const char *selected_directory,
                                  pslog_logger *logger, cai_error *error) {
  chrome_host h;
  char directory[4096], path[4096], control_path[4096], executable[4096];
  struct sockaddr_un address;
  struct pollfd fds[6];
  int listener = -1, control_listener = -1, rc, ready, accept_fd;
  int old_in = -1, old_out = -1;
  ssize_t executable_length;
  int pipe_changed = 0;
  struct sigaction old_pipe, ignore_pipe;
  long long now;
  memset(&h, 0, sizeof(h));
  h.client = -1;
  h.control = -1;
  h.logger = logger;
  if (origin == NULL || strcmp(origin, EXTENSION_ORIGIN) != 0)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "CAI native host accepts only the official ChatGPT "
                         "Chrome extension origin");
  if (selected_directory != NULL) {
    if (strlen(selected_directory) >= sizeof(directory))
      return cai_set_error(error, CAI_ERR_INVALID,
                           "Chrome socket directory is too long");
    strcpy(directory, selected_directory);
    rc = cai_chrome_private_directory(directory, error);
  } else
    rc = cai_cli_chrome_directory(directory, sizeof(directory), 1, error);
  if (rc != CAI_OK)
    return rc;
  executable_length =
      readlink("/proc/self/exe", executable, sizeof(executable) - 1U);
  if (executable_length <= 0 ||
      (size_t)executable_length >= sizeof(executable) - 1U)
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "Cannot identify Chrome host executable");
  executable[executable_length] = '\0';
  if (snprintf(path, sizeof(path), "%s/%ld.sock", directory, (long)getpid()) >=
          (int)sizeof(address.sun_path) ||
      snprintf(control_path, sizeof(control_path), "%s/%ld.ctl", directory,
               (long)getpid()) >= (int)sizeof(address.sun_path))
    return cai_set_error(
        error, CAI_ERR_INVALID,
        "Chrome socket path exceeds the Unix socket path limit");
  control_listener = host_listener(control_path, error);
  if (control_listener < 0)
    return error != NULL ? error->code : CAI_ERR_TRANSPORT;
  listener = host_listener(path, error);
  if (listener < 0) {
    rc = error != NULL ? error->code : CAI_ERR_TRANSPORT;
    close(control_listener);
    (void)unlink(control_path);
    return rc;
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
  /* stdout contains framed JSON only; restore host signal ownership on return.
   */
  memset(&ignore_pipe, 0, sizeof(ignore_pipe));
  ignore_pipe.sa_handler = SIG_IGN;
  sigemptyset(&ignore_pipe.sa_mask);
  if (sigaction(SIGPIPE, &ignore_pipe, &old_pipe) != 0) {
    rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                       "Cannot configure Chrome native messaging signals");
    goto done;
  }
  pipe_changed = 1;
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
    fds[3].fd = h.stopping ? -1 : listener;
    fds[3].events = POLLIN;
    fds[4].fd = h.stopping ? -1 : control_listener;
    fds[4].events = POLLIN;
    fds[5].fd = h.control;
    fds[5].events = h.stopping ? 0 : POLLIN;
    if (h.control_queue.length != 0U)
      fds[5].events |= POLLOUT;
    if (poll(fds, 6U, 100) < 0) {
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
        if (h.client >= 0 || h.abandoned || h.finalizing ||
            getsockopt(accept_fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 ||
            peer.uid != geteuid())
          close(accept_fd);
        else
          h.client = accept_fd;
      }
    }
    if (fds[4].revents & POLLIN) {
      accept_fd =
          accept4(control_listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (accept_fd >= 0) {
        struct ucred peer;
        socklen_t size = sizeof(peer);
        if (h.control >= 0 ||
            getsockopt(accept_fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 ||
            peer.uid != geteuid())
          close(accept_fd);
        else {
          h.control = accept_fd;
          h.control_frame.deadline = cai_chrome_now_ms() + 1000LL;
        }
      }
    }
    if (h.control >= 0 && (fds[5].revents & POLLIN)) {
      rc = control_read(&h, executable, error);
      if (rc != CAI_OK) {
        control_close(&h);
        cai_error_cleanup(error);
        cai_error_init(error);
        rc = CAI_OK;
      }
    }
    if (h.control >= 0 && (fds[5].revents & POLLOUT)) {
      rc = flush_queue(&h.control_queue, h.control, 1, error);
      if (rc != CAI_OK || h.control_queue.length == 0U) {
        control_close(&h);
        cai_error_cleanup(error);
        cai_error_init(error);
        rc = CAI_OK;
      }
    }
    if (h.control >= 0 && (fds[5].revents & (POLLHUP | POLLERR | POLLNVAL)))
      control_close(&h);
    rc = host_cleanup(&h, error);
    if (rc != CAI_OK)
      break;
    now = cai_chrome_now_ms();
    if (h.stopping && h.session == NULL && h.extension_queue.length == 0U)
      break;
    if (h.stopping && now >= h.stop_deadline) {
      rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                         "Chrome host cleanup timed out during shutdown");
      break;
    }
    if (!h.stopping && h.control >= 0 &&
        ((h.control_frame.deadline && now >= h.control_frame.deadline) ||
         (h.control_queue.deadline && now >= h.control_queue.deadline)))
      control_close(&h);
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
  if (h.stopping && h.session != NULL && rc == CAI_OK)
    rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                       "Chrome disconnected before shutdown cleanup completed");
  close(control_listener);
  close(listener);
  (void)unlink(control_path);
  (void)unlink(path);
  if (h.stopping && h.control >= 0) {
    cai_error reply_error;
    long long deadline = cai_chrome_now_ms() + 250LL;
    int reply_rc;
    cai_error_init(&reply_error);
    reply_rc = control_reply(&h, "\"stopped\"", rc == CAI_OK ? NULL : error,
                             &reply_error);
    while (reply_rc == CAI_OK && h.control_queue.length != 0U &&
           cai_chrome_now_ms() < deadline) {
      struct pollfd output;
      output.fd = h.control;
      output.events = POLLOUT;
      output.revents = 0;
      if (poll(&output, 1U, 25) > 0)
        reply_rc = flush_queue(&h.control_queue, h.control, 1, &reply_error);
    }
    cai_error_cleanup(&reply_error);
  }
  control_close(&h);
  free(h.control_id);
  if (pipe_changed && sigaction(SIGPIPE, &old_pipe, NULL) != 0)
    rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                       "Cannot restore Chrome native messaging signals");
  if (old_in >= 0)
    (void)fcntl(STDIN_FILENO, F_SETFL, old_in);
  if (old_out >= 0)
    (void)fcntl(STDOUT_FILENO, F_SETFL, old_out);
  drop_client(&h);
  frame_clear(&h.extension_frame);
  queue_clear(&h.extension_queue);
  clear_identity(&h);
  free(h.pending_id);
  return rc;
}
