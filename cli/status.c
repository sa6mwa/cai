#define _POSIX_C_SOURCE 200809L

#include "status.h"

#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static void cai_cli_status_copy(char *out, size_t capacity, const char *text) {
  size_t i;
  size_t written;
  if (text == NULL) {
    text = "";
  }
  i = 0U;
  written = 0U;
  while (text[i] != '\0' && written + 1U < capacity) {
    unsigned char ch = (unsigned char)text[i];
    size_t sequence;
    unsigned int codepoint;
    size_t j;
    if (ch < 0x80U) {
      out[written++] = ch < 32U || ch == 127U ? ' ' : (char)ch;
      i++;
      continue;
    }
    if (ch >= 0xc2U && ch <= 0xdfU) {
      sequence = 2U;
      codepoint = ch & 0x1fU;
    } else if (ch >= 0xe0U && ch <= 0xefU) {
      sequence = 3U;
      codepoint = ch & 0x0fU;
    } else if (ch >= 0xf0U && ch <= 0xf4U) {
      sequence = 4U;
      codepoint = ch & 0x07U;
    } else {
      out[written++] = '?';
      i++;
      continue;
    }
    for (j = 1U; j < sequence && text[i + j] != '\0'; j++) {
      ch = (unsigned char)text[i + j];
      if ((ch & 0xc0U) != 0x80U) {
        break;
      }
      codepoint = (codepoint << 6U) | (ch & 0x3fU);
    }
    if (j != sequence || (sequence == 2U && codepoint < 0x80U) ||
        (sequence == 3U && codepoint < 0x800U) ||
        (sequence == 4U && codepoint < 0x10000U) || codepoint > 0x10ffffU ||
        (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
      out[written++] = '?';
      i++;
      continue;
    }
    if (codepoint >= 0x80U && codepoint <= 0x9fU) {
      out[written++] = ' ';
      i += sequence;
      continue;
    }
    if (written + sequence >= capacity) {
      break;
    }
    memcpy(out + written, text + i, sequence);
    written += sequence;
    i += sequence;
  }
  out[written] = '\0';
}

void cai_cli_status_init(cai_cli_status *status, const char *workspace,
                         const char *home) {
  const char *display;
  const char *base;
  const char *parent;
  size_t home_length;
  memset(status, 0, sizeof(*status));
  display = workspace;
  home_length = home != NULL ? strlen(home) : 0U;
  if (home_length > 0U && strncmp(workspace, home, home_length) == 0 &&
      (workspace[home_length] == '/' || workspace[home_length] == '\0')) {
    char relative[sizeof(status->directory)];
    snprintf(relative, sizeof(relative), "~%s", workspace + home_length);
    cai_cli_status_copy(status->directory, sizeof(status->directory), relative);
    display = status->directory;
  } else {
    cai_cli_status_copy(status->directory, sizeof(status->directory),
                        workspace);
    display = status->directory;
  }
  if (strlen(display) > 42U) {
    base = strrchr(display, '/');
    if (base != NULL && base != display) {
      parent = base - 1;
      while (parent > display && parent[-1] != '/') {
        parent--;
      }
      {
        char short_directory[sizeof(status->directory)];
        snprintf(short_directory, sizeof(short_directory), "~/.../%s", parent);
        cai_cli_status_copy(status->directory, sizeof(status->directory),
                            short_directory);
      }
    }
  }
  cai_cli_status_refresh_branch(status, workspace);
}

