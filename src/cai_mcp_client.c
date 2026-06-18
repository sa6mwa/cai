#include "cai_internal.h"

#include <cai/mcp.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>

typedef struct cai_mcp_client_tool_impl {
  cai_mcp_client_tool public_tool;
  char *name;
  char *title;
  char *description;
  char *input_schema_json;
  char *output_schema_json;
  char *annotations_json;
  char *icons_json;
  char *execution_json;
} cai_mcp_client_tool_impl;

typedef struct cai_mcp_client_resource_impl {
  cai_mcp_client_resource public_resource;
  char *uri;
  char *name;
  char *title;
  char *description;
  char *mime_type;
  char *icons_json;
  char *annotations_json;
  int has_size;
  long long size;
} cai_mcp_client_resource_impl;

typedef struct cai_mcp_client_resource_template_impl {
  cai_mcp_client_resource_template public_resource_template;
  char *uri_template;
  char *name;
  char *title;
  char *description;
  char *mime_type;
  char *icons_json;
  char *annotations_json;
} cai_mcp_client_resource_template_impl;

typedef struct cai_mcp_client_prompt_impl {
  cai_mcp_client_prompt public_prompt;
  char *name;
  char *title;
  char *description;
  char *arguments_json;
  char *icons_json;
} cai_mcp_client_prompt_impl;

typedef struct cai_mcp_streamable_http_client_impl {
  cai_mcp_client public_client;
  cai_allocator allocator;
  char *url;
  char *client_name;
  char *client_version;
  char *protocol_version;
  long timeout_ms;
  int insecure_skip_verify;
  char *ca_bundle_path;
  char *ca_path;
  cai_mcp_client_receiver receiver;
  cai_mcp_client_notification_fn notification;
  void *notification_context;
  void (*notification_context_cleanup)(void *context);
  char *session_id;
  int initialized;
  long long next_id;
  long long active_request_id;
  int has_active_request;
  int has_active_progress;
  double active_progress;
  cai_mcp_client_tool_impl *tools;
  size_t tool_count;
  size_t tool_capacity;
  cai_mcp_client_resource_impl *resources;
  size_t resource_count;
  size_t resource_capacity;
  cai_mcp_client_resource_template_impl *resource_templates;
  size_t resource_template_count;
  size_t resource_template_capacity;
  cai_mcp_client_prompt_impl *prompts;
  size_t prompt_count;
  size_t prompt_capacity;
} cai_mcp_streamable_http_client_impl;

static void
cai_mcp_client_clear_tools(cai_mcp_streamable_http_client_impl *impl);
static void
cai_mcp_client_clear_resources(cai_mcp_streamable_http_client_impl *impl);
static void cai_mcp_client_clear_resource_templates(
    cai_mcp_streamable_http_client_impl *impl);
static void
cai_mcp_client_clear_prompts(cai_mcp_streamable_http_client_impl *impl);

typedef struct cai_mcp_http_response_capture {
  lonejson_spooled body;
  char *content_type;
  char *session_id;
  long status;
} cai_mcp_http_response_capture;

typedef struct cai_mcp_sse_resume_state {
  char *last_event_id;
  long retry_ms;
  int has_retry;
} cai_mcp_sse_resume_state;

typedef struct cai_mcp_sse_stream_state {
  cai_mcp_streamable_http_client_impl *impl;
  lonejson_spooled event_data;
  cai_mcp_sse_resume_state resume;
  cai_buffer_builder line;
  char event[64];
  cai_error *error;
  int event_ready;
  int failed;
  int rc;
} cai_mcp_sse_stream_state;

typedef struct cai_mcp_spooled_upload {
  lonejson_spooled cursor;
  int rewound;
} cai_mcp_spooled_upload;

typedef struct cai_mcp_spooled_reader {
  lonejson_spooled cursor;
} cai_mcp_spooled_reader;

typedef struct cai_mcp_jsonrpc_error_doc {
  int has_code;
  int64_t code;
  char *message;
} cai_mcp_jsonrpc_error_doc;

typedef struct cai_mcp_jsonrpc_sink_response_doc {
  lonejson_json_value result;
  cai_mcp_jsonrpc_error_doc error_doc;
} cai_mcp_jsonrpc_sink_response_doc;

typedef struct cai_mcp_jsonrpc_message_doc {
  char *jsonrpc;
  int has_id;
  lonejson_json_value id;
  char *method;
  lonejson_json_value params;
  lonejson_json_value result;
  cai_mcp_jsonrpc_error_doc error_doc;
} cai_mcp_jsonrpc_message_doc;

typedef struct cai_mcp_jsonrpc_id_doc {
  char *jsonrpc;
  lonejson_json_value id;
} cai_mcp_jsonrpc_id_doc;

typedef struct cai_mcp_list_tool_doc {
  char *name;
  char *description;
  char *title;
  lonejson_json_value input_schema;
  lonejson_json_value output_schema;
  lonejson_json_value annotations;
  lonejson_json_value icons;
  lonejson_json_value execution;
} cai_mcp_list_tool_doc;

typedef struct cai_mcp_tools_list_result_doc {
  lonejson_object_array tools;
  char *next_cursor;
} cai_mcp_tools_list_result_doc;

typedef struct cai_mcp_tools_list_response_doc {
  cai_mcp_tools_list_result_doc result;
  cai_mcp_jsonrpc_error_doc error_doc;
} cai_mcp_tools_list_response_doc;

typedef struct cai_mcp_resource_content_doc {
  char *uri;
  char *mime_type;
  char *text;
  char *blob;
} cai_mcp_resource_content_doc;

typedef struct cai_mcp_tool_content_doc {
  char *type;
  char *text;
  char *data;
  char *mime_type;
  char *uri;
  char *name;
  lonejson_json_value resource;
} cai_mcp_tool_content_doc;

typedef struct cai_mcp_sampling_content_doc {
  char *type;
  char *text;
  char *data;
  char *mime_type;
  char *id;
  char *name;
  char *tool_use_id;
  lonejson_json_value input;
  lonejson_json_value content;
  lonejson_json_value structured_content;
} cai_mcp_sampling_content_doc;

typedef struct cai_mcp_sampling_content_array_doc {
  lonejson_object_array content;
} cai_mcp_sampling_content_array_doc;

typedef struct cai_mcp_tool_call_result_doc {
  lonejson_object_array content;
  lonejson_json_value structured_content;
  int has_is_error;
  int is_error;
} cai_mcp_tool_call_result_doc;

typedef struct cai_mcp_tool_call_response_doc {
  cai_mcp_tool_call_result_doc result;
  cai_mcp_jsonrpc_error_doc error_doc;
} cai_mcp_tool_call_response_doc;

typedef struct cai_mcp_task_doc {
  char *task_id;
  char *status;
  char *created_at;
  char *last_updated_at;
  lonejson_json_value ttl;
} cai_mcp_task_doc;

typedef struct cai_mcp_create_task_result_doc {
  cai_mcp_task_doc task;
} cai_mcp_create_task_result_doc;

typedef struct cai_mcp_create_task_response_doc {
  cai_mcp_create_task_result_doc result;
  cai_mcp_jsonrpc_error_doc error_doc;
} cai_mcp_create_task_response_doc;

typedef struct cai_mcp_task_response_doc {
  cai_mcp_task_doc result;
  cai_mcp_jsonrpc_error_doc error_doc;
} cai_mcp_task_response_doc;

typedef struct cai_mcp_tasks_list_result_doc {
  lonejson_object_array tasks;
  char *next_cursor;
} cai_mcp_tasks_list_result_doc;

typedef struct cai_mcp_tasks_list_response_doc {
  cai_mcp_tasks_list_result_doc result;
  cai_mcp_jsonrpc_error_doc error_doc;
} cai_mcp_tasks_list_response_doc;

typedef struct cai_mcp_list_resource_doc {
  char *uri;
  char *name;
  char *title;
  char *description;
  char *mime_type;
  lonejson_json_value icons;
  lonejson_json_value annotations;
  int has_size;
  int64_t size;
} cai_mcp_list_resource_doc;

typedef struct cai_mcp_resources_list_result_doc {
  lonejson_object_array resources;
  char *next_cursor;
} cai_mcp_resources_list_result_doc;

typedef struct cai_mcp_resources_list_response_doc {
  cai_mcp_resources_list_result_doc result;
  cai_mcp_jsonrpc_error_doc error_doc;
} cai_mcp_resources_list_response_doc;

typedef struct cai_mcp_list_resource_template_doc {
  char *uri_template;
  char *name;
  char *title;
  char *description;
  char *mime_type;
  lonejson_json_value icons;
  lonejson_json_value annotations;
} cai_mcp_list_resource_template_doc;

typedef struct cai_mcp_resource_templates_list_result_doc {
  lonejson_object_array resource_templates;
  char *next_cursor;
} cai_mcp_resource_templates_list_result_doc;

typedef struct cai_mcp_resource_templates_list_response_doc {
  cai_mcp_resource_templates_list_result_doc result;
  cai_mcp_jsonrpc_error_doc error_doc;
} cai_mcp_resource_templates_list_response_doc;

typedef struct cai_mcp_list_prompt_doc {
  char *name;
  char *title;
  char *description;
  lonejson_json_value arguments;
  lonejson_json_value icons;
} cai_mcp_list_prompt_doc;

typedef struct cai_mcp_prompts_list_result_doc {
  lonejson_object_array prompts;
  char *next_cursor;
} cai_mcp_prompts_list_result_doc;

typedef struct cai_mcp_prompts_list_response_doc {
  cai_mcp_prompts_list_result_doc result;
  cai_mcp_jsonrpc_error_doc error_doc;
} cai_mcp_prompts_list_response_doc;

typedef struct cai_mcp_resource_read_result_doc {
  lonejson_object_array contents;
} cai_mcp_resource_read_result_doc;

typedef struct cai_mcp_resource_read_response_doc {
  cai_mcp_resource_read_result_doc result;
  cai_mcp_jsonrpc_error_doc error_doc;
} cai_mcp_resource_read_response_doc;

typedef struct cai_mcp_prompt_message_doc {
  char *role;
  lonejson_json_value content;
} cai_mcp_prompt_message_doc;

typedef struct cai_mcp_prompt_get_result_doc {
  char *description;
  lonejson_object_array messages;
} cai_mcp_prompt_get_result_doc;

typedef struct cai_mcp_prompt_get_response_doc {
  cai_mcp_prompt_get_result_doc result;
  cai_mcp_jsonrpc_error_doc error_doc;
} cai_mcp_prompt_get_response_doc;

typedef struct cai_mcp_completion_doc {
  lonejson_string_array values;
  int has_total;
  int64_t total;
  int has_has_more;
  int has_more;
} cai_mcp_completion_doc;

typedef struct cai_mcp_completion_result_doc {
  cai_mcp_completion_doc completion;
} cai_mcp_completion_result_doc;

typedef struct cai_mcp_completion_response_doc {
  cai_mcp_completion_result_doc result;
  cai_mcp_jsonrpc_error_doc error_doc;
} cai_mcp_completion_response_doc;

typedef struct cai_mcp_server_info_doc {
  char *name;
  char *version;
} cai_mcp_server_info_doc;

typedef struct cai_mcp_initialize_result_doc {
  char *protocol_version;
  lonejson_json_value capabilities;
  cai_mcp_server_info_doc server_info;
} cai_mcp_initialize_result_doc;

typedef struct cai_mcp_initialize_response_doc {
  cai_mcp_initialize_result_doc result;
  cai_mcp_jsonrpc_error_doc error_doc;
} cai_mcp_initialize_response_doc;

typedef struct cai_mcp_sampling_params_doc {
  lonejson_json_value messages;
  lonejson_json_value tools;
  lonejson_json_value tool_choice;
  char *include_context;
} cai_mcp_sampling_params_doc;

typedef struct cai_mcp_cancelled_params_doc {
  lonejson_json_value request_id;
  char *reason;
} cai_mcp_cancelled_params_doc;

typedef struct cai_mcp_progress_params_doc {
  lonejson_json_value progress_token;
  int has_progress;
  double progress;
  int has_total;
  double total;
  char *message;
} cai_mcp_progress_params_doc;

typedef struct cai_mcp_progress_token_doc {
  lonejson_json_value progress_token;
} cai_mcp_progress_token_doc;

typedef struct cai_mcp_logging_message_params_doc {
  char *level;
  char *logger;
  lonejson_json_value data;
} cai_mcp_logging_message_params_doc;

typedef struct cai_mcp_resource_updated_params_doc {
  char *uri;
} cai_mcp_resource_updated_params_doc;

typedef struct cai_mcp_elicitation_complete_params_doc {
  char *elicitation_id;
} cai_mcp_elicitation_complete_params_doc;

typedef struct cai_mcp_elicitation_params_doc {
  char *mode;
  char *message;
  lonejson_json_value requested_schema;
  char *url;
  char *elicitation_id;
} cai_mcp_elicitation_params_doc;

typedef struct cai_mcp_elicitation_schema_doc {
  char *type;
  lonejson_json_value properties;
} cai_mcp_elicitation_schema_doc;

typedef struct cai_mcp_root_doc {
  char *uri;
  char *name;
} cai_mcp_root_doc;

typedef struct cai_mcp_roots_list_result_doc {
  lonejson_object_array roots;
} cai_mcp_roots_list_result_doc;

typedef struct cai_mcp_sampling_result_doc {
  char *role;
  lonejson_json_value content;
  char *model;
  char *stop_reason;
} cai_mcp_sampling_result_doc;

typedef struct cai_mcp_elicitation_result_doc {
  char *action;
  lonejson_json_value content;
} cai_mcp_elicitation_result_doc;

typedef struct cai_mcp_registry_tool_context {
  cai_mcp_client *client;
  char *remote_name;
} cai_mcp_registry_tool_context;

static void
cai_mcp_streamable_reset_session(cai_mcp_streamable_http_client_impl *impl);
static void cai_mcp_spooled_cleanup_if_initialized(lonejson_spooled *spool);
static int cai_mcp_set_json_error(cai_error *error, const char *message,
                                  const lonejson_error *json_error);
static int cai_mcp_json_value_is_object(const lonejson_json_value *value,
                                        cai_error *error);
static int cai_mcp_json_value_root_is(const lonejson_json_value *value,
                                      char root, cai_error *error);
static int cai_mcp_spooled_root_is(const lonejson_spooled *json, int root,
                                   const char *message, cai_error *error);
static int cai_mcp_log_level_valid(const char *level);
static int cai_mcp_task_validate(const cai_mcp_task_doc *task,
                                 cai_error *error);
static int cai_mcp_validate_jsonrpc_id_value(const lonejson_json_value *id,
                                             cai_error *error);
static int cai_mcp_validate_roots_result(lonejson_spooled *json,
                                         cai_error *error);
static int cai_mcp_validate_sampling_result(lonejson_spooled *json,
                                            cai_error *error);
static int
cai_mcp_sampling_content_value_validate(const lonejson_json_value *content,
                                        cai_error *error);
static int cai_mcp_validate_elicitation_result(lonejson_spooled *json,
                                               cai_error *error);
static int cai_mcp_validate_elicitation_schema_properties(
    const lonejson_json_value *properties, cai_error *error);
static int cai_mcp_validate_sampling_params(
    const cai_mcp_streamable_http_client_impl *impl,
    lonejson_spooled *params_json, int *uses_tools, cai_error *error);
static int cai_mcp_validate_elicitation_params(
    const cai_mcp_streamable_http_client_impl *impl,
    lonejson_spooled *params_json, cai_error *error);
static char *cai_mcp_json_value_to_cstr(const lonejson_json_value *value,
                                        cai_error *error);
static int
cai_mcp_jsonrpc_response_result_error_presence(const lonejson_spooled *json,
                                               int *has_result, int *has_error,
                                               cai_error *error);
static int cai_mcp_jsonrpc_top_level_member_presence(
    const lonejson_spooled *json, const char *member, int *present,
    const char *message, cai_error *error);
static int cai_mcp_jsonrpc_response_result_root_is_object(
    const lonejson_spooled *json, int *is_object, cai_error *error);
static int cai_mcp_write_json_string(lonejson_spooled *spool, const char *text,
                                     cai_error *error);
static int
cai_mcp_sse_normalize_response(cai_mcp_streamable_http_client_impl *impl,
                               cai_mcp_http_response_capture *response,
                               int allow_resume, cai_error *error);
static int cai_mcp_get_resume_response(
    cai_mcp_streamable_http_client_impl *impl, const char *last_event_id,
    cai_mcp_http_response_capture *response, cai_error *error);

static const lonejson_field cai_mcp_jsonrpc_error_fields[] = {
    LONEJSON_FIELD_I64_PRESENT(cai_mcp_jsonrpc_error_doc, code, has_code,
                               "code"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_jsonrpc_error_doc, message, "message")};
LONEJSON_MAP_DEFINE(cai_mcp_jsonrpc_error_map, cai_mcp_jsonrpc_error_doc,
                    cai_mcp_jsonrpc_error_fields);

static const lonejson_field cai_mcp_jsonrpc_sink_response_fields[] = {
    LONEJSON_FIELD_JSON_VALUE(cai_mcp_jsonrpc_sink_response_doc, result,
                              "result"),
    LONEJSON_FIELD_OBJECT(cai_mcp_jsonrpc_sink_response_doc, error_doc, "error",
                          &cai_mcp_jsonrpc_error_map)};
LONEJSON_MAP_DEFINE(cai_mcp_jsonrpc_sink_response_map,
                    cai_mcp_jsonrpc_sink_response_doc,
                    cai_mcp_jsonrpc_sink_response_fields);

static const lonejson_field cai_mcp_jsonrpc_message_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_jsonrpc_message_doc, jsonrpc,
                                "jsonrpc"),
    {"id", LONEJSON__KEY_LEN("id"), LONEJSON__KEY_FIRST("id"),
     LONEJSON__KEY_LAST("id"), offsetof(cai_mcp_jsonrpc_message_doc, id),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT},
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_jsonrpc_message_doc, method, "method"),
    {"params", LONEJSON__KEY_LEN("params"), LONEJSON__KEY_FIRST("params"),
     LONEJSON__KEY_LAST("params"),
     offsetof(cai_mcp_jsonrpc_message_doc, params),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT},
    {"result", LONEJSON__KEY_LEN("result"), LONEJSON__KEY_FIRST("result"),
     LONEJSON__KEY_LAST("result"),
     offsetof(cai_mcp_jsonrpc_message_doc, result),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT},
    LONEJSON_FIELD_OBJECT(cai_mcp_jsonrpc_message_doc, error_doc, "error",
                          &cai_mcp_jsonrpc_error_map)};
LONEJSON_MAP_DEFINE(cai_mcp_jsonrpc_message_map, cai_mcp_jsonrpc_message_doc,
                    cai_mcp_jsonrpc_message_fields);

static const lonejson_field cai_mcp_jsonrpc_id_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_jsonrpc_id_doc, jsonrpc, "jsonrpc"),
    {"id", LONEJSON__KEY_LEN("id"), LONEJSON__KEY_FIRST("id"),
     LONEJSON__KEY_LAST("id"), offsetof(cai_mcp_jsonrpc_id_doc, id),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT}};
LONEJSON_MAP_DEFINE(cai_mcp_jsonrpc_id_map, cai_mcp_jsonrpc_id_doc,
                    cai_mcp_jsonrpc_id_fields);

static const lonejson_field cai_mcp_sampling_params_fields[] = {
    {"messages", LONEJSON__KEY_LEN("messages"), LONEJSON__KEY_FIRST("messages"),
     LONEJSON__KEY_LAST("messages"),
     offsetof(cai_mcp_sampling_params_doc, messages),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL,
     LONEJSON_FIELD_REQUIRED | LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U,
     0U, NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT},
    {"tools", LONEJSON__KEY_LEN("tools"), LONEJSON__KEY_FIRST("tools"),
     LONEJSON__KEY_LAST("tools"), offsetof(cai_mcp_sampling_params_doc, tools),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT},
    {"toolChoice", LONEJSON__KEY_LEN("toolChoice"),
     LONEJSON__KEY_FIRST("toolChoice"), LONEJSON__KEY_LAST("toolChoice"),
     offsetof(cai_mcp_sampling_params_doc, tool_choice),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT},
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_sampling_params_doc, include_context,
                                "includeContext")};
LONEJSON_MAP_DEFINE(cai_mcp_sampling_params_map, cai_mcp_sampling_params_doc,
                    cai_mcp_sampling_params_fields);

static const lonejson_field cai_mcp_cancelled_params_fields[] = {
    {"requestId", LONEJSON__KEY_LEN("requestId"),
     LONEJSON__KEY_FIRST("requestId"), LONEJSON__KEY_LAST("requestId"),
     offsetof(cai_mcp_cancelled_params_doc, request_id),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL,
     LONEJSON_FIELD_REQUIRED | LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U,
     0U, NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT},
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_cancelled_params_doc, reason,
                                "reason")};
LONEJSON_MAP_DEFINE(cai_mcp_cancelled_params_map, cai_mcp_cancelled_params_doc,
                    cai_mcp_cancelled_params_fields);

static const lonejson_field cai_mcp_progress_params_fields[] = {
    {"progressToken", LONEJSON__KEY_LEN("progressToken"),
     LONEJSON__KEY_FIRST("progressToken"), LONEJSON__KEY_LAST("progressToken"),
     offsetof(cai_mcp_progress_params_doc, progress_token),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL,
     LONEJSON_FIELD_REQUIRED | LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U,
     0U, NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT},
    LONEJSON_FIELD_F64_PRESENT(cai_mcp_progress_params_doc, progress,
                               has_progress, "progress"),
    LONEJSON_FIELD_F64_PRESENT(cai_mcp_progress_params_doc, total, has_total,
                               "total"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_progress_params_doc, message,
                                "message")};
LONEJSON_MAP_DEFINE(cai_mcp_progress_params_map, cai_mcp_progress_params_doc,
                    cai_mcp_progress_params_fields);

static const lonejson_field cai_mcp_progress_token_fields[] = {
    {"progressToken", LONEJSON__KEY_LEN("progressToken"),
     LONEJSON__KEY_FIRST("progressToken"), LONEJSON__KEY_LAST("progressToken"),
     offsetof(cai_mcp_progress_token_doc, progress_token),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL,
     LONEJSON_FIELD_REQUIRED | LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U,
     0U, NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT}};
LONEJSON_MAP_DEFINE(cai_mcp_progress_token_map, cai_mcp_progress_token_doc,
                    cai_mcp_progress_token_fields);

static const lonejson_field cai_mcp_logging_message_params_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_logging_message_params_doc, level,
                                    "level"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_logging_message_params_doc, logger,
                                "logger"),
    {"data", LONEJSON__KEY_LEN("data"), LONEJSON__KEY_FIRST("data"),
     LONEJSON__KEY_LAST("data"),
     offsetof(cai_mcp_logging_message_params_doc, data),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL,
     LONEJSON_FIELD_REQUIRED | LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U,
     0U, NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT}};
LONEJSON_MAP_DEFINE(cai_mcp_logging_message_params_map,
                    cai_mcp_logging_message_params_doc,
                    cai_mcp_logging_message_params_fields);

static const lonejson_field cai_mcp_resource_updated_params_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_resource_updated_params_doc, uri,
                                    "uri")};
LONEJSON_MAP_DEFINE(cai_mcp_resource_updated_params_map,
                    cai_mcp_resource_updated_params_doc,
                    cai_mcp_resource_updated_params_fields);

static const lonejson_field cai_mcp_elicitation_complete_params_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_elicitation_complete_params_doc,
                                    elicitation_id, "elicitationId")};
LONEJSON_MAP_DEFINE(cai_mcp_elicitation_complete_params_map,
                    cai_mcp_elicitation_complete_params_doc,
                    cai_mcp_elicitation_complete_params_fields);

static const lonejson_field cai_mcp_elicitation_params_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_elicitation_params_doc, mode, "mode"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_elicitation_params_doc, message,
                                    "message"),
    {"requestedSchema", LONEJSON__KEY_LEN("requestedSchema"),
     LONEJSON__KEY_FIRST("requestedSchema"),
     LONEJSON__KEY_LAST("requestedSchema"),
     offsetof(cai_mcp_elicitation_params_doc, requested_schema),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT},
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_elicitation_params_doc, url, "url"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_elicitation_params_doc, elicitation_id,
                                "elicitationId")};
LONEJSON_MAP_DEFINE(cai_mcp_elicitation_params_map,
                    cai_mcp_elicitation_params_doc,
                    cai_mcp_elicitation_params_fields);

static const lonejson_field cai_mcp_elicitation_schema_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_elicitation_schema_doc, type,
                                    "type"),
    {"properties", LONEJSON__KEY_LEN("properties"),
     LONEJSON__KEY_FIRST("properties"), LONEJSON__KEY_LAST("properties"),
     offsetof(cai_mcp_elicitation_schema_doc, properties),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT}};
LONEJSON_MAP_DEFINE(cai_mcp_elicitation_schema_map,
                    cai_mcp_elicitation_schema_doc,
                    cai_mcp_elicitation_schema_fields);

static const lonejson_field cai_mcp_root_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_root_doc, uri, "uri"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_root_doc, name, "name")};
LONEJSON_MAP_DEFINE(cai_mcp_root_map, cai_mcp_root_doc, cai_mcp_root_fields);

static const lonejson_field cai_mcp_roots_list_result_fields[] = {
    {"roots", LONEJSON__KEY_LEN("roots"), LONEJSON__KEY_FIRST("roots"),
     LONEJSON__KEY_LAST("roots"),
     offsetof(cai_mcp_roots_list_result_doc, roots),
     LONEJSON_FIELD_KIND_OBJECT_ARRAY, LONEJSON_STORAGE_DYNAMIC,
     LONEJSON_OVERFLOW_FAIL, LONEJSON_FIELD_REQUIRED, 0U,
     sizeof(cai_mcp_root_doc), &cai_mcp_root_map, NULL, 0U,
     LONEJSON_SPOOL_CLASS_DEFAULT}};
LONEJSON_MAP_DEFINE(cai_mcp_roots_list_result_map,
                    cai_mcp_roots_list_result_doc,
                    cai_mcp_roots_list_result_fields);

static const lonejson_field cai_mcp_sampling_result_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_sampling_result_doc, role, "role"),
    {"content", LONEJSON__KEY_LEN("content"), LONEJSON__KEY_FIRST("content"),
     LONEJSON__KEY_LAST("content"),
     offsetof(cai_mcp_sampling_result_doc, content),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL,
     LONEJSON_FIELD_REQUIRED | LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U,
     0U, NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT},
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_sampling_result_doc, model,
                                    "model"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_sampling_result_doc, stop_reason,
                                "stopReason")};
LONEJSON_MAP_DEFINE(cai_mcp_sampling_result_map, cai_mcp_sampling_result_doc,
                    cai_mcp_sampling_result_fields);

static const lonejson_field cai_mcp_elicitation_result_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_elicitation_result_doc, action,
                                    "action"),
    {"content", LONEJSON__KEY_LEN("content"), LONEJSON__KEY_FIRST("content"),
     LONEJSON__KEY_LAST("content"),
     offsetof(cai_mcp_elicitation_result_doc, content),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT}};
LONEJSON_MAP_DEFINE(cai_mcp_elicitation_result_map,
                    cai_mcp_elicitation_result_doc,
                    cai_mcp_elicitation_result_fields);

static const lonejson_field cai_mcp_list_tool_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_list_tool_doc, name, "name"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_list_tool_doc, description,
                                "description"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_list_tool_doc, title, "title"),
    {"inputSchema", LONEJSON__KEY_LEN("inputSchema"),
     LONEJSON__KEY_FIRST("inputSchema"), LONEJSON__KEY_LAST("inputSchema"),
     offsetof(cai_mcp_list_tool_doc, input_schema),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL,
     LONEJSON_FIELD_REQUIRED | LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U,
     0U, NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT},
    {"outputSchema", LONEJSON__KEY_LEN("outputSchema"),
     LONEJSON__KEY_FIRST("outputSchema"), LONEJSON__KEY_LAST("outputSchema"),
     offsetof(cai_mcp_list_tool_doc, output_schema),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT},
    {"annotations", LONEJSON__KEY_LEN("annotations"),
     LONEJSON__KEY_FIRST("annotations"), LONEJSON__KEY_LAST("annotations"),
     offsetof(cai_mcp_list_tool_doc, annotations),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT},
    {"icons", LONEJSON__KEY_LEN("icons"), LONEJSON__KEY_FIRST("icons"),
     LONEJSON__KEY_LAST("icons"), offsetof(cai_mcp_list_tool_doc, icons),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT},
    {"execution", LONEJSON__KEY_LEN("execution"),
     LONEJSON__KEY_FIRST("execution"), LONEJSON__KEY_LAST("execution"),
     offsetof(cai_mcp_list_tool_doc, execution), LONEJSON_FIELD_KIND_JSON_VALUE,
     LONEJSON_STORAGE_FIXED, LONEJSON_OVERFLOW_FAIL,
     LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U, NULL, NULL, 0U,
     LONEJSON_SPOOL_CLASS_DEFAULT}};
LONEJSON_MAP_DEFINE(cai_mcp_list_tool_map, cai_mcp_list_tool_doc,
                    cai_mcp_list_tool_fields);

static const lonejson_field cai_mcp_tools_list_result_fields[] = {
    {"tools", LONEJSON__KEY_LEN("tools"), LONEJSON__KEY_FIRST("tools"),
     LONEJSON__KEY_LAST("tools"),
     offsetof(cai_mcp_tools_list_result_doc, tools),
     LONEJSON_FIELD_KIND_OBJECT_ARRAY, LONEJSON_STORAGE_DYNAMIC,
     LONEJSON_OVERFLOW_FAIL, LONEJSON_FIELD_REQUIRED, 0U,
     sizeof(cai_mcp_list_tool_doc), &cai_mcp_list_tool_map, NULL, 0U,
     LONEJSON_SPOOL_CLASS_DEFAULT},
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_tools_list_result_doc, next_cursor,
                                "nextCursor")};
LONEJSON_MAP_DEFINE(cai_mcp_tools_list_result_map,
                    cai_mcp_tools_list_result_doc,
                    cai_mcp_tools_list_result_fields);

static const lonejson_field cai_mcp_tools_list_response_fields[] = {
    LONEJSON_FIELD_OBJECT(cai_mcp_tools_list_response_doc, result, "result",
                          &cai_mcp_tools_list_result_map),
    LONEJSON_FIELD_OBJECT(cai_mcp_tools_list_response_doc, error_doc, "error",
                          &cai_mcp_jsonrpc_error_map)};
LONEJSON_MAP_DEFINE(cai_mcp_tools_list_response_map,
                    cai_mcp_tools_list_response_doc,
                    cai_mcp_tools_list_response_fields);

static const lonejson_field cai_mcp_tool_content_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_tool_content_doc, type, "type"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_tool_content_doc, text, "text"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_tool_content_doc, data, "data"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_tool_content_doc, mime_type,
                                "mimeType"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_tool_content_doc, uri, "uri"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_tool_content_doc, name, "name"),
    {"resource", LONEJSON__KEY_LEN("resource"), LONEJSON__KEY_FIRST("resource"),
     LONEJSON__KEY_LAST("resource"),
     offsetof(cai_mcp_tool_content_doc, resource),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT}};
LONEJSON_MAP_DEFINE(cai_mcp_tool_content_map, cai_mcp_tool_content_doc,
                    cai_mcp_tool_content_fields);

static const lonejson_field cai_mcp_sampling_content_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_sampling_content_doc, type, "type"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_sampling_content_doc, text, "text"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_sampling_content_doc, data, "data"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_sampling_content_doc, mime_type,
                                "mimeType"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_sampling_content_doc, id, "id"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_sampling_content_doc, name, "name"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_sampling_content_doc, tool_use_id,
                                "toolUseId"),
    {"input", LONEJSON__KEY_LEN("input"), LONEJSON__KEY_FIRST("input"),
     LONEJSON__KEY_LAST("input"), offsetof(cai_mcp_sampling_content_doc, input),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT},
    {"content", LONEJSON__KEY_LEN("content"), LONEJSON__KEY_FIRST("content"),
     LONEJSON__KEY_LAST("content"),
     offsetof(cai_mcp_sampling_content_doc, content),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT},
    {"structuredContent", LONEJSON__KEY_LEN("structuredContent"),
     LONEJSON__KEY_FIRST("structuredContent"),
     LONEJSON__KEY_LAST("structuredContent"),
     offsetof(cai_mcp_sampling_content_doc, structured_content),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT}};
LONEJSON_MAP_DEFINE(cai_mcp_sampling_content_map, cai_mcp_sampling_content_doc,
                    cai_mcp_sampling_content_fields);

static const lonejson_field cai_mcp_sampling_content_array_fields[] = {
    {"content", LONEJSON__KEY_LEN("content"), LONEJSON__KEY_FIRST("content"),
     LONEJSON__KEY_LAST("content"),
     offsetof(cai_mcp_sampling_content_array_doc, content),
     LONEJSON_FIELD_KIND_OBJECT_ARRAY, LONEJSON_STORAGE_DYNAMIC,
     LONEJSON_OVERFLOW_FAIL, LONEJSON_FIELD_REQUIRED, 0U,
     sizeof(cai_mcp_sampling_content_doc), &cai_mcp_sampling_content_map, NULL,
     0U, LONEJSON_SPOOL_CLASS_DEFAULT}};
LONEJSON_MAP_DEFINE(cai_mcp_sampling_content_array_map,
                    cai_mcp_sampling_content_array_doc,
                    cai_mcp_sampling_content_array_fields);

static const lonejson_field cai_mcp_tool_call_result_fields[] = {
    {"content", LONEJSON__KEY_LEN("content"), LONEJSON__KEY_FIRST("content"),
     LONEJSON__KEY_LAST("content"),
     offsetof(cai_mcp_tool_call_result_doc, content),
     LONEJSON_FIELD_KIND_OBJECT_ARRAY, LONEJSON_STORAGE_DYNAMIC,
     LONEJSON_OVERFLOW_FAIL, LONEJSON_FIELD_REQUIRED, 0U,
     sizeof(cai_mcp_tool_content_doc), &cai_mcp_tool_content_map, NULL, 0U,
     LONEJSON_SPOOL_CLASS_DEFAULT},
    {"structuredContent", LONEJSON__KEY_LEN("structuredContent"),
     LONEJSON__KEY_FIRST("structuredContent"),
     LONEJSON__KEY_LAST("structuredContent"),
     offsetof(cai_mcp_tool_call_result_doc, structured_content),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT},
    LONEJSON_FIELD_BOOL_PRESENT(cai_mcp_tool_call_result_doc, is_error,
                                has_is_error, "isError")};
LONEJSON_MAP_DEFINE(cai_mcp_tool_call_result_map, cai_mcp_tool_call_result_doc,
                    cai_mcp_tool_call_result_fields);

static const lonejson_field cai_mcp_tool_call_response_fields[] = {
    LONEJSON_FIELD_OBJECT(cai_mcp_tool_call_response_doc, result, "result",
                          &cai_mcp_tool_call_result_map),
    LONEJSON_FIELD_OBJECT(cai_mcp_tool_call_response_doc, error_doc, "error",
                          &cai_mcp_jsonrpc_error_map)};
LONEJSON_MAP_DEFINE(cai_mcp_tool_call_response_map,
                    cai_mcp_tool_call_response_doc,
                    cai_mcp_tool_call_response_fields);

