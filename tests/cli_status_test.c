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
  sl_t *sl;
  int failures;
  failures = 0;
  memset(&goal, 0, sizeof(goal));
  cai_cli_status_init(&status, "/home/alice/project", "/home/alice");
  status.branch[0] = '\0';
  cai_cli_status_build(&status, "gpt-6-luna", "medium", 0.0, 0, 0, 0.0, 0, 0.0,
                       &goal);
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
  cai_cli_status_build(&status, "gpt-6-luna", "high", 37.4, 1, 1, 48.0, 1, 72.0,
                       &goal);
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
  cai_cli_status_build(&status, "gpt-6-luna", "medium", 0.0, 0, 0, 0.0, 0, 0.0,
                       &goal);
  failures += check(strcmp(status.elements[4], "goal active: Line Break") == 0,
                    "goal controls sanitized");
  cai_cli_status_refresh_branch(&status, "/proc");
  failures += check(status.branch[0] == '\0', "nonrepository branch hidden");
  goal.status = "complete";
  cai_cli_status_build(&status, "gpt-6-luna", "medium", 0.0, 0, 0, 0.0, 0, 0.0,
                       &goal);
  failures += check(status.count == 3U, "completed goal hidden");
  return failures > 0 ? 1 : 0;
}
