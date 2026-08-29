#ifndef CAI_INTERNAL_H
#define CAI_INTERNAL_H

#include <cai/cai.h>
#include <cai/skills.h>

#include "cai_lj.h"

#include <curl/curl.h>
#include <lonejson.h>
#include <stddef.h>
#include <stdio.h>

struct curl_slist;

#define CAI_DEFAULT_BASE_URL CAI_OPENAI_BASE_URL
#define CAI_DEFAULT_HTTP_TIMEOUT_MS 60000L
#define CAI_DEFAULT_JSON_RESPONSE_LIMIT (1024UL * 1024UL)
#define CAI_DEFAULT_SSE_EVENT_LIMIT (4UL * 1024UL * 1024UL)

typedef struct cai_client_impl {
  cai_allocator allocator;
  char *api_key;
  cai_chatgpt_auth *chatgpt_auth;
  char *base_url;
  char *organization_id;
  char *project_id;
  long timeout_ms;
  int http_2_disabled;
  int insecure_skip_verify;
  char *ca_bundle_path;
  char *ca_path;
  int responses_websocket_fallback_disabled;
  int responses_websocket_fallback_active;
  size_t json_response_limit_bytes;
  struct pslog_logger *logger;
  int logger_disabled;
  cai_usage_limits usage_limits;
  cai_usage_accounting usage;
  CURL *responses_ws_curl;
  struct curl_slist *responses_ws_headers;
} cai_client_impl;

typedef struct cai_response_request_upload cai_response_request_upload;

typedef struct cai_agent_impl {
  cai_client *client;
  char *model;
  char *developer_instructions;
  char *prompt_cache_key;
  char *tool_choice;
  char *tool_choice_json;
  char *service_tier;
  char *truncation;
  char *reasoning_effort;
  char *reasoning_mode;
  char *reasoning_summary;
  char *text_format_name;
  char *text_format_description;
  char *text_format_schema_json;
  int text_format_strict;
  int max_output_tokens;
  int max_tool_calls;
  int parallel_tool_calls;
  int session_continuity;
  int auto_compact;
  long long auto_compact_token_limit;
  unsigned int compact_threshold_percent;
  int local_history_enabled;
  size_t history_memory_limit;
  char *history_spool_dir;
  cai_usage_limits session_usage_limits;
  lonejson *history_runtime;
  lonejson_object_array hosted_tools;
  cai_tool_registry *tools;
  cai_session *default_session;
} cai_agent_impl;

typedef struct cai_session_text_input {
  int kind;
  char *role;
  char *text;
  lonejson_spooled text_spooled;
  int has_text_spooled;
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
  /** Model recorded in the imported/exported portable session state. */
  char *state_model;
  /** Preset identity recorded in the imported/exported portable session state.
   */
  char *state_preset_name;
  /** Prompt revision recorded in the imported/exported portable session state.
   */
  char *state_preset_prompt_version;
  char *goal_objective;
  char *goal_status;
  long long goal_token_budget;
  int goal_has_token_budget;
  /** Cumulative session usage at goal creation; not user-visible accounting. */
  long long goal_token_usage_baseline;
  long long goal_tokens_used;
  /** Accumulated active wall time; excludes paused and terminal intervals. */
  long long goal_elapsed_seconds;
  /** Wall clock when the current active interval began, or zero. */
  long long goal_active_started_at;
  long long goal_created_at;
  long long goal_updated_at;
  /** Monotonic user-turn ordinal used to qualify blocked-goal updates. */
  long long goal_turn_count;
  /** Last turn that counted toward blocked-goal qualification, or -1. */
  long long goal_blocked_last_turn;
  int goal_blocked_attempts;
  cai_token_usage last_usage;
  int has_last_usage;
  cai_usage_limits usage_limits;
  cai_usage_accounting usage;
  lonejson_spooled history;
  cai_session_input *inputs;
  size_t input_count;
  size_t input_capacity;
} cai_session_impl;

#define CAI_CLIENT_IMPL(client) ((cai_client_impl *)((client)->impl))
#define CAI_AGENT_IMPL(agent) ((cai_agent_impl *)((agent)->impl))
#define CAI_SESSION_IMPL(session) ((cai_session_impl *)((session)->impl))
#define CAI_SESSION_AGENT_IMPL(session)                                        \
  (CAI_AGENT_IMPL(CAI_SESSION_IMPL(session)->agent))
#define CAI_SESSION_CLIENT(session) (CAI_SESSION_AGENT_IMPL(session)->client)
#define CAI_SESSION_CLIENT_IMPL(session)                                       \
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
int cai_usage_limits_validate(const cai_usage_limits *limits, cai_error *error);

