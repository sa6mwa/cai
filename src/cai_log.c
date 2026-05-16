#include "cai_internal.h"

#include <pslog.h>

static pslog_logger *cai_logger(const cai_client_impl *client) {
  if (client == NULL || client->logger_disabled) {
    return NULL;
  }
  return client->logger;
}

void cai_log_client_opened(const cai_client_impl *client) {
  pslog_logger *log;

  log = cai_logger(client);
  if (log == NULL || log->infof == NULL) {
    return;
  }
  log->infof(log, "cai.client.opened", "base_url=%s timeout_ms=%d http2=%b",
             client->base_url != NULL ? client->base_url : "",
             (int)client->timeout_ms, client->http_2_disabled ? 0 : 1);
}

void cai_log_openrouter_server_continuity(const cai_client_impl *client) {
  pslog_logger *log;

  log = cai_logger(client);
  if (log == NULL || log->warnf == NULL) {
    return;
  }
  log->warnf(log, "cai.openrouter.server_side_continuity",
             "base_url=%s note=%s",
             client->base_url != NULL ? client->base_url : "",
             "OpenRouter Responses beta is stateless; use client_history "
             "continuity for multi-turn sessions");
}

void cai_log_http_request_start(const cai_client_impl *client,
                                const char *method, const char *path,
                                int stream, size_t request_bytes) {
  pslog_logger *log;

  log = cai_logger(client);
  if (log == NULL || log->tracef == NULL) {
    return;
  }
  log->tracef(log, "cai.http.request.start",
              "method=%s path=%s stream=%b request_bytes=%u",
              method != NULL ? method : "", path != NULL ? path : "", stream,
              (unsigned int)request_bytes);
}

void cai_log_http_request_done(const cai_client_impl *client,
                               const char *method, const char *path,
                               long http_status, size_t response_bytes,
                               const char *request_id) {
  pslog_logger *log;
  const char *safe_request_id;

  log = cai_logger(client);
  if (log == NULL) {
    return;
  }
  safe_request_id = request_id != NULL ? request_id : "";
  if (http_status >= 500L && log->errorf != NULL) {
    log->errorf(log, "cai.http.request.done",
                "method=%s path=%s status=%d response_bytes=%u request_id=%s",
                method != NULL ? method : "", path != NULL ? path : "",
                (int)http_status, (unsigned int)response_bytes,
                safe_request_id);
  } else if (http_status >= 400L && log->warnf != NULL) {
    log->warnf(log, "cai.http.request.done",
               "method=%s path=%s status=%d response_bytes=%u request_id=%s",
               method != NULL ? method : "", path != NULL ? path : "",
               (int)http_status, (unsigned int)response_bytes, safe_request_id);
  } else if (log->debugf != NULL) {
    log->debugf(log, "cai.http.request.done",
                "method=%s path=%s status=%d response_bytes=%u request_id=%s",
                method != NULL ? method : "", path != NULL ? path : "",
                (int)http_status, (unsigned int)response_bytes,
                safe_request_id);
  }
}

void cai_log_http_transport_error(const cai_client_impl *client,
                                  const char *method, const char *path,
                                  const char *detail) {
  pslog_logger *log;

  log = cai_logger(client);
  if (log == NULL || log->errorf == NULL) {
    return;
  }
  log->errorf(log, "cai.http.transport_error", "method=%s path=%s error=%s",
              method != NULL ? method : "", path != NULL ? path : "",
              detail != NULL ? detail : "");
}

void cai_log_http_response_limit(const cai_client_impl *client,
                                 const char *method, const char *path,
                                 size_t limit) {
  pslog_logger *log;

  log = cai_logger(client);
  if (log == NULL || log->warnf == NULL) {
    return;
  }
  log->warnf(log, "cai.http.response_limit", "method=%s path=%s limit=%u",
             method != NULL ? method : "", path != NULL ? path : "",
             (unsigned int)limit);
}
