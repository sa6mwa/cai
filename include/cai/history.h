/** @file cai/history.h
 * Whole conversation entries used by runtime history inspection and trimming.
 */
#ifndef CAI_HISTORY_H
#define CAI_HISTORY_H
#include <cai/cai.h>
#ifdef __cplusplus
extern "C" {
#endif

/** Indivisible units of retained conversation history. */
typedef enum cai_agent_history_kind {
  CAI_AGENT_HISTORY_TURN = 1,
  CAI_AGENT_HISTORY_SHELL = 2,
  CAI_AGENT_HISTORY_COMPACTION = 3
} cai_agent_history_kind;

/** Borrowed entry view, valid only during its visitor callback. */
typedef struct cai_agent_history_entry {
  /** Number of newest entries removed to include this entry; starts at one. */
  size_t remove_count;
  cai_agent_history_kind kind;
  /** Bounded, plain, single-line assistant summary; falls back to the input. */
  const char *summary;
} cai_agent_history_entry;

/** Return CAI_OK to continue visiting, or an error to stop. Do not mutate the
 * runtime from this callback. */
typedef int (*cai_agent_history_entry_fn)(void *context,
                                          const cai_agent_history_entry *entry,
                                          cai_error *error);
#ifdef __cplusplus
}
#endif
#endif
