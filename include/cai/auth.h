#ifndef CAI_AUTH_H
#define CAI_AUTH_H

#include <cai/cai.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Default OpenAI OAuth issuer used by Codex ChatGPT login. */
#define CAI_CHATGPT_AUTH_DEFAULT_ISSUER "https://auth.openai.com"
/** Codex OAuth client id used for ChatGPT subscription login. */
#define CAI_CHATGPT_AUTH_DEFAULT_CLIENT_ID "app_EMoamEEZ73f0CkXaXp7hrann"
/** Refresh access tokens this many seconds before JWT expiry. */
#define CAI_CHATGPT_AUTH_DEFAULT_REFRESH_WINDOW_SECONDS 300LL
/** Default localhost callback port used by Codex browser login. */
#define CAI_CHATGPT_AUTH_DEFAULT_CALLBACK_PORT 1455
/** Fallback localhost callback port registered by Codex. */
#define CAI_CHATGPT_AUTH_FALLBACK_CALLBACK_PORT 1457
/** Default callback path used by Codex browser login. */
#define CAI_CHATGPT_AUTH_DEFAULT_CALLBACK_PATH "/auth/callback"
/** Default OAuth scope set used by Codex ChatGPT login. */
#define CAI_CHATGPT_AUTH_DEFAULT_SCOPES                                        \
  "openid profile email offline_access api.connectors.read "                   \
  "api.connectors.invoke"
/** Default originator sent in ChatGPT OAuth and backend requests. */
#define CAI_CHATGPT_AUTH_DEFAULT_ORIGINATOR "cai"
/** Default timeout for ChatGPT OAuth token HTTP requests. */
#define CAI_CHATGPT_AUTH_DEFAULT_HTTP_TIMEOUT_MS 30000L

/** Codex-compatible ChatGPT auth session configuration. */
typedef struct cai_chatgpt_auth_config {
  /**
   * Path to Codex-style auth.json storage. NULL/empty selects cai's default
   * path: $XDG_CONFIG_HOME/cai/auth.json, or $HOME/.config/cai/auth.json.
   */
  const char *auth_json_path;
  /** OAuth issuer; NULL selects CAI_CHATGPT_AUTH_DEFAULT_ISSUER. */
  const char *issuer;
  /** OAuth client id; NULL selects CAI_CHATGPT_AUTH_DEFAULT_CLIENT_ID. */
  const char *client_id;
  /** Seconds before access-token expiry to refresh; zero uses default. */
  long long refresh_window_seconds;
  /** Overall OAuth token HTTP timeout in milliseconds; zero uses default. */
  long http_timeout_ms;
  /** Non-zero disables TLS certificate verification for OAuth token HTTP. */
  int insecure_skip_verify;
  /** Optional PEM CA bundle path for OAuth token TLS verification. */
  const char *ca_bundle_path;
  /** Optional CA certificate directory for OAuth token TLS verification. */
  const char *ca_path;
  /** Optional pslog logger. */
  struct pslog_logger *logger;
  /** Non-zero disables auth logging even when logger is set. */
  int logger_disabled;
  /** Optional custom allocator callbacks. */
  cai_allocator allocator;
} cai_chatgpt_auth_config;

/** ChatGPT OAuth auth session with receiver methods for token access. */
struct cai_chatgpt_auth {
  /** Return the current access token, refreshing first when near expiry. */
  int (*access_token)(cai_chatgpt_auth *auth, char **out, cai_error *error);
  /** Force a refresh-token grant and persist returned token fields. */
  int (*refresh)(cai_chatgpt_auth *auth, cai_error *error);
  /** Close this auth session and clear sensitive token memory. */
  void (*close)(cai_chatgpt_auth *auth);
  /** Allocator used for this receiver shell. */
  cai_allocator allocator;
  /** Private implementation owned by cai. */
  void *impl;
};

/** Server-agnostic request passed to the ChatGPT OAuth callback handler. */
typedef struct cai_chatgpt_login_request {
  /** HTTP method, usually GET. */
  const char *method;
  /** HTTP request target, for example /auth/callback?code=...&state=... */
  const char *target;
} cai_chatgpt_login_request;

/** Response produced by the ChatGPT OAuth callback handler. */
typedef struct cai_chatgpt_login_response {
  /** HTTP status code to send. */
  int status;
  /** Response content type. Borrowed static string. */
  const char *content_type;
  /** Response body. Release with cai_chatgpt_login_response_cleanup. */
  char *body;
  /** Non-zero when the login flow reached a terminal callback state. */
  int completed;
} cai_chatgpt_login_response;

