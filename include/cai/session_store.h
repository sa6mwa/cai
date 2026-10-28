/** @file cai/session_store.h
 *  Pluggable durable commit storage for CAI agent sessions.
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
  /** Stable event type, for example steering_queued. "subagent" contains a
   * bounded cai_execution_record envelope: kind is the child's journal event
   * type; session_id, subagent, parent_session_id, parent_call_id and
   * source_sequence identify its origin. The outer sequence orders it in the
   * parent. Chunk offset/final refer to the original child event's data.
   * These observations are replayable presentation, never parent model input.
   * Child execution records retain type "execution" and their own identity.
   */
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

/** Visit one resumable session in a local store. Timestamp is nanoseconds
 * since the Unix epoch and belongs to its newest complete commit. */
typedef int (*cai_agent_local_session_visit_fn)(
    void *context, const char *session_id,
    unsigned long long commit_created_at_ns, cai_error *error);

/**
 * Callback-backed append-only session journal.
 *
 * commit receives an incremental state edit owned by CAI. Consume it
 * synchronously without closing/retaining the source or replacing earlier
 * records. Append it atomically in a JSONL envelope with record_type "commit",
 * commit_created_at_ns, applied_event_sequence and state (the source value).
 * A successful return acknowledges durable publication. Its watermark covers
 * the input/control events incorporated into this edit. The source contains
 * only new history bytes, explicit prefix replacements, and index changes;
 * it is not independently resumable without the preceding journal.
 * scope and session_id are borrowed, non-empty opaque strings chosen by CAI or
 * the host; stores must not interpret scope as a filesystem path.
 * load_latest returns the entire ordered journal as a new source owned by the
 * caller, or sets *out to NULL
 * when no commit exists. Callbacks may run on the agent runtime worker
 * thread and must be thread-safe when one store is shared by multiple
 * runtimes.
 */
typedef struct cai_agent_session_store {
  int (*commit)(void *context, const char *scope, const char *session_id,
                cai_source *state, unsigned long long applied_event_sequence,
                cai_error *error);
  /**
   * Select the most recently committed session in scope. On success,
   * session_id receives its NUL-terminated stable identifier and *out receives
   * a new ordered JSONL journal source, including all preceding commits.
   * A no-session result is CAI_OK with *out set to NULL. Recovery obtains its
   * authoritative watermark from the journal, not a derived index hint.
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
   * Replay journal events strictly after the supplied commit watermark.
   * Invoke callback synchronously in strictly increasing sequence order.
   */
  int (*load_events_after)(void *context, const char *scope,
                           const char *session_id,
                           unsigned long long after_sequence,
                           cai_agent_session_event_fn callback,
                           void *callback_context, cai_error *error);
  /** Optional exact-ID journal lookup for hosts offering session selection.
   */
  int (*load_id)(void *context, const char *scope, const char *session_id,
                 cai_source **out,
                 unsigned long long *out_applied_event_sequence,
                 cai_error *error);
  /** Host-owned callback context, valid until every using runtime has closed.
   */
  void *context;
} cai_agent_session_store;

/** Session identifier capacity including NUL; at most 128 identifier bytes. */
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
 * The commit callback fsyncs accepted records before returning success;
 * opening the store alone writes no commit. Each commit stores
 * its own creation timestamp, so later journal-event appends do not affect
 * newest-commit selection. Tied commit timestamps select the
 * lexicographically later session identifier deterministically. Old snapshot
 * journals are not migrated or read as incremental journals.
 */
int cai_agent_local_session_store_open(
    const cai_agent_local_session_store_config *config,
    cai_agent_session_store *out, cai_error *error);
/** Close a store returned by cai_agent_local_session_store_open. */
void cai_agent_local_session_store_close(cai_agent_session_store *store);

/** Enumerate complete commits in one exact scope. Callback order is
 * unspecified; callers may sort by timestamp and ID. */
int cai_agent_local_session_store_list(const cai_agent_session_store *store,
                                       const char *scope,
                                       cai_agent_local_session_visit_fn visit,
                                       void *context, cai_error *error);

#ifdef __cplusplus
}
#endif

#endif