static const lonejson_field cai_mcp_task_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_task_doc, task_id, "taskId"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_task_doc, status, "status"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_task_doc, created_at, "createdAt"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_task_doc, last_updated_at,
                                    "lastUpdatedAt"),
    {"ttl", LONEJSON__KEY_LEN("ttl"), LONEJSON__KEY_FIRST("ttl"),
     LONEJSON__KEY_LAST("ttl"), offsetof(cai_mcp_task_doc, ttl),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL,
     LONEJSON_FIELD_REQUIRED | LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U,
     0U, NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT}};
LONEJSON_MAP_DEFINE(cai_mcp_task_map, cai_mcp_task_doc, cai_mcp_task_fields);

static const lonejson_field cai_mcp_create_task_result_fields[] = {
    LONEJSON_FIELD_OBJECT_REQ(cai_mcp_create_task_result_doc, task, "task",
                              &cai_mcp_task_map)};
LONEJSON_MAP_DEFINE(cai_mcp_create_task_result_map,
                    cai_mcp_create_task_result_doc,
                    cai_mcp_create_task_result_fields);

static const lonejson_field cai_mcp_create_task_response_fields[] = {
    LONEJSON_FIELD_OBJECT(cai_mcp_create_task_response_doc, result, "result",
                          &cai_mcp_create_task_result_map),
    LONEJSON_FIELD_OBJECT(cai_mcp_create_task_response_doc, error_doc, "error",
                          &cai_mcp_jsonrpc_error_map)};
LONEJSON_MAP_DEFINE(cai_mcp_create_task_response_map,
                    cai_mcp_create_task_response_doc,
                    cai_mcp_create_task_response_fields);

static const lonejson_field cai_mcp_task_response_fields[] = {
    LONEJSON_FIELD_OBJECT(cai_mcp_task_response_doc, result, "result",
                          &cai_mcp_task_map),
    LONEJSON_FIELD_OBJECT(cai_mcp_task_response_doc, error_doc, "error",
                          &cai_mcp_jsonrpc_error_map)};
LONEJSON_MAP_DEFINE(cai_mcp_task_response_map, cai_mcp_task_response_doc,
                    cai_mcp_task_response_fields);

static const lonejson_field cai_mcp_tasks_list_result_fields[] = {
    {"tasks", LONEJSON__KEY_LEN("tasks"), LONEJSON__KEY_FIRST("tasks"),
     LONEJSON__KEY_LAST("tasks"),
     offsetof(cai_mcp_tasks_list_result_doc, tasks),
     LONEJSON_FIELD_KIND_OBJECT_ARRAY, LONEJSON_STORAGE_DYNAMIC,
     LONEJSON_OVERFLOW_FAIL, LONEJSON_FIELD_REQUIRED, 0U,
     sizeof(cai_mcp_task_doc), &cai_mcp_task_map, NULL, 0U,
     LONEJSON_SPOOL_CLASS_DEFAULT},
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_tasks_list_result_doc, next_cursor,
                                "nextCursor")};
LONEJSON_MAP_DEFINE(cai_mcp_tasks_list_result_map,
                    cai_mcp_tasks_list_result_doc,
                    cai_mcp_tasks_list_result_fields);

static const lonejson_field cai_mcp_tasks_list_response_fields[] = {
    LONEJSON_FIELD_OBJECT(cai_mcp_tasks_list_response_doc, result, "result",
                          &cai_mcp_tasks_list_result_map),
    LONEJSON_FIELD_OBJECT(cai_mcp_tasks_list_response_doc, error_doc, "error",
                          &cai_mcp_jsonrpc_error_map)};
LONEJSON_MAP_DEFINE(cai_mcp_tasks_list_response_map,
                    cai_mcp_tasks_list_response_doc,
                    cai_mcp_tasks_list_response_fields);

static const lonejson_field cai_mcp_list_resource_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_list_resource_doc, uri, "uri"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_list_resource_doc, name, "name"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_list_resource_doc, title, "title"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_list_resource_doc, description,
                                "description"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_list_resource_doc, mime_type,
                                "mimeType"),
    {"icons", LONEJSON__KEY_LEN("icons"), LONEJSON__KEY_FIRST("icons"),
     LONEJSON__KEY_LAST("icons"), offsetof(cai_mcp_list_resource_doc, icons),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT},
    {"annotations", LONEJSON__KEY_LEN("annotations"),
     LONEJSON__KEY_FIRST("annotations"), LONEJSON__KEY_LAST("annotations"),
     offsetof(cai_mcp_list_resource_doc, annotations),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT},
    LONEJSON_FIELD_I64_PRESENT(cai_mcp_list_resource_doc, size, has_size,
                               "size")};
LONEJSON_MAP_DEFINE(cai_mcp_list_resource_map, cai_mcp_list_resource_doc,
                    cai_mcp_list_resource_fields);

static const lonejson_field cai_mcp_resources_list_result_fields[] = {
    {"resources", LONEJSON__KEY_LEN("resources"),
     LONEJSON__KEY_FIRST("resources"), LONEJSON__KEY_LAST("resources"),
     offsetof(cai_mcp_resources_list_result_doc, resources),
     LONEJSON_FIELD_KIND_OBJECT_ARRAY, LONEJSON_STORAGE_DYNAMIC,
     LONEJSON_OVERFLOW_FAIL, LONEJSON_FIELD_REQUIRED, 0U,
     sizeof(cai_mcp_list_resource_doc), &cai_mcp_list_resource_map, NULL, 0U,
     LONEJSON_SPOOL_CLASS_DEFAULT},
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_resources_list_result_doc, next_cursor,
                                "nextCursor")};
LONEJSON_MAP_DEFINE(cai_mcp_resources_list_result_map,
                    cai_mcp_resources_list_result_doc,
                    cai_mcp_resources_list_result_fields);

static const lonejson_field cai_mcp_resources_list_response_fields[] = {
    LONEJSON_FIELD_OBJECT(cai_mcp_resources_list_response_doc, result, "result",
                          &cai_mcp_resources_list_result_map),
    LONEJSON_FIELD_OBJECT(cai_mcp_resources_list_response_doc, error_doc,
                          "error", &cai_mcp_jsonrpc_error_map)};
LONEJSON_MAP_DEFINE(cai_mcp_resources_list_response_map,
                    cai_mcp_resources_list_response_doc,
                    cai_mcp_resources_list_response_fields);

static const lonejson_field cai_mcp_list_resource_template_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_list_resource_template_doc,
                                    uri_template, "uriTemplate"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_list_resource_template_doc, name,
                                    "name"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_list_resource_template_doc, title,
                                "title"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_list_resource_template_doc, description,
                                "description"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_list_resource_template_doc, mime_type,
                                "mimeType"),
    {"icons", LONEJSON__KEY_LEN("icons"), LONEJSON__KEY_FIRST("icons"),
     LONEJSON__KEY_LAST("icons"),
     offsetof(cai_mcp_list_resource_template_doc, icons),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT},
    {"annotations", LONEJSON__KEY_LEN("annotations"),
     LONEJSON__KEY_FIRST("annotations"), LONEJSON__KEY_LAST("annotations"),
     offsetof(cai_mcp_list_resource_template_doc, annotations),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT}};
LONEJSON_MAP_DEFINE(cai_mcp_list_resource_template_map,
                    cai_mcp_list_resource_template_doc,
                    cai_mcp_list_resource_template_fields);

static const lonejson_field cai_mcp_resource_templates_list_result_fields[] = {
    {"resourceTemplates", LONEJSON__KEY_LEN("resourceTemplates"),
     LONEJSON__KEY_FIRST("resourceTemplates"),
     LONEJSON__KEY_LAST("resourceTemplates"),
     offsetof(cai_mcp_resource_templates_list_result_doc, resource_templates),
     LONEJSON_FIELD_KIND_OBJECT_ARRAY, LONEJSON_STORAGE_DYNAMIC,
     LONEJSON_OVERFLOW_FAIL, LONEJSON_FIELD_REQUIRED, 0U,
     sizeof(cai_mcp_list_resource_template_doc),
     &cai_mcp_list_resource_template_map, NULL, 0U,
     LONEJSON_SPOOL_CLASS_DEFAULT},
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_resource_templates_list_result_doc,
                                next_cursor, "nextCursor")};
LONEJSON_MAP_DEFINE(cai_mcp_resource_templates_list_result_map,
                    cai_mcp_resource_templates_list_result_doc,
                    cai_mcp_resource_templates_list_result_fields);

static const lonejson_field cai_mcp_resource_templates_list_response_fields[] =
    {LONEJSON_FIELD_OBJECT(cai_mcp_resource_templates_list_response_doc, result,
                           "result",
                           &cai_mcp_resource_templates_list_result_map),
     LONEJSON_FIELD_OBJECT(cai_mcp_resource_templates_list_response_doc,
                           error_doc, "error", &cai_mcp_jsonrpc_error_map)};
LONEJSON_MAP_DEFINE(cai_mcp_resource_templates_list_response_map,
                    cai_mcp_resource_templates_list_response_doc,
                    cai_mcp_resource_templates_list_response_fields);

static const lonejson_field cai_mcp_list_prompt_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_list_prompt_doc, name, "name"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_list_prompt_doc, title, "title"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_list_prompt_doc, description,
                                "description"),
    {"arguments", LONEJSON__KEY_LEN("arguments"),
     LONEJSON__KEY_FIRST("arguments"), LONEJSON__KEY_LAST("arguments"),
     offsetof(cai_mcp_list_prompt_doc, arguments),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT},
    {"icons", LONEJSON__KEY_LEN("icons"), LONEJSON__KEY_FIRST("icons"),
     LONEJSON__KEY_LAST("icons"), offsetof(cai_mcp_list_prompt_doc, icons),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT}};
LONEJSON_MAP_DEFINE(cai_mcp_list_prompt_map, cai_mcp_list_prompt_doc,
                    cai_mcp_list_prompt_fields);

static const lonejson_field cai_mcp_prompts_list_result_fields[] = {
    {"prompts", LONEJSON__KEY_LEN("prompts"), LONEJSON__KEY_FIRST("prompts"),
     LONEJSON__KEY_LAST("prompts"),
     offsetof(cai_mcp_prompts_list_result_doc, prompts),
     LONEJSON_FIELD_KIND_OBJECT_ARRAY, LONEJSON_STORAGE_DYNAMIC,
     LONEJSON_OVERFLOW_FAIL, LONEJSON_FIELD_REQUIRED, 0U,
     sizeof(cai_mcp_list_prompt_doc), &cai_mcp_list_prompt_map, NULL, 0U,
     LONEJSON_SPOOL_CLASS_DEFAULT},
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_prompts_list_result_doc, next_cursor,
                                "nextCursor")};
LONEJSON_MAP_DEFINE(cai_mcp_prompts_list_result_map,
                    cai_mcp_prompts_list_result_doc,
                    cai_mcp_prompts_list_result_fields);

static const lonejson_field cai_mcp_prompts_list_response_fields[] = {
    LONEJSON_FIELD_OBJECT(cai_mcp_prompts_list_response_doc, result, "result",
                          &cai_mcp_prompts_list_result_map),
    LONEJSON_FIELD_OBJECT(cai_mcp_prompts_list_response_doc, error_doc, "error",
                          &cai_mcp_jsonrpc_error_map)};
LONEJSON_MAP_DEFINE(cai_mcp_prompts_list_response_map,
                    cai_mcp_prompts_list_response_doc,
                    cai_mcp_prompts_list_response_fields);

static const lonejson_field cai_mcp_resource_content_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_resource_content_doc, uri, "uri"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_resource_content_doc, mime_type,
                                "mimeType"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_resource_content_doc, text, "text"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_resource_content_doc, blob, "blob")};
LONEJSON_MAP_DEFINE(cai_mcp_resource_content_map, cai_mcp_resource_content_doc,
                    cai_mcp_resource_content_fields);

static const lonejson_field cai_mcp_resource_read_result_fields[] = {
    {"contents", LONEJSON__KEY_LEN("contents"), LONEJSON__KEY_FIRST("contents"),
     LONEJSON__KEY_LAST("contents"),
     offsetof(cai_mcp_resource_read_result_doc, contents),
     LONEJSON_FIELD_KIND_OBJECT_ARRAY, LONEJSON_STORAGE_DYNAMIC,
     LONEJSON_OVERFLOW_FAIL, LONEJSON_FIELD_REQUIRED, 0U,
     sizeof(cai_mcp_resource_content_doc), &cai_mcp_resource_content_map, NULL,
     0U, LONEJSON_SPOOL_CLASS_DEFAULT}};
LONEJSON_MAP_DEFINE(cai_mcp_resource_read_result_map,
                    cai_mcp_resource_read_result_doc,
                    cai_mcp_resource_read_result_fields);

static const lonejson_field cai_mcp_resource_read_response_fields[] = {
    LONEJSON_FIELD_OBJECT(cai_mcp_resource_read_response_doc, result, "result",
                          &cai_mcp_resource_read_result_map),
    LONEJSON_FIELD_OBJECT(cai_mcp_resource_read_response_doc, error_doc,
                          "error", &cai_mcp_jsonrpc_error_map)};
LONEJSON_MAP_DEFINE(cai_mcp_resource_read_response_map,
                    cai_mcp_resource_read_response_doc,
                    cai_mcp_resource_read_response_fields);

static const lonejson_field cai_mcp_prompt_message_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_prompt_message_doc, role, "role"),
    {"content", LONEJSON__KEY_LEN("content"), LONEJSON__KEY_FIRST("content"),
     LONEJSON__KEY_LAST("content"),
     offsetof(cai_mcp_prompt_message_doc, content),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL,
     LONEJSON_FIELD_REQUIRED | LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U,
     0U, NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT}};
LONEJSON_MAP_DEFINE(cai_mcp_prompt_message_map, cai_mcp_prompt_message_doc,
                    cai_mcp_prompt_message_fields);

static const lonejson_field cai_mcp_prompt_get_result_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_prompt_get_result_doc, description,
                                "description"),
    {"messages", LONEJSON__KEY_LEN("messages"), LONEJSON__KEY_FIRST("messages"),
     LONEJSON__KEY_LAST("messages"),
     offsetof(cai_mcp_prompt_get_result_doc, messages),
     LONEJSON_FIELD_KIND_OBJECT_ARRAY, LONEJSON_STORAGE_DYNAMIC,
     LONEJSON_OVERFLOW_FAIL, LONEJSON_FIELD_REQUIRED, 0U,
     sizeof(cai_mcp_prompt_message_doc), &cai_mcp_prompt_message_map, NULL, 0U,
     LONEJSON_SPOOL_CLASS_DEFAULT}};
LONEJSON_MAP_DEFINE(cai_mcp_prompt_get_result_map,
                    cai_mcp_prompt_get_result_doc,
                    cai_mcp_prompt_get_result_fields);

static const lonejson_field cai_mcp_prompt_get_response_fields[] = {
    LONEJSON_FIELD_OBJECT(cai_mcp_prompt_get_response_doc, result, "result",
                          &cai_mcp_prompt_get_result_map),
    LONEJSON_FIELD_OBJECT(cai_mcp_prompt_get_response_doc, error_doc, "error",
                          &cai_mcp_jsonrpc_error_map)};
LONEJSON_MAP_DEFINE(cai_mcp_prompt_get_response_map,
                    cai_mcp_prompt_get_response_doc,
                    cai_mcp_prompt_get_response_fields);

static const lonejson_field cai_mcp_completion_fields[] = {
    {"values", LONEJSON__KEY_LEN("values"), LONEJSON__KEY_FIRST("values"),
     LONEJSON__KEY_LAST("values"), offsetof(cai_mcp_completion_doc, values),
     LONEJSON_FIELD_KIND_STRING_ARRAY, LONEJSON_STORAGE_DYNAMIC,
     LONEJSON_OVERFLOW_FAIL, LONEJSON_FIELD_REQUIRED, 0U, 0U, NULL, NULL, 0U,
     LONEJSON_SPOOL_CLASS_DEFAULT},
    LONEJSON_FIELD_I64_PRESENT(cai_mcp_completion_doc, total, has_total,
                               "total"),
    LONEJSON_FIELD_BOOL_PRESENT(cai_mcp_completion_doc, has_more, has_has_more,
                                "hasMore")};
LONEJSON_MAP_DEFINE(cai_mcp_completion_map, cai_mcp_completion_doc,
                    cai_mcp_completion_fields);

static const lonejson_field cai_mcp_completion_result_fields[] = {
    LONEJSON_FIELD_OBJECT_REQ(cai_mcp_completion_result_doc, completion,
                              "completion", &cai_mcp_completion_map)};
LONEJSON_MAP_DEFINE(cai_mcp_completion_result_map,
                    cai_mcp_completion_result_doc,
                    cai_mcp_completion_result_fields);

static const lonejson_field cai_mcp_completion_response_fields[] = {
    LONEJSON_FIELD_OBJECT(cai_mcp_completion_response_doc, result, "result",
                          &cai_mcp_completion_result_map),
    LONEJSON_FIELD_OBJECT(cai_mcp_completion_response_doc, error_doc, "error",
                          &cai_mcp_jsonrpc_error_map)};
LONEJSON_MAP_DEFINE(cai_mcp_completion_response_map,
                    cai_mcp_completion_response_doc,
                    cai_mcp_completion_response_fields);

static const lonejson_field cai_mcp_server_info_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_server_info_doc, name, "name"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_server_info_doc, version,
                                    "version")};
LONEJSON_MAP_DEFINE(cai_mcp_server_info_map, cai_mcp_server_info_doc,
                    cai_mcp_server_info_fields);

static const lonejson_field cai_mcp_initialize_result_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_initialize_result_doc, protocol_version,
                                "protocolVersion"),
    {"capabilities", LONEJSON__KEY_LEN("capabilities"),
     LONEJSON__KEY_FIRST("capabilities"), LONEJSON__KEY_LAST("capabilities"),
     offsetof(cai_mcp_initialize_result_doc, capabilities),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL,
     LONEJSON_FIELD_REQUIRED | LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U,
     0U, NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT},
    LONEJSON_FIELD_OBJECT_REQ(cai_mcp_initialize_result_doc, server_info,
                              "serverInfo", &cai_mcp_server_info_map)};
LONEJSON_MAP_DEFINE(cai_mcp_initialize_result_map,
                    cai_mcp_initialize_result_doc,
                    cai_mcp_initialize_result_fields);

static const lonejson_field cai_mcp_initialize_response_fields[] = {
    LONEJSON_FIELD_OBJECT(cai_mcp_initialize_response_doc, result, "result",
                          &cai_mcp_initialize_result_map),
    LONEJSON_FIELD_OBJECT(cai_mcp_initialize_response_doc, error_doc, "error",
                          &cai_mcp_jsonrpc_error_map)};
LONEJSON_MAP_DEFINE(cai_mcp_initialize_response_map,
                    cai_mcp_initialize_response_doc,
                    cai_mcp_initialize_response_fields);

static cai_mcp_streamable_http_client_impl *
cai_mcp_streamable_impl(cai_mcp_client *client) {
  return client != NULL ? (cai_mcp_streamable_http_client_impl *)client->impl
                        : NULL;
}

static const cai_mcp_streamable_http_client_impl *
cai_mcp_streamable_const_impl(const cai_mcp_client *client) {
  return client != NULL
             ? (const cai_mcp_streamable_http_client_impl *)client->impl
             : NULL;
}

static int cai_mcp_ascii_ieq_n(const char *a, const char *b, size_t n) {
  size_t i;

  if (a == NULL || b == NULL) {
    return 0;
  }
  for (i = 0U; i < n; i++) {
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
      return 0;
    }
  }
  return 1;
}

static int cai_mcp_header_is(const char *line, const char *name,
                             size_t name_len) {
  return strlen(line) > name_len && line[name_len] == ':' &&
         cai_mcp_ascii_ieq_n(line, name, name_len);
}

static char *cai_mcp_trim_header_value(const cai_allocator *allocator,
                                       const char *value, size_t len) {
  while (len > 0U && (*value == ' ' || *value == '\t')) {
    value++;
    len--;
  }
  while (len > 0U && (value[len - 1U] == '\r' || value[len - 1U] == '\n' ||
                      value[len - 1U] == ' ' || value[len - 1U] == '\t')) {
    len--;
  }
  return cai_strndup(allocator, value, len);
}

static int cai_mcp_session_id_is_valid(const char *session_id) {
  const unsigned char *p;

  if (session_id == NULL || session_id[0] == '\0') {
    return 0;
  }
  p = (const unsigned char *)session_id;
  while (*p != '\0') {
    if (*p < 0x21U || *p > 0x7eU) {
      return 0;
    }
    p++;
  }
  return 1;
}

static size_t cai_mcp_header_callback(char *buffer, size_t size, size_t nitems,
                                      void *userdata) {
  cai_mcp_http_response_capture *capture;
  size_t len;

  capture = (cai_mcp_http_response_capture *)userdata;
  len = size * nitems;
  if (len == 0U || capture == NULL) {
    return len;
  }
  if (cai_mcp_header_is(buffer, "content-type", strlen("content-type"))) {
    cai_free_mem(NULL, capture->content_type);
    capture->content_type =
        cai_mcp_trim_header_value(NULL, buffer + strlen("content-type") + 1U,
                                  len - strlen("content-type") - 1U);
  } else if (cai_mcp_header_is(buffer, "mcp-session-id",
                               strlen("mcp-session-id"))) {
    cai_free_mem(NULL, capture->session_id);
    capture->session_id =
        cai_mcp_trim_header_value(NULL, buffer + strlen("mcp-session-id") + 1U,
                                  len - strlen("mcp-session-id") - 1U);
  }
  return len;
}

static size_t cai_mcp_response_write(char *ptr, size_t size, size_t nmemb,
                                     void *userdata) {
  cai_mcp_http_response_capture *capture;
  lonejson_error json_error;
  size_t len;

  capture = (cai_mcp_http_response_capture *)userdata;
  len = size * nmemb;
  if (len == 0U) {
    return 0U;
  }
  lonejson_error_init(&json_error);
  if (capture == NULL ||
      capture->body.append(&capture->body, ptr, len, &json_error) !=
          LONEJSON_STATUS_OK) {
    return 0U;
  }
  return len;
}

static size_t cai_mcp_upload_read(char *ptr, size_t size, size_t nmemb,
                                  void *userdata) {
  cai_mcp_spooled_upload *upload;
  lonejson_error json_error;
  lonejson_read_result result;
  size_t capacity;

  upload = (cai_mcp_spooled_upload *)userdata;
  capacity = size * nmemb;
  if (upload == NULL || capacity == 0U) {
    return 0U;
  }
  if (!upload->rewound) {
    lonejson_error_init(&json_error);
    if (upload->cursor.rewind(&upload->cursor, &json_error) !=
        LONEJSON_STATUS_OK) {
      return CURL_READFUNC_ABORT;
    }
    upload->rewound = 1;
  }
  result = upload->cursor.read(&upload->cursor, (unsigned char *)ptr, capacity);
  if (result.error_code != 0) {
    return CURL_READFUNC_ABORT;
  }
  return result.bytes_read;
}

static lonejson_read_result
cai_mcp_spooled_read(void *user, unsigned char *buffer, size_t capacity) {
  cai_mcp_spooled_reader *reader;

  reader = (cai_mcp_spooled_reader *)user;
  return reader->cursor.read(&reader->cursor, buffer, capacity);
}

static lonejson_status cai_mcp_spool_sink(void *user, const void *data,
                                          size_t len,
                                          lonejson_error *json_error) {
  lonejson_spooled *spool;

  spool = (lonejson_spooled *)user;
  return spool->append(spool, data, len, json_error);
}

static int cai_mcp_spooled_sink_write(void *context, const void *bytes,
                                      size_t count, cai_error *error) {
  lonejson_spooled *spool;
  lonejson_error json_error;

  spool = (lonejson_spooled *)context;
  if (spool == NULL || bytes == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP sink write is required");
  }
  lonejson_error_init(&json_error);
  if (spool->append(spool, bytes, count, &json_error) != LONEJSON_STATUS_OK) {
    return cai_mcp_set_json_error(error, "failed to write MCP receiver result",
                                  &json_error);
  }
  return CAI_OK;
}

static lonejson_status cai_mcp_cai_sink_bridge(void *user, const void *data,
                                               size_t len,
                                               lonejson_error *json_error) {
  cai_sink *sink;
  cai_error error;

  sink = (cai_sink *)user;
  cai_error_init(&error);
  if (sink != NULL && cai_sink_write(sink, data, len, &error) == CAI_OK) {
    cai_error_cleanup(&error);
    return LONEJSON_STATUS_OK;
  }
  if (json_error != NULL) {
    snprintf(json_error->message, sizeof(json_error->message), "%s",
             error.message != NULL ? error.message : "sink write failed");
  }
  cai_error_cleanup(&error);
  return LONEJSON_STATUS_CALLBACK_FAILED;
}

static lonejson_status cai_mcp_discard_sink(void *user, const void *data,
                                            size_t len,
                                            lonejson_error *json_error) {
  (void)user;
  (void)data;
  (void)len;
  (void)json_error;
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_mcp_buffer_sink(void *user, const void *data,
                                           size_t len,
                                           lonejson_error *json_error) {
  cai_buffer_builder *builder;
  cai_error error;

  (void)json_error;
  builder = (cai_buffer_builder *)user;
  if (builder == NULL || data == NULL) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  cai_error_init(&error);
  if (cai_buffer_append(builder, (const char *)data, len, &error) != CAI_OK) {
    cai_error_cleanup(&error);
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  cai_error_cleanup(&error);
  return LONEJSON_STATUS_OK;
}

static int cai_mcp_set_json_error(cai_error *error, const char *message,
                                  const lonejson_error *json_error) {
  return cai_set_error_detail(error, CAI_ERR_PROTOCOL, message,
                              json_error != NULL ? json_error->message : NULL);
}

static int cai_mcp_set_rpc_error(cai_error *error,
                                 const cai_mcp_jsonrpc_error_doc *rpc_error) {
  char detail[80];

  if (rpc_error == NULL || !rpc_error->has_code || rpc_error->message == NULL) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP JSON-RPC error must include code and message");
  }
  snprintf(detail, sizeof(detail), "JSON-RPC error code %lld",
           (long long)rpc_error->code);
  return cai_set_error_detail(error, CAI_ERR_SERVER, rpc_error->message,
                              detail);
}

static int cai_mcp_response_has_jsonrpc_error(const lonejson_spooled *json_body,
                                              int *has_error,
                                              cai_error *error) {
  int has_result;

  return cai_mcp_jsonrpc_response_result_error_presence(json_body, &has_result,
                                                        has_error, error);
}

static int cai_mcp_content_type_is(const char *content_type,
                                   const char *media_type) {
  const char *end;
  size_t media_type_len;
  size_t len;

  if (content_type == NULL || media_type == NULL) {
    return 0;
  }
  while (*content_type == ' ' || *content_type == '\t') {
    content_type++;
  }
  end = content_type;
  while (*end != '\0' && *end != ';') {
    end++;
  }
  len = (size_t)(end - content_type);
  while (len > 0U &&
         (content_type[len - 1U] == ' ' || content_type[len - 1U] == '\t')) {
    len--;
  }
  media_type_len = strlen(media_type);
  return len == media_type_len &&
         cai_mcp_ascii_ieq_n(content_type, media_type, media_type_len);
}

static int cai_mcp_response_is_json(const cai_mcp_http_response_capture *res) {
  return res != NULL &&
         cai_mcp_content_type_is(res->content_type, "application/json");
}

static int cai_mcp_response_is_sse(const cai_mcp_http_response_capture *res) {
  return res != NULL &&
         cai_mcp_content_type_is(res->content_type, "text/event-stream");
}

static int cai_mcp_response_ok(const cai_mcp_http_response_capture *res,
                               cai_error *error) {
  if (res == NULL) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "MCP HTTP response is missing");
  }
  if (res->status < 200L || res->status >= 300L) {
    return cai_set_error_http(error, CAI_ERR_SERVER, res->status,
                              "MCP server returned HTTP error", NULL, NULL,
                              NULL);
  }
  return CAI_OK;
}

static int cai_mcp_validate_notification_response(
    const cai_mcp_http_response_capture *response, cai_error *error) {
  if (response == NULL) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "MCP HTTP response is missing");
  }
  if (response->status != 202L) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP notification response must be 202 Accepted");
  }
  if (response->body.size_fn(&response->body) != 0U) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP notification response body must be empty");
  }
  return CAI_OK;
}

static int cai_mcp_append_prefixed_header(struct curl_slist **headers,
                                          const char *prefix, const char *value,
                                          const char *error_message,
                                          cai_error *error) {
  size_t prefix_len;
  size_t value_len;
  char *header;
  int rc;

  if (value == NULL) {
    return CAI_OK;
  }
  prefix_len = strlen(prefix);
  value_len = strlen(value);
  if (value_len > SIZE_MAX - prefix_len - 1U) {
    return cai_set_error(error, CAI_ERR_NOMEM, error_message);
  }
  header = (char *)cai_alloc(NULL, prefix_len + value_len + 1U);
  if (header == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, error_message);
  }
  memcpy(header, prefix, prefix_len);
  memcpy(header + prefix_len, value, value_len);
  header[prefix_len + value_len] = '\0';
  rc = cai_append_header(headers, header, error);
  cai_free_mem(NULL, header);
  return rc;
}

static int
cai_mcp_append_session_headers(cai_mcp_streamable_http_client_impl *impl,
                               struct curl_slist **headers, cai_error *error) {
  int rc;

  if (impl == NULL || !impl->initialized) {
    return CAI_OK;
  }
  rc = cai_mcp_append_prefixed_header(
      headers, "MCP-Session-Id: ", impl->session_id,
      "failed to allocate MCP session header", error);
  if (rc == CAI_OK) {
    rc = cai_mcp_append_prefixed_header(
        headers, "MCP-Protocol-Version: ", impl->protocol_version,
        "failed to allocate MCP protocol header", error);
  }
  return rc;
}

static int cai_mcp_post(cai_mcp_streamable_http_client_impl *impl,
                        const lonejson_spooled *request, size_t request_len,
                        int is_request, cai_mcp_http_response_capture *response,
                        cai_error *error) {
  CURL *curl;
  CURLcode curl_rc;
  struct curl_slist *headers;
  cai_mcp_spooled_upload upload;
  int rc;

  if (impl == NULL || request == NULL || response == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client request is required");
  }
  headers = NULL;
  curl = curl_easy_init();
  if (curl == NULL) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to initialize MCP HTTP request");
  }
  rc = cai_append_header(&headers, "Content-Type: application/json", error);
  if (rc == CAI_OK) {
    rc = cai_append_header(
        &headers, "Accept: application/json, text/event-stream", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_append_session_headers(impl, &headers, error);
  }
  if (rc != CAI_OK) {
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    return rc;
  }

  response->content_type = NULL;
  response->session_id = NULL;
  response->status = 0L;
  CAI_LJ->spooled_init(CAI_LJ, &response->body);
  if (is_request) {
    impl->active_request_id = impl->next_id;
    impl->has_active_request = 1;
    impl->has_active_progress = 0;
    impl->active_progress = 0.0;
  }

  upload.cursor = *request;
  upload.rewound = 0;
  curl_easy_setopt(curl, CURLOPT_URL, impl->url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_READFUNCTION, cai_mcp_upload_read);
  curl_easy_setopt(curl, CURLOPT_READDATA, &upload);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)request_len);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cai_mcp_response_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, cai_mcp_header_callback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, response);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, impl->timeout_ms);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, impl->timeout_ms);
  cai_configure_curl_tls(curl, impl->insecure_skip_verify, impl->ca_bundle_path,
                         impl->ca_path);
  curl_rc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response->status);
  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);
  if (curl_rc != CURLE_OK) {
    if (is_request) {
      impl->has_active_request = 0;
      impl->has_active_progress = 0;
    }
    response->body.cleanup(&response->body);
    memset(&response->body, 0, sizeof(response->body));
    cai_free_mem(NULL, response->content_type);
    response->content_type = NULL;
    cai_free_mem(NULL, response->session_id);
    response->session_id = NULL;
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "MCP HTTP request failed",
                                curl_easy_strerror(curl_rc));
  }
  rc = cai_mcp_response_ok(response, error);
  if (rc == CAI_OK && !is_request) {
    rc = cai_mcp_validate_notification_response(response, error);
  }
  if (rc == CAI_OK && is_request && cai_mcp_response_is_sse(response)) {
    rc = cai_mcp_sse_normalize_response(impl, response, 1, error);
  }
  if (is_request) {
    impl->has_active_request = 0;
    impl->has_active_progress = 0;
  }
  return rc;
}

static int cai_mcp_append_last_event_id_header(struct curl_slist **headers,
                                               const char *last_event_id,
                                               cai_error *error) {
  cai_buffer_builder builder;
  int rc;

  if (last_event_id == NULL || last_event_id[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP SSE resume event id is required");
  }
  memset(&builder, 0, sizeof(builder));
  rc = cai_buffer_append_cstr(&builder, "Last-Event-ID: ", error);
  if (rc == CAI_OK) {
    rc = cai_buffer_append_cstr(&builder, last_event_id, error);
  }
  if (rc == CAI_OK) {
    rc = cai_buffer_append(&builder, "", 1U, error);
  }
  if (rc == CAI_OK) {
    rc = cai_append_header(headers, builder.data, error);
  }
  cai_free_mem(NULL, builder.data);
  return rc;
}

static int cai_mcp_get_resume_response(
    cai_mcp_streamable_http_client_impl *impl, const char *last_event_id,
    cai_mcp_http_response_capture *response, cai_error *error) {
  CURL *curl;
  CURLcode curl_rc;
  struct curl_slist *headers;
  int rc;

  if (impl == NULL || response == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP client is required");
  }
  headers = NULL;
  curl = curl_easy_init();
  if (curl == NULL) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to initialize MCP HTTP request");
  }
  rc = cai_append_header(&headers, "Accept: text/event-stream", error);
  if (rc == CAI_OK) {
    rc = cai_mcp_append_session_headers(impl, &headers, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_append_last_event_id_header(&headers, last_event_id, error);
  }
  if (rc != CAI_OK) {
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    return rc;
  }

  response->content_type = NULL;
  response->session_id = NULL;
  response->status = 0L;
  CAI_LJ->spooled_init(CAI_LJ, &response->body);
  curl_easy_setopt(curl, CURLOPT_URL, impl->url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cai_mcp_response_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, cai_mcp_header_callback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, response);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, impl->timeout_ms);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, impl->timeout_ms);
  cai_configure_curl_tls(curl, impl->insecure_skip_verify, impl->ca_bundle_path,
                         impl->ca_path);
  curl_rc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response->status);
  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);
  if (curl_rc != CURLE_OK) {
    response->body.cleanup(&response->body);
    memset(&response->body, 0, sizeof(response->body));
    cai_free_mem(NULL, response->content_type);
    response->content_type = NULL;
    cai_free_mem(NULL, response->session_id);
    response->session_id = NULL;
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "MCP HTTP event stream resume failed",
                                curl_easy_strerror(curl_rc));
  }
  rc = cai_mcp_response_ok(response, error);
  if (rc == CAI_OK && !cai_mcp_response_is_sse(response)) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "MCP resumed event stream response was not SSE");
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_sse_normalize_response(impl, response, 0, error);
  }
  return rc;
}

