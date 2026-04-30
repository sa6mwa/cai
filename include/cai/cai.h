#ifndef CAI_CAI_H
#define CAI_CAI_H

#include <cai/models.h>
#include <cai/version.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

struct lc_source;
struct lc_sink;
struct lonejson_map;
struct lonejson_spooled;
struct pslog_logger;

typedef struct cai_client cai_client;
typedef struct cai_agent cai_agent;
typedef struct cai_session cai_session;
typedef struct cai_conversation cai_conversation;
typedef struct cai_source cai_source;
typedef struct cai_sink cai_sink;
typedef struct cai_output cai_output;
typedef struct cai_response_create_params cai_response_create_params;
typedef struct cai_response cai_response;
typedef struct cai_input_item_list cai_input_item_list;
typedef struct cai_conversation_item cai_conversation_item;
typedef struct cai_conversation_items_params cai_conversation_items_params;
typedef struct cai_tool_registry cai_tool_registry;
typedef struct cai_tool_schema cai_tool_schema;

typedef enum cai_status {
  CAI_OK = 0,
  CAI_ERR_INVALID = 1,
  CAI_ERR_NOMEM = 2,
  CAI_ERR_TRANSPORT = 3,
  CAI_ERR_PROTOCOL = 4,
  CAI_ERR_SERVER = 5,
  CAI_ERR_CANCELLED = 6
} cai_status;

typedef struct cai_error {
  int code;
  long http_status;
  char *message;
  char *detail;
  char *server_code;
  char *request_id;
} cai_error;

typedef void *(*cai_malloc_fn)(void *context, size_t size);
typedef void *(*cai_realloc_fn)(void *context, void *ptr, size_t size);
typedef void (*cai_free_fn)(void *context, void *ptr);

typedef struct cai_allocator {
  cai_malloc_fn malloc_fn;
  cai_realloc_fn realloc_fn;
  cai_free_fn free_fn;
  void *context;
} cai_allocator;

typedef struct cai_client_config {
  const char *api_key;
  const char *base_url;
  const char *organization_id;
  const char *project_id;
  long timeout_ms;
  int http_2_disabled;
  int insecure_skip_verify;
  size_t json_response_limit_bytes;
  struct pslog_logger *logger;
  int logger_disabled;
  cai_allocator allocator;
} cai_client_config;

typedef struct cai_agent_config {
  const char *model;
  const char *developer_instructions;
  const char *prompt_cache_key;
  const char *reasoning_effort;
  const char *reasoning_summary;
  const char *text_format_name;
  const char *text_format_description;
  const char *text_format_schema_json;
  int text_format_strict;
  int max_output_tokens;
  int parallel_tool_calls;
  int disable_auto_compaction;
  long long compact_threshold_tokens;
  unsigned int compact_threshold_percent;
  /* Deprecated compatibility fields; use disable_auto_compaction and
     compact_threshold_tokens for new code. */
  int auto_compact;
  long long auto_compact_token_limit;
  int enable_local_history;
  size_t history_memory_limit;
  const char *history_spool_dir;
} cai_agent_config;

#define CAI_REASONING_EFFORT_NONE "none"
#define CAI_REASONING_EFFORT_MINIMAL "minimal"
#define CAI_REASONING_EFFORT_LOW "low"
#define CAI_REASONING_EFFORT_MEDIUM "medium"
#define CAI_REASONING_EFFORT_HIGH "high"
#define CAI_REASONING_EFFORT_XHIGH "xhigh"

#define CAI_REASONING_SUMMARY_AUTO "auto"
#define CAI_REASONING_SUMMARY_CONCISE "concise"
#define CAI_REASONING_SUMMARY_DETAILED "detailed"

typedef struct cai_list_params {
  const char *after;
  int limit;
  const char *order;
} cai_list_params;

typedef struct cai_run_options {
  int max_tool_rounds;
  size_t tool_output_memory_limit;
  const char *tool_spool_dir;
} cai_run_options;

