#include "cai_internal.h"

#include <pslog.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct cai_tool_output_capture {
  lonejson_spooled output;
} cai_tool_output_capture;

typedef struct cai_history_sink_context {
  lonejson_spooled *spool;
} cai_history_sink_context;

typedef struct cai_lonejson_cai_sink_context {
  cai_sink *sink;
  cai_error *error;
} cai_lonejson_cai_sink_context;

typedef struct cai_stream_tool_call_list {
  cai_response_tool_call *items;
  size_t count;
  size_t capacity;
} cai_stream_tool_call_list;

typedef struct cai_stream_tool_capture {
  cai_session *session;
  const cai_stream_sinks *user_sinks;
  cai_stream_tool_call_list *tool_calls;
  lonejson_spooled *output_text;
  lonejson_spooled output_items;
  int output_items_initialized;
  size_t output_items_count;
} cai_stream_tool_capture;

typedef struct cai_spooled_record_reader {
  lonejson_spooled cursor;
  unsigned char buffer[4096];
  size_t offset;
  size_t length;
  int eof;
} cai_spooled_record_reader;

typedef struct cai_history_record_json_reader {
  cai_spooled_record_reader *records;
  unsigned long remaining;
  cai_error *error;
} cai_history_record_json_reader;

typedef struct cai_spooled_reader_context {
  lonejson_spooled cursor;
} cai_spooled_reader_context;

typedef struct cai_source_reader_context {
  cai_source *source;
  cai_error *error;
  int failed;
} cai_source_reader_context;

typedef struct cai_json_root_array_check {
  int depth;
  int root_seen;
  int root_is_array;
} cai_json_root_array_check;

typedef struct cai_session_state_doc {
  long long version;
  char *model;
  char *previous_response_id;
  char *conversation_id;
  lonejson_json_value history;
} cai_session_state_doc;

static const lonejson_field cai_session_state_fields[] = {
    LONEJSON_FIELD_I64_REQ(cai_session_state_doc, version, "version"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_session_state_doc, model,
                                          "model"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(
        cai_session_state_doc, previous_response_id, "previous_response_id"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_session_state_doc,
                                          conversation_id, "conversation_id"),
    LONEJSON_FIELD_JSON_VALUE_OMIT_NULL(cai_session_state_doc, history,
                                        "history")};
LONEJSON_MAP_DEFINE(cai_session_state_map, cai_session_state_doc,
                    cai_session_state_fields);

enum {
  CAI_SESSION_INPUT_TEXT = 0,
  CAI_SESSION_INPUT_IMAGE = 1,
  CAI_SESSION_INPUT_FUNCTION_CALL_OUTPUT = 2,
  CAI_SESSION_INPUT_FILE_DATA = 3
};

static int cai_history_append_array_record_spooled(cai_session *session,
                                                   const lonejson_spooled *json,
                                                   cai_error *error);
static int cai_history_append_array_record_to_spool(
    cai_session *session, lonejson_spooled *history,
    const lonejson_spooled *json, cai_error *error);
static lonejson *cai_agent_history_runtime(cai_session *session);
static void cai_history_init_spooled(cai_session *session,
                                     lonejson_spooled *spool);
static void cai_history_replace(cai_session *session,
                                lonejson_spooled *next_history);
static lonejson_read_result
cai_history_spooled_reader(void *user, unsigned char *buffer, size_t capacity);
static void cai_history_cleanup(cai_session *session);
static int cai_token_usage_is_empty(const cai_token_usage *usage);
static int cai_session_prepare_history_params(
    cai_session *session, cai_response_create_params *params,
    lonejson_spooled *out_pending_items, int *out_has_pending_items,
    cai_error *error);
static int cai_session_after_response(cai_session *session,
                                      const lonejson_spooled *pending_items,
                                      int has_pending_items,
                                      const cai_response *response,
                                      cai_error *error);
static int cai_session_after_stream(cai_session *session,
                                    const lonejson_spooled *pending_items,
                                    int has_pending_items,
                                    const char *response_id,
                                    const cai_token_usage *usage,
                                    cai_error *error);
static int cai_session_init_response_params(cai_session *session,
                                            cai_response_create_params **out,
                                            cai_error *error);
static int cai_session_remember_response_id(cai_session *session,
                                            const char *response_id,
                                            cai_error *error);
static int cai_session_set_last_usage(cai_session *session,
                                      const cai_token_usage *usage,
                                      cai_error *error);
static int cai_session_record_usage(cai_session *session,
                                    const cai_token_usage *usage,
                                    cai_error *error);
static int cai_session_check_usage_available(cai_session *session,
                                             cai_error *error);
static int cai_session_usage_limits_enabled(cai_session *session);
static int cai_session_remember_stream(cai_session *session,
                                       const char *response_id,
                                       const cai_token_usage *usage,
                                       cai_error *error);
static int cai_agent_add_user_text(cai_agent *agent, const char *text,
                                   cai_error *error);
static int cai_agent_add_user_text_spooled(cai_agent *agent,
                                           lonejson_spooled *text,
                                           cai_error *error);
static int cai_agent_add_user_text_source(cai_agent *agent, cai_source *source,
                                          cai_error *error);
static int cai_agent_add_user_image_url(cai_agent *agent, const char *url,
                                        const char *detail, cai_error *error);
static int cai_agent_add_user_file_data_spooled(cai_agent *agent,
                                                const char *filename,
                                                lonejson_spooled *file_data,
                                                const char *detail,
                                                cai_error *error);
static int cai_agent_add_user_file_source(cai_agent *agent,
                                          const char *filename,
                                          cai_source *source,
                                          const char *detail, cai_error *error);
static int cai_agent_add_user_file_path(cai_agent *agent, const char *path,
                                        const char *filename,
                                        const char *detail, cai_error *error);
static int cai_agent_run(cai_agent *agent, cai_response **out,
                         cai_error *error);
static int cai_agent_run_output(cai_agent *agent, cai_output **out,
                                cai_error *error);
static int cai_agent_run_auto(cai_agent *agent, const cai_run_options *options,
                              cai_response **out, cai_error *error);
static int cai_agent_run_auto_output(cai_agent *agent,
                                     const cai_run_options *options,
                                     cai_output **out, cai_error *error);
static int cai_agent_stream_auto(cai_agent *agent,
                                 const cai_run_options *options,
                                 const cai_stream_sinks *sinks,
                                 cai_error *error);
static int cai_agent_stream(cai_agent *agent, const cai_stream_sinks *sinks,
                            cai_error *error);
static int cai_agent_stream_text(cai_agent *agent, cai_sink *sink,
                                 cai_error *error);
static int cai_agent_open_text_source(cai_agent *agent, cai_source **out,
                                      cai_error *error);
static int cai_agent_send_text(cai_agent *agent, const char *text,
                               cai_response **out, cai_error *error);
static int cai_agent_last_usage(const cai_agent *agent, cai_token_usage *out,
                                cai_error *error);
int cai_agent_set_session_usage_limits(cai_agent *agent,
                                       const cai_usage_limits *limits,
                                       cai_error *error);
int cai_agent_usage(const cai_agent *agent, cai_usage_accounting *out,
                    cai_error *error);
static int cai_agent_context_percent(const cai_agent *agent, double *out,
                                     cai_error *error);
static void cai_agent_init_methods(cai_agent *agent);
static void cai_session_init_methods(cai_session *session);
static int cai_stream_tool_call_list_append(
    cai_stream_tool_call_list *list, const char *item_id, int output_index,
    const char *call_id, const char *name, const lonejson_spooled *arguments,
    cai_error *error);
static void cai_stream_tool_call_list_cleanup(cai_stream_tool_call_list *list);
static int cai_history_to_array_spool(cai_session *session,
                                      lonejson_spooled *out, cai_error *error);
static int cai_client_base_url_is_openrouter(const cai_client_impl *client);
static void
cai_agent_warn_openrouter_server_continuity(const cai_client_impl *client);

void cai_agent_config_init(cai_agent_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
}

static int cai_client_base_url_is_openrouter(const cai_client_impl *client) {
  return client != NULL && client->base_url != NULL &&
         strstr(client->base_url, "openrouter.ai") != NULL;
}

static void
cai_agent_warn_openrouter_server_continuity(const cai_client_impl *client) {
  if (!cai_client_base_url_is_openrouter(client)) {
    return;
  }
  cai_log_openrouter_server_continuity(client);
}

static int
cai_run_options_effective_max_tool_rounds(const cai_run_options *options) {
  if (options == NULL || options->disable_tool_auto_run) {
    return 0;
  }
  return options->max_tool_rounds > 0 ? options->max_tool_rounds : 4;
}

static size_t cai_run_options_effective_tool_output_memory_limit(
    const cai_run_options *options) {
  if (options == NULL || options->tool_output_memory_limit == 0U) {
    return 1024U * 1024U;
  }
  return options->tool_output_memory_limit;
}

static int cai_run_options_open_tool_output_runtime(
    const cai_run_options *options, lonejson **out_runtime, cai_error *error) {
  lonejson_config config;
  size_t memory_limit;
  size_t max_bytes;

  if (out_runtime == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "tool output runtime output pointer is required");
  }
  *out_runtime = NULL;
  memory_limit = cai_run_options_effective_tool_output_memory_limit(options);
  max_bytes = options != NULL ? options->tool_output_max_bytes : 0U;
  config = lonejson_default_config();
  config.spool_default.memory_limit = memory_limit;
  config.spool_blob.memory_limit = memory_limit;
  config.spool_large_text.memory_limit = memory_limit;
  config.spool_default.max_bytes = max_bytes;
  config.spool_blob.max_bytes = max_bytes;
  config.spool_large_text.max_bytes = max_bytes;
  config.spool_default.temp_dir =
      options != NULL ? options->tool_spool_dir : NULL;
  config.spool_blob.temp_dir = options != NULL ? options->tool_spool_dir : NULL;
  config.spool_large_text.temp_dir =
      options != NULL ? options->tool_spool_dir : NULL;
  return cai_lonejson_runtime_open(&config, out_runtime, error);
}

static void cai_token_usage_add(cai_token_usage *total,
                                const cai_token_usage *usage) {
  if (total == NULL || usage == NULL) {
    return;
  }
  total->input_tokens += usage->input_tokens;
  total->input_cached_tokens += usage->input_cached_tokens;
  total->output_tokens += usage->output_tokens;
  total->output_reasoning_tokens += usage->output_reasoning_tokens;
  total->total_tokens += usage->total_tokens;
}

static int cai_usage_limits_active(const cai_usage_limits *limits) {
  return limits != NULL &&
         (limits->max_input_tokens > 0LL ||
          limits->max_input_cached_tokens > 0LL ||
          limits->max_output_tokens > 0LL ||
          limits->max_output_reasoning_tokens > 0LL ||
          limits->max_total_tokens > 0LL || limits->max_spend_usd > 0.0);
}

static int cai_usage_accounting_exceeds(const cai_usage_accounting *usage,
                                        const cai_usage_limits *limits) {
  if (usage == NULL || !cai_usage_limits_active(limits)) {
    return 0;
  }
  return (limits->max_input_tokens > 0LL &&
          usage->usage.input_tokens > limits->max_input_tokens) ||
         (limits->max_input_cached_tokens > 0LL &&
          usage->usage.input_cached_tokens > limits->max_input_cached_tokens) ||
         (limits->max_output_tokens > 0LL &&
          usage->usage.output_tokens > limits->max_output_tokens) ||
         (limits->max_output_reasoning_tokens > 0LL &&
          usage->usage.output_reasoning_tokens >
              limits->max_output_reasoning_tokens) ||
         (limits->max_total_tokens > 0LL &&
          usage->usage.total_tokens > limits->max_total_tokens) ||
         (limits->max_spend_usd > 0.0 &&
          usage->estimated_spend_usd > limits->max_spend_usd);
}

static int cai_usage_limits_error(cai_error *error) {
  return cai_set_error(error, CAI_ERR_LIMIT,
                       "configured usage or spend limit exceeded");
}

static int cai_usage_spend_pricing_error(cai_error *error) {
  return cai_set_error(
      error, CAI_ERR_INVALID,
      "positive USD spend limit requires model pricing metadata");
}

static int cai_usage_limits_require_pricing(const cai_usage_limits *limits,
                                            const char *model,
                                            cai_error *error) {
  if (limits != NULL && limits->max_spend_usd > 0.0 &&
      !cai_model_can_estimate_usage_usd(model)) {
    return cai_usage_spend_pricing_error(error);
  }
  return CAI_OK;
}

static int cai_usage_limits_preflight(const cai_usage_accounting *usage,
                                      const cai_usage_limits *limits,
                                      cai_error *error) {
  if (usage != NULL && usage->limit_exceeded) {
    return cai_usage_limits_error(error);
  }
  if (cai_usage_accounting_exceeds(usage, limits)) {
    return cai_usage_limits_error(error);
  }
  return CAI_OK;
}

static void cai_usage_accounting_add(cai_usage_accounting *total,
                                     const cai_token_usage *usage,
                                     double estimated_spend_usd) {
  if (total == NULL || usage == NULL) {
    return;
  }
  cai_token_usage_add(&total->usage, usage);
  total->estimated_spend_usd += estimated_spend_usd;
}

void cai_run_options_init(cai_run_options *options) {
  if (options == NULL) {
    return;
  }
  memset(options, 0, sizeof(*options));
}

int cai_client_new_agent(cai_client *client, const cai_agent_config *config,
                         cai_agent **out, cai_error *error) {
  cai_agent *agent;
  cai_agent_impl *impl;
  cai_client_impl *client_impl;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "agent output pointer is required");
  }
  *out = NULL;
  if (client == NULL || config == NULL || config->model == NULL ||
      config->model[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "client and agent model are required");
  }
  client_impl = CAI_CLIENT_IMPL(client);
  if (client_impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "client is closed");
  }
  if (config->max_output_tokens < 0) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "max output tokens must not be negative");
  }
  if (config->max_tool_calls < 0) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "max tool calls must not be negative");
  }
  rc = cai_usage_limits_validate(&config->session_usage_limits, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_usage_limits_require_pricing(&config->session_usage_limits,
                                        config->model, error);
  if (rc != CAI_OK) {
    return rc;
  }
  agent = (cai_agent *)cai_alloc(&client_impl->allocator, sizeof(*agent));
  if (agent == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate agent");
  }
  memset(agent, 0, sizeof(*agent));
  impl = (cai_agent_impl *)cai_alloc(&client_impl->allocator, sizeof(*impl));
  if (impl == NULL) {
    cai_free_mem(&client_impl->allocator, agent);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate agent implementation");
  }
  memset(impl, 0, sizeof(*impl));
  agent->impl = impl;
  cai_agent_init_methods(agent);
  impl->client = client;
  impl->model = cai_strdup(&client_impl->allocator, config->model);
  impl->developer_instructions =
      cai_strdup(&client_impl->allocator, config->developer_instructions);
  impl->prompt_cache_key =
      cai_strdup(&client_impl->allocator, config->prompt_cache_key);
  impl->tool_choice = cai_strdup(&client_impl->allocator, config->tool_choice);
  impl->tool_choice_json =
      cai_strdup(&client_impl->allocator, config->tool_choice_json);
  impl->reasoning_effort =
      cai_strdup(&client_impl->allocator, config->reasoning_effort);
  impl->reasoning_summary =
      cai_strdup(&client_impl->allocator, config->reasoning_summary);
  impl->text_format_name =
      cai_strdup(&client_impl->allocator, config->text_format_name);
  impl->text_format_description =
      cai_strdup(&client_impl->allocator, config->text_format_description);
  impl->text_format_schema_json =
      cai_strdup(&client_impl->allocator, config->text_format_schema_json);
  impl->text_format_strict = config->text_format_strict;
  impl->max_output_tokens = config->max_output_tokens;
  impl->max_tool_calls = config->max_tool_calls;
  impl->parallel_tool_calls = config->disable_parallel_tool_calls ? 0 : -1;
  if (config->session_continuity != CAI_SESSION_CONTINUITY_SERVER &&
      config->session_continuity != CAI_SESSION_CONTINUITY_CLIENT_HISTORY &&
      config->session_continuity != CAI_SESSION_CONTINUITY_AUTO) {
    cai_agent_destroy(agent);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "invalid session continuity mode");
  }
  if (config->session_continuity == CAI_SESSION_CONTINUITY_AUTO) {
    impl->session_continuity = cai_client_base_url_is_openrouter(client_impl)
                                   ? CAI_SESSION_CONTINUITY_CLIENT_HISTORY
                                   : CAI_SESSION_CONTINUITY_SERVER;
  } else {
    impl->session_continuity = config->session_continuity;
  }
  if (config->session_continuity == CAI_SESSION_CONTINUITY_SERVER &&
      cai_client_base_url_is_openrouter(client_impl)) {
    cai_agent_warn_openrouter_server_continuity(client_impl);
  }
  impl->auto_compact = config->disable_auto_compaction ? 0 : 1;
  if (impl->session_continuity == CAI_SESSION_CONTINUITY_CLIENT_HISTORY) {
    impl->auto_compact = 0;
  }
  impl->local_history_enabled =
      config->enable_local_history ||
              impl->session_continuity == CAI_SESSION_CONTINUITY_CLIENT_HISTORY
          ? 1
          : 0;
  impl->compact_threshold_percent = config->compact_threshold_percent != 0U
                                        ? config->compact_threshold_percent
                                        : 80U;
  if (config->compact_threshold_tokens > 0LL) {
    impl->auto_compact_token_limit = config->compact_threshold_tokens;
  } else if (impl->auto_compact) {
    long long context_window;

    context_window = cai_model_context_window_tokens(impl->model);
    impl->auto_compact_token_limit =
        context_window > 0LL
            ? (context_window * (long long)impl->compact_threshold_percent) /
                  100LL
            : 0LL;
  } else {
    impl->auto_compact_token_limit = 0LL;
  }
  impl->history_memory_limit = config->history_memory_limit != 0U
                                   ? config->history_memory_limit
                                   : 128U * 1024U;
  impl->history_spool_dir =
      cai_strdup(&client_impl->allocator, config->history_spool_dir);
  impl->session_usage_limits = config->session_usage_limits;
  impl->history_runtime = NULL;
  impl->hosted_tools.items = NULL;
  impl->hosted_tools.count = 0U;
  impl->hosted_tools.capacity = 0U;
  impl->hosted_tools.elem_size = sizeof(struct cai_function_tool);
  impl->hosted_tools.flags = 0U;
  impl->tools = NULL;
  impl->default_session = NULL;
  if (impl->model == NULL ||
      (config->developer_instructions != NULL &&
       impl->developer_instructions == NULL) ||
      (config->prompt_cache_key != NULL && impl->prompt_cache_key == NULL) ||
      (config->tool_choice != NULL && impl->tool_choice == NULL) ||
      (config->tool_choice_json != NULL && impl->tool_choice_json == NULL) ||
      (config->reasoning_effort != NULL && impl->reasoning_effort == NULL) ||
      (config->reasoning_summary != NULL && impl->reasoning_summary == NULL) ||
      (config->text_format_name != NULL && impl->text_format_name == NULL) ||
      (config->text_format_description != NULL &&
       impl->text_format_description == NULL) ||
      (config->text_format_schema_json != NULL &&
       impl->text_format_schema_json == NULL) ||
      (config->history_spool_dir != NULL && impl->history_spool_dir == NULL)) {
    cai_agent_destroy(agent);
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate agent");
  }
  if (impl->auto_compact && impl->auto_compact_token_limit <= 0LL) {
    cai_agent_destroy(agent);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "model has unknown context window; set "
                         "compact_threshold_tokens or disable auto compaction");
  }
  if (impl->auto_compact && impl->compact_threshold_percent > 95U &&
      config->compact_threshold_tokens <= 0LL) {
    cai_agent_destroy(agent);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "compact threshold percent must be 1..95");
  }
  if (impl->auto_compact && impl->auto_compact_token_limit < 1000LL) {
    cai_agent_destroy(agent);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "compact threshold must be at least 1000 tokens");
  }
  {
    lonejson_config runtime_config;

    runtime_config = lonejson_default_config();
    runtime_config.spool_default.memory_limit = impl->history_memory_limit;
    runtime_config.spool_blob.memory_limit = impl->history_memory_limit;
    runtime_config.spool_large_text.memory_limit = impl->history_memory_limit;
    runtime_config.spool_default.temp_dir = impl->history_spool_dir;
    runtime_config.spool_blob.temp_dir = impl->history_spool_dir;
    runtime_config.spool_large_text.temp_dir = impl->history_spool_dir;
    if (cai_lonejson_runtime_open(&runtime_config, &impl->history_runtime,
                                  error) != CAI_OK) {
      cai_agent_destroy(agent);
      return error != NULL ? error->code : CAI_ERR_NOMEM;
    }
  }
  if (cai_tool_registry_new(&impl->tools, error) != CAI_OK) {
    cai_agent_destroy(agent);
    return error != NULL ? error->code : CAI_ERR_NOMEM;
  }
  *out = agent;
  return CAI_OK;
}

