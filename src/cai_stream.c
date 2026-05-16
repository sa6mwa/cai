#include "cai_internal.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct cai_stream_input_tokens_details_doc {
  long long cached_tokens;
} cai_stream_input_tokens_details_doc;

typedef struct cai_stream_output_tokens_details_doc {
  long long reasoning_tokens;
} cai_stream_output_tokens_details_doc;

typedef struct cai_stream_usage_doc {
  long long input_tokens;
  long long input_cached_tokens;
  long long output_tokens;
  long long output_reasoning_tokens;
  long long total_tokens;
  cai_stream_input_tokens_details_doc input_tokens_details;
  cai_stream_output_tokens_details_doc output_tokens_details;
} cai_stream_usage_doc;

typedef struct cai_stream_response_doc {
  char *id;
  lonejson_json_value usage;
} cai_stream_response_doc;

typedef struct cai_stream_output_item_doc {
  char *id;
  char *type;
  char *call_id;
  char *name;
  char *arguments;
} cai_stream_output_item_doc;

typedef struct cai_stream_delta_doc {
  char *type;
  char *delta;
  char *item_id;
  long long output_index;
  char *call_id;
  char *name;
  char *arguments;
  cai_stream_output_item_doc item;
  lonejson_json_value response;
} cai_stream_delta_doc;

static const lonejson_field cai_stream_input_tokens_details_fields[] = {
    LONEJSON_FIELD_I64(cai_stream_input_tokens_details_doc, cached_tokens,
                       "cached_tokens")};
LONEJSON_MAP_DEFINE(cai_stream_input_tokens_details_map,
                    cai_stream_input_tokens_details_doc,
                    cai_stream_input_tokens_details_fields);

static const lonejson_field cai_stream_output_tokens_details_fields[] = {
    LONEJSON_FIELD_I64(cai_stream_output_tokens_details_doc, reasoning_tokens,
                       "reasoning_tokens")};
LONEJSON_MAP_DEFINE(cai_stream_output_tokens_details_map,
                    cai_stream_output_tokens_details_doc,
                    cai_stream_output_tokens_details_fields);

static const lonejson_field cai_stream_usage_fields[] = {
    LONEJSON_FIELD_I64(cai_stream_usage_doc, input_tokens, "input_tokens"),
    LONEJSON_FIELD_I64(cai_stream_usage_doc, input_cached_tokens,
                       "input_cached_tokens"),
    LONEJSON_FIELD_OBJECT(cai_stream_usage_doc, input_tokens_details,
                          "input_tokens_details",
                          &cai_stream_input_tokens_details_map),
    LONEJSON_FIELD_I64(cai_stream_usage_doc, output_tokens, "output_tokens"),
    LONEJSON_FIELD_I64(cai_stream_usage_doc, output_reasoning_tokens,
                       "output_reasoning_tokens"),
    LONEJSON_FIELD_OBJECT(cai_stream_usage_doc, output_tokens_details,
                          "output_tokens_details",
                          &cai_stream_output_tokens_details_map),
    LONEJSON_FIELD_I64(cai_stream_usage_doc, total_tokens, "total_tokens")};
LONEJSON_MAP_DEFINE(cai_stream_usage_map, cai_stream_usage_doc,
                    cai_stream_usage_fields);

static const lonejson_field cai_stream_response_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_stream_response_doc, id, "id"),
    LONEJSON_FIELD_JSON_VALUE_OMIT_NULL(cai_stream_response_doc, usage,
                                        "usage")};
LONEJSON_MAP_DEFINE(cai_stream_response_map, cai_stream_response_doc,
                    cai_stream_response_fields);

static const lonejson_field cai_stream_output_item_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_stream_output_item_doc, id, "id"),
    LONEJSON_FIELD_STRING_ALLOC(cai_stream_output_item_doc, type, "type"),
    LONEJSON_FIELD_STRING_ALLOC(cai_stream_output_item_doc, call_id, "call_id"),
    LONEJSON_FIELD_STRING_ALLOC(cai_stream_output_item_doc, name, "name"),
    LONEJSON_FIELD_STRING_ALLOC(cai_stream_output_item_doc, arguments,
                                "arguments")};
LONEJSON_MAP_DEFINE(cai_stream_output_item_map, cai_stream_output_item_doc,
                    cai_stream_output_item_fields);

static const lonejson_field cai_stream_delta_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_stream_delta_doc, type, "type"),
    LONEJSON_FIELD_STRING_ALLOC(cai_stream_delta_doc, delta, "delta"),
    LONEJSON_FIELD_STRING_ALLOC(cai_stream_delta_doc, item_id, "item_id"),
    LONEJSON_FIELD_I64(cai_stream_delta_doc, output_index, "output_index"),
    LONEJSON_FIELD_STRING_ALLOC(cai_stream_delta_doc, call_id, "call_id"),
    LONEJSON_FIELD_STRING_ALLOC(cai_stream_delta_doc, name, "name"),
    LONEJSON_FIELD_STRING_ALLOC(cai_stream_delta_doc, arguments, "arguments"),
    LONEJSON_FIELD_OBJECT(cai_stream_delta_doc, item, "item",
                          &cai_stream_output_item_map),
    LONEJSON_FIELD_JSON_VALUE_OMIT_NULL(cai_stream_delta_doc, response,
                                        "response")};