typedef struct cai_token_usage {
  long long input_tokens;
  long long input_cached_tokens;
  long long output_tokens;
  long long output_reasoning_tokens;
  long long total_tokens;
} cai_token_usage;

typedef size_t (*cai_source_read_fn)(void *context, void *buffer, size_t count,
                                     cai_error *error);
typedef int (*cai_source_reset_fn)(void *context, cai_error *error);
typedef void (*cai_source_close_fn)(void *context);

typedef struct cai_source_callbacks {
  cai_source_read_fn read;
  cai_source_reset_fn reset;
  cai_source_close_fn close;
  void *context;
} cai_source_callbacks;

typedef int (*cai_sink_write_fn)(void *context, const void *bytes, size_t count,
                                 cai_error *error);
typedef void (*cai_sink_close_fn)(void *context);

typedef struct cai_sink_callbacks {
  cai_sink_write_fn write;
  cai_sink_close_fn close;
  void *context;
} cai_sink_callbacks;

typedef const char *(*cai_stream_affix_fn)(void *context);

typedef struct cai_stream_affix {
  const char *text;
  cai_stream_affix_fn callback;
  void *context;
} cai_stream_affix;

typedef struct cai_stream_sinks {
  cai_sink *reasoning_summary;
  cai_sink *output_text;
  cai_stream_affix reasoning_summary_prefix;
  cai_stream_affix reasoning_summary_suffix;
  cai_stream_affix output_text_prefix;
  cai_stream_affix output_text_suffix;
} cai_stream_sinks;

typedef int (*cai_tool_fn)(void *context, const void *params, void *result,
                           cai_error *error);
typedef cai_tool_fn cai_tool_lonejson_fn;
typedef int (*cai_tool_raw_fn)(void *context, const char *arguments_json,
                               cai_sink *output, cai_error *error);

char *cai_tool_result_strdup(const char *value, cai_error *error);

struct cai_client {
  int (*new_agent)(cai_client *client, const cai_agent_config *config,
                   cai_agent **out, cai_error *error);
  int (*create_conversation)(cai_client *client, cai_conversation **out,
                             cai_error *error);
  void (*close)(cai_client *client);
  void *impl;
};

struct cai_agent {
  int (*register_tool)(cai_agent *agent, const char *name,
                       const char *description,
                       const struct lonejson_map *params_map,
                       const struct lonejson_map *result_map,
                       cai_tool_fn callback, void *context, cai_error *error);
  int (*register_raw_tool)(cai_agent *agent, const char *name,
                           const char *description, const char *schema_json,
                           int strict, cai_tool_raw_fn callback, void *context,
                           cai_error *error);
  int (*new_session)(cai_agent *agent, cai_session **out, cai_error *error);
  int (*new_conversation_session)(cai_agent *agent, cai_session **out,
                                  cai_error *error);
  int (*new_session_for_conversation)(cai_agent *agent,
                                      const cai_conversation *conversation,
                                      cai_session **out, cai_error *error);
  int (*add_user_text)(cai_agent *agent, const char *text, cai_error *error);
  int (*add_user_image_url)(cai_agent *agent, const char *url,
                            const char *detail, cai_error *error);
  int (*run)(cai_agent *agent, cai_response **out, cai_error *error);
  int (*run_output)(cai_agent *agent, cai_output **out, cai_error *error);
  int (*run_auto)(cai_agent *agent, const cai_run_options *options,
                  cai_response **out, cai_error *error);
  int (*run_auto_output)(cai_agent *agent, const cai_run_options *options,
                         cai_output **out, cai_error *error);
  int (*stream)(cai_agent *agent, const cai_stream_sinks *sinks,
                cai_error *error);
  int (*stream_text)(cai_agent *agent, cai_sink *sink, cai_error *error);
  int (*open_text_source)(cai_agent *agent, cai_source **out,
                          cai_error *error);
  int (*send_text)(cai_agent *agent, const char *text, cai_response **out,
                   cai_error *error);
  int (*last_usage)(const cai_agent *agent, cai_token_usage *out,
                    cai_error *error);
  int (*context_percent)(const cai_agent *agent, double *out,
                         cai_error *error);
  void (*close)(cai_agent *agent);
  void *impl;
};

