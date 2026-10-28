#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "status.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

enum {
  FIELD_MODEL,
  FIELD_CONTEXT,
  FIELD_DIRECTORY,
  FIELD_WEEKLY,
  FIELD_CREDITS,
  FIELD_YOLO,
  FIELD_GOAL,
  FIELD_BRANCH,
  FIELD_PROVIDER
};

const char *cai_cli_status_fields_parse(const char *text,
                                        cai_cli_status_fields *out) {
  static const struct {
    const char *name;
    unsigned int id;
  } names[] = {{"model", FIELD_MODEL},
               {"ctx", FIELD_CONTEXT},
               {"context", FIELD_CONTEXT},
               {"dir", FIELD_DIRECTORY},
               {"directory", FIELD_DIRECTORY},
               {"folder", FIELD_DIRECTORY},
               {"workspace", FIELD_DIRECTORY},
               {"weekly", FIELD_WEEKLY},
               {"w", FIELD_WEEKLY},
               {"crd", FIELD_CREDITS},
               {"credits", FIELD_CREDITS},
               {"yolo", FIELD_YOLO},
               {"goal", FIELD_GOAL},
               {"branch", FIELD_BRANCH},
               {"provider", FIELD_PROVIDER}};
  cai_cli_status_fields selected, defaults;
  const char *end;
  size_t length, i, j;
  unsigned int id;
  memset(&selected, 0, sizeof(selected));
  if (text == NULL || text[0] == '\0' || out == NULL)
    return "status-fields requires a nonempty colon-separated field list";
  for (;;) {
    end = strchr(text, ':');
    length = end != NULL ? (size_t)(end - text) : strlen(text);
    if (length == 7U && strncmp(text, "default", length) == 0) {
      (void)cai_cli_status_fields_parse(CAI_CLI_STATUS_DEFAULT_FIELDS,
                                        &defaults);
      for (i = 0U; i < defaults.count; i++) {
        id = defaults.ids[i];
        for (j = 0U; j < selected.count && selected.ids[j] != id; j++) {
        }
        if (j == selected.count)
          selected.ids[selected.count++] = id;
      }
    } else {
      for (i = 0U; i < sizeof(names) / sizeof(names[0]); i++)
        if (strlen(names[i].name) == length &&
            strncmp(text, names[i].name, length) == 0)
          break;
      if (i == sizeof(names) / sizeof(names[0]))
        return "unknown or empty status field; use model, ctx, dir, weekly, "
               "crd, yolo, goal, branch, provider or default";
      id = names[i].id;
      for (j = 0U; j < selected.count && selected.ids[j] != id; j++) {
      }
      if (j == selected.count)
        selected.ids[selected.count++] = id;
    }
    if (end == NULL)
      break;
    text = end + 1U;
  }
  *out = selected;
  return NULL;
}

