#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include "cai_chrome_protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static const lonejson_field rpc_error_fields[] = {
    LONEJSON_FIELD_I64_PRESENT(cai_chrome_rpc_error, code, has_code, "code"),
    LONEJSON_FIELD_STRING_ALLOC(cai_chrome_rpc_error, message, "message")};
LONEJSON_MAP_DEFINE(rpc_error_map, cai_chrome_rpc_error, rpc_error_fields);
static const lonejson_field rpc_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_chrome_rpc, jsonrpc, "jsonrpc"),
    CAI_CHROME_JSON_FIELD(cai_chrome_rpc, id, "id"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_chrome_rpc, method, "method"),
    CAI_CHROME_JSON_FIELD(cai_chrome_rpc, params, "params"),
    CAI_CHROME_JSON_FIELD(cai_chrome_rpc, result, "result"),
    LONEJSON_FIELD_OBJECT(cai_chrome_rpc, error, "error", &rpc_error_map)};
const lonejson_map cai_chrome_rpc_map = {
    "cai_chrome_rpc",
    sizeof(cai_chrome_rpc),
    rpc_fields,
    sizeof(rpc_fields) / sizeof(rpc_fields[0]),
    &cai_chrome_rpc_map,
    LONEJSON__MAP_STATIC_COOKIE(__LINE__, sizeof(cai_chrome_rpc),
                                sizeof(rpc_fields) / sizeof(rpc_fields[0]))};

