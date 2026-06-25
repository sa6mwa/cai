#include "cai_internal.h"

#include <cai/mcp.h>

#include <pslog.h>

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#if defined(__GNUC__)
#define CAI_MCP_UNUSED_FN __attribute__((unused))
#else
#define CAI_MCP_UNUSED_FN
#endif

typedef struct cai_mcp_client_tool_impl {
  cai_mcp_client_tool public_tool;
  char *name;
  char *title;
  char *description;
  cai_mcp_client_schema input_schema;
  cai_mcp_client_schema output_schema;
  char *input_schema_uri;
  char *output_schema_uri;
  char *input_schema_type;
  char *output_schema_type;
  char **input_schema_properties;
  char **input_schema_required;
  char **output_schema_properties;
  char **output_schema_required;
  cai_mcp_client_icon *icons;
  cai_mcp_client_tool_annotations annotations;
  char *annotations_title;
  char *input_schema_json;
  char *output_schema_json;
} cai_mcp_client_tool_impl;

typedef struct cai_mcp_client_resource_impl {
  cai_mcp_client_resource public_resource;
  char *uri;
  char *name;
  char *title;
  char *description;
  char *mime_type;
  cai_mcp_client_icon *icons;
  cai_mcp_client_annotations annotations;
  char **annotation_audience;
  char *annotation_last_modified;
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
  cai_mcp_client_icon *icons;
  cai_mcp_client_annotations annotations;
  char **annotation_audience;
  char *annotation_last_modified;
} cai_mcp_client_resource_template_impl;

typedef struct cai_mcp_client_prompt_impl {
  cai_mcp_client_prompt public_prompt;
  char *name;
  char *title;
  char *description;
  cai_mcp_client_prompt_argument *arguments;
  cai_mcp_client_icon *icons;
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
  struct pslog_logger *logger;
  int logger_disabled;
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

static pslog_logger *
cai_mcp_logger(const cai_mcp_streamable_http_client_impl *impl) {
  if (impl == NULL || impl->logger_disabled) {
    return NULL;
  }
  return impl->logger;
}

static void
cai_mcp_log_client_opened(const cai_mcp_streamable_http_client_impl *impl) {
  pslog_logger *log;

  log = cai_mcp_logger(impl);
  if (log == NULL || log->infof == NULL) {
    return;
  }
  log->infof(log, "cai.mcp.client.opened",
             "url=%s timeout_ms=%d verify_tls=%b protocol_version=%s",
             impl->url != NULL ? impl->url : "", (int)impl->timeout_ms,
             impl->insecure_skip_verify ? 0 : 1,
             impl->protocol_version != NULL ? impl->protocol_version : "");
}

static void
cai_mcp_log_http_request_start(const cai_mcp_streamable_http_client_impl *impl,
                               const char *method, int stream,
                               size_t request_bytes) {
  pslog_logger *log;

  log = cai_mcp_logger(impl);
  if (log == NULL || log->tracef == NULL) {
    return;
  }
  log->tracef(
      log, "cai.mcp.http.request.start", "method=%s stream=%b request_bytes=%u",
      method != NULL ? method : "", stream, (unsigned int)request_bytes);
}

static void
cai_mcp_log_http_request_done(const cai_mcp_streamable_http_client_impl *impl,
                              const char *method, long http_status,
                              int stream) {
  pslog_logger *log;

  log = cai_mcp_logger(impl);
  if (log == NULL) {
    return;
  }
  if (http_status >= 500L && log->errorf != NULL) {
    log->errorf(log, "cai.mcp.http.request.done",
                "method=%s status=%d stream=%b", method != NULL ? method : "",
                (int)http_status, stream);
  } else if (http_status >= 400L && log->warnf != NULL) {
    log->warnf(log, "cai.mcp.http.request.done",
               "method=%s status=%d stream=%b", method != NULL ? method : "",
               (int)http_status, stream);
  } else if (log->debugf != NULL) {
    log->debugf(log, "cai.mcp.http.request.done",
                "method=%s status=%d stream=%b", method != NULL ? method : "",
                (int)http_status, stream);
  }
}

static void cai_mcp_log_http_transport_error(
    const cai_mcp_streamable_http_client_impl *impl, const char *method,
    const char *detail) {
  pslog_logger *log;

  log = cai_mcp_logger(impl);
  if (log == NULL || log->errorf == NULL) {
    return;
  }
  log->errorf(log, "cai.mcp.http.transport_error", "method=%s error=%s",
              method != NULL ? method : "", detail != NULL ? detail : "");
}

static void
cai_mcp_client_clear_tools(cai_mcp_streamable_http_client_impl *impl);
static void
cai_mcp_client_clear_resources(cai_mcp_streamable_http_client_impl *impl);
static void cai_mcp_client_clear_resource_templates(
    cai_mcp_streamable_http_client_impl *impl);
static void
cai_mcp_client_clear_prompts(cai_mcp_streamable_http_client_impl *impl);

static int cai_mcp_allocator_is_empty(const cai_allocator *allocator) {
  return allocator->malloc_fn == NULL && allocator->realloc_fn == NULL &&
         allocator->free_fn == NULL;
}

static int cai_mcp_allocator_is_complete(const cai_allocator *allocator) {
  return allocator->malloc_fn != NULL && allocator->realloc_fn != NULL &&
         allocator->free_fn != NULL;
}

typedef struct cai_mcp_http_response_capture {
  lonejson_spooled body;
  char *content_type;
  char *session_id;
  long status;
} cai_mcp_http_response_capture;

typedef struct cai_mcp_response_write_context {
  cai_mcp_http_response_capture *response;
} cai_mcp_response_write_context;

typedef struct cai_mcp_pipe_reader {
  int fd;
} cai_mcp_pipe_reader;

typedef struct cai_mcp_stream_char_reader {
  lonejson_read_result (*read_fn)(void *, unsigned char *, size_t);
  void *read_user;
  int has_pushback;
  int pushback;
} cai_mcp_stream_char_reader;

typedef struct cai_mcp_result_sink_context {
  cai_sink *sink;
  cai_error *error;
  int rc;
  size_t bytes_written;
} cai_mcp_result_sink_context;

typedef struct cai_mcp_sse_resume_state {
  char *last_event_id;
  long retry_ms;
  int has_retry;
} cai_mcp_sse_resume_state;

typedef struct cai_mcp_streaming_response_context {
  cai_mcp_http_response_capture *response;
  cai_buffer_builder line;
  cai_buffer_builder pending_data;
  cai_mcp_sse_resume_state resume;
  int write_fd;
  int rc;
  char event[64];
  int data_started;
  int done;
  int data_line_active;
  int data_line_streaming;
  int data_line_skip_space;
  int data_line_pending_cr;
  int data_line_pending_newline;
  int pending_data_started;
} cai_mcp_streaming_response_context;

typedef struct cai_mcp_streaming_post_context {
  cai_mcp_streamable_http_client_impl *impl;
  const lonejson_spooled *request;
  size_t request_len;
  cai_mcp_http_response_capture *response;
  const char *last_event_id;
  char *next_last_event_id;
  long next_retry_ms;
  int write_fd;
  int is_request;
  int has_next_retry;
  CURLcode curl_rc;
  int rc;
} cai_mcp_streaming_post_context;

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

typedef struct cai_mcp_icon_doc {
  char *src;
  lonejson_string_array sizes;
  char *mime_type;
  char *theme;
} cai_mcp_icon_doc;

typedef struct cai_mcp_icons_doc {
  lonejson_object_array icons;
} cai_mcp_icons_doc;

typedef struct cai_mcp_prompt_argument_doc {
  char *name;
  char *title;
  char *description;
  int has_required;
  int required;
} cai_mcp_prompt_argument_doc;

typedef struct cai_mcp_prompt_arguments_doc {
  lonejson_object_array arguments;
} cai_mcp_prompt_arguments_doc;

typedef struct cai_mcp_tool_annotations_doc {
  char *title;
  int has_read_only_hint;
  int read_only_hint;
  int has_destructive_hint;
  int destructive_hint;
  int has_idempotent_hint;
  int idempotent_hint;
  int has_open_world_hint;
  int open_world_hint;
} cai_mcp_tool_annotations_doc;

typedef struct cai_mcp_annotations_doc {
  lonejson_string_array audience;
  char *last_modified;
  int has_priority;
  double priority;
} cai_mcp_annotations_doc;

typedef struct cai_mcp_tool_execution_doc {
  char *task_support;
} cai_mcp_tool_execution_doc;

typedef struct cai_mcp_json_schema_doc {
  char *schema;
  char *type;
  lonejson_json_value properties;
  lonejson_json_value required;
} cai_mcp_json_schema_doc;

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
  char *title;
  char *description;
  lonejson_json_value icons;
  lonejson_json_value annotations;
  lonejson_json_value resource;
  int has_size;
  int64_t size;
} cai_mcp_tool_content_doc;

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
  char *title;
  char *version;
  char *description;
  char *website_url;
  lonejson_json_value icons;
} cai_mcp_server_info_doc;

typedef struct cai_mcp_initialize_result_doc {
  char *protocol_version;
  lonejson_json_value capabilities;
  cai_mcp_server_info_doc server_info;
  char *instructions;
} cai_mcp_initialize_result_doc;

typedef struct cai_mcp_initialize_response_doc {
  cai_mcp_initialize_result_doc result;
  cai_mcp_jsonrpc_error_doc error_doc;
} cai_mcp_initialize_response_doc;

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

typedef struct cai_mcp_registry_tool_context {
  cai_mcp_client *client;
  char *remote_name;
} cai_mcp_registry_tool_context;

static void
cai_mcp_streamable_reset_session(cai_mcp_streamable_http_client_impl *impl);
static void
cai_mcp_http_response_capture_cleanup(cai_mcp_http_response_capture *response);
static void cai_mcp_spooled_cleanup_if_initialized(lonejson_spooled *spool);
static int cai_mcp_set_json_error(cai_error *error, const char *message,
                                  const lonejson_error *json_error);
static int cai_mcp_json_value_is_object(const lonejson_json_value *value,
                                        cai_error *error);
static int cai_mcp_json_value_root_is(const lonejson_json_value *value,
                                      char root, cai_error *error);
static int cai_mcp_spooled_root_is(const lonejson_spooled *json, int root,
                                   const char *message, cai_error *error);
static int cai_mcp_spooled_object_string_values(const lonejson_spooled *json,
                                                const char *object_error,
                                                const char *value_error,
                                                const char *parse_error,
                                                cai_error *error);
static int cai_mcp_log_level_valid(const char *level);
static int cai_mcp_prompt_role_is_valid(const char *role);
static int cai_mcp_validate_optional_icons(const lonejson_json_value *icons,
                                           const char *array_error_message,
                                           const char *parse_error_message,
                                           const char *theme_error_message,
                                           cai_error *error);
static int cai_mcp_validate_jsonrpc_id_value(const lonejson_json_value *id,
                                             cai_error *error);
static char *
cai_mcp_json_value_to_cstr_with_allocator(const cai_allocator *allocator,
                                          const lonejson_json_value *value,
                                          cai_error *error);
static char *cai_mcp_json_value_to_cstr(const lonejson_json_value *value,
                                        cai_error *error);
static void cai_mcp_string_array_cleanup(const cai_allocator *allocator,
                                         char **items, size_t count);
static void cai_mcp_client_icons_cleanup(const cai_allocator *allocator,
                                         cai_mcp_client_icon *icons,
                                         size_t count);
static void cai_mcp_client_schema_cleanup(const cai_allocator *allocator,
                                          cai_mcp_client_schema *schema,
                                          char **schema_uri, char **type,
                                          char ***properties, char ***required);
static void cai_mcp_client_prompt_arguments_cleanup(
    const cai_allocator *allocator, cai_mcp_client_prompt_argument *arguments,
    size_t count);
static int
cai_mcp_jsonrpc_response_result_error_presence(const lonejson_spooled *json,
                                               int *has_result, int *has_error,
                                               cai_error *error);
static int cai_mcp_jsonrpc_top_level_member_presence(
    const lonejson_spooled *json, const char *member, int *present,
    const char *message, cai_error *error);
static int cai_mcp_jsonrpc_response_result_root_is_object(
    const lonejson_spooled *json, int *is_object, cai_error *error);
static int cai_mcp_spool_copy(const lonejson_spooled *src,
                              lonejson_spooled *dst, cai_error *error);
static int
cai_mcp_validate_response_envelope(const lonejson_spooled *request,
                                   const cai_mcp_http_response_capture *reply,
                                   cai_error *error);
static int cai_mcp_write_json_string(lonejson_spooled *spool, const char *text,
                                     cai_error *error);
static int cai_mcp_sse_line_is_blank(const char *line, size_t len);
static void cai_mcp_sse_set_event(char *event, size_t event_size,
                                  const char *value, size_t len);
static int cai_mcp_sse_set_last_event_id(char **last_event_id,
                                         const char *value, size_t len,
                                         cai_error *error);
static void cai_mcp_sse_set_retry(cai_mcp_sse_resume_state *resume,
                                  const char *value, size_t len);
static int
cai_mcp_sse_normalize_response(cai_mcp_streamable_http_client_impl *impl,
                               cai_mcp_http_response_capture *response,
                               int allow_resume, cai_error *error);
static int cai_mcp_get_resume_response(
    cai_mcp_streamable_http_client_impl *impl, const char *last_event_id,
    cai_mcp_http_response_capture *response, cai_error *error);
static int cai_mcp_append_last_event_id_header(struct curl_slist **headers,
                                               const char *last_event_id,
                                               cai_error *error);
#ifdef CAI_TESTING
int cai_mcp_test_header_callback_unterminated(void);
#endif

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

static const lonejson_field cai_mcp_icon_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_icon_doc, src, "src"),
    {"sizes", LONEJSON__KEY_LEN("sizes"), LONEJSON__KEY_FIRST("sizes"),
     LONEJSON__KEY_LAST("sizes"), offsetof(cai_mcp_icon_doc, sizes),
     LONEJSON_FIELD_KIND_STRING_ARRAY, LONEJSON_STORAGE_DYNAMIC,
     LONEJSON_OVERFLOW_FAIL, 0U, 0U, 0U, NULL, NULL, 0U,
     LONEJSON_SPOOL_CLASS_DEFAULT},
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_icon_doc, mime_type, "mimeType"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_icon_doc, theme, "theme")};
LONEJSON_MAP_DEFINE(cai_mcp_icon_map, cai_mcp_icon_doc, cai_mcp_icon_fields);

static const lonejson_field cai_mcp_icons_fields[] = {
    {"icons", LONEJSON__KEY_LEN("icons"), LONEJSON__KEY_FIRST("icons"),
     LONEJSON__KEY_LAST("icons"), offsetof(cai_mcp_icons_doc, icons),
     LONEJSON_FIELD_KIND_OBJECT_ARRAY, LONEJSON_STORAGE_DYNAMIC,
     LONEJSON_OVERFLOW_FAIL, LONEJSON_FIELD_REQUIRED, 0U,
     sizeof(cai_mcp_icon_doc), &cai_mcp_icon_map, NULL, 0U,
     LONEJSON_SPOOL_CLASS_DEFAULT}};
LONEJSON_MAP_DEFINE(cai_mcp_icons_map, cai_mcp_icons_doc, cai_mcp_icons_fields);

static const lonejson_field cai_mcp_prompt_argument_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_prompt_argument_doc, name, "name"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_prompt_argument_doc, title, "title"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_prompt_argument_doc, description,
                                "description"),
    LONEJSON_FIELD_BOOL_PRESENT(cai_mcp_prompt_argument_doc, required,
                                has_required, "required")};
LONEJSON_MAP_DEFINE(cai_mcp_prompt_argument_map, cai_mcp_prompt_argument_doc,
                    cai_mcp_prompt_argument_fields);

static const lonejson_field cai_mcp_prompt_arguments_fields[] = {
    {"arguments", LONEJSON__KEY_LEN("arguments"),
     LONEJSON__KEY_FIRST("arguments"), LONEJSON__KEY_LAST("arguments"),
     offsetof(cai_mcp_prompt_arguments_doc, arguments),
     LONEJSON_FIELD_KIND_OBJECT_ARRAY, LONEJSON_STORAGE_DYNAMIC,
     LONEJSON_OVERFLOW_FAIL, LONEJSON_FIELD_REQUIRED, 0U,
     sizeof(cai_mcp_prompt_argument_doc), &cai_mcp_prompt_argument_map, NULL,
     0U, LONEJSON_SPOOL_CLASS_DEFAULT}};
LONEJSON_MAP_DEFINE(cai_mcp_prompt_arguments_map, cai_mcp_prompt_arguments_doc,
                    cai_mcp_prompt_arguments_fields);

static const lonejson_field cai_mcp_tool_annotations_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_tool_annotations_doc, title, "title"),
    LONEJSON_FIELD_BOOL_PRESENT(cai_mcp_tool_annotations_doc, read_only_hint,
                                has_read_only_hint, "readOnlyHint"),
    LONEJSON_FIELD_BOOL_PRESENT(cai_mcp_tool_annotations_doc, destructive_hint,
                                has_destructive_hint, "destructiveHint"),
    LONEJSON_FIELD_BOOL_PRESENT(cai_mcp_tool_annotations_doc, idempotent_hint,
                                has_idempotent_hint, "idempotentHint"),
    LONEJSON_FIELD_BOOL_PRESENT(cai_mcp_tool_annotations_doc, open_world_hint,
                                has_open_world_hint, "openWorldHint")};
LONEJSON_MAP_DEFINE(cai_mcp_tool_annotations_map, cai_mcp_tool_annotations_doc,
                    cai_mcp_tool_annotations_fields);

static const lonejson_field cai_mcp_annotations_fields[] = {
    {"audience", LONEJSON__KEY_LEN("audience"), LONEJSON__KEY_FIRST("audience"),
     LONEJSON__KEY_LAST("audience"),
     offsetof(cai_mcp_annotations_doc, audience),
     LONEJSON_FIELD_KIND_STRING_ARRAY, LONEJSON_STORAGE_DYNAMIC,
     LONEJSON_OVERFLOW_FAIL, 0U, 0U, 0U, NULL, NULL, 0U,
     LONEJSON_SPOOL_CLASS_DEFAULT},
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_annotations_doc, last_modified,
                                "lastModified"),
    LONEJSON_FIELD_F64_PRESENT(cai_mcp_annotations_doc, priority, has_priority,
                               "priority")};
LONEJSON_MAP_DEFINE(cai_mcp_annotations_map, cai_mcp_annotations_doc,
                    cai_mcp_annotations_fields);

static const lonejson_field cai_mcp_tool_execution_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_tool_execution_doc, task_support,
                                "taskSupport")};
LONEJSON_MAP_DEFINE(cai_mcp_tool_execution_map, cai_mcp_tool_execution_doc,
                    cai_mcp_tool_execution_fields);

static const lonejson_field cai_mcp_json_schema_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_json_schema_doc, schema, "$schema"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_json_schema_doc, type, "type"),
    {"properties", LONEJSON__KEY_LEN("properties"),
     LONEJSON__KEY_FIRST("properties"), LONEJSON__KEY_LAST("properties"),
     offsetof(cai_mcp_json_schema_doc, properties),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT},
    {"required", LONEJSON__KEY_LEN("required"), LONEJSON__KEY_FIRST("required"),
     LONEJSON__KEY_LAST("required"),
     offsetof(cai_mcp_json_schema_doc, required),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT}};