LONEJSON_MAP_DEFINE(cai_stream_delta_map, cai_stream_delta_doc,
                    cai_stream_delta_fields);

typedef struct cai_sse_state {
  cai_stream_sinks sinks;
  char **out_response_id;
  cai_token_usage *out_usage;
  lonejson_sse *sse;
  cai_stream_delta_doc doc;
  lonejson_parse_options parse_options;
  lonejson_sse_json_options json_options;
  char *body;
  size_t body_length;
  size_t body_capacity;
  int failed;
  int failed_code;
  const char *failed_message;
  int reasoning_summary_started;
  int reasoning_summary_suffixed;
  int output_text_started;
  int output_text_suffixed;
  char done_line[32];
  size_t done_line_length;
  int done_line_start;
  int done_seen;
} cai_sse_state;

typedef struct cai_pipe_stream {
  cai_client *client;
  cai_response_create_params *params;
  int has_params;
  char *response_id;
  cai_stream_complete_fn on_complete;
  void *complete_context;
  cai_token_usage usage;
  int has_usage;
  int read_fd;
  int write_fd;
  pthread_t thread;
  int thread_started;
} cai_pipe_stream;

static int cai_client_open_response_text_source_common(
    cai_client *client, const cai_response_create_params *params,
    cai_response_create_params *owned_params,
    cai_stream_complete_fn on_complete, void *complete_context, cai_source **out,
    cai_error *error);

static void cai_stream_copy_usage(cai_token_usage *out,
                                  const cai_stream_usage_doc *usage) {
  if (out == NULL || usage == NULL) {
    return;
  }
  out->input_tokens = usage->input_tokens;
  out->input_cached_tokens = usage->input_cached_tokens != 0LL
                                 ? usage->input_cached_tokens
                                 : usage->input_tokens_details.cached_tokens;
  out->output_tokens = usage->output_tokens;
  out->output_reasoning_tokens =
      usage->output_reasoning_tokens != 0LL
          ? usage->output_reasoning_tokens
          : usage->output_tokens_details.reasoning_tokens;
  out->total_tokens = usage->total_tokens;
}

static lonejson_status cai_stream_doc_init(cai_stream_delta_doc *doc,
                                           lonejson_error *error) {
  memset(doc, 0, sizeof(*doc));
  lonejson_init(&cai_stream_delta_map, doc);
  lonejson_json_value_init(&doc->response);
  return lonejson_json_value_enable_parse_capture(&doc->response, error);
}

static void cai_stream_doc_cleanup(cai_stream_delta_doc *doc) {
  lonejson_cleanup(&cai_stream_delta_map, doc);
}

static int cai_stream_copy_usage_value(cai_token_usage *out,
                                       const lonejson_json_value *value) {
  cai_stream_usage_doc usage;
  lonejson_error json_error;
  lonejson_status status;
  size_t i;
  const char *json;

  if (out == NULL || value == NULL || value->kind == LONEJSON_JSON_VALUE_NULL ||
      value->json == NULL || value->len == 0U) {
    return CAI_OK;
  }
  json = value->json;
  for (i = 0U; i < value->len; i++) {
    if (json[i] == ' ' || json[i] == '\n' || json[i] == '\r' ||
        json[i] == '\t') {
      continue;
    }
    if (json[i] == 'n') {
      return CAI_OK;
    }
    break;
  }
  memset(&usage, 0, sizeof(usage));
  lonejson_init(&cai_stream_usage_map, &usage);
  lonejson_error_init(&json_error);
  status = lonejson_parse_buffer(&cai_stream_usage_map, &usage, value->json,
                                 value->len, NULL, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    lonejson_cleanup(&cai_stream_usage_map, &usage);
    return CAI_ERR_PROTOCOL;
  }
  cai_stream_copy_usage(out, &usage);
  lonejson_cleanup(&cai_stream_usage_map, &usage);
  return CAI_OK;
}

static lonejson_status
cai_stream_response_doc_init(cai_stream_response_doc *doc,
                             lonejson_error *error) {
  memset(doc, 0, sizeof(*doc));
  lonejson_init(&cai_stream_response_map, doc);
  lonejson_json_value_init(&doc->usage);
  return lonejson_json_value_enable_parse_capture(&doc->usage, error);
}

static int cai_stream_parse_response_value(
    const lonejson_json_value *value, cai_stream_response_doc *response) {
  lonejson_parse_options options;
  lonejson_error json_error;
  lonejson_status status;
  size_t i;
  const char *json;

  if (value == NULL || value->kind == LONEJSON_JSON_VALUE_NULL ||
      value->json == NULL || value->len == 0U) {
    return CAI_OK;
  }
  json = value->json;
  for (i = 0U; i < value->len; i++) {
    if (json[i] == ' ' || json[i] == '\n' || json[i] == '\r' ||
        json[i] == '\t') {
      continue;
    }
    if (json[i] == 'n') {
      return CAI_OK;
    }
    break;
  }
  lonejson_error_init(&json_error);
  if (cai_stream_response_doc_init(response, &json_error) !=
      LONEJSON_STATUS_OK) {
    return CAI_ERR_NOMEM;
  }
  options = lonejson_default_parse_options();
  options.clear_destination = 0;
  lonejson_error_init(&json_error);
  status = lonejson_parse_buffer(&cai_stream_response_map, response,
                                 value->json, value->len, &options,
                                 &json_error);
  if (status != LONEJSON_STATUS_OK) {
    lonejson_cleanup(&cai_stream_response_map, response);
    return CAI_ERR_PROTOCOL;
  }
  return CAI_OK;
}

