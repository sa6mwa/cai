#include <cai/cai.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CAI_ANSI_RESET "\033[0m"
#define CAI_ANSI_GRAY "\033[90m"
#define CAI_ANSI_GREEN "\033[32m"
#define CAI_ANSI_BRIGHT_CYAN "\033[96m"

static int print_error(const char *operation, int rc, const cai_error *error) {
  fprintf(stderr, "%s failed: %s\n", operation,
          error->message != NULL ? error->message : cai_status_string(rc));
  if (error->detail != NULL) {
    fprintf(stderr, "detail: %s\n", error->detail);
  }
  return 1;
}

static int stdout_sink_write(void *context, const void *bytes, size_t count,
                             cai_error *error) {
  (void)context;
  (void)error;
  if (count == 0U) {
    return CAI_OK;
  }
  if (fwrite(bytes, 1U, count, stdout) != count) {
    return CAI_ERR_TRANSPORT;
  }
  fflush(stdout);
  return CAI_OK;
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
            "[usage] input=%lld cached=%lld output=%lld reasoning=%lld "
            "total=%lld context=%.2f%% estimated_cost=$%.8f\n",
            usage->input_tokens, usage->input_cached_tokens,
            usage->output_tokens, usage->output_reasoning_tokens,
            usage->total_tokens, context_percent, total_spent_usd);
    return;
  }
  fprintf(stderr,
          "[usage] input=%lld cached=%lld output=%lld reasoning=%lld "
          "total=%lld context=n/a estimated_cost=$%.8f\n",
          usage->input_tokens, usage->input_cached_tokens, usage->output_tokens,
          usage->output_reasoning_tokens, usage->total_tokens, total_spent_usd);
}

int main(void) {
  cai_agent_config agent_config;
  cai_client_config client_config;
  cai_sink_callbacks stdout_callbacks;
  cai_stream_sinks stream_sinks;
  cai_client *client;
  cai_agent *agent;
  cai_session *session;
  cai_sink *stdout_sink;
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
  agent_config.model = CAI_MODEL_GPT_5_NANO;
  agent_config.developer_instructions =
      "You are a concise terminal chat assistant. Answer plainly.";
  agent_config.prompt_cache_key = "cai:example:terminal-chat:v1";
  agent_config.reasoning_summary = CAI_REASONING_SUMMARY_AUTO;
  client = NULL;
  agent = NULL;
  session = NULL;
  stdout_sink = NULL;
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
  rc = agent->new_session(agent, &session, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_agent_new_session", rc, &error);
    goto done;
  }
  stdout_callbacks.write = stdout_sink_write;
  stdout_callbacks.close = NULL;
  stdout_callbacks.context = NULL;
  rc = cai_sink_from_callbacks(&stdout_callbacks, &stdout_sink, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_sink_from_callbacks(stdout)", rc, &error);
    goto done;
  }
  cai_stream_sinks_init(&stream_sinks);
  stream_sinks.reasoning_summary = stdout_sink;
  stream_sinks.output_text = stdout_sink;
  stream_sinks.reasoning_summary_prefix.text = CAI_ANSI_GRAY
      "[" CAI_ANSI_BRIGHT_CYAN "reasoning" CAI_ANSI_GRAY "] " CAI_ANSI_GRAY;
  stream_sinks.reasoning_summary_suffix.text = CAI_ANSI_RESET "\n\n";
  stream_sinks.output_text_prefix.text = CAI_ANSI_GRAY
      "[" CAI_ANSI_GREEN "response" CAI_ANSI_GRAY "]" CAI_ANSI_RESET " ";
  stream_sinks.output_text_suffix.text = CAI_ANSI_RESET "\n";

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
    if (strcmp(line, "/compact") == 0) {
      rc = cai_session_compact_experimental(session, &error);
      if (rc != CAI_OK) {
        exit_code = print_error("cai_session_compact_experimental", rc, &error);
        break;
      }
      fputs("[compact] manual experimental compaction complete\n", stderr);
      continue;
    }
    rc = session->add_user_text(session, line, &error);
    if (rc == CAI_OK) {
      rc = session->stream(session, &stream_sinks, &error);
    }
    fputc('\n', stdout);
    if (rc != CAI_OK) {
      exit_code = print_error("cai_session_stream", rc, &error);
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
