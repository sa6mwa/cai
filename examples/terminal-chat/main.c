#include <cai/cai.h>
#include <cai/tools/exec.h>
#include <cai/tools/read.h>
#include <cai/tools/searxng.h>
#include <cai/tools/todo.h>

#include "../common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CAI_ANSI_RESET "\033[0m"
#define CAI_ANSI_GRAY "\033[90m"
#define CAI_ANSI_GREEN "\033[32m"
#define CAI_ANSI_BRIGHT_CYAN "\033[96m"
#define CAI_ANSI_MAGENTA "\033[35m"
#define CAI_ANSI_BOLD_WHITE "\033[1;37m"

#define CAI_USAGE_LABEL                                                        \
  CAI_ANSI_GRAY "[" CAI_ANSI_BRIGHT_CYAN "usage" CAI_ANSI_GRAY                 \
                "]" CAI_ANSI_RESET
#define CAI_RESPONSE_PREFIX                                                    \
  CAI_ANSI_GRAY "[" CAI_ANSI_GREEN "response" CAI_ANSI_GRAY "]"                \
                CAI_ANSI_RESET " "
#define CAI_RESPONSE_SUFFIX CAI_ANSI_RESET "\n"
#define CAI_TOOL_LABEL                                                         \
  CAI_ANSI_GRAY "[" CAI_ANSI_BRIGHT_CYAN "tool" CAI_ANSI_GRAY "]"              \
                CAI_ANSI_RESET
#define CAI_REASONING_PREFIX                                                   \
  CAI_ANSI_GRAY "[" CAI_ANSI_MAGENTA "reasoning" CAI_ANSI_GRAY "] "            \
                CAI_ANSI_GRAY
#define CAI_REASONING_SUFFIX CAI_ANSI_RESET "\n\n"

static int print_error(const char *operation, int rc, const cai_error *error) {
  fprintf(stderr, "%s failed: %s\n", operation,
          error->message != NULL ? error->message : cai_status_string(rc));
  if (error->detail != NULL) {
    fprintf(stderr, "detail: %s\n", error->detail);
  }
  return 1;
}

static void trim_newline(char *line) {
  size_t length;

  length = strlen(line);
  while (length > 0U &&
         (line[length - 1U] == '\n' || line[length - 1U] == '\r')) {
    line[length - 1U] = '\0';
    length--;
  }
}

static void print_usage(const cai_token_usage *usage, double context_percent,
                        int has_context_percent, double total_spent_usd) {
  if (has_context_percent) {
    fprintf(stderr,
            CAI_USAGE_LABEL
            " input=" CAI_ANSI_BOLD_WHITE "%lld" CAI_ANSI_RESET
            " cached=" CAI_ANSI_BOLD_WHITE "%lld" CAI_ANSI_RESET
            " output=" CAI_ANSI_BOLD_WHITE "%lld" CAI_ANSI_RESET
            " reasoning=" CAI_ANSI_BOLD_WHITE "%lld" CAI_ANSI_RESET
            " total=" CAI_ANSI_BOLD_WHITE "%lld" CAI_ANSI_RESET
            " context=" CAI_ANSI_BOLD_WHITE "%.2f%%" CAI_ANSI_RESET
            " estimated_cost=" CAI_ANSI_BOLD_WHITE "$%.8f" CAI_ANSI_RESET "\n",
            usage->input_tokens, usage->input_cached_tokens,
            usage->output_tokens, usage->output_reasoning_tokens,
            usage->total_tokens, context_percent, total_spent_usd);
    return;
  }
  fprintf(stderr,
          CAI_USAGE_LABEL
          " input=" CAI_ANSI_BOLD_WHITE "%lld" CAI_ANSI_RESET
          " cached=" CAI_ANSI_BOLD_WHITE "%lld" CAI_ANSI_RESET
          " output=" CAI_ANSI_BOLD_WHITE "%lld" CAI_ANSI_RESET
          " reasoning=" CAI_ANSI_BOLD_WHITE "%lld" CAI_ANSI_RESET
          " total=" CAI_ANSI_BOLD_WHITE "%lld" CAI_ANSI_RESET
          " context=" CAI_ANSI_BOLD_WHITE "n/a" CAI_ANSI_RESET
          " estimated_cost=" CAI_ANSI_BOLD_WHITE "$%.8f" CAI_ANSI_RESET "\n",
          usage->input_tokens, usage->input_cached_tokens, usage->output_tokens,
          usage->output_reasoning_tokens, usage->total_tokens, total_spent_usd);
}