static int cai_mcp_delete_session(cai_mcp_streamable_http_client_impl *impl,
                                  cai_mcp_http_response_capture *response,
                                  cai_error *error) {
  CURL *curl;
  CURLcode curl_rc;
  struct curl_slist *headers;
  int rc;

  if (impl == NULL || response == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP client is required");
  }
  if (!impl->initialized || impl->session_id == NULL) {
    return CAI_OK;
  }
  headers = NULL;
  curl = curl_easy_init();
  if (curl == NULL) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to initialize MCP HTTP request");
  }
  rc = cai_append_header(&headers, "Accept: application/json", error);
  if (rc == CAI_OK) {
    rc = cai_mcp_append_session_headers(impl, &headers, error);
  }
  if (rc != CAI_OK) {
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    return rc;
  }

  response->content_type = NULL;
  response->session_id = NULL;
  response->status = 0L;
  CAI_LJ->spooled_init(CAI_LJ, &response->body);

  curl_easy_setopt(curl, CURLOPT_URL, impl->url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cai_mcp_response_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, cai_mcp_header_callback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, response);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, impl->timeout_ms);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, impl->timeout_ms);
  cai_configure_curl_tls(curl, impl->insecure_skip_verify, impl->ca_bundle_path,
                         impl->ca_path);
  curl_rc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response->status);
  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);
  if (curl_rc != CURLE_OK) {
    response->body.cleanup(&response->body);
    memset(&response->body, 0, sizeof(response->body));
    cai_free_mem(NULL, response->content_type);
    response->content_type = NULL;
    cai_free_mem(NULL, response->session_id);
    response->session_id = NULL;
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "MCP HTTP request failed",
                                curl_easy_strerror(curl_rc));
  }
  if ((response->status >= 200L && response->status < 300L) ||
      response->status == 404L || response->status == 405L) {
    cai_mcp_streamable_reset_session(impl);
    return CAI_OK;
  }
  return cai_set_error_http(error, CAI_ERR_SERVER, response->status,
                            "MCP server returned HTTP error", NULL, NULL, NULL);
}

static void
cai_mcp_http_response_capture_cleanup(cai_mcp_http_response_capture *response) {
  if (response != NULL) {
    if (response->body.cleanup != NULL) {
      response->body.cleanup(&response->body);
    }
    cai_free_mem(NULL, response->content_type);
    cai_free_mem(NULL, response->session_id);
    memset(response, 0, sizeof(*response));
  }
}

static void
cai_mcp_streamable_reset_session(cai_mcp_streamable_http_client_impl *impl) {
  if (impl != NULL) {
    cai_free_mem(&impl->allocator, impl->session_id);
    impl->session_id = NULL;
    impl->initialized = 0;
  }
}

static void cai_mcp_clear_error(cai_error *error) {
  if (error != NULL) {
    cai_error_cleanup(error);
    cai_error_init(error);
  }
}

static void cai_mcp_spooled_cleanup_if_initialized(lonejson_spooled *spool) {
  if (spool != NULL && spool->cleanup != NULL) {
    spool->cleanup(spool);
    memset(spool, 0, sizeof(*spool));
  }
}

static int cai_mcp_write_cstr(lonejson_spooled *spool, const char *text,
                              cai_error *error) {
  lonejson_error json_error;

  lonejson_error_init(&json_error);
  if (spool->append(spool, text, strlen(text), &json_error) !=
      LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to build MCP request",
                                json_error.message);
  }
  return CAI_OK;
}

static int cai_mcp_spool_copy(const lonejson_spooled *src,
                              lonejson_spooled *dst, cai_error *error) {
  lonejson_spooled cursor;
  lonejson_error json_error;
  unsigned char buffer[4096];
  lonejson_read_result chunk;

  CAI_LJ->spooled_init(CAI_LJ, dst);
  cursor = *src;
  lonejson_error_init(&json_error);
  if (cursor.rewind(&cursor, &json_error) != LONEJSON_STATUS_OK) {
    dst->cleanup(dst);
    return cai_mcp_set_json_error(error, "failed to rewind MCP response",
                                  &json_error);
  }
  for (;;) {
    chunk = cursor.read(&cursor, buffer, sizeof(buffer));
    if (chunk.error_code != 0) {
      dst->cleanup(dst);
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "failed to read MCP response body");
    }
    if (chunk.bytes_read != 0U &&
        dst->append(dst, buffer, chunk.bytes_read, &json_error) !=
            LONEJSON_STATUS_OK) {
      dst->cleanup(dst);
      return cai_mcp_set_json_error(error, "failed to copy MCP response",
                                    &json_error);
    }
    if (chunk.eof) {
      break;
    }
  }
  return CAI_OK;
}

static int cai_mcp_json_value_to_spooled(const lonejson_json_value *value,
                                         lonejson_spooled *out,
                                         cai_error *error) {
  lonejson_error json_error;
  lonejson_status status;

  if (value == NULL || value->methods == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP JSON value is required");
  }
  CAI_LJ->spooled_init(CAI_LJ, out);
  lonejson_error_init(&json_error);
  status = value->methods->write_to_sink(value, cai_mcp_spool_sink, out,
                                         &json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = out->rewind(out, &json_error);
  }
  if (status != LONEJSON_STATUS_OK) {
    out->cleanup(out);
    memset(out, 0, sizeof(*out));
    return cai_mcp_set_json_error(error, "failed to copy MCP JSON value",
                                  &json_error);
  }
  return CAI_OK;
}

static void
cai_mcp_invalidate_for_notification(cai_mcp_streamable_http_client_impl *impl,
                                    const char *method) {
  if (impl == NULL || method == NULL) {
    return;
  }
  if (strcmp(method, "notifications/tools/list_changed") == 0) {
    cai_mcp_client_clear_tools(impl);
  } else if (strcmp(method, "notifications/resources/list_changed") == 0) {
    cai_mcp_client_clear_resources(impl);
    cai_mcp_client_clear_resource_templates(impl);
  } else if (strcmp(method, "notifications/prompts/list_changed") == 0) {
    cai_mcp_client_clear_prompts(impl);
  }
}

static int
cai_mcp_json_value_id_shape_is_valid(const lonejson_json_value *value) {
  cai_error ignored;
  int rc;

  cai_error_init(&ignored);
  rc = cai_mcp_validate_jsonrpc_id_value(value, &ignored);
  cai_error_cleanup(&ignored);
  return rc == CAI_OK;
}

static int cai_mcp_cancelled_request_id_text(const lonejson_json_value *params,
                                             char **out) {
  cai_mcp_cancelled_params_doc doc;
  lonejson_spooled spool;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  char *request_id;

  if (out != NULL) {
    *out = NULL;
  }
  if (params == NULL || params->kind == LONEJSON_JSON_VALUE_NULL ||
      out == NULL || !cai_mcp_json_value_root_is(params, '{', NULL)) {
    return CAI_ERR_PROTOCOL;
  }
  memset(&doc, 0, sizeof(doc));
  memset(&spool, 0, sizeof(spool));
  if (cai_mcp_json_value_to_spooled(params, &spool, NULL) != CAI_OK) {
    return CAI_ERR_TRANSPORT;
  }
  reader.cursor = spool;
  lonejson_error_init(&json_error);
  status = CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_cancelled_params_map, &doc,
                                cai_mcp_spooled_read, &reader, &json_error);
  request_id = NULL;
  if (status == LONEJSON_STATUS_OK &&
      cai_mcp_json_value_id_shape_is_valid(&doc.request_id)) {
    request_id = cai_mcp_json_value_to_cstr(&doc.request_id, NULL);
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_cancelled_params_map, &doc);
  spool.cleanup(&spool);
  if (request_id == NULL) {
    return CAI_ERR_PROTOCOL;
  }
  *out = request_id;
  return CAI_OK;
}

static int cai_mcp_dispatch_cancelled_notification(
    cai_mcp_streamable_http_client_impl *impl,
    const lonejson_json_value *params, cai_error *error) {
  char *request_id;
  char active_request_id[64];
  int matches;

  request_id = NULL;
  if (cai_mcp_cancelled_request_id_text(params, &request_id) != CAI_OK) {
    return CAI_OK;
  }
  snprintf(active_request_id, sizeof(active_request_id), "%lld",
           impl != NULL ? impl->active_request_id : 0LL);
  matches = impl != NULL && impl->has_active_request &&
            strcmp(request_id, active_request_id) == 0;
  cai_free_mem(NULL, request_id);
  if (!matches) {
    return CAI_OK;
  }
  return cai_set_error(error, CAI_ERR_CANCELLED, "MCP request was cancelled");
}

static int cai_mcp_progress_token_text(const lonejson_json_value *params,
                                       char **out) {
  cai_mcp_progress_token_doc doc;
  lonejson_spooled spool;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  char *progress_token;

  if (out != NULL) {
    *out = NULL;
  }
  if (params == NULL || params->kind == LONEJSON_JSON_VALUE_NULL ||
      out == NULL || !cai_mcp_json_value_root_is(params, '{', NULL)) {
    return CAI_ERR_PROTOCOL;
  }
  memset(&doc, 0, sizeof(doc));
  memset(&spool, 0, sizeof(spool));
  if (cai_mcp_json_value_to_spooled(params, &spool, NULL) != CAI_OK) {
    return CAI_ERR_TRANSPORT;
  }
  reader.cursor = spool;
  lonejson_error_init(&json_error);
  status = CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_progress_token_map, &doc,
                                cai_mcp_spooled_read, &reader, &json_error);
  progress_token = NULL;
  if (status == LONEJSON_STATUS_OK &&
      cai_mcp_json_value_id_shape_is_valid(&doc.progress_token)) {
    progress_token = cai_mcp_json_value_to_cstr(&doc.progress_token, NULL);
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_progress_token_map, &doc);
  spool.cleanup(&spool);
  if (progress_token == NULL) {
    return CAI_ERR_PROTOCOL;
  }
  *out = progress_token;
  return CAI_OK;
}

static int cai_mcp_progress_matches_active_request(
    const cai_mcp_streamable_http_client_impl *impl,
    const lonejson_json_value *params) {
  char *progress_token;
  char active_request_id[64];
  int matches;

  progress_token = NULL;
  if (cai_mcp_progress_token_text(params, &progress_token) != CAI_OK) {
    return 0;
  }
  snprintf(active_request_id, sizeof(active_request_id), "%lld",
           impl != NULL ? impl->active_request_id : 0LL);
  matches = impl != NULL && impl->has_active_request &&
            strcmp(progress_token, active_request_id) == 0;
  cai_free_mem(NULL, progress_token);
  return matches;
}

static int
cai_mcp_validate_progress_notification_params(
    cai_mcp_streamable_http_client_impl *impl,
    const lonejson_json_value *params, cai_error *error) {
  cai_mcp_progress_params_doc doc;
  lonejson_spooled spool;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  int rc;

  if (params == NULL || params->kind == LONEJSON_JSON_VALUE_NULL ||
      !cai_mcp_json_value_root_is(params, '{', NULL)) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP progress params must be an object");
  }
  memset(&doc, 0, sizeof(doc));
  memset(&spool, 0, sizeof(spool));
  rc = cai_mcp_json_value_to_spooled(params, &spool, error);
  if (rc != CAI_OK) {
    return rc;
  }
  reader.cursor = spool;
  lonejson_error_init(&json_error);
  status = CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_progress_params_map, &doc,
                                cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_progress_params_map, &doc);
    spool.cleanup(&spool);
    return cai_mcp_set_json_error(
        error, "failed to parse MCP progress notification params", &json_error);
  }
  rc = CAI_OK;
  if (!doc.has_progress) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "MCP progress notification requires progress");
  }
  if (rc == CAI_OK && impl != NULL && impl->has_active_progress &&
      doc.progress <= impl->active_progress) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "MCP progress value must increase");
  }
  if (rc == CAI_OK && impl != NULL) {
    impl->has_active_progress = 1;
    impl->active_progress = doc.progress;
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_progress_params_map, &doc);
  spool.cleanup(&spool);
  return rc;
}

static int
cai_mcp_validate_logging_message_params(const lonejson_json_value *params,
                                        cai_error *error) {
  cai_mcp_logging_message_params_doc doc;
  lonejson_spooled spool;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  int rc;

  if (params == NULL || params->kind == LONEJSON_JSON_VALUE_NULL ||
      !cai_mcp_json_value_root_is(params, '{', NULL)) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP logging notification params must be an object");
  }
  memset(&doc, 0, sizeof(doc));
  memset(&spool, 0, sizeof(spool));
  rc = cai_mcp_json_value_to_spooled(params, &spool, error);
  if (rc != CAI_OK) {
    return rc;
  }
  reader.cursor = spool;
  lonejson_error_init(&json_error);
  status =
      CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_logging_message_params_map, &doc,
                           cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_logging_message_params_map, &doc);
    spool.cleanup(&spool);
    return cai_mcp_set_json_error(
        error, "failed to parse MCP logging notification params", &json_error);
  }
  rc = CAI_OK;
  if (!cai_mcp_log_level_valid(doc.level)) {
    rc = cai_set_error(
        error, CAI_ERR_PROTOCOL,
        "MCP logging notification level must be a standard syslog severity");
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_logging_message_params_map, &doc);
  spool.cleanup(&spool);
  return rc;
}

static int
cai_mcp_validate_resource_updated_params(const lonejson_json_value *params,
                                         cai_error *error) {
  cai_mcp_resource_updated_params_doc doc;
  lonejson_spooled spool;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  int rc;

  if (params == NULL || params->kind == LONEJSON_JSON_VALUE_NULL ||
      !cai_mcp_json_value_root_is(params, '{', NULL)) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP resource updated params must be an object");
  }
  memset(&doc, 0, sizeof(doc));
  memset(&spool, 0, sizeof(spool));
  rc = cai_mcp_json_value_to_spooled(params, &spool, error);
  if (rc != CAI_OK) {
    return rc;
  }
  reader.cursor = spool;
  lonejson_error_init(&json_error);
  status =
      CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_resource_updated_params_map, &doc,
                           cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_resource_updated_params_map, &doc);
    spool.cleanup(&spool);
    return cai_mcp_set_json_error(
        error, "failed to parse MCP resource updated params", &json_error);
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_resource_updated_params_map, &doc);
  spool.cleanup(&spool);
  return CAI_OK;
}

static int
cai_mcp_validate_elicitation_complete_params(const lonejson_json_value *params,
                                             cai_error *error) {
  cai_mcp_elicitation_complete_params_doc doc;
  lonejson_spooled spool;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  int rc;

  if (params == NULL || params->kind == LONEJSON_JSON_VALUE_NULL ||
      !cai_mcp_json_value_root_is(params, '{', NULL)) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP elicitation complete params must be an object");
  }
  memset(&doc, 0, sizeof(doc));
  memset(&spool, 0, sizeof(spool));
  rc = cai_mcp_json_value_to_spooled(params, &spool, error);
  if (rc != CAI_OK) {
    return rc;
  }
  reader.cursor = spool;
  lonejson_error_init(&json_error);
  status =
      CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_elicitation_complete_params_map,
                           &doc, cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_elicitation_complete_params_map, &doc);
    spool.cleanup(&spool);
    return cai_mcp_set_json_error(
        error, "failed to parse MCP elicitation complete params", &json_error);
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_elicitation_complete_params_map, &doc);
  spool.cleanup(&spool);
  return CAI_OK;
}

static int cai_mcp_validate_task_status_notification_params(
    const lonejson_json_value *params, cai_error *error) {
  cai_mcp_task_doc doc;
  lonejson_spooled spool;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  int rc;

  if (params == NULL || params->kind == LONEJSON_JSON_VALUE_NULL ||
      !cai_mcp_json_value_root_is(params, '{', NULL)) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP task status params must be an object");
  }
  memset(&doc, 0, sizeof(doc));
  memset(&spool, 0, sizeof(spool));
  rc = cai_mcp_json_value_to_spooled(params, &spool, error);
  if (rc != CAI_OK) {
    return rc;
  }
  reader.cursor = spool;
  lonejson_error_init(&json_error);
  status = CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_task_map, &doc,
                                cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_task_map, &doc);
    spool.cleanup(&spool);
    return cai_mcp_set_json_error(
        error, "failed to parse MCP task status params", &json_error);
  }
  rc = cai_mcp_task_validate(&doc, error);
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_task_map, &doc);
  spool.cleanup(&spool);
  return rc;
}

static int
cai_mcp_validate_optional_notification_params(const lonejson_json_value *params,
                                              const char *message,
                                              cai_error *error) {
  if (params == NULL || params->kind == LONEJSON_JSON_VALUE_NULL) {
    return CAI_OK;
  }
  if (!cai_mcp_json_value_root_is(params, '{', NULL)) {
    return cai_set_error(error, CAI_ERR_PROTOCOL, message);
  }
  return CAI_OK;
}

static int
cai_mcp_validate_optional_request_params(const lonejson_json_value *params,
                                         const char *message,
                                         cai_error *error) {
  if (params == NULL || params->kind == LONEJSON_JSON_VALUE_NULL) {
    return CAI_OK;
  }
  if (!cai_mcp_json_value_root_is(params, '{', NULL)) {
    return cai_set_error(error, CAI_ERR_PROTOCOL, message);
  }
  return CAI_OK;
}

static int cai_mcp_notification_should_dispatch(
    const cai_mcp_streamable_http_client_impl *impl,
    const cai_mcp_jsonrpc_message_doc *doc) {
  if (doc == NULL || doc->method == NULL) {
    return 0;
  }
  if (strcmp(doc->method, "notifications/progress") == 0) {
    return cai_mcp_progress_matches_active_request(impl, &doc->params);
  }
  return 1;
}

static int
cai_mcp_dispatch_notification(cai_mcp_streamable_http_client_impl *impl,
                              const cai_mcp_jsonrpc_message_doc *doc,
                              cai_error *error) {
  cai_mcp_client_notification_fn notification;
  void *context;
  lonejson_spooled params;
  lonejson_spooled *params_ptr;
  int rc;

  if (impl == NULL || doc == NULL || doc->method == NULL) {
    return CAI_OK;
  }
  if (strcmp(doc->method, "notifications/cancelled") == 0) {
    return cai_mcp_dispatch_cancelled_notification(impl, &doc->params, error);
  }
  if (strcmp(doc->method, "notifications/message") == 0) {
    rc = cai_mcp_validate_logging_message_params(&doc->params, error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  if (strcmp(doc->method, "notifications/resources/updated") == 0) {
    rc = cai_mcp_validate_resource_updated_params(&doc->params, error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  if (strcmp(doc->method, "notifications/elicitation/complete") == 0) {
    rc = cai_mcp_validate_elicitation_complete_params(&doc->params, error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  if (strcmp(doc->method, "notifications/tasks/status") == 0) {
    rc = cai_mcp_validate_task_status_notification_params(&doc->params, error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  if (strcmp(doc->method, "notifications/tools/list_changed") == 0 ||
      strcmp(doc->method, "notifications/resources/list_changed") == 0 ||
      strcmp(doc->method, "notifications/prompts/list_changed") == 0) {
    rc = cai_mcp_validate_optional_notification_params(
        &doc->params, "MCP list changed params must be an object", error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  if (strcmp(doc->method, "notifications/progress") == 0) {
    if (!cai_mcp_notification_should_dispatch(impl, doc)) {
      return CAI_OK;
    }
    rc = cai_mcp_validate_progress_notification_params(impl, &doc->params,
                                                       error);
    if (rc != CAI_OK) {
      return rc;
    }
  } else {
    if (!cai_mcp_notification_should_dispatch(impl, doc)) {
      return CAI_OK;
    }
  }
  cai_mcp_invalidate_for_notification(impl, doc->method);
  notification = impl->receiver.notification != NULL
                     ? impl->receiver.notification
                     : impl->notification;
  context = impl->receiver.notification != NULL ? impl->receiver.context
                                                : impl->notification_context;
  if (notification == NULL) {
    return CAI_OK;
  }
  params_ptr = NULL;
  memset(&params, 0, sizeof(params));
  if (doc->params.kind != LONEJSON_JSON_VALUE_NULL) {
    rc = cai_mcp_json_value_to_spooled(&doc->params, &params, error);
    if (rc != CAI_OK) {
      return rc;
    }
    params_ptr = &params;
  }
  rc = notification(context, doc->method, params_ptr, error);
  cai_mcp_spooled_cleanup_if_initialized(&params);
  return rc;
}

static int cai_mcp_write_json_value(lonejson_spooled *spool,
                                    const lonejson_json_value *value,
                                    const char *operation, cai_error *error) {
  lonejson_error json_error;

  if (spool == NULL || value == NULL || value->methods == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP JSON value is required");
  }
  lonejson_error_init(&json_error);
  if (value->methods->write_to_sink(value, cai_mcp_spool_sink, spool,
                                    &json_error) != LONEJSON_STATUS_OK) {
    return cai_mcp_set_json_error(error, operation, &json_error);
  }
  return CAI_OK;
}

static int cai_mcp_build_roots_result(cai_mcp_streamable_http_client_impl *impl,
                                      lonejson_spooled *result,
                                      int *jsonrpc_error_code,
                                      int *preserve_error, cai_error *error) {
  cai_sink_callbacks callbacks;
  cai_sink *sink;
  int rc;

  if (jsonrpc_error_code != NULL) {
    *jsonrpc_error_code = -32603;
  }
  if (preserve_error != NULL) {
    *preserve_error = 1;
  }
  if (impl == NULL || impl->receiver.list_roots == NULL) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP roots/list receiver is not configured");
  }
  CAI_LJ->spooled_init(CAI_LJ, result);
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = cai_mcp_spooled_sink_write;
  callbacks.context = result;
  rc = cai_sink_from_callbacks(&callbacks, &sink, error);
  if (rc == CAI_OK) {
    rc = impl->receiver.list_roots(impl->receiver.context, sink, error);
    cai_sink_close(sink);
  }
  if (rc != CAI_OK) {
    result->cleanup(result);
    memset(result, 0, sizeof(*result));
  }
  return rc;
}

static int cai_mcp_sampling_context_is_remote(const char *include_context) {
  return include_context != NULL &&
         (strcmp(include_context, "thisServer") == 0 ||
          strcmp(include_context, "allServers") == 0);
}

static int cai_mcp_sampling_context_is_valid(const char *include_context) {
  return include_context == NULL || strcmp(include_context, "none") == 0 ||
         strcmp(include_context, "thisServer") == 0 ||
         strcmp(include_context, "allServers") == 0;
}

static int cai_mcp_validate_sampling_params(
    const cai_mcp_streamable_http_client_impl *impl,
    lonejson_spooled *params_json, int *uses_tools, cai_error *error) {
  cai_mcp_sampling_params_doc doc;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  int rc;

  if (uses_tools == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP sampling tool output pointer is required");
  }
  *uses_tools = 0;
  if (impl == NULL || params_json == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP sampling params are required");
  }
  memset(&doc, 0, sizeof(doc));
  reader.cursor = *params_json;
  lonejson_error_init(&json_error);
  if (reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    return cai_mcp_set_json_error(error, "failed to rewind MCP sampling params",
                                  &json_error);
  }
  status = CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_sampling_params_map, &doc,
                                cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_sampling_params_map, &doc);
    return cai_mcp_set_json_error(error, "failed to parse MCP sampling params",
                                  &json_error);
  }
  *uses_tools = doc.tools.kind != LONEJSON_JSON_VALUE_NULL ||
                doc.tool_choice.kind != LONEJSON_JSON_VALUE_NULL;
  rc = CAI_OK;
  if (!cai_mcp_sampling_context_is_valid(doc.include_context)) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "MCP sampling includeContext is invalid");
  } else if (cai_mcp_sampling_context_is_remote(doc.include_context) &&
      !impl->receiver.sampling_context) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "MCP sampling context was not advertised");
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_sampling_params_map, &doc);
  return rc;
}

static int cai_mcp_elicitation_form_is_supported(
    const cai_mcp_streamable_http_client_impl *impl) {
  return impl != NULL && impl->receiver.elicit != NULL &&
         (!impl->receiver.elicitation_url || impl->receiver.elicitation_form);
}

static int cai_mcp_validate_elicitation_requested_schema(
    const lonejson_json_value *schema, cai_error *error) {
  cai_mcp_elicitation_schema_doc doc;
  cai_mcp_spooled_reader reader;
  lonejson_spooled schema_json;
  lonejson_error json_error;
  lonejson_status status;
  int rc;

  if (schema == NULL || schema->kind == LONEJSON_JSON_VALUE_NULL) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP elicitation form requestedSchema is required");
  }
  if (!cai_mcp_json_value_root_is(schema, '{', error)) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP elicitation requestedSchema must be an object");
  }
  memset(&schema_json, 0, sizeof(schema_json));
  rc = cai_mcp_json_value_to_spooled(schema, &schema_json, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&doc, 0, sizeof(doc));
  reader.cursor = schema_json;
  lonejson_error_init(&json_error);
  status = CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_elicitation_schema_map, &doc,
                                cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_elicitation_schema_map, &doc);
    schema_json.cleanup(&schema_json);
    return cai_mcp_set_json_error(
        error, "failed to parse MCP elicitation requestedSchema", &json_error);
  }
  if (strcmp(doc.type, "object") != 0) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "MCP elicitation requestedSchema type must be object");
  } else if (doc.properties.kind == LONEJSON_JSON_VALUE_NULL) {
    rc = cai_set_error(
        error, CAI_ERR_PROTOCOL,
        "MCP elicitation requestedSchema properties are required");
  } else if (!cai_mcp_json_value_root_is(&doc.properties, '{', error)) {
    rc = cai_set_error(
        error, CAI_ERR_PROTOCOL,
        "MCP elicitation requestedSchema properties must be an object");
  } else {
    rc = cai_mcp_validate_elicitation_schema_properties(&doc.properties, error);
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_elicitation_schema_map, &doc);
  schema_json.cleanup(&schema_json);
  return rc;
}

static int cai_mcp_validate_elicitation_params(
    const cai_mcp_streamable_http_client_impl *impl,
    lonejson_spooled *params_json, cai_error *error) {
  cai_mcp_elicitation_params_doc doc;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  const char *mode;
  int rc;

  if (impl == NULL || params_json == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP elicitation params are required");
  }
  memset(&doc, 0, sizeof(doc));
  reader.cursor = *params_json;
  lonejson_error_init(&json_error);
  if (reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    return cai_mcp_set_json_error(
        error, "failed to rewind MCP elicitation params", &json_error);
  }
  status = CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_elicitation_params_map, &doc,
                                cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_elicitation_params_map, &doc);
    return cai_mcp_set_json_error(
        error, "failed to parse MCP elicitation params", &json_error);
  }

  mode = doc.mode != NULL ? doc.mode : "form";
  rc = CAI_OK;
  if (strcmp(mode, "form") == 0) {
    if (!cai_mcp_elicitation_form_is_supported(impl)) {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP elicitation form was not advertised");
    } else {
      rc = cai_mcp_validate_elicitation_requested_schema(
          &doc.requested_schema, error);
    }
  } else if (strcmp(mode, "url") == 0) {
    if (!impl->receiver.elicitation_url) {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP elicitation url was not advertised");
    } else if (doc.url == NULL || doc.elicitation_id == NULL) {
      rc = cai_set_error(
          error, CAI_ERR_PROTOCOL,
          "MCP elicitation url and elicitationId are required for url mode");
    }
  } else {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "MCP elicitation mode must be form or url");
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_elicitation_params_map, &doc);
  return rc;
}

static int
cai_mcp_build_sampling_result(cai_mcp_streamable_http_client_impl *impl,
                              const cai_mcp_jsonrpc_message_doc *doc,
                              lonejson_spooled *result, int *jsonrpc_error_code,
                              int *preserve_error, cai_error *error) {
  cai_sink_callbacks callbacks;
  lonejson_spooled params_json;
  cai_sink *sink;
  int uses_tools;
  int rc;

  if (jsonrpc_error_code != NULL) {
    *jsonrpc_error_code = -32603;
  }
  if (preserve_error != NULL) {
    *preserve_error = 1;
  }
  if (impl == NULL || impl->receiver.create_message == NULL) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP sampling/createMessage receiver is not "
                         "configured");
  }
  memset(&params_json, 0, sizeof(params_json));
  rc = cai_mcp_json_value_to_spooled(&doc->params, &params_json, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_validate_sampling_params(impl, &params_json, &uses_tools,
                                          error);
    if (rc != CAI_OK) {
      if (jsonrpc_error_code != NULL) {
        *jsonrpc_error_code = -32602;
      }
      if (preserve_error != NULL) {
        *preserve_error = 0;
      }
    }
  }
  if (rc == CAI_OK && uses_tools && !impl->receiver.sampling_tools) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "MCP sampling tools were not advertised");
    if (jsonrpc_error_code != NULL) {
      *jsonrpc_error_code = -32602;
    }
    if (preserve_error != NULL) {
      *preserve_error = 0;
    }
  }
  if (rc == CAI_OK) {
    CAI_LJ->spooled_init(CAI_LJ, result);
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.write = cai_mcp_spooled_sink_write;
    callbacks.context = result;
    rc = cai_sink_from_callbacks(&callbacks, &sink, error);
    if (rc == CAI_OK) {
      rc = impl->receiver.create_message(impl->receiver.context, &params_json,
                                         sink, error);
      cai_sink_close(sink);
    }
    if (rc != CAI_OK) {
      result->cleanup(result);
      memset(result, 0, sizeof(*result));
    }
  }
  cai_mcp_spooled_cleanup_if_initialized(&params_json);
  return rc;
}

static int cai_mcp_build_elicitation_result(
    cai_mcp_streamable_http_client_impl *impl,
    const cai_mcp_jsonrpc_message_doc *doc, lonejson_spooled *result,
    int *jsonrpc_error_code, int *preserve_error, cai_error *error) {
  cai_sink_callbacks callbacks;
  lonejson_spooled params_json;
  cai_sink *sink;
  int rc;

  if (jsonrpc_error_code != NULL) {
    *jsonrpc_error_code = -32603;
  }
  if (preserve_error != NULL) {
    *preserve_error = 1;
  }
  if (impl == NULL || impl->receiver.elicit == NULL) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP elicitation/create receiver is not configured");
  }
  memset(&params_json, 0, sizeof(params_json));
  rc = cai_mcp_json_value_to_spooled(&doc->params, &params_json, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_validate_elicitation_params(impl, &params_json, error);
    if (rc != CAI_OK) {
      if (jsonrpc_error_code != NULL) {
        *jsonrpc_error_code = -32602;
      }
      if (preserve_error != NULL) {
        *preserve_error = 0;
      }
    }
  }
  if (rc == CAI_OK) {
    CAI_LJ->spooled_init(CAI_LJ, result);
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.write = cai_mcp_spooled_sink_write;
    callbacks.context = result;
    rc = cai_sink_from_callbacks(&callbacks, &sink, error);
    if (rc == CAI_OK) {
      rc = impl->receiver.elicit(impl->receiver.context, &params_json, sink,
                                 error);
      cai_sink_close(sink);
    }
    if (rc != CAI_OK) {
      result->cleanup(result);
      memset(result, 0, sizeof(*result));
    }
  }
  cai_mcp_spooled_cleanup_if_initialized(&params_json);
  return rc;
}

static int cai_mcp_write_jsonrpc_error(lonejson_spooled *response,
                                       const lonejson_json_value *id, int code,
                                       const char *message, cai_error *error) {
  int rc;

  rc = cai_mcp_write_cstr(response, "{\"jsonrpc\":\"2.0\",\"id\":", error);
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_value(
        response, id, "failed to write MCP server request id", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(response, ",\"error\":{\"code\":", error);
  }
  if (rc == CAI_OK) {
    char code_json[32];

    snprintf(code_json, sizeof(code_json), "%d", code);
    rc = cai_mcp_write_cstr(response, code_json, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(response, ",\"message\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(response, message, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(response, "}}", error);
  }
  return rc;
}

static int
cai_mcp_server_request_response(cai_mcp_streamable_http_client_impl *impl,
                                const cai_mcp_jsonrpc_message_doc *doc,
                                lonejson_spooled *response, size_t *out_len,
                                cai_error *error) {
  lonejson_spooled result;
  lonejson_error json_error;
  lonejson_writer writer;
  lonejson_status status;
  const char *result_error_message;
  int jsonrpc_error_code;
  int preserve_error;
  int rc;

  memset(&result, 0, sizeof(result));
  jsonrpc_error_code = -32603;
  preserve_error = 1;
  if (doc == NULL || doc->method == NULL ||
      doc->id.kind == LONEJSON_JSON_VALUE_NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP server request is required");
  }
  CAI_LJ->spooled_init(CAI_LJ, response);
  if (strcmp(doc->method, "ping") == 0) {
    rc = cai_mcp_validate_optional_request_params(
        &doc->params, "MCP ping params must be an object", error);
    if (rc != CAI_OK) {
      jsonrpc_error_code = -32602;
      preserve_error = 0;
    }
    if (rc == CAI_OK) {
      CAI_LJ->spooled_init(CAI_LJ, &result);
      rc = cai_mcp_write_cstr(&result, "{}", error);
    }
    result_error_message = "failed to write MCP ping result";
  } else if (strcmp(doc->method, "roots/list") == 0) {
    rc = cai_mcp_validate_optional_request_params(
        &doc->params, "MCP roots/list params must be an object", error);
    if (rc != CAI_OK) {
      jsonrpc_error_code = -32602;
      preserve_error = 0;
    }
    if (rc == CAI_OK) {
      rc = cai_mcp_build_roots_result(impl, &result, &jsonrpc_error_code,
                                      &preserve_error, error);
    }
    result_error_message = "failed to write MCP roots result";
  } else if (strcmp(doc->method, "sampling/createMessage") == 0) {
    rc = cai_mcp_build_sampling_result(impl, doc, &result, &jsonrpc_error_code,
                                       &preserve_error, error);
    result_error_message = "failed to write MCP sampling result";
  } else if (strcmp(doc->method, "elicitation/create") == 0) {
    rc = cai_mcp_build_elicitation_result(
        impl, doc, &result, &jsonrpc_error_code, &preserve_error, error);
    result_error_message = "failed to write MCP elicitation result";
  } else {
    rc = cai_mcp_write_jsonrpc_error(
        response, &doc->id, -32601,
        "MCP server-to-client request method is not supported", error);
    if (out_len != NULL) {
      *out_len = response->size_fn(response);
    }
    return rc;
  }
  if (rc == CAI_OK && strcmp(doc->method, "roots/list") == 0) {
    rc = cai_mcp_validate_roots_result(&result, error);
    if (rc != CAI_OK) {
      jsonrpc_error_code = -32603;
      preserve_error = 1;
    }
  } else if (rc == CAI_OK &&
             strcmp(doc->method, "sampling/createMessage") == 0) {
    rc = cai_mcp_validate_sampling_result(&result, error);
    if (rc != CAI_OK) {
      jsonrpc_error_code = -32603;
      preserve_error = 1;
    }
  } else if (rc == CAI_OK && strcmp(doc->method, "elicitation/create") == 0) {
    rc = cai_mcp_validate_elicitation_result(&result, error);
    if (rc != CAI_OK) {
      jsonrpc_error_code = -32603;
      preserve_error = 1;
    }
  }
  if (rc != CAI_OK && response->size_fn(response) == 0U) {
    char message[256];
    int response_rc;

    snprintf(message, sizeof(message), "%s",
             error != NULL && error->message != NULL
                 ? error->message
                 : "MCP server request failed");
    if (!preserve_error) {
      cai_mcp_clear_error(error);
    }
    response_rc = cai_mcp_write_jsonrpc_error(
        response, &doc->id, jsonrpc_error_code, message, error);
    if (out_len != NULL) {
      *out_len = response->size_fn(response);
    }
    cai_mcp_spooled_cleanup_if_initialized(&result);
    if (response_rc != CAI_OK) {
      return response_rc;
    }
    return preserve_error ? rc : CAI_OK;
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(response, "{\"jsonrpc\":\"2.0\",\"id\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_value(
        response, &doc->id, "failed to write MCP server request id", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(response, ",\"result\":", error);
  }
  if (rc == CAI_OK) {
    lonejson_error_init(&json_error);
    memset(&writer, 0, sizeof(writer));
    status = CAI_LJ->writer_init_sink(CAI_LJ, &writer, cai_mcp_spool_sink,
                                      response, &json_error);
    if (status == LONEJSON_STATUS_OK) {
      status = writer.json_value_spooled(&writer, &result, &json_error);
    }
    if (writer.cleanup != NULL) {
      writer.cleanup(&writer);
    }
    if (status != LONEJSON_STATUS_OK) {
      rc = cai_mcp_set_json_error(error, result_error_message, &json_error);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(response, "}", error);
  }
  if (out_len != NULL) {
    *out_len = response->size_fn(response);
  }
  cai_mcp_spooled_cleanup_if_initialized(&result);
  if (rc != CAI_OK) {
    response->cleanup(response);
    memset(response, 0, sizeof(*response));
  }
  return rc;
}

static int
cai_mcp_handle_server_request(cai_mcp_streamable_http_client_impl *impl,
                              const cai_mcp_jsonrpc_message_doc *doc,
                              cai_error *error) {
  cai_mcp_http_response_capture ack;
  lonejson_spooled response;
  size_t response_len;
  int rc;

  memset(&ack, 0, sizeof(ack));
  memset(&response, 0, sizeof(response));
  response_len = 0U;
  rc = cai_mcp_server_request_response(impl, doc, &response, &response_len,
                                       error);
  if (response.size_fn != NULL && response_len > 0U) {
    int response_rc;

    response_rc = cai_mcp_post(impl, &response, response_len, 0, &ack, error);
    cai_mcp_http_response_capture_cleanup(&ack);
    if (response_rc != CAI_OK) {
      rc = response_rc;
    }
  }
  cai_mcp_spooled_cleanup_if_initialized(&response);
  return rc;
}

static int cai_mcp_parse_sse_message(lonejson_spooled *data,
                                     cai_mcp_jsonrpc_message_doc *doc,
                                     cai_error *error) {
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  int has_error;
  int has_id;
  int has_result;
  int rc;

  memset(doc, 0, sizeof(*doc));
  reader.cursor = *data;
  lonejson_error_init(&json_error);
  if (reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    return cai_mcp_set_json_error(error, "failed to rewind MCP SSE data",
                                  &json_error);
  }
  status = CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_jsonrpc_message_map, doc,
                                cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_jsonrpc_message_map, doc);
    return cai_mcp_set_json_error(error, "failed to parse MCP SSE JSON-RPC",
                                  &json_error);
  }
  if (doc->jsonrpc == NULL || strcmp(doc->jsonrpc, "2.0") != 0) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_jsonrpc_message_map, doc);
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP JSON-RPC version must be 2.0");
  }
  rc = cai_mcp_jsonrpc_top_level_member_presence(
      data, "id", &has_id, "failed to parse MCP SSE JSON-RPC", error);
  if (rc != CAI_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_jsonrpc_message_map, doc);
    return rc;
  }
  doc->has_id = has_id;
  if (doc->has_id) {
    rc = cai_mcp_validate_jsonrpc_id_value(&doc->id, error);
    if (rc != CAI_OK) {
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_jsonrpc_message_map, doc);
      return rc;
    }
  }
  if (doc->method != NULL) {
    rc = cai_mcp_jsonrpc_response_result_error_presence(data, &has_result,
                                                        &has_error, error);
    if (rc != CAI_OK) {
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_jsonrpc_message_map, doc);
      return rc;
    }
    if (has_result || has_error) {
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_jsonrpc_message_map, doc);
      return cai_set_error(
          error, CAI_ERR_PROTOCOL,
          "MCP JSON-RPC message with method must not include result or error");
    }
  } else {
    if (!doc->has_id) {
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_jsonrpc_message_map, doc);
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP JSON-RPC was missing id");
    }
    rc = cai_mcp_jsonrpc_response_result_error_presence(data, &has_result,
                                                        &has_error, error);
    if (rc != CAI_OK) {
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_jsonrpc_message_map, doc);
      return rc;
    }
    if (has_result == has_error) {
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_jsonrpc_message_map, doc);
      return cai_set_error(
          error, CAI_ERR_PROTOCOL,
          "MCP JSON-RPC response must include exactly one of result or error");
    }
  }
  return CAI_OK;
}