/** Configuration for an interactive ChatGPT OAuth browser login flow. */
typedef struct cai_chatgpt_login_config {
  /**
   * Path where Codex-style auth.json should be persisted. NULL/empty selects
   * cai's default path: $XDG_CONFIG_HOME/cai/auth.json, or
   * $HOME/.config/cai/auth.json.
   */
  const char *auth_json_path;
  /** OAuth redirect URI served by the embedding HTTP server. Required. */
  const char *redirect_uri;
  /** Callback path extracted from redirect_uri; NULL selects default. */
  const char *callback_path;
  /** OAuth issuer; NULL selects CAI_CHATGPT_AUTH_DEFAULT_ISSUER. */
  const char *issuer;
  /** OAuth client id; NULL selects CAI_CHATGPT_AUTH_DEFAULT_CLIENT_ID. */
  const char *client_id;
  /** OAuth scopes; NULL selects CAI_CHATGPT_AUTH_DEFAULT_SCOPES. */
  const char *scopes;
  /** Optional Codex-style originator query value. */
  const char *originator;
  /** Optional caller-supplied CSRF state. NULL generates one. */
  const char *state;
  /** Optional caller-supplied PKCE verifier. NULL generates one. */
  const char *code_verifier;
  /** Overall OAuth token HTTP timeout in milliseconds; zero uses default. */
  long http_timeout_ms;
  /** Non-zero disables TLS certificate verification for OAuth token HTTP. */
  int insecure_skip_verify;
  /** Optional PEM CA bundle path for OAuth token TLS verification. */
  const char *ca_bundle_path;
  /** Optional CA certificate directory for OAuth token TLS verification. */
  const char *ca_path;
  /** Optional pslog logger. */
  struct pslog_logger *logger;
  /** Non-zero disables auth logging even when logger is set. */
  int logger_disabled;
  /** Optional custom allocator callbacks. */
  cai_allocator allocator;
} cai_chatgpt_login_config;

/** Optional browser opener configuration for ChatGPT OAuth login helpers. */
typedef struct cai_chatgpt_login_browser_config {
  /**
   * Browser opener executable. NULL selects the platform default: `open` on
   * Darwin/macOS, `xdg-open` elsewhere. The URL is passed as one argv element;
   * no shell is used.
   */
  const char *command;
} cai_chatgpt_login_browser_config;

/** Interactive ChatGPT OAuth login flow with receiver callback methods. */
typedef struct cai_chatgpt_login {
  /** Handle one HTTP callback request and persist auth.json on success. */
  int (*handle_callback)(struct cai_chatgpt_login *login,
                         const cai_chatgpt_login_request *request,
                         cai_chatgpt_login_response *response,
                         cai_error *error);
  /** Return non-zero after the login callback has succeeded. */
  int (*completed)(const struct cai_chatgpt_login *login);
  /** Close this interactive login flow and clear sensitive state. */
  void (*close)(struct cai_chatgpt_login *login);
  /** Allocator used for this receiver shell. */
  cai_allocator allocator;
  /** Private implementation owned by cai. */
  void *impl;
} cai_chatgpt_login;

/** Initialize ChatGPT auth config defaults. */
void cai_chatgpt_auth_config_init(cai_chatgpt_auth_config *config);
/** Return cai's default ChatGPT auth.json path. Free with cai_string_destroy.
 */
int cai_chatgpt_auth_default_path(char **out, cai_error *error);
/** Open a Codex-compatible ChatGPT auth session from auth.json storage. */
int cai_chatgpt_auth_open(const cai_chatgpt_auth_config *config,
                          cai_chatgpt_auth **out, cai_error *error);
/** Return the current access token, refreshing first when it is near expiry. */
int cai_chatgpt_auth_access_token(cai_chatgpt_auth *auth, char **out,
                                  cai_error *error);
/** Force a refresh-token grant and persist returned token fields. */
int cai_chatgpt_auth_refresh(cai_chatgpt_auth *auth, cai_error *error);
/** Close a ChatGPT auth session and clear sensitive token memory. */
void cai_chatgpt_auth_close(cai_chatgpt_auth *auth);

/** Initialize ChatGPT OAuth login config defaults. */
void cai_chatgpt_login_config_init(cai_chatgpt_login_config *config);
/** Initialize browser opener helper config defaults. */
void cai_chatgpt_login_browser_config_init(
    cai_chatgpt_login_browser_config *config);
/** Return the platform browser opener command, such as "open" or "xdg-open". */
int cai_chatgpt_login_browser_command(char *out, size_t out_size,
                                      cai_error *error);
/** Launch the platform browser opener for an OAuth authorization URL. */
int cai_chatgpt_login_open_browser(const char *authorize_url, cai_error *error);
/** Launch a configured browser opener for an OAuth authorization URL. */
int cai_chatgpt_login_open_browser_with_config(
    const cai_chatgpt_login_browser_config *config, const char *authorize_url,
    cai_error *error);
/** Start an OAuth login flow and return the browser authorization URL. */
int cai_chatgpt_login_start(const cai_chatgpt_login_config *config,
                            cai_chatgpt_login **out, char **out_authorize_url,
                            cai_error *error);
/** Handle one HTTP callback request and persist auth.json on success. */
int cai_chatgpt_login_handle_callback(cai_chatgpt_login *login,
                                      const cai_chatgpt_login_request *request,
                                      cai_chatgpt_login_response *response,
                                      cai_error *error);
/** Release response-owned memory produced by the callback handler. */
void cai_chatgpt_login_response_cleanup(cai_chatgpt_login_response *response);
/** Return non-zero after the login callback has succeeded. */
int cai_chatgpt_login_completed(const cai_chatgpt_login *login);
/** Close an interactive ChatGPT login flow and clear sensitive state. */
void cai_chatgpt_login_close(cai_chatgpt_login *login);

#ifdef __cplusplus
}
#endif

#endif