struct cai_session {
  int (*set_conversation_id)(cai_session *session, const char *conversation_id,
                             cai_error *error);
  int (*set_conversation)(cai_session *session,
                          const cai_conversation *conversation,
                          cai_error *error);
  const char *(*conversation_id)(const cai_session *session);
  int (*set_previous_response_id)(cai_session *session,
                                  const char *response_id,
                                  cai_error *error);
  const char *(*previous_response_id)(const cai_session *session);
  int (*add_user_text)(cai_session *session, const char *text,
                       cai_error *error);
  int (*add_user_image_url)(cai_session *session, const char *url,
                            const char *detail, cai_error *error);
  int (*add_function_call_output)(cai_session *session, const char *call_id,
                                  const char *output, cai_error *error);
  int (*run)(cai_session *session, cai_response **out, cai_error *error);
  int (*run_output)(cai_session *session, cai_output **out, cai_error *error);
  int (*run_auto)(cai_session *session, const cai_run_options *options,
                  cai_response **out, cai_error *error);
  int (*run_auto_output)(cai_session *session,
                         const cai_run_options *options, cai_output **out,
                         cai_error *error);
  int (*stream)(cai_session *session, const cai_stream_sinks *sinks,
                cai_error *error);
  int (*stream_text)(cai_session *session, cai_sink *sink, cai_error *error);
  int (*open_text_source)(cai_session *session, cai_source **out,
                          cai_error *error);
  int (*send_text)(cai_session *session, const char *text, cai_response **out,
                   cai_error *error);
  int (*last_usage)(const cai_session *session, cai_token_usage *out,
                    cai_error *error);
  long long (*context_window_tokens)(const cai_session *session);
  long long (*auto_compact_token_limit)(const cai_session *session);
  int (*context_percent)(const cai_session *session, double *out,
                         cai_error *error);
  int (*history_spilled)(const cai_session *session);
  int (*export_history_source)(cai_session *session, cai_source **out,
                               cai_error *error);
  int (*import_history_source)(cai_session *session, cai_source *source,
                               cai_error *error);
  void (*close)(cai_session *session);
  void *impl;
};

struct cai_tool_schema {
  int (*set_strict)(cai_tool_schema *schema, int strict, cai_error *error);
  int (*string)(cai_tool_schema *schema, const char *name,
                const char *description, int required, cai_error *error);
  int (*integer)(cai_tool_schema *schema, const char *name,
                 const char *description, int required, cai_error *error);
  int (*number)(cai_tool_schema *schema, const char *name,
                const char *description, int required, cai_error *error);
  int (*boolean)(cai_tool_schema *schema, const char *name,
                 const char *description, int required, cai_error *error);
  int (*string_enum)(cai_tool_schema *schema, const char *name,
                     const char *description, const char *const *values,
                     size_t value_count, int required, cai_error *error);
  int (*describe)(cai_tool_schema *schema, const char *name,
                  const char *description, cai_error *error);
  int (*raw_property)(cai_tool_schema *schema, const char *name,
                      const char *description, const char *schema_json,
                      int required, cai_error *error);
  const char *(*json)(const cai_tool_schema *schema);
  int (*strict)(const cai_tool_schema *schema);
  void (*close)(cai_tool_schema *schema);
  void *impl;
};

void cai_client_config_init(cai_client_config *config);
int cai_client_open(const cai_client_config *config, cai_client **out,
                    cai_error *error);
void cai_client_close(cai_client *client);

