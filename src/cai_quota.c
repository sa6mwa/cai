#include "cai_internal.h"

#include <cai/quota.h>

#include <string.h>

typedef struct cai_quota_window_doc {
  double used_percent;
  int has_used_percent;
  long long limit_window_seconds;
  int has_limit_window_seconds;
} cai_quota_window_doc;

typedef struct cai_quota_limit_doc {
  cai_quota_window_doc primary_window;
  cai_quota_window_doc secondary_window;
} cai_quota_limit_doc;

typedef struct cai_quota_doc {
  cai_quota_limit_doc rate_limit;
} cai_quota_doc;

static const lonejson_field cai_quota_window_fields[] = {
    LONEJSON_FIELD_F64_PRESENT(cai_quota_window_doc, used_percent,
                               has_used_percent, "used_percent"),
    LONEJSON_FIELD_I64_PRESENT(cai_quota_window_doc, limit_window_seconds,
                               has_limit_window_seconds,
                               "limit_window_seconds")};
LONEJSON_MAP_DEFINE(cai_quota_window_map, cai_quota_window_doc,
                    cai_quota_window_fields);

static const lonejson_field cai_quota_limit_fields[] = {
    LONEJSON_FIELD_OBJECT_OMIT_EMPTY(cai_quota_limit_doc, primary_window,
                                     "primary_window", &cai_quota_window_map),
    LONEJSON_FIELD_OBJECT_OMIT_EMPTY(cai_quota_limit_doc, secondary_window,
                                     "secondary_window",
                                     &cai_quota_window_map)};
LONEJSON_MAP_DEFINE(cai_quota_limit_map, cai_quota_limit_doc,
                    cai_quota_limit_fields);

static const lonejson_field cai_quota_fields[] = {
    LONEJSON_FIELD_OBJECT_OMIT_EMPTY(cai_quota_doc, rate_limit, "rate_limit",
                                     &cai_quota_limit_map)};
LONEJSON_MAP_DEFINE(cai_quota_map, cai_quota_doc, cai_quota_fields);

int cai_chatgpt_quota_parse_json(const char *json, cai_chatgpt_quota *out,
                                 cai_error *error) {
  cai_quota_doc doc;
  lonejson_error json_error;
  lonejson_status status;
  if (json == NULL || out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "quota JSON and output are required");
  }
  memset(out, 0, sizeof(*out));
  memset(&doc, 0, sizeof(doc));
  CAI_LJ->init(CAI_LJ, &cai_quota_map, &doc);
  status = CAI_LJ->parse_cstr(CAI_LJ, &cai_quota_map, &doc, json, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_quota_map, &doc);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to parse ChatGPT quota",
                                json_error.message);
  }
  if (doc.rate_limit.primary_window.has_used_percent &&
      doc.rate_limit.primary_window.has_limit_window_seconds &&
      doc.rate_limit.primary_window.limit_window_seconds == 18000LL &&
      doc.rate_limit.primary_window.used_percent >= 0.0 &&
      doc.rate_limit.primary_window.used_percent <= 100.0) {
    out->has_five_hour = 1;
    out->five_hour_remaining_percent =
        100.0 - doc.rate_limit.primary_window.used_percent;
  }
  if (doc.rate_limit.secondary_window.has_used_percent &&
      doc.rate_limit.secondary_window.has_limit_window_seconds &&
      doc.rate_limit.secondary_window.limit_window_seconds == 604800LL &&
      doc.rate_limit.secondary_window.used_percent >= 0.0 &&
      doc.rate_limit.secondary_window.used_percent <= 100.0) {
    out->has_weekly = 1;
    out->weekly_remaining_percent =
        100.0 - doc.rate_limit.secondary_window.used_percent;
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_quota_map, &doc);
  return CAI_OK;
}

int cai_client_chatgpt_quota(cai_client *client, cai_chatgpt_quota *out,
                             cai_error *error) {
  char *body;
  char *request_id;
  long http_status;
  int rc;
  if (client == NULL || client->impl == NULL || out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "ChatGPT client and quota output are required");
  }
  memset(out, 0, sizeof(*out));
  if (CAI_CLIENT_IMPL(client)->chatgpt_auth == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "ChatGPT quota requires ChatGPT auth");
  }
  body = NULL;
  request_id = NULL;
  http_status = 0L;
  rc = cai_http_json_request(client, "GET", "../wham/usage", NULL, &body,
                             &http_status, &request_id, error);
  if (rc == CAI_OK && (http_status < 200L || http_status >= 300L)) {
    rc = cai_set_openai_error(error, http_status, body, request_id);
  }
  if (rc == CAI_OK) {
    rc = cai_chatgpt_quota_parse_json(body, out, error);
  }
  cai_free_mem(NULL, body);
  cai_free_mem(NULL, request_id);
  return rc;
}