void cai_agent_destroy(cai_agent *agent) {
  cai_allocator *allocator;
  cai_agent_impl *impl;

  if (agent == NULL) {
    return;
  }
  impl = CAI_AGENT_IMPL(agent);
  if (impl == NULL) {
    return;
  }
  allocator = &CAI_CLIENT_IMPL(impl->client)->allocator;
  cai_session_destroy(impl->default_session);
  cai_free_mem(allocator, impl->model);
  cai_free_mem(allocator, impl->developer_instructions);
  cai_free_mem(allocator, impl->prompt_cache_key);
  cai_free_mem(allocator, impl->tool_choice);
  cai_free_mem(allocator, impl->tool_choice_json);
  cai_free_mem(allocator, impl->reasoning_effort);
  cai_free_mem(allocator, impl->reasoning_summary);
  cai_free_mem(allocator, impl->text_format_name);
  cai_free_mem(allocator, impl->text_format_description);
  cai_free_mem(allocator, impl->text_format_schema_json);
  cai_lonejson_runtime_close(&impl->history_runtime);
  cai_free_mem(allocator, impl->history_spool_dir);
  if (impl->hosted_tools.items != NULL) {
    struct cai_function_tool *hosted_tools;
    size_t i;

    hosted_tools = (struct cai_function_tool *)impl->hosted_tools.items;
    for (i = 0U; i < impl->hosted_tools.count; i++) {
      cai_free_mem(allocator, hosted_tools[i].raw_json);
    }
    cai_free_mem(allocator, impl->hosted_tools.items);
  }
  if (impl->tools != NULL) {
    impl->tools->destroy(impl->tools);
  }
  cai_free_mem(allocator, impl);
  agent->impl = NULL;
  cai_free_mem(allocator, agent);
}

int cai_agent_register_tool(cai_agent *agent, const char *name,
                            const char *description,
                            const lonejson_map *params_map,
                            const lonejson_map *result_map,
                            cai_tool_fn callback, void *context,
                            cai_error *error) {
  return cai_agent_register_lonejson_tool(agent, name, description, params_map,
                                          result_map, callback, context, error);
}

int cai_agent_register_lonejson_tool(cai_agent *agent, const char *name,
                                     const char *description,
                                     const lonejson_map *params_map,
                                     const lonejson_map *result_map,
                                     cai_tool_fn callback, void *context,
                                     cai_error *error) {
  cai_agent_impl *impl;

  if (agent == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  impl = CAI_AGENT_IMPL(agent);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is closed");
  }
  return impl->tools->register_lonejson(impl->tools, name, description,
                                        params_map, result_map, callback,
                                        context, error);
}

int cai_agent_register_raw_tool(cai_agent *agent, const char *name,
                                const char *description,
                                const char *schema_json, int strict,
                                cai_tool_raw_fn callback, void *context,
                                cai_error *error) {
  cai_agent_impl *impl;

  if (agent == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  impl = CAI_AGENT_IMPL(agent);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is closed");
  }
  return impl->tools->register_raw(impl->tools, name, description, schema_json,
                                   strict, callback, context, error);
}

int cai_agent_register_raw_spooled_tool(cai_agent *agent, const char *name,
                                        const char *description,
                                        const char *schema_json, int strict,
                                        cai_tool_raw_spooled_fn callback,
                                        void *context, cai_error *error) {
  cai_agent_impl *impl;

  if (agent == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  impl = CAI_AGENT_IMPL(agent);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is closed");
  }
  return impl->tools->register_raw_spooled(impl->tools, name, description,
                                           schema_json, strict, callback,
                                           context, error);
}

static int cai_agent_hosted_tools_grow(cai_agent_impl *impl, cai_error *error) {
  cai_client_impl *client_impl;
  size_t new_capacity;
  void *items;

  if (impl->hosted_tools.count < impl->hosted_tools.capacity) {
    return CAI_OK;
  }
  client_impl = CAI_CLIENT_IMPL(impl->client);
  new_capacity =
      impl->hosted_tools.capacity == 0U ? 2U : impl->hosted_tools.capacity * 2U;
  items = cai_realloc_mem(&client_impl->allocator, impl->hosted_tools.items,
                          new_capacity * sizeof(struct cai_function_tool));
  if (items == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to grow hosted tools");
  }
  impl->hosted_tools.items = items;
  impl->hosted_tools.capacity = new_capacity;
  return CAI_OK;
}

int cai_agent_add_hosted_tool_json(cai_agent *agent, const char *tool_json,
                                   cai_error *error) {
  cai_agent_impl *impl;
  cai_client_impl *client_impl;
  cai_response_create_params *params;
  struct cai_function_tool *tools;
  struct cai_function_tool *tool;
  int rc;

  if (agent == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  impl = CAI_AGENT_IMPL(agent);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is closed");
  }
  params = NULL;
  rc = cai_response_create_params_new(&params, error);
  if (rc == CAI_OK) {
    rc = cai_response_create_params_add_hosted_tool_json(params, tool_json,
                                                         error);
  }
  cai_response_create_params_destroy(params);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_agent_hosted_tools_grow(impl, error);
  if (rc != CAI_OK) {
    return rc;
  }
  client_impl = CAI_CLIENT_IMPL(impl->client);
  tools = (struct cai_function_tool *)impl->hosted_tools.items;
  tool = &tools[impl->hosted_tools.count];
  memset(tool, 0, sizeof(*tool));
  tool->raw_json = cai_strdup(&client_impl->allocator, tool_json);
  tool->is_raw = 1;
  if (tool->raw_json == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate hosted tool");
  }
  impl->hosted_tools.count++;
  return CAI_OK;
}

int cai_agent_add_simple_hosted_tool(cai_agent *agent, const char *type,
                                     cai_error *error) {
  cai_response_create_params *params;
  struct cai_function_tool *tools;
  const char *tool_json;
  int rc;

  params = NULL;
  rc = cai_response_create_params_new(&params, error);
  if (rc == CAI_OK) {
    rc = cai_response_create_params_add_simple_hosted_tool(params, type, error);
  }
  if (rc == CAI_OK) {
    tools = (struct cai_function_tool *)params->tools.items;
    tool_json =
        tools != NULL && params->tools.count == 1U ? tools[0].raw_json : NULL;
    rc = cai_agent_add_hosted_tool_json(agent, tool_json, error);
  }
  cai_response_create_params_destroy(params);
  return rc;
}

int cai_agent_add_hosted_mcp_tool(cai_agent *agent,
                                  const cai_hosted_mcp_tool_config *config,
                                  cai_error *error) {
  cai_response_create_params *params;
  struct cai_function_tool *tools;
  const char *tool_json;
  int rc;

  params = NULL;
  rc = cai_response_create_params_new(&params, error);
  if (rc == CAI_OK) {
    rc = cai_response_create_params_add_hosted_mcp_tool(params, config, error);
  }
  if (rc == CAI_OK) {
    tools = (struct cai_function_tool *)params->tools.items;
    tool_json =
        tools != NULL && params->tools.count == 1U ? tools[0].raw_json : NULL;
    rc = cai_agent_add_hosted_tool_json(agent, tool_json, error);
  }
  cai_response_create_params_destroy(params);
  return rc;
}

int cai_agent_new_session(cai_agent *agent, cai_session **out,
                          cai_error *error) {
  cai_session *session;
  cai_session_impl *impl;
  cai_agent_impl *agent_impl;
  cai_client_impl *client_impl;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session output pointer is required");
  }
  *out = NULL;
  if (agent == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  agent_impl = CAI_AGENT_IMPL(agent);
  if (agent_impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is closed");
  }
  client_impl = CAI_CLIENT_IMPL(agent_impl->client);
  session = (cai_session *)cai_alloc(&client_impl->allocator, sizeof(*session));
  if (session == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate session");
  }
  memset(session, 0, sizeof(*session));
  impl = (cai_session_impl *)cai_alloc(&client_impl->allocator, sizeof(*impl));
  if (impl == NULL) {
    cai_free_mem(&client_impl->allocator, session);
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate session implementation");
  }
  memset(impl, 0, sizeof(*impl));
  session->impl = impl;
  cai_session_init_methods(session);
  impl->agent = agent;
  impl->previous_response_id = NULL;
  impl->conversation_id = NULL;
  memset(&impl->last_usage, 0, sizeof(impl->last_usage));
  impl->has_last_usage = 0;
  impl->usage_limits = agent_impl->session_usage_limits;
  cai_usage_accounting_init(&impl->usage);
  agent_impl->history_runtime->spooled_init(agent_impl->history_runtime,
                                            &impl->history);
  impl->inputs = NULL;
  impl->input_count = 0U;
  impl->input_capacity = 0U;
  *out = session;
  return CAI_OK;
}

int cai_agent_new_conversation_session(cai_agent *agent, cai_session **out,
                                       cai_error *error) {
  cai_conversation *conversation;
  cai_session *session;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session output pointer is required");
  }
  *out = NULL;
  if (agent == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  conversation = NULL;
  session = NULL;
  rc = cai_client_create_conversation(CAI_AGENT_IMPL(agent)->client,
                                      &conversation, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_agent_new_session(agent, &session, error);
  if (rc == CAI_OK) {
    rc = cai_session_set_conversation_id(
        session, cai_conversation_id(conversation), error);
  }
  cai_conversation_destroy(conversation);
  if (rc != CAI_OK) {
    cai_session_destroy(session);
    return rc;
  }
  *out = session;
  return CAI_OK;
}

int cai_agent_new_session_for_conversation(cai_agent *agent,
                                           const cai_conversation *conversation,
                                           cai_session **out,
                                           cai_error *error) {
  cai_session *session;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session output pointer is required");
  }
  *out = NULL;
  if (agent == NULL || conversation == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "agent and conversation are required");
  }
  session = NULL;
  rc = cai_agent_new_session(agent, &session, error);
  if (rc == CAI_OK) {
    rc = cai_session_set_conversation(session, conversation, error);
  }
  if (rc != CAI_OK) {
    cai_session_destroy(session);
    return rc;
  }
  *out = session;
  return CAI_OK;
}

static void cai_session_clear_inputs(cai_session *session) {
  cai_allocator *allocator;
  size_t i;

  if (session == NULL) {
    return;
  }
  allocator = &CAI_SESSION_CLIENT_IMPL(session)->allocator;
  for (i = 0U; i < CAI_SESSION_IMPL(session)->input_count; i++) {
    cai_free_mem(allocator, CAI_SESSION_IMPL(session)->inputs[i].role);
    cai_free_mem(allocator, CAI_SESSION_IMPL(session)->inputs[i].text);
    if (CAI_SESSION_IMPL(session)->inputs[i].has_text_spooled) {
      CAI_SESSION_IMPL(session)->inputs[i].text_spooled.cleanup(
          &CAI_SESSION_IMPL(session)->inputs[i].text_spooled);
    }
    cai_free_mem(allocator, CAI_SESSION_IMPL(session)->inputs[i].image_url);
    cai_free_mem(allocator, CAI_SESSION_IMPL(session)->inputs[i].filename);
    cai_free_mem(allocator, CAI_SESSION_IMPL(session)->inputs[i].detail);
    cai_free_mem(allocator, CAI_SESSION_IMPL(session)->inputs[i].call_id);
    cai_free_mem(allocator, CAI_SESSION_IMPL(session)->inputs[i].output);
    if (CAI_SESSION_IMPL(session)->inputs[i].has_file_data) {
      CAI_SESSION_IMPL(session)->inputs[i].file_data.cleanup(
          &CAI_SESSION_IMPL(session)->inputs[i].file_data);
    }
  }
  CAI_SESSION_IMPL(session)->input_count = 0U;
}

static int cai_history_append_bytes(lonejson_spooled *history,
                                    const void *bytes, size_t length,
                                    cai_error *error) {
  lonejson_error json_error;

  if (length == 0U) {
    return CAI_OK;
  }
  lonejson_error_init(&json_error);
  if (history->append(history, bytes, length, &json_error) ==
      LONEJSON_STATUS_OK) {
    return CAI_OK;
  }
  return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                              "failed to append history spool",
                              json_error.message);
}

static lonejson_status cai_history_lonejson_sink(void *user, const void *data,
                                                 size_t len,
                                                 lonejson_error *error) {
  cai_history_sink_context *context;
  (void)error;
  context = (cai_history_sink_context *)user;
  if (context->spool->append(context->spool, data, len, error) ==
      LONEJSON_STATUS_OK) {
    return LONEJSON_STATUS_OK;
  }
  return LONEJSON_STATUS_CALLBACK_FAILED;
}

static int cai_agent_clone_spooled(cai_session *session,
                                   const lonejson_spooled *src,
                                   lonejson_spooled *dst, cai_error *error) {
  cai_history_sink_context sink_context;
  lonejson_error json_error;

  if (src == NULL || dst == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "spooled value and output are required");
  }
  memset(dst, 0, sizeof(*dst));
  cai_history_init_spooled(session, dst);
  sink_context.spool = dst;
  lonejson_error_init(&json_error);
  if (src->write_to_sink(src, cai_history_lonejson_sink, &sink_context,
                         &json_error) == LONEJSON_STATUS_OK) {
    return CAI_OK;
  }
  dst->cleanup(dst);
  return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                              "failed to clone spooled value",
                              json_error.message);
}

static lonejson_status cai_lonejson_write_cai_sink(void *user, const void *data,
                                                   size_t len,
                                                   lonejson_error *error) {
  cai_lonejson_cai_sink_context *context;
  int rc;

  (void)error;
  context = (cai_lonejson_cai_sink_context *)user;
  if (context == NULL || context->sink == NULL) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  rc = cai_sink_write(context->sink, data, len, context->error);
  return rc == CAI_OK ? LONEJSON_STATUS_OK : LONEJSON_STATUS_CALLBACK_FAILED;
}

static void cai_stream_tool_call_cleanup(cai_response_tool_call *call) {
  if (call == NULL) {
    return;
  }
  cai_free_mem(NULL, call->id);
  cai_free_mem(NULL, call->call_id);
  cai_free_mem(NULL, call->name);
  cai_free_mem(NULL, call->arguments);
  if (call->has_arguments_spooled) {
    call->arguments_spooled.cleanup(&call->arguments_spooled);
  }
  memset(call, 0, sizeof(*call));
}

static cai_response_tool_call *
cai_stream_tool_call_list_find(cai_stream_tool_call_list *list,
                               const char *item_id, int output_index) {
  size_t i;

  if (list == NULL || item_id == NULL) {
    return NULL;
  }
  for (i = 0U; i < list->count; i++) {
    if (list->items[i].id != NULL && strcmp(list->items[i].id, item_id) == 0 &&
        list->items[i].output_index == output_index) {
      return &list->items[i];
    }
  }
  return NULL;
}

static int cai_stream_tool_call_list_grow(cai_stream_tool_call_list *list) {
  cai_response_tool_call *next_items;
  size_t next_capacity;

  if (list == NULL) {
    return CAI_ERR_INVALID;
  }
  if (list->count < list->capacity) {
    return CAI_OK;
  }
  next_capacity = list->capacity == 0U ? 4U : list->capacity * 2U;
  next_items = (cai_response_tool_call *)cai_realloc_mem(
      NULL, list->items, next_capacity * sizeof(*next_items));
  if (next_items == NULL) {
    return CAI_ERR_NOMEM;
  }
  list->items = next_items;
  list->capacity = next_capacity;
  return CAI_OK;
}

static int cai_stream_tool_call_list_append_delta(
    cai_stream_tool_call_list *list, const char *item_id, int output_index,
    const lonejson_spooled *delta, cai_error *error) {
  cai_response_tool_call *call;
  cai_history_sink_context sink_context;
  lonejson_error json_error;
  int rc;

  if (list == NULL || item_id == NULL || delta == NULL ||
      delta->size_fn(delta) == 0U) {
    return CAI_OK;
  }
  call = cai_stream_tool_call_list_find(list, item_id, output_index);
  if (call == NULL) {
    rc = cai_stream_tool_call_list_grow(list);
    if (rc != CAI_OK) {
      return cai_set_error(error, rc, "failed to grow stream tool calls");
    }
    call = &list->items[list->count];
    memset(call, 0, sizeof(*call));
    call->id = cai_strdup(NULL, item_id);
    if (call->id == NULL) {
      cai_stream_tool_call_cleanup(call);
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate stream tool call id");
    }
    call->output_index = output_index;
    CAI_LJ->spooled_init(CAI_LJ, &call->arguments_spooled);
    call->has_arguments_spooled = 1;
    list->count++;
  } else if (!call->has_arguments_spooled) {
    CAI_LJ->spooled_init(CAI_LJ, &call->arguments_spooled);
    call->has_arguments_spooled = 1;
  }
  lonejson_error_init(&json_error);
  sink_context.spool = &call->arguments_spooled;
  if (delta->write_to_sink(delta, cai_history_lonejson_sink, &sink_context,
                           &json_error) != LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to spool streamed tool arguments",
                                json_error.message);
  }
  return CAI_OK;
}

static void cai_stream_tool_call_list_cleanup(cai_stream_tool_call_list *list) {
  size_t i;

  if (list == NULL) {
    return;
  }
  for (i = 0U; i < list->count; i++) {
    cai_stream_tool_call_cleanup(&list->items[i]);
  }
  cai_free_mem(NULL, list->items);
  memset(list, 0, sizeof(*list));
}

static lonejson_status cai_agent_spool_sink(void *user, const void *data,
                                            size_t len, lonejson_error *error);

static int
cai_stream_tool_call_set_final_arguments(cai_response_tool_call *call,
                                         const lonejson_spooled *arguments,
                                         cai_error *error) {
  cai_history_sink_context sink_context;
  lonejson_error json_error;
  lonejson_status status;

  if (call == NULL || arguments == NULL) {
    return CAI_ERR_INVALID;
  }
  cai_free_mem(NULL, call->arguments);
  call->arguments = NULL;
  if (call->has_arguments_spooled) {
    call->arguments_spooled.cleanup(&call->arguments_spooled);
    call->has_arguments_spooled = 0;
  }
  CAI_LJ->spooled_init(CAI_LJ, &call->arguments_spooled);
  call->has_arguments_spooled = 1;
  lonejson_error_init(&json_error);
  sink_context.spool = &call->arguments_spooled;
  status = arguments->write_to_sink(arguments, cai_history_lonejson_sink,
                                    &sink_context, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to spool streamed tool arguments",
                                json_error.message);
  }
  return CAI_OK;
}

static int cai_stream_tool_call_list_append(
    cai_stream_tool_call_list *list, const char *item_id, int output_index,
    const char *call_id, const char *name, const lonejson_spooled *arguments,
    cai_error *error) {
  cai_response_tool_call *call;
  int rc;
  int appended;
  size_t i;

  if (list == NULL || call_id == NULL || name == NULL || arguments == NULL) {
    return CAI_ERR_INVALID;
  }
  for (i = 0U; i < list->count; i++) {
    if (list->items[i].call_id != NULL &&
        strcmp(list->items[i].call_id, call_id) == 0) {
      return cai_stream_tool_call_set_final_arguments(&list->items[i],
                                                      arguments, error);
    }
  }
  call = cai_stream_tool_call_list_find(list, item_id, output_index);
  appended = 0;
  if (call == NULL) {
    rc = cai_stream_tool_call_list_grow(list);
    if (rc != CAI_OK) {
      return rc;
    }
    call = &list->items[list->count];
    memset(call, 0, sizeof(*call));
    call->id = cai_strdup(NULL, item_id != NULL ? item_id : "");
    call->output_index = output_index;
    list->count++;
    appended = 1;
  } else if (call->id == NULL) {
    call->id = cai_strdup(NULL, item_id != NULL ? item_id : "");
  }
  call->call_id = cai_strdup(NULL, call_id);
  call->name = cai_strdup(NULL, name);
  if (call->id == NULL || call->call_id == NULL || call->name == NULL) {
    cai_stream_tool_call_cleanup(call);
    if (appended) {
      list->count--;
    }
    return CAI_ERR_NOMEM;
  }
  rc = cai_stream_tool_call_set_final_arguments(call, arguments, error);
  if (rc != CAI_OK) {
    cai_stream_tool_call_cleanup(call);
    if (appended) {
      list->count--;
    }
    return rc;
  }
  return CAI_OK;
}