static const char *cai_stream_affix_text(const cai_stream_affix *affix) {
  if (affix == NULL) {
    return NULL;
  }
  if (affix->callback != NULL) {
    return affix->callback(affix->context);
  }
  return affix->text;
}

static int cai_stream_write_affix(cai_sink *sink,
                                  const cai_stream_affix *affix) {
  const char *text;
  size_t length;

  text = cai_stream_affix_text(affix);
  if (sink == NULL || text == NULL || text[0] == '\0') {
    return CAI_OK;
  }
  length = strlen(text);
  return cai_sink_write(sink, text, length, NULL);
}

static int cai_sse_finish_reasoning(cai_sse_state *state) {
  int rc;

  if (!state->reasoning_summary_started || state->reasoning_summary_suffixed) {
    return CAI_OK;
  }
  rc = cai_stream_write_affix(state->sinks.reasoning_summary,
                              &state->sinks.reasoning_summary_suffix);
  if (rc == CAI_OK) {
    state->reasoning_summary_suffixed = 1;
    state->reasoning_summary_started = 0;
  }
  return rc;
}

static int cai_sse_finish_output(cai_sse_state *state) {
  int rc;

  if (!state->output_text_started || state->output_text_suffixed) {
    return CAI_OK;
  }
  rc = cai_stream_write_affix(state->sinks.output_text,
                              &state->sinks.output_text_suffix);
  if (rc == CAI_OK) {
    state->output_text_suffixed = 1;
  }
  return rc;
}

static int cai_sse_write_reasoning_delta(cai_sse_state *state,
                                         const char *delta) {
  size_t length;
  int rc;

  if (state->sinks.reasoning_summary == NULL || delta == NULL) {
    return CAI_OK;
  }
  length = strlen(delta);
  if (length == 0U) {
    return CAI_OK;
  }
  if (!state->reasoning_summary_started) {
    rc = cai_stream_write_affix(state->sinks.reasoning_summary,
                                &state->sinks.reasoning_summary_prefix);
    if (rc != CAI_OK) {
      return rc;
    }
    state->reasoning_summary_started = 1;
  }
  state->reasoning_summary_suffixed = 0;
  return cai_sink_write(state->sinks.reasoning_summary, delta, length, NULL);
}

static int cai_sse_write_output_delta(cai_sse_state *state,
                                      const char *delta) {
  size_t length;
  int rc;

  if (state->sinks.output_text == NULL || delta == NULL) {
    return CAI_OK;
  }
  length = strlen(delta);
  if (length == 0U) {
    return CAI_OK;
  }
  rc = cai_sse_finish_reasoning(state);
  if (rc != CAI_OK) {
    return rc;
  }
  if (!state->output_text_started) {
    rc = cai_stream_write_affix(state->sinks.output_text,
                                &state->sinks.output_text_prefix);
    if (rc != CAI_OK) {
      return rc;
    }
    state->output_text_started = 1;
  }
  return cai_sink_write(state->sinks.output_text, delta, length, NULL);
}

static int cai_sse_emit_output_text_delta(cai_sse_state *state,
                                          const cai_stream_delta_doc *doc) {
  int rc;

  rc = CAI_OK;
  if (state->sinks.output_text_delta != NULL) {
    rc = state->sinks.output_text_delta(
        state->sinks.output_text_context, doc->item_id, (int)doc->output_index,
        doc->delta, NULL);
  }
  if (rc == CAI_OK) {
    rc = cai_sse_write_output_delta(state, doc->delta);
  }
  return rc;
}

static int cai_sse_emit_function_call_delta(cai_sse_state *state,
                                            const cai_stream_delta_doc *doc) {
  if (state->sinks.function_call_arguments_delta == NULL ||
      doc->delta == NULL) {
    return CAI_OK;
  }
  return state->sinks.function_call_arguments_delta(
      state->sinks.function_call_context, doc->item_id,
      (int)doc->output_index, doc->delta, NULL);
}

static int cai_sse_emit_function_call_done(cai_sse_state *state,
                                           const cai_stream_delta_doc *doc) {
  if (state->sinks.function_call_arguments_done == NULL) {
    return CAI_OK;
  }
  return state->sinks.function_call_arguments_done(
      state->sinks.function_call_context, doc->item_id,
      (int)doc->output_index, doc->call_id, doc->name, doc->arguments, NULL);
}

