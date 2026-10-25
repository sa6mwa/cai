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

typedef struct ids_capture {
  char bytes[8192];
  size_t used;
  int fail;
} ids_capture;
static int ids_write(void *context, const char *bytes, size_t count) {
  ids_capture *out = context;
  if (out->fail || count >= sizeof(out->bytes) - out->used)
    return -1;
  memcpy(out->bytes + out->used, bytes, count);
  out->used += count;
  out->bytes[out->used] = '\0';
  return 0;
}
static int test_ids(void) {
  ids_capture out;
  cai_agent_runtime_ids ids;
  char long_id[4097];
  int failures = 0;
  memset(&out, 0, sizeof(out));
  memset(&ids, 0, sizeof(ids));
  ids.session_id = "session_1";
  failures += check(
      cai_cli_status_ids_markdown(&ids, ids_write, &out) == 0 &&
          strstr(out.bytes, "| Session ID | session\\_1 |") != NULL &&
          strstr(out.bytes, "| Last response ID | Not available |") != NULL &&
          strstr(out.bytes, "Provider conversation ID") == NULL,
      "identifier rows distinguish local and missing provider IDs");
  memset(&out, 0, sizeof(out));
  ids.response_id = "resp|`*\n\033_1";
  ids.conversation_id = "conv_1";
  failures +=
      check(cai_cli_status_ids_markdown(&ids, ids_write, &out) == 0 &&
                strstr(out.bytes, "resp\\|\\`\\*  \\_1") != NULL &&
                strstr(out.bytes, "| Provider conversation ID | conv\\_1 |") !=
                    NULL &&
                strchr(out.bytes, '\033') == NULL,
            "provider identifiers are literal sanitized table cells");
  memset(&out, 0, sizeof(out));
  memset(long_id, 'x', sizeof(long_id) - 1U);
  long_id[sizeof(long_id) - 1U] = '\0';
  ids.response_id = long_id;
  failures += check(cai_cli_status_ids_markdown(&ids, ids_write, &out) == 0 &&
                        strstr(out.bytes, long_id) != NULL,
                    "long provider identifiers are not truncated");
  out.fail = 1;
  failures +=
      check(cai_cli_status_ids_markdown(&ids, ids_write, &out) == -1 &&
                cai_cli_status_ids_markdown(NULL, ids_write, &out) == -1 &&
                cai_cli_status_ids_markdown(&ids, NULL, &out) == -1,
            "identifier writer propagates errors");
  return failures;
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
  failures = test_ids();
  memset(&goal, 0, sizeof(goal));
  memset(&metrics, 0, sizeof(metrics));
  memset(&quota, 0, sizeof(quota));
  cai_cli_status_init(&status, "/home/alice/project", "/home/alice");
  status.branch[0] = '\0';
  cai_cli_status_build(&status, "gpt-6-luna", "medium", NULL, 0.0, 0, &quota,
                       0.0, 0.0, "openai", &goal);
  failures += check(status.count == 3U, "unknown quota hidden");
  failures += check(strcmp(status.elements[0], "gpt-6-luna medium") == 0,
                    "model effort");
  {
    const char *tiers[] = {"default", "priority", "flex", "auto",
                           "bad\033tier"};
    const char *expected[] = {"gpt-6-luna high", "gpt-6-luna high fast",
                              "gpt-6-luna high flex", "gpt-6-luna high auto",
                              "gpt-6-luna high bad tier"};
    size_t i;
    for (i = 0U; i < sizeof(tiers) / sizeof(tiers[0]); i++) {
      cai_cli_status_build(&status, "gpt-6-luna", "high", tiers[i], 0.0, 0,
                           &quota, 0.0, 0.0, "openai", &goal);
      failures += check(strcmp(status.elements[0], expected[i]) == 0,
                        "selected tier suffix and sanitized label");
    }
  }
  {
    cai_agent_instruction_status sources;
    char rendered[1024];
    memset(&sources, 0, sizeof(sources));
    failures += check(
        cai_cli_status_markdown(rendered, sizeof(rendered), "cai", "gpt-6-luna",
                                "medium", "chatgpt", NULL, NULL,
                                &sources) == 0 &&
            strstr(rendered, "cai global AGENTS.md | No (missing or empty)") !=
                NULL &&
            strstr(rendered, "Codex global AGENTS.md | No (disabled)") !=
                NULL &&
            strstr(rendered, "Repository AGENTS.md | 0 file(s)") != NULL &&
            strstr(rendered, "| Metric | Value |") != NULL &&
            strstr(rendered, "Instruction files in context") == NULL &&
            strstr(rendered, "| Source |") == NULL &&
            strstr(rendered, "Restart CAI") == NULL &&
            strstr(strstr(rendered, "| Provider |"), "\n\n") == NULL,
        "status distinguishes missing from disabled instruction files");
    sources.global_agents_md_loaded = sources.codex_agents_md_loaded = 1;
    sources.codex_agents_md_enabled = 1;
    sources.repository_agents_md_count = 2U;
    failures +=
        check(cai_cli_status_markdown(rendered, sizeof(rendered), "cai",
                                      "gpt-6-luna", "medium", "chatgpt", NULL,
                                      NULL, &sources) == 0 &&
                  strstr(rendered, "Codex global AGENTS.md | Yes") != NULL &&
                  strstr(rendered, "Repository AGENTS.md | 2 file(s)") != NULL,
              "status shows captured instruction sources");
    sources.codex_agents_md_loaded = 0;
    failures += check(
        cai_cli_status_markdown(rendered, sizeof(rendered), "cai", "gpt-6-luna",
                                "medium", "chatgpt", NULL, NULL,
                                &sources) == 0 &&
            strstr(rendered,
                   "Codex global AGENTS.md | No (missing or empty)") != NULL &&
            cai_cli_status_markdown(rendered, 1U, "cai", "gpt-6-luna", "medium",
                                    "chatgpt", NULL, NULL, &sources) != 0 &&
            cai_cli_status_markdown(rendered, sizeof(rendered), "cai",
                                    "gpt-6-luna", "medium", "chatgpt", NULL,
                                    NULL, NULL) == 0 &&
            strstr(rendered, "AGENTS.md") == NULL,
        "instruction status handles absent files and bounded output");
    {
      size_t capacity;
      cai_cli_status_markdown(rendered, sizeof(rendered), "cai", "gpt-6-luna",
                              "medium", "chatgpt", NULL, NULL, &sources);
      capacity = strlen(rendered) + 1U;
      failures +=
          check(cai_cli_status_markdown(rendered, capacity, "cai", "gpt-6-luna",
                                        "medium", "chatgpt", NULL, NULL,
                                        &sources) == 0 &&
                    cai_cli_status_markdown(rendered, capacity - 1U, "cai",
                                            "gpt-6-luna", "medium", "chatgpt",
                                            NULL, NULL, &sources) != 0,
                "combined status respects exact buffer capacity");
    }
  }
  failures +=
      check(strcmp(status.elements[1], "ctx --") == 0, "unknown context");
  failures += check(strcmp(status.elements[2], "~/project") == 0, "home path");
  quota.has_credit_balance = 1;
  quota.credit_balance = 42.75;
  cai_cli_status_build(&status, "gpt-6-luna", "medium", NULL, 0.0, 0, &quota,
                       0.0, 0.0, "openai", &goal);
  failures +=
      check(status.count == 4U && status.usage[0] == '\0' &&
                strcmp(status.elements[3], "crd 43") == 0,
            "available fractional credits round up without quota windows");
  quota.credit_balance = 42.0;
  cai_cli_status_build(&status, "gpt-6-luna", "medium", NULL, 0.0, 0, &quota,
                       0.0, 0.0, "openai", &goal);
  failures +=
      check(status.count == 4U && strcmp(status.elements[3], "crd 42") == 0,
            "whole credits have no decimal suffix");
  quota.credit_balance = 42.25;
  cai_cli_status_build(&status, "gpt-6-luna", "medium", NULL, 0.0, 0, &quota,
                       0.0, 0.0, "openai", &goal);
  failures +=
      check(status.count == 4U && strcmp(status.elements[3], "crd 42") == 0,
            "fractional credits round down");
  quota.credit_balance = 0.0;
  cai_cli_status_build(&status, "gpt-6-luna", "medium", NULL, 0.0, 0, &quota,
                       0.0, 0.0, "openai", &goal);
  failures +=
      check(status.count == 4U && strcmp(status.elements[3], "crd 0") == 0,
            "zero credit balance remains visible");
  quota.credits_unlimited = 1;
  quota.has_credit_balance = 0;
  cai_cli_status_build(&status, "gpt-6-luna", "medium", NULL, 0.0, 0, &quota,
                       0.0, 0.0, "openai", &goal);
  failures += check(status.count == 4U &&
                        strcmp(status.elements[3], "crd unlimited") == 0,
                    "unlimited credits without numeric balance");
  quota.has_credit_balance = 1;
  quota.credits_unlimited = 0;
  quota.credit_balance = 42.75;
  strcpy(status.branch, "feature/ui");
  goal.has_goal = 1;
  goal.status = "active";
  goal.objective = "Ship agent";
  quota.has_short_window = 1;
  quota.short_window_seconds = 18000;
  quota.short_window_remaining_percent = 48.0;
  quota.has_weekly = 1;
  quota.weekly_remaining_percent = 72.0;
  cai_cli_status_build(&status, "gpt-6-luna", "high", NULL, 37.4, 1, &quota,
                       0.0, 0.0, "openai", &goal);
  failures += check(status.count == 7U, "full element count");
  failures +=
      check(strcmp(status.elements[1], "ctx 37%") == 0, "context rounded");
  failures +=
      check(strcmp(status.elements[3], "w 72% 5h 48%") == 0, "quota order");
  failures += check(strcmp(status.elements[4], "crd 43") == 0, "credits order");
  failures +=
      check(strcmp(status.elements[5], "feature/ui") == 0, "branch order");
  failures +=
      check(strcmp(status.elements[6], "goal=active 0m") == 0, "goal order");
  sl = sl_create();
  failures += check(sl != NULL && cai_cli_status_apply(sl, &status) == 0,
                    "softline accepts status");
  if (sl != NULL) {
    sl_destroy(sl);
  }
  goal.objective = "Line\nBreak";
  memset(&quota, 0, sizeof(quota));
  cai_cli_status_build(&status, "gpt-6-luna", "medium", NULL, 0.0, 0, &quota,
                       0.0, 0.0, "openai", &goal);
  failures += check(status.count == 5U && status.credits[0] == '\0',
                    "unavailable credits cleared on refresh");
  failures += check(strcmp(status.elements[4], "goal=active 0m") == 0,
                    "goal objective omitted, including controls");
  {
    const long long seconds[] = {-1LL,   59LL,    60LL,    2759LL,
                                 3600LL, 38699LL, 86400LL, 903900LL};
    const char *expected[] = {"goal=active 0m",     "goal=active 0m",
                              "goal=active 1m",     "goal=active 45m",
                              "goal=active 1h0m",   "goal=active 10h44m",
                              "goal=active 1d0h0m", "goal=active 10d11h5m"};
    size_t i;
    for (i = 0U; i < sizeof(seconds) / sizeof(seconds[0]); i++) {
      goal.elapsed_seconds = seconds[i];
      cai_cli_status_build(&status, "gpt-6-luna", "medium", NULL, 0.0, 0,
                           &quota, 0.0, 0.0, "openai", &goal);
      failures += check(strcmp(status.elements[4], expected[i]) == 0,
                        "compact goal duration without seconds");
    }
    goal.status = "paused";
    goal.elapsed_seconds = 902700LL;
    cai_cli_status_build(&status, "gpt-6-luna", "medium", NULL, 0.0, 0, &quota,
                         0.0, 0.0, "openai", &goal);
    failures += check(strcmp(status.elements[4], "goal=paused 10d10h45m") == 0,
                      "paused goal retains elapsed time");
  }
  cai_cli_status_refresh_branch(&status, "/proc");
  failures += check(status.branch[0] == '\0', "nonrepository branch hidden");
  goal.status = "complete";
  cai_cli_status_build(&status, "gpt-6-luna", "medium", NULL, 0.0, 0, &quota,
                       0.0, 0.0, "openai", &goal);
  failures +=
      check(status.count == 4U &&
                strcmp(status.elements[3], "goal=achieved 10d10h45m") == 0,
            "achieved goal shows final elapsed time");
  goal.has_goal = 0;
  cai_cli_status_build(&status, "gpt-6-luna", "medium", NULL, 0.0, 0, &quota,
                       0.0, 0.0, "openai", &goal);
  failures += check(status.count == 3U, "cleared goal hidden");
  quota.has_weekly = 1;
  quota.weekly_remaining_percent = 73.0;
  cai_cli_status_build(&status, "gpt-6-luna", "medium", NULL, 0.0, 0, &quota,
                       0.0, 0.0, "openai", &goal);
  failures += check(strcmp(status.usage, "w 73%") == 0, "weekly only");
  quota.has_short_window = 1;
  quota.short_window_seconds = 3600;
  quota.short_window_remaining_percent = 55.0;
  cai_cli_status_build(&status, "gpt-6-luna", "medium", NULL, 0.0, 0, &quota,
                       0.0, 0.0, "openai", &goal);
  failures += check(strcmp(status.usage, "w 73% 1h 55%") == 0, "hourly label");
  quota.has_credit_balance = 1;
  quota.credit_balance = 42.5;
  cai_cli_status_build(&status, "gpt-6-luna", "medium", NULL, 0.0, 0, &quota,
                       0.0, 0.0, "openai", &goal);
  failures += check(status.count == 5U && status.credits[0] != '\0',
                    "credits present before provider switch");
  cai_cli_status_build(&status, "gpt-6-luna", "medium", NULL, 20.0, 1, NULL,
                       1.25, 0.0, "openai", &goal);
  failures +=
      check(strcmp(status.usage, "cost ~$1.2500") == 0, "API cost status");
  failures += check(status.count == 4U, "API status element count");
  failures += check(status.credits[0] == '\0', "API provider hides credits");
  cai_cli_status_build(&status, "gpt-6-luna", "medium", NULL, 0.0, 0, NULL, 0.0,
                       0.0, "openai", &goal);
  failures += check(strcmp(status.usage, "cost ?$") == 0,
                    "unknown API cost placeholder");
  cai_cli_status_build(&status, "zai-org/GLM-5.3-Flash", "medium", NULL, 0.0, 0,
                       NULL, 0.0, 1.25, "berget", &goal);
  failures += check(strcmp(status.usage, "cost ~€1.2500") == 0,
                    "Berget cost status uses EUR");
  cai_cli_status_build(&status, "zai-org/GLM-5.3-Flash", "medium", NULL, 0.0, 0,
                       NULL, 0.0, 0.0, "berget", &goal);
  failures += check(strcmp(status.usage, "cost ?€") == 0,
                    "Berget unknown cost uses EUR");
  metrics.context_window_tokens = 200000;
  metrics.context_used_tokens = 50000;
  metrics.has_context_usage = 1;
  metrics.session_usage.estimated_spend_usd = 1.25;
  quota.has_credit_balance = 1;
  quota.credit_balance = 42.5;
  failures += check(cai_cli_status_markdown(markdown, sizeof(markdown), "cai",
                                            "gpt-6-luna", "medium", "chatgpt",
                                            &metrics, &quota, NULL) == 0,
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
  metrics.session_usage.estimated_spend_usd = 0.0;
  metrics.session_usage.estimated_spend_eur = 1.25;
  failures +=
      check(cai_cli_status_markdown(markdown, sizeof(markdown), "cai",
                                    "zai-org/GLM-5.3-Flash", "medium", "berget",
                                    &metrics, NULL, NULL) == 0,
            "Berget status markdown builds");
  failures += check(strstr(markdown, "| Cost estimate | €1.2500 EUR |") != NULL,
                    "Berget cost row uses EUR");
  metrics.session_usage.estimated_spend_usd = 1.25;
  metrics.session_usage.estimated_spend_eur = 0.0;
  failures += check(
      cai_cli_status_markdown(markdown, sizeof(markdown), "cai", "gpt-6-luna",
                              "medium", "openai", &metrics, NULL, NULL) == 0 &&
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
