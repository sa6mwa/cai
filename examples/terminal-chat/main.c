#include <cai/cai.h>
#include <cai/tools/searxng.h>

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

int main(void) {
  cai_agent_config agent_config;
  cai_client_config client_config;
  cai_run_options run_options;
  cai_searxng_tool_config searxng_config;
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
  int has_context_percent;
  char line[4096];
  int exit_code;
  int rc;

  cai_error_init(&error);
  cai_client_config_init(&client_config);
  cai_agent_config_init(&agent_config);
  cai_run_options_init(&run_options);
  memset(&searxng_config, 0, sizeof(searxng_config));
  agent_config.model = CAI_MODEL_GPT_5_NANO;
  agent_config.reasoning_effort = CAI_REASONING_EFFORT_LOW;
  agent_config.developer_instructions =
      "You are a concise terminal chat assistant. Answer plainly. You have "
      "access to searxng_search for web search. Use it when the user asks "
      "for current, external, or source-backed information, and cite the URL "
      "from the tool result when you use it.";
  agent_config.prompt_cache_key = "cai:example:terminal-chat:v1";
  agent_config.reasoning_summary = CAI_REASONING_SUMMARY_AUTO;
  run_options.max_tool_rounds = 10;
  searxng_config.base_url = searxng_base_url();
  searxng_config.engine = getenv("CAI_SEARXNG_ENGINE");
  client = NULL;
  agent = NULL;
  session = NULL;
  stdout_sink = NULL;
  tool_trace.fp = stdout;
  tool_trace.sink = NULL;
  exit_code = 1;
  total_spent_usd = 0.0;

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
  cai_error_cleanup(&error);
  return exit_code;
}
