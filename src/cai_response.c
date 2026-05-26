#include "cai_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CAI_INLINE_TOOL_ARGUMENTS_LIMIT = 64 * 1024 };
enum { CAI_INPUT_MESSAGE = 0, CAI_INPUT_FUNCTION_CALL_OUTPUT = 1 };

typedef struct cai_response_content_doc {
  char *type;
  lonejson_spooled text;
  lonejson_spooled refusal;
} cai_response_content_doc;

typedef struct cai_response_output_doc {
  char *id;
  char *type;
  char *status;
  char *role;
  char *call_id;
  char *name;
  lonejson_spooled arguments;
  char *created_by;
  lonejson_spooled encrypted_content;
  lonejson_object_array summary;
  lonejson_object_array content;
} cai_response_output_doc;

typedef struct cai_response_input_tokens_details_doc {
  long long cached_tokens;
} cai_response_input_tokens_details_doc;

typedef struct cai_response_output_tokens_details_doc {
  long long reasoning_tokens;
} cai_response_output_tokens_details_doc;

typedef struct cai_response_usage_doc {
  long long input_tokens;
  long long input_cached_tokens;
  long long output_tokens;
  long long output_reasoning_tokens;
  long long total_tokens;
  cai_response_input_tokens_details_doc input_tokens_details;
  cai_response_output_tokens_details_doc output_tokens_details;
} cai_response_usage_doc;

typedef struct cai_response_error_doc {
  char *code;
  char *message;
} cai_response_error_doc;

typedef struct cai_response_incomplete_doc {
  char *reason;
} cai_response_incomplete_doc;

typedef struct cai_response_conversation_doc {
  char *id;
} cai_response_conversation_doc;

typedef struct cai_response_doc {
  char *id;
  char *object;
  char *status;
  char *model;
  lonejson_spooled instructions;
  lonejson_spooled output_text;
  long long created_at;
  cai_response_error_doc error;
  cai_response_incomplete_doc incomplete_details;
  cai_response_conversation_doc conversation;
  cai_response_usage_doc usage;
  lonejson_object_array output;
} cai_response_doc;

typedef struct cai_response_request_reasoning_doc {
  const char *effort;
  const char *summary;
} cai_response_request_reasoning_doc;

typedef struct cai_response_request_text_format_doc {
  const char *type;
  const char *name;
  const char *description;
  lonejson_json_value schema;
  bool strict;
  int has_strict;
} cai_response_request_text_format_doc;

typedef struct cai_response_request_text_doc {
  cai_response_request_text_format_doc format;
  const char *verbosity;
} cai_response_request_text_doc;

typedef struct cai_response_request_context_doc {
  const char *type;
  long long compact_threshold;
} cai_response_request_context_doc;

typedef struct cai_response_spooled_reader_context {
  lonejson_spooled cursor;
} cai_response_spooled_reader_context;

typedef struct cai_request_content_part_doc {
  const char *type;
  lonejson_json_value text;
  lonejson_spooled text_json;
  cai_response_spooled_reader_context text_reader;
  int has_text_json;
  const char *image_url;
  const char *file_id;
  const char *filename;
  lonejson_json_value file_data;
  lonejson_spooled file_data_json;
  cai_response_spooled_reader_context file_data_reader;
  int has_file_data_json;
  const char *file_url;
  const char *detail;
} cai_request_content_part_doc;

typedef struct cai_request_input_item_doc {
  const char *type;
  const char *role;
  lonejson_object_array content;
  const char *call_id;
  lonejson_json_value output;
  lonejson_spooled output_json;
  cai_response_spooled_reader_context output_reader;
  int has_output_json;
} cai_request_input_item_doc;

typedef struct cai_history_output_doc {
  const char *type;
  const char *id;
  const char *status;
  const char *role;
  const char *call_id;
  const char *name;
  lonejson_spooled arguments;
  const char *created_by;
  lonejson_spooled encrypted_content;
  lonejson_json_value summary;
  lonejson_spooled summary_json;
  cai_response_spooled_reader_context summary_reader;
  int has_summary_json;
  lonejson_object_array content;
} cai_history_output_doc;

typedef struct cai_response_request_doc {
  const char *model;
  const char *instructions;
  const char *previous_response_id;
  const char *conversation;
  const char *prompt_cache_key;
  const char *service_tier;
  const char *truncation;
  lonejson_json_value metadata;
  lonejson_json_value include;
  lonejson_json_value prompt;
  lonejson_json_value tool_choice;
  long long max_output_tokens;
  int has_max_output_tokens;
  long long max_tool_calls;
  int has_max_tool_calls;
  bool background;
  int has_background;
  bool store;
  int has_store;
  cai_response_request_reasoning_doc reasoning;
  cai_response_request_text_doc text;
  bool parallel_tool_calls;
  int has_parallel_tool_calls;
  lonejson_object_array context_management;
  lonejson_json_value input;
  lonejson_json_value tools;
  bool stream;
  int has_stream;
} cai_response_request_doc;

typedef struct cai_response_request_write_context {
  lonejson_sink_fn sink;
  void *sink_user;
  lonejson_error *sink_error;
  size_t length;
} cai_response_request_write_context;

typedef struct cai_response_buffer_sink_context {
  cai_buffer_builder *builder;
  cai_error *error;
} cai_response_buffer_sink_context;

typedef struct cai_response_request_state {
  cai_response_request_doc doc;
  cai_response_request_context_doc context_item;
  cai_response_spooled_reader_context input_reader;
  lonejson_spooled input_json;
  int has_input_json;
  cai_response_spooled_reader_context tools_reader;
  lonejson_spooled tools_json;
  int has_tools_json;
} cai_response_request_state;

typedef struct cai_response_json_root_check {
  int depth;
  int root_seen;
  int root_is_object;
  int root_is_array;
  int root_is_string;
} cai_response_json_root_check;

struct cai_response_request_upload {
  cai_response_request_state state;
  lonejson_curl_upload curl;
  curl_off_t size;
  int curl_started;
};

static lonejson_status
cai_response_buffer_sink(void *user, const void *data, size_t len,
                               lonejson_error *error);
static int cai_spool_mapped_object_array(const lonejson_map *map,
                                         const void *items, size_t count,
                                         size_t elem_size,
                                         lonejson_spooled *out,
                                         size_t *out_len,
                                         const char *message,
                                         cai_error *error);
static int cai_response_spooled_clone(const lonejson_spooled *src,
                                      lonejson_spooled *dst,
                                      cai_error *error);

static const lonejson_field cai_response_content_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_response_content_doc, type, "type"),
    LONEJSON_FIELD_STRING_STREAM(cai_response_content_doc, text, "text"),
    LONEJSON_FIELD_STRING_STREAM(cai_response_content_doc, refusal, "refusal")};
LONEJSON_MAP_DEFINE(cai_response_content_map, cai_response_content_doc,
                    cai_response_content_fields);

static const lonejson_field cai_response_output_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_response_output_doc, id, "id"),
    LONEJSON_FIELD_STRING_ALLOC(cai_response_output_doc, type, "type"),
    LONEJSON_FIELD_STRING_ALLOC(cai_response_output_doc, status, "status"),
    LONEJSON_FIELD_STRING_ALLOC(cai_response_output_doc, role, "role"),
    LONEJSON_FIELD_STRING_ALLOC(cai_response_output_doc, call_id, "call_id"),
    LONEJSON_FIELD_STRING_ALLOC(cai_response_output_doc, name, "name"),
    LONEJSON_FIELD_STRING_STREAM_OMIT_EMPTY(cai_response_output_doc,
                                            arguments, "arguments"),
    LONEJSON_FIELD_STRING_ALLOC(cai_response_output_doc, created_by,
                                "created_by"),
    LONEJSON_FIELD_STRING_STREAM(cai_response_output_doc, encrypted_content,
                                 "encrypted_content"),
    LONEJSON_FIELD_OBJECT_ARRAY(cai_response_output_doc, summary, "summary",
                                cai_response_content_doc,
                                &cai_response_content_map,
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_OBJECT_ARRAY(
        cai_response_output_doc, content, "content", cai_response_content_doc,
        &cai_response_content_map, LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(cai_response_output_map, cai_response_output_doc,
                    cai_response_output_fields);

static const lonejson_field cai_history_content_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_response_content_doc, type,
                                          "type"),
    LONEJSON_FIELD_STRING_STREAM_OMIT_EMPTY(cai_response_content_doc, text,
                                            "text"),
    LONEJSON_FIELD_STRING_STREAM_OMIT_EMPTY(cai_response_content_doc, refusal,
                                            "refusal")};
LONEJSON_MAP_DEFINE(cai_history_content_map, cai_response_content_doc,
                    cai_history_content_fields);

static const lonejson_field cai_history_output_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_history_output_doc, type,
                                          "type"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_history_output_doc, id, "id"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_history_output_doc, status,
                                          "status"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_history_output_doc, role,
                                          "role"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_history_output_doc, call_id,
                                          "call_id"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_history_output_doc, name,
                                          "name"),
    LONEJSON_FIELD_STRING_STREAM_OMIT_EMPTY(cai_history_output_doc, arguments,
                                            "arguments"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_history_output_doc, created_by,
                                          "created_by"),
    LONEJSON_FIELD_STRING_STREAM_OMIT_EMPTY(
        cai_history_output_doc, encrypted_content, "encrypted_content"),
    LONEJSON_FIELD_JSON_VALUE_OMIT_NULL(cai_history_output_doc, summary,
                                        "summary"),
    LONEJSON_FIELD_OBJECT_ARRAY_OMIT_EMPTY(
        cai_history_output_doc, content, "content", cai_response_content_doc,
        &cai_history_content_map, LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(cai_history_output_map, cai_history_output_doc,
                    cai_history_output_fields);

static const lonejson_field cai_response_input_tokens_details_fields[] = {
    LONEJSON_FIELD_I64(cai_response_input_tokens_details_doc, cached_tokens,
                       "cached_tokens")};
LONEJSON_MAP_DEFINE(cai_response_input_tokens_details_map,
                    cai_response_input_tokens_details_doc,
                    cai_response_input_tokens_details_fields);

static const lonejson_field cai_response_output_tokens_details_fields[] = {
    LONEJSON_FIELD_I64(cai_response_output_tokens_details_doc, reasoning_tokens,
                       "reasoning_tokens")};
LONEJSON_MAP_DEFINE(cai_response_output_tokens_details_map,
                    cai_response_output_tokens_details_doc,
                    cai_response_output_tokens_details_fields);

static const lonejson_field cai_response_usage_fields[] = {
    LONEJSON_FIELD_I64(cai_response_usage_doc, input_tokens, "input_tokens"),
    LONEJSON_FIELD_I64(cai_response_usage_doc, input_cached_tokens,
                       "input_cached_tokens"),
    LONEJSON_FIELD_OBJECT(cai_response_usage_doc, input_tokens_details,
                          "input_tokens_details",
                          &cai_response_input_tokens_details_map),
    LONEJSON_FIELD_I64(cai_response_usage_doc, output_tokens, "output_tokens"),
    LONEJSON_FIELD_I64(cai_response_usage_doc, output_reasoning_tokens,
                       "output_reasoning_tokens"),
    LONEJSON_FIELD_OBJECT(cai_response_usage_doc, output_tokens_details,
                          "output_tokens_details",
                          &cai_response_output_tokens_details_map),
    LONEJSON_FIELD_I64(cai_response_usage_doc, total_tokens, "total_tokens")};
LONEJSON_MAP_DEFINE(cai_response_usage_map, cai_response_usage_doc,
                    cai_response_usage_fields);

static const lonejson_field cai_response_error_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_response_error_doc, code, "code"),
    LONEJSON_FIELD_STRING_ALLOC(cai_response_error_doc, message, "message")};
LONEJSON_MAP_DEFINE(cai_response_error_map, cai_response_error_doc,
                    cai_response_error_fields);

static const lonejson_field cai_response_incomplete_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_response_incomplete_doc, reason, "reason")};
LONEJSON_MAP_DEFINE(cai_response_incomplete_map, cai_response_incomplete_doc,
                    cai_response_incomplete_fields);

static const lonejson_field cai_response_conversation_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_response_conversation_doc, id, "id")};
LONEJSON_MAP_DEFINE(cai_response_conversation_map,
                    cai_response_conversation_doc,
                    cai_response_conversation_fields);

static const lonejson_field cai_response_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_response_doc, id, "id"),
    LONEJSON_FIELD_STRING_ALLOC(cai_response_doc, object, "object"),
    LONEJSON_FIELD_STRING_ALLOC(cai_response_doc, status, "status"),
    LONEJSON_FIELD_STRING_ALLOC(cai_response_doc, model, "model"),
    LONEJSON_FIELD_STRING_STREAM_OMIT_EMPTY(cai_response_doc, instructions,
                                            "instructions"),
    LONEJSON_FIELD_STRING_STREAM_OMIT_EMPTY(cai_response_doc, output_text,
                                            "output_text"),
    LONEJSON_FIELD_I64(cai_response_doc, created_at, "created_at"),
    LONEJSON_FIELD_OBJECT(cai_response_doc, error, "error",
                          &cai_response_error_map),
    LONEJSON_FIELD_OBJECT(cai_response_doc, incomplete_details,
                          "incomplete_details", &cai_response_incomplete_map),
    LONEJSON_FIELD_OBJECT(cai_response_doc, conversation, "conversation",
                          &cai_response_conversation_map),
    LONEJSON_FIELD_OBJECT(cai_response_doc, usage, "usage",
                          &cai_response_usage_map),
    LONEJSON_FIELD_OBJECT_ARRAY(
        cai_response_doc, output, "output", cai_response_output_doc,
        &cai_response_output_map, LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(cai_response_map, cai_response_doc, cai_response_fields);

static const lonejson_field cai_response_request_reasoning_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_response_request_reasoning_doc,
                                          effort, "effort"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_response_request_reasoning_doc,
                                          summary, "summary")};
LONEJSON_MAP_DEFINE(cai_response_request_reasoning_map,
                    cai_response_request_reasoning_doc,
                    cai_response_request_reasoning_fields);

static const lonejson_field cai_response_request_text_format_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_response_request_text_format_doc,
                                          type, "type"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_response_request_text_format_doc,
                                          name, "name"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_response_request_text_format_doc,
                                          description, "description"),
    LONEJSON_FIELD_JSON_VALUE_OMIT_NULL(cai_response_request_text_format_doc,
                                        schema, "schema"),
    LONEJSON_FIELD_BOOL_PRESENT(cai_response_request_text_format_doc, strict,
                                has_strict, "strict")};
LONEJSON_MAP_DEFINE(cai_response_request_text_format_map,
                    cai_response_request_text_format_doc,
                    cai_response_request_text_format_fields);

static const lonejson_field cai_response_request_text_fields[] = {
    LONEJSON_FIELD_OBJECT_OMIT_EMPTY(cai_response_request_text_doc, format,
                                     "format",
                                     &cai_response_request_text_format_map),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_response_request_text_doc,
                                          verbosity, "verbosity")};
LONEJSON_MAP_DEFINE(cai_response_request_text_map,
                    cai_response_request_text_doc,
                    cai_response_request_text_fields);

static const lonejson_field cai_response_request_context_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_response_request_context_doc, type,
                                    "type"),
    LONEJSON_FIELD_I64_REQ(cai_response_request_context_doc, compact_threshold,
                           "compact_threshold")};
LONEJSON_MAP_DEFINE(cai_response_request_context_map,
                    cai_response_request_context_doc,
                    cai_response_request_context_fields);

static const lonejson_field cai_request_content_part_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_request_content_part_doc, type, "type"),
    LONEJSON_FIELD_JSON_VALUE_OMIT_NULL(cai_request_content_part_doc, text,
                                        "text"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_request_content_part_doc,
                                          image_url, "image_url"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_request_content_part_doc,
                                          file_id, "file_id"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_request_content_part_doc,
                                          filename, "filename"),
    LONEJSON_FIELD_JSON_VALUE_OMIT_NULL(cai_request_content_part_doc,
                                        file_data, "file_data"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_request_content_part_doc,
                                          file_url, "file_url"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_request_content_part_doc, detail,
                                          "detail")};
LONEJSON_MAP_DEFINE(cai_request_content_part_map, cai_request_content_part_doc,
                    cai_request_content_part_fields);

static const lonejson_field cai_request_input_item_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_request_input_item_doc, type,
                                          "type"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_request_input_item_doc, role,
                                          "role"),
    LONEJSON_FIELD_OBJECT_ARRAY_OMIT_EMPTY(
        cai_request_input_item_doc, content, "content",
        cai_request_content_part_doc, &cai_request_content_part_map,
        LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_request_input_item_doc, call_id,
                                          "call_id"),
    LONEJSON_FIELD_JSON_VALUE_OMIT_NULL(cai_request_input_item_doc, output,
                                        "output")};
LONEJSON_MAP_DEFINE(cai_request_input_item_map, cai_request_input_item_doc,
                    cai_request_input_item_fields);

static const lonejson_field cai_response_request_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_response_request_doc, model, "model"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_response_request_doc,
                                          instructions, "instructions"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_response_request_doc,
                                          previous_response_id,
                                          "previous_response_id"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_response_request_doc,
                                          conversation, "conversation"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_response_request_doc,
                                          prompt_cache_key,
                                          "prompt_cache_key"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_response_request_doc,
                                          service_tier, "service_tier"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_response_request_doc,
                                          truncation, "truncation"),
    LONEJSON_FIELD_JSON_VALUE_OMIT_NULL(cai_response_request_doc, metadata,
                                        "metadata"),
    LONEJSON_FIELD_JSON_VALUE_OMIT_NULL(cai_response_request_doc, include,
                                        "include"),
    LONEJSON_FIELD_JSON_VALUE_OMIT_NULL(cai_response_request_doc, prompt,
                                        "prompt"),
    LONEJSON_FIELD_JSON_VALUE_OMIT_NULL(cai_response_request_doc, tool_choice,
                                        "tool_choice"),
    LONEJSON_FIELD_I64_PRESENT(cai_response_request_doc, max_output_tokens,
                               has_max_output_tokens, "max_output_tokens"),
    LONEJSON_FIELD_I64_PRESENT(cai_response_request_doc, max_tool_calls,
                               has_max_tool_calls, "max_tool_calls"),
    LONEJSON_FIELD_BOOL_PRESENT(cai_response_request_doc, background,
                                has_background, "background"),
    LONEJSON_FIELD_BOOL_PRESENT(cai_response_request_doc, store, has_store,
                                "store"),
    LONEJSON_FIELD_OBJECT_OMIT_EMPTY(cai_response_request_doc, reasoning,
                                     "reasoning",
                                     &cai_response_request_reasoning_map),
    LONEJSON_FIELD_OBJECT_OMIT_EMPTY(cai_response_request_doc, text, "text",
                                     &cai_response_request_text_map),
    LONEJSON_FIELD_BOOL_PRESENT(cai_response_request_doc, parallel_tool_calls,
                                has_parallel_tool_calls,
                                "parallel_tool_calls"),
    LONEJSON_FIELD_OBJECT_ARRAY_OMIT_EMPTY(
        cai_response_request_doc, context_management, "context_management",
        cai_response_request_context_doc, &cai_response_request_context_map,
        LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_JSON_VALUE_REQ(cai_response_request_doc, input, "input"),
    LONEJSON_FIELD_JSON_VALUE_OMIT_NULL(cai_response_request_doc, tools,
                                        "tools"),
    LONEJSON_FIELD_BOOL_PRESENT(cai_response_request_doc, stream, has_stream,
                                "stream")};
