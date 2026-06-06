/** @file cai/cai.h
 *  Public C89 API for cai OpenAI Responses clients, sessions, streams,
 *  tool registration, hosted tools, sources, sinks, and conversations.
 */
#ifndef CAI_CAI_H
#define CAI_CAI_H

#include <cai/models.h>
#include <cai/version.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Forward declaration for lockd/lonejson sources. */
struct lc_source;
/** Forward declaration for lockd/lonejson sinks. */
struct lc_sink;
/** Forward declaration for lonejson typed maps. */
struct lonejson_map;
/** Forward declaration for lonejson spooled values. */
struct lonejson_spooled;
/** Forward declaration for pslog loggers. */
struct pslog_logger;

/** OpenAI API client handle. */
typedef struct cai_client cai_client;
/** Agent facade handle that owns config, tools, and an implicit session. */
typedef struct cai_agent cai_agent;
/** Stateful Responses session handle. */
typedef struct cai_session cai_session;
/** Conversation handle wrapping a server-side conversation id. */
typedef struct cai_conversation cai_conversation;
/** Streaming byte input handle. */
typedef struct cai_source cai_source;
/** Streaming byte output handle. */
typedef struct cai_sink cai_sink;
/** High-level response output wrapper. */
typedef struct cai_output cai_output;
/** Low-level Responses create parameter builder. */
typedef struct cai_response_create_params cai_response_create_params;
/** Low-level Responses API response handle. */
typedef struct cai_response cai_response;
/** List response for response/conversation input items. */
typedef struct cai_input_item_list cai_input_item_list;
/** Retrieved conversation item handle. */
typedef struct cai_conversation_item cai_conversation_item;
/** Conversation items create parameter builder. */
typedef struct cai_conversation_items_params cai_conversation_items_params;
/** Registry of callable local cai tools. */
typedef struct cai_tool_registry cai_tool_registry;
/** Builder for JSON schema used by function tools. */
typedef struct cai_tool_schema cai_tool_schema;
/** ChatGPT OAuth auth session from cai/auth.h. */
typedef struct cai_chatgpt_auth cai_chatgpt_auth;

/** cai status codes returned by public APIs. */
typedef enum cai_status {
  /** Operation completed successfully. */
  CAI_OK = 0,
  /** Invalid argument, state, configuration, or input data. */
  CAI_ERR_INVALID = 1,
  /** Allocation failed. */
  CAI_ERR_NOMEM = 2,
  /** Transport layer failed. */
  CAI_ERR_TRANSPORT = 3,
  /** Protocol parsing or validation failed. */
  CAI_ERR_PROTOCOL = 4,
  /** Server returned an error response. */
  CAI_ERR_SERVER = 5,
  /** Operation was cancelled. */
  CAI_ERR_CANCELLED = 6,
  /** Configured usage or spend budget was exceeded. */
  CAI_ERR_LIMIT = 7
} cai_status;

/** Detailed error information populated by failing cai APIs. */
typedef struct cai_error {
  /** cai_status code for the failure. */
  int code;
  /** HTTP status from server responses, or zero when not applicable. */
  long http_status;
  /** Human-readable error message owned by the cai_error. */
  char *message;
  /** Additional diagnostic detail owned by the cai_error. */
  char *detail;
  /** Server error code owned by the cai_error when provided. */
  char *server_code;
  /** Server request id owned by the cai_error when provided. */
  char *request_id;
} cai_error;

/** Custom allocation callback. */
typedef void *(*cai_malloc_fn)(void *context, size_t size);
/** Custom reallocation callback. */
typedef void *(*cai_realloc_fn)(void *context, void *ptr, size_t size);
/** Custom free callback. */
typedef void (*cai_free_fn)(void *context, void *ptr);

/** Optional allocator callback table used by cai objects. */
typedef struct cai_allocator {
  /** Allocation callback. */
  cai_malloc_fn malloc_fn;
  /** Reallocation callback. */
  cai_realloc_fn realloc_fn;
  /** Free callback. */
  cai_free_fn free_fn;
  /** User context passed to allocator callbacks. */
  void *context;
} cai_allocator;

/** Default OpenAI REST API base URL. */
#define CAI_OPENAI_BASE_URL "https://api.openai.com/v1"
/** Default ChatGPT subscription/Codex backend base URL. */
#define CAI_CHATGPT_CODEX_BASE_URL "https://chatgpt.com/backend-api/codex"
/** Default OpenRouter OpenAI-compatible REST API base URL. */
#define CAI_OPENROUTER_BASE_URL "https://openrouter.ai/api/v1"
/** Default dotenv path used by explicit dotenv helper calls. */
#define CAI_DEFAULT_DOTENV_PATH ".env"
/** Environment variable name for OpenAI API keys. */
#define CAI_OPENAI_API_KEY_ENV "OPENAI_API_KEY"
/** Environment variable name for OpenRouter API keys. */
#define CAI_OPENROUTER_API_KEY_ENV "OPENROUTER_API_KEY"

/** Token usage counters reported by Responses. */
typedef struct cai_token_usage {
  /** Input tokens charged for the request. */
  long long input_tokens;
  /** Input tokens served from prompt cache. */
  long long input_cached_tokens;
  /** Output tokens produced by the model. */
  long long output_tokens;
  /** Output tokens attributed to reasoning. */
  long long output_reasoning_tokens;
  /** Total tokens reported by the API. */
  long long total_tokens;
} cai_token_usage;

/** Cumulative token and USD spend limits; zero means unset. */
typedef struct cai_usage_limits {
  /** Maximum cumulative input tokens; zero disables this limit. */
  long long max_input_tokens;
  /** Maximum cumulative cached input tokens; zero disables this limit. */
  long long max_input_cached_tokens;
  /** Maximum cumulative output tokens; zero disables this limit. */
  long long max_output_tokens;
  /** Maximum cumulative output reasoning tokens; zero disables this limit. */
  long long max_output_reasoning_tokens;
  /** Maximum cumulative total tokens; zero disables this limit. */
  long long max_total_tokens;
  /** Maximum estimated cumulative spend in USD; zero disables this limit. */
  double max_spend_usd;
} cai_usage_limits;

/** Cumulative usage and estimated USD spend recorded by a client or session. */
typedef struct cai_usage_accounting {
  /** Cumulative token usage. */
  cai_token_usage usage;
  /** Cumulative estimated spend in USD using cai's model price table. */
  double estimated_spend_usd;
  /** Non-zero once a configured limit has been exceeded. */
  int limit_exceeded;
} cai_usage_accounting;

/** Configuration for opening a cai client. */
typedef struct cai_client_config {
  /** API key string; when NULL cai looks at api_key_env. */
  const char *api_key;
  /** Environment variable to read when api_key is NULL. */
  const char *api_key_env;
  /**
   * API base URL. NULL selects CAI_CHATGPT_CODEX_BASE_URL when chatgpt_auth is
   * set, otherwise CAI_OPENAI_BASE_URL.
   */
  const char *base_url;
  /** Optional OpenAI organization id header. */
  const char *organization_id;
  /** Optional OpenAI project id header. */
  const char *project_id;
  /** HTTP timeout in milliseconds; zero uses default. */
  long timeout_ms;
  /** Non-zero disables HTTP/2 negotiation. */
  int http_2_disabled;
  /** Non-zero disables TLS certificate verification. */
  int insecure_skip_verify;
  /** Maximum non-streamed JSON response bytes; zero uses default. */
  size_t json_response_limit_bytes;
  /** Optional pslog logger. */
  struct pslog_logger *logger;
  /** Non-zero disables cai logging even when logger is set. */
  int logger_disabled;
  /** Optional ChatGPT OAuth auth session; when set, supplies bearer tokens. */
  cai_chatgpt_auth *chatgpt_auth;
  /** Optional cumulative usage and USD spend limits for the client. */
  cai_usage_limits usage_limits;
  /** Optional custom allocator callbacks. */
  cai_allocator allocator;
} cai_client_config;

/**
 * Use previous_response_id/server-side state for session continuity on
 * providers that support it. OpenAI and ChatGPT subscription streaming use
 * the Responses WebSocket transport when available.
 */
#define CAI_SESSION_CONTINUITY_SERVER 0
/** Use local client history when creating follow-up requests. */
#define CAI_SESSION_CONTINUITY_CLIENT_HISTORY 1
/** Auto-select continuity mode based on provider behavior. */
#define CAI_SESSION_CONTINUITY_AUTO 2

/** Configuration for constructing an agent facade. */
typedef struct cai_agent_config {
  /** Model id for Responses requests. */
  const char *model;
  /** Developer instructions sent as Responses instructions. */
  const char *developer_instructions;
  /** Optional prompt cache key. */
  const char *prompt_cache_key;
  /** Tool choice string such as CAI_TOOL_CHOICE_AUTO. */
  const char *tool_choice;
  /** Raw JSON tool_choice override for provider-specific values. */
  const char *tool_choice_json;
  /** Reasoning effort string such as CAI_REASONING_EFFORT_LOW. */
  const char *reasoning_effort;
  /** Reasoning summary mode such as CAI_REASONING_SUMMARY_AUTO. */
  const char *reasoning_summary;
  /** Structured output schema name. */
  const char *text_format_name;
  /** Structured output schema description. */
  const char *text_format_description;
  /** Structured output JSON schema. */
  const char *text_format_schema_json;
  /** Non-zero requests strict structured output validation. */
  int text_format_strict;
  /** Maximum output tokens; zero leaves unset. */
  int max_output_tokens;
  /** Maximum tool calls; zero leaves unset. */
  int max_tool_calls;
  /** Non-zero disables parallel tool calls. */
  int disable_parallel_tool_calls;
  /** CAI_SESSION_CONTINUITY_* mode. */
  int session_continuity;
  /** Non-zero disables default server-side auto-compaction. */
  int disable_auto_compaction;
  /** Explicit server-side compaction token threshold; zero uses default. */
  long long compact_threshold_tokens;
  /** Percent of known context window for compaction; zero uses default. */
  unsigned int compact_threshold_percent;
  /** Non-zero records local history for export/import or client mode. */
  int enable_local_history;
  /** Local history memory limit before spilling; zero uses default. */
  size_t history_memory_limit;
  /** Optional local history spool directory. */
  const char *history_spool_dir;
  /** Optional usage and USD spend limits inherited by new sessions. */
  cai_usage_limits session_usage_limits;
} cai_agent_config;

