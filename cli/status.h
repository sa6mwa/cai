#ifndef CAI_CLI_STATUS_H
#define CAI_CLI_STATUS_H

#include <cai/agent_runtime.h>
#include <softline/softline.h>

typedef struct cai_cli_status {
  char directory[160];
  char branch[160];
  char model_effort[192];
  char context[32];
  char quota[64];
  char goal[192];
  const char *elements[6];
  size_t count;
} cai_cli_status;

void cai_cli_status_init(cai_cli_status *status, const char *workspace,
                         const char *home);
void cai_cli_status_refresh_branch(cai_cli_status *status,
                                   const char *workspace);
void cai_cli_status_build(cai_cli_status *status, const char *model,
                          const char *effort, double context_percent,
                          int has_context, int has_five_hour,
                          double five_hour_remaining, int has_weekly,
                          double weekly_remaining,
                          const cai_agent_goal_snapshot *goal);
int cai_cli_status_apply(sl_t *sl, const cai_cli_status *status);

#endif