LONEJSON_MAP_DEFINE(cai_response_request_map, cai_response_request_doc,
                    cai_response_request_fields);

static int cai_response_validate_json_value(const char *json,
                                            const char *message,
                                            cai_error *error) {
  lonejson_json_value value;
  lonejson_error json_error;
  int rc;

  if (json == NULL) {
    return CAI_OK;
  }
  if (json[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, message);
  }
  CAI_LJ->json_value_init(CAI_LJ, &value);
  lonejson_error_init(&json_error);
  if (value.methods->set_buffer(&value, json, strlen(json),
                                     &json_error) != LONEJSON_STATUS_OK) {
    rc = cai_set_error_detail(error, CAI_ERR_INVALID, message,
                              json_error.message);
  } else {
    rc = CAI_OK;
  }
  CAI_LJ->json_value_cleanup(CAI_LJ, &value);
  return rc;
}

static void cai_content_part_cleanup(const cai_allocator *allocator,
                                     struct cai_content_part *part) {
  if (part == NULL) {
    return;
  }
  cai_free_mem(allocator, part->type);
  cai_free_mem(allocator, part->text);
  if (part->has_text_spooled) {
    part->text_spooled.cleanup(&part->text_spooled);
    part->has_text_spooled = 0;
  }
  cai_free_mem(allocator, part->image_url);
  cai_free_mem(allocator, part->file_id);
  cai_free_mem(allocator, part->filename);
  cai_free_mem(allocator, part->file_url);
  if (part->has_file_data) {
    part->file_data.cleanup(&part->file_data);
    part->has_file_data = 0;
  }
  cai_free_mem(allocator, part->detail);
}

static void cai_input_message_cleanup(const cai_allocator *allocator,
                                      struct cai_input_message *message) {
  struct cai_content_part *parts;
  size_t i;

  if (message == NULL) {
    return;
  }
  cai_free_mem(allocator, message->role);
  cai_free_mem(allocator, message->call_id);
  cai_free_mem(allocator, message->output);
  if (message->has_output_spooled) {
    message->output_spooled.cleanup(&message->output_spooled);
    message->has_output_spooled = 0;
  }
  parts = (struct cai_content_part *)message->content.items;
  for (i = 0U; i < message->content.count; i++) {
    cai_content_part_cleanup(allocator, &parts[i]);
  }
  cai_free_mem(allocator, message->content.items);
}

static void cai_function_tool_cleanup(const cai_allocator *allocator,
                                      struct cai_function_tool *tool) {
  if (tool == NULL) {
    return;
  }
  cai_free_mem(allocator, tool->name);
  cai_free_mem(allocator, tool->description);
  cai_free_mem(allocator, tool->parameters_json);
  cai_free_mem(allocator, tool->raw_json);
}

static void cai_object_array_init(lonejson_object_array *array,
                                  size_t elem_size) {
  array->items = NULL;
  array->count = 0U;
  array->capacity = 0U;
  array->elem_size = elem_size;
  array->flags = 0U;
}

static int cai_object_array_grow(const cai_allocator *allocator,
                                 lonejson_object_array *array, size_t elem_size,
                                 cai_error *error) {
  size_t new_capacity;
  void *new_items;

  if (array->count < array->capacity) {
    return CAI_OK;
  }
  new_capacity = array->capacity == 0U ? 2U : array->capacity * 2U;
  new_items =
      cai_realloc_mem(allocator, array->items, new_capacity * elem_size);
  if (new_items == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to grow JSON array");
  }
  array->items = new_items;
  array->capacity = new_capacity;
  array->elem_size = elem_size;
  return CAI_OK;
}

static int cai_replace_string(const cai_allocator *allocator, char **slot,
                              const char *value, cai_error *error) {
  char *copy;

  copy = NULL;
  if (value != NULL) {
    copy = cai_strdup(allocator, value);
    if (copy == NULL) {
      return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate string");
    }
  }
  cai_free_mem(allocator, *slot);
  *slot = copy;
  return CAI_OK;
}

int cai_buffer_append(cai_buffer_builder *builder, const char *text,
                            size_t length, cai_error *error) {
  size_t needed;
  size_t new_capacity;
  char *grown;

  if (builder->sink != NULL) {
    lonejson_status status;

    status =
        builder->sink(builder->sink_user, text, length, builder->sink_error);
    if (status != LONEJSON_STATUS_OK) {
      return cai_set_error_detail(
          error, CAI_ERR_TRANSPORT, "failed to write buffer sink",
          builder->sink_error != NULL ? builder->sink_error->message : NULL);
    }
    builder->length += length;
    return CAI_OK;
  }
  needed = builder->length + length + 1U;
  if (needed > builder->capacity) {
    new_capacity = builder->capacity == 0U ? 256U : builder->capacity;
    while (new_capacity < needed) {
      new_capacity *= 2U;
    }
    grown = (char *)cai_realloc_mem(NULL, builder->data, new_capacity);
    if (grown == NULL) {
      return cai_set_error(error, CAI_ERR_NOMEM, "failed to grow buffer");
    }
    builder->data = grown;
    builder->capacity = new_capacity;
  }
  if (length > 0U) {
    memcpy(builder->data + builder->length, text, length);
    builder->length += length;
  }
  builder->data[builder->length] = '\0';
  return CAI_OK;
}

int cai_buffer_append_cstr(cai_buffer_builder *builder, const char *text,
                         cai_error *error) {
  return cai_buffer_append(builder, text, strlen(text), error);
}

int cai_buffer_append_json_string(cai_buffer_builder *builder, const char *value,
                            cai_error *error) {
  cai_response_buffer_sink_context sink_context;
  lonejson_error json_error;
  lonejson_status status;

  if (value == NULL) {
    value = "";
  }
  sink_context.builder = builder;
  sink_context.error = error;
  lonejson_error_init(&json_error);
  status = lonejson_write_json_string_buffer_sink(
      CAI_LJ, value, strlen(value), cai_response_buffer_sink, &sink_context,
      &json_error);
  if (status != LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to serialize JSON string",
                                json_error.message);
  }
  return CAI_OK;
}

static int cai_buffer_append_json_string_spooled(cai_buffer_builder *builder,
                                           const lonejson_spooled *value,
                                           cai_error *error) {
  cai_response_buffer_sink_context sink_context;
  lonejson_writer writer;
  lonejson_error json_error;
  lonejson_status status;

  sink_context.builder = builder;
  sink_context.error = error;
  lonejson_error_init(&json_error);
  if (value == NULL) {
    status = CAI_LJ->writer_init_sink(CAI_LJ, &writer, cai_response_buffer_sink, &sink_context, &json_error);
    if (status == LONEJSON_STATUS_OK) {
      status = writer.null_fn(&writer, &json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = writer.finish(&writer, &json_error);
    }
    writer.cleanup(&writer);
    if (status != LONEJSON_STATUS_OK) {
      return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                  "failed to serialize JSON null",
                                  json_error.message);
    }
    return CAI_OK;
  }
  status = lonejson_write_json_string_spooled_sink(
      CAI_LJ, value, cai_response_buffer_sink, &sink_context, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to serialize JSON spooled string",
                                json_error.message);
  }
  return CAI_OK;
}

static int cai_spooled_copy_to_builder(cai_buffer_builder *builder,
                                       const lonejson_spooled *value,
                                       cai_error *error) {
  lonejson_spooled cursor;
  lonejson_error json_error;
  lonejson_read_result chunk;
  unsigned char buffer[4096];
  int rc;

  if (value == NULL) {
    return CAI_OK;
  }
  cursor = *value;
  lonejson_error_init(&json_error);
  if (cursor.rewind(&cursor, &json_error) != LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to rewind spooled JSON",
                                json_error.message);
  }
  for (;;) {
    chunk = cursor.read(&cursor, buffer, sizeof(buffer));
    if (chunk.error_code != 0) {
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "failed to read spooled JSON");
    }
    if (chunk.bytes_read > 0U) {
      rc = cai_buffer_append(builder, (const char *)buffer,
                                   chunk.bytes_read, error);
      if (rc != CAI_OK) {
        return rc;
      }
    }
    if (chunk.eof) {
      break;
    }
  }
  return CAI_OK;
}

static int cai_spooled_copy_to_cstr(const cai_allocator *allocator,
                                    const lonejson_spooled *value,
                                    char **out, cai_error *error) {
  cai_buffer_builder builder;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "string output pointer is required");
  }
  *out = NULL;
  builder.data = NULL;
  builder.length = 0U;
  builder.capacity = 0U;
  builder.sink = NULL;
  builder.sink_user = NULL;
  builder.sink_error = NULL;
  rc = cai_spooled_copy_to_builder(&builder, value, error);
  if (rc != CAI_OK) {
    cai_free_mem(NULL, builder.data);
    return rc;
  }
  if (allocator != NULL && allocator->malloc_fn != NULL) {
    char *copy;

    copy = cai_strdup(allocator, builder.data != NULL ? builder.data : "");
    cai_free_mem(NULL, builder.data);
    if (copy == NULL) {
      return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate string");
    }
    *out = copy;
  } else {
    *out = builder.data;
  }
  return CAI_OK;
}

static lonejson_read_result
cai_response_spooled_reader(void *user, unsigned char *buffer,
                            size_t capacity) {
  cai_response_spooled_reader_context *context;
  lonejson_read_result result;

  context = (cai_response_spooled_reader_context *)user;
  if (context == NULL) {
    result = lonejson_default_read_result();
    result.eof = 1;
    result.error_code = 1;
    return result;
  }
  return context->cursor.read(&context->cursor, buffer, capacity);
}