static void status_copy(char *out, size_t capacity, const char *text,
                        int branch) {
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
    if (branch && ((codepoint >= 0x80U && codepoint <= 0x9fU) ||
                   codepoint == 0xadU || codepoint == 0x61cU ||
                   (codepoint >= 0x200bU && codepoint <= 0x200fU) ||
                   (codepoint >= 0x2028U && codepoint <= 0x202eU) ||
                   (codepoint >= 0x2060U && codepoint <= 0x206fU) ||
                   codepoint == 0xfeffU)) {
      if (written + 6U >= capacity)
        break;
      snprintf(out + written, capacity - written, "\\u%04x", codepoint);
      written += 6U;
      i += sequence;
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

static void cai_cli_status_copy(char *out, size_t capacity, const char *text) {
  status_copy(out, capacity, text, 0);
}

void cai_cli_status_init(cai_cli_status *status, const char *workspace,
                         const char *home) {
  const char *display;
  const char *base;
  const char *parent;
  size_t home_length;
  memset(status, 0, sizeof(*status));
  (void)cai_cli_status_fields_parse(CAI_CLI_STATUS_DEFAULT_FIELDS,
                                    &status->fields);
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

static int status_git_path(char *out, const char *directory, const char *name) {
  int count = snprintf(out, PATH_MAX, "%s%s%s", directory,
                       strcmp(directory, "/") == 0 ? "" : "/", name);
  return count >= 0 && count < PATH_MAX;
}

/* Only small regular metadata files are read. Nonblocking open also avoids
 * hanging the UI if a broken repository replaces HEAD/gitfile with a FIFO. */
static int status_git_line(const char *path, char *out, size_t capacity) {
  struct stat info;
  size_t used = 0U;
  ssize_t count = 0;
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0)
    return 0;
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
    close(fd);
    return 0;
  }
  while (used + 1U < capacity) {
    count = read(fd, out + used, capacity - used - 1U);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      break;
    used += (size_t)count;
  }
  close(fd);
  if (count < 0 || used + 1U == capacity || memchr(out, '\0', used) != NULL)
    return 0;
  while (used != 0U && (out[used - 1U] == '\n' || out[used - 1U] == '\r'))
    used--;
  out[used] = '\0';
  return used != 0U && strpbrk(out, "\r\n") == NULL;
}

static void status_git_head(cai_cli_status *status, const char *directory) {
  static const char prefix[] = "ref: refs/heads/";
  char path[PATH_MAX], line[PATH_MAX];
  const unsigned char *p;
  if (!status_git_path(path, directory, "HEAD") ||
      !status_git_line(path, line, sizeof(line)) ||
      strncmp(line, prefix, sizeof(prefix) - 1U) != 0 ||
      line[sizeof(prefix) - 1U] == '\0')
    return;
  for (p = (const unsigned char *)line + sizeof(prefix) - 1U; *p; p++)
    if (*p <= 32U || *p == 127U || strchr("~^:?*[\\", *p) != NULL)
      return;
  status_copy(status->branch, sizeof(status->branch),
              line + sizeof(prefix) - 1U, 1);
}

static int status_git_ceiling(const char *directory) {
  const char *entry = getenv("GIT_CEILING_DIRECTORIES"), *end;
  size_t length = strlen(directory);
  while (entry != NULL && *entry != '\0') {
    end = strchr(entry, ':');
    if ((end != NULL ? (size_t)(end - entry) : strlen(entry)) == length &&
        strncmp(directory, entry, length) == 0)
      return 1;
    entry = end != NULL ? end + 1U : NULL;
  }
  return 0;
}

static void status_git_discover(cai_cli_status *status, const char *workspace) {
  char directory[PATH_MAX], marker[PATH_MAX], line[PATH_MAX];
  char *parent;
  const char *explicit_directory = getenv("GIT_DIR");
  struct stat info;
  status->branch[0] = '\0';
  if (workspace == NULL || realpath(workspace, directory) == NULL)
    return;
  if (explicit_directory != NULL && explicit_directory[0] != '\0') {
    if (explicit_directory[0] == '/')
      status_git_head(status, explicit_directory);
    else if (status_git_path(marker, directory, explicit_directory))
      status_git_head(status, marker);
    return;
  }
  for (;;) {
    if (!status_git_path(marker, directory, ".git"))
      return;
    if (lstat(marker, &info) == 0) {
      if (stat(marker, &info) != 0)
        return;
      if (S_ISDIR(info.st_mode))
        status_git_head(status, marker);
      else if (S_ISREG(info.st_mode) &&
               status_git_line(marker, line, sizeof(line)) &&
               strncmp(line, "gitdir: ", 8U) == 0) {
        if (line[8] == '/')
          status_git_head(status, line + 8U);
        else if (line[8] != '\0' &&
                 status_git_path(marker, directory, line + 8U))
          status_git_head(status, marker);
      }
      return;
    }
    if (errno != ENOENT && errno != ENOTDIR)
      return;
    /* Bare repositories have HEAD, objects and refs directly in this folder. */
    if (status_git_path(marker, directory, "objects") &&
        stat(marker, &info) == 0 && S_ISDIR(info.st_mode) &&
        status_git_path(marker, directory, "refs") &&
        stat(marker, &info) == 0 && S_ISDIR(info.st_mode) &&
        status_git_path(marker, directory, "HEAD") &&
        stat(marker, &info) == 0 && S_ISREG(info.st_mode)) {
      status_git_head(status, directory);
      return;
    }
    if (strcmp(directory, "/") == 0)
      return;
    parent = strrchr(directory, '/');
    if (parent == directory)
      directory[1] = '\0';
    else
      *parent = '\0';
    if (status_git_ceiling(directory))
      return;
  }
}

int cai_cli_status_refresh_branch_at(cai_cli_status *status,
                                     const char *workspace,
                                     long long monotonic_ms, int force) {
  if (!force && status->branch_checked &&
      (monotonic_ms < status->branch_checked_ms ||
       monotonic_ms - status->branch_checked_ms < 60000LL))
    return 0;
  status_git_discover(status, workspace);
  status->branch_checked_ms = monotonic_ms;
  status->branch_checked = 1;
  return 1;
}

static void status_branch_refresh(cai_cli_status *status, const char *workspace,
                                  int force) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
    (void)cai_cli_status_refresh_branch_at(
        status, workspace,
        (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000L, force);
  else if (force)
    status_git_discover(status, workspace);
}

void cai_cli_status_refresh_branch(cai_cli_status *status,
                                   const char *workspace) {
  status_branch_refresh(status, workspace, 1);
}

void cai_cli_status_maybe_refresh_branch(cai_cli_status *status,
                                         const char *workspace) {
  status_branch_refresh(status, workspace, 0);
}

void cai_cli_status_build(cai_cli_status *status, const char *model,
                          const char *effort, const char *tier,
                          double context_percent, int has_context,
                          const cai_chatgpt_quota *quota,
                          double estimated_spend_usd,
                          double estimated_spend_eur, const char *provider,
                          const cai_agent_goal_snapshot *goal, int yolo) {
  char safe_model[128];
  char safe_effort[32];
  char safe_tier[32];
  char safe_status[32];
  size_t n;
  size_t i;
  const char *values[CAI_CLI_STATUS_FIELD_COUNT];
  cai_cli_status_copy(safe_model, sizeof(safe_model), model);
  cai_cli_status_copy(safe_effort, sizeof(safe_effort), effort);
  cai_cli_status_copy(safe_tier, sizeof(safe_tier),
                      tier == NULL || strcmp(tier, "default") == 0 ? ""
                      : strcmp(tier, "priority") == 0              ? "fast"
                                                                   : tier);
  snprintf(status->model_effort, sizeof(status->model_effort), "%s %s%s%s",
           safe_model, safe_effort, safe_tier[0] != '\0' ? " " : "", safe_tier);
  if (has_context) {
    snprintf(status->context, sizeof(status->context), "ctx %.0f%%",
             context_percent < 0.0     ? 0.0
             : context_percent > 100.0 ? 100.0
                                       : context_percent);
  } else {
    snprintf(status->context, sizeof(status->context), "ctx --");
  }
  status->count = 0U;
  status->usage[0] = '\0';
  if (quota == NULL) {
    if (provider != NULL && strcmp(provider, "berget") == 0) {
      if (estimated_spend_eur > 0.0) {
        snprintf(status->usage, sizeof(status->usage), "cost ~€%.4f",
                 estimated_spend_eur);
      } else {
        snprintf(status->usage, sizeof(status->usage), "cost ?€");
      }
    } else if (estimated_spend_usd > 0.0) {
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
  status->credits[0] = '\0';
  if (quota != NULL && quota->credits_unlimited) {
    snprintf(status->credits, sizeof(status->credits), "crd unlimited");
  } else if (quota != NULL && quota->has_credit_balance) {
    snprintf(status->credits, sizeof(status->credits), "crd %.0f",
             quota->credit_balance);
  }
  status->goal[0] = '\0';
  if (goal != NULL && goal->has_goal && goal->status != NULL) {
    long long minutes =
        goal->elapsed_seconds > 0LL ? goal->elapsed_seconds / 60LL : 0LL;
    char duration[80];
    if (minutes >= 1440LL)
      snprintf(duration, sizeof(duration), "%lldd%lldh%lldm", minutes / 1440LL,
               (minutes / 60LL) % 24LL, minutes % 60LL);
    else if (minutes >= 60LL)
      snprintf(duration, sizeof(duration), "%lldh%lldm", minutes / 60LL,
               minutes % 60LL);
    else
      snprintf(duration, sizeof(duration), "%lldm", minutes);
    cai_cli_status_copy(safe_status, sizeof(safe_status),
                        strcmp(goal->status, "complete") == 0 ? "achieved"
                                                              : goal->status);
    snprintf(status->goal, sizeof(status->goal), "goal=%s %s", safe_status,
             duration);
  }
  cai_cli_status_copy(status->provider, sizeof(status->provider), provider);
  values[FIELD_MODEL] = status->model_effort;
  values[FIELD_CONTEXT] = status->context;
  values[FIELD_DIRECTORY] = status->directory;
  values[FIELD_WEEKLY] = status->usage;
  values[FIELD_CREDITS] = status->credits;
  values[FIELD_YOLO] = yolo ? "YOLO" : "";
  values[FIELD_GOAL] = status->goal;
  values[FIELD_BRANCH] = status->branch;
  values[FIELD_PROVIDER] = status->provider;
  for (i = 0U; i < status->fields.count; i++) {
    const char *value = values[status->fields.ids[i]];
    if (value[0] != '\0')
      status->elements[status->count++] = value;
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

int cai_cli_status_ids_markdown(const cai_agent_runtime_ids *ids,
                                int (*write)(void *, const char *, size_t),
                                void *context) {
  static const char *const labels[] = {"| Session ID | ",
                                       "| Last response ID | ",
                                       "| Provider conversation ID | "};
  const char *values[3];
  const unsigned char *p;
  char buffer[256];
  size_t i, used;
  if (ids == NULL || write == NULL)
    return -1;
  values[0] = ids->session_id;
  values[1] = ids->response_id;
  values[2] = ids->conversation_id;
  for (i = 0U; i < 3U; i++) {
    if (i == 2U && (values[i] == NULL || values[i][0] == '\0'))
      continue;
    if (write(context, labels[i], strlen(labels[i])) != 0)
      return -1;
    p = (const unsigned char *)(values[i] != NULL && values[i][0] != '\0'
                                    ? values[i]
                                    : "Not available");
    used = 0U;
    for (; *p; p++) {
      if (used + 2U > sizeof(buffer)) {
        if (write(context, buffer, used) != 0)
          return -1;
        used = 0U;
      }
      if (strchr("\\|`*_~[]<>#", *p) != NULL)
        buffer[used++] = '\\';
      buffer[used++] = *p < 32U || *p == 127U ? ' ' : (char)*p;
    }
    if ((used != 0U && write(context, buffer, used) != 0) ||
        write(context, " |\n", 3U) != 0)
      return -1;
  }
  return 0;
}

int cai_cli_status_markdown(char *out, size_t capacity,
                            const char *program_name, const char *model,
                            const char *effort, const char *provider,
                            const cai_agent_runtime_metrics *metrics,
                            const cai_chatgpt_quota *quota,
                            const cai_agent_instruction_status *instructions) {
  char instruction_label[96];
  char safe_model[128];
  char safe_effort[32];
  char safe_provider[32];
  char value[160];
  size_t used;
  int written;
  size_t i;
  if (out == NULL || capacity == 0U || model == NULL || effort == NULL ||
      program_name == NULL || provider == NULL) {
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
  if (metrics != NULL && metrics->session_usage.estimated_spend_eur > 0.0) {
    snprintf(value, sizeof(value), "€%.4f EUR",
             metrics->session_usage.estimated_spend_eur);
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
  if (instructions != NULL) {
    snprintf(instruction_label, sizeof(instruction_label),
             "%s global AGENTS.md", program_name);
    if (cai_cli_table_row(out, capacity, &used, instruction_label,
                          instructions->global_agents_md_loaded
                              ? "Yes"
                              : "No (missing or empty)") != 0 ||
        cai_cli_table_row(out, capacity, &used, "Codex global AGENTS.md",
                          !instructions->codex_agents_md_enabled
                              ? "No (disabled)"
                          : instructions->codex_agents_md_loaded
                              ? "Yes"
                              : "No (missing or empty)") != 0)
      return -1;
    snprintf(value, sizeof(value), "%lu file(s)",
             (unsigned long)instructions->repository_agents_md_count);
    if (cai_cli_table_row(out, capacity, &used, "Repository AGENTS.md",
                          value) != 0)
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