LONEJSON_MAP_DEFINE(cai_mcp_json_schema_map, cai_mcp_json_schema_doc,
                    cai_mcp_json_schema_fields);

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
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_tool_content_doc, title, "title"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_tool_content_doc, description,
                                "description"),
    {"icons", LONEJSON__KEY_LEN("icons"), LONEJSON__KEY_FIRST("icons"),
     LONEJSON__KEY_LAST("icons"), offsetof(cai_mcp_tool_content_doc, icons),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT},
    {"annotations", LONEJSON__KEY_LEN("annotations"),
     LONEJSON__KEY_FIRST("annotations"), LONEJSON__KEY_LAST("annotations"),
     offsetof(cai_mcp_tool_content_doc, annotations),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT},
    {"resource", LONEJSON__KEY_LEN("resource"), LONEJSON__KEY_FIRST("resource"),
     LONEJSON__KEY_LAST("resource"),
     offsetof(cai_mcp_tool_content_doc, resource),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT},
    LONEJSON_FIELD_I64_PRESENT(cai_mcp_tool_content_doc, size, has_size,
                               "size")};
LONEJSON_MAP_DEFINE(cai_mcp_tool_content_map, cai_mcp_tool_content_doc,
                    cai_mcp_tool_content_fields);

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
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_server_info_doc, title, "title"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_mcp_server_info_doc, version,
                                    "version"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_server_info_doc, description,
                                "description"),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_server_info_doc, website_url,
                                "websiteUrl"),
    {"icons", LONEJSON__KEY_LEN("icons"), LONEJSON__KEY_FIRST("icons"),
     LONEJSON__KEY_LAST("icons"), offsetof(cai_mcp_server_info_doc, icons),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL, LONEJSON__FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U, 0U,
     NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT}};
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
                              "serverInfo", &cai_mcp_server_info_map),
    LONEJSON_FIELD_STRING_ALLOC(cai_mcp_initialize_result_doc, instructions,
                                "instructions")};
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

static int cai_mcp_header_is(const char *line, size_t line_len,
                             const char *name, size_t name_len) {
  return line_len > name_len && line[name_len] == ':' &&
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

static int cai_mcp_parse_http_status_line(const char *line, size_t len,
                                          long *out_status) {
  size_t i;
  long status;

  if (line == NULL || out_status == NULL || len < 8U ||
      !cai_mcp_ascii_ieq_n(line, "HTTP/", 5U)) {
    return 0;
  }
  i = 5U;
  while (i < len && line[i] != ' ') {
    i++;
  }
  while (i < len && line[i] == ' ') {
    i++;
  }
  if (i + 2U >= len || !isdigit((unsigned char)line[i]) ||
      !isdigit((unsigned char)line[i + 1U]) ||
      !isdigit((unsigned char)line[i + 2U])) {
    return 0;
  }
  status = (long)(line[i] - '0') * 100L + (long)(line[i + 1U] - '0') * 10L +
           (long)(line[i + 2U] - '0');
  *out_status = status;
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
  if (cai_mcp_parse_http_status_line(buffer, len, &capture->status)) {
    return len;
  }
  if (cai_mcp_header_is(buffer, len, "content-type", strlen("content-type"))) {
    cai_free_mem(NULL, capture->content_type);
    capture->content_type =
        cai_mcp_trim_header_value(NULL, buffer + strlen("content-type") + 1U,
                                  len - strlen("content-type") - 1U);
  } else if (cai_mcp_header_is(buffer, len, "mcp-session-id",
                               strlen("mcp-session-id"))) {
    cai_free_mem(NULL, capture->session_id);
    capture->session_id =
        cai_mcp_trim_header_value(NULL, buffer + strlen("mcp-session-id") + 1U,
                                  len - strlen("mcp-session-id") - 1U);
  }
  return len;
}

#ifdef CAI_TESTING
int cai_mcp_test_header_callback_unterminated(void) {
  static const char header_text[] = "Content-Type: application/json\r\n";
  cai_mcp_http_response_capture capture;
  char *header;
  size_t len;
  size_t written;
  int ok;

  len = sizeof(header_text) - 1U;
  header = (char *)cai_alloc(NULL, len);
  if (header == NULL) {
    return 0;
  }
  memcpy(header, header_text, len);
  memset(&capture, 0, sizeof(capture));
  written = cai_mcp_header_callback(header, 1U, len, &capture);
  ok = written == len && capture.content_type != NULL &&
       strcmp(capture.content_type, "application/json") == 0;
  cai_mcp_http_response_capture_cleanup(&capture);
  cai_free_mem(NULL, header);
  return ok;
}
#endif

static int cai_mcp_response_is_json(const cai_mcp_http_response_capture *res);
static int cai_mcp_response_is_sse(const cai_mcp_http_response_capture *res);

static size_t cai_mcp_response_write(char *ptr, size_t size, size_t nmemb,
                                     void *userdata) {
  cai_mcp_response_write_context *context;
  cai_mcp_http_response_capture *capture;
  lonejson_error json_error;
  size_t len;

  context = (cai_mcp_response_write_context *)userdata;
  capture = context != NULL ? context->response : NULL;
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

static void
cai_mcp_configure_curl_common(CURL *curl,
                              const cai_mcp_streamable_http_client_impl *impl) {
  if (curl == NULL || impl == NULL) {
    return;
  }
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, impl->timeout_ms);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, impl->timeout_ms);
  cai_configure_curl_tls(curl, impl->insecure_skip_verify, impl->ca_bundle_path,
                         impl->ca_path);
}

static lonejson_read_result
cai_mcp_spooled_read(void *user, unsigned char *buffer, size_t capacity) {
  cai_mcp_spooled_reader *reader;

  reader = (cai_mcp_spooled_reader *)user;
  return reader->cursor.read(&reader->cursor, buffer, capacity);
}

static lonejson_read_result cai_mcp_pipe_read(void *user, unsigned char *buffer,
                                              size_t capacity) {
  cai_mcp_pipe_reader *reader;
  lonejson_read_result result;
  ssize_t got;

  memset(&result, 0, sizeof(result));
  reader = (cai_mcp_pipe_reader *)user;
  if (reader == NULL || reader->fd < 0 || buffer == NULL || capacity == 0U) {
    result.error_code = EINVAL;
    result.eof = 1;
    return result;
  }
  do {
    got = read(reader->fd, buffer, capacity);
  } while (got < 0 && errno == EINTR);
  if (got < 0) {
    result.error_code = errno != 0 ? errno : EIO;
    result.eof = 1;
    return result;
  }
  if (got == 0) {
    result.eof = 1;
    return result;
  }
  result.bytes_read = (size_t)got;
  return result;
}

static int cai_mcp_write_all_fd(int fd, const char *data, size_t len) {
  ssize_t written;
  size_t offset;

  if (fd < 0) {
    return EINVAL;
  }
  offset = 0U;
  while (offset < len) {
    do {
      written = write(fd, data + offset, len - offset);
    } while (written < 0 && errno == EINTR);
    if (written <= 0) {
      return errno != 0 ? errno : EIO;
    }
    offset += (size_t)written;
  }
  return 0;
}

static int cai_mcp_streaming_sse_buffer_pending(
    cai_mcp_streaming_response_context *context, char ch) {
  cai_error ignored;

  if (context == NULL) {
    return EINVAL;
  }
  cai_error_init(&ignored);
  if (context->data_line_pending_newline) {
    if (cai_buffer_append(&context->pending_data, "\n", 1U, &ignored) !=
        CAI_OK) {
      cai_error_cleanup(&ignored);
      return ENOMEM;
    }
    context->data_line_pending_newline = 0;
  }
  if (cai_buffer_append(&context->pending_data, &ch, 1U, &ignored) != CAI_OK) {
    cai_error_cleanup(&ignored);
    return ENOMEM;
  }
  context->pending_data_started = 1;
  cai_error_cleanup(&ignored);
  return 0;
}

static int cai_mcp_streaming_sse_flush_pending(
    cai_mcp_streaming_response_context *context) {
  if (context == NULL || context->pending_data.length == 0U) {
    return 0;
  }
  if (context->data_started) {
    context->rc = cai_mcp_write_all_fd(context->write_fd, "\n", 1U);
    if (context->rc != 0) {
      return context->rc;
    }
  }
  context->rc =
      cai_mcp_write_all_fd(context->write_fd, context->pending_data.data,
                           context->pending_data.length);
  if (context->rc != 0) {
    return context->rc;
  }
  context->data_started = 1;
  context->pending_data.length = 0U;
  context->pending_data_started = 0;
  if (context->pending_data.data != NULL) {
    context->pending_data.data[0] = '\0';
  }
  return 0;
}

static void
cai_mcp_streaming_sse_reset_event(cai_mcp_streaming_response_context *context) {
  if (context == NULL) {
    return;
  }
  context->event[0] = '\0';
  context->data_started = 0;
  context->pending_data.length = 0U;
  context->pending_data_started = 0;
  context->data_line_pending_newline = 0;
  if (context->pending_data.data != NULL) {
    context->pending_data.data[0] = '\0';
  }
}

static int
cai_mcp_streaming_sse_line(cai_mcp_streaming_response_context *context,
                           const char *line, size_t len) {
  const char *value;

  if (context == NULL || context->done) {
    return 0;
  }
  if (cai_mcp_sse_line_is_blank(line, len)) {
    if (context->event[0] == '\0' || strcmp(context->event, "message") == 0) {
      context->rc = cai_mcp_streaming_sse_flush_pending(context);
      if (context->rc != 0) {
        return context->rc;
      }
    }
    cai_mcp_streaming_sse_reset_event(context);
    return 0;
  }
  if (len >= 6U && memcmp(line, "event:", 6U) == 0) {
    cai_mcp_sse_set_event(context->event, sizeof(context->event), line + 6U,
                          len - 6U);
    if (strcmp(context->event, "message") == 0) {
      context->rc = cai_mcp_streaming_sse_flush_pending(context);
      if (context->rc != 0) {
        return context->rc;
      }
    }
    return 0;
  }
  if (len >= 3U && memcmp(line, "id:", 3U) == 0) {
    return cai_mcp_sse_set_last_event_id(&context->resume.last_event_id,
                                         line + 3U, len - 3U, NULL);
  }
  if (len >= 6U && memcmp(line, "retry:", 6U) == 0) {
    cai_mcp_sse_set_retry(&context->resume, line + 6U, len - 6U);
    return 0;
  }
  if (len < 5U || memcmp(line, "data:", 5U) != 0) {
    return 0;
  }
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
    return 0;
  }
  if (context->data_started) {
    context->rc = cai_mcp_write_all_fd(context->write_fd, "\n", 1U);
    if (context->rc != 0) {
      return context->rc;
    }
  }
  context->data_started = 1;
  context->rc = cai_mcp_write_all_fd(context->write_fd, value, len);
  return context->rc;
}

static int
cai_mcp_streaming_sse_write(cai_mcp_streaming_response_context *context,
                            const char *ptr, size_t len) {
  size_t i;
  char ch;

  if (context == NULL) {
    return EINVAL;
  }
  for (i = 0U; i < len; i++) {
    ch = ptr[i];
    if (context->data_line_active) {
      if (context->data_line_pending_cr) {
        if (ch == '\n') {
          context->data_line_pending_cr = 0;
          context->data_line_active = 0;
          context->data_line_streaming = 0;
          context->data_line_skip_space = 0;
          continue;
        }
        if (context->data_line_streaming) {
          context->rc = cai_mcp_write_all_fd(context->write_fd, "\r", 1U);
          if (context->rc != 0) {
            return context->rc;
          }
        } else {
          context->rc = cai_mcp_streaming_sse_buffer_pending(context, '\r');
          if (context->rc != 0) {
            return context->rc;
          }
        }
        context->data_line_pending_cr = 0;
      }
      if (ch == '\r') {
        context->data_line_pending_cr = 1;
        continue;
      }
      if (ch == '\n') {
        context->data_line_active = 0;
        context->data_line_streaming = 0;
        context->data_line_skip_space = 0;
        context->data_line_pending_newline = 0;
        continue;
      }
      if (context->data_line_skip_space) {
        context->data_line_skip_space = 0;
        if (ch == ' ') {
          continue;
        }
      }
      if (context->data_line_streaming) {
        if (!context->data_started) {
          context->data_started = 1;
        }
        context->rc = cai_mcp_write_all_fd(context->write_fd, &ch, 1U);
        if (context->rc != 0) {
          return context->rc;
        }
      } else {
        context->rc = cai_mcp_streaming_sse_buffer_pending(context, ch);
        if (context->rc != 0) {
          return context->rc;
        }
      }
      continue;
    }
    if (ch == '\n') {
      context->rc = cai_mcp_streaming_sse_line(context, context->line.data,
                                               context->line.length);
      context->line.length = 0U;
      if (context->line.data != NULL) {
        context->line.data[0] = '\0';
      }
      if (context->rc != 0) {
        return context->rc;
      }
    } else {
      cai_error ignored;

      cai_error_init(&ignored);
      if (cai_buffer_append(&context->line, &ch, 1U, &ignored) != CAI_OK) {
        cai_error_cleanup(&ignored);
        context->rc = ENOMEM;
        return context->rc;
      }
      cai_error_cleanup(&ignored);
      if (context->line.length == 5U &&
          memcmp(context->line.data, "data:", 5U) == 0) {
        context->data_line_active = 1;
        context->data_line_streaming = strcmp(context->event, "message") == 0;
        context->data_line_skip_space = 1;
        context->data_line_pending_cr = 0;
        context->data_line_pending_newline =
            !context->data_line_streaming && context->pending_data_started;
        if (context->data_line_streaming && context->data_started) {
          context->rc = cai_mcp_write_all_fd(context->write_fd, "\n", 1U);
          if (context->rc != 0) {
            return context->rc;
          }
        }
        context->line.length = 0U;
        if (context->line.data != NULL) {
          context->line.data[0] = '\0';
        }
      }
    }
  }
  return 0;
}

static size_t cai_mcp_streaming_response_write(char *ptr, size_t size,
                                               size_t nmemb, void *userdata) {
  cai_mcp_streaming_response_context *context;
  size_t len;

  context = (cai_mcp_streaming_response_context *)userdata;
  len = size * nmemb;
  if (len == 0U) {
    return 0U;
  }
  if (context == NULL || context->write_fd < 0) {
    return 0U;
  }
  if (context->response != NULL && context->response->status != 0L &&
      (context->response->status < 200L || context->response->status >= 300L)) {
    return len;
  }
  if (context->response != NULL && cai_mcp_response_is_sse(context->response)) {
    context->rc = cai_mcp_streaming_sse_write(context, ptr, len);
  } else {
    context->rc = cai_mcp_write_all_fd(context->write_fd, ptr, len);
  }
  return context->rc == 0 ? len : 0U;
}

static int cai_mcp_stream_read_char(cai_mcp_stream_char_reader *reader,
                                    int *out, cai_error *error) {
  lonejson_read_result result;
  unsigned char ch;

  if (reader == NULL || out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP stream reader is required");
  }
  if (reader->has_pushback) {
    *out = reader->pushback;
    reader->has_pushback = 0;
    return 1;
  }
  result = reader->read_fn(reader->read_user, &ch, 1U);
  if (result.error_code != 0) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to read MCP streamed response");
  }
  if (result.eof && result.bytes_read == 0U) {
    return 0;
  }
  *out = ch;
  return 1;
}

static void cai_mcp_stream_unread_char(cai_mcp_stream_char_reader *reader,
                                       int ch) {
  if (reader != NULL) {
    reader->has_pushback = 1;
    reader->pushback = ch;
  }
}

static int cai_mcp_stream_skip_ws(cai_mcp_stream_char_reader *reader, int *out,
                                  cai_error *error) {
  int rc;
  int ch = 0;

  do {
    rc = cai_mcp_stream_read_char(reader, &ch, error);
    if (rc <= 0) {
      return rc;
    }
  } while (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n');
  *out = ch;
  return 1;
}

static int cai_mcp_stream_read_string_key(cai_mcp_stream_char_reader *reader,
                                          char *key, size_t key_size,
                                          cai_error *error) {
  size_t len;
  int escaped;
  int rc;
  int ch = 0;

  if (key == NULL || key_size == 0U) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP JSON key is required");
  }
  len = 0U;
  escaped = 0;
  for (;;) {
    rc = cai_mcp_stream_read_char(reader, &ch, error);
    if (rc <= 0) {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "failed to parse MCP JSON-RPC response");
    }
    if (escaped) {
      escaped = 0;
    } else if (ch == '\\') {
      escaped = 1;
      continue;
    } else if (ch == '"') {
      key[len] = '\0';
      return CAI_OK;
    }
    if (len + 1U < key_size) {
      key[len++] = (char)ch;
    }
  }
}

static int cai_mcp_json_primitive_is_valid(const char *text, size_t len) {
  size_t i;

  if (text == NULL || len == 0U) {
    return 0;
  }
  if ((len == 4U && memcmp(text, "true", 4U) == 0) ||
      (len == 4U && memcmp(text, "null", 4U) == 0) ||
      (len == 5U && memcmp(text, "false", 5U) == 0)) {
    return 1;
  }
  i = 0U;
  if (text[i] == '-') {
    i++;
    if (i == len) {
      return 0;
    }
  }
  if (text[i] == '0') {
    i++;
  } else if (text[i] >= '1' && text[i] <= '9') {
    do {
      i++;
    } while (i < len && isdigit((unsigned char)text[i]));
  } else {
    return 0;
  }
  if (i < len && text[i] == '.') {
    i++;
    if (i == len || !isdigit((unsigned char)text[i])) {
      return 0;
    }
    do {
      i++;
    } while (i < len && isdigit((unsigned char)text[i]));
  }
  if (i < len && (text[i] == 'e' || text[i] == 'E')) {
    i++;
    if (i < len && (text[i] == '+' || text[i] == '-')) {
      i++;
    }
    if (i == len || !isdigit((unsigned char)text[i])) {
      return 0;
    }
    do {
      i++;
    } while (i < len && isdigit((unsigned char)text[i]));
  }
  return i == len;
}

static int cai_mcp_stream_emit(cai_sink *sink, cai_buffer_builder *capture,
                               size_t *bytes_written, int ch,
                               cai_error *error) {
  char byte;
  int rc;

  byte = (char)ch;
  if (sink != NULL) {
    rc = cai_sink_write(sink, &byte, 1U, error);
    if (rc != CAI_OK) {
      return rc;
    }
    if (bytes_written != NULL) {
      (*bytes_written)++;
    }
  }
  if (capture != NULL) {
    rc = cai_buffer_append(capture, &byte, 1U, error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  return CAI_OK;
}

static int cai_mcp_stream_read_emit(cai_mcp_stream_char_reader *reader,
                                    cai_sink *sink, cai_buffer_builder *capture,
                                    size_t *bytes_written, int *out,
                                    cai_error *error) {
  int rc;
  int ch = 0;

  rc = cai_mcp_stream_read_char(reader, &ch, error);
  if (rc <= 0) {
    return rc;
  }
  rc = cai_mcp_stream_emit(sink, capture, bytes_written, ch, error);
  if (rc != CAI_OK) {
    return -1;
  }
  *out = ch;
  return 1;
}

static int cai_mcp_stream_skip_ws_emit(cai_mcp_stream_char_reader *reader,
                                       cai_sink *sink,
                                       cai_buffer_builder *capture,
                                       size_t *bytes_written, int *out,
                                       cai_error *error) {
  int rc;
  int ch = 0;

  for (;;) {
    rc = cai_mcp_stream_read_emit(reader, sink, capture, bytes_written, &ch,
                                  error);
    if (rc <= 0) {
      return rc;
    }
    if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
      *out = ch;
      return 1;
    }
  }
}

static int cai_mcp_stream_string_tail(cai_mcp_stream_char_reader *reader,
                                      cai_sink *sink,
                                      cai_buffer_builder *capture,
                                      size_t *bytes_written, cai_error *error) {
  int escaped;
  int rc;
  int ch = 0;

  escaped = 0;
  for (;;) {
    rc = cai_mcp_stream_read_emit(reader, sink, capture, bytes_written, &ch,
                                  error);
    if (rc <= 0) {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "failed to parse MCP JSON-RPC response");
    }
    if (escaped) {
      escaped = 0;
    } else if (ch == '\\') {
      escaped = 1;
    } else if (ch == '"') {
      return CAI_OK;
    } else if ((unsigned char)ch < 0x20U) {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "failed to parse MCP JSON-RPC response");
    }
  }
}

