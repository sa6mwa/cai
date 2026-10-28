#include "../cli/replay_tools.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct fixture {
  cai_cli_replay_tools *tools;
  cai_tool_display *display;
  cai_sink *sink;
  cai_error error;
  char text[32768];
  size_t length;
  unsigned long long sequence;
  int fail, phase;
} fixture;
static int capture(void *context, const void *bytes, size_t count,
                   cai_error *error) {
  fixture *f = context;
  (void)error;
  if (f->fail)
    return CAI_ERR_TRANSPORT;
  assert(count < sizeof(f->text) - f->length);
  memcpy(f->text + f->length, bytes, count);
  f->length += count;
  f->text[f->length] = '\0';
  return CAI_OK;
}
static int markdown(void *context, const char *bytes, size_t count) {
  return capture(context, bytes, count, NULL) == CAI_OK ? 0 : -1;
}
static void open_fixture(fixture *f, int full) {
  cai_sink_callbacks sink;
  cai_tool_display_config config;
  memset(f, 0, sizeof(*f));
  cai_error_init(&f->error);
  memset(&sink, 0, sizeof(sink));
  sink.context = f;
  sink.write = capture;
  assert(cai_sink_from_callbacks(&sink, &f->sink, &f->error) == CAI_OK);
  memset(&config, 0, sizeof(config));
  config.sink = f->sink;
  config.mode = full ? CAI_TOOL_OUTPUT_FULL : CAI_TOOL_OUTPUT_COMPACT;
  assert(cai_tool_display_new(&config, &f->display, &f->error) == CAI_OK);
  assert(cai_tool_display_set_width(f->display, 80U, &f->error) == CAI_OK);
}
static void close_fixture(fixture *f) {
  cai_cli_replay_tools_close(f->tools);
  cai_tool_display_close(f->display);
  cai_sink_close(f->sink);
  cai_error_cleanup(&f->error);
}
static int record(fixture *f, const char *session, const char *id,
                  const char *name, const char *kind, const char *field,
                  const char *data, size_t step) {
  cai_execution_record r;
  size_t count = strlen(data), offset = 0U, n;
  int handled, rc;
  do {
    memset(&r, 0, sizeof(r));
    r.session_id = session;
    r.call_id = id;
    r.tool_name = name;
    r.kind = kind;
    r.field = field;
    r.offset = offset;
    n = count - offset;
    if (n > step)
      n = step;
    assert(n <= CAI_EXECUTION_CHUNK_BYTES);
    memcpy(r.data, data + offset, n);
    r.data[n] = '\0';
    offset += n;
    r.final = offset == count;
    if (!f->phase)
      rc = cai_cli_replay_tools_index(&f->tools, &r, ++f->sequence, 1U,
                                      &f->error);
    else {
      rc = cai_cli_replay_tools_render(f->tools, &r, f->display, markdown, f,
                                       &handled, &f->error);
      assert(handled);
    }
    if (rc != CAI_OK)
      return rc;
  } while (offset < count);
  return CAI_OK;
}
#define FEED(s, id, tool, kind, field, data, step)                             \
  assert(record(&f, s, id, tool, kind, field, data, step) == CAI_OK)