static lonejson_status cai_agent_spool_sink(void *user, const void *data,
                                            size_t len, lonejson_error *error) {
  return ((lonejson_spooled *)user)
      ->append((lonejson_spooled *)user, data, len, error);
}

static int cai_stream_tool_calls_spool(cai_session *session,
                                       const cai_stream_tool_call_list *calls,
                                       lonejson_spooled *out, size_t *out_len,
                                       cai_error *error) {
  lonejson_writer writer;
  lonejson_error json_error;
  lonejson_status status;
  size_t i;
  int writer_initialized;

  if (out_len != NULL) {
    *out_len = 0U;
  }
  cai_history_init_spooled(session, out);
  if (calls == NULL || calls->count == 0U) {
    return CAI_OK;
  }
  lonejson_error_init(&json_error);
  writer_initialized = 0;
  status = CAI_LJ->writer_init_sink(CAI_LJ, &writer, cai_agent_spool_sink, out,
                                    &json_error);
  if (status == LONEJSON_STATUS_OK) {
    writer_initialized = 1;
    status = writer.begin_array(&writer, &json_error);
  }
  for (i = 0U; status == LONEJSON_STATUS_OK && i < calls->count; i++) {
    status = writer.begin_object(&writer, &json_error);
    if (status == LONEJSON_STATUS_OK) {
      status = writer.key(&writer, "type", 4U, &json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = writer.string(&writer, "function_call", 13U, &json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = writer.key(&writer, "call_id", 7U, &json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = writer.string(
          &writer,
          calls->items[i].call_id != NULL ? calls->items[i].call_id : "",
          calls->items[i].call_id != NULL ? strlen(calls->items[i].call_id)
                                          : 0U,
          &json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = writer.key(&writer, "name", 4U, &json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = writer.string(
          &writer, calls->items[i].name != NULL ? calls->items[i].name : "",
          calls->items[i].name != NULL ? strlen(calls->items[i].name) : 0U,
          &json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = writer.key(&writer, "arguments", 9U, &json_error);
    }
    if (status == LONEJSON_STATUS_OK) {
      if (calls->items[i].has_arguments_spooled) {
        status = writer.string_spooled(
            &writer, &calls->items[i].arguments_spooled, &json_error);
      } else {
        status = writer.string(
            &writer,
            calls->items[i].arguments != NULL ? calls->items[i].arguments : "",
            calls->items[i].arguments != NULL
                ? strlen(calls->items[i].arguments)
                : 0U,
            &json_error);
      }
    }
    if (status == LONEJSON_STATUS_OK) {
      status = writer.end_object(&writer, &json_error);
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
                                "failed to serialize streamed tool calls",
                                json_error.message);
  }
  if (out_len != NULL) {
    *out_len = out->size_fn(out);
  }
  return CAI_OK;
}

static int cai_stream_output_text_spool(cai_session *session,
                                        const lonejson_spooled *text,
                                        lonejson_spooled *out, size_t *out_len,
                                        cai_error *error) {
  lonejson_writer writer;
  lonejson_error json_error;
  lonejson_status status;
  int writer_initialized;

  if (out_len != NULL) {
    *out_len = 0U;
  }
  cai_history_init_spooled(session, out);
  if (text == NULL || text->size_fn(text) == 0U) {
    return CAI_OK;
  }
  lonejson_error_init(&json_error);
  writer_initialized = 0;
  status = CAI_LJ->writer_init_sink(CAI_LJ, &writer, cai_agent_spool_sink, out,
                                    &json_error);
  if (status == LONEJSON_STATUS_OK) {
    writer_initialized = 1;
    status = writer.begin_array(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.key(&writer, "type", 4U, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.string(&writer, "message", 7U, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.key(&writer, "role", 4U, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.string(&writer, "assistant", 9U, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.key(&writer, "content", 7U, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.begin_array(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.begin_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.key(&writer, "type", 4U, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.string(&writer, "output_text", 11U, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.key(&writer, "text", 4U, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.string_spooled(&writer, text, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.end_object(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.end_array(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.end_object(&writer, &json_error);
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
                                "failed to serialize streamed output text",
                                json_error.message);
  }
  if (out_len != NULL) {
    *out_len = out->size_fn(out);
  }
  return CAI_OK;
}

static int
cai_stream_capture_output_items_start(cai_stream_tool_capture *capture,
                                      cai_error *error) {
  if (capture == NULL || capture->session == NULL) {
    return CAI_OK;
  }
  if (capture->output_items_initialized) {
    return CAI_OK;
  }
  cai_history_init_spooled(capture->session, &capture->output_items);
  capture->output_items_initialized = 1;
  return cai_history_append_bytes(&capture->output_items, "[", 1U, error);
}

static int
cai_stream_capture_output_items_finish(cai_stream_tool_capture *capture,
                                       cai_error *error) {
  if (capture == NULL || !capture->output_items_initialized) {
    return CAI_OK;
  }
  return cai_history_append_bytes(&capture->output_items, "]", 1U, error);
}

static int
cai_stream_capture_sanitized_output_item(cai_stream_tool_capture *capture,
                                         const lonejson_spooled *item_json,
                                         cai_error *error) {
  cai_spooled_reader_context reader_context;
  cai_history_sink_context sink_context;
  lonejson_value_rewrite_selector_options options;
  lonejson_error json_error;
  lonejson_status status;
  int rc;

  if (capture == NULL || item_json == NULL ||
      item_json->size_fn(item_json) == 0U) {
    return CAI_OK;
  }
  rc = cai_stream_capture_output_items_start(capture, error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (capture->output_items_count > 0U) {
    rc = cai_history_append_bytes(&capture->output_items, ",", 1U, error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  memset(&reader_context, 0, sizeof(reader_context));
  reader_context.cursor = *item_json;
  status = reader_context.cursor.rewind(&reader_context.cursor, NULL);
  if (status != LONEJSON_STATUS_OK) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to rewind streamed output item");
  }
  memset(&sink_context, 0, sizeof(sink_context));
  sink_context.spool = &capture->output_items;
  memset(&options, 0, sizeof(options));
  options.selector = "id";
  options.action = LONEJSON_VALUE_REWRITE_DROP;
  lonejson_error_init(&json_error);
  status = CAI_LJ->value_rewrite_selector_reader(
      CAI_LJ, cai_history_spooled_reader, &reader_context,
      cai_history_lonejson_sink, &sink_context, &options, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to sanitize streamed output item",
                                json_error.message);
  }
  capture->output_items_count++;
  return CAI_OK;
}

static int cai_stream_capture_output_text(void *context, const char *item_id,
                                          int output_index,
                                          const lonejson_spooled *delta,
                                          cai_error *error) {
  cai_stream_tool_capture *capture;
  cai_history_sink_context sink_context;
  lonejson_error json_error;
  int rc;

  (void)item_id;
  (void)output_index;
  capture = (cai_stream_tool_capture *)context;
  rc = CAI_OK;
  if (capture != NULL && capture->output_text != NULL && delta != NULL &&
      delta->size_fn(delta) > 0U) {
    lonejson_error_init(&json_error);
    sink_context.spool = capture->output_text;
    if (delta->write_to_sink(delta, cai_history_lonejson_sink, &sink_context,
                             &json_error) != LONEJSON_STATUS_OK) {
      rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to spool streamed output text",
                                json_error.message);
    }
  }
  if (rc == CAI_OK && capture != NULL && capture->user_sinks != NULL &&
      capture->user_sinks->output_text_delta != NULL) {
    rc = capture->user_sinks->output_text_delta(
        capture->user_sinks->output_text_context, item_id, output_index, delta,
        error);
  }
  return rc;
}

static int cai_stream_capture_tool_delta(void *context, const char *item_id,
                                         int output_index,
                                         const lonejson_spooled *delta,
                                         cai_error *error) {
  cai_stream_tool_capture *capture;
  int rc;

  capture = (cai_stream_tool_capture *)context;
  rc = CAI_OK;
  if (capture != NULL && capture->tool_calls != NULL) {
    rc = cai_stream_tool_call_list_append_delta(capture->tool_calls, item_id,
                                                output_index, delta, error);
  }
  if (rc == CAI_OK && capture != NULL && capture->user_sinks != NULL &&
      capture->user_sinks->function_call_arguments_delta != NULL) {
    rc = capture->user_sinks->function_call_arguments_delta(
        capture->user_sinks->function_call_context, item_id, output_index,
        delta, error);
  }
  return rc;
}

static int cai_stream_capture_tool_done(void *context, const char *item_id,
                                        int output_index, const char *call_id,
                                        const char *name,
                                        const lonejson_spooled *arguments,
                                        cai_error *error) {
  cai_stream_tool_capture *capture;
  int rc;

  capture = (cai_stream_tool_capture *)context;
  rc = CAI_OK;
  if (capture != NULL && capture->tool_calls != NULL) {
    rc = cai_stream_tool_call_list_append(capture->tool_calls, item_id,
                                          output_index, call_id, name,
                                          arguments, error);
  }
  if (rc == CAI_OK && capture != NULL && capture->user_sinks != NULL &&
      capture->user_sinks->function_call_arguments_done != NULL) {
    rc = capture->user_sinks->function_call_arguments_done(
        capture->user_sinks->function_call_context, item_id, output_index,
        call_id, name, arguments, error);
  }
  return rc;
}

static int cai_stream_capture_output_item_done(
    void *context, const char *item_id, int output_index, const char *type,
    const lonejson_spooled *item_json, cai_error *error) {
  cai_stream_tool_capture *capture;
  int rc;

  capture = (cai_stream_tool_capture *)context;
  rc = CAI_OK;
  if (capture != NULL &&
      CAI_SESSION_AGENT_IMPL(capture->session)->session_continuity ==
          CAI_SESSION_CONTINUITY_CLIENT_HISTORY &&
      (type == NULL || strcmp(type, "function_call") != 0)) {
    rc = cai_stream_capture_sanitized_output_item(capture, item_json, error);
  }
  if (rc == CAI_OK && capture != NULL && capture->user_sinks != NULL &&
      capture->user_sinks->output_item_done != NULL) {
    rc = capture->user_sinks->output_item_done(
        capture->user_sinks->output_item_context, item_id, output_index, type,
        item_json, error);
  }
  return rc;
}

static int cai_session_after_stream_tool_calls(
    cai_session *session, const lonejson_spooled *pending_items,
    int has_pending_items, const char *response_id,
    const cai_token_usage *usage, const cai_stream_tool_call_list *tool_calls,
    const lonejson_spooled *output_text,
    const lonejson_spooled *stream_output_items,
    size_t stream_output_items_count, cai_error *error) {
  lonejson_spooled call_items;
  lonejson_spooled output_items;
  size_t call_items_len;
  size_t output_items_len;
  int has_call_items;
  int has_output_items;
  int rc;

  memset(&call_items, 0, sizeof(call_items));
  memset(&output_items, 0, sizeof(output_items));
  call_items_len = 0U;
  output_items_len = 0U;
  has_call_items = 0;
  has_output_items = 0;
  rc = cai_session_remember_response_id(session, response_id, error);
  if (rc == CAI_OK && !cai_token_usage_is_empty(usage)) {
    rc = cai_session_record_usage(session, usage, error);
  }
  if (rc == CAI_OK && !CAI_SESSION_AGENT_IMPL(session)->local_history_enabled) {
    return rc;
  }
  if (rc == CAI_OK && has_pending_items) {
    rc = cai_history_append_array_record_spooled(session, pending_items, error);
  }
  if (rc == CAI_OK && tool_calls != NULL && tool_calls->count > 0U) {
    rc = cai_stream_tool_calls_spool(session, tool_calls, &call_items,
                                     &call_items_len, error);
    if (rc == CAI_OK) {
      has_call_items = 1;
    }
  }
  if (rc == CAI_OK && call_items_len > 0U) {
    rc = cai_history_append_array_record_spooled(session, &call_items, error);
  }
  if (rc == CAI_OK && stream_output_items_count > 0U &&
      stream_output_items != NULL) {
    rc = cai_history_append_array_record_spooled(session, stream_output_items,
                                                 error);
  }
  if (rc == CAI_OK && stream_output_items_count == 0U && output_text != NULL &&
      output_text->size_fn(output_text) > 0U) {
    rc = cai_stream_output_text_spool(session, output_text, &output_items,
                                      &output_items_len, error);
    if (rc == CAI_OK) {
      has_output_items = 1;
    }
  }
  if (rc == CAI_OK && output_items_len > 0U) {
    rc = cai_history_append_array_record_spooled(session, &output_items, error);
  }
  if (has_call_items) {
    call_items.cleanup(&call_items);
  }
  if (has_output_items) {
    output_items.cleanup(&output_items);
  }
  return rc;
}

int cai_tool_event_write_output(const cai_tool_event *event, cai_sink *sink,
                                cai_error *error) {
  cai_lonejson_cai_sink_context context;
  lonejson_error json_error;

  if (event == NULL || event->output_json == NULL || sink == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "tool event output and sink are required");
  }
  context.sink = sink;
  context.error = error;
  lonejson_error_init(&json_error);
  if (event->output_json->write_to_sink(event->output_json,
                                        cai_lonejson_write_cai_sink, &context,
                                        &json_error) == LONEJSON_STATUS_OK) {
    return CAI_OK;
  }
  if (error != NULL && error->message != NULL) {
    return error->code;
  }
  return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                              "failed to write tool event output",
                              json_error.message);
}

int cai_tool_event_write_arguments(const cai_tool_event *event, cai_sink *sink,
                                   cai_error *error) {
  cai_lonejson_cai_sink_context context;
  lonejson_error json_error;

  if (event == NULL || sink == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "tool event and sink are required");
  }
  if (event->arguments_json != NULL) {
    return cai_sink_write(sink, event->arguments_json,
                          strlen(event->arguments_json), error);
  }
  if (event->arguments_json_spooled == NULL) {
    return cai_sink_write(sink, "{}", 2U, error);
  }
  context.sink = sink;
  context.error = error;
  lonejson_error_init(&json_error);
  if (event->arguments_json_spooled->write_to_sink(
          event->arguments_json_spooled, cai_lonejson_write_cai_sink, &context,
          &json_error) == LONEJSON_STATUS_OK) {
    return CAI_OK;
  }
  if (error != NULL && error->message != NULL) {
    return error->code;
  }
  return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                              "failed to write tool event arguments",
                              json_error.message);
}

static void cai_history_init_spooled(cai_session *session,
                                     lonejson_spooled *spool) {
  lonejson *runtime;

  runtime = cai_agent_history_runtime(session);
  runtime->spooled_init(runtime, spool);
}

static lonejson *cai_agent_history_runtime(cai_session *session) {
  lonejson *runtime;

  runtime = CAI_SESSION_AGENT_IMPL(session)->history_runtime;
  return runtime != NULL ? runtime : CAI_LJ;
}

static int cai_history_append_array_record_spooled(cai_session *session,
                                                   const lonejson_spooled *json,
                                                   cai_error *error) {
  return cai_history_append_array_record_to_spool(
      session, &CAI_SESSION_IMPL(session)->history, json, error);
}

static int cai_history_append_array_record_to_spool(
    cai_session *session, lonejson_spooled *history,
    const lonejson_spooled *json, cai_error *error) {
  cai_history_sink_context sink_context;
  lonejson_error json_error;
  char header[32];
  size_t header_length;
  int rc;

  (void)session;
  if (json == NULL || json->size_fn(json) == 0U) {
    return CAI_OK;
  }
  snprintf(header, sizeof(header), "%lu\n", (unsigned long)json->size_fn(json));
  header_length = strlen(header);
  rc = cai_history_append_bytes(history, header, header_length, error);
  if (rc == CAI_OK) {
    sink_context.spool = history;
    lonejson_error_init(&json_error);
    if (json->write_to_sink(json, cai_history_lonejson_sink, &sink_context,
                            &json_error) != LONEJSON_STATUS_OK) {
      rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to copy history spool",
                                json_error.message);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_history_append_bytes(history, "\n", 1U, error);
  }
  return rc;
}

static void cai_history_replace(cai_session *session,
                                lonejson_spooled *next_history) {
  lonejson_spooled old_history;

  old_history = CAI_SESSION_IMPL(session)->history;
  CAI_SESSION_IMPL(session)->history = *next_history;
  memset(next_history, 0, sizeof(*next_history));
  old_history.cleanup(&old_history);
}

static void cai_history_cleanup(cai_session *session) {
  if (session != NULL) {
    CAI_SESSION_IMPL(session)->history.cleanup(
        &CAI_SESSION_IMPL(session)->history);
  }
}

void cai_session_destroy(cai_session *session) {
  cai_allocator *allocator;
  cai_session_impl *impl;

  if (session == NULL) {
    return;
  }
  impl = CAI_SESSION_IMPL(session);
  if (impl == NULL) {
    return;
  }
  allocator = &CAI_SESSION_CLIENT_IMPL(session)->allocator;
  cai_session_clear_inputs(session);
  cai_history_cleanup(session);
  cai_free_mem(allocator, impl->inputs);
  cai_free_mem(allocator, impl->previous_response_id);
  cai_free_mem(allocator, impl->conversation_id);
  cai_free_mem(allocator, impl);
  session->impl = NULL;
  cai_free_mem(allocator, session);
}

int cai_session_set_conversation_id(cai_session *session,
                                    const char *conversation_id,
                                    cai_error *error) {
  char *copy;
  cai_allocator *allocator;

  if (session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "session is required");
  }
  allocator = &CAI_SESSION_CLIENT_IMPL(session)->allocator;
  copy = NULL;
  if (conversation_id != NULL) {
    if (conversation_id[0] == '\0') {
      return cai_set_error(error, CAI_ERR_INVALID,
                           "conversation id must not be empty");
    }
    copy = cai_strdup(allocator, conversation_id);
    if (copy == NULL) {
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate conversation id");
    }
  }
  cai_free_mem(allocator, CAI_SESSION_IMPL(session)->conversation_id);
  CAI_SESSION_IMPL(session)->conversation_id = copy;
  if (copy != NULL) {
    cai_free_mem(allocator, CAI_SESSION_IMPL(session)->previous_response_id);
    CAI_SESSION_IMPL(session)->previous_response_id = NULL;
  }
  return CAI_OK;
}

int cai_session_set_conversation(cai_session *session,
                                 const cai_conversation *conversation,
                                 cai_error *error) {
  if (conversation == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "conversation is required");
  }
  return cai_session_set_conversation_id(
      session, cai_conversation_id(conversation), error);
}

const char *cai_session_conversation_id(const cai_session *session) {
  return session != NULL ? CAI_SESSION_IMPL(session)->conversation_id : NULL;
}

int cai_session_set_previous_response_id(cai_session *session,
                                         const char *response_id,
                                         cai_error *error) {
  char *copy;
  cai_allocator *allocator;

  if (session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "session is required");
  }
  allocator = &CAI_SESSION_CLIENT_IMPL(session)->allocator;
  copy = NULL;
  if (response_id != NULL) {
    if (response_id[0] == '\0') {
      return cai_set_error(error, CAI_ERR_INVALID,
                           "previous response id must not be empty");
    }
    copy = cai_strdup(allocator, response_id);
    if (copy == NULL) {
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate previous response id");
    }
  }
  cai_free_mem(allocator, CAI_SESSION_IMPL(session)->previous_response_id);
  CAI_SESSION_IMPL(session)->previous_response_id = copy;
  if (copy != NULL) {
    cai_free_mem(allocator, CAI_SESSION_IMPL(session)->conversation_id);
    CAI_SESSION_IMPL(session)->conversation_id = NULL;
  }
  return CAI_OK;
}

const char *cai_session_previous_response_id(const cai_session *session) {
  return session != NULL ? CAI_SESSION_IMPL(session)->previous_response_id
                         : NULL;
}

static int cai_session_grow_inputs(cai_session *session, cai_error *error) {
  size_t new_capacity;
  void *grown;

  if (CAI_SESSION_IMPL(session)->input_count <
      CAI_SESSION_IMPL(session)->input_capacity) {
    return CAI_OK;
  }
  new_capacity = CAI_SESSION_IMPL(session)->input_capacity == 0U
                     ? 2U
                     : CAI_SESSION_IMPL(session)->input_capacity * 2U;
  grown = cai_realloc_mem(&CAI_SESSION_CLIENT_IMPL(session)->allocator,
                          CAI_SESSION_IMPL(session)->inputs,
                          new_capacity *
                              sizeof(CAI_SESSION_IMPL(session)->inputs[0]));
  if (grown == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to grow session input list");
  }
  CAI_SESSION_IMPL(session)->inputs = (cai_session_input *)grown;
  CAI_SESSION_IMPL(session)->input_capacity = new_capacity;
  return CAI_OK;
}

static int cai_session_add_input(cai_session *session, int kind,
                                 const char *role, const char *text,
                                 const char *image_url, const char *detail,
                                 const char *call_id, const char *output,
                                 cai_error *error) {
  cai_session_input *input;
  cai_allocator *allocator;
  int rc;

  if (session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "session is required");
  }
  if (kind != CAI_SESSION_INPUT_FUNCTION_CALL_OUTPUT &&
      (role == NULL || role[0] == '\0')) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session and role are required");
  }
  rc = cai_session_grow_inputs(session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  allocator = &CAI_SESSION_CLIENT_IMPL(session)->allocator;
  input = &CAI_SESSION_IMPL(session)
               ->inputs[CAI_SESSION_IMPL(session)->input_count];
  memset(input, 0, sizeof(*input));
  input->kind = kind;
  input->role = cai_strdup(allocator, role);
  input->text = cai_strdup(allocator, text);
  input->image_url = cai_strdup(allocator, image_url);
  input->detail = cai_strdup(allocator, detail);
  input->call_id = cai_strdup(allocator, call_id);
  input->output = cai_strdup(allocator, output);
  if ((role != NULL && input->role == NULL) ||
      (text != NULL && input->text == NULL) ||
      (image_url != NULL && input->image_url == NULL) ||
      (detail != NULL && input->detail == NULL) ||
      (call_id != NULL && input->call_id == NULL) ||
      (output != NULL && input->output == NULL)) {
    cai_free_mem(allocator, input->role);
    cai_free_mem(allocator, input->text);
    cai_free_mem(allocator, input->image_url);
    cai_free_mem(allocator, input->detail);
    cai_free_mem(allocator, input->call_id);
    cai_free_mem(allocator, input->output);
    memset(input, 0, sizeof(*input));
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate session input");
  }
  CAI_SESSION_IMPL(session)->input_count++;
  return CAI_OK;
}

static const char *cai_session_basename(const char *path) {
  const char *slash;

  if (path == NULL) {
    return NULL;
  }
  slash = strrchr(path, '/');
  return slash != NULL && slash[1] != '\0' ? slash + 1 : path;
}

static int cai_session_source_to_spooled(cai_session *session,
                                         cai_source *source,
                                         lonejson_spooled *out,
                                         cai_error *error) {
  lonejson_error json_error;
  unsigned char buffer[4096];
  size_t nread;
  int previous_error_code;

  if (source == NULL || out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "source and spool output are required");
  }
  memset(out, 0, sizeof(*out));
  cai_history_init_spooled(session, out);
  for (;;) {
    previous_error_code = error != NULL ? error->code : CAI_OK;
    nread = cai_source_read(source, buffer, sizeof(buffer), error);
    if (nread == 0U && error != NULL && error->code != previous_error_code &&
        error->code != CAI_OK) {
      out->cleanup(out);
      return error->code;
    }
    if (nread == 0U) {
      break;
    }
    lonejson_error_init(&json_error);
    if (out->append(out, buffer, nread, &json_error) != LONEJSON_STATUS_OK) {
      out->cleanup(out);
      return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                  "failed to spool file input",
                                  json_error.message);
    }
  }
  return CAI_OK;
}

static int cai_session_add_file_input_spooled(
    cai_session *session, const char *role, const char *filename,
    lonejson_spooled *file_data, const char *detail, cai_error *error) {
  cai_session_input *input;
  cai_allocator *allocator;
  int rc;

  if (session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "session is required");
  }
  if (role == NULL || role[0] == '\0' || file_data == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "role and file data spool are required");
  }
  rc = cai_session_grow_inputs(session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  allocator = &CAI_SESSION_CLIENT_IMPL(session)->allocator;
  input = &CAI_SESSION_IMPL(session)
               ->inputs[CAI_SESSION_IMPL(session)->input_count];
  memset(input, 0, sizeof(*input));
  input->kind = CAI_SESSION_INPUT_FILE_DATA;
  input->role = cai_strdup(allocator, role);
  input->filename = cai_strdup(allocator, filename);
  input->detail = cai_strdup(allocator, detail);
  if (input->role == NULL || (filename != NULL && input->filename == NULL) ||
      (detail != NULL && input->detail == NULL)) {
    cai_free_mem(allocator, input->role);
    cai_free_mem(allocator, input->filename);
    cai_free_mem(allocator, input->detail);
    memset(input, 0, sizeof(*input));
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate session file input");
  }
  input->file_data = *file_data;
  input->has_file_data = 1;
  memset(file_data, 0, sizeof(*file_data));
  CAI_SESSION_IMPL(session)->input_count++;
  return CAI_OK;
}

static int cai_session_add_text_input_spooled(cai_session *session,
                                              const char *role,
                                              lonejson_spooled *text,
                                              cai_error *error) {
  cai_session_input *input;
  cai_allocator *allocator;
  int rc;

  if (session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "session is required");
  }
  if (role == NULL || role[0] == '\0' || text == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "role and text spool are required");
  }
  rc = cai_session_grow_inputs(session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  allocator = &CAI_SESSION_CLIENT_IMPL(session)->allocator;
  input = &CAI_SESSION_IMPL(session)
               ->inputs[CAI_SESSION_IMPL(session)->input_count];
  memset(input, 0, sizeof(*input));
  input->kind = CAI_SESSION_INPUT_TEXT;
  input->role = cai_strdup(allocator, role);
  if (input->role == NULL) {
    memset(input, 0, sizeof(*input));
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate session text input");
  }
  input->text_spooled = *text;
  input->has_text_spooled = 1;
  memset(text, 0, sizeof(*text));
  CAI_SESSION_IMPL(session)->input_count++;
  return CAI_OK;
}

int cai_session_add_user_text(cai_session *session, const char *text,
                              cai_error *error) {
  if (text == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "text is required");
  }
  return cai_session_add_input(session, CAI_SESSION_INPUT_TEXT, "user", text,
                               NULL, NULL, NULL, NULL, error);
}

int cai_session_add_user_text_spooled(cai_session *session,
                                      lonejson_spooled *text,
                                      cai_error *error) {
  if (text == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "text spool is required");
  }
  return cai_session_add_text_input_spooled(session, "user", text, error);
}

int cai_session_add_user_text_source(cai_session *session, cai_source *source,
                                     cai_error *error) {
  lonejson_spooled text;
  int rc;

  if (session == NULL || source == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session and text source are required");
  }
  memset(&text, 0, sizeof(text));
  rc = cai_session_source_to_spooled(session, source, &text, error);
  if (rc == CAI_OK) {
    rc = cai_session_add_user_text_spooled(session, &text, error);
  }
  if (rc != CAI_OK) {
    text.cleanup(&text);
  }
  return rc;
}

int cai_session_add_user_image_url(cai_session *session, const char *url,
                                   const char *detail, cai_error *error) {
  if (url == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "image URL is required");
  }
  return cai_session_add_input(session, CAI_SESSION_INPUT_IMAGE, "user", NULL,
                               url, detail, NULL, NULL, error);
}

int cai_session_add_user_file_data_spooled(cai_session *session,
                                           const char *filename,
                                           lonejson_spooled *file_data,
                                           const char *detail,
                                           cai_error *error) {
  if (file_data == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "file data spool is required");
  }
  return cai_session_add_file_input_spooled(session, "user", filename,
                                            file_data, detail, error);
}

int cai_session_add_user_file_source(cai_session *session, const char *filename,
                                     cai_source *source, const char *detail,
                                     cai_error *error) {
  lonejson_spooled file_data;
  int rc;

  if (session == NULL || source == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session and file source are required");
  }
  memset(&file_data, 0, sizeof(file_data));
  rc = cai_session_source_to_spooled(session, source, &file_data, error);
  if (rc == CAI_OK) {
    rc = cai_session_add_user_file_data_spooled(session, filename, &file_data,
                                                detail, error);
  }
  if (rc != CAI_OK) {
    file_data.cleanup(&file_data);
  }
  return rc;
}

int cai_session_add_user_file_path(cai_session *session, const char *path,
                                   const char *filename, const char *detail,
                                   cai_error *error) {
  cai_source *source;
  FILE *fp;
  int rc;

  if (session == NULL || path == NULL || path[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session and file path are required");
  }
  source = NULL;
  fp = fopen(path, "rb");
  if (fp == NULL) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to open user file input path");
  }
  rc = cai_source_file(fp, 1, &source, error);
  if (rc == CAI_OK) {
    fp = NULL;
    rc = cai_session_add_user_file_source(
        session, filename != NULL ? filename : cai_session_basename(path),
        source, detail, error);
  }
  if (fp != NULL) {
    fclose(fp);
  }
  cai_source_close(source);
  return rc;
}

int cai_session_add_function_call_output(cai_session *session,
                                         const char *call_id,
                                         const char *output, cai_error *error) {
  if (call_id == NULL || call_id[0] == '\0' || output == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "function call id and output are required");
  }
  return cai_session_add_input(session, CAI_SESSION_INPUT_FUNCTION_CALL_OUTPUT,
                               NULL, NULL, NULL, NULL, call_id, output, error);
}

static int cai_session_add_pending_inputs(cai_session *session,
                                          cai_response_create_params *params,
                                          cai_error *error) {
  lonejson_spooled file_data;
  lonejson_spooled text;
  size_t i;
  int rc;

  rc = CAI_OK;
  for (i = 0U; rc == CAI_OK && i < CAI_SESSION_IMPL(session)->input_count;
       i++) {
    if (CAI_SESSION_IMPL(session)->inputs[i].kind == CAI_SESSION_INPUT_IMAGE) {
      rc = cai_response_create_params_add_image_url(
          params, CAI_SESSION_IMPL(session)->inputs[i].role,
          CAI_SESSION_IMPL(session)->inputs[i].image_url,
          CAI_SESSION_IMPL(session)->inputs[i].detail, error);
    } else if (CAI_SESSION_IMPL(session)->inputs[i].kind ==
               CAI_SESSION_INPUT_FILE_DATA) {
      memset(&file_data, 0, sizeof(file_data));
      rc = cai_agent_clone_spooled(
          session, &CAI_SESSION_IMPL(session)->inputs[i].file_data, &file_data,
          error);
      if (rc == CAI_OK) {
        rc = cai_response_create_params_add_file_data_spooled(
            params, CAI_SESSION_IMPL(session)->inputs[i].role,
            CAI_SESSION_IMPL(session)->inputs[i].filename, &file_data,
            CAI_SESSION_IMPL(session)->inputs[i].detail, error);
      }
      if (rc != CAI_OK) {
        file_data.cleanup(&file_data);
      }
    } else if (CAI_SESSION_IMPL(session)->inputs[i].kind ==
               CAI_SESSION_INPUT_FUNCTION_CALL_OUTPUT) {
      rc = cai_response_create_params_add_function_call_output(
          params, CAI_SESSION_IMPL(session)->inputs[i].call_id,
          CAI_SESSION_IMPL(session)->inputs[i].output, error);
    } else if (CAI_SESSION_IMPL(session)->inputs[i].has_text_spooled) {
      memset(&text, 0, sizeof(text));
      rc = cai_agent_clone_spooled(
          session, &CAI_SESSION_IMPL(session)->inputs[i].text_spooled, &text,
          error);
      if (rc == CAI_OK) {
        rc = cai_response_create_params_add_text_spooled(
            params, CAI_SESSION_IMPL(session)->inputs[i].role, &text, error);
      }
      if (rc != CAI_OK) {
        text.cleanup(&text);
      }
    } else {
      rc = cai_response_create_params_add_text(
          params, CAI_SESSION_IMPL(session)->inputs[i].role,
          CAI_SESSION_IMPL(session)->inputs[i].text, error);
    }
  }
  return rc;
}

static int cai_spooled_record_reader_next(cai_spooled_record_reader *reader,
                                          unsigned char *out,
                                          cai_error *error) {
  lonejson_read_result chunk;

  if (reader->offset >= reader->length) {
    if (reader->eof) {
      return 0;
    }
    chunk = reader->cursor.read(&reader->cursor, reader->buffer,
                                sizeof(reader->buffer));
    if (chunk.error_code != 0) {
      (void)cai_set_error(error, CAI_ERR_TRANSPORT,
                          "failed to read history spool");
      return -1;
    }
    reader->offset = 0U;
    reader->length = chunk.bytes_read;
    reader->eof = chunk.eof;
    if (reader->length == 0U) {
      return 0;
    }
  }
  *out = reader->buffer[reader->offset];
  reader->offset++;
  return 1;
}

static lonejson_read_result cai_history_record_json_read(void *user,
                                                         unsigned char *buffer,
                                                         size_t capacity) {
  cai_history_record_json_reader *reader;
  lonejson_read_result result;
  size_t want;
  size_t i;
  int rc;

  reader = (cai_history_record_json_reader *)user;
  result = lonejson_default_read_result();
  if (reader == NULL || buffer == NULL) {
    result.error_code = CAI_ERR_INVALID;
    return result;
  }
  if (reader->remaining == 0UL) {
    result.eof = 1;
    return result;
  }
  want = capacity;
  if ((unsigned long)want > reader->remaining) {
    want = (size_t)reader->remaining;
  }
  for (i = 0U; i < want; i++) {
    rc = cai_spooled_record_reader_next(reader->records, &buffer[i],
                                        reader->error);
    if (rc <= 0) {
      result.error_code = rc < 0 ? (reader->error != NULL ? reader->error->code
                                                          : CAI_ERR_TRANSPORT)
                                 : CAI_ERR_PROTOCOL;
      return result;
    }
  }
  reader->remaining -= (unsigned long)want;
  result.bytes_read = want;
  result.eof = reader->remaining == 0UL ? 1 : 0;
  return result;
}

static int cai_history_read_record_length(cai_spooled_record_reader *reader,
                                          unsigned long *out_length,
                                          int *out_have_record,
                                          cai_error *error) {
  unsigned long item_length;
  unsigned char ch;
  int rc;

  if (out_length == NULL || out_have_record == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "history record output is required");
  }
  *out_length = 0UL;
  *out_have_record = 0;
  rc = cai_spooled_record_reader_next(reader, &ch, error);
  if (rc < 0) {
    return error != NULL ? error->code : CAI_ERR_TRANSPORT;
  }
  while (rc > 0 && (ch == '\n' || ch == '\r')) {
    rc = cai_spooled_record_reader_next(reader, &ch, error);
    if (rc < 0) {
      return error != NULL ? error->code : CAI_ERR_TRANSPORT;
    }
  }
  if (rc == 0) {
    return CAI_OK;
  }
  if (ch < '0' || ch > '9') {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "invalid history record length");
  }
  item_length = 0UL;
  do {
    if (item_length > (ULONG_MAX / 10UL)) {
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "history record length overflow");
    }
    item_length = item_length * 10UL + (unsigned long)(ch - '0');
    rc = cai_spooled_record_reader_next(reader, &ch, error);
    if (rc <= 0) {
      return rc < 0 ? (error != NULL ? error->code : CAI_ERR_TRANSPORT)
                    : cai_set_error(error, CAI_ERR_PROTOCOL,
                                    "truncated history record length");
    }
  } while (ch >= '0' && ch <= '9');
  if (ch != '\n') {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "invalid history record length");
  }
  *out_length = item_length;
  *out_have_record = 1;
  return CAI_OK;
}

