#ifndef CAI_INTERNAL_H
#define CAI_INTERNAL_H

#include <cai/cai.h>

#include <lonejson.h>
#include <stddef.h>
#include <stdio.h>

struct curl_slist;

#define CAI_DEFAULT_BASE_URL "https://api.openai.com/v1"
#define CAI_DEFAULT_JSON_RESPONSE_LIMIT (1024UL * 1024UL)

struct cai_client {
  cai_allocator allocator;
  char *api_key;
  char *base_url;
  char *organization_id;
  char *project_id;
  long timeout_ms;
  int http_2_disabled;
  int insecure_skip_verify;
  size_t json_response_limit_bytes;
  struct pslog_logger *logger;
  int logger_disabled;
};

struct cai_agent {
  cai_client *client;
  char *model;
  char *instructions;
  char *reasoning_effort;
  char *reasoning_summary;
  char *text_format_name;
  char *text_format_description;
  char *text_format_schema_json;
  int text_format_strict;
  int max_output_tokens;
  int parallel_tool_calls;
  int auto_compact;
  long long auto_compact_token_limit;
  size_t history_memory_limit;
  char *history_spool_dir;
  cai_tool_registry *tools;
};

typedef struct cai_session_text_input {
  int kind;
  char *role;
  char *text;
  char *image_url;
  char *detail;
  char *call_id;
  char *output;
} cai_session_input;

struct cai_session {
  cai_agent *agent;
  char *previous_response_id;
  char *conversation_id;
  cai_token_usage last_usage;
  int has_last_usage;
  lonejson_spooled history;
  cai_session_input *inputs;
  size_t input_count;
  size_t input_capacity;
};

void *cai_alloc(const cai_allocator *allocator, size_t size);
void *cai_realloc_mem(const cai_allocator *allocator, void *ptr, size_t size);
void cai_free_mem(const cai_allocator *allocator, void *ptr);
char *cai_strdup(const cai_allocator *allocator, const char *value);
char *cai_strndup(const cai_allocator *allocator, const char *value,
                  size_t length);

int cai_set_error(cai_error *error, int code, const char *message);
int cai_set_error_detail(cai_error *error, int code, const char *message,
                         const char *detail);
int cai_set_error_http(cai_error *error, int code, long http_status,
                       const char *message, const char *detail,
                       const char *server_code, const char *request_id);

int cai_resolve_api_key(const cai_allocator *allocator,
                        const char *explicit_key, char **out, cai_error *error);

struct cai_content_part {
  char *type;
  char *text;
  char *image_url;
  char *file_id;
  char *filename;
  char *file_url;
  lonejson_spooled file_data;
  int has_file_data;
  char *detail;
};

struct cai_input_message {
  int kind;
  char *role;
  lonejson_object_array content;
  char *call_id;
  char *output;
  lonejson_spooled output_spooled;
  int has_output_spooled;
};

struct cai_function_tool {
  char *name;
  char *description;
  char *parameters_json;
  int strict;
};

struct cai_response_create_params {
  cai_allocator allocator;
  char *model;
  char *conversation_id;
  char *instructions;
  char *previous_response_id;
  char *reasoning_effort;
  char *reasoning_summary;
  char *text_format_type;
  char *text_format_name;
  char *text_format_description;
  char *text_format_schema_json;
  int text_format_strict;
  int max_output_tokens;
  int parallel_tool_calls;
  char *raw_input_json;
  lonejson_spooled raw_input_spooled;
  int has_raw_input_spooled;
  lonejson_object_array input;
  lonejson_object_array tools;
};

struct cai_conversation_items_params {
  cai_allocator allocator;
  lonejson_object_array items;
};

typedef struct cai_response_tool_call {
  char *id;
  char *call_id;
  char *name;
  char *arguments;
} cai_response_tool_call;

struct cai_response {
  char *id;
  char *status;
  char *model;
  char *conversation_id;
  char *output_text;
  char *refusal;
  char *raw_json;
  char *error_code;
  char *error_message;
  char *incomplete_reason;
  lonejson_spooled output_items_json;
  int has_output_items_json;
  long long created_at;
  long long input_tokens;
  long long input_cached_tokens;
  long long output_tokens;
  long long output_reasoning_tokens;
  long long total_tokens;
  cai_response_tool_call *tool_calls;
  size_t tool_call_count;
};

struct cai_output {
  cai_response *response;
};

typedef struct cai_input_item {
  char *id;
  char *type;
  char *role;
} cai_input_item;

struct cai_input_item_list {
  char *object;
  char *first_id;
  char *last_id;
  char *raw_json;
  int has_more;
  cai_input_item *items;
  size_t count;
};

struct cai_conversation_item {
  char *id;
  char *type;
  char *role;
  char *raw_json;
};