static int cai_mcp_sse_line_is_blank(const char *line, size_t len) {
  size_t i;

  for (i = 0U; i < len; i++) {
    if (line[i] != '\r' && line[i] != ' ' && line[i] != '\t') {
      return 0;
    }
  }
  return 1;
}

static void cai_mcp_sse_set_event(char *event, size_t event_size,
                                  const char *value, size_t len) {
  while (len > 0U && (*value == ' ' || *value == '\t')) {
    value++;
    len--;
  }
  while (len > 0U && (value[len - 1U] == '\r' || value[len - 1U] == ' ' ||
                      value[len - 1U] == '\t')) {
    len--;
  }
  if (event_size == 0U) {
    return;
  }
  if (len >= event_size) {
    len = event_size - 1U;
  }
  memcpy(event, value, len);
  event[len] = '\0';
}

static int cai_mcp_sse_data_event_done(const char *event,
                                       const lonejson_spooled *data) {
  return data->size_fn(data) != 0U &&
         (event[0] == '\0' || strcmp(event, "message") == 0);
}

static int cai_mcp_sse_set_last_event_id(char **last_event_id,
                                         const char *value, size_t len,
                                         cai_error *error) {
  char *copy;

  if (last_event_id == NULL) {
    return CAI_OK;
  }
  if (len > 0U && *value == ' ') {
    value++;
    len--;
  }
  while (len > 0U && value[len - 1U] == '\r') {
    len--;
  }
  copy = cai_strndup(NULL, value, len);
  if (copy == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to store MCP SSE event id");
  }
  cai_free_mem(NULL, *last_event_id);
  *last_event_id = copy;
  return CAI_OK;
}

static void cai_mcp_sse_set_retry(cai_mcp_sse_resume_state *resume,
                                  const char *value, size_t len) {
  long retry_ms;
  size_t i;

  if (resume == NULL) {
    return;
  }
  if (len > 0U && *value == ' ') {
    value++;
    len--;
  }
  while (len > 0U && value[len - 1U] == '\r') {
    len--;
  }
  if (len == 0U) {
    return;
  }
  retry_ms = 0L;
  for (i = 0U; i < len; i++) {
    if (value[i] < '0' || value[i] > '9') {
      return;
    }
    if (retry_ms < 214748364L) {
      retry_ms = retry_ms * 10L + (long)(value[i] - '0');
    }
  }
  resume->retry_ms = retry_ms;
  resume->has_retry = 1;
}

typedef void (*cai_mcp_sleep_ms_fn)(long ms);

#ifdef CAI_TESTING
static cai_mcp_sleep_ms_fn cai_mcp_test_sleep_ms_fn = NULL;

void cai_mcp_test_set_sleep_ms_fn(cai_mcp_sleep_ms_fn fn);

void cai_mcp_test_set_sleep_ms_fn(cai_mcp_sleep_ms_fn fn) {
  cai_mcp_test_sleep_ms_fn = fn;
}
#endif

static void cai_mcp_sleep_ms(long ms) {
  struct timeval tv;

  if (ms <= 0L) {
    return;
  }
#ifdef CAI_TESTING
  if (cai_mcp_test_sleep_ms_fn != NULL) {
    cai_mcp_test_sleep_ms_fn(ms);
    return;
  }
#endif
  tv.tv_sec = ms / 1000L;
  tv.tv_usec = (ms % 1000L) * 1000L;
  (void)select(0, NULL, NULL, NULL, &tv);
}

static int cai_mcp_sse_handle_line(lonejson_spooled *data, char *event,
                                   size_t event_size,
                                   cai_mcp_sse_resume_state *resume,
                                   const char *line, size_t len, int *done,
                                   cai_error *error) {
  lonejson_error json_error;
  const char *value;

  if (cai_mcp_sse_line_is_blank(line, len)) {
    if (cai_mcp_sse_data_event_done(event, data)) {
      *done = 1;
    } else {
      data->reset(data);
      event[0] = '\0';
    }
    return CAI_OK;
  }
  if (len >= 5U && memcmp(line, "data:", 5U) == 0) {
    value = line + 5U;
    len -= 5U;
    if (len > 0U && *value == ' ') {
      value++;
      len--;
    }
    while (len > 0U && value[len - 1U] == '\r') {
      len--;
    }
    if (len == 0U) {
      return CAI_OK;
    }
    if (data->size_fn(data) != 0U) {
      lonejson_error_init(&json_error);
      if (data->append(data, "\n", 1U, &json_error) != LONEJSON_STATUS_OK) {
        return cai_mcp_set_json_error(error, "failed to parse MCP SSE data",
                                      &json_error);
      }
    }
    lonejson_error_init(&json_error);
    if (data->append(data, value, len, &json_error) != LONEJSON_STATUS_OK) {
      return cai_mcp_set_json_error(error, "failed to parse MCP SSE data",
                                    &json_error);
    }
  } else if (len >= 6U && memcmp(line, "event:", 6U) == 0) {
    cai_mcp_sse_set_event(event, event_size, line + 6U, len - 6U);
  } else if (len >= 3U && memcmp(line, "id:", 3U) == 0) {
    return cai_mcp_sse_set_last_event_id(resume != NULL ? &resume->last_event_id
                                                        : NULL,
                                         line + 3U, len - 3U, error);
  } else if (len >= 6U && memcmp(line, "retry:", 6U) == 0) {
    cai_mcp_sse_set_retry(resume, line + 6U, len - 6U);
  }
  return CAI_OK;
}

static int cai_mcp_process_sse_message(
    cai_mcp_streamable_http_client_impl *impl, lonejson_spooled *data,
    lonejson_spooled *final_body, int *final_seen, cai_error *error) {
  cai_mcp_jsonrpc_message_doc doc;
  int rc;

  if (data == NULL || final_body == NULL || final_seen == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP SSE message is required");
  }
  rc = cai_mcp_parse_sse_message(data, &doc, error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (doc.method != NULL) {
    if (doc.has_id) {
      rc = cai_mcp_handle_server_request(impl, &doc, error);
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_jsonrpc_message_map, &doc);
      return rc;
    }
    rc = cai_mcp_dispatch_notification(impl, &doc, error);
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_jsonrpc_message_map, &doc);
    return rc;
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_jsonrpc_message_map, &doc);
  if (*final_seen) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP SSE response included multiple JSON-RPC replies");
  }
  rc = cai_mcp_spool_copy(data, final_body, error);
  if (rc == CAI_OK) {
    *final_seen = 1;
  }
  return rc;
}

static int
cai_mcp_process_sse_server_message(cai_mcp_streamable_http_client_impl *impl,
                                   lonejson_spooled *data, cai_error *error) {
  cai_mcp_jsonrpc_message_doc doc;
  int rc;

  if (data == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP SSE message is required");
  }
  rc = cai_mcp_parse_sse_message(data, &doc, error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (doc.method != NULL) {
    if (doc.has_id) {
      rc = cai_mcp_handle_server_request(impl, &doc, error);
    } else {
      rc = cai_mcp_dispatch_notification(impl, &doc, error);
    }
  } else {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "MCP SSE server message was missing method");
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_jsonrpc_message_map, &doc);
  return rc;
}

static int
cai_mcp_sse_normalize_response(cai_mcp_streamable_http_client_impl *impl,
                               cai_mcp_http_response_capture *response,
                               int allow_resume, cai_error *error) {
  lonejson_spooled cursor;
  lonejson_spooled event_data;
  lonejson_spooled final_body;
  lonejson_error json_error;
  lonejson_read_result chunk;
  cai_mcp_http_response_capture resumed;
  cai_mcp_sse_resume_state resume;
  cai_buffer_builder line;
  unsigned char buffer[4096];
  char event[64];
  char *content_type;
  size_t i;
  int event_ready;
  int final_seen;
  int rc;

  if (response == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP response is required");
  }
  cursor = response->body;
  CAI_LJ->spooled_init(CAI_LJ, &event_data);
  memset(&final_body, 0, sizeof(final_body));
  memset(&resumed, 0, sizeof(resumed));
  memset(&resume, 0, sizeof(resume));
  memset(&line, 0, sizeof(line));
  event[0] = '\0';
  event_ready = 0;
  final_seen = 0;
  rc = CAI_OK;
  lonejson_error_init(&json_error);
  if (cursor.rewind(&cursor, &json_error) != LONEJSON_STATUS_OK) {
    event_data.cleanup(&event_data);
    return cai_mcp_set_json_error(error, "failed to rewind MCP SSE response",
                                  &json_error);
  }
  for (;;) {
    chunk = cursor.read(&cursor, buffer, sizeof(buffer));
    if (chunk.error_code != 0) {
      rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to read MCP SSE response");
      break;
    }
    for (i = 0U; i < chunk.bytes_read; i++) {
      if (buffer[i] == '\n') {
        rc = cai_mcp_sse_handle_line(&event_data, event, sizeof(event), &resume,
                                     line.data, line.length, &event_ready,
                                     error);
        line.length = 0U;
        if (line.data != NULL) {
          line.data[0] = '\0';
        }
        if (rc == CAI_OK && event_ready) {
          rc = cai_mcp_process_sse_message(impl, &event_data, &final_body,
                                           &final_seen, error);
          event_data.reset(&event_data);
          event[0] = '\0';
          event_ready = 0;
        }
      } else {
        rc = cai_buffer_append(&line, (const char *)&buffer[i], 1U, error);
      }
      if (rc != CAI_OK) {
        break;
      }
    }
    if (rc != CAI_OK || chunk.eof) {
      break;
    }
  }
  if (rc == CAI_OK && line.length != 0U) {
    rc = cai_mcp_sse_handle_line(&event_data, event, sizeof(event), &resume,
                                 line.data, line.length, &event_ready, error);
    if (rc == CAI_OK && event_ready) {
      rc = cai_mcp_process_sse_message(impl, &event_data, &final_body,
                                       &final_seen, error);
    }
  }
  cai_free_mem(NULL, line.data);
  event_data.cleanup(&event_data);
  if (rc == CAI_OK && !final_seen && allow_resume &&
      resume.last_event_id != NULL && resume.last_event_id[0] != '\0') {
    if (resume.has_retry) {
      cai_mcp_sleep_ms(resume.retry_ms);
    }
    rc = cai_mcp_get_resume_response(impl, resume.last_event_id, &resumed,
                                     error);
    if (rc == CAI_OK) {
      response->body.cleanup(&response->body);
      cai_free_mem(NULL, response->content_type);
      cai_free_mem(NULL, response->session_id);
      *response = resumed;
      memset(&resumed, 0, sizeof(resumed));
      cai_free_mem(NULL, resume.last_event_id);
      return CAI_OK;
    }
  }
  if (rc == CAI_OK && !final_seen) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "MCP SSE response did not include JSON-RPC response");
  }
  cai_mcp_http_response_capture_cleanup(&resumed);
  cai_free_mem(NULL, resume.last_event_id);
  if (rc != CAI_OK) {
    cai_mcp_spooled_cleanup_if_initialized(&final_body);
    return rc;
  }
  content_type = cai_strdup(NULL, "application/json");
  if (content_type == NULL) {
    final_body.cleanup(&final_body);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to store MCP response content type");
  }
  response->body.cleanup(&response->body);
  response->body = final_body;
  cai_free_mem(NULL, response->content_type);
  response->content_type = content_type;
  return CAI_OK;
}

static int cai_mcp_sse_stream_process_ready(cai_mcp_sse_stream_state *state) {
  int rc;

  if (state == NULL) {
    return CAI_ERR_INVALID;
  }
  rc = cai_mcp_process_sse_server_message(state->impl, &state->event_data,
                                          state->error);
  state->event_data.reset(&state->event_data);
  state->event[0] = '\0';
  state->event_ready = 0;
  return rc;
}

static size_t cai_mcp_sse_stream_write(char *ptr, size_t size, size_t nmemb,
                                       void *userdata) {
  cai_mcp_sse_stream_state *state;
  size_t total;
  size_t i;
  int rc;

  state = (cai_mcp_sse_stream_state *)userdata;
  total = size * nmemb;
  if (state == NULL || ptr == NULL) {
    return 0U;
  }
  rc = CAI_OK;
  for (i = 0U; i < total; i++) {
    if (ptr[i] == '\n') {
      rc = cai_mcp_sse_handle_line(&state->event_data, state->event,
                                   sizeof(state->event), &state->resume,
                                   state->line.data, state->line.length,
                                   &state->event_ready, state->error);
      state->line.length = 0U;
      if (state->line.data != NULL) {
        state->line.data[0] = '\0';
      }
      if (rc == CAI_OK && state->event_ready) {
        rc = cai_mcp_sse_stream_process_ready(state);
      }
    } else {
      rc = cai_buffer_append(&state->line, &ptr[i], 1U, state->error);
    }
    if (rc != CAI_OK) {
      state->failed = 1;
      state->rc = rc;
      return 0U;
    }
  }
  return total;
}

static int cai_mcp_sse_stream_finish(cai_mcp_sse_stream_state *state) {
  int rc;

  if (state == NULL) {
    return CAI_ERR_INVALID;
  }
  rc = CAI_OK;
  if (state->line.length != 0U) {
    rc = cai_mcp_sse_handle_line(&state->event_data, state->event,
                                 sizeof(state->event), &state->resume,
                                 state->line.data, state->line.length,
                                 &state->event_ready, state->error);
    state->line.length = 0U;
    if (state->line.data != NULL) {
      state->line.data[0] = '\0';
    }
  }
  if (rc == CAI_OK && !state->event_ready &&
      cai_mcp_sse_data_event_done(state->event, &state->event_data)) {
    state->event_ready = 1;
  }
  if (rc == CAI_OK && state->event_ready) {
    rc = cai_mcp_sse_stream_process_ready(state);
  }
  return rc;
}

static int cai_mcp_get_events_once(cai_mcp_streamable_http_client_impl *impl,
                                   const char *last_event_id, int allow_405,
                                   cai_mcp_sse_resume_state *resume_out,
                                   cai_error *error) {
  CURL *curl;
  CURLcode curl_rc;
  struct curl_slist *headers;
  cai_mcp_http_response_capture response;
  cai_mcp_sse_stream_state stream;
  int rc;

  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP client is required");
  }
  headers = NULL;
  curl = curl_easy_init();
  if (curl == NULL) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to initialize MCP HTTP request");
  }
  memset(&response, 0, sizeof(response));
  CAI_LJ->spooled_init(CAI_LJ, &response.body);
  memset(&stream, 0, sizeof(stream));
  stream.impl = impl;
  stream.error = error;
  stream.rc = CAI_OK;
  CAI_LJ->spooled_init(CAI_LJ, &stream.event_data);

  rc = cai_append_header(&headers, "Accept: text/event-stream", error);
  if (rc == CAI_OK) {
    rc = cai_mcp_append_session_headers(impl, &headers, error);
  }
  if (rc == CAI_OK && last_event_id != NULL) {
    rc = cai_mcp_append_last_event_id_header(&headers, last_event_id, error);
  }
  if (rc != CAI_OK) {
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    stream.event_data.cleanup(&stream.event_data);
    cai_free_mem(NULL, stream.line.data);
    cai_free_mem(NULL, stream.resume.last_event_id);
    cai_mcp_http_response_capture_cleanup(&response);
    return rc;
  }

  curl_easy_setopt(curl, CURLOPT_URL, impl->url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cai_mcp_sse_stream_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &stream);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, cai_mcp_header_callback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, impl->timeout_ms);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, impl->timeout_ms);
  cai_configure_curl_tls(curl, impl->insecure_skip_verify, impl->ca_bundle_path,
                         impl->ca_path);
  curl_rc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);
  if (curl_rc != CURLE_OK) {
    if (stream.failed && stream.rc != CAI_OK) {
      rc = stream.rc;
    } else {
      rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "MCP HTTP event stream failed",
                                curl_easy_strerror(curl_rc));
    }
  } else if (response.status == 405L && allow_405) {
    rc = CAI_OK;
  } else {
    rc = cai_mcp_response_ok(&response, error);
    if (rc == CAI_OK && !cai_mcp_response_is_sse(&response)) {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP event stream response was not SSE");
    }
    if (rc == CAI_OK) {
      rc = cai_mcp_sse_stream_finish(&stream);
    }
  }
  if (rc == CAI_OK && resume_out != NULL &&
      stream.resume.last_event_id != NULL &&
      stream.resume.last_event_id[0] != '\0') {
    cai_free_mem(NULL, resume_out->last_event_id);
    *resume_out = stream.resume;
    memset(&stream.resume, 0, sizeof(stream.resume));
  }
  stream.event_data.cleanup(&stream.event_data);
  cai_free_mem(NULL, stream.line.data);
  cai_free_mem(NULL, stream.resume.last_event_id);
  cai_mcp_http_response_capture_cleanup(&response);
  return rc;
}

static int cai_mcp_get_events(cai_mcp_streamable_http_client_impl *impl,
                              cai_error *error) {
  cai_mcp_sse_resume_state resume;
  int rc;

  memset(&resume, 0, sizeof(resume));
  rc = cai_mcp_get_events_once(impl, NULL, 1, &resume, error);
  if (rc == CAI_OK && resume.last_event_id != NULL &&
      resume.last_event_id[0] != '\0') {
    if (resume.has_retry) {
      cai_mcp_sleep_ms(resume.retry_ms);
    }
    rc = cai_mcp_get_events_once(impl, resume.last_event_id, 0, NULL, error);
  }
  cai_free_mem(NULL, resume.last_event_id);
  return rc;
}

static int
cai_mcp_sse_response_json_body(const cai_mcp_http_response_capture *response,
                               lonejson_spooled *out, cai_error *error) {
  lonejson_spooled cursor;
  lonejson_error json_error;
  lonejson_read_result chunk;
  cai_buffer_builder line;
  unsigned char buffer[4096];
  char event[64];
  size_t i;
  int done;
  int rc;

  CAI_LJ->spooled_init(CAI_LJ, out);
  cursor = response->body;
  memset(&line, 0, sizeof(line));
  event[0] = '\0';
  done = 0;
  rc = CAI_OK;
  lonejson_error_init(&json_error);
  if (cursor.rewind(&cursor, &json_error) != LONEJSON_STATUS_OK) {
    out->cleanup(out);
    return cai_mcp_set_json_error(error, "failed to rewind MCP SSE response",
                                  &json_error);
  }
  while (!done) {
    chunk = cursor.read(&cursor, buffer, sizeof(buffer));
    if (chunk.error_code != 0) {
      rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to read MCP SSE response");
      break;
    }
    for (i = 0U; i < chunk.bytes_read && !done; i++) {
      if (buffer[i] == '\n') {
        rc = cai_mcp_sse_handle_line(out, event, sizeof(event), NULL, line.data,
                                     line.length, &done, error);
        line.length = 0U;
        if (line.data != NULL) {
          line.data[0] = '\0';
        }
      } else {
        rc = cai_buffer_append(&line, (const char *)&buffer[i], 1U, error);
      }
      if (rc != CAI_OK) {
        break;
      }
    }
    if (rc != CAI_OK || chunk.eof) {
      break;
    }
  }
  if (rc == CAI_OK && !done && line.length != 0U) {
    rc = cai_mcp_sse_handle_line(out, event, sizeof(event), NULL, line.data,
                                 line.length, &done, error);
  }
  cai_free_mem(NULL, line.data);
  if (rc == CAI_OK && !done && !cai_mcp_sse_data_event_done(event, out)) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "MCP SSE response did not include JSON-RPC message");
  }
  if (rc != CAI_OK) {
    out->cleanup(out);
  }
  return rc;
}

static int
cai_mcp_response_json_body(const cai_mcp_http_response_capture *response,
                           const char *operation, lonejson_spooled *out,
                           cai_error *error) {
  if (cai_mcp_response_is_json(response)) {
    return cai_mcp_spool_copy(&response->body, out, error);
  }
  if (cai_mcp_response_is_sse(response)) {
    return cai_mcp_sse_response_json_body(response, out, error);
  }
  return cai_set_error_detail(error, CAI_ERR_PROTOCOL, operation,
                              "MCP response was neither JSON nor SSE");
}

static int cai_mcp_write_json_string(lonejson_spooled *spool, const char *text,
                                     cai_error *error) {
  lonejson_error json_error;
  lonejson_writer writer;
  lonejson_status status;

  lonejson_error_init(&json_error);
  status = CAI_LJ->writer_init_sink(CAI_LJ, &writer, cai_mcp_spool_sink, spool,
                                    &json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = writer.string(&writer, text != NULL ? text : "",
                           text != NULL ? strlen(text) : 0U, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.finish(&writer, &json_error);
  }
  if (writer.cleanup != NULL) {
    writer.cleanup(&writer);
  }
  if (status != LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to write MCP JSON string",
                                json_error.message);
  }
  return CAI_OK;
}

static int cai_mcp_log_level_valid(const char *level) {
  static const char *levels[] = {"debug", "info",     "notice", "warning",
                                 "error", "critical", "alert",  "emergency"};
  size_t i;

  if (level == NULL || level[0] == '\0') {
    return 0;
  }
  for (i = 0U; i < sizeof(levels) / sizeof(levels[0]); i++) {
    if (strcmp(level, levels[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

static int cai_mcp_request_begin(cai_mcp_streamable_http_client_impl *impl,
                                 lonejson_spooled *spool, long long id,
                                 const char *method, cai_error *error) {
  char id_buf[64];
  int rc;

  snprintf(id_buf, sizeof(id_buf), "%lld", id);
  rc = cai_mcp_write_cstr(spool, "{\"jsonrpc\":\"2.0\",\"id\":", error);
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, id_buf, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, ",\"method\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(spool, method, error);
  }
  (void)impl;
  return rc;
}

static int cai_mcp_initialize_capability_separator(lonejson_spooled *spool,
                                                   int *has_capability,
                                                   cai_error *error) {
  int rc;

  if (has_capability != NULL && *has_capability) {
    rc = cai_mcp_write_cstr(spool, ",", error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  if (has_capability != NULL) {
    *has_capability = 1;
  }
  return CAI_OK;
}

static int cai_mcp_initialize_task_request_separator(lonejson_spooled *spool,
                                                     int *has_request,
                                                     cai_error *error) {
  int rc;

  if (has_request != NULL && *has_request) {
    rc = cai_mcp_write_cstr(spool, ",", error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  if (has_request != NULL) {
    *has_request = 1;
  }
  return CAI_OK;
}

static int
cai_mcp_initialize_task_requests(cai_mcp_streamable_http_client_impl *impl,
                                 lonejson_spooled *spool, int *has_capability,
                                 cai_error *error) {
  int has_request;
  int rc;

  if ((impl->receiver.task_sampling_create_message == 0 ||
       impl->receiver.create_message == NULL) &&
      (impl->receiver.task_elicitation_create == 0 ||
       impl->receiver.elicit == NULL)) {
    return CAI_OK;
  }
  rc = cai_mcp_initialize_capability_separator(spool, has_capability, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, "\"tasks\":{\"requests\":{", error);
  }
  has_request = 0;
  if (rc == CAI_OK && impl->receiver.task_sampling_create_message &&
      impl->receiver.create_message != NULL) {
    rc = cai_mcp_initialize_task_request_separator(spool, &has_request, error);
    if (rc == CAI_OK) {
      rc = cai_mcp_write_cstr(spool, "\"sampling\":{\"createMessage\":{}}",
                              error);
    }
  }
  if (rc == CAI_OK && impl->receiver.task_elicitation_create &&
      impl->receiver.elicit != NULL) {
    rc = cai_mcp_initialize_task_request_separator(spool, &has_request, error);
    if (rc == CAI_OK) {
      rc = cai_mcp_write_cstr(spool, "\"elicitation\":{\"create\":{}}", error);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, "}}", error);
  }
  return rc;
}

static int cai_mcp_initialize_request(cai_mcp_streamable_http_client_impl *impl,
                                      lonejson_spooled *spool, size_t *out_len,
                                      cai_error *error) {
  int has_capability;
  int has_subcapability;
  int rc;

  CAI_LJ->spooled_init(CAI_LJ, spool);
  has_capability = 0;
  rc = cai_mcp_request_begin(impl, spool, ++impl->next_id, "initialize", error);
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, ",\"params\":{\"protocolVersion\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(spool, impl->protocol_version, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, ",\"capabilities\":{", error);
  }
  if (rc == CAI_OK && impl->receiver.list_roots != NULL) {
    rc = cai_mcp_initialize_capability_separator(spool, &has_capability, error);
    if (rc == CAI_OK) {
      rc = cai_mcp_write_cstr(spool, "\"roots\":{\"listChanged\":", error);
    }
    if (rc == CAI_OK) {
      rc = cai_mcp_write_cstr(
          spool, impl->receiver.roots_list_changed ? "true}" : "false}", error);
    }
  }
  if (rc == CAI_OK && impl->receiver.create_message != NULL) {
    rc = cai_mcp_initialize_capability_separator(spool, &has_capability, error);
    if (rc == CAI_OK) {
      rc = cai_mcp_write_cstr(spool, "\"sampling\":{", error);
    }
    has_subcapability = 0;
    if (rc == CAI_OK && impl->receiver.sampling_tools) {
      rc = cai_mcp_write_cstr(spool, "\"tools\":{}", error);
      has_subcapability = 1;
    }
    if (rc == CAI_OK && impl->receiver.sampling_context) {
      if (has_subcapability) {
        rc = cai_mcp_write_cstr(spool, ",", error);
      }
      if (rc == CAI_OK) {
        rc = cai_mcp_write_cstr(spool, "\"context\":{}", error);
      }
    }
    if (rc == CAI_OK) {
      rc = cai_mcp_write_cstr(spool, "}", error);
    }
  }
  if (rc == CAI_OK && impl->receiver.elicit != NULL) {
    rc = cai_mcp_initialize_capability_separator(spool, &has_capability, error);
    if (rc == CAI_OK) {
      rc = cai_mcp_write_cstr(spool, "\"elicitation\":{", error);
    }
    has_subcapability = 0;
    if (rc == CAI_OK && impl->receiver.elicitation_form) {
      rc = cai_mcp_write_cstr(spool, "\"form\":{}", error);
      has_subcapability = 1;
    }
    if (rc == CAI_OK && impl->receiver.elicitation_url) {
      if (has_subcapability) {
        rc = cai_mcp_write_cstr(spool, ",", error);
      }
      if (rc == CAI_OK) {
        rc = cai_mcp_write_cstr(spool, "\"url\":{}", error);
      }
    }
    if (rc == CAI_OK) {
      rc = cai_mcp_write_cstr(spool, "}", error);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_initialize_task_requests(impl, spool, &has_capability, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, "},\"clientInfo\":{\"name\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(spool, impl->client_name, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, ",\"version\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(spool, impl->client_version, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, "}}}", error);
  }
  if (out_len != NULL) {
    *out_len = spool->size_fn(spool);
  }
  if (rc != CAI_OK) {
    spool->cleanup(spool);
  }
  return rc;
}

static int cai_mcp_list_request(cai_mcp_streamable_http_client_impl *impl,
                                const char *method, const char *cursor,
                                lonejson_spooled *spool, size_t *out_len,
                                cai_error *error) {
  int rc;

  CAI_LJ->spooled_init(CAI_LJ, spool);
  rc = cai_mcp_request_begin(impl, spool, ++impl->next_id, method, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, ",\"params\":{", error);
  }
  if (rc == CAI_OK && cursor != NULL && cursor[0] != '\0') {
    rc = cai_mcp_write_cstr(spool, "\"cursor\":", error);
    if (rc == CAI_OK) {
      rc = cai_mcp_write_json_string(spool, cursor, error);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, "}}", error);
  }
  if (out_len != NULL) {
    *out_len = spool->size_fn(spool);
  }
  if (rc != CAI_OK) {
    spool->cleanup(spool);
  }
  return rc;
}

static int cai_mcp_generic_request(cai_mcp_streamable_http_client_impl *impl,
                                   const char *method,
                                   lonejson_spooled *params_json,
                                   lonejson_spooled *spool, size_t *out_len,
                                   cai_error *error) {
  lonejson_error json_error;
  lonejson_writer writer;
  lonejson_status status;
  int rc;

  if (method == NULL || method[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP request method is required");
  }
  CAI_LJ->spooled_init(CAI_LJ, spool);
  rc = cai_mcp_request_begin(impl, spool, ++impl->next_id, method, error);
  if (rc == CAI_OK && params_json != NULL) {
    rc = cai_mcp_write_cstr(spool, ",\"params\":", error);
    if (rc == CAI_OK) {
      lonejson_error_init(&json_error);
      status = CAI_LJ->writer_init_sink(CAI_LJ, &writer, cai_mcp_spool_sink,
                                        spool, &json_error);
      if (status == LONEJSON_STATUS_OK) {
        status = writer.json_value_spooled(&writer, params_json, &json_error);
      }
      if (writer.cleanup != NULL) {
        writer.cleanup(&writer);
      }
      if (status != LONEJSON_STATUS_OK) {
        rc = cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                  "failed to write MCP request params",
                                  json_error.message);
      }
    }
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, "}", error);
  }
  if (out_len != NULL) {
    *out_len = spool->size_fn(spool);
  }
  if (rc != CAI_OK) {
    spool->cleanup(spool);
  }
  return rc;
}

static int cai_mcp_initialized_notification(lonejson_spooled *spool,
                                            size_t *out_len, cai_error *error) {
  int rc;

  CAI_LJ->spooled_init(CAI_LJ, spool);
  rc = cai_mcp_write_cstr(
      spool, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}",
      error);
  if (out_len != NULL) {
    *out_len = spool->size_fn(spool);
  }
  if (rc != CAI_OK) {
    spool->cleanup(spool);
  }
  return rc;
}

static int cai_mcp_notification_request(const char *method,
                                        lonejson_spooled *params_json,
                                        lonejson_spooled *spool,
                                        size_t *out_len, cai_error *error) {
  lonejson_error json_error;
  lonejson_writer writer;
  lonejson_status status;
  int rc;

  if (method == NULL || method[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP notification method is required");
  }
  CAI_LJ->spooled_init(CAI_LJ, spool);
  rc = cai_mcp_write_cstr(spool, "{\"jsonrpc\":\"2.0\",\"method\":", error);
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(spool, method, error);
  }
  if (rc == CAI_OK && params_json != NULL) {
    rc = cai_mcp_write_cstr(spool, ",\"params\":", error);
    if (rc == CAI_OK) {
      lonejson_error_init(&json_error);
      status = CAI_LJ->writer_init_sink(CAI_LJ, &writer, cai_mcp_spool_sink,
                                        spool, &json_error);
      if (status == LONEJSON_STATUS_OK) {
        status = writer.json_value_spooled(&writer, params_json, &json_error);
      }
      if (writer.cleanup != NULL) {
        writer.cleanup(&writer);
      }
      if (status != LONEJSON_STATUS_OK) {
        rc = cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                  "failed to write MCP notification params",
                                  json_error.message);
      }
    }
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, "}", error);
  }
  if (out_len != NULL) {
    *out_len = spool->size_fn(spool);
  }
  if (rc != CAI_OK) {
    spool->cleanup(spool);
  }
  return rc;
}

static int cai_mcp_ping_request(cai_mcp_streamable_http_client_impl *impl,
                                lonejson_spooled *spool, size_t *out_len,
                                cai_error *error) {
  int rc;

  CAI_LJ->spooled_init(CAI_LJ, spool);
  rc = cai_mcp_request_begin(impl, spool, ++impl->next_id, "ping", error);
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, "}", error);
  }
  if (out_len != NULL) {
    *out_len = spool->size_fn(spool);
  }
  if (rc != CAI_OK) {
    spool->cleanup(spool);
  }
  return rc;
}