static int cai_mcp_stream_value_inner(cai_mcp_stream_char_reader *reader,
                                      int first, cai_sink *sink,
                                      cai_buffer_builder *capture,
                                      size_t *bytes_written, size_t depth,
                                      cai_error *error) {
  char primitive_token[128];
  size_t primitive_len;
  int primitive_overflow;
  int rc;
  int ch = 0;

  if (depth > 64U) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "failed to parse MCP JSON-RPC response");
  }
  if (first == '"') {
    return cai_mcp_stream_string_tail(reader, sink, capture, bytes_written,
                                      error);
  }
  if (first == '{') {
    rc = cai_mcp_stream_skip_ws_emit(reader, sink, capture, bytes_written, &ch,
                                     error);
    if (rc <= 0) {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "failed to parse MCP JSON-RPC response");
    }
    if (ch == '}') {
      return CAI_OK;
    }
    for (;;) {
      if (ch != '"') {
        return cai_set_error(error, CAI_ERR_PROTOCOL,
                             "failed to parse MCP JSON-RPC response");
      }
      rc = cai_mcp_stream_string_tail(reader, sink, capture, bytes_written,
                                      error);
      if (rc != CAI_OK) {
        return rc;
      }
      rc = cai_mcp_stream_skip_ws_emit(reader, sink, capture, bytes_written,
                                       &ch, error);
      if (rc <= 0 || ch != ':') {
        return cai_set_error(error, CAI_ERR_PROTOCOL,
                             "failed to parse MCP JSON-RPC response");
      }
      rc = cai_mcp_stream_skip_ws_emit(reader, sink, capture, bytes_written,
                                       &ch, error);
      if (rc <= 0) {
        return cai_set_error(error, CAI_ERR_PROTOCOL,
                             "failed to parse MCP JSON-RPC response");
      }
      rc = cai_mcp_stream_value_inner(reader, ch, sink, capture, bytes_written,
                                      depth + 1U, error);
      if (rc != CAI_OK) {
        return rc;
      }
      rc = cai_mcp_stream_skip_ws_emit(reader, sink, capture, bytes_written,
                                       &ch, error);
      if (rc <= 0) {
        return cai_set_error(error, CAI_ERR_PROTOCOL,
                             "failed to parse MCP JSON-RPC response");
      }
      if (ch == '}') {
        return CAI_OK;
      }
      if (ch != ',') {
        return cai_set_error(error, CAI_ERR_PROTOCOL,
                             "failed to parse MCP JSON-RPC response");
      }
      rc = cai_mcp_stream_skip_ws_emit(reader, sink, capture, bytes_written,
                                       &ch, error);
      if (rc <= 0) {
        return cai_set_error(error, CAI_ERR_PROTOCOL,
                             "failed to parse MCP JSON-RPC response");
      }
    }
  }
  if (first == '[') {
    rc = cai_mcp_stream_skip_ws_emit(reader, sink, capture, bytes_written, &ch,
                                     error);
    if (rc <= 0) {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "failed to parse MCP JSON-RPC response");
    }
    if (ch == ']') {
      return CAI_OK;
    }
    for (;;) {
      rc = cai_mcp_stream_value_inner(reader, ch, sink, capture, bytes_written,
                                      depth + 1U, error);
      if (rc != CAI_OK) {
        return rc;
      }
      rc = cai_mcp_stream_skip_ws_emit(reader, sink, capture, bytes_written,
                                       &ch, error);
      if (rc <= 0) {
        return cai_set_error(error, CAI_ERR_PROTOCOL,
                             "failed to parse MCP JSON-RPC response");
      }
      if (ch == ']') {
        return CAI_OK;
      }
      if (ch != ',') {
        return cai_set_error(error, CAI_ERR_PROTOCOL,
                             "failed to parse MCP JSON-RPC response");
      }
      rc = cai_mcp_stream_skip_ws_emit(reader, sink, capture, bytes_written,
                                       &ch, error);
      if (rc <= 0) {
        return cai_set_error(error, CAI_ERR_PROTOCOL,
                             "failed to parse MCP JSON-RPC response");
      }
    }
  }
  primitive_len = 0U;
  primitive_overflow = 0;
  ch = first;
  if (primitive_len < sizeof(primitive_token)) {
    primitive_token[primitive_len++] = (char)ch;
  } else {
    primitive_overflow = 1;
  }
  for (;;) {
    rc = cai_mcp_stream_read_char(reader, &ch, error);
    if (rc <= 0) {
      if (!primitive_overflow &&
          cai_mcp_json_primitive_is_valid(primitive_token, primitive_len)) {
        return CAI_OK;
      }
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "failed to parse MCP JSON-RPC response");
    }
    if (ch == ',' || ch == '}' || ch == ']' || ch == ' ' || ch == '\t' ||
        ch == '\r' || ch == '\n') {
      if (primitive_overflow ||
          !cai_mcp_json_primitive_is_valid(primitive_token, primitive_len)) {
        return cai_set_error(error, CAI_ERR_PROTOCOL,
                             "failed to parse MCP JSON-RPC response");
      }
      cai_mcp_stream_unread_char(reader, ch);
      return CAI_OK;
    }
    rc = cai_mcp_stream_emit(sink, capture, bytes_written, ch, error);
    if (rc != CAI_OK) {
      return rc;
    }
    if (primitive_len < sizeof(primitive_token)) {
      primitive_token[primitive_len++] = (char)ch;
    } else {
      primitive_overflow = 1;
    }
  }
}

static int cai_mcp_stream_value(cai_mcp_stream_char_reader *reader,
                                cai_sink *sink, cai_buffer_builder *capture,
                                size_t *bytes_written, cai_error *error) {
  int rc;
  int ch;

  rc = cai_mcp_stream_skip_ws(reader, &ch, error);
  if (rc <= 0) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "failed to parse MCP JSON-RPC response");
  }
  rc = cai_mcp_stream_emit(sink, capture, bytes_written, ch, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_mcp_stream_value_inner(reader, ch, sink, capture, bytes_written,
                                    0U, error);
}

static lonejson_status cai_mcp_spool_sink(void *user, const void *data,
                                          size_t len,
                                          lonejson_error *json_error) {
  lonejson_spooled *spool;

  spool = (lonejson_spooled *)user;
  return spool->append(spool, data, len, json_error);
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

static lonejson_status cai_mcp_result_sink(void *user, const void *data,
                                           size_t len,
                                           lonejson_error *json_error) {
  cai_mcp_result_sink_context *context;

  (void)json_error;
  context = (cai_mcp_result_sink_context *)user;
  if (context == NULL || context->sink == NULL) {
    if (context != NULL) {
      context->rc = cai_set_error(context->error, CAI_ERR_INVALID,
                                  "MCP result output sink is required");
    }
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  context->rc = cai_sink_write(context->sink, data, len, context->error);
  if (context->rc == CAI_OK) {
    context->bytes_written += len;
  }
  return context->rc == CAI_OK ? LONEJSON_STATUS_OK
                               : LONEJSON_STATUS_CALLBACK_FAILED;
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

static int cai_mcp_parse_jsonrpc_id(const lonejson_spooled *json,
                                    cai_mcp_jsonrpc_id_doc *doc,
                                    const char *operation, cai_error *error);

static int cai_mcp_request_id_i64(const lonejson_spooled *request,
                                  long long *out, cai_error *error) {
  cai_mcp_jsonrpc_id_doc doc;
  char *text;
  long long value;
  const char *cursor;
  int rc;

  if (out != NULL) {
    *out = 0LL;
  }
  if (request == NULL || out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP request is required");
  }
  memset(&doc, 0, sizeof(doc));
  rc = cai_mcp_parse_jsonrpc_id(request, &doc, "failed to parse MCP request id",
                                error);
  if (rc != CAI_OK) {
    return rc;
  }
  text = cai_mcp_json_value_to_cstr(&doc.id, error);
  if (text == NULL) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_jsonrpc_id_map, &doc);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to read MCP request id");
  }
  cursor = text;
  value = 0LL;
  if (*cursor == '\0') {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "MCP request id must be an integer");
  } else {
    while (*cursor != '\0') {
      int digit;

      if (*cursor < '0' || *cursor > '9') {
        rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP request id must be an integer");
        break;
      }
      digit = *cursor - '0';
      if (value > (9223372036854775807LL - digit) / 10LL) {
        rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP request id must be an integer");
        break;
      }
      value = (value * 10LL) + digit;
      cursor++;
    }
    if (rc == CAI_OK) {
      *out = value;
    }
  }
  cai_free_mem(NULL, text);
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_jsonrpc_id_map, &doc);
  return rc;
}

static int cai_mcp_post_ex(cai_mcp_streamable_http_client_impl *impl,
                           const lonejson_spooled *request, size_t request_len,
                           int is_request,
                           cai_mcp_http_response_capture *response,
                           cai_error *error) {
  CURL *curl;
  CURLcode curl_rc;
  struct curl_slist *headers;
  cai_mcp_spooled_upload upload;
  cai_mcp_response_write_context write_context;
  long long request_id;
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
    request_id = 0LL;
    rc = cai_mcp_request_id_i64(request, &request_id, error);
    if (rc != CAI_OK) {
      response->body.cleanup(&response->body);
      memset(&response->body, 0, sizeof(response->body));
      curl_easy_cleanup(curl);
      curl_slist_free_all(headers);
      return rc;
    }
    impl->active_request_id = request_id;
    impl->has_active_request = 1;
    impl->has_active_progress = 0;
    impl->active_progress = 0.0;
  }

  upload.cursor = *request;
  upload.rewound = 0;
  write_context.response = response;
  curl_easy_setopt(curl, CURLOPT_URL, impl->url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_READFUNCTION, cai_mcp_upload_read);
  curl_easy_setopt(curl, CURLOPT_READDATA, &upload);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)request_len);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cai_mcp_response_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_context);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, cai_mcp_header_callback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, response);
  cai_mcp_configure_curl_common(curl, impl);
  cai_mcp_log_http_request_start(impl, "POST", 0, request_len);
  curl_rc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response->status);
  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);
  if (curl_rc != CURLE_OK) {
    if (is_request) {
      impl->has_active_request = 0;
      impl->has_active_progress = 0;
    }
    cai_mcp_log_http_transport_error(impl, "POST", curl_easy_strerror(curl_rc));
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
  cai_mcp_log_http_request_done(impl, "POST", response->status, 0);
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

static int cai_mcp_post(cai_mcp_streamable_http_client_impl *impl,
                        const lonejson_spooled *request, size_t request_len,
                        int is_request, cai_mcp_http_response_capture *response,
                        cai_error *error) {
  return cai_mcp_post_ex(impl, request, request_len, is_request, response,
                         error);
}

static void *cai_mcp_streaming_post_thread(void *user) {
  cai_mcp_streaming_post_context *context;
  cai_mcp_streaming_response_context write_context;
  cai_mcp_spooled_upload upload;
  struct curl_slist *headers;
  CURL *curl;
  int rc;

  context = (cai_mcp_streaming_post_context *)user;
  if (context == NULL || context->impl == NULL || context->response == NULL ||
      context->write_fd < 0 ||
      (context->request == NULL && context->last_event_id == NULL)) {
    if (context != NULL) {
      context->rc = CAI_ERR_INVALID;
      if (context->write_fd >= 0) {
        close(context->write_fd);
        context->write_fd = -1;
      }
    }
    return NULL;
  }

  headers = NULL;
  curl = curl_easy_init();
  if (curl == NULL) {
    context->rc = CAI_ERR_TRANSPORT;
    close(context->write_fd);
    context->write_fd = -1;
    return NULL;
  }
  rc = CAI_OK;
  if (context->request != NULL) {
    rc = cai_append_header(&headers, "Content-Type: application/json", NULL);
  }
  if (rc == CAI_OK && context->request != NULL) {
    rc = cai_append_header(&headers,
                           "Accept: application/json, text/event-stream", NULL);
  } else if (rc == CAI_OK) {
    rc = cai_append_header(&headers, "Accept: text/event-stream", NULL);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_append_session_headers(context->impl, &headers, NULL);
  }
  if (rc == CAI_OK && context->last_event_id != NULL) {
    rc = cai_mcp_append_last_event_id_header(&headers, context->last_event_id,
                                             NULL);
  }
  if (rc != CAI_OK) {
    context->rc = rc;
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    close(context->write_fd);
    context->write_fd = -1;
    return NULL;
  }

  context->response->content_type = NULL;
  context->response->session_id = NULL;
  context->response->status = 0L;
  memset(&context->response->body, 0, sizeof(context->response->body));
  memset(&upload, 0, sizeof(upload));
  if (context->request != NULL) {
    upload.cursor = *context->request;
  }
  upload.rewound = 0;
  memset(&write_context, 0, sizeof(write_context));
  write_context.response = context->response;
  write_context.write_fd = context->write_fd;
  write_context.rc = 0;
  write_context.event[0] = '\0';

  curl_easy_setopt(curl, CURLOPT_URL, context->impl->url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  if (context->request != NULL) {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, cai_mcp_upload_read);
    curl_easy_setopt(curl, CURLOPT_READDATA, &upload);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                     (curl_off_t)context->request_len);
  } else {
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  }
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                   cai_mcp_streaming_response_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_context);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, cai_mcp_header_callback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, context->response);
  cai_mcp_configure_curl_common(curl, context->impl);
  cai_mcp_log_http_request_start(context->impl,
                                 context->request != NULL ? "POST" : "GET", 1,
                                 context->request_len);
  context->curl_rc = curl_easy_perform(curl);
  if (context->curl_rc == CURLE_OK && write_context.rc == 0 &&
      write_context.line.length != 0U && context->response != NULL &&
      cai_mcp_response_is_sse(context->response)) {
    write_context.rc = cai_mcp_streaming_sse_line(
        &write_context, write_context.line.data, write_context.line.length);
  }
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &context->response->status);
  cai_mcp_log_http_request_done(context->impl,
                                context->request != NULL ? "POST" : "GET",
                                context->response->status, 1);
  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);
  cai_free_mem(NULL, write_context.line.data);
  cai_free_mem(NULL, write_context.pending_data.data);
  context->next_last_event_id = write_context.resume.last_event_id;
  context->next_retry_ms = write_context.resume.retry_ms;
  context->has_next_retry = write_context.resume.has_retry;
  close(context->write_fd);
  context->write_fd = -1;
  if (context->curl_rc != CURLE_OK) {
    context->rc = CAI_ERR_TRANSPORT;
  } else if (write_context.rc != 0) {
    context->rc = CAI_ERR_TRANSPORT;
  } else {
    context->rc = CAI_OK;
  }
  return NULL;
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
  cai_mcp_response_write_context write_context;
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
  write_context.response = response;
  curl_easy_setopt(curl, CURLOPT_URL, impl->url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cai_mcp_response_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_context);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, cai_mcp_header_callback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, response);
  cai_mcp_configure_curl_common(curl, impl);
  cai_mcp_log_http_request_start(impl, "GET", 1, 0U);
  curl_rc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response->status);
  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);
  if (curl_rc != CURLE_OK) {
    cai_mcp_log_http_transport_error(impl, "GET", curl_easy_strerror(curl_rc));
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
  cai_mcp_log_http_request_done(impl, "GET", response->status, 1);
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

static int cai_mcp_validate_progress_notification_params(
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
  if (rc == CAI_OK && doc.progress < 0.0) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "MCP progress value must be non-negative");
  }
  if (rc == CAI_OK && doc.has_total && doc.total < 0.0) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "MCP progress total must be non-negative");
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

static int cai_mcp_validate_optional_notification_params(
    const lonejson_json_value *params, const char *message, cai_error *error) {
  if (params == NULL || params->kind == LONEJSON_JSON_VALUE_NULL) {
    return CAI_OK;
  }
  if (!cai_mcp_json_value_root_is(params, '{', NULL)) {
    return cai_set_error(error, CAI_ERR_PROTOCOL, message);
  }
  return CAI_OK;
}

