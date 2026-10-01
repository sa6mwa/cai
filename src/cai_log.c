#include "cai_internal.h"

#include <pslog.h>
#include <string.h>

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
  log->errorf(log, "cai.http.transport_error",
              "method=%s path=%s error=%s base_url=%s",
              method != NULL ? method : "", path != NULL ? path : "",
              detail != NULL ? detail : "",
              client->base_url != NULL ? client->base_url : "");
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

/* Log diagnostics, never the request, transcript, or raw SSE payload. */
void cai_log_chat_result(const cai_client_impl *client, const char *model,
                         int rc, long http_status, const char *request_id,
                         const cai_error *error) {
  pslog_logger *log = cai_logger(client);
  void (*emit)(pslog_logger *, const char *, const char *, ...);

  if (log == NULL)
    return;
  if (rc == CAI_OK || rc == CAI_ERR_CANCELLED)
    emit = log->debugf;
  else if (rc == CAI_ERR_LIMIT)
    emit = log->infof;
  else if (rc == CAI_ERR_SERVER && http_status >= 400L && http_status < 500L)
    emit = log->warnf;
  else if (rc == CAI_ERR_SERVER || rc == CAI_ERR_TRANSPORT ||
           rc == CAI_ERR_NOMEM)
    emit = log->errorf;
  else
    emit = log->warnf;
  if (emit != NULL) {
    emit(log, "cai.chat.stream.done",
         "model=%s status=%d code=%d server_code=%s request_id=%s error=%s "
         "detail=%s",
         model != NULL ? model : "", (int)http_status, rc,
         error != NULL && error->server_code != NULL ? error->server_code : "",
         request_id != NULL ? request_id : "",
         rc != CAI_OK && error != NULL && error->message != NULL
             ? error->message
             : "",
         rc != CAI_OK && error != NULL && error->detail != NULL ? error->detail
                                                                : "");
  }
}

void cai_log_chat_compaction(const cai_client_impl *client, const char *model,
                             int started, size_t history_bytes, int rc,
                             const cai_error *error) {
  pslog_logger *log = cai_logger(client);
  void (*emit)(pslog_logger *, const char *, const char *, ...);

  if (log == NULL)
    return;
  emit = rc == CAI_OK              ? log->infof
         : rc == CAI_ERR_CANCELLED ? log->debugf
                                   : log->warnf;
  if (emit != NULL) {
    emit(
        log, started ? "cai.chat.compaction.start" : "cai.chat.compaction.done",
        "model=%s history_bytes=%lu code=%d error=%s detail=%s",
        model != NULL ? model : "", (unsigned long)history_bytes, rc,
        rc != CAI_OK && error != NULL && error->message != NULL ? error->message
                                                                : "",
        rc != CAI_OK && error != NULL && error->detail != NULL ? error->detail
                                                               : "");
  }
}

void cai_log_model_catalog(const cai_client_impl *client, const char *source,
                           size_t count, int rc, const cai_error *error) {
  pslog_logger *log = cai_logger(client);
  void (*emit)(pslog_logger *, const char *, const char *, ...);

  if (log == NULL)
    return;
  emit = rc != CAI_OK ? log->warnf : log->debugf;
  if (emit != NULL) {
    emit(log, "cai.models.catalog",
         "source=%s models=%lu code=%d error=%s detail=%s base_url=%s "
         "http_status=%ld provider_code=%s request_id=%s",
         source != NULL ? source : "", (unsigned long)count, rc,
         rc != CAI_OK && error != NULL && error->message != NULL
             ? error->message
             : "",
         rc != CAI_OK && error != NULL && error->detail != NULL ? error->detail
                                                                : "",
         client->base_url != NULL ? client->base_url : "",
         rc != CAI_OK && error != NULL ? error->http_status : 0L,
         rc != CAI_OK && error != NULL && error->server_code != NULL
             ? error->server_code
             : "",
         rc != CAI_OK && error != NULL && error->request_id != NULL
             ? error->request_id
             : "");
  }
}

