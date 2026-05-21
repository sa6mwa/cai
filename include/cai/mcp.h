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
typedef const char *(*cai_mcp_header_get_fn)(void *context,
                                             const char *name);
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
                char *session_id, size_t session_id_capacity,
                cai_error *error);
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
  /** Optional allow-list of accepted Origin header values. */
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

/** HTTP request view passed from an embedding route to cai MCP. */
typedef struct cai_mcp_http_request {
  /** HTTP method, usually "POST" for MCP JSON-RPC requests. */
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

/** Initialize an MCP handler config with default values. */
void cai_mcp_handler_config_init(cai_mcp_handler_config *config);
/** Create a new MCP handler from a validated config. */
int cai_mcp_handler_new(const cai_mcp_handler_config *config,
                        cai_mcp_handler **out, cai_error *error);
/** Handle one HTTP request using streaming body source/sink callbacks. */
int cai_mcp_handler_handle_http(cai_mcp_handler *handler,
                                const cai_mcp_http_request *request,
                                cai_mcp_http_response *response,
                                cai_error *error);
/** Destroy an MCP handler and release associated resources. */
void cai_mcp_handler_destroy(cai_mcp_handler *handler);

#ifdef __cplusplus
}
#endif

#endif