static int cai_history_to_array_spool(cai_session *session,
                                      lonejson_spooled *out, cai_error *error) {
  cai_spooled_record_reader reader;
  cai_history_record_json_reader record_reader;
  cai_history_sink_context sink_context;
  lonejson_writer writer;
  lonejson_error json_error;
  unsigned long item_length;
  int have_record;
  int rc;
  lonejson_status status;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "history array spool output pointer is required");
  }
  cai_history_init_spooled(session, out);
  sink_context.spool = out;
  lonejson_error_init(&json_error);
  status = CAI_LJ->writer_init_sink(CAI_LJ, &writer, cai_history_lonejson_sink,
                                    &sink_context, &json_error);
  if (status == LONEJSON_STATUS_OK) {
    status = writer.begin_array(&writer, &json_error);
  }
  reader.cursor = CAI_SESSION_IMPL(session)->history;
  reader.offset = 0U;
  reader.length = 0U;
  reader.eof = 0;
  if (status == LONEJSON_STATUS_OK &&
      reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    status = LONEJSON_STATUS_CALLBACK_FAILED;
  }
  while (status == LONEJSON_STATUS_OK) {
    rc = cai_history_read_record_length(&reader, &item_length, &have_record,
                                        error);
    if (rc != CAI_OK) {
      status = LONEJSON_STATUS_CALLBACK_FAILED;
      break;
    }
    if (!have_record) {
      break;
    }
    memset(&record_reader, 0, sizeof(record_reader));
    record_reader.records = &reader;
    record_reader.remaining = item_length;
    record_reader.error = error;
    status = writer.array_items_reader(
        &writer, "", cai_history_record_json_read, &record_reader, &json_error);
    if (status == LONEJSON_STATUS_OK && record_reader.remaining != 0UL) {
      status = LONEJSON_STATUS_CALLBACK_FAILED;
    }
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.end_array(&writer, &json_error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = writer.finish(&writer, &json_error);
  }
  writer.cleanup(&writer);
  if (status != LONEJSON_STATUS_OK) {
    out->cleanup(out);
    if (error != NULL && error->message != NULL) {
      return error->code;
    }
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to build history JSON array",
                                json_error.message);
  }
  return CAI_OK;
}

static int cai_session_prepare_history_params(
    cai_session *session, cai_response_create_params *params,
    lonejson_spooled *out_pending_items, int *out_has_pending_items,
    cai_error *error) {
  lonejson_spooled history_items;
  lonejson_spooled pending_items;
  int has_history_items;
  int has_pending_items;
  int rc;

  if (out_has_pending_items != NULL) {
    *out_has_pending_items = 0;
  }
  if (CAI_SESSION_AGENT_IMPL(session)->session_continuity !=
          CAI_SESSION_CONTINUITY_CLIENT_HISTORY &&
      !CAI_SESSION_AGENT_IMPL(session)->local_history_enabled) {
    return cai_session_add_pending_inputs(session, params, error);
  }
  if (CAI_SESSION_AGENT_IMPL(session)->session_continuity ==
      CAI_SESSION_CONTINUITY_CLIENT_HISTORY) {
    memset(&history_items, 0, sizeof(history_items));
    memset(&pending_items, 0, sizeof(pending_items));
    has_history_items = 0;
    has_pending_items = 0;
    rc = cai_history_to_array_spool(session, &history_items, error);
    if (rc == CAI_OK) {
      has_history_items = 1;
      rc = cai_session_add_pending_inputs(session, params, error);
    }
    if (rc == CAI_OK && params->input.count > 0U) {
      rc = cai_response_params_input_items_spool(params, &pending_items, NULL,
                                                 error);
      if (rc == CAI_OK) {
        has_pending_items = 1;
      }
    }
    if (rc == CAI_OK) {
      rc = cai_response_create_params_set_raw_input_spooled(
          params, &history_items, error);
      if (rc == CAI_OK) {
        has_history_items = 0;
      }
    }
    if (out_pending_items != NULL && out_has_pending_items != NULL &&
        rc == CAI_OK) {
      *out_pending_items = pending_items;
      *out_has_pending_items = has_pending_items;
      has_pending_items = 0;
    }
    if (has_pending_items) {
      pending_items.cleanup(&pending_items);
    }
    if (has_history_items) {
      history_items.cleanup(&history_items);
    }
    return rc;
  }
  memset(&pending_items, 0, sizeof(pending_items));
  has_pending_items = 0;
  rc = cai_session_add_pending_inputs(session, params, error);
  if (rc == CAI_OK) {
    rc = cai_response_params_input_items_spool(params, &pending_items, NULL,
                                               error);
    if (rc == CAI_OK) {
      has_pending_items = 1;
    }
  }
  if (out_pending_items != NULL && out_has_pending_items != NULL &&
      rc == CAI_OK) {
    *out_pending_items = pending_items;
    *out_has_pending_items = has_pending_items;
    has_pending_items = 0;
  }
  if (has_pending_items) {
    pending_items.cleanup(&pending_items);
  }
  return rc;
}