long long cai_session_goal_elapsed_seconds(const cai_session *session,
                                           long long now);
void cai_session_goal_start_elapsed(cai_session *session, long long now);
void cai_session_goal_stop_elapsed(cai_session *session, long long now);

int cai_resolve_api_key(const cai_allocator *allocator,
                        const char *explicit_key, const char *env_name,
                        char **out, cai_error *error);
int cai_client_refresh_chatgpt_auth(cai_client *client, cai_error *error);
int cai_client_refresh_chatgpt_auth_after_http(cai_client *client,
                                               long http_status,
                                               cai_error *error);

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
  lonejson_spooled text_spooled;
  int has_text_spooled;
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
  char *raw_json;
  char *custom_format_type;
  char *custom_format_syntax;
  char *custom_format_definition;
  int strict;
  int is_raw;
  int is_custom;
};

typedef struct cai_buffer_builder {
  char *data;
  size_t length;
  size_t capacity;
  const cai_allocator *allocator;
  lonejson_sink_fn sink;
  void *sink_user;
  lonejson_error *sink_error;
} cai_buffer_builder;

int cai_buffer_append_cstr(cai_buffer_builder *builder, const char *text,
                           cai_error *error);
int cai_buffer_append(cai_buffer_builder *builder, const char *text,
                      size_t length, cai_error *error);
int cai_buffer_append_json_string(cai_buffer_builder *builder,
                                  const char *value, cai_error *error);
int cai_input_messages_spool_json_array(const lonejson_object_array *messages,
                                        lonejson_spooled *out, size_t *out_len,
                                        cai_error *error);
int cai_response_create_params_serialize_json(
    const cai_response_create_params *params, char **out_json, size_t *out_len,
    cai_error *error);
int cai_response_create_params_spool_json(
    const cai_response_create_params *params, int stream, lonejson_spooled *out,
    size_t *out_len, cai_error *error);
int cai_response_create_params_clone(const cai_response_create_params *params,
                                     cai_response_create_params **out,
                                     cai_error *error);
int cai_response_create_params_write_json_sink(
    const cai_response_create_params *params, int stream, lonejson_sink_fn sink,
    void *sink_user, lonejson_error *sink_error, size_t *out_len,
    cai_error *error);
int cai_response_request_upload_open(
    const cai_response_create_params *params, int stream, int default_has_store,
    int default_store, int omit_max_output_tokens, const char *event_type,
    cai_response_request_upload **out, cai_error *error);
size_t cai_response_request_upload_read(char *ptr, size_t size, size_t nmemb,
                                        void *userdata);
curl_off_t
cai_response_request_upload_size(const cai_response_request_upload *upload);
void cai_response_request_upload_close(cai_response_request_upload *upload);
#ifdef CAI_TESTING
int cai_test_response_request_upload_active(void);
void cai_session_store_test_set_fail_scope_parent_sync(int enabled);
void cai_agent_runtime_test_set_fail_goal_status_replace(int enabled);
unsigned int cai_agent_runtime_test_goal_status_replace_failures(void);
typedef void (*cai_agent_runtime_test_export_cleanup_fn)(const char *path,
                                                         void *context);
void cai_agent_runtime_test_set_export_cleanup_hook(
    cai_agent_runtime_test_export_cleanup_fn hook, void *context);
size_t cai_terminal_test_output_limit(long long value, int present,
                                      size_t maximum);
