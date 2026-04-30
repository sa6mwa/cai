#include <cai/cai.h>

#include <stdio.h>
#include <string.h>

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
                        int has_context_percent) {
  if (has_context_percent) {
    fprintf(stderr,
            "[usage] input=%lld cached=%lld output=%lld reasoning=%lld "
            "total=%lld context=%.2f%%\n",
            usage->input_tokens, usage->input_cached_tokens,
            usage->output_tokens, usage->output_reasoning_tokens,
            usage->total_tokens, context_percent);
    return;
  }
  fprintf(stderr,
          "[usage] input=%lld cached=%lld output=%lld reasoning=%lld "
          "total=%lld context=n/a\n",
          usage->input_tokens, usage->input_cached_tokens, usage->output_tokens,
          usage->output_reasoning_tokens, usage->total_tokens);
}

int main(void) {
  cai_agent_config agent_config;
  cai_client_config client_config;
  cai_sink_callbacks sink_callbacks;
  cai_client *client;
  cai_agent *agent;
  cai_session *session;
  cai_sink *sink;
  cai_error error;
  cai_token_usage usage;
  double context_percent;
  int has_context_percent;
  char line[4096];
  int exit_code;
  int rc;

  cai_error_init(&error);
  cai_client_config_init(&client_config);
  cai_agent_config_init(&agent_config);
  agent_config.model = CAI_MODEL_GPT_5_NANO;
  agent_config.instructions =
      "You are a concise terminal chat assistant. Answer plainly.";
  agent_config.auto_compact = 1;
  client = NULL;
  agent = NULL;
  session = NULL;
  sink = NULL;
  exit_code = 1;

  rc = cai_client_open(&client_config, &client, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_client_open", rc, &error);
    goto done;
  }
  rc = cai_client_new_agent(client, &agent_config, &agent, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_client_new_agent", rc, &error);
    goto done;
  }
  rc = cai_agent_new_session(agent, &session, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_agent_new_session", rc, &error);
    goto done;
  }
  sink_callbacks.write = stdout_sink_write;
  sink_callbacks.close = NULL;
  sink_callbacks.context = NULL;
  rc = cai_sink_from_callbacks(&sink_callbacks, &sink, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_sink_from_callbacks", rc, &error);
    goto done;
  }

  for (;;) {
    fputs("> ", stdout);
    fflush(stdout);
    if (fgets(line, sizeof(line), stdin) == NULL) {
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
    rc = cai_session_add_text(session, "user", line, &error);
    if (rc == CAI_OK) {
      rc = cai_session_stream_text(session, sink, &error);
    }
    fputc('\n', stdout);
    if (rc != CAI_OK) {
      exit_code = print_error("cai_session_stream_text", rc, &error);
      break;
    }
    if (cai_session_last_usage(session, &usage, &error) == CAI_OK) {
      context_percent = 0.0;
      has_context_percent = cai_session_context_percent(
                                session, &context_percent, &error) == CAI_OK;
      if (!has_context_percent) {
        cai_error_cleanup(&error);
        cai_error_init(&error);
      }
      print_usage(&usage, context_percent, has_context_percent);
    } else {
      cai_error_cleanup(&error);
      cai_error_init(&error);
    }
  }

done:
  cai_sink_close(sink);
  cai_session_destroy(session);
  cai_agent_destroy(agent);
  cai_client_close(client);
  cai_error_cleanup(&error);
  return exit_code;
}
