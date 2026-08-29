/** @file cai/session_store.h
 *  Pluggable durable checkpoint storage for CAI agent sessions.
 */
#ifndef CAI_SESSION_STORE_H
#define CAI_SESSION_STORE_H

#include <cai/cai.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A durable event in the runtime-owned session journal. */
typedef struct cai_agent_session_event {
  /** Monotonic identifier assigned by CAI for this session. */
  unsigned long long sequence;
  /** Stable event type, for example steering_queued. */
  const char *type;
  /** Optional UTF-8 payload; borrowed for the callback duration. */
  const char *data;
} cai_agent_session_event;

/**
 * Consume one journal event during synchronous replay. event and all of its
 * strings are borrowed only for this call; copy any data that must outlive it.
 * Return non-OK to stop recovery and fail the enclosing load operation.
 */
typedef int (*cai_agent_session_event_fn)(void *context,
                                          const cai_agent_session_event *event,
                                          cai_error *error);

/**
 * Callback-backed store for complete agent-session checkpoints.
 *
 * checkpoint receives a source owned by CAI. It must consume it synchronously
 * if needed and must not close or retain the source. Its watermark declares
 * every state-changing journal event incorporated into that checkpoint.
 * scope and session_id are borrowed, non-empty opaque strings chosen by CAI or
 * the host; stores must not interpret scope as a filesystem path.
 * load_latest returns a new source owned by the caller, or sets *out to NULL
 * when no checkpoint exists. Callbacks may run on the agent runtime worker
 * thread and must be thread-safe when one store is shared by multiple
 * runtimes.
 */
typedef struct cai_agent_session_store {
  int (*checkpoint)(void *context, const char *scope, const char *session_id,
                    cai_source *state,
                    unsigned long long applied_event_sequence,
                    cai_error *error);
  /**
   * Load the newest checkpoint for scope. On success with a checkpoint,
   * session_id receives its NUL-terminated stable identifier and *out receives
   * a new source. A no-checkpoint result is CAI_OK with *out set to NULL.
   */
  int (*load_latest)(void *context, const char *scope, char *session_id,
                     size_t session_id_capacity, cai_source **out,
                     unsigned long long *out_applied_event_sequence,
                     cai_error *error);
  /**
   * Append and durably acknowledge a journal event before returning success.
   * event and its strings are borrowed only for the callback duration.
   */
  int (*append_event)(void *context, const char *scope, const char *session_id,
                      const cai_agent_session_event *event, cai_error *error);
  /**
   * Replay journal events strictly after the supplied checkpoint watermark.
   * Invoke callback synchronously in strictly increasing sequence order.
   */
  int (*load_events_after)(void *context, const char *scope,
                           const char *session_id,
                           unsigned long long after_sequence,
                           cai_agent_session_event_fn callback,
                           void *callback_context, cai_error *error);
  /** Host-owned callback context, valid until every using runtime has closed.
   */
  void *context;
} cai_agent_session_store;

/** Maximum bytes in a local or callback-provided session identifier. */
#define CAI_AGENT_SESSION_ID_MAX 129U

/** Configuration for the built-in append-only local JSONL session store. */
typedef struct cai_agent_local_session_store_config {
  /**
   * Root directory for CAI session data; NULL selects
   * $XDG_STATE_HOME/cai/sessions, or $HOME/.local/state/cai/sessions. The
   * resulting directory must be private (owned by the effective user with no
   * group or other permissions).
   */
  const char *root_directory;
} cai_agent_local_session_store_config;

/** Initialize a zero-defaultable local session-store configuration. */
void cai_agent_local_session_store_config_init(
    cai_agent_local_session_store_config *config);
/**
 * Open CAI's local append-only JSONL store.
 *
 * Each non-empty scope key gets an opaque SHA-256 directory and each opaque
 * session identifier gets one private JSONL file through a reversible safe
 * filename encoding. The local store does not interpret scope or session keys
 * as paths. The returned callback table owns private local-store state and is
 * valid until cai_agent_local_session_store_close.
 * Checkpoints are fsync'd before this function returns. Each checkpoint stores
 * its own creation timestamp, so later journal-event appends do not affect
 * newest-checkpoint selection. Tied checkpoint timestamps select the
 * lexicographically later session identifier deterministically. Legacy
 * checkpoint records without a timestamp fall back to their journal mtime.
 */
int cai_agent_local_session_store_open(
    const cai_agent_local_session_store_config *config,
    cai_agent_session_store *out, cai_error *error);
/** Close a store returned by cai_agent_local_session_store_open. */
void cai_agent_local_session_store_close(cai_agent_session_store *store);

#ifdef __cplusplus
}
#endif

#endif