struct cai_conversation {
  char *id;
  char *object;
};

typedef struct cai_json_builder {
  char *data;
  size_t length;
  size_t capacity;
  lonejson_sink_fn sink;
  void *sink_user;
  lonejson_error *sink_error;
} cai_json_builder;

int cai_json_builder_lit(cai_json_builder *builder, const char *text,
                         cai_error *error);
int cai_json_builder_append(cai_json_builder *builder, const char *text,
                            size_t length, cai_error *error);
int cai_json_builder_string(cai_json_builder *builder, const char *value,
                            cai_error *error);
int cai_json_builder_field_string(cai_json_builder *builder, const char *name,
                                  const char *value, int *need_comma,
                                  cai_error *error);
int cai_serialize_input_messages_json(cai_json_builder *builder,
                                      const char *field_name,
                                      const lonejson_object_array *messages,
                                      cai_error *error);
int cai_response_create_params_serialize_json(
    const cai_response_create_params *params, char **out_json, size_t *out_len,
    cai_error *error);
int cai_response_create_params_spool_json(
    const cai_response_create_params *params, int stream,
    lonejson_spooled *out, size_t *out_len, cai_error *error);
int cai_response_create_params_set_raw_input_json(
    cai_response_create_params *params, const char *raw_input_json,
    cai_error *error);
int cai_response_create_params_set_raw_input_spooled(
    cai_response_create_params *params, lonejson_spooled *raw_input_json,
    cai_error *error);
void cai_response_create_params_clear_input(cai_response_create_params *params);
int cai_response_create_params_add_function_call_output_spooled(
    cai_response_create_params *params, const char *call_id,
    lonejson_spooled *output, cai_error *error);
int cai_response_params_input_items_json(
    const cai_response_create_params *params, char **out_json,
    cai_error *error);
int cai_response_params_input_items_spool(
    const cai_response_create_params *params, lonejson_spooled *out,
    size_t *out_len, cai_error *error);
int cai_response_output_items_spool(const cai_response *response,
                                    lonejson_spooled *out, size_t *out_len,
                                    cai_error *error);
int cai_response_parse_json(const char *json, cai_response **out,
                            cai_error *error);
int cai_response_output_items_json(const cai_response *response,
                                   char **out_json, cai_error *error);
int cai_output_from_response(cai_response *response, cai_output **out,
                             cai_error *error);
int cai_input_item_list_parse_json(const char *json, cai_input_item_list **out,
                                   cai_error *error);
int cai_conversation_item_parse_json(const char *json,
                                     cai_conversation_item **out,
                                     cai_error *error);
int cai_build_url(const cai_allocator *allocator, const char *base_url,
                  const char *path, char **out, cai_error *error);
int cai_append_list_query_params(const cai_allocator *allocator, char **path,
                                 const cai_list_params *params,
                                 cai_error *error);
int cai_append_header(struct curl_slist **headers, const char *header,
                      cai_error *error);
int cai_append_bearer_header(cai_client *client, struct curl_slist **headers,
                             cai_error *error);
int cai_append_prefixed_header(cai_client *client, struct curl_slist **headers,
                               const char *prefix, const char *value,
                               cai_error *error);
int cai_http_json_request(cai_client *client, const char *method,
                          const char *path, const char *request_json,
                          char **out_json, long *out_http_status,
                          char **out_request_id, cai_error *error);
int cai_http_json_request_spooled(cai_client *client, const char *method,
                                  const char *path,
                                  const lonejson_spooled *request_json,
                                  size_t request_json_len, char **out_json,
                                  long *out_http_status,
                                  char **out_request_id, cai_error *error);
int cai_client_stream_response_text_json_with_id(
    cai_client *client, const char *request_json, cai_sink *sink,
    char **out_response_id, cai_token_usage *out_usage, cai_error *error);
int cai_client_stream_response_text_spooled_with_id(
    cai_client *client, const lonejson_spooled *request_json,
    size_t request_json_len, cai_sink *sink, char **out_response_id,
    cai_token_usage *out_usage, cai_error *error);
typedef int (*cai_stream_complete_fn)(void *context, const char *response_id,
                                      const cai_token_usage *usage);
int cai_client_open_response_text_source_with_complete(
    cai_client *client, const cai_response_create_params *params,
    cai_stream_complete_fn on_complete, void *complete_context,
    cai_source **out, cai_error *error);
int cai_client_stream_response_text_with_id(
    cai_client *client, const cai_response_create_params *params,
    cai_sink *sink, char **out_response_id, cai_token_usage *out_usage,
    cai_error *error);
int cai_set_openai_error(cai_error *error, long http_status, const char *body,
                         const char *request_id);
int cai_conversation_parse_json(const char *json, cai_conversation **out,
                                cai_error *error);

#endif