static int cai_session_replay_history_with_params_input(
    cai_session *session, cai_response_create_params *params,
    lonejson_spooled *out_pending_items, int *out_has_pending_items,
    cai_error *error) {
  lonejson_spooled history_items;
  lonejson_spooled pending_items;
  int has_history_items;
  int has_pending_items;
  int rc;

  if (out_has_pending_items != NULL) {
    *out_has_pending_items = 0;
  }
  if (CAI_SESSION_AGENT_IMPL(session)->session_continuity !=
      CAI_SESSION_CONTINUITY_CLIENT_HISTORY) {
    return CAI_OK;
  }
  memset(&history_items, 0, sizeof(history_items));
  memset(&pending_items, 0, sizeof(pending_items));
  has_history_items = 0;
  has_pending_items = 0;
  rc = cai_history_to_array_spool(session, &history_items, error);
  if (rc == CAI_OK) {
    has_history_items = 1;
  }
  if (rc == CAI_OK && params->input.count > 0U) {
    rc = cai_response_params_input_items_spool(params, &pending_items, NULL,
                                               error);
    if (rc == CAI_OK) {
      has_pending_items = 1;
    }
  }
  if (rc == CAI_OK) {
    rc = cai_response_create_params_set_raw_input_spooled(
        params, &history_items, error);
    if (rc == CAI_OK) {
      has_history_items = 0;
    }
  }
  if (out_pending_items != NULL && out_has_pending_items != NULL &&
      rc == CAI_OK) {
    *out_pending_items = pending_items;
    *out_has_pending_items = has_pending_items;
    has_pending_items = 0;
  }
  if (has_pending_items) {
    pending_items.cleanup(&pending_items);
  }
  if (has_history_items) {
    history_items.cleanup(&history_items);
  }
  return rc;
}

static int cai_session_remember_response(cai_session *session,
                                         const cai_response *response,
                                         cai_error *error) {
  int rc;
  cai_token_usage usage;

  rc = cai_session_remember_response_id(session, cai_response_id(response),
                                        error);
  if (rc == CAI_OK && response != NULL) {
    memset(&usage, 0, sizeof(usage));
    usage.input_tokens = cai_response_input_tokens(response);
    usage.input_cached_tokens = cai_response_input_cached_tokens(response);
    usage.output_tokens = cai_response_output_tokens(response);
    usage.output_reasoning_tokens =
        cai_response_output_reasoning_tokens(response);
    usage.total_tokens = cai_response_total_tokens(response);
    rc = cai_session_record_usage(session, &usage, error);
  }
  return rc;
}

static int cai_session_set_last_usage(cai_session *session,
                                      const cai_token_usage *usage,
                                      cai_error *error) {
  if (session == NULL || usage == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session and usage are required");
  }
  CAI_SESSION_IMPL(session)->last_usage = *usage;
  CAI_SESSION_IMPL(session)->has_last_usage = 1;
  return CAI_OK;
}

static int cai_session_record_usage(cai_session *session,
                                    const cai_token_usage *usage,
                                    cai_error *error) {
  cai_client_impl *client_impl;
  cai_agent_impl *agent_impl;
  double estimated_spend_usd;

  if (session == NULL || usage == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session and usage are required");
  }
  if (cai_token_usage_is_empty(usage) &&
      (cai_usage_limits_active(&CAI_SESSION_IMPL(session)->usage_limits) ||
       cai_usage_limits_active(
           &CAI_SESSION_CLIENT_IMPL(session)->usage_limits))) {
    return cai_set_error(
        error, CAI_ERR_PROTOCOL,
        "response usage is required when usage limits are enabled");
  }
  if (cai_token_usage_is_empty(usage)) {
    return CAI_OK;
  }
  agent_impl = CAI_SESSION_AGENT_IMPL(session);
  client_impl = CAI_SESSION_CLIENT_IMPL(session);
  if (cai_usage_limits_require_pricing(&CAI_SESSION_IMPL(session)->usage_limits,
                                       agent_impl->model, error) != CAI_OK ||
      cai_usage_limits_require_pricing(&client_impl->usage_limits,
                                       agent_impl->model, error) != CAI_OK) {
    return CAI_ERR_INVALID;
  }
  estimated_spend_usd = cai_model_estimate_usage_usd(
      agent_impl->model, usage->input_tokens, usage->input_cached_tokens,
      usage->output_tokens);
  cai_session_set_last_usage(session, usage, NULL);
  cai_usage_accounting_add(&CAI_SESSION_IMPL(session)->usage, usage,
                           estimated_spend_usd);
  cai_usage_accounting_add(&client_impl->usage, usage, estimated_spend_usd);
  if (cai_usage_accounting_exceeds(&CAI_SESSION_IMPL(session)->usage,
                                   &CAI_SESSION_IMPL(session)->usage_limits)) {
    CAI_SESSION_IMPL(session)->usage.limit_exceeded = 1;
  }
  if (cai_usage_accounting_exceeds(&client_impl->usage,
                                   &client_impl->usage_limits)) {
    client_impl->usage.limit_exceeded = 1;
  }
  if (CAI_SESSION_IMPL(session)->usage.limit_exceeded ||
      client_impl->usage.limit_exceeded) {
    return cai_usage_limits_error(error);
  }
  return CAI_OK;
}

static int cai_session_check_usage_available(cai_session *session,
                                             cai_error *error) {
  cai_agent_impl *agent_impl;
  int rc;

  if (session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "session is required");
  }
  agent_impl = CAI_SESSION_AGENT_IMPL(session);
  rc = cai_usage_limits_require_pricing(
      &CAI_SESSION_IMPL(session)->usage_limits, agent_impl->model, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_usage_limits_require_pricing(
      &CAI_SESSION_CLIENT_IMPL(session)->usage_limits, agent_impl->model,
      error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_usage_limits_preflight(&CAI_SESSION_IMPL(session)->usage,
                                  &CAI_SESSION_IMPL(session)->usage_limits,
                                  error);
  if (rc != CAI_OK) {
    CAI_SESSION_IMPL(session)->usage.limit_exceeded = 1;
    return rc;
  }
  rc = cai_usage_limits_preflight(
      &CAI_SESSION_CLIENT_IMPL(session)->usage,
      &CAI_SESSION_CLIENT_IMPL(session)->usage_limits, error);
  if (rc != CAI_OK) {
    CAI_SESSION_CLIENT_IMPL(session)->usage.limit_exceeded = 1;
  }
  return rc;
}

static int cai_session_usage_limits_enabled(cai_session *session) {
  return session != NULL &&
         (cai_usage_limits_active(&CAI_SESSION_IMPL(session)->usage_limits) ||
          cai_usage_limits_active(
              &CAI_SESSION_CLIENT_IMPL(session)->usage_limits));
}

static int cai_token_usage_is_empty(const cai_token_usage *usage) {
  if (usage == NULL) {
    return 1;
  }
  return usage->input_tokens == 0LL && usage->input_cached_tokens == 0LL &&
         usage->output_tokens == 0LL && usage->output_reasoning_tokens == 0LL &&
         usage->total_tokens == 0LL;
}

int cai_session_compact_experimental(cai_session *session, cai_error *error) {
  cai_response_create_params *params;
  cai_response *response;
  lonejson_spooled history_items;
  lonejson_spooled next_history;
  size_t history_len;
  int has_history_items;
  int has_next_history;
  lonejson_spooled output_items;
  size_t output_items_len;
  int has_output_items;
  char *body;
  char *request_id;
  long http_status;
  int rc;

  params = NULL;
  response = NULL;
  memset(&history_items, 0, sizeof(history_items));
  memset(&next_history, 0, sizeof(next_history));
  history_len = 0U;
  has_history_items = 0;
  has_next_history = 0;
  memset(&output_items, 0, sizeof(output_items));
  output_items_len = 0U;
  has_output_items = 0;
  body = NULL;
  request_id = NULL;
  if (session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "session is required");
  }
  if (!CAI_SESSION_AGENT_IMPL(session)->local_history_enabled) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "local history capture is disabled");
  }
  rc = cai_history_to_array_spool(session, &history_items, error);
  if (rc == CAI_OK) {
    has_history_items = 1;
    history_len = history_items.size_fn(&history_items);
  }
  if (rc == CAI_OK && history_len == 0U) {
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "session has no local history to compact");
    goto done;
  }
  if (rc == CAI_OK) {
    rc = cai_response_create_params_new(&params, error);
  }
  if (rc == CAI_OK) {
    rc = cai_response_create_params_set_model(
        params, CAI_SESSION_AGENT_IMPL(session)->model, error);
  }
  if (rc == CAI_OK &&
      CAI_SESSION_AGENT_IMPL(session)->developer_instructions != NULL) {
    rc = cai_response_create_params_set_instructions(
        params, CAI_SESSION_AGENT_IMPL(session)->developer_instructions, error);
  }
  if (rc == CAI_OK) {
    rc = cai_response_create_params_set_raw_input_spooled(
        params, &history_items, error);
    if (rc == CAI_OK) {
      has_history_items = 0;
    }
  }
  if (rc == CAI_OK) {
    rc = cai_session_check_usage_available(session, error);
  }
  if (rc == CAI_OK) {
    rc = cai_http_response_params_request(
        CAI_SESSION_AGENT_IMPL(session)->client, "responses/compact", params, 0,
        &body, &http_status, &request_id, error);
  }
  if (rc == CAI_OK && (http_status < 200L || http_status >= 300L)) {
    rc = cai_set_openai_error(error, http_status, body, request_id);
  }
  if (rc == CAI_OK) {
    rc = cai_response_parse_json_with_allocator(
        &CAI_SESSION_CLIENT_IMPL(session)->allocator, body != NULL ? body : "",
        &response, error);
  }
  if (rc == CAI_OK) {
    rc = cai_response_output_items_spool(response, &output_items,
                                         &output_items_len, error);
    if (rc == CAI_OK) {
      has_output_items = 1;
    }
  }
  if (rc == CAI_OK) {
    cai_history_init_spooled(session, &next_history);
    has_next_history = 1;
    if (output_items_len > 0U) {
      rc = cai_history_append_array_record_to_spool(session, &next_history,
                                                    &output_items, error);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_session_remember_response(session, response, error);
  }
  if (rc == CAI_OK) {
    cai_history_replace(session, &next_history);
    has_next_history = 0;
  }

done:
  cai_response_create_params_destroy(params);
  cai_response_destroy(response);
  if (has_history_items) {
    history_items.cleanup(&history_items);
  }
  if (has_next_history) {
    next_history.cleanup(&next_history);
  }
  cai_free_mem(NULL, body);
  cai_free_mem(NULL, request_id);
  if (has_output_items) {
    output_items.cleanup(&output_items);
  }
  return rc;
}

static int cai_session_after_response(cai_session *session,
                                      const lonejson_spooled *pending_items,
                                      int has_pending_items,
                                      const cai_response *response,
                                      cai_error *error) {
  lonejson_spooled output_items;
  size_t output_items_len;
  int has_output_items;
  int rc;

  rc = cai_session_remember_response(session, response, error);
  if (rc != CAI_OK || !CAI_SESSION_AGENT_IMPL(session)->local_history_enabled) {
    return rc;
  }
  memset(&output_items, 0, sizeof(output_items));
  output_items_len = 0U;
  has_output_items = 0;
  rc = cai_response_output_items_spool(response, &output_items,
                                       &output_items_len, error);
  if (rc == CAI_OK) {
    has_output_items = 1;
  }
  if (rc == CAI_OK && has_pending_items) {
    rc = cai_history_append_array_record_spooled(session, pending_items, error);
  }
  if (rc == CAI_OK && output_items_len > 0U) {
    rc = cai_history_append_array_record_spooled(session, &output_items, error);
  }
  if (has_output_items) {
    output_items.cleanup(&output_items);
  }
  return rc;
}

static int cai_session_after_stream(cai_session *session,
                                    const lonejson_spooled *pending_items,
                                    int has_pending_items,
                                    const char *response_id,
                                    const cai_token_usage *usage,
                                    cai_error *error) {
  cai_response *response;
  cai_error retrieve_error;
  lonejson_spooled output_items;
  size_t output_items_len;
  int has_output_items;
  int rc;

  response = NULL;
  memset(&output_items, 0, sizeof(output_items));
  output_items_len = 0U;
  has_output_items = 0;
  if (!CAI_SESSION_AGENT_IMPL(session)->local_history_enabled) {
    return cai_session_remember_stream(session, response_id, usage, error);
  }
  rc = cai_session_remember_response_id(session, response_id, error);
  if (rc == CAI_OK && !cai_token_usage_is_empty(usage)) {
    rc = cai_session_record_usage(session, usage, error);
  }
  if (rc == CAI_OK) {
    cai_error_init(&retrieve_error);
    rc = cai_client_retrieve_response(CAI_SESSION_AGENT_IMPL(session)->client,
                                      response_id, &response, &retrieve_error);
    if (rc != CAI_OK) {
      cai_error_cleanup(&retrieve_error);
      if (cai_token_usage_is_empty(usage) &&
          cai_session_usage_limits_enabled(session)) {
        rc = cai_set_error(
            error, CAI_ERR_PROTOCOL,
            "response usage is required when usage limits are enabled");
        goto done;
      }
      rc = CAI_OK;
      goto done;
    }
    cai_error_cleanup(&retrieve_error);
  }
  if (rc == CAI_OK && cai_token_usage_is_empty(usage)) {
    cai_token_usage retrieved_usage;

    memset(&retrieved_usage, 0, sizeof(retrieved_usage));
    retrieved_usage.input_tokens = cai_response_input_tokens(response);
    retrieved_usage.input_cached_tokens =
        cai_response_input_cached_tokens(response);
    retrieved_usage.output_tokens = cai_response_output_tokens(response);
    retrieved_usage.output_reasoning_tokens =
        cai_response_output_reasoning_tokens(response);
    retrieved_usage.total_tokens = cai_response_total_tokens(response);
    rc = cai_session_record_usage(session, &retrieved_usage, error);
  }
  if (rc == CAI_OK) {
    rc = cai_response_output_items_spool(response, &output_items,
                                         &output_items_len, error);
    if (rc == CAI_OK) {
      has_output_items = 1;
    }
  }
  if (rc == CAI_OK && has_pending_items) {
    rc = cai_history_append_array_record_spooled(session, pending_items, error);
  }
  if (rc == CAI_OK && output_items_len > 0U) {
    rc = cai_history_append_array_record_spooled(session, &output_items, error);
  }
done:
  cai_response_destroy(response);
  if (has_output_items) {
    output_items.cleanup(&output_items);
  }
  return rc;
}

static int cai_session_remember_response_id(cai_session *session,
                                            const char *response_id,
                                            cai_error *error) {
  char *next_response_id;

  if (response_id == NULL || response_id[0] == '\0') {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "streaming response did not include a response id");
  }
  next_response_id =
      cai_strdup(&CAI_SESSION_CLIENT_IMPL(session)->allocator, response_id);
  if (next_response_id == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to remember previous response id");
  }
  cai_free_mem(&CAI_SESSION_CLIENT_IMPL(session)->allocator,
               CAI_SESSION_IMPL(session)->previous_response_id);
  CAI_SESSION_IMPL(session)->previous_response_id = next_response_id;
  return CAI_OK;
}

static int cai_session_remember_stream(cai_session *session,
                                       const char *response_id,
                                       const cai_token_usage *usage,
                                       cai_error *error) {
  cai_response *response;
  cai_error retrieve_error;
  int rc;

  rc = cai_session_remember_response_id(session, response_id, error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (!cai_token_usage_is_empty(usage)) {
    return cai_session_record_usage(session, usage, error);
  }
  response = NULL;
  cai_error_init(&retrieve_error);
  rc = cai_client_retrieve_response(CAI_SESSION_AGENT_IMPL(session)->client,
                                    response_id, &response, &retrieve_error);
  if (rc != CAI_OK) {
    cai_error_cleanup(&retrieve_error);
    if (cai_session_usage_limits_enabled(session)) {
      return cai_set_error(
          error, CAI_ERR_PROTOCOL,
          "response usage is required when usage limits are enabled");
    }
    return CAI_OK;
  }
  cai_error_cleanup(&retrieve_error);
  if (rc == CAI_OK) {
    rc = cai_session_remember_response(session, response, error);
  }
  cai_response_destroy(response);
  if (rc != CAI_OK) {
    CAI_SESSION_IMPL(session)->has_last_usage = 0;
  }
  return rc;
}

static int cai_session_stream_complete(void *context, const char *response_id,
                                       const cai_token_usage *usage) {
  return cai_session_remember_stream((cai_session *)context, response_id, usage,
                                     NULL);
}

static int cai_session_create_response_from_params(
    cai_session *session, cai_response_create_params *params,
    const lonejson_spooled *pending_items, int has_pending_items,
    cai_response **out, cai_error *error) {
  cai_response *response;
  int rc;

  response = NULL;
  rc = cai_session_check_usage_available(session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_client_create_response(CAI_SESSION_AGENT_IMPL(session)->client,
                                  params, &response, error);
  if (rc != CAI_OK) {
    cai_response_destroy(response);
    return rc;
  }
  rc = cai_session_after_response(session, pending_items, has_pending_items,
                                  response, error);
  if (rc != CAI_OK) {
    if (rc == CAI_ERR_LIMIT) {
      cai_session_clear_inputs(session);
    }
    cai_response_destroy(response);
    return rc;
  }
  *out = response;
  return CAI_OK;
}

int cai_session_run(cai_session *session, cai_response **out,
                    cai_error *error) {
  cai_response_create_params *params;
  lonejson_spooled pending_items;
  int has_pending_items;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response output pointer is required");
  }
  *out = NULL;
  if (session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "session is required");
  }
  if (CAI_SESSION_IMPL(session)->input_count == 0U) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session has no pending input");
  }
  params = NULL;
  memset(&pending_items, 0, sizeof(pending_items));
  has_pending_items = 0;
  rc = cai_session_init_response_params(session, &params, error);
  if (rc == CAI_OK) {
    rc = cai_session_prepare_history_params(session, params, &pending_items,
                                            &has_pending_items, error);
  }
  if (rc == CAI_OK) {
    rc = cai_session_create_response_from_params(
        session, params, &pending_items, has_pending_items, out, error);
  }
  cai_response_create_params_destroy(params);
  if (has_pending_items) {
    pending_items.cleanup(&pending_items);
  }
  if (rc == CAI_OK) {
    cai_session_clear_inputs(session);
  }
  return rc;
}

int cai_session_run_output(cai_session *session, cai_output **out,
                           cai_error *error) {
  cai_response *response;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "output pointer is required");
  }
  *out = NULL;
  response = NULL;
  rc = cai_session_run(session, &response, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_output_from_response(response, out, error);
  if (rc != CAI_OK) {
    cai_response_destroy(response);
  }
  return rc;
}

static int cai_capture_tool_output(void *context, const void *bytes,
                                   size_t count, cai_error *error) {
  cai_tool_output_capture *capture;
  lonejson_error json_error;

  capture = (cai_tool_output_capture *)context;
  lonejson_error_init(&json_error);
  if (capture->output.append(&capture->output, bytes, count, &json_error) ==
      LONEJSON_STATUS_OK) {
    return CAI_OK;
  }
  return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                              "failed to spool tool output",
                              json_error.message);
}

static void cai_capture_cleanup(cai_tool_output_capture *capture) {
  if (capture == NULL) {
    return;
  }
  capture->output.cleanup(&capture->output);
}

