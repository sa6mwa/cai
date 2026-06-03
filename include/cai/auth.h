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

/** Codex-compatible ChatGPT auth session configuration. */
typedef struct cai_chatgpt_auth_config {
  /** Path to Codex-style auth.json storage. Required for file-backed auth. */
  const char *auth_json_path;
  /** OAuth issuer; NULL selects CAI_CHATGPT_AUTH_DEFAULT_ISSUER. */
  const char *issuer;
  /** OAuth client id; NULL selects CAI_CHATGPT_AUTH_DEFAULT_CLIENT_ID. */
  const char *client_id;
  /** Seconds before access-token expiry to refresh; zero uses default. */
  long long refresh_window_seconds;
  /** Optional pslog logger. */
  struct pslog_logger *logger;
  /** Non-zero disables auth logging even when logger is set. */
  int logger_disabled;
  /** Optional custom allocator callbacks. */
  cai_allocator allocator;
} cai_chatgpt_auth_config;

/** Initialize ChatGPT auth config defaults. */
void cai_chatgpt_auth_config_init(cai_chatgpt_auth_config *config);
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

#ifdef __cplusplus
}
#endif

#endif