static int cai_mcp_validate_optional_request_params(
    const lonejson_json_value *params, const char *message, cai_error *error) {
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
  notification = impl->receiver.notification;
  context = impl->receiver.context;
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

  (void)impl;
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
  } else {
    rc = cai_mcp_write_jsonrpc_error(
        response, &doc->id, -32601,
        "MCP server-to-client request method is not supported", error);
    if (out_len != NULL) {
      *out_len = response->size_fn(response);
    }
    return rc;
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

static int cai_mcp_response_reader_init(
    const cai_mcp_http_response_capture *response, const char *operation,
    int require_result_object, lonejson_spooled *json_body,
    cai_mcp_spooled_reader *reader, int *has_error, cai_error *error) {
  lonejson_error json_error;
  int result_is_object;
  int rc;

  if (json_body == NULL || reader == NULL || has_error == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP response reader output is required");
  }
  rc = cai_mcp_response_json_body(response, operation, json_body, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_mcp_response_has_jsonrpc_error(json_body, has_error, error);
  if (rc != CAI_OK) {
    json_body->cleanup(json_body);
    return rc;
  }
  if (!*has_error && require_result_object) {
    rc = cai_mcp_jsonrpc_response_result_root_is_object(
        json_body, &result_is_object, error);
    if (rc != CAI_OK) {
      json_body->cleanup(json_body);
      return rc;
    }
    if (!result_is_object) {
      json_body->cleanup(json_body);
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP result must be an object");
    }
  }
  reader->cursor = *json_body;
  lonejson_error_init(&json_error);
  if (reader->cursor.rewind(&reader->cursor, &json_error) !=
      LONEJSON_STATUS_OK) {
    json_body->cleanup(json_body);
    return cai_mcp_set_json_error(error, "failed to rewind MCP response",
                                  &json_error);
  }
  return CAI_OK;
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

static int cai_mcp_initialize_request(cai_mcp_streamable_http_client_impl *impl,
                                      lonejson_spooled *spool, size_t *out_len,
                                      cai_error *error) {
  int rc;

  CAI_LJ->spooled_init(CAI_LJ, spool);
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
  if (rc == CAI_OK && cursor != NULL && cursor[0] != '\0') {
    rc = cai_mcp_write_cstr(spool, ",\"params\":{", error);
    if (rc == CAI_OK) {
      rc = cai_mcp_write_cstr(spool, "\"cursor\":", error);
    }
    if (rc == CAI_OK) {
      rc = cai_mcp_write_json_string(spool, cursor, error);
    }
    if (rc == CAI_OK) {
      rc = cai_mcp_write_cstr(spool, "}", error);
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
    rc = cai_mcp_spooled_object_string_values(
        arguments_json, "MCP prompt arguments must be an object",
        "MCP prompt argument values must be strings",
        "failed to parse MCP prompt arguments", error);
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
    rc = cai_mcp_spooled_object_string_values(
        context_arguments_json,
        "MCP completion context arguments must be an object",
        "MCP completion context argument values must be strings",
        "failed to parse MCP completion context arguments", error);
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

static int cai_mcp_call_request(cai_mcp_streamable_http_client_impl *impl,
                                const char *name,
                                lonejson_spooled *arguments_json,
                                lonejson_spooled *spool, size_t *out_len,
                                cai_error *error) {
  lonejson_error json_error;
  lonejson_writer writer;
  lonejson_status status;
  int rc;

  if (name == NULL || name[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "MCP tool name is required");
  }
  if (arguments_json != NULL) {
    rc = cai_mcp_spooled_root_is(arguments_json, '{',
                                 "MCP tool arguments must be an object", error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  CAI_LJ->spooled_init(CAI_LJ, spool);
  rc = cai_mcp_request_begin(impl, spool, ++impl->next_id, "tools/call", error);
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
                                  "failed to write MCP tool arguments",
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
  int rc;

  rc = cai_mcp_response_reader_init(response, "MCP initialize response", 1,
                                    &json_body, &reader, &has_error, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
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
  rc = cai_mcp_validate_optional_icons(
      &doc.result.server_info.icons, "MCP serverInfo icons must be an array",
      "failed to parse MCP serverInfo icons",
      "MCP serverInfo icon theme must be light or dark", error);
  if (rc != CAI_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_initialize_response_map, &doc);
    json_body.cleanup(&json_body);
    return rc;
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
    cai_mcp_client_schema_cleanup(
        allocator, &tool->input_schema, &tool->input_schema_uri,
        &tool->input_schema_type, &tool->input_schema_properties,
        &tool->input_schema_required);
    cai_mcp_client_schema_cleanup(
        allocator, &tool->output_schema, &tool->output_schema_uri,
        &tool->output_schema_type, &tool->output_schema_properties,
        &tool->output_schema_required);
    cai_mcp_client_icons_cleanup(allocator, tool->icons,
                                 tool->public_tool.icon_count);
    cai_free_mem(allocator, tool->annotations_title);
    cai_free_mem(allocator, tool->input_schema_json);
    cai_free_mem(allocator, tool->output_schema_json);
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
    cai_mcp_client_icons_cleanup(allocator, resource->icons,
                                 resource->public_resource.icon_count);
    cai_mcp_string_array_cleanup(allocator, resource->annotation_audience,
                                 resource->annotations.audience_count);
    cai_free_mem(allocator, resource->annotation_last_modified);
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
    cai_mcp_client_icons_cleanup(
        allocator, resource_template->icons,
        resource_template->public_resource_template.icon_count);
    cai_mcp_string_array_cleanup(allocator,
                                 resource_template->annotation_audience,
                                 resource_template->annotations.audience_count);
    cai_free_mem(allocator, resource_template->annotation_last_modified);
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
    cai_mcp_client_prompt_arguments_cleanup(
        allocator, prompt->arguments, prompt->public_prompt.argument_count);
    cai_mcp_client_icons_cleanup(allocator, prompt->icons,
                                 prompt->public_prompt.icon_count);
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
  size_t i;

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
  for (i = 0U; i < impl->tool_count; i++) {
    impl->tools[i].public_tool.input_schema = &impl->tools[i].input_schema;
    impl->tools[i].public_tool.output_schema =
        impl->tools[i].output_schema.type != NULL
            ? &impl->tools[i].output_schema
            : NULL;
  }
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

static char *
cai_mcp_json_value_to_cstr_with_allocator(const cai_allocator *allocator,
                                          const lonejson_json_value *value,
                                          cai_error *error) {
  cai_buffer_builder builder;
  lonejson_error json_error;

  memset(&builder, 0, sizeof(builder));
  builder.allocator = allocator;
  lonejson_error_init(&json_error);
  if (value == NULL ||
      value->methods->write_to_sink(value, cai_mcp_buffer_sink, &builder,
                                    &json_error) != LONEJSON_STATUS_OK) {
    cai_free_mem(allocator, builder.data);
    return NULL;
  }
  if (cai_buffer_append(&builder, "", 1U, error) != CAI_OK) {
    cai_free_mem(allocator, builder.data);
    return NULL;
  }
  return builder.data;
}

static char *cai_mcp_json_value_to_cstr(const lonejson_json_value *value,
                                        cai_error *error) {
  return cai_mcp_json_value_to_cstr_with_allocator(NULL, value, error);
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

static int cai_mcp_json_scan_skip_string(cai_mcp_json_scan *scan,
                                         cai_error *error);

static int cai_mcp_spooled_object_string_values(const lonejson_spooled *json,
                                                const char *object_error,
                                                const char *value_error,
                                                const char *parse_error,
                                                cai_error *error) {
  cai_mcp_json_scan scan;
  int ch;
  int rc;

  rc = cai_mcp_json_scan_init(&scan, json, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_mcp_json_scan_skip_ws(&scan, &ch, error);
  if (rc <= 0 || ch != '{') {
    return cai_set_error(error, CAI_ERR_PROTOCOL, object_error);
  }
  rc = cai_mcp_json_scan_skip_ws(&scan, &ch, error);
  if (rc <= 0) {
    return cai_set_error(error, CAI_ERR_PROTOCOL, parse_error);
  }
  if (ch == '}') {
    goto trailing;
  }
  cai_mcp_json_scan_unget(&scan, ch);
  for (;;) {
    rc = cai_mcp_json_scan_skip_ws(&scan, &ch, error);
    if (rc <= 0 || ch != '"') {
      return cai_set_error(error, CAI_ERR_PROTOCOL, parse_error);
    }
    rc = cai_mcp_json_scan_skip_string(&scan, error);
    if (rc <= 0) {
      return rc == 0 ? cai_set_error(error, CAI_ERR_PROTOCOL, parse_error) : rc;
    }
    rc = cai_mcp_json_scan_skip_ws(&scan, &ch, error);
    if (rc <= 0 || ch != ':') {
      return cai_set_error(error, CAI_ERR_PROTOCOL, parse_error);
    }
    rc = cai_mcp_json_scan_skip_ws(&scan, &ch, error);
    if (rc <= 0) {
      return cai_set_error(error, CAI_ERR_PROTOCOL, parse_error);
    }
    if (ch != '"') {
      return cai_set_error(error, CAI_ERR_PROTOCOL, value_error);
    }
    rc = cai_mcp_json_scan_skip_string(&scan, error);
    if (rc <= 0) {
      return rc == 0 ? cai_set_error(error, CAI_ERR_PROTOCOL, parse_error) : rc;
    }
    rc = cai_mcp_json_scan_skip_ws(&scan, &ch, error);
    if (rc <= 0) {
      return cai_set_error(error, CAI_ERR_PROTOCOL, parse_error);
    }
    if (ch == '}') {
      break;
    }
    if (ch != ',') {
      return cai_set_error(error, CAI_ERR_PROTOCOL, parse_error);
    }
  }

trailing:
  while ((rc = cai_mcp_json_scan_get(&scan, &ch, error)) > 0) {
    if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
      return cai_set_error(error, CAI_ERR_PROTOCOL, parse_error);
    }
  }
  return rc < 0 ? rc : CAI_OK;
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
  int ch = 0;
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
  return cai_mcp_json_value_to_cstr_with_allocator(allocator, value, error);
}

static void cai_mcp_string_array_cleanup(const cai_allocator *allocator,
                                         char **items, size_t count) {
  size_t i;

  if (items == NULL) {
    return;
  }
  for (i = 0U; i < count; i++) {
    cai_free_mem(allocator, items[i]);
  }
  cai_free_mem(allocator, items);
}

static void cai_mcp_free_const_mem(const cai_allocator *allocator,
                                   const void *ptr) {
  void *mutable_ptr;

  if (ptr == NULL) {
    return;
  }
  memcpy(&mutable_ptr, &ptr, sizeof(mutable_ptr));
  cai_free_mem(allocator, mutable_ptr);
}

static int cai_mcp_copy_lonejson_string_array(const cai_allocator *allocator,
                                              const lonejson_string_array *src,
                                              char ***dst_items,
                                              size_t *dst_count,
                                              cai_error *error) {
  char **items;
  size_t i;

  if (dst_items != NULL) {
    *dst_items = NULL;
  }
  if (dst_count != NULL) {
    *dst_count = 0U;
  }
  if (src == NULL || src->count == 0U) {
    return CAI_OK;
  }
  items = (char **)cai_alloc(allocator, src->count * sizeof(*items));
  if (items == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate MCP string array");
  }
  memset(items, 0, src->count * sizeof(*items));
  for (i = 0U; i < src->count; i++) {
    items[i] =
        cai_strdup(allocator, src->items[i] != NULL ? src->items[i] : "");
    if (items[i] == NULL) {
      cai_mcp_string_array_cleanup(allocator, items, src->count);
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to copy MCP string array");
    }
  }
  if (dst_items != NULL) {
    *dst_items = items;
  }
  if (dst_count != NULL) {
    *dst_count = src->count;
  }
  return CAI_OK;
}

static void cai_mcp_client_icons_cleanup(const cai_allocator *allocator,
                                         cai_mcp_client_icon *icons,
                                         size_t count) {
  size_t i;

  if (icons == NULL) {
    return;
  }
  for (i = 0U; i < count; i++) {
    char **sizes;
    cai_mcp_free_const_mem(allocator, icons[i].src);
    cai_mcp_free_const_mem(allocator, icons[i].mime_type);
    cai_mcp_free_const_mem(allocator, icons[i].theme);
    sizes = NULL;
    memcpy(&sizes, &icons[i].sizes, sizeof(sizes));
    cai_mcp_string_array_cleanup(allocator, sizes, icons[i].size_count);
  }
  cai_free_mem(allocator, icons);
}

static int cai_mcp_parse_wrapped_json_value(const lonejson_json_value *value,
                                            const char *field_name,
                                            const lonejson_map *map, void *doc,
                                            const char *alloc_error_message,
                                            const char *parse_error_message,
                                            cai_error *error) {
  lonejson_error json_error;
  lonejson_status status;
  char *value_json;
  char *wrapped_json;
  size_t field_len;
  size_t value_len;
  size_t wrapped_len;

  value_json = cai_mcp_json_value_to_cstr(value, error);
  if (value_json == NULL) {
    return error != NULL && error->code != CAI_OK ? error->code : CAI_ERR_NOMEM;
  }
  field_len = strlen(field_name);
  value_len = strlen(value_json);
  wrapped_len = 2U + field_len + 2U + value_len + 2U;
  wrapped_json = (char *)cai_alloc(NULL, wrapped_len);
  if (wrapped_json == NULL) {
    cai_free_mem(NULL, value_json);
    return cai_set_error(error, CAI_ERR_NOMEM, alloc_error_message);
  }
  snprintf(wrapped_json, wrapped_len, "{\"%s\":%s}", field_name, value_json);
  cai_free_mem(NULL, value_json);
  lonejson_error_init(&json_error);
  status = CAI_LJ->parse_cstr(CAI_LJ, map, doc, wrapped_json, &json_error);
  cai_free_mem(NULL, wrapped_json);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, map, doc);
    return cai_mcp_set_json_error(error, parse_error_message, &json_error);
  }
  return CAI_OK;
}

static int cai_mcp_copy_optional_icons(const cai_allocator *allocator,
                                       const lonejson_json_value *icons,
                                       cai_mcp_client_icon **dst_icons,
                                       size_t *dst_count, cai_error *error) {
  cai_mcp_icons_doc doc;
  cai_mcp_icon_doc *src;
  cai_mcp_client_icon *items;
  size_t i;
  int rc;

  if (dst_icons != NULL) {
    *dst_icons = NULL;
  }
  if (dst_count != NULL) {
    *dst_count = 0U;
  }
  if (icons == NULL || icons->kind == LONEJSON_JSON_VALUE_NULL) {
    return CAI_OK;
  }
  memset(&doc, 0, sizeof(doc));
  rc = cai_mcp_parse_wrapped_json_value(
      icons, "icons", &cai_mcp_icons_map, &doc,
      "failed to allocate MCP icons metadata JSON", "failed to parse MCP icons",
      error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (doc.icons.count == 0U) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_icons_map, &doc);
    return CAI_OK;
  }
  items = (cai_mcp_client_icon *)cai_alloc(allocator,
                                           doc.icons.count * sizeof(*items));
  if (items == NULL) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_icons_map, &doc);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate MCP icons metadata");
  }
  memset(items, 0, doc.icons.count * sizeof(*items));
  src = (cai_mcp_icon_doc *)doc.icons.items;
  for (i = 0U; i < doc.icons.count; i++) {
    char **sizes;
    items[i].src = cai_strdup(allocator, src[i].src);
    items[i].mime_type =
        cai_strdup(allocator, src[i].mime_type != NULL ? src[i].mime_type : "");
    items[i].theme =
        cai_strdup(allocator, src[i].theme != NULL ? src[i].theme : "");
    sizes = NULL;
    rc = cai_mcp_copy_lonejson_string_array(allocator, &src[i].sizes, &sizes,
                                            &items[i].size_count, error);
    items[i].sizes = (const char *const *)sizes;
    if (items[i].src == NULL || items[i].mime_type == NULL ||
        items[i].theme == NULL || rc != CAI_OK) {
      if (rc == CAI_OK) {
        rc = cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to copy MCP icons metadata");
      }
      cai_mcp_client_icons_cleanup(allocator, items, doc.icons.count);
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_icons_map, &doc);
      return rc;
    }
  }
  if (dst_icons != NULL) {
    *dst_icons = items;
  }
  if (dst_count != NULL) {
    *dst_count = doc.icons.count;
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_icons_map, &doc);
  return CAI_OK;
}

static int cai_mcp_icon_theme_is_valid(const char *theme) {
  return theme == NULL || strcmp(theme, "light") == 0 ||
         strcmp(theme, "dark") == 0;
}

static int cai_mcp_validate_optional_icons(const lonejson_json_value *icons,
                                           const char *array_error_message,
                                           const char *parse_error_message,
                                           const char *theme_error_message,
                                           cai_error *error) {
  cai_mcp_icons_doc doc;
  lonejson_error json_error;
  lonejson_status status;
  cai_mcp_icon_doc *items;
  char *icons_json;
  char *wrapped_json;
  size_t icons_json_len;
  size_t wrapped_len;
  size_t i;
  int rc;

  if (!cai_mcp_optional_json_array_is_valid(icons, error)) {
    return cai_set_error(error, CAI_ERR_PROTOCOL, array_error_message);
  }
  if (icons == NULL || icons->kind == LONEJSON_JSON_VALUE_NULL) {
    return CAI_OK;
  }
  icons_json = cai_mcp_json_value_to_cstr(icons, error);
  if (icons_json == NULL) {
    return error != NULL && error->code != CAI_OK ? error->code : CAI_ERR_NOMEM;
  }
  icons_json_len = strlen(icons_json);
  wrapped_len = sizeof("{\"icons\":}") + icons_json_len;
  wrapped_json = (char *)cai_alloc(NULL, wrapped_len);
  if (wrapped_json == NULL) {
    cai_free_mem(NULL, icons_json);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate MCP icons validation JSON");
  }
  snprintf(wrapped_json, wrapped_len, "{\"icons\":%s}", icons_json);
  cai_free_mem(NULL, icons_json);
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
  status = CAI_LJ->parse_cstr(CAI_LJ, &cai_mcp_icons_map, &doc, wrapped_json,
                              &json_error);
  cai_free_mem(NULL, wrapped_json);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_icons_map, &doc);
    return cai_mcp_set_json_error(error, parse_error_message, &json_error);
  }
  items = (cai_mcp_icon_doc *)doc.icons.items;
  rc = CAI_OK;
  for (i = 0U; i < doc.icons.count; i++) {
    if (!cai_mcp_icon_theme_is_valid(items[i].theme)) {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL, theme_error_message);
      break;
    }
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_icons_map, &doc);
  return rc;
}

static int
cai_mcp_validate_optional_prompt_arguments(const lonejson_json_value *arguments,
                                           cai_error *error) {
  cai_mcp_prompt_arguments_doc doc;
  lonejson_error json_error;
  lonejson_status status;
  char *arguments_json;
  char *wrapped_json;
  size_t arguments_json_len;
  size_t wrapped_len;

  if (!cai_mcp_optional_json_array_is_valid(arguments, error)) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP prompt arguments must be an array");
  }
  if (arguments == NULL || arguments->kind == LONEJSON_JSON_VALUE_NULL) {
    return CAI_OK;
  }
  arguments_json = cai_mcp_json_value_to_cstr(arguments, error);
  if (arguments_json == NULL) {
    return error != NULL && error->code != CAI_OK ? error->code : CAI_ERR_NOMEM;
  }
  arguments_json_len = strlen(arguments_json);
  wrapped_len = sizeof("{\"arguments\":}") + arguments_json_len;
  wrapped_json = (char *)cai_alloc(NULL, wrapped_len);
  if (wrapped_json == NULL) {
    cai_free_mem(NULL, arguments_json);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate MCP prompt arguments "
                         "validation JSON");
  }
  snprintf(wrapped_json, wrapped_len, "{\"arguments\":%s}", arguments_json);
  cai_free_mem(NULL, arguments_json);
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
  status = CAI_LJ->parse_cstr(CAI_LJ, &cai_mcp_prompt_arguments_map, &doc,
                              wrapped_json, &json_error);
  cai_free_mem(NULL, wrapped_json);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_prompt_arguments_map, &doc);
    return cai_mcp_set_json_error(error, "failed to parse MCP prompt arguments",
                                  &json_error);
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_prompt_arguments_map, &doc);
  return CAI_OK;
}

static int cai_mcp_validate_optional_tool_annotations(
    const lonejson_json_value *annotations, cai_error *error) {
  cai_mcp_tool_annotations_doc doc;
  lonejson_error json_error;
  lonejson_status status;
  char *annotations_json;

  if (!cai_mcp_optional_json_object_is_valid(annotations, error)) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP tool annotations must be an object");
  }
  if (annotations == NULL || annotations->kind == LONEJSON_JSON_VALUE_NULL) {
    return CAI_OK;
  }
  annotations_json = cai_mcp_json_value_to_cstr(annotations, error);
  if (annotations_json == NULL) {
    return error != NULL && error->code != CAI_OK ? error->code : CAI_ERR_NOMEM;
  }
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
  status = CAI_LJ->parse_cstr(CAI_LJ, &cai_mcp_tool_annotations_map, &doc,
                              annotations_json, &json_error);
  cai_free_mem(NULL, annotations_json);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tool_annotations_map, &doc);
    return cai_mcp_set_json_error(error, "failed to parse MCP tool annotations",
                                  &json_error);
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tool_annotations_map, &doc);
  return CAI_OK;
}

static int cai_mcp_annotation_role_is_valid(const char *role) {
  return role != NULL &&
         (strcmp(role, "user") == 0 || strcmp(role, "assistant") == 0);
}