void cai_cli_status_refresh_branch(cai_cli_status *status,
                                   const char *workspace) {
  int descriptors[2];
  pid_t child;
  ssize_t count;
  int wait_status;
  int action_status;
  posix_spawn_file_actions_t actions;
  char *workspace_arg;
  char git[] = "git";
  char flag_c[] = "-C";
  char symbolic_ref[] = "symbolic-ref";
  char quiet[] = "--quiet";
  char short_flag[] = "--short";
  char head[] = "HEAD";
  char *arguments[8];
  char branch[sizeof(status->branch)];
  status->branch[0] = '\0';
  if (pipe(descriptors) != 0) {
    return;
  }
  workspace_arg = strdup(workspace);
  if (workspace_arg == NULL) {
    close(descriptors[0]);
    close(descriptors[1]);
    return;
  }
  arguments[0] = git;
  arguments[1] = flag_c;
  arguments[2] = workspace_arg;
  arguments[3] = symbolic_ref;
  arguments[4] = quiet;
  arguments[5] = short_flag;
  arguments[6] = head;
  arguments[7] = NULL;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    free(workspace_arg);
    close(descriptors[0]);
    close(descriptors[1]);
    return;
  }
  action_status =
      posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDOUT_FILENO);
  if (action_status == 0) {
    action_status = posix_spawn_file_actions_addclose(&actions, descriptors[0]);
  }
  if (action_status == 0) {
    action_status = posix_spawn_file_actions_addclose(&actions, descriptors[1]);
  }
  if (action_status == 0) {
    action_status = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO,
                                                     "/dev/null", O_WRONLY, 0);
  }
  if (action_status != 0) {
    posix_spawn_file_actions_destroy(&actions);
    free(workspace_arg);
    close(descriptors[0]);
    close(descriptors[1]);
    return;
  }
  wait_status = posix_spawnp(&child, git, &actions, NULL, arguments, environ);
  posix_spawn_file_actions_destroy(&actions);
  free(workspace_arg);
  close(descriptors[1]);
  if (wait_status != 0) {
    close(descriptors[0]);
    return;
  }
  count = read(descriptors[0], branch, sizeof(branch) - 1U);
  close(descriptors[0]);
  if (waitpid(child, &wait_status, 0) != child || !WIFEXITED(wait_status) ||
      WEXITSTATUS(wait_status) != 0 || count <= 0) {
    return;
  }
  branch[count] = '\0';
  if (branch[count - 1] == '\n') {
    branch[count - 1] = '\0';
  }
  cai_cli_status_copy(status->branch, sizeof(status->branch), branch);
}

void cai_cli_status_build(cai_cli_status *status, const char *model,
                          const char *effort, double context_percent,
                          int has_context, const cai_chatgpt_quota *quota,
                          double estimated_spend_usd,
                          const cai_agent_goal_snapshot *goal) {
  char safe_model[128];
  char safe_effort[32];
  char safe_objective[120];
  char safe_status[32];
  size_t n;
  cai_cli_status_copy(safe_model, sizeof(safe_model), model);
  cai_cli_status_copy(safe_effort, sizeof(safe_effort), effort);
  snprintf(status->model_effort, sizeof(status->model_effort), "%s %s",
           safe_model, safe_effort);
  if (has_context) {
    snprintf(status->context, sizeof(status->context), "ctx %.0f%%",
             context_percent < 0.0     ? 0.0
             : context_percent > 100.0 ? 100.0
                                       : context_percent);
  } else {
    snprintf(status->context, sizeof(status->context), "ctx --");
  }
  status->count = 0U;
  status->elements[status->count++] = status->model_effort;
  status->elements[status->count++] = status->context;
  status->elements[status->count++] = status->directory;
  status->usage[0] = '\0';
  if (quota == NULL) {
    if (estimated_spend_usd > 0.0) {
      snprintf(status->usage, sizeof(status->usage), "cost ~$%.4f",
               estimated_spend_usd);
    } else {
      snprintf(status->usage, sizeof(status->usage), "cost ?$");
    }
  }
  if (quota != NULL && quota->has_weekly &&
      quota->weekly_remaining_percent >= 0.0 &&
      quota->weekly_remaining_percent <= 100.0) {
    snprintf(status->usage, sizeof(status->usage), "w %.0f%%",
             quota->weekly_remaining_percent);
  }
  if (quota != NULL && quota->has_short_window &&
      quota->short_window_remaining_percent >= 0.0 &&
      quota->short_window_remaining_percent <= 100.0) {
    n = strlen(status->usage);
    if (quota->short_window_seconds % 3600LL == 0LL) {
      snprintf(status->usage + n, sizeof(status->usage) - n, "%s%lldh %.0f%%",
               n > 0U ? " " : "", quota->short_window_seconds / 3600LL,
               quota->short_window_remaining_percent);
    } else if (quota->short_window_seconds % 60LL == 0LL) {
      snprintf(status->usage + n, sizeof(status->usage) - n, "%s%lldm %.0f%%",
               n > 0U ? " " : "", quota->short_window_seconds / 60LL,
               quota->short_window_remaining_percent);
    } else {
      snprintf(status->usage + n, sizeof(status->usage) - n, "%s%llds %.0f%%",
               n > 0U ? " " : "", quota->short_window_seconds,
               quota->short_window_remaining_percent);
    }
  }
  if (status->usage[0] != '\0') {
    status->elements[status->count++] = status->usage;
  }
  if (status->branch[0] != '\0') {
    status->elements[status->count++] = status->branch;
  }
  if (goal != NULL && goal->has_goal && goal->status != NULL &&
      strcmp(goal->status, "complete") != 0) {
    cai_cli_status_copy(safe_objective, sizeof(safe_objective),
                        goal->objective);
    cai_cli_status_copy(safe_status, sizeof(safe_status), goal->status);
    snprintf(status->goal, sizeof(status->goal), "goal %s: %s", safe_status,
             safe_objective);
    status->elements[status->count++] = status->goal;
  }
}