static int cai_sse_emit_doc(cai_sse_state *state,
                            const cai_stream_delta_doc *doc) {
  cai_stream_response_doc response;
  int has_response;
  int rc;

  if (doc == NULL) {
    return CAI_OK;
  }
  memset(&response, 0, sizeof(response));
  has_response = 0;
  rc = CAI_OK;
  if (doc->response.kind != LONEJSON_JSON_VALUE_NULL) {
    rc = cai_stream_parse_response_value(&doc->response, &response);
    if (rc == CAI_OK) {
      has_response = 1;
    }
  }
  if (doc->type != NULL &&
      strcmp(doc->type, "response.output_text.delta") == 0 &&
      doc->delta != NULL) {
    rc = cai_sse_emit_output_text_delta(state, doc);
  } else if (doc->type != NULL &&
             (strcmp(doc->type, "response.reasoning_summary_text.delta") ==
                  0 ||
              strcmp(doc->type, "response.reasoning_summary.delta") == 0 ||
              strcmp(doc->type, "response.reasoning_text.delta") == 0) &&
             doc->delta != NULL) {
    rc = cai_sse_write_reasoning_delta(state, doc->delta);
  } else if (doc->type != NULL &&
             (strcmp(doc->type, "response.reasoning_summary_text.done") == 0 ||
              strcmp(doc->type, "response.reasoning_summary.done") == 0 ||
              strcmp(doc->type, "response.reasoning_text.done") == 0)) {
    rc = cai_sse_finish_reasoning(state);
  } else if (doc->type != NULL &&
             strcmp(doc->type, "response.output_text.done") == 0) {
    rc = cai_sse_finish_output(state);
  } else if (doc->type != NULL &&
             strcmp(doc->type, "response.function_call_arguments.delta") == 0) {
    rc = cai_sse_emit_function_call_delta(state, doc);
  } else if (doc->type != NULL &&
             strcmp(doc->type, "response.function_call_arguments.done") == 0 &&
             doc->call_id != NULL && doc->name != NULL &&
             doc->arguments != NULL) {
    rc = cai_sse_emit_function_call_done(state, doc);
  } else if (doc->type != NULL &&
             strcmp(doc->type, "response.output_item.done") == 0 &&
             doc->item.type != NULL &&
             strcmp(doc->item.type, "function_call") == 0 &&
             doc->item.call_id != NULL && doc->item.name != NULL &&
             doc->item.arguments != NULL) {
    cai_stream_delta_doc item_doc;

    memset(&item_doc, 0, sizeof(item_doc));
    item_doc.item_id = doc->item.id;
    item_doc.output_index = doc->output_index;
    item_doc.call_id = doc->item.call_id;
    item_doc.name = doc->item.name;
    item_doc.arguments = doc->item.arguments;
    rc = cai_sse_emit_function_call_done(state, &item_doc);
  }
  if (rc == CAI_OK && state->out_response_id != NULL &&
      *state->out_response_id == NULL && has_response && response.id != NULL) {
    *state->out_response_id = cai_strdup(NULL, response.id);
    if (*state->out_response_id == NULL) {
      rc = CAI_ERR_NOMEM;
    }
  }
  if (rc == CAI_OK && doc->type != NULL &&
      strcmp(doc->type, "response.completed") == 0) {
    rc = cai_sse_finish_reasoning(state);
    if (rc == CAI_OK) {
      rc = cai_sse_finish_output(state);
    }
    if (rc == CAI_OK && state->out_usage != NULL) {
      rc = cai_stream_copy_usage_value(state->out_usage, &response.usage);
      if (rc != CAI_OK) {
        state->failed = 1;
        state->failed_code = rc;
        state->failed_message = "failed to parse streaming response usage";
      }
    }
  }
  if (has_response) {
    lonejson_cleanup(&cai_stream_response_map, &response);
  }
  return rc;
}

