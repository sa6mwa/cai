/** @file cai/session_store.h
 *  Pluggable durable checkpoint storage for CAI agent sessions.
 */
#ifndef CAI_SESSION_STORE_H
#define CAI_SESSION_STORE_H

#include <cai/cai.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Callback-backed store for complete agent-session checkpoints.
 *
 * checkpoint receives a source owned by CAI. It must consume it synchronously
 * if needed and must not close or retain the source. load_latest returns a new
 * source owned by the caller, or sets *out to NULL when no checkpoint exists.
 * Callbacks may run on the agent runtime worker thread and must be thread-safe
 * when one store is shared by multiple runtimes.
 */
typedef struct cai_agent_session_store {
  int (*checkpoint)(void *context, const char *scope, const char *session_id,
                    cai_source *state, cai_error *error);
  /**
   * Load the newest checkpoint for scope. On success with a checkpoint,
   * session_id receives its NUL-terminated stable identifier and *out receives
   * a new source. A no-checkpoint result is CAI_OK with *out set to NULL.
   */
  int (*load_latest)(void *context, const char *scope, char *session_id,
                     size_t session_id_capacity, cai_source **out,
                     cai_error *error);
  void *context;
} cai_agent_session_store;

/** Maximum bytes in a local or callback-provided session identifier. */
#define CAI_AGENT_SESSION_ID_MAX 129U

/** Configuration for the built-in append-only local JSONL session store. */
typedef struct cai_agent_local_session_store_config {
  /** Root directory for CAI session data; NULL selects the XDG state default.
   */
  const char *root_directory;
} cai_agent_local_session_store_config;

/** Initialize a zero-defaultable local session-store configuration. */
void cai_agent_local_session_store_config_init(
    cai_agent_local_session_store_config *config);
/**
 * Open CAI's local append-only JSONL store.
 *
 * Each canonical scope gets an opaque SHA-256 directory and each session gets
 * one JSONL file. Checkpoints are fsync'd before this function returns.
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