static int
cai_mcp_write_progress_meta(cai_mcp_streamable_http_client_impl *impl,
                            lonejson_spooled *spool, cai_error *error) {
  char token[64];
  int rc;

  snprintf(token, sizeof(token), "%lld", impl->next_id);
  rc = cai_mcp_write_cstr(spool, ",\"_meta\":{\"progressToken\":", error);
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, token, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, "}", error);
  }
  return rc;
}

static int
cai_mcp_write_progress_meta_member(cai_mcp_streamable_http_client_impl *impl,
                                   lonejson_spooled *spool,
                                   int has_prior_member, cai_error *error) {
  int rc;

  rc = CAI_OK;
  if (has_prior_member) {
    rc = cai_mcp_write_cstr(spool, ",", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, "\"_meta\":{\"progressToken\":", error);
  }
  if (rc == CAI_OK) {
    char token[64];

    snprintf(token, sizeof(token), "%lld", impl->next_id);
    rc = cai_mcp_write_cstr(spool, token, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, "}", error);
  }
  return rc;
}

static int
cai_mcp_resource_read_request(cai_mcp_streamable_http_client_impl *impl,
                              const char *uri, lonejson_spooled *spool,
                              size_t *out_len, cai_error *error) {
  int rc;

  if (uri == NULL || uri[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP resource URI is required");
  }
  CAI_LJ->spooled_init(CAI_LJ, spool);
  rc = cai_mcp_request_begin(impl, spool, ++impl->next_id, "resources/read",
                             error);
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, ",\"params\":{\"uri\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(spool, uri, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_progress_meta(impl, spool, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, "}}", error);
  }
  if (out_len != NULL) {
    *out_len = spool->size_fn(spool);
  }
  if (rc != CAI_OK) {
    spool->cleanup(spool);
  }
  return rc;
}

static int
cai_mcp_resource_subscription_request(cai_mcp_streamable_http_client_impl *impl,
                                      const char *method, const char *uri,
                                      lonejson_spooled *spool, size_t *out_len,
                                      cai_error *error) {
  int rc;

  if (uri == NULL || uri[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP resource URI is required");
  }
  CAI_LJ->spooled_init(CAI_LJ, spool);
  rc = cai_mcp_request_begin(impl, spool, ++impl->next_id, method, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, ",\"params\":{\"uri\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(spool, uri, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, "}}", error);
  }
  if (out_len != NULL) {
    *out_len = spool->size_fn(spool);
  }
  if (rc != CAI_OK) {
    spool->cleanup(spool);
  }
  return rc;
}

static int cai_mcp_prompt_get_request(cai_mcp_streamable_http_client_impl *impl,
                                      const char *name,
                                      lonejson_spooled *arguments_json,
                                      lonejson_spooled *spool, size_t *out_len,
                                      cai_error *error) {
  lonejson_error json_error;
  lonejson_writer writer;
  lonejson_status status;
  int rc;

  if (name == NULL || name[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP prompt name is required");
  }
  if (arguments_json != NULL) {
    rc = cai_mcp_spooled_root_is(
        arguments_json, '{', "MCP prompt arguments must be an object", error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  CAI_LJ->spooled_init(CAI_LJ, spool);
  rc =
      cai_mcp_request_begin(impl, spool, ++impl->next_id, "prompts/get", error);
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, ",\"params\":{\"name\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(spool, name, error);
  }
  if (rc == CAI_OK && arguments_json != NULL) {
    rc = cai_mcp_write_cstr(spool, ",\"arguments\":", error);
    if (rc == CAI_OK) {
      lonejson_error_init(&json_error);
      status = CAI_LJ->writer_init_sink(CAI_LJ, &writer, cai_mcp_spool_sink,
                                        spool, &json_error);
      if (status == LONEJSON_STATUS_OK) {
        status =
            writer.json_value_spooled(&writer, arguments_json, &json_error);
      }
      if (writer.cleanup != NULL) {
        writer.cleanup(&writer);
      }
      if (status != LONEJSON_STATUS_OK) {
        rc = cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                  "failed to write MCP prompt arguments",
                                  json_error.message);
      }
    }
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_progress_meta(impl, spool, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, "}}", error);
  }
  if (out_len != NULL) {
    *out_len = spool->size_fn(spool);
  }
  if (rc != CAI_OK) {
    spool->cleanup(spool);
  }
  return rc;
}

static int cai_mcp_completion_request(
    cai_mcp_streamable_http_client_impl *impl, const char *ref_type,
    const char *ref_value, const char *argument_name,
    const char *argument_value, lonejson_spooled *context_arguments_json,
    lonejson_spooled *spool, size_t *out_len, cai_error *error) {
  lonejson_error json_error;
  lonejson_writer writer;
  lonejson_status status;
  int rc;

  if (ref_type == NULL || ref_type[0] == '\0' || ref_value == NULL ||
      ref_value[0] == '\0' || argument_name == NULL ||
      argument_name[0] == '\0') {
    return cai_set_error(
        error, CAI_ERR_INVALID,
        "MCP completion reference and argument name are required");
  }
  if (strcmp(ref_type, "ref/prompt") != 0 &&
      strcmp(ref_type, "ref/resource") != 0) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP completion reference type is invalid");
  }
  if (context_arguments_json != NULL) {
    rc = cai_mcp_spooled_root_is(
        context_arguments_json, '{',
        "MCP completion context arguments must be an object", error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  CAI_LJ->spooled_init(CAI_LJ, spool);
  rc = cai_mcp_request_begin(impl, spool, ++impl->next_id,
                             "completion/complete", error);
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, ",\"params\":{\"ref\":{\"type\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(spool, ref_type, error);
  }
  if (rc == CAI_OK) {
    rc = strcmp(ref_type, "ref/resource") == 0
             ? cai_mcp_write_cstr(spool, ",\"uri\":", error)
             : cai_mcp_write_cstr(spool, ",\"name\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(spool, ref_value, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, "},\"argument\":{\"name\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(spool, argument_name, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, ",\"value\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(
        spool, argument_value != NULL ? argument_value : "", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, "}", error);
  }
  if (rc == CAI_OK && context_arguments_json != NULL) {
    rc = cai_mcp_write_cstr(spool, ",\"context\":{\"arguments\":", error);
    if (rc == CAI_OK) {
      lonejson_error_init(&json_error);
      status = CAI_LJ->writer_init_sink(CAI_LJ, &writer, cai_mcp_spool_sink,
                                        spool, &json_error);
      if (status == LONEJSON_STATUS_OK) {
        status = writer.json_value_spooled(&writer, context_arguments_json,
                                           &json_error);
      }
      if (writer.cleanup != NULL) {
        writer.cleanup(&writer);
      }
      if (status != LONEJSON_STATUS_OK) {
        rc = cai_set_error_detail(
            error, CAI_ERR_PROTOCOL,
            "failed to write MCP completion context arguments",
            json_error.message);
      }
    }
    if (rc == CAI_OK) {
      rc = cai_mcp_write_cstr(spool, "}", error);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_progress_meta(impl, spool, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, "}}", error);
  }
  if (out_len != NULL) {
    *out_len = spool->size_fn(spool);
  }
  if (rc != CAI_OK) {
    spool->cleanup(spool);
  }
  return rc;
}

static int
cai_mcp_logging_set_level_request(cai_mcp_streamable_http_client_impl *impl,
                                  const char *level, lonejson_spooled *spool,
                                  size_t *out_len, cai_error *error) {
  int rc;

  if (!cai_mcp_log_level_valid(level)) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP log level must be a standard syslog severity");
  }
  CAI_LJ->spooled_init(CAI_LJ, spool);
  rc = cai_mcp_request_begin(impl, spool, ++impl->next_id, "logging/setLevel",
                             error);
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, ",\"params\":{\"level\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(spool, level, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, "}}", error);
  }
  if (out_len != NULL) {
    *out_len = spool->size_fn(spool);
  }
  if (rc != CAI_OK) {
    spool->cleanup(spool);
  }
  return rc;
}

static int cai_mcp_call_request(cai_mcp_streamable_http_client_impl *impl,
                                const char *name,
                                lonejson_spooled *arguments_json,
                                int task_augmented, long long task_ttl_ms,
                                lonejson_spooled *spool, size_t *out_len,
                                cai_error *error) {
  lonejson_error json_error;
  lonejson_writer writer;
  lonejson_status status;
  char ttl_buf[64];
  int rc;

  if (name == NULL || name[0] == '\0' || arguments_json == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP tool name and arguments are required");
  }
  rc = cai_mcp_spooled_root_is(arguments_json, '{',
                               "MCP tool arguments must be an object", error);
  if (rc != CAI_OK) {
    return rc;
  }
  CAI_LJ->spooled_init(CAI_LJ, spool);
  rc = cai_mcp_request_begin(impl, spool, ++impl->next_id, "tools/call", error);
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, ",\"params\":{\"name\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(spool, name, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, ",\"arguments\":", error);
  }
  if (rc == CAI_OK) {
    lonejson_error_init(&json_error);
    status = CAI_LJ->writer_init_sink(CAI_LJ, &writer, cai_mcp_spool_sink,
                                      spool, &json_error);
    if (status == LONEJSON_STATUS_OK) {
      status = writer.json_value_spooled(&writer, arguments_json, &json_error);
    }
    if (writer.cleanup != NULL) {
      writer.cleanup(&writer);
    }
    if (status != LONEJSON_STATUS_OK) {
      rc = cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to write MCP tool arguments",
                                json_error.message);
    }
  }
  if (rc == CAI_OK && task_augmented) {
    rc = cai_mcp_write_cstr(spool, ",\"task\":{", error);
    if (rc == CAI_OK && task_ttl_ms >= 0LL) {
      snprintf(ttl_buf, sizeof(ttl_buf), "%lld", task_ttl_ms);
      rc = cai_mcp_write_cstr(spool, "\"ttl\":", error);
      if (rc == CAI_OK) {
        rc = cai_mcp_write_cstr(spool, ttl_buf, error);
      }
    }
    if (rc == CAI_OK) {
      rc = cai_mcp_write_cstr(spool, "}", error);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_progress_meta(impl, spool, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, "}}", error);
  }
  if (out_len != NULL) {
    *out_len = spool->size_fn(spool);
  }
  if (rc != CAI_OK) {
    spool->cleanup(spool);
  }
  return rc;
}

static int
cai_mcp_parse_initialize_response(cai_mcp_streamable_http_client_impl *impl,
                                  const cai_mcp_http_response_capture *response,
                                  cai_error *error) {
  cai_mcp_initialize_response_doc doc;
  lonejson_spooled json_body;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  int has_error;
  int result_is_object;
  int rc;

  rc = cai_mcp_response_json_body(response, "MCP initialize response",
                                  &json_body, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_mcp_response_has_jsonrpc_error(&json_body, &has_error, error);
  if (rc != CAI_OK) {
    json_body.cleanup(&json_body);
    return rc;
  }
  if (!has_error) {
    rc = cai_mcp_jsonrpc_response_result_root_is_object(
        &json_body, &result_is_object, error);
    if (rc != CAI_OK) {
      json_body.cleanup(&json_body);
      return rc;
    }
    if (!result_is_object) {
      json_body.cleanup(&json_body);
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP result must be an object");
    }
  }
  memset(&doc, 0, sizeof(doc));
  reader.cursor = json_body;
  lonejson_error_init(&json_error);
  if (reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_initialize_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to rewind MCP response",
                                  &json_error);
  }
  status = CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_initialize_response_map, &doc,
                                cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_initialize_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to parse MCP initialize",
                                  &json_error);
  }
  if (has_error) {
    rc = cai_mcp_set_rpc_error(error, &doc.error_doc);
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_initialize_response_map, &doc);
    json_body.cleanup(&json_body);
    return rc;
  }
  if (doc.result.protocol_version == NULL ||
      strcmp(doc.result.protocol_version, impl->protocol_version) != 0) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_initialize_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP server negotiated unsupported protocol version");
  }
  if (!cai_mcp_json_value_is_object(&doc.result.capabilities, error)) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_initialize_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP initialize capabilities must be an object");
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_initialize_response_map, &doc);
  json_body.cleanup(&json_body);
  return CAI_OK;
}

static void cai_mcp_client_tool_impl_cleanup(const cai_allocator *allocator,
                                             cai_mcp_client_tool_impl *tool) {
  if (tool != NULL) {
    cai_free_mem(allocator, tool->name);
    cai_free_mem(allocator, tool->title);
    cai_free_mem(allocator, tool->description);
    cai_free_mem(allocator, tool->input_schema_json);
    cai_free_mem(allocator, tool->output_schema_json);
    cai_free_mem(allocator, tool->annotations_json);
    cai_free_mem(allocator, tool->icons_json);
    cai_free_mem(allocator, tool->execution_json);
    memset(tool, 0, sizeof(*tool));
  }
}

static void
cai_mcp_client_resource_impl_cleanup(const cai_allocator *allocator,
                                     cai_mcp_client_resource_impl *resource) {
  if (resource != NULL) {
    cai_free_mem(allocator, resource->uri);
    cai_free_mem(allocator, resource->name);
    cai_free_mem(allocator, resource->title);
    cai_free_mem(allocator, resource->description);
    cai_free_mem(allocator, resource->mime_type);
    cai_free_mem(allocator, resource->icons_json);
    cai_free_mem(allocator, resource->annotations_json);
    memset(resource, 0, sizeof(*resource));
  }
}

static void cai_mcp_client_resource_template_impl_cleanup(
    const cai_allocator *allocator,
    cai_mcp_client_resource_template_impl *resource_template) {
  if (resource_template != NULL) {
    cai_free_mem(allocator, resource_template->uri_template);
    cai_free_mem(allocator, resource_template->name);
    cai_free_mem(allocator, resource_template->title);
    cai_free_mem(allocator, resource_template->description);
    cai_free_mem(allocator, resource_template->mime_type);
    cai_free_mem(allocator, resource_template->icons_json);
    cai_free_mem(allocator, resource_template->annotations_json);
    memset(resource_template, 0, sizeof(*resource_template));
  }
}

static void
cai_mcp_client_prompt_impl_cleanup(const cai_allocator *allocator,
                                   cai_mcp_client_prompt_impl *prompt) {
  if (prompt != NULL) {
    cai_free_mem(allocator, prompt->name);
    cai_free_mem(allocator, prompt->title);
    cai_free_mem(allocator, prompt->description);
    cai_free_mem(allocator, prompt->arguments_json);
    cai_free_mem(allocator, prompt->icons_json);
    memset(prompt, 0, sizeof(*prompt));
  }
}

static void
cai_mcp_client_clear_tools(cai_mcp_streamable_http_client_impl *impl) {
  size_t i;

  if (impl == NULL) {
    return;
  }
  for (i = 0U; i < impl->tool_count; i++) {
    cai_mcp_client_tool_impl_cleanup(&impl->allocator, &impl->tools[i]);
  }
  impl->tool_count = 0U;
}

static void
cai_mcp_client_clear_resources(cai_mcp_streamable_http_client_impl *impl) {
  size_t i;

  if (impl == NULL) {
    return;
  }
  for (i = 0U; i < impl->resource_count; i++) {
    cai_mcp_client_resource_impl_cleanup(&impl->allocator, &impl->resources[i]);
  }
  impl->resource_count = 0U;
}

static void cai_mcp_client_clear_resource_templates(
    cai_mcp_streamable_http_client_impl *impl) {
  size_t i;

  if (impl == NULL) {
    return;
  }
  for (i = 0U; i < impl->resource_template_count; i++) {
    cai_mcp_client_resource_template_impl_cleanup(&impl->allocator,
                                                  &impl->resource_templates[i]);
  }
  impl->resource_template_count = 0U;
}

static void
cai_mcp_client_clear_prompts(cai_mcp_streamable_http_client_impl *impl) {
  size_t i;

  if (impl == NULL) {
    return;
  }
  for (i = 0U; i < impl->prompt_count; i++) {
    cai_mcp_client_prompt_impl_cleanup(&impl->allocator, &impl->prompts[i]);
  }
  impl->prompt_count = 0U;
}

static int
cai_mcp_client_reserve_tools(cai_mcp_streamable_http_client_impl *impl,
                             size_t count, cai_error *error) {
  cai_mcp_client_tool_impl *tools;

  if (count <= impl->tool_capacity) {
    return CAI_OK;
  }
  tools = (cai_mcp_client_tool_impl *)cai_realloc_mem(
      &impl->allocator, impl->tools, count * sizeof(*tools));
  if (tools == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate MCP tool cache");
  }
  memset(tools + impl->tool_capacity, 0,
         (count - impl->tool_capacity) * sizeof(*tools));
  impl->tools = tools;
  impl->tool_capacity = count;
  return CAI_OK;
}

static int
cai_mcp_client_reserve_resources(cai_mcp_streamable_http_client_impl *impl,
                                 size_t count, cai_error *error) {
  cai_mcp_client_resource_impl *resources;

  if (count <= impl->resource_capacity) {
    return CAI_OK;
  }
  resources = (cai_mcp_client_resource_impl *)cai_realloc_mem(
      &impl->allocator, impl->resources, count * sizeof(*resources));
  if (resources == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate MCP resource cache");
  }
  memset(resources + impl->resource_capacity, 0,
         (count - impl->resource_capacity) * sizeof(*resources));
  impl->resources = resources;
  impl->resource_capacity = count;
  return CAI_OK;
}

static int cai_mcp_client_reserve_resource_templates(
    cai_mcp_streamable_http_client_impl *impl, size_t count, cai_error *error) {
  cai_mcp_client_resource_template_impl *resource_templates;

  if (count <= impl->resource_template_capacity) {
    return CAI_OK;
  }
  resource_templates = (cai_mcp_client_resource_template_impl *)cai_realloc_mem(
      &impl->allocator, impl->resource_templates,
      count * sizeof(*resource_templates));
  if (resource_templates == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate MCP resource template cache");
  }
  memset(resource_templates + impl->resource_template_capacity, 0,
         (count - impl->resource_template_capacity) *
             sizeof(*resource_templates));
  impl->resource_templates = resource_templates;
  impl->resource_template_capacity = count;
  return CAI_OK;
}

static int
cai_mcp_client_reserve_prompts(cai_mcp_streamable_http_client_impl *impl,
                               size_t count, cai_error *error) {
  cai_mcp_client_prompt_impl *prompts;

  if (count <= impl->prompt_capacity) {
    return CAI_OK;
  }
  prompts = (cai_mcp_client_prompt_impl *)cai_realloc_mem(
      &impl->allocator, impl->prompts, count * sizeof(*prompts));
  if (prompts == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate MCP prompt cache");
  }
  memset(prompts + impl->prompt_capacity, 0,
         (count - impl->prompt_capacity) * sizeof(*prompts));
  impl->prompts = prompts;
  impl->prompt_capacity = count;
  return CAI_OK;
}

static char *cai_mcp_json_value_to_cstr(const lonejson_json_value *value,
                                        cai_error *error) {
  cai_buffer_builder builder;
  lonejson_error json_error;

  memset(&builder, 0, sizeof(builder));
  lonejson_error_init(&json_error);
  if (value == NULL ||
      value->methods->write_to_sink(value, cai_mcp_buffer_sink, &builder,
                                    &json_error) != LONEJSON_STATUS_OK) {
    cai_free_mem(NULL, builder.data);
    return NULL;
  }
  if (cai_buffer_append(&builder, "", 1U, error) != CAI_OK) {
    cai_free_mem(NULL, builder.data);
    return NULL;
  }
  return builder.data;
}

static int cai_mcp_json_value_is_object(const lonejson_json_value *value,
                                        cai_error *error) {
  char *text;
  const char *cursor;
  int is_object;

  text = cai_mcp_json_value_to_cstr(value, error);
  if (text == NULL) {
    return 0;
  }
  cursor = text;
  while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' ||
         *cursor == '\n') {
    cursor++;
  }
  is_object = *cursor == '{';
  cai_free_mem(NULL, text);
  return is_object;
}

static int cai_mcp_json_value_root_is(const lonejson_json_value *value,
                                      char root, cai_error *error) {
  char *text;
  const char *cursor;
  int matches;

  if (value == NULL || value->kind == LONEJSON_JSON_VALUE_NULL) {
    return 1;
  }
  text = cai_mcp_json_value_to_cstr(value, error);
  if (text == NULL) {
    return 0;
  }
  cursor = text;
  while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' ||
         *cursor == '\n') {
    cursor++;
  }
  matches = *cursor == root;
  cai_free_mem(NULL, text);
  return matches;
}

static int
cai_mcp_optional_json_object_is_valid(const lonejson_json_value *value,
                                      cai_error *error) {
  return cai_mcp_json_value_root_is(value, '{', error);
}

static int
cai_mcp_optional_json_array_is_valid(const lonejson_json_value *value,
                                     cai_error *error) {
  return cai_mcp_json_value_root_is(value, '[', error);
}

static int cai_mcp_jsonrpc_id_text_is_valid(const char *id) {
  size_t i;

  if (id == NULL || id[0] == '\0') {
    return 0;
  }
  if (id[0] == '"') {
    return 1;
  }
  i = id[0] == '-' ? 1U : 0U;
  if (id[i] == '\0') {
    return 0;
  }
  for (; id[i] != '\0'; i++) {
    if (id[i] < '0' || id[i] > '9') {
      return 0;
    }
  }
  return 1;
}

static int cai_mcp_validate_jsonrpc_id_value(const lonejson_json_value *id,
                                             cai_error *error) {
  char *text;
  int valid;

  if (id == NULL || id->kind == LONEJSON_JSON_VALUE_NULL) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP JSON-RPC was missing id");
  }
  text = cai_mcp_json_value_to_cstr(id, error);
  if (text == NULL) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to read MCP JSON-RPC id");
  }
  valid = cai_mcp_jsonrpc_id_text_is_valid(text);
  cai_free_mem(NULL, text);
  if (!valid) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP JSON-RPC id must be a string or integer");
  }
  return CAI_OK;
}

static int cai_mcp_parse_jsonrpc_id(const lonejson_spooled *json,
                                    cai_mcp_jsonrpc_id_doc *doc,
                                    const char *operation, cai_error *error) {
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  int rc;

  if (json == NULL || doc == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP JSON-RPC is required");
  }
  memset(doc, 0, sizeof(*doc));
  reader.cursor = *json;
  lonejson_error_init(&json_error);
  if (reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    return cai_mcp_set_json_error(error, "failed to rewind MCP JSON-RPC",
                                  &json_error);
  }
  status = CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_jsonrpc_id_map, doc,
                                cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_jsonrpc_id_map, doc);
    return cai_mcp_set_json_error(error, operation, &json_error);
  }
  if (doc->jsonrpc == NULL || strcmp(doc->jsonrpc, "2.0") != 0) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_jsonrpc_id_map, doc);
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP JSON-RPC version must be 2.0");
  }
  if (doc->id.kind == LONEJSON_JSON_VALUE_NULL) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_jsonrpc_id_map, doc);
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP JSON-RPC was missing id");
  }
  rc = cai_mcp_validate_jsonrpc_id_value(&doc->id, error);
  if (rc != CAI_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_jsonrpc_id_map, doc);
    return rc;
  }
  return CAI_OK;
}

static int cai_mcp_validate_roots_result(lonejson_spooled *json,
                                         cai_error *error) {
  cai_mcp_roots_list_result_doc doc;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;

  if (json == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP roots result is required");
  }
  memset(&doc, 0, sizeof(doc));
  reader.cursor = *json;
  lonejson_error_init(&json_error);
  if (reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    return cai_mcp_set_json_error(error, "failed to rewind MCP roots result",
                                  &json_error);
  }
  status = CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_roots_list_result_map, &doc,
                                cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_roots_list_result_map, &doc);
    return cai_mcp_set_json_error(error, "failed to parse MCP roots result",
                                  &json_error);
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_roots_list_result_map, &doc);
  return CAI_OK;
}

static int cai_mcp_validate_sampling_result(lonejson_spooled *json,
                                            cai_error *error) {
  cai_mcp_sampling_result_doc doc;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  int rc;

  if (json == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP sampling result is required");
  }
  memset(&doc, 0, sizeof(doc));
  reader.cursor = *json;
  lonejson_error_init(&json_error);
  if (reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    return cai_mcp_set_json_error(error, "failed to rewind MCP sampling result",
                                  &json_error);
  }
  status = CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_sampling_result_map, &doc,
                                cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_sampling_result_map, &doc);
    return cai_mcp_set_json_error(error, "failed to parse MCP sampling result",
                                  &json_error);
  }
  if (strcmp(doc.role, "assistant") != 0) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_sampling_result_map, &doc);
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP sampling result role must be assistant");
  }
  rc = cai_mcp_sampling_content_value_validate(&doc.content, error);
  if (rc != CAI_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_sampling_result_map, &doc);
    return rc;
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_sampling_result_map, &doc);
  return CAI_OK;
}

static int cai_mcp_elicitation_action_is_valid(const char *action) {
  return action != NULL &&
         (strcmp(action, "accept") == 0 || strcmp(action, "decline") == 0 ||
          strcmp(action, "cancel") == 0);
}

static int cai_mcp_validate_elicitation_result(lonejson_spooled *json,
                                               cai_error *error) {
  cai_mcp_elicitation_result_doc doc;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;

  if (json == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP elicitation result is required");
  }
  memset(&doc, 0, sizeof(doc));
  reader.cursor = *json;
  lonejson_error_init(&json_error);
  if (reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    return cai_mcp_set_json_error(
        error, "failed to rewind MCP elicitation result", &json_error);
  }
  status = CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_elicitation_result_map, &doc,
                                cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_elicitation_result_map, &doc);
    return cai_mcp_set_json_error(
        error, "failed to parse MCP elicitation result", &json_error);
  }
  if (!cai_mcp_elicitation_action_is_valid(doc.action)) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_elicitation_result_map, &doc);
    return cai_set_error(
        error, CAI_ERR_PROTOCOL,
        "MCP elicitation result action must be accept, decline, or cancel");
  }
  if (doc.content.kind != LONEJSON_JSON_VALUE_NULL &&
      !cai_mcp_json_value_root_is(&doc.content, '{', error)) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_elicitation_result_map, &doc);
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP elicitation result content must be an object");
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_elicitation_result_map, &doc);
  return CAI_OK;
}

typedef struct cai_mcp_json_scan {
  cai_mcp_spooled_reader reader;
  unsigned char buffer[512];
  size_t offset;
  size_t length;
  int pushed;
  int pushed_ch;
} cai_mcp_json_scan;

static int cai_mcp_json_scan_init(cai_mcp_json_scan *scan,
                                  const lonejson_spooled *json,
                                  cai_error *error) {
  lonejson_error json_error;

  if (scan == NULL || json == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP JSON is required");
  }
  memset(scan, 0, sizeof(*scan));
  scan->reader.cursor = *json;
  lonejson_error_init(&json_error);
  if (scan->reader.cursor.rewind(&scan->reader.cursor, &json_error) !=
      LONEJSON_STATUS_OK) {
    return cai_mcp_set_json_error(error, "failed to rewind MCP JSON-RPC",
                                  &json_error);
  }
  return CAI_OK;
}

static int cai_mcp_json_scan_get(cai_mcp_json_scan *scan, int *ch,
                                 cai_error *error) {
  lonejson_read_result chunk;

  if (scan->pushed) {
    scan->pushed = 0;
    *ch = scan->pushed_ch;
    return 1;
  }
  if (scan->offset >= scan->length) {
    chunk = scan->reader.cursor.read(&scan->reader.cursor, scan->buffer,
                                     sizeof(scan->buffer));
    if (chunk.error_code != 0) {
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "failed to read MCP JSON-RPC response");
    }
    if (chunk.bytes_read == 0U) {
      *ch = 0;
      return 0;
    }
    scan->offset = 0U;
    scan->length = chunk.bytes_read;
  }
  *ch = scan->buffer[scan->offset++];
  return 1;
}

static void cai_mcp_json_scan_unget(cai_mcp_json_scan *scan, int ch) {
  scan->pushed = 1;
  scan->pushed_ch = ch;
}

static int cai_mcp_json_scan_skip_ws(cai_mcp_json_scan *scan, int *ch,
                                     cai_error *error) {
  int rc;

  do {
    rc = cai_mcp_json_scan_get(scan, ch, error);
    if (rc <= 0) {
      return rc;
    }
  } while (*ch == ' ' || *ch == '\t' || *ch == '\r' || *ch == '\n');
  return 1;
}

static int cai_mcp_spooled_root_is(const lonejson_spooled *json, int root,
                                   const char *message, cai_error *error) {
  cai_mcp_json_scan scan;
  int ch;
  int rc;

  rc = cai_mcp_json_scan_init(&scan, json, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_mcp_json_scan_skip_ws(&scan, &ch, error);
  if (rc <= 0 || ch != root) {
    return cai_set_error(error, CAI_ERR_PROTOCOL, message);
  }
  return CAI_OK;
}

static int cai_mcp_json_hex_value(int ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return 10 + ch - 'a';
  }
  if (ch >= 'A' && ch <= 'F') {
    return 10 + ch - 'A';
  }
  return -1;
}

static int cai_mcp_json_scan_string(cai_mcp_json_scan *scan, char *out,
                                    size_t out_size, cai_error *error) {
  size_t len;
  int ch;
  int rc;

  len = 0U;
  for (;;) {
    rc = cai_mcp_json_scan_get(scan, &ch, error);
    if (rc <= 0) {
      return rc;
    }
    if (ch == '"') {
      if (out_size != 0U) {
        out[len < out_size ? len : out_size - 1U] = '\0';
      }
      return 1;
    }
    if ((unsigned int)ch < 0x20U) {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "failed to parse MCP JSON-RPC response");
    }
    if (ch == '\\') {
      int hex[4];
      size_t i;

      rc = cai_mcp_json_scan_get(scan, &ch, error);
      if (rc <= 0) {
        return rc;
      }
      if (ch == 'u') {
        unsigned int codepoint;

        codepoint = 0U;
        for (i = 0U; i < 4U; i++) {
          rc = cai_mcp_json_scan_get(scan, &ch, error);
          if (rc <= 0) {
            return rc;
          }
          hex[i] = cai_mcp_json_hex_value(ch);
          if (hex[i] < 0) {
            return cai_set_error(error, CAI_ERR_PROTOCOL,
                                 "failed to parse MCP JSON-RPC response");
          }
          codepoint = (codepoint << 4U) | (unsigned int)hex[i];
        }
        ch = codepoint <= 0x7FU ? (int)codepoint : '?';
      } else if (ch == '"' || ch == '\\' || ch == '/') {
      } else if (ch == 'b') {
        ch = '\b';
      } else if (ch == 'f') {
        ch = '\f';
      } else if (ch == 'n') {
        ch = '\n';
      } else if (ch == 'r') {
        ch = '\r';
      } else if (ch == 't') {
        ch = '\t';
      } else {
        return cai_set_error(error, CAI_ERR_PROTOCOL,
                             "failed to parse MCP JSON-RPC response");
      }
    }
    if (out_size > 0U && len + 1U < out_size) {
      out[len] = (char)ch;
    }
    len++;
  }
}

static int cai_mcp_json_scan_skip_string(cai_mcp_json_scan *scan,
                                         cai_error *error) {
  return cai_mcp_json_scan_string(scan, NULL, 0U, error);
}

static int cai_mcp_json_scan_skip_value(cai_mcp_json_scan *scan,
                                        cai_error *error) {
  int stack[64];
  size_t depth;
  int ch;
  int rc;

  rc = cai_mcp_json_scan_skip_ws(scan, &ch, error);
  if (rc <= 0) {
    return rc;
  }
  if (ch == '"') {
    return cai_mcp_json_scan_skip_string(scan, error);
  }
  if (ch != '{' && ch != '[') {
    while ((rc = cai_mcp_json_scan_get(scan, &ch, error)) > 0) {
      if (ch == ',' || ch == '}' || ch == ']' || ch == ' ' || ch == '\t' ||
          ch == '\r' || ch == '\n') {
        cai_mcp_json_scan_unget(scan, ch);
        return 1;
      }
    }
    return rc;
  }
  stack[0] = ch;
  depth = 1U;
  while (depth != 0U) {
    rc = cai_mcp_json_scan_get(scan, &ch, error);
    if (rc <= 0) {
      return rc;
    }
    if (ch == '"') {
      rc = cai_mcp_json_scan_skip_string(scan, error);
      if (rc <= 0) {
        return rc;
      }
    } else if (ch == '{' || ch == '[') {
      if (depth >= sizeof(stack) / sizeof(stack[0])) {
        return cai_set_error(error, CAI_ERR_PROTOCOL,
                             "failed to parse MCP JSON-RPC response");
      }
      stack[depth++] = ch;
    } else if (ch == '}' || ch == ']') {
      if (depth == 0U || (ch == '}' && stack[depth - 1U] != '{') ||
          (ch == ']' && stack[depth - 1U] != '[')) {
        return cai_set_error(error, CAI_ERR_PROTOCOL,
                             "failed to parse MCP JSON-RPC response");
      }
      depth--;
    }
  }
  return 1;
}

static int
cai_mcp_jsonrpc_response_result_error_presence(const lonejson_spooled *json,
                                               int *has_result, int *has_error,
                                               cai_error *error) {
  cai_mcp_json_scan scan;
  char key[32];
  int ch;
  int rc;

  *has_result = 0;
  *has_error = 0;
  rc = cai_mcp_json_scan_init(&scan, json, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_mcp_json_scan_skip_ws(&scan, &ch, error);
  if (rc <= 0 || ch != '{') {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "failed to parse MCP JSON-RPC response");
  }
  rc = cai_mcp_json_scan_skip_ws(&scan, &ch, error);
  if (rc <= 0) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "failed to parse MCP JSON-RPC response");
  }
  if (ch == '}') {
    return CAI_OK;
  }
  cai_mcp_json_scan_unget(&scan, ch);
  for (;;) {
    rc = cai_mcp_json_scan_skip_ws(&scan, &ch, error);
    if (rc <= 0 || ch != '"') {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "failed to parse MCP JSON-RPC response");
    }
    rc = cai_mcp_json_scan_string(&scan, key, sizeof(key), error);
    if (rc <= 0) {
      return rc == 0 ? cai_set_error(error, CAI_ERR_PROTOCOL,
                                     "failed to parse MCP JSON-RPC response")
                     : rc;
    }
    rc = cai_mcp_json_scan_skip_ws(&scan, &ch, error);
    if (rc <= 0 || ch != ':') {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "failed to parse MCP JSON-RPC response");
    }
    if (strcmp(key, "result") == 0) {
      *has_result = 1;
    } else if (strcmp(key, "error") == 0) {
      *has_error = 1;
    }
    rc = cai_mcp_json_scan_skip_value(&scan, error);
    if (rc <= 0) {
      return rc == 0 ? cai_set_error(error, CAI_ERR_PROTOCOL,
                                     "failed to parse MCP JSON-RPC response")
                     : rc;
    }
    rc = cai_mcp_json_scan_skip_ws(&scan, &ch, error);
    if (rc <= 0) {
      return rc == 0 ? cai_set_error(error, CAI_ERR_PROTOCOL,
                                     "failed to parse MCP JSON-RPC response")
                     : rc;
    }
    if (ch == '}') {
      return CAI_OK;
    }
    if (ch != ',') {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "failed to parse MCP JSON-RPC response");
    }
  }
}

static int cai_mcp_elicitation_property_type_is_valid(const char *type) {
  return type != NULL &&
         (strcmp(type, "string") == 0 || strcmp(type, "number") == 0 ||
          strcmp(type, "integer") == 0 || strcmp(type, "boolean") == 0 ||
          strcmp(type, "array") == 0);
}