typedef struct rpc_presence {
  cai_chrome_rpc *rpc;
  char key[20];
  size_t length;
  unsigned seen;
} rpc_presence;
static lonejson_status presence_begin(void *context,
                                      const lonejson_value_path *path,
                                      lonejson_error *je) {
  rpc_presence *p = context;
  (void)je;
  if (path->segment_count == 0U) {
    p->length = 0U;
    p->key[0] = '\0';
  }
  return LONEJSON_STATUS_OK;
}
static lonejson_status presence_chunk(void *context,
                                      const lonejson_value_path *path,
                                      const char *bytes, size_t length,
                                      lonejson_error *je) {
  rpc_presence *p = context;
  (void)je;
  if (path->segment_count == 0U) {
    if (p->length + length < sizeof(p->key)) {
      memcpy(p->key + p->length, bytes, length);
      p->length += length;
      p->key[p->length] = '\0';
    } else {
      p->length = sizeof(p->key);
      p->key[0] = '\0';
    }
  }
  return LONEJSON_STATUS_OK;
}
static lonejson_status presence_end(void *context,
                                    const lonejson_value_path *path,
                                    lonejson_error *je) {
  rpc_presence *p = context;
  unsigned bit = 0U;
  if (path->segment_count != 0U)
    return LONEJSON_STATUS_OK;
  if (strcmp(p->key, "id") == 0) {
    p->rpc->has_id = 1;
    bit = 1U;
  } else if (strcmp(p->key, "params") == 0) {
    p->rpc->has_params = 1;
    bit = 2U;
  } else if (strcmp(p->key, "result") == 0) {
    p->rpc->has_result = 1;
    bit = 4U;
  } else if (strcmp(p->key, "error") == 0) {
    p->rpc->has_error = 1;
    bit = 8U;
  } else if (strcmp(p->key, "method") == 0)
    bit = 16U;
  else if (strcmp(p->key, "jsonrpc") == 0)
    bit = 32U;
  if (bit != 0U && (p->seen & bit)) {
    je->code = LONEJSON_STATUS_CALLBACK_FAILED;
    snprintf(je->message, sizeof(je->message), "duplicate JSON-RPC member %s",
             p->key);
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  p->seen |= bit;
  return LONEJSON_STATUS_OK;
}
int cai_chrome_parse(const char *json, cai_chrome_rpc *rpc, cai_error *error) {
  lonejson_error je;
  rpc_presence presence;
  lonejson_path_value_visitor visitor;
  memset(rpc, 0, sizeof(*rpc));
  lonejson_error_init(&je);
  CAI_LJ_CHROME->json_value_init(CAI_LJ_CHROME, &rpc->id);
  CAI_LJ_CHROME->json_value_init(CAI_LJ_CHROME, &rpc->params);
  CAI_LJ_CHROME->json_value_init(CAI_LJ_CHROME, &rpc->result);
  if (lonejson_json_value_enable_parse_capture(&rpc->id, &je) !=
          LONEJSON_STATUS_OK ||
      lonejson_json_value_enable_parse_capture(&rpc->params, &je) !=
          LONEJSON_STATUS_OK ||
      lonejson_json_value_enable_parse_capture(&rpc->result, &je) !=
          LONEJSON_STATUS_OK)
    return cai_set_error_detail(error, CAI_ERR_NOMEM,
                                "Cannot initialize Chrome JSON capture",
                                je.message);
  if (CAI_LJ_CHROME->parse_cstr(CAI_LJ_CHROME, &cai_chrome_rpc_map, rpc, json,
                                &je) != LONEJSON_STATUS_OK)
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "Invalid Chrome JSON-RPC frame", je.message);
  memset(&presence, 0, sizeof(presence));
  presence.rpc = rpc;
  visitor = lonejson_default_path_value_visitor();
  visitor.object_key_begin = presence_begin;
  visitor.object_key_chunk = presence_chunk;
  visitor.object_key_end = presence_end;
  if (lonejson_visit_path_value_cstr(CAI_LJ_CHROME, json, &visitor, &presence,
                                     &je) != LONEJSON_STATUS_OK)
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "Invalid Chrome JSON-RPC members", je.message);
  if (rpc->has_error && rpc->has_result)
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "Chrome reply contains both result and error");
  if (rpc->jsonrpc == NULL || strcmp(rpc->jsonrpc, "2.0") != 0)
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "Chrome JSON-RPC version must be 2.0");
  if (rpc->method != NULL) {
    if (rpc->method[0] == '\0' || rpc->has_result || rpc->has_error)
      return cai_set_error(
          error, CAI_ERR_PROTOCOL,
          "Chrome request or notification has invalid reply members");
  } else if (!rpc->has_id || (!rpc->has_result && !rpc->has_error) ||
             (rpc->has_error &&
              (!rpc->error.has_code || rpc->error.message == NULL)))
    return cai_set_error(
        error, CAI_ERR_PROTOCOL,
        "Chrome reply must contain an ID and a result or a complete error");
  return CAI_OK;
}
char *cai_chrome_serialize(const lonejson_map *map, const void *doc,
                           cai_error *error) {
  lonejson_error je;
  char *json;
  lonejson_error_init(&je);
  json = CAI_LJ_CHROME->serialize_alloc(CAI_LJ_CHROME, map, doc, NULL, &je);
  if (json == NULL)
    cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                         "Cannot encode Chrome message", je.message);
  return json;
}
static lonejson_status value_sink(void *user, const void *data, size_t length,
                                  lonejson_error *je) {
  cai_error error;
  int rc;
  (void)je;
  cai_error_init(&error);
  rc = cai_buffer_append((cai_buffer_builder *)user, data, length, &error);
  cai_error_cleanup(&error);
  return rc == CAI_OK ? LONEJSON_STATUS_OK : LONEJSON_STATUS_ALLOCATION_FAILED;
}
char *cai_chrome_value(const lonejson_json_value *value, cai_error *error) {
  cai_buffer_builder b;
  lonejson_error je;
  memset(&b, 0, sizeof(b));
  lonejson_error_init(&je);
  if (lonejson_json_value_write_to_sink(value, value_sink, &b, &je) !=
      LONEJSON_STATUS_OK) {
    cai_free_mem(NULL, b.data);
    cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                         "Cannot read Chrome JSON value", je.message);
    return NULL;
  }
  if (cai_buffer_append(&b, "", 1U, error) != CAI_OK) {
    cai_free_mem(NULL, b.data);
    return NULL;
  }
  return b.data;
}
int cai_chrome_value_buffer(lonejson_json_value *value, const char *json,
                            cai_error *error) {
  lonejson_error je;
  lonejson_error_init(&je);
  CAI_LJ_CHROME->json_value_init(CAI_LJ_CHROME, value);
  if (lonejson_json_value_set_buffer(value, json, strlen(json), &je) !=
      LONEJSON_STATUS_OK)
    return cai_set_error_detail(error, CAI_ERR_INVALID,
                                "Invalid Chrome JSON value", je.message);
  return CAI_OK;
}
long long cai_chrome_now_ms(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t) != 0)
    return 0;
  return (long long)t.tv_sec * 1000LL + (long long)t.tv_nsec / 1000000LL;
}
int cai_chrome_private_directory(const char *path, cai_error *error) {
  struct stat st;
  if (path == NULL || path[0] != '/' || lstat(path, &st) != 0 ||
      !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077U))
    return cai_set_error_detail(error, CAI_ERR_INVALID,
                                "Chrome socket directory must be an absolute, "
                                "private directory owned by you",
                                path);
  return CAI_OK;
}
int cai_chrome_connect_socket(const char *path, int *out, cai_error *error) {
  struct sockaddr_un address;
  struct stat st;
  int fd;
  *out = -1;
  if (path == NULL || path[0] != '/' ||
      strlen(path) >= sizeof(address.sun_path))
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Chrome socket path is invalid or too long");
  if (lstat(path, &st) != 0 || !S_ISSOCK(st.st_mode) ||
      st.st_uid != geteuid() || (st.st_mode & 077U))
    return cai_set_error_detail(
        error, CAI_ERR_INVALID,
        "Chrome endpoint must be a private socket owned by you", path);
  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "Cannot create Chrome socket", strerror(errno));