static int cai_capture_tool_error_output(cai_tool_output_capture *capture,
                                         int tool_rc,
                                         const cai_error *tool_error,
                                         cai_error *error) {
  cai_buffer_builder builder;
  const char *message;
  int rc;

  if (capture == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "tool capture is required");
  }
  message = tool_error != NULL && tool_error->message != NULL
                ? tool_error->message
                : cai_status_string(tool_rc);
  if (capture->output.cleanup != NULL) {
    capture->output.cleanup(&capture->output);
  }
  CAI_LJ->spooled_init(CAI_LJ, &capture->output);
  memset(&builder, 0, sizeof(builder));
  rc = cai_buffer_append_cstr(&builder,
                              "{\"ok\":false,\"error\":{\"message\":", error);
  if (rc == CAI_OK) {
    rc = cai_buffer_append_json_string(&builder, message, error);
  }
  if (rc == CAI_OK) {
    rc = cai_buffer_append_cstr(&builder, ",\"code\":", error);
  }
  if (rc == CAI_OK) {
    rc = cai_buffer_append_json_string(&builder,
                                       tool_error != NULL &&
                                               tool_error->server_code != NULL
                                           ? tool_error->server_code
                                           : cai_status_string(tool_rc),
                                       error);
  }
  if (rc == CAI_OK) {
    rc = cai_buffer_append_cstr(&builder, "}}", error);
  }
  if (rc == CAI_OK) {
    rc = cai_capture_tool_output(capture, builder.data, builder.length, error);
  }
  cai_free_mem(NULL, builder.data);
  return rc;
}

static int cai_session_init_response_params(cai_session *session,
                                            cai_response_create_params **out,
                                            cai_error *error) {
  cai_response_create_params *params;
  int rc;

  params = NULL;
  rc = cai_response_create_params_new(&params, error);
  if (rc == CAI_OK) {
    rc = cai_response_create_params_set_model(
        params, CAI_SESSION_AGENT_IMPL(session)->model, error);
  }
  if (rc == CAI_OK &&
      CAI_SESSION_AGENT_IMPL(session)->developer_instructions != NULL) {
    rc = cai_response_create_params_set_instructions(
        params, CAI_SESSION_AGENT_IMPL(session)->developer_instructions, error);
  }
  if (rc == CAI_OK &&
      CAI_SESSION_AGENT_IMPL(session)->prompt_cache_key != NULL) {
    rc = cai_response_create_params_set_prompt_cache_key(
        params, CAI_SESSION_AGENT_IMPL(session)->prompt_cache_key, error);
  }
  if (rc == CAI_OK && CAI_SESSION_AGENT_IMPL(session)->tool_choice != NULL) {
    rc = cai_response_create_params_set_tool_choice(
        params, CAI_SESSION_AGENT_IMPL(session)->tool_choice, error);
  }
  if (rc == CAI_OK &&
      CAI_SESSION_AGENT_IMPL(session)->tool_choice_json != NULL) {
    rc = cai_response_create_params_set_tool_choice_json(
        params, CAI_SESSION_AGENT_IMPL(session)->tool_choice_json, error);
  }
  if (rc == CAI_OK && CAI_SESSION_AGENT_IMPL(session)->max_output_tokens > 0) {
    rc = cai_response_create_params_set_max_output_tokens(
        params, CAI_SESSION_AGENT_IMPL(session)->max_output_tokens, error);
  }
  if (rc == CAI_OK && CAI_SESSION_AGENT_IMPL(session)->max_tool_calls > 0) {
    rc = cai_response_create_params_set_max_tool_calls(
        params, CAI_SESSION_AGENT_IMPL(session)->max_tool_calls, error);
  }
  if (rc == CAI_OK &&
      (CAI_SESSION_AGENT_IMPL(session)->reasoning_effort != NULL ||
       CAI_SESSION_AGENT_IMPL(session)->reasoning_summary != NULL)) {
    rc = cai_response_create_params_set_reasoning(
        params, CAI_SESSION_AGENT_IMPL(session)->reasoning_effort,
        CAI_SESSION_AGENT_IMPL(session)->reasoning_summary, error);
  }
  if (rc == CAI_OK &&
      CAI_SESSION_AGENT_IMPL(session)->text_format_schema_json != NULL) {
    rc = cai_response_create_params_set_text_format_json_schema(
        params, CAI_SESSION_AGENT_IMPL(session)->text_format_name,
        CAI_SESSION_AGENT_IMPL(session)->text_format_description,
        CAI_SESSION_AGENT_IMPL(session)->text_format_schema_json,
        CAI_SESSION_AGENT_IMPL(session)->text_format_strict, error);
  }
  if (rc == CAI_OK &&
      CAI_SESSION_AGENT_IMPL(session)->parallel_tool_calls >= 0) {
    rc = cai_response_create_params_set_parallel_tool_calls(
        params, CAI_SESSION_AGENT_IMPL(session)->parallel_tool_calls, error);
  }
  if (rc == CAI_OK && CAI_SESSION_AGENT_IMPL(session)->auto_compact &&
      CAI_SESSION_AGENT_IMPL(session)->auto_compact_token_limit > 0LL) {
    rc = cai_response_create_params_set_compact_threshold(
        params, CAI_SESSION_AGENT_IMPL(session)->auto_compact_token_limit,
        error);
  }
  if (rc == CAI_OK && CAI_SESSION_IMPL(session)->conversation_id != NULL) {
    rc = cai_response_create_params_set_conversation_id(
        params, CAI_SESSION_IMPL(session)->conversation_id, error);
  }
  if (rc == CAI_OK &&
      CAI_SESSION_AGENT_IMPL(session)->session_continuity !=
          CAI_SESSION_CONTINUITY_CLIENT_HISTORY &&
      CAI_SESSION_IMPL(session)->conversation_id == NULL &&
      CAI_SESSION_IMPL(session)->previous_response_id != NULL) {
    rc = cai_response_create_params_set_previous_response_id(
        params, CAI_SESSION_IMPL(session)->previous_response_id, error);
  }
  if (rc == CAI_OK &&
      CAI_SESSION_AGENT_IMPL(session)->hosted_tools.count > 0U) {
    struct cai_function_tool *hosted_tools;
    size_t i;

    hosted_tools = (struct cai_function_tool *)CAI_SESSION_AGENT_IMPL(session)
                       ->hosted_tools.items;
    for (i = 0U; rc == CAI_OK &&
                 i < CAI_SESSION_AGENT_IMPL(session)->hosted_tools.count;
         i++) {
      rc = cai_response_create_params_add_hosted_tool_json(
          params, hosted_tools[i].raw_json, error);
    }
  }
  if (rc == CAI_OK) {
    rc = CAI_SESSION_AGENT_IMPL(session)->tools->add_to_response_params(
        CAI_SESSION_AGENT_IMPL(session)->tools, params, error);
  }
  if (rc != CAI_OK) {
    cai_response_create_params_destroy(params);
    return rc;
  }
  *out = params;
  return CAI_OK;
}

static int cai_session_clear_tool_choice_for_tool_continuation(
    cai_response_create_params *params, cai_error *error) {
  return cai_response_create_params_set_tool_choice(params, NULL, error);
}

static int cai_session_run_tool_round(cai_session *session,
                                      const cai_response *response,
                                      const cai_run_options *options,
                                      cai_response **out, cai_error *error) {
  cai_response_create_params *params;
  cai_sink_callbacks callbacks;
  cai_sink *sink;
  cai_tool_output_capture capture;
  lonejson *tool_output_runtime;
  lonejson_spooled pending_items;
  lonejson_spooled arguments_cursor;
  const lonejson_spooled *spooled_arguments;
  int has_pending_items;
  int capture_output_owned;
  size_t i;
  int rc;

  params = NULL;
  memset(&pending_items, 0, sizeof(pending_items));
  has_pending_items = 0;
  tool_output_runtime = NULL;
  rc = cai_session_init_response_params(session, &params, error);
  if (rc == CAI_OK) {
    rc = cai_session_clear_tool_choice_for_tool_continuation(params, error);
  }
  if (rc == CAI_OK) {
    rc = cai_session_check_usage_available(session, error);
  }
  if (rc == CAI_OK) {
    rc = cai_run_options_open_tool_output_runtime(options, &tool_output_runtime,
                                                  error);
  }
  for (i = 0U; rc == CAI_OK && i < cai_response_tool_call_count(response);
       i++) {
    cai_tool_event event;
    int tool_failed;
    int tool_invoked;

    memset(&event, 0, sizeof(event));
    memset(&capture, 0, sizeof(capture));
    capture_output_owned = 1;
    tool_failed = 0;
    tool_invoked = 0;
    event.name = cai_response_tool_call_name(response, i);
    event.arguments_json = cai_response_tool_call_arguments(response, i);
    event.arguments_json_spooled =
        cai_response_tool_call_arguments_spooled(response, i);
    if (options->tool_event != NULL) {
      event.type = CAI_TOOL_EVENT_START;
      rc = options->tool_event(options->tool_event_context, &event, error);
    }
    if (rc != CAI_OK) {
      break;
    }
    tool_output_runtime->spooled_init(tool_output_runtime, &capture.output);
    callbacks.write = cai_capture_tool_output;
    callbacks.close = NULL;
    callbacks.context = &capture;
    sink = NULL;
    rc = cai_sink_from_callbacks(&callbacks, &sink, error);
    if (rc == CAI_OK) {
      spooled_arguments = cai_response_tool_call_arguments_spooled(response, i);
      tool_invoked = 1;
      if (spooled_arguments != NULL) {
        arguments_cursor = *spooled_arguments;
        rc = CAI_SESSION_AGENT_IMPL(session)->tools->run_spooled(
            CAI_SESSION_AGENT_IMPL(session)->tools,
            cai_response_tool_call_name(response, i), &arguments_cursor, sink,
            error);
      } else {
        rc = CAI_SESSION_AGENT_IMPL(session)->tools->run(
            CAI_SESSION_AGENT_IMPL(session)->tools,
            cai_response_tool_call_name(response, i),
            cai_response_tool_call_arguments(response, i), sink, error);
      }
    }
    cai_sink_close(sink);
    tool_failed = rc != CAI_OK;
    if (options->tool_event != NULL) {
      int tool_rc;
      int event_rc;

      tool_rc = rc;
      event.type = rc == CAI_OK ? CAI_TOOL_EVENT_OUTPUT : CAI_TOOL_EVENT_ERROR;
      event.output_json = rc == CAI_OK ? &capture.output : NULL;
      event.tool_error = rc == CAI_OK ? NULL : error;
      event_rc =
          options->tool_event(options->tool_event_context, &event, error);
      if (event_rc != CAI_OK) {
        rc = event_rc;
        tool_failed = 0;
      } else {
        rc = tool_rc;
      }
    }
    if (tool_invoked && tool_failed && rc != CAI_OK &&
        capture.output.cleanup != NULL) {
      int tool_rc;

      tool_rc = rc;
      rc = cai_capture_tool_error_output(&capture, tool_rc, error, error);
      if (rc == CAI_OK && error != NULL) {
        cai_error_cleanup(error);
        cai_error_init(error);
      }
    }
    if (rc == CAI_OK) {
      rc = cai_response_create_params_add_function_call_output_spooled(
          params, cai_response_tool_call_id(response, i), &capture.output,
          error);
      if (rc == CAI_OK) {
        memset(&capture.output, 0, sizeof(capture.output));
        capture_output_owned = 0;
      }
    }
    if (capture_output_owned) {
      cai_capture_cleanup(&capture);
    }
  }
  if (rc == CAI_OK) {
    rc = cai_session_replay_history_with_params_input(
        session, params, &pending_items, &has_pending_items, error);
  }
  if (rc == CAI_OK) {
    rc = cai_session_create_response_from_params(
        session, params, &pending_items, has_pending_items, out, error);
  }
  cai_response_create_params_destroy(params);
  cai_lonejson_runtime_close(&tool_output_runtime);
  if (has_pending_items) {
    pending_items.cleanup(&pending_items);
  }
  return rc;
}

int cai_session_run_auto(cai_session *session, const cai_run_options *options,
                         cai_response **out, cai_error *error) {
  cai_run_options defaults;
  const cai_run_options *effective;
  cai_response *current;
  cai_response *next;
  int rounds;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "response output pointer is required");
  }
  *out = NULL;
  if (session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "session is required");
  }
  cai_run_options_init(&defaults);
  effective = options != NULL ? options : &defaults;
  if (effective->max_tool_rounds < 0) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "max tool rounds cannot be negative");
  }
  current = NULL;
  rc = cai_session_run(session, &current, error);
  rounds = 0;
  while (rc == CAI_OK && cai_response_tool_call_count(current) > 0U &&
         rounds < cai_run_options_effective_max_tool_rounds(effective)) {
    next = NULL;
    rc = cai_session_run_tool_round(session, current, effective, &next, error);
    cai_response_destroy(current);
    current = next;
    rounds++;
  }
  if (rc != CAI_OK) {
    cai_response_destroy(current);
    return rc;
  }
  if (cai_response_tool_call_count(current) > 0U) {
    cai_response_destroy(current);
    return cai_set_error(error, CAI_ERR_CANCELLED,
                         "tool auto-run exhausted max tool rounds");
  }
  *out = current;
  return CAI_OK;
}

int cai_session_run_auto_output(cai_session *session,
                                const cai_run_options *options,
                                cai_output **out, cai_error *error) {
  cai_response *response;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "output pointer is required");
  }
  *out = NULL;
  response = NULL;
  rc = cai_session_run_auto(session, options, &response, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_output_from_response(response, out, error);
  if (rc != CAI_OK) {
    cai_response_destroy(response);
  }
  return rc;
}

static int cai_session_stream_once(cai_session *session,
                                   const cai_stream_sinks *sinks,
                                   cai_stream_tool_call_list *tool_calls,
                                   cai_error *error) {
  cai_response_create_params *params;
  cai_stream_tool_capture capture;
  cai_stream_sinks effective_sinks;
  char *response_id;
  lonejson_spooled pending_items;
  lonejson_spooled output_text;
  int has_pending_items;
  int has_output_text;
  int capture_stream;
  cai_token_usage usage;
  int rc;

  if (session == NULL || sinks == NULL ||
      (sinks->output_text == NULL && sinks->reasoning_summary == NULL &&
       sinks->output_item_done == NULL && sinks->output_text_delta == NULL &&
       sinks->function_call_arguments_delta == NULL &&
       sinks->function_call_arguments_done == NULL && tool_calls == NULL)) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session and at least one stream sink or callback are "
                         "required");
  }
  effective_sinks = *sinks;
  capture_stream = tool_calls != NULL ||
                   CAI_SESSION_AGENT_IMPL(session)->session_continuity ==
                       CAI_SESSION_CONTINUITY_CLIENT_HISTORY;
  has_output_text = 0;
  if (capture_stream) {
    memset(&capture, 0, sizeof(capture));
    capture.user_sinks = sinks;
    capture.tool_calls = tool_calls;
    if (CAI_SESSION_AGENT_IMPL(session)->session_continuity ==
        CAI_SESSION_CONTINUITY_CLIENT_HISTORY) {
      capture.session = session;
      cai_history_init_spooled(session, &output_text);
      has_output_text = 1;
      capture.output_text = &output_text;
      effective_sinks.output_text_delta = cai_stream_capture_output_text;
      effective_sinks.output_text_context = &capture;
      effective_sinks.output_item_done = cai_stream_capture_output_item_done;
      effective_sinks.output_item_context = &capture;
    }
    if (tool_calls != NULL) {
      effective_sinks.function_call_arguments_delta =
          cai_stream_capture_tool_delta;
      effective_sinks.function_call_arguments_done =
          cai_stream_capture_tool_done;
      effective_sinks.function_call_context = &capture;
    }
  }
  params = NULL;
  response_id = NULL;
  memset(&pending_items, 0, sizeof(pending_items));
  has_pending_items = 0;
  memset(&usage, 0, sizeof(usage));
  rc = cai_session_init_response_params(session, &params, error);
  if (rc == CAI_OK) {
    rc = cai_session_prepare_history_params(session, params, &pending_items,
                                            &has_pending_items, error);
  }
  if (rc == CAI_OK) {
    rc = cai_session_check_usage_available(session, error);
  }
  if (rc == CAI_OK) {
    rc = cai_client_stream_response_with_id(
        CAI_SESSION_AGENT_IMPL(session)->client, params, &effective_sinks,
        &response_id, &usage, error);
  }
  if (rc == CAI_OK) {
    if (CAI_SESSION_AGENT_IMPL(session)->session_continuity ==
        CAI_SESSION_CONTINUITY_CLIENT_HISTORY) {
      rc = cai_stream_capture_output_items_finish(&capture, error);
    }
  }
  if (rc == CAI_OK) {
    if (CAI_SESSION_AGENT_IMPL(session)->session_continuity ==
        CAI_SESSION_CONTINUITY_CLIENT_HISTORY) {
      rc = cai_session_after_stream_tool_calls(
          session, &pending_items, has_pending_items, response_id, &usage,
          tool_calls, &output_text, &capture.output_items,
          capture.output_items_count, error);
    } else {
      rc = cai_session_after_stream(session, &pending_items, has_pending_items,
                                    response_id, &usage, error);
    }
  }
  cai_response_create_params_destroy(params);
  cai_free_mem(NULL, response_id);
  if (has_pending_items) {
    pending_items.cleanup(&pending_items);
  }
  if (has_output_text) {
    output_text.cleanup(&output_text);
  }
  if (capture_stream && capture.output_items_initialized) {
    capture.output_items.cleanup(&capture.output_items);
  }
  if (rc == CAI_OK || (rc == CAI_ERR_LIMIT && response_id != NULL)) {
    cai_session_clear_inputs(session);
  }
  return rc;
}

int cai_session_stream(cai_session *session, const cai_stream_sinks *sinks,
                       cai_error *error) {
  return cai_session_stream_once(session, sinks, NULL, error);
}

static int cai_session_add_stream_tool_outputs(
    cai_session *session, cai_response_create_params *params,
    const cai_stream_tool_call_list *calls, const cai_run_options *options,
    cai_error *error) {
  cai_sink_callbacks callbacks;
  cai_sink *sink;
  cai_tool_output_capture capture;
  lonejson *tool_output_runtime;
  size_t i;
  int capture_output_owned;
  int rc;

  if (calls == NULL) {
    return CAI_OK;
  }
  tool_output_runtime = NULL;
  rc = cai_run_options_open_tool_output_runtime(options, &tool_output_runtime,
                                                error);
  for (i = 0U; rc == CAI_OK && i < calls->count; i++) {
    cai_tool_event event;
    int tool_failed;
    int tool_invoked;

    memset(&event, 0, sizeof(event));
    memset(&capture, 0, sizeof(capture));
    capture_output_owned = 1;
    tool_failed = 0;
    tool_invoked = 0;
    event.name = calls->items[i].name;
    event.arguments_json = calls->items[i].arguments;
    event.arguments_json_spooled = calls->items[i].has_arguments_spooled
                                       ? &calls->items[i].arguments_spooled
                                       : NULL;
    if (options->tool_event != NULL) {
      event.type = CAI_TOOL_EVENT_START;
      rc = options->tool_event(options->tool_event_context, &event, error);
    }
    if (rc != CAI_OK) {
      break;
    }
    tool_output_runtime->spooled_init(tool_output_runtime, &capture.output);
    callbacks.write = cai_capture_tool_output;
    callbacks.close = NULL;
    callbacks.context = &capture;
    sink = NULL;
    rc = cai_sink_from_callbacks(&callbacks, &sink, error);
    if (rc == CAI_OK) {
      tool_invoked = 1;
      if (calls->items[i].has_arguments_spooled) {
        rc = CAI_SESSION_AGENT_IMPL(session)->tools->run_spooled(
            CAI_SESSION_AGENT_IMPL(session)->tools, calls->items[i].name,
            &calls->items[i].arguments_spooled, sink, error);
      } else {
        rc = CAI_SESSION_AGENT_IMPL(session)->tools->run(
            CAI_SESSION_AGENT_IMPL(session)->tools, calls->items[i].name,
            calls->items[i].arguments, sink, error);
      }
    }
    cai_sink_close(sink);
    tool_failed = rc != CAI_OK;
    if (options->tool_event != NULL) {
      int tool_rc;
      int event_rc;

      tool_rc = rc;
      event.type = rc == CAI_OK ? CAI_TOOL_EVENT_OUTPUT : CAI_TOOL_EVENT_ERROR;
      event.output_json = rc == CAI_OK ? &capture.output : NULL;
      event.tool_error = rc == CAI_OK ? NULL : error;
      event_rc =
          options->tool_event(options->tool_event_context, &event, error);
      if (event_rc != CAI_OK) {
        rc = event_rc;
        tool_failed = 0;
      } else {
        rc = tool_rc;
      }
    }
    if (tool_invoked && tool_failed && rc != CAI_OK &&
        capture.output.cleanup != NULL) {
      int tool_rc;

      tool_rc = rc;
      rc = cai_capture_tool_error_output(&capture, tool_rc, error, error);
      if (rc == CAI_OK && error != NULL) {
        cai_error_cleanup(error);
        cai_error_init(error);
      }
    }
    if (rc == CAI_OK) {
      rc = cai_response_create_params_add_function_call_output_spooled(
          params, calls->items[i].call_id, &capture.output, error);
      if (rc == CAI_OK) {
        memset(&capture.output, 0, sizeof(capture.output));
        capture_output_owned = 0;
      }
    }
    if (capture_output_owned) {
      cai_capture_cleanup(&capture);
    }
  }
  cai_lonejson_runtime_close(&tool_output_runtime);
  return rc;
}