void cai_agent_config_init(cai_agent_config *config);
int cai_client_new_agent(cai_client *client, const cai_agent_config *config,
                         cai_agent **out, cai_error *error);
void cai_agent_destroy(cai_agent *agent);
int cai_agent_register_tool(cai_agent *agent, const char *name,
                            const char *description,
                            const struct lonejson_map *params_map,
                            const struct lonejson_map *result_map,
                            cai_tool_fn callback, void *context,
                            cai_error *error);
int cai_agent_register_lonejson_tool(cai_agent *agent, const char *name,
                                     const char *description,
                                     const struct lonejson_map *params_map,
                                     const struct lonejson_map *result_map,
                                     cai_tool_fn callback, void *context,
                                     cai_error *error);
int cai_agent_register_raw_tool(cai_agent *agent, const char *name,
                                const char *description,
                                const char *schema_json, int strict,
                                cai_tool_raw_fn callback, void *context,
                                cai_error *error);
int cai_agent_new_session(cai_agent *agent, cai_session **out,
                          cai_error *error);
int cai_agent_new_conversation_session(cai_agent *agent, cai_session **out,
                                       cai_error *error);
int cai_agent_new_session_for_conversation(cai_agent *agent,
                                           const cai_conversation *conversation,
                                           cai_session **out, cai_error *error);
void cai_session_destroy(cai_session *session);
int cai_session_set_conversation_id(cai_session *session,
                                    const char *conversation_id,
                                    cai_error *error);
int cai_session_set_conversation(cai_session *session,
                                 const cai_conversation *conversation,
                                 cai_error *error);
const char *cai_session_conversation_id(const cai_session *session);
int cai_session_set_previous_response_id(cai_session *session,
                                         const char *response_id,
                                         cai_error *error);
const char *cai_session_previous_response_id(const cai_session *session);
int cai_session_add_user_text(cai_session *session, const char *text,
                              cai_error *error);
int cai_session_add_user_image_url(cai_session *session, const char *url,
                                   const char *detail, cai_error *error);
int cai_session_add_function_call_output(cai_session *session,
                                         const char *call_id,
                                         const char *output, cai_error *error);
int cai_session_run(cai_session *session, cai_response **out, cai_error *error);
int cai_session_run_output(cai_session *session, cai_output **out,
                           cai_error *error);
void cai_run_options_init(cai_run_options *options);
int cai_session_run_auto(cai_session *session, const cai_run_options *options,
                         cai_response **out, cai_error *error);
int cai_session_run_auto_output(cai_session *session,
                                const cai_run_options *options,
                                cai_output **out, cai_error *error);
int cai_session_stream(cai_session *session, const cai_stream_sinks *sinks,
                       cai_error *error);
int cai_session_stream_text(cai_session *session, cai_sink *sink,
                            cai_error *error);
int cai_session_open_text_source(cai_session *session, cai_source **out,
                                 cai_error *error);
int cai_session_compact_experimental(cai_session *session, cai_error *error);
int cai_session_send_text(cai_session *session, const char *text,
                          cai_response **out, cai_error *error);
int cai_session_last_usage(const cai_session *session, cai_token_usage *out,
                           cai_error *error);
long long cai_session_context_window_tokens(const cai_session *session);
long long cai_session_auto_compact_token_limit(const cai_session *session);
int cai_session_context_percent(const cai_session *session, double *out,
                                cai_error *error);
int cai_session_history_spilled(const cai_session *session);
int cai_session_export_history_source(cai_session *session, cai_source **out,
                                      cai_error *error);
int cai_session_import_history_source(cai_session *session, cai_source *source,
                                      cai_error *error);

void cai_error_init(cai_error *error);
void cai_error_cleanup(cai_error *error);
const char *cai_status_string(int status);

int cai_source_from_callbacks(const cai_source_callbacks *callbacks,
                              cai_source **out, cai_error *error);
int cai_source_from_lc(struct lc_source *source, cai_source **out,
                       cai_error *error);