static int cai_mcp_validate_elicitation_schema_property(cai_mcp_json_scan *scan,
                                                        cai_error *error) {
  char key[32];
  char type[16];
  int has_type;
  int ch;
  int rc;

  rc = cai_mcp_json_scan_skip_ws(scan, &ch, error);
  if (rc <= 0 || ch != '{') {
    return cai_set_error(
        error, CAI_ERR_PROTOCOL,
        "MCP elicitation requestedSchema property must be an object");
  }
  rc = cai_mcp_json_scan_skip_ws(scan, &ch, error);
  if (rc <= 0) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "failed to parse MCP elicitation requestedSchema");
  }
  has_type = 0;
  if (ch == '}') {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP elicitation requestedSchema property type is "
                         "required");
  }
  cai_mcp_json_scan_unget(scan, ch);
  for (;;) {
    rc = cai_mcp_json_scan_skip_ws(scan, &ch, error);
    if (rc <= 0 || ch != '"') {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "failed to parse MCP elicitation requestedSchema");
    }
    rc = cai_mcp_json_scan_string(scan, key, sizeof(key), error);
    if (rc <= 0) {
      return rc == 0
                 ? cai_set_error(
                       error, CAI_ERR_PROTOCOL,
                       "failed to parse MCP elicitation requestedSchema")
                 : rc;
    }
    rc = cai_mcp_json_scan_skip_ws(scan, &ch, error);
    if (rc <= 0 || ch != ':') {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "failed to parse MCP elicitation requestedSchema");
    }
    if (strcmp(key, "type") == 0) {
      rc = cai_mcp_json_scan_skip_ws(scan, &ch, error);
      if (rc <= 0 || ch != '"') {
        return cai_set_error(error, CAI_ERR_PROTOCOL,
                             "MCP elicitation requestedSchema property type "
                             "must be a string");
      }
      rc = cai_mcp_json_scan_string(scan, type, sizeof(type), error);
      if (rc <= 0) {
        return rc == 0
                   ? cai_set_error(
                         error, CAI_ERR_PROTOCOL,
                         "failed to parse MCP elicitation requestedSchema")
                   : rc;
      }
      if (!cai_mcp_elicitation_property_type_is_valid(type)) {
        return cai_set_error(
            error, CAI_ERR_PROTOCOL,
            "MCP elicitation requestedSchema property type must be primitive");
      }
      has_type = 1;
    } else {
      rc = cai_mcp_json_scan_skip_value(scan, error);
      if (rc <= 0) {
        return rc == 0
                   ? cai_set_error(
                         error, CAI_ERR_PROTOCOL,
                         "failed to parse MCP elicitation requestedSchema")
                   : rc;
      }
    }
    rc = cai_mcp_json_scan_skip_ws(scan, &ch, error);
    if (rc <= 0) {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "failed to parse MCP elicitation requestedSchema");
    }
    if (ch == '}') {
      if (!has_type) {
        return cai_set_error(error, CAI_ERR_PROTOCOL,
                             "MCP elicitation requestedSchema property type is "
                             "required");
      }
      return CAI_OK;
    }
    if (ch != ',') {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "failed to parse MCP elicitation requestedSchema");
    }
  }
}

static int cai_mcp_validate_elicitation_schema_properties(
    const lonejson_json_value *properties, cai_error *error) {
  lonejson_spooled properties_json;
  cai_mcp_json_scan scan;
  int ch;
  int rc;

  if (properties == NULL || properties->kind == LONEJSON_JSON_VALUE_NULL) {
    return cai_set_error(
        error, CAI_ERR_PROTOCOL,
        "MCP elicitation requestedSchema properties are required");
  }
  memset(&properties_json, 0, sizeof(properties_json));
  rc = cai_mcp_json_value_to_spooled(properties, &properties_json, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_mcp_json_scan_init(&scan, &properties_json, error);
  if (rc != CAI_OK) {
    properties_json.cleanup(&properties_json);
    return rc;
  }
  rc = cai_mcp_json_scan_skip_ws(&scan, &ch, error);
  if (rc <= 0 || ch != '{') {
    properties_json.cleanup(&properties_json);
    return cai_set_error(
        error, CAI_ERR_PROTOCOL,
        "MCP elicitation requestedSchema properties must be an object");
  }
  rc = cai_mcp_json_scan_skip_ws(&scan, &ch, error);
  if (rc <= 0) {
    properties_json.cleanup(&properties_json);
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "failed to parse MCP elicitation requestedSchema");
  }
  if (ch == '}') {
    properties_json.cleanup(&properties_json);
    return CAI_OK;
  }
  cai_mcp_json_scan_unget(&scan, ch);
  for (;;) {
    rc = cai_mcp_json_scan_skip_ws(&scan, &ch, error);
    if (rc <= 0 || ch != '"') {
      properties_json.cleanup(&properties_json);
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "failed to parse MCP elicitation requestedSchema");
    }
    rc = cai_mcp_json_scan_skip_string(&scan, error);
    if (rc <= 0) {
      properties_json.cleanup(&properties_json);
      return rc == 0
                 ? cai_set_error(
                       error, CAI_ERR_PROTOCOL,
                       "failed to parse MCP elicitation requestedSchema")
                 : rc;
    }
    rc = cai_mcp_json_scan_skip_ws(&scan, &ch, error);
    if (rc <= 0 || ch != ':') {
      properties_json.cleanup(&properties_json);
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "failed to parse MCP elicitation requestedSchema");
    }
    rc = cai_mcp_validate_elicitation_schema_property(&scan, error);
    if (rc != CAI_OK) {
      properties_json.cleanup(&properties_json);
      return rc;
    }
    rc = cai_mcp_json_scan_skip_ws(&scan, &ch, error);
    if (rc <= 0) {
      properties_json.cleanup(&properties_json);
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "failed to parse MCP elicitation requestedSchema");
    }
    if (ch == '}') {
      properties_json.cleanup(&properties_json);
      return CAI_OK;
    }
    if (ch != ',') {
      properties_json.cleanup(&properties_json);
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "failed to parse MCP elicitation requestedSchema");
    }
  }
}

static int cai_mcp_jsonrpc_top_level_member_presence(
    const lonejson_spooled *json, const char *member, int *present,
    const char *message, cai_error *error) {
  cai_mcp_json_scan scan;
  char key[32];
  int ch;
  int rc;

  if (present == NULL || member == NULL || message == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP JSON-RPC member scan is required");
  }
  *present = 0;
  rc = cai_mcp_json_scan_init(&scan, json, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_mcp_json_scan_skip_ws(&scan, &ch, error);
  if (rc <= 0 || ch != '{') {
    return cai_set_error(error, CAI_ERR_PROTOCOL, message);
  }
  rc = cai_mcp_json_scan_skip_ws(&scan, &ch, error);
  if (rc <= 0) {
    return cai_set_error(error, CAI_ERR_PROTOCOL, message);
  }
  if (ch == '}') {
    return CAI_OK;
  }
  cai_mcp_json_scan_unget(&scan, ch);
  for (;;) {
    rc = cai_mcp_json_scan_skip_ws(&scan, &ch, error);
    if (rc <= 0 || ch != '"') {
      return cai_set_error(error, CAI_ERR_PROTOCOL, message);
    }
    rc = cai_mcp_json_scan_string(&scan, key, sizeof(key), error);
    if (rc <= 0) {
      return rc == 0 ? cai_set_error(error, CAI_ERR_PROTOCOL, message) : rc;
    }
    rc = cai_mcp_json_scan_skip_ws(&scan, &ch, error);
    if (rc <= 0 || ch != ':') {
      return cai_set_error(error, CAI_ERR_PROTOCOL, message);
    }
    if (strcmp(key, member) == 0) {
      *present = 1;
    }
    rc = cai_mcp_json_scan_skip_value(&scan, error);
    if (rc <= 0) {
      return rc == 0 ? cai_set_error(error, CAI_ERR_PROTOCOL, message) : rc;
    }
    rc = cai_mcp_json_scan_skip_ws(&scan, &ch, error);
    if (rc <= 0) {
      return cai_set_error(error, CAI_ERR_PROTOCOL, message);
    }
    if (ch == '}') {
      return CAI_OK;
    }
    if (ch != ',') {
      return cai_set_error(error, CAI_ERR_PROTOCOL, message);
    }
  }
}

static int cai_mcp_jsonrpc_response_result_root_is_object(
    const lonejson_spooled *json, int *is_object, cai_error *error) {
  cai_mcp_json_scan scan;
  char key[32];
  int ch;
  int rc;

  *is_object = 0;
  rc = cai_mcp_json_scan_init(&scan, json, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_mcp_json_scan_skip_ws(&scan, &ch, error);
  if (rc <= 0 || ch != '{') {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "failed to parse MCP JSON-RPC response");
  }
  rc = cai_mcp_json_scan_skip_ws(&scan, &ch, error);
  if (rc <= 0) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "failed to parse MCP JSON-RPC response");
  }
  if (ch == '}') {
    return CAI_OK;
  }
  cai_mcp_json_scan_unget(&scan, ch);
  for (;;) {
    rc = cai_mcp_json_scan_skip_ws(&scan, &ch, error);
    if (rc <= 0 || ch != '"') {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "failed to parse MCP JSON-RPC response");
    }
    rc = cai_mcp_json_scan_string(&scan, key, sizeof(key), error);
    if (rc <= 0) {
      return rc == 0 ? cai_set_error(error, CAI_ERR_PROTOCOL,
                                     "failed to parse MCP JSON-RPC response")
                     : rc;
    }
    rc = cai_mcp_json_scan_skip_ws(&scan, &ch, error);
    if (rc <= 0 || ch != ':') {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "failed to parse MCP JSON-RPC response");
    }
    rc = cai_mcp_json_scan_skip_ws(&scan, &ch, error);
    if (rc <= 0) {
      return rc == 0 ? cai_set_error(error, CAI_ERR_PROTOCOL,
                                     "failed to parse MCP JSON-RPC response")
                     : rc;
    }
    if (strcmp(key, "result") == 0) {
      *is_object = ch == '{';
    }
    cai_mcp_json_scan_unget(&scan, ch);
    rc = cai_mcp_json_scan_skip_value(&scan, error);
    if (rc <= 0) {
      return rc == 0 ? cai_set_error(error, CAI_ERR_PROTOCOL,
                                     "failed to parse MCP JSON-RPC response")
                     : rc;
    }
    if (strcmp(key, "result") == 0) {
      return CAI_OK;
    }
    rc = cai_mcp_json_scan_skip_ws(&scan, &ch, error);
    if (rc <= 0) {
      return rc == 0 ? cai_set_error(error, CAI_ERR_PROTOCOL,
                                     "failed to parse MCP JSON-RPC response")
                     : rc;
    }
    if (ch == '}') {
      return CAI_OK;
    }
    if (ch != ',') {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "failed to parse MCP JSON-RPC response");
    }
  }
}

static int
cai_mcp_validate_response_envelope(const lonejson_spooled *request,
                                   const cai_mcp_http_response_capture *reply,
                                   cai_error *error) {
  cai_mcp_jsonrpc_id_doc request_doc;
  cai_mcp_jsonrpc_id_doc reply_doc;
  char *request_id;
  char *reply_id;
  int has_result;
  int has_error;
  int rc;

  if (request == NULL || reply == NULL || !cai_mcp_response_is_json(reply)) {
    return CAI_OK;
  }
  memset(&request_doc, 0, sizeof(request_doc));
  memset(&reply_doc, 0, sizeof(reply_doc));
  request_id = NULL;
  reply_id = NULL;
  rc = cai_mcp_parse_jsonrpc_id(request, &request_doc,
                                "failed to parse MCP request id", error);
  if (rc == CAI_OK) {
    rc = cai_mcp_parse_jsonrpc_id(&reply->body, &reply_doc,
                                  "failed to parse MCP response id", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_jsonrpc_response_result_error_presence(
        &reply->body, &has_result, &has_error, error);
  }
  if (rc == CAI_OK && has_result == has_error) {
    rc = cai_set_error(
        error, CAI_ERR_PROTOCOL,
        "MCP JSON-RPC response must include exactly one of result or error");
  }
  if (rc == CAI_OK) {
    request_id = cai_mcp_json_value_to_cstr(&request_doc.id, error);
    reply_id = cai_mcp_json_value_to_cstr(&reply_doc.id, error);
    if (request_id == NULL || reply_id == NULL) {
      rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to read MCP JSON-RPC id");
    } else if (strcmp(request_id, reply_id) != 0) {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP JSON-RPC response id did not match request id");
    }
  }
  cai_free_mem(NULL, request_id);
  cai_free_mem(NULL, reply_id);
  if (request_doc.id.methods != NULL) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_jsonrpc_id_map, &request_doc);
  }
  if (reply_doc.id.methods != NULL) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_jsonrpc_id_map, &reply_doc);
  }
  return rc;
}

static char *
cai_mcp_optional_json_value_to_cstr(const cai_allocator *allocator,
                                    const lonejson_json_value *value,
                                    cai_error *error) {
  if (value == NULL || value->kind == LONEJSON_JSON_VALUE_NULL) {
    return cai_strdup(allocator, "null");
  }
  return cai_mcp_json_value_to_cstr(value, error);
}

static char *
cai_mcp_optional_json_array_to_cstr(const cai_allocator *allocator,
                                    const lonejson_json_value *value,
                                    cai_error *error) {
  if (value == NULL || value->kind == LONEJSON_JSON_VALUE_NULL) {
    return cai_strdup(allocator, "[]");
  }
  return cai_mcp_json_value_to_cstr(value, error);
}

static int
cai_mcp_parse_tools_list_response(cai_mcp_streamable_http_client_impl *impl,
                                  const cai_mcp_http_response_capture *response,
                                  char **next_cursor, cai_error *error) {
  cai_mcp_tools_list_response_doc doc;
  lonejson_spooled json_body;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  cai_mcp_list_tool_doc *src_tools;
  size_t base_count;
  size_t i;
  int has_error;
  int rc;

  if (next_cursor != NULL) {
    *next_cursor = NULL;
  }
  rc = cai_mcp_response_json_body(response, "MCP tools/list response",
                                  &json_body, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_mcp_response_has_jsonrpc_error(&json_body, &has_error, error);
  if (rc != CAI_OK) {
    json_body.cleanup(&json_body);
    return rc;
  }
  memset(&doc, 0, sizeof(doc));
  reader.cursor = json_body;
  lonejson_error_init(&json_error);
  if (reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to rewind MCP response",
                                  &json_error);
  }
  status = CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_tools_list_response_map, &doc,
                                cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tools_list_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to parse MCP tools/list",
                                  &json_error);
  }
  if (has_error) {
    rc = cai_mcp_set_rpc_error(error, &doc.error_doc);
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tools_list_response_map, &doc);
    json_body.cleanup(&json_body);
    return rc;
  }
  src_tools = (cai_mcp_list_tool_doc *)doc.result.tools.items;
  for (i = 0U; i < doc.result.tools.count; i++) {
    if (!cai_mcp_json_value_is_object(&src_tools[i].input_schema, error)) {
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tools_list_response_map, &doc);
      json_body.cleanup(&json_body);
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP tool inputSchema must be an object");
    }
    if (!cai_mcp_optional_json_object_is_valid(&src_tools[i].output_schema,
                                               error)) {
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tools_list_response_map, &doc);
      json_body.cleanup(&json_body);
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP tool outputSchema must be an object");
    }
    if (!cai_mcp_optional_json_object_is_valid(&src_tools[i].annotations,
                                               error)) {
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tools_list_response_map, &doc);
      json_body.cleanup(&json_body);
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP tool annotations must be an object");
    }
    if (!cai_mcp_optional_json_array_is_valid(&src_tools[i].icons, error)) {
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tools_list_response_map, &doc);
      json_body.cleanup(&json_body);
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP tool icons must be an array");
    }
    if (!cai_mcp_optional_json_object_is_valid(&src_tools[i].execution,
                                               error)) {
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tools_list_response_map, &doc);
      json_body.cleanup(&json_body);
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP tool execution must be an object");
    }
  }
  base_count = impl->tool_count;
  rc = cai_mcp_client_reserve_tools(impl, base_count + doc.result.tools.count,
                                    error);
  if (rc == CAI_OK) {
    for (i = 0U; i < doc.result.tools.count; i++) {
      cai_mcp_client_tool_impl *dst = &impl->tools[base_count + i];
      dst->name = cai_strdup(&impl->allocator, src_tools[i].name);
      dst->title =
          cai_strdup(&impl->allocator, src_tools[i].title != NULL
                                           ? src_tools[i].title
                                           : (src_tools[i].description != NULL
                                                  ? src_tools[i].description
                                                  : ""));
      dst->description = cai_strdup(
          &impl->allocator,
          src_tools[i].description != NULL
              ? src_tools[i].description
              : (src_tools[i].title != NULL ? src_tools[i].title : ""));
      dst->input_schema_json =
          cai_mcp_json_value_to_cstr(&src_tools[i].input_schema, error);
      dst->output_schema_json = cai_mcp_optional_json_value_to_cstr(
          &impl->allocator, &src_tools[i].output_schema, error);
      dst->annotations_json = cai_mcp_optional_json_value_to_cstr(
          &impl->allocator, &src_tools[i].annotations, error);
      dst->icons_json = cai_mcp_optional_json_array_to_cstr(
          &impl->allocator, &src_tools[i].icons, error);
      dst->execution_json = cai_mcp_optional_json_value_to_cstr(
          &impl->allocator, &src_tools[i].execution, error);
      if (dst->name == NULL || dst->title == NULL || dst->description == NULL ||
          dst->input_schema_json == NULL || dst->output_schema_json == NULL ||
          dst->annotations_json == NULL || dst->icons_json == NULL ||
          dst->execution_json == NULL) {
        rc = error != NULL && error->code != CAI_OK
                 ? error->code
                 : cai_set_error(error, CAI_ERR_NOMEM,
                                 "failed to copy MCP tool metadata");
        cai_mcp_client_tool_impl_cleanup(&impl->allocator, dst);
        break;
      }
      dst->public_tool.name = dst->name;
      dst->public_tool.title = dst->title;
      dst->public_tool.description = dst->description;
      dst->public_tool.input_schema_json = dst->input_schema_json;
      dst->public_tool.output_schema_json = dst->output_schema_json;
      dst->public_tool.annotations_json = dst->annotations_json;
      dst->public_tool.icons_json = dst->icons_json;
      dst->public_tool.execution_json = dst->execution_json;
      impl->tool_count++;
    }
  }
  if (rc == CAI_OK && next_cursor != NULL && doc.result.next_cursor != NULL) {
    *next_cursor = cai_strdup(&impl->allocator, doc.result.next_cursor);
    if (*next_cursor == NULL) {
      rc = cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to copy MCP tools/list cursor");
    }
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tools_list_response_map, &doc);
  if (rc != CAI_OK) {
    cai_mcp_client_clear_tools(impl);
    if (next_cursor != NULL) {
      cai_free_mem(&impl->allocator, *next_cursor);
      *next_cursor = NULL;
    }
  }
  json_body.cleanup(&json_body);
  return rc;
}

static int cai_mcp_parse_resources_list_response(
    cai_mcp_streamable_http_client_impl *impl,
    const cai_mcp_http_response_capture *response, char **next_cursor,
    cai_error *error) {
  cai_mcp_resources_list_response_doc doc;
  lonejson_spooled json_body;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  cai_mcp_list_resource_doc *src_resources;
  size_t base_count;
  size_t i;
  int has_error;
  int rc;

  if (next_cursor != NULL) {
    *next_cursor = NULL;
  }
  rc = cai_mcp_response_json_body(response, "MCP resources/list response",
                                  &json_body, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_mcp_response_has_jsonrpc_error(&json_body, &has_error, error);
  if (rc != CAI_OK) {
    json_body.cleanup(&json_body);
    return rc;
  }
  memset(&doc, 0, sizeof(doc));
  reader.cursor = json_body;
  lonejson_error_init(&json_error);
  if (reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to rewind MCP response",
                                  &json_error);
  }
  status =
      CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_resources_list_response_map, &doc,
                           cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_resources_list_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to parse MCP resources/list",
                                  &json_error);
  }
  if (has_error) {
    rc = cai_mcp_set_rpc_error(error, &doc.error_doc);
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_resources_list_response_map, &doc);
    json_body.cleanup(&json_body);
    return rc;
  }
  src_resources = (cai_mcp_list_resource_doc *)doc.result.resources.items;
  for (i = 0U; i < doc.result.resources.count; i++) {
    if (!cai_mcp_optional_json_array_is_valid(&src_resources[i].icons, error)) {
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_resources_list_response_map, &doc);
      json_body.cleanup(&json_body);
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP resource icons must be an array");
    }
    if (!cai_mcp_optional_json_object_is_valid(&src_resources[i].annotations,
                                               error)) {
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_resources_list_response_map, &doc);
      json_body.cleanup(&json_body);
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP resource annotations must be an object");
    }
    if (src_resources[i].has_size && src_resources[i].size < 0) {
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_resources_list_response_map, &doc);
      json_body.cleanup(&json_body);
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP resource size must be non-negative");
    }
  }
  base_count = impl->resource_count;
  rc = cai_mcp_client_reserve_resources(
      impl, base_count + doc.result.resources.count, error);
  if (rc == CAI_OK) {
    for (i = 0U; i < doc.result.resources.count; i++) {
      cai_mcp_client_resource_impl *dst = &impl->resources[base_count + i];
      dst->uri = cai_strdup(&impl->allocator, src_resources[i].uri);
      dst->name = cai_strdup(&impl->allocator, src_resources[i].name);
      dst->title = cai_strdup(&impl->allocator,
                              src_resources[i].title != NULL
                                  ? src_resources[i].title
                                  : (src_resources[i].description != NULL
                                         ? src_resources[i].description
                                         : ""));
      dst->description =
          cai_strdup(&impl->allocator, src_resources[i].description != NULL
                                           ? src_resources[i].description
                                           : "");
      dst->mime_type = cai_strdup(
          &impl->allocator,
          src_resources[i].mime_type != NULL ? src_resources[i].mime_type : "");
      dst->icons_json = cai_mcp_optional_json_array_to_cstr(
          &impl->allocator, &src_resources[i].icons, error);
      dst->annotations_json = cai_mcp_optional_json_value_to_cstr(
          &impl->allocator, &src_resources[i].annotations, error);
      dst->has_size = src_resources[i].has_size;
      dst->size = (long long)src_resources[i].size;
      if (dst->uri == NULL || dst->name == NULL || dst->title == NULL ||
          dst->description == NULL || dst->mime_type == NULL ||
          dst->icons_json == NULL || dst->annotations_json == NULL) {
        rc = error != NULL && error->code != CAI_OK
                 ? error->code
                 : cai_set_error(error, CAI_ERR_NOMEM,
                                 "failed to copy MCP resource metadata");
        cai_mcp_client_resource_impl_cleanup(&impl->allocator, dst);
        break;
      }
      dst->public_resource.uri = dst->uri;
      dst->public_resource.name = dst->name;
      dst->public_resource.title = dst->title;
      dst->public_resource.description = dst->description;
      dst->public_resource.mime_type = dst->mime_type;
      dst->public_resource.icons_json = dst->icons_json;
      dst->public_resource.annotations_json = dst->annotations_json;
      dst->public_resource.has_size = dst->has_size;
      dst->public_resource.size = dst->size;
      impl->resource_count++;
    }
  }
  if (rc == CAI_OK && next_cursor != NULL && doc.result.next_cursor != NULL) {
    *next_cursor = cai_strdup(&impl->allocator, doc.result.next_cursor);
    if (*next_cursor == NULL) {
      rc = cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to copy MCP resources/list cursor");
    }
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_resources_list_response_map, &doc);
  if (rc != CAI_OK) {
    cai_mcp_client_clear_resources(impl);
    if (next_cursor != NULL) {
      cai_free_mem(&impl->allocator, *next_cursor);
      *next_cursor = NULL;
    }
  }
  json_body.cleanup(&json_body);
  return rc;
}

static int cai_mcp_parse_resource_templates_list_response(
    cai_mcp_streamable_http_client_impl *impl,
    const cai_mcp_http_response_capture *response, char **next_cursor,
    cai_error *error) {
  cai_mcp_resource_templates_list_response_doc doc;
  lonejson_spooled json_body;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  cai_mcp_list_resource_template_doc *src_resource_templates;
  size_t base_count;
  size_t i;
  int has_error;
  int rc;

  if (next_cursor != NULL) {
    *next_cursor = NULL;
  }
  rc = cai_mcp_response_json_body(
      response, "MCP resources/templates/list response", &json_body, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_mcp_response_has_jsonrpc_error(&json_body, &has_error, error);
  if (rc != CAI_OK) {
    json_body.cleanup(&json_body);
    return rc;
  }
  memset(&doc, 0, sizeof(doc));
  reader.cursor = json_body;
  lonejson_error_init(&json_error);
  if (reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to rewind MCP response",
                                  &json_error);
  }
  status = CAI_LJ->parse_reader(
      CAI_LJ, &cai_mcp_resource_templates_list_response_map, &doc,
      cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_resource_templates_list_response_map,
                    &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(
        error, "failed to parse MCP resources/templates/list", &json_error);
  }
  if (has_error) {
    rc = cai_mcp_set_rpc_error(error, &doc.error_doc);
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_resource_templates_list_response_map,
                    &doc);
    json_body.cleanup(&json_body);
    return rc;
  }
  src_resource_templates =
      (cai_mcp_list_resource_template_doc *)doc.result.resource_templates.items;
  for (i = 0U; i < doc.result.resource_templates.count; i++) {
    if (!cai_mcp_optional_json_array_is_valid(&src_resource_templates[i].icons,
                                              error)) {
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_resource_templates_list_response_map,
                      &doc);
      json_body.cleanup(&json_body);
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP resource template icons must be an array");
    }
    if (!cai_mcp_optional_json_object_is_valid(
            &src_resource_templates[i].annotations, error)) {
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_resource_templates_list_response_map,
                      &doc);
      json_body.cleanup(&json_body);
      return cai_set_error(
          error, CAI_ERR_PROTOCOL,
          "MCP resource template annotations must be an object");
    }
  }
  base_count = impl->resource_template_count;
  rc = cai_mcp_client_reserve_resource_templates(
      impl, base_count + doc.result.resource_templates.count, error);
  if (rc == CAI_OK) {
    for (i = 0U; i < doc.result.resource_templates.count; i++) {
      cai_mcp_client_resource_template_impl *dst =
          &impl->resource_templates[base_count + i];
      dst->uri_template =
          cai_strdup(&impl->allocator, src_resource_templates[i].uri_template);
      dst->name = cai_strdup(&impl->allocator, src_resource_templates[i].name);
      dst->title = cai_strdup(
          &impl->allocator, src_resource_templates[i].title != NULL
                                ? src_resource_templates[i].title
                                : (src_resource_templates[i].description != NULL
                                       ? src_resource_templates[i].description
                                       : ""));
      dst->description = cai_strdup(
          &impl->allocator, src_resource_templates[i].description != NULL
                                ? src_resource_templates[i].description
                                : "");
      dst->mime_type = cai_strdup(&impl->allocator,
                                  src_resource_templates[i].mime_type != NULL
                                      ? src_resource_templates[i].mime_type
                                      : "");
      dst->icons_json = cai_mcp_optional_json_array_to_cstr(
          &impl->allocator, &src_resource_templates[i].icons, error);
      dst->annotations_json = cai_mcp_optional_json_value_to_cstr(
          &impl->allocator, &src_resource_templates[i].annotations, error);
      if (dst->uri_template == NULL || dst->name == NULL ||
          dst->title == NULL || dst->description == NULL ||
          dst->mime_type == NULL || dst->icons_json == NULL ||
          dst->annotations_json == NULL) {
        rc = error != NULL && error->code != CAI_OK
                 ? error->code
                 : cai_set_error(
                       error, CAI_ERR_NOMEM,
                       "failed to copy MCP resource template metadata");
        cai_mcp_client_resource_template_impl_cleanup(&impl->allocator, dst);
        break;
      }
      dst->public_resource_template.uri_template = dst->uri_template;
      dst->public_resource_template.name = dst->name;
      dst->public_resource_template.title = dst->title;
      dst->public_resource_template.description = dst->description;
      dst->public_resource_template.mime_type = dst->mime_type;
      dst->public_resource_template.icons_json = dst->icons_json;
      dst->public_resource_template.annotations_json = dst->annotations_json;
      impl->resource_template_count++;
    }
  }
  if (rc == CAI_OK && next_cursor != NULL && doc.result.next_cursor != NULL) {
    *next_cursor = cai_strdup(&impl->allocator, doc.result.next_cursor);
    if (*next_cursor == NULL) {
      rc = cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to copy MCP resources/templates/list cursor");
    }
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_resource_templates_list_response_map, &doc);
  if (rc != CAI_OK) {
    cai_mcp_client_clear_resource_templates(impl);
    if (next_cursor != NULL) {
      cai_free_mem(&impl->allocator, *next_cursor);
      *next_cursor = NULL;
    }
  }
  json_body.cleanup(&json_body);
  return rc;
}

static int cai_mcp_parse_prompts_list_response(
    cai_mcp_streamable_http_client_impl *impl,
    const cai_mcp_http_response_capture *response, char **next_cursor,
    cai_error *error) {
  cai_mcp_prompts_list_response_doc doc;
  lonejson_spooled json_body;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  cai_mcp_list_prompt_doc *src_prompts;
  size_t base_count;
  size_t i;
  int has_error;
  int rc;

  if (next_cursor != NULL) {
    *next_cursor = NULL;
  }
  rc = cai_mcp_response_json_body(response, "MCP prompts/list response",
                                  &json_body, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_mcp_response_has_jsonrpc_error(&json_body, &has_error, error);
  if (rc != CAI_OK) {
    json_body.cleanup(&json_body);
    return rc;
  }
  memset(&doc, 0, sizeof(doc));
  reader.cursor = json_body;
  lonejson_error_init(&json_error);
  if (reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to rewind MCP response",
                                  &json_error);
  }
  status =
      CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_prompts_list_response_map, &doc,
                           cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_prompts_list_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to parse MCP prompts/list",
                                  &json_error);
  }
  if (has_error) {
    rc = cai_mcp_set_rpc_error(error, &doc.error_doc);
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_prompts_list_response_map, &doc);
    json_body.cleanup(&json_body);
    return rc;
  }
  src_prompts = (cai_mcp_list_prompt_doc *)doc.result.prompts.items;
  for (i = 0U; i < doc.result.prompts.count; i++) {
    if (!cai_mcp_optional_json_array_is_valid(&src_prompts[i].arguments,
                                              error)) {
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_prompts_list_response_map, &doc);
      json_body.cleanup(&json_body);
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP prompt arguments must be an array");
    }
    if (!cai_mcp_optional_json_array_is_valid(&src_prompts[i].icons, error)) {
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_prompts_list_response_map, &doc);
      json_body.cleanup(&json_body);
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP prompt icons must be an array");
    }
  }
  base_count = impl->prompt_count;
  rc = cai_mcp_client_reserve_prompts(
      impl, base_count + doc.result.prompts.count, error);
  if (rc == CAI_OK) {
    for (i = 0U; i < doc.result.prompts.count; i++) {
      cai_mcp_client_prompt_impl *dst = &impl->prompts[base_count + i];
      dst->name = cai_strdup(&impl->allocator, src_prompts[i].name);
      dst->title =
          cai_strdup(&impl->allocator, src_prompts[i].title != NULL
                                           ? src_prompts[i].title
                                           : (src_prompts[i].description != NULL
                                                  ? src_prompts[i].description
                                                  : ""));
      dst->description = cai_strdup(
          &impl->allocator,
          src_prompts[i].description != NULL ? src_prompts[i].description : "");
      dst->arguments_json = cai_mcp_optional_json_array_to_cstr(
          &impl->allocator, &src_prompts[i].arguments, error);
      dst->icons_json = cai_mcp_optional_json_array_to_cstr(
          &impl->allocator, &src_prompts[i].icons, error);
      if (dst->name == NULL || dst->title == NULL || dst->description == NULL ||
          dst->arguments_json == NULL || dst->icons_json == NULL) {
        rc = error != NULL && error->code != CAI_OK
                 ? error->code
                 : cai_set_error(error, CAI_ERR_NOMEM,
                                 "failed to copy MCP prompt metadata");
        cai_mcp_client_prompt_impl_cleanup(&impl->allocator, dst);
        break;
      }
      dst->public_prompt.name = dst->name;
      dst->public_prompt.title = dst->title;
      dst->public_prompt.description = dst->description;
      dst->public_prompt.arguments_json = dst->arguments_json;
      dst->public_prompt.icons_json = dst->icons_json;
      impl->prompt_count++;
    }
  }
  if (rc == CAI_OK && next_cursor != NULL && doc.result.next_cursor != NULL) {
    *next_cursor = cai_strdup(&impl->allocator, doc.result.next_cursor);
    if (*next_cursor == NULL) {
      rc = cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to copy MCP prompts/list cursor");
    }
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_prompts_list_response_map, &doc);
  if (rc != CAI_OK) {
    cai_mcp_client_clear_prompts(impl);
    if (next_cursor != NULL) {
      cai_free_mem(&impl->allocator, *next_cursor);
      *next_cursor = NULL;
    }
  }
  json_body.cleanup(&json_body);
  return rc;
}

static int
cai_mcp_resource_content_has_body(const cai_mcp_resource_content_doc *content) {
  return content != NULL && ((content->text != NULL && content->blob == NULL) ||
                             (content->text == NULL && content->blob != NULL));
}

static int cai_mcp_validate_resource_read_response_shape(
    const cai_mcp_http_response_capture *response, cai_error *error) {
  cai_mcp_resource_read_response_doc doc;
  cai_mcp_resource_content_doc *contents;
  lonejson_spooled json_body;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  size_t i;
  int has_error;
  int rc;

  rc = cai_mcp_response_json_body(response, "MCP resources/read response",
                                  &json_body, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_mcp_response_has_jsonrpc_error(&json_body, &has_error, error);
  if (rc != CAI_OK || has_error) {
    json_body.cleanup(&json_body);
    return rc;
  }
  memset(&doc, 0, sizeof(doc));
  reader.cursor = json_body;
  lonejson_error_init(&json_error);
  if (reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to rewind MCP response",
                                  &json_error);
  }
  status =
      CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_resource_read_response_map, &doc,
                           cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_resource_read_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to parse MCP resources/read",
                                  &json_error);
  }
  contents = (cai_mcp_resource_content_doc *)doc.result.contents.items;
  rc = CAI_OK;
  for (i = 0U; i < doc.result.contents.count; i++) {
    if (!cai_mcp_resource_content_has_body(&contents[i])) {
      rc = cai_set_error(
          error, CAI_ERR_PROTOCOL,
          "MCP resource content must include exactly one of text or blob");
      break;
    }
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_resource_read_response_map, &doc);
  json_body.cleanup(&json_body);
  return rc;
}

static int cai_mcp_prompt_role_is_valid(const char *role) {
  return role != NULL &&
         (strcmp(role, "user") == 0 || strcmp(role, "assistant") == 0);
}

static int cai_mcp_task_status_is_valid(const char *status) {
  return status != NULL &&
         (strcmp(status, "working") == 0 ||
          strcmp(status, "input_required") == 0 ||
          strcmp(status, "completed") == 0 || strcmp(status, "failed") == 0 ||
          strcmp(status, "cancelled") == 0);
}