static int cai_session_stream_tool_round(
    cai_session *session, const cai_run_options *options,
    const cai_stream_sinks *sinks, const cai_stream_tool_call_list *input_calls,
    cai_stream_tool_call_list *output_calls, cai_error *error) {
  cai_response_create_params *params;
  cai_stream_tool_capture capture;
  cai_stream_sinks effective_sinks;
  char *response_id;
  lonejson_spooled pending_items;
  lonejson_spooled output_text;
  cai_token_usage usage;
  int has_pending_items;
  int has_output_text;
  int rc;

  params = NULL;
  response_id = NULL;
  memset(&pending_items, 0, sizeof(pending_items));
  has_pending_items = 0;
  has_output_text = 0;
  memset(&usage, 0, sizeof(usage));
  effective_sinks = *sinks;
  memset(&capture, 0, sizeof(capture));
  capture.user_sinks = sinks;
  capture.session = session;
  capture.tool_calls = output_calls;
  if (CAI_SESSION_AGENT_IMPL(session)->session_continuity ==
      CAI_SESSION_CONTINUITY_CLIENT_HISTORY) {
    cai_history_init_spooled(session, &output_text);
    has_output_text = 1;
    capture.output_text = &output_text;
    effective_sinks.output_text_delta = cai_stream_capture_output_text;
    effective_sinks.output_text_context = &capture;
    effective_sinks.output_item_done = cai_stream_capture_output_item_done;
    effective_sinks.output_item_context = &capture;
  }
  effective_sinks.function_call_arguments_delta = cai_stream_capture_tool_delta;
  effective_sinks.function_call_arguments_done = cai_stream_capture_tool_done;
  effective_sinks.function_call_context = &capture;
  rc = cai_session_init_response_params(session, &params, error);
  if (rc == CAI_OK) {
    rc = cai_session_clear_tool_choice_for_tool_continuation(params, error);
  }
  if (rc == CAI_OK) {
    rc = cai_session_check_usage_available(session, error);
  }
  if (rc == CAI_OK) {
    rc = cai_session_add_stream_tool_outputs(session, params, input_calls,
                                             options, error);
  }
  if (rc == CAI_OK) {
    rc = cai_session_replay_history_with_params_input(
        session, params, &pending_items, &has_pending_items, error);
  }
  if (rc == CAI_OK) {
    rc = cai_session_check_usage_available(session, error);
  }
  if (rc == CAI_OK) {
    rc = cai_client_stream_response_with_id(
        CAI_SESSION_AGENT_IMPL(session)->client, params, &effective_sinks,
        &response_id, &usage, error);
  }
  if (rc == CAI_OK) {
    if (CAI_SESSION_AGENT_IMPL(session)->session_continuity ==
        CAI_SESSION_CONTINUITY_CLIENT_HISTORY) {
      rc = cai_stream_capture_output_items_finish(&capture, error);
      if (rc != CAI_OK && (error == NULL || error->message == NULL)) {
        rc = cai_set_error(error, rc,
                           "failed to finish streamed output item history");
      }
    }
  }
  if (rc == CAI_OK) {
    if (CAI_SESSION_AGENT_IMPL(session)->session_continuity ==
        CAI_SESSION_CONTINUITY_CLIENT_HISTORY) {
      rc = cai_session_after_stream_tool_calls(
          session, &pending_items, has_pending_items, response_id, &usage,
          output_calls, &output_text, &capture.output_items,
          capture.output_items_count, error);
      if (rc != CAI_OK && (error == NULL || error->message == NULL)) {
        rc = cai_set_error(error, rc,
                           "failed to record streamed tool-call response");
      }
    } else {
      rc = cai_session_after_stream(session, &pending_items, has_pending_items,
                                    response_id, &usage, error);
      if (rc != CAI_OK && (error == NULL || error->message == NULL)) {
        rc = cai_set_error(error, rc, "failed to record streamed response");
      }
    }
  }
  cai_response_create_params_destroy(params);
  cai_free_mem(NULL, response_id);
  if (has_pending_items) {
    pending_items.cleanup(&pending_items);
  }
  if (has_output_text) {
    output_text.cleanup(&output_text);
  }
  if (capture.output_items_initialized) {
    capture.output_items.cleanup(&capture.output_items);
  }
  return rc;
}

int cai_session_stream_auto(cai_session *session,
                            const cai_run_options *options,
                            const cai_stream_sinks *sinks, cai_error *error) {
  cai_run_options defaults;
  const cai_run_options *effective;
  cai_stream_tool_call_list current_calls;
  cai_stream_tool_call_list next_calls;
  int rounds;
  int rc;

  if (session == NULL || sinks == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session and stream sinks are required");
  }
  cai_run_options_init(&defaults);
  effective = options != NULL ? options : &defaults;
  if (effective->max_tool_rounds < 0) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "max tool rounds cannot be negative");
  }
  memset(&current_calls, 0, sizeof(current_calls));
  memset(&next_calls, 0, sizeof(next_calls));
  rc = cai_session_stream_once(session, sinks, &current_calls, error);
  rounds = 0;
  while (rc == CAI_OK && current_calls.count > 0U &&
         rounds < cai_run_options_effective_max_tool_rounds(effective)) {
    cai_stream_tool_call_list_cleanup(&next_calls);
    memset(&next_calls, 0, sizeof(next_calls));
    rc = cai_session_stream_tool_round(session, effective, sinks,
                                       &current_calls, &next_calls, error);
    cai_stream_tool_call_list_cleanup(&current_calls);
    current_calls = next_calls;
    memset(&next_calls, 0, sizeof(next_calls));
    rounds++;
  }
  if (rc == CAI_OK && current_calls.count > 0U) {
    rc = cai_set_error(error, CAI_ERR_CANCELLED,
                       "tool auto-run exhausted max tool rounds");
  }
  cai_stream_tool_call_list_cleanup(&current_calls);
  cai_stream_tool_call_list_cleanup(&next_calls);
  return rc;
}

int cai_session_stream_text(cai_session *session, cai_sink *sink,
                            cai_error *error) {
  cai_stream_sinks sinks;

  cai_stream_sinks_init(&sinks);
  sinks.output_text = sink;
  return cai_session_stream(session, &sinks, error);
}

int cai_session_open_text_source(cai_session *session, cai_source **out,
                                 cai_error *error) {
  cai_response_create_params *params;
  lonejson_spooled pending_items;
  int has_pending_items;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "source output pointer is required");
  }
  *out = NULL;
  if (session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "session is required");
  }
  if (CAI_SESSION_AGENT_IMPL(session)->local_history_enabled) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "local history capture is not supported for sources");
  }
  params = NULL;
  memset(&pending_items, 0, sizeof(pending_items));
  has_pending_items = 0;
  rc = cai_session_init_response_params(session, &params, error);
  if (rc == CAI_OK) {
    rc = cai_session_prepare_history_params(session, params, &pending_items,
                                            &has_pending_items, error);
  }
  if (rc == CAI_OK) {
    rc = cai_session_check_usage_available(session, error);
  }
  if (rc == CAI_OK) {
    rc = cai_client_open_response_text_source_take_params(
        CAI_SESSION_AGENT_IMPL(session)->client, params,
        cai_session_stream_complete, session, out, error);
    if (rc == CAI_OK) {
      params = NULL;
    }
  }
  cai_response_create_params_destroy(params);
  if (has_pending_items) {
    pending_items.cleanup(&pending_items);
  }
  if (rc == CAI_OK) {
    cai_session_clear_inputs(session);
  }
  return rc;
}

int cai_session_send_text(cai_session *session, const char *text,
                          cai_response **out, cai_error *error) {
  int rc;

  if (session == NULL || text == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session and text are required");
  }
  rc = cai_session_add_user_text(session, text, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_session_run(session, out, error);
  if (rc != CAI_OK) {
    cai_session_clear_inputs(session);
  }
  return rc;
}

int cai_session_last_usage(const cai_session *session, cai_token_usage *out,
                           cai_error *error) {
  if (session == NULL || out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session and usage output are required");
  }
  if (!CAI_SESSION_IMPL(session)->has_last_usage) {
    memset(out, 0, sizeof(*out));
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session has no completed response usage");
  }
  *out = CAI_SESSION_IMPL(session)->last_usage;
  return CAI_OK;
}

int cai_session_set_usage_limits(cai_session *session,
                                 const cai_usage_limits *limits,
                                 cai_error *error) {
  cai_usage_limits empty;
  int rc;

  if (session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "session is required");
  }
  if (limits == NULL) {
    cai_usage_limits_init(&empty);
    limits = &empty;
  }
  rc = cai_usage_limits_validate(limits, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_usage_limits_require_pricing(
      limits, CAI_SESSION_AGENT_IMPL(session)->model, error);
  if (rc != CAI_OK) {
    return rc;
  }
  CAI_SESSION_IMPL(session)->usage_limits = *limits;
  if (cai_usage_accounting_exceeds(&CAI_SESSION_IMPL(session)->usage,
                                   &CAI_SESSION_IMPL(session)->usage_limits)) {
    CAI_SESSION_IMPL(session)->usage.limit_exceeded = 1;
  } else {
    CAI_SESSION_IMPL(session)->usage.limit_exceeded = 0;
  }
  return CAI_OK;
}

int cai_session_usage(const cai_session *session, cai_usage_accounting *out,
                      cai_error *error) {
  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "usage accounting output pointer is required");
  }
  cai_usage_accounting_init(out);
  if (session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "session is required");
  }
  *out = CAI_SESSION_IMPL(session)->usage;
  return CAI_OK;
}

int cai_session_close_with_usage(cai_session *session,
                                 cai_usage_accounting *out, cai_error *error) {
  int rc;

  if (session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "session is required");
  }
  rc = cai_session_usage(session, out, error);
  cai_session_destroy(session);
  return rc;
}

long long cai_session_context_window_tokens(const cai_session *session) {
  if (session == NULL || CAI_SESSION_IMPL(session)->agent == NULL) {
    return 0LL;
  }
  return cai_model_context_window_tokens(
      CAI_SESSION_AGENT_IMPL(session)->model);
}

long long cai_session_auto_compact_token_limit(const cai_session *session) {
  if (session == NULL || CAI_SESSION_IMPL(session)->agent == NULL) {
    return 0LL;
  }
  return CAI_SESSION_AGENT_IMPL(session)->auto_compact_token_limit;
}

int cai_session_context_percent(const cai_session *session, double *out,
                                cai_error *error) {
  long long window;

  if (session == NULL || out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session and percent output are required");
  }
  if (!CAI_SESSION_IMPL(session)->has_last_usage) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session has no completed response usage");
  }
  window = cai_session_context_window_tokens(session);
  if (window <= 0LL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session model has unknown context window");
  }
  *out = ((double)CAI_SESSION_IMPL(session)->last_usage.total_tokens * 100.0) /
         (double)window;
  return CAI_OK;
}

int cai_session_history_spilled(const cai_session *session) {
  if (session == NULL) {
    return 0;
  }
  return CAI_SESSION_IMPL(session)->history.spilled_fn(
      &CAI_SESSION_IMPL(session)->history);
}

static lonejson_status cai_state_spooled_sink(void *user, const void *data,
                                              size_t len,
                                              lonejson_error *error) {
  lonejson_spooled *spool;

  spool = (lonejson_spooled *)user;
  if (spool == NULL) {
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return spool->append(spool, data, len, error);
}

static lonejson_read_result
cai_state_source_reader(void *user, unsigned char *buffer, size_t capacity) {
  cai_source_reader_context *context;
  lonejson_read_result result;
  size_t nread;
  int previous_error_code;

  result = lonejson_default_read_result();
  context = (cai_source_reader_context *)user;
  if (context == NULL || context->source == NULL) {
    result.eof = 1;
    result.error_code = 1;
    return result;
  }
  previous_error_code = context->error != NULL ? context->error->code : CAI_OK;
  nread = cai_source_read(context->source, buffer, capacity, context->error);
  result.bytes_read = nread;
  if (context->error != NULL && context->error->code != previous_error_code &&
      context->error->code != CAI_OK) {
    context->failed = 1;
    result.error_code = context->error->code;
  }
  if (nread == 0U && result.error_code == 0) {
    result.eof = 1;
  }
  return result;
}

static lonejson_status cai_root_check_object_begin(void *user,
                                                   lonejson_error *error) {
  cai_json_root_array_check *check;

  (void)error;
  check = (cai_json_root_array_check *)user;
  if (check->depth == 0) {
    check->root_seen = 1;
    check->root_is_array = 0;
  }
  check->depth++;
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_root_check_object_end(void *user,
                                                 lonejson_error *error) {
  cai_json_root_array_check *check;

  (void)error;
  check = (cai_json_root_array_check *)user;
  if (check->depth > 0) {
    check->depth--;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_root_check_array_begin(void *user,
                                                  lonejson_error *error) {
  cai_json_root_array_check *check;

  (void)error;
  check = (cai_json_root_array_check *)user;
  if (check->depth == 0) {
    check->root_seen = 1;
    check->root_is_array = 1;
  }
  check->depth++;
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_root_check_array_end(void *user,
                                                lonejson_error *error) {
  return cai_root_check_object_end(user, error);
}

static lonejson_status cai_root_check_scalar(void *user,
                                             lonejson_error *error) {
  cai_json_root_array_check *check;

  (void)error;
  check = (cai_json_root_array_check *)user;
  if (check->depth == 0) {
    check->root_seen = 1;
    check->root_is_array = 0;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_root_check_bool(void *user, int value,
                                           lonejson_error *error) {
  (void)value;
  return cai_root_check_scalar(user, error);
}

static int cai_spooled_json_is_array(const lonejson_spooled *spool,
                                     cai_error *error) {
  lonejson_spooled cursor;
  lonejson_error json_error;
  lonejson_value_visitor visitor;
  cai_json_root_array_check check;
  cai_spooled_reader_context reader_context;

  if (spool == NULL || spool->size_fn(spool) == 0U) {
    return cai_set_error(error, CAI_ERR_INVALID, "JSON value is empty");
  }
  cursor = *spool;
  lonejson_error_init(&json_error);
  if (cursor.rewind(&cursor, &json_error) != LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to rewind JSON value",
                                json_error.message);
  }
  reader_context.cursor = cursor;
  memset(&check, 0, sizeof(check));
  visitor = lonejson_default_value_visitor();
  visitor.object_begin = cai_root_check_object_begin;
  visitor.object_end = cai_root_check_object_end;
  visitor.array_begin = cai_root_check_array_begin;
  visitor.array_end = cai_root_check_array_end;
  visitor.string_begin = cai_root_check_scalar;
  visitor.number_begin = cai_root_check_scalar;
  visitor.boolean_value = cai_root_check_bool;
  visitor.null_value = cai_root_check_scalar;
  lonejson_error_init(&json_error);
  if (CAI_LJ->visit_value_reader(CAI_LJ, cai_history_spooled_reader,
                                 &reader_context, &visitor, &check,
                                 &json_error) != LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_INVALID,
                                "JSON value is not valid JSON",
                                json_error.message);
  }
  if (!check.root_seen || !check.root_is_array) {
    return cai_set_error(error, CAI_ERR_INVALID, "JSON value must be an array");
  }
  return CAI_OK;
}

static int cai_compact_spooled_json(cai_session *session,
                                    const lonejson_spooled *json,
                                    lonejson_spooled *out, cai_error *error) {
  lonejson_spooled cursor;
  lonejson_json_value value;
  lonejson_error json_error;
  cai_spooled_reader_context reader_context;
  cai_history_sink_context sink_context;
  int has_out;
  int rc;

  memset(out, 0, sizeof(*out));
  has_out = 0;
  rc = CAI_OK;
  cursor = *json;
  lonejson_error_init(&json_error);
  if (cursor.rewind(&cursor, &json_error) != LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to rewind JSON value",
                                json_error.message);
  }
  cai_history_init_spooled(session, out);
  has_out = 1;
  memset(&value, 0, sizeof(value));
  CAI_LJ->json_value_init(CAI_LJ, &value);
  reader_context.cursor = cursor;
  lonejson_error_init(&json_error);
  if (value.methods->set_reader(&value, cai_history_spooled_reader,
                                &reader_context,
                                &json_error) != LONEJSON_STATUS_OK) {
    rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                              "failed to prepare JSON value",
                              json_error.message);
    goto done;
  }
  sink_context.spool = out;
  lonejson_error_init(&json_error);
  if (value.methods->write_to_sink(&value, cai_history_lonejson_sink,
                                   &sink_context,
                                   &json_error) != LONEJSON_STATUS_OK) {
    rc = cai_set_error_detail(error, CAI_ERR_INVALID,
                              "failed to compact JSON value",
                              json_error.message);
    goto done;
  }

done:
  CAI_LJ->json_value_cleanup(CAI_LJ, &value);
  if (rc != CAI_OK && has_out) {
    out->cleanup(out);
  }
  return rc;
}

static lonejson_read_result
cai_history_spooled_reader(void *user, unsigned char *buffer, size_t capacity) {
  cai_spooled_reader_context *context;
  lonejson_read_result result;

  context = (cai_spooled_reader_context *)user;
  if (context == NULL) {
    result = lonejson_default_read_result();
    result.eof = 1;
    result.error_code = 1;
    return result;
  }
  return context->cursor.read(&context->cursor, buffer, capacity);
}

static int cai_history_source_to_array_spooled(cai_session *session,
                                               cai_source *source,
                                               lonejson_spooled *out,
                                               cai_error *error) {
  lonejson_spooled raw;
  lonejson_spooled compact;
  lonejson_error json_error;
  unsigned char buffer[4096];
  size_t nread;
  int has_raw;
  int has_compact;
  int previous_error_code;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "history spool output pointer is required");
  }
  memset(&raw, 0, sizeof(raw));
  memset(&compact, 0, sizeof(compact));
  has_raw = 0;
  has_compact = 0;
  rc = CAI_OK;
  cai_history_init_spooled(session, &raw);
  has_raw = 1;
  for (;;) {
    previous_error_code = error != NULL ? error->code : CAI_OK;
    nread = cai_source_read(source, buffer, sizeof(buffer), error);
    if (nread == 0U && error != NULL && error->code != previous_error_code &&
        error->code != CAI_OK) {
      rc = error->code;
      goto done;
    }
    if (nread == 0U) {
      break;
    }
    lonejson_error_init(&json_error);
    if (raw.append(&raw, buffer, nread, &json_error) != LONEJSON_STATUS_OK) {
      rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "failed to append imported history",
                                json_error.message);
      goto done;
    }
  }
  if (raw.size_fn(&raw) == 0U) {
    rc =
        cai_set_error(error, CAI_ERR_INVALID, "imported history JSON is empty");
    goto done;
  }
  rc = cai_spooled_json_is_array(&raw, error);
  if (rc == CAI_OK) {
    rc = cai_compact_spooled_json(session, &raw, &compact, error);
    if (rc == CAI_OK) {
      has_compact = 1;
    }
  }
  if (rc == CAI_OK) {
    *out = compact;
    has_compact = 0;
  }

done:
  if (has_raw) {
    raw.cleanup(&raw);
  }
  if (has_compact) {
    compact.cleanup(&compact);
  }
  return rc;
}

