#ifndef CAI_INTERNAL_H
#define CAI_INTERNAL_H

#include <cai/cai.h>

#include <curl/curl.h>
#include <lonejson.h>
#include <stddef.h>
#include <stdio.h>

struct curl_slist;

#define CAI_DEFAULT_BASE_URL CAI_OPENAI_BASE_URL
#define CAI_DEFAULT_JSON_RESPONSE_LIMIT (1024UL * 1024UL)
#define CAI_DEFAULT_SSE_EVENT_LIMIT (256UL * 1024UL)

typedef struct cai_client_impl {
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
} cai_client_impl;

typedef struct cai_response_request_upload cai_response_request_upload;

typedef struct cai_agent_impl {
  cai_client *client;
  char *model;
  char *developer_instructions;
  char *prompt_cache_key;
  char *reasoning_effort;
  char *reasoning_summary;
  char *text_format_name;
  char *text_format_description;
  char *text_format_schema_json;
  int text_format_strict;
  int max_output_tokens;
  int parallel_tool_calls;
  int session_continuity;
  int auto_compact;
  long long auto_compact_token_limit;
  unsigned int compact_threshold_percent;
  int local_history_enabled;
  size_t history_memory_limit;
  char *history_spool_dir;
  cai_tool_registry *tools;
  cai_session *default_session;
} cai_agent_impl;

typedef struct cai_session_text_input {
  int kind;
  char *role;
  char *text;
  char *image_url;
  char *filename;
  char *detail;
  char *call_id;
  char *output;
  lonejson_spooled file_data;
  int has_file_data;
} cai_session_input;

typedef struct cai_session_impl {
  cai_agent *agent;
  char *previous_response_id;
  char *conversation_id;
  cai_token_usage last_usage;
  int has_last_usage;
  lonejson_spooled history;
  cai_session_input *inputs;
  size_t input_count;
  size_t input_capacity;
} cai_session_impl;

#define CAI_CLIENT_IMPL(client) ((cai_client_impl *)((client)->impl))
#define CAI_AGENT_IMPL(agent) ((cai_agent_impl *)((agent)->impl))
#define CAI_SESSION_IMPL(session) ((cai_session_impl *)((session)->impl))
#define CAI_SESSION_AGENT_IMPL(session) \
  (CAI_AGENT_IMPL(CAI_SESSION_IMPL(session)->agent))
#define CAI_SESSION_CLIENT(session) (CAI_SESSION_AGENT_IMPL(session)->client)
#define CAI_SESSION_CLIENT_IMPL(session) \
  (CAI_CLIENT_IMPL(CAI_SESSION_CLIENT(session)))

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
                        const char *explicit_key, const char *env_name,
                        char **out, cai_error *error);

void cai_log_client_opened(const cai_client_impl *client);
void cai_log_openrouter_server_continuity(const cai_client_impl *client);
void cai_log_http_request_start(const cai_client_impl *client,
                                const char *method, const char *path,
                                int stream, size_t request_bytes);
void cai_log_http_request_done(const cai_client_impl *client,
                               const char *method, const char *path,
                               long http_status, size_t response_bytes,
                               const char *request_id);
void cai_log_http_transport_error(const cai_client_impl *client,
                                  const char *method, const char *path,
                                  const char *detail);
void cai_log_http_response_limit(const cai_client_impl *client,
                                 const char *method, const char *path,
                                 size_t limit);

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
  char *prompt_cache_key;
  char *reasoning_effort;
  char *reasoning_summary;
  char *text_format_type;
  char *text_format_name;
  char *text_format_description;
  char *text_format_schema_json;
  int text_format_strict;
  int max_output_tokens;
  int parallel_tool_calls;
  long long compact_threshold_tokens;
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
  int output_index;
  lonejson_spooled arguments_spooled;
  int has_arguments_spooled;
} cai_response_tool_call;

typedef struct cai_response_output_item {
  char *id;
  char *type;
  char *status;
  char *role;
  char *call_id;
  char *name;
} cai_response_output_item;

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
  cai_response_output_item *output_items;
  size_t output_item_count;
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
int cai_serialize_input_message_items_json(cai_json_builder *builder,
                                           const lonejson_object_array *input,
                                           cai_error *error);