size_t cai_source_read(cai_source *source, void *buffer, size_t count,
                       cai_error *error);
int cai_source_reset(cai_source *source, cai_error *error);
void cai_source_close(cai_source *source);
int cai_source_copy_to_sink(cai_source *source, cai_sink *sink,
                            cai_error *error);

int cai_sink_from_callbacks(const cai_sink_callbacks *callbacks, cai_sink **out,
                            cai_error *error);
int cai_sink_from_lc(struct lc_sink *sink, cai_sink **out, cai_error *error);
int cai_sink_file(FILE *fp, int close_file, cai_sink **out, cai_error *error);
int cai_sink_stdout(cai_sink **out, cai_error *error);
int cai_sink_stderr(cai_sink **out, cai_error *error);
int cai_sink_write(cai_sink *sink, const void *bytes, size_t count,
                   cai_error *error);
void cai_sink_close(cai_sink *sink);
void cai_stream_sinks_init(cai_stream_sinks *sinks);

int cai_output_as_lc_source(cai_output *output, struct lc_source **out,
                            cai_error *error);
const cai_response *cai_output_response(const cai_output *output);
const char *cai_output_text(const cai_output *output);
const char *cai_output_refusal(const cai_output *output);
const char *cai_output_raw_json(const cai_output *output);
int cai_output_write_text(const cai_output *output, cai_sink *sink,
                          cai_error *error);
int cai_output_write_refusal(const cai_output *output, cai_sink *sink,
                             cai_error *error);
int cai_output_write_raw_json(const cai_output *output, cai_sink *sink,
                              cai_error *error);
int cai_output_write_json(cai_output *output, const struct lonejson_map *map,
                          void *value, cai_error *error);
void cai_output_destroy(cai_output *output);

int cai_tool_registry_new(cai_tool_registry **out, cai_error *error);
void cai_tool_registry_destroy(cai_tool_registry *registry);
int cai_tool_registry_register_lonejson(
    cai_tool_registry *registry, const char *name, const char *description,
    const struct lonejson_map *params_map, const struct lonejson_map *result_map,
    cai_tool_fn callback, void *context, cai_error *error);
int cai_tool_registry_register_raw(cai_tool_registry *registry,
                                   const char *name, const char *description,
                                   const char *schema_json, int strict,
                                   cai_tool_raw_fn callback, void *context,
                                   cai_error *error);
int cai_tool_registry_add_to_response_params(const cai_tool_registry *registry,
                                             cai_response_create_params *params,
                                             cai_error *error);
int cai_tool_registry_run(cai_tool_registry *registry, const char *name,
                          const char *arguments_json, cai_sink *output,
                          cai_error *error);

int cai_tool_schema_new(cai_tool_schema **out, cai_error *error);
int cai_tool_schema_from_map(const struct lonejson_map *map,
                             cai_tool_schema **out, cai_error *error);
void cai_tool_schema_destroy(cai_tool_schema *schema);
int cai_tool_schema_set_strict(cai_tool_schema *schema, int strict,
                               cai_error *error);
int cai_tool_schema_add_string(cai_tool_schema *schema, const char *name,
                               const char *description, int required,
                               cai_error *error);
int cai_tool_schema_add_integer(cai_tool_schema *schema, const char *name,
                                const char *description, int required,
                                cai_error *error);
int cai_tool_schema_add_number(cai_tool_schema *schema, const char *name,
                               const char *description, int required,
                               cai_error *error);
int cai_tool_schema_add_boolean(cai_tool_schema *schema, const char *name,
                                const char *description, int required,
                                cai_error *error);
int cai_tool_schema_add_string_enum(cai_tool_schema *schema, const char *name,
                                    const char *description,
                                    const char *const *values,
                                    size_t value_count, int required,
                                    cai_error *error);
int cai_tool_schema_describe(cai_tool_schema *schema, const char *name,
                             const char *description, cai_error *error);