static const char *searxng_base_url(void) {
  const char *base_url;

  base_url = getenv("CAI_SEARXNG_BASE_URL");
  if (base_url == NULL || base_url[0] == '\0') {
    return CAI_SEARXNG_DEFAULT_BASE_URL;
  }
  return base_url;
}

static void print_help(const char *program) {
  fprintf(stderr,
          "usage: %s [--exec-tool-dir <path>] [--read-tool-dir <path>]\n\n"
          "  --exec-tool-dir <path>  Register exec_command rooted to <path>.\n"
          "  --read-tool-dir <path>  Register read_file rooted to <path>.\n",
          program != NULL ? program : "cai_example_terminal_chat");
}

static int parse_args(int argc, char **argv, const char **exec_tool_dir,
                      const char **read_tool_dir) {
  int i;

  *exec_tool_dir = NULL;
  *read_tool_dir = NULL;
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--exec-tool-dir") == 0) {
      if (i + 1 >= argc || argv[i + 1][0] == '\0') {
        fprintf(stderr, "--exec-tool-dir requires a path\n");
        return 0;
      }
      *exec_tool_dir = argv[++i];
      continue;
    }
    if (strcmp(argv[i], "--read-tool-dir") == 0) {
      if (i + 1 >= argc || argv[i + 1][0] == '\0') {
        fprintf(stderr, "--read-tool-dir requires a path\n");
        return 0;
      }
      *read_tool_dir = argv[++i];
      continue;
    }
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      print_help(argv[0]);
      return -1;
    }
    fprintf(stderr, "unknown argument: %s\n", argv[i]);
    print_help(argv[0]);
    return 0;
  }
  return 1;
}

typedef struct terminal_tool_trace {
  FILE *fp;
  cai_sink *sink;
} terminal_tool_trace;

static int print_tool_event(void *context, const cai_tool_event *event,
                            cai_error *error) {
  terminal_tool_trace *trace;

  trace = (terminal_tool_trace *)context;
  if (trace == NULL || trace->fp == NULL || event == NULL) {
    return CAI_OK;
  }
  if (event->type == CAI_TOOL_EVENT_START) {
    fprintf(trace->fp, CAI_TOOL_LABEL " %s input=%s\n",
            event->name != NULL ? event->name : "(unknown)",
            event->arguments_json != NULL ? event->arguments_json : "{}");
    fflush(trace->fp);
    return CAI_OK;
  }
  if (event->type == CAI_TOOL_EVENT_OUTPUT) {
    fprintf(trace->fp, CAI_TOOL_LABEL " %s output=",
            event->name != NULL ? event->name : "(unknown)");
    fflush(trace->fp);
    if (trace->sink != NULL) {
      int rc;

      rc = cai_tool_event_write_output(event, trace->sink, error);
      if (rc != CAI_OK) {
        return rc;
      }
    }
    fputc('\n', trace->fp);
    fflush(trace->fp);
    return CAI_OK;
  }
  if (event->type == CAI_TOOL_EVENT_ERROR) {
    fprintf(trace->fp, CAI_TOOL_LABEL " %s failed",
            event->name != NULL ? event->name : "(unknown)");
    if (event->tool_error != NULL && event->tool_error->message != NULL) {
      fprintf(trace->fp, ": %s", event->tool_error->message);
    }
    fputc('\n', trace->fp);
    fflush(trace->fp);
  }
  return CAI_OK;
}