#endif
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
int cai_response_create_params_add_custom_tool_call_output_spooled(
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
int cai_response_parse_json_with_allocator(const cai_allocator *allocator,
                                           const char *json, cai_response **out,
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
int cai_append_client_headers(cai_client *client, struct curl_slist **headers,
                              cai_error *error);
int cai_http_json_request(cai_client *client, const char *method,
                          const char *path, const char *request_json,
                          char **out_json, long *out_http_status,
                          char **out_request_id, cai_error *error);
int cai_http_json_request_spooled(cai_client *client, const char *method,
                                  const char *path,
                                  const lonejson_spooled *request_json,
                                  size_t request_json_len, char **out_json,
                                  long *out_http_status, char **out_request_id,
                                  cai_error *error);
int cai_http_response_params_request(cai_client *client, const char *path,
                                     const cai_response_create_params *params,
                                     int stream, char **out_json,
                                     long *out_http_status,
                                     char **out_request_id, cai_error *error);
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
int cai_client_stream_response_with_id(cai_client *client,
                                       const cai_response_create_params *params,
                                       const cai_stream_sinks *sinks,
                                       char **out_response_id,
                                       cai_token_usage *out_usage,
                                       cai_error *error);
int cai_client_stream_response_internal_with_id(
    cai_client *client, const cai_response_create_params *params,
    const cai_stream_sinks *sinks, char **out_response_id,
    cai_token_usage *out_usage, cai_error *error);
void cai_client_close_responses_websocket(cai_client_impl *impl);
#ifdef CAI_TESTING
int cai_client_stream_response_websocket_test(
    cai_client *client, const cai_response_create_params *params,
    const cai_stream_sinks *sinks, char **out_response_id,
    cai_token_usage *out_usage, cai_error *error);
int cai_mcp_test_authenticated_url_status(const char *url, const char *field,
                                          char *detail, size_t detail_len);
#endif
int cai_stream_fuzz_sse(const unsigned char *data, size_t size);
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
int cai_agent_register_lonejson_tool_schema_internal(
    cai_agent *agent, const char *name, const char *description,
    const char *schema_json, int strict, const lonejson_map *params_map,
    const lonejson_map *result_map, cai_tool_fn callback, void *context,
    cai_error *error);
int cai_tool_registry_register_raw_spooled_owned(
    cai_tool_registry *registry, const char *name, const char *description,
    const char *schema_json, int strict, cai_tool_raw_spooled_fn callback,
    void *context, void (*context_cleanup)(void *context), cai_error *error);
int cai_tool_registry_register_custom_owned(
    cai_tool_registry *registry, const char *name, const char *description,
    const cai_custom_tool_format *format, cai_tool_custom_fn callback,
    void *context, void (*context_cleanup)(void *context), cai_error *error);
int cai_tool_registry_register_custom_spooled_owned(
    cai_tool_registry *registry, const char *name, const char *description,
    const cai_custom_tool_format *format, cai_tool_custom_spooled_fn callback,
    void *context, void (*context_cleanup)(void *context), cai_error *error);
typedef struct cai_skill_catalog cai_skill_catalog;
int cai_skills_prepare(const cai_skill_config *config,
                       const char *agent_config_directory,
                       cai_skill_catalog **out_catalog, char **out_prompt,
                       cai_error *error);
void cai_skills_catalog_cleanup(cai_skill_catalog *catalog);
int cai_skills_catalog_has_entries(const cai_skill_catalog *catalog);
int cai_agent_register_skill_tool_owned(cai_agent *agent,
                                        cai_skill_catalog *catalog,
                                        cai_error *error);
/*
 * A local tool may replace its ordinary JSON function-call output with typed
 * response content.  This is intentionally internal until the public tool
 * result ABI grows a stable typed-output representation.
 */
typedef int (*cai_tool_result_delivery_fn)(void *context, const char *call_id,
                                           cai_response_create_params *params,
                                           const lonejson_spooled *output_json,
                                           int *out_delivered,
                                           cai_error *error);
int cai_tool_registry_set_result_delivery(cai_tool_registry *registry,
                                          const char *name,
                                          cai_tool_result_delivery_fn callback,
                                          cai_error *error);
int cai_tool_registry_deliver_result(cai_tool_registry *registry,
                                     const char *name, const char *call_id,
                                     cai_response_create_params *params,
                                     const lonejson_spooled *output_json,
                                     int *out_delivered, cai_error *error);
int cai_tool_registry_run_spooled(cai_tool_registry *registry, const char *name,
                                  lonejson_spooled *arguments_json,
                                  cai_sink *output, cai_error *error);
size_t cai_tool_registry_count(const cai_tool_registry *registry);
void cai_tool_registry_truncate(cai_tool_registry *registry, size_t count);
const char *cai_tool_registry_name_at(const cai_tool_registry *registry,
                                      size_t index);
const char *cai_tool_registry_description_at(const cai_tool_registry *registry,
                                             size_t index);
const char *cai_tool_registry_schema_at(const cai_tool_registry *registry,
                                        size_t index);
int cai_set_openai_error(cai_error *error, long http_status, const char *body,
                         const char *request_id);
void cai_configure_curl_tls(CURL *curl, int insecure_skip_verify,
                            const char *ca_bundle_path, const char *ca_path);
int cai_conversation_parse_json(const char *json, cai_conversation **out,
                                cai_error *error);
int cai_session_commit_pending_inputs(cai_session *session, cai_error *error);
/* Insert steering into the active model cycle without creating a user turn. */
int cai_session_add_steering_text(cai_session *session, const char *text,
                                  cai_error *error);
/* Add CAI-generated trusted context without creating a user turn. */
int cai_session_add_internal_context_text(cai_session *session,
                                          const char *text, cai_error *error);

#endif