static lonejson_status cai_response_root_object_begin(void *user,
                                                      lonejson_error *error) {
  cai_response_json_root_check *check;

  (void)error;
  check = (cai_response_json_root_check *)user;
  if (check->depth == 0) {
    check->root_seen = 1;
    check->root_is_object = 1;
    check->root_is_array = 0;
    check->root_is_string = 0;
  }
  check->depth++;
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_response_root_object_end(void *user,
                                                    lonejson_error *error) {
  cai_response_json_root_check *check;

  (void)error;
  check = (cai_response_json_root_check *)user;
  if (check->depth > 0) {
    check->depth--;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_response_root_array_begin(void *user,
                                                     lonejson_error *error) {
  cai_response_json_root_check *check;

  (void)error;
  check = (cai_response_json_root_check *)user;
  if (check->depth == 0) {
    check->root_seen = 1;
    check->root_is_object = 0;
    check->root_is_array = 1;
    check->root_is_string = 0;
  }
  check->depth++;
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_response_root_array_end(void *user,
                                                   lonejson_error *error) {
  return cai_response_root_object_end(user, error);
}

static lonejson_status cai_response_root_scalar(void *user,
                                                lonejson_error *error) {
  cai_response_json_root_check *check;

  (void)error;
  check = (cai_response_json_root_check *)user;
  if (check->depth == 0) {
    check->root_seen = 1;
    check->root_is_object = 0;
    check->root_is_array = 0;
    check->root_is_string = 0;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_response_root_string(void *user,
                                                lonejson_error *error) {
  cai_response_json_root_check *check;

  (void)error;
  check = (cai_response_json_root_check *)user;
  if (check->depth == 0) {
    check->root_seen = 1;
    check->root_is_object = 0;
    check->root_is_array = 0;
    check->root_is_string = 1;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_response_root_bool(void *user, int value,
                                              lonejson_error *error) {
  (void)value;
  return cai_response_root_scalar(user, error);
}

static int cai_response_spooled_root_is_array(const lonejson_spooled *spool,
                                              int *out_is_array,
                                              cai_error *error) {
  cai_response_spooled_reader_context reader_context;
  cai_response_json_root_check check;
  lonejson_value_visitor visitor;
  lonejson_error json_error;

  if (out_is_array == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "JSON root output pointer is required");
  }
  *out_is_array = 0;
  if (spool == NULL || spool->size_fn(spool) == 0U) {
    return cai_set_error(error, CAI_ERR_INVALID, "JSON value is empty");
  }
  reader_context.cursor = *spool;
  lonejson_error_init(&json_error);
  if (reader_context.cursor.rewind(&reader_context.cursor, &json_error) !=
      LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to rewind JSON value",
                                json_error.message);
  }
  memset(&check, 0, sizeof(check));
  visitor = lonejson_default_value_visitor();
  visitor.object_begin = cai_response_root_object_begin;
  visitor.object_end = cai_response_root_object_end;
  visitor.array_begin = cai_response_root_array_begin;
  visitor.array_end = cai_response_root_array_end;
  visitor.string_begin = cai_response_root_string;
  visitor.number_begin = cai_response_root_scalar;
  visitor.boolean_value = cai_response_root_bool;
  visitor.null_value = cai_response_root_scalar;
  lonejson_error_init(&json_error);
  if (CAI_LJ->visit_value_reader(CAI_LJ, cai_response_spooled_reader,
                                 &reader_context, &visitor, &check,
                                 &json_error) !=
      LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_INVALID,
                                "JSON value is not valid JSON",
                                json_error.message);
  }
  *out_is_array = check.root_seen && check.root_is_array;
  return CAI_OK;
}

static int cai_response_json_root_kind(const char *json, int *out_is_object,
                                       int *out_is_array, int *out_is_string,
                                       const char *message, cai_error *error) {
  lonejson_spooled spool;
  lonejson_error json_error;
  cai_response_spooled_reader_context reader_context;
  cai_response_json_root_check check;
  lonejson_value_visitor visitor;
  int rc;

  if (out_is_object != NULL) {
    *out_is_object = 0;
  }
  if (out_is_array != NULL) {
    *out_is_array = 0;
  }
  if (out_is_string != NULL) {
    *out_is_string = 0;
  }
  if (json == NULL || json[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, message);
  }
  lonejson_error_init(&json_error);
  CAI_LJ->spooled_init(CAI_LJ, &spool);
  if (spool.append(&spool, json, strlen(json), &json_error) !=
      LONEJSON_STATUS_OK) {
    spool.cleanup(&spool);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to spool JSON value",
                                json_error.message);
  }
  reader_context.cursor = spool;
  rc = CAI_OK;
  lonejson_error_init(&json_error);
  if (reader_context.cursor.rewind(&reader_context.cursor, &json_error) !=
      LONEJSON_STATUS_OK) {
    rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                              "failed to rewind JSON value",
                              json_error.message);
  } else {
    memset(&check, 0, sizeof(check));
    visitor = lonejson_default_value_visitor();
    visitor.object_begin = cai_response_root_object_begin;
    visitor.object_end = cai_response_root_object_end;
    visitor.array_begin = cai_response_root_array_begin;
    visitor.array_end = cai_response_root_array_end;
    visitor.string_begin = cai_response_root_string;
    visitor.number_begin = cai_response_root_scalar;
    visitor.boolean_value = cai_response_root_bool;
    visitor.null_value = cai_response_root_scalar;
    lonejson_error_init(&json_error);
    if (CAI_LJ->visit_value_reader(CAI_LJ, cai_response_spooled_reader,
                                   &reader_context, &visitor, &check,
                                   &json_error) != LONEJSON_STATUS_OK) {
      rc = cai_set_error_detail(error, CAI_ERR_INVALID, message,
                                json_error.message);
    } else {
      if (out_is_object != NULL) {
        *out_is_object = check.root_seen && check.root_is_object;
      }
      if (out_is_array != NULL) {
        *out_is_array = check.root_seen && check.root_is_array;
      }
      if (out_is_string != NULL) {
        *out_is_string = check.root_seen && check.root_is_string;
      }
    }
  }
  spool.cleanup(&spool);
  return rc;
}

static lonejson_status
cai_response_request_write_sink(void *user, const void *data, size_t len,
                                lonejson_error *error) {
  cai_response_request_write_context *context;
  lonejson_status status;

  context = (cai_response_request_write_context *)user;
  if (context == NULL || context->sink == NULL) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  context->length += len;
  status = context->sink(context->sink_user, data, len, error);
  if (status != LONEJSON_STATUS_OK && context->sink_error != NULL &&
      error != NULL) {
    *context->sink_error = *error;
  }
  return status;
}

static lonejson_status
cai_response_buffer_sink(void *user, const void *data, size_t len,
                               lonejson_error *error) {
  cai_response_buffer_sink_context *context;

  (void)error;
  context = (cai_response_buffer_sink_context *)user;
  if (context == NULL || context->builder == NULL || data == NULL) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (cai_buffer_append(context->builder, (const char *)data, len,
                              context->error) != CAI_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_spooled_lonejson_sink(void *user, const void *data,
                                                 size_t len,
                                                 lonejson_error *error) {
  return ((lonejson_spooled *)user)->append((lonejson_spooled *)user, data, len,
                                            error);
}

static lonejson_status
cai_writer_mapped_object_array(lonejson_writer *writer,
                               const lonejson_map *map, const void *items,
                               size_t count, size_t elem_size,
                               lonejson_error *json_error) {
  const unsigned char *cursor;
  lonejson_status status;
  size_t i;

  if (map == NULL || (count > 0U && items == NULL)) {
    return LONEJSON_STATUS_INVALID_ARGUMENT;
  }
  status = writer->begin_array(writer, json_error);
  cursor = (const unsigned char *)items;
  for (i = 0U; status == LONEJSON_STATUS_OK && i < count; i++) {
    status = writer->mapped(writer, map, cursor + (i * elem_size), json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer->end_array(writer, json_error);
  }
  return status;
}

static int cai_spool_mapped_object_array(const lonejson_map *map,
                                         const void *items, size_t count,
                                         size_t elem_size,
                                         lonejson_spooled *out,
                                         size_t *out_len,
                                         const char *message,
                                         cai_error *error) {
  lonejson_writer writer;
  lonejson_error json_error;
  lonejson_status status;
  int writer_initialized;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "mapped array spool output pointer is required");
  }
  lonejson_error_init(&json_error);
  CAI_LJ->spooled_init(CAI_LJ, out);
  writer_initialized = 0;
  status = CAI_LJ->writer_init_sink(CAI_LJ, &writer, cai_spooled_lonejson_sink, out, &json_error);
  if (status == LONEJSON_STATUS_OK) {
    writer_initialized = 1;
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_writer_mapped_object_array(&writer, map, items, count,
                                            elem_size, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.finish(&writer, &json_error);
  }
  if (writer_initialized) {
    writer.cleanup(&writer);
  }
  if (status != LONEJSON_STATUS_OK) {
    out->cleanup(out);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT, message,
                                json_error.message);
  }
  if (out_len != NULL) {
    *out_len = out->size_fn(out);
  }
  return CAI_OK;
}

static void cai_history_output_docs_cleanup(cai_history_output_doc *docs,
                                            size_t count) {
  size_t i;

  if (docs == NULL) {
    return;
  }
  for (i = 0U; i < count; i++) {
    CAI_LJ->json_value_cleanup(CAI_LJ, &docs[i].summary);
    if (docs[i].has_summary_json) {
      docs[i].summary_json.cleanup(&docs[i].summary_json);
    }
  }
  cai_free_mem(NULL, docs);
}

static int cai_spool_history_content_array(
    const lonejson_object_array *content, lonejson_spooled *out,
    cai_error *error) {
  return cai_spool_mapped_object_array(
      &cai_history_content_map, content != NULL ? content->items : NULL,
      content != NULL ? content->count : 0U, sizeof(cai_response_content_doc),
      out, NULL, "failed to serialize history content JSON", error);
}

static int cai_build_history_output_docs(cai_response_doc *response_doc,
                                         cai_history_output_doc **out,
                                         size_t *out_count,
                                         cai_error *error) {
  cai_response_output_doc *items;
  cai_history_output_doc *docs;
  lonejson_error json_error;
  size_t i;

  *out = NULL;
  *out_count = 0U;
  if (response_doc->output.count == 0U) {
    return CAI_OK;
  }
  docs = (cai_history_output_doc *)cai_alloc(
      NULL, response_doc->output.count * sizeof(*docs));
  if (docs == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate history output docs");
  }
  memset(docs, 0, response_doc->output.count * sizeof(*docs));
  items = (cai_response_output_doc *)response_doc->output.items;
  for (i = 0U; i < response_doc->output.count; i++) {
    docs[i].type = items[i].type;
    docs[i].id = items[i].id;
    docs[i].status = items[i].status;
    if (items[i].type != NULL && strcmp(items[i].type, "message") == 0) {
      docs[i].role = items[i].role;
    }
    docs[i].call_id = items[i].call_id;
    docs[i].name = items[i].name;
    docs[i].arguments = items[i].arguments;
    docs[i].created_by = items[i].created_by;
    docs[i].encrypted_content = items[i].encrypted_content;
    docs[i].content = items[i].content;
    docs[i].content.flags = LONEJSON_ARRAY_FIXED_CAPACITY;
    CAI_LJ->json_value_init(CAI_LJ, &docs[i].summary);
    if (items[i].summary.count > 0U ||
        (items[i].type != NULL && strcmp(items[i].type, "reasoning") == 0)) {
      int rc;

      rc = cai_spool_history_content_array(&items[i].summary,
                                           &docs[i].summary_json, error);
      if (rc != CAI_OK) {
        cai_history_output_docs_cleanup(docs, i + 1U);
        return rc;
      }
      docs[i].has_summary_json = 1;
      docs[i].summary_reader.cursor = docs[i].summary_json;
      lonejson_error_init(&json_error);
      if (docs[i].summary.methods->set_reader(&docs[i].summary,
                                     cai_response_spooled_reader,
                                     &docs[i].summary_reader,
                                     &json_error) != LONEJSON_STATUS_OK) {
        cai_history_output_docs_cleanup(docs, i + 1U);
        return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                    "failed to prepare history summary JSON",
                                    json_error.message);
      }
    }
    *out_count = i + 1U;
  }
  *out = docs;
  return CAI_OK;
}

static int cai_capture_response_output_json(cai_response_doc *response_doc,
                                            lonejson_spooled *out,
                                            cai_error *error) {
  cai_history_output_doc *docs;
  size_t count;
  int rc;

  docs = NULL;
  count = 0U;
  rc = cai_build_history_output_docs(response_doc, &docs, &count, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_spool_mapped_object_array(&cai_history_output_map, docs, count,
                                     sizeof(*docs), out, NULL,
                                     "failed to serialize history output JSON",
                                     error);
  cai_history_output_docs_cleanup(docs, count);
  return rc;
}

int cai_response_create_params_new(cai_response_create_params **out,
                                   cai_error *error) {
  cai_response_create_params *params;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params output pointer is required");
  }
  *out = NULL;
  params = (cai_response_create_params *)cai_alloc(NULL, sizeof(*params));
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate response params");
  }
  params->allocator.malloc_fn = NULL;
  params->allocator.realloc_fn = NULL;
  params->allocator.free_fn = NULL;
  params->allocator.context = NULL;
  params->model = NULL;
  params->conversation_id = NULL;
  params->instructions = NULL;
  params->previous_response_id = NULL;
  params->prompt_cache_key = NULL;
  params->service_tier = NULL;
  params->truncation = NULL;
  params->metadata_json = NULL;
  params->include_json = NULL;
  params->prompt_json = NULL;
  params->tool_choice = NULL;
  params->tool_choice_json = NULL;
  params->reasoning_effort = NULL;
  params->reasoning_summary = NULL;
  params->text_format_type = NULL;
  params->text_format_name = NULL;
  params->text_format_description = NULL;
  params->text_format_schema_json = NULL;
  params->text_verbosity = NULL;
  params->text_format_strict = 0;
  params->max_output_tokens = 0;
  params->max_tool_calls = 0;
  params->parallel_tool_calls = -1;
  params->background = 0;
  params->has_background = 0;
  params->store = 0;
  params->has_store = 0;
  params->compact_threshold_tokens = 0LL;
  params->raw_input_json = NULL;
  params->has_raw_input_spooled = 0;
  cai_object_array_init(&params->input, sizeof(struct cai_input_message));
  cai_object_array_init(&params->tools, sizeof(struct cai_function_tool));
  *out = params;
  return CAI_OK;
}

void cai_response_create_params_destroy(cai_response_create_params *params) {
  struct cai_input_message *messages;
  struct cai_function_tool *tools;
  size_t i;

  if (params == NULL) {
    return;
  }
  cai_free_mem(&params->allocator, params->model);
  cai_free_mem(&params->allocator, params->conversation_id);
  cai_free_mem(&params->allocator, params->instructions);
  cai_free_mem(&params->allocator, params->previous_response_id);
  cai_free_mem(&params->allocator, params->prompt_cache_key);
  cai_free_mem(&params->allocator, params->service_tier);
  cai_free_mem(&params->allocator, params->truncation);
  cai_free_mem(&params->allocator, params->metadata_json);
  cai_free_mem(&params->allocator, params->include_json);
  cai_free_mem(&params->allocator, params->prompt_json);
  cai_free_mem(&params->allocator, params->tool_choice);
  cai_free_mem(&params->allocator, params->tool_choice_json);
  cai_free_mem(&params->allocator, params->reasoning_effort);
  cai_free_mem(&params->allocator, params->reasoning_summary);
  cai_free_mem(&params->allocator, params->text_format_type);
  cai_free_mem(&params->allocator, params->text_format_name);
  cai_free_mem(&params->allocator, params->text_format_description);
  cai_free_mem(&params->allocator, params->text_format_schema_json);
  cai_free_mem(&params->allocator, params->text_verbosity);
  cai_free_mem(&params->allocator, params->raw_input_json);
  if (params->has_raw_input_spooled) {
    params->raw_input_spooled.cleanup(&params->raw_input_spooled);
  }
  messages = (struct cai_input_message *)params->input.items;
  for (i = 0U; i < params->input.count; i++) {
    cai_input_message_cleanup(&params->allocator, &messages[i]);
  }
  tools = (struct cai_function_tool *)params->tools.items;
  for (i = 0U; i < params->tools.count; i++) {
    cai_function_tool_cleanup(&params->allocator, &tools[i]);
  }
  cai_free_mem(&params->allocator, params->input.items);
  cai_free_mem(&params->allocator, params->tools.items);
  cai_free_mem(&params->allocator, params);
}

void cai_response_create_params_clear_input(
    cai_response_create_params *params) {
  struct cai_input_message *messages;
  size_t i;

  if (params == NULL) {
    return;
  }
  messages = (struct cai_input_message *)params->input.items;
  for (i = 0U; i < params->input.count; i++) {
    cai_input_message_cleanup(&params->allocator, &messages[i]);
  }
  params->input.count = 0U;
}

int cai_response_create_params_set_model(cai_response_create_params *params,
                                         const char *model, cai_error *error) {
  if (params == NULL || model == NULL || model[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "model is required");
  }
  return cai_replace_string(&params->allocator, &params->model, model, error);
}

int cai_response_create_params_set_instructions(
    cai_response_create_params *params, const char *instructions,
    cai_error *error) {
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  return cai_replace_string(&params->allocator, &params->instructions,
                            instructions, error);
}

int cai_response_create_params_set_previous_response_id(
    cai_response_create_params *params, const char *response_id,
    cai_error *error) {
  int rc;

  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  if (response_id != NULL && response_id[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "previous response id must not be empty");
  }
  rc = cai_replace_string(&params->allocator, &params->previous_response_id,
                          response_id, error);
  if (rc == CAI_OK && response_id != NULL) {
    cai_free_mem(&params->allocator, params->conversation_id);
    params->conversation_id = NULL;
  }
  return rc;
}

int cai_response_create_params_set_conversation_id(
    cai_response_create_params *params, const char *conversation_id,
    cai_error *error) {
  int rc;

  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  if (conversation_id != NULL && conversation_id[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "conversation id must not be empty");
  }
  rc = cai_replace_string(&params->allocator, &params->conversation_id,
                          conversation_id, error);
  if (rc == CAI_OK && conversation_id != NULL) {
    cai_free_mem(&params->allocator, params->previous_response_id);
    params->previous_response_id = NULL;
  }
  return rc;
}

int cai_response_create_params_set_prompt_cache_key(
    cai_response_create_params *params, const char *prompt_cache_key,
    cai_error *error) {
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  return cai_replace_string(&params->allocator, &params->prompt_cache_key,
                            prompt_cache_key, error);
}

int cai_response_create_params_set_background(
    cai_response_create_params *params, int enabled, cai_error *error) {
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  if (enabled != 0 && enabled != 1) {
    return cai_set_error(error, CAI_ERR_INVALID, "background must be 0 or 1");
  }
  params->background = enabled;
  params->has_background = 1;
  return CAI_OK;
}

int cai_response_create_params_set_store(cai_response_create_params *params,
                                         int enabled, cai_error *error) {
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  if (enabled != 0 && enabled != 1) {
    return cai_set_error(error, CAI_ERR_INVALID, "store must be 0 or 1");
  }
  params->store = enabled;
  params->has_store = 1;
  return CAI_OK;
}

int cai_response_create_params_set_service_tier(
    cai_response_create_params *params, const char *service_tier,
    cai_error *error) {
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  return cai_replace_string(&params->allocator, &params->service_tier,
                            service_tier, error);
}

int cai_response_create_params_set_truncation(
    cai_response_create_params *params, const char *truncation,
    cai_error *error) {
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  return cai_replace_string(&params->allocator, &params->truncation,
                            truncation, error);
}

static int cai_response_params_set_json_object_string(
    cai_response_create_params *params, char **slot, const char *json,
    const char *message, cai_error *error) {
  int is_object;
  int rc;

  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  if (json != NULL) {
    is_object = 0;
    rc = cai_response_json_root_kind(json, &is_object, NULL, NULL, message,
                                     error);
    if (rc != CAI_OK) {
      return rc;
    }
    if (!is_object) {
      return cai_set_error(error, CAI_ERR_INVALID, message);
    }
  }
  return cai_replace_string(&params->allocator, slot, json, error);
}

static int cai_response_params_set_json_array_string(
    cai_response_create_params *params, char **slot, const char *json,
    const char *message, cai_error *error) {
  int is_array;
  int rc;

  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  if (json != NULL) {
    is_array = 0;
    rc = cai_response_json_root_kind(json, NULL, &is_array, NULL, message,
                                     error);
    if (rc != CAI_OK) {
      return rc;
    }
    if (!is_array) {
      return cai_set_error(error, CAI_ERR_INVALID, message);
    }
  }
  return cai_replace_string(&params->allocator, slot, json, error);
}

int cai_response_create_params_set_metadata_json(
    cai_response_create_params *params, const char *metadata_json,
    cai_error *error) {
  return cai_response_params_set_json_object_string(
      params, &params->metadata_json, metadata_json,
      "metadata must be a valid JSON object", error);
}

int cai_response_create_params_set_include_json(
    cai_response_create_params *params, const char *include_json,
    cai_error *error) {
  return cai_response_params_set_json_array_string(
      params, &params->include_json, include_json,
      "include must be a valid JSON array", error);
}

int cai_response_create_params_set_prompt_json(
    cai_response_create_params *params, const char *prompt_json,
    cai_error *error) {
  return cai_response_params_set_json_object_string(
      params, &params->prompt_json, prompt_json,
      "prompt must be a valid JSON object", error);
}

int cai_response_create_params_set_tool_choice(
    cai_response_create_params *params, const char *tool_choice,
    cai_error *error) {
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  if (tool_choice != NULL && tool_choice[0] != '\0' &&
      strcmp(tool_choice, CAI_TOOL_CHOICE_AUTO) != 0 &&
      strcmp(tool_choice, CAI_TOOL_CHOICE_NONE) != 0 &&
      strcmp(tool_choice, CAI_TOOL_CHOICE_REQUIRED) != 0) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "tool choice must be auto, none, or required");
  }
  if (params->tool_choice_json != NULL) {
    cai_free_mem(&params->allocator, params->tool_choice_json);
    params->tool_choice_json = NULL;
  }
  return cai_replace_string(&params->allocator, &params->tool_choice,
                            tool_choice, error);
}

int cai_response_create_params_set_tool_choice_json(
    cai_response_create_params *params, const char *tool_choice_json,
    cai_error *error) {
  int rc;

  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  if (tool_choice_json != NULL) {
    rc = cai_response_validate_json_value(
        tool_choice_json, "tool choice must be valid JSON", error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  rc = cai_replace_string(&params->allocator, &params->tool_choice_json,
                          tool_choice_json, error);
  if (rc == CAI_OK && params->tool_choice_json != NULL) {
    cai_free_mem(&params->allocator, params->tool_choice);
    params->tool_choice = NULL;
  }
  return rc;
}

int cai_response_create_params_set_max_output_tokens(
    cai_response_create_params *params, int max_output_tokens,
    cai_error *error) {
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  if (max_output_tokens < 0) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "max output tokens cannot be negative");
  }
  params->max_output_tokens = max_output_tokens;
  return CAI_OK;
}

int cai_response_create_params_set_max_tool_calls(
    cai_response_create_params *params, int max_tool_calls, cai_error *error) {
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  if (max_tool_calls < 0) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "max tool calls cannot be negative");
  }
  params->max_tool_calls = max_tool_calls;
  return CAI_OK;
}

int cai_response_create_params_set_reasoning(cai_response_create_params *params,
                                             const char *effort,
                                             const char *summary,
                                             cai_error *error) {
  int rc;

  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  rc = cai_replace_string(&params->allocator, &params->reasoning_effort, effort,
                          error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_replace_string(&params->allocator, &params->reasoning_summary,
                            summary, error);
}

int cai_response_create_params_set_parallel_tool_calls(
    cai_response_create_params *params, int enabled, cai_error *error) {
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  if (enabled != 0 && enabled != 1) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "parallel tool calls must be 0 or 1");
  }
  params->parallel_tool_calls = enabled;
  return CAI_OK;
}

int cai_response_create_params_set_compact_threshold(
    cai_response_create_params *params, long long compact_threshold_tokens,
    cai_error *error) {
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  if (compact_threshold_tokens < 1000LL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "compact threshold must be at least 1000 tokens");
  }
  params->compact_threshold_tokens = compact_threshold_tokens;
  return CAI_OK;
}

int cai_response_create_params_set_raw_input_json(
    cai_response_create_params *params, const char *raw_input_json,
    cai_error *error) {
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  if (raw_input_json != NULL &&
      cai_response_validate_json_value(raw_input_json,
                                       "raw input must be valid JSON",
                                       error) != CAI_OK) {
    return error != NULL ? error->code : CAI_ERR_INVALID;
  }
  if (params->has_raw_input_spooled) {
    params->raw_input_spooled.cleanup(&params->raw_input_spooled);
    params->has_raw_input_spooled = 0;
  }
  return cai_replace_string(&params->allocator, &params->raw_input_json,
                            raw_input_json, error);
}

int cai_response_create_params_set_raw_input_spooled(
    cai_response_create_params *params, lonejson_spooled *raw_input_json,
    cai_error *error) {
  if (params == NULL || raw_input_json == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params and raw input spool are required");
  }
  cai_free_mem(&params->allocator, params->raw_input_json);
  params->raw_input_json = NULL;
  if (params->has_raw_input_spooled) {
    params->raw_input_spooled.cleanup(&params->raw_input_spooled);
  }
  params->raw_input_spooled = *raw_input_json;
  params->has_raw_input_spooled = 1;
  memset(raw_input_json, 0, sizeof(*raw_input_json));
  return CAI_OK;
}

static int
cai_response_params_set_text_format_type(cai_response_create_params *params,
                                         const char *type, cai_error *error) {
  int rc;

  rc = cai_replace_string(&params->allocator, &params->text_format_type, type,
                          error);
  if (rc == CAI_OK && strcmp(type, "json_object") == 0) {
    cai_free_mem(&params->allocator, params->text_format_name);
    cai_free_mem(&params->allocator, params->text_format_description);
    cai_free_mem(&params->allocator, params->text_format_schema_json);
    params->text_format_name = NULL;
    params->text_format_description = NULL;
    params->text_format_schema_json = NULL;
    params->text_format_strict = 0;
  }
  return rc;
}

int cai_response_create_params_set_text_format_json_object(
    cai_response_create_params *params, cai_error *error) {
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  return cai_response_params_set_text_format_type(params, "json_object", error);
}

int cai_response_create_params_set_text_format_json_schema(
    cai_response_create_params *params, const char *name,
    const char *description, const char *schema_json, int strict,
    cai_error *error) {
  int rc;

  if (params == NULL || name == NULL || name[0] == '\0' ||
      schema_json == NULL || schema_json[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "text format name and schema are required");
  }
  rc = cai_response_validate_json_value(
      schema_json, "text format schema must be valid JSON", error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_response_params_set_text_format_type(params, "json_schema", error);
  if (rc == CAI_OK) {
    rc = cai_replace_string(&params->allocator, &params->text_format_name, name,
                            error);
  }
  if (rc == CAI_OK) {
    rc =
        cai_replace_string(&params->allocator, &params->text_format_description,
                           description, error);
  }
  if (rc == CAI_OK) {
    rc =
        cai_replace_string(&params->allocator, &params->text_format_schema_json,
                           schema_json, error);
  }
  if (rc == CAI_OK) {
    params->text_format_strict = strict ? 1 : 0;
  }
  return rc;
}

int cai_response_create_params_set_text_verbosity(
    cai_response_create_params *params, const char *verbosity,
    cai_error *error) {
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  if (verbosity != NULL && verbosity[0] != '\0' &&
      strcmp(verbosity, CAI_TEXT_VERBOSITY_LOW) != 0 &&
      strcmp(verbosity, CAI_TEXT_VERBOSITY_MEDIUM) != 0 &&
      strcmp(verbosity, CAI_TEXT_VERBOSITY_HIGH) != 0) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "text verbosity must be low, medium, or high");
  }
  return cai_replace_string(&params->allocator, &params->text_verbosity,
                            verbosity, error);
}

