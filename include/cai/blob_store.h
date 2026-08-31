/** @file cai/blob_store.h
 *  Callback-backed named byte-object storage for CAI-owned state.
 */
#ifndef CAI_BLOB_STORE_H
#define CAI_BLOB_STORE_H

#include <cai/cai.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A host-owned named byte store.
 *
 * load returns a new source owned by CAI, or CAI_OK with *out set to NULL
 * when key is absent. replace receives a source owned by CAI and must consume
 * it synchronously; it must not close or retain that source. Callbacks may run
 * from an auth refresh or agent-runtime worker context and therefore must not
 * enter a language runtime or otherwise require an owner thread. key is an
 * opaque, non-empty CAI-defined name such as "auth.json" or "AGENTS.md".
 */
typedef struct cai_blob_store {
  /** Load a complete named value, or return CAI_OK with *out == NULL if absent.
   */
  int (*load)(void *context, const char *key, cai_source **out,
              cai_error *error);
  /** Atomically replace a complete named value. */
  int (*replace)(void *context, const char *key, cai_source *value,
                 cai_error *error);
  /** Host-owned callback context, valid until every consumer has closed. */
  void *context;
} cai_blob_store;

#ifdef __cplusplus
}
#endif

#endif