static int cai_mcp_validate_optional_annotations(
    const lonejson_json_value *annotations, const char *object_error,
    const char *parse_error, const char *audience_error,
    const char *priority_error, cai_error *error) {
  cai_mcp_annotations_doc doc;
  lonejson_error json_error;
  lonejson_status status;
  char *annotations_json;
  char **audience;
  size_t i;
  int rc;

  if (!cai_mcp_optional_json_object_is_valid(annotations, error)) {
    return cai_set_error(error, CAI_ERR_PROTOCOL, object_error);
  }
  if (annotations == NULL || annotations->kind == LONEJSON_JSON_VALUE_NULL) {
    return CAI_OK;
  }
  annotations_json = cai_mcp_json_value_to_cstr(annotations, error);
  if (annotations_json == NULL) {
    return error != NULL && error->code != CAI_OK ? error->code : CAI_ERR_NOMEM;
  }
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
  status = CAI_LJ->parse_cstr(CAI_LJ, &cai_mcp_annotations_map, &doc,
                              annotations_json, &json_error);
  cai_free_mem(NULL, annotations_json);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_annotations_map, &doc);
    return cai_mcp_set_json_error(error, parse_error, &json_error);
  }
  rc = CAI_OK;
  audience = (char **)doc.audience.items;
  for (i = 0U; i < doc.audience.count; i++) {
    if (!cai_mcp_annotation_role_is_valid(audience[i])) {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL, audience_error);
      break;
    }
  }
  if (rc == CAI_OK && doc.has_priority &&
      (doc.priority < 0.0 || doc.priority > 1.0)) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL, priority_error);
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_annotations_map, &doc);
  return rc;
}

static int
cai_mcp_tool_execution_task_support_is_valid(const char *task_support) {
  return task_support == NULL || strcmp(task_support, "forbidden") == 0 ||
         strcmp(task_support, "optional") == 0 ||
         strcmp(task_support, "required") == 0;
}

static int
cai_mcp_validate_optional_tool_execution(const lonejson_json_value *execution,
                                         cai_error *error) {
  cai_mcp_tool_execution_doc doc;
  lonejson_error json_error;
  lonejson_status status;
  char *execution_json;
  int rc;

  if (!cai_mcp_optional_json_object_is_valid(execution, error)) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP tool execution must be an object");
  }
  if (execution == NULL || execution->kind == LONEJSON_JSON_VALUE_NULL) {
    return CAI_OK;
  }
  execution_json = cai_mcp_json_value_to_cstr(execution, error);
  if (execution_json == NULL) {
    return error != NULL && error->code != CAI_OK ? error->code : CAI_ERR_NOMEM;
  }
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
  status = CAI_LJ->parse_cstr(CAI_LJ, &cai_mcp_tool_execution_map, &doc,
                              execution_json, &json_error);
  cai_free_mem(NULL, execution_json);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tool_execution_map, &doc);
    return cai_mcp_set_json_error(error, "failed to parse MCP tool execution",
                                  &json_error);
  }
  if (!cai_mcp_tool_execution_task_support_is_valid(doc.task_support)) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "MCP tool execution taskSupport is invalid");
  } else {
    rc = CAI_OK;
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tool_execution_map, &doc);
  return rc;
}

static int cai_mcp_copy_optional_tool_annotations(
    const cai_allocator *allocator, const lonejson_json_value *annotations,
    cai_mcp_client_tool_annotations *dst, char **title, cai_error *error) {
  cai_mcp_tool_annotations_doc doc;
  char *annotations_json;
  lonejson_error json_error;
  lonejson_status status;
  int rc;

  memset(dst, 0, sizeof(*dst));
  *title = NULL;
  if (annotations == NULL || annotations->kind == LONEJSON_JSON_VALUE_NULL) {
    *title = cai_strdup(allocator, "");
    if (*title == NULL) {
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to copy MCP tool annotations");
    }
    dst->title = *title;
    return CAI_OK;
  }
  annotations_json = cai_mcp_json_value_to_cstr(annotations, error);
  if (annotations_json == NULL) {
    return error != NULL && error->code != CAI_OK ? error->code : CAI_ERR_NOMEM;
  }
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
  status = CAI_LJ->parse_cstr(CAI_LJ, &cai_mcp_tool_annotations_map, &doc,
                              annotations_json, &json_error);
  cai_free_mem(NULL, annotations_json);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tool_annotations_map, &doc);
    return cai_mcp_set_json_error(error, "failed to parse MCP tool annotations",
                                  &json_error);
  }
  *title = cai_strdup(allocator, doc.title != NULL ? doc.title : "");
  if (*title == NULL) {
    rc = cai_set_error(error, CAI_ERR_NOMEM,
                       "failed to copy MCP tool annotations");
  } else {
    dst->title = *title;
    dst->has_read_only_hint = doc.has_read_only_hint;
    dst->read_only_hint = doc.read_only_hint;
    dst->has_destructive_hint = doc.has_destructive_hint;
    dst->destructive_hint = doc.destructive_hint;
    dst->has_idempotent_hint = doc.has_idempotent_hint;
    dst->idempotent_hint = doc.idempotent_hint;
    dst->has_open_world_hint = doc.has_open_world_hint;
    dst->open_world_hint = doc.open_world_hint;
    rc = CAI_OK;
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tool_annotations_map, &doc);
  return rc;
}

static cai_mcp_client_tool_task_support
cai_mcp_task_support_from_string(const char *task_support) {
  if (task_support == NULL) {
    return CAI_MCP_CLIENT_TOOL_TASK_SUPPORT_UNSPECIFIED;
  }
  if (strcmp(task_support, "forbidden") == 0) {
    return CAI_MCP_CLIENT_TOOL_TASK_SUPPORT_FORBIDDEN;
  }
  if (strcmp(task_support, "optional") == 0) {
    return CAI_MCP_CLIENT_TOOL_TASK_SUPPORT_OPTIONAL;
  }
  if (strcmp(task_support, "required") == 0) {
    return CAI_MCP_CLIENT_TOOL_TASK_SUPPORT_REQUIRED;
  }
  return CAI_MCP_CLIENT_TOOL_TASK_SUPPORT_UNSPECIFIED;
}

static int cai_mcp_copy_optional_tool_execution(
    const lonejson_json_value *execution,
    cai_mcp_client_tool_task_support *task_support, cai_error *error) {
  cai_mcp_tool_execution_doc doc;
  char *execution_json;
  lonejson_error json_error;
  lonejson_status status;

  *task_support = CAI_MCP_CLIENT_TOOL_TASK_SUPPORT_UNSPECIFIED;
  if (execution == NULL || execution->kind == LONEJSON_JSON_VALUE_NULL) {
    return CAI_OK;
  }
  execution_json = cai_mcp_json_value_to_cstr(execution, error);
  if (execution_json == NULL) {
    return error != NULL && error->code != CAI_OK ? error->code : CAI_ERR_NOMEM;
  }
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
  status = CAI_LJ->parse_cstr(CAI_LJ, &cai_mcp_tool_execution_map, &doc,
                              execution_json, &json_error);
  cai_free_mem(NULL, execution_json);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tool_execution_map, &doc);
    return cai_mcp_set_json_error(error, "failed to parse MCP tool execution",
                                  &json_error);
  }
  *task_support = cai_mcp_task_support_from_string(doc.task_support);
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tool_execution_map, &doc);
  return CAI_OK;
}

static int cai_mcp_copy_optional_annotations(
    const cai_allocator *allocator, const lonejson_json_value *annotations,
    cai_mcp_client_annotations *dst, char ***audience, char **last_modified,
    cai_error *error) {
  cai_mcp_annotations_doc doc;
  char *annotations_json;
  lonejson_error json_error;
  lonejson_status status;
  int rc;

  memset(dst, 0, sizeof(*dst));
  *audience = NULL;
  *last_modified = NULL;
  if (annotations == NULL || annotations->kind == LONEJSON_JSON_VALUE_NULL) {
    *last_modified = cai_strdup(allocator, "");
    if (*last_modified == NULL) {
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to copy MCP annotations");
    }
    dst->last_modified = *last_modified;
    return CAI_OK;
  }
  annotations_json = cai_mcp_json_value_to_cstr(annotations, error);
  if (annotations_json == NULL) {
    return error != NULL && error->code != CAI_OK ? error->code : CAI_ERR_NOMEM;
  }
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
  status = CAI_LJ->parse_cstr(CAI_LJ, &cai_mcp_annotations_map, &doc,
                              annotations_json, &json_error);
  cai_free_mem(NULL, annotations_json);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_annotations_map, &doc);
    return cai_mcp_set_json_error(error, "failed to parse MCP annotations",
                                  &json_error);
  }
  rc = cai_mcp_copy_lonejson_string_array(allocator, &doc.audience, audience,
                                          &dst->audience_count, error);
  if (rc == CAI_OK) {
    *last_modified = cai_strdup(
        allocator, doc.last_modified != NULL ? doc.last_modified : "");
    if (*last_modified == NULL) {
      rc =
          cai_set_error(error, CAI_ERR_NOMEM, "failed to copy MCP annotations");
    }
  }
  if (rc == CAI_OK) {
    dst->audience = (const char *const *)*audience;
    dst->last_modified = *last_modified;
    dst->has_priority = doc.has_priority;
    dst->priority = doc.priority;
  } else {
    cai_mcp_string_array_cleanup(allocator, *audience, dst->audience_count);
    *audience = NULL;
    cai_free_mem(allocator, *last_modified);
    *last_modified = NULL;
    memset(dst, 0, sizeof(*dst));
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_annotations_map, &doc);
  return rc;
}

static void cai_mcp_client_prompt_arguments_cleanup(
    const cai_allocator *allocator, cai_mcp_client_prompt_argument *arguments,
    size_t count) {
  size_t i;

  if (arguments == NULL) {
    return;
  }
  for (i = 0U; i < count; i++) {
    cai_mcp_free_const_mem(allocator, arguments[i].name);
    cai_mcp_free_const_mem(allocator, arguments[i].title);
    cai_mcp_free_const_mem(allocator, arguments[i].description);
  }
  cai_free_mem(allocator, arguments);
}

static int cai_mcp_copy_optional_prompt_arguments(
    const cai_allocator *allocator, const lonejson_json_value *arguments,
    cai_mcp_client_prompt_argument **dst_arguments, size_t *dst_count,
    cai_error *error) {
  cai_mcp_prompt_arguments_doc doc;
  cai_mcp_prompt_argument_doc *src;
  cai_mcp_client_prompt_argument *items;
  size_t i;
  int rc;

  *dst_arguments = NULL;
  *dst_count = 0U;
  if (arguments == NULL || arguments->kind == LONEJSON_JSON_VALUE_NULL) {
    return CAI_OK;
  }
  memset(&doc, 0, sizeof(doc));
  rc = cai_mcp_parse_wrapped_json_value(
      arguments, "arguments", &cai_mcp_prompt_arguments_map, &doc,
      "failed to allocate MCP prompt arguments metadata JSON",
      "failed to parse MCP prompt arguments", error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (doc.arguments.count == 0U) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_prompt_arguments_map, &doc);
    return CAI_OK;
  }
  items = (cai_mcp_client_prompt_argument *)cai_alloc(
      allocator, doc.arguments.count * sizeof(*items));
  if (items == NULL) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_prompt_arguments_map, &doc);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate MCP prompt arguments metadata");
  }
  memset(items, 0, doc.arguments.count * sizeof(*items));
  src = (cai_mcp_prompt_argument_doc *)doc.arguments.items;
  for (i = 0U; i < doc.arguments.count; i++) {
    items[i].name = cai_strdup(allocator, src[i].name);
    items[i].title =
        cai_strdup(allocator, src[i].title != NULL ? src[i].title : "");
    items[i].description = cai_strdup(
        allocator, src[i].description != NULL ? src[i].description : "");
    items[i].has_required = src[i].has_required;
    items[i].required = src[i].required;
    if (items[i].name == NULL || items[i].title == NULL ||
        items[i].description == NULL) {
      cai_mcp_client_prompt_arguments_cleanup(allocator, items,
                                              doc.arguments.count);
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_prompt_arguments_map, &doc);
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to copy MCP prompt arguments metadata");
    }
  }
  *dst_arguments = items;
  *dst_count = doc.arguments.count;
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_prompt_arguments_map, &doc);
  return CAI_OK;
}

static void cai_mcp_json_cstr_skip_ws(const char **cursor) {
  while (**cursor == ' ' || **cursor == '\t' || **cursor == '\r' ||
         **cursor == '\n') {
    (*cursor)++;
  }
}

static int cai_mcp_json_cstr_skip_string(const char **cursor) {
  const char *p;
  size_t i;

  p = *cursor;
  if (*p != '"') {
    return 0;
  }
  p++;
  for (;;) {
    if (*p == '\0') {
      return 0;
    }
    if (*p == '"') {
      *cursor = p + 1;
      return 1;
    }
    if (*p == '\\') {
      p++;
      if (*p == '\0') {
        return 0;
      }
      if (*p == 'u') {
        for (i = 0U; i < 4U; i++) {
          p++;
          if (*p == '\0') {
            return 0;
          }
        }
      }
    }
    p++;
  }
}

static int cai_mcp_json_cstr_skip_value(const char **cursor) {
  int stack[64];
  size_t depth;
  const char *p;
  const char *start;

  cai_mcp_json_cstr_skip_ws(cursor);
  p = *cursor;
  if (*p == '"') {
    return cai_mcp_json_cstr_skip_string(cursor);
  }
  if (*p != '{' && *p != '[') {
    start = p;
    while (*p != '\0' && *p != ',' && *p != '}' && *p != ']' && *p != ' ' &&
           *p != '\t' && *p != '\r' && *p != '\n') {
      p++;
    }
    *cursor = p;
    return p != start;
  }
  stack[0] = *p;
  depth = 1U;
  p++;
  *cursor = p;
  while (depth != 0U) {
    if (**cursor == '\0') {
      return 0;
    }
    if (**cursor == '"') {
      if (!cai_mcp_json_cstr_skip_string(cursor)) {
        return 0;
      }
      continue;
    }
    if (**cursor == '{' || **cursor == '[') {
      if (depth >= sizeof(stack) / sizeof(stack[0])) {
        return 0;
      }
      stack[depth++] = **cursor;
    } else if (**cursor == '}' || **cursor == ']') {
      if ((**cursor == '}' && stack[depth - 1U] != '{') ||
          (**cursor == ']' && stack[depth - 1U] != '[')) {
        return 0;
      }
      depth--;
    }
    (*cursor)++;
  }
  return 1;
}

static int cai_mcp_validate_json_schema_properties(
    const lonejson_json_value *properties, const char *object_message,
    const char *value_message, const char *parse_message, cai_error *error) {
  char *properties_json;
  const char *cursor;
  int rc;

  if (properties == NULL || properties->kind == LONEJSON_JSON_VALUE_NULL) {
    return CAI_OK;
  }
  if (!cai_mcp_json_value_root_is(properties, '{', error)) {
    return cai_set_error(error, CAI_ERR_PROTOCOL, object_message);
  }
  properties_json = cai_mcp_json_value_to_cstr(properties, error);
  if (properties_json == NULL) {
    return error != NULL && error->code != CAI_OK ? error->code : CAI_ERR_NOMEM;
  }
  cursor = properties_json;
  rc = CAI_OK;
  cai_mcp_json_cstr_skip_ws(&cursor);
  if (*cursor != '{') {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL, object_message);
    goto done;
  }
  cursor++;
  cai_mcp_json_cstr_skip_ws(&cursor);
  if (*cursor == '}') {
    cursor++;
    goto trailing;
  }
  for (;;) {
    if (!cai_mcp_json_cstr_skip_string(&cursor)) {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL, parse_message);
      goto done;
    }
    cai_mcp_json_cstr_skip_ws(&cursor);
    if (*cursor != ':') {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL, parse_message);
      goto done;
    }
    cursor++;
    cai_mcp_json_cstr_skip_ws(&cursor);
    if (*cursor != '{') {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL, value_message);
      goto done;
    }
    if (!cai_mcp_json_cstr_skip_value(&cursor)) {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL, parse_message);
      goto done;
    }
    cai_mcp_json_cstr_skip_ws(&cursor);
    if (*cursor == '}') {
      cursor++;
      break;
    }
    if (*cursor != ',') {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL, parse_message);
      goto done;
    }
    cursor++;
    cai_mcp_json_cstr_skip_ws(&cursor);
  }

trailing:
  cai_mcp_json_cstr_skip_ws(&cursor);
  if (*cursor != '\0') {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL, parse_message);
  }

done:
  cai_free_mem(NULL, properties_json);
  return rc;
}

static int cai_mcp_validate_json_schema_required(
    const lonejson_json_value *required, const char *array_message,
    const char *value_message, const char *parse_message, cai_error *error) {
  char *required_json;
  const char *cursor;
  int rc;

  if (required == NULL || required->kind == LONEJSON_JSON_VALUE_NULL) {
    return CAI_OK;
  }
  if (!cai_mcp_json_value_root_is(required, '[', error)) {
    return cai_set_error(error, CAI_ERR_PROTOCOL, array_message);
  }
  required_json = cai_mcp_json_value_to_cstr(required, error);
  if (required_json == NULL) {
    return error != NULL && error->code != CAI_OK ? error->code : CAI_ERR_NOMEM;
  }
  cursor = required_json;
  rc = CAI_OK;
  cai_mcp_json_cstr_skip_ws(&cursor);
  if (*cursor != '[') {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL, array_message);
    goto done;
  }
  cursor++;
  cai_mcp_json_cstr_skip_ws(&cursor);
  if (*cursor == ']') {
    cursor++;
    goto trailing;
  }
  for (;;) {
    if (*cursor != '"') {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL, value_message);
      goto done;
    }
    if (!cai_mcp_json_cstr_skip_string(&cursor)) {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL, parse_message);
      goto done;
    }
    cai_mcp_json_cstr_skip_ws(&cursor);
    if (*cursor == ']') {
      cursor++;
      break;
    }
    if (*cursor != ',') {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL, parse_message);
      goto done;
    }
    cursor++;
    cai_mcp_json_cstr_skip_ws(&cursor);
  }

trailing:
  cai_mcp_json_cstr_skip_ws(&cursor);
  if (*cursor != '\0') {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL, parse_message);
  }

done:
  cai_free_mem(NULL, required_json);
  return rc;
}

static char *cai_mcp_dup_simple_json_string_token(
    const cai_allocator *allocator, const char *begin, const char **end,
    const char *unsupported_message, cai_error *error) {
  const char *p;
  char *out;
  size_t len;

  if (begin == NULL || *begin != '"') {
    cai_set_error(error, CAI_ERR_PROTOCOL, unsupported_message);
    return NULL;
  }
  p = begin + 1;
  while (*p != '\0' && *p != '"') {
    if (*p == '\\') {
      cai_set_error(error, CAI_ERR_PROTOCOL, unsupported_message);
      return NULL;
    }
    p++;
  }
  if (*p != '"') {
    cai_set_error(error, CAI_ERR_PROTOCOL, unsupported_message);
    return NULL;
  }
  len = (size_t)(p - begin - 1);
  out = (char *)cai_alloc(allocator, len + 1U);
  if (out == NULL) {
    cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate MCP schema name");
    return NULL;
  }
  memcpy(out, begin + 1, len);
  out[len] = '\0';
  if (end != NULL) {
    *end = p + 1;
  }
  return out;
}