static int cai_response_params_add_part(cai_response_create_params *params,
                                        const char *role,
                                        struct cai_content_part *part,
                                        cai_error *error) {
  struct cai_input_message *messages;
  struct cai_input_message *message;
  struct cai_content_part *parts;
  int rc;

  if (params == NULL || role == NULL || role[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "role is required");
  }
  rc = cai_object_array_grow(&params->allocator, &params->input,
                             sizeof(struct cai_input_message), error);
  if (rc != CAI_OK) {
    return rc;
  }
  messages = (struct cai_input_message *)params->input.items;
  message = &messages[params->input.count];
  memset(message, 0, sizeof(*message));
  message->kind = CAI_INPUT_MESSAGE;
  message->role = NULL;
  cai_object_array_init(&message->content, sizeof(struct cai_content_part));
  message->role = cai_strdup(&params->allocator, role);
  if (message->role == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate role");
  }
  rc = cai_object_array_grow(&params->allocator, &message->content,
                             sizeof(struct cai_content_part), error);
  if (rc != CAI_OK) {
    cai_input_message_cleanup(&params->allocator, message);
    return rc;
  }
  parts = (struct cai_content_part *)message->content.items;
  parts[0] = *part;
  message->content.count = 1U;
  params->input.count++;
  return CAI_OK;
}

static void cai_content_part_zero(struct cai_content_part *part) {
  memset(part, 0, sizeof(*part));
}

int cai_response_create_params_add_text(cai_response_create_params *params,
                                        const char *role, const char *text,
                                        cai_error *error) {
  struct cai_content_part part;
  int rc;

  if (text == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "text is required");
  }
  cai_content_part_zero(&part);
  part.type =
      cai_strdup(params != NULL ? &params->allocator : NULL, "input_text");
  part.text = cai_strdup(params != NULL ? &params->allocator : NULL, text);
  if (part.type == NULL || part.text == NULL) {
    cai_content_part_cleanup(params != NULL ? &params->allocator : NULL, &part);
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate text input");
  }
  rc = cai_response_params_add_part(params, role, &part, error);
  if (rc != CAI_OK) {
    cai_content_part_cleanup(params != NULL ? &params->allocator : NULL, &part);
  }
  return rc;
}

int cai_response_create_params_add_text_spooled(
    cai_response_create_params *params, const char *role,
    lonejson_spooled *text, cai_error *error) {
  struct cai_content_part part;
  int rc;

  if (text == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "text spool is required");
  }
  cai_content_part_zero(&part);
  part.type =
      cai_strdup(params != NULL ? &params->allocator : NULL, "input_text");
  part.text_spooled = *text;
  part.has_text_spooled = 1;
  memset(text, 0, sizeof(*text));
  if (part.type == NULL) {
    cai_content_part_cleanup(params != NULL ? &params->allocator : NULL, &part);
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate text input");
  }
  rc = cai_response_params_add_part(params, role, &part, error);
  if (rc != CAI_OK) {
    cai_content_part_cleanup(params != NULL ? &params->allocator : NULL, &part);
  }
  return rc;
}

int cai_response_create_params_add_image_url(cai_response_create_params *params,
                                             const char *role, const char *url,
                                             const char *detail,
                                             cai_error *error) {
  struct cai_content_part part;
  const cai_allocator *allocator;
  int rc;

  if (url == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "image URL is required");
  }
  allocator = params != NULL ? &params->allocator : NULL;
  cai_content_part_zero(&part);
  part.type = cai_strdup(allocator, "input_image");
  part.image_url = cai_strdup(allocator, url);
  part.detail = cai_strdup(allocator, detail);
  if (part.type == NULL || part.image_url == NULL ||
      (detail != NULL && part.detail == NULL)) {
    cai_content_part_cleanup(allocator, &part);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate image input");
  }
  rc = cai_response_params_add_part(params, role, &part, error);
  if (rc != CAI_OK) {
    cai_content_part_cleanup(allocator, &part);
  }
  return rc;
}

int cai_response_create_params_add_image_file_id(
    cai_response_create_params *params, const char *role, const char *file_id,
    const char *detail, cai_error *error) {
  struct cai_content_part part;
  const cai_allocator *allocator;
  int rc;

  if (file_id == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "image file id is required");
  }
  allocator = params != NULL ? &params->allocator : NULL;
  cai_content_part_zero(&part);
  part.type = cai_strdup(allocator, "input_image");
  part.file_id = cai_strdup(allocator, file_id);
  part.detail = cai_strdup(allocator, detail);
  if (part.type == NULL || part.file_id == NULL ||
      (detail != NULL && part.detail == NULL)) {
    cai_content_part_cleanup(allocator, &part);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate image file input");
  }
  rc = cai_response_params_add_part(params, role, &part, error);
  if (rc != CAI_OK) {
    cai_content_part_cleanup(allocator, &part);
  }
  return rc;
}

int cai_response_create_params_add_file_id(cai_response_create_params *params,
                                           const char *role,
                                           const char *file_id,
                                           const char *detail,
                                           cai_error *error) {
  struct cai_content_part part;
  const cai_allocator *allocator;
  int rc;

  if (file_id == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "file id is required");
  }
  allocator = params != NULL ? &params->allocator : NULL;
  cai_content_part_zero(&part);
  part.type = cai_strdup(allocator, "input_file");
  part.file_id = cai_strdup(allocator, file_id);
  part.detail = cai_strdup(allocator, detail);
  if (part.type == NULL || part.file_id == NULL ||
      (detail != NULL && part.detail == NULL)) {
    cai_content_part_cleanup(allocator, &part);
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate file input");
  }
  rc = cai_response_params_add_part(params, role, &part, error);
  if (rc != CAI_OK) {
    cai_content_part_cleanup(allocator, &part);
  }
  return rc;
}

int cai_response_create_params_add_file_url(cai_response_create_params *params,
                                            const char *role,
                                            const char *file_url,
                                            const char *detail,
                                            cai_error *error) {
  struct cai_content_part part;
  const cai_allocator *allocator;
  int rc;

  if (file_url == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "file URL is required");
  }
  allocator = params != NULL ? &params->allocator : NULL;
  cai_content_part_zero(&part);
  part.type = cai_strdup(allocator, "input_file");
  part.file_url = cai_strdup(allocator, file_url);
  part.detail = cai_strdup(allocator, detail);
  if (part.type == NULL || part.file_url == NULL ||
      (detail != NULL && part.detail == NULL)) {
    cai_content_part_cleanup(allocator, &part);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate file URL input");
  }
  rc = cai_response_params_add_part(params, role, &part, error);
  if (rc != CAI_OK) {
    cai_content_part_cleanup(allocator, &part);
  }
  return rc;
}

int cai_response_create_params_add_file_data_spooled(
    cai_response_create_params *params, const char *role, const char *filename,
    struct lonejson_spooled *file_data, const char *detail, cai_error *error) {
  struct cai_content_part part;
  const cai_allocator *allocator;
  int rc;

  if (file_data == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "file data spool is required");
  }
  allocator = params != NULL ? &params->allocator : NULL;
  cai_content_part_zero(&part);
  part.type = cai_strdup(allocator, "input_file");
  part.filename = cai_strdup(allocator, filename);
  part.detail = cai_strdup(allocator, detail);
  part.file_data = *file_data;
  part.has_file_data = 1;
  memset(file_data, 0, sizeof(*file_data));
  if (part.type == NULL || (filename != NULL && part.filename == NULL) ||
      (detail != NULL && part.detail == NULL)) {
    cai_content_part_cleanup(allocator, &part);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate file data input");
  }
  rc = cai_response_params_add_part(params, role, &part, error);
  if (rc != CAI_OK) {
    cai_content_part_cleanup(allocator, &part);
  }
  return rc;
}

int cai_response_create_params_add_function_tool(
    cai_response_create_params *params, const char *name,
    const char *description, const char *parameters_json, int strict,
    cai_error *error) {
  struct cai_function_tool *tools;
  struct cai_function_tool *tool;
  int rc;

  if (params == NULL || name == NULL || name[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "tool name is required");
  }
  rc = cai_response_validate_json_value(
      parameters_json != NULL ? parameters_json : "{}",
      "function tool parameters must be valid JSON", error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_object_array_grow(&params->allocator, &params->tools,
                             sizeof(struct cai_function_tool), error);
  if (rc != CAI_OK) {
    return rc;
  }
  tools = (struct cai_function_tool *)params->tools.items;
  tool = &tools[params->tools.count];
  memset(tool, 0, sizeof(*tool));
  tool->name = cai_strdup(&params->allocator, name);
  tool->description = cai_strdup(&params->allocator, description);
  tool->parameters_json = cai_strdup(
      &params->allocator, parameters_json != NULL ? parameters_json : "{}");
  tool->strict = strict ? 1 : 0;
  if (tool->name == NULL ||
      (description != NULL && tool->description == NULL) ||
      tool->parameters_json == NULL) {
    cai_function_tool_cleanup(&params->allocator, tool);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate function tool");
  }
  params->tools.count++;
  return CAI_OK;
}

int cai_response_create_params_add_hosted_tool_json(
    cai_response_create_params *params, const char *tool_json,
    cai_error *error) {
  struct cai_function_tool *tools;
  struct cai_function_tool *tool;
  int is_object;
  int rc;

  if (params == NULL || tool_json == NULL || tool_json[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "hosted tool JSON object is required");
  }
  is_object = 0;
  rc = cai_response_json_root_kind(
      tool_json, &is_object, NULL, NULL,
      "hosted tool must be a valid JSON object", error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (!is_object) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "hosted tool must be a JSON object");
  }
  rc = cai_object_array_grow(&params->allocator, &params->tools,
                             sizeof(struct cai_function_tool), error);
  if (rc != CAI_OK) {
    return rc;
  }
  tools = (struct cai_function_tool *)params->tools.items;
  tool = &tools[params->tools.count];
  memset(tool, 0, sizeof(*tool));
  tool->raw_json = cai_strdup(&params->allocator, tool_json);
  tool->is_raw = 1;
  if (tool->raw_json == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate hosted tool JSON");
  }
  params->tools.count++;
  return CAI_OK;
}

int cai_response_create_params_add_simple_hosted_tool(
    cai_response_create_params *params, const char *type, cai_error *error) {
  cai_buffer_builder builder;
  cai_response_buffer_sink_context sink_context;
  lonejson_writer writer;
  lonejson_error json_error;
  lonejson_status status;
  int writer_initialized;
  int rc;

  if (type == NULL || type[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "hosted tool type is required");
  }
  lonejson_error_init(&json_error);
  builder.data = NULL;
  builder.length = 0U;
  builder.capacity = 0U;
  builder.sink = NULL;
  builder.sink_user = NULL;
  builder.sink_error = NULL;
  sink_context.builder = &builder;
  sink_context.error = error;
  writer_initialized = 0;
  status = CAI_LJ->writer_init_sink(CAI_LJ, &writer, cai_response_buffer_sink, &sink_context, &json_error);
  if (status == LONEJSON_STATUS_OK) {
    writer_initialized = 1;
    status = writer.begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.key(&writer, "type", 4U, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.string(&writer, type, strlen(type), &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.finish(&writer, &json_error);
  }
  if (writer_initialized) {
    writer.cleanup(&writer);
  }
  if (status != LONEJSON_STATUS_OK) {
    cai_free_mem(NULL, builder.data);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to serialize hosted tool",
                                json_error.message);
  }
  rc = cai_response_create_params_add_hosted_tool_json(params, builder.data,
                                                       error);
  cai_free_mem(NULL, builder.data);
  return rc;
}

void cai_hosted_mcp_tool_config_init(cai_hosted_mcp_tool_config *config) {
  if (config != NULL) {
    memset(config, 0, sizeof(*config));
  }
}

static lonejson_status cai_response_write_json_member(
    lonejson_writer *writer, const char *key, const char *json,
    const char *message, lonejson_error *json_error, cai_error *error) {
  int rc;

  rc = cai_response_validate_json_value(json, message, error);
  if (rc != CAI_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (writer->key(writer, key, strlen(key), json_error) !=
      LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return writer->json_value_buffer(writer, json, strlen(json), json_error);
}

static lonejson_status cai_response_write_string_member(
    lonejson_writer *writer, const char *key, const char *value,
    lonejson_error *json_error) {
  if (writer->key(writer, key, strlen(key), json_error) !=
      LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return writer->string(writer, value, strlen(value), json_error);
}

static lonejson_status cai_response_write_string_array_member(
    lonejson_writer *writer, const char *key, const char *const *values,
    size_t count, lonejson_error *json_error) {
  lonejson_status status;
  size_t i;

  status = writer->key(writer, key, strlen(key), json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = writer->begin_array(writer, json_error);
  }
  for (i = 0U; status == LONEJSON_STATUS_OK && i < count; i++) {
    status = writer->string(writer, values[i], strlen(values[i]), json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer->end_array(writer, json_error);
  }
  return status;
}

int cai_response_create_params_add_hosted_mcp_tool(
    cai_response_create_params *params,
    const cai_hosted_mcp_tool_config *config, cai_error *error) {
  cai_buffer_builder builder;
  cai_response_buffer_sink_context sink_context;
  lonejson_writer writer;
  lonejson_error json_error;
  lonejson_status status;
  int writer_initialized;
  int has_server_url;
  int has_connector_id;
  int is_object;
  int is_array;
  int is_string;
  size_t i;
  int rc;

  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "params are required");
  }
  if (config == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "hosted MCP tool config is required");
  }
  if (config->server_label == NULL || config->server_label[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "hosted MCP server label is required");
  }
  has_server_url =
      config->server_url != NULL && config->server_url[0] != '\0';
  has_connector_id =
      config->connector_id != NULL && config->connector_id[0] != '\0';
  if (!has_server_url && !has_connector_id) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "hosted MCP server_url or connector_id is required");
  }
  if (has_server_url && has_connector_id) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "hosted MCP server_url and connector_id are mutually "
                         "exclusive");
  }
  if (config->allowed_tools_json != NULL &&
      config->allowed_tools_json[0] != '\0' &&
      config->allowed_tool_name_count > 0U) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "hosted MCP allowed_tools_json and "
                         "allowed_tool_names are mutually exclusive");
  }
  if (config->allowed_tool_name_count > 0U &&
      config->allowed_tool_names == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "hosted MCP allowed tool names are required");
  }
  for (i = 0U; i < config->allowed_tool_name_count; i++) {
    if (config->allowed_tool_names[i] == NULL ||
        config->allowed_tool_names[i][0] == '\0') {
      return cai_set_error(error, CAI_ERR_INVALID,
                           "hosted MCP allowed tool name is required");
    }
  }
  if (config->headers_json != NULL && config->headers_json[0] != '\0') {
    is_object = 0;
    rc = cai_response_json_root_kind(
        config->headers_json, &is_object, NULL, NULL,
        "hosted MCP headers must be a valid JSON object", error);
    if (rc != CAI_OK) {
      return rc;
    }
    if (!is_object) {
      return cai_set_error(error, CAI_ERR_INVALID,
                           "hosted MCP headers must be a JSON object");
    }
  }
  if (config->allowed_tools_json != NULL &&
      config->allowed_tools_json[0] != '\0') {
    is_object = 0;
    is_array = 0;
    rc = cai_response_json_root_kind(
        config->allowed_tools_json, &is_object, &is_array, NULL,
        "hosted MCP allowed_tools must be a valid JSON array or object", error);
    if (rc != CAI_OK) {
      return rc;
    }
    if (!is_object && !is_array) {
      return cai_set_error(error, CAI_ERR_INVALID,
                           "hosted MCP allowed_tools must be a JSON array or "
                           "object");
    }
  }
  if (config->require_approval_json != NULL &&
      config->require_approval_json[0] != '\0') {
    is_object = 0;
    is_string = 0;
    rc = cai_response_json_root_kind(
        config->require_approval_json, &is_object, NULL, &is_string,
        "hosted MCP require_approval must be a valid JSON string or object",
        error);
    if (rc != CAI_OK) {
      return rc;
    }
    if (!is_object && !is_string) {
      return cai_set_error(error, CAI_ERR_INVALID,
                           "hosted MCP require_approval must be a JSON string "
                           "or object");
    }
  }

  lonejson_error_init(&json_error);
  builder.data = NULL;
  builder.length = 0U;
  builder.capacity = 0U;
  builder.sink = NULL;
  builder.sink_user = NULL;
  builder.sink_error = NULL;
  sink_context.builder = &builder;
  sink_context.error = error;
  writer_initialized = 0;
  status = CAI_LJ->writer_init_sink(CAI_LJ, &writer, cai_response_buffer_sink, &sink_context, &json_error);
  if (status == LONEJSON_STATUS_OK) {
    writer_initialized = 1;
    status = writer.begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_response_write_string_member(&writer, "type",
                                              CAI_HOSTED_TOOL_MCP, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = cai_response_write_string_member(
        &writer, "server_label", config->server_label, &json_error);
  }
  if (status == LONEJSON_STATUS_OK && has_server_url) {
    status = cai_response_write_string_member(
        &writer, "server_url", config->server_url, &json_error);
  }
  if (status == LONEJSON_STATUS_OK && has_connector_id) {
    status = cai_response_write_string_member(
        &writer, "connector_id", config->connector_id, &json_error);
  }
  if (status == LONEJSON_STATUS_OK && config->server_description != NULL &&
      config->server_description[0] != '\0') {
    status = cai_response_write_string_member(
        &writer, "server_description", config->server_description, &json_error);
  }
  if (status == LONEJSON_STATUS_OK && config->headers_json != NULL &&
      config->headers_json[0] != '\0') {
    status = cai_response_write_json_member(
        &writer, "headers", config->headers_json,
        "hosted MCP headers must be valid JSON", &json_error, error);
  }
  if (status == LONEJSON_STATUS_OK && config->allowed_tool_name_count > 0U) {
    status = cai_response_write_string_array_member(
        &writer, "allowed_tools", config->allowed_tool_names,
        config->allowed_tool_name_count, &json_error);
  }
  if (status == LONEJSON_STATUS_OK && config->allowed_tools_json != NULL &&
      config->allowed_tools_json[0] != '\0') {
    status = cai_response_write_json_member(
        &writer, "allowed_tools", config->allowed_tools_json,
        "hosted MCP allowed_tools must be valid JSON", &json_error, error);
  }
  if (status == LONEJSON_STATUS_OK && config->require_approval_json != NULL &&
      config->require_approval_json[0] != '\0') {
    status = cai_response_write_json_member(
        &writer, "require_approval", config->require_approval_json,
        "hosted MCP require_approval must be valid JSON", &json_error, error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.finish(&writer, &json_error);
  }
  if (writer_initialized) {
    writer.cleanup(&writer);
  }
  if (status != LONEJSON_STATUS_OK) {
    cai_free_mem(NULL, builder.data);
    if (error != NULL && error->message != NULL) {
      return error->code;
    }
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to serialize hosted MCP tool",
                                json_error.message);
  }
  rc = cai_response_create_params_add_hosted_tool_json(params, builder.data,
                                                       error);
  cai_free_mem(NULL, builder.data);
  return rc;
}