void cai_log_chat_retry(const cai_client_impl *client, unsigned int attempt,
                        long status, long delay_ms) {
  pslog_logger *log = cai_logger(client);
  if (log != NULL && log->infof != NULL)
    log->infof(log, "cai.chat.retry", "attempt=%u status=%d delay_ms=%ld",
               attempt, (int)status, delay_ms);
}

/* Construct fields locally: libcai borrows the receiver and requires only
 * pslog headers, never a linked libpslog implementation. */
static pslog_field cai_log_string(const char *key, const char *value) {
  pslog_field field;
  memset(&field, 0, sizeof(field));
  field.key = key;
  field.key_len = strlen(key);
  field.type = PSLOG_FIELD_STRING;
  field.as.string_value = value;
  field.value_len = strlen(value);
  return field;
}
static pslog_field cai_log_integer(const char *key, pslog_int64 value) {
  pslog_field field;
  memset(&field, 0, sizeof(field));
  field.key = key;
  field.key_len = strlen(key);
  field.type = PSLOG_FIELD_SIGNED;
  field.as.signed_value = value;
  return field;
}
static pslog_field cai_log_number(const char *key, double value) {
  pslog_field field;
  memset(&field, 0, sizeof(field));
  field.key = key;
  field.key_len = strlen(key);
  field.type = PSLOG_FIELD_DOUBLE;
  field.as.double_value = value;
  return field;
}
static pslog_field cai_log_boolean(const char *key, int value) {
  pslog_field field;
  memset(&field, 0, sizeof(field));
  field.key = key;
  field.key_len = strlen(key);
  field.type = PSLOG_FIELD_BOOL;
  field.as.bool_value = value;
  return field;
}

void cai_log_chat_metrics(const cai_client *client, const char *model,
                          const char *request_id, unsigned int attempt,
                          double elapsed_ms, double first_token_ms,
                          int has_usage, const cai_token_usage *usage) {
  pslog_logger *log = cai_logger(CAI_CLIENT_IMPL(client));
  const cai_model_catalog_entry *entry;
  pslog_field fields[15];
  size_t count = 0U;
  double usd, eur;
  int has_usd, has_eur;
  if (log == NULL || log->debug == NULL)
    return;
  fields[count++] = cai_log_string("model", model != NULL ? model : "");
  fields[count++] =
      cai_log_string("request_id", request_id != NULL ? request_id : "");
  fields[count++] = cai_log_integer("attempt", attempt);
  fields[count++] = cai_log_number("elapsed_ms", elapsed_ms);
  if (first_token_ms >= 0.0)
    fields[count++] = cai_log_number("first_token_ms", first_token_ms);
  fields[count++] = cai_log_boolean("has_usage", has_usage);
  if (has_usage) {
    fields[count++] = cai_log_integer("input_tokens", usage->input_tokens);
    fields[count++] =
        cai_log_integer("cached_input_tokens", usage->input_cached_tokens);
    fields[count++] = cai_log_integer("output_tokens", usage->output_tokens);
    fields[count++] =
        cai_log_integer("reasoning_tokens", usage->output_reasoning_tokens);
    fields[count++] = cai_log_integer("total_tokens", usage->total_tokens);
    cai_client_model_estimate_usage(client, model, usage, &usd, &eur);
    has_usd = cai_client_model_can_estimate_usd(client, model);
    entry = cai_client_cached_model_metadata(client, model);
    has_eur = entry != NULL && entry->has_pricing &&
              entry->pricing_currency != NULL &&
              strcmp(entry->pricing_currency, "EUR") == 0 &&
              entry->pricing_unit != NULL &&
              strstr(entry->pricing_unit, "M Token") != NULL;
    if (has_usd)
      fields[count++] = cai_log_number("estimated_cost_usd", usd);
    if (has_eur)
      fields[count++] = cai_log_number("estimated_cost_eur", eur);
  }
  log->debug(log, "cai.chat.request.metrics", fields, count);
}
