#include "app_internal.h"
#include <stdio.h>
#include <string.h>

static const cai_app_option options[] = {
    {"--service-tier", '\0', CAI_APP_STRING, "NAME",
     "Provider routing tier (see /tier list)", 0, 0},
    {"--new", 'N', CAI_APP_FLAG, NULL, "Start a new session", 0, 0},
    {"--non-interactive", 'n', CAI_APP_FLAG, NULL, "Run supplied work and exit",
     0, 0},
    {"--resume", '\0', CAI_APP_OPTIONAL_STRING, "ID",
     "List directory sessions or resume ID", 0, 0},
    {"--list", 'l', CAI_APP_FLAG, NULL, "List conversations in all directories",
     0, 0},
    {"--export", '\0', CAI_APP_STRING, "ID",
     "Export resumable JSONL and Markdown", 0, 0},
    {"--import", '\0', CAI_APP_STRING, "FILE",
     "Import JSONL as a new session and exit", 0, 0},
    {"--export-dir", '\0', CAI_APP_STRING, "DIR",
     "Export root (default $XDG_DATA_HOME/cai/exports)", 0, 0},
    {"--directory", 'C', CAI_APP_STRING, "DIR", "Change to DIR before starting",
     0, 0},
    {"--config", '\0', CAI_APP_STRING, "FILE", "Select config.ini", 0, 0},
    {"--no-config", '\0', CAI_APP_FLAG, NULL, "Skip configuration and creation",
     0, 0},
    {"--instruction", 'i', CAI_APP_STRING, "TEXT",
     "Submit a prompt (repeatable)", 1, 0},
    {"--goal", 'g', CAI_APP_STRING, "TEXT", "Start or replace a durable goal",
     0, 0},
    {"--review", '\0', CAI_APP_FLAG, "[uncommitted]",
     "Review Git default branch, or uncommitted changes; exit", 0, 0},
    {"--review-and-fix", '\0', CAI_APP_FLAG, "[uncommitted]",
     "Review and fix against Git default branch, or uncommitted changes", 0, 0},
    {"--base", '\0', CAI_APP_STRING, "REF", "Review changes against REF", 0, 0},
    {"--out", 'o', CAI_APP_STRING, "FILE", "Write review findings to FILE", 0,
     0},
    {"--output-type", 'T', CAI_APP_STRING, "TYPE",
     "Review findings: markdown|json", 0, 0},
    {"--tool-output", '\0', CAI_APP_STRING, "MODE",
     "Tool previews: compact|full (interactive default compact)", 0, 0},
    {"--auth-json", 'a', CAI_APP_STRING, "PATH",
     "ChatGPT auth file (default ~/.codex/auth.json, "
     "then encrypted cai store)",
     0, 0},
    {"--login", '\0', CAI_APP_FLAG, NULL, "Log in to ChatGPT and exit", 0, 0},
    {"--lockd", '\0', CAI_APP_STRING, "ENDPOINT",
     "Storage endpoint (default state/cai/pouch)", 0, 0},
    {"--lockd-client-pem", '\0', CAI_APP_STRING, "FILE",
     "Remote lockd mTLS PEM bundle", 0, 0},
    {"--provider", 'p', CAI_APP_STRING, "NAME",
     "chatgpt|openai|openrouter|berget|custom", 0, 0},
    {"--endpoint", '\0', CAI_APP_STRING, "URL", "Custom provider API base URL",
     0, 0},
    {"--api-key-env", '\0', CAI_APP_STRING, "NAME",
     "Custom API key variable (default CAI_API_KEY)", 0, 0},
    {"--chrome", '\0', CAI_APP_FLAG, NULL,
     "Chrome extension tools (default on)", 0, 1},
    {"--mcp", '\0', CAI_APP_FLAG, NULL,
     "Configured HTTP MCP servers (default on)", 0, 1},
    {"--messaging", '\0', CAI_APP_FLAG, NULL,
     "Shared Pouch conversation messaging (default off)", 0, 1},
    {"--queue", 'q', CAI_APP_STRING, "NAME", "Destination for --message", 0, 0},
    {"--message", 'M', CAI_APP_STRING, "TEXT",
     "Send standalone message and exit", 0, 0},
    {"--message-queue", '\0', CAI_APP_STRING, "NAME",
     "Full mailbox name (default basename + short path hash)", 0, 0},
    {"--autoqueue", '\0', CAI_APP_FLAG, NULL,
     "TUI auto-approval of incoming prompts (default off)", 0, 1},
    {"--message-ack-timeout-ms", '\0', CAI_APP_INTEGER, "MS",
     "Delivery ACK deadline (default 300000)", 0, 0},
    {"--web-search", '\0', CAI_APP_FLAG, NULL,
     "Hosted Responses search (default on)", 0, 1},
    {"--todo", '\0', CAI_APP_FLAG, NULL, "Encrypted kanban tool (default off)",
     0, 1},
    {"--board", '\0', CAI_APP_STRING, "NAME",
     "Kanban board (default: default; requires --todo)", 0, 0},
    {"--searxng", '\0', CAI_APP_FLAG, NULL, "Local search tool (default off)",
     0, 1},
    {"--searxng-url", '\0', CAI_APP_STRING, "URL",
     "Search base (default http://127.0.0.1:8888)", 0, 0},
    {"--revgeo", '\0', CAI_APP_FLAG, NULL, "Reverse geocoding (default off)", 0,
     1},
    {"--revgeo-url", '\0', CAI_APP_STRING, "URL",
     "Reverse geocoding provider base URL", 0, 0},
    {"--read-file", '\0', CAI_APP_FLAG, NULL, "File reading (default on)", 0,
     1},
    {"--list-files", '\0', CAI_APP_FLAG, NULL, "File listing (default on)", 0,
     1},
    {"--apply-patch", '\0', CAI_APP_FLAG, NULL,
     "Workspace patching (default on)", 0, 1},
    {"--view-image", '\0', CAI_APP_FLAG, NULL, "Image inspection (default on)",
     0, 1},
    {"--goals", '\0', CAI_APP_FLAG, NULL, "Goal tools (default on)", 0, 1},
    {"--skills", '\0', CAI_APP_FLAG, NULL,
     "Skill discovery and reading (default on)", 0, 1},
    {"--chrome-socket", '\0', CAI_APP_STRING, "PATH",
     "Select a connected CAI Chrome host", 0, 0},
    {"--chrome-setup", '\0', CAI_APP_FLAG, NULL,
     "Register CAI as Chrome native host and exit", 0, 0},
    {"--chrome-remove", '\0', CAI_APP_FLAG, NULL,
     "Remove CAI's own registration and exit", 0, 0},
    {"--chrome-manifest", '\0', CAI_APP_STRING, "FILE",
     "Alternate native-host registration path", 0, 0},
    {"--shell-context", '\0', CAI_APP_FLAG, NULL,
     "Share !COMMAND results (not /shell, !sh)", 0, 1},
    {"--shell-context-max-lines", '\0', CAI_APP_INTEGER, "N",
     "Retain last N lines (default 5000)", 0, 0},
    {"--shell-context-max-bytes", '\0', CAI_APP_INTEGER, "N",
     "Bound retained text (default 4194304)", 0, 0},
    {"--model-metadata", '\0', CAI_APP_STRING, "FILE",
     "Provider model metadata JSON", 0, 0},
    {"--model", 'm', CAI_APP_STRING, "ID",
     "Model (provider default if omitted)", 0, 0},
    {"--reasoning-effort", 'r', CAI_APP_STRING, "LEVEL",
     "none|minimal|low|medium|high|xhigh|max|ultra", 0, 0},
    {"--reasoning-summary", '\0', CAI_APP_STRING, "MODE",
     "none|auto|concise|detailed (default concise)", 0, 0},
    {"--review-model", '\0', CAI_APP_STRING, "ID", "Model for review children",
     0, 0},
    {"--review-reasoning-effort", '\0', CAI_APP_STRING, "LEVEL",
     "Reasoning effort for review children", 0, 0},
    {"--review-reasoning-summary", '\0', CAI_APP_STRING, "MODE",
     "Reasoning summaries for review children", 0, 0},
    {"--agents-md", '\0', CAI_APP_STRING, "PATH", "Global AGENTS.md path", 0,
     0},
    {"--config-dir", '\0', CAI_APP_STRING, "DIR",
     "Global agent config directory", 0, 0},
    {"--skills-dir", '\0', CAI_APP_STRING, "DIR", "Global skills directory", 0,
     0},
    {"--identity", '\0', CAI_APP_STRING, "TEXT", "Visible agent identity", 0,
     0},
    {"--developer-instructions", 'I', CAI_APP_STRING, "TEXT",
     "Append developer instructions", 0, 0},
    {"--install-skill", '\0', CAI_APP_STRING, "DIR",
     "Install a SKILL.md package and exit", 0, 0},
    {"--replace", '\0', CAI_APP_FLAG, NULL,
     "Explicitly replace an installed skill", 0, 0},
    {"--codex-agents-md", '\0', CAI_APP_FLAG, NULL,
     "Load Codex global and ancestor AGENTS.md", 0, 1},
    {"--image-generation", '\0', CAI_APP_FLAG, NULL,
     "Hosted image generation (default on)", 0, 1},
    {"--terminal", '\0', CAI_APP_FLAG, NULL, "Terminal tools (default on)", 0,
     1},
    {"--review-subagent", '\0', CAI_APP_FLAG, NULL,
     "Built-in reviewer (default on)", 0, 1},
    {"--verbose", 'v', CAI_APP_COUNTER, NULL,
     "Print runtime events (repeat for sequences)", 1, 0},
    {"--help", 'h', CAI_APP_FLAG, NULL, "Show this help", 0, 0},
    {"--version", '\0', CAI_APP_FLAG, NULL, "Show version", 0, 0}};