static int cai_mcp_collect_json_object_keys(
    const cai_allocator *allocator, const lonejson_json_value *object_value,
    char ***items_out, size_t *count_out, const char *parse_message,
    const char *unsupported_message, cai_error *error) {
  char *json;
  const char *cursor;
  char **items;
  size_t count;
  size_t capacity;
  int rc;

  if (items_out != NULL) {
    *items_out = NULL;
  }
  if (count_out != NULL) {
    *count_out = 0U;
  }
  if (object_value == NULL || object_value->kind == LONEJSON_JSON_VALUE_NULL) {
    return CAI_OK;
  }
  json = cai_mcp_json_value_to_cstr(object_value, error);
  if (json == NULL) {
    return error != NULL && error->code != CAI_OK ? error->code : CAI_ERR_NOMEM;
  }
  cursor = json;
  items = NULL;
  count = 0U;
  capacity = 0U;
  rc = CAI_OK;
  cai_mcp_json_cstr_skip_ws(&cursor);
  if (*cursor != '{') {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL, parse_message);
    goto done;
  }
  cursor++;
  cai_mcp_json_cstr_skip_ws(&cursor);
  if (*cursor == '}') {
    goto done;
  }
  for (;;) {
    char *name;
    char **grown;
    name = cai_mcp_dup_simple_json_string_token(allocator, cursor, &cursor,
                                                unsupported_message, error);
    if (name == NULL) {
      rc = error != NULL && error->code != CAI_OK ? error->code : CAI_ERR_NOMEM;
      goto done;
    }
    if (count == capacity) {
      size_t new_capacity = capacity == 0U ? 4U : capacity * 2U;
      grown = (char **)cai_realloc_mem(allocator, items,
                                       new_capacity * sizeof(*items));
      if (grown == NULL) {
        cai_free_mem(allocator, name);
        rc = cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate MCP schema names");
        goto done;
      }
      items = grown;
      capacity = new_capacity;
    }
    items[count++] = name;
    cai_mcp_json_cstr_skip_ws(&cursor);
    if (*cursor != ':') {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL, parse_message);
      goto done;
    }
    cursor++;
    if (!cai_mcp_json_cstr_skip_value(&cursor)) {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL, parse_message);
      goto done;
    }
    cai_mcp_json_cstr_skip_ws(&cursor);
    if (*cursor == '}') {
      break;
    }
    if (*cursor != ',') {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL, parse_message);
      goto done;
    }
    cursor++;
    cai_mcp_json_cstr_skip_ws(&cursor);
  }

done:
  cai_free_mem(NULL, json);
  if (rc != CAI_OK) {
    cai_mcp_string_array_cleanup(allocator, items, count);
    return rc;
  }
  if (items_out != NULL) {
    *items_out = items;
  }
  if (count_out != NULL) {
    *count_out = count;
  }
  return CAI_OK;
}

static int cai_mcp_collect_json_string_array(
    const cai_allocator *allocator, const lonejson_json_value *array_value,
    char ***items_out, size_t *count_out, const char *parse_message,
    const char *unsupported_message, cai_error *error) {
  char *json;
  const char *cursor;
  char **items;
  size_t count;
  size_t capacity;
  int rc;

  if (items_out != NULL) {
    *items_out = NULL;
  }
  if (count_out != NULL) {
    *count_out = 0U;
  }
  if (array_value == NULL || array_value->kind == LONEJSON_JSON_VALUE_NULL) {
    return CAI_OK;
  }
  json = cai_mcp_json_value_to_cstr(array_value, error);
  if (json == NULL) {
    return error != NULL && error->code != CAI_OK ? error->code : CAI_ERR_NOMEM;
  }
  cursor = json;
  items = NULL;
  count = 0U;
  capacity = 0U;
  rc = CAI_OK;
  cai_mcp_json_cstr_skip_ws(&cursor);
  if (*cursor != '[') {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL, parse_message);
    goto done;
  }
  cursor++;
  cai_mcp_json_cstr_skip_ws(&cursor);
  if (*cursor == ']') {
    goto done;
  }
  for (;;) {
    char *name;
    char **grown;
    name = cai_mcp_dup_simple_json_string_token(allocator, cursor, &cursor,
                                                unsupported_message, error);
    if (name == NULL) {
      rc = error != NULL && error->code != CAI_OK ? error->code : CAI_ERR_NOMEM;
      goto done;
    }
    if (count == capacity) {
      size_t new_capacity = capacity == 0U ? 4U : capacity * 2U;
      grown = (char **)cai_realloc_mem(allocator, items,
                                       new_capacity * sizeof(*items));
      if (grown == NULL) {
        cai_free_mem(allocator, name);
        rc = cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate MCP schema names");
        goto done;
      }
      items = grown;
      capacity = new_capacity;
    }
    items[count++] = name;
    cai_mcp_json_cstr_skip_ws(&cursor);
    if (*cursor == ']') {
      break;
    }
    if (*cursor != ',') {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL, parse_message);
      goto done;
    }
    cursor++;
    cai_mcp_json_cstr_skip_ws(&cursor);
  }

done:
  cai_free_mem(NULL, json);
  if (rc != CAI_OK) {
    cai_mcp_string_array_cleanup(allocator, items, count);
    return rc;
  }
  if (items_out != NULL) {
    *items_out = items;
  }
  if (count_out != NULL) {
    *count_out = count;
  }
  return CAI_OK;
}

static void cai_mcp_client_schema_cleanup(const cai_allocator *allocator,
                                          cai_mcp_client_schema *schema,
                                          char **schema_uri, char **type,
                                          char ***properties,
                                          char ***required) {
  if (schema == NULL) {
    return;
  }
  cai_free_mem(allocator, schema_uri != NULL ? *schema_uri : NULL);
  cai_free_mem(allocator, type != NULL ? *type : NULL);
  cai_mcp_string_array_cleanup(allocator,
                               properties != NULL ? *properties : NULL,
                               schema->property_count);
  cai_mcp_string_array_cleanup(allocator, required != NULL ? *required : NULL,
                               schema->required_count);
  if (schema_uri != NULL) {
    *schema_uri = NULL;
  }
  if (type != NULL) {
    *type = NULL;
  }
  if (properties != NULL) {
    *properties = NULL;
  }
  if (required != NULL) {
    *required = NULL;
  }
  memset(schema, 0, sizeof(*schema));
}

static int cai_mcp_copy_schema(const cai_allocator *allocator,
                               const lonejson_json_value *schema_value,
                               int required_schema,
                               cai_mcp_client_schema *schema, char **schema_uri,
                               char **type, char ***properties,
                               char ***required, cai_error *error) {
  cai_mcp_json_schema_doc doc;
  char *schema_json;
  lonejson_error json_error;
  lonejson_status status;
  int rc;

  if (schema_value == NULL || schema_value->kind == LONEJSON_JSON_VALUE_NULL) {
    return required_schema
               ? cai_set_error(error, CAI_ERR_PROTOCOL,
                               "MCP tool inputSchema must be an object")
               : CAI_OK;
  }
  schema_json = cai_mcp_json_value_to_cstr(schema_value, error);
  if (schema_json == NULL) {
    return error != NULL && error->code != CAI_OK ? error->code : CAI_ERR_NOMEM;
  }
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
  status = CAI_LJ->parse_cstr(CAI_LJ, &cai_mcp_json_schema_map, &doc,
                              schema_json, &json_error);
  cai_free_mem(NULL, schema_json);
  if (status != LONEJSON_STATUS_OK) {
    return cai_mcp_set_json_error(error, "failed to parse MCP tool schema",
                                  &json_error);
  }
  *schema_uri = cai_strdup(allocator, doc.schema != NULL ? doc.schema : "");
  *type = cai_strdup(allocator, doc.type != NULL ? doc.type : "");
  rc = cai_mcp_collect_json_object_keys(
      allocator, &doc.properties, properties, &schema->property_count,
      "failed to parse MCP tool schema properties",
      "MCP tool schema property names with JSON escapes are unsupported",
      error);
  if (rc == CAI_OK) {
    rc = cai_mcp_collect_json_string_array(
        allocator, &doc.required, required, &schema->required_count,
        "failed to parse MCP tool schema required",
        "MCP tool schema required names with JSON escapes are unsupported",
        error);
  }
  if (rc == CAI_OK && (*schema_uri == NULL || *type == NULL)) {
    rc = cai_set_error(error, CAI_ERR_NOMEM,
                       "failed to copy MCP tool schema metadata");
  }
  if (rc == CAI_OK) {
    schema->schema_uri = *schema_uri;
    schema->type = *type;
    schema->properties = (const char *const *)*properties;
    schema->required = (const char *const *)*required;
  } else {
    cai_mcp_client_schema_cleanup(allocator, schema, schema_uri, type,
                                  properties, required);
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_json_schema_map, &doc);
  return rc;
}

static int cai_mcp_validate_tool_schema_type(
    const lonejson_json_value *schema, const char *object_message,
    const char *parse_message, const char *type_message,
    const char *properties_object_message, const char *properties_value_message,
    const char *required_array_message, const char *required_value_message,
    int required, cai_error *error) {
  cai_mcp_json_schema_doc doc;
  lonejson_error json_error;
  lonejson_status status;
  char *schema_json;
  int rc;

  if (schema == NULL || schema->kind == LONEJSON_JSON_VALUE_NULL) {
    return required ? cai_set_error(error, CAI_ERR_PROTOCOL, object_message)
                    : CAI_OK;
  }
  if (!cai_mcp_json_value_is_object(schema, error)) {
    return cai_set_error(error, CAI_ERR_PROTOCOL, object_message);
  }
  schema_json = cai_mcp_json_value_to_cstr(schema, error);
  if (schema_json == NULL) {
    return error != NULL && error->code != CAI_OK ? error->code : CAI_ERR_NOMEM;
  }
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
  status = CAI_LJ->parse_cstr(CAI_LJ, &cai_mcp_json_schema_map, &doc,
                              schema_json, &json_error);
  cai_free_mem(NULL, schema_json);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_json_schema_map, &doc);
    return cai_mcp_set_json_error(error, parse_message, &json_error);
  }
  if (doc.type == NULL || strcmp(doc.type, "object") != 0) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL, type_message);
  } else {
    rc = cai_mcp_validate_json_schema_properties(
        &doc.properties, properties_object_message, properties_value_message,
        parse_message, error);
    if (rc == CAI_OK) {
      rc = cai_mcp_validate_json_schema_required(
          &doc.required, required_array_message, required_value_message,
          parse_message, error);
    }
  }
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_json_schema_map, &doc);
  return rc;
}