static int
cai_mcp_json_value_is_non_negative_number_or_null(
    const lonejson_json_value *value, int *is_number, int *is_negative,
    cai_error *error) {
  char *text;
  char *cursor;
  char *end;
  double number;
  int ok;

  if (is_number != NULL) {
    *is_number = 0;
  }
  if (is_negative != NULL) {
    *is_negative = 0;
  }
  if (value == NULL || value->kind == LONEJSON_JSON_VALUE_NULL) {
    return 1;
  }
  text = cai_mcp_json_value_to_cstr(value, error);
  if (text == NULL) {
    return 0;
  }
  cursor = text;
  while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' ||
         *cursor == '\n') {
    cursor++;
  }
  ok = 0;
  if (strncmp(cursor, "null", 4U) == 0) {
    end = cursor + 4U;
  } else {
    number = strtod(cursor, &end);
    if (end != cursor && is_number != NULL) {
      *is_number = 1;
    }
    if (end != cursor && number < 0.0 && is_negative != NULL) {
      *is_negative = 1;
    }
  }
  if (end != cursor) {
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
      end++;
    }
    ok = *end == '\0' && (is_negative == NULL || !*is_negative);
  }
  cai_free_mem(NULL, text);
  return ok;
}

static int cai_mcp_task_validate(const cai_mcp_task_doc *task,
                                 cai_error *error) {
  int ttl_is_number;
  int ttl_is_negative;

  if (task == NULL || !cai_mcp_task_status_is_valid(task->status)) {
    return cai_set_error(error, CAI_ERR_PROTOCOL, "MCP task status is invalid");
  }
  ttl_is_number = 0;
  ttl_is_negative = 0;
  if (!cai_mcp_json_value_is_non_negative_number_or_null(
          &task->ttl, &ttl_is_number, &ttl_is_negative, error)) {
    if (ttl_is_number && ttl_is_negative) {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP task ttl must be non-negative or null");
    }
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP task ttl must be a number or null");
  }
  return CAI_OK;
}

static int
cai_mcp_content_block_validate(const cai_mcp_tool_content_doc *content,
                               cai_error *error) {
  cai_mcp_resource_content_doc resource;
  lonejson_error json_error;
  lonejson_status status;
  char *resource_json;
  int rc;

  if (content == NULL || content->type == NULL) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP content block type is required");
  }
  if (strcmp(content->type, "text") == 0) {
    if (content->text == NULL) {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP text content requires text");
    }
  } else if (strcmp(content->type, "image") == 0 ||
             strcmp(content->type, "audio") == 0) {
    if (content->data == NULL || content->mime_type == NULL) {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP media content requires data and mimeType");
    }
  } else if (strcmp(content->type, "resource_link") == 0) {
    if (content->uri == NULL || content->name == NULL) {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP resource_link content requires uri and name");
    }
  } else if (strcmp(content->type, "resource") == 0) {
    if (content->resource.kind == LONEJSON_JSON_VALUE_NULL ||
        !cai_mcp_json_value_root_is(&content->resource, '{', error)) {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP embedded resource content requires resource");
    }
    resource_json = cai_mcp_json_value_to_cstr(&content->resource, error);
    if (resource_json == NULL) {
      return CAI_ERR_NOMEM;
    }
    memset(&resource, 0, sizeof(resource));
    lonejson_error_init(&json_error);
    status = CAI_LJ->parse_cstr(CAI_LJ, &cai_mcp_resource_content_map,
                                &resource, resource_json, &json_error);
    cai_free_mem(NULL, resource_json);
    if (status != LONEJSON_STATUS_OK) {
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_resource_content_map, &resource);
      return cai_mcp_set_json_error(
          error, "failed to parse MCP embedded resource content", &json_error);
    }
    rc = CAI_OK;
    if (!cai_mcp_resource_content_has_body(&resource)) {
      rc = cai_set_error(
          error, CAI_ERR_PROTOCOL,
          "MCP embedded resource content must include exactly one of text or "
          "blob");
    }
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_resource_content_map, &resource);
    if (rc != CAI_OK) {
      return rc;
    }
  } else {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP content block type is not supported");
  }
  return CAI_OK;
}

static char *cai_mcp_wrap_content_array_json(const char *array_json,
                                             cai_error *error) {
  static const char prefix[] = "{\"content\":";
  cai_buffer_builder builder;

  if (array_json == NULL) {
    return NULL;
  }
  memset(&builder, 0, sizeof(builder));
  if (cai_buffer_append(&builder, prefix, sizeof(prefix) - 1U, error) !=
          CAI_OK ||
      cai_buffer_append(&builder, array_json, strlen(array_json), error) !=
          CAI_OK ||
      cai_buffer_append(&builder, "}", 1U, error) != CAI_OK ||
      cai_buffer_append(&builder, "", 1U, error) != CAI_OK) {
    cai_free_mem(NULL, builder.data);
    return NULL;
  }
  return builder.data;
}

static int
cai_mcp_tool_result_content_validate(const lonejson_json_value *content,
                                     cai_error *error) {
  cai_mcp_tool_call_result_doc doc;
  cai_mcp_tool_content_doc *contents;
  lonejson_error json_error;
  lonejson_status status;
  char *content_json;
  char *wrapped_json;
  size_t i;
  int rc;

  if (content == NULL || content->kind == LONEJSON_JSON_VALUE_NULL) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP tool_result content is required");
  }
  if (!cai_mcp_json_value_root_is(content, '[', error)) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP tool_result content must be an array");
  }
  content_json = cai_mcp_json_value_to_cstr(content, error);
  if (content_json == NULL) {
    return CAI_ERR_NOMEM;
  }
  wrapped_json = cai_mcp_wrap_content_array_json(content_json, error);
  cai_free_mem(NULL, content_json);
  if (wrapped_json == NULL) {
    return CAI_ERR_NOMEM;
  }
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
  status = CAI_LJ->parse_cstr(CAI_LJ, &cai_mcp_tool_call_result_map, &doc,
                              wrapped_json, &json_error);
  cai_free_mem(NULL, wrapped_json);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tool_call_result_map, &doc);
    return cai_mcp_set_json_error(
        error, "failed to parse MCP tool_result content", &json_error);
  }
  contents = (cai_mcp_tool_content_doc *)doc.content.items;
  rc = CAI_OK;
  for (i = 0U; i < doc.content.count; i++) {
    rc = cai_mcp_content_block_validate(&contents[i], error);
    if (rc != CAI_OK) {
      break;
    }
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tool_call_result_map, &doc);
  return rc;
}

static int
cai_mcp_sampling_content_doc_validate(const cai_mcp_sampling_content_doc *doc,
                                      cai_error *error) {
  if (doc == NULL || doc->type == NULL) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP sampling content block type is required");
  }
  if (strcmp(doc->type, "text") == 0) {
    if (doc->text == NULL) {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP text content requires text");
    }
  } else if (strcmp(doc->type, "image") == 0 ||
             strcmp(doc->type, "audio") == 0) {
    if (doc->data == NULL || doc->mime_type == NULL) {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP media content requires data and mimeType");
    }
  } else if (strcmp(doc->type, "tool_use") == 0) {
    if (doc->id == NULL || doc->name == NULL ||
        doc->input.kind == LONEJSON_JSON_VALUE_NULL) {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP tool_use content requires id, name, and input");
    }
    if (!cai_mcp_json_value_root_is(&doc->input, '{', error)) {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP tool_use input must be an object");
    }
  } else if (strcmp(doc->type, "tool_result") == 0) {
    if (doc->tool_use_id == NULL) {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP tool_result content requires toolUseId");
    }
    if (doc->structured_content.kind != LONEJSON_JSON_VALUE_NULL &&
        !cai_mcp_json_value_root_is(&doc->structured_content, '{', error)) {
      return cai_set_error(
          error, CAI_ERR_PROTOCOL,
          "MCP tool_result structuredContent must be an object");
    }
    return cai_mcp_tool_result_content_validate(&doc->content, error);
  } else {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP sampling content block type is not supported");
  }
  return CAI_OK;
}

static int cai_mcp_sampling_content_json_validate(const char *content_json,
                                                  cai_error *error) {
  cai_mcp_sampling_content_doc doc;
  lonejson_error json_error;
  lonejson_status status;
  int rc;

  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
  status = CAI_LJ->parse_cstr(CAI_LJ, &cai_mcp_sampling_content_map, &doc,
                              content_json, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_sampling_content_map, &doc);
    return cai_mcp_set_json_error(
        error, "failed to parse MCP sampling content block", &json_error);
  }
  rc = cai_mcp_sampling_content_doc_validate(&doc, error);
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_sampling_content_map, &doc);
  return rc;
}

static int cai_mcp_sampling_content_array_json_validate(const char *array_json,
                                                        cai_error *error) {
  cai_mcp_sampling_content_array_doc doc;
  cai_mcp_sampling_content_doc *contents;
  lonejson_error json_error;
  lonejson_status status;
  char *wrapped_json;
  size_t i;
  int rc;

  wrapped_json = cai_mcp_wrap_content_array_json(array_json, error);
  if (wrapped_json == NULL) {
    return CAI_ERR_NOMEM;
  }
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
  status = CAI_LJ->parse_cstr(CAI_LJ, &cai_mcp_sampling_content_array_map, &doc,
                              wrapped_json, &json_error);
  cai_free_mem(NULL, wrapped_json);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_sampling_content_array_map, &doc);
    return cai_mcp_set_json_error(
        error, "failed to parse MCP sampling content block", &json_error);
  }
  contents = (cai_mcp_sampling_content_doc *)doc.content.items;
  rc = CAI_OK;
  for (i = 0U; i < doc.content.count; i++) {
    rc = cai_mcp_sampling_content_doc_validate(&contents[i], error);
    if (rc != CAI_OK) {
      break;
    }
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_sampling_content_array_map, &doc);
  return rc;
}

static int
cai_mcp_sampling_content_value_validate(const lonejson_json_value *content,
                                        cai_error *error) {
  char *content_json;
  int is_object;
  int is_array;
  int rc;

  if (content == NULL || content->kind == LONEJSON_JSON_VALUE_NULL) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP sampling result content must be an object or "
                         "array");
  }
  is_object = cai_mcp_json_value_root_is(content, '{', error);
  is_array = !is_object && cai_mcp_json_value_root_is(content, '[', error);
  if (!is_object && !is_array) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP sampling result content must be an object or "
                         "array");
  }
  content_json = cai_mcp_json_value_to_cstr(content, error);
  if (content_json == NULL) {
    return CAI_ERR_NOMEM;
  }
  if (is_object) {
    rc = cai_mcp_sampling_content_json_validate(content_json, error);
  } else {
    rc = cai_mcp_sampling_content_array_json_validate(content_json, error);
  }
  cai_free_mem(NULL, content_json);
  return rc;
}

static int cai_mcp_prompt_content_validate(const lonejson_json_value *content,
                                           cai_error *error) {
  cai_mcp_tool_content_doc doc;
  lonejson_error json_error;
  lonejson_status status;
  char *content_json;
  int rc;

  if (!cai_mcp_json_value_root_is(content, '{', error)) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP prompt message content must be an object");
  }
  content_json = cai_mcp_json_value_to_cstr(content, error);
  if (content_json == NULL) {
    return CAI_ERR_NOMEM;
  }
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
  status = CAI_LJ->parse_cstr(CAI_LJ, &cai_mcp_tool_content_map, &doc,
                              content_json, &json_error);
  cai_free_mem(NULL, content_json);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tool_content_map, &doc);
    return cai_mcp_set_json_error(
        error, "failed to parse MCP prompt message content", &json_error);
  }
  rc = cai_mcp_content_block_validate(&doc, error);
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tool_content_map, &doc);
  return rc;
}

static int cai_mcp_validate_tool_call_response_shape(
    const cai_mcp_http_response_capture *response, cai_error *error) {
  cai_mcp_tool_call_response_doc doc;
  cai_mcp_create_task_response_doc task_doc;
  cai_mcp_tool_content_doc *contents;
  lonejson_spooled json_body;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_error tool_json_error;
  lonejson_status status;
  size_t i;
  int has_error;
  int result_is_object;
  int rc;

  rc = cai_mcp_response_json_body(response, "MCP tools/call response",
                                  &json_body, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_mcp_response_has_jsonrpc_error(&json_body, &has_error, error);
  if (rc != CAI_OK || has_error) {
    json_body.cleanup(&json_body);
    return rc;
  }
  rc = cai_mcp_jsonrpc_response_result_root_is_object(&json_body,
                                                      &result_is_object, error);
  if (rc != CAI_OK) {
    json_body.cleanup(&json_body);
    return rc;
  }
  if (!result_is_object) {
    json_body.cleanup(&json_body);
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP result must be an object");
  }
  memset(&doc, 0, sizeof(doc));
  reader.cursor = json_body;
  lonejson_error_init(&json_error);
  if (reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to rewind MCP response",
                                  &json_error);
  }
  status = CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_tool_call_response_map, &doc,
                                cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    tool_json_error = json_error;
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tool_call_response_map, &doc);
    memset(&task_doc, 0, sizeof(task_doc));
    reader.cursor = json_body;
    lonejson_error_init(&json_error);
    if (reader.cursor.rewind(&reader.cursor, &json_error) !=
        LONEJSON_STATUS_OK) {
      json_body.cleanup(&json_body);
      return cai_mcp_set_json_error(error, "failed to rewind MCP response",
                                    &json_error);
    }
    status = CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_create_task_response_map,
                                  &task_doc, cai_mcp_spooled_read, &reader,
                                  &json_error);
    if (status == LONEJSON_STATUS_OK) {
      rc = cai_mcp_task_validate(&task_doc.result.task, error);
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_create_task_response_map, &task_doc);
      json_body.cleanup(&json_body);
      return rc;
    }
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_create_task_response_map, &task_doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to parse MCP tools/call",
                                  &tool_json_error);
  }
  contents = (cai_mcp_tool_content_doc *)doc.result.content.items;
  rc = CAI_OK;
  for (i = 0U; i < doc.result.content.count; i++) {
    rc = cai_mcp_content_block_validate(&contents[i], error);
    if (rc != CAI_OK) {
      break;
    }
  }
  if (rc == CAI_OK &&
      doc.result.structured_content.kind != LONEJSON_JSON_VALUE_NULL &&
      !cai_mcp_json_value_root_is(&doc.result.structured_content, '{', error)) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "MCP tool structuredContent must be an object");
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tool_call_response_map, &doc);
  json_body.cleanup(&json_body);
  return rc;
}

static int cai_mcp_validate_prompt_get_response_shape(
    const cai_mcp_http_response_capture *response, cai_error *error) {
  cai_mcp_prompt_get_response_doc doc;
  cai_mcp_prompt_message_doc *messages;
  lonejson_spooled json_body;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  size_t i;
  int has_error;
  int rc;

  rc = cai_mcp_response_json_body(response, "MCP prompts/get response",
                                  &json_body, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_mcp_response_has_jsonrpc_error(&json_body, &has_error, error);
  if (rc != CAI_OK || has_error) {
    json_body.cleanup(&json_body);
    return rc;
  }
  memset(&doc, 0, sizeof(doc));
  reader.cursor = json_body;
  lonejson_error_init(&json_error);
  if (reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to rewind MCP response",
                                  &json_error);
  }
  status = CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_prompt_get_response_map, &doc,
                                cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_prompt_get_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to parse MCP prompts/get",
                                  &json_error);
  }
  messages = (cai_mcp_prompt_message_doc *)doc.result.messages.items;
  rc = CAI_OK;
  for (i = 0U; i < doc.result.messages.count; i++) {
    if (!cai_mcp_prompt_role_is_valid(messages[i].role)) {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP prompt message role must be user or assistant");
      break;
    }
    rc = cai_mcp_prompt_content_validate(&messages[i].content, error);
    if (rc != CAI_OK) {
      break;
    }
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_prompt_get_response_map, &doc);
  json_body.cleanup(&json_body);
  return rc;
}

static int cai_mcp_validate_completion_response_shape(
    const cai_mcp_http_response_capture *response, cai_error *error) {
  cai_mcp_completion_response_doc doc;
  lonejson_spooled json_body;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  int has_error;
  int rc;

  rc = cai_mcp_response_json_body(response, "MCP completion/complete response",
                                  &json_body, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_mcp_response_has_jsonrpc_error(&json_body, &has_error, error);
  if (rc != CAI_OK || has_error) {
    json_body.cleanup(&json_body);
    return rc;
  }
  memset(&doc, 0, sizeof(doc));
  reader.cursor = json_body;
  lonejson_error_init(&json_error);
  if (reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to rewind MCP response",
                                  &json_error);
  }
  status = CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_completion_response_map, &doc,
                                cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_completion_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(
        error, "failed to parse MCP completion/complete", &json_error);
  }
  if (doc.result.completion.values.count > 100U) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "MCP completion values must not exceed 100 items");
  }
  if (rc == CAI_OK && doc.result.completion.has_total &&
      doc.result.completion.total < 0) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "MCP completion total must be non-negative");
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_completion_response_map, &doc);
  json_body.cleanup(&json_body);
  return rc;
}

static int cai_mcp_validate_tasks_list_response_shape(
    const cai_mcp_http_response_capture *response, cai_error *error) {
  cai_mcp_tasks_list_response_doc doc;
  cai_mcp_task_doc *tasks;
  lonejson_spooled json_body;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  size_t i;
  int has_error;
  int rc;

  rc = cai_mcp_response_json_body(response, "MCP tasks/list response",
                                  &json_body, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_mcp_response_has_jsonrpc_error(&json_body, &has_error, error);
  if (rc != CAI_OK || has_error) {
    json_body.cleanup(&json_body);
    return rc;
  }
  memset(&doc, 0, sizeof(doc));
  reader.cursor = json_body;
  lonejson_error_init(&json_error);
  if (reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to rewind MCP response",
                                  &json_error);
  }
  status = CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_tasks_list_response_map, &doc,
                                cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tasks_list_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to parse MCP tasks/list",
                                  &json_error);
  }
  tasks = (cai_mcp_task_doc *)doc.result.tasks.items;
  rc = CAI_OK;
  for (i = 0U; i < doc.result.tasks.count; i++) {
    rc = cai_mcp_task_validate(&tasks[i], error);
    if (rc != CAI_OK) {
      break;
    }
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tasks_list_response_map, &doc);
  json_body.cleanup(&json_body);
  return rc;
}

static int cai_mcp_validate_task_response_shape(
    const cai_mcp_http_response_capture *response, const char *response_name,
    const char *parse_name, cai_error *error) {
  cai_mcp_task_response_doc doc;
  lonejson_spooled json_body;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  int has_error;
  int rc;

  rc = cai_mcp_response_json_body(response, response_name, &json_body, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_mcp_response_has_jsonrpc_error(&json_body, &has_error, error);
  if (rc != CAI_OK || has_error) {
    json_body.cleanup(&json_body);
    return rc;
  }
  memset(&doc, 0, sizeof(doc));
  reader.cursor = json_body;
  lonejson_error_init(&json_error);
  if (reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to rewind MCP response",
                                  &json_error);
  }
  status = CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_task_response_map, &doc,
                                cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_task_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, parse_name, &json_error);
  }
  rc = cai_mcp_task_validate(&doc.result, error);
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_task_response_map, &doc);
  json_body.cleanup(&json_body);
  return rc;
}

static int
cai_mcp_parse_result_response(const cai_mcp_http_response_capture *response,
                              const char *response_name, const char *parse_name,
                              cai_sink *output, cai_error *error) {
  cai_mcp_jsonrpc_sink_response_doc doc;
  lonejson_spooled json_body;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  int has_error;
  int result_is_object;
  int rc;

  rc = cai_mcp_response_json_body(response, response_name, &json_body, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_mcp_response_has_jsonrpc_error(&json_body, &has_error, error);
  if (rc != CAI_OK) {
    json_body.cleanup(&json_body);
    return rc;
  }
  if (!has_error) {
    rc = cai_mcp_jsonrpc_response_result_root_is_object(
        &json_body, &result_is_object, error);
    if (rc != CAI_OK) {
      json_body.cleanup(&json_body);
      return rc;
    }
    if (!result_is_object) {
      json_body.cleanup(&json_body);
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP result must be an object");
    }
  }
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
  CAI_LJ_PRESERVE->json_value_init(CAI_LJ_PRESERVE, &doc.result);
  if (doc.result.methods->set_parse_sink(&doc.result, cai_mcp_cai_sink_bridge,
                                         output,
                                         &json_error) != LONEJSON_STATUS_OK) {
    CAI_LJ_PRESERVE->cleanup(CAI_LJ_PRESERVE,
                             &cai_mcp_jsonrpc_sink_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to prepare MCP result sink",
                                  &json_error);
  }
  reader.cursor = json_body;
  if (reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    CAI_LJ_PRESERVE->cleanup(CAI_LJ_PRESERVE,
                             &cai_mcp_jsonrpc_sink_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to rewind MCP response",
                                  &json_error);
  }
  status = CAI_LJ_PRESERVE->parse_reader(
      CAI_LJ_PRESERVE, &cai_mcp_jsonrpc_sink_response_map, &doc,
      cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ_PRESERVE->cleanup(CAI_LJ_PRESERVE,
                             &cai_mcp_jsonrpc_sink_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, parse_name, &json_error);
  }
  if (has_error) {
    rc = cai_mcp_set_rpc_error(error, &doc.error_doc);
    CAI_LJ_PRESERVE->cleanup(CAI_LJ_PRESERVE,
                             &cai_mcp_jsonrpc_sink_response_map, &doc);
    json_body.cleanup(&json_body);
    return rc;
  }
  CAI_LJ_PRESERVE->cleanup(CAI_LJ_PRESERVE, &cai_mcp_jsonrpc_sink_response_map,
                           &doc);
  json_body.cleanup(&json_body);
  return CAI_OK;
}

static int cai_mcp_parse_empty_result_response(
    const cai_mcp_http_response_capture *response, const char *response_name,
    const char *parse_name, cai_error *error) {
  cai_mcp_jsonrpc_sink_response_doc doc;
  lonejson_spooled json_body;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  int has_error;
  int rc;

  rc = cai_mcp_response_json_body(response, response_name, &json_body, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_mcp_response_has_jsonrpc_error(&json_body, &has_error, error);
  if (rc != CAI_OK) {
    json_body.cleanup(&json_body);
    return rc;
  }
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
  CAI_LJ_PRESERVE->json_value_init(CAI_LJ_PRESERVE, &doc.result);
  if (doc.result.methods->set_parse_sink(&doc.result, cai_mcp_discard_sink,
                                         NULL,
                                         &json_error) != LONEJSON_STATUS_OK) {
    CAI_LJ_PRESERVE->cleanup(CAI_LJ_PRESERVE,
                             &cai_mcp_jsonrpc_sink_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to prepare MCP result sink",
                                  &json_error);
  }
  reader.cursor = json_body;
  if (reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    CAI_LJ_PRESERVE->cleanup(CAI_LJ_PRESERVE,
                             &cai_mcp_jsonrpc_sink_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to rewind MCP response",
                                  &json_error);
  }
  status = CAI_LJ_PRESERVE->parse_reader(
      CAI_LJ_PRESERVE, &cai_mcp_jsonrpc_sink_response_map, &doc,
      cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ_PRESERVE->cleanup(CAI_LJ_PRESERVE,
                             &cai_mcp_jsonrpc_sink_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, parse_name, &json_error);
  }
  if (has_error) {
    rc = cai_mcp_set_rpc_error(error, &doc.error_doc);
    CAI_LJ_PRESERVE->cleanup(CAI_LJ_PRESERVE,
                             &cai_mcp_jsonrpc_sink_response_map, &doc);
    json_body.cleanup(&json_body);
    return rc;
  }
  CAI_LJ_PRESERVE->cleanup(CAI_LJ_PRESERVE, &cai_mcp_jsonrpc_sink_response_map,
                           &doc);
  json_body.cleanup(&json_body);
  return CAI_OK;
}

static int
cai_mcp_parse_ping_response(const cai_mcp_http_response_capture *response,
                            cai_error *error) {
  return cai_mcp_parse_empty_result_response(response, "MCP ping response",
                                             "failed to parse MCP ping", error);
}

static int
cai_mcp_parse_call_response(const cai_mcp_http_response_capture *response,
                            cai_sink *output, cai_error *error) {
  int rc;

  rc = cai_mcp_validate_tool_call_response_shape(response, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_mcp_parse_result_response(response, "MCP tools/call response",
                                       "failed to parse MCP tools/call", output,
                                       error);
}

static int cai_mcp_parse_resource_read_response(
    const cai_mcp_http_response_capture *response, cai_sink *output,
    cai_error *error) {
  int rc;

  rc = cai_mcp_validate_resource_read_response_shape(response, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_mcp_parse_result_response(response, "MCP resources/read response",
                                       "failed to parse MCP resources/read",
                                       output, error);
}

static int cai_mcp_parse_resource_subscribe_response(
    const cai_mcp_http_response_capture *response, cai_error *error) {
  return cai_mcp_parse_empty_result_response(
      response, "MCP resources/subscribe response",
      "failed to parse MCP resources/subscribe", error);
}

static int cai_mcp_parse_resource_unsubscribe_response(
    const cai_mcp_http_response_capture *response, cai_error *error) {
  return cai_mcp_parse_empty_result_response(
      response, "MCP resources/unsubscribe response",
      "failed to parse MCP resources/unsubscribe", error);
}

static int
cai_mcp_parse_prompt_get_response(const cai_mcp_http_response_capture *response,
                                  cai_sink *output, cai_error *error) {
  int rc;

  rc = cai_mcp_validate_prompt_get_response_shape(response, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_mcp_parse_result_response(response, "MCP prompts/get response",
                                       "failed to parse MCP prompts/get",
                                       output, error);
}

static int
cai_mcp_parse_completion_response(const cai_mcp_http_response_capture *response,
                                  cai_sink *output, cai_error *error) {
  int rc;

  rc = cai_mcp_validate_completion_response_shape(response, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_mcp_parse_result_response(
      response, "MCP completion/complete response",
      "failed to parse MCP completion/complete", output, error);
}

static int cai_mcp_parse_logging_set_level_response(
    const cai_mcp_http_response_capture *response, cai_error *error) {
  return cai_mcp_parse_empty_result_response(
      response, "MCP logging/setLevel response",
      "failed to parse MCP logging/setLevel", error);
}

static int cai_mcp_streamable_initialize(cai_mcp_client *client,
                                         cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_mcp_http_response_capture response;
  lonejson_spooled request;
  size_t request_len;
  int rc;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP client is required");
  }
  if (impl->initialized) {
    return CAI_OK;
  }
  memset(&response, 0, sizeof(response));
  memset(&request, 0, sizeof(request));
  rc = cai_mcp_initialize_request(impl, &request, &request_len, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_post(impl, &request, request_len, 1, &response, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_validate_response_envelope(&request, &response, error);
  }
  cai_mcp_spooled_cleanup_if_initialized(&request);
  if (rc == CAI_OK) {
    rc = cai_mcp_parse_initialize_response(impl, &response, error);
  }
  if (rc == CAI_OK && response.session_id != NULL) {
    if (!cai_mcp_session_id_is_valid(response.session_id)) {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP session id must contain visible ASCII only");
    }
  }
  if (rc == CAI_OK && response.session_id != NULL) {
    impl->session_id = cai_strdup(&impl->allocator, response.session_id);
    if (impl->session_id == NULL) {
      rc =
          cai_set_error(error, CAI_ERR_NOMEM, "failed to store MCP session id");
    }
  }
  cai_mcp_http_response_capture_cleanup(&response);
  if (rc == CAI_OK) {
    lonejson_spooled notification;
    size_t notification_len;

    impl->initialized = 1;
    memset(&notification, 0, sizeof(notification));
    rc = cai_mcp_initialized_notification(&notification, &notification_len,
                                          error);
    if (rc == CAI_OK) {
      rc = cai_mcp_post(impl, &notification, notification_len, 0, &response,
                        error);
      cai_mcp_http_response_capture_cleanup(&response);
    }
    cai_mcp_spooled_cleanup_if_initialized(&notification);
  }
  return rc;
}

static int cai_mcp_post_with_session_recovery(
    cai_mcp_streamable_http_client_impl *impl, const lonejson_spooled *request,
    size_t request_len, int is_request, cai_mcp_http_response_capture *response,
    cai_error *error) {
  int had_session;
  int rc;

  had_session = impl != NULL && impl->initialized && impl->session_id != NULL;
  rc = cai_mcp_post(impl, request, request_len, is_request, response, error);
  if (rc == CAI_OK || !had_session || response == NULL ||
      response->status != 404L) {
    if (rc == CAI_OK && is_request) {
      rc = cai_mcp_validate_response_envelope(request, response, error);
    }
    return rc;
  }

  cai_mcp_http_response_capture_cleanup(response);
  cai_mcp_clear_error(error);
  cai_mcp_streamable_reset_session(impl);
  rc = cai_mcp_streamable_initialize(&impl->public_client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_mcp_post(impl, request, request_len, is_request, response, error);
  if (rc == CAI_OK && is_request) {
    rc = cai_mcp_validate_response_envelope(request, response, error);
  }
  return rc;
}

static int cai_mcp_post_request_with_session_recovery(
    cai_mcp_streamable_http_client_impl *impl, const lonejson_spooled *request,
    size_t request_len, cai_mcp_http_response_capture *response,
    cai_error *error) {
  return cai_mcp_post_with_session_recovery(impl, request, request_len, 1,
                                            response, error);
}

static int cai_mcp_streamable_ping(cai_mcp_client *client, cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_mcp_http_response_capture response;
  lonejson_spooled request;
  size_t request_len;
  int rc;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP client is required");
  }
  rc = cai_mcp_client_initialize(client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&response, 0, sizeof(response));
  memset(&request, 0, sizeof(request));
  rc = cai_mcp_ping_request(impl, &request, &request_len, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_post_request_with_session_recovery(impl, &request, request_len,
                                                    &response, error);
  }
  cai_mcp_spooled_cleanup_if_initialized(&request);
  if (rc == CAI_OK) {
    rc = cai_mcp_parse_ping_response(&response, error);
  }
  cai_mcp_http_response_capture_cleanup(&response);
  return rc;
}

static int cai_mcp_streamable_refresh_tools(cai_mcp_client *client,
                                            cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_mcp_http_response_capture response;
  lonejson_spooled request;
  char *cursor;
  char *next_cursor;
  size_t request_len;
  int rc;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP client is required");
  }
  rc = cai_mcp_client_initialize(client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  cursor = NULL;
  cai_mcp_client_clear_tools(impl);
  do {
    next_cursor = NULL;
    memset(&response, 0, sizeof(response));
    memset(&request, 0, sizeof(request));
    rc = cai_mcp_list_request(impl, "tools/list", cursor, &request,
                              &request_len, error);
    if (rc == CAI_OK) {
      rc = cai_mcp_post_request_with_session_recovery(
          impl, &request, request_len, &response, error);
    }
    cai_mcp_spooled_cleanup_if_initialized(&request);
    if (rc == CAI_OK) {
      rc = cai_mcp_parse_tools_list_response(impl, &response, &next_cursor,
                                             error);
    }
    cai_mcp_http_response_capture_cleanup(&response);
    cai_free_mem(&impl->allocator, cursor);
    cursor = next_cursor;
  } while (rc == CAI_OK && cursor != NULL && cursor[0] != '\0');
  cai_free_mem(&impl->allocator, cursor);
  if (rc != CAI_OK) {
    cai_mcp_client_clear_tools(impl);
  }
  return rc;
}

static size_t cai_mcp_streamable_tool_count(const cai_mcp_client *client) {
  const cai_mcp_streamable_http_client_impl *impl;

  impl = cai_mcp_streamable_const_impl(client);
  return impl != NULL ? impl->tool_count : 0U;
}

static const cai_mcp_client_tool *
cai_mcp_streamable_tool_at(const cai_mcp_client *client, size_t index) {
  const cai_mcp_streamable_http_client_impl *impl;

  impl = cai_mcp_streamable_const_impl(client);
  if (impl == NULL || index >= impl->tool_count) {
    return NULL;
  }
  return &impl->tools[index].public_tool;
}

static int cai_mcp_streamable_call_tool(cai_mcp_client *client,
                                        const char *name,
                                        lonejson_spooled *arguments_json,
                                        cai_sink *output, cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_mcp_http_response_capture response;
  lonejson_spooled request;
  size_t request_len;
  int rc;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL || output == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client and output sink are required");
  }
  rc = cai_mcp_client_initialize(client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&response, 0, sizeof(response));
  memset(&request, 0, sizeof(request));
  rc = cai_mcp_call_request(impl, name, arguments_json, 0, -1LL, &request,
                            &request_len, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_post_request_with_session_recovery(impl, &request, request_len,
                                                    &response, error);
  }
  cai_mcp_spooled_cleanup_if_initialized(&request);
  if (rc == CAI_OK) {
    rc = cai_mcp_parse_call_response(&response, output, error);
  }
  cai_mcp_http_response_capture_cleanup(&response);
  return rc;
}

static int cai_mcp_streamable_call_tool_task(cai_mcp_client *client,
                                             const char *name,
                                             lonejson_spooled *arguments_json,
                                             long long ttl_ms, cai_sink *output,
                                             cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_mcp_http_response_capture response;
  lonejson_spooled request;
  size_t request_len;
  int rc;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL || output == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client and output sink are required");
  }
  rc = cai_mcp_client_initialize(client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&response, 0, sizeof(response));
  memset(&request, 0, sizeof(request));
  rc = cai_mcp_call_request(impl, name, arguments_json, 1, ttl_ms, &request,
                            &request_len, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_post_request_with_session_recovery(impl, &request, request_len,
                                                    &response, error);
  }
  cai_mcp_spooled_cleanup_if_initialized(&request);
  if (rc == CAI_OK) {
    rc = cai_mcp_parse_call_response(&response, output, error);
  }
  cai_mcp_http_response_capture_cleanup(&response);
  return rc;
}

static int cai_mcp_streamable_refresh_resources(cai_mcp_client *client,
                                                cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_mcp_http_response_capture response;
  lonejson_spooled request;
  char *cursor;
  char *next_cursor;
  size_t request_len;
  int rc;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP client is required");
  }
  rc = cai_mcp_client_initialize(client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  cursor = NULL;
  cai_mcp_client_clear_resources(impl);
  do {
    next_cursor = NULL;
    memset(&response, 0, sizeof(response));
    memset(&request, 0, sizeof(request));
    rc = cai_mcp_list_request(impl, "resources/list", cursor, &request,
                              &request_len, error);
    if (rc == CAI_OK) {
      rc = cai_mcp_post_request_with_session_recovery(
          impl, &request, request_len, &response, error);
    }
    cai_mcp_spooled_cleanup_if_initialized(&request);
    if (rc == CAI_OK) {
      rc = cai_mcp_parse_resources_list_response(impl, &response, &next_cursor,
                                                 error);
    }
    cai_mcp_http_response_capture_cleanup(&response);
    cai_free_mem(&impl->allocator, cursor);
    cursor = next_cursor;
  } while (rc == CAI_OK && cursor != NULL && cursor[0] != '\0');
  cai_free_mem(&impl->allocator, cursor);
  if (rc != CAI_OK) {
    cai_mcp_client_clear_resources(impl);
  }
  return rc;
}

static size_t cai_mcp_streamable_resource_count(const cai_mcp_client *client) {
  const cai_mcp_streamable_http_client_impl *impl;

  impl = cai_mcp_streamable_const_impl(client);
  return impl != NULL ? impl->resource_count : 0U;
}

static const cai_mcp_client_resource *
cai_mcp_streamable_resource_at(const cai_mcp_client *client, size_t index) {
  const cai_mcp_streamable_http_client_impl *impl;

  impl = cai_mcp_streamable_const_impl(client);
  if (impl == NULL || index >= impl->resource_count) {
    return NULL;
  }
  return &impl->resources[index].public_resource;
}

static int cai_mcp_streamable_read_resource(cai_mcp_client *client,
                                            const char *uri, cai_sink *output,
                                            cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_mcp_http_response_capture response;
  lonejson_spooled request;
  size_t request_len;
  int rc;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL || output == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client and output sink are required");
  }
  rc = cai_mcp_client_initialize(client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&response, 0, sizeof(response));
  memset(&request, 0, sizeof(request));
  rc = cai_mcp_resource_read_request(impl, uri, &request, &request_len, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_post_request_with_session_recovery(impl, &request, request_len,
                                                    &response, error);
  }
  cai_mcp_spooled_cleanup_if_initialized(&request);
  if (rc == CAI_OK) {
    rc = cai_mcp_parse_resource_read_response(&response, output, error);
  }
  cai_mcp_http_response_capture_cleanup(&response);
  return rc;
}

static int cai_mcp_streamable_resource_subscription(
    cai_mcp_client *client, const char *uri, const char *method,
    int (*parse_response)(const cai_mcp_http_response_capture *, cai_error *),
    cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_mcp_http_response_capture response;
  lonejson_spooled request;
  size_t request_len;
  int rc;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP client is required");
  }
  rc = cai_mcp_client_initialize(client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&response, 0, sizeof(response));
  memset(&request, 0, sizeof(request));
  rc = cai_mcp_resource_subscription_request(impl, method, uri, &request,
                                             &request_len, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_post_request_with_session_recovery(impl, &request, request_len,
                                                    &response, error);
  }
  cai_mcp_spooled_cleanup_if_initialized(&request);
  if (rc == CAI_OK) {
    rc = parse_response(&response, error);
  }
  cai_mcp_http_response_capture_cleanup(&response);
  return rc;
}

static int cai_mcp_streamable_subscribe_resource(cai_mcp_client *client,
                                                 const char *uri,
                                                 cai_error *error) {
  return cai_mcp_streamable_resource_subscription(
      client, uri, "resources/subscribe",
      cai_mcp_parse_resource_subscribe_response, error);
}

static int cai_mcp_streamable_unsubscribe_resource(cai_mcp_client *client,
                                                   const char *uri,
                                                   cai_error *error) {
  return cai_mcp_streamable_resource_subscription(
      client, uri, "resources/unsubscribe",
      cai_mcp_parse_resource_unsubscribe_response, error);
}

static int cai_mcp_streamable_refresh_resource_templates(cai_mcp_client *client,
                                                         cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_mcp_http_response_capture response;
  lonejson_spooled request;
  char *cursor;
  char *next_cursor;
  size_t request_len;
  int rc;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP client is required");
  }
  rc = cai_mcp_client_initialize(client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  cursor = NULL;
  cai_mcp_client_clear_resource_templates(impl);
  do {
    next_cursor = NULL;
    memset(&response, 0, sizeof(response));
    memset(&request, 0, sizeof(request));
    rc = cai_mcp_list_request(impl, "resources/templates/list", cursor,
                              &request, &request_len, error);
    if (rc == CAI_OK) {
      rc = cai_mcp_post_request_with_session_recovery(
          impl, &request, request_len, &response, error);
    }
    cai_mcp_spooled_cleanup_if_initialized(&request);
    if (rc == CAI_OK) {
      rc = cai_mcp_parse_resource_templates_list_response(impl, &response,
                                                          &next_cursor, error);
    }
    cai_mcp_http_response_capture_cleanup(&response);
    cai_free_mem(&impl->allocator, cursor);
    cursor = next_cursor;
  } while (rc == CAI_OK && cursor != NULL && cursor[0] != '\0');
  cai_free_mem(&impl->allocator, cursor);
  if (rc != CAI_OK) {
    cai_mcp_client_clear_resource_templates(impl);
  }
  return rc;
}

static size_t
cai_mcp_streamable_resource_template_count(const cai_mcp_client *client) {
  const cai_mcp_streamable_http_client_impl *impl;

  impl = cai_mcp_streamable_const_impl(client);
  return impl != NULL ? impl->resource_template_count : 0U;
}

static const cai_mcp_client_resource_template *
cai_mcp_streamable_resource_template_at(const cai_mcp_client *client,
                                        size_t index) {
  const cai_mcp_streamable_http_client_impl *impl;

  impl = cai_mcp_streamable_const_impl(client);
  if (impl == NULL || index >= impl->resource_template_count) {
    return NULL;
  }
  return &impl->resource_templates[index].public_resource_template;
}

static int cai_mcp_streamable_refresh_prompts(cai_mcp_client *client,
                                              cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_mcp_http_response_capture response;
  lonejson_spooled request;
  char *cursor;
  char *next_cursor;
  size_t request_len;
  int rc;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP client is required");
  }
  rc = cai_mcp_client_initialize(client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  cursor = NULL;
  cai_mcp_client_clear_prompts(impl);
  do {
    next_cursor = NULL;
    memset(&response, 0, sizeof(response));
    memset(&request, 0, sizeof(request));
    rc = cai_mcp_list_request(impl, "prompts/list", cursor, &request,
                              &request_len, error);
    if (rc == CAI_OK) {
      rc = cai_mcp_post_request_with_session_recovery(
          impl, &request, request_len, &response, error);
    }
    cai_mcp_spooled_cleanup_if_initialized(&request);
    if (rc == CAI_OK) {
      rc = cai_mcp_parse_prompts_list_response(impl, &response, &next_cursor,
                                               error);
    }
    cai_mcp_http_response_capture_cleanup(&response);
    cai_free_mem(&impl->allocator, cursor);
    cursor = next_cursor;
  } while (rc == CAI_OK && cursor != NULL && cursor[0] != '\0');
  cai_free_mem(&impl->allocator, cursor);
  if (rc != CAI_OK) {
    cai_mcp_client_clear_prompts(impl);
  }
  return rc;
}

static size_t cai_mcp_streamable_prompt_count(const cai_mcp_client *client) {
  const cai_mcp_streamable_http_client_impl *impl;

  impl = cai_mcp_streamable_const_impl(client);
  return impl != NULL ? impl->prompt_count : 0U;
}

static const cai_mcp_client_prompt *
cai_mcp_streamable_prompt_at(const cai_mcp_client *client, size_t index) {
  const cai_mcp_streamable_http_client_impl *impl;

  impl = cai_mcp_streamable_const_impl(client);
  if (impl == NULL || index >= impl->prompt_count) {
    return NULL;
  }
  return &impl->prompts[index].public_prompt;
}

static int cai_mcp_streamable_get_prompt(cai_mcp_client *client,
                                         const char *name,
                                         lonejson_spooled *arguments_json,
                                         cai_sink *output, cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_mcp_http_response_capture response;
  lonejson_spooled request;
  size_t request_len;
  int rc;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL || output == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client and output sink are required");
  }
  rc = cai_mcp_client_initialize(client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&response, 0, sizeof(response));
  memset(&request, 0, sizeof(request));
  rc = cai_mcp_prompt_get_request(impl, name, arguments_json, &request,
                                  &request_len, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_post_request_with_session_recovery(impl, &request, request_len,
                                                    &response, error);
  }
  cai_mcp_spooled_cleanup_if_initialized(&request);
  if (rc == CAI_OK) {
    rc = cai_mcp_parse_prompt_get_response(&response, output, error);
  }
  cai_mcp_http_response_capture_cleanup(&response);
  return rc;
}

static int cai_mcp_streamable_complete(cai_mcp_client *client,
                                       const char *ref_type,
                                       const char *ref_value,
                                       const char *argument_name,
                                       const char *argument_value,
                                       lonejson_spooled *context_arguments_json,
                                       cai_sink *output, cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_mcp_http_response_capture response;
  lonejson_spooled request;
  size_t request_len;
  int rc;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL || output == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client and output sink are required");
  }
  rc = cai_mcp_client_initialize(client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&response, 0, sizeof(response));
  memset(&request, 0, sizeof(request));
  rc = cai_mcp_completion_request(impl, ref_type, ref_value, argument_name,
                                  argument_value, context_arguments_json,
                                  &request, &request_len, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_post_request_with_session_recovery(impl, &request, request_len,
                                                    &response, error);
  }
  cai_mcp_spooled_cleanup_if_initialized(&request);
  if (rc == CAI_OK) {
    rc = cai_mcp_parse_completion_response(&response, output, error);
  }
  cai_mcp_http_response_capture_cleanup(&response);
  return rc;
}

