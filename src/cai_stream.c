#include "cai_internal.h"

#include <curl/curl.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CAI_STREAM_FIELD_MEMORY_LIMIT (64U * 1024U)

static const char *const cai_stream_json_event_names[] = {
    "",
    "response.created",
    "response.in_progress",
    "response.completed",
    "response.output_text.delta",
    "response.reasoning_summary_text.delta",
    "response.reasoning_summary.delta",
    "response.reasoning_text.delta",
    "response.reasoning_summary_text.done",
    "response.reasoning_summary.done",
    "response.reasoning_text.done",
    "response.function_call_arguments.delta",
    "response.function_call_arguments.done",
    "response.output_item.done"};

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
  cai_stream_usage_doc usage;
} cai_stream_response_doc;

typedef struct cai_stream_response_event_doc {
  lonejson_spooled response_storage;
  lonejson_json_value response;
} cai_stream_response_event_doc;

typedef struct cai_stream_output_item_doc {
  char *id;
  char *type;
  char *call_id;
  char *name;
  lonejson_spooled arguments;
} cai_stream_output_item_doc;

typedef struct cai_stream_delta_event_doc {
  lonejson_spooled delta;
  char *item_id;
  long long output_index;
} cai_stream_delta_event_doc;

typedef struct cai_stream_function_call_done_event_doc {
  char *item_id;
  long long output_index;
  char *call_id;
  char *name;
  lonejson_spooled arguments;
} cai_stream_function_call_done_event_doc;

typedef struct cai_stream_output_item_event_doc {
  long long output_index;
  lonejson_spooled item_storage;
  lonejson_json_value item;
} cai_stream_output_item_event_doc;

typedef struct cai_stream_spooled_reader {
  lonejson_spooled cursor;
} cai_stream_spooled_reader;

static lonejson_read_result cai_stream_spooled_read(void *user,
                                                    unsigned char *buffer,
                                                    size_t capacity) {
  cai_stream_spooled_reader *reader;

  reader = (cai_stream_spooled_reader *)user;
  if (reader == NULL) {
    return lonejson_default_read_result();
  }
  return lonejson_spooled_read(&reader->cursor, buffer, capacity);
}

static const lonejson_field cai_stream_output_item_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_stream_output_item_doc, id, "id"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_stream_output_item_doc, type,
                                          "type"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_stream_output_item_doc, call_id,
                                          "call_id"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_stream_output_item_doc, name,
                                          "name"),
    LONEJSON_FIELD_STRING_STREAM_CLASS(cai_stream_output_item_doc, arguments,
                                       "arguments",
                                       LONEJSON_SPOOL_CLASS_LARGE_TEXT)};
LONEJSON_MAP_DEFINE(cai_stream_output_item_map, cai_stream_output_item_doc,
                    cai_stream_output_item_fields);

static const lonejson_field cai_stream_delta_event_fields[] = {
    LONEJSON_FIELD_STRING_STREAM_REQ(cai_stream_delta_event_doc, delta, "delta"),
    LONEJSON_FIELD_STRING_ALLOC(cai_stream_delta_event_doc, item_id, "item_id"),
    LONEJSON_FIELD_I64(cai_stream_delta_event_doc, output_index,
                       "output_index")};
LONEJSON_MAP_DEFINE(cai_stream_delta_event_map, cai_stream_delta_event_doc,
                    cai_stream_delta_event_fields);

static const lonejson_field cai_stream_function_call_done_event_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_stream_function_call_done_event_doc,
                                item_id, "item_id"),
    LONEJSON_FIELD_I64(cai_stream_function_call_done_event_doc, output_index,
                       "output_index"),
    LONEJSON_FIELD_STRING_ALLOC(cai_stream_function_call_done_event_doc,
                                call_id, "call_id"),
    LONEJSON_FIELD_STRING_ALLOC(cai_stream_function_call_done_event_doc,
                                name, "name"),
    LONEJSON_FIELD_STRING_STREAM_REQ(cai_stream_function_call_done_event_doc,
                                     arguments, "arguments")};
LONEJSON_MAP_DEFINE(cai_stream_function_call_done_event_map,
                    cai_stream_function_call_done_event_doc,
                    cai_stream_function_call_done_event_fields);

static const lonejson_field cai_stream_output_item_event_fields[] = {
    LONEJSON_FIELD_I64(cai_stream_output_item_event_doc, output_index,
                       "output_index"),
    LONEJSON_FIELD_JSON_VALUE_REQ(cai_stream_output_item_event_doc, item,
                                  "item")};
LONEJSON_MAP_DEFINE(cai_stream_output_item_event_map,
                    cai_stream_output_item_event_doc,
                    cai_stream_output_item_event_fields);

static const lonejson_field cai_stream_response_event_fields[] = {
    LONEJSON_FIELD_JSON_VALUE_REQ(cai_stream_response_event_doc, response,
                                  "response")};
LONEJSON_MAP_DEFINE(cai_stream_response_event_map,
                    cai_stream_response_event_doc,
                    cai_stream_response_event_fields);

typedef struct cai_sse_state {
  cai_stream_sinks sinks;
  char **out_response_id;
  cai_token_usage *out_usage;
  lonejson_sse *sse;
  lonejson_spooled event_json_storage;
  lonejson_json_value event_json;
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
  char **emitted_call_ids;
  size_t emitted_call_count;
  size_t emitted_call_capacity;
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
  pthread_mutex_t lock;
  int lock_initialized;
  cai_error worker_error;
  int worker_error_initialized;
  int worker_done;
  int worker_rc;
  int worker_error_reported;
} cai_pipe_stream;

typedef struct cai_stream_sink_context {
  cai_sink *sink;
  cai_error *error;
} cai_stream_sink_context;

static int cai_client_open_response_text_source_common(
    cai_client *client, const cai_response_create_params *params,
    cai_response_create_params *owned_params,
    cai_stream_complete_fn on_complete, void *complete_context, cai_source **out,
    cai_error *error);

static void cai_pipe_stream_finish(cai_pipe_stream *stream, int rc,
                                   const cai_error *error) {
  if (stream == NULL || !stream->lock_initialized) {
    return;
  }
  pthread_mutex_lock(&stream->lock);
  stream->worker_rc = rc;
  if (rc != CAI_OK) {
    (void)cai_set_error_http(
        &stream->worker_error,
        error != NULL && error->code != CAI_OK ? error->code : rc,
        error != NULL ? error->http_status : 0L,
        error != NULL && error->message != NULL ? error->message
                                                : cai_status_string(rc),
        error != NULL ? error->detail : NULL,
        error != NULL ? error->server_code : NULL,
        error != NULL ? error->request_id : NULL);
  }
  stream->worker_done = 1;
  pthread_mutex_unlock(&stream->lock);
}

static int cai_pipe_stream_take_error(cai_pipe_stream *stream,
                                      cai_error *error) {
  int rc;

  if (stream == NULL || !stream->lock_initialized) {
    return CAI_OK;
  }
  pthread_mutex_lock(&stream->lock);
  rc = CAI_OK;
  if (stream->worker_done && stream->worker_rc != CAI_OK &&
      !stream->worker_error_reported) {
    rc = stream->worker_error.code != CAI_OK ? stream->worker_error.code
                                             : stream->worker_rc;
    stream->worker_error_reported = 1;
    (void)cai_set_error_http(error, rc, stream->worker_error.http_status,
                             stream->worker_error.message,
                             stream->worker_error.detail,
                             stream->worker_error.server_code,
                             stream->worker_error.request_id);
  }
  pthread_mutex_unlock(&stream->lock);
  return rc;
}

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

static lonejson_status cai_stream_discard_json_sink(void *user,
                                                    const void *data,
                                                    size_t len,
                                                    lonejson_error *error);