static lonejson_status cai_sse_json_event(void *user,
                                          const lonejson_sse_event *event,
                                          void *dst, lonejson_error *error) {
  cai_sse_state *state;
  cai_stream_delta_doc *doc;
  int rc;

  (void)event;
  (void)error;
  state = (cai_sse_state *)user;
  doc = (cai_stream_delta_doc *)dst;
  rc = cai_sse_emit_doc(state, doc);
  cai_stream_doc_cleanup(doc);
  lonejson_error_init(error);
  if (cai_stream_doc_init(doc, error) != LONEJSON_STATUS_OK) {
    state->failed = 1;
    state->failed_code = CAI_ERR_NOMEM;
    state->failed_message = "failed to allocate streaming SSE buffer";
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  if (rc == CAI_ERR_NOMEM) {
    state->failed = 1;
    state->failed_code = CAI_ERR_NOMEM;
    state->failed_message = "failed to allocate streaming SSE buffer";
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  if (rc != CAI_OK) {
    state->failed = 1;
    state->failed_code = rc;
    state->failed_message = "streaming response callback failed";
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static int cai_sse_status_to_code(lonejson_status status) {
  if (status == LONEJSON_STATUS_ALLOCATION_FAILED) {
    return CAI_ERR_NOMEM;
  }
  if (status == LONEJSON_STATUS_OVERFLOW) {
    return CAI_ERR_PROTOCOL;
  }
  if (status == LONEJSON_STATUS_INVALID_JSON ||
      status == LONEJSON_STATUS_TYPE_MISMATCH ||
      status == LONEJSON_STATUS_MISSING_REQUIRED_FIELD) {
    return CAI_ERR_PROTOCOL;
  }
  return CAI_ERR_TRANSPORT;
}

static const char *cai_sse_status_to_message(lonejson_status status) {
  if (status == LONEJSON_STATUS_ALLOCATION_FAILED) {
    return "failed to allocate streaming SSE buffer";
  }
  if (status == LONEJSON_STATUS_OVERFLOW) {
    return "streaming SSE event exceeded configured limit";
  }
  if (status == LONEJSON_STATUS_INVALID_JSON ||
      status == LONEJSON_STATUS_TYPE_MISMATCH ||
      status == LONEJSON_STATUS_MISSING_REQUIRED_FIELD) {
    return "failed to parse streaming SSE JSON";
  }
  return "streaming response callback failed";
}

static int cai_sse_push_json_bytes(cai_sse_state *state, const char *bytes,
                                   size_t total) {
  lonejson_error sse_error;
  lonejson_status status;

  if (total == 0U || state->done_seen) {
    return CAI_OK;
  }
  memset(&sse_error, 0, sizeof(sse_error));
  status = lonejson_sse_push_json(state->sse, &cai_stream_delta_map,
                                  &state->doc, bytes, total,
                                  &state->json_options, cai_sse_json_event,
                                  state, &sse_error);
  if (status != LONEJSON_STATUS_OK) {
    if (!state->failed) {
      state->failed = 1;
      state->failed_code = cai_sse_status_to_code(status);
      state->failed_message = cai_sse_status_to_message(status);
    }
    return state->failed_code;
  }
  return CAI_OK;
}

static int cai_sse_flush_done_line(cai_sse_state *state) {
  int rc;

  if (state->done_line_length == 0U) {
    return CAI_OK;
  }
  rc = cai_sse_push_json_bytes(state, state->done_line,
                               state->done_line_length);
  state->done_line_length = 0U;
  return rc;
}

static int cai_sse_feed_filtered_byte(cai_sse_state *state, char ch) {
  static const char done_prefix[] = "data: [DONE]";
  size_t prefix_len;
  int rc;

  if (state->done_seen) {
    return CAI_OK;
  }
  prefix_len = sizeof(done_prefix) - 1U;
  if (state->done_line_start || state->done_line_length > 0U) {
    if (state->done_line_length < prefix_len) {
      if (ch == done_prefix[state->done_line_length]) {
        state->done_line[state->done_line_length++] = ch;
        state->done_line_start = 0;
        return CAI_OK;
      }
      rc = cai_sse_flush_done_line(state);
      if (rc != CAI_OK) {
        return rc;
      }
      state->done_line_start = ch == '\n';
      return cai_sse_push_json_bytes(state, &ch, 1U);
    }
    if (ch == '\r') {
      if (state->done_line_length < sizeof(state->done_line)) {
        state->done_line[state->done_line_length++] = ch;
      }
      return CAI_OK;
    }
    if (ch == '\n') {
      state->done_line_length = 0U;
      state->done_line_start = 1;
      state->done_seen = 1;
      return CAI_OK;
    }
    rc = cai_sse_flush_done_line(state);
    if (rc != CAI_OK) {
      return rc;
    }
    state->done_line_start = ch == '\n';
    return cai_sse_push_json_bytes(state, &ch, 1U);
  }
  state->done_line_start = ch == '\n';
  return cai_sse_push_json_bytes(state, &ch, 1U);
}

static int cai_sse_push_filtered(cai_sse_state *state, const char *bytes,
                                 size_t total) {
  size_t i;
  int rc;

  for (i = 0U; i < total; i++) {
    rc = cai_sse_feed_filtered_byte(state, bytes[i]);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  return CAI_OK;
}

static size_t cai_sse_write(void *ptr, size_t size, size_t nmemb, void *user) {
  cai_sse_state *state;
  const char *bytes;
  size_t total;
  size_t capture;
  size_t needed;
  size_t new_capacity;
  char *grown;
  int rc;

  state = (cai_sse_state *)user;
  bytes = (const char *)ptr;
  total = size * nmemb;
  capture = total;
  if (state->body_length + capture > 65536U) {
    capture = 65536U - state->body_length;
  }
  if (capture > 0U) {
    needed = state->body_length + capture + 1U;
    if (needed > state->body_capacity) {
      new_capacity = state->body_capacity == 0U ? 1024U : state->body_capacity;
      while (new_capacity < needed) {
        new_capacity *= 2U;
      }
      grown = (char *)cai_realloc_mem(NULL, state->body, new_capacity);
      if (grown == NULL) {
        state->failed = 1;
        state->failed_code = CAI_ERR_NOMEM;
        state->failed_message = "failed to allocate streaming response buffer";
        return 0U;
      }
      state->body = grown;
      state->body_capacity = new_capacity;
    }
    memcpy(state->body + state->body_length, bytes, capture);
    state->body_length += capture;
    state->body[state->body_length] = '\0';
  }
  rc = cai_sse_push_filtered(state, bytes, total);
  if (rc != CAI_OK) {
    return 0U;
  }
  return total;
}

static int cai_client_stream_response_params_with_id(
    cai_client *client, const cai_response_create_params *params,
    const cai_stream_sinks *sinks, char **out_response_id,
    cai_token_usage *out_usage, cai_error *error) {
  CURL *curl;
  CURLcode curl_rc;
  struct curl_slist *headers;
  cai_sse_state state;
  cai_response_request_upload *upload;
  char *url;
  long http_status;
  int rc;
  lonejson_sse_options sse_options;
  lonejson_error sse_error;
  lonejson_status sse_status;

  if (client == NULL || params == NULL || sinks == NULL ||
      (sinks->output_text == NULL && sinks->reasoning_summary == NULL &&
       sinks->function_call_arguments_delta == NULL &&
       sinks->function_call_arguments_done == NULL)) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "client, params, and at least one stream sink or "
                         "callback are required");
  }
  url = NULL;
  headers = NULL;
  upload = NULL;
  if (out_response_id != NULL) {
    *out_response_id = NULL;
  }
  if (out_usage != NULL) {
    memset(out_usage, 0, sizeof(*out_usage));
  }
  sse_options = lonejson_default_sse_options();
  sse_options.max_line_bytes = CAI_DEFAULT_SSE_EVENT_LIMIT;
  sse_options.max_event_data_bytes = CAI_DEFAULT_SSE_EVENT_LIMIT;
  sse_options.max_buffered_bytes = CAI_DEFAULT_SSE_EVENT_LIMIT;
  memset(&sse_error, 0, sizeof(sse_error));
  state.sinks = *sinks;
  state.out_response_id = out_response_id;
  state.out_usage = out_usage;
  state.sse = lonejson_sse_open(&sse_options, &sse_error);
  if (cai_stream_doc_init(&state.doc, &sse_error) != LONEJSON_STATUS_OK) {
    if (state.sse != NULL) {
      lonejson_sse_close(state.sse);
    }
    return cai_set_error(error, cai_sse_status_to_code(sse_error.code),
                         cai_sse_status_to_message(sse_error.code));
  }
  state.parse_options = lonejson_default_parse_options();
  state.parse_options.clear_destination = 0;
  memset(&state.json_options, 0, sizeof(state.json_options));
  state.json_options.parse_options = &state.parse_options;
  state.body = NULL;
  state.body_length = 0U;
  state.body_capacity = 0U;
  state.failed = 0;
  state.failed_code = CAI_ERR_TRANSPORT;
  state.failed_message = "streaming response sink failed";
  state.reasoning_summary_started = 0;
  state.reasoning_summary_suffixed = 0;
  state.output_text_started = 0;
  state.output_text_suffixed = 0;
  state.done_line_length = 0U;
  state.done_line_start = 1;
  state.done_seen = 0;
  if (state.sse == NULL) {
    rc = cai_set_error(error, cai_sse_status_to_code(sse_error.code),
                       cai_sse_status_to_message(sse_error.code));
    cai_stream_doc_cleanup(&state.doc);
    return rc;
  }

  rc = cai_response_request_upload_open(params, 1, &upload, error);
  if (rc == CAI_OK) {
    rc = cai_build_url(&CAI_CLIENT_IMPL(client)->allocator,
                       CAI_CLIENT_IMPL(client)->base_url, "responses", &url,
                       error);
  }
  if (rc == CAI_OK) {
    rc = cai_append_header(&headers, "Content-Type: application/json", error);
  }
  if (rc == CAI_OK) {
    rc = cai_append_header(&headers, "Accept: text/event-stream", error);
  }
  if (rc == CAI_OK) {
    rc = cai_append_header(&headers, "Expect:", error);
  }
  if (rc == CAI_OK) {
    rc = cai_append_bearer_header(client, &headers, error);
  }
  if (rc == CAI_OK) {
    rc = cai_append_prefixed_header(client, &headers, "OpenAI-Organization: ",
                                    CAI_CLIENT_IMPL(client)->organization_id,
                                    error);
  }
  if (rc == CAI_OK) {
    rc = cai_append_prefixed_header(client, &headers, "OpenAI-Project: ",
                                    CAI_CLIENT_IMPL(client)->project_id,
                                    error);
  }
  if (rc != CAI_OK) {
    curl_slist_free_all(headers);
    cai_free_mem(&CAI_CLIENT_IMPL(client)->allocator, url);
    lonejson_sse_close(state.sse);
    cai_stream_doc_cleanup(&state.doc);
    return rc;
  }
  curl = curl_easy_init();
  if (curl == NULL) {
    curl_slist_free_all(headers);
    cai_free_mem(&CAI_CLIENT_IMPL(client)->allocator, url);
    cai_response_request_upload_close(upload);
    lonejson_sse_close(state.sse);
    cai_stream_doc_cleanup(&state.doc);
    return cai_set_error(error, CAI_ERR_TRANSPORT, "failed to initialize curl");
  }
  if (rc == CAI_OK) {
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION,
                     cai_response_request_upload_read);
    curl_easy_setopt(curl, CURLOPT_READDATA, upload);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                     cai_response_request_upload_size(upload));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cai_sse_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (CAI_CLIENT_IMPL(client)->timeout_ms > 0L) {
      curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                       CAI_CLIENT_IMPL(client)->timeout_ms);
    }
    if (CAI_CLIENT_IMPL(client)->http_2_disabled) {
      curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,
                       (long)CURL_HTTP_VERSION_1_1);
    } else {
      curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,
                       (long)CURL_HTTP_VERSION_2TLS);
    }
    if (CAI_CLIENT_IMPL(client)->insecure_skip_verify) {
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    cai_log_http_request_start(CAI_CLIENT_IMPL(client), "POST", "responses", 1,
                               (size_t)cai_response_request_upload_size(
                                   upload));
    curl_rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    if (curl_rc == CURLE_OK) {
      cai_log_http_request_done(CAI_CLIENT_IMPL(client), "POST", "responses",
                                http_status, state.body_length, NULL);
    }
  } else {
    curl_rc = CURLE_OK;
    http_status = 0L;
  }
  curl_easy_cleanup(curl);
  if (curl_rc == CURLE_OK && !state.failed) {
    rc = cai_sse_flush_done_line(&state);
    if (rc != CAI_OK) {
      state.failed = 1;
      state.failed_code = rc;
      state.failed_message = "streaming response callback failed";
    }
  }
  if (curl_rc == CURLE_OK && !state.failed && !state.done_seen) {
    memset(&sse_error, 0, sizeof(sse_error));
    sse_status = lonejson_sse_finish_json(
        state.sse, &cai_stream_delta_map, &state.doc, &state.json_options,
        cai_sse_json_event, &state, &sse_error);
    if (sse_status != LONEJSON_STATUS_OK) {
      if (!state.failed) {
        state.failed = 1;
        state.failed_code = cai_sse_status_to_code(sse_status);
        state.failed_message = cai_sse_status_to_message(sse_status);
      }
    }
  }
  curl_slist_free_all(headers);
  cai_free_mem(&CAI_CLIENT_IMPL(client)->allocator, url);
  lonejson_sse_close(state.sse);
  cai_stream_doc_cleanup(&state.doc);
  cai_response_request_upload_close(upload);
  if (rc != CAI_OK) {
    cai_free_mem(NULL, state.body);
    return rc;
  }
  if (curl_rc != CURLE_OK) {
    cai_free_mem(NULL, state.body);
    if (state.failed) {
      cai_log_http_transport_error(CAI_CLIENT_IMPL(client), "POST",
                                   "responses", state.failed_message);
      return cai_set_error(error, state.failed_code, state.failed_message);
    }
    cai_log_http_transport_error(CAI_CLIENT_IMPL(client), "POST", "responses",
                                 curl_easy_strerror(curl_rc));
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "streaming HTTP request failed",
                                curl_easy_strerror(curl_rc));
  }
  if (state.failed) {
    cai_free_mem(NULL, state.body);
    cai_log_http_transport_error(CAI_CLIENT_IMPL(client), "POST", "responses",
                                 state.failed_message);
    return cai_set_error(error, state.failed_code, state.failed_message);
  }
  if (http_status < 200L || http_status >= 300L) {
    rc = cai_set_openai_error(error, http_status,
                              state.body != NULL ? state.body : "", NULL);
    cai_free_mem(NULL, state.body);
    return rc;
  }
  cai_free_mem(NULL, state.body);
  return CAI_OK;
}

