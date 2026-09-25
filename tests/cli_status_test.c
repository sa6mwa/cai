#include "../cli/status.h"

#include <stdio.h>
#include <string.h>

static int check(int condition, const char *name) {
  if (!condition) {
    fprintf(stderr, "status failed: %s\n", name);
    return 1;
  }
  return 0;
}

int main(void) {
  cai_cli_status status;
  cai_agent_goal_snapshot goal;
  cai_agent_runtime_metrics metrics;
  cai_chatgpt_quota quota;
  char markdown[2048];
  sl_t *sl;
  int failures;
  failures = 0;
  memset(&goal, 0, sizeof(goal));
  memset(&metrics, 0, sizeof(metrics));
  memset(&quota, 0, sizeof(quota));
  cai_cli_status_init(&status, "/home/alice/project", "/home/alice");
  status.branch[0] = '\0';
  cai_cli_status_build(&status, "gpt-6-luna", "medium", 0.0, 0, &quota, &goal);
  failures += check(status.count == 3U, "unknown quota hidden");
  failures += check(strcmp(status.elements[0], "gpt-6-luna medium") == 0,
                    "model effort");
  failures +=
      check(strcmp(status.elements[1], "ctx --") == 0, "unknown context");
  failures += check(strcmp(status.elements[2], "~/project") == 0, "home path");
  strcpy(status.branch, "feature/ui");
  goal.has_goal = 1;
  goal.status = "active";
  goal.objective = "Ship agent";
  quota.has_short_window = 1;
  quota.short_window_seconds = 18000;
  quota.short_window_remaining_percent = 48.0;
  quota.has_weekly = 1;
  quota.weekly_remaining_percent = 72.0;
  cai_cli_status_build(&status, "gpt-6-luna", "high", 37.4, 1, &quota, &goal);
  failures += check(status.count == 6U, "full element count");
  failures +=
      check(strcmp(status.elements[1], "ctx 37%") == 0, "context rounded");
  failures +=
      check(strcmp(status.elements[3], "w 72% 5h 48%") == 0, "quota order");
  failures +=
      check(strcmp(status.elements[4], "feature/ui") == 0, "branch order");
  failures += check(strcmp(status.elements[5], "goal active: Ship agent") == 0,
                    "goal order");
  sl = sl_create();
  failures += check(sl != NULL && cai_cli_status_apply(sl, &status) == 0,
                    "softline accepts status");
  if (sl != NULL) {
    sl_destroy(sl);
  }
  goal.objective = "Line\nBreak";
  memset(&quota, 0, sizeof(quota));
  cai_cli_status_build(&status, "gpt-6-luna", "medium", 0.0, 0, &quota, &goal);
  failures += check(strcmp(status.elements[4], "goal active: Line Break") == 0,
                    "goal controls sanitized");
  cai_cli_status_refresh_branch(&status, "/proc");
  failures += check(status.branch[0] == '\0', "nonrepository branch hidden");
  goal.status = "complete";
  cai_cli_status_build(&status, "gpt-6-luna", "medium", 0.0, 0, &quota, &goal);
  failures += check(status.count == 3U, "completed goal hidden");
  quota.has_weekly = 1;
  quota.weekly_remaining_percent = 73.0;
  cai_cli_status_build(&status, "gpt-6-luna", "medium", 0.0, 0, &quota, &goal);
  failures += check(strcmp(status.quota, "w 73%") == 0, "weekly only");
  quota.has_short_window = 1;
  quota.short_window_seconds = 3600;
  quota.short_window_remaining_percent = 55.0;
  cai_cli_status_build(&status, "gpt-6-luna", "medium", 0.0, 0, &quota, &goal);
  failures += check(strcmp(status.quota, "w 73% 1h 55%") == 0, "hourly label");
  metrics.context_window_tokens = 200000;
  metrics.context_used_tokens = 50000;
  metrics.has_context_usage = 1;
  metrics.session_usage.estimated_spend_usd = 1.25;
  quota.has_credit_balance = 1;
  quota.credit_balance = 42.5;
  failures +=
      check(cai_cli_status_markdown(markdown, sizeof(markdown), "gpt-6-luna",
                                    "medium", &metrics, &quota) == 0,
            "status markdown builds");
  failures +=
      check(strstr(markdown, "| Context window | 200000 tokens |") != NULL,
            "context window row");
  failures +=
      check(strstr(markdown, "| Context used | 50000 tokens (25%) |") != NULL,
            "context used row");
  failures +=
      check(strstr(markdown,
                   "| Cost estimate | $1.2500 USD (API equivalent) |") != NULL,
            "cost row");
  failures +=
      check(strstr(markdown, "| Weekly limit | 73% remaining |") != NULL,
            "weekly row");
  failures +=
      check(strstr(markdown, "| 1-hour limit | 55% remaining |") != NULL,
            "hourly row");
  failures += check(strstr(markdown, "| Credits left | 42.50 |") != NULL,
                    "credits row");
  return failures > 0 ? 1 : 0;
}
