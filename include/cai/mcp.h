#ifndef CAI_MCP_H
#define CAI_MCP_H

#include <cai/cai.h>

#ifdef __cplusplus
extern "C" {
#endif

/** MCP protocol version advertised by the cai MCP handler. */
#define CAI_MCP_PROTOCOL_VERSION "2025-11-25"
/** Maximum bytes, including terminator, for MCP session ids. */
#define CAI_MCP_SESSION_ID_MAX 128
/** Maximum bytes, including terminator, for session protocol version. */
#define CAI_MCP_SESSION_PROTOCOL_VERSION_MAX 32
/** Maximum bytes, including terminator, for session client name. */
#define CAI_MCP_SESSION_CLIENT_NAME_MAX 128
/** Maximum bytes, including terminator, for session client version. */
#define CAI_MCP_SESSION_CLIENT_VERSION_MAX 64
/** Default maximum bytes accepted from one MCP tool call result. */
#define CAI_MCP_DEFAULT_TOOL_OUTPUT_MAX_BYTES (1024U * 1024U)
/** Explicit value for unbounded MCP tool output streaming. */
#define CAI_MCP_TOOL_OUTPUT_UNLIMITED ((size_t)-1)

/** Opaque MCP route handler instance. */
typedef struct cai_mcp_handler cai_mcp_handler;
/** Callback used by the MCP handler to read request headers. */
typedef const char *(*cai_mcp_header_get_fn)(void *context, const char *name);
/** Callback used by the MCP handler to set response headers. */
typedef int (*cai_mcp_header_set_fn)(void *context, const char *name,
                                     const char *value, cai_error *error);

/** Persistable MCP session metadata owned by the embedding server. */
typedef struct cai_mcp_session_state {
  /** Non-zero once initialize has completed for the session. */
  int initialized;
  /** Negotiated MCP protocol version. */
  char protocol_version[CAI_MCP_SESSION_PROTOCOL_VERSION_MAX];
  /** Client name reported during initialize. */
  char client_name[CAI_MCP_SESSION_CLIENT_NAME_MAX];
  /** Client version reported during initialize. */
  char client_version[CAI_MCP_SESSION_CLIENT_VERSION_MAX];
  /** Unix timestamp when the session was created. */
  long long created_at;
  /** Unix timestamp when the session was last observed. */
  long long last_seen_at;
} cai_mcp_session_state;

/** Lifecycle callbacks for embedding persistent MCP sessions. */
typedef struct cai_mcp_session_callbacks {
  /** Create a new session id and initial persisted state. */
  int (*create)(void *context, const cai_mcp_session_state *initial_state,
                char *session_id, size_t session_id_capacity, cai_error *error);
  /** Load persisted state for an existing session id. */
  int (*load)(void *context, const char *session_id,
              cai_mcp_session_state *state, cai_error *error);
  /** Save updated state for an existing session id. */
  int (*save)(void *context, const char *session_id,
              const cai_mcp_session_state *state, cai_error *error);
  /** Destroy persisted state for a session id. */
  int (*destroy)(void *context, const char *session_id, cai_error *error);
  /** Release callback context when the handler is destroyed. */
  void (*cleanup)(void *context);
} cai_mcp_session_callbacks;

/** Configuration for a server-framework-agnostic MCP route handler. */
typedef struct cai_mcp_handler_config {
  /** Server name advertised in initialize responses. */
  const char *name;
  /** Server version advertised in initialize responses. */
  const char *version;
  /** Tool registry exposed through MCP tools/list and tools/call. */
  cai_tool_registry *tools;
  /** Maximum request body size in bytes; zero uses default. */
  size_t request_max_bytes;
  /** In-memory response spool limit before spilling to disk. */
  size_t response_spool_memory_limit;
  /** Maximum tool output size in bytes; zero uses default. */
  size_t tool_output_max_bytes;
  /** Non-zero enables MCP session id negotiation and persistence. */
  int enable_sessions;
  /** Non-zero disables Origin header validation. */
  int disable_origin_validation;
  /** Optional allow-list of accepted non-empty Origin header values. */
  const char **allowed_origins;
  /** Number of entries in allowed_origins. */
  size_t allowed_origin_count;
  /** Protocol version to advertise, or NULL for CAI_MCP_PROTOCOL_VERSION. */
  const char *protocol_version;
  /** Non-zero rejects requests missing the MCP protocol header. */
  int require_protocol_version;
  /** Optional persistent session callback table. */
  const cai_mcp_session_callbacks *session;
  /** Context passed to persistent session callbacks. */
  void *session_context;
  /** User context reserved for embedding applications. */
  void *user_context;
} cai_mcp_handler_config;

/** One JSON Schema object discovered in MCP tool metadata.
 *
 * cai accepts MCP tool schemas as a typed, supported subset: object schemas
 * with optional `$schema`, `properties`, and `required`. Property schemas must
 * be JSON objects; their nested contents are validated but not exposed.
 */
typedef struct cai_mcp_client_schema {
  /** JSON Schema URI, or an empty string when absent. */
  const char *schema_uri;
  /** Root schema type. Currently always "object". */
  const char *type;
  /** Property names advertised under `properties`. */
  const char *const *properties;
  /** Number of entries in `properties`. */
  size_t property_count;
  /** Required property names advertised under `required`. */
  const char *const *required;
  /** Number of entries in `required`. */
  size_t required_count;
} cai_mcp_client_schema;

/** One icon descriptor discovered in MCP metadata. */
typedef struct cai_mcp_client_icon {
  /** Icon source URI. */
  const char *src;
  /** MIME type, or an empty string when absent. */
  const char *mime_type;
  /** Theme hint: "light", "dark", or an empty string when absent. */
  const char *theme;
  /** Icon sizes such as "48x48" or "any". */
  const char *const *sizes;
  /** Number of entries in `sizes`. */
  size_t size_count;
} cai_mcp_client_icon;

/** Audience annotations shared by MCP resources and resource templates. */
typedef struct cai_mcp_client_annotations {
  /** Audience roles: "user" and/or "assistant". */
  const char *const *audience;
  /** Number of entries in `audience`. */
  size_t audience_count;
  /** Last modified timestamp, or an empty string when absent. */
  const char *last_modified;
  /** Non-zero when `priority` was advertised. */
  int has_priority;
  /** Priority value in the inclusive range [0, 1] when present. */
  double priority;
} cai_mcp_client_annotations;

/** Tool annotations discovered from tools/list. */
typedef struct cai_mcp_client_tool_annotations {
  /** Annotation title, or an empty string when absent. */
  const char *title;
  int has_read_only_hint;
  int read_only_hint;
  int has_destructive_hint;
  int destructive_hint;
  int has_idempotent_hint;
  int idempotent_hint;
  int has_open_world_hint;
  int open_world_hint;
} cai_mcp_client_tool_annotations;

/** MCP tool task support policy. */
typedef enum cai_mcp_client_tool_task_support {
  CAI_MCP_CLIENT_TOOL_TASK_SUPPORT_UNSPECIFIED = 0,
  CAI_MCP_CLIENT_TOOL_TASK_SUPPORT_FORBIDDEN,
  CAI_MCP_CLIENT_TOOL_TASK_SUPPORT_OPTIONAL,
  CAI_MCP_CLIENT_TOOL_TASK_SUPPORT_REQUIRED
} cai_mcp_client_tool_task_support;

/** One prompt argument descriptor discovered from prompts/list. */
typedef struct cai_mcp_client_prompt_argument {
  /** Argument name. */
  const char *name;
  /** Display title, or an empty string when absent. */
  const char *title;
  /** Description, or an empty string when absent. */
  const char *description;
  /** Non-zero when `required` was advertised. */
  int has_required;
  /** Required value when present. */
  int required;
} cai_mcp_client_prompt_argument;

/** One remote MCP tool descriptor discovered from tools/list. */
typedef struct cai_mcp_client_tool {
  /** Tool name advertised by the MCP server. */
  const char *name;
  /** Tool display title: title, annotations.title, or name fallback. */
  const char *title;
  /** Tool description, or an empty string when absent. */
  const char *description;
  /** Tool input schema. */
  const cai_mcp_client_schema *input_schema;
  /** Tool output schema, or NULL when absent. */
  const cai_mcp_client_schema *output_schema;
  /** Tool annotations. */
  cai_mcp_client_tool_annotations annotations;
  /** Tool execution task support. */
  cai_mcp_client_tool_task_support task_support;
  /** Tool icons. */
  const cai_mcp_client_icon *icons;
  /** Number of entries in `icons`. */
  size_t icon_count;
} cai_mcp_client_tool;

/** One remote MCP resource descriptor discovered from resources/list. */
typedef struct cai_mcp_client_resource {
  /** Resource URI advertised by the MCP server. */
  const char *uri;
  /** Resource name advertised by the MCP server. */
  const char *name;
  /** Resource display title: title or name fallback. */
  const char *title;
  /** Resource description, or an empty string when absent. */
  const char *description;
  /** Resource MIME type, or an empty string when absent. */
  const char *mime_type;
  /** Resource icons. */
  const cai_mcp_client_icon *icons;
  /** Number of entries in `icons`. */
  size_t icon_count;
  /** Resource annotations. */
  cai_mcp_client_annotations annotations;
  /** Non-zero when `size` was advertised by the server. */
  int has_size;
  /** Resource size in bytes when `has_size` is non-zero. */
  long long size;
} cai_mcp_client_resource;

/** One remote MCP resource template descriptor discovered from
 * resources/templates/list.
 */
typedef struct cai_mcp_client_resource_template {
  /** URI template advertised by the MCP server. */
  const char *uri_template;
  /** Resource template name advertised by the MCP server. */
  const char *name;
  /** Resource template display title: title or name fallback. */
  const char *title;
  /** Resource template description, or an empty string when absent. */
  const char *description;
  /** Resource MIME type, or an empty string when absent. */
  const char *mime_type;
  /** Resource template icons. */
  const cai_mcp_client_icon *icons;
  /** Number of entries in `icons`. */
  size_t icon_count;
  /** Resource template annotations. */
  cai_mcp_client_annotations annotations;
} cai_mcp_client_resource_template;

/** One remote MCP prompt descriptor discovered from prompts/list. */
typedef struct cai_mcp_client_prompt {
  /** Prompt name advertised by the MCP server. */
  const char *name;
  /** Prompt display title: title or name fallback. */
  const char *title;
  /** Prompt description, or an empty string when absent. */
  const char *description;
  /** Prompt arguments. */
  const cai_mcp_client_prompt_argument *arguments;
  /** Number of entries in `arguments`. */
  size_t argument_count;
  /** Prompt icons. */
  const cai_mcp_client_icon *icons;
  /** Number of entries in `icons`. */
  size_t icon_count;
} cai_mcp_client_prompt;

/** Callback for server-to-client MCP notifications observed by a client.
 *
 * `params_json` is NULL when the notification has no params. When non-NULL, it
 * is valid only for the duration of the callback and is owned by cai.
 */
typedef int (*cai_mcp_client_notification_fn)(
    void *context, const char *method, struct lonejson_spooled *params_json,
    cai_error *error);

/** Receiver callbacks for server-to-client MCP messages. */
typedef struct cai_mcp_client_receiver {
  /** Shared receiver context passed to all callbacks. */
  void *context;
  /** Optional cleanup for context. */
  void (*cleanup)(void *context);
  /** Optional callback for server notifications on response streams. */
  cai_mcp_client_notification_fn notification;
} cai_mcp_client_receiver;

/** Configuration for cai's built-in Streamable HTTP MCP client. */
typedef struct cai_mcp_streamable_http_client_config {
  /** MCP endpoint URL, e.g. http://127.0.0.1:3001/mcp. */
  const char *url;
  /** Client name sent in initialize. NULL uses "cai". */
  const char *client_name;
  /** Client version sent in initialize. NULL uses cai's version. */
  const char *client_version;
  /** MCP protocol version to request. NULL uses CAI_MCP_PROTOCOL_VERSION. */
  const char *protocol_version;
  /** Overall HTTP timeout in milliseconds; zero uses cai default. */
  long timeout_ms;
  /** Non-zero disables TLS certificate verification. */
  int insecure_skip_verify;
  /** Optional PEM CA bundle path for TLS verification. */
  const char *ca_bundle_path;
  /** Optional CA certificate directory path for TLS verification. */
  const char *ca_path;
  /** Optional receiver callbacks for server-to-client MCP messages. */
  cai_mcp_client_receiver receiver;
  /** Optional callback for server notifications on response streams. */
  cai_mcp_client_notification_fn notification;
  /** Context passed to notification. */
  void *notification_context;
  /** Optional cleanup for notification_context. */
  void (*notification_context_cleanup)(void *context);
  /** Optional pslog logger. */
  struct pslog_logger *logger;
  /** Non-zero disables MCP client logging even when logger is set. */
  int logger_disabled;
  /** Optional custom allocator callbacks. */
  cai_allocator allocator;
} cai_mcp_streamable_http_client_config;

/** Options for registering remote MCP tools as local cai function tools. */
struct cai_mcp_tool_registration_config {
  /** Optional prefix prepended to each registered local tool name. */
  const char *name_prefix;
  /** Non-zero marks generated local function schemas strict. */
  int strict;
};

/** HTTP request view passed from an embedding route to cai MCP. */
typedef struct cai_mcp_http_request {
  /** HTTP method, usually "POST" for JSON-RPC or "GET" for SSE stream
   * compatibility.
   */
  const char *method;
  /** Streaming request body source. */
  cai_source *body;
  /** Header lookup callback supplied by the embedding server. */
  cai_mcp_header_get_fn header;
  /** Context passed to header. */
  void *header_context;
  /** User context reserved for embedding applications. */
  void *user_context;
} cai_mcp_http_request;

/** HTTP response sink passed from an embedding route to cai MCP. */
typedef struct cai_mcp_http_response {
  /** HTTP status set by cai_mcp_handler_handle_http. */
  int status;
  /** Streaming response body sink. */
  cai_sink *body;
  /** Header setter callback supplied by the embedding server. */
  cai_mcp_header_set_fn set_header;
  /** Context passed to set_header. */
  void *header_context;
} cai_mcp_http_response;

/** MCP route handler instance with receiver methods for request handling. */
struct cai_mcp_handler {
  /** Handle one MCP Streamable HTTP request. */
  int (*handle_http)(cai_mcp_handler *handler,
                     const cai_mcp_http_request *request,
                     cai_mcp_http_response *response, cai_error *error);
  /** Destroy this MCP handler and release associated resources. */
  void (*destroy)(cai_mcp_handler *handler);
};

/** Transport-independent MCP client interface consumed by cai. */
struct cai_mcp_client {
  /** Initialize the MCP session if needed. */
  int (*initialize)(cai_mcp_client *client, cai_error *error);
  /** Send an MCP ping request to verify the remote server is responsive. */
  int (*ping)(cai_mcp_client *client, cai_error *error);
  /** Refresh the cached remote tools/list metadata. */
  int (*refresh_tools)(cai_mcp_client *client, cai_error *error);
  /** Return the number of cached tools. */
  size_t (*tool_count)(const cai_mcp_client *client);
  /** Return cached tool metadata by index, or NULL when out of range. */
  const cai_mcp_client_tool *(*tool_at)(const cai_mcp_client *client,
                                        size_t index);
  /** Call one remote tool and write the validated result JSON to `output`. */
  int (*call_tool)(cai_mcp_client *client, const char *name,
                   struct lonejson_spooled *arguments_json, cai_sink *output,
                   cai_error *error);
  /** Refresh the cached remote resources/list metadata. */
  int (*refresh_resources)(cai_mcp_client *client, cai_error *error);
  /** Return the number of cached resources. */
  size_t (*resource_count)(const cai_mcp_client *client);
  /** Return cached resource metadata by index, or NULL when out of range. */
  const cai_mcp_client_resource *(*resource_at)(const cai_mcp_client *client,
                                                size_t index);
  /** Read one remote resource and write the validated result JSON. */
  int (*read_resource)(cai_mcp_client *client, const char *uri,
                       cai_sink *output, cai_error *error);
  /** Refresh the cached remote resources/templates/list metadata. */
  int (*refresh_resource_templates)(cai_mcp_client *client, cai_error *error);
  /** Return the number of cached resource templates. */
  size_t (*resource_template_count)(const cai_mcp_client *client);
  /** Return cached resource template metadata by index, or NULL. */
  const cai_mcp_client_resource_template *(*resource_template_at)(
      const cai_mcp_client *client, size_t index);
  /** Refresh the cached remote prompts/list metadata. */
  int (*refresh_prompts)(cai_mcp_client *client, cai_error *error);
  /** Return the number of cached prompts. */
  size_t (*prompt_count)(const cai_mcp_client *client);
  /** Return cached prompt metadata by index, or NULL when out of range. */
  const cai_mcp_client_prompt *(*prompt_at)(const cai_mcp_client *client,
                                            size_t index);
  /** Get one remote prompt by name and optional spooled JSON arguments. */
  int (*get_prompt)(cai_mcp_client *client, const char *name,
                    struct lonejson_spooled *arguments_json, cai_sink *output,
                    cai_error *error);
  /** Complete one prompt or resource argument and write result JSON.
   *
   * `ref_type` is usually "ref/prompt" with `ref_value` as a prompt name, or
   * "ref/resource" with `ref_value` as a resource/template URI.
   * `context_arguments_json`, when non-NULL, must be a JSON object.
   */
  int (*complete)(cai_mcp_client *client, const char *ref_type,
                  const char *ref_value, const char *argument_name,
                  const char *argument_value,
                  struct lonejson_spooled *context_arguments_json,
                  cai_sink *output, cai_error *error);
  /** Send one client-to-server request and write the validated result JSON. */
  int (*send_request)(cai_mcp_client *client, const char *method,
                      struct lonejson_spooled *params_json, cai_sink *output,
                      cai_error *error);
  /** Send one client-to-server JSON-RPC notification.
   *
   * `params_json`, when non-NULL, must be a complete JSON value and is written
   * as the notification `params` member.
   */
  int (*send_notification)(cai_mcp_client *client, const char *method,
                           struct lonejson_spooled *params_json,
                           cai_error *error);
  /** Destroy this MCP client and release associated resources. */
  void (*destroy)(cai_mcp_client *client);
  /** Private implementation pointer; custom clients may use this freely. */
  void *impl;
};

/** Initialize an MCP handler config with default values. */
void cai_mcp_handler_config_init(cai_mcp_handler_config *config);
/** Create a new MCP handler from a validated config. */
int cai_mcp_handler_new(const cai_mcp_handler_config *config,
                        cai_mcp_handler **out, cai_error *error);
/** Handle one MCP Streamable HTTP request using streaming body source/sink
 * callbacks. cai supports JSON POST replies by default, SSE-only POST replies
 * when the client asks only for `text/event-stream`, and a lightweight GET
 * SSE stream heartbeat.
 */
int cai_mcp_handler_handle_http(cai_mcp_handler *handler,
                                const cai_mcp_http_request *request,
                                cai_mcp_http_response *response,
                                cai_error *error);
/** Destroy an MCP handler and release associated resources. */
void cai_mcp_handler_destroy(cai_mcp_handler *handler);

/** Initialize Streamable HTTP MCP client config with defaults. */
void cai_mcp_streamable_http_client_config_init(
    cai_mcp_streamable_http_client_config *config);
/** Create cai's built-in Streamable HTTP MCP client. */
int cai_mcp_streamable_http_client_open(
    const cai_mcp_streamable_http_client_config *config, cai_mcp_client **out,
    cai_error *error);
/** Initialize an MCP client through its receiver interface. */
int cai_mcp_client_initialize(cai_mcp_client *client, cai_error *error);
/** Send an MCP ping request through the client receiver interface. */
int cai_mcp_client_ping(cai_mcp_client *client, cai_error *error);
/** Refresh cached remote tools/list metadata. */
int cai_mcp_client_refresh_tools(cai_mcp_client *client, cai_error *error);
/** Return the number of cached MCP tools. */
size_t cai_mcp_client_tool_count(const cai_mcp_client *client);
/** Return cached MCP tool metadata by index, or NULL when out of range. */
const cai_mcp_client_tool *cai_mcp_client_tool_at(const cai_mcp_client *client,
                                                  size_t index);
/** Call one remote MCP tool and write validated result JSON to `output`.
    `arguments_json` may be NULL for tools without arguments. */
int cai_mcp_client_call_tool(cai_mcp_client *client, const char *name,
                             struct lonejson_spooled *arguments_json,
                             cai_sink *output, cai_error *error);
/** Refresh cached remote resources/list metadata. */
int cai_mcp_client_refresh_resources(cai_mcp_client *client, cai_error *error);
/** Return the number of cached MCP resources. */
size_t cai_mcp_client_resource_count(const cai_mcp_client *client);
/** Return cached MCP resource metadata by index, or NULL when out of range. */
const cai_mcp_client_resource *
cai_mcp_client_resource_at(const cai_mcp_client *client, size_t index);
/** Read one remote MCP resource and write validated result JSON to `output`. */
int cai_mcp_client_read_resource(cai_mcp_client *client, const char *uri,
                                 cai_sink *output, cai_error *error);
/** Refresh cached remote resources/templates/list metadata. */
int cai_mcp_client_refresh_resource_templates(cai_mcp_client *client,
                                              cai_error *error);
/** Return the number of cached MCP resource templates. */
size_t cai_mcp_client_resource_template_count(const cai_mcp_client *client);
/** Return cached MCP resource template metadata by index, or NULL. */
const cai_mcp_client_resource_template *
cai_mcp_client_resource_template_at(const cai_mcp_client *client, size_t index);
/** Refresh cached remote prompts/list metadata. */
int cai_mcp_client_refresh_prompts(cai_mcp_client *client, cai_error *error);
/** Return the number of cached MCP prompts. */
size_t cai_mcp_client_prompt_count(const cai_mcp_client *client);
/** Return cached MCP prompt metadata by index, or NULL when out of range. */
const cai_mcp_client_prompt *
cai_mcp_client_prompt_at(const cai_mcp_client *client, size_t index);
/** Get one remote MCP prompt and write validated result JSON to `output`. */
int cai_mcp_client_get_prompt(cai_mcp_client *client, const char *name,
                              struct lonejson_spooled *arguments_json,
                              cai_sink *output, cai_error *error);
/** Complete one MCP prompt/resource argument and write validated result JSON.
 */
int cai_mcp_client_complete(cai_mcp_client *client, const char *ref_type,
                            const char *ref_value, const char *argument_name,
                            const char *argument_value,
                            struct lonejson_spooled *context_arguments_json,
                            cai_sink *output, cai_error *error);
/** Send one client-to-server request and write validated result JSON. */
int cai_mcp_client_send_request(cai_mcp_client *client, const char *method,
                                struct lonejson_spooled *params_json,
                                cai_sink *output, cai_error *error);
/** Send one client-to-server JSON-RPC notification. */
int cai_mcp_client_send_notification(cai_mcp_client *client, const char *method,
                                     struct lonejson_spooled *params_json,
                                     cai_error *error);
/** Register all cached/discovered MCP client tools into a local tool registry.
 *
 * The registry callbacks keep a non-owning pointer to `client`; callers must
 * keep the client alive for at least as long as the registered tools can run.
 */
int cai_mcp_client_register_tools(
    cai_mcp_client *client, cai_tool_registry *registry,
    const cai_mcp_tool_registration_config *config, cai_error *error);
/** Destroy an MCP client through its interface. */
void cai_mcp_client_destroy(cai_mcp_client *client);

#ifdef __cplusplus
}
#endif

#endif