/** Low text verbosity setting for supported models. */
#define CAI_TEXT_VERBOSITY_LOW "low"
/** Medium text verbosity setting for supported models. */
#define CAI_TEXT_VERBOSITY_MEDIUM "medium"
/** High text verbosity setting for supported models. */
#define CAI_TEXT_VERBOSITY_HIGH "high"

/** Responses truncation mode that lets the API decide. */
#define CAI_RESPONSE_TRUNCATION_AUTO "auto"
/** Responses truncation mode that disables truncation. */
#define CAI_RESPONSE_TRUNCATION_DISABLED "disabled"

/** Service tier that lets the API select the tier. */
#define CAI_SERVICE_TIER_AUTO "auto"
/** Default service tier. */
#define CAI_SERVICE_TIER_DEFAULT "default"
/** Flex service tier. */
#define CAI_SERVICE_TIER_FLEX "flex"
/** Priority service tier. */
#define CAI_SERVICE_TIER_PRIORITY "priority"

/** Hosted OpenAI web search tool type. */
#define CAI_HOSTED_TOOL_WEB_SEARCH "web_search"
/** Hosted OpenAI file search tool type. */
#define CAI_HOSTED_TOOL_FILE_SEARCH "file_search"
/** Hosted OpenAI MCP connector tool type. */
#define CAI_HOSTED_TOOL_MCP "mcp"
/** Hosted OpenAI computer-use preview tool type. */
#define CAI_HOSTED_TOOL_COMPUTER_USE "computer_use_preview"
/** Hosted OpenAI image generation tool type. */
#define CAI_HOSTED_TOOL_IMAGE_GENERATION "image_generation"
/** Hosted OpenAI code interpreter tool type. */
#define CAI_HOSTED_TOOL_CODE_INTERPRETER "code_interpreter"
/** Hosted OpenAI tool search tool type. */
#define CAI_HOSTED_TOOL_TOOL_SEARCH "tool_search"

/** Disable reasoning effort where supported. */
#define CAI_REASONING_EFFORT_NONE "none"
/** Minimal reasoning effort. */
#define CAI_REASONING_EFFORT_MINIMAL "minimal"
/** Low reasoning effort. */
#define CAI_REASONING_EFFORT_LOW "low"
/** Medium reasoning effort. */
#define CAI_REASONING_EFFORT_MEDIUM "medium"
/** High reasoning effort. */
#define CAI_REASONING_EFFORT_HIGH "high"
/** Extra-high reasoning effort. */
#define CAI_REASONING_EFFORT_XHIGH "xhigh"

/** Let the API decide whether to stream reasoning summaries. */
#define CAI_REASONING_SUMMARY_AUTO "auto"
/** Concise reasoning summary mode. */
#define CAI_REASONING_SUMMARY_CONCISE "concise"
/** Detailed reasoning summary mode. */
#define CAI_REASONING_SUMMARY_DETAILED "detailed"

/** Automatic tool choice. */
#define CAI_TOOL_CHOICE_AUTO "auto"
/** Disable tool calls. */
#define CAI_TOOL_CHOICE_NONE "none"
/** Require a tool call. */
#define CAI_TOOL_CHOICE_REQUIRED "required"

/** Configuration for hosted MCP tools handled by the Responses API. */
typedef struct cai_hosted_mcp_tool_config {
  /** Server label exposed to the API. */
  const char *server_label;
  /** Remote MCP server URL. */
  const char *server_url;
  /** Optional hosted connector id. */
  const char *connector_id;
  /** Optional server description. */
  const char *server_description;
  /** Optional raw JSON object of headers for the hosted MCP server. */
  const char *headers_json;
  /** Optional raw JSON allowed_tools value. */
  const char *allowed_tools_json;
  /** Optional allow-list of hosted tool names. */
  const char *const *allowed_tool_names;
  /** Number of entries in allowed_tool_names. */
  size_t allowed_tool_name_count;
  /** Optional raw JSON require_approval policy. */
  const char *require_approval_json;
} cai_hosted_mcp_tool_config;

/** Tool event emitted before a local tool runs. */
#define CAI_TOOL_EVENT_START 1
/** Tool event emitted after local tool output is available. */
#define CAI_TOOL_EVENT_OUTPUT 2
/** Tool event emitted when a local tool fails. */
#define CAI_TOOL_EVENT_ERROR 3

/** Observable tool execution event for auto-run sessions and agents. */
typedef struct cai_tool_event {
  /** CAI_TOOL_EVENT_* event type. */
  int type;
  /** Tool name. */
  const char *name;
  /** Tool arguments as JSON when available as a string. */
  const char *arguments_json;
  /** Tool arguments as a spooled JSON value when large. */
  const struct lonejson_spooled *arguments_json_spooled;
  /** Tool output as a spooled JSON value for output events. */
  const struct lonejson_spooled *output_json;
  /** Tool error for error events. */
  const cai_error *tool_error;
} cai_tool_event;

/** Callback invoked for tool start/output/error events. */
typedef int (*cai_tool_event_fn)(void *context, const cai_tool_event *event,
                                 cai_error *error);

/** Common cursor pagination parameters. */
typedef struct cai_list_params {
  /** Cursor id after which to list. */
  const char *after;
  /** Maximum number of items to return; zero leaves unset. */
  int limit;
  /** Sort order string accepted by the API. */
  const char *order;
} cai_list_params;

/** Options controlling local tool auto-run loops. */
typedef struct cai_run_options {
  /** Maximum auto-run tool rounds; zero uses default. */
  int max_tool_rounds;
  /** Non-zero disables local tool auto-run. */
  int disable_tool_auto_run;
  /** In-memory tool output spool limit before spilling. */
  size_t tool_output_memory_limit;
  /** Maximum accepted tool output bytes; zero uses default. */
  size_t tool_output_max_bytes;
  /** Optional directory for tool output spill files. */
  const char *tool_spool_dir;
  /** Optional callback for observable tool events. */
  cai_tool_event_fn tool_event;
  /** Context passed to tool_event. */
  void *tool_event_context;
} cai_run_options;

/** Source read callback. Returns bytes read, 0 for EOF or error. */
typedef size_t (*cai_source_read_fn)(void *context, void *buffer, size_t count,
                                     cai_error *error);
/** Source reset callback for seekable/replayable sources. */
typedef int (*cai_source_reset_fn)(void *context, cai_error *error);
/** Source close callback. */
typedef void (*cai_source_close_fn)(void *context);

/** Callback table for custom pull-based byte sources. */
typedef struct cai_source_callbacks {
  /** Read bytes into buffer. */
  cai_source_read_fn read;
  /** Reset source to the beginning, or NULL if unsupported. */
  cai_source_reset_fn reset;
  /** Close callback invoked by cai_source_close. */
  cai_source_close_fn close;
  /** User context passed to callbacks. */
  void *context;
} cai_source_callbacks;

/** Sink write callback. */
typedef int (*cai_sink_write_fn)(void *context, const void *bytes, size_t count,
                                 cai_error *error);
/** Sink close callback. */
typedef void (*cai_sink_close_fn)(void *context);

/** Callback table for custom streaming output sinks. */
typedef struct cai_sink_callbacks {
  /** Write bytes from a streaming producer. */
  cai_sink_write_fn write;
  /** Close callback invoked by cai_sink_close. */
  cai_sink_close_fn close;
  /** User context passed to callbacks. */
  void *context;
} cai_sink_callbacks;

/** Streaming byte input handle with receiver methods for pull-based reads. */
struct cai_source {
  /** Read bytes from the source. */
  size_t (*read)(cai_source *source, void *buffer, size_t count,
                 cai_error *error);
  /** Reset the source to the beginning when supported. */
  int (*reset)(cai_source *source, cai_error *error);
  /** Stream all bytes from this source into a sink. */
  int (*copy_to_sink)(cai_source *source, cai_sink *sink, cai_error *error);
  /** Close and destroy this source. */
  void (*close)(cai_source *source);
  /** Callback implementation backing this source. */
  cai_source_callbacks callbacks;
};

/** Streaming byte output handle with receiver methods for push-based writes. */
struct cai_sink {
  /** Write bytes to the sink. */
  int (*write)(cai_sink *sink, const void *bytes, size_t count,
               cai_error *error);
  /** Close and destroy this sink. */
  void (*close)(cai_sink *sink);
  /** Callback implementation backing this sink. */
  cai_sink_callbacks callbacks;
};

/** Callback returning dynamic stream prefix/suffix text. */
typedef const char *(*cai_stream_affix_fn)(void *context);
/** Callback for incremental function-call argument deltas. */
typedef int (*cai_stream_function_call_delta_fn)(
    void *context, const char *item_id, int output_index,
    const struct lonejson_spooled *delta, cai_error *error);