int cai_tool_schema_add_raw_property(cai_tool_schema *schema, const char *name,
                                     const char *description,
                                     const char *schema_json, int required,
                                     cai_error *error);
const char *cai_tool_schema_json(const cai_tool_schema *schema);
int cai_tool_schema_strict(const cai_tool_schema *schema);

int cai_response_create_params_new(cai_response_create_params **out,
                                   cai_error *error);
void cai_response_create_params_destroy(cai_response_create_params *params);
int cai_response_create_params_set_model(cai_response_create_params *params,
                                         const char *model, cai_error *error);
int cai_response_create_params_set_instructions(
    cai_response_create_params *params, const char *instructions,
    cai_error *error);
int cai_response_create_params_set_previous_response_id(
    cai_response_create_params *params, const char *response_id,
    cai_error *error);
int cai_response_create_params_set_conversation_id(
    cai_response_create_params *params, const char *conversation_id,
    cai_error *error);
int cai_response_create_params_set_prompt_cache_key(
    cai_response_create_params *params, const char *prompt_cache_key,
    cai_error *error);
int cai_response_create_params_set_max_output_tokens(
    cai_response_create_params *params, int max_output_tokens,
    cai_error *error);
int cai_response_create_params_set_reasoning(cai_response_create_params *params,
                                             const char *effort,
                                             const char *summary,
                                             cai_error *error);
int cai_response_create_params_set_parallel_tool_calls(
    cai_response_create_params *params, int enabled, cai_error *error);
int cai_response_create_params_set_compact_threshold(
    cai_response_create_params *params, long long compact_threshold_tokens,
    cai_error *error);
int cai_response_create_params_set_text_format_json_object(
    cai_response_create_params *params, cai_error *error);
int cai_response_create_params_set_text_format_json_schema(
    cai_response_create_params *params, const char *name,
    const char *description, const char *schema_json, int strict,
    cai_error *error);
int cai_response_create_params_add_text(cai_response_create_params *params,
                                        const char *role, const char *text,
                                        cai_error *error);
int cai_response_create_params_add_image_url(cai_response_create_params *params,
                                             const char *role, const char *url,
                                             const char *detail,
                                             cai_error *error);
int cai_response_create_params_add_image_file_id(
    cai_response_create_params *params, const char *role, const char *file_id,
    const char *detail, cai_error *error);
int cai_response_create_params_add_file_id(cai_response_create_params *params,
                                           const char *role, const char *file_id,
                                           const char *detail, cai_error *error);
int cai_response_create_params_add_file_url(cai_response_create_params *params,
                                            const char *role,
                                            const char *file_url,
                                            const char *detail,
                                            cai_error *error);
int cai_response_create_params_add_file_data_spooled(
    cai_response_create_params *params, const char *role, const char *filename,
    struct lonejson_spooled *file_data, const char *detail, cai_error *error);
int cai_response_create_params_add_function_tool(
    cai_response_create_params *params, const char *name,
    const char *description, const char *parameters_json, int strict,
    cai_error *error);
int cai_response_create_params_add_function_call_output(
    cai_response_create_params *params, const char *call_id, const char *output,
    cai_error *error);
int cai_response_create_params_add_function_call_output_text(
    cai_response_create_params *params, const char *call_id, const char *text,
    cai_error *error);
int cai_response_create_params_add_function_call_output_image_url(
    cai_response_create_params *params, const char *call_id, const char *url,
    const char *detail, cai_error *error);
int cai_response_create_params_add_function_call_output_file_id(
    cai_response_create_params *params, const char *call_id, const char *file_id,
    const char *detail, cai_error *error);
int cai_response_create_params_add_function_call_output_file_data_spooled(
    cai_response_create_params *params, const char *call_id,
    const char *filename, struct lonejson_spooled *file_data,
    const char *detail, cai_error *error);
int cai_client_create_response(cai_client *client,
                               const cai_response_create_params *params,
                               cai_response **out, cai_error *error);