int cai_serialize_input_messages_json(cai_json_builder *builder,
                                      const char *field_name,
                                      const lonejson_object_array *messages,
                                      cai_error *error);
int cai_input_messages_spool_json_array(const lonejson_object_array *messages,
                                        lonejson_spooled *out,
                                        size_t *out_len, cai_error *error);
int cai_response_create_params_serialize_json(
    const cai_response_create_params *params, char **out_json, size_t *out_len,
    cai_error *error);
int cai_response_create_params_spool_json(
    const cai_response_create_params *params, int stream,
    lonejson_spooled *out, size_t *out_len, cai_error *error);
int cai_response_create_params_clone(const cai_response_create_params *params,
                                     cai_response_create_params **out,
                                     cai_error *error);
int cai_response_create_params_write_json_sink(
    const cai_response_create_params *params, int stream, lonejson_sink_fn sink,
    void *sink_user, lonejson_error *sink_error, size_t *out_len,
    cai_error *error);
int cai_response_request_upload_open(const cai_response_create_params *params,
                                     int stream,
                                     cai_response_request_upload **out,
                                     cai_error *error);
size_t cai_response_request_upload_read(char *ptr, size_t size, size_t nmemb,
                                        void *userdata);
curl_off_t cai_response_request_upload_size(
    const cai_response_request_upload *upload);
void cai_response_request_upload_close(cai_response_request_upload *upload);
int cai_response_create_params_set_raw_input_json(
    cai_response_create_params *params, const char *raw_input_json,
    cai_error *error);
int cai_response_create_params_set_raw_input_spooled(
    cai_response_create_params *params, lonejson_spooled *raw_input_json,
    cai_error *error);
void cai_response_create_params_clear_input(cai_response_create_params *params);
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
int cai_source_from_spooled(lonejson_spooled *spool, cai_source **out,
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
int cai_http_response_params_request(
    cai_client *client, const char *path,
    const cai_response_create_params *params, int stream, char **out_json,
    long *out_http_status, char **out_request_id, cai_error *error);
typedef int (*cai_stream_complete_fn)(void *context, const char *response_id,
                                      const cai_token_usage *usage);
int cai_client_open_response_text_source_with_complete(
    cai_client *client, const cai_response_create_params *params,
    cai_stream_complete_fn on_complete, void *complete_context,
    cai_source **out, cai_error *error);
int cai_client_open_response_text_source_take_params(
    cai_client *client, cai_response_create_params *params,
    cai_stream_complete_fn on_complete, void *complete_context,
    cai_source **out, cai_error *error);
int cai_client_stream_response_text_with_id(
    cai_client *client, const cai_response_create_params *params,
    cai_sink *sink, char **out_response_id, cai_token_usage *out_usage,
    cai_error *error);
int cai_client_stream_response_with_id(
    cai_client *client, const cai_response_create_params *params,
    const cai_stream_sinks *sinks, char **out_response_id,
    cai_token_usage *out_usage, cai_error *error);
int cai_tool_registry_register_lonejson_owned(
    cai_tool_registry *registry, const char *name, const char *description,
    const lonejson_map *params_map, const lonejson_map *result_map,
    cai_tool_fn callback, void *context, void (*context_cleanup)(void *context),
    cai_error *error);
int cai_tool_registry_register_lonejson_schema_owned(
    cai_tool_registry *registry, const char *name, const char *description,
    const char *schema_json, int strict, const lonejson_map *params_map,
    const lonejson_map *result_map, cai_tool_fn callback, void *context,
    void (*context_cleanup)(void *context), cai_error *error);
int cai_tool_registry_run_spooled(cai_tool_registry *registry,
                                  const char *name,
                                  lonejson_spooled *arguments_json,
                                  cai_sink *output, cai_error *error);
size_t cai_tool_registry_count(const cai_tool_registry *registry);
const char *cai_tool_registry_name_at(const cai_tool_registry *registry,
                                      size_t index);
const char *cai_tool_registry_description_at(const cai_tool_registry *registry,
                                             size_t index);
const char *cai_tool_registry_schema_at(const cai_tool_registry *registry,
                                        size_t index);
int cai_set_openai_error(cai_error *error, long http_status, const char *body,
                         const char *request_id);
int cai_conversation_parse_json(const char *json, cai_conversation **out,
                                cai_error *error);

#endif