static int cai_response_spooled_clone(const lonejson_spooled *src,
                                      lonejson_spooled *dst,
                                      cai_error *error) {
  lonejson_error json_error;

  memset(dst, 0, sizeof(*dst));
  lonejson_error_init(&json_error);
  CAI_LJ->spooled_init(CAI_LJ, dst);
  if (src->write_to_sink(src, cai_spooled_lonejson_sink, dst,
                                     &json_error) != LONEJSON_STATUS_OK) {
    dst->cleanup(dst);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to clone spooled value",
                                json_error.message);
  }
  return CAI_OK;
}

static int
cai_response_content_part_clone(const cai_allocator *allocator,
                                struct cai_content_part *dst,
                                const struct cai_content_part *src,
                                cai_error *error) {
  int rc;

  memset(dst, 0, sizeof(*dst));
  dst->type = cai_strdup(allocator, src->type);
  dst->text = cai_strdup(allocator, src->text);
  dst->image_url = cai_strdup(allocator, src->image_url);
  dst->file_id = cai_strdup(allocator, src->file_id);
  dst->filename = cai_strdup(allocator, src->filename);
  dst->file_url = cai_strdup(allocator, src->file_url);
  dst->detail = cai_strdup(allocator, src->detail);
  if ((src->type != NULL && dst->type == NULL) ||
      (src->text != NULL && dst->text == NULL) ||
      (src->image_url != NULL && dst->image_url == NULL) ||
      (src->file_id != NULL && dst->file_id == NULL) ||
      (src->filename != NULL && dst->filename == NULL) ||
      (src->file_url != NULL && dst->file_url == NULL) ||
      (src->detail != NULL && dst->detail == NULL)) {
    cai_content_part_cleanup(allocator, dst);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to clone content part");
  }
  if (src->has_file_data) {
    rc = cai_response_spooled_clone(&src->file_data, &dst->file_data, error);
    if (rc != CAI_OK) {
      cai_content_part_cleanup(allocator, dst);
      return rc;
    }
    dst->has_file_data = 1;
  }
  if (src->has_text_spooled) {
    rc = cai_response_spooled_clone(&src->text_spooled, &dst->text_spooled,
                                    error);
    if (rc != CAI_OK) {
      cai_content_part_cleanup(allocator, dst);
      return rc;
    }
    dst->has_text_spooled = 1;
  }
  return CAI_OK;
}

static int
cai_response_input_message_clone(cai_response_create_params *dst_params,
                                 struct cai_input_message *dst,
                                 const struct cai_input_message *src,
                                 cai_error *error) {
  const cai_allocator *allocator;
  struct cai_content_part *src_parts;
  struct cai_content_part *dst_parts;
  size_t i;
  int rc;

  allocator = &dst_params->allocator;
  memset(dst, 0, sizeof(*dst));
  dst->kind = src->kind;
  dst->role = cai_strdup(allocator, src->role);
  dst->call_id = cai_strdup(allocator, src->call_id);
  dst->output = cai_strdup(allocator, src->output);
  cai_object_array_init(&dst->content, sizeof(struct cai_content_part));
  if ((src->role != NULL && dst->role == NULL) ||
      (src->call_id != NULL && dst->call_id == NULL) ||
      (src->output != NULL && dst->output == NULL)) {
    cai_input_message_cleanup(allocator, dst);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to clone input message");
  }
  if (src->has_output_spooled) {
    rc = cai_response_spooled_clone(&src->output_spooled,
                                    &dst->output_spooled, error);
    if (rc != CAI_OK) {
      cai_input_message_cleanup(allocator, dst);
      return rc;
    }
    dst->has_output_spooled = 1;
  }
  src_parts = (struct cai_content_part *)src->content.items;
  for (i = 0U; i < src->content.count; i++) {
    rc = cai_object_array_grow(allocator, &dst->content,
                               sizeof(struct cai_content_part), error);
    if (rc != CAI_OK) {
      cai_input_message_cleanup(allocator, dst);
      return rc;
    }
    dst_parts = (struct cai_content_part *)dst->content.items;
    rc = cai_response_content_part_clone(allocator, &dst_parts[dst->content.count],
                                         &src_parts[i], error);
    if (rc != CAI_OK) {
      cai_input_message_cleanup(allocator, dst);
      return rc;
    }
    dst->content.count++;
  }
  return CAI_OK;
}

static int cai_response_function_tool_clone(const cai_allocator *allocator,
                                            struct cai_function_tool *dst,
                                            const struct cai_function_tool *src,
                                            cai_error *error) {
  memset(dst, 0, sizeof(*dst));
  dst->name = cai_strdup(allocator, src->name);
  dst->description = cai_strdup(allocator, src->description);
  dst->parameters_json = cai_strdup(allocator, src->parameters_json);
  dst->raw_json = cai_strdup(allocator, src->raw_json);
  dst->strict = src->strict;
  dst->is_raw = src->is_raw;
  if ((src->name != NULL && dst->name == NULL) ||
      (src->description != NULL && dst->description == NULL) ||
      (src->parameters_json != NULL && dst->parameters_json == NULL) ||
      (src->raw_json != NULL && dst->raw_json == NULL)) {
    cai_function_tool_cleanup(allocator, dst);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to clone function tool");
  }
  return CAI_OK;
}

int cai_response_create_params_clone(const cai_response_create_params *params,
                                     cai_response_create_params **out,
                                     cai_error *error) {
  cai_response_create_params *clone;
  struct cai_input_message *src_messages;
  struct cai_input_message *dst_messages;
  struct cai_function_tool *src_tools;
  struct cai_function_tool *dst_tools;
  size_t i;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params output pointer is required");
  }
  *out = NULL;
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  rc = cai_response_create_params_new(&clone, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_response_create_params_set_model(clone, params->model, error);
  if (rc == CAI_OK) {
    rc = cai_response_create_params_set_conversation_id(
        clone, params->conversation_id, error);
  }
  if (rc == CAI_OK) {
    rc = cai_response_create_params_set_instructions(clone,
                                                     params->instructions,
                                                     error);
  }
  if (rc == CAI_OK) {
    rc = cai_response_create_params_set_previous_response_id(
        clone, params->previous_response_id, error);
  }
  if (rc == CAI_OK) {
    rc = cai_response_create_params_set_prompt_cache_key(
        clone, params->prompt_cache_key, error);
  }
  if (rc == CAI_OK) {
    rc = cai_response_create_params_set_service_tier(clone,
                                                     params->service_tier,
                                                     error);
  }
  if (rc == CAI_OK) {
    rc = cai_response_create_params_set_truncation(clone, params->truncation,
                                                   error);
  }
  if (rc == CAI_OK) {
    rc = cai_response_create_params_set_metadata_json(clone,
                                                      params->metadata_json,
                                                      error);
  }
  if (rc == CAI_OK) {
    rc = cai_response_create_params_set_include_json(clone, params->include_json,
                                                     error);
  }
  if (rc == CAI_OK) {
    rc = cai_response_create_params_set_prompt_json(clone, params->prompt_json,
                                                    error);
  }
  if (rc == CAI_OK) {
    if (params->tool_choice_json != NULL) {
      rc = cai_response_create_params_set_tool_choice_json(
          clone, params->tool_choice_json, error);
    } else {
      rc = cai_response_create_params_set_tool_choice(
          clone, params->tool_choice, error);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_response_create_params_set_reasoning(
        clone, params->reasoning_effort, params->reasoning_summary, error);
  }
  if (rc == CAI_OK && params->text_format_type != NULL) {
    if (strcmp(params->text_format_type, "json_schema") == 0) {
      rc = cai_response_create_params_set_text_format_json_schema(
          clone, params->text_format_name, params->text_format_description,
          params->text_format_schema_json, params->text_format_strict, error);
    } else if (strcmp(params->text_format_type, "json_object") == 0) {
      rc = cai_response_create_params_set_text_format_json_object(clone, error);
    } else {
      rc = cai_response_params_set_text_format_type(
          clone, params->text_format_type, error);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_response_create_params_set_text_verbosity(
        clone, params->text_verbosity, error);
  }
  if (rc == CAI_OK) {
    clone->max_output_tokens = params->max_output_tokens;
    clone->max_tool_calls = params->max_tool_calls;
    clone->parallel_tool_calls = params->parallel_tool_calls;
    clone->compact_threshold_tokens = params->compact_threshold_tokens;
    clone->background = params->background;
    clone->has_background = params->has_background;
    clone->store = params->store;
    clone->has_store = params->has_store;
  }
  if (rc == CAI_OK && params->raw_input_json != NULL) {
    rc = cai_response_create_params_set_raw_input_json(
        clone, params->raw_input_json, error);
  }
  if (rc == CAI_OK && params->has_raw_input_spooled) {
    rc = cai_response_spooled_clone(&params->raw_input_spooled,
                                    &clone->raw_input_spooled, error);
    if (rc == CAI_OK) {
      clone->has_raw_input_spooled = 1;
    }
  }
  src_messages = (struct cai_input_message *)params->input.items;
  for (i = 0U; rc == CAI_OK && i < params->input.count; i++) {
    rc = cai_object_array_grow(&clone->allocator, &clone->input,
                               sizeof(struct cai_input_message), error);
    if (rc == CAI_OK) {
      dst_messages = (struct cai_input_message *)clone->input.items;
      rc = cai_response_input_message_clone(
          clone, &dst_messages[clone->input.count], &src_messages[i], error);
      if (rc == CAI_OK) {
        clone->input.count++;
      }
    }
  }
  src_tools = (struct cai_function_tool *)params->tools.items;
  for (i = 0U; rc == CAI_OK && i < params->tools.count; i++) {
    rc = cai_object_array_grow(&clone->allocator, &clone->tools,
                               sizeof(struct cai_function_tool), error);
    if (rc == CAI_OK) {
      dst_tools = (struct cai_function_tool *)clone->tools.items;
      rc = cai_response_function_tool_clone(&clone->allocator,
                                            &dst_tools[clone->tools.count],
                                            &src_tools[i], error);
      if (rc == CAI_OK) {
        clone->tools.count++;
      }
    }
  }
  if (rc != CAI_OK) {
    cai_response_create_params_destroy(clone);
    return rc;
  }
  *out = clone;
  return CAI_OK;
}

int cai_response_create_params_add_function_call_output(
    cai_response_create_params *params, const char *call_id, const char *output,
    cai_error *error) {
  lonejson_spooled spooled;
  lonejson_error json_error;
  int rc;

  if (output == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "function call id and output are required");
  }
  lonejson_error_init(&json_error);
  CAI_LJ->spooled_init(CAI_LJ, &spooled);
  if (spooled.append(&spooled, output, strlen(output), &json_error) !=
      LONEJSON_STATUS_OK) {
    spooled.cleanup(&spooled);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to spool function call output",
                                json_error.message);
  }
  rc = cai_response_create_params_add_function_call_output_spooled(
      params, call_id, &spooled, error);
  if (rc != CAI_OK) {
    spooled.cleanup(&spooled);
  }
  return rc;
}

int cai_response_create_params_add_function_call_output_spooled(
    cai_response_create_params *params, const char *call_id,
    lonejson_spooled *output, cai_error *error) {
  struct cai_input_message *messages;
  struct cai_input_message *message;
  int rc;

  if (params == NULL || call_id == NULL || call_id[0] == '\0' ||
      output == NULL) {
    if (output != NULL) {
      output->cleanup(output);
    }
    return cai_set_error(error, CAI_ERR_INVALID,
                         "function call id and output are required");
  }
  rc = cai_object_array_grow(&params->allocator, &params->input,
                             sizeof(struct cai_input_message), error);
  if (rc != CAI_OK) {
    output->cleanup(output);
    return rc;
  }
  messages = (struct cai_input_message *)params->input.items;
  message = &messages[params->input.count];
  memset(message, 0, sizeof(*message));
  message->kind = CAI_INPUT_FUNCTION_CALL_OUTPUT;
  cai_object_array_init(&message->content, sizeof(struct cai_content_part));
  message->call_id = cai_strdup(&params->allocator, call_id);
  message->output_spooled = *output;
  message->has_output_spooled = 1;
  memset(output, 0, sizeof(*output));
  if (message->call_id == NULL) {
    cai_input_message_cleanup(&params->allocator, message);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate function call output");
  }
  params->input.count++;
  return CAI_OK;
}

static int cai_response_params_add_function_output_part(
    cai_response_create_params *params, const char *call_id,
    struct cai_content_part *part, cai_error *error) {
  struct cai_input_message *messages;
  struct cai_input_message *message;
  struct cai_content_part *parts;
  int rc;

  if (params == NULL || call_id == NULL || call_id[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "function call id is required");
  }
  rc = cai_object_array_grow(&params->allocator, &params->input,
                             sizeof(struct cai_input_message), error);
  if (rc != CAI_OK) {
    return rc;
  }
  messages = (struct cai_input_message *)params->input.items;
  message = &messages[params->input.count];
  memset(message, 0, sizeof(*message));
  message->kind = CAI_INPUT_FUNCTION_CALL_OUTPUT;
  cai_object_array_init(&message->content, sizeof(struct cai_content_part));
  message->call_id = cai_strdup(&params->allocator, call_id);
  if (message->call_id == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate function call output");
  }
  rc = cai_object_array_grow(&params->allocator, &message->content,
                             sizeof(struct cai_content_part), error);
  if (rc != CAI_OK) {
    cai_input_message_cleanup(&params->allocator, message);
    return rc;
  }
  parts = (struct cai_content_part *)message->content.items;
  parts[0] = *part;
  message->content.count = 1U;
  params->input.count++;
  return CAI_OK;
}

int cai_response_create_params_add_function_call_output_text(
    cai_response_create_params *params, const char *call_id, const char *text,
    cai_error *error) {
  struct cai_content_part part;
  const cai_allocator *allocator;
  int rc;

  if (text == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "function output text is required");
  }
  allocator = params != NULL ? &params->allocator : NULL;
  cai_content_part_zero(&part);
  part.type = cai_strdup(allocator, "input_text");
  part.text = cai_strdup(allocator, text);
  if (part.type == NULL || part.text == NULL) {
    cai_content_part_cleanup(allocator, &part);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate function output text");
  }
  rc = cai_response_params_add_function_output_part(params, call_id, &part,
                                                    error);
  if (rc != CAI_OK) {
    cai_content_part_cleanup(allocator, &part);
  }
  return rc;
}

int cai_response_create_params_add_function_call_output_image_url(
    cai_response_create_params *params, const char *call_id, const char *url,
    const char *detail, cai_error *error) {
  struct cai_content_part part;
  const cai_allocator *allocator;
  int rc;

  if (url == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "function output image URL is required");
  }
  allocator = params != NULL ? &params->allocator : NULL;
  cai_content_part_zero(&part);
  part.type = cai_strdup(allocator, "input_image");
  part.image_url = cai_strdup(allocator, url);
  part.detail = cai_strdup(allocator, detail);
  if (part.type == NULL || part.image_url == NULL ||
      (detail != NULL && part.detail == NULL)) {
    cai_content_part_cleanup(allocator, &part);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate function output image");
  }
  rc = cai_response_params_add_function_output_part(params, call_id, &part,
                                                    error);
  if (rc != CAI_OK) {
    cai_content_part_cleanup(allocator, &part);
  }
  return rc;
}

int cai_response_create_params_add_function_call_output_file_id(
    cai_response_create_params *params, const char *call_id,
    const char *file_id, const char *detail, cai_error *error) {
  struct cai_content_part part;
  const cai_allocator *allocator;
  int rc;

  if (file_id == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "function output file id is required");
  }
  allocator = params != NULL ? &params->allocator : NULL;
  cai_content_part_zero(&part);
  part.type = cai_strdup(allocator, "input_file");
  part.file_id = cai_strdup(allocator, file_id);
  part.detail = cai_strdup(allocator, detail);
  if (part.type == NULL || part.file_id == NULL ||
      (detail != NULL && part.detail == NULL)) {
    cai_content_part_cleanup(allocator, &part);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate function output file");
  }
  rc = cai_response_params_add_function_output_part(params, call_id, &part,
                                                    error);
  if (rc != CAI_OK) {
    cai_content_part_cleanup(allocator, &part);
  }
  return rc;
}

int cai_response_create_params_add_function_call_output_file_data_spooled(
    cai_response_create_params *params, const char *call_id,
    const char *filename, struct lonejson_spooled *file_data,
    const char *detail, cai_error *error) {
  struct cai_content_part part;
  const cai_allocator *allocator;
  int rc;

  if (file_data == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "function output file data spool is required");
  }
  allocator = params != NULL ? &params->allocator : NULL;
  cai_content_part_zero(&part);
  part.type = cai_strdup(allocator, "input_file");
  part.filename = cai_strdup(allocator, filename);
  part.detail = cai_strdup(allocator, detail);
  part.file_data = *file_data;
  part.has_file_data = 1;
  memset(file_data, 0, sizeof(*file_data));
  if (part.type == NULL || (filename != NULL && part.filename == NULL) ||
      (detail != NULL && part.detail == NULL)) {
    cai_content_part_cleanup(allocator, &part);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate function output file data");
  }
  rc = cai_response_params_add_function_output_part(params, call_id, &part,
                                                    error);
  if (rc != CAI_OK) {
    cai_content_part_cleanup(allocator, &part);
  }
  return rc;
}

static void cai_request_content_part_docs_cleanup(
    cai_request_content_part_doc *docs, size_t count) {
  size_t i;

  if (docs == NULL) {
    return;
  }
  for (i = 0U; i < count; i++) {
    CAI_LJ->json_value_cleanup(CAI_LJ, &docs[i].file_data);
    if (docs[i].has_file_data_json) {
      docs[i].file_data_json.cleanup(&docs[i].file_data_json);
    }
    CAI_LJ->json_value_cleanup(CAI_LJ, &docs[i].text);
    if (docs[i].has_text_json) {
      docs[i].text_json.cleanup(&docs[i].text_json);
    }
  }
  cai_free_mem(NULL, docs);
}