static int cai_cli_table_row(char *out, size_t capacity, size_t *used,
                             const char *label, const char *value) {
  int written =
      snprintf(out + *used, capacity - *used, "| %s | %s |\n", label, value);
  if (written < 0 || (size_t)written >= capacity - *used) {
    return -1;
  }
  *used += (size_t)written;
  return 0;
}

int cai_cli_status_markdown(char *out, size_t capacity, const char *model,
                            const char *effort, const char *provider,
                            const cai_agent_runtime_metrics *metrics,
                            const cai_chatgpt_quota *quota) {
  char safe_model[128];
  char safe_effort[32];
  char safe_provider[32];
  char value[160];
  size_t used;
  int written;
  size_t i;
  if (out == NULL || capacity == 0U || model == NULL || effort == NULL ||
      provider == NULL) {
    return -1;
  }
  written = snprintf(out, capacity,
                     "## Status\n\n| Metric | Value |\n| --- | --- |\n");
  if (written < 0 || (size_t)written >= capacity) {
    return -1;
  }
  used = (size_t)written;
  cai_cli_status_copy(safe_model, sizeof(safe_model), model);
  cai_cli_status_copy(safe_effort, sizeof(safe_effort), effort);
  cai_cli_status_copy(safe_provider, sizeof(safe_provider), provider);
  for (i = 0U; safe_model[i] != '\0'; i++) {
    if (safe_model[i] == '|')
      safe_model[i] = ' ';
  }
  for (i = 0U; safe_effort[i] != '\0'; i++) {
    if (safe_effort[i] == '|')
      safe_effort[i] = ' ';
  }
  if (cai_cli_table_row(out, capacity, &used, "Provider", safe_provider) != 0 ||
      cai_cli_table_row(out, capacity, &used, "Model", safe_model) != 0 ||
      cai_cli_table_row(out, capacity, &used, "Reasoning effort",
                        safe_effort) != 0) {
    return -1;
  }
  if (metrics != NULL && metrics->context_window_tokens > 0LL) {
    snprintf(value, sizeof(value), "%lld tokens",
             metrics->context_window_tokens);
    if (cai_cli_table_row(out, capacity, &used, "Context window", value) != 0)
      return -1;
    if (metrics->has_context_usage) {
      snprintf(value, sizeof(value), "%lld tokens (%.0f%%)",
               metrics->context_used_tokens,
               (double)metrics->context_used_tokens * 100.0 /
                   (double)metrics->context_window_tokens);
      if (cai_cli_table_row(out, capacity, &used, "Context used", value) != 0)
        return -1;
    }
  }
  if (metrics != NULL && metrics->session_usage.estimated_spend_usd > 0.0) {
    snprintf(value, sizeof(value), "$%.4f USD%s",
             metrics->session_usage.estimated_spend_usd,
             quota != NULL ? " (API equivalent)" : "");
    if (cai_cli_table_row(out, capacity, &used, "Cost estimate", value) != 0)
      return -1;
  }
  if (quota != NULL && quota->has_weekly) {
    snprintf(value, sizeof(value), "%.0f%% remaining",
             quota->weekly_remaining_percent);
    if (cai_cli_table_row(out, capacity, &used, "Weekly limit", value) != 0)
      return -1;
  }
  if (quota != NULL && quota->has_short_window) {
    if (quota->short_window_seconds % 3600LL == 0LL) {
      snprintf(value, sizeof(value), "%lld-hour limit",
               quota->short_window_seconds / 3600LL);
    } else if (quota->short_window_seconds % 60LL == 0LL) {
      snprintf(value, sizeof(value), "%lld-minute limit",
               quota->short_window_seconds / 60LL);
    } else {
      snprintf(value, sizeof(value), "%lld-second limit",
               quota->short_window_seconds);
    }
    {
      char remaining[64];
      snprintf(remaining, sizeof(remaining), "%.0f%% remaining",
               quota->short_window_remaining_percent);
      if (cai_cli_table_row(out, capacity, &used, value, remaining) != 0)
        return -1;
    }
  }
  if (quota != NULL && quota->credits_unlimited) {
    if (cai_cli_table_row(out, capacity, &used, "Credits left", "Unlimited") !=
        0)
      return -1;
  } else if (quota != NULL && quota->has_credit_balance) {
    snprintf(value, sizeof(value), "%.2f", quota->credit_balance);
    if (cai_cli_table_row(out, capacity, &used, "Credits left", value) != 0)
      return -1;
  }
  return 0;
}

