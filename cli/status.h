#ifndef CAI_CLI_STATUS_H
#define CAI_CLI_STATUS_H

#include "options.h"
#include <cai/agent_runtime.h>
#include <cai/quota.h>
#include <softline/softline.h>

#define CAI_CLI_STATUS_FIELD_COUNT 9
typedef struct cai_cli_status_fields {
  unsigned int ids[CAI_CLI_STATUS_FIELD_COUNT];
  size_t count;
} cai_cli_status_fields;

/* Expand default and aliases, keeping the first occurrence of each field.
 * Invalid/empty selections leave out unchanged and return an explanation. */
const char *cai_cli_status_fields_parse(const char *text,
                                        cai_cli_status_fields *out);

typedef struct cai_cli_status {
  char directory[160];
  char branch[160];
  char model_effort[192];
  char context[32];
  char usage[64];
  char credits[64];
  char goal[192];
  char provider[64];
  cai_cli_status_fields fields;
  long long branch_checked_ms;
  int branch_checked;
  const char *elements[CAI_CLI_STATUS_FIELD_COUNT];
  size_t count;
} cai_cli_status;

void cai_cli_status_init(cai_cli_status *status, const char *workspace,
                         const char *home);
/* In-process HEAD discovery, including parent repositories and gitfiles.
 * Detached/missing/broken metadata clears the label. Output is literal UTF-8,
 * clipped safely, with invalid bytes replaced and format controls escaped. */
void cai_cli_status_refresh_branch(cai_cli_status *status,
                                   const char *workspace);
/* Call only on work boundaries, never idle timers. Negative lookups share the
 * 60-second debounce. Explicit startup/status requests use the force API. */
void cai_cli_status_maybe_refresh_branch(cai_cli_status *status,
                                         const char *workspace);
/* Clock-injected seam for deterministic debounce checks; returns lookup done.
 */
int cai_cli_status_refresh_branch_at(cai_cli_status *status,
                                     const char *workspace,
                                     long long monotonic_ms, int force);
void cai_cli_status_build(cai_cli_status *status, const char *model,
                          const char *effort, const char *tier,
                          double context_percent, int has_context,
                          const cai_chatgpt_quota *quota,
                          double estimated_spend_usd,
                          double estimated_spend_eur, const char *provider,
                          const cai_agent_goal_snapshot *goal, int yolo);
/** Write status rows, including captured instruction sources when supplied,
 * into one Markdown table; return 0 on success. */
int cai_cli_status_markdown(char *out, size_t capacity,
                            const char *program_name, const char *model,
                            const char *effort, const char *provider,
                            const cai_agent_runtime_metrics *metrics,
                            const cai_chatgpt_quota *quota,
                            const cai_agent_instruction_status *instructions);
/* Append identifier rows to the same table, escaping provider text in bounded
 * chunks so IDs are neither truncated nor interpreted as Markdown/control. */
int cai_cli_status_ids_markdown(const cai_agent_runtime_ids *ids,
                                int (*write)(void *, const char *, size_t),
                                void *context);
/** Format the active summary or most recent turn duration for softline. */
int cai_cli_turn_status_message(char *out, size_t capacity,
                                const char *reasoning_summary,
                                const cai_agent_runtime_metrics *metrics);
int cai_cli_status_apply(sl_t *sl, const cai_cli_status *status);

#endif