static lonejson_status cai_stream_event_json_init(cai_sse_state *state,
                                                  lonejson_error *error) {
  lonejson_spooled_init(CAI_LJ_STREAM, &state->event_json_storage);
  CAI_LJ_STREAM->json_value_init(CAI_LJ_STREAM, &state->event_json);
  if (lonejson_json_value_set_parse_sink(&state->event_json,
                                         cai_stream_discard_json_sink,
                                         &state->event_json_storage,
                                         error) != LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static void cai_stream_event_json_cleanup(cai_sse_state *state) {
  CAI_LJ_STREAM->json_value_cleanup(CAI_LJ_STREAM, &state->event_json);
  lonejson_spooled_cleanup(&state->event_json_storage);
}

static int cai_sse_has_emitted_call_id(const cai_sse_state *state,
                                       const char *call_id) {
  size_t i;

  if (state == NULL || call_id == NULL || call_id[0] == '\0') {
    return 0;
  }
  for (i = 0U; i < state->emitted_call_count; i++) {
    if (state->emitted_call_ids[i] != NULL &&
        strcmp(state->emitted_call_ids[i], call_id) == 0) {
      return 1;
    }
  }
  return 0;
}

static int cai_sse_record_call_id(cai_sse_state *state, const char *call_id) {
  char **grown;
  char *copy;

  if (state == NULL || call_id == NULL || call_id[0] == '\0' ||
      cai_sse_has_emitted_call_id(state, call_id)) {
    return CAI_OK;
  }
  if (state->emitted_call_count == state->emitted_call_capacity) {
    size_t new_capacity;

    new_capacity = state->emitted_call_capacity == 0U
                       ? 4U
                       : state->emitted_call_capacity * 2U;
    grown = (char **)cai_realloc_mem(NULL, state->emitted_call_ids,
                                     new_capacity * sizeof(*grown));
    if (grown == NULL) {
      return CAI_ERR_NOMEM;
    }
    state->emitted_call_ids = grown;
    state->emitted_call_capacity = new_capacity;
  }
  copy = cai_strdup(NULL, call_id);
  if (copy == NULL) {
    return CAI_ERR_NOMEM;
  }
  state->emitted_call_ids[state->emitted_call_count++] = copy;
  return CAI_OK;
}

static void cai_sse_cleanup_call_ids(cai_sse_state *state) {
  size_t i;

  if (state == NULL) {
    return;
  }
  for (i = 0U; i < state->emitted_call_count; i++) {
    cai_free_mem(NULL, state->emitted_call_ids[i]);
  }
  cai_free_mem(NULL, state->emitted_call_ids);
  state->emitted_call_ids = NULL;
  state->emitted_call_count = 0U;
  state->emitted_call_capacity = 0U;
}

static int cai_stream_parse_spooled(const lonejson_map *map, void *dst,
                                    const lonejson_spooled *value) {
  cai_stream_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;

  if (map == NULL || dst == NULL || value == NULL) {
    return CAI_ERR_INVALID;
  }
  reader.cursor = *value;
  lonejson_error_init(&json_error);
  if (lonejson_spooled_rewind(&reader.cursor, &json_error) !=
      LONEJSON_STATUS_OK) {
    return CAI_ERR_PROTOCOL;
  }
  lonejson_error_init(&json_error);
  status = CAI_LJ_STREAM->parse_reader(CAI_LJ_STREAM, map, dst,
                                       cai_stream_spooled_read, &reader,
                                       &json_error);
  if (status != LONEJSON_STATUS_OK) {
    return CAI_ERR_PROTOCOL;
  }
  return CAI_OK;
}

static int cai_stream_parse_output_item_event(
    const lonejson_spooled *value, cai_stream_output_item_event_doc *event_doc) {
  cai_stream_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;

  memset(event_doc, 0, sizeof(*event_doc));
  lonejson_init(CAI_LJ_STREAM, &cai_stream_output_item_event_map, event_doc);
  lonejson_spooled_init(CAI_LJ_STREAM, &event_doc->item_storage);
  CAI_LJ_STREAM->json_value_init(CAI_LJ_STREAM, &event_doc->item);
  lonejson_error_init(&json_error);
  if (lonejson_json_value_set_parse_sink(&event_doc->item,
                                         cai_stream_discard_json_sink,
                                         &event_doc->item_storage,
                                         &json_error) != LONEJSON_STATUS_OK) {
    CAI_LJ_STREAM->json_value_cleanup(CAI_LJ_STREAM, &event_doc->item);
    lonejson_spooled_cleanup(&event_doc->item_storage);
    lonejson_cleanup(&cai_stream_output_item_event_map, event_doc);
    return CAI_ERR_NOMEM;
  }
  reader.cursor = *value;
  lonejson_error_init(&json_error);
  if (lonejson_spooled_rewind(&reader.cursor, &json_error) !=
      LONEJSON_STATUS_OK) {
    CAI_LJ_STREAM->json_value_cleanup(CAI_LJ_STREAM, &event_doc->item);
    lonejson_spooled_cleanup(&event_doc->item_storage);
    lonejson_cleanup(&cai_stream_output_item_event_map, event_doc);
    return CAI_ERR_PROTOCOL;
  }
  lonejson_error_init(&json_error);
  status = CAI_LJ_STREAM->parse_reader(CAI_LJ_STREAM,
                                       &cai_stream_output_item_event_map,
                                       event_doc, cai_stream_spooled_read,
                                       &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ_STREAM->json_value_cleanup(CAI_LJ_STREAM, &event_doc->item);
    lonejson_spooled_cleanup(&event_doc->item_storage);
    lonejson_cleanup(&cai_stream_output_item_event_map, event_doc);
    return CAI_ERR_PROTOCOL;
  }
  return CAI_OK;
}

static void cai_stream_output_item_event_cleanup(
    cai_stream_output_item_event_doc *event_doc) {
  CAI_LJ_STREAM->json_value_cleanup(CAI_LJ_STREAM, &event_doc->item);
  lonejson_spooled_cleanup(&event_doc->item_storage);
  lonejson_cleanup(&cai_stream_output_item_event_map, event_doc);
}

static int cai_stream_parse_response_event(
    const lonejson_spooled *value, cai_stream_response_event_doc *event_doc) {
  cai_stream_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;

  memset(event_doc, 0, sizeof(*event_doc));
  lonejson_init(CAI_LJ_STREAM, &cai_stream_response_event_map, event_doc);
  lonejson_spooled_init(CAI_LJ_STREAM, &event_doc->response_storage);
  CAI_LJ_STREAM->json_value_init(CAI_LJ_STREAM, &event_doc->response);
  lonejson_error_init(&json_error);
  if (lonejson_json_value_set_parse_sink(&event_doc->response,
                                         cai_stream_discard_json_sink,
                                         &event_doc->response_storage,
                                         &json_error) != LONEJSON_STATUS_OK) {
    CAI_LJ_STREAM->json_value_cleanup(CAI_LJ_STREAM, &event_doc->response);
    lonejson_spooled_cleanup(&event_doc->response_storage);
    lonejson_cleanup(&cai_stream_response_event_map, event_doc);
    return CAI_ERR_NOMEM;
  }
  reader.cursor = *value;
  lonejson_error_init(&json_error);
  if (lonejson_spooled_rewind(&reader.cursor, &json_error) !=
      LONEJSON_STATUS_OK) {
    CAI_LJ_STREAM->json_value_cleanup(CAI_LJ_STREAM, &event_doc->response);
    lonejson_spooled_cleanup(&event_doc->response_storage);
    lonejson_cleanup(&cai_stream_response_event_map, event_doc);
    return CAI_ERR_PROTOCOL;
  }
  lonejson_error_init(&json_error);
  status = CAI_LJ_STREAM->parse_reader(CAI_LJ_STREAM,
                                       &cai_stream_response_event_map,
                                       event_doc, cai_stream_spooled_read,
                                       &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ_STREAM->json_value_cleanup(CAI_LJ_STREAM, &event_doc->response);
    lonejson_spooled_cleanup(&event_doc->response_storage);
    lonejson_cleanup(&cai_stream_response_event_map, event_doc);
    return CAI_ERR_PROTOCOL;
  }
  return CAI_OK;
}

static void cai_stream_response_event_cleanup(
    cai_stream_response_event_doc *event_doc) {
  CAI_LJ_STREAM->json_value_cleanup(CAI_LJ_STREAM, &event_doc->response);
  lonejson_spooled_cleanup(&event_doc->response_storage);
  lonejson_cleanup(&cai_stream_response_event_map, event_doc);
}

static int cai_stream_parse_output_item_spooled(
    const lonejson_spooled *value, cai_stream_output_item_doc *item) {
  memset(item, 0, sizeof(*item));
  lonejson_init(CAI_LJ_STREAM, &cai_stream_output_item_map, item);
  if (cai_stream_parse_spooled(&cai_stream_output_item_map, item, value) !=
      CAI_OK) {
    lonejson_cleanup(&cai_stream_output_item_map, item);
    return CAI_ERR_PROTOCOL;
  }
  return CAI_OK;
}

typedef enum cai_stream_visitor_context {
  CAI_STREAM_VISITOR_CONTEXT_NONE = 0,
  CAI_STREAM_VISITOR_CONTEXT_OBJECT = 1,
  CAI_STREAM_VISITOR_CONTEXT_ARRAY = 2,
  CAI_STREAM_VISITOR_CONTEXT_USAGE = 3,
  CAI_STREAM_VISITOR_CONTEXT_INPUT_DETAILS = 4,
  CAI_STREAM_VISITOR_CONTEXT_OUTPUT_DETAILS = 5
} cai_stream_visitor_context;

typedef struct cai_stream_type_visitor_state {
  int depth;
  char current_key[64];
  size_t current_key_len;
  int capture_key;
  char type[64];
  size_t type_len;
  int capture_type;
  int has_type;
} cai_stream_type_visitor_state;

typedef struct cai_stream_response_meta_state {
  int depth;
  cai_stream_visitor_context context_stack[32];
  char current_key[64];
  size_t current_key_len;
  int capture_key;
  char id[128];
  size_t id_len;
  int capture_id;
  int has_id;
  char number[64];
  size_t number_len;
  long long *current_number_target;
  cai_stream_usage_doc usage;
} cai_stream_response_meta_state;

typedef struct cai_stream_item_meta_state {
  int depth;
  char current_key[64];
  size_t current_key_len;
  int capture_key;
  char id[128];
  size_t id_len;
  int has_id;
  int capture_id;
  char type[64];
  size_t type_len;
  int has_type;
  int capture_type;
} cai_stream_item_meta_state;

static lonejson_status cai_stream_value_ok(void *user, lonejson_error *error) {
  (void)user;
  lonejson_error_init(error);
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_stream_type_key_begin(void *user,
                                                 lonejson_error *error) {
  cai_stream_type_visitor_state *state;

  state = (cai_stream_type_visitor_state *)user;
  state->current_key_len = 0U;
  state->current_key[0] = '\0';
  state->capture_key = 1;
  return cai_stream_value_ok(user, error);
}

static lonejson_status cai_stream_type_key_chunk(void *user, const char *data,
                                                 size_t len,
                                                 lonejson_error *error) {
  cai_stream_type_visitor_state *state;
  size_t copy_len;

  state = (cai_stream_type_visitor_state *)user;
  if (!state->capture_key) {
    return cai_stream_value_ok(user, error);
  }
  copy_len = len;
  if (copy_len > sizeof(state->current_key) - state->current_key_len - 1U) {
    copy_len = sizeof(state->current_key) - state->current_key_len - 1U;
  }
  if (copy_len > 0U) {
    memcpy(state->current_key + state->current_key_len, data, copy_len);
    state->current_key_len += copy_len;
    state->current_key[state->current_key_len] = '\0';
  }
  return cai_stream_value_ok(user, error);
}

static lonejson_status cai_stream_type_key_end(void *user,
                                               lonejson_error *error) {
  cai_stream_type_visitor_state *state;

  state = (cai_stream_type_visitor_state *)user;
  state->capture_key = 0;
  return cai_stream_value_ok(user, error);
}

static lonejson_status cai_stream_type_string_begin(void *user,
                                                    lonejson_error *error) {
  cai_stream_type_visitor_state *state;

  state = (cai_stream_type_visitor_state *)user;
  state->capture_type =
      state->depth == 1 && strcmp(state->current_key, "type") == 0;
  if (state->capture_type) {
    state->type_len = 0U;
    state->type[0] = '\0';
    state->has_type = 0;
  }
  return cai_stream_value_ok(user, error);
}

static lonejson_status cai_stream_type_string_chunk(void *user, const char *data,
                                                    size_t len,
                                                    lonejson_error *error) {
  cai_stream_type_visitor_state *state;
  size_t copy_len;

  state = (cai_stream_type_visitor_state *)user;
  if (!state->capture_type) {
    return cai_stream_value_ok(user, error);
  }
  copy_len = len;
  if (copy_len > sizeof(state->type) - state->type_len - 1U) {
    copy_len = sizeof(state->type) - state->type_len - 1U;
  }
  if (copy_len > 0U) {
    memcpy(state->type + state->type_len, data, copy_len);
    state->type_len += copy_len;
    state->type[state->type_len] = '\0';
  }
  return cai_stream_value_ok(user, error);
}

static lonejson_status cai_stream_type_string_end(void *user,
                                                  lonejson_error *error) {
  cai_stream_type_visitor_state *state;

  state = (cai_stream_type_visitor_state *)user;
  if (state->capture_type) {
    state->capture_type = 0;
    state->has_type = 1;
  }
  return cai_stream_value_ok(user, error);
}

static lonejson_status cai_stream_type_object_begin(void *user,
                                                    lonejson_error *error) {
  cai_stream_type_visitor_state *state;

  state = (cai_stream_type_visitor_state *)user;
  state->depth++;
  return cai_stream_value_ok(user, error);
}

static lonejson_status cai_stream_type_object_end(void *user,
                                                  lonejson_error *error) {
  cai_stream_type_visitor_state *state;

  state = (cai_stream_type_visitor_state *)user;
  if (state->depth > 0) {
    state->depth--;
  }
  return cai_stream_value_ok(user, error);
}

static int cai_stream_extract_event_type(const lonejson_spooled *json,
                                         char *type_out,
                                         size_t type_out_size) {
  cai_stream_spooled_reader reader;
  cai_stream_type_visitor_state state;
  lonejson_value_visitor visitor;
  lonejson_error json_error;
  lonejson_status status;

  memset(&state, 0, sizeof(state));
  visitor = lonejson_default_value_visitor();
  visitor.object_begin = cai_stream_type_object_begin;
  visitor.object_end = cai_stream_type_object_end;
  visitor.object_key_begin = cai_stream_type_key_begin;
  visitor.object_key_chunk = cai_stream_type_key_chunk;
  visitor.object_key_end = cai_stream_type_key_end;
  visitor.string_begin = cai_stream_type_string_begin;
  visitor.string_chunk = cai_stream_type_string_chunk;
  visitor.string_end = cai_stream_type_string_end;
  reader.cursor = *json;
  lonejson_error_init(&json_error);
  if (lonejson_spooled_rewind(&reader.cursor, &json_error) !=
      LONEJSON_STATUS_OK) {
    return CAI_ERR_PROTOCOL;
  }
  lonejson_error_init(&json_error);
  status = lonejson_visit_value_reader(CAI_LJ_STREAM, cai_stream_spooled_read,
                                       &reader, &visitor, &state,
                                       &json_error);
  if (status != LONEJSON_STATUS_OK || !state.has_type) {
    return CAI_ERR_PROTOCOL;
  }
  snprintf(type_out, type_out_size, "%s", state.type);
  return CAI_OK;
}

static lonejson_status cai_stream_response_key_begin(void *user,
                                                     lonejson_error *error) {
  cai_stream_response_meta_state *state;

  state = (cai_stream_response_meta_state *)user;
  state->current_key_len = 0U;
  state->current_key[0] = '\0';
  state->capture_key = 1;
  return cai_stream_value_ok(user, error);
}

static lonejson_status cai_stream_response_key_chunk(void *user,
                                                     const char *data,
                                                     size_t len,
                                                     lonejson_error *error) {
  cai_stream_response_meta_state *state;
  size_t copy_len;

  state = (cai_stream_response_meta_state *)user;
  if (!state->capture_key) {
    return cai_stream_value_ok(user, error);
  }
  copy_len = len;
  if (copy_len > sizeof(state->current_key) - state->current_key_len - 1U) {
    copy_len = sizeof(state->current_key) - state->current_key_len - 1U;
  }
  if (copy_len > 0U) {
    memcpy(state->current_key + state->current_key_len, data, copy_len);
    state->current_key_len += copy_len;
    state->current_key[state->current_key_len] = '\0';
  }
  return cai_stream_value_ok(user, error);
}

static lonejson_status cai_stream_response_key_end(void *user,
                                                   lonejson_error *error) {
  cai_stream_response_meta_state *state;

  state = (cai_stream_response_meta_state *)user;
  state->capture_key = 0;
  return cai_stream_value_ok(user, error);
}

static lonejson_status cai_stream_response_object_begin(void *user,
                                                        lonejson_error *error) {
  cai_stream_response_meta_state *state;
  cai_stream_visitor_context parent;
  cai_stream_visitor_context next;

  state = (cai_stream_response_meta_state *)user;
  parent = state->depth > 0 ? state->context_stack[state->depth - 1]
                            : CAI_STREAM_VISITOR_CONTEXT_NONE;
  next = CAI_STREAM_VISITOR_CONTEXT_OBJECT;
  if (state->depth == 0) {
    next = CAI_STREAM_VISITOR_CONTEXT_OBJECT;
  } else if (parent == CAI_STREAM_VISITOR_CONTEXT_OBJECT &&
             strcmp(state->current_key, "usage") == 0) {
    next = CAI_STREAM_VISITOR_CONTEXT_USAGE;
  } else if (parent == CAI_STREAM_VISITOR_CONTEXT_USAGE &&
             strcmp(state->current_key, "input_tokens_details") == 0) {
    next = CAI_STREAM_VISITOR_CONTEXT_INPUT_DETAILS;
  } else if (parent == CAI_STREAM_VISITOR_CONTEXT_USAGE &&
             strcmp(state->current_key, "output_tokens_details") == 0) {
    next = CAI_STREAM_VISITOR_CONTEXT_OUTPUT_DETAILS;
  }
  if ((size_t)state->depth < sizeof(state->context_stack) /
                                sizeof(state->context_stack[0])) {
    state->context_stack[state->depth] = next;
  }
  state->depth++;
  return cai_stream_value_ok(user, error);
}

static lonejson_status cai_stream_response_object_end(void *user,
                                                      lonejson_error *error) {
  cai_stream_response_meta_state *state;

  state = (cai_stream_response_meta_state *)user;
  if (state->depth > 0) {
    state->depth--;
  }
  return cai_stream_value_ok(user, error);
}

static lonejson_status cai_stream_response_array_begin(void *user,
                                                       lonejson_error *error) {
  cai_stream_response_meta_state *state;

  state = (cai_stream_response_meta_state *)user;
  if ((size_t)state->depth < sizeof(state->context_stack) /
                                sizeof(state->context_stack[0])) {
    state->context_stack[state->depth] = CAI_STREAM_VISITOR_CONTEXT_ARRAY;
  }
  state->depth++;
  return cai_stream_value_ok(user, error);
}

static lonejson_status cai_stream_response_array_end(void *user,
                                                     lonejson_error *error) {
  cai_stream_response_meta_state *state;

  state = (cai_stream_response_meta_state *)user;
  if (state->depth > 0) {
    state->depth--;
  }
  return cai_stream_value_ok(user, error);
}

static lonejson_status cai_stream_response_string_begin(void *user,
                                                        lonejson_error *error) {
  cai_stream_response_meta_state *state;
  cai_stream_visitor_context current;

  state = (cai_stream_response_meta_state *)user;
  current = state->depth > 0 ? state->context_stack[state->depth - 1]
                             : CAI_STREAM_VISITOR_CONTEXT_NONE;
  state->capture_id =
      current == CAI_STREAM_VISITOR_CONTEXT_OBJECT &&
      strcmp(state->current_key, "id") == 0;
  if (state->capture_id) {
    state->id_len = 0U;
    state->id[0] = '\0';
    state->has_id = 0;
  }
  return cai_stream_value_ok(user, error);
}

static lonejson_status cai_stream_response_string_chunk(void *user,
                                                        const char *data,
                                                        size_t len,
                                                        lonejson_error *error) {
  cai_stream_response_meta_state *state;
  size_t copy_len;

  state = (cai_stream_response_meta_state *)user;
  if (!state->capture_id) {
    return cai_stream_value_ok(user, error);
  }
  copy_len = len;
  if (copy_len > sizeof(state->id) - state->id_len - 1U) {
    copy_len = sizeof(state->id) - state->id_len - 1U;
  }
  if (copy_len > 0U) {
    memcpy(state->id + state->id_len, data, copy_len);
    state->id_len += copy_len;
    state->id[state->id_len] = '\0';
  }
  return cai_stream_value_ok(user, error);
}

static lonejson_status cai_stream_response_string_end(void *user,
                                                      lonejson_error *error) {
  cai_stream_response_meta_state *state;

  state = (cai_stream_response_meta_state *)user;
  if (state->capture_id) {
    state->capture_id = 0;
    state->has_id = 1;
  }
  return cai_stream_value_ok(user, error);
}

static lonejson_status cai_stream_response_number_begin(void *user,
                                                        lonejson_error *error) {
  cai_stream_response_meta_state *state;
  cai_stream_visitor_context current;

  state = (cai_stream_response_meta_state *)user;
  current = state->depth > 0 ? state->context_stack[state->depth - 1]
                             : CAI_STREAM_VISITOR_CONTEXT_NONE;
  state->current_number_target = NULL;
  if (current == CAI_STREAM_VISITOR_CONTEXT_USAGE) {
    if (strcmp(state->current_key, "input_tokens") == 0) {
      state->current_number_target = &state->usage.input_tokens;
    } else if (strcmp(state->current_key, "output_tokens") == 0) {
      state->current_number_target = &state->usage.output_tokens;
    } else if (strcmp(state->current_key, "total_tokens") == 0) {
      state->current_number_target = &state->usage.total_tokens;
    }
  } else if (current == CAI_STREAM_VISITOR_CONTEXT_INPUT_DETAILS &&
             strcmp(state->current_key, "cached_tokens") == 0) {
    state->current_number_target = &state->usage.input_cached_tokens;
  } else if (current == CAI_STREAM_VISITOR_CONTEXT_OUTPUT_DETAILS &&
             strcmp(state->current_key, "reasoning_tokens") == 0) {
    state->current_number_target = &state->usage.output_reasoning_tokens;
  }
  state->number_len = 0U;
  state->number[0] = '\0';
  return cai_stream_value_ok(user, error);
}

static lonejson_status cai_stream_response_number_chunk(void *user,
                                                        const char *data,
                                                        size_t len,
                                                        lonejson_error *error) {
  cai_stream_response_meta_state *state;
  size_t copy_len;

  state = (cai_stream_response_meta_state *)user;
  if (state->current_number_target == NULL) {
    return cai_stream_value_ok(user, error);
  }
  copy_len = len;
  if (copy_len > sizeof(state->number) - state->number_len - 1U) {
    copy_len = sizeof(state->number) - state->number_len - 1U;
  }
  if (copy_len > 0U) {
    memcpy(state->number + state->number_len, data, copy_len);
    state->number_len += copy_len;
    state->number[state->number_len] = '\0';
  }
  return cai_stream_value_ok(user, error);
}

static lonejson_status cai_stream_response_number_end(void *user,
                                                      lonejson_error *error) {
  cai_stream_response_meta_state *state;

  state = (cai_stream_response_meta_state *)user;
  if (state->current_number_target != NULL) {
    *state->current_number_target = strtoll(state->number, NULL, 10);
    state->current_number_target = NULL;
  }
  return cai_stream_value_ok(user, error);
}

static int cai_stream_parse_response_meta(const lonejson_spooled *json,
                                          cai_stream_response_doc *response) {
  cai_stream_spooled_reader reader;
  cai_stream_response_meta_state state;
  lonejson_value_visitor visitor;
  lonejson_error json_error;
  lonejson_status status;

  memset(&state, 0, sizeof(state));
  memset(response, 0, sizeof(*response));
  visitor = lonejson_default_value_visitor();
  visitor.object_begin = cai_stream_response_object_begin;
  visitor.object_end = cai_stream_response_object_end;
  visitor.object_key_begin = cai_stream_response_key_begin;
  visitor.object_key_chunk = cai_stream_response_key_chunk;
  visitor.object_key_end = cai_stream_response_key_end;
  visitor.array_begin = cai_stream_response_array_begin;
  visitor.array_end = cai_stream_response_array_end;
  visitor.string_begin = cai_stream_response_string_begin;
  visitor.string_chunk = cai_stream_response_string_chunk;
  visitor.string_end = cai_stream_response_string_end;
  visitor.number_begin = cai_stream_response_number_begin;
  visitor.number_chunk = cai_stream_response_number_chunk;
  visitor.number_end = cai_stream_response_number_end;
  reader.cursor = *json;
  lonejson_error_init(&json_error);
  if (lonejson_spooled_rewind(&reader.cursor, &json_error) !=
      LONEJSON_STATUS_OK) {
    return CAI_ERR_PROTOCOL;
  }
  lonejson_error_init(&json_error);
  status = lonejson_visit_value_reader(CAI_LJ_STREAM, cai_stream_spooled_read,
                                       &reader, &visitor, &state,
                                       &json_error);
  if (status != LONEJSON_STATUS_OK) {
    return CAI_ERR_PROTOCOL;
  }
  if (state.has_id) {
    response->id = cai_strdup(NULL, state.id);
    if (response->id == NULL) {
      return CAI_ERR_NOMEM;
    }
  }
  response->usage = state.usage;
  return CAI_OK;
}

static lonejson_status cai_stream_item_key_begin(void *user,
                                                 lonejson_error *error) {
  cai_stream_item_meta_state *state;

  state = (cai_stream_item_meta_state *)user;
  state->current_key_len = 0U;
  state->current_key[0] = '\0';
  state->capture_key = 1;
  return cai_stream_value_ok(user, error);
}

static lonejson_status cai_stream_item_key_chunk(void *user, const char *data,
                                                 size_t len,
                                                 lonejson_error *error) {
  cai_stream_item_meta_state *state;
  size_t copy_len;

  state = (cai_stream_item_meta_state *)user;
  if (!state->capture_key) {
    return cai_stream_value_ok(user, error);
  }
  copy_len = len;
  if (copy_len > sizeof(state->current_key) - state->current_key_len - 1U) {
    copy_len = sizeof(state->current_key) - state->current_key_len - 1U;
  }
  if (copy_len > 0U) {
    memcpy(state->current_key + state->current_key_len, data, copy_len);
    state->current_key_len += copy_len;
    state->current_key[state->current_key_len] = '\0';
  }
  return cai_stream_value_ok(user, error);
}

static lonejson_status cai_stream_item_key_end(void *user,
                                               lonejson_error *error) {
  cai_stream_item_meta_state *state;

  state = (cai_stream_item_meta_state *)user;
  state->capture_key = 0;
  return cai_stream_value_ok(user, error);
}

static lonejson_status cai_stream_item_string_begin(void *user,
                                                    lonejson_error *error) {
  cai_stream_item_meta_state *state;

  state = (cai_stream_item_meta_state *)user;
  state->capture_id =
      state->depth == 1 && strcmp(state->current_key, "id") == 0;
  state->capture_type =
      state->depth == 1 && strcmp(state->current_key, "type") == 0;
  if (state->capture_id) {
    state->id_len = 0U;
    state->id[0] = '\0';
    state->has_id = 0;
  }
  if (state->capture_type) {
    state->type_len = 0U;
    state->type[0] = '\0';
    state->has_type = 0;
  }
  return cai_stream_value_ok(user, error);
}

static lonejson_status cai_stream_item_string_chunk(void *user, const char *data,
                                                    size_t len,
                                                    lonejson_error *error) {
  cai_stream_item_meta_state *state;
  size_t copy_len;

  state = (cai_stream_item_meta_state *)user;
  if (state->capture_id) {
    copy_len = len;
    if (copy_len > sizeof(state->id) - state->id_len - 1U) {
      copy_len = sizeof(state->id) - state->id_len - 1U;
    }
    if (copy_len > 0U) {
      memcpy(state->id + state->id_len, data, copy_len);
      state->id_len += copy_len;
      state->id[state->id_len] = '\0';
    }
  } else if (state->capture_type) {
    copy_len = len;
    if (copy_len > sizeof(state->type) - state->type_len - 1U) {
      copy_len = sizeof(state->type) - state->type_len - 1U;
    }
    if (copy_len > 0U) {
      memcpy(state->type + state->type_len, data, copy_len);
      state->type_len += copy_len;
      state->type[state->type_len] = '\0';
    }
  }
  return cai_stream_value_ok(user, error);
}

static lonejson_status cai_stream_item_string_end(void *user,
                                                  lonejson_error *error) {
  cai_stream_item_meta_state *state;

  state = (cai_stream_item_meta_state *)user;
  if (state->capture_id) {
    state->capture_id = 0;
    state->has_id = 1;
  }
  if (state->capture_type) {
    state->capture_type = 0;
    state->has_type = 1;
  }
  return cai_stream_value_ok(user, error);
}

static lonejson_status cai_stream_item_object_begin(void *user,
                                                    lonejson_error *error) {
  cai_stream_item_meta_state *state;

  state = (cai_stream_item_meta_state *)user;
  state->depth++;
  return cai_stream_value_ok(user, error);
}

static lonejson_status cai_stream_item_object_end(void *user,
                                                  lonejson_error *error) {
  cai_stream_item_meta_state *state;

  state = (cai_stream_item_meta_state *)user;
  if (state->depth > 0) {
    state->depth--;
  }
  return cai_stream_value_ok(user, error);
}

static lonejson_status cai_stream_item_array_begin(void *user,
                                                   lonejson_error *error) {
  cai_stream_item_meta_state *state;

  state = (cai_stream_item_meta_state *)user;
  state->depth++;
  return cai_stream_value_ok(user, error);
}

static lonejson_status cai_stream_item_array_end(void *user,
                                                 lonejson_error *error) {
  cai_stream_item_meta_state *state;

  state = (cai_stream_item_meta_state *)user;
  if (state->depth > 0) {
    state->depth--;
  }
  return cai_stream_value_ok(user, error);
}

static int cai_stream_parse_item_meta(const lonejson_spooled *json,
                                      cai_stream_output_item_doc *item) {
  cai_stream_spooled_reader reader;
  cai_stream_item_meta_state state;
  lonejson_value_visitor visitor;
  lonejson_error json_error;
  lonejson_status status;

  memset(&state, 0, sizeof(state));
  memset(item, 0, sizeof(*item));
  visitor = lonejson_default_value_visitor();
  visitor.object_begin = cai_stream_item_object_begin;
  visitor.object_end = cai_stream_item_object_end;
  visitor.object_key_begin = cai_stream_item_key_begin;
  visitor.object_key_chunk = cai_stream_item_key_chunk;
  visitor.object_key_end = cai_stream_item_key_end;
  visitor.array_begin = cai_stream_item_array_begin;
  visitor.array_end = cai_stream_item_array_end;
  visitor.string_begin = cai_stream_item_string_begin;
  visitor.string_chunk = cai_stream_item_string_chunk;
  visitor.string_end = cai_stream_item_string_end;
  reader.cursor = *json;
  lonejson_error_init(&json_error);
  if (lonejson_spooled_rewind(&reader.cursor, &json_error) !=
      LONEJSON_STATUS_OK) {
    return CAI_ERR_PROTOCOL;
  }
  lonejson_error_init(&json_error);
  status = lonejson_visit_value_reader(CAI_LJ_STREAM, cai_stream_spooled_read,
                                       &reader, &visitor, &state,
                                       &json_error);
  if (status != LONEJSON_STATUS_OK) {
    return CAI_ERR_PROTOCOL;
  }
  if (state.has_id) {
    item->id = cai_strdup(NULL, state.id);
    if (item->id == NULL) {
      return CAI_ERR_NOMEM;
    }
  }
  if (state.has_type) {
    item->type = cai_strdup(NULL, state.type);
    if (item->type == NULL) {
      cai_free_mem(NULL, item->id);
      item->id = NULL;
      return CAI_ERR_NOMEM;
    }
  }
  return CAI_OK;
}

static void cai_stream_item_meta_cleanup(cai_stream_output_item_doc *item) {
  if (item == NULL) {
    return;
  }
  cai_free_mem(NULL, item->id);
  cai_free_mem(NULL, item->type);
  item->id = NULL;
  item->type = NULL;
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

static int cai_stream_spooled_empty(const lonejson_spooled *value) {
  return value == NULL || lonejson_spooled_size(value) == 0U;
}

static lonejson_status cai_stream_discard_json_sink(void *user,
                                                    const void *data,
                                                    size_t len,
                                                    lonejson_error *error) {
  if (lonejson_spooled_append((lonejson_spooled *)user, data, len, error) ==
      LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_OK;
  }
  return LONEJSON_STATUS_CALLBACK_FAILED;
}

static lonejson_status cai_stream_lonejson_sink(void *user, const void *data,
                                                size_t len,
                                                lonejson_error *error) {
  cai_stream_sink_context *context;

  (void)error;
  context = (cai_stream_sink_context *)user;
  if (context == NULL || context->sink == NULL) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (cai_sink_write(context->sink, data, len, context->error) != CAI_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static int cai_stream_write_spooled(cai_sink *sink,
                                    const lonejson_spooled *value,
                                    cai_error *error) {
  lonejson_error json_error;
  cai_stream_sink_context context;
  lonejson_status status;

  if (sink == NULL || cai_stream_spooled_empty(value)) {
    return CAI_OK;
  }
  context.sink = sink;
  context.error = error;
  lonejson_error_init(&json_error);
  status = lonejson_spooled_write_to_sink(value, cai_stream_lonejson_sink,
                                          &context, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    return error != NULL && error->code != CAI_OK
               ? error->code
               : cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                      "failed to write streamed text delta",
                                      json_error.message);
  }
  return CAI_OK;
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
    state->output_text_started = 0;
  }
  return rc;
}

static int cai_sse_write_reasoning_delta(cai_sse_state *state,
                                         const lonejson_spooled *delta) {
  int rc;

  if (state->sinks.reasoning_summary == NULL ||
      cai_stream_spooled_empty(delta)) {
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
  return cai_stream_write_spooled(state->sinks.reasoning_summary, delta, NULL);
}

static int cai_sse_write_output_delta(cai_sse_state *state,
                                      const lonejson_spooled *delta) {
  int rc;

  if (state->sinks.output_text == NULL || cai_stream_spooled_empty(delta)) {
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
  state->output_text_suffixed = 0;
  return cai_stream_write_spooled(state->sinks.output_text, delta, NULL);
}

static int cai_sse_emit_output_text_delta(cai_sse_state *state,
                                          const cai_stream_delta_event_doc *doc) {
  int rc;

  rc = CAI_OK;
  if (state->sinks.output_text_delta != NULL) {
    rc = state->sinks.output_text_delta(
        state->sinks.output_text_context, doc->item_id, (int)doc->output_index,
        &doc->delta, NULL);
  }
  if (rc == CAI_OK) {
    rc = cai_sse_write_output_delta(state, &doc->delta);
  }
  return rc;
}

static int cai_sse_emit_function_call_delta(cai_sse_state *state,
                                            const cai_stream_delta_event_doc *doc) {
  if (state->sinks.function_call_arguments_delta == NULL ||
      cai_stream_spooled_empty(&doc->delta)) {
    return CAI_OK;
  }
  return state->sinks.function_call_arguments_delta(
      state->sinks.function_call_context, doc->item_id,
      (int)doc->output_index, &doc->delta, NULL);
}

static int cai_sse_emit_function_call_done_values(
    cai_sse_state *state, const char *item_id, int output_index,
    const char *call_id, const char *name,
    const lonejson_spooled *arguments) {
  if (state->sinks.function_call_arguments_done == NULL) {
    return CAI_OK;
  }
  return state->sinks.function_call_arguments_done(
      state->sinks.function_call_context, item_id, output_index, call_id, name,
      arguments, NULL);
}

static int cai_sse_emit_function_call_done(cai_sse_state *state,
                                           const cai_stream_function_call_done_event_doc *doc) {
  int rc;

  rc = cai_sse_emit_function_call_done_values(
      state, doc->item_id, (int)doc->output_index, doc->call_id, doc->name,
      &doc->arguments);
  if (rc == CAI_OK) {
    rc = cai_sse_record_call_id(state, doc->call_id);
  }
  return rc;
}

static int cai_sse_emit_output_item_done(
    cai_sse_state *state, const char *item_id, int output_index,
    const char *type, const lonejson_spooled *item_json) {
  if (state->sinks.output_item_done == NULL) {
    return CAI_OK;
  }
  return state->sinks.output_item_done(
      state->sinks.output_item_context, item_id, output_index, type, item_json,
      NULL);
}

static int cai_sse_set_response_id(cai_sse_state *state, const char *id) {
  if (state->out_response_id == NULL || *state->out_response_id != NULL ||
      id == NULL) {
    return CAI_OK;
  }
  *state->out_response_id = cai_strdup(NULL, id);
  if (*state->out_response_id == NULL) {
    return CAI_ERR_NOMEM;
  }
  return CAI_OK;
}

static int cai_sse_event_name_from_body(cai_sse_state *state, char **out) {
  char type[64];

  if (out == NULL) {
    return CAI_ERR_INVALID;
  }
  *out = NULL;
  memset(type, 0, sizeof(type));
  if (cai_stream_extract_event_type(&state->event_json_storage, type,
                                    sizeof(type)) != CAI_OK) {
    return CAI_ERR_PROTOCOL;
  }
  *out = cai_strdup(NULL, type);
  return *out != NULL ? CAI_OK : CAI_ERR_NOMEM;
}

static int cai_sse_emit_event(cai_sse_state *state,
                              const lonejson_sse_event *event) {
  cai_stream_delta_event_doc delta_doc;
  cai_stream_function_call_done_event_doc done_doc;
  cai_stream_output_item_event_doc item_event;
  cai_stream_output_item_doc item_doc;
  cai_stream_output_item_doc function_item_doc;
  cai_stream_response_event_doc response_event;
  cai_stream_response_doc response_doc;
  const char *event_name;
  char *owned_event_name;
  int rc;
  int item_doc_initialized;
  int function_item_doc_initialized;
  int item_event_initialized;
  int completed_initialized;
  int delta_initialized;
  int done_initialized;

  event_name = event != NULL ? event->event : NULL;
  owned_event_name = NULL;
  if (event_name == NULL || event_name[0] == '\0') {
    rc = cai_sse_event_name_from_body(state, &owned_event_name);
    if (rc != CAI_OK) {
      return rc;
    }
    event_name = owned_event_name;
  }
  if (event_name == NULL || event_name[0] == '\0') {
    cai_free_mem(NULL, owned_event_name);
    return CAI_OK;
  }

  memset(&delta_doc, 0, sizeof(delta_doc));
  memset(&done_doc, 0, sizeof(done_doc));
  memset(&item_event, 0, sizeof(item_event));
  memset(&item_doc, 0, sizeof(item_doc));
  memset(&function_item_doc, 0, sizeof(function_item_doc));
  memset(&response_event, 0, sizeof(response_event));
  memset(&response_doc, 0, sizeof(response_doc));
  delta_initialized = 0;
  done_initialized = 0;
  item_event_initialized = 0;
  item_doc_initialized = 0;
  function_item_doc_initialized = 0;
  completed_initialized = 0;
  rc = CAI_OK;

  if (strcmp(event_name, "response.output_text.delta") == 0 ||
      strcmp(event_name, "response.reasoning_summary_text.delta") == 0 ||
      strcmp(event_name, "response.reasoning_summary.delta") == 0 ||
      strcmp(event_name, "response.reasoning_text.delta") == 0 ||
      strcmp(event_name, "response.function_call_arguments.delta") == 0) {
    lonejson_init(CAI_LJ_STREAM, &cai_stream_delta_event_map, &delta_doc);
    delta_initialized = 1;
    rc = cai_stream_parse_spooled(&cai_stream_delta_event_map, &delta_doc,
                                  &state->event_json_storage);
    if (rc != CAI_OK) {
      goto done;
    }
    if (strcmp(event_name, "response.output_text.delta") == 0 &&
        !cai_stream_spooled_empty(&delta_doc.delta)) {
      rc = cai_sse_emit_output_text_delta(state, &delta_doc);
    } else if (strcmp(event_name, "response.function_call_arguments.delta") ==
                   0 &&
               !cai_stream_spooled_empty(&delta_doc.delta)) {
      rc = cai_sse_emit_function_call_delta(state, &delta_doc);
    } else if (!cai_stream_spooled_empty(&delta_doc.delta)) {
      rc = cai_sse_write_reasoning_delta(state, &delta_doc.delta);
    }
    goto done;
  }

  if (strcmp(event_name, "response.reasoning_summary_text.done") == 0 ||
      strcmp(event_name, "response.reasoning_summary.done") == 0 ||
      strcmp(event_name, "response.reasoning_text.done") == 0) {
    rc = cai_sse_finish_reasoning(state);
    goto done;
  }

  if (strcmp(event_name, "response.output_text.done") == 0) {
    rc = cai_sse_finish_output(state);
    goto done;
  }

  if (strcmp(event_name, "response.function_call_arguments.done") == 0) {
    lonejson_init(CAI_LJ_STREAM, &cai_stream_function_call_done_event_map,
                  &done_doc);
    done_initialized = 1;
    rc = cai_stream_parse_spooled(&cai_stream_function_call_done_event_map,
                                  &done_doc, &state->event_json_storage);
    if (rc != CAI_OK) {
      goto done;
    }
    if (done_doc.call_id != NULL && done_doc.name != NULL &&
        !cai_stream_spooled_empty(&done_doc.arguments)) {
      rc = cai_sse_emit_function_call_done(state, &done_doc);
    }
    goto done;
  }

  if (strcmp(event_name, "response.output_item.done") == 0) {
    rc = cai_stream_parse_output_item_event(&state->event_json_storage,
                                            &item_event);
    if (rc != CAI_OK) {
      goto done;
    }
    item_event_initialized = 1;
    rc = cai_stream_parse_item_meta(&item_event.item_storage, &item_doc);
    if (rc != CAI_OK) {
      goto done;
    }
    item_doc_initialized = 1;
    rc = cai_sse_emit_output_item_done(state, item_doc.id,
                                       (int)item_event.output_index,
                                       item_doc.type,
                                       &item_event.item_storage);
    if (rc == CAI_OK && item_doc.type != NULL &&
        strcmp(item_doc.type, "function_call") == 0 &&
        cai_stream_parse_output_item_spooled(&item_event.item_storage,
                                             &function_item_doc) == CAI_OK) {
      function_item_doc_initialized = 1;
    }
    if (rc == CAI_OK && function_item_doc_initialized &&
        function_item_doc.call_id != NULL && function_item_doc.name != NULL &&
        !cai_stream_spooled_empty(&function_item_doc.arguments) &&
        !cai_sse_has_emitted_call_id(state, function_item_doc.call_id)) {
      rc = cai_sse_emit_function_call_done_values(
          state, function_item_doc.id, (int)item_event.output_index,
          function_item_doc.call_id, function_item_doc.name,
          &function_item_doc.arguments);
      if (rc == CAI_OK) {
        rc = cai_sse_record_call_id(state, function_item_doc.call_id);
      }
    }
    goto done;
  }

  if (strcmp(event_name, "response.created") == 0 ||
      strcmp(event_name, "response.in_progress") == 0) {
    rc = cai_stream_parse_response_event(&state->event_json_storage,
                                         &response_event);
    if (rc != CAI_OK) {
      goto done;
    }
    completed_initialized = 1;
    rc = cai_stream_parse_response_meta(&response_event.response_storage,
                                        &response_doc);
    if (rc != CAI_OK) {
      goto done;
    }
    rc = cai_sse_set_response_id(state, response_doc.id);
    goto done;
  }

  if (strcmp(event_name, "response.completed") == 0) {
    rc = cai_stream_parse_response_event(&state->event_json_storage,
                                         &response_event);
    if (rc != CAI_OK) {
      goto done;
    }
    completed_initialized = 1;
    rc = cai_stream_parse_response_meta(&response_event.response_storage,
                                        &response_doc);
    if (rc != CAI_OK) {
      goto done;
    }
    rc = cai_sse_finish_reasoning(state);
    if (rc == CAI_OK) {
      rc = cai_sse_finish_output(state);
    }
    if (rc == CAI_OK) {
      rc = cai_sse_set_response_id(state, response_doc.id);
    }
    if (rc == CAI_OK && state->out_usage != NULL) {
      cai_stream_copy_usage(state->out_usage, &response_doc.usage);
    }
  }

done:
  cai_free_mem(NULL, response_doc.id);
  cai_free_mem(NULL, owned_event_name);
  if (completed_initialized) {
    cai_stream_response_event_cleanup(&response_event);
  }
  if (item_doc_initialized) {
    cai_stream_item_meta_cleanup(&item_doc);
  }
  if (function_item_doc_initialized) {
    lonejson_cleanup(&cai_stream_output_item_map, &function_item_doc);
  }
  if (item_event_initialized) {
    cai_stream_output_item_event_cleanup(&item_event);
  }
  if (done_initialized) {
    lonejson_cleanup(&cai_stream_function_call_done_event_map, &done_doc);
  }
  if (delta_initialized) {
    lonejson_cleanup(&cai_stream_delta_event_map, &delta_doc);
  }
  return rc;
}

static lonejson_status cai_sse_json_value_event(
    void *user, const lonejson_sse_event *event, lonejson_json_value *value,
    lonejson_error *error) {
  cai_sse_state *state;
  int rc;

  (void)error;
  (void)value;
  state = (cai_sse_state *)user;
  rc = cai_sse_emit_event(state, event);
  lonejson_spooled_reset(&state->event_json_storage);
  if (rc == CAI_ERR_NOMEM) {
    state->failed = 1;
    state->failed_code = CAI_ERR_NOMEM;
    state->failed_message = "failed to allocate streaming SSE buffer";
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  if (rc != CAI_OK) {
    state->failed = 1;
    state->failed_code = rc;
    if (rc == CAI_ERR_TRANSPORT) {
      state->failed_message = "streaming response transport callback failed";
    } else if (rc == CAI_ERR_PROTOCOL) {
      state->failed_message = "streaming response protocol callback failed";
    } else if (rc == CAI_ERR_INVALID) {
      state->failed_message = "streaming response invalid callback failed";
    } else {
      state->failed_message = "streaming response callback failed";
    }
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
  status = lonejson_sse_push_json_value(
      CAI_LJ_STREAM, state->sse, &state->event_json, bytes, total,
      &state->json_options,
      cai_sse_json_value_event, state, &sse_error);
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
  int http_success;
  lonejson_sse_options sse_options;
  lonejson_error sse_error;
  lonejson_status sse_status;

  if (client == NULL || params == NULL || sinks == NULL ||
      (sinks->output_text == NULL && sinks->reasoning_summary == NULL &&
       sinks->output_text_delta == NULL &&
       sinks->output_item_done == NULL &&
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
  memset(&state, 0, sizeof(state));
  sse_options = lonejson_default_sse_options();
  sse_options.max_line_bytes = CAI_DEFAULT_SSE_EVENT_LIMIT;
  sse_options.max_event_data_bytes = CAI_DEFAULT_SSE_EVENT_LIMIT;
  sse_options.max_buffered_bytes = CAI_DEFAULT_SSE_EVENT_LIMIT;
  memset(&sse_error, 0, sizeof(sse_error));
  state.sinks = *sinks;
  state.out_response_id = out_response_id;
  state.out_usage = out_usage;
  state.sse = lonejson_sse_open(&sse_options, &sse_error);
  if (cai_stream_event_json_init(&state, &sse_error) != LONEJSON_STATUS_OK) {
    if (state.sse != NULL) {
      lonejson_sse_close(state.sse);
    }
    cai_sse_cleanup_call_ids(&state);
    return cai_set_error(error, cai_sse_status_to_code(sse_error.code),
                         cai_sse_status_to_message(sse_error.code));
  }
  memset(&state.json_options, 0, sizeof(state.json_options));
  state.json_options.event_names = cai_stream_json_event_names;
  state.json_options.event_name_count =
      sizeof(cai_stream_json_event_names) /
      sizeof(cai_stream_json_event_names[0]);
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
  http_success = 0;
  if (state.sse == NULL) {
    rc = cai_set_error(error, cai_sse_status_to_code(sse_error.code),
                       cai_sse_status_to_message(sse_error.code));
    cai_stream_event_json_cleanup(&state);
    cai_sse_cleanup_call_ids(&state);
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
    cai_stream_event_json_cleanup(&state);
    cai_sse_cleanup_call_ids(&state);
    return rc;
  }
  curl = curl_easy_init();
  if (curl == NULL) {
    curl_slist_free_all(headers);
    cai_free_mem(&CAI_CLIENT_IMPL(client)->allocator, url);
    cai_response_request_upload_close(upload);
    lonejson_sse_close(state.sse);
    cai_stream_event_json_cleanup(&state);
    cai_sse_cleanup_call_ids(&state);
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
  http_success =
      curl_rc == CURLE_OK && http_status >= 200L && http_status < 300L;
  if (http_success && !state.failed) {
    rc = cai_sse_flush_done_line(&state);
    if (rc != CAI_OK) {
      state.failed = 1;
      state.failed_code = rc;
      state.failed_message = "streaming response callback failed";
    }
  }
  if (http_success && !state.failed && !state.done_seen) {
    memset(&sse_error, 0, sizeof(sse_error));
    sse_status = lonejson_sse_finish_json_value(
        CAI_LJ_STREAM, state.sse, &state.event_json, &state.json_options,
        cai_sse_json_value_event, &state, &sse_error);
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
  cai_stream_event_json_cleanup(&state);
  cai_sse_cleanup_call_ids(&state);
  cai_response_request_upload_close(upload);
  if (rc != CAI_OK) {
    cai_free_mem(NULL, state.body);
    return rc;
  }
  if (curl_rc != CURLE_OK) {
    if (http_status > 0L && (http_status < 200L || http_status >= 300L)) {
      rc = cai_set_openai_error(error, http_status,
                                state.body != NULL ? state.body : "", NULL);
      cai_free_mem(NULL, state.body);
      return rc;
    }
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
  if (!http_success) {
    rc = cai_set_openai_error(error, http_status,
                              state.body != NULL ? state.body : "", NULL);
    cai_free_mem(NULL, state.body);
    return rc;
  }
  if (state.failed) {
    cai_free_mem(NULL, state.body);
    cai_log_http_transport_error(CAI_CLIENT_IMPL(client), "POST", "responses",
                                 state.failed_message);
    return cai_set_error(error, state.failed_code, state.failed_message);
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
#ifdef MSG_NOSIGNAL
    written = send(fd, data + offset, count - offset, MSG_NOSIGNAL);
#else
    written = write(fd, data + offset, count - offset);
#endif
    if (written < 0 && errno == EINTR) {
      continue;
    }
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
  rc = cai_sink_from_callbacks(&callbacks, &sink, &error);
  if (rc == CAI_OK) {
    rc = cai_client_stream_response_text_with_id(
        stream->client, stream->params, sink, &stream->response_id,
        &stream->usage, &error);
    if (rc == CAI_OK) {
      stream->has_usage = 1;
    }
  }
  cai_pipe_stream_finish(stream, rc, &error);
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

  stream = (cai_pipe_stream *)context;
  if (stream == NULL || stream->read_fd < 0) {
    return 0U;
  }
  if (count == 0U) {
    return 0U;
  }
  do {
    got = read(stream->read_fd, buffer, count);
  } while (got < 0 && errno == EINTR);
  if (got < 0) {
    (void)cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                               "failed to read response text stream",
                               strerror(errno));
    return 0U;
  }
  if (got == 0) {
    (void)cai_pipe_stream_take_error(stream, error);
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
  if (stream->worker_error_initialized) {
    cai_error_cleanup(&stream->worker_error);
  }
  if (stream->lock_initialized) {
    pthread_mutex_destroy(&stream->lock);
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
#ifdef AF_UNIX
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
#else
  if (pipe(fds) != 0) {
#endif
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to create streaming pipe");
  }
#ifdef SO_NOSIGPIPE
  {
    int one;

    one = 1;
    (void)setsockopt(fds[1], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
  }
#endif
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
  stream->lock_initialized = 0;
  cai_error_init(&stream->worker_error);
  stream->worker_error_initialized = 1;
  stream->worker_done = 0;
  stream->worker_rc = CAI_OK;
  stream->worker_error_reported = 0;
  if (pthread_mutex_init(&stream->lock, NULL) != 0) {
    cai_pipe_source_close(stream);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to initialize streaming source lock");
  }
  stream->lock_initialized = 1;
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