/** Callback for completed streamed function-call arguments. */
typedef int (*cai_stream_function_call_done_fn)(
    void *context, const char *item_id, int output_index, const char *call_id,
    const char *name, const struct lonejson_spooled *arguments,
    cai_error *error);
/** Callback for completed streamed output items. */
typedef int (*cai_stream_output_item_done_fn)(
    void *context, const char *item_id, int output_index, const char *type,
    const struct lonejson_spooled *item_json, cai_error *error);
/** Callback for incremental output text deltas. */
typedef int (*cai_stream_output_text_delta_fn)(
    void *context, const char *item_id, int output_index,
    const struct lonejson_spooled *delta, cai_error *error);

/** Static or dynamic prefix/suffix emitted around stream sections. */
typedef struct cai_stream_affix {
  /** Static text to emit, or NULL. */
  const char *text;
  /** Dynamic callback used when text is NULL. */
  cai_stream_affix_fn callback;
  /** Context passed to callback. */
  void *context;
} cai_stream_affix;

/** Stream routing table for reasoning summaries, text, and tool calls. */
typedef struct cai_stream_sinks {
  /** Sink for reasoning summary deltas. */
  cai_sink *reasoning_summary;
  /** Sink for final output text deltas. */
  cai_sink *output_text;
  /** Prefix emitted before each reasoning summary segment. */
  cai_stream_affix reasoning_summary_prefix;
  /** Suffix emitted after each reasoning summary segment. */
  cai_stream_affix reasoning_summary_suffix;
  /** Prefix emitted before the first output text segment. */
  cai_stream_affix output_text_prefix;
  /** Suffix emitted after output text completes. */
  cai_stream_affix output_text_suffix;
  /** Callback for streamed function-call argument deltas. */
  cai_stream_function_call_delta_fn function_call_arguments_delta;
  /** Callback for completed function-call arguments. */
  cai_stream_function_call_done_fn function_call_arguments_done;
  /** Context passed to function-call callbacks. */
  void *function_call_context;
  /** Callback for completed output items. */
  cai_stream_output_item_done_fn output_item_done;
  /** Context passed to output_item_done. */
  void *output_item_context;
  /** Callback for output text deltas in addition to output_text sink. */
  cai_stream_output_text_delta_fn output_text_delta;
  /** Context passed to output_text_delta. */
  void *output_text_context;
} cai_stream_sinks;

/** Typed lonejson-backed tool callback. */
typedef int (*cai_tool_fn)(void *context, const void *params, void *result,
                           cai_error *error);
/** Alias for typed lonejson-backed tool callbacks. */
typedef cai_tool_fn cai_tool_lonejson_fn;
/** Raw JSON tool callback for small argument strings. */
typedef int (*cai_tool_raw_fn)(void *context, const char *arguments_json,
                               cai_sink *output, cai_error *error);
/** Spooled raw JSON tool callback for large argument values. */
typedef int (*cai_tool_raw_spooled_fn)(void *context,
                                       struct lonejson_spooled *arguments_json,
                                       cai_sink *output, cai_error *error);

/** Duplicate a string using cai ownership for typed tool results. */
char *cai_tool_result_strdup(const char *value, cai_error *error);
/** Set a typed tool result field from a file path source. */
int cai_tool_result_set_source_path(const struct lonejson_map *result_map,
                                    void *result, const char *field_name,
                                    const char *path, cai_error *error);
/** Set a typed tool result field from a spooled JSON/string value. */
int cai_tool_result_set_spooled(const struct lonejson_map *result_map,
                                void *result, const char *field_name,
                                struct lonejson_spooled *spool,
                                cai_error *error);

/** Public method-table facade for a cai client. */
struct cai_client {
  /** Create a new agent bound to this client. */
  int (*new_agent)(cai_client *client, const cai_agent_config *config,
                   cai_agent **out, cai_error *error);
  /** Create a Responses API response. */
  int (*create_response)(cai_client *client,
                         const cai_response_create_params *params,
                         cai_response **out, cai_error *error);
  /** Count input tokens for a Responses request. */
  int (*count_response_input_tokens)(cai_client *client,
                                     const cai_response_create_params *params,
                                     cai_token_usage *out, cai_error *error);
  /** Stream response output text to a sink. */
  int (*stream_response_text)(cai_client *client,
                              const cai_response_create_params *params,
                              cai_sink *sink, cai_error *error);
  /** Open response output text as a streaming source. */
  int (*open_response_text_source)(cai_client *client,
                                   const cai_response_create_params *params,
                                   cai_source **out, cai_error *error);
  /** Retrieve a stored response by id. */
  int (*retrieve_response)(cai_client *client, const char *response_id,
                           cai_response **out, cai_error *error);
  /** Cancel a background response by id. */
  int (*cancel_response)(cai_client *client, const char *response_id,
                         cai_response **out, cai_error *error);
  /** Delete a stored response by id. */
  int (*delete_response)(cai_client *client, const char *response_id,
                         cai_error *error);
  /** List input items for a response. */
  int (*list_response_input_items)(cai_client *client, const char *response_id,
                                   const cai_list_params *params,
                                   cai_input_item_list **out, cai_error *error);
  /** Create a new server-side conversation. */
  int (*create_conversation)(cai_client *client, cai_conversation **out,
                             cai_error *error);
  /** Retrieve a conversation by id. */
  int (*retrieve_conversation)(cai_client *client, const char *conversation_id,
                               cai_conversation **out, cai_error *error);
  /** Retrieve a conversation by handle. */
  int (*retrieve_conversation_handle)(cai_client *client,
                                      const cai_conversation *conversation,
                                      cai_conversation **out, cai_error *error);
  /** Update conversation metadata by id. */
  int (*update_conversation_metadata)(cai_client *client,
                                      const char *conversation_id,
                                      const char *metadata_json,
                                      cai_conversation **out, cai_error *error);
  /** Update conversation metadata by handle. */
  int (*update_conversation_metadata_handle)(
      cai_client *client, const cai_conversation *conversation,
      const char *metadata_json, cai_conversation **out, cai_error *error);
  /** Delete a conversation by id. */
  int (*delete_conversation)(cai_client *client, const char *conversation_id,
                             cai_error *error);
  /** Delete a conversation by handle. */
  int (*delete_conversation_handle)(cai_client *client,
                                    const cai_conversation *conversation,
                                    cai_error *error);
  /** List conversation items by conversation id. */
  int (*list_conversation_items)(cai_client *client,
                                 const char *conversation_id,
                                 const cai_list_params *params,
                                 cai_input_item_list **out, cai_error *error);
  /** List conversation items by handle. */
  int (*list_conversation_items_handle)(cai_client *client,
                                        const cai_conversation *conversation,
                                        const cai_list_params *params,
                                        cai_input_item_list **out,
                                        cai_error *error);
  /** Delete one conversation item by ids. */
  int (*delete_conversation_item)(cai_client *client,
                                  const char *conversation_id,
                                  const char *item_id, cai_error *error);
  /** Delete one conversation item by conversation handle. */
  int (*delete_conversation_item_handle)(cai_client *client,
                                         const cai_conversation *conversation,
                                         const char *item_id, cai_error *error);
  /** Retrieve one conversation item by ids. */
  int (*retrieve_conversation_item)(cai_client *client,
                                    const char *conversation_id,
                                    const char *item_id,
                                    cai_conversation_item **out,
                                    cai_error *error);
  /** Retrieve one conversation item by conversation handle. */
  int (*retrieve_conversation_item_handle)(cai_client *client,
                                           const cai_conversation *conversation,
                                           const char *item_id,
                                           cai_conversation_item **out,
                                           cai_error *error);
  /** Create conversation items by conversation id. */
  int (*create_conversation_items)(cai_client *client,
                                   const char *conversation_id,
                                   const cai_conversation_items_params *params,
                                   cai_input_item_list **out, cai_error *error);
  /** Create conversation items by conversation handle. */
  int (*create_conversation_items_handle)(
      cai_client *client, const cai_conversation *conversation,
      const cai_conversation_items_params *params, cai_input_item_list **out,
      cai_error *error);
  /** Replace cumulative usage and USD spend limits for this client. */
  int (*set_usage_limits)(cai_client *client, const cai_usage_limits *limits,
                          cai_error *error);
  /** Return cumulative usage and estimated USD spend for this client. */
  int (*usage)(const cai_client *client, cai_usage_accounting *out,
               cai_error *error);
  /** Close and destroy the client. */
  void (*close)(cai_client *client);
  /** Private implementation pointer; do not access directly. */
  void *impl;
};