static void cai_request_input_item_docs_cleanup(
    cai_request_input_item_doc *docs, size_t count) {
  size_t i;

  if (docs == NULL) {
    return;
  }
  for (i = 0U; i < count; i++) {
    if (docs[i].content.items != NULL) {
      cai_request_content_part_docs_cleanup(
          (cai_request_content_part_doc *)docs[i].content.items,
          docs[i].content.count);
    }
    CAI_LJ->json_value_cleanup(CAI_LJ, &docs[i].output);
    if (docs[i].has_output_json) {
      docs[i].output_json.cleanup(&docs[i].output_json);
    }
  }
  cai_free_mem(NULL, docs);
}

static int cai_spooled_json_string_from_cstr(const char *value,
                                             lonejson_spooled *out,
                                             cai_error *error) {
  cai_buffer_builder builder;
  lonejson_error json_error;
  int rc;

  lonejson_error_init(&json_error);
  CAI_LJ->spooled_init(CAI_LJ, out);
  builder.data = NULL;
  builder.length = 0U;
  builder.capacity = 0U;
  builder.sink = cai_spooled_lonejson_sink;
  builder.sink_user = out;
  builder.sink_error = &json_error;
  rc = cai_buffer_append_json_string(&builder, value, error);
  if (rc != CAI_OK) {
    out->cleanup(out);
  }
  return rc;
}

static int cai_spooled_json_string_from_spooled(const lonejson_spooled *value,
                                                lonejson_spooled *out,
                                                cai_error *error) {
  cai_buffer_builder builder;
  lonejson_error json_error;
  int rc;

  lonejson_error_init(&json_error);
  CAI_LJ->spooled_init(CAI_LJ, out);
  builder.data = NULL;
  builder.length = 0U;
  builder.capacity = 0U;
  builder.sink = cai_spooled_lonejson_sink;
  builder.sink_user = out;
  builder.sink_error = &json_error;
  rc = cai_buffer_append_json_string_spooled(&builder, value, error);
  if (rc != CAI_OK) {
    out->cleanup(out);
  }
  return rc;
}

static int cai_spool_request_input_item_array(cai_request_input_item_doc *items,
                                              size_t count,
                                              lonejson_spooled *out,
                                              size_t *out_len,
                                              cai_error *error) {
  return cai_spool_mapped_object_array(
      &cai_request_input_item_map, items, count, sizeof(*items), out, out_len,
      "failed to serialize input items JSON", error);
}

static int cai_spool_request_content_part_array(
    cai_request_content_part_doc *items, size_t count, lonejson_spooled *out,
    size_t *out_len, cai_error *error) {
  return cai_spool_mapped_object_array(
      &cai_request_content_part_map, items, count, sizeof(*items), out, out_len,
      "failed to serialize content parts JSON", error);
}

static int cai_build_request_content_part_docs(
    const lonejson_object_array *content, cai_request_content_part_doc **out,
    size_t *out_count, cai_error *error) {
  struct cai_content_part *parts;
  cai_request_content_part_doc *docs;
  lonejson_error json_error;
  size_t i;

  *out = NULL;
  *out_count = 0U;
  if (content == NULL || content->count == 0U) {
    return CAI_OK;
  }
  docs = (cai_request_content_part_doc *)cai_alloc(
      NULL, content->count * sizeof(*docs));
  if (docs == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate content item docs");
  }
  memset(docs, 0, content->count * sizeof(*docs));
  parts = (struct cai_content_part *)content->items;
  for (i = 0U; i < content->count; i++) {
    int rc;

    docs[i].type = parts[i].type;
    docs[i].image_url = parts[i].image_url;
    docs[i].file_id = parts[i].file_id;
    docs[i].filename = parts[i].filename;
    docs[i].file_url = parts[i].file_url;
    docs[i].detail = parts[i].detail;
    CAI_LJ->json_value_init(CAI_LJ, &docs[i].text);
    CAI_LJ->json_value_init(CAI_LJ, &docs[i].file_data);
    if (parts[i].has_text_spooled || parts[i].text != NULL) {
      if (parts[i].has_text_spooled) {
        rc = cai_spooled_json_string_from_spooled(&parts[i].text_spooled,
                                                  &docs[i].text_json, error);
      } else {
        rc = cai_spooled_json_string_from_cstr(parts[i].text,
                                               &docs[i].text_json, error);
      }
      if (rc != CAI_OK) {
        cai_request_content_part_docs_cleanup(docs, i + 1U);
        return rc;
      }
      docs[i].has_text_json = 1;
      docs[i].text_reader.cursor = docs[i].text_json;
      lonejson_error_init(&json_error);
      if (docs[i].text.methods->set_reader(&docs[i].text, cai_response_spooled_reader,
                                  &docs[i].text_reader,
                                  &json_error) != LONEJSON_STATUS_OK) {
        cai_request_content_part_docs_cleanup(docs, i + 1U);
        return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                    "failed to prepare text JSON",
                                    json_error.message);
      }
    }
    if (parts[i].has_file_data) {
      rc = cai_spooled_json_string_from_spooled(&parts[i].file_data,
                                                &docs[i].file_data_json,
                                                error);
      if (rc != CAI_OK) {
        cai_request_content_part_docs_cleanup(docs, i + 1U);
        return rc;
      }
      docs[i].has_file_data_json = 1;
      docs[i].file_data_reader.cursor = docs[i].file_data_json;
      lonejson_error_init(&json_error);
      if (docs[i].file_data.methods->set_reader(&docs[i].file_data,
                                       cai_response_spooled_reader,
                                       &docs[i].file_data_reader,
                                       &json_error) != LONEJSON_STATUS_OK) {
        cai_request_content_part_docs_cleanup(docs, i + 1U);
        return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                    "failed to prepare file data JSON",
                                    json_error.message);
      }
    }
    *out_count = i + 1U;
  }
  *out = docs;
  return CAI_OK;
}

static int cai_spool_request_content_parts(
    const lonejson_object_array *content, lonejson_spooled *out,
    size_t *out_len, cai_error *error) {
  cai_request_content_part_doc *docs;
  size_t count;
  int rc;

  docs = NULL;
  count = 0U;
  rc = cai_build_request_content_part_docs(content, &docs, &count, error);
  if (rc == CAI_OK) {
    rc = cai_spool_request_content_part_array(docs, count, out, out_len,
                                              error);
  }
  cai_request_content_part_docs_cleanup(docs, count);
  return rc;
}

static int cai_build_request_input_item_docs(
    const lonejson_object_array *input, cai_request_input_item_doc **out,
    size_t *out_count, cai_error *error) {
  struct cai_input_message *messages;
  cai_request_input_item_doc *docs;
  lonejson_error json_error;
  size_t i;

  *out = NULL;
  *out_count = 0U;
  if (input == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "input messages are required");
  }
  if (input->count == 0U) {
    return CAI_OK;
  }
  docs = (cai_request_input_item_doc *)cai_alloc(NULL,
                                                 input->count * sizeof(*docs));
  if (docs == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate input item docs");
  }
  memset(docs, 0, input->count * sizeof(*docs));
  messages = (struct cai_input_message *)input->items;
  for (i = 0U; i < input->count; i++) {
    int rc;

    CAI_LJ->json_value_init(CAI_LJ, &docs[i].output);
    if (messages[i].kind == CAI_INPUT_FUNCTION_CALL_OUTPUT) {
      docs[i].type = "function_call_output";
      docs[i].call_id = messages[i].call_id;
      if (messages[i].content.count > 0U) {
        rc = cai_spool_request_content_parts(&messages[i].content,
                                             &docs[i].output_json, NULL,
                                             error);
      } else if (messages[i].has_output_spooled) {
        rc = cai_spooled_json_string_from_spooled(&messages[i].output_spooled,
                                                  &docs[i].output_json,
                                                  error);
      } else {
        rc = cai_spooled_json_string_from_cstr(messages[i].output,
                                              &docs[i].output_json, error);
      }
      if (rc != CAI_OK) {
        cai_request_input_item_docs_cleanup(docs, i + 1U);
        return rc;
      }
      docs[i].has_output_json = 1;
      docs[i].output_reader.cursor = docs[i].output_json;
      lonejson_error_init(&json_error);
      if (docs[i].output.methods->set_reader(&docs[i].output,
                                    cai_response_spooled_reader,
                                    &docs[i].output_reader,
                                    &json_error) != LONEJSON_STATUS_OK) {
        cai_request_input_item_docs_cleanup(docs, i + 1U);
        return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                    "failed to prepare function output JSON",
                                    json_error.message);
      }
    } else {
      cai_request_content_part_doc *content_docs;
      size_t content_count;

      content_docs = NULL;
      content_count = 0U;
      docs[i].role = messages[i].role;
      rc = cai_build_request_content_part_docs(&messages[i].content,
                                               &content_docs, &content_count,
                                               error);
      if (rc != CAI_OK) {
        cai_request_input_item_docs_cleanup(docs, i + 1U);
        return rc;
      }
      docs[i].content.items = content_docs;
      docs[i].content.count = content_count;
      docs[i].content.capacity = content_count;
      docs[i].content.elem_size = sizeof(cai_request_content_part_doc);
      docs[i].content.flags = LONEJSON_ARRAY_FIXED_CAPACITY;
    }
    *out_count = i + 1U;
  }
  *out = docs;
  return CAI_OK;
}

static int cai_spool_request_input_items(
    const lonejson_object_array *input, lonejson_spooled *out, size_t *out_len,
    cai_error *error) {
  cai_request_input_item_doc *docs;
  size_t count;
  int rc;

  docs = NULL;
  count = 0U;
  rc = cai_build_request_input_item_docs(input, &docs, &count, error);
  if (rc == CAI_OK) {
    rc = cai_spool_request_input_item_array(docs, count, out, out_len, error);
  }
  cai_request_input_item_docs_cleanup(docs, count);
  return rc;
}

int cai_input_messages_spool_json_array(const lonejson_object_array *input,
                                        lonejson_spooled *out,
                                        size_t *out_len, cai_error *error) {
  cai_request_input_item_doc *docs;
  size_t count;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input JSON array spool output pointer is required");
  }
  docs = NULL;
  count = 0U;
  rc = cai_build_request_input_item_docs(input, &docs, &count, error);
  if (rc == CAI_OK) {
    rc = cai_spool_request_input_item_array(docs, count, out, out_len, error);
  }
  cai_request_input_item_docs_cleanup(docs, count);
  return rc;
}

int cai_response_params_input_items_json(
    const cai_response_create_params *params, char **out_json,
    cai_error *error) {
  lonejson_spooled spooled;
  cai_buffer_builder builder;
  int rc;

  if (out_json == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input JSON output pointer is required");
  }
  *out_json = NULL;
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  memset(&spooled, 0, sizeof(spooled));
  rc = cai_response_params_input_items_spool(params, &spooled, NULL, error);
  if (rc != CAI_OK) {
    return rc;
  }
  builder.data = NULL;
  builder.length = 0U;
  builder.capacity = 0U;
  builder.sink = NULL;
  builder.sink_user = NULL;
  builder.sink_error = NULL;
  rc = cai_spooled_copy_to_builder(&builder, &spooled, error);
  spooled.cleanup(&spooled);
  if (rc == CAI_OK) {
    *out_json = builder.data;
  } else {
    cai_free_mem(NULL, builder.data);
  }
  return rc;
}

int cai_response_params_input_items_spool(
    const cai_response_create_params *params, lonejson_spooled *out,
    size_t *out_len, cai_error *error) {
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input JSON spool output pointer is required");
  }
  if (params == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response params are required");
  }
  rc = cai_spool_request_input_items(&params->input, out, out_len, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return CAI_OK;
}

static int
cai_response_create_params_write_json(const cai_response_create_params *params,
                                      cai_buffer_builder *builder, int stream,
                                      cai_error *error) {
  cai_response_buffer_sink_context sink_context;

  sink_context.builder = builder;
  sink_context.error = error;
  return cai_response_create_params_write_json_sink(
      params, stream, cai_response_buffer_sink, &sink_context, NULL,
      NULL, error);
}

static int cai_response_create_params_build_input_json(
    const cai_response_create_params *params, lonejson_spooled *out,
    cai_response_spooled_reader_context *reader_context, cai_error *error) {
  lonejson_writer writer;
  lonejson_error json_error;
  int has_raw_input;
  int raw_is_array;
  lonejson_spooled raw_input;
  lonejson_spooled typed_input;
  int has_raw_input_spool;
  int has_typed_input;
  lonejson_status status;

  has_raw_input =
      params->has_raw_input_spooled ||
      (params->raw_input_json != NULL && params->raw_input_json[0] != '\0');
  lonejson_error_init(&json_error);
  CAI_LJ->spooled_init(CAI_LJ, out);
  memset(&raw_input, 0, sizeof(raw_input));
  memset(&typed_input, 0, sizeof(typed_input));
  has_raw_input_spool = 0;
  has_typed_input = 0;
  status = CAI_LJ->writer_init_sink(CAI_LJ, &writer, cai_spooled_lonejson_sink, out, &json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = writer.begin_array(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK && has_raw_input &&
      params->has_raw_input_spooled) {
    raw_is_array = 0;
    if (cai_response_spooled_root_is_array(&params->raw_input_spooled,
                                           &raw_is_array, error) != CAI_OK) {
      status = LONEJSON_STATUS_INVALID_ARGUMENT;
    } else if (raw_is_array) {
      status = writer.array_items_spooled(&writer, "", &params->raw_input_spooled,
                                          &json_error);
    } else {
      status = writer.json_value_spooled(&writer, &params->raw_input_spooled,
                                         &json_error);
    }
  }
  if (status == LONEJSON_STATUS_OK && has_raw_input &&
      !params->has_raw_input_spooled) {
    CAI_LJ->spooled_init(CAI_LJ, &raw_input);
    has_raw_input_spool = 1;
    if (raw_input.append(&raw_input, params->raw_input_json,
                                strlen(params->raw_input_json),
                                &json_error) != LONEJSON_STATUS_OK) {
      status = LONEJSON_STATUS_CALLBACK_FAILED;
    } else if (cai_response_spooled_root_is_array(&raw_input, &raw_is_array,
                                                  error) != CAI_OK) {
      status = LONEJSON_STATUS_INVALID_ARGUMENT;
    } else if (raw_is_array) {
      status = writer.array_items_spooled(&writer, "", &raw_input,
                                                   &json_error);
    } else {
      status = writer.json_value_spooled(&writer, &raw_input,
                                                  &json_error);
    }
  }
  if (status == LONEJSON_STATUS_OK && params->input.count > 0U) {
    if (cai_spool_request_input_items(&params->input, &typed_input, NULL,
                                      error) != CAI_OK) {
      status = LONEJSON_STATUS_INVALID_ARGUMENT;
    } else {
      has_typed_input = 1;
      status = writer.array_items_spooled(&writer, "", &typed_input,
                                                   &json_error);
    }
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.end_array(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.finish(&writer, &json_error);
  }
  writer.cleanup(&writer);
  if (has_raw_input_spool) {
    raw_input.cleanup(&raw_input);
  }
  if (has_typed_input) {
    typed_input.cleanup(&typed_input);
  }
  if (status == LONEJSON_STATUS_OK) {
    reader_context->cursor = *out;
    lonejson_error_init(&json_error);
    if (reader_context->cursor.rewind(&reader_context->cursor, &json_error) !=
        LONEJSON_STATUS_OK) {
      out->cleanup(out);
      return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                  "failed to rewind input JSON",
                                  json_error.message);
    }
    return CAI_OK;
  }
  out->cleanup(out);
  if (error != NULL && error->message != NULL) {
    return error->code;
  }
  return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                              "failed to serialize input JSON",
                              json_error.message);
}

static int cai_response_create_params_build_tools_json(
    const cai_response_create_params *params, lonejson_spooled *out,
    cai_response_spooled_reader_context *reader_context, cai_error *error) {
  lonejson_writer writer;
  lonejson_error json_error;
  lonejson_status status;
  struct cai_function_tool *tools;
  size_t i;
  int writer_initialized;

  if (params == NULL || params->tools.count == 0U) {
    return cai_set_error(error, CAI_ERR_INVALID, "tools are required");
  }
  lonejson_error_init(&json_error);
  CAI_LJ->spooled_init(CAI_LJ, out);
  writer_initialized = 0;
  status = CAI_LJ->writer_init_sink(CAI_LJ, &writer, cai_spooled_lonejson_sink, out, &json_error);
  if (status == LONEJSON_STATUS_OK) {
    writer_initialized = 1;
    status = writer.begin_array(&writer, &json_error);
  }
  tools = (struct cai_function_tool *)params->tools.items;
  for (i = 0U; status == LONEJSON_STATUS_OK && i < params->tools.count; i++) {
    if (tools[i].is_raw) {
      status = writer.json_value_buffer(
          &writer, tools[i].raw_json, strlen(tools[i].raw_json), &json_error);
    } else {
      status = writer.begin_object(&writer, &json_error);
      if (status == LONEJSON_STATUS_OK) {
        status = writer.key(&writer, "type", 4U, &json_error);
      }
      if (status == LONEJSON_STATUS_OK) {
        status = writer.string(&writer, "function", 8U,
                                        &json_error);
      }
      if (status == LONEJSON_STATUS_OK) {
        status = writer.key(&writer, "name", 4U, &json_error);
      }
      if (status == LONEJSON_STATUS_OK) {
        status = writer.string(&writer, tools[i].name,
                                        strlen(tools[i].name), &json_error);
      }
      if (status == LONEJSON_STATUS_OK && tools[i].description != NULL) {
        status = writer.key(&writer, "description", 11U,
                                     &json_error);
        if (status == LONEJSON_STATUS_OK) {
          status = writer.string(&writer, tools[i].description,
                                          strlen(tools[i].description),
                                          &json_error);
        }
      }
      if (status == LONEJSON_STATUS_OK) {
        status = writer.key(&writer, "parameters", 10U,
                                     &json_error);
      }
      if (status == LONEJSON_STATUS_OK) {
        status = writer.json_value_buffer(
            &writer, tools[i].parameters_json, strlen(tools[i].parameters_json),
            &json_error);
      }
      if (status == LONEJSON_STATUS_OK) {
        status = writer.key(&writer, "strict", 6U, &json_error);
      }
      if (status == LONEJSON_STATUS_OK) {
        status = writer.bool_fn(&writer, tools[i].strict ? 1 : 0,
                                      &json_error);
      }
      if (status == LONEJSON_STATUS_OK) {
        status = writer.end_object(&writer, &json_error);
      }
    }
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.end_array(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.finish(&writer, &json_error);
  }
  if (writer_initialized) {
    writer.cleanup(&writer);
  }
  if (status != LONEJSON_STATUS_OK) {
    out->cleanup(out);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to serialize tools JSON",
                                json_error.message);
  }
  reader_context->cursor = *out;
  lonejson_error_init(&json_error);
  if (reader_context->cursor.rewind(&reader_context->cursor, &json_error) !=
      LONEJSON_STATUS_OK) {
    out->cleanup(out);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to rewind tools JSON",
                                json_error.message);
  }
  return CAI_OK;
}