#if defined(SO_NOSIGPIPE)
  {
    int on = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
  }
#endif
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  memcpy(address.sun_path, path, strlen(path) + 1U);
  if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0 ||
      fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
    close(fd);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "Cannot configure Chrome socket");
  }
  if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
    int saved = errno;
    int status = -1;
    struct pollfd p;
    socklen_t size = sizeof(saved);
    long long deadline = cai_chrome_now_ms() + 1000LL;
    p.fd = fd;
    p.events = POLLOUT;
    p.revents = 0;
    if (saved == EINPROGRESS) {
      while (cai_chrome_now_ms() < deadline) {
        status = poll(&p, 1U, 50);
        if (status < 0 && errno == EINTR)
          continue;
        if (status != 0)
          break;
      }
      if (status > 0 &&
          getsockopt(fd, SOL_SOCKET, SO_ERROR, &saved, &size) == 0 &&
          saved == 0)
        status = 0;
      else {
        if (status == 0)
          saved = ETIMEDOUT;
        status = -1;
      }
    }
    if (status != 0) {
      close(fd);
      return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                  "Cannot connect to Chrome host",
                                  strerror(saved));
    }
  }
#if defined(__linux__)
  {
    struct ucred peer;
    socklen_t length = sizeof(peer);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) != 0 ||
        peer.uid != geteuid()) {
      close(fd);
      return cai_set_error(error, CAI_ERR_INVALID,
                           "Chrome host belongs to another user");
    }
  }
#endif
  if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0 ||
      fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
    close(fd);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "Cannot configure Chrome socket");
  }
  *out = fd;
  return CAI_OK;
}

static const lonejson_field request_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_chrome_rpc, jsonrpc, "jsonrpc"),
    LONEJSON_FIELD_JSON_VALUE(cai_chrome_rpc, id, "id"),
    LONEJSON_FIELD_STRING_ALLOC(cai_chrome_rpc, method, "method"),
    LONEJSON_FIELD_JSON_VALUE(cai_chrome_rpc, params, "params")};
const lonejson_map cai_chrome_request_map = {
    "cai_chrome_rpc",
    sizeof(cai_chrome_rpc),
    request_fields,
    sizeof(request_fields) / sizeof(request_fields[0]),
    &cai_chrome_request_map,
    LONEJSON__MAP_STATIC_COOKIE(__LINE__, sizeof(cai_chrome_rpc),
                                sizeof(request_fields) /
                                    sizeof(request_fields[0]))};
static const lonejson_field success_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_chrome_rpc, jsonrpc, "jsonrpc"),
    LONEJSON_FIELD_JSON_VALUE(cai_chrome_rpc, id, "id"),
    LONEJSON_FIELD_JSON_VALUE(cai_chrome_rpc, result, "result")};
const lonejson_map cai_chrome_success_map = {
    "cai_chrome_rpc",
    sizeof(cai_chrome_rpc),
    success_fields,
    sizeof(success_fields) / sizeof(success_fields[0]),
    &cai_chrome_success_map,
    LONEJSON__MAP_STATIC_COOKIE(__LINE__, sizeof(cai_chrome_rpc),
                                sizeof(success_fields) /
                                    sizeof(success_fields[0]))};
static const lonejson_field failure_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_chrome_rpc, jsonrpc, "jsonrpc"),
    LONEJSON_FIELD_JSON_VALUE(cai_chrome_rpc, id, "id"),
    LONEJSON_FIELD_OBJECT(cai_chrome_rpc, error, "error", &rpc_error_map)};
const lonejson_map cai_chrome_failure_map = {
    "cai_chrome_rpc",
    sizeof(cai_chrome_rpc),
    failure_fields,
    sizeof(failure_fields) / sizeof(failure_fields[0]),
    &cai_chrome_failure_map,
    LONEJSON__MAP_STATIC_COOKIE(__LINE__, sizeof(cai_chrome_rpc),
                                sizeof(failure_fields) /
                                    sizeof(failure_fields[0]))};
