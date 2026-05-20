#ifndef CAI_MCP_H
#define CAI_MCP_H

#include <cai/cai.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAI_MCP_PROTOCOL_VERSION "2025-11-25"
#define CAI_MCP_SESSION_ID_MAX 128
#define CAI_MCP_SESSION_PROTOCOL_VERSION_MAX 32
#define CAI_MCP_SESSION_CLIENT_NAME_MAX 128
#define CAI_MCP_SESSION_CLIENT_VERSION_MAX 64

typedef struct cai_mcp_handler cai_mcp_handler;

typedef const char *(*cai_mcp_header_get_fn)(void *context,
                                             const char *name);
typedef int (*cai_mcp_header_set_fn)(void *context, const char *name,
                                     const char *value, cai_error *error);

typedef struct cai_mcp_session_state {
  int initialized;
  char protocol_version[CAI_MCP_SESSION_PROTOCOL_VERSION_MAX];
  char client_name[CAI_MCP_SESSION_CLIENT_NAME_MAX];
  char client_version[CAI_MCP_SESSION_CLIENT_VERSION_MAX];
  long long created_at;
  long long last_seen_at;
} cai_mcp_session_state;

typedef struct cai_mcp_session_callbacks {
  int (*create)(void *context, const cai_mcp_session_state *initial_state,
                char *session_id, size_t session_id_capacity,
                cai_error *error);
  int (*load)(void *context, const char *session_id,
              cai_mcp_session_state *state, cai_error *error);
  int (*save)(void *context, const char *session_id,
              const cai_mcp_session_state *state, cai_error *error);
  int (*destroy)(void *context, const char *session_id, cai_error *error);
  void (*cleanup)(void *context);
} cai_mcp_session_callbacks;

typedef struct cai_mcp_handler_config {
  const char *name;
  const char *version;
  cai_tool_registry *tools;
  size_t request_max_bytes;
  size_t response_spool_memory_limit;
  size_t tool_output_max_bytes;
  int stateless;
  int validate_origin;
  const char **allowed_origins;
  size_t allowed_origin_count;
  const char *protocol_version;
  int allow_legacy_no_version;
  const cai_mcp_session_callbacks *session;
  void *session_context;
  void *user_context;
} cai_mcp_handler_config;

typedef struct cai_mcp_http_request {
  const char *method;
  cai_source *body;
  cai_mcp_header_get_fn header;
  void *header_context;
  void *user_context;
} cai_mcp_http_request;

typedef struct cai_mcp_http_response {
  int status;
  cai_sink *body;
  cai_mcp_header_set_fn set_header;
  void *header_context;
} cai_mcp_http_response;

void cai_mcp_handler_config_init(cai_mcp_handler_config *config);
int cai_mcp_handler_new(const cai_mcp_handler_config *config,
                        cai_mcp_handler **out, cai_error *error);
int cai_mcp_handler_handle_http(cai_mcp_handler *handler,
                                const cai_mcp_http_request *request,
                                cai_mcp_http_response *response,
                                cai_error *error);
void cai_mcp_handler_destroy(cai_mcp_handler *handler);

#ifdef __cplusplus
}
#endif

#endif