int cai_session_export_history_source(cai_session *session, cai_source **out,
                                      cai_error *error) {
  lonejson_spooled history_json;
  int has_history_json;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "history source output pointer is required");
  }
  *out = NULL;
  if (session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "session is required");
  }
  if (!CAI_SESSION_AGENT_IMPL(session)->local_history_enabled) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "local history capture is disabled");
  }
  memset(&history_json, 0, sizeof(history_json));
  has_history_json = 0;
  rc = cai_history_to_array_spool(session, &history_json, error);
  if (rc == CAI_OK) {
    has_history_json = 1;
    rc = cai_source_from_spooled(&history_json, out, error);
    if (rc == CAI_OK) {
      has_history_json = 0;
    }
  }
  if (has_history_json) {
    history_json.cleanup(&history_json);
  }
  return rc;
}

int cai_session_import_history_source(cai_session *session, cai_source *source,
                                      cai_error *error) {
  lonejson_spooled history_json;
  lonejson_spooled next_history;
  int has_history_json;
  int has_next_history;
  int rc;

  if (session == NULL || source == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session and history source are required");
  }
  if (!CAI_SESSION_AGENT_IMPL(session)->local_history_enabled) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "local history capture is disabled");
  }
  memset(&history_json, 0, sizeof(history_json));
  memset(&next_history, 0, sizeof(next_history));
  has_history_json = 0;
  has_next_history = 0;
  rc = cai_history_source_to_array_spooled(session, source, &history_json,
                                           error);
  if (rc == CAI_OK) {
    has_history_json = 1;
    cai_history_init_spooled(session, &next_history);
    has_next_history = 1;
    rc = cai_history_append_array_record_to_spool(session, &next_history,
                                                  &history_json, error);
  }
  if (rc == CAI_OK) {
    cai_history_replace(session, &next_history);
    has_next_history = 0;
  }
  if (has_history_json) {
    history_json.cleanup(&history_json);
  }
  if (has_next_history) {
    next_history.cleanup(&next_history);
  }
  return rc;
}

int cai_session_export_state_source(cai_session *session, cai_source **out,
                                    cai_error *error) {
  cai_session_state_doc doc;
  lonejson_spooled state_json;
  lonejson_spooled history_json;
  cai_history_sink_context sink_context;
  cai_spooled_reader_context reader_context;
  lonejson_error json_error;
  int has_state_json;
  int has_history_json;
  int include_history;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "state source output pointer is required");
  }
  *out = NULL;
  if (session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "session is required");
  }
  memset(&state_json, 0, sizeof(state_json));
  memset(&history_json, 0, sizeof(history_json));
  memset(&doc, 0, sizeof(doc));
  has_state_json = 0;
  has_history_json = 0;
  include_history = 0;
  rc = CAI_OK;
  doc.version = 1LL;
  doc.model = CAI_SESSION_AGENT_IMPL(session)->model;
  if (CAI_SESSION_IMPL(session)->conversation_id != NULL) {
    doc.conversation_id = CAI_SESSION_IMPL(session)->conversation_id;
  } else {
    doc.previous_response_id = CAI_SESSION_IMPL(session)->previous_response_id;
  }
  CAI_LJ->json_value_init(CAI_LJ, &doc.history);
  if (CAI_SESSION_AGENT_IMPL(session)->local_history_enabled) {
    rc = cai_history_to_array_spool(session, &history_json, error);
    if (rc == CAI_OK) {
      has_history_json = 1;
      include_history = 1;
    }
  }
  if (rc == CAI_OK) {
    cai_history_init_spooled(session, &state_json);
    has_state_json = 1;
    sink_context.spool = &state_json;
    lonejson_error_init(&json_error);
    if (include_history) {
      reader_context.cursor = history_json;
      lonejson_error_init(&json_error);
      if (reader_context.cursor.rewind(&reader_context.cursor, &json_error) !=
          LONEJSON_STATUS_OK) {
        rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                  "failed to rewind session history JSON",
                                  json_error.message);
      }
    }
    if (rc == CAI_OK) {
      lonejson_error_init(&json_error);
      if (include_history &&
          doc.history.methods->set_reader(
              &doc.history, cai_history_spooled_reader, &reader_context,
              &json_error) != LONEJSON_STATUS_OK) {
        rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                  "failed to prepare session history JSON",
                                  json_error.message);
      }
    }
    if (rc == CAI_OK) {
      lonejson_error_init(&json_error);
      if (CAI_LJ->serialize_sink(CAI_LJ, &cai_session_state_map, &doc,
                                 cai_history_lonejson_sink, &sink_context,
                                 &json_error) != LONEJSON_STATUS_OK) {
        rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                  "failed to serialize session state JSON",
                                  json_error.message);
      }
    }
  }
  if (rc == CAI_OK) {
    rc = cai_source_from_spooled(&state_json, out, error);
    if (rc == CAI_OK) {
      has_state_json = 0;
    }
  }
  if (has_state_json) {
    state_json.cleanup(&state_json);
  }
  if (has_history_json) {
    history_json.cleanup(&history_json);
  }
  CAI_LJ->json_value_cleanup(CAI_LJ, &doc.history);
  return rc;
}

int cai_session_import_state_source(cai_session *session, cai_source *source,
                                    cai_error *error) {
  cai_session_state_doc doc;
  lonejson_spooled history_json;
  lonejson_spooled next_history;
  lonejson_error json_error;
  cai_source_reader_context reader_context;
  int has_history_json;
  int has_next_history;
  int rc;

  if (session == NULL || source == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session and state source are required");
  }
  memset(&doc, 0, sizeof(doc));
  memset(&history_json, 0, sizeof(history_json));
  memset(&next_history, 0, sizeof(next_history));
  has_history_json = 0;
  has_next_history = 0;
  rc = CAI_OK;
  cai_history_init_spooled(session, &history_json);
  has_history_json = 1;
  CAI_LJ->json_value_init(CAI_LJ, &doc.history);
  lonejson_error_init(&json_error);
  if (doc.history.methods->set_parse_sink(&doc.history, cai_state_spooled_sink,
                                          &history_json,
                                          &json_error) != LONEJSON_STATUS_OK) {
    rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                              "failed to prepare state history parser",
                              json_error.message);
    goto done;
  }
  reader_context.source = source;
  reader_context.error = error;
  reader_context.failed = 0;
  lonejson_error_init(&json_error);
  if (CAI_LJ_PRESERVE->parse_reader(CAI_LJ_PRESERVE, &cai_session_state_map,
                                    &doc, cai_state_source_reader,
                                    &reader_context,
                                    &json_error) != LONEJSON_STATUS_OK) {
    rc = cai_set_error_detail(error, CAI_ERR_INVALID,
                              "failed to parse session state JSON",
                              json_error.message);
    goto done;
  }
  if (doc.version != 1LL) {
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "unsupported session state version");
    goto done;
  }
  if (doc.previous_response_id != NULL && doc.conversation_id != NULL) {
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "session state has multiple continuation handles");
    goto done;
  }
  if (CAI_SESSION_AGENT_IMPL(session)->local_history_enabled &&
      history_json.size_fn(&history_json) > 0U) {
    rc = cai_spooled_json_is_array(&history_json, error);
  }
  if (rc == CAI_OK && CAI_SESSION_AGENT_IMPL(session)->local_history_enabled) {
    cai_history_init_spooled(session, &next_history);
    has_next_history = 1;
    if (history_json.size_fn(&history_json) > 0U) {
      rc = cai_history_append_array_record_to_spool(session, &next_history,
                                                    &history_json, error);
    }
  }
  if (rc == CAI_OK) {
    if (doc.conversation_id != NULL) {
      rc = cai_session_set_conversation_id(session, doc.conversation_id, error);
    } else if (doc.previous_response_id != NULL) {
      rc = cai_session_set_previous_response_id(
          session, doc.previous_response_id, error);
    } else {
      rc = cai_session_set_previous_response_id(session, NULL, error);
    }
  }
  if (rc == CAI_OK && has_next_history) {
    cai_history_replace(session, &next_history);
    has_next_history = 0;
  }

done:
  CAI_LJ->cleanup(CAI_LJ, &cai_session_state_map, &doc);
  if (has_history_json) {
    history_json.cleanup(&history_json);
  }
  if (has_next_history) {
    next_history.cleanup(&next_history);
  }
  return rc;
}

int cai_session_save_state_path(cai_session *session, const char *path,
                                cai_error *error) {
  cai_source *source;
  cai_sink *sink;
  FILE *fp;
  int rc;

  if (session == NULL || path == NULL || path[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session and state path are required");
  }
  source = NULL;
  sink = NULL;
  fp = NULL;
  rc = cai_session_export_state_source(session, &source, error);
  if (rc == CAI_OK) {
    fp = fopen(path, "wb");
    if (fp == NULL) {
      rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to open session state path for writing");
    }
  }
  if (rc == CAI_OK) {
    rc = cai_sink_file(fp, 1, &sink, error);
    if (rc == CAI_OK) {
      fp = NULL;
    }
  }
  if (rc == CAI_OK) {
    rc = cai_source_copy_to_sink(source, sink, error);
  }
  if (fp != NULL) {
    fclose(fp);
  }
  cai_sink_close(sink);
  cai_source_close(source);
  return rc;
}

int cai_session_load_state_path(cai_session *session, const char *path,
                                cai_error *error) {
  cai_source *source;
  FILE *fp;
  int rc;

  if (session == NULL || path == NULL || path[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session and state path are required");
  }
  source = NULL;
  fp = fopen(path, "rb");
  if (fp == NULL) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to open session state path for reading");
  }
  rc = cai_source_file(fp, 1, &source, error);
  if (rc == CAI_OK) {
    fp = NULL;
    rc = cai_session_import_state_source(session, source, error);
  }
  if (fp != NULL) {
    fclose(fp);
  }
  cai_source_close(source);
  return rc;
}

static void cai_agent_init_methods(cai_agent *agent) {
  agent->register_tool = cai_agent_register_tool;
  agent->register_raw_tool = cai_agent_register_raw_tool;
  agent->register_raw_spooled_tool = cai_agent_register_raw_spooled_tool;
  agent->add_hosted_tool_json = cai_agent_add_hosted_tool_json;
  agent->add_simple_hosted_tool = cai_agent_add_simple_hosted_tool;
  agent->add_hosted_mcp_tool = cai_agent_add_hosted_mcp_tool;
  agent->new_session = cai_agent_new_session;
  agent->new_conversation_session = cai_agent_new_conversation_session;
  agent->new_session_for_conversation = cai_agent_new_session_for_conversation;
  agent->add_user_text = cai_agent_add_user_text;
  agent->add_user_text_spooled = cai_agent_add_user_text_spooled;
  agent->add_user_text_source = cai_agent_add_user_text_source;
  agent->add_user_image_url = cai_agent_add_user_image_url;
  agent->add_user_file_data_spooled = cai_agent_add_user_file_data_spooled;
  agent->add_user_file_source = cai_agent_add_user_file_source;
  agent->add_user_file_path = cai_agent_add_user_file_path;
  agent->run = cai_agent_run;
  agent->run_output = cai_agent_run_output;
  agent->run_auto = cai_agent_run_auto;
  agent->run_auto_output = cai_agent_run_auto_output;
  agent->stream_auto = cai_agent_stream_auto;
  agent->stream = cai_agent_stream;
  agent->stream_text = cai_agent_stream_text;
  agent->open_text_source = cai_agent_open_text_source;
  agent->send_text = cai_agent_send_text;
  agent->last_usage = cai_agent_last_usage;
  agent->set_session_usage_limits = cai_agent_set_session_usage_limits;
  agent->usage = cai_agent_usage;
  agent->context_percent = cai_agent_context_percent;
  agent->close = cai_agent_destroy;
}

static void cai_session_init_methods(cai_session *session) {
  session->set_conversation_id = cai_session_set_conversation_id;
  session->set_conversation = cai_session_set_conversation;
  session->conversation_id = cai_session_conversation_id;
  session->set_previous_response_id = cai_session_set_previous_response_id;
  session->previous_response_id = cai_session_previous_response_id;
  session->add_user_text = cai_session_add_user_text;
  session->add_user_text_spooled = cai_session_add_user_text_spooled;
  session->add_user_text_source = cai_session_add_user_text_source;
  session->add_user_image_url = cai_session_add_user_image_url;
  session->add_user_file_data_spooled = cai_session_add_user_file_data_spooled;
  session->add_user_file_source = cai_session_add_user_file_source;
  session->add_user_file_path = cai_session_add_user_file_path;
  session->add_function_call_output = cai_session_add_function_call_output;
  session->run = cai_session_run;
  session->run_output = cai_session_run_output;
  session->run_auto = cai_session_run_auto;
  session->run_auto_output = cai_session_run_auto_output;
  session->stream_auto = cai_session_stream_auto;
  session->stream = cai_session_stream;
  session->stream_text = cai_session_stream_text;
  session->open_text_source = cai_session_open_text_source;
  session->send_text = cai_session_send_text;
  session->last_usage = cai_session_last_usage;
  session->set_usage_limits = cai_session_set_usage_limits;
  session->usage = cai_session_usage;
  session->close_with_usage = cai_session_close_with_usage;
  session->context_window_tokens = cai_session_context_window_tokens;
  session->auto_compact_token_limit = cai_session_auto_compact_token_limit;
  session->context_percent = cai_session_context_percent;
  session->history_spilled = cai_session_history_spilled;
  session->export_history_source = cai_session_export_history_source;
  session->import_history_source = cai_session_import_history_source;
  session->export_state_source = cai_session_export_state_source;
  session->import_state_source = cai_session_import_state_source;
  session->save_state_path = cai_session_save_state_path;
  session->load_state_path = cai_session_load_state_path;
  session->close = cai_session_destroy;
}

static int cai_agent_default_session(cai_agent *agent, cai_session **out,
                                     cai_error *error) {
  cai_agent_impl *impl;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session output pointer is required");
  }
  *out = NULL;
  if (agent == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  impl = CAI_AGENT_IMPL(agent);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is closed");
  }
  if (impl->default_session == NULL) {
    rc = cai_agent_new_session(agent, &impl->default_session, error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  *out = impl->default_session;
  return CAI_OK;
}

static int cai_agent_add_user_text(cai_agent *agent, const char *text,
                                   cai_error *error) {
  cai_session *session;
  int rc;

  rc = cai_agent_default_session(agent, &session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_session_add_user_text(session, text, error);
}

static int cai_agent_add_user_text_spooled(cai_agent *agent,
                                           lonejson_spooled *text,
                                           cai_error *error) {
  cai_session *session;
  int rc;

  rc = cai_agent_default_session(agent, &session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_session_add_user_text_spooled(session, text, error);
}

static int cai_agent_add_user_text_source(cai_agent *agent, cai_source *source,
                                          cai_error *error) {
  cai_session *session;
  int rc;

  rc = cai_agent_default_session(agent, &session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_session_add_user_text_source(session, source, error);
}

static int cai_agent_add_user_image_url(cai_agent *agent, const char *url,
                                        const char *detail, cai_error *error) {
  cai_session *session;
  int rc;

  rc = cai_agent_default_session(agent, &session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_session_add_user_image_url(session, url, detail, error);
}

static int cai_agent_add_user_file_data_spooled(cai_agent *agent,
                                                const char *filename,
                                                lonejson_spooled *file_data,
                                                const char *detail,
                                                cai_error *error) {
  cai_session *session;
  int rc;

  rc = cai_agent_default_session(agent, &session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_session_add_user_file_data_spooled(session, filename, file_data,
                                                detail, error);
}

static int cai_agent_add_user_file_source(cai_agent *agent,
                                          const char *filename,
                                          cai_source *source,
                                          const char *detail,
                                          cai_error *error) {
  cai_session *session;
  int rc;

  rc = cai_agent_default_session(agent, &session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_session_add_user_file_source(session, filename, source, detail,
                                          error);
}

static int cai_agent_add_user_file_path(cai_agent *agent, const char *path,
                                        const char *filename,
                                        const char *detail, cai_error *error) {
  cai_session *session;
  int rc;

  rc = cai_agent_default_session(agent, &session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_session_add_user_file_path(session, path, filename, detail, error);
}

static int cai_agent_run(cai_agent *agent, cai_response **out,
                         cai_error *error) {
  cai_session *session;
  int rc;

  rc = cai_agent_default_session(agent, &session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_session_run(session, out, error);
}

static int cai_agent_run_output(cai_agent *agent, cai_output **out,
                                cai_error *error) {
  cai_session *session;
  int rc;

  rc = cai_agent_default_session(agent, &session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_session_run_output(session, out, error);
}

static int cai_agent_run_auto(cai_agent *agent, const cai_run_options *options,
                              cai_response **out, cai_error *error) {
  cai_session *session;
  int rc;

  rc = cai_agent_default_session(agent, &session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_session_run_auto(session, options, out, error);
}

static int cai_agent_run_auto_output(cai_agent *agent,
                                     const cai_run_options *options,
                                     cai_output **out, cai_error *error) {
  cai_session *session;
  int rc;

  rc = cai_agent_default_session(agent, &session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_session_run_auto_output(session, options, out, error);
}

static int cai_agent_stream_auto(cai_agent *agent,
                                 const cai_run_options *options,
                                 const cai_stream_sinks *sinks,
                                 cai_error *error) {
  cai_session *session;
  int rc;

  rc = cai_agent_default_session(agent, &session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_session_stream_auto(session, options, sinks, error);
}

static int cai_agent_stream_text(cai_agent *agent, cai_sink *sink,
                                 cai_error *error) {
  cai_stream_sinks sinks;

  cai_stream_sinks_init(&sinks);
  sinks.output_text = sink;
  return cai_agent_stream(agent, &sinks, error);
}

static int cai_agent_stream(cai_agent *agent, const cai_stream_sinks *sinks,
                            cai_error *error) {
  cai_session *session;
  int rc;

  rc = cai_agent_default_session(agent, &session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_session_stream(session, sinks, error);
}

static int cai_agent_open_text_source(cai_agent *agent, cai_source **out,
                                      cai_error *error) {
  cai_session *session;
  int rc;

  rc = cai_agent_default_session(agent, &session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_session_open_text_source(session, out, error);
}

static int cai_agent_send_text(cai_agent *agent, const char *text,
                               cai_response **out, cai_error *error) {
  cai_session *session;
  int rc;

  rc = cai_agent_default_session(agent, &session, error);
  if (rc != CAI_OK) {
    return rc;
  }
  return cai_session_send_text(session, text, out, error);
}

static int cai_agent_last_usage(const cai_agent *agent, cai_token_usage *out,
                                cai_error *error) {
  const cai_agent_impl *impl;

  if (agent == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  impl = CAI_AGENT_IMPL(agent);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is closed");
  }
  if (impl->default_session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "agent has no default session usage");
  }
  return cai_session_last_usage(impl->default_session, out, error);
}

int cai_agent_set_session_usage_limits(cai_agent *agent,
                                       const cai_usage_limits *limits,
                                       cai_error *error) {
  cai_agent_impl *impl;
  cai_usage_limits empty;
  int rc;

  if (agent == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  impl = CAI_AGENT_IMPL(agent);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is closed");
  }
  if (limits == NULL) {
    cai_usage_limits_init(&empty);
    limits = &empty;
  }
  rc = cai_usage_limits_validate(limits, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_usage_limits_require_pricing(limits, impl->model, error);
  if (rc != CAI_OK) {
    return rc;
  }
  impl->session_usage_limits = *limits;
  if (impl->default_session != NULL) {
    rc = cai_session_set_usage_limits(impl->default_session, limits, error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  return CAI_OK;
}

int cai_agent_usage(const cai_agent *agent, cai_usage_accounting *out,
                    cai_error *error) {
  const cai_agent_impl *impl;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "usage accounting output pointer is required");
  }
  cai_usage_accounting_init(out);
  if (agent == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  impl = CAI_AGENT_IMPL(agent);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is closed");
  }
  if (impl->default_session == NULL) {
    return CAI_OK;
  }
  return cai_session_usage(impl->default_session, out, error);
}

static int cai_agent_context_percent(const cai_agent *agent, double *out,
                                     cai_error *error) {
  const cai_agent_impl *impl;

  if (agent == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  impl = CAI_AGENT_IMPL(agent);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is closed");
  }
  if (impl->default_session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "agent has no default session usage");
  }
  return cai_session_context_percent(impl->default_session, out, error);
}