/** Public method-table facade for a high-level agent. */
struct cai_agent {
  /** Register a typed lonejson-backed local tool. */
  int (*register_tool)(cai_agent *agent, const char *name,
                       const char *description,
                       const struct lonejson_map *params_map,
                       const struct lonejson_map *result_map,
                       cai_tool_fn callback, void *context, cai_error *error);
  /** Register a raw JSON local tool. */
  int (*register_raw_tool)(cai_agent *agent, const char *name,
                           const char *description, const char *schema_json,
                           int strict, cai_tool_raw_fn callback, void *context,
                           cai_error *error);
  /** Register a spooled raw JSON local tool. */
  int (*register_raw_spooled_tool)(cai_agent *agent, const char *name,
                                   const char *description,
                                   const char *schema_json, int strict,
                                   cai_tool_raw_spooled_fn callback,
                                   void *context, cai_error *error);
  /** Add a provider-hosted tool described by raw JSON. */
  int (*add_hosted_tool_json)(cai_agent *agent, const char *tool_json,
                              cai_error *error);
  /** Add a simple provider-hosted tool by type string. */
  int (*add_simple_hosted_tool)(cai_agent *agent, const char *type,
                                cai_error *error);
  /** Add a provider-hosted MCP tool configuration. */
  int (*add_hosted_mcp_tool)(cai_agent *agent,
                             const cai_hosted_mcp_tool_config *config,
                             cai_error *error);
  /** Create an explicit session from this agent. */
  int (*new_session)(cai_agent *agent, cai_session **out, cai_error *error);
  /** Create an explicit session with a new server-side conversation. */
  int (*new_conversation_session)(cai_agent *agent, cai_session **out,
                                  cai_error *error);
  /** Create an explicit session bound to an existing conversation. */
  int (*new_session_for_conversation)(cai_agent *agent,
                                      const cai_conversation *conversation,
                                      cai_session **out, cai_error *error);
  /** Add user text to the agent's implicit session. */
  int (*add_user_text)(cai_agent *agent, const char *text, cai_error *error);
  /** Add spooled user text to the agent's implicit session. */
  int (*add_user_text_spooled)(cai_agent *agent, struct lonejson_spooled *text,
                               cai_error *error);
  /** Read a source into spooled user text for the implicit session. */
  int (*add_user_text_source)(cai_agent *agent, cai_source *source,
                              cai_error *error);
  /** Add a user image URL to the agent's implicit session. */
  int (*add_user_image_url)(cai_agent *agent, const char *url,
                            const char *detail, cai_error *error);
  /** Add spooled user file data to the agent's implicit session. */
  int (*add_user_file_data_spooled)(cai_agent *agent, const char *filename,
                                    struct lonejson_spooled *file_data,
                                    const char *detail, cai_error *error);
  /** Read a source into spooled user file data for the implicit session. */
  int (*add_user_file_source)(cai_agent *agent, const char *filename,
                              cai_source *source, const char *detail,
                              cai_error *error);
  /** Add a user file from a filesystem path to the implicit session. */
  int (*add_user_file_path)(cai_agent *agent, const char *path,
                            const char *filename, const char *detail,
                            cai_error *error);
  /** Run the implicit session once. */
  int (*run)(cai_agent *agent, cai_response **out, cai_error *error);
  /** Run the implicit session and return high-level output. */
  int (*run_output)(cai_agent *agent, cai_output **out, cai_error *error);
  /** Run the implicit session with local tool auto-run. */
  int (*run_auto)(cai_agent *agent, const cai_run_options *options,
                  cai_response **out, cai_error *error);
  /** Run with auto-run and return high-level output. */
  int (*run_auto_output)(cai_agent *agent, const cai_run_options *options,
                         cai_output **out, cai_error *error);
  /** Stream the implicit session with local tool auto-run. */
  int (*stream_auto)(cai_agent *agent, const cai_run_options *options,
                     const cai_stream_sinks *sinks, cai_error *error);
  /** Stream the implicit session without local tool auto-run. */
  int (*stream)(cai_agent *agent, const cai_stream_sinks *sinks,
                cai_error *error);
  /** Stream only output text to a sink. */
  int (*stream_text)(cai_agent *agent, cai_sink *sink, cai_error *error);
  /** Open output text as a source. */
  int (*open_text_source)(cai_agent *agent, cai_source **out, cai_error *error);
  /** Add one user text message and run the implicit session. */
  int (*send_text)(cai_agent *agent, const char *text, cai_response **out,
                   cai_error *error);
  /** Return last recorded usage for the implicit session. */
  int (*last_usage)(const cai_agent *agent, cai_token_usage *out,
                    cai_error *error);
  /** Replace usage and USD spend limits inherited by new sessions. */
  int (*set_session_usage_limits)(cai_agent *agent,
                                  const cai_usage_limits *limits,
                                  cai_error *error);
  /** Return cumulative usage for the implicit session. */
  int (*usage)(const cai_agent *agent, cai_usage_accounting *out,
               cai_error *error);
  /** Return estimated context-window percentage for the implicit session. */
  int (*context_percent)(const cai_agent *agent, double *out, cai_error *error);
  /** Close and destroy the agent. */
  void (*close)(cai_agent *agent);
  /** Private implementation pointer; do not access directly. */
  void *impl;
};

/** Public method-table facade for a stateful Responses session. */
struct cai_session {
  /** Bind the session to a server-side conversation id. */
  int (*set_conversation_id)(cai_session *session, const char *conversation_id,
                             cai_error *error);
  /** Bind the session to a conversation handle. */
  int (*set_conversation)(cai_session *session,
                          const cai_conversation *conversation,
                          cai_error *error);
  /** Return the bound conversation id, or NULL. */
  const char *(*conversation_id)(const cai_session *session);
  /** Set the previous response id used for server-side continuity. */
  int (*set_previous_response_id)(cai_session *session, const char *response_id,
                                  cai_error *error);
  /** Return the previous response id, or NULL. */
  const char *(*previous_response_id)(const cai_session *session);
  /** Add user text to the pending session input. */
  int (*add_user_text)(cai_session *session, const char *text,
                       cai_error *error);
  /** Add spooled user text to the pending session input. */
  int (*add_user_text_spooled)(cai_session *session,
                               struct lonejson_spooled *text, cai_error *error);
  /** Read a source into spooled user text for the pending session input. */
  int (*add_user_text_source)(cai_session *session, cai_source *source,
                              cai_error *error);
  /** Add an image URL to the pending session input. */
  int (*add_user_image_url)(cai_session *session, const char *url,
                            const char *detail, cai_error *error);
  /** Add spooled file data to the pending session input. */
  int (*add_user_file_data_spooled)(cai_session *session, const char *filename,
                                    struct lonejson_spooled *file_data,
                                    const char *detail, cai_error *error);
  /** Read a source into spooled file data for the pending session input. */
  int (*add_user_file_source)(cai_session *session, const char *filename,
                              cai_source *source, const char *detail,
                              cai_error *error);
  /** Add file data from a filesystem path to the pending input. */
  int (*add_user_file_path)(cai_session *session, const char *path,
                            const char *filename, const char *detail,
                            cai_error *error);
  /** Add local function-call output to the pending session input. */
  int (*add_function_call_output)(cai_session *session, const char *call_id,
                                  const char *output, cai_error *error);
  /** Run the pending session once. */
  int (*run)(cai_session *session, cai_response **out, cai_error *error);
  /** Run the session and return a high-level output wrapper. */
  int (*run_output)(cai_session *session, cai_output **out, cai_error *error);
  /** Run the session with local tool auto-run. */
  int (*run_auto)(cai_session *session, const cai_run_options *options,
                  cai_response **out, cai_error *error);
  /** Run with local tool auto-run and return high-level output. */
  int (*run_auto_output)(cai_session *session, const cai_run_options *options,
                         cai_output **out, cai_error *error);
  /** Stream the session with local tool auto-run. */
  int (*stream_auto)(cai_session *session, const cai_run_options *options,
                     const cai_stream_sinks *sinks, cai_error *error);
  /** Stream the session without local tool auto-run. */
  int (*stream)(cai_session *session, const cai_stream_sinks *sinks,
                cai_error *error);
  /** Stream only output text to a sink. */
  int (*stream_text)(cai_session *session, cai_sink *sink, cai_error *error);
  /** Open output text as a streaming source. */
  int (*open_text_source)(cai_session *session, cai_source **out,
                          cai_error *error);
  /** Add one user text message and run the session. */
  int (*send_text)(cai_session *session, const char *text, cai_response **out,
                   cai_error *error);
  /** Return last recorded usage for the session. */
  int (*last_usage)(const cai_session *session, cai_token_usage *out,
                    cai_error *error);
  /** Replace cumulative usage and USD spend limits for this session. */
  int (*set_usage_limits)(cai_session *session, const cai_usage_limits *limits,
                          cai_error *error);
  /** Return cumulative usage and estimated USD spend for this session. */
  int (*usage)(const cai_session *session, cai_usage_accounting *out,
               cai_error *error);
  /** Return cumulative usage while closing and destroying the session. */
  int (*close_with_usage)(cai_session *session, cai_usage_accounting *out,
                          cai_error *error);
  /** Return the model context window in tokens, or zero if unknown. */
  long long (*context_window_tokens)(const cai_session *session);
  /** Return the configured auto-compaction token threshold. */
  long long (*auto_compact_token_limit)(const cai_session *session);
  /** Return estimated context-window usage percentage. */
  int (*context_percent)(const cai_session *session, double *out,
                         cai_error *error);
  /** Return non-zero if local history has spilled to disk. */
  int (*history_spilled)(const cai_session *session);
  /** Export local history as a streaming source. */
  int (*export_history_source)(cai_session *session, cai_source **out,
                               cai_error *error);
  /** Import local history from a streaming source. */
  int (*import_history_source)(cai_session *session, cai_source *source,
                               cai_error *error);
  /** Export complete session state as a streaming source. */
  int (*export_state_source)(cai_session *session, cai_source **out,
                             cai_error *error);
  /** Import complete session state from a streaming source. */
  int (*import_state_source)(cai_session *session, cai_source *source,
                             cai_error *error);
  /** Save complete session state to a filesystem path. */
  int (*save_state_path)(cai_session *session, const char *path,
                         cai_error *error);
  /** Load complete session state from a filesystem path. */
  int (*load_state_path)(cai_session *session, const char *path,
                         cai_error *error);
  /** Close and destroy the session. */
  void (*close)(cai_session *session);
  /** Private implementation pointer; do not access directly. */
  void *impl;
};

