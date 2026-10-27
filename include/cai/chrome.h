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
/** Run the Linux Chrome native-messaging bridge synchronously on stdin/stdout.
 * origin must identify the supported ChatGPT extension. The host owns process
 * dispatch and must route Chrome's extension-origin argument here. Logger is
 * borrowed for this call. No browser is launched. */
int cai_chrome_native_host_run(const char *origin, struct pslog_logger *logger,
                               cai_error *error);
/** As native_host_run, using an explicit private socket directory instead of
 * the core library's standard location. NULL retains standard discovery. */
int cai_chrome_native_host_run_at(const char *origin, const char *directory,
                                  struct pslog_logger *logger,
                                  cai_error *error);
/** Explicitly install/remove a user native-host manifest for the current
 * executable on Linux. NULL selects the standard CAI registration filename.
 * Conflicting registrations are rejected. The executable must dispatch native
 * messaging to cai_chrome_native_host_run. Removal also requests shutdown of
 * this executable's connected hosts in the standard private socket directory,
 * after unregistering. Hosts detach tracked tabs and end their turn before
 * acknowledging; failed cleanup is an error. Older hosts without a shutdown
 * endpoint require one extension reload or browser restart. Writes utility
 * results to stdout;
 * logger is borrowed for this call. Other platforms report unsupported. */
int cai_chrome_native_host_setup(const char *manifest, int remove,
                                 struct pslog_logger *logger, cai_error *error);
/** Install/remove a registration owned by a named embedding application.
 * program_name is a lowercase identifier of at most 32 bytes. The extension's
 * fixed native-host name and conflict checks remain unchanged. */
int cai_chrome_native_host_setup_named(const char *program_name,
                                       const char *manifest, int remove,
                                       struct pslog_logger *logger,
                                       cai_error *error);
/** As setup_named, selecting the private socket directory for shutdown.
 * NULL uses core discovery defaults. Registration is removed first; matching
 * connected hosts then receive a bounded shutdown request. */
int cai_chrome_native_host_setup_at(const char *program_name,
                                    const char *manifest, int remove,
                                    const char *socket_directory,
                                    struct pslog_logger *logger,
                                    cai_error *error);
/** Local browser connection settings; no Chrome configuration is changed. */
typedef struct cai_chrome_config {
  /** Absolute socket path; NULL discovers exactly one host in directory. */
  const char *socket_path;
  /** Private, user-owned directory; required by eager open unless socket_path
   * is set. Lazy open can discover the standard CAI directory. */
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
/** Create a lazy connection without contacting the host. NULL configuration
 * uses the standard CAI socket directory (runtime directory, cache, then HOME).
 * Discovery and identity validation happen on the first request. Missing hosts
 * and disconnected sockets can be retried by a later request; a sent operation
 * is never automatically retried. Configuration strings are copied. */
int cai_chrome_open_lazy(const cai_chrome_config *config, cai_chrome **out,
                         cai_error *error);
/** Send a browser JSON-RPC method and return its buffered JSON result.
 * params_json must be a JSON object. Browser operations require session_id,
 * turn_id and session_context in params, as defined by the extension.
 * Use cai_chrome_result_free on *out_json. Protocol/transport failures
 * invalidate the connection; calls are never automatically retried. Extension
 * errors include their message and numeric code. Cancellation follows the
 * runtime's worker cancellation scope, when present.
 * Known void-capable methods (attach/detach, tab marking/finalization, session
 * naming, turnEnded and executeCdp) return JSON "null" if the extension omits
 * the result member. Identity and tab-data replies still require a result;
 * screenshot handlers separately require image data.
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
 * deadline of five seconds. The CAI native host detaches tracked tabs before
 * forwarding turnEnded. Agent-created tabs close unless marked deliverable
 * or handoff; detaching alone does not close them.
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
