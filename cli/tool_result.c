#define _POSIX_C_SOURCE 200809L
#include "tool_result.h"
#include "pouch_stream.h"
#include <stdlib.h>
#include <string.h>

typedef struct result_row {
  char *id, *key, *name, *title, *status;
} result_row;
static const lonejson_field row_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(result_row, id, "id"),
    LONEJSON_FIELD_STRING_ALLOC(result_row, key, "key"),
    LONEJSON_FIELD_STRING_ALLOC(result_row, name, "name"),
    LONEJSON_FIELD_STRING_ALLOC(result_row, title, "title"),
    LONEJSON_FIELD_STRING_ALLOC(result_row, status, "status")};
LONEJSON_MAP_DEFINE(row_map, result_row, row_fields);
typedef struct result_doc {
  char *operation, *code, *message, *status, *recipient;
  char *board_name, *item_id, *title, *query, *url, *snippet, *label;
  int ok, has_ok, truncated;
  lonejson_object_array items, boards;
} result_doc;
static const lonejson_field result_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(result_doc, operation, "operation"),
    LONEJSON_FIELD_STRING_ALLOC(result_doc, code, "code"),
    LONEJSON_FIELD_STRING_ALLOC(result_doc, message, "message"),
    LONEJSON_FIELD_STRING_ALLOC(result_doc, status, "status"),
    LONEJSON_FIELD_STRING_ALLOC(result_doc, recipient, "recipient"),
    LONEJSON_FIELD_STRING_ALLOC(result_doc, board_name, "board_name"),
    LONEJSON_FIELD_STRING_ALLOC(result_doc, item_id, "item_id"),
    LONEJSON_FIELD_STRING_ALLOC(result_doc, title, "title"),
    LONEJSON_FIELD_STRING_ALLOC(result_doc, query, "query"),
    LONEJSON_FIELD_STRING_ALLOC(result_doc, url, "url"),
    LONEJSON_FIELD_STRING_ALLOC(result_doc, snippet, "snippet"),
    LONEJSON_FIELD_STRING_ALLOC(result_doc, label, "label"),
    LONEJSON_FIELD_BOOL_PRESENT(result_doc, ok, has_ok, "ok"),
    LONEJSON_FIELD_BOOL(result_doc, truncated, "truncated"),
    LONEJSON_FIELD_OBJECT_ARRAY(result_doc, items, "items", result_row,
                                &row_map, LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_OBJECT_ARRAY(result_doc, boards, "boards", result_row,
                                &row_map, LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(result_map, result_doc, result_fields);

struct cai_cli_tool_result {
  result_doc doc;
  const char *verb, *label, *markdown;
  size_t markdown_length;
  int success, failure;
};
static int same(const char *a, const char *b) {
  return a != NULL && strcmp(a, b) == 0;
}
void cai_cli_tool_result_close(cai_cli_tool_result *r) {
  if (r != NULL) {
    lonejson_cleanup(&result_map, &r->doc);
    free(r);
  }
}
int cai_cli_tool_result_open(const cai_agent_runtime_event *e,
                             cai_cli_tool_result **out, cai_error *error) {
  static const char queue_heading[] = "## Message queues\n\n";
  cai_cli_tool_result *r;
  lonejson *json;
  lonejson_error local;
  const char *op;
  int recognized = 0;
  *out = NULL;
  if (e->type == CAI_AGENT_EVENT_TOOL_CALL_FAILED &&
      e->tool_action == CAI_AGENT_TOOL_ACTION_PATCH) {
    r = calloc(1U, sizeof(*r));
    if (r == NULL)
      return cai_cli_error(error, CAI_ERR_NOMEM, "prepare patch failure");
    r->verb = "Failed";
    r->label = e->tool_name != NULL ? e->tool_name : "apply_patch";
    r->failure = 1;
    r->markdown = e->data != NULL && e->data_length != 0U
                      ? e->data
                      : "Patch could not be applied; no details provided.";
    r->markdown_length = e->data != NULL && e->data_length != 0U
                             ? e->data_length
                             : strlen(r->markdown);
    *out = r;
    return CAI_OK;
  }
  if (e->type != CAI_AGENT_EVENT_TOOL_CALL_COMPLETED ||
      e->tool_action != CAI_AGENT_TOOL_ACTION_EXTERNAL || e->data == NULL ||
      (!same(e->tool_name, "set_reasoning") &&
       !same(e->tool_name, "agent_message") &&
       !same(e->tool_name, "todo_kanban") &&
       !same(e->tool_name, "searxng_search") &&
       !same(e->tool_name, "reverse_geocode")))
    return CAI_OK;
  r = calloc(1U, sizeof(*r));
  if (r == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "prepare tool presentation");
  r->success = 1;
  if (same(e->tool_name, "set_reasoning")) {
    if (e->data_length != 2U || memcmp(e->data, "ok", 2U) != 0)
      goto unrecognized;
    /* Acknowledged does not imply that the effective effort changed. */
    r->verb = "Accepted";
    r->label = "reasoning request";
  } else if (same(e->tool_name, "agent_message") &&
             e->data_length >= sizeof(queue_heading) - 1U &&
             memcmp(e->data, queue_heading, sizeof(queue_heading) - 1U) == 0) {
    r->verb = "Listed";
    r->label = "message queues";
    r->markdown = e->data;
    r->markdown_length = e->data_length;
  } else {
    lonejson_error_init(&local);
    json = lonejson_new(NULL, &local);
    if (json == NULL) {
      cai_cli_tool_result_close(r);
      return cai_cli_error(error, CAI_ERR_NOMEM, "allocate tool result parser");
    }
    recognized =
        lonejson_parse_buffer(json, &result_map, &r->doc, e->data,
                              e->data_length, &local) == LONEJSON_STATUS_OK;
    lonejson_free(json);
    if (!recognized)
      goto unrecognized;
    if (same(e->tool_name, "agent_message")) {
      if (!same(r->doc.status, "delivered") || r->doc.recipient == NULL ||
          r->doc.recipient[0] == '\0')
        goto unrecognized;
      r->verb = "Delivered message to";
      r->label = r->doc.recipient;
    } else if (same(e->tool_name, "todo_kanban")) {
      op = r->doc.operation;
      if (!r->doc.has_ok || op == NULL || r->doc.code == NULL)
        goto unrecognized;
      r->success = r->doc.ok;
      r->verb = !r->success                 ? "Kanban declined"
                : same(op, "add_item")      ? "Added task"
                : same(op, "move_item")     ? "Moved task"
                : same(op, "complete_item") ? "Completed task"
                : same(op, "create_board")  ? "Created board"
                : same(op, "select_board")  ? "Selected board"
                : same(op, "set_wip_limit") ? "Updated board limit"
                : same(op, "get_item")      ? "Read task"
                : same(op, "current_board") ? "Read board"
                : same(op, "help")          ? "Read"
                                            : "Listed";
      r->label = !r->success                 ? r->doc.code
                 : r->doc.item_id != NULL    ? r->doc.item_id
                 : r->doc.board_name != NULL ? r->doc.board_name
                                             : "kanban boards";
    } else if (same(e->tool_name, "searxng_search")) {
      if (r->doc.query == NULL || r->doc.title == NULL || r->doc.url == NULL)
        goto unrecognized;
      r->verb = "Searched";
      r->label = r->doc.query;
    } else {
      if (r->doc.label == NULL)
        goto unrecognized;
      r->verb = "Located";
      r->label = r->doc.label;
    }
  }
  *out = r;
  return CAI_OK;
unrecognized:
  cai_cli_tool_result_close(r);
  return CAI_OK;
}

int cai_cli_tool_result_heading(cai_cli_tool_result *r,
                                cai_tool_display *display,
                                const cai_agent_runtime_event *event,
                                cai_error *error) {
  return cai_tool_display_summary(display, event, r->verb, r->label, r->success,
                                  error);
}

/* Escape metadata as text even inside Markdown tables. Description Markdown
 * remains in /todo see; tool receipts are deliberately short summaries. */
static int literal(int (*write)(void *, const char *, size_t), void *context,
                   const char *text, size_t length, int escape) {
  const unsigned char *p = (const unsigned char *)(text != NULL ? text : "");
  const unsigned char *end = p + length;
  for (; p < end; p++) {
    if (*p < 32U || *p == 127U) {
      if (write(context, " ", 1U) != 0)
        return -1;
    } else if (*p == 0xc2U && p + 1U < end && p[1] >= 0x80U && p[1] <= 0x9fU) {
      if (write(context, " ", 1U) != 0)
        return -1;
      p++;
    } else {
      if (escape && strchr("\\|`*_[]<>!#~", *p) != NULL &&
          write(context, "\\", 1U) != 0)
        return -1;
      if (write(context, (const char *)p, 1U) != 0)
        return -1;
    }
  }
  return 0;
}
static int cell(int (*write)(void *, const char *, size_t), void *context,
                const char *text) {
  return literal(write, context, text, text != NULL ? strlen(text) : 0U, 1);
}

/* Quote diagnostics as prose with literal expected/actual code spans. Source
 * whitespace stays visible and excerpts cannot become Markdown markup.
 * libmdf owns wrapping/theme; logs/export retain the original event. */
static int failure(int (*write)(void *, const char *, size_t), void *context,
                   const char *text, size_t length) {
  size_t start = 0U, end, i, label, run, fence;
  while (start < length) {
    for (end = start; end < length && text[end] != '\n'; end++) {
    }
    label = end - start >= 9U && memcmp(text + start, "Expected:", 9U) == 0 ? 9U
            : end - start >= 7U && memcmp(text + start, "Actual:", 7U) == 0 ? 7U
            : end - start >= 23U &&
                    memcmp(text + start, "Expected starting line:", 23U) == 0
                ? 23U
                : 0U;
    if (write(context, "> ", 2U) != 0)
      return -1;
    if (label != 0U) {
      if (write(context, text + start, label) != 0 ||
          write(context, " ", 1U) != 0)
        return -1;
      start += label;
      while (start < end && (text[start] == ' ' || text[start] == '\t'))
        start++;
      run = 0U;
      fence = 1U;
      for (i = start; i < end; i++) {
        run = text[i] == '`' ? run + 1U : 0U;
        if (run >= fence)
          fence = run + 1U;
      }
      for (i = 0U; i < fence; i++)
        if (write(context, "`", 1U) != 0)
          return -1;
      if (write(context, " ", 1U) != 0 ||
          literal(write, context, text + start, end - start, 0) != 0 ||
          write(context, " ", 1U) != 0)
        return -1;
      for (i = 0U; i < fence; i++)
        if (write(context, "`", 1U) != 0)
          return -1;
    } else if (literal(write, context, text + start, end - start, 1) != 0)
      return -1;
    if (write(context, "  \n", 3U) != 0)
      return -1;
    start = end + 1U;
  }
  return write(context, "\n", 1U);
}
#define WRITE(s)                                                               \
  do {                                                                         \
    if (write(context, (s), strlen(s)) != 0)                                   \
      goto failed;                                                             \
  } while (0)
#define CELL(s)                                                                \
  do {                                                                         \
    if (cell(write, context, (s)) != 0)                                        \
      goto failed;                                                             \
  } while (0)
int cai_cli_tool_result_write(cai_cli_tool_result *r,
                              int (*write)(void *, const char *, size_t),
                              void *context, cai_error *error) {
  result_row *rows;
  size_t i, count;
  int boards;
  if (r->failure) {
    if (failure(write, context, r->markdown, r->markdown_length) != 0)
      goto failed;
    return CAI_OK;
  }
  if (r->markdown != NULL) {
    if (write(context, r->markdown, r->markdown_length) != 0)
      goto failed;
    return CAI_OK;
  }
  if (r->doc.title != NULL) {
    CELL(r->doc.title);
    if (r->doc.status != NULL) {
      WRITE(" — ");
      CELL(r->doc.status);
    }
    WRITE("\n\n");
  } else if (r->doc.message != NULL) {
    CELL(r->doc.message);
    WRITE("\n\n");
  }
  boards = same(r->doc.operation, "list_boards");
  rows = boards ? r->doc.boards.items : r->doc.items.items;
  count = boards ? r->doc.boards.count : r->doc.items.count;
  if (count != 0U) {
    WRITE(boards ? "| Key | Board |\n| --- | --- |\n"
                 : "| Task | Status | Title |\n| --- | --- | --- |\n");
    for (i = 0U; i < count; i++) {
      WRITE("| ");
      CELL(boards ? rows[i].key : rows[i].id);
      WRITE(" | ");
      CELL(boards ? rows[i].name : rows[i].status);
      if (!boards) {
        WRITE(" | ");
        CELL(rows[i].title);
      }
      WRITE(" |\n");
    }
  } else if (boards || same(r->doc.operation, "list_board") ||
             same(r->doc.operation, "snapshot_board") ||
             same(r->doc.operation, "current_work")) {
    WRITE(boards ? "No boards.\n" : "No tasks.\n");
  }
  if (r->doc.url != NULL) {
    CELL(r->doc.url);
    WRITE("\n\n");
  }
  if (r->doc.snippet != NULL) {
    CELL(r->doc.snippet);
    WRITE("\n");
  }
  if (r->doc.truncated)
    WRITE("\nResult limit reached; some entries are omitted.\n");
  return CAI_OK;
failed:
  return cai_cli_error(error, CAI_ERR_TRANSPORT, "render built-in tool result");
}
#undef WRITE
#undef CELL