/** Public method-table facade for function tool schema building. */
struct cai_tool_schema {
  /** Enable or disable strict tool schema generation. */
  int (*set_strict)(cai_tool_schema *schema, int strict, cai_error *error);
  /** Add a string property. */
  int (*string)(cai_tool_schema *schema, const char *name,
                const char *description, int required, cai_error *error);
  /** Add an integer property. */
  int (*integer)(cai_tool_schema *schema, const char *name,
                 const char *description, int required, cai_error *error);
  /** Add a number property. */
  int (*number)(cai_tool_schema *schema, const char *name,
                const char *description, int required, cai_error *error);
  /** Add a boolean property. */
  int (*boolean)(cai_tool_schema *schema, const char *name,
                 const char *description, int required, cai_error *error);
  /** Add a string enum property. */
  int (*string_enum)(cai_tool_schema *schema, const char *name,
                     const char *description, const char *const *values,
                     size_t value_count, int required, cai_error *error);
  /** Update an existing property description. */
  int (*describe)(cai_tool_schema *schema, const char *name,
                  const char *description, cai_error *error);
  /** Add a raw JSON schema property. */
  int (*raw_property)(cai_tool_schema *schema, const char *name,
                      const char *description, const char *schema_json,
                      int required, cai_error *error);
  /** Return the generated JSON schema string. */
  const char *(*json)(const cai_tool_schema *schema);
  /** Return non-zero when strict mode is enabled. */
  int (*strict)(const cai_tool_schema *schema);
  /** Close and destroy the schema builder. */
  void (*close)(cai_tool_schema *schema);
  /** Private implementation pointer; do not access directly. */
  void *impl;
};

/** Initialize client config with default values. */
void cai_client_config_init(cai_client_config *config);
/** Initialize usage limits with all limits disabled. */
void cai_usage_limits_init(cai_usage_limits *limits);
/** Initialize usage accounting with zero usage and spend. */
void cai_usage_accounting_init(cai_usage_accounting *accounting);
/** Configure a client config to use OpenRouter defaults. */
void cai_client_config_use_openrouter(cai_client_config *config);
/** Explicitly load one API key from a dotenv-formatted file. */
int cai_load_dotenv_api_key(const char *dotenv_path, const char *api_key_env,
                            char **out, cai_error *error);
/** Free a cai-owned string returned by helper APIs. */
void cai_string_destroy(char *value);
/** Open a cai client. */
int cai_client_open(const cai_client_config *config, cai_client **out,
                    cai_error *error);
/** Close and destroy a cai client. */
void cai_client_close(cai_client *client);
/** Replace cumulative usage and USD spend limits for a client. */
int cai_client_set_usage_limits(cai_client *client,
                                const cai_usage_limits *limits,
                                cai_error *error);
/** Return cumulative usage and estimated USD spend for a client. */
int cai_client_usage(const cai_client *client, cai_usage_accounting *out,
                     cai_error *error);

/** Initialize agent config with default values. */
void cai_agent_config_init(cai_agent_config *config);
/** Create a new agent from a client. */
int cai_client_new_agent(cai_client *client, const cai_agent_config *config,
                         cai_agent **out, cai_error *error);
/** Destroy an agent. */
void cai_agent_destroy(cai_agent *agent);
/** Replace usage and USD spend limits inherited by new agent sessions. */
int cai_agent_set_session_usage_limits(cai_agent *agent,
                                       const cai_usage_limits *limits,
                                       cai_error *error);
/** Return cumulative usage for the agent's implicit session. */
int cai_agent_usage(const cai_agent *agent, cai_usage_accounting *out,
                    cai_error *error);
/** Register a typed lonejson-backed tool on an agent. */
int cai_agent_register_tool(cai_agent *agent, const char *name,
                            const char *description,
                            const struct lonejson_map *params_map,
                            const struct lonejson_map *result_map,
                            cai_tool_fn callback, void *context,
                            cai_error *error);
/** Register a typed lonejson-backed tool on an agent. */
int cai_agent_register_lonejson_tool(cai_agent *agent, const char *name,
                                     const char *description,
                                     const struct lonejson_map *params_map,
                                     const struct lonejson_map *result_map,
                                     cai_tool_fn callback, void *context,
                                     cai_error *error);
/** Register a raw JSON string tool on an agent. */
int cai_agent_register_raw_tool(cai_agent *agent, const char *name,
                                const char *description,
                                const char *schema_json, int strict,
                                cai_tool_raw_fn callback, void *context,
                                cai_error *error);
/** Register a raw JSON spooled tool on an agent. */
int cai_agent_register_raw_spooled_tool(cai_agent *agent, const char *name,
                                        const char *description,
                                        const char *schema_json, int strict,
                                        cai_tool_raw_spooled_fn callback,
                                        void *context, cai_error *error);
/** Add a provider-hosted tool described by raw JSON to an agent. */
int cai_agent_add_hosted_tool_json(cai_agent *agent, const char *tool_json,
                                   cai_error *error);
/** Add a simple provider-hosted tool type to an agent. */
int cai_agent_add_simple_hosted_tool(cai_agent *agent, const char *type,
                                     cai_error *error);
/** Add a hosted MCP tool configuration to an agent. */
int cai_agent_add_hosted_mcp_tool(cai_agent *agent,
                                  const cai_hosted_mcp_tool_config *config,
                                  cai_error *error);
/** Create an explicit session from an agent. */
int cai_agent_new_session(cai_agent *agent, cai_session **out,
                          cai_error *error);
/** Create an explicit session with a new server-side conversation. */
int cai_agent_new_conversation_session(cai_agent *agent, cai_session **out,
                                       cai_error *error);
/** Create a session bound to an existing conversation. */
int cai_agent_new_session_for_conversation(cai_agent *agent,
                                           const cai_conversation *conversation,
                                           cai_session **out, cai_error *error);
/** Destroy a session. */
void cai_session_destroy(cai_session *session);
/** Return cumulative usage while closing and destroying a session. */
int cai_session_close_with_usage(cai_session *session,
                                 cai_usage_accounting *out, cai_error *error);
/** Bind a session to a server-side conversation id. */
int cai_session_set_conversation_id(cai_session *session,
                                    const char *conversation_id,
                                    cai_error *error);
/** Bind a session to a conversation handle. */
int cai_session_set_conversation(cai_session *session,
                                 const cai_conversation *conversation,
                                 cai_error *error);
/** Return the session conversation id, or NULL. */
const char *cai_session_conversation_id(const cai_session *session);
/** Set the previous response id used for server-side continuity. */
int cai_session_set_previous_response_id(cai_session *session,
                                         const char *response_id,
                                         cai_error *error);
/** Return the previous response id, or NULL. */
const char *cai_session_previous_response_id(const cai_session *session);
/** Add user text to the pending session input. */
int cai_session_add_user_text(cai_session *session, const char *text,
                              cai_error *error);
/** Add spooled user text to the pending session input. */
int cai_session_add_user_text_spooled(cai_session *session,
                                      struct lonejson_spooled *text,
                                      cai_error *error);
/** Read a source into spooled user text for the pending session input. */
int cai_session_add_user_text_source(cai_session *session, cai_source *source,
                                     cai_error *error);
/** Add an image URL to the pending session input. */
int cai_session_add_user_image_url(cai_session *session, const char *url,
                                   const char *detail, cai_error *error);
/** Add spooled file data to the pending session input. */
int cai_session_add_user_file_data_spooled(cai_session *session,
                                           const char *filename,
                                           struct lonejson_spooled *file_data,
                                           const char *detail,
                                           cai_error *error);
/** Read a source into spooled file data for the pending session input. */
int cai_session_add_user_file_source(cai_session *session, const char *filename,
                                     cai_source *source, const char *detail,
                                     cai_error *error);
/** Add file data from a filesystem path to the pending input. */
int cai_session_add_user_file_path(cai_session *session, const char *path,
                                   const char *filename, const char *detail,
                                   cai_error *error);
/** Add local function-call output to the pending session input. */
int cai_session_add_function_call_output(cai_session *session,
                                         const char *call_id,
                                         const char *output, cai_error *error);
/** Run the pending session once. */
int cai_session_run(cai_session *session, cai_response **out, cai_error *error);
/** Run the session and return a high-level output wrapper. */
int cai_session_run_output(cai_session *session, cai_output **out,
                           cai_error *error);
/** Initialize local tool auto-run options. */
void cai_run_options_init(cai_run_options *options);
/** Run the session with local tool auto-run. */
int cai_session_run_auto(cai_session *session, const cai_run_options *options,
                         cai_response **out, cai_error *error);
/** Run with local tool auto-run and return high-level output. */
int cai_session_run_auto_output(cai_session *session,
                                const cai_run_options *options,
                                cai_output **out, cai_error *error);
/** Stream the session with local tool auto-run. */
int cai_session_stream_auto(cai_session *session,
                            const cai_run_options *options,
                            const cai_stream_sinks *sinks, cai_error *error);
/** Stream the session without local tool auto-run. */
int cai_session_stream(cai_session *session, const cai_stream_sinks *sinks,
                       cai_error *error);
/** Stream only output text to a sink. */
int cai_session_stream_text(cai_session *session, cai_sink *sink,
                            cai_error *error);
/** Open output text as a streaming source. */
int cai_session_open_text_source(cai_session *session, cai_source **out,
                                 cai_error *error);
