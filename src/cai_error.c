#include "cai_internal.h"

#include <string.h>

void cai_error_init(cai_error *error) {
  if (error == NULL) {
    return;
  }
  error->code = CAI_OK;
  error->http_status = 0L;
  error->message = NULL;
  error->detail = NULL;
  error->server_code = NULL;
  error->request_id = NULL;
}

void cai_error_cleanup(cai_error *error) {
  if (error == NULL) {
    return;
  }
  cai_free_mem(NULL, error->message);
  cai_free_mem(NULL, error->detail);
  cai_free_mem(NULL, error->server_code);
  cai_free_mem(NULL, error->request_id);
  cai_error_init(error);
}

const char *cai_status_string(int status) {
  switch (status) {
  case CAI_OK:
    return "ok";
  case CAI_ERR_INVALID:
    return "invalid";
  case CAI_ERR_NOMEM:
    return "out of memory";
  case CAI_ERR_TRANSPORT:
    return "transport error";
  case CAI_ERR_PROTOCOL:
    return "protocol error";
  case CAI_ERR_SERVER:
    return "server error";
  case CAI_ERR_CANCELLED:
    return "cancelled";
  case CAI_ERR_LIMIT:
    return "limit exceeded";
  default:
    return "unknown error";
  }
}

int cai_set_error_detail(cai_error *error, int code, const char *message,
                         const char *detail) {
  return cai_set_error_http(error, code, 0L, message, detail, NULL, NULL);
}

int cai_set_error(cai_error *error, int code, const char *message) {
  return cai_set_error_detail(error, code, message, NULL);
}

int cai_set_error_http(cai_error *error, int code, long http_status,
                       const char *message, const char *detail,
                       const char *server_code, const char *request_id) {
  cai_error_cleanup(error);
  if (error == NULL) {
    return code;
  }
  error->code = code;
  error->http_status = http_status;
  if (message != NULL) {
    error->message = cai_strdup(NULL, message);
    if (error->message == NULL) {
      error->code = CAI_ERR_NOMEM;
      return CAI_ERR_NOMEM;
    }
  }
  if (detail != NULL) {
    error->detail = cai_strdup(NULL, detail);
    if (error->detail == NULL) {
      cai_error_cleanup(error);
      error->code = CAI_ERR_NOMEM;
      return CAI_ERR_NOMEM;
    }
  }
  if (server_code != NULL) {
    error->server_code = cai_strdup(NULL, server_code);
    if (error->server_code == NULL) {
      cai_error_cleanup(error);
      error->code = CAI_ERR_NOMEM;
      return CAI_ERR_NOMEM;
    }
  }
  if (request_id != NULL) {
    error->request_id = cai_strdup(NULL, request_id);
    if (error->request_id == NULL) {
      cai_error_cleanup(error);
      error->code = CAI_ERR_NOMEM;
      return CAI_ERR_NOMEM;
    }
  }
  return code;
}