int cai_client_stream_response_text(cai_client *client,
                                    const cai_response_create_params *params,
                                    cai_sink *sink, cai_error *error);
int cai_client_open_response_text_source(
    cai_client *client, const cai_response_create_params *params,
    cai_source **out, cai_error *error);
int cai_client_retrieve_response(cai_client *client, const char *response_id,
                                 cai_response **out, cai_error *error);
int cai_client_cancel_response(cai_client *client, const char *response_id,
                               cai_response **out, cai_error *error);
int cai_client_delete_response(cai_client *client, const char *response_id,
                               cai_error *error);
void cai_list_params_init(cai_list_params *params);
int cai_client_list_response_input_items(cai_client *client,
                                         const char *response_id,
                                         const cai_list_params *params,
                                         cai_input_item_list **out,
                                         cai_error *error);

const char *cai_response_id(const cai_response *response);
const char *cai_response_status(const cai_response *response);
const char *cai_response_model(const cai_response *response);
const char *cai_response_conversation_id(const cai_response *response);
long long cai_response_created_at(const cai_response *response);
const char *cai_response_output_text(const cai_response *response);
const char *cai_response_refusal(const cai_response *response);
int cai_response_write_output_text(const cai_response *response, cai_sink *sink,
                                   cai_error *error);
int cai_response_write_refusal(const cai_response *response, cai_sink *sink,
                               cai_error *error);
const char *cai_response_raw_json(const cai_response *response);
const char *cai_response_error_code(const cai_response *response);
const char *cai_response_error_message(const cai_response *response);
const char *cai_response_incomplete_reason(const cai_response *response);
long long cai_response_input_tokens(const cai_response *response);
long long cai_response_input_cached_tokens(const cai_response *response);
long long cai_response_output_tokens(const cai_response *response);
long long cai_response_output_reasoning_tokens(const cai_response *response);
long long cai_response_total_tokens(const cai_response *response);
int cai_response_usage(const cai_response *response, cai_token_usage *out,
                       cai_error *error);
size_t cai_response_tool_call_count(const cai_response *response);
const char *cai_response_tool_call_id(const cai_response *response,
                                      size_t index);
const char *cai_response_tool_call_name(const cai_response *response,
                                        size_t index);
const char *cai_response_tool_call_arguments(const cai_response *response,
                                             size_t index);
size_t cai_response_output_item_count(const cai_response *response);
const char *cai_response_output_item_id(const cai_response *response,
                                        size_t index);
const char *cai_response_output_item_type(const cai_response *response,
                                          size_t index);
const char *cai_response_output_item_status(const cai_response *response,
                                            size_t index);
const char *cai_response_output_item_role(const cai_response *response,
                                          size_t index);
const char *cai_response_output_item_call_id(const cai_response *response,
                                             size_t index);
const char *cai_response_output_item_name(const cai_response *response,
                                          size_t index);
void cai_response_destroy(cai_response *response);
size_t cai_input_item_list_count(const cai_input_item_list *list);
int cai_input_item_list_has_more(const cai_input_item_list *list);
const char *cai_input_item_list_first_id(const cai_input_item_list *list);
const char *cai_input_item_list_last_id(const cai_input_item_list *list);
const char *cai_input_item_list_raw_json(const cai_input_item_list *list);
const char *cai_input_item_id(const cai_input_item_list *list, size_t index);
const char *cai_input_item_type(const cai_input_item_list *list, size_t index);
const char *cai_input_item_role(const cai_input_item_list *list, size_t index);
void cai_input_item_list_destroy(cai_input_item_list *list);

int cai_client_create_conversation(cai_client *client, cai_conversation **out,
                                   cai_error *error);
int cai_conversation_from_id(const char *conversation_id,
                             cai_conversation **out, cai_error *error);