int cai_client_stream_response_text(cai_client *client,
                                    const cai_response_create_params *params,
                                    cai_sink *sink, cai_error *error) {
  return cai_client_stream_response_text_with_id(client, params, sink, NULL,
                                                 NULL, error);
}

int cai_client_stream_response_text_with_id(
    cai_client *client, const cai_response_create_params *params,
    cai_sink *sink, char **out_response_id, cai_token_usage *out_usage,
    cai_error *error) {
  cai_stream_sinks sinks;

  cai_stream_sinks_init(&sinks);
  sinks.output_text = sink;
  return cai_client_stream_response_with_id(client, params, &sinks,
                                            out_response_id, out_usage, error);
}

int cai_client_stream_response_with_id(
    cai_client *client, const cai_response_create_params *params,
  const cai_stream_sinks *sinks, char **out_response_id,
  cai_token_usage *out_usage, cai_error *error) {
  if (out_response_id != NULL) {
    *out_response_id = NULL;
  }
  if (out_usage != NULL) {
    memset(out_usage, 0, sizeof(*out_usage));
  }
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "params are required");
  }
  return cai_client_stream_response_params_with_id(
      client, params, sinks, out_response_id, out_usage, error);
}

static int cai_pipe_sink_write(void *context, const void *bytes, size_t count,
                               cai_error *error) {
  int fd;
  const char *data;
  size_t offset;
  ssize_t written;

  (void)error;
  fd = *(int *)context;
  data = (const char *)bytes;
  offset = 0U;
  while (offset < count) {
    written = write(fd, data + offset, count - offset);
    if (written <= 0) {
      return CAI_ERR_TRANSPORT;
    }
    offset += (size_t)written;
  }
  return CAI_OK;
}