int cai_response_create_params_serialize_json(
    const cai_response_create_params *params, char **out_json, size_t *out_len,
    cai_error *error) {
  cai_buffer_builder builder;
  int rc;

  if (out_json == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "JSON output pointer is required");
  }
  *out_json = NULL;
  builder.data = NULL;
  builder.length = 0U;
  builder.capacity = 0U;
  builder.sink = NULL;
  builder.sink_user = NULL;
  builder.sink_error = NULL;
  rc = cai_response_create_params_write_json(params, &builder, 0, error);
  if (rc != CAI_OK) {
    cai_free_mem(NULL, builder.data);
    return rc;
  }
  if (out_len != NULL) {
    *out_len = builder.length;
  }
  *out_json = builder.data;
  return CAI_OK;
}

int cai_response_create_params_spool_json(
    const cai_response_create_params *params, int stream, lonejson_spooled *out,
    size_t *out_len, cai_error *error) {
  cai_buffer_builder builder;
  lonejson_error json_error;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "JSON spool output pointer is required");
  }
  lonejson_error_init(&json_error);
  CAI_LJ->spooled_init(CAI_LJ, out);
  builder.data = NULL;
  builder.length = 0U;
  builder.capacity = 0U;
  builder.sink = cai_spooled_lonejson_sink;
  builder.sink_user = out;
  builder.sink_error = &json_error;
  rc = cai_response_create_params_write_json(params, &builder, stream, error);
  if (rc != CAI_OK) {
    out->cleanup(out);
    return rc;
  }
  if (out_len != NULL) {
    *out_len = builder.length;
  }
  return CAI_OK;
}

static void cai_response_request_state_init(cai_response_request_state *state) {
  memset(state, 0, sizeof(*state));
  CAI_LJ->json_value_init(CAI_LJ, &state->doc.text.format.schema);
  CAI_LJ->json_value_init(CAI_LJ, &state->doc.metadata);
  CAI_LJ->json_value_init(CAI_LJ, &state->doc.include);
  CAI_LJ->json_value_init(CAI_LJ, &state->doc.prompt);
  CAI_LJ->json_value_init(CAI_LJ, &state->doc.tool_choice);
  CAI_LJ->json_value_init(CAI_LJ, &state->doc.input);
  CAI_LJ->json_value_init(CAI_LJ, &state->doc.tools);
}

static void
cai_response_request_state_cleanup(cai_response_request_state *state) {
  CAI_LJ->json_value_cleanup(CAI_LJ, &state->doc.text.format.schema);
  CAI_LJ->json_value_cleanup(CAI_LJ, &state->doc.metadata);
  CAI_LJ->json_value_cleanup(CAI_LJ, &state->doc.include);
  CAI_LJ->json_value_cleanup(CAI_LJ, &state->doc.prompt);
  CAI_LJ->json_value_cleanup(CAI_LJ, &state->doc.tool_choice);
  CAI_LJ->json_value_cleanup(CAI_LJ, &state->doc.input);
  if (state->has_input_json) {
    state->input_json.cleanup(&state->input_json);
    state->has_input_json = 0;
  }
  if (state->has_tools_json) {
    state->tools_json.cleanup(&state->tools_json);
    state->has_tools_json = 0;
  }
}

static lonejson_status cai_response_request_count_sink(
    void *user, const void *data, size_t len, lonejson_error *error) {
  size_t *count;

  (void)data;
  (void)error;
  count = (size_t *)user;
  *count += len;
  return LONEJSON_STATUS_OK;
}

static int cai_response_request_state_prepare(
    cai_response_request_state *state, const cai_response_create_params *params,
    int stream, cai_error *error) {
  lonejson_error json_error;
  int rc;

  if (params == NULL || params->model == NULL ||
      (params->input.count == 0U && !params->has_raw_input_spooled &&
       (params->raw_input_json == NULL || params->raw_input_json[0] == '\0'))) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "model and at least one input message are required");
  }
  if (params->previous_response_id != NULL && params->conversation_id != NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "previous response id and conversation id are "
                         "mutually exclusive");
  }

  state->doc.model = params->model;
  state->doc.instructions = params->instructions;
  state->doc.previous_response_id = params->previous_response_id;
  state->doc.conversation = params->conversation_id;
  state->doc.prompt_cache_key = params->prompt_cache_key;
  state->doc.service_tier = params->service_tier;
  state->doc.truncation = params->truncation;
  if (params->metadata_json != NULL) {
    lonejson_error_init(&json_error);
    if (state->doc.metadata.methods->set_buffer(&state->doc.metadata,
                                       params->metadata_json,
                                       strlen(params->metadata_json),
                                       &json_error) != LONEJSON_STATUS_OK) {
      return cai_set_error_detail(error, CAI_ERR_INVALID,
                                  "metadata must be valid JSON",
                                  json_error.message);
    }
  }
  if (params->include_json != NULL) {
    lonejson_error_init(&json_error);
    if (state->doc.include.methods->set_buffer(&state->doc.include,
                                       params->include_json,
                                       strlen(params->include_json),
                                       &json_error) != LONEJSON_STATUS_OK) {
      return cai_set_error_detail(error, CAI_ERR_INVALID,
                                  "include must be valid JSON",
                                  json_error.message);
    }
  }
  if (params->prompt_json != NULL) {
    lonejson_error_init(&json_error);
    if (state->doc.prompt.methods->set_buffer(&state->doc.prompt, params->prompt_json,
                                       strlen(params->prompt_json),
                                       &json_error) != LONEJSON_STATUS_OK) {
      return cai_set_error_detail(error, CAI_ERR_INVALID,
                                  "prompt must be valid JSON",
                                  json_error.message);
    }
  }
  if (params->tool_choice_json != NULL) {
    lonejson_error_init(&json_error);
    if (state->doc.tool_choice.methods->set_buffer(&state->doc.tool_choice,
                                       params->tool_choice_json,
                                       strlen(params->tool_choice_json),
                                       &json_error) != LONEJSON_STATUS_OK) {
      return cai_set_error_detail(error, CAI_ERR_INVALID,
                                  "tool choice must be valid JSON",
                                  json_error.message);
    }
  } else if (params->tool_choice != NULL) {
    cai_buffer_builder choice_builder;

    choice_builder.data = NULL;
    choice_builder.length = 0U;
    choice_builder.capacity = 0U;
    choice_builder.sink = NULL;
    choice_builder.sink_user = NULL;
    choice_builder.sink_error = NULL;
    if (cai_buffer_append_json_string(&choice_builder, params->tool_choice,
                                      error) != CAI_OK) {
      cai_free_mem(NULL, choice_builder.data);
      return error != NULL && error->message != NULL ? error->code
                                                     : CAI_ERR_TRANSPORT;
    }
    lonejson_error_init(&json_error);
    if (state->doc.tool_choice.methods->set_buffer(&state->doc.tool_choice,
                                       choice_builder.data,
                                       choice_builder.length,
                                       &json_error) != LONEJSON_STATUS_OK) {
      cai_free_mem(NULL, choice_builder.data);
      return cai_set_error_detail(error, CAI_ERR_INVALID,
                                  "tool choice must be valid JSON",
                                  json_error.message);
    }
    cai_free_mem(NULL, choice_builder.data);
  }
  if (params->max_output_tokens > 0) {
    state->doc.max_output_tokens = params->max_output_tokens;
    state->doc.has_max_output_tokens = 1;
  }
  if (params->max_tool_calls > 0) {
    state->doc.max_tool_calls = params->max_tool_calls;
    state->doc.has_max_tool_calls = 1;
  }
  if (params->has_background) {
    state->doc.background = params->background ? true : false;
    state->doc.has_background = 1;
  }
  if (params->has_store) {
    state->doc.store = params->store ? true : false;
    state->doc.has_store = 1;
  }
  state->doc.reasoning.effort = params->reasoning_effort;
  state->doc.reasoning.summary = params->reasoning_summary;
  state->doc.text.format.type = params->text_format_type;
  state->doc.text.format.name = params->text_format_name;
  state->doc.text.format.description = params->text_format_description;
  state->doc.text.verbosity = params->text_verbosity;
  if (params->text_format_schema_json != NULL) {
    lonejson_error_init(&json_error);
    if (state->doc.text.format.schema.methods->set_buffer(
            &state->doc.text.format.schema, params->text_format_schema_json,
            strlen(params->text_format_schema_json),
            &json_error) != LONEJSON_STATUS_OK) {
      return cai_set_error_detail(error, CAI_ERR_INVALID,
                                  "text format schema must be valid JSON",
                                  json_error.message);
    }
    state->doc.text.format.strict = params->text_format_strict ? true : false;
    state->doc.text.format.has_strict = 1;
  }
  if (params->parallel_tool_calls >= 0) {
    state->doc.parallel_tool_calls = params->parallel_tool_calls ? true : false;
    state->doc.has_parallel_tool_calls = 1;
  }
  if (params->compact_threshold_tokens > 0LL) {
    state->context_item.type = "compaction";
    state->context_item.compact_threshold = params->compact_threshold_tokens;
    state->doc.context_management.items = &state->context_item;
    state->doc.context_management.count = 1U;
    state->doc.context_management.capacity = 1U;
    state->doc.context_management.elem_size = sizeof(state->context_item);
    state->doc.context_management.flags = LONEJSON_ARRAY_FIXED_CAPACITY;
  }
  rc = cai_response_create_params_build_input_json(
      params, &state->input_json, &state->input_reader, error);
  if (rc != CAI_OK) {
    return rc;
  }
  state->has_input_json = 1;
  lonejson_error_init(&json_error);
  if (state->doc.input.methods->set_reader(&state->doc.input,
                                     cai_response_spooled_reader,
                                     &state->input_reader,
                                     &json_error) != LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to prepare input JSON",
                                json_error.message);
  }
  if (params->tools.count > 0U) {
    rc = cai_response_create_params_build_tools_json(
        params, &state->tools_json, &state->tools_reader, error);
    if (rc != CAI_OK) {
      return rc;
    }
    state->has_tools_json = 1;
    lonejson_error_init(&json_error);
    if (state->doc.tools.methods->set_reader(&state->doc.tools,
                                       cai_response_spooled_reader,
                                       &state->tools_reader,
                                       &json_error) != LONEJSON_STATUS_OK) {
      return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                  "failed to prepare tools JSON",
                                  json_error.message);
    }
  }
  if (stream) {
    state->doc.stream = true;
    state->doc.has_stream = 1;
  }
  return CAI_OK;
}

int cai_response_create_params_write_json_sink(
    const cai_response_create_params *params, int stream, lonejson_sink_fn sink,
    void *sink_user, lonejson_error *sink_error, size_t *out_len,
    cai_error *error) {
  cai_response_request_state state;
  cai_response_request_write_context write_context;
  lonejson_error json_error;
  int rc;

  if (sink == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "JSON sink is required");
  }
  cai_response_request_state_init(&state);
  rc = cai_response_request_state_prepare(&state, params, stream, error);
  if (rc != CAI_OK) {
    cai_response_request_state_cleanup(&state);
    return rc;
  }

  write_context.sink = sink;
  write_context.sink_user = sink_user;
  write_context.sink_error = sink_error;
  write_context.length = 0U;
  lonejson_error_init(&json_error);
  rc = lonejson_serialize_sink(CAI_LJ, &cai_response_request_map, &state.doc, cai_response_request_write_sink, &write_context, &json_error) == LONEJSON_STATUS_OK
           ? CAI_OK
           : cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                  "failed to serialize response request JSON",
                                  json_error.message);
  if (out_len != NULL) {
    *out_len = write_context.length;
  }
  cai_response_request_state_cleanup(&state);
  return rc;
}

int cai_response_request_upload_open(const cai_response_create_params *params,
                                     int stream,
                                     cai_response_request_upload **out,
                                     cai_error *error) {
  cai_response_request_upload *upload;
  lonejson_error json_error;
  lonejson_status status;
  size_t request_size;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "request upload output pointer is required");
  }
  *out = NULL;
  request_size = 0U;
  lonejson_error_init(&json_error);
  rc = cai_response_create_params_write_json_sink(
      params, stream, cai_response_request_count_sink, &request_size,
      &json_error, NULL, error);
  if (rc != CAI_OK) {
    return rc;
  }
  upload = (cai_response_request_upload *)cai_alloc(NULL, sizeof(*upload));
  if (upload == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate response request upload");
  }
  memset(upload, 0, sizeof(*upload));
  upload->size = (curl_off_t)request_size;
  cai_response_request_state_init(&upload->state);
  rc = cai_response_request_state_prepare(&upload->state, params, stream,
                                          error);
  if (rc != CAI_OK) {
    cai_response_request_upload_close(upload);
    return rc;
  }
  status = lonejson_curl_upload_init(&upload->curl, CAI_LJ,
                                     &cai_response_request_map,
                                     &upload->state.doc);
  if (status != LONEJSON_STATUS_OK) {
    rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                              "failed to prepare response request upload",
                              upload->curl.generator.error.message);
    cai_response_request_upload_close(upload);
    return rc;
  }
  upload->curl_started = 1;
  *out = upload;
  return CAI_OK;
}

size_t cai_response_request_upload_read(char *ptr, size_t size, size_t nmemb,
                                        void *userdata) {
  cai_response_request_upload *upload;

  upload = (cai_response_request_upload *)userdata;
  if (upload == NULL) {
    return CURL_READFUNC_ABORT;
  }
  return lonejson_curl_read_callback(ptr, size, nmemb, &upload->curl);
}

curl_off_t cai_response_request_upload_size(
    const cai_response_request_upload *upload) {
  if (upload == NULL) {
    return (curl_off_t)-1;
  }
  return upload->size;
}

void cai_response_request_upload_close(cai_response_request_upload *upload) {
  if (upload == NULL) {
    return;
  }
  if (upload->curl_started) {
    lonejson_curl_upload_cleanup(&upload->curl);
    upload->curl_started = 0;
  }
  cai_response_request_state_cleanup(&upload->state);
  cai_free_mem(NULL, upload);
}

static int cai_response_copy_spooled_string(lonejson_spooled *value,
                                            char **cursor, cai_error *error) {
  lonejson_spooled reader;
  lonejson_error json_error;
  lonejson_read_result result;
  unsigned char buffer[4096];

  reader = *value;
  lonejson_error_init(&json_error);
  if (reader.rewind(&reader, &json_error) != LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to read spooled response text",
                                json_error.message);
  }
  do {
    result = reader.read(&reader, buffer, sizeof(buffer));
    if (result.error_code != 0) {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "failed to read spooled response text");
    }
    if (result.bytes_read > 0U) {
      memcpy(*cursor, buffer, result.bytes_read);
      *cursor += result.bytes_read;
    }
  } while (!result.eof);
  return CAI_OK;
}

static char *cai_response_collect_text(const cai_allocator *allocator,
                                       cai_response_doc *doc,
                                       cai_error *error) {
  cai_response_output_doc *outputs;
  cai_response_content_doc *content;
  size_t total;
  size_t i;
  size_t j;
  char *text;
  char *cursor;
  size_t len;

  total = 0U;
  outputs = (cai_response_output_doc *)doc->output.items;
  for (i = 0U; i < doc->output.count; i++) {
    content = (cai_response_content_doc *)outputs[i].content.items;
    for (j = 0U; j < outputs[i].content.count; j++) {
      len = content[j].text.size_fn(&content[j].text);
      if (len > 0U) {
        if (len > ((size_t)-1) - total - 1U) {
          cai_set_error(error, CAI_ERR_NOMEM,
                        "response output text is too large");
          return NULL;
        }
        total += len;
      }
    }
  }
  text = (char *)cai_alloc(allocator, total + 1U);
  if (text == NULL) {
    cai_set_error(error, CAI_ERR_NOMEM,
                  "failed to allocate response output text");
    return NULL;
  }
  cursor = text;
  for (i = 0U; i < doc->output.count; i++) {
    content = (cai_response_content_doc *)outputs[i].content.items;
    for (j = 0U; j < outputs[i].content.count; j++) {
      len = content[j].text.size_fn(&content[j].text);
      if (len > 0U && cai_response_copy_spooled_string(
                          &content[j].text, &cursor, error) != CAI_OK) {
        cai_free_mem(allocator, text);
        return NULL;
      }
    }
  }
  *cursor = '\0';
  return text;
}

static char *cai_response_collect_refusal(const cai_allocator *allocator,
                                          cai_response_doc *doc, int *present,
                                          cai_error *error) {
  cai_response_output_doc *outputs;
  cai_response_content_doc *content;
  size_t total;
  size_t i;
  size_t j;
  char *text;
  char *cursor;
  size_t len;

  *present = 0;
  total = 0U;
  outputs = (cai_response_output_doc *)doc->output.items;
  for (i = 0U; i < doc->output.count; i++) {
    content = (cai_response_content_doc *)outputs[i].content.items;
    for (j = 0U; j < outputs[i].content.count; j++) {
      len = content[j].refusal.size_fn(&content[j].refusal);
      if (len > 0U) {
        *present = 1;
        if (len > ((size_t)-1) - total - 1U) {
          cai_set_error(error, CAI_ERR_NOMEM,
                        "response refusal text is too large");
          return NULL;
        }
        total += len;
      }
    }
  }
  if (!*present) {
    return NULL;
  }
  text = (char *)cai_alloc(allocator, total + 1U);
  if (text == NULL) {
    cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate response refusal");
    return NULL;
  }
  cursor = text;
  for (i = 0U; i < doc->output.count; i++) {
    content = (cai_response_content_doc *)outputs[i].content.items;
    for (j = 0U; j < outputs[i].content.count; j++) {
      len = content[j].refusal.size_fn(&content[j].refusal);
      if (len > 0U && cai_response_copy_spooled_string(
                          &content[j].refusal, &cursor, error) != CAI_OK) {
        cai_free_mem(allocator, text);
        return NULL;
      }
    }
  }
  *cursor = '\0';
  return text;
}

static size_t cai_response_count_tool_calls(cai_response_doc *doc) {
  cai_response_output_doc *outputs;
  size_t count;
  size_t i;

  count = 0U;
  outputs = (cai_response_output_doc *)doc->output.items;
  for (i = 0U; i < doc->output.count; i++) {
    if (outputs[i].type != NULL &&
        strcmp(outputs[i].type, "function_call") == 0) {
      count++;
    }
  }
  return count;
}

