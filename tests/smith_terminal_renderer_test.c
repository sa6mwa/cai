int cai_smith_terminal_example_main(int argc, char **argv);

#define main cai_smith_terminal_example_main
#include "../examples/smith-terminal/main.c"
#undef main

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

static void expect_contains(const char *label, const char *text,
                            const char *needle) {
  if (strstr(text, needle) == NULL) {
    fprintf(stderr, "%s: missing [%s] in [%s]\n", label, needle, text);
    failures++;
  }
}

static void expect_not_contains(const char *label, const char *text,
                                const char *needle) {
  if (strstr(text, needle) != NULL) {
    fprintf(stderr, "%s: unexpectedly found [%s] in [%s]\n", label, needle,
            text);
    failures++;
  }
}

static void deliver(render_state *renderer, int type, const char *data,
                    const char *tool_name, int tool_action,
                    int terminal_has_exit_code, long long terminal_exit_code) {
  cai_agent_runtime_event event;
  cai_error error;

  memset(&event, 0, sizeof(event));
  cai_error_init(&error);
  event.type = type;
  event.data = data;
  event.data_length = data != NULL ? strlen(data) : 0U;
  event.tool_name = tool_name;
  event.tool_action = tool_action;
  event.terminal_has_exit_code = terminal_has_exit_code;
  event.terminal_exit_code = terminal_exit_code;
  if (render_event(renderer, &event, &error) != CAI_OK) {
    fprintf(stderr, "render_event failed: %s\n",
            error.message != NULL ? error.message : "unknown error");
    failures++;
  }
  cai_error_cleanup(&error);
}

static void deliver_subagent_started(render_state *renderer, const char *name,
                                     const char *task) {
  cai_agent_runtime_event event;
  cai_error error;

  memset(&event, 0, sizeof(event));
  cai_error_init(&error);
  event.type = CAI_AGENT_EVENT_SUBAGENT_STARTED;
  event.subagent_name = name;
  event.data = task;
  event.data_length = task != NULL ? strlen(task) : 0U;
  if (render_event(renderer, &event, &error) != CAI_OK) {
    fprintf(stderr, "render subagent start failed: %s\n",
            error.message != NULL ? error.message : "unknown error");
    failures++;
  }
  cai_error_cleanup(&error);
}

static char *capture_fixture(void) {
  FILE *capture;
  long length;
  char *output;
  int saved_stdout;
  render_state renderer;
  const char *report;

  capture = tmpfile();
  if (capture == NULL) {
    perror("tmpfile");
    return NULL;
  }
  fflush(stdout);
  saved_stdout = dup(STDOUT_FILENO);
  if (saved_stdout < 0 || dup2(fileno(capture), STDOUT_FILENO) < 0) {
    perror("dup2");
    if (saved_stdout >= 0) {
      close(saved_stdout);
    }
    fclose(capture);
    return NULL;
  }

  memset(&renderer, 0, sizeof(renderer));
  deliver(&renderer, CAI_AGENT_EVENT_REASONING_SUMMARY, "**Planning ", NULL, 0,
          0, 0);
  deliver(&renderer, CAI_AGENT_EVENT_REASONING_SUMMARY,
          "files**\nInspecting the workspace.", NULL, 0, 0, 0);
  deliver(&renderer, CAI_AGENT_EVENT_REASONING_SUMMARY, "**Planning files**",
          NULL, 0, 0, 0);
  deliver(&renderer, CAI_AGENT_EVENT_TERMINAL_COMMAND_STARTED, "pwd", NULL, 0,
          0, 0);
  deliver(&renderer, CAI_AGENT_EVENT_TERMINAL_OUTPUT, "/tmp/project\n", NULL, 0,
          0, 0);
  deliver(&renderer, CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED, NULL, NULL, 0,
          1, 0);
  deliver(&renderer, CAI_AGENT_EVENT_TOOL_CALL_COMPLETED, NULL, "read_file",
          CAI_AGENT_TOOL_ACTION_READ, 0, 0);
  deliver_subagent_started(&renderer, "review",
                           "Review changes against trunk.\033[31m");
  renderer.review_report_visible = 0;
  report = "{\"findings\":[{\"title\":\"Use stable state\",\"body\":\"Avoid "
           "raw JSON.\","
           "\"confidence_score\":0.9,\"priority\":2,\"code_location\":{"
           "\"absolute_file_path\":\"/tmp/project/a.c\",\"line_range\":{"
           "\"start\":7,\"end\":7}}}],\"overall_correctness\":\"incorrect\","
           "\"overall_explanation\":\"One actionable issue.\","
           "\"overall_confidence_score\":0.9}";
  deliver(&renderer, CAI_AGENT_EVENT_REVIEW_REPORT, report, NULL, 0, 0, 0);
  deliver(&renderer, CAI_AGENT_EVENT_REVIEW_HANDED_OFF, report, NULL, 0, 0, 0);
  fflush(stdout);
  if (dup2(saved_stdout, STDOUT_FILENO) < 0) {
    perror("restore stdout");
    close(saved_stdout);
    fclose(capture);
    return NULL;
  }
  close(saved_stdout);
  if (fseek(capture, 0L, SEEK_END) != 0 || (length = ftell(capture)) < 0 ||
      fseek(capture, 0L, SEEK_SET) != 0) {
    perror("seek capture");
    fclose(capture);
    return NULL;
  }
  output = (char *)malloc((size_t)length + 1U);
  if (output == NULL ||
      fread(output, 1U, (size_t)length, capture) != (size_t)length) {
    perror("read capture");
    free(output);
    fclose(capture);
    return NULL;
  }
  output[length] = '\0';
  fclose(capture);
  return output;
}

int main(void) {
  char *output;

  output = capture_fixture();
  if (output == NULL) {
    return 1;
  }
  expect_contains("heading", output, "Thinking: \033[0mPlanning files\n");
  expect_contains("reasoning body", output, "  Inspecting the workspace.");
  expect_not_contains("raw heading", output, "**Planning files**");
  expect_contains("terminal start", output, "$ pwd\n");
  expect_contains("terminal completion", output, "Ran pwd (exit 0, 0.0s)\n");
  expect_contains("semantic tool receipt", output, "Read\033[0m\n");
  expect_contains("subagent task", output,
                  "Starting \033[0mreview\033[90m subagent\033[0m"
                  "\033[90m — task: \033[0mReview changes against trunk."
                  "\\x1B[31m\n");
  expect_contains("review explanation", output, "One actionable issue.\n");
  expect_contains("review finding", output,
                  "- Use stable state — /tmp/project/a.c:7-7\n");
  expect_contains("review body", output, "  Avoid raw JSON.\n");
  expect_not_contains("review raw json", output, "\"overall_correctness\"");
  free(output);
  return failures == 0 ? 0 : 1;
}