/** Run experimental client-side manual compaction. */
int cai_session_compact_experimental(cai_session *session, cai_error *error);
/** Add one user text message and run the session. */
int cai_session_send_text(cai_session *session, const char *text,
                          cai_response **out, cai_error *error);
/** Return last recorded token usage for the session. */
int cai_session_last_usage(const cai_session *session, cai_token_usage *out,
                           cai_error *error);
/** Replace cumulative usage and USD spend limits for a session. */
int cai_session_set_usage_limits(cai_session *session,
                                 const cai_usage_limits *limits,
                                 cai_error *error);
/** Return cumulative usage and estimated USD spend for a session. */
int cai_session_usage(const cai_session *session, cai_usage_accounting *out,
                      cai_error *error);
/** Return the model context window in tokens, or zero if unknown. */
long long cai_session_context_window_tokens(const cai_session *session);
/** Return the configured auto-compaction token threshold. */
long long cai_session_auto_compact_token_limit(const cai_session *session);
/** Return estimated context-window usage percentage. */
int cai_session_context_percent(const cai_session *session, double *out,
                                cai_error *error);
/** Return non-zero if local history has spilled to disk. */
int cai_session_history_spilled(const cai_session *session);
/** Export local history as a streaming source. */
int cai_session_export_history_source(cai_session *session, cai_source **out,
                                      cai_error *error);
/** Import local history from a streaming source. */
int cai_session_import_history_source(cai_session *session, cai_source *source,
                                      cai_error *error);
/** Export complete session state as a streaming source. */
int cai_session_export_state_source(cai_session *session, cai_source **out,
                                    cai_error *error);
/** Import complete session state from a streaming source. */
int cai_session_import_state_source(cai_session *session, cai_source *source,
                                    cai_error *error);
/** Save complete session state to a filesystem path. */
int cai_session_save_state_path(cai_session *session, const char *path,
                                cai_error *error);
/** Load complete session state from a filesystem path. */
int cai_session_load_state_path(cai_session *session, const char *path,
                                cai_error *error);

/** Initialize an error object before first use. */
void cai_error_init(cai_error *error);
/** Release memory owned by an error object. */
void cai_error_cleanup(cai_error *error);
/** Return a static string for a cai_status code. */
const char *cai_status_string(int status);

/** Create a source from callbacks. */
int cai_source_from_callbacks(const cai_source_callbacks *callbacks,
                              cai_source **out, cai_error *error);
/** Wrap an lc_source as a cai_source. */
int cai_source_from_lc(struct lc_source *source, cai_source **out,
                       cai_error *error);
/** Create a source from a FILE pointer. */
int cai_source_file(FILE *fp, int close_file, cai_source **out,
                    cai_error *error);
/** Read bytes from a source. */
size_t cai_source_read(cai_source *source, void *buffer, size_t count,
                       cai_error *error);
/** Reset a source to the beginning when supported. */
int cai_source_reset(cai_source *source, cai_error *error);
/** Close and destroy a source. */
void cai_source_close(cai_source *source);
/** Stream all bytes from a source to a sink. */
int cai_source_copy_to_sink(cai_source *source, cai_sink *sink,
                            cai_error *error);

/** Create a sink from callbacks. */
int cai_sink_from_callbacks(const cai_sink_callbacks *callbacks, cai_sink **out,
                            cai_error *error);
/** Wrap an lc_sink as a cai_sink. */
int cai_sink_from_lc(struct lc_sink *sink, cai_sink **out, cai_error *error);
/** Create a sink from a FILE pointer. */
int cai_sink_file(FILE *fp, int close_file, cai_sink **out, cai_error *error);
/** Create a sink that writes to stdout. */
int cai_sink_stdout(cai_sink **out, cai_error *error);
/** Create a sink that writes to stderr. */
int cai_sink_stderr(cai_sink **out, cai_error *error);
/** Write bytes to a sink. */
int cai_sink_write(cai_sink *sink, const void *bytes, size_t count,
                   cai_error *error);
/** Close and destroy a sink. */
void cai_sink_close(cai_sink *sink);
/** Initialize stream sink routing with zero/default values. */
void cai_stream_sinks_init(cai_stream_sinks *sinks);

/** Expose high-level output as an lc_source. */
int cai_output_as_lc_source(cai_output *output, struct lc_source **out,
                            cai_error *error);
/** Return the underlying response for high-level output. */
const cai_response *cai_output_response(const cai_output *output);
/** Return materialized output text, or NULL. */
const char *cai_output_text(const cai_output *output);
/** Return materialized refusal text, or NULL. */
const char *cai_output_refusal(const cai_output *output);
/** Return raw response JSON, or NULL. */
const char *cai_output_raw_json(const cai_output *output);
/** Write output text to a sink. */
int cai_output_write_text(const cai_output *output, cai_sink *sink,
                          cai_error *error);
/** Write refusal text to a sink. */
int cai_output_write_refusal(const cai_output *output, cai_sink *sink,
                             cai_error *error);
/** Write raw response JSON to a sink. */
int cai_output_write_raw_json(const cai_output *output, cai_sink *sink,
                              cai_error *error);
/** Serialize selected output fields through a lonejson map. */
int cai_output_write_json(cai_output *output, const struct lonejson_map *map,
                          void *value, cai_error *error);
/** Destroy high-level output. */
void cai_output_destroy(cai_output *output);
/** Write a tool event's spooled output JSON to a sink. */
int cai_tool_event_write_output(const cai_tool_event *event, cai_sink *sink,
                                cai_error *error);
/** Write a tool event's arguments JSON to a sink. */
int cai_tool_event_write_arguments(const cai_tool_event *event, cai_sink *sink,
                                   cai_error *error);

/** Create an empty local tool registry. */
int cai_tool_registry_new(cai_tool_registry **out, cai_error *error);
/** Destroy a local tool registry. */
void cai_tool_registry_destroy(cai_tool_registry *registry);
/** Register a typed lonejson-backed local tool. */
int cai_tool_registry_register_lonejson(cai_tool_registry *registry,
                                        const char *name,
                                        const char *description,
                                        const struct lonejson_map *params_map,
                                        const struct lonejson_map *result_map,
                                        cai_tool_fn callback, void *context,
                                        cai_error *error);
/** Register a raw JSON local tool. */
int cai_tool_registry_register_raw(cai_tool_registry *registry,
                                   const char *name, const char *description,
                                   const char *schema_json, int strict,
                                   cai_tool_raw_fn callback, void *context,
                                   cai_error *error);
/** Register a raw JSON spooled local tool. */
int cai_tool_registry_register_raw_spooled(cai_tool_registry *registry,
                                           const char *name,
                                           const char *description,
                                           const char *schema_json, int strict,
                                           cai_tool_raw_spooled_fn callback,
                                           void *context, cai_error *error);
/** Add all registered tools to a Responses parameter builder. */
int cai_tool_registry_add_to_response_params(const cai_tool_registry *registry,
                                             cai_response_create_params *params,
                                             cai_error *error);
/** Run a registered tool by name using raw JSON arguments. */
int cai_tool_registry_run(cai_tool_registry *registry, const char *name,
                          const char *arguments_json, cai_sink *output,
                          cai_error *error);

/** Create an empty function tool schema builder. */
int cai_tool_schema_new(cai_tool_schema **out, cai_error *error);
/** Create a tool schema builder from a lonejson map. */
int cai_tool_schema_from_map(const struct lonejson_map *map,
                             cai_tool_schema **out, cai_error *error);
/** Destroy a function tool schema builder. */
void cai_tool_schema_destroy(cai_tool_schema *schema);
/** Enable or disable strict tool schema generation. */
int cai_tool_schema_set_strict(cai_tool_schema *schema, int strict,
                               cai_error *error);
/** Add a string property to a tool schema. */
int cai_tool_schema_add_string(cai_tool_schema *schema, const char *name,
                               const char *description, int required,
                               cai_error *error);
/** Add an integer property to a tool schema. */
int cai_tool_schema_add_integer(cai_tool_schema *schema, const char *name,
                                const char *description, int required,
                                cai_error *error);
/** Add a number property to a tool schema. */
int cai_tool_schema_add_number(cai_tool_schema *schema, const char *name,
                               const char *description, int required,
                               cai_error *error);
/** Add a boolean property to a tool schema. */
int cai_tool_schema_add_boolean(cai_tool_schema *schema, const char *name,
                                const char *description, int required,
                                cai_error *error);
/** Add a string enum property to a tool schema. */
int cai_tool_schema_add_string_enum(cai_tool_schema *schema, const char *name,
                                    const char *description,
                                    const char *const *values,
                                    size_t value_count, int required,
                                    cai_error *error);
/** Update the description for a schema property. */
int cai_tool_schema_describe(cai_tool_schema *schema, const char *name,
                             const char *description, cai_error *error);
/** Add a raw JSON property schema. */
int cai_tool_schema_add_raw_property(cai_tool_schema *schema, const char *name,
                                     const char *description,
                                     const char *schema_json, int required,
                                     cai_error *error);
/** Return the generated JSON schema string. */
const char *cai_tool_schema_json(const cai_tool_schema *schema);
/** Return non-zero when strict mode is enabled. */
int cai_tool_schema_strict(const cai_tool_schema *schema);

/** Create a low-level Responses create parameter builder. */
int cai_response_create_params_new(cai_response_create_params **out,
                                   cai_error *error);
