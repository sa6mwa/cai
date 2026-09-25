#include "cai_internal.h"

#include <cai/quota.h>

#include <errno.h>
#include <math.h>
#include <stdlib.h>
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

typedef struct cai_quota_credits_doc {
  lonejson_json_value balance;
  int unlimited;
} cai_quota_credits_doc;

typedef struct cai_quota_doc {
  cai_quota_limit_doc rate_limit;
  cai_quota_credits_doc credits;
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

static const lonejson_field cai_quota_credits_fields[] = {
    LONEJSON_FIELD_JSON_VALUE(cai_quota_credits_doc, balance, "balance"),
    LONEJSON_FIELD_BOOL(cai_quota_credits_doc, unlimited, "unlimited")};
LONEJSON_MAP_DEFINE(cai_quota_credits_map, cai_quota_credits_doc,
                    cai_quota_credits_fields);

static const lonejson_field cai_quota_fields[] = {
    LONEJSON_FIELD_OBJECT_OMIT_EMPTY(cai_quota_doc, rate_limit, "rate_limit",
                                     &cai_quota_limit_map),
    LONEJSON_FIELD_OBJECT_OMIT_EMPTY(cai_quota_doc, credits, "credits",
                                     &cai_quota_credits_map)};
LONEJSON_MAP_DEFINE(cai_quota_map, cai_quota_doc, cai_quota_fields);

static void cai_quota_apply_window(cai_chatgpt_quota *out,
                                   const cai_quota_window_doc *window) {
  long long seconds;
  if (!window->has_used_percent || !window->has_limit_window_seconds ||
      window->used_percent < 0.0 || window->used_percent > 100.0) {
    return;
  }
  seconds = window->limit_window_seconds;
  if (seconds == 604800LL) {
    out->has_weekly = 1;
    out->weekly_remaining_percent = 100.0 - window->used_percent;
  } else if (seconds > 0LL && seconds < 604800LL &&
             (!out->has_short_window || seconds < out->short_window_seconds)) {
    out->has_short_window = 1;
    out->short_window_seconds = seconds;
    out->short_window_remaining_percent = 100.0 - window->used_percent;
  }
}

int cai_chatgpt_quota_parse_json(const char *json, cai_chatgpt_quota *out,
                                 cai_error *error) {
  cai_quota_doc doc;
  lonejson_error json_error;
  lonejson_status status;
  const char *raw_balance;
  char *end_balance;
  double balance;
  size_t balance_length;
  if (json == NULL || out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "quota JSON and output are required");
  }
  memset(out, 0, sizeof(*out));
  memset(&doc, 0, sizeof(doc));
  CAI_LJ->init(CAI_LJ, &cai_quota_map, &doc);
  status = lonejson_json_value_enable_parse_capture(&doc.credits.balance,
                                                    &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_quota_map, &doc);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to capture ChatGPT credit balance",
                                json_error.message);
  }
  status = CAI_LJ->parse_cstr(CAI_LJ, &cai_quota_map, &doc, json, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_quota_map, &doc);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to parse ChatGPT quota",
                                json_error.message);
  }
  cai_quota_apply_window(out, &doc.rate_limit.primary_window);
  cai_quota_apply_window(out, &doc.rate_limit.secondary_window);
  out->credits_unlimited = doc.credits.unlimited;
  raw_balance = doc.credits.balance.json;
  balance_length = doc.credits.balance.len;
  if (raw_balance != NULL && balance_length > 0U && raw_balance[0] != 'n') {
    if (raw_balance[0] == '"' && balance_length >= 2U &&
        raw_balance[balance_length - 1U] == '"') {
      raw_balance++;
      balance_length -= 2U;
    }
    if (balance_length > 0U) {
      errno = 0;
      balance = strtod(raw_balance, &end_balance);
      if (errno == 0 && isfinite(balance) && balance >= 0.0 &&
          end_balance == raw_balance + balance_length) {
        out->has_credit_balance = 1;
        out->credit_balance = balance;
      }
    }
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