static int cai_response_copy_tool_calls(cai_response *response,
                                        cai_response_doc *doc,
                                        cai_error *error) {
  cai_response_output_doc *outputs;
  size_t index;
  size_t i;

  response->tool_call_count = cai_response_count_tool_calls(doc);
  response->tool_calls = NULL;
  if (response->tool_call_count == 0U) {
    return CAI_OK;
  }
  response->tool_calls = (cai_response_tool_call *)cai_alloc(
      &response->allocator,
      response->tool_call_count * sizeof(response->tool_calls[0]));
  if (response->tool_calls == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate response tool calls");
  }
  memset(response->tool_calls, 0,
         response->tool_call_count * sizeof(response->tool_calls[0]));
  outputs = (cai_response_output_doc *)doc->output.items;
  index = 0U;
  for (i = 0U; i < doc->output.count; i++) {
    if (outputs[i].type == NULL ||
        strcmp(outputs[i].type, "function_call") != 0) {
      continue;
    }
    response->tool_calls[index].id =
        cai_strdup(&response->allocator, outputs[i].id);
    response->tool_calls[index].call_id =
        cai_strdup(&response->allocator, outputs[i].call_id);
    response->tool_calls[index].name =
        cai_strdup(&response->allocator, outputs[i].name);
    if (outputs[i].arguments.size_fn(&outputs[i].arguments) > 0U) {
      int clone_rc;

      clone_rc = cai_response_spooled_clone(
          &outputs[i].arguments,
          &response->tool_calls[index].arguments_spooled, error);
      if (clone_rc != CAI_OK) {
        return clone_rc;
      }
      response->tool_calls[index].has_arguments_spooled = 1;
      if (outputs[i].arguments.size_fn(&outputs[i].arguments) <=
          CAI_INLINE_TOOL_ARGUMENTS_LIMIT) {
        int copy_rc;

        copy_rc = cai_spooled_copy_to_cstr(&response->allocator,
                                           &outputs[i].arguments,
                                           &response->tool_calls[index]
                                                .arguments,
                                           error);
        if (copy_rc != CAI_OK) {
          return copy_rc;
        }
      }
    }
    if ((outputs[i].id != NULL && response->tool_calls[index].id == NULL) ||
        (outputs[i].call_id != NULL &&
         response->tool_calls[index].call_id == NULL) ||
        (outputs[i].name != NULL && response->tool_calls[index].name == NULL)) {
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate response tool call fields");
    }
    index++;
  }
  return CAI_OK;
}

static int cai_response_copy_output_items(cai_response *response,
                                          cai_response_doc *doc,
                                          cai_error *error) {
  cai_response_output_doc *outputs;
  size_t i;

  response->output_item_count = doc->output.count;
  response->output_items = NULL;
  if (response->output_item_count == 0U) {
    return CAI_OK;
  }
  response->output_items = (cai_response_output_item *)cai_alloc(
      &response->allocator,
      response->output_item_count * sizeof(response->output_items[0]));
  if (response->output_items == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate response output items");
  }
  memset(response->output_items, 0,
         response->output_item_count * sizeof(response->output_items[0]));
  outputs = (cai_response_output_doc *)doc->output.items;
  for (i = 0U; i < doc->output.count; i++) {
    response->output_items[i].id =
        cai_strdup(&response->allocator, outputs[i].id);
    response->output_items[i].type =
        cai_strdup(&response->allocator, outputs[i].type);
    response->output_items[i].status =
        cai_strdup(&response->allocator, outputs[i].status);
    response->output_items[i].role =
        cai_strdup(&response->allocator, outputs[i].role);
    response->output_items[i].call_id =
        cai_strdup(&response->allocator, outputs[i].call_id);
    response->output_items[i].name =
        cai_strdup(&response->allocator, outputs[i].name);
    if ((outputs[i].id != NULL && response->output_items[i].id == NULL) ||
        (outputs[i].type != NULL && response->output_items[i].type == NULL) ||
        (outputs[i].status != NULL &&
         response->output_items[i].status == NULL) ||
        (outputs[i].role != NULL && response->output_items[i].role == NULL) ||
        (outputs[i].call_id != NULL &&
         response->output_items[i].call_id == NULL) ||
        (outputs[i].name != NULL && response->output_items[i].name == NULL)) {
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate response output item fields");
    }
  }
  return CAI_OK;
}

int cai_response_parse_json_with_allocator(const cai_allocator *allocator,
                                           const char *json, cai_response **out,
                                           cai_error *error) {
  cai_response_doc doc;
  cai_response *response;
  lonejson_spooled output_items_json;
  lonejson_error json_error;
  lonejson_status status;
  int refusal_present;
  int have_output_items_json;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response output pointer is required");
  }
  *out = NULL;
  if (json == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "response JSON is required");
  }
  have_output_items_json = 0;
  memset(&doc, 0, sizeof(doc));
  CAI_LJ->init(CAI_LJ, &cai_response_map, &doc);
  status = CAI_LJ->parse_cstr(CAI_LJ, &cai_response_map, &doc, json,
                              &json_error);
  if (status != LONEJSON_STATUS_OK) {
    lonejson_cleanup(&cai_response_map, &doc);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to parse response JSON",
                                json_error.message);
  }
  if (doc.id == NULL ||
      (doc.status == NULL &&
       (doc.object == NULL || strcmp(doc.object, "response.compaction") != 0))) {
    lonejson_cleanup(&cai_response_map, &doc);
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "response JSON is missing id or status");
  }
  if (cai_capture_response_output_json(&doc, &output_items_json, error) !=
      CAI_OK) {
    lonejson_cleanup(&cai_response_map, &doc);
    return error != NULL ? error->code : CAI_ERR_PROTOCOL;
  }
  have_output_items_json = 1;
  response = (cai_response *)cai_alloc(allocator, sizeof(*response));
  if (response == NULL) {
    if (have_output_items_json) {
      output_items_json.cleanup(&output_items_json);
    }
    lonejson_cleanup(&cai_response_map, &doc);
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate response");
  }
  memset(response, 0, sizeof(*response));
  if (allocator != NULL) {
    response->allocator = *allocator;
  }
  response->raw_json = NULL;
  response->id = cai_strdup(&response->allocator, doc.id);
  response->status = cai_strdup(
      &response->allocator, doc.status != NULL ? doc.status : "completed");
  response->model = cai_strdup(&response->allocator, doc.model);
  response->conversation_id =
      cai_strdup(&response->allocator, doc.conversation.id);
  response->output_text =
      cai_response_collect_text(&response->allocator, &doc, error);
  response->refusal =
      cai_response_collect_refusal(&response->allocator, &doc, &refusal_present,
                                   error);
  response->raw_json = cai_strdup(&response->allocator, json);
  response->error_code = cai_strdup(&response->allocator, doc.error.code);
  response->error_message = cai_strdup(&response->allocator, doc.error.message);
  response->incomplete_reason =
      cai_strdup(&response->allocator, doc.incomplete_details.reason);
  response->output_items_json = output_items_json;
  response->has_output_items_json = 1;
  have_output_items_json = 0;
  response->created_at = doc.created_at;
  response->input_tokens = doc.usage.input_tokens;
  response->input_cached_tokens =
      doc.usage.input_cached_tokens != 0LL
          ? doc.usage.input_cached_tokens
          : doc.usage.input_tokens_details.cached_tokens;
  response->output_tokens = doc.usage.output_tokens;
  response->output_reasoning_tokens =
      doc.usage.output_reasoning_tokens != 0LL
          ? doc.usage.output_reasoning_tokens
          : doc.usage.output_tokens_details.reasoning_tokens;
  response->total_tokens = doc.usage.total_tokens;
  response->tool_calls = NULL;
  response->tool_call_count = 0U;
  response->output_items = NULL;
  response->output_item_count = 0U;
  if (response->id == NULL || response->status == NULL ||
      (doc.model != NULL && response->model == NULL) ||
      (doc.conversation.id != NULL && response->conversation_id == NULL) ||
      response->output_text == NULL || response->raw_json == NULL ||
      (refusal_present && response->refusal == NULL) ||
      (doc.error.code != NULL && response->error_code == NULL) ||
      (doc.error.message != NULL && response->error_message == NULL) ||
      (doc.incomplete_details.reason != NULL &&
       response->incomplete_reason == NULL)) {
    cai_response_destroy(response);
    lonejson_cleanup(&cai_response_map, &doc);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate parsed response");
  }
  if (cai_response_copy_output_items(response, &doc, error) != CAI_OK) {
    cai_response_destroy(response);
    lonejson_cleanup(&cai_response_map, &doc);
    return error != NULL ? error->code : CAI_ERR_NOMEM;
  }
  if (cai_response_copy_tool_calls(response, &doc, error) != CAI_OK) {
    cai_response_destroy(response);
    lonejson_cleanup(&cai_response_map, &doc);
    return error != NULL ? error->code : CAI_ERR_NOMEM;
  }
  lonejson_cleanup(&cai_response_map, &doc);
  *out = response;
  if (have_output_items_json) {
    output_items_json.cleanup(&output_items_json);
  }
  return CAI_OK;
}

int cai_response_parse_json(const char *json, cai_response **out,
                            cai_error *error) {
  return cai_response_parse_json_with_allocator(NULL, json, out, error);
}

const char *cai_response_id(const cai_response *response) {
  return response != NULL ? response->id : NULL;
}

const char *cai_response_status(const cai_response *response) {
  return response != NULL ? response->status : NULL;
}

const char *cai_response_model(const cai_response *response) {
  return response != NULL ? response->model : NULL;
}

const char *cai_response_conversation_id(const cai_response *response) {
  return response != NULL ? response->conversation_id : NULL;
}

long long cai_response_created_at(const cai_response *response) {
  return response != NULL ? response->created_at : 0LL;
}

const char *cai_response_output_text(const cai_response *response) {
  return response != NULL ? response->output_text : NULL;
}

const char *cai_response_refusal(const cai_response *response) {
  return response != NULL ? response->refusal : NULL;
}

int cai_response_write_output_text(const cai_response *response, cai_sink *sink,
                                   cai_error *error) {
  size_t length;

  if (response == NULL || sink == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response and sink are required");
  }
  if (response->output_text == NULL) {
    return CAI_OK;
  }
  length = strlen(response->output_text);
  if (length == 0U) {
    return CAI_OK;
  }
  return cai_sink_write(sink, response->output_text, length, error);
}

int cai_response_write_refusal(const cai_response *response, cai_sink *sink,
                               cai_error *error) {
  size_t length;

  if (response == NULL || sink == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response and sink are required");
  }
  if (response->refusal == NULL) {
    return CAI_OK;
  }
  length = strlen(response->refusal);
  if (length == 0U) {
    return CAI_OK;
  }
  return cai_sink_write(sink, response->refusal, length, error);
}

const char *cai_response_raw_json(const cai_response *response) {
  return response != NULL ? response->raw_json : NULL;
}

int cai_response_output_items_json(const cai_response *response,
                                   char **out_json, cai_error *error) {
  cai_buffer_builder builder;
  cai_response_buffer_sink_context sink_context;
  lonejson_writer writer;
  lonejson_error json_error;
  lonejson_status status;

  if (out_json == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "array JSON output pointer is required");
  }
  *out_json = NULL;
  if (response == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "response is required");
  }
  memset(&builder, 0, sizeof(builder));
  sink_context.builder = &builder;
  sink_context.error = error;
  lonejson_error_init(&json_error);
  status = CAI_LJ->writer_init_sink(CAI_LJ, &writer, cai_response_buffer_sink, &sink_context, &json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = writer.json_value_spooled(&writer, &response->output_items_json,
                                       &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.finish(&writer, &json_error);
  }
  writer.cleanup(&writer);
  if (status != LONEJSON_STATUS_OK) {
    cai_free_mem(NULL, builder.data);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to serialize response output JSON",
                                json_error.message);
  }
  *out_json = builder.data;
  return CAI_OK;
}

int cai_response_write_output_items_json(const cai_response *response,
                                         cai_sink *sink, cai_error *error) {
  lonejson_spooled cursor;
  lonejson_error json_error;
  lonejson_read_result chunk;
  unsigned char buffer[4096];
  int rc;

  if (response == NULL || sink == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response and sink are required");
  }
  if (!response->has_output_items_json) {
    return CAI_OK;
  }
  cursor = response->output_items_json;
  lonejson_error_init(&json_error);
  if (cursor.rewind(&cursor, &json_error) != LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to rewind output items JSON",
                                json_error.message);
  }
  for (;;) {
    chunk = cursor.read(&cursor, buffer, sizeof(buffer));
    if (chunk.error_code != 0) {
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "failed to read output items JSON");
    }
    if (chunk.bytes_read > 0U) {
      rc = cai_sink_write(sink, buffer, chunk.bytes_read, error);
      if (rc != CAI_OK) {
        return rc;
      }
    }
    if (chunk.eof) {
      break;
    }
  }
  return CAI_OK;
}

int cai_response_output_items_spool(const cai_response *response,
                                    lonejson_spooled *out, size_t *out_len,
                                    cai_error *error) {
  if (response == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "response is required");
  }
  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "array JSON spool output pointer is required");
  }
  if (cai_response_spooled_clone(&response->output_items_json, out, error) !=
      CAI_OK) {
    return error != NULL ? error->code : CAI_ERR_TRANSPORT;
  }
  if (out_len != NULL) {
    *out_len = out->size_fn(out);
  }
  return CAI_OK;
}

const char *cai_response_error_code(const cai_response *response) {
  return response != NULL ? response->error_code : NULL;
}

const char *cai_response_error_message(const cai_response *response) {
  return response != NULL ? response->error_message : NULL;
}

const char *cai_response_incomplete_reason(const cai_response *response) {
  return response != NULL ? response->incomplete_reason : NULL;
}

long long cai_response_input_tokens(const cai_response *response) {
  return response != NULL ? response->input_tokens : 0LL;
}

long long cai_response_input_cached_tokens(const cai_response *response) {
  return response != NULL ? response->input_cached_tokens : 0LL;
}

long long cai_response_output_tokens(const cai_response *response) {
  return response != NULL ? response->output_tokens : 0LL;
}

long long cai_response_output_reasoning_tokens(const cai_response *response) {
  return response != NULL ? response->output_reasoning_tokens : 0LL;
}

long long cai_response_total_tokens(const cai_response *response) {
  return response != NULL ? response->total_tokens : 0LL;
}

int cai_response_usage(const cai_response *response, cai_token_usage *out,
                       cai_error *error) {
  if (response == NULL || out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response and usage output are required");
  }
  out->input_tokens = response->input_tokens;
  out->input_cached_tokens = response->input_cached_tokens;
  out->output_tokens = response->output_tokens;
  out->output_reasoning_tokens = response->output_reasoning_tokens;
  out->total_tokens = response->total_tokens;
  return CAI_OK;
}

size_t cai_response_tool_call_count(const cai_response *response) {
  return response != NULL ? response->tool_call_count : 0U;
}

const char *cai_response_tool_call_id(const cai_response *response,
                                      size_t index) {
  if (response == NULL || index >= response->tool_call_count) {
    return NULL;
  }
  return response->tool_calls[index].call_id != NULL
             ? response->tool_calls[index].call_id
             : response->tool_calls[index].id;
}

const char *cai_response_tool_call_name(const cai_response *response,
                                        size_t index) {
  if (response == NULL || index >= response->tool_call_count) {
    return NULL;
  }
  return response->tool_calls[index].name;
}

const char *cai_response_tool_call_arguments(const cai_response *response,
                                             size_t index) {
  if (response == NULL || index >= response->tool_call_count) {
    return NULL;
  }
  return response->tool_calls[index].arguments;
}

const struct lonejson_spooled *
cai_response_tool_call_arguments_spooled(const cai_response *response,
                                         size_t index) {
  if (response == NULL || index >= response->tool_call_count ||
      !response->tool_calls[index].has_arguments_spooled) {
    return NULL;
  }
  return &response->tool_calls[index].arguments_spooled;
}

size_t cai_response_output_item_count(const cai_response *response) {
  return response != NULL ? response->output_item_count : 0U;
}

const char *cai_response_output_item_id(const cai_response *response,
                                        size_t index) {
  if (response == NULL || index >= response->output_item_count) {
    return NULL;
  }
  return response->output_items[index].id;
}

const char *cai_response_output_item_type(const cai_response *response,
                                          size_t index) {
  if (response == NULL || index >= response->output_item_count) {
    return NULL;
  }
  return response->output_items[index].type;
}

const char *cai_response_output_item_status(const cai_response *response,
                                            size_t index) {
  if (response == NULL || index >= response->output_item_count) {
    return NULL;
  }
  return response->output_items[index].status;
}

const char *cai_response_output_item_role(const cai_response *response,
                                          size_t index) {
  if (response == NULL || index >= response->output_item_count) {
    return NULL;
  }
  return response->output_items[index].role;
}

const char *cai_response_output_item_call_id(const cai_response *response,
                                             size_t index) {
  if (response == NULL || index >= response->output_item_count) {
    return NULL;
  }
  return response->output_items[index].call_id;
}

const char *cai_response_output_item_name(const cai_response *response,
                                          size_t index) {
  if (response == NULL || index >= response->output_item_count) {
    return NULL;
  }
  return response->output_items[index].name;
}

void cai_response_destroy(cai_response *response) {
  cai_allocator allocator;
  size_t i;

  if (response == NULL) {
    return;
  }
  allocator = response->allocator;
  for (i = 0U; i < response->tool_call_count; i++) {
    cai_free_mem(&allocator, response->tool_calls[i].id);
    cai_free_mem(&allocator, response->tool_calls[i].call_id);
    cai_free_mem(&allocator, response->tool_calls[i].name);
    cai_free_mem(&allocator, response->tool_calls[i].arguments);
    if (response->tool_calls[i].has_arguments_spooled) {
      response->tool_calls[i].arguments_spooled.cleanup(&response->tool_calls[i].arguments_spooled);
    }
  }
  cai_free_mem(&allocator, response->tool_calls);
  for (i = 0U; i < response->output_item_count; i++) {
    cai_free_mem(&allocator, response->output_items[i].id);
    cai_free_mem(&allocator, response->output_items[i].type);
    cai_free_mem(&allocator, response->output_items[i].status);
    cai_free_mem(&allocator, response->output_items[i].role);
    cai_free_mem(&allocator, response->output_items[i].call_id);
    cai_free_mem(&allocator, response->output_items[i].name);
  }
  cai_free_mem(&allocator, response->output_items);
  cai_free_mem(&allocator, response->id);
  cai_free_mem(&allocator, response->status);
  cai_free_mem(&allocator, response->model);
  cai_free_mem(&allocator, response->conversation_id);
  cai_free_mem(&allocator, response->output_text);
  cai_free_mem(&allocator, response->refusal);
  cai_free_mem(&allocator, response->raw_json);
  if (response->has_output_items_json) {
    response->output_items_json.cleanup(&response->output_items_json);
  }
  cai_free_mem(&allocator, response->error_code);
  cai_free_mem(&allocator, response->error_message);
  cai_free_mem(&allocator, response->incomplete_reason);
  cai_free_mem(&allocator, response);
}
