#define _POSIX_C_SOURCE 200809L

#include "../cli/status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
  char turn_message[640];
  struct tm finished_local;
  sl_t *sl;
  int failures;
  failures = 0;
  memset(&goal, 0, sizeof(goal));
  memset(&metrics, 0, sizeof(metrics));
  memset(&quota, 0, sizeof(quota));
  cai_cli_status_init(&status, "/home/alice/project", "/home/alice");
  status.branch[0] = '\0';
  cai_cli_status_build(&status, "gpt-6-luna", "medium", 0.0, 0, &quota, 0.0,
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
  quota.has_short_window = 1;
  quota.short_window_seconds = 18000;
  quota.short_window_remaining_percent = 48.0;
  quota.has_weekly = 1;
  quota.weekly_remaining_percent = 72.0;
  cai_cli_status_build(&status, "gpt-6-luna", "high", 37.4, 1, &quota, 0.0,
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
  memset(&quota, 0, sizeof(quota));
  cai_cli_status_build(&status, "gpt-6-luna", "medium", 0.0, 0, &quota, 0.0,
                       &goal);
  failures += check(strcmp(status.elements[4], "goal active: Line Break") == 0,
                    "goal controls sanitized");
  cai_cli_status_refresh_branch(&status, "/proc");
  failures += check(status.branch[0] == '\0', "nonrepository branch hidden");
  goal.status = "complete";
  cai_cli_status_build(&status, "gpt-6-luna", "medium", 0.0, 0, &quota, 0.0,
                       &goal);
  failures += check(status.count == 3U, "completed goal hidden");
  quota.has_weekly = 1;
  quota.weekly_remaining_percent = 73.0;
  cai_cli_status_build(&status, "gpt-6-luna", "medium", 0.0, 0, &quota, 0.0,
                       &goal);
  failures += check(strcmp(status.usage, "w 73%") == 0, "weekly only");
  quota.has_short_window = 1;
  quota.short_window_seconds = 3600;
  quota.short_window_remaining_percent = 55.0;
  cai_cli_status_build(&status, "gpt-6-luna", "medium", 0.0, 0, &quota, 0.0,
                       &goal);
  failures += check(strcmp(status.usage, "w 73% 1h 55%") == 0, "hourly label");
  cai_cli_status_build(&status, "gpt-6-luna", "medium", 20.0, 1, NULL, 1.25,
                       &goal);
  failures +=
      check(strcmp(status.usage, "cost ~$1.2500") == 0, "API cost status");
  failures += check(status.count == 4U, "API status element count");
  cai_cli_status_build(&status, "gpt-6-luna", "medium", 0.0, 0, NULL, 0.0,
                       &goal);
  failures += check(strcmp(status.usage, "cost ?$") == 0,
                    "unknown API cost placeholder");
  metrics.context_window_tokens = 200000;
  metrics.context_used_tokens = 50000;
  metrics.has_context_usage = 1;
  metrics.session_usage.estimated_spend_usd = 1.25;
  quota.has_credit_balance = 1;
  quota.credit_balance = 42.5;
  failures +=
      check(cai_cli_status_markdown(markdown, sizeof(markdown), "gpt-6-luna",
                                    "medium", "chatgpt", &metrics, &quota) == 0,
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
  failures +=
      check(cai_cli_status_markdown(markdown, sizeof(markdown), "gpt-6-luna",
                                    "medium", "openai", &metrics, NULL) == 0 &&
                strstr(markdown, "| Provider | openai |") != NULL &&
                strstr(markdown, "| Cost estimate | $1.2500 USD |") != NULL &&
                strstr(markdown, "Weekly limit") == NULL,
            "API provider status rows");
  memset(&metrics, 0, sizeof(metrics));
  failures +=
      check(cai_cli_turn_status_message(turn_message, sizeof(turn_message), "",
                                        &metrics) == 0 &&
                turn_message[0] == '\0',
            "idle turn has no message");
  metrics.turn_active = 1;
  metrics.turn_elapsed_ms = 812000ULL;
  failures +=
      check(cai_cli_turn_status_message(turn_message, sizeof(turn_message),
                                        "Checking files", &metrics) == 0 &&
                strcmp(turn_message, "Checking files (13m 32s)") == 0,
            "active minutes seconds");
  failures += check(
      cai_cli_turn_status_message(turn_message, sizeof(turn_message),
                                  " \nChecking\n files  ", &metrics) == 0 &&
          strcmp(turn_message, "Checking files (13m 32s)") == 0,
      "reasoning summary remains a single status line");
  metrics.turn_elapsed_ms = 3720000ULL;
  failures +=
      check(cai_cli_turn_status_message(turn_message, sizeof(turn_message),
                                        "Checking files", &metrics) == 0 &&
                strcmp(turn_message, "Checking files (1h 2m)") == 0,
            "active hours minutes");
  metrics.turn_elapsed_ms = 93780000ULL;
  failures +=
      check(cai_cli_turn_status_message(turn_message, sizeof(turn_message), "",
                                        &metrics) == 0 &&
                strcmp(turn_message, "Working (1d 2h 3m)") == 0,
            "active days and no fabricated reasoning");
  failures += check(setenv("TZ", "UTC", 1) == 0, "set test timezone");
  tzset();
  memset(&finished_local, 0, sizeof(finished_local));
  finished_local.tm_year = 2026 - 1900;
  finished_local.tm_mon = 8;
  finished_local.tm_mday = 25;
  finished_local.tm_hour = 22;
  finished_local.tm_min = 43;
  metrics.turn_active = 0;
  metrics.has_last_turn = 1;
  metrics.last_turn_duration_ms = 812000ULL;
  metrics.last_turn_finished_unix_seconds = (long long)mktime(&finished_local);
  failures +=
      check(cai_cli_turn_status_message(turn_message, sizeof(turn_message),
                                        "ignored", &metrics) == 0 &&
                strcmp(turn_message, "Worked for 13m 32s - 252243") == 0,
            "completed local DTG");
  failures += check(setenv("TZ", "UTC-2", 1) == 0, "shift test timezone");
  tzset();
  failures +=
      check(cai_cli_turn_status_message(turn_message, sizeof(turn_message),
                                        "ignored", &metrics) == 0 &&
                strcmp(turn_message, "Worked for 13m 32s - 260043") == 0,
            "completed DTG uses local timezone and date rollover");
  return failures > 0 ? 1 : 0;
}