/** Destroy a Responses create parameter builder. */
void cai_response_create_params_destroy(cai_response_create_params *params);
/** Set the model id for a Responses request. */
int cai_response_create_params_set_model(cai_response_create_params *params,
                                         const char *model, cai_error *error);
/** Set developer instructions for a Responses request. */
int cai_response_create_params_set_instructions(
    cai_response_create_params *params, const char *instructions,
    cai_error *error);
/** Set previous_response_id for server-side continuation. */
int cai_response_create_params_set_previous_response_id(
    cai_response_create_params *params, const char *response_id,
    cai_error *error);
/** Set a conversation id for a Responses request. */
int cai_response_create_params_set_conversation_id(
    cai_response_create_params *params, const char *conversation_id,
    cai_error *error);
/** Set a prompt cache key for a Responses request. */
int cai_response_create_params_set_prompt_cache_key(
    cai_response_create_params *params, const char *prompt_cache_key,
    cai_error *error);
/** Enable or disable background response processing. */
int cai_response_create_params_set_background(
    cai_response_create_params *params, int enabled, cai_error *error);
/** Enable or disable server-side response storage. */
int cai_response_create_params_set_store(cai_response_create_params *params,
                                         int enabled, cai_error *error);
/** Set the Responses service tier. */
int cai_response_create_params_set_service_tier(
    cai_response_create_params *params, const char *service_tier,
    cai_error *error);
/** Set the Responses truncation mode. */
int cai_response_create_params_set_truncation(
    cai_response_create_params *params, const char *truncation,
    cai_error *error);
/** Set raw JSON metadata for a Responses request. */
int cai_response_create_params_set_metadata_json(
    cai_response_create_params *params, const char *metadata_json,
    cai_error *error);
/** Set raw JSON include array for a Responses request. */
int cai_response_create_params_set_include_json(
    cai_response_create_params *params, const char *include_json,
    cai_error *error);
/** Set raw JSON prompt object for a Responses request. */
int cai_response_create_params_set_prompt_json(
    cai_response_create_params *params, const char *prompt_json,
    cai_error *error);
/** Set a simple tool_choice value. */
int cai_response_create_params_set_tool_choice(
    cai_response_create_params *params, const char *tool_choice,
    cai_error *error);
/** Set raw JSON tool_choice value. */
int cai_response_create_params_set_tool_choice_json(
    cai_response_create_params *params, const char *tool_choice_json,
    cai_error *error);
/** Set max_output_tokens for a Responses request. */
int cai_response_create_params_set_max_output_tokens(
    cai_response_create_params *params, int max_output_tokens,
    cai_error *error);
/** Set max_tool_calls for a Responses request. */
int cai_response_create_params_set_max_tool_calls(
    cai_response_create_params *params, int max_tool_calls, cai_error *error);
/** Set reasoning effort and summary mode. */
int cai_response_create_params_set_reasoning(cai_response_create_params *params,
                                             const char *effort,
                                             const char *summary,
                                             cai_error *error);
/** Enable or disable parallel tool calls. */
int cai_response_create_params_set_parallel_tool_calls(
    cai_response_create_params *params, int enabled, cai_error *error);
/** Set server-side context compaction threshold in tokens. */
int cai_response_create_params_set_compact_threshold(
    cai_response_create_params *params, long long compact_threshold_tokens,
    cai_error *error);
/** Request JSON object text format. */
int cai_response_create_params_set_text_format_json_object(
    cai_response_create_params *params, cai_error *error);
/** Request JSON schema text format. */
int cai_response_create_params_set_text_format_json_schema(
    cai_response_create_params *params, const char *name,
    const char *description, const char *schema_json, int strict,
    cai_error *error);
/** Set text verbosity for supported models. */
int cai_response_create_params_set_text_verbosity(
    cai_response_create_params *params, const char *verbosity,
    cai_error *error);
/** Add a text input item. */
int cai_response_create_params_add_text(cai_response_create_params *params,
                                        const char *role, const char *text,
                                        cai_error *error);
/** Add a spooled text input item. */
int cai_response_create_params_add_text_spooled(
    cai_response_create_params *params, const char *role,
    struct lonejson_spooled *text, cai_error *error);
/** Add an image URL input item. */
int cai_response_create_params_add_image_url(cai_response_create_params *params,
                                             const char *role, const char *url,
                                             const char *detail,
                                             cai_error *error);
/** Add an image file id input item. */
int cai_response_create_params_add_image_file_id(
    cai_response_create_params *params, const char *role, const char *file_id,
    const char *detail, cai_error *error);
/** Add a file id input item. */
int cai_response_create_params_add_file_id(cai_response_create_params *params,
                                           const char *role,
                                           const char *file_id,
                                           const char *detail,
                                           cai_error *error);
/** Add a file URL input item. */
int cai_response_create_params_add_file_url(cai_response_create_params *params,
                                            const char *role,
                                            const char *file_url,
                                            const char *detail,
                                            cai_error *error);
/** Add spooled file data as an input item. */
int cai_response_create_params_add_file_data_spooled(
    cai_response_create_params *params, const char *role, const char *filename,
    struct lonejson_spooled *file_data, const char *detail, cai_error *error);
/** Add a function tool definition to a Responses request. */
int cai_response_create_params_add_function_tool(
    cai_response_create_params *params, const char *name,
    const char *description, const char *parameters_json, int strict,
    cai_error *error);
/** Add a hosted tool described by raw JSON. */
int cai_response_create_params_add_hosted_tool_json(
    cai_response_create_params *params, const char *tool_json,
    cai_error *error);
/** Add a simple hosted tool by type string. */
int cai_response_create_params_add_simple_hosted_tool(
    cai_response_create_params *params, const char *type, cai_error *error);
/** Initialize hosted MCP tool config with defaults. */
void cai_hosted_mcp_tool_config_init(cai_hosted_mcp_tool_config *config);
/** Add a hosted MCP tool definition. */
int cai_response_create_params_add_hosted_mcp_tool(
    cai_response_create_params *params,
    const cai_hosted_mcp_tool_config *config, cai_error *error);
/** Add raw JSON function-call output. */
int cai_response_create_params_add_function_call_output(
    cai_response_create_params *params, const char *call_id, const char *output,
    cai_error *error);
/** Add text function-call output. */
int cai_response_create_params_add_function_call_output_text(
    cai_response_create_params *params, const char *call_id, const char *text,
    cai_error *error);
/** Add image URL function-call output. */
int cai_response_create_params_add_function_call_output_image_url(
    cai_response_create_params *params, const char *call_id, const char *url,
    const char *detail, cai_error *error);
/** Add file id function-call output. */
int cai_response_create_params_add_function_call_output_file_id(
    cai_response_create_params *params, const char *call_id,
    const char *file_id, const char *detail, cai_error *error);
/** Add spooled file data function-call output. */
int cai_response_create_params_add_function_call_output_file_data_spooled(
    cai_response_create_params *params, const char *call_id,
    const char *filename, struct lonejson_spooled *file_data,
    const char *detail, cai_error *error);
/** Create a Responses API response. */
int cai_client_create_response(cai_client *client,
                               const cai_response_create_params *params,
                               cai_response **out, cai_error *error);
/** Count input tokens for a Responses request. */
int cai_client_count_response_input_tokens(
    cai_client *client, const cai_response_create_params *params,
    cai_token_usage *out, cai_error *error);
/** Stream response output text to a sink. */
int cai_client_stream_response_text(cai_client *client,
                                    const cai_response_create_params *params,
                                    cai_sink *sink, cai_error *error);
/** Open response output text as a streaming source. */
int cai_client_open_response_text_source(
    cai_client *client, const cai_response_create_params *params,
    cai_source **out, cai_error *error);
/** Retrieve a stored response by id. */
int cai_client_retrieve_response(cai_client *client, const char *response_id,
                                 cai_response **out, cai_error *error);
/** Cancel a background response by id. */
int cai_client_cancel_response(cai_client *client, const char *response_id,
                               cai_response **out, cai_error *error);
/** Delete a stored response by id. */
int cai_client_delete_response(cai_client *client, const char *response_id,
                               cai_error *error);
/** Initialize list pagination parameters. */
void cai_list_params_init(cai_list_params *params);
/** List input items for a response. */
int cai_client_list_response_input_items(cai_client *client,
                                         const char *response_id,
                                         const cai_list_params *params,
                                         cai_input_item_list **out,
                                         cai_error *error);

/** Return the response id, or NULL. */
const char *cai_response_id(const cai_response *response);
/** Return the response status, or NULL. */
const char *cai_response_status(const cai_response *response);
/** Return the response model id, or NULL. */
const char *cai_response_model(const cai_response *response);
/** Return the response conversation id, or NULL. */
const char *cai_response_conversation_id(const cai_response *response);
/** Return response creation time as a Unix timestamp, or zero. */
long long cai_response_created_at(const cai_response *response);
/** Return materialized output text, or NULL. */
const char *cai_response_output_text(const cai_response *response);
/** Return materialized refusal text, or NULL. */
const char *cai_response_refusal(const cai_response *response);
/** Write response output text to a sink. */
int cai_response_write_output_text(const cai_response *response, cai_sink *sink,
                                   cai_error *error);
/** Write response refusal text to a sink. */
int cai_response_write_refusal(const cai_response *response, cai_sink *sink,
                               cai_error *error);
/** Return raw response JSON, or NULL. */
const char *cai_response_raw_json(const cai_response *response);
/** Materialize response output items JSON into a cai-owned string. */
int cai_response_output_items_json(const cai_response *response,
                                   char **out_json, cai_error *error);