int cai_client_retrieve_conversation(cai_client *client,
                                     const char *conversation_id,
                                     cai_conversation **out, cai_error *error);
int cai_client_retrieve_conversation_handle(
    cai_client *client, const cai_conversation *conversation,
    cai_conversation **out, cai_error *error);
int cai_client_update_conversation_metadata(cai_client *client,
                                            const char *conversation_id,
                                            const char *metadata_json,
                                            cai_conversation **out,
                                            cai_error *error);
int cai_client_update_conversation_metadata_handle(
    cai_client *client, const cai_conversation *conversation,
    const char *metadata_json, cai_conversation **out, cai_error *error);
int cai_client_delete_conversation(cai_client *client,
                                   const char *conversation_id,
                                   cai_error *error);
int cai_client_delete_conversation_handle(cai_client *client,
                                          const cai_conversation *conversation,
                                          cai_error *error);
int cai_client_list_conversation_items(cai_client *client,
                                       const char *conversation_id,
                                       const cai_list_params *params,
                                       cai_input_item_list **out,
                                       cai_error *error);
int cai_client_list_conversation_items_handle(
    cai_client *client, const cai_conversation *conversation,
    const cai_list_params *params, cai_input_item_list **out, cai_error *error);
int cai_client_delete_conversation_item(cai_client *client,
                                        const char *conversation_id,
                                        const char *item_id, cai_error *error);
int cai_client_delete_conversation_item_handle(
    cai_client *client, const cai_conversation *conversation,
    const char *item_id, cai_error *error);
int cai_client_retrieve_conversation_item(cai_client *client,
                                          const char *conversation_id,
                                          const char *item_id,
                                          cai_conversation_item **out,
                                          cai_error *error);
int cai_client_retrieve_conversation_item_handle(
    cai_client *client, const cai_conversation *conversation,
    const char *item_id, cai_conversation_item **out, cai_error *error);
int cai_conversation_items_params_new(cai_conversation_items_params **out,
                                      cai_error *error);
void cai_conversation_items_params_destroy(
    cai_conversation_items_params *params);
int cai_conversation_items_params_add_text(
    cai_conversation_items_params *params, const char *role, const char *text,
    cai_error *error);
int cai_conversation_items_params_add_image_url(
    cai_conversation_items_params *params, const char *role, const char *url,
    const char *detail, cai_error *error);
int cai_conversation_items_params_add_image_file_id(
    cai_conversation_items_params *params, const char *role,
    const char *file_id, const char *detail, cai_error *error);
int cai_conversation_items_params_add_file_id(
    cai_conversation_items_params *params, const char *role,
    const char *file_id, const char *detail, cai_error *error);
int cai_conversation_items_params_add_file_url(
    cai_conversation_items_params *params, const char *role,
    const char *file_url, const char *detail, cai_error *error);
int cai_conversation_items_params_add_file_data_spooled(
    cai_conversation_items_params *params, const char *role,
    const char *filename, struct lonejson_spooled *file_data,
    const char *detail, cai_error *error);
int cai_client_create_conversation_items(
    cai_client *client, const char *conversation_id,
    const cai_conversation_items_params *params, cai_input_item_list **out,
    cai_error *error);
int cai_client_create_conversation_items_handle(
    cai_client *client, const cai_conversation *conversation,
    const cai_conversation_items_params *params, cai_input_item_list **out,
    cai_error *error);
const char *cai_conversation_id(const cai_conversation *conversation);
const char *cai_conversation_object(const cai_conversation *conversation);
void cai_conversation_destroy(cai_conversation *conversation);
const char *cai_conversation_item_id(const cai_conversation_item *item);
const char *cai_conversation_item_type(const cai_conversation_item *item);
const char *cai_conversation_item_role(const cai_conversation_item *item);
const char *cai_conversation_item_raw_json(const cai_conversation_item *item);
void cai_conversation_item_destroy(cai_conversation_item *item);

#ifdef __cplusplus
}
#endif

#endif
