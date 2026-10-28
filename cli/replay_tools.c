#define _POSIX_C_SOURCE 200809L
#define LONEJSON_WITH_CURL
#include "replay_tools.h"
#include "pouch_stream.h"
#include "tool_result.h"
#include <stdlib.h>
#include <string.h>

/* The existing push decoder supports mapped JSON_VALUE visitors. Wrap each
 * chunked value in a mapped envelope; this does not buffer or spool the value.
 * JSON escapes and UTF-8 may span any number of journal records. */
typedef struct replay_value {
  lonejson_json_value value;
} replay_value;
static const lonejson_field value_fields[] = {
    LONEJSON_FIELD_JSON_VALUE(replay_value, value, "value")};
LONEJSON_MAP_DEFINE(value_map, replay_value, value_fields);
typedef struct replay_call {
  struct replay_call *next;
  struct replay_call *origin, *last_output;
  char *session, *id, *name, *label;
  char terminal[257], number[64];
  size_t label_length, terminal_length, number_length;
  unsigned long long last, offset, command_id, duration_ms;
  int failed, completed, action, has_exit, has_signal, truncated, is_error;
  long long exit_code, signal;
  int parsing, invalid, output, rendering, opened, patch, has_preview,
      bad_output;
  int replayed;
  cai_cli_tool_text markdown;
  lonejson_curl_parse parser;
  replay_value value;
  lonejson_path_value_visitor visitor;
  cai_tool_display *display;
  cai_agent_runtime_event event;
  cai_error *error;
  int rc;
} replay_call;
struct cai_cli_replay_tools {
  lonejson *json;
  replay_call *calls;
};
static int same(const char *a, const char *b) {
  return a != NULL && strcmp(a, b) == 0;
}
static int path_is(const lonejson_value_path *p, const char *key) {
  return p->segment_count == 1U && p->segments[0].len == strlen(key) &&
         memcmp(p->segments[0].data, key, strlen(key)) == 0;
}
static int action(const char *name) {
  if (same(name, "exec_command"))
    return CAI_AGENT_TOOL_ACTION_EXECUTE;
  if (same(name, "write_stdin"))
    return CAI_AGENT_TOOL_ACTION_WRITE_STDIN;
  if (same(name, "apply_patch"))
    return CAI_AGENT_TOOL_ACTION_PATCH;
  if (same(name, "read_file"))
    return CAI_AGENT_TOOL_ACTION_READ;
  if (same(name, "list_files"))
    return CAI_AGENT_TOOL_ACTION_LIST;
  if (same(name, "view_image"))
    return CAI_AGENT_TOOL_ACTION_VIEW;
  if (same(name, "chrome"))
    return CAI_AGENT_TOOL_ACTION_CHROME;
  if (same(name, "get_goal"))
    return CAI_AGENT_TOOL_ACTION_GET_GOAL;
  if (same(name, "create_goal"))
    return CAI_AGENT_TOOL_ACTION_CREATE_GOAL;
  if (same(name, "update_goal"))
    return CAI_AGENT_TOOL_ACTION_UPDATE_GOAL;
  if (same(name, "clear_goal"))
    return CAI_AGENT_TOOL_ACTION_CLEAR_GOAL;
  if (same(name, "run_review") || same(name, "run_worker"))
    return CAI_AGENT_TOOL_ACTION_SUBAGENT;
  if (name != NULL && strncmp(name, "mcp_", 4U) == 0)
    return CAI_AGENT_TOOL_ACTION_MCP;
  return CAI_AGENT_TOOL_ACTION_EXTERNAL;
}
static void parse_close(replay_call *c) {
  lonejson_curl_parse_cleanup(&c->parser);
  lonejson_cleanup(&value_map, &c->value);
  memset(&c->parser, 0, sizeof(c->parser));
  memset(&c->value, 0, sizeof(c->value));
  c->parsing = 0;
}
static void call_free(replay_call *c) {
  parse_close(c);
  free(c->id);
  free(c->session);
  free(c->name);
  free(c->label);
  free(c);
}
void cai_cli_replay_tools_close(cai_cli_replay_tools *r) {
  replay_call *c;
  if (r == NULL)
    return;
  while ((c = r->calls) != NULL) {
    r->calls = c->next;
    call_free(c);
  }
  lonejson_free(r->json);
  free(r);
}
static replay_call *find(cai_cli_replay_tools *r,
                         const cai_execution_record *record) {
  replay_call *c;
  for (c = r->calls; c != NULL; c = c->next)
    if (same(c->session, record->session_id) && same(c->id, record->call_id))
      return c;
  return NULL;
}
static void append(char *dst, size_t *used, size_t capacity, const char *bytes,
                   size_t count) {
  size_t n = capacity - 1U - *used;
  if (n > count)
    n = count;
  if (n != 0U)
    memcpy(dst + *used, bytes, n);
  *used += n;
  dst[*used] = '\0';
}
static lonejson_status string_chunk(void *context,
                                    const lonejson_value_path *path,
                                    const char *bytes, size_t count,
                                    lonejson_error *error) {
  replay_call *c = context;
  (void)error;
  if (c->rendering) {
    if (c->output && path_is(path, "output")) {
      c->event.type = CAI_AGENT_EVENT_TERMINAL_OUTPUT;
      c->event.data = bytes;
      c->event.data_length = count;
      c->rc = cai_tool_display_event(c->display, &c->event, c->error);
      if (c->rc != CAI_OK)
        return LONEJSON_STATUS_IO_ERROR;
    }
  } else if (!c->output && (path_is(path, "cmd") || path_is(path, "path") ||
                            path_is(path, "action"))) {
    size_t n = count;
    char *label;
    if (n > 8192U - c->label_length)
      n = 8192U - c->label_length;
    if (n != 0U) {
      label = realloc(c->label, c->label_length + n + 1U);
      if (label == NULL) {
        c->rc = CAI_ERR_NOMEM;
        return LONEJSON_STATUS_ALLOCATION_FAILED;
      }
      c->label = label;
      memcpy(c->label + c->label_length, bytes, n);
      c->label_length += n;
      c->label[c->label_length] = '\0';
    }
  } else if (path_is(path, "session_id")) {
    append(c->terminal, &c->terminal_length, sizeof(c->terminal), bytes, count);
  }
  return LONEJSON_STATUS_OK;
}
static lonejson_status number_chunk(void *context,
                                    const lonejson_value_path *path,
                                    const char *bytes, size_t count,
                                    lonejson_error *error) {
  replay_call *c = context;
  (void)error;
  if (!c->rendering && c->output &&
      (path_is(path, "exit_code") || path_is(path, "signal") ||
       path_is(path, "command_id") || path_is(path, "duration_ms")))
    append(c->number, &c->number_length, sizeof(c->number), bytes, count);
  return LONEJSON_STATUS_OK;
}
static lonejson_status number_end(void *context,
                                  const lonejson_value_path *path,
                                  lonejson_error *error) {
  replay_call *c = context;
  (void)error;
  if (c->number_length != 0U) {
    if (path_is(path, "exit_code")) {
      c->exit_code = strtoll(c->number, NULL, 10);
      c->has_exit = 1;
    } else if (path_is(path, "signal")) {
      c->signal = strtoll(c->number, NULL, 10);
      c->has_signal = 1;
    } else if (path_is(path, "command_id")) {
      c->command_id = strtoull(c->number, NULL, 10);
    } else if (path_is(path, "duration_ms")) {
      c->duration_ms = strtoull(c->number, NULL, 10);
    }
    c->number_length = 0U;
  }
  return LONEJSON_STATUS_OK;
}
static lonejson_status bool_value(void *context,
                                  const lonejson_value_path *path, int value,
                                  lonejson_error *error) {
  replay_call *c = context;
  (void)error;
  if (!c->rendering && c->output) {
    if (path_is(path, "output_truncated"))
      c->truncated = value;
    if (path_is(path, "isError"))
      c->is_error = value;
  }
  return LONEJSON_STATUS_OK;
}
static int parse_feed(cai_cli_replay_tools *r, replay_call *c,
                      const cai_execution_record *record, int output) {
  static char prefix[] = "{\"value\":";
  char suffix[] = "}";
  char chunk[CAI_EXECUTION_CHUNK_BYTES + 1U];
  lonejson_error local;
  size_t n = strlen(record->data);
  if (record->offset == 0U) {
    parse_close(c);
    c->invalid = 0;
    c->offset = 0U;
    c->output = output;
    c->visitor = lonejson_default_path_value_visitor();
    c->visitor.string_chunk = string_chunk;
    c->visitor.number_chunk = number_chunk;
    c->visitor.number_end = number_end;
    c->visitor.boolean_value = bool_value;
    lonejson_error_init(&local);
    lonejson_json_value_init(r->json, &c->value.value);
    if (lonejson_json_value_set_parse_path_visitor(
            &c->value.value, &c->visitor, c, &local) != LONEJSON_STATUS_OK ||
        lonejson_curl_parse_init(&c->parser, r->json, &value_map, &c->value) !=
            LONEJSON_STATUS_OK)
      c->invalid = 1;
    c->parsing = 1;
    if (!c->invalid &&
        lonejson_curl_write_callback(prefix, 1U, sizeof(prefix) - 1U,
                                     &c->parser) != sizeof(prefix) - 1U)
      c->invalid = 1;
  }
  if (!c->parsing || record->offset != c->offset)
    c->invalid = 1;
  c->offset = record->offset + n;
  memcpy(chunk, record->data, n);
  if (!c->invalid &&
      lonejson_curl_write_callback(chunk, 1U, n, &c->parser) != n)
    c->invalid = 1;
  if (record->final) {
    if (!c->invalid &&
        (lonejson_curl_write_callback(suffix, 1U, 1U, &c->parser) != 1U ||
         lonejson_curl_parse_finish(&c->parser) != LONEJSON_STATUS_OK))
      c->invalid = 1;
    parse_close(c);
  }
  return c->invalid ? CAI_ERR_PROTOCOL : CAI_OK;
}
int cai_cli_replay_tools_index(cai_cli_replay_tools **out,
                               const cai_execution_record *record,
                               unsigned long long sequence,
                               unsigned long long first, cai_error *error) {
  cai_cli_replay_tools *r = *out;
  replay_call *c, *other, **link;
  lonejson_config config;
  lonejson_error local;
  if (record->call_id == NULL || record->tool_name == NULL ||
      record->session_id == NULL)
    return CAI_OK;
  if (r == NULL) {
    r = calloc(1U, sizeof(*r));
    if (r == NULL)
      return cai_cli_error(error, CAI_ERR_NOMEM, "index replay tools");
    config = lonejson_default_config();
    config.clear_destination_by_default = 0;
    config.json_value_max_total_bytes = 0U;
    config.json_value_max_string_bytes = 0U;
    lonejson_error_init(&local);
    r->json = lonejson_new(&config, &local);
    if (r->json == NULL) {
      free(r);
      return cai_cli_error(error, CAI_ERR_NOMEM, "open replay decoder");
    }
    *out = r;
  }
  for (link = &r->calls; (c = *link) != NULL;) {
    if (first != 0U && c->last < first && !c->parsing) {
      *link = c->next;
      call_free(c);
    } else
      link = &c->next;
  }
  c = find(r, record);
  if (c == NULL) {
    c = calloc(1U, sizeof(*c));
    if (c != NULL) {
      c->session = strdup(record->session_id);
      c->id = strdup(record->call_id);
      c->name = strdup(record->tool_name);
    }
    if (c == NULL || c->session == NULL || c->id == NULL || c->name == NULL) {
      if (c != NULL)
        call_free(c);
      return cai_cli_error(error, CAI_ERR_NOMEM, "index replay call");
    }
    c->action = action(c->name);
    c->next = r->calls;
    r->calls = c;
  }
  c->last = sequence;
  if (same(record->kind, "tool.completed"))
    c->completed = 1;
  if (same(record->kind, "tool.failed"))
    c->failed = 1;
  if (same(record->kind, "tool.patch"))
    c->has_preview = 1;
  if (same(record->kind, "tool.arguments") &&
      c->action != CAI_AGENT_TOOL_ACTION_PATCH)
    (void)parse_feed(r, c, record, 0);
  if ((same(record->kind, "tool.output") ||
       same(record->kind, "tool.partial_output")) &&
      (c->action == CAI_AGENT_TOOL_ACTION_EXECUTE ||
       c->action == CAI_AGENT_TOOL_ACTION_WRITE_STDIN ||
       c->action == CAI_AGENT_TOOL_ACTION_MCP)) {
    if (record->offset == 0U)
      c->terminal_length = 0U;
    if (parse_feed(r, c, record, 1) != CAI_OK)
      c->bad_output = 1;
    if (record->final && c->action == CAI_AGENT_TOOL_ACTION_EXECUTE)
      c->origin = c;
    if (record->final && c->action == CAI_AGENT_TOOL_ACTION_WRITE_STDIN &&
        c->terminal_length != 0U)
      for (other = r->calls; other != NULL; other = other->next)
        if (other != c && other->action == CAI_AGENT_TOOL_ACTION_EXECUTE &&
            same(other->session, c->session) &&
            strcmp(other->terminal, c->terminal) == 0 &&
            other->command_id == c->command_id) {
          c->origin = other;
          break;
        }
    if (record->final && c->action != CAI_AGENT_TOOL_ACTION_MCP) {
      replay_call *origin = c->origin != NULL ? c->origin : c;
      origin->last_output = c;
      origin->last = c->last;
      origin->duration_ms = c->duration_ms;
      if (c->has_exit) {
        origin->has_exit = 1;
        origin->exit_code = c->exit_code;
      }
      if (c->has_signal) {
        origin->has_signal = 1;
        origin->signal = c->signal;
      }
      origin->truncated |= c->truncated;
    }
  }
  if (c->rc != CAI_OK)
    return cai_cli_error(error, c->rc, "decode replay tool metadata");
  return CAI_OK;
}
static void event_init(replay_call *c) {
  memset(&c->event, 0, sizeof(c->event));
  c->event.type = c->failed ? CAI_AGENT_EVENT_TOOL_CALL_FAILED
                            : CAI_AGENT_EVENT_TOOL_CALL_COMPLETED;
  c->event.runtime_session_id = c->session;
  c->event.tool_call_id = c->id;
  c->event.tool_name = c->name;
  c->event.tool_action = c->action;
  c->event.tool_path = c->label_length != 0U ? c->label : NULL;
  c->event.terminal_has_exit_code = c->has_exit;
  c->event.terminal_exit_code = c->exit_code;
  c->event.terminal_has_signal = c->has_signal;
  c->event.terminal_signal = c->signal;
  c->event.terminal_output_truncated = c->truncated;
}
int cai_cli_replay_tools_render(cai_cli_replay_tools *r,
                                const cai_execution_record *record,
                                cai_tool_display *display,
                                int (*markdown)(void *, const char *, size_t),
                                void *context, int *handled, cai_error *error) {
  replay_call *c, *origin;
  cai_cli_tool_result *result = NULL;
  const char *verb, *label;
  int rc = CAI_OK, command, success;
  if (same(record->kind, "reasoning.receipt")) {
    cai_agent_runtime_event event;
    memset(&event, 0, sizeof(event));
    event.type = CAI_AGENT_EVENT_REASONING_TOOL_RESULT;
    event.runtime_session_id = record->session_id;
    event.tool_call_id = record->call_id;
    event.data = record->data;
    event.data_length = strlen(record->data);
    *handled = 1;
    return cai_tool_display_event(display, &event, error);
  }
  *handled = record->kind != NULL && (strncmp(record->kind, "tool.", 5U) == 0 ||
                                      same(record->kind, "hosted.tool"));
  if (!*handled || r == NULL || (c = find(r, record)) == NULL)
    return CAI_OK;
  c->display = display;
  c->error = error;
  c->rc = CAI_OK;
  event_init(c);
  if (same(c->name, "set_reasoning") && !c->failed)
    return CAI_OK;
  /* The runner's failure message is a diagnostic. Only its captured output
   * value contains raw subprocess bytes, including partial output on failure.
   */
  command = (c->action == CAI_AGENT_TOOL_ACTION_EXECUTE ||
             c->action == CAI_AGENT_TOOL_ACTION_WRITE_STDIN) &&
            !same(record->kind, "tool.failed");
  origin = command && c->origin != NULL ? c->origin : c;
  if (command) {
    event_init(origin);
    c->event = origin->event;
    origin->display = display;
    if ((same(record->kind, "tool.output") ||
         same(record->kind, "tool.partial_output")) &&
        !c->replayed) {
      if (c->parsing && c->output)
        c->bad_output = 1;
      c->replayed = 1;
    }
  }
  if (command &&
      (same(record->kind, "tool.output") ||
       same(record->kind, "tool.partial_output")) &&
      c->bad_output) {
    if (record->offset != 0U)
      return CAI_OK;
    label = origin->label_length != 0U ? origin->label : c->name;
    c->event.type = CAI_AGENT_EVENT_TOOL_CALL_FAILED;
    c->event.data = "Saved command output is incomplete or invalid. See the "
                    "JSONL export.\n";
    c->event.data_length = strlen(c->event.data);
    rc = cai_cli_tool_result_open(&c->event, &result, error);
    if (rc == CAI_OK)
      rc = cai_tool_display_summary(display, &c->event, "Failed to replay",
                                    label, 0, error);
    if (rc == CAI_OK)
      rc = cai_cli_tool_result_write(result, markdown, context, error);
    cai_cli_tool_result_close(result);
    return rc;
  }
  if (same(record->kind, "tool.patch_locations") ||
      same(record->kind, "tool.patch")) {
    if (!c->patch) {
      c->event.type = CAI_AGENT_EVENT_TOOL_CALL_STARTED;
      rc = cai_tool_display_event(display, &c->event, error);
      c->patch = 1;
    }
    c->event.type = same(record->kind, "tool.patch")
                        ? CAI_AGENT_EVENT_TOOL_PATCH
                        : CAI_AGENT_EVENT_TOOL_PATCH_LOCATIONS;
    c->event.data = record->data;
    c->event.data_length = strlen(record->data);
    if (rc == CAI_OK && c->event.data_length != 0U)
      rc = cai_tool_display_event(display, &c->event, error);
    if (rc == CAI_OK && record->final) {
      c->event.data = NULL;
      c->event.data_length = 0U;
      rc = cai_tool_display_event(display, &c->event, error);
    }
    return rc;
  }
  if (same(record->kind, "tool.arguments") &&
      c->action == CAI_AGENT_TOOL_ACTION_PATCH && c->completed && !c->failed &&
      !c->has_preview) {
    if (record->offset == 0U) {
      c->event.type = CAI_AGENT_EVENT_TOOL_CALL_STARTED;
      rc = cai_tool_display_event(display, &c->event, error);
      c->patch = 1;
    }
    c->event.type = CAI_AGENT_EVENT_TOOL_PATCH;
    c->event.data = record->data;
    c->event.data_length = strlen(record->data);
    if (rc == CAI_OK && c->event.data_length != 0U)
      rc = cai_tool_display_event(display, &c->event, error);
    if (rc == CAI_OK && record->final) {
      c->event.data = NULL;
      c->event.data_length = 0U;
      rc = cai_tool_display_event(display, &c->event, error);
    }
    return rc;
  }
  if (same(record->kind, "tool.completed") ||
      same(record->kind, "hosted.tool")) {
    if (same(record->kind, "hosted.tool")) {
      if (record->offset != 0U)
        return CAI_OK;
      return cai_tool_display_summary(
          display, &c->event, "Used",
          same(c->name, "web_search_call") ? "web search" : c->name, 1, error);
    }
    if (c->action != CAI_AGENT_TOOL_ACTION_EXTERNAL &&
        c->action != CAI_AGENT_TOOL_ACTION_MCP &&
        c->action != CAI_AGENT_TOOL_ACTION_PATCH && !command)
      return cai_tool_display_event(display, &c->event, error);
    return CAI_OK;
  }
  if (!(same(record->kind, "tool.output") ||
        same(record->kind, "tool.partial_output") ||
        (same(record->kind, "tool.failed") && same(record->field, "message"))))
    return CAI_OK;
  if (c->patch || c->has_preview ||
      (!c->failed && (c->action != CAI_AGENT_TOOL_ACTION_EXTERNAL &&
                      c->action != CAI_AGENT_TOOL_ACTION_MCP && !command)))
    return CAI_OK;
  c->event.data = record->data;
  c->event.data_length = strlen(record->data);
  if (!command && record->offset == 0U && record->final) {
    rc = cai_cli_tool_result_open(&c->event, &result, error);
    if (rc == CAI_OK && result != NULL) {
      rc = cai_cli_tool_result_heading(result, display, &c->event, error);
      if (rc == CAI_OK)
        rc = cai_cli_tool_result_write(result, markdown, context, error);
      cai_cli_tool_result_close(result);
      return rc;
    }
    if (rc != CAI_OK)
      return rc;
  }
  if (!command) {
    if (!origin->opened) {
      verb = same(record->kind, "tool.partial_output") ? "Partial result from"
             : c->failed || c->is_error
                 ? (c->action == CAI_AGENT_TOOL_ACTION_MCP ? "MCP failed"
                                                           : "Failed")
             : c->action == CAI_AGENT_TOOL_ACTION_MCP ? "MCP"
                                                      : "Completed";
      label = c->name;
      rc = cai_tool_display_summary(display, &c->event, verb, label,
                                    !c->failed && !c->is_error, error);
      origin->opened = rc == CAI_OK;
      memset(&origin->markdown, 0, sizeof(origin->markdown));
    }
    if (rc == CAI_OK)
      rc = cai_cli_tool_text_write(
          &origin->markdown, record->data, strlen(record->data), record->final,
          c->failed || c->is_error, markdown, context, error);
    if (record->final)
      origin->opened = 0;
    return rc;
  }
  if (record->offset == 0U && !origin->opened) {
    success = !c->failed && !c->is_error &&
              (!command || ((!origin->has_exit || origin->exit_code == 0) &&
                            (!origin->has_signal || origin->signal == 0)));
    verb =
        same(record->kind, "tool.partial_output") ? "Partial output from"
        : c->failed || c->is_error
            ? (c->action == CAI_AGENT_TOOL_ACTION_MCP ? "MCP failed" : "Failed")
        : command                                ? "Ran"
        : c->action == CAI_AGENT_TOOL_ACTION_MCP ? "MCP"
                                                 : "Completed";
    label = origin->label_length != 0U ? origin->label : c->name;
    c->event.terminal_duration_ms = origin->duration_ms;
    rc = cai_tool_display_replay_begin(display, &c->event, verb, label, success,
                                       error);
    origin->opened = rc == CAI_OK;
  }
  if (rc == CAI_OK && command && !same(record->kind, "tool.failed")) {
    c->rendering = 1;
    rc = parse_feed(r, c, record, 1);
    c->rendering = 0;
    if (c->rc != CAI_OK)
      return c->rc;
  } else if (rc == CAI_OK) {
    c->event.type = CAI_AGENT_EVENT_TERMINAL_OUTPUT;
    rc = cai_tool_display_event(display, &c->event, error);
  }
  if (record->final && origin->opened &&
      (!command || origin->last_output == c)) {
    c->event.type = CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED;
    c->event.data = NULL;
    c->event.data_length = 0U;
    if (!command || c->failed || !origin->has_exit) {
      c->event.terminal_has_exit_code = 1;
      c->event.terminal_exit_code = 0;
      c->event.terminal_has_signal = 0;
    }
    if (rc == CAI_OK)
      rc = cai_tool_display_event(display, &c->event, error);
    origin->opened = 0;
  }
  return rc;
}
int cai_cli_replay_tools_finish(cai_cli_replay_tools *r, cai_error *error) {
  replay_call *c;
  int rc = CAI_OK;
  if (r == NULL)
    return CAI_OK;
  for (c = r->calls; c != NULL && rc == CAI_OK; c = c->next) {
    if (!c->opened)
      continue;
    event_init(c);
    c->event.type = CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED;
    if (!c->has_exit) {
      c->event.terminal_has_exit_code = 1;
      c->event.terminal_exit_code = 0;
    }
    rc = cai_tool_display_event(c->display, &c->event, error);
    c->opened = 0;
  }
  return rc;
}