int cai_cli_turn_status_message(char *out, size_t capacity,
                                const char *reasoning_summary,
                                const cai_agent_runtime_metrics *metrics) {
  unsigned long long seconds;
  unsigned long long minutes;
  unsigned long long hours;
  unsigned long long days;
  char duration[64];
  char summary[512];
  struct tm local_time;
  time_t finished;
  int written;
  if (out == NULL || capacity == 0U || metrics == NULL)
    return -1;
  out[0] = '\0';
  if (!metrics->turn_active && !metrics->has_last_turn)
    return 0;
  seconds = (metrics->turn_active ? metrics->turn_elapsed_ms
                                  : metrics->last_turn_duration_ms) /
            1000ULL;
  days = seconds / 86400ULL;
  hours = (seconds / 3600ULL) % 24ULL;
  minutes = (seconds / 60ULL) % 60ULL;
  if (days > 0ULL) {
    written = snprintf(duration, sizeof(duration), "%llud %lluh %llum", days,
                       hours, minutes);
  } else if (seconds >= 3600ULL) {
    written =
        snprintf(duration, sizeof(duration), "%lluh %llum", hours, minutes);
  } else {
    written = snprintf(duration, sizeof(duration), "%llum %llus", minutes,
                       seconds % 60ULL);
  }
  if (written < 0 || (size_t)written >= sizeof(duration))
    return -1;
  if (metrics->turn_active) {
    size_t read_at;
    size_t write_at = 0U;
    cai_cli_status_copy(summary, sizeof(summary), reasoning_summary);
    for (read_at = 0U; summary[read_at] != '\0'; read_at++) {
      if (summary[read_at] == ' ' &&
          (write_at == 0U || summary[write_at - 1U] == ' '))
        continue;
      summary[write_at++] = summary[read_at];
    }
    if (write_at > 0U && summary[write_at - 1U] == ' ')
      write_at--;
    summary[write_at] = '\0';
    written = snprintf(out, capacity, "%s (%s)",
                       summary[0] != '\0' ? summary : "Working", duration);
  } else {
    finished = (time_t)metrics->last_turn_finished_unix_seconds;
    if (localtime_r(&finished, &local_time) == NULL)
      return -1;
    written =
        snprintf(out, capacity, "Worked for %s - %02d%02d%02d", duration,
                 local_time.tm_mday, local_time.tm_hour, local_time.tm_min);
  }
  return written < 0 || (size_t)written >= capacity ? -1 : 0;
}

int cai_cli_status_apply(sl_t *sl, const cai_cli_status *status) {
  return sl_set_status_elements(sl, status->elements, status->count) == SL_OK
             ? 0
             : -1;
}