static int cai_mcp_validate_list_tool_doc(const cai_mcp_list_tool_doc *tool,
                                          cai_error *error) {
  int rc;

  if (tool == NULL) {
    return cai_set_error(error, CAI_ERR_PROTOCOL, "MCP tool is required");
  }
  rc = cai_mcp_validate_tool_schema_type(
      &tool->input_schema, "MCP tool inputSchema must be an object",
      "failed to parse MCP tool inputSchema",
      "MCP tool inputSchema type must be object",
      "MCP tool inputSchema properties must be an object",
      "MCP tool inputSchema properties values must be objects",
      "MCP tool inputSchema required must be an array",
      "MCP tool inputSchema required values must be strings", 1, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_mcp_validate_tool_schema_type(
      &tool->output_schema, "MCP tool outputSchema must be an object",
      "failed to parse MCP tool outputSchema",
      "MCP tool outputSchema type must be object",
      "MCP tool outputSchema properties must be an object",
      "MCP tool outputSchema properties values must be objects",
      "MCP tool outputSchema required must be an array",
      "MCP tool outputSchema required values must be strings", 0, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_mcp_validate_optional_tool_annotations(&tool->annotations, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_mcp_validate_optional_icons(
      &tool->icons, "MCP tool icons must be an array",
      "failed to parse MCP tool icons",
      "MCP tool icon theme must be light or dark", error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_mcp_validate_optional_tool_execution(&tool->execution, error);
}

static char *cai_mcp_tool_display_title(const cai_allocator *allocator,
                                        const cai_mcp_list_tool_doc *tool,
                                        cai_error *error) {
  cai_mcp_tool_annotations_doc doc;
  lonejson_error json_error;
  lonejson_status status;
  char *annotations_json;
  char *title;

  if (tool->title != NULL) {
    return cai_strdup(allocator, tool->title);
  }
  if (tool->annotations.kind == LONEJSON_JSON_VALUE_NULL) {
    return cai_strdup(allocator, tool->name);
  }
  annotations_json = cai_mcp_json_value_to_cstr(&tool->annotations, error);
  if (annotations_json == NULL) {
    return NULL;
  }
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
  status = CAI_LJ->parse_cstr(CAI_LJ, &cai_mcp_tool_annotations_map, &doc,
                              annotations_json, &json_error);
  cai_free_mem(NULL, annotations_json);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tool_annotations_map, &doc);
    cai_mcp_set_json_error(error, "failed to parse MCP tool annotations",
                           &json_error);
    return NULL;
  }
  title = cai_strdup(allocator, doc.title != NULL ? doc.title : tool->name);
  CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tool_annotations_map, &doc);
  return title;
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
  rc = cai_mcp_response_reader_init(response, "MCP tools/list response", 0,
                                    &json_body, &reader, &has_error, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
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
    rc = cai_mcp_validate_list_tool_doc(&src_tools[i], error);
    if (rc != CAI_OK) {
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tools_list_response_map, &doc);
      json_body.cleanup(&json_body);
      return rc;
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
          cai_mcp_tool_display_title(&impl->allocator, &src_tools[i], error);
      dst->description = cai_strdup(
          &impl->allocator,
          src_tools[i].description != NULL ? src_tools[i].description : "");
      rc = cai_mcp_copy_schema(
          &impl->allocator, &src_tools[i].input_schema, 1, &dst->input_schema,
          &dst->input_schema_uri, &dst->input_schema_type,
          &dst->input_schema_properties, &dst->input_schema_required, error);
      if (rc == CAI_OK) {
        rc = cai_mcp_copy_schema(
            &impl->allocator, &src_tools[i].output_schema, 0,
            &dst->output_schema, &dst->output_schema_uri,
            &dst->output_schema_type, &dst->output_schema_properties,
            &dst->output_schema_required, error);
      }
      if (rc == CAI_OK) {
        rc = cai_mcp_copy_optional_tool_annotations(
            &impl->allocator, &src_tools[i].annotations, &dst->annotations,
            &dst->annotations_title, error);
      }
      if (rc == CAI_OK) {
        rc = cai_mcp_copy_optional_icons(&impl->allocator, &src_tools[i].icons,
                                         &dst->icons,
                                         &dst->public_tool.icon_count, error);
      }
      if (rc == CAI_OK) {
        rc = cai_mcp_copy_optional_tool_execution(
            &src_tools[i].execution, &dst->public_tool.task_support, error);
      }
      dst->input_schema_json = cai_mcp_json_value_to_cstr_with_allocator(
          &impl->allocator, &src_tools[i].input_schema, error);
      dst->output_schema_json = cai_mcp_optional_json_value_to_cstr(
          &impl->allocator, &src_tools[i].output_schema, error);
      if (dst->name == NULL || dst->title == NULL || dst->description == NULL ||
          rc != CAI_OK || dst->input_schema_json == NULL ||
          dst->output_schema_json == NULL) {
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
      dst->public_tool.input_schema = &dst->input_schema;
      dst->public_tool.output_schema =
          dst->output_schema.type != NULL ? &dst->output_schema : NULL;
      dst->public_tool.annotations = dst->annotations;
      dst->public_tool.icons = dst->icons;
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
  rc = cai_mcp_response_reader_init(response, "MCP resources/list response", 0,
                                    &json_body, &reader, &has_error, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
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
    rc = cai_mcp_validate_optional_icons(
        &src_resources[i].icons, "MCP resource icons must be an array",
        "failed to parse MCP resource icons",
        "MCP resource icon theme must be light or dark", error);
    if (rc != CAI_OK) {
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_resources_list_response_map, &doc);
      json_body.cleanup(&json_body);
      return rc;
    }
    rc = cai_mcp_validate_optional_annotations(
        &src_resources[i].annotations,
        "MCP resource annotations must be an object",
        "failed to parse MCP resource annotations",
        "MCP resource annotation audience role is invalid",
        "MCP resource annotation priority must be between 0 and 1", error);
    if (rc != CAI_OK) {
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_resources_list_response_map, &doc);
      json_body.cleanup(&json_body);
      return rc;
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
      dst->title = cai_strdup(&impl->allocator, src_resources[i].title != NULL
                                                    ? src_resources[i].title
                                                    : src_resources[i].name);
      dst->description =
          cai_strdup(&impl->allocator, src_resources[i].description != NULL
                                           ? src_resources[i].description
                                           : "");
      dst->mime_type = cai_strdup(
          &impl->allocator,
          src_resources[i].mime_type != NULL ? src_resources[i].mime_type : "");
      rc = cai_mcp_copy_optional_icons(&impl->allocator,
                                       &src_resources[i].icons, &dst->icons,
                                       &dst->public_resource.icon_count, error);
      if (rc == CAI_OK) {
        rc = cai_mcp_copy_optional_annotations(
            &impl->allocator, &src_resources[i].annotations, &dst->annotations,
            &dst->annotation_audience, &dst->annotation_last_modified, error);
      }
      dst->has_size = src_resources[i].has_size;
      dst->size = (long long)src_resources[i].size;
      if (dst->uri == NULL || dst->name == NULL || dst->title == NULL ||
          dst->description == NULL || dst->mime_type == NULL || rc != CAI_OK) {
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
      dst->public_resource.icons = dst->icons;
      dst->public_resource.annotations = dst->annotations;
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
  rc = cai_mcp_response_reader_init(response,
                                    "MCP resources/templates/list response", 0,
                                    &json_body, &reader, &has_error, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
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
    rc = cai_mcp_validate_optional_icons(
        &src_resource_templates[i].icons,
        "MCP resource template icons must be an array",
        "failed to parse MCP resource template icons",
        "MCP resource template icon theme must be light or dark", error);
    if (rc != CAI_OK) {
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_resource_templates_list_response_map,
                      &doc);
      json_body.cleanup(&json_body);
      return rc;
    }
    rc = cai_mcp_validate_optional_annotations(
        &src_resource_templates[i].annotations,
        "MCP resource template annotations must be an object",
        "failed to parse MCP resource template annotations",
        "MCP resource template annotation audience role is invalid",
        "MCP resource template annotation priority must be between 0 and 1",
        error);
    if (rc != CAI_OK) {
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_resource_templates_list_response_map,
                      &doc);
      json_body.cleanup(&json_body);
      return rc;
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
      dst->title =
          cai_strdup(&impl->allocator, src_resource_templates[i].title != NULL
                                           ? src_resource_templates[i].title
                                           : src_resource_templates[i].name);
      dst->description = cai_strdup(
          &impl->allocator, src_resource_templates[i].description != NULL
                                ? src_resource_templates[i].description
                                : "");
      dst->mime_type = cai_strdup(&impl->allocator,
                                  src_resource_templates[i].mime_type != NULL
                                      ? src_resource_templates[i].mime_type
                                      : "");
      rc = cai_mcp_copy_optional_icons(
          &impl->allocator, &src_resource_templates[i].icons, &dst->icons,
          &dst->public_resource_template.icon_count, error);
      if (rc == CAI_OK) {
        rc = cai_mcp_copy_optional_annotations(
            &impl->allocator, &src_resource_templates[i].annotations,
            &dst->annotations, &dst->annotation_audience,
            &dst->annotation_last_modified, error);
      }
      if (dst->uri_template == NULL || dst->name == NULL ||
          dst->title == NULL || dst->description == NULL ||
          dst->mime_type == NULL || rc != CAI_OK) {
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
      dst->public_resource_template.icons = dst->icons;
      dst->public_resource_template.annotations = dst->annotations;
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
  rc = cai_mcp_response_reader_init(response, "MCP prompts/list response", 0,
                                    &json_body, &reader, &has_error, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
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
    rc = cai_mcp_validate_optional_prompt_arguments(&src_prompts[i].arguments,
                                                    error);
    if (rc != CAI_OK) {
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_prompts_list_response_map, &doc);
      json_body.cleanup(&json_body);
      return rc;
    }
    rc = cai_mcp_validate_optional_icons(
        &src_prompts[i].icons, "MCP prompt icons must be an array",
        "failed to parse MCP prompt icons",
        "MCP prompt icon theme must be light or dark", error);
    if (rc != CAI_OK) {
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_prompts_list_response_map, &doc);
      json_body.cleanup(&json_body);
      return rc;
    }
  }
  base_count = impl->prompt_count;
  rc = cai_mcp_client_reserve_prompts(
      impl, base_count + doc.result.prompts.count, error);
  if (rc == CAI_OK) {
    for (i = 0U; i < doc.result.prompts.count; i++) {
      cai_mcp_client_prompt_impl *dst = &impl->prompts[base_count + i];
      dst->name = cai_strdup(&impl->allocator, src_prompts[i].name);
      dst->title = cai_strdup(&impl->allocator, src_prompts[i].title != NULL
                                                    ? src_prompts[i].title
                                                    : src_prompts[i].name);
      dst->description = cai_strdup(
          &impl->allocator,
          src_prompts[i].description != NULL ? src_prompts[i].description : "");
      rc = cai_mcp_copy_optional_prompt_arguments(
          &impl->allocator, &src_prompts[i].arguments, &dst->arguments,
          &dst->public_prompt.argument_count, error);
      if (rc == CAI_OK) {
        rc = cai_mcp_copy_optional_icons(&impl->allocator,
                                         &src_prompts[i].icons, &dst->icons,
                                         &dst->public_prompt.icon_count, error);
      }
      if (dst->name == NULL || dst->title == NULL || dst->description == NULL ||
          rc != CAI_OK) {
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
      dst->public_prompt.arguments = dst->arguments;
      dst->public_prompt.icons = dst->icons;
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

static int cai_mcp_base64_char_is_valid(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '+' || c == '/';
}

static int cai_mcp_base64_is_valid(const char *text) {
  size_t len;
  size_t i;
  size_t padding;
  size_t first_padding;

  if (text == NULL) {
    return 0;
  }
  len = strlen(text);
  if (len % 4U == 1U) {
    return 0;
  }
  padding = 0U;
  first_padding = len;
  for (i = 0U; i < len; i++) {
    if (text[i] == '=') {
      if (first_padding == len) {
        first_padding = i;
      }
      padding++;
      if (padding > 2U) {
        return 0;
      }
    } else {
      if (padding != 0U || !cai_mcp_base64_char_is_valid(text[i])) {
        return 0;
      }
    }
  }
  if (padding != 0U &&
      (len % 4U != 0U || first_padding < len - 2U || first_padding < 2U)) {
    return 0;
  }
  return 1;
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

  rc = cai_mcp_response_reader_init(response, "MCP resources/read response", 0,
                                    &json_body, &reader, &has_error, error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (has_error) {
    json_body.cleanup(&json_body);
    return CAI_OK;
  }
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
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
    if (contents[i].blob != NULL &&
        !cai_mcp_base64_is_valid(contents[i].blob)) {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP resource blob must be base64");
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
  rc = cai_mcp_validate_optional_annotations(
      &content->annotations, "MCP content annotations must be an object",
      "failed to parse MCP content annotations",
      "MCP content annotation audience role is invalid",
      "MCP content annotation priority must be between 0 and 1", error);
  if (rc != CAI_OK) {
    return rc;
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
    if (!cai_mcp_base64_is_valid(content->data)) {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP media content data must be base64");
    }
  } else if (strcmp(content->type, "resource_link") == 0) {
    if (content->uri == NULL || content->name == NULL) {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP resource_link content requires uri and name");
    }
    rc = cai_mcp_validate_optional_icons(
        &content->icons, "MCP resource_link icons must be an array",
        "failed to parse MCP resource_link icons",
        "MCP resource_link icon theme must be light or dark", error);
    if (rc != CAI_OK) {
      return rc;
    }
    if (content->has_size && content->size < 0) {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP resource_link size must be non-negative");
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
    } else if (resource.blob != NULL &&
               !cai_mcp_base64_is_valid(resource.blob)) {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP resource blob must be base64");
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
  cai_mcp_tool_content_doc *contents;
  lonejson_spooled json_body;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  size_t i;
  int has_error;
  int rc;

  rc = cai_mcp_response_reader_init(response, "MCP tools/call response", 1,
                                    &json_body, &reader, &has_error, error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (has_error) {
    json_body.cleanup(&json_body);
    return CAI_OK;
  }
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
  status = CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_tool_call_response_map, &doc,
                                cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_tool_call_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to parse MCP tools/call",
                                  &json_error);
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

  rc = cai_mcp_response_reader_init(response, "MCP prompts/get response", 0,
                                    &json_body, &reader, &has_error, error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (has_error) {
    json_body.cleanup(&json_body);
    return CAI_OK;
  }
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
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

  rc =
      cai_mcp_response_reader_init(response, "MCP completion/complete response",
                                   0, &json_body, &reader, &has_error, error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (has_error) {
    json_body.cleanup(&json_body);
    return CAI_OK;
  }
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&json_error);
  status = CAI_LJ->parse_reader(CAI_LJ, &cai_mcp_completion_response_map, &doc,
                                cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_completion_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(
        error, "failed to parse MCP completion/complete", &json_error);
  }
  rc = CAI_OK;
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

static int cai_mcp_spooled_append_bytes(lonejson_spooled *spool,
                                        const char *data, size_t len,
                                        const char *message, cai_error *error) {
  lonejson_error json_error;

  lonejson_error_init(&json_error);
  if (spool == NULL ||
      spool->append(spool, data, len, &json_error) != LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT, message,
                                json_error.message);
  }
  return CAI_OK;
}

static int cai_mcp_dispatch_streamed_notification(
    cai_mcp_streamable_http_client_impl *impl,
    const cai_buffer_builder *method_json,
    const cai_buffer_builder *params_json, cai_error *error) {
  lonejson_spooled data;
  lonejson_spooled final_body;
  int final_seen;
  int rc;

  if (method_json == NULL || method_json->data == NULL ||
      method_json->length == 0U) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP JSON-RPC notification method is required");
  }
  memset(&data, 0, sizeof(data));
  memset(&final_body, 0, sizeof(final_body));
  CAI_LJ->spooled_init(CAI_LJ, &data);
  final_seen = 0;
  rc = cai_mcp_write_cstr(&data, "{\"jsonrpc\":\"2.0\",\"method\":", error);
  if (rc == CAI_OK) {
    rc = cai_mcp_spooled_append_bytes(
        &data, method_json->data, method_json->length,
        "failed to build MCP notification", error);
  }
  if (rc == CAI_OK && params_json != NULL && params_json->data != NULL &&
      params_json->length != 0U) {
    rc = cai_mcp_write_cstr(&data, ",\"params\":", error);
    if (rc == CAI_OK) {
      rc = cai_mcp_spooled_append_bytes(
          &data, params_json->data, params_json->length,
          "failed to build MCP notification", error);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(&data, "}", error);
  }
  if (rc == CAI_OK) {
    CAI_LJ->spooled_init(CAI_LJ, &final_body);
    rc = cai_mcp_process_sse_message(impl, &data, &final_body, &final_seen,
                                     error);
    cai_mcp_spooled_cleanup_if_initialized(&final_body);
  }
  cai_mcp_spooled_cleanup_if_initialized(&data);
  return rc;
}

static int cai_mcp_handle_streamed_server_request(
    cai_mcp_streamable_http_client_impl *impl,
    const cai_buffer_builder *id_json, const cai_buffer_builder *method_json,
    const cai_buffer_builder *params_json, cai_error *error) {
  cai_mcp_jsonrpc_message_doc doc;
  lonejson_spooled data;
  int rc;

  if (id_json == NULL || id_json->data == NULL || id_json->length == 0U ||
      method_json == NULL || method_json->data == NULL ||
      method_json->length == 0U) {
    return cai_set_error(
        error, CAI_ERR_PROTOCOL,
        "MCP JSON-RPC server request id and method are required");
  }
  memset(&data, 0, sizeof(data));
  CAI_LJ->spooled_init(CAI_LJ, &data);
  rc = cai_mcp_write_cstr(&data, "{\"jsonrpc\":\"2.0\",\"id\":", error);
  if (rc == CAI_OK) {
    rc = cai_mcp_spooled_append_bytes(&data, id_json->data, id_json->length,
                                      "failed to build MCP server request",
                                      error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(&data, ",\"method\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_spooled_append_bytes(
        &data, method_json->data, method_json->length,
        "failed to build MCP server request", error);
  }
  if (rc == CAI_OK && params_json != NULL && params_json->data != NULL &&
      params_json->length != 0U) {
    rc = cai_mcp_write_cstr(&data, ",\"params\":", error);
    if (rc == CAI_OK) {
      rc = cai_mcp_spooled_append_bytes(
          &data, params_json->data, params_json->length,
          "failed to build MCP server request", error);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_write_cstr(&data, "}", error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_parse_sse_message(&data, &doc, error);
    if (rc == CAI_OK) {
      rc = cai_mcp_handle_server_request(impl, &doc, error);
      CAI_LJ->cleanup(CAI_LJ, &cai_mcp_jsonrpc_message_map, &doc);
    }
  }
  cai_mcp_spooled_cleanup_if_initialized(&data);
  return rc;
}

static int
cai_mcp_streamed_id_matches_request(const cai_buffer_builder *id_json,
                                    long long request_id, cai_error *error) {
  char expected_id[32];
  size_t expected_len;

  if (id_json == NULL || id_json->data == NULL || id_json->length == 0U) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP JSON-RPC was missing id");
  }
  snprintf(expected_id, sizeof(expected_id), "%lld", request_id);
  expected_len = strlen(expected_id);
  if (id_json->length != expected_len ||
      memcmp(id_json->data, expected_id, expected_len) != 0) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP JSON-RPC response id did not match request id");
  }
  return CAI_OK;
}

static int
cai_mcp_streamed_jsonrpc_is_valid(const cai_buffer_builder *jsonrpc_json,
                                  cai_error *error) {
  if (jsonrpc_json == NULL || jsonrpc_json->data == NULL ||
      jsonrpc_json->length != 5U ||
      memcmp(jsonrpc_json->data, "\"2.0\"", 5U) != 0) {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "MCP JSON-RPC version must be 2.0");
  }
  return CAI_OK;
}

static int cai_mcp_parse_streamed_result_response(
    cai_mcp_streamable_http_client_impl *impl,
    lonejson_read_result (*read_fn)(void *, unsigned char *, size_t),
    void *read_user, long long request_id, int require_result_object,
    cai_sink *output, int *result_seen, cai_error *error) {
  cai_mcp_stream_char_reader reader;
  cai_mcp_jsonrpc_error_doc error_doc;
  cai_buffer_builder jsonrpc_json;
  cai_buffer_builder id_json;
  cai_buffer_builder error_json;
  cai_buffer_builder method_json;
  cai_buffer_builder params_json;
  cai_buffer_builder result_json;
  lonejson_error json_error;
  lonejson_status json_status;
  char key[64];
  int has_jsonrpc;
  int has_id;
  int has_result;
  int has_error;
  int has_method;
  int has_params;
  int result_streamed;
  int closed;
  int ch;
  int rc;

  if (read_fn == NULL || output == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP streamed response reader and sink are required");
  }
  memset(&reader, 0, sizeof(reader));
  memset(&jsonrpc_json, 0, sizeof(jsonrpc_json));
  memset(&id_json, 0, sizeof(id_json));
  memset(&error_json, 0, sizeof(error_json));
  memset(&method_json, 0, sizeof(method_json));
  memset(&params_json, 0, sizeof(params_json));
  memset(&result_json, 0, sizeof(result_json));
  reader.read_fn = read_fn;
  reader.read_user = read_user;
  has_jsonrpc = 0;
  has_id = 0;
  has_result = 0;
  has_error = 0;
  has_method = 0;
  has_params = 0;
  result_streamed = 0;
  closed = 0;
  rc = CAI_OK;
  rc = cai_mcp_stream_skip_ws(&reader, &ch, error);
  if (rc <= 0 || ch != '{') {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "failed to parse MCP JSON-RPC response");
  }
  for (;;) {
    rc = cai_mcp_stream_skip_ws(&reader, &ch, error);
    if (rc <= 0) {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                         "failed to parse MCP JSON-RPC response");
      break;
    }
    if (ch == '}') {
      rc = CAI_OK;
      closed = 1;
      break;
    }
    if (ch != '"') {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                         "failed to parse MCP JSON-RPC response");
      break;
    }
    rc = cai_mcp_stream_read_string_key(&reader, key, sizeof(key), error);
    if (rc != CAI_OK) {
      break;
    }
    rc = cai_mcp_stream_skip_ws(&reader, &ch, error);
    if (rc <= 0 || ch != ':') {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                         "failed to parse MCP JSON-RPC response");
      break;
    }
    if (strcmp(key, "jsonrpc") == 0) {
      rc = cai_mcp_stream_value(&reader, NULL, &jsonrpc_json, NULL, error);
      has_jsonrpc = rc == CAI_OK;
    } else if (strcmp(key, "id") == 0) {
      rc = cai_mcp_stream_value(&reader, NULL, &id_json, NULL, error);
      has_id = rc == CAI_OK;
    } else if (strcmp(key, "method") == 0) {
      if (has_result || has_error) {
        rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP JSON-RPC response must not include method");
        break;
      }
      rc = cai_mcp_stream_value(&reader, NULL, &method_json, NULL, error);
      has_method = rc == CAI_OK;
    } else if (strcmp(key, "params") == 0) {
      rc = cai_mcp_stream_value(&reader, NULL, &params_json, NULL, error);
      has_params = rc == CAI_OK;
    } else if (strcmp(key, "result") == 0) {
      size_t bytes_written;

      bytes_written = 0U;
      if (has_result || has_error) {
        rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP JSON-RPC response must include exactly one of "
                           "result or error");
        break;
      }
      if (has_method) {
        rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP JSON-RPC response must not include method");
        break;
      }
      if (has_jsonrpc) {
        rc = cai_mcp_streamed_jsonrpc_is_valid(&jsonrpc_json, error);
        if (rc != CAI_OK) {
          break;
        }
      }
      if (has_id && has_jsonrpc) {
        rc = cai_mcp_streamed_id_matches_request(&id_json, request_id, error);
        if (rc != CAI_OK) {
          break;
        }
      }
      rc = cai_mcp_stream_skip_ws(&reader, &ch, error);
      if (rc <= 0) {
        rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                           "failed to parse MCP JSON-RPC response");
        break;
      }
      if (require_result_object && ch != '{') {
        rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP result must be an object");
        break;
      }
      cai_mcp_stream_unread_char(&reader, ch);
      if (has_id && has_jsonrpc) {
        rc = cai_mcp_stream_value(&reader, output, NULL, &bytes_written, error);
        if (result_seen != NULL && bytes_written > 0U) {
          *result_seen = 1;
        }
        result_streamed = rc == CAI_OK;
        has_result = rc == CAI_OK && bytes_written > 0U;
      } else {
        rc = cai_mcp_stream_value(&reader, NULL, &result_json, &bytes_written,
                                  error);
        (void)bytes_written;
        has_result = rc == CAI_OK && result_json.length > 0U;
      }
    } else if (strcmp(key, "error") == 0) {
      if (has_result) {
        rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP JSON-RPC response must include exactly one of "
                           "result or error");
        break;
      }
      rc = cai_mcp_stream_value(&reader, NULL, &error_json, NULL, error);
      has_error = rc == CAI_OK;
    } else {
      rc = cai_mcp_stream_value(&reader, NULL, NULL, NULL, error);
    }
    if (rc != CAI_OK) {
      break;
    }
    rc = cai_mcp_stream_skip_ws(&reader, &ch, error);
    if (rc <= 0) {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                         "failed to parse MCP JSON-RPC response");
      break;
    }
    if (ch == '}') {
      rc = CAI_OK;
      closed = 1;
      break;
    }
    if (ch != ',') {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                         "failed to parse MCP JSON-RPC response");
      break;
    }
  }
  if (rc == CAI_OK && has_result == has_error) {
    if (has_method && !has_result && !has_error) {
      if (has_id) {
        rc = cai_mcp_handle_streamed_server_request(
            impl, &id_json, &method_json, has_params ? &params_json : NULL,
            error);
      } else {
        rc = cai_mcp_dispatch_streamed_notification(
            impl, &method_json, has_params ? &params_json : NULL, error);
      }
      cai_free_mem(NULL, jsonrpc_json.data);
      cai_free_mem(NULL, id_json.data);
      cai_free_mem(NULL, error_json.data);
      cai_free_mem(NULL, method_json.data);
      cai_free_mem(NULL, params_json.data);
      cai_free_mem(NULL, result_json.data);
      if (rc != CAI_OK) {
        return rc;
      }
      return cai_mcp_parse_streamed_result_response(
          impl, read_fn, read_user, request_id, require_result_object, output,
          result_seen, error);
    }
    rc = cai_set_error(
        error, CAI_ERR_PROTOCOL,
        "MCP JSON-RPC response must include exactly one of result or error");
  }
  if (rc == CAI_OK && has_jsonrpc) {
    rc = cai_mcp_streamed_jsonrpc_is_valid(&jsonrpc_json, error);
  }
  if (rc == CAI_OK && has_error) {
    memset(&error_doc, 0, sizeof(error_doc));
    rc = cai_buffer_append(&error_json, "", 1U, error);
    if (rc == CAI_OK) {
      lonejson_error_init(&json_error);
      json_status =
          CAI_LJ->parse_cstr(CAI_LJ, &cai_mcp_jsonrpc_error_map, &error_doc,
                             error_json.data, &json_error);
      if (json_status != LONEJSON_STATUS_OK) {
        rc = cai_mcp_set_json_error(error, "failed to parse MCP error",
                                    &json_error);
      } else {
        rc = cai_mcp_set_rpc_error(error, &error_doc);
      }
    }
    CAI_LJ->cleanup(CAI_LJ, &cai_mcp_jsonrpc_error_map, &error_doc);
  }
  if (rc == CAI_OK && !has_result) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "MCP JSON-RPC response was missing result");
  }
  if (rc == CAI_OK && !has_jsonrpc) {
    rc = cai_mcp_streamed_jsonrpc_is_valid(&jsonrpc_json, error);
  }
  if (rc == CAI_OK && has_method) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "MCP JSON-RPC response must not include method");
  }
  if (rc == CAI_OK && !has_id) {
    rc = cai_set_error(error, CAI_ERR_PROTOCOL, "MCP JSON-RPC was missing id");
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_streamed_id_matches_request(&id_json, request_id, error);
  }
  if (rc == CAI_OK && closed) {
    rc = cai_mcp_stream_skip_ws(&reader, &ch, error);
    if (rc > 0) {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                         "failed to parse MCP JSON-RPC response");
    } else if (rc == 0) {
      rc = CAI_OK;
    }
  }
  if (rc == CAI_OK && !result_streamed && output != NULL &&
      result_json.data != NULL && result_json.length != 0U) {
    rc = cai_sink_write(output, result_json.data, result_json.length, error);
  }
  cai_free_mem(NULL, jsonrpc_json.data);
  cai_free_mem(NULL, id_json.data);
  cai_free_mem(NULL, error_json.data);
  cai_free_mem(NULL, method_json.data);
  cai_free_mem(NULL, params_json.data);
  cai_free_mem(NULL, result_json.data);
  return rc;
}