static void *cai_pipe_stream_main(void *arg) {
  cai_pipe_stream *stream;
  cai_sink_callbacks callbacks;
  cai_sink *sink;
  cai_error error;
  int rc;

  stream = (cai_pipe_stream *)arg;
  sink = NULL;
  cai_error_init(&error);
  callbacks.write = cai_pipe_sink_write;
  callbacks.close = NULL;
  callbacks.context = &stream->write_fd;
  if (cai_sink_from_callbacks(&callbacks, &sink, &error) == CAI_OK) {
    rc = cai_client_stream_response_text_with_id(
        stream->client, stream->params, sink, &stream->response_id,
        &stream->usage, &error);
    if (rc == CAI_OK) {
      stream->has_usage = 1;
    }
  }
  cai_sink_close(sink);
  close(stream->write_fd);
  stream->write_fd = -1;
  cai_error_cleanup(&error);
  return NULL;
}

static size_t cai_pipe_source_read(void *context, void *buffer, size_t count,
                                   cai_error *error) {
  cai_pipe_stream *stream;
  ssize_t got;

  (void)error;
  stream = (cai_pipe_stream *)context;
  if (stream == NULL || stream->read_fd < 0) {
    return 0U;
  }
  got = read(stream->read_fd, buffer, count);
  if (got <= 0) {
    return 0U;
  }
  return (size_t)got;
}