static void commands(int full, size_t step) {
  fixture f;
  int phase;
  const char *body =
      "{\"output\":\"one\\ntwo\\nthree\\nfour\\nfive\\nsix\\n\\u03bb\\n\","
      "\"session_id\":\"terminal-1\",\"running\":true}";
  open_fixture(&f, full);
  for (phase = 0; phase < 2; phase++) {
    f.phase = phase;
    FEED("parent", "a", "exec_command", "tool.started", NULL, "", step);
    FEED("parent", "a", "exec_command", "tool.arguments", "data",
         "{\"cmd\":\"printf 'hello'\"}", step);
    FEED("parent", "a", "exec_command", "tool.completed", NULL, "", step);
    FEED("parent", "a", "exec_command", "tool.output", "data", body, step);
    /* write_stdin recovers the originating command, even with a distinct
     * tool call ID. A child's identical call/terminal IDs cannot replace it. */
    FEED("child", "a", "exec_command", "tool.arguments", "data",
         "{\"cmd\":\"child command\"}", step);
    FEED("child", "a", "exec_command", "tool.completed", NULL, "", step);
    FEED("child", "a", "exec_command", "tool.output", "data",
         "{\"session_id\":\"terminal-1\",\"output\":\"\\u001b[32mchild\\u001b["
         "0m\\n\\u001b]0;hidden\\u0007\",\"exit_code\":0}",
         step);
    FEED("parent", "b", "write_stdin", "tool.arguments", "data",
         "{\"session_id\":\"terminal-1\"}", step);
    FEED("parent", "b", "write_stdin", "tool.completed", NULL, "", step);
    FEED("parent", "b", "write_stdin", "tool.output", "data",
         "{\"session_id\":\"terminal-1\",\"output\":\"later\\n\",\"exit_code\":"
         "7,\"signal\":0,\"duration_ms\":63000}",
         step);
  }
  assert(strstr(
      f.text, "* Ran for 1m3s printf 'hello'\none\ntwo\nthree\nfour\nfive\n"));
  assert(strstr(f.text, "exit=7 signal=0"));
  assert(strstr(f.text, "* Ran for 0s child command\nchild\n"));
  assert(strstr(strstr(f.text, "* Ran for 1m3s printf 'hello'") + 1,
                "* Ran for 1m3s printf 'hello'") == NULL);
  assert(!strstr(f.text, "Running"));
  assert(!strstr(f.text, "Tool completed"));
  assert(!strstr(f.text, "session_id"));
  assert(!strstr(f.text, "hidden"));
  assert(!strchr(f.text, '\033'));
  if (full) {
    assert(strstr(f.text, "six\nλ\n"));
    assert(strstr(f.text, "later\n"));
    assert(!strstr(f.text, "omitted"));
  } else {
    assert(!strstr(f.text, "six\n"));
    assert(strstr(f.text, "output omitted"));
  }
  close_fixture(&f);
}
static void native_tools(void) {
  fixture f;
  int phase;
  open_fixture(&f, 0);
  for (phase = 0; phase < 2; phase++) {
    f.phase = phase;
    FEED("s", "read", "read_file", "tool.arguments", "data",
         "{\"path\":\"src/main.c\"}", 2U);
    FEED("s", "read", "read_file", "tool.completed", NULL, "", 2U);
    FEED("s", "read", "read_file", "tool.output", "data",
         "private file content", 2U);
    FEED("s", "reason", "set_reasoning", "tool.completed", NULL, "", 10U);
    FEED("s", "reason", "set_reasoning", "tool.output", "data", "ok", 10U);
    FEED("s", "reason", "set_reasoning", "reasoning.receipt", NULL,
         "requested high (high): medium -> high", 4096U);
    FEED("s", "patch", "apply_patch", "tool.arguments", "data",
         "*** Begin Patch\n*** Update File: x.c\n@@\n-old\n+new\n*** End "
         "Patch\n",
         7U);
    FEED("s", "patch", "apply_patch", "tool.completed", NULL, "", 10U);
    FEED("s", "patch", "apply_patch", "tool.output", "data",
         "Success. Updated the following files:\nM x.c\n", 10U);
    FEED("s", "patch", "apply_patch", "tool.patch_locations", "data", "20 20\n",
         2U);
    FEED("s", "patch", "apply_patch", "tool.patch", "data",
         "*** Begin Patch\n*** Update File: x.c\n@@\n context\n-old\n+new\n "
         "after\n*** End Patch\n",
         2U);
    FEED("s", "bad", "apply_patch", "tool.failed", NULL, "", 10U);
    FEED("s", "bad", "apply_patch", "tool.failed", "message",
         "Patch does not match.", 100U);
    FEED("s", "hosted", "web_search_call", "hosted.tool", "item",
         "{\"id\":\"hosted\"}", 2U);
    FEED("s", "partial", "exec_command", "tool.arguments", "data",
         "{\"cmd\":\"interrupted command\"}", 2U);
    FEED("s", "partial", "exec_command", "tool.failed", "message",
         "command interrupted", 2U);
    FEED("s", "partial", "exec_command", "tool.partial_output", "data",
         "{\"output\":\"partial command text\\n\"}", 2U);
    FEED("s", "original", "apply_patch", "tool.arguments", "data",
         "*** Begin Patch\n*** Update File: "
         "original.c\n@@\n-old\n+retained\n*** End Patch\n",
         7U);
    FEED("s", "original", "apply_patch", "tool.completed", NULL, "", 10U);
    FEED("s", "original", "apply_patch", "tool.output", "data", "Success", 10U);
    FEED("s", "message", "agent_message", "tool.completed", NULL, "", 10U);
    FEED("s", "message", "agent_message", "tool.output", "data",
         "{\"status\":\"delivered\",\"recipient\":\"cai.test\"}", 4096U);
    FEED("s", "mcp", "mcp_server_tool", "tool.completed", NULL, "", 10U);
    FEED("s", "mcp", "mcp_server_tool", "tool.output", "data",
         "{\"isError\":true,\"content\":[]}", 2U);
  }
  assert(strstr(f.text, "* Read src/main.c\n"));
  assert(!strstr(f.text, "private file content"));
  assert(strstr(f.text, "* Reasoning requested high (high): medium -> high\n"));
  assert(!strstr(f.text, "Accepted reasoning request"));
  assert(!strstr(f.text, "Completed set_reasoning"));
  assert(!strstr(f.text, "\nok\n"));
  assert(strstr(f.text, "* Edited x.c"));
  assert(strstr(f.text, "  21 -old"));
  assert(strstr(f.text, "  21 +new"));
  assert(strstr(f.text, "* Failed apply_patch\n"));
  assert(strstr(f.text, "Patch does not match."));
  assert(strstr(f.text, "* Used web search\n"));
  assert(strstr(f.text, "* Partial output from interrupted command\n"));
  assert(strstr(f.text, ">     command interrupted\n\n"));
  assert(strstr(f.text, "partial command text\n"));
  assert(strstr(f.text, "* Edited original.c\n"));
  assert(strstr(f.text, "+retained"));
  assert(strstr(f.text, "* Delivered message to cai.test\n"));
  assert(!strstr(f.text, "\"recipient\""));
  assert(strstr(f.text, "* MCP failed mcp_server_tool\n"));
  close_fixture(&f);
}
static void streaming_and_errors(void) {
  fixture f;
  cai_execution_record r;
  int handled;
  open_fixture(&f, 1);
  FEED("s", "c", "exec_command", "tool.arguments", "data",
       "{\"cmd\":\"stream\"}", 4096U);
  FEED("s", "c", "exec_command", "tool.completed", NULL, "", 4096U);
  FEED("s", "c", "exec_command", "tool.output", "data",
       "{\"output\":\"ready\\n\",\"exit_code\":0}", 4096U);
  f.phase = 1;
  memset(&r, 0, sizeof(r));
  r.session_id = "s";
  r.call_id = "c";
  r.tool_name = "exec_command";
  r.kind = "tool.output";
  r.field = "data";
  strcpy(r.data, "{\"output\":\"ready\\n");
  assert(cai_cli_replay_tools_render(f.tools, &r, f.display, markdown, &f,
                                     &handled, &f.error) == CAI_OK);
  assert(strstr(f.text, "ready\n")); /* Before EOF or the closing string. */
  r.offset = strlen(r.data);
  strcpy(r.data, "\",\"exit_code\":0}");
  r.final = 1;
  assert(cai_cli_replay_tools_render(f.tools, &r, f.display, markdown, &f,
                                     &handled, &f.error) == CAI_OK);
  f.fail = 1;
  r.offset = 0U;
  assert(cai_cli_replay_tools_render(f.tools, &r, f.display, markdown, &f,
                                     &handled, &f.error) == CAI_ERR_TRANSPORT);
  close_fixture(&f);
}
static void streamed_markdown(void) {
  fixture f;
  int phase;
  open_fixture(&f, 0);
  for (phase = 0; phase < 2; phase++) {
    f.phase = phase;
    FEED("s", "md", "custom_tool", "tool.arguments", "data",
         "{\"path\":\"unrelated label\"}", 1U);
    FEED("s", "md", "custom_tool", "tool.completed", NULL, "", 1U);
    FEED("s", "md", "custom_tool", "tool.output", "data",
         "## Report\n\n**details**\n", 1U);
    FEED("s", "failure", "run_review", "tool.failed", "message",
         "pouch lease already held", 1U);
  }
  assert(
      strstr(f.text, "* Completed custom_tool\n## Report\n\n**details**\n\n"));
  assert(!strstr(f.text, "unrelated label"));
  assert(strstr(f.text,
                "* Failed run_review\n>     pouch lease already held\n\n"));
  close_fixture(&f);
}
static void large_output(void) {
  fixture f;
  cai_execution_record r;
  size_t offset;
  int phase, i, handled;
  open_fixture(&f, 0);
  for (phase = 0; phase < 2; phase++) {
    f.phase = phase;
    FEED("s", "large", "exec_command", "tool.arguments", "data",
         "{\"cmd\":\"large output\"}", 4096U);
    FEED("s", "large", "exec_command", "tool.completed", NULL, "", 4096U);
    memset(&r, 0, sizeof(r));
    r.session_id = "s";
    r.call_id = "large";
    r.tool_name = "exec_command";
    r.kind = "tool.output";
    r.field = "data";
    strcpy(r.data, "{\"output\":\"");
    offset = 0U;
    for (i = 0; i < 130; i++) {
      r.offset = offset;
      if (!phase)
        assert(cai_cli_replay_tools_index(&f.tools, &r, ++f.sequence, 1U,
                                          &f.error) == CAI_OK);
      else
        assert(cai_cli_replay_tools_render(f.tools, &r, f.display, markdown, &f,
                                           &handled, &f.error) == CAI_OK);
      offset += strlen(r.data);
      if (i == 128) {
        strcpy(r.data, "\",\"exit_code\":0}");
        r.final = 1;
      } else {
        size_t n;
        for (n = 0U; n < 4095U; n += 5U)
          memcpy(r.data + n, "row\\n", 5U);
        r.data[4095] = '\0';
      }
    }
  }
  assert(strstr(f.text, "* Ran for 0s large output\nrow\n"));
  assert(strstr(f.text, "output omitted"));
  assert(f.length < 1024U);
  assert(cai_cli_replay_tools_finish(f.tools, &f.error) == CAI_OK);
  close_fixture(&f);
}
static void invalid_output(void) {
  fixture f;
  int phase;
  open_fixture(&f, 0);
  for (phase = 0; phase < 2; phase++) {
    f.phase = phase;
    FEED("s", "bad", "exec_command", "tool.arguments", "data",
         "{\"cmd\":\"broken result\"}", 4096U);
    FEED("s", "bad", "exec_command", "tool.completed", NULL, "", 4096U);
    FEED("s", "bad", "exec_command", "tool.output", "data",
         "{\"output\":\"unfinished", 2U);
  }
  assert(strstr(f.text, "* Failed to replay broken result"));
  assert(strstr(f.text, "Saved command output is incomplete or invalid."));
  assert(
      strstr(f.text, ">     Saved command output is incomplete or invalid."));
  assert(cai_cli_replay_tools_finish(f.tools, &f.error) == CAI_OK);
  close_fixture(&f);
}
int main(void) {
  commands(0, 1U);
  commands(1, 3U);
  commands(0, 4096U);
  native_tools();
  streaming_and_errors();
  streamed_markdown();
  large_output();
  invalid_output();
  puts("replay tool presentation passed");
  return 0;
}