int main(int argc, char **argv) {
  cai_agent_config agent_config;
  cai_client_config client_config;
  cai_run_options run_options;
  cai_exec_tool_config exec_config;
  cai_read_tool_config read_config;
  cai_searxng_tool_config searxng_config;
  cai_todo_tool_config todo_config;
  cai_stream_sinks stream_sinks;
  cai_client *client;
  cai_agent *agent;
  cai_session *session;
  cai_sink *stdout_sink;
  terminal_tool_trace tool_trace;
  cai_error error;
  cai_token_usage usage;
  double context_percent;
  double total_spent_usd;
  char *dotenv_api_key;
  const char *exec_tool_dir;
  const char *read_tool_dir;
  int has_context_percent;
  char line[4096];
  int exit_code;
  int rc;

  cai_error_init(&error);
  cai_client_config_init(&client_config);
  cai_agent_config_init(&agent_config);
  cai_run_options_init(&run_options);
  memset(&exec_config, 0, sizeof(exec_config));
  memset(&read_config, 0, sizeof(read_config));
  memset(&searxng_config, 0, sizeof(searxng_config));
  memset(&todo_config, 0, sizeof(todo_config));
  rc = parse_args(argc, argv, &exec_tool_dir, &read_tool_dir);
  if (rc < 0) {
    return 0;
  }
  if (rc == 0) {
    return 2;
  }
  agent_config.model = CAI_MODEL_GPT_5_NANO;
  agent_config.reasoning_effort = CAI_REASONING_EFFORT_LOW;
  if (exec_tool_dir != NULL || read_tool_dir != NULL) {
    agent_config.developer_instructions =
        "You are a concise terminal chat assistant. Tools: searxng_search for "
        "web search, todo_kanban for a local kanban board, read_file for "
        "sandboxed file reads, and optionally exec_command for sandboxed "
        "commands. Cite search URLs. todo_kanban has a default board; omit "
        "board_id and board_name for ordinary use. Prefer read_file for file "
        "contents. Use exec_command only when explicitly asked; set workdir "
        "when a directory matters and do not assume network.";
  } else {
    agent_config.developer_instructions =
        "You are a concise terminal chat assistant. Answer plainly. You have "
        "access to searxng_search for web search and todo_kanban for managing "
        "a local kanban board. Use search when the user asks for current, "
        "external, or source-backed information, and cite the URL from the tool "
        "result when you use it. Use todo_kanban when the user asks you to "
        "remember, plan, list, move, limit, or archive work. todo_kanban has a "
        "default board; omit board_id and board_name for ordinary single-board "
        "usage.";
  }
  agent_config.prompt_cache_key = "cai:example:terminal-chat:v1";
  agent_config.reasoning_summary = CAI_REASONING_SUMMARY_AUTO;
  run_options.max_tool_rounds = 10;
  searxng_config.base_url = searxng_base_url();
  searxng_config.engine = getenv("CAI_SEARXNG_ENGINE");
  todo_config.store_path = getenv("CAI_TODO_STORE");
  todo_config.lock_path = getenv("CAI_TODO_LOCK");
  todo_config.default_board = getenv("CAI_TODO_BOARD");
  if (todo_config.default_board == NULL ||
      todo_config.default_board[0] == '\0') {
    todo_config.default_board = "default";
  }
  if (exec_tool_dir != NULL) {
    exec_config.root_path = exec_tool_dir;
    exec_config.default_workdir = exec_tool_dir;
    exec_config.timeout_ms = 10000L;
    exec_config.max_timeout_ms = 60000L;
    exec_config.output_memory_limit = 128U * 1024U;
    exec_config.output_max_bytes = 1024U * 1024U;
    exec_config.allow_pty = 1;
  }
  if (read_tool_dir != NULL) {
    read_config.root_path = read_tool_dir;
    read_config.default_workdir = read_tool_dir;
    read_config.content_memory_limit = 128U * 1024U;
    read_config.content_max_bytes = 1024U * 1024U;
  }
  client = NULL;
  agent = NULL;
  session = NULL;
  stdout_sink = NULL;
  tool_trace.fp = stdout;
  tool_trace.sink = NULL;
  dotenv_api_key = NULL;
  exit_code = 1;
  total_spent_usd = 0.0;

  if (exec_tool_dir == NULL) {
    fprintf(stderr,
            "hint: pass --exec-tool-dir <path> to enable exec_command rooted "
            "to that path\n");
  } else {
    fprintf(stderr, "exec_command enabled with root: %s\n", exec_tool_dir);
  }
  if (read_tool_dir == NULL) {
    fprintf(stderr,
            "hint: pass --read-tool-dir <path> to enable read_file rooted "
            "to that path\n");
  } else {
    fprintf(stderr, "read_file enabled with root: %s\n", read_tool_dir);
  }

  rc = cai_example_load_dotenv_api_key(&client_config, &dotenv_api_key, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_load_dotenv_api_key", rc, &error);
    goto done;
  }
  rc = cai_client_open(&client_config, &client, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_client_open", rc, &error);
    goto done;
  }
  rc = client->new_agent(client, &agent_config, &agent, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_client_new_agent", rc, &error);
    goto done;
  }
  rc = cai_agent_register_searxng_tool(agent, &searxng_config, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_agent_register_searxng_tool", rc, &error);
    goto done;
  }
  rc = cai_agent_register_todo_tool(agent, &todo_config, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_agent_register_todo_tool", rc, &error);
    goto done;
  }
  if (exec_tool_dir != NULL) {
    rc = cai_agent_register_exec_tool(agent, &exec_config, &error);
    if (rc != CAI_OK) {
      exit_code = print_error("cai_agent_register_exec_tool", rc, &error);
      goto done;
    }
  }
  if (read_tool_dir != NULL) {
    rc = cai_agent_register_read_tool(agent, &read_config, &error);
    if (rc != CAI_OK) {
      exit_code = print_error("cai_agent_register_read_tool", rc, &error);
      goto done;
    }
  }
  rc = agent->new_session(agent, &session, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_agent_new_session", rc, &error);
    goto done;
  }
  rc = cai_sink_stdout(&stdout_sink, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_sink_stdout", rc, &error);
    goto done;
  }
  tool_trace.sink = stdout_sink;
  run_options.tool_event = print_tool_event;
  run_options.tool_event_context = &tool_trace;
  cai_stream_sinks_init(&stream_sinks);
  stream_sinks.reasoning_summary = stdout_sink;
  stream_sinks.output_text = stdout_sink;
  stream_sinks.reasoning_summary_prefix.text = CAI_REASONING_PREFIX;
  stream_sinks.reasoning_summary_suffix.text = CAI_REASONING_SUFFIX;
  stream_sinks.output_text_prefix.text = CAI_RESPONSE_PREFIX;
  stream_sinks.output_text_suffix.text = CAI_RESPONSE_SUFFIX;

  for (;;) {
    fputs("> ", stdout);
    fflush(stdout);
    if (fgets(line, sizeof(line), stdin) == NULL) {
      if (ferror(stdin)) {
        fprintf(stderr, "stdin read failed\n");
        exit_code = 1;
        break;
      }
      if (isatty(STDIN_FILENO)) {
        fputc('\n', stdout);
      }
      exit_code = 0;
      break;
    }
    trim_newline(line);
    if (line[0] == '\0') {
      continue;
    }
    if (strcmp(line, "/exit") == 0 || strcmp(line, "/quit") == 0 ||
        strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
      exit_code = 0;
      break;
    }
    rc = session->add_user_text(session, line, &error);
    if (rc == CAI_OK) {
      rc = session->stream_auto(session, &run_options, &stream_sinks, &error);
    }
    fputc('\n', stdout);
    fflush(stdout);
    if (rc != CAI_OK) {
      exit_code = print_error("cai_session_stream_auto", rc, &error);
      break;
    }
    if (session->last_usage(session, &usage, &error) == CAI_OK) {
      total_spent_usd += cai_model_estimate_usage_usd(
          agent_config.model, usage.input_tokens, usage.input_cached_tokens,
          usage.output_tokens);
      context_percent = 0.0;
      has_context_percent =
          session->context_percent(session, &context_percent, &error) == CAI_OK;
      if (!has_context_percent) {
        cai_error_cleanup(&error);
        cai_error_init(&error);
      }
      print_usage(&usage, context_percent, has_context_percent,
                  total_spent_usd);
    } else {
      cai_error_cleanup(&error);
      cai_error_init(&error);
    }
  }

done:
  cai_sink_close(stdout_sink);
  if (session != NULL) {
    session->close(session);
  }
  if (agent != NULL) {
    agent->close(agent);
  }
  if (client != NULL) {
    client->close(client);
  }
  cai_string_destroy(dotenv_api_key);
  cai_error_cleanup(&error);
  return exit_code;
}