size_t cai_cli_help_count(void) { return sizeof(options) / sizeof(options[0]); }
const cai_app_option *cai_cli_help_at(size_t index) {
  return index < cai_cli_help_count() ? &options[index] : NULL;
}
static int text(cai_sink *sink, const char *value, cai_error *error) {
  return cai_sink_write(sink, value, strlen(value), error);
}
int cai_cli_help_write(const char *program, cai_sink *sink, cai_error *error) {
  size_t i;
  int rc;
  char flags[128], row[640];
  rc = text(sink,
            "cai and libcai Copyright (C) 2026 C89 Systems AB "
            "https://c89.systems\n\nUsage: ",
            error);
  if (rc == CAI_OK)
    rc = text(sink, program, error);
  if (rc == CAI_OK)
    rc = text(sink,
              " [options]\n\nStart Cai Smith in the workspace directory. The "
              "newest session resumes\nautomatically; use --new to start a "
              "fresh one. In the prompt, /resume\nlists sessions for this "
              "directory and /resume N opens entry N.\n\n",
              error);
  for (i = 0; rc == CAI_OK && i < cai_cli_help_count(); i++) {
    const cai_app_option *o = &options[i];
    if (o->short_flag)
      snprintf(flags, sizeof(flags), "-%c, %s%s%s%s%s", o->short_flag,
               o->long_flag, o->argument ? " " : "",
               o->type == CAI_APP_OPTIONAL_STRING ? "[" : "",
               o->argument ? o->argument : "",
               o->type == CAI_APP_OPTIONAL_STRING ? "]" : "");
    else
      snprintf(flags, sizeof(flags), "    %s%s%s%s%s%s",
               o->negatable ? "--[no-]" : "",
               o->negatable ? o->long_flag + 2 : o->long_flag,
               o->argument ? " " : "",
               o->type == CAI_APP_OPTIONAL_STRING ? "[" : "",
               o->argument ? o->argument : "",
               o->type == CAI_APP_OPTIONAL_STRING ? "]" : "");
    snprintf(row, sizeof(row), "  %-36s %s\n", flags, o->description);
    rc = text(sink, row, error);
  }
  return rc;
}
int cai_cli_help_print(const char *program) {
  cai_sink *sink = NULL;
  int rc = cai_sink_stdout(&sink, NULL);
  if (rc == CAI_OK)
    rc = cai_cli_help_write(program, sink, NULL);
  cai_sink_close(sink);
  return rc;
}