static void cai_pipe_source_close(void *context) {
  cai_pipe_stream *stream;

  stream = (cai_pipe_stream *)context;
  if (stream == NULL) {
    return;
  }
  if (stream->read_fd >= 0) {
    close(stream->read_fd);
    stream->read_fd = -1;
  }
  if (stream->thread_started) {
    pthread_join(stream->thread, NULL);
  }
  if (stream->on_complete != NULL && stream->response_id != NULL) {
    (void)stream->on_complete(stream->complete_context, stream->response_id,
                              stream->has_usage ? &stream->usage : NULL);
  }
  if (stream->write_fd >= 0) {
    close(stream->write_fd);
  }
  if (stream->has_params) {
    cai_response_create_params_destroy(stream->params);
  }
  cai_free_mem(NULL, stream->response_id);
  cai_free_mem(NULL, stream);
}

int cai_client_open_response_text_source(
    cai_client *client, const cai_response_create_params *params,
    cai_source **out, cai_error *error) {
  return cai_client_open_response_text_source_with_complete(
      client, params, NULL, NULL, out, error);
}

int cai_client_open_response_text_source_with_complete(
    cai_client *client, const cai_response_create_params *params,
    cai_stream_complete_fn on_complete, void *complete_context,
    cai_source **out, cai_error *error) {
  return cai_client_open_response_text_source_common(
      client, params, NULL, on_complete, complete_context, out, error);
}

int cai_client_open_response_text_source_take_params(
    cai_client *client, cai_response_create_params *params,
    cai_stream_complete_fn on_complete, void *complete_context,
    cai_source **out, cai_error *error) {
  return cai_client_open_response_text_source_common(
      client, params, params, on_complete, complete_context, out, error);
}

static int cai_client_open_response_text_source_common(
    cai_client *client, const cai_response_create_params *params,
    cai_response_create_params *owned_params,
    cai_stream_complete_fn on_complete, void *complete_context,
    cai_source **out, cai_error *error) {
  cai_pipe_stream *stream;
  cai_source_callbacks callbacks;
  int fds[2];
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "source output pointer is required");
  }
  *out = NULL;
  if (client == NULL || params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "client and params are required");
  }
  if (pipe(fds) != 0) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to create streaming pipe");
  }
  stream = (cai_pipe_stream *)cai_alloc(NULL, sizeof(*stream));
  if (stream == NULL) {
    close(fds[0]);
    close(fds[1]);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate streaming source");
  }
  stream->client = client;
  stream->params = NULL;
  stream->has_params = 0;
  stream->response_id = NULL;
  stream->on_complete = on_complete;
  stream->complete_context = complete_context;
  memset(&stream->usage, 0, sizeof(stream->usage));
  stream->has_usage = 0;
  stream->read_fd = fds[0];
  stream->write_fd = fds[1];
  stream->thread_started = 0;
  if (owned_params != NULL) {
    stream->params = owned_params;
    stream->has_params = 1;
    rc = CAI_OK;
  } else {
    rc = cai_response_create_params_clone(params, &stream->params, error);
    if (rc != CAI_OK) {
      cai_pipe_source_close(stream);
      return rc;
    }
    stream->has_params = 1;
  }
  if (pthread_create(&stream->thread, NULL, cai_pipe_stream_main, stream) !=
      0) {
    cai_pipe_source_close(stream);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to start streaming thread");
  }
  stream->thread_started = 1;
  callbacks.read = cai_pipe_source_read;
  callbacks.reset = NULL;
  callbacks.close = cai_pipe_source_close;
  callbacks.context = stream;
  rc = cai_source_from_callbacks(&callbacks, out, error);
  if (rc != CAI_OK) {
    cai_pipe_source_close(stream);
  }
  return rc;
}
