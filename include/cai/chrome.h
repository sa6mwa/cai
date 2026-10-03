/** @file cai/chrome.h
 * Buffered local transport for the ChatGPT Chrome extension.
 */
#ifndef CAI_CHROME_H
#define CAI_CHROME_H
#include <cai/cai.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Opaque, single-owner browser connection. */
typedef struct cai_chrome cai_chrome;
/** Explicit local browser opt-in; no Chrome configuration is changed. */
typedef struct cai_chrome_config {
  /** Absolute socket path; NULL discovers exactly one host in directory. */
  const char *socket_path;
  /** Private, user-owned directory; required unless socket_path is set. */
  const char *socket_directory;
  /** Whole request deadline in milliseconds; zero selects 30000, maximum 60000.
   */
  long timeout_ms;
  /** Maximum buffered response frame; zero selects 16 MiB, ceiling 64 MiB. */
  size_t max_response_bytes;
  /** Optional borrowed logger; must remain alive until close. Payloads are not
   * logged.
   */
  struct pslog_logger *logger;
} cai_chrome_config;
/** Open and validate the extension identity. *out is NULL on failure.
 * Uses an already running cai native host; never launches a browser or host.
 * Configuration strings are borrowed for open only. Frames are bounded and
 * buffered, not streamed. Only one owner may call a connection at a time.
 */
int cai_chrome_open(const cai_chrome_config *config, cai_chrome **out,
                    cai_error *error);
/** Send a browser JSON-RPC method and return its buffered JSON result.
 * params_json must be a JSON object. Browser operations require session_id,
 * turn_id and session_context in params, as defined by the extension.
 * Use cai_chrome_result_free on *out_json. Protocol/transport failures
 * invalidate the connection; calls are never automatically retried. Extension
 * errors include their message and numeric code. Cancellation follows the
 * runtime's worker cancellation scope, when present.
 */
int cai_chrome_request(cai_chrome *chrome, const char *method,
                       const char *params_json, char **out_json,
                       cai_error *error);
/** Release a JSON result returned by cai_chrome_request. NULL is accepted. */
void cai_chrome_result_free(char *json);
/** Set the identity used by the registered chrome tool; strings are copied.
 * Call once before each turn, after the preceding turn has ended.
 */
int cai_chrome_begin_turn(cai_chrome *chrome, const char *session_id,
                          const char *turn_id, cai_error *error);
/** Release tab leases/debuggers using turnEnded, when browser tools ran.
 * No-op for an unused turn. Works after cancellation with a bounded cleanup
 * deadline. Agent-created tabs close unless marked deliverable or handoff.
 */
int cai_chrome_end_turn(cai_chrome *chrome, cai_error *error);
/** Close; attempts bounded turn cleanup. NULL is accepted. */
void cai_chrome_close(cai_chrome *chrome);
/** Register the chrome tool, including typed screenshot delivery.
 * chrome is borrowed until agent close. Only one agent may use it at a time.
 */
int cai_agent_register_chrome_tool(cai_agent *agent, cai_chrome *chrome,
                                   cai_error *error);
#ifdef __cplusplus
}
#endif
#endif