static int cai_mcp_streamable_set_log_level(cai_mcp_client *client,
                                            const char *level,
                                            cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_mcp_http_response_capture response;
  lonejson_spooled request;
  size_t request_len;
  int rc;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP client is required");
  }
  rc = cai_mcp_client_initialize(client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&response, 0, sizeof(response));
  memset(&request, 0, sizeof(request));
  rc = cai_mcp_logging_set_level_request(impl, level, &request, &request_len,
                                         error);
  if (rc == CAI_OK) {
    rc = cai_mcp_post_request_with_session_recovery(impl, &request, request_len,
                                                    &response, error);
  }
  cai_mcp_spooled_cleanup_if_initialized(&request);
  if (rc == CAI_OK) {
    rc = cai_mcp_parse_logging_set_level_response(&response, error);
  }
  cai_mcp_http_response_capture_cleanup(&response);
  return rc;
}

static int cai_mcp_streamable_terminate_session(cai_mcp_client *client,
                                                cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_mcp_http_response_capture response;
  int rc;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP client is required");
  }
  memset(&response, 0, sizeof(response));
  rc = cai_mcp_delete_session(impl, &response, error);
  cai_mcp_http_response_capture_cleanup(&response);
  return rc;
}

static int cai_mcp_streamable_send_request(cai_mcp_client *client,
                                           const char *method,
                                           lonejson_spooled *params_json,
                                           cai_sink *output, cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_mcp_http_response_capture response;
  lonejson_spooled request;
  size_t request_len;
  int rc;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL || output == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client and output sink are required");
  }
  rc = cai_mcp_client_initialize(client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&response, 0, sizeof(response));
  memset(&request, 0, sizeof(request));
  rc = cai_mcp_generic_request(impl, method, params_json, &request,
                               &request_len, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_post_request_with_session_recovery(impl, &request, request_len,
                                                    &response, error);
  }
  cai_mcp_spooled_cleanup_if_initialized(&request);
  if (rc == CAI_OK) {
    rc = cai_mcp_parse_result_response(&response, "MCP request response",
                                       "failed to parse MCP request", output,
                                       error);
  }
  cai_mcp_http_response_capture_cleanup(&response);
  return rc;
}

static int cai_mcp_streamable_send_notification(cai_mcp_client *client,
                                                const char *method,
                                                lonejson_spooled *params_json,
                                                cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_mcp_http_response_capture response;
  lonejson_spooled notification;
  size_t notification_len;
  int rc;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP client is required");
  }
  rc = cai_mcp_client_initialize(client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&response, 0, sizeof(response));
  memset(&notification, 0, sizeof(notification));
  rc = cai_mcp_notification_request(method, params_json, &notification,
                                    &notification_len, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_post_with_session_recovery(
        impl, &notification, notification_len, 0, &response, error);
  }
  cai_mcp_spooled_cleanup_if_initialized(&notification);
  cai_mcp_http_response_capture_cleanup(&response);
  return rc;
}

static int cai_mcp_task_request(cai_mcp_streamable_http_client_impl *impl,
                                const char *method, const char *task_id,
                                const char *cursor, lonejson_spooled *spool,
                                size_t *out_len, cai_error *error) {
  int has_param;
  int rc;

  if (method == NULL || method[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP task request method is required");
  }
  if (strcmp(method, "tasks/list") != 0 &&
      (task_id == NULL || task_id[0] == '\0')) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP task id is required");
  }
  if (task_id != NULL && task_id[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP task id is required");
  }
  CAI_LJ->spooled_init(CAI_LJ, spool);
  has_param = 0;
  rc = cai_mcp_request_begin(impl, spool, ++impl->next_id, method, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, ",\"params\":{", error);
  }
  if (rc == CAI_OK && task_id != NULL) {
    rc = cai_mcp_write_cstr(spool, "\"taskId\":", error);
    if (rc == CAI_OK) {
      rc = cai_mcp_write_json_string(spool, task_id, error);
    }
    has_param = 1;
  }
  if (rc == CAI_OK && cursor != NULL && cursor[0] != '\0') {
    if (has_param) {
      rc = cai_mcp_write_cstr(spool, ",", error);
    }
    if (rc == CAI_OK) {
      rc = cai_mcp_write_cstr(spool, "\"cursor\":", error);
    }
    if (rc == CAI_OK) {
      rc = cai_mcp_write_json_string(spool, cursor, error);
    }
    has_param = 1;
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_progress_meta_member(impl, spool, has_param, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(spool, "}}", error);
  }
  if (out_len != NULL) {
    *out_len = spool->size_fn(spool);
  }
  if (rc != CAI_OK) {
    spool->cleanup(spool);
  }
  return rc;
}

static int cai_mcp_streamable_task_request(cai_mcp_client *client,
                                           const char *method,
                                           const char *task_id,
                                           const char *cursor, cai_sink *output,
                                           cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_mcp_http_response_capture response;
  lonejson_spooled request;
  size_t request_len;
  int rc;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL || output == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client and output sink are required");
  }
  rc = cai_mcp_client_initialize(client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&response, 0, sizeof(response));
  memset(&request, 0, sizeof(request));
  rc = cai_mcp_task_request(impl, method, task_id, cursor, &request,
                            &request_len, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_post_request_with_session_recovery(impl, &request, request_len,
                                                    &response, error);
  }
  cai_mcp_spooled_cleanup_if_initialized(&request);
  if (rc == CAI_OK) {
    if (strcmp(method, "tasks/list") == 0) {
      rc = cai_mcp_validate_tasks_list_response_shape(&response, error);
    } else if (strcmp(method, "tasks/get") == 0) {
      rc = cai_mcp_validate_task_response_shape(
          &response, "MCP tasks/get response", "failed to parse MCP tasks/get",
          error);
    } else if (strcmp(method, "tasks/cancel") == 0) {
      rc = cai_mcp_validate_task_response_shape(
          &response, "MCP tasks/cancel response",
          "failed to parse MCP tasks/cancel", error);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_parse_result_response(&response, "MCP task response",
                                       "failed to parse MCP task response",
                                       output, error);
  }
  cai_mcp_http_response_capture_cleanup(&response);
  return rc;
}

static int cai_mcp_streamable_drain_events(cai_mcp_client *client,
                                           cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  int rc;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP client is required");
  }
  rc = cai_mcp_client_initialize(client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_mcp_get_events(impl, error);
}

static void cai_mcp_streamable_destroy(cai_mcp_client *client) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_allocator allocator;

  impl = cai_mcp_streamable_impl(client);
  if (impl == NULL) {
    return;
  }
  allocator = impl->allocator;
  cai_mcp_client_clear_tools(impl);
  cai_mcp_client_clear_resources(impl);
  cai_mcp_client_clear_resource_templates(impl);
  cai_mcp_client_clear_prompts(impl);
  cai_free_mem(&allocator, impl->tools);
  cai_free_mem(&allocator, impl->resources);
  cai_free_mem(&allocator, impl->resource_templates);
  cai_free_mem(&allocator, impl->prompts);
  cai_free_mem(&allocator, impl->url);
  cai_free_mem(&allocator, impl->client_name);
  cai_free_mem(&allocator, impl->client_version);
  cai_free_mem(&allocator, impl->protocol_version);
  cai_free_mem(&allocator, impl->ca_bundle_path);
  cai_free_mem(&allocator, impl->ca_path);
  if (impl->receiver.cleanup != NULL) {
    impl->receiver.cleanup(impl->receiver.context);
  }
  if (impl->notification_context_cleanup != NULL &&
      impl->receiver.cleanup != NULL &&
      impl->notification_context != impl->receiver.context) {
    impl->notification_context_cleanup(impl->notification_context);
  } else if (impl->notification_context_cleanup != NULL &&
             impl->receiver.cleanup == NULL) {
    impl->notification_context_cleanup(impl->notification_context);
  }
  cai_free_mem(&allocator, impl->session_id);
  memset(impl, 0, sizeof(*impl));
  cai_free_mem(&allocator, impl);
}

void cai_mcp_streamable_http_client_config_init(
    cai_mcp_streamable_http_client_config *config) {
  if (config != NULL) {
    memset(config, 0, sizeof(*config));
  }
}

int cai_mcp_streamable_http_client_open(
    const cai_mcp_streamable_http_client_config *config, cai_mcp_client **out,
    cai_error *error) {
  cai_mcp_streamable_http_client_config defaults;
  const cai_mcp_streamable_http_client_config *effective;
  cai_mcp_streamable_http_client_impl *impl;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client output pointer is required");
  }
  *out = NULL;
  cai_mcp_streamable_http_client_config_init(&defaults);
  effective = config != NULL ? config : &defaults;
  if (effective->url == NULL || effective->url[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP Streamable HTTP URL is required");
  }
  impl = (cai_mcp_streamable_http_client_impl *)cai_alloc(&effective->allocator,
                                                          sizeof(*impl));
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate MCP client");
  }
  memset(impl, 0, sizeof(*impl));
  impl->allocator = effective->allocator;
  impl->url = cai_strdup(&impl->allocator, effective->url);
  impl->client_name = cai_strdup(
      &impl->allocator,
      effective->client_name != NULL ? effective->client_name : "cai");
  impl->client_version =
      cai_strdup(&impl->allocator, effective->client_version != NULL
                                       ? effective->client_version
                                       : CAI_VERSION_STRING);
  impl->protocol_version =
      cai_strdup(&impl->allocator, effective->protocol_version != NULL
                                       ? effective->protocol_version
                                       : CAI_MCP_PROTOCOL_VERSION);
  impl->timeout_ms = effective->timeout_ms > 0L ? effective->timeout_ms
                                                : CAI_DEFAULT_HTTP_TIMEOUT_MS;
  impl->insecure_skip_verify = effective->insecure_skip_verify;
  impl->ca_bundle_path =
      cai_strdup(&impl->allocator, effective->ca_bundle_path);
  impl->ca_path = cai_strdup(&impl->allocator, effective->ca_path);
  impl->receiver = effective->receiver;
  if (impl->receiver.notification == NULL && effective->notification != NULL) {
    impl->receiver.notification = effective->notification;
    impl->receiver.context = effective->notification_context;
  }
  impl->notification = effective->notification;
  impl->notification_context = effective->notification_context;
  impl->notification_context_cleanup = effective->notification_context_cleanup;
  if (impl->url == NULL || impl->client_name == NULL ||
      impl->client_version == NULL || impl->protocol_version == NULL ||
      (effective->ca_bundle_path != NULL && impl->ca_bundle_path == NULL) ||
      (effective->ca_path != NULL && impl->ca_path == NULL)) {
    cai_mcp_streamable_destroy(&impl->public_client);
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to copy MCP config");
  }
  impl->public_client.initialize = cai_mcp_streamable_initialize;
  impl->public_client.ping = cai_mcp_streamable_ping;
  impl->public_client.refresh_tools = cai_mcp_streamable_refresh_tools;
  impl->public_client.tool_count = cai_mcp_streamable_tool_count;
  impl->public_client.tool_at = cai_mcp_streamable_tool_at;
  impl->public_client.call_tool = cai_mcp_streamable_call_tool;
  impl->public_client.call_tool_task = cai_mcp_streamable_call_tool_task;
  impl->public_client.refresh_resources = cai_mcp_streamable_refresh_resources;
  impl->public_client.resource_count = cai_mcp_streamable_resource_count;
  impl->public_client.resource_at = cai_mcp_streamable_resource_at;
  impl->public_client.read_resource = cai_mcp_streamable_read_resource;
  impl->public_client.subscribe_resource =
      cai_mcp_streamable_subscribe_resource;
  impl->public_client.unsubscribe_resource =
      cai_mcp_streamable_unsubscribe_resource;
  impl->public_client.refresh_resource_templates =
      cai_mcp_streamable_refresh_resource_templates;
  impl->public_client.resource_template_count =
      cai_mcp_streamable_resource_template_count;
  impl->public_client.resource_template_at =
      cai_mcp_streamable_resource_template_at;
  impl->public_client.refresh_prompts = cai_mcp_streamable_refresh_prompts;
  impl->public_client.prompt_count = cai_mcp_streamable_prompt_count;
  impl->public_client.prompt_at = cai_mcp_streamable_prompt_at;
  impl->public_client.get_prompt = cai_mcp_streamable_get_prompt;
  impl->public_client.complete = cai_mcp_streamable_complete;
  impl->public_client.set_log_level = cai_mcp_streamable_set_log_level;
  impl->public_client.terminate_session = cai_mcp_streamable_terminate_session;
  impl->public_client.send_request = cai_mcp_streamable_send_request;
  impl->public_client.send_notification = cai_mcp_streamable_send_notification;
  impl->public_client.drain_events = cai_mcp_streamable_drain_events;
  impl->public_client.destroy = cai_mcp_streamable_destroy;
  impl->public_client.impl = impl;
  *out = &impl->public_client;
  return CAI_OK;
}

int cai_mcp_client_initialize(cai_mcp_client *client, cai_error *error) {
  if (client == NULL || client->initialize == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client initialize receiver is required");
  }
  return client->initialize(client, error);
}

int cai_mcp_client_ping(cai_mcp_client *client, cai_error *error) {
  if (client == NULL || client->ping == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client ping receiver is required");
  }
  return client->ping(client, error);
}

int cai_mcp_client_refresh_tools(cai_mcp_client *client, cai_error *error) {
  if (client == NULL || client->refresh_tools == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client refresh_tools receiver is required");
  }
  return client->refresh_tools(client, error);
}

size_t cai_mcp_client_tool_count(const cai_mcp_client *client) {
  return client != NULL && client->tool_count != NULL
             ? client->tool_count(client)
             : 0U;
}

const cai_mcp_client_tool *cai_mcp_client_tool_at(const cai_mcp_client *client,
                                                  size_t index) {
  return client != NULL && client->tool_at != NULL
             ? client->tool_at(client, index)
             : NULL;
}

int cai_mcp_client_call_tool(cai_mcp_client *client, const char *name,
                             lonejson_spooled *arguments_json, cai_sink *output,
                             cai_error *error) {
  if (client == NULL || client->call_tool == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client call_tool receiver is required");
  }
  return client->call_tool(client, name, arguments_json, output, error);
}

int cai_mcp_client_call_tool_task(cai_mcp_client *client, const char *name,
                                  lonejson_spooled *arguments_json,
                                  long long ttl_ms, cai_sink *output,
                                  cai_error *error) {
  if (client == NULL || client->call_tool_task == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client call_tool_task receiver is required");
  }
  return client->call_tool_task(client, name, arguments_json, ttl_ms, output,
                                error);
}

int cai_mcp_client_refresh_resources(cai_mcp_client *client, cai_error *error) {
  if (client == NULL || client->refresh_resources == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client refresh_resources receiver is required");
  }
  return client->refresh_resources(client, error);
}

size_t cai_mcp_client_resource_count(const cai_mcp_client *client) {
  return client != NULL && client->resource_count != NULL
             ? client->resource_count(client)
             : 0U;
}

const cai_mcp_client_resource *
cai_mcp_client_resource_at(const cai_mcp_client *client, size_t index) {
  return client != NULL && client->resource_at != NULL
             ? client->resource_at(client, index)
             : NULL;
}

int cai_mcp_client_read_resource(cai_mcp_client *client, const char *uri,
                                 cai_sink *output, cai_error *error) {
  if (client == NULL || client->read_resource == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client read_resource receiver is required");
  }
  return client->read_resource(client, uri, output, error);
}

int cai_mcp_client_subscribe_resource(cai_mcp_client *client, const char *uri,
                                      cai_error *error) {
  if (client == NULL || client->subscribe_resource == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client subscribe_resource receiver is required");
  }
  return client->subscribe_resource(client, uri, error);
}

int cai_mcp_client_unsubscribe_resource(cai_mcp_client *client, const char *uri,
                                        cai_error *error) {
  if (client == NULL || client->unsubscribe_resource == NULL) {
    return cai_set_error(
        error, CAI_ERR_INVALID,
        "MCP client unsubscribe_resource receiver is required");
  }
  return client->unsubscribe_resource(client, uri, error);
}

int cai_mcp_client_refresh_resource_templates(cai_mcp_client *client,
                                              cai_error *error) {
  if (client == NULL || client->refresh_resource_templates == NULL) {
    return cai_set_error(
        error, CAI_ERR_INVALID,
        "MCP client refresh_resource_templates receiver is required");
  }
  return client->refresh_resource_templates(client, error);
}

size_t cai_mcp_client_resource_template_count(const cai_mcp_client *client) {
  return client != NULL && client->resource_template_count != NULL
             ? client->resource_template_count(client)
             : 0U;
}

const cai_mcp_client_resource_template *
cai_mcp_client_resource_template_at(const cai_mcp_client *client,
                                    size_t index) {
  return client != NULL && client->resource_template_at != NULL
             ? client->resource_template_at(client, index)
             : NULL;
}

int cai_mcp_client_refresh_prompts(cai_mcp_client *client, cai_error *error) {
  if (client == NULL || client->refresh_prompts == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client refresh_prompts receiver is required");
  }
  return client->refresh_prompts(client, error);
}

size_t cai_mcp_client_prompt_count(const cai_mcp_client *client) {
  return client != NULL && client->prompt_count != NULL
             ? client->prompt_count(client)
             : 0U;
}

const cai_mcp_client_prompt *
cai_mcp_client_prompt_at(const cai_mcp_client *client, size_t index) {
  return client != NULL && client->prompt_at != NULL
             ? client->prompt_at(client, index)
             : NULL;
}

int cai_mcp_client_get_prompt(cai_mcp_client *client, const char *name,
                              lonejson_spooled *arguments_json,
                              cai_sink *output, cai_error *error) {
  if (client == NULL || client->get_prompt == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client get_prompt receiver is required");
  }
  return client->get_prompt(client, name, arguments_json, output, error);
}

int cai_mcp_client_complete(cai_mcp_client *client, const char *ref_type,
                            const char *ref_value, const char *argument_name,
                            const char *argument_value,
                            lonejson_spooled *context_arguments_json,
                            cai_sink *output, cai_error *error) {
  if (client == NULL || client->complete == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client complete receiver is required");
  }
  return client->complete(client, ref_type, ref_value, argument_name,
                          argument_value, context_arguments_json, output,
                          error);
}

int cai_mcp_client_set_log_level(cai_mcp_client *client, const char *level,
                                 cai_error *error) {
  if (client == NULL || client->set_log_level == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client set_log_level receiver is required");
  }
  return client->set_log_level(client, level, error);
}

int cai_mcp_client_terminate_session(cai_mcp_client *client, cai_error *error) {
  if (client == NULL || client->terminate_session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client terminate_session receiver is required");
  }
  return client->terminate_session(client, error);
}

int cai_mcp_client_send_request(cai_mcp_client *client, const char *method,
                                lonejson_spooled *params_json, cai_sink *output,
                                cai_error *error) {
  if (client == NULL || client->send_request == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client send_request receiver is required");
  }
  return client->send_request(client, method, params_json, output, error);
}

int cai_mcp_client_send_notification(cai_mcp_client *client, const char *method,
                                     lonejson_spooled *params_json,
                                     cai_error *error) {
  if (client == NULL || client->send_notification == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client send_notification receiver is required");
  }
  return client->send_notification(client, method, params_json, error);
}

int cai_mcp_client_notify_roots_list_changed(cai_mcp_client *client,
                                             cai_error *error) {
  return cai_mcp_client_send_notification(
      client, "notifications/roots/list_changed", NULL, error);
}

static int cai_mcp_client_is_streamable_builtin(cai_mcp_client *client) {
  return client != NULL && client->impl != NULL &&
         client->initialize == cai_mcp_streamable_initialize &&
         client->send_request == cai_mcp_streamable_send_request &&
         client->destroy == cai_mcp_streamable_destroy;
}

static int cai_mcp_task_id_params(const char *task_id, lonejson_spooled *params,
                                  cai_error *error) {
  int rc;

  if (task_id == NULL || task_id[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP task id is required");
  }
  CAI_LJ->spooled_init(CAI_LJ, params);
  rc = cai_mcp_write_cstr(params, "{\"taskId\":", error);
  if (rc == CAI_OK) {
    rc = cai_mcp_write_json_string(params, task_id, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(params, "}", error);
  }
  if (rc != CAI_OK) {
    params->cleanup(params);
  }
  return rc;
}

static int cai_mcp_task_list_params(const char *cursor,
                                    lonejson_spooled *params,
                                    cai_error *error) {
  int rc;

  CAI_LJ->spooled_init(CAI_LJ, params);
  rc = cai_mcp_write_cstr(params, "{", error);
  if (rc == CAI_OK && cursor != NULL && cursor[0] != '\0') {
    rc = cai_mcp_write_cstr(params, "\"cursor\":", error);
    if (rc == CAI_OK) {
      rc = cai_mcp_write_json_string(params, cursor, error);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(params, "}", error);
  }
  if (rc != CAI_OK) {
    params->cleanup(params);
  }
  return rc;
}

int cai_mcp_client_list_tasks(cai_mcp_client *client, const char *cursor,
                              cai_sink *output, cai_error *error) {
  lonejson_spooled params;
  int rc;

  if (cai_mcp_client_is_streamable_builtin(client)) {
    return cai_mcp_streamable_task_request(client, "tasks/list", NULL, cursor,
                                           output, error);
  }
  rc = cai_mcp_task_list_params(cursor, &params, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_client_send_request(client, "tasks/list", &params, output,
                                     error);
    params.cleanup(&params);
  }
  return rc;
}

int cai_mcp_client_get_task(cai_mcp_client *client, const char *task_id,
                            cai_sink *output, cai_error *error) {
  lonejson_spooled params;
  int rc;

  if (cai_mcp_client_is_streamable_builtin(client)) {
    return cai_mcp_streamable_task_request(client, "tasks/get", task_id, NULL,
                                           output, error);
  }
  rc = cai_mcp_task_id_params(task_id, &params, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_client_send_request(client, "tasks/get", &params, output,
                                     error);
    params.cleanup(&params);
  }
  return rc;
}

int cai_mcp_client_get_task_result(cai_mcp_client *client, const char *task_id,
                                   cai_sink *output, cai_error *error) {
  lonejson_spooled params;
  int rc;

  if (cai_mcp_client_is_streamable_builtin(client)) {
    return cai_mcp_streamable_task_request(client, "tasks/result", task_id,
                                           NULL, output, error);
  }
  rc = cai_mcp_task_id_params(task_id, &params, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_client_send_request(client, "tasks/result", &params, output,
                                     error);
    params.cleanup(&params);
  }
  return rc;
}

int cai_mcp_client_cancel_task(cai_mcp_client *client, const char *task_id,
                               cai_sink *output, cai_error *error) {
  lonejson_spooled params;
  int rc;

  if (cai_mcp_client_is_streamable_builtin(client)) {
    return cai_mcp_streamable_task_request(client, "tasks/cancel", task_id,
                                           NULL, output, error);
  }
  rc = cai_mcp_task_id_params(task_id, &params, error);
  if (rc == CAI_OK) {
    rc = cai_mcp_client_send_request(client, "tasks/cancel", &params, output,
                                     error);
    params.cleanup(&params);
  }
  return rc;
}

int cai_mcp_client_drain_events(cai_mcp_client *client, cai_error *error) {
  if (client == NULL || client->drain_events == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client drain_events receiver is required");
  }
  return client->drain_events(client, error);
}

static int cai_mcp_registered_tool_callback(void *context,
                                            lonejson_spooled *arguments_json,
                                            cai_sink *output,
                                            cai_error *error) {
  cai_mcp_registry_tool_context *tool_context;

  tool_context = (cai_mcp_registry_tool_context *)context;
  if (tool_context == NULL || tool_context->client == NULL ||
      tool_context->remote_name == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP registered tool context is invalid");
  }
  return cai_mcp_client_call_tool(tool_context->client,
                                  tool_context->remote_name, arguments_json,
                                  output, error);
}

static void cai_mcp_registered_tool_context_cleanup(void *context) {
  cai_mcp_registry_tool_context *tool_context;

  tool_context = (cai_mcp_registry_tool_context *)context;
  if (tool_context != NULL) {
    cai_free_mem(NULL, tool_context->remote_name);
    cai_free_mem(NULL, tool_context);
  }
}

int cai_mcp_client_register_tools(
    cai_mcp_client *client, cai_tool_registry *registry,
    const cai_mcp_tool_registration_config *config, cai_error *error) {
  cai_mcp_tool_registration_config defaults;
  const cai_mcp_tool_registration_config *effective;
  const cai_mcp_client_tool *tool;
  cai_mcp_registry_tool_context *context;
  cai_buffer_builder name_builder;
  size_t i;
  int rc;

  if (client == NULL || registry == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client and tool registry are required");
  }
  memset(&defaults, 0, sizeof(defaults));
  effective = config != NULL ? config : &defaults;
  rc = cai_mcp_client_refresh_tools(client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  for (i = 0U; i < cai_mcp_client_tool_count(client); i++) {
    tool = cai_mcp_client_tool_at(client, i);
    if (tool == NULL || tool->name == NULL || tool->input_schema_json == NULL) {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP tool metadata is incomplete");
    }
    memset(&name_builder, 0, sizeof(name_builder));
    if (effective->name_prefix != NULL) {
      rc = cai_buffer_append_cstr(&name_builder, effective->name_prefix, error);
      if (rc != CAI_OK) {
        cai_free_mem(NULL, name_builder.data);
        return rc;
      }
    }
    rc = cai_buffer_append_cstr(&name_builder, tool->name, error);
    if (rc == CAI_OK) {
      rc = cai_buffer_append(&name_builder, "", 1U, error);
    }
    if (rc != CAI_OK) {
      cai_free_mem(NULL, name_builder.data);
      return rc;
    }
    context =
        (cai_mcp_registry_tool_context *)cai_alloc(NULL, sizeof(*context));
    if (context == NULL) {
      cai_free_mem(NULL, name_builder.data);
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate MCP tool context");
    }
    memset(context, 0, sizeof(*context));
    context->client = client;
    context->remote_name = cai_strdup(NULL, tool->name);
    if (context->remote_name == NULL) {
      cai_free_mem(NULL, context);
      cai_free_mem(NULL, name_builder.data);
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate MCP tool name");
    }
    rc = cai_tool_registry_register_raw_spooled_owned(
        registry, name_builder.data, tool->description, tool->input_schema_json,
        effective->strict, cai_mcp_registered_tool_callback, context,
        cai_mcp_registered_tool_context_cleanup, error);
    cai_free_mem(NULL, name_builder.data);
    if (rc != CAI_OK) {
      cai_mcp_registered_tool_context_cleanup(context);
      return rc;
    }
  }
  return CAI_OK;
}

int cai_agent_register_mcp_client_tools(
    cai_agent *agent, cai_mcp_client *client,
    const cai_mcp_tool_registration_config *config, cai_error *error) {
  cai_agent_impl *impl;

  if (agent == NULL || client == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "agent and MCP client are required");
  }
  impl = CAI_AGENT_IMPL(agent);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is closed");
  }
  return cai_mcp_client_register_tools(client, impl->tools, config, error);
}

void cai_mcp_client_destroy(cai_mcp_client *client) {
  if (client != NULL && client->destroy != NULL) {
    client->destroy(client);
  }
}