/** Stream response output items JSON to a sink. */
int cai_response_write_output_items_json(const cai_response *response,
                                         cai_sink *sink, cai_error *error);
/** Return response error code, or NULL. */
const char *cai_response_error_code(const cai_response *response);
/** Return response error message, or NULL. */
const char *cai_response_error_message(const cai_response *response);
/** Return incomplete_details reason, or NULL. */
const char *cai_response_incomplete_reason(const cai_response *response);
/** Return input token count. */
long long cai_response_input_tokens(const cai_response *response);
/** Return cached input token count. */
long long cai_response_input_cached_tokens(const cai_response *response);
/** Return output token count. */
long long cai_response_output_tokens(const cai_response *response);
/** Return reasoning output token count. */
long long cai_response_output_reasoning_tokens(const cai_response *response);
/** Return total token count. */
long long cai_response_total_tokens(const cai_response *response);
/** Copy response usage into a cai_token_usage struct. */
int cai_response_usage(const cai_response *response, cai_token_usage *out,
                       cai_error *error);
/** Return number of function/tool calls in the response. */
size_t cai_response_tool_call_count(const cai_response *response);
/** Return tool call id by index, or NULL. */
const char *cai_response_tool_call_id(const cai_response *response,
                                      size_t index);
/** Return tool call name by index, or NULL. */
const char *cai_response_tool_call_name(const cai_response *response,
                                        size_t index);
/** Return tool call arguments JSON by index, or NULL. */
const char *cai_response_tool_call_arguments(const cai_response *response,
                                             size_t index);
/** Return spooled tool call arguments JSON by index, or NULL. */
const struct lonejson_spooled *
cai_response_tool_call_arguments_spooled(const cai_response *response,
                                         size_t index);
/** Return output item count. */
size_t cai_response_output_item_count(const cai_response *response);
/** Return output item id by index, or NULL. */
const char *cai_response_output_item_id(const cai_response *response,
                                        size_t index);
/** Return output item type by index, or NULL. */
const char *cai_response_output_item_type(const cai_response *response,
                                          size_t index);
/** Return output item status by index, or NULL. */
const char *cai_response_output_item_status(const cai_response *response,
                                            size_t index);
/** Return output item role by index, or NULL. */
const char *cai_response_output_item_role(const cai_response *response,
                                          size_t index);
/** Return output item call id by index, or NULL. */
const char *cai_response_output_item_call_id(const cai_response *response,
                                             size_t index);
/** Return output item name by index, or NULL. */
const char *cai_response_output_item_name(const cai_response *response,
                                          size_t index);
/** Destroy a response. */
void cai_response_destroy(cai_response *response);
/** Return item count in an input item list. */
size_t cai_input_item_list_count(const cai_input_item_list *list);
/** Return non-zero if more items are available. */
int cai_input_item_list_has_more(const cai_input_item_list *list);
/** Return first item id in a list page, or NULL. */
const char *cai_input_item_list_first_id(const cai_input_item_list *list);
/** Return last item id in a list page, or NULL. */
const char *cai_input_item_list_last_id(const cai_input_item_list *list);
/** Return raw list JSON, or NULL. */
const char *cai_input_item_list_raw_json(const cai_input_item_list *list);
/** Return input item id by index, or NULL. */
const char *cai_input_item_id(const cai_input_item_list *list, size_t index);
/** Return input item type by index, or NULL. */
const char *cai_input_item_type(const cai_input_item_list *list, size_t index);
/** Return input item role by index, or NULL. */
const char *cai_input_item_role(const cai_input_item_list *list, size_t index);
/** Destroy an input item list. */
void cai_input_item_list_destroy(cai_input_item_list *list);

/** Create a server-side conversation. */
int cai_client_create_conversation(cai_client *client, cai_conversation **out,
                                   cai_error *error);
/** Create a local conversation handle from an existing id. */
int cai_conversation_from_id(const char *conversation_id,
                             cai_conversation **out, cai_error *error);
/** Retrieve a conversation by id. */
int cai_client_retrieve_conversation(cai_client *client,
                                     const char *conversation_id,
                                     cai_conversation **out, cai_error *error);
/** Retrieve a conversation by handle. */
int cai_client_retrieve_conversation_handle(
    cai_client *client, const cai_conversation *conversation,
    cai_conversation **out, cai_error *error);
/** Update conversation metadata by id. */
int cai_client_update_conversation_metadata(cai_client *client,
                                            const char *conversation_id,
                                            const char *metadata_json,
                                            cai_conversation **out,
                                            cai_error *error);
/** Update conversation metadata by handle. */
int cai_client_update_conversation_metadata_handle(
    cai_client *client, const cai_conversation *conversation,
    const char *metadata_json, cai_conversation **out, cai_error *error);
/** Delete a conversation by id. */
int cai_client_delete_conversation(cai_client *client,
                                   const char *conversation_id,
                                   cai_error *error);
/** Delete a conversation by handle. */
int cai_client_delete_conversation_handle(cai_client *client,
                                          const cai_conversation *conversation,
                                          cai_error *error);
/** List conversation items by conversation id. */
int cai_client_list_conversation_items(cai_client *client,
                                       const char *conversation_id,
                                       const cai_list_params *params,
                                       cai_input_item_list **out,
                                       cai_error *error);
/** List conversation items by conversation handle. */
int cai_client_list_conversation_items_handle(
    cai_client *client, const cai_conversation *conversation,
    const cai_list_params *params, cai_input_item_list **out, cai_error *error);
/** Delete a conversation item by conversation id and item id. */
int cai_client_delete_conversation_item(cai_client *client,
                                        const char *conversation_id,
                                        const char *item_id, cai_error *error);
/** Delete a conversation item by conversation handle and item id. */
int cai_client_delete_conversation_item_handle(
    cai_client *client, const cai_conversation *conversation,
    const char *item_id, cai_error *error);
/** Retrieve a conversation item by conversation id and item id. */
int cai_client_retrieve_conversation_item(cai_client *client,
                                          const char *conversation_id,
                                          const char *item_id,
                                          cai_conversation_item **out,
                                          cai_error *error);
/** Retrieve a conversation item by conversation handle and item id. */
int cai_client_retrieve_conversation_item_handle(
    cai_client *client, const cai_conversation *conversation,
    const char *item_id, cai_conversation_item **out, cai_error *error);
/** Create a conversation-items parameter builder. */
int cai_conversation_items_params_new(cai_conversation_items_params **out,
                                      cai_error *error);
/** Destroy a conversation-items parameter builder. */
void cai_conversation_items_params_destroy(
    cai_conversation_items_params *params);
/** Add text to a conversation-items create request. */
int cai_conversation_items_params_add_text(
    cai_conversation_items_params *params, const char *role, const char *text,
    cai_error *error);
/** Add spooled text to a conversation-items create request. */
int cai_conversation_items_params_add_text_spooled(
    cai_conversation_items_params *params, const char *role,
    struct lonejson_spooled *text, cai_error *error);
/** Read a source into spooled text for a conversation-items create request. */
int cai_conversation_items_params_add_text_source(
    cai_conversation_items_params *params, const char *role, cai_source *source,
    cai_error *error);
/** Add an image URL to a conversation-items create request. */
int cai_conversation_items_params_add_image_url(
    cai_conversation_items_params *params, const char *role, const char *url,
    const char *detail, cai_error *error);
/** Add an image file id to a conversation-items create request. */
int cai_conversation_items_params_add_image_file_id(
    cai_conversation_items_params *params, const char *role,
    const char *file_id, const char *detail, cai_error *error);
/** Add a file id to a conversation-items create request. */
int cai_conversation_items_params_add_file_id(
    cai_conversation_items_params *params, const char *role,
    const char *file_id, const char *detail, cai_error *error);
/** Add a file URL to a conversation-items create request. */
int cai_conversation_items_params_add_file_url(
    cai_conversation_items_params *params, const char *role,
    const char *file_url, const char *detail, cai_error *error);
/** Add spooled file data to a conversation-items create request. */
int cai_conversation_items_params_add_file_data_spooled(
    cai_conversation_items_params *params, const char *role,
    const char *filename, struct lonejson_spooled *file_data,
    const char *detail, cai_error *error);
/** Create conversation items by conversation id. */
int cai_client_create_conversation_items(
    cai_client *client, const char *conversation_id,
    const cai_conversation_items_params *params, cai_input_item_list **out,
    cai_error *error);
/** Create conversation items by conversation handle. */
int cai_client_create_conversation_items_handle(
    cai_client *client, const cai_conversation *conversation,
    const cai_conversation_items_params *params, cai_input_item_list **out,
    cai_error *error);
/** Return a conversation id, or NULL. */
const char *cai_conversation_id(const cai_conversation *conversation);
/** Return a conversation object type, or NULL. */
const char *cai_conversation_object(const cai_conversation *conversation);
/** Destroy a conversation handle. */
void cai_conversation_destroy(cai_conversation *conversation);
/** Return a conversation item id, or NULL. */
const char *cai_conversation_item_id(const cai_conversation_item *item);
/** Return a conversation item type, or NULL. */
const char *cai_conversation_item_type(const cai_conversation_item *item);
/** Return a conversation item role, or NULL. */
const char *cai_conversation_item_role(const cai_conversation_item *item);
/** Return raw conversation item JSON, or NULL. */
const char *cai_conversation_item_raw_json(const cai_conversation_item *item);
/** Destroy a conversation item. */
void cai_conversation_item_destroy(cai_conversation_item *item);

#ifdef __cplusplus
}
#endif

#endif
