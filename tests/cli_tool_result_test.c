#include "../cli/tool_result.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct capture {
  char text[16384];
  size_t length;
  int fail;
} capture;
static int write_text(void *context, const char *bytes, size_t count) {
  capture *c = context;
  if (c->fail)
    return -1;
  assert(count < sizeof(c->text) - c->length);
  memcpy(c->text + c->length, bytes, count);
  c->length += count;
  c->text[c->length] = '\0';
  return 0;
}
static int write_sink(void *context, const void *bytes, size_t count,
                      cai_error *error) {
  (void)error;
  return write_text(context, bytes, count) == 0 ? CAI_OK : CAI_ERR_TRANSPORT;
}
static int format(capture *c, const char *tool, int action, int type,
                  const char *body) {
  cai_cli_tool_result *result = NULL;
  cai_agent_runtime_event event;
  cai_tool_display_config config;
  cai_tool_display *display = NULL;
  cai_sink_callbacks callbacks;
  cai_sink *sink = NULL;
  cai_error error;
  int handled, rc;
  memset(&event, 0, sizeof(event));
  event.type = type;
  event.tool_name = tool;
  event.tool_action = action;
  event.tool_call_id = "call";
  event.runtime_session_id = "session";
  event.data = body;
  event.data_length = body != NULL ? strlen(body) : 0U;
  cai_error_init(&error);
  assert(cai_cli_tool_result_open(&event, &result, &error) == CAI_OK);
  handled = result != NULL;
  if (handled) {
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.context = c;
    callbacks.write = write_sink;
    assert(cai_sink_from_callbacks(&callbacks, &sink, &error) == CAI_OK);
    memset(&config, 0, sizeof(config));
    config.sink = sink;
    config.color = 1;
    assert(cai_tool_display_new(&config, &display, &error) == CAI_OK);
    event.type = CAI_AGENT_EVENT_TOOL_CALL_STARTED;
    assert(cai_tool_display_event(display, &event, &error) == CAI_OK);
    event.type = type;
    rc = cai_cli_tool_result_heading(result, display, &event, &error);
    if (rc == CAI_OK)
      rc = cai_cli_tool_result_write(result, write_text, c, &error);
    assert(rc == (c->fail ? CAI_ERR_TRANSPORT : CAI_OK));
    /* Summary completion releases call state, including sink failures. */
    event.type = CAI_AGENT_EVENT_TOOL_CALL_STARTED;
    assert(cai_tool_display_event(display, &event, &error) == CAI_OK);
    cai_tool_display_close(display);
    cai_sink_close(sink);
  }
  cai_cli_tool_result_close(result);
  cai_error_cleanup(&error);
  return handled;
}
static int result(capture *c, const char *tool, const char *body) {
  memset(c, 0, sizeof(*c));
  return format(c, tool, CAI_AGENT_TOOL_ACTION_EXTERNAL,
                CAI_AGENT_EVENT_TOOL_CALL_COMPLETED, body);
}
int main(void) {
  capture c;
  size_t i;
  static const char *invalid[] = {
      NULL,
      "",
      "not json",
      "{}",
      "{\"status\":\"delivered\"}",
      "{\"status\":\"failed\",\"recipient\":\"q\"}",
      "{\"status\":\"delivered\",\"recipient\":12}",
      "{\"status\":\"delivered\",\"recipient\":\"\"}",
      "{\"status\":\"delivered\",\"recipient\":\"q\"} trailing"};
  assert(result(&c, "set_reasoning", "ok"));
  assert(strstr(c.text, "Accepted ") && strstr(c.text, "reasoning request"));
  assert(!strstr(c.text, "\nok") && !strstr(c.text, "Completed"));
  assert(!result(&c, "set_reasoning", "not ok"));
  assert(result(&c, "agent_message",
                "{\"status\":\"delivered\",\"recipient\":\"cai.y\",\"message_"
                "id\":\"id\"}"));
  assert(strstr(c.text, "Delivered message to ") && strstr(c.text, "cai.y"));
  assert(!strstr(c.text, "message_id") && !strstr(c.text, "{\""));
  assert(result(&c, "agent_message",
                "## Message queues\n\n| Queue | Workspace | Conversation |\n| "
                "--- | --- | --- |\n| cai.y | /work | session |\n"));
  assert(strstr(c.text, "Listed ") &&
         strstr(c.text, "| cai.y | /work | session |"));
  assert(
      result(&c, "todo_kanban",
             "{\"ok\":true,\"operation\":\"add_item\",\"code\":\"ok\",\"item_"
             "id\":\"DEF-001\",\"title\":\"Fix it\",\"status\":\"todo\"}"));
  assert(strstr(c.text, "Added task ") && strstr(c.text, "DEF-001"));
  assert(strstr(c.text, "Fix it — todo") && !strstr(c.text, "{\""));
  assert(
      result(&c, "todo_kanban",
             "{\"ok\":true,\"operation\":\"list_board\",\"code\":\"ok\","
             "\"items\":[{\"id\":\"DEF-001\",\"status\":\"todo\",\"title\":"
             "\"Pipe | **star** \\u001b[2J \\u009b\"}],\"truncated\":true}"));
  assert(strstr(c.text, "| Task | Status | Title |"));
  assert(strstr(c.text, "Pipe \\| \\*\\*star\\*\\*  \\[2J"));
  assert(!strstr(c.text, "\033[2J") && !strstr(c.text, "\302\233"));
  assert(strstr(c.text, "Result limit reached"));
  assert(result(&c, "todo_kanban",
                "{\"ok\":true,\"operation\":\"list_board\",\"code\":\"ok\","
                "\"items\":[]}"));
  assert(strstr(c.text, "No tasks."));
  assert(result(&c, "todo_kanban",
                "{\"ok\":true,\"operation\":\"list_boards\",\"code\":\"ok\","
                "\"boards\":[{\"key\":\"DEF\",\"name\":\"default\"}]}"));
  assert(strstr(c.text, "| Key | Board |") &&
         strstr(c.text, "| DEF | default |"));
  assert(result(&c, "todo_kanban",
                "{\"ok\":false,\"operation\":\"move_item\",\"code\":\"wip_"
                "limit_exceeded\",\"message\":\"Limit exceeded\"}"));
  assert(strstr(c.text, "\033[91m*\033[0m") &&
         strstr(c.text, "Kanban declined"));
  assert(strstr(c.text, "Limit exceeded"));
  assert(!result(&c, "todo_kanban", "{\"operation\":\"list_board\"}"));
  assert(result(&c, "searxng_search",
                "{\"query\":\"query\",\"title\":\"Title\",\"url\":\"https://"
                "example.test\",\"snippet\":\"Result snippet\"}"));
  assert(strstr(c.text, "Searched ") && strstr(c.text, "Result snippet"));
  assert(result(&c, "reverse_geocode", "{\"label\":\"Gothenburg\"}"));
  assert(strstr(c.text, "Located ") && strstr(c.text, "Gothenburg"));
  for (i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); i++)
    assert(!result(&c, "agent_message", invalid[i]));
  assert(!result(&c, "custom_tool", "ok"));
  memset(&c, 0, sizeof(c));
  assert(!format(&c, "set_reasoning", CAI_AGENT_TOOL_ACTION_MCP,
                 CAI_AGENT_EVENT_TOOL_CALL_COMPLETED, "ok"));
  assert(!format(&c, "set_reasoning", CAI_AGENT_TOOL_ACTION_EXTERNAL,
                 CAI_AGENT_EVENT_TOOL_CALL_FAILED, "failure detail"));
  memset(&c, 0, sizeof(c));
  assert(format(&c, "apply_patch", CAI_AGENT_TOOL_ACTION_PATCH,
                CAI_AGENT_EVENT_TOOL_CALL_FAILED,
                "Patch context does not match in \"file.c\" (hunk 2).\n"
                "Expected: \"int    x;\"\nActual:   \"int  x;\""));
  assert(strstr(c.text, "\033[91m*\033[0m") && strstr(c.text, "Failed"));
  assert(strstr(c.text, "> Patch context"));
  assert(strstr(
      c.text, "\n> Expected: ` \"int    x;\" `  \n> Actual: ` \"int  x;\" `"));
  memset(&c, 0, sizeof(c));
  assert(format(&c, "apply_patch", CAI_AGENT_TOOL_ACTION_PATCH,
                CAI_AGENT_EVENT_TOOL_CALL_FAILED,
                "head\n```\n**source** [link](url)\033\302\233\nend"));
  assert(strstr(
      c.text, "> head  \n> \\`\\`\\`  \n> \\*\\*source\\*\\* \\[link\\](url)"));
  assert(strstr(c.text, "\n> end  \n\n"));
  assert(!strstr(c.text, "\033\302\233") && !strstr(c.text, "\302\233"));
  memset(&c, 0, sizeof(c));
  assert(format(&c, "apply_patch", CAI_AGENT_TOOL_ACTION_PATCH,
                CAI_AGENT_EVENT_TOOL_CALL_FAILED, NULL));
  assert(strstr(c.text, "no details provided"));
  memset(&c, 0, sizeof(c));
  assert(format(&c, "apply_patch", CAI_AGENT_TOOL_ACTION_PATCH,
                CAI_AGENT_EVENT_TOOL_CALL_FAILED, ""));
  assert(strstr(c.text, "no details provided"));
  memset(&c, 0, sizeof(c));
  c.fail = 1;
  assert(format(&c, "apply_patch", CAI_AGENT_TOOL_ACTION_PATCH,
                CAI_AGENT_EVENT_TOOL_CALL_FAILED, "failure detail"));
  c.fail = 1;
  assert(format(&c, "agent_message", CAI_AGENT_TOOL_ACTION_EXTERNAL,
                CAI_AGENT_EVENT_TOOL_CALL_COMPLETED,
                "{\"status\":\"delivered\",\"recipient\":\"q\"}"));
  {
    cai_cli_tool_result *prepared = NULL;
    cai_agent_runtime_event event;
    cai_error error;
    memset(&event, 0, sizeof(event));
    event.type = CAI_AGENT_EVENT_TOOL_CALL_COMPLETED;
    event.tool_action = CAI_AGENT_TOOL_ACTION_EXTERNAL;
    event.tool_name = "todo_kanban";
    event.data = "{\"ok\":true,\"operation\":\"add_item\",\"code\":\"ok\","
                 "\"message\":\"item added\"}";
    event.data_length = strlen(event.data);
    cai_error_init(&error);
    assert(cai_cli_tool_result_open(&event, &prepared, &error) == CAI_OK &&
           prepared != NULL);
    assert(cai_cli_tool_result_write(prepared, write_text, &c, &error) ==
           CAI_ERR_TRANSPORT);
    cai_cli_tool_result_close(prepared);
    cai_error_cleanup(&error);
    prepared = NULL;
    event.type = CAI_AGENT_EVENT_TOOL_CALL_FAILED;
    event.tool_action = CAI_AGENT_TOOL_ACTION_PATCH;
    event.tool_name = "apply_patch";
    event.data = "Cannot find the requested context.";
    event.data_length = strlen(event.data);
    cai_error_init(&error);
    assert(cai_cli_tool_result_open(&event, &prepared, &error) == CAI_OK &&
           prepared != NULL);
    assert(cai_cli_tool_result_write(prepared, write_text, &c, &error) ==
           CAI_ERR_TRANSPORT);
    cai_cli_tool_result_close(prepared);
    cai_error_cleanup(&error);
  }
  puts("built-in tool presentation checks passed");
  return 0;
}