static int
cai_mcp_parse_result_response(const cai_mcp_http_response_capture *response,
                              const char *response_name, const char *parse_name,
                              int require_result_object, cai_sink *output,
                              cai_error *error) {
  cai_mcp_jsonrpc_sink_response_doc doc;
  cai_mcp_result_sink_context sink_context;
  lonejson_spooled json_body;
  cai_mcp_spooled_reader reader;
  lonejson_error json_error;
  lonejson_status status;
  int has_error;
  int rc;

  rc = cai_mcp_response_reader_init(response, response_name,
                                    require_result_object, &json_body, &reader,
                                    &has_error, error);
  if (rc != CAI_OK) {
    return rc;
  }
  memset(&doc, 0, sizeof(doc));
  memset(&sink_context, 0, sizeof(sink_context));
  sink_context.sink = output;
  sink_context.error = error;
  sink_context.rc = CAI_OK;
  lonejson_error_init(&json_error);
  CAI_LJ_PRESERVE->json_value_init(CAI_LJ_PRESERVE, &doc.result);
  if (doc.result.methods->set_parse_sink(&doc.result, cai_mcp_result_sink,
                                         &sink_context,
                                         &json_error) != LONEJSON_STATUS_OK) {
    CAI_LJ_PRESERVE->cleanup(CAI_LJ_PRESERVE,
                             &cai_mcp_jsonrpc_sink_response_map, &doc);
    json_body.cleanup(&json_body);
    return cai_mcp_set_json_error(error, "failed to prepare MCP result sink",
                                  &json_error);
  }
  status = CAI_LJ_PRESERVE->parse_reader(
      CAI_LJ_PRESERVE, &cai_mcp_jsonrpc_sink_response_map, &doc,
      cai_mcp_spooled_read, &reader, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    CAI_LJ_PRESERVE->cleanup(CAI_LJ_PRESERVE,
                             &cai_mcp_jsonrpc_sink_response_map, &doc);
    json_body.cleanup(&json_body);
    if (sink_context.rc != CAI_OK) {
      return sink_context.rc;
    }
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

  rc = cai_mcp_response_reader_init(response, response_name, 0, &json_body,
                                    &reader, &has_error, error);
  if (rc != CAI_OK) {
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

static int CAI_MCP_UNUSED_FN
cai_mcp_parse_call_response(const cai_mcp_http_response_capture *response,
                            cai_sink *output, cai_error *error) {
  int rc;

  rc = cai_mcp_validate_tool_call_response_shape(response, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_mcp_parse_result_response(response, "MCP tools/call response",
                                       "failed to parse MCP tools/call", 1,
                                       output, error);
}

static int CAI_MCP_UNUSED_FN cai_mcp_parse_resource_read_response(
    const cai_mcp_http_response_capture *response, cai_sink *output,
    cai_error *error) {
  int rc;

  rc = cai_mcp_validate_resource_read_response_shape(response, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_mcp_parse_result_response(response, "MCP resources/read response",
                                       "failed to parse MCP resources/read", 1,
                                       output, error);
}

static int CAI_MCP_UNUSED_FN
cai_mcp_parse_prompt_get_response(const cai_mcp_http_response_capture *response,
                                  cai_sink *output, cai_error *error) {
  int rc;

  rc = cai_mcp_validate_prompt_get_response_shape(response, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_mcp_parse_result_response(response, "MCP prompts/get response",
                                       "failed to parse MCP prompts/get", 1,
                                       output, error);
}

static int CAI_MCP_UNUSED_FN
cai_mcp_parse_completion_response(const cai_mcp_http_response_capture *response,
                                  cai_sink *output, cai_error *error) {
  int rc;

  rc = cai_mcp_validate_completion_response_shape(response, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_mcp_parse_result_response(
      response, "MCP completion/complete response",
      "failed to parse MCP completion/complete", 1, output, error);
}

static int cai_mcp_streamable_initialize(cai_mcp_client *client,
                                         cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_mcp_http_response_capture response;
  lonejson_spooled request;
  size_t request_len = 0U;
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
    if (rc != CAI_OK) {
      cai_mcp_streamable_reset_session(impl);
    }
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

static void cai_mcp_drain_fd(int fd) {
  unsigned char buffer[4096];
  ssize_t got;

  if (fd < 0) {
    return;
  }
  do {
    got = read(fd, buffer, sizeof(buffer));
  } while (got > 0 || (got < 0 && errno == EINTR));
}

static int cai_mcp_stream_request_result_once(
    cai_mcp_streamable_http_client_impl *impl, const lonejson_spooled *request,
    size_t request_len, int require_result_object, cai_sink *output,
    cai_mcp_http_response_capture *response, cai_error *error) {
  cai_mcp_streaming_post_context context;
  cai_mcp_pipe_reader reader;
  pthread_t thread;
  long long request_id;
  int fds[2];
  int thread_started;
  int result_seen;
  int resume_attempted;
  int rc;

  if (impl == NULL || request == NULL || response == NULL || output == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client request and output sink are required");
  }
  request_id = 0LL;
  rc = cai_mcp_request_id_i64(request, &request_id, error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (pipe(fds) != 0) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to create MCP streaming pipe");
  }
  memset(response, 0, sizeof(*response));
  memset(&context, 0, sizeof(context));
  context.impl = impl;
  context.request = request;
  context.request_len = request_len;
  context.response = response;
  context.write_fd = fds[1];
  context.is_request = 1;
  context.rc = CAI_OK;
  thread_started = 0;
  result_seen = 0;
  resume_attempted = 0;
  impl->active_request_id = request_id;
  impl->has_active_request = 1;
  impl->has_active_progress = 0;
  impl->active_progress = 0.0;
  if (pthread_create(&thread, NULL, cai_mcp_streaming_post_thread, &context) !=
      0) {
    close(fds[0]);
    close(fds[1]);
    impl->has_active_request = 0;
    impl->has_active_progress = 0;
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to start MCP streaming request");
  }
  thread_started = 1;
  reader.fd = fds[0];
  rc = cai_mcp_parse_streamed_result_response(impl, cai_mcp_pipe_read, &reader,
                                              request_id, require_result_object,
                                              output, &result_seen, error);
  if (rc != CAI_OK) {
    cai_mcp_drain_fd(fds[0]);
  }
  close(fds[0]);
  if (thread_started) {
    pthread_join(thread, NULL);
  }
  while (rc != CAI_OK && !result_seen && context.rc == CAI_OK &&
         response->status >= 200L && response->status < 300L &&
         cai_mcp_response_is_sse(response) &&
         context.next_last_event_id != NULL &&
         context.next_last_event_id[0] != '\0' && !resume_attempted) {
    char *resume_event_id;
    long retry_ms;
    int has_retry;

    resume_event_id = context.next_last_event_id;
    retry_ms = context.next_retry_ms;
    has_retry = context.has_next_retry;
    context.next_last_event_id = NULL;
    resume_attempted = 1;
    cai_mcp_clear_error(error);
    if (has_retry) {
      cai_mcp_sleep_ms(retry_ms);
    }
    cai_mcp_http_response_capture_cleanup(response);
    if (pipe(fds) != 0) {
      cai_free_mem(NULL, resume_event_id);
      rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to create MCP streaming pipe");
      break;
    }
    memset(&context, 0, sizeof(context));
    context.impl = impl;
    context.response = response;
    context.last_event_id = resume_event_id;
    context.write_fd = fds[1];
    context.is_request = 1;
    context.rc = CAI_OK;
    if (pthread_create(&thread, NULL, cai_mcp_streaming_post_thread,
                       &context) != 0) {
      close(fds[0]);
      close(fds[1]);
      cai_free_mem(NULL, resume_event_id);
      rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to start MCP streaming resume request");
      break;
    }
    reader.fd = fds[0];
    rc = cai_mcp_parse_streamed_result_response(
        impl, cai_mcp_pipe_read, &reader, request_id, require_result_object,
        output, &result_seen, error);
    if (rc != CAI_OK) {
      cai_mcp_drain_fd(fds[0]);
    }
    close(fds[0]);
    pthread_join(thread, NULL);
    cai_free_mem(NULL, resume_event_id);
  }
  impl->has_active_request = 0;
  impl->has_active_progress = 0;
  if (rc == CAI_OK && context.rc != CAI_OK) {
    cai_mcp_log_http_transport_error(
        impl, context.request != NULL ? "POST" : "GET",
        context.curl_rc != CURLE_OK ? curl_easy_strerror(context.curl_rc)
                                    : "streaming write failed");
    rc = cai_set_error_detail(
        error, CAI_ERR_TRANSPORT, "MCP HTTP request failed",
        context.curl_rc != CURLE_OK ? curl_easy_strerror(context.curl_rc)
                                    : "streaming write failed");
  }
  if (context.rc == CAI_OK && response->status != 0L &&
      (response->status < 200L || response->status >= 300L)) {
    rc = cai_mcp_response_ok(response, error);
  }
  if (rc == CAI_OK) {
    rc = cai_mcp_response_ok(response, error);
  }
  cai_free_mem(NULL, context.next_last_event_id);
  return rc;
}

static int cai_mcp_stream_request_result_with_session_recovery(
    cai_mcp_streamable_http_client_impl *impl, const lonejson_spooled *request,
    size_t request_len, int require_result_object, cai_sink *output,
    cai_mcp_http_response_capture *response, cai_error *error) {
  int had_session;
  int rc;

  had_session = impl != NULL && impl->initialized && impl->session_id != NULL;
  rc = cai_mcp_stream_request_result_once(impl, request, request_len,
                                          require_result_object, output,
                                          response, error);
  if (rc == CAI_OK || !had_session || response == NULL ||
      response->status != 404L) {
    return rc;
  }
  cai_mcp_http_response_capture_cleanup(response);
  cai_mcp_clear_error(error);
  cai_mcp_streamable_reset_session(impl);
  rc = cai_mcp_streamable_initialize(&impl->public_client, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_mcp_stream_request_result_once(impl, request, request_len,
                                            require_result_object, output,
                                            response, error);
}

static int cai_mcp_request_result_with_session_recovery(
    cai_mcp_streamable_http_client_impl *impl, const lonejson_spooled *request,
    size_t request_len, int require_result_object, cai_sink *output,
    cai_mcp_http_response_capture *response, cai_error *error) {
  if (impl == NULL || request == NULL || response == NULL || output == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "MCP client request and output sink are required");
  }
  return cai_mcp_stream_request_result_with_session_recovery(
      impl, request, request_len, require_result_object, output, response,
      error);
}

static int cai_mcp_streamable_ping(cai_mcp_client *client, cai_error *error) {
  cai_mcp_streamable_http_client_impl *impl;
  cai_mcp_http_response_capture response;
  lonejson_spooled request;
  size_t request_len = 0U;
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
  size_t request_len = 0U;
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
  rc = cai_mcp_call_request(impl, name, arguments_json, &request, &request_len,
                            error);
  if (rc == CAI_OK) {
    rc = cai_mcp_request_result_with_session_recovery(
        impl, &request, request_len, 1, output, &response, error);
  }
  cai_mcp_spooled_cleanup_if_initialized(&request);
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
  size_t request_len = 0U;
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
    rc = cai_mcp_request_result_with_session_recovery(
        impl, &request, request_len, 1, output, &response, error);
  }
  cai_mcp_spooled_cleanup_if_initialized(&request);
  cai_mcp_http_response_capture_cleanup(&response);
  return rc;
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
  size_t request_len = 0U;
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
    rc = cai_mcp_request_result_with_session_recovery(
        impl, &request, request_len, 1, output, &response, error);
  }
  cai_mcp_spooled_cleanup_if_initialized(&request);
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
  size_t request_len = 0U;
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
    rc = cai_mcp_request_result_with_session_recovery(
        impl, &request, request_len, 1, output, &response, error);
  }
  cai_mcp_spooled_cleanup_if_initialized(&request);
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
  size_t request_len = 0U;
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
    rc = cai_mcp_request_result_with_session_recovery(
        impl, &request, request_len, 0, output, &response, error);
  }
  cai_mcp_spooled_cleanup_if_initialized(&request);
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
  size_t notification_len = 0U;
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
  if (!cai_mcp_allocator_is_empty(&effective->allocator) &&
      !cai_mcp_allocator_is_complete(&effective->allocator)) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "custom allocator requires malloc, realloc, and free");
  }
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
  impl->public_client.impl = impl;
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
  impl->logger = effective->logger_disabled ? NULL : effective->logger;
  impl->logger_disabled = effective->logger_disabled;
  impl->receiver = effective->receiver;
  if (impl->receiver.notification == NULL && effective->notification != NULL) {
    impl->receiver.notification = effective->notification;
    impl->receiver.context = effective->notification_context;
    impl->receiver.cleanup = effective->notification_context_cleanup;
  }
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
  impl->public_client.refresh_resources = cai_mcp_streamable_refresh_resources;
  impl->public_client.resource_count = cai_mcp_streamable_resource_count;
  impl->public_client.resource_at = cai_mcp_streamable_resource_at;
  impl->public_client.read_resource = cai_mcp_streamable_read_resource;
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
  impl->public_client.send_request = cai_mcp_streamable_send_request;
  impl->public_client.send_notification = cai_mcp_streamable_send_notification;
  impl->public_client.destroy = cai_mcp_streamable_destroy;
  *out = &impl->public_client;
  cai_mcp_log_client_opened(impl);
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

static const char *
cai_mcp_streamable_tool_schema_json(cai_mcp_client *client,
                                    const cai_mcp_client_tool *tool) {
  cai_mcp_streamable_http_client_impl *impl;
  size_t i;

  if (client == NULL || tool == NULL || client->impl == NULL ||
      client->tool_at != cai_mcp_streamable_tool_at) {
    return NULL;
  }
  impl = (cai_mcp_streamable_http_client_impl *)client->impl;
  for (i = 0U; i < impl->tool_count; i++) {
    if (&impl->tools[i].public_tool == tool) {
      return impl->tools[i].input_schema_json;
    }
  }
  return NULL;
}

static int cai_mcp_append_json_string_literal(cai_buffer_builder *builder,
                                              const char *text,
                                              cai_error *error) {
  const unsigned char *p;
  char escape[7];
  int rc;

  rc = cai_buffer_append(builder, "\"", 1U, error);
  if (rc != CAI_OK) {
    return rc;
  }
  p = (const unsigned char *)(text != NULL ? text : "");
  while (*p != '\0') {
    switch (*p) {
    case '"':
      rc = cai_buffer_append(builder, "\\\"", 2U, error);
      break;
    case '\\':
      rc = cai_buffer_append(builder, "\\\\", 2U, error);
      break;
    case '\b':
      rc = cai_buffer_append(builder, "\\b", 2U, error);
      break;
    case '\f':
      rc = cai_buffer_append(builder, "\\f", 2U, error);
      break;
    case '\n':
      rc = cai_buffer_append(builder, "\\n", 2U, error);
      break;
    case '\r':
      rc = cai_buffer_append(builder, "\\r", 2U, error);
      break;
    case '\t':
      rc = cai_buffer_append(builder, "\\t", 2U, error);
      break;
    default:
      if (*p < 0x20U) {
        snprintf(escape, sizeof(escape), "\\u%04x", (unsigned int)*p);
        rc = cai_buffer_append(builder, escape, 6U, error);
      } else {
        rc = cai_buffer_append(builder, (const char *)p, 1U, error);
      }
      break;
    }
    if (rc != CAI_OK) {
      return rc;
    }
    p++;
  }
  return cai_buffer_append(builder, "\"", 1U, error);
}

static char *
cai_mcp_generate_registry_schema_json(const cai_mcp_client_schema *schema,
                                      cai_error *error) {
  cai_buffer_builder builder;
  size_t i;
  int rc;

  if (schema == NULL || schema->type == NULL ||
      strcmp(schema->type, "object") != 0) {
    cai_set_error(error, CAI_ERR_PROTOCOL,
                  "MCP tool input schema is incomplete");
    return NULL;
  }
  memset(&builder, 0, sizeof(builder));
  rc = cai_buffer_append_cstr(&builder, "{\"type\":\"object\",\"properties\":{",
                              error);
  for (i = 0U; rc == CAI_OK && i < schema->property_count; i++) {
    if (i != 0U) {
      rc = cai_buffer_append(&builder, ",", 1U, error);
    }
    if (rc == CAI_OK) {
      rc = cai_mcp_append_json_string_literal(&builder, schema->properties[i],
                                              error);
    }
    if (rc == CAI_OK) {
      rc = cai_buffer_append_cstr(&builder, ":{}", error);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_buffer_append_cstr(&builder, "},\"required\":[", error);
  }
  for (i = 0U; rc == CAI_OK && i < schema->required_count; i++) {
    if (i != 0U) {
      rc = cai_buffer_append(&builder, ",", 1U, error);
    }
    if (rc == CAI_OK) {
      rc = cai_mcp_append_json_string_literal(&builder, schema->required[i],
                                              error);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_buffer_append_cstr(&builder, "]}", error);
  }
  if (rc == CAI_OK) {
    rc = cai_buffer_append(&builder, "", 1U, error);
  }
  if (rc != CAI_OK) {
    cai_free_mem(NULL, builder.data);
    return NULL;
  }
  return builder.data;
}

int cai_mcp_client_register_tools(
    cai_mcp_client *client, cai_tool_registry *registry,
    const cai_mcp_tool_registration_config *config, cai_error *error) {
  cai_mcp_tool_registration_config defaults;
  const cai_mcp_tool_registration_config *effective;
  const cai_mcp_client_tool *tool;
  cai_mcp_registry_tool_context *context;
  cai_buffer_builder name_builder;
  const char *schema_json;
  char *generated_schema_json;
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
    if (tool == NULL || tool->name == NULL || tool->input_schema == NULL) {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "MCP tool metadata is incomplete");
    }
    schema_json = cai_mcp_streamable_tool_schema_json(client, tool);
    generated_schema_json = NULL;
    if (schema_json == NULL) {
      generated_schema_json =
          cai_mcp_generate_registry_schema_json(tool->input_schema, error);
      if (generated_schema_json == NULL) {
        return error != NULL && error->code != CAI_OK ? error->code
                                                      : CAI_ERR_NOMEM;
      }
      schema_json = generated_schema_json;
    }
    memset(&name_builder, 0, sizeof(name_builder));
    if (effective->name_prefix != NULL) {
      rc = cai_buffer_append_cstr(&name_builder, effective->name_prefix, error);
      if (rc != CAI_OK) {
        cai_free_mem(NULL, generated_schema_json);
        cai_free_mem(NULL, name_builder.data);
        return rc;
      }
    }
    rc = cai_buffer_append_cstr(&name_builder, tool->name, error);
    if (rc == CAI_OK) {
      rc = cai_buffer_append(&name_builder, "", 1U, error);
    }
    if (rc != CAI_OK) {
      cai_free_mem(NULL, generated_schema_json);
      cai_free_mem(NULL, name_builder.data);
      return rc;
    }
    context =
        (cai_mcp_registry_tool_context *)cai_alloc(NULL, sizeof(*context));
    if (context == NULL) {
      cai_free_mem(NULL, generated_schema_json);
      cai_free_mem(NULL, name_builder.data);
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate MCP tool context");
    }
    memset(context, 0, sizeof(*context));
    context->client = client;
    context->remote_name = cai_strdup(NULL, tool->name);
    if (context->remote_name == NULL) {
      cai_free_mem(NULL, context);
      cai_free_mem(NULL, generated_schema_json);
      cai_free_mem(NULL, name_builder.data);
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate MCP tool name");
    }
    rc = cai_tool_registry_register_raw_spooled_owned(
        registry, name_builder.data, tool->description, schema_json,
        effective->strict, cai_mcp_registered_tool_callback, context,
        cai_mcp_registered_tool_context_cleanup, error);
    cai_free_mem(NULL, name_builder.data);
    cai_free_mem(NULL, generated_schema_json);
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
