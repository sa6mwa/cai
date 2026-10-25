#define _POSIX_C_SOURCE 200809L
#include "todo.h"
#include "../src/cai_utf8.h"
#include "pouch_stream.h"
#include <string.h>

typedef struct todo_request {
  const char *operation;
  const char *board_name;
} todo_request;
static const lonejson_field request_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(todo_request, operation, "operation"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(todo_request, board_name,
                                          "board_name")};
LONEJSON_MAP_DEFINE(request_map, todo_request, request_fields);
typedef struct todo_entry {
  char *id, *key, *name, *title, *status;
} todo_entry;
static const lonejson_field entry_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(todo_entry, id, "id"),
    LONEJSON_FIELD_STRING_ALLOC(todo_entry, key, "key"),
    LONEJSON_FIELD_STRING_ALLOC(todo_entry, name, "name"),
    LONEJSON_FIELD_STRING_ALLOC(todo_entry, title, "title"),
    LONEJSON_FIELD_STRING_ALLOC(todo_entry, status, "status")};
LONEJSON_MAP_DEFINE(entry_map, todo_entry, entry_fields);
typedef struct todo_view {
  int ok, truncated;
  char *message, *board_name;
  lonejson_object_array boards, items;
} todo_view;
static const lonejson_field view_fields[] = {
    LONEJSON_FIELD_BOOL(todo_view, ok, "ok"),
    LONEJSON_FIELD_BOOL(todo_view, truncated, "truncated"),
    LONEJSON_FIELD_STRING_ALLOC(todo_view, message, "message"),
    LONEJSON_FIELD_STRING_ALLOC(todo_view, board_name, "board_name"),
    LONEJSON_FIELD_OBJECT_ARRAY(todo_view, boards, "boards", todo_entry,
                                &entry_map, LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_OBJECT_ARRAY(todo_view, items, "items", todo_entry,
                                &entry_map, LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(view_map, todo_view, view_fields);
typedef struct todo_call {
  cai_todo *todo;
  todo_request request;
} todo_call;
static int produce(void *context, cai_sink *sink, cai_error *error) {
  todo_call *call = context;
  cai_source *source = NULL;
  int rc = cai_cli_mapped_source(&request_map, &call->request, &source, error);
  if (rc == CAI_OK)
    rc = cai_todo_call(call->todo, source, sink, error);
  cai_source_close(source);
  return rc;
}
static int cell(cai_cli_todo_write write, void *context, const char *text) {
  const char *p = text != NULL ? text : "";
  for (; *p != '\0'; p++) {
    if (*p == '\n' || *p == '\r' || *p == '\t') {
      if (write(context, " ", 1U) != 0)
        return -1;
    } else {
      if (strchr("\\|`*_[]<>!#~", *p) != NULL && write(context, "\\", 1U) != 0)
        return -1;
      if (write(context, p, 1U) != 0)
        return -1;
    }
  }
  return 0;
}

#define CARD_TITLE_CHARACTERS 28U
/* UTF-8 character limit, including the ellipsis. Never split a character or
 * pass terminal control bytes through a card title. The full title stays in
 * storage and in the list view. */
static void card_title(char out[CARD_TITLE_CHARACTERS * 4U + 1U],
                       const char *text) {
  const char *p = text != NULL && text[0] != '\0' ? text : "Untitled";
  size_t used = 0U, characters = 0U, last = 0U;
  while (*p != '\0' && characters < CARD_TITLE_CHARACTERS) {
    unsigned char ch = (unsigned char)*p;
    size_t n = ch >= 0xc2U && ch < 0xe0U    ? 2U
               : ch >= 0xe0U && ch < 0xf0U  ? 3U
               : ch >= 0xf0U && ch <= 0xf4U ? 4U
                                            : 1U;
    size_t j;
    char rune[4];
    for (j = 1U; j < n; j++)
      if (p[j] == '\0') {
        n = j;
        break;
      }
    memcpy(rune, p, n);
    cai_utf8_repair_text(rune, n);
    if (memcmp(rune, p, n) != 0) {
      n = 1U;
      rune[0] = '?';
    }
    last = used;
    if (ch < 32U || ch == 127U ||
        (ch == 0xc2U && n == 2U && (unsigned char)p[1] >= 0x80U &&
         (unsigned char)p[1] <= 0x9fU))
      out[used++] = ' ';
    else {
      memcpy(out + used, rune, n);
      used += n;
    }
    p += n;
    characters++;
  }
  if (*p != '\0') {
    used = last;
    memcpy(out + used, "\342\200\246", 3U);
    used += 3U;
  }
  out[used] = '\0';
}
static size_t lane(const todo_entry *entry) {
  if (entry->status != NULL && strcmp(entry->status, "todo") == 0)
    return 0U;
  if (entry->status != NULL && strcmp(entry->status, "in_process") == 0)
    return 1U;
  if (entry->status != NULL && strcmp(entry->status, "done") == 0)
    return 2U;
  return 3U;
}
static int board_view(const todo_view *view, cai_cli_todo_write write,
                      void *context) {
  size_t columns = 3U, cursor[4] = {0U, 0U, 0U, 0U};
  size_t i, c, phase;
  int first = 1;
  const todo_entry *items = view->items.items;
  const char *header;
  for (i = 0U; i < view->items.count; i++)
    if (lane(&items[i]) == 3U)
      columns = 4U;
  header = columns == 3U
               ? "| TODO | DOING | DONE |\n| --- | --- | --- |\n"
               : "| TODO | DOING | DONE | OTHER |\n| --- | --- | --- | --- |\n";
  if (write(context, header, strlen(header)) != 0)
    return CAI_ERR_TRANSPORT;
  for (;;) {
    const todo_entry *row[4] = {NULL, NULL, NULL, NULL};
    int any = 0;
    for (c = 0U; c < columns; c++) {
      for (i = cursor[c]; i < view->items.count; i++)
        if (lane(&items[i]) == c) {
          row[c] = &items[i];
          cursor[c] = i + 1U;
          any = 1;
          break;
        }
      if (row[c] == NULL)
        cursor[c] = view->items.count;
    }
    if (!any)
      break;
    if (!first) {
      for (c = 0U; c < columns; c++)
        if (write(context, "| ", 2U) != 0)
          return CAI_ERR_TRANSPORT;
      if (write(context, "|\n", 2U) != 0)
        return CAI_ERR_TRANSPORT;
    }
    first = 0;
    for (phase = 0U; phase < 2U; phase++) {
      for (c = 0U; c < columns; c++) {
        char title[CARD_TITLE_CHARACTERS * 4U + 1U];
        const char *style = phase == 0U ? "**" : "*";
        if (write(context, "| ", 2U) != 0)
          return CAI_ERR_TRANSPORT;
        if (row[c] != NULL) {
          const char *value = row[c]->id != NULL ? row[c]->id : "?";
          if (phase != 0U) {
            card_title(title, row[c]->title);
            value = title;
          }
          if (write(context, style, strlen(style)) != 0 ||
              cell(write, context, value) != 0 ||
              write(context, style, strlen(style)) != 0)
            return CAI_ERR_TRANSPORT;
        }
        if (write(context, " ", 1U) != 0)
          return CAI_ERR_TRANSPORT;
      }
      if (write(context, "|\n", 2U) != 0)
        return CAI_ERR_TRANSPORT;
    }
  }
  if (first) {
    if (write(context, "|  |  |  |\n\nNo cards.\n",
              sizeof("|  |  |  |\n\nNo cards.\n") - 1U) != 0)
      return CAI_ERR_TRANSPORT;
  }
  return CAI_OK;
}
int cai_cli_todo_status(cai_agent_runtime *runtime, cai_cli_todo_write write,
                        void *context, cai_error *error) {
  char name[CAI_TODO_BOARD_NAME_MAX + 1U];
  cai_todo *todo = cai_agent_runtime_todo(runtime);
  int rc = todo != NULL ? cai_todo_get_board(todo, name, sizeof(name), error)
                        : CAI_OK;
  if (rc != CAI_OK)
    return rc;
  if (write(context, "| Kanban board | ", 17U) != 0 ||
      cell(write, context, todo != NULL ? name : "disabled") != 0 ||
      write(context, " |\n", 3U) != 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "display kanban status");
  return CAI_OK;
}
int cai_cli_todo_command(cai_agent_runtime *runtime, const char *arguments,
                         const char **layout, cai_cli_todo_write write,
                         void *context, cai_error *error) {
  todo_call call;
  todo_view view;
  cai_source *source = NULL;
  lonejson *json;
  lonejson_error local;
  const char *name;
  size_t i, count;
  int list = 0, changed = 0, rc = CAI_OK;
  if (layout == NULL || write == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "kanban layout and output are required");
  memset(&call, 0, sizeof(call));
  memset(&view, 0, sizeof(view));
  while (*arguments == ' ' || *arguments == '\t')
    arguments++;
  if (strcmp(arguments, "help") == 0) {
    static const char *const help[] = {
        "## Kanban\n\n| Command | Action |\n| --- | --- |\n"
        "| `/todo` | Show this conversation's selected board. |\n"
        "| `/todo list` | List boards shared on this backend. |\n"
        "| `/todo layout board` | Group cards by column (default). Bold ID, "
        "then an italic title capped at 28 characters. |\n"
        "| `/todo layout list` | Show item/status/title rows with full titles. "
        "|\n",
        "| `/todo use NAME` | Select an existing board between turns. |\n"
        "| `/todo new NAME` | Create or select a named board between turns. |\n"
        "\nSelection is saved with this conversation; other conversations keep "
        "their selection. Layout changes apply to future output in this "
        "invocation.\n"};
    for (i = 0U; i < sizeof(help) / sizeof(help[0]); i++)
      if (write(context, help[i], strlen(help[i])) != 0)
        return cai_cli_error(error, CAI_ERR_TRANSPORT, "display kanban help");
    return CAI_OK;
  }
  if (strcmp(arguments, "layout") == 0) {
    const char *value = *layout != NULL ? *layout : "board";
    if (write(context, "**Kanban layout:** `", 20U) != 0 ||
        write(context, value, strlen(value)) != 0 ||
        write(context, "`. Use `/todo layout board|list`.\n",
              sizeof("`. Use `/todo layout board|list`.\n") - 1U) != 0)
      return cai_cli_error(error, CAI_ERR_TRANSPORT, "display kanban layout");
    return CAI_OK;
  }
  if (strncmp(arguments, "layout ", 7U) == 0 ||
      strncmp(arguments, "layout\t", 7U) == 0) {
    size_t length;
    const char *value = arguments + 7U;
    while (*value == ' ' || *value == '\t')
      value++;
    length = strlen(value);
    while (length > 0U &&
           (value[length - 1U] == ' ' || value[length - 1U] == '\t'))
      length--;
    if (length == 4U && strncmp(value, "list", 4U) == 0)
      *layout = "list";
    else if (length == 5U && strncmp(value, "board", 5U) == 0)
      *layout = "board";
    else
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "Usage: /todo layout board|list");
    changed = 1;
    arguments = "";
  }
  call.todo = cai_agent_runtime_todo(runtime);
  if (changed) {
    if (write(context, "**Kanban layout:** `", 20U) != 0 ||
        write(context, *layout, strlen(*layout)) != 0 ||
        write(context, "`.\n\n", 4U) != 0)
      return cai_cli_error(error, CAI_ERR_TRANSPORT, "display kanban layout");
    if (call.todo == NULL)
      return CAI_OK;
  }
  if (call.todo == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "Kanban is disabled; enable --todo.");
  if (strncmp(arguments, "use ", 4U) == 0 ||
      strncmp(arguments, "new ", 4U) == 0) {
    name = arguments + 4U;
    while (*name == ' ' || *name == '\t')
      name++;
    rc = cai_agent_runtime_select_board(runtime, name, arguments[0] == 'n',
                                        error);
    if (rc != CAI_OK)
      return rc;
  } else if (strcmp(arguments, "list") == 0)
    list = 1;
  else if (*arguments != '\0')
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "Usage: /todo [list|use NAME|new NAME|layout board|list|help]");
  call.request.operation = list ? "list_boards" : "snapshot_board";
  json = lonejson_new(NULL, &local);
  if (json == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate kanban view parser");
  rc = cai_cli_stream_source(produce, &call, NULL, &source, error);
  if (rc == CAI_OK &&
      lonejson_parse_reader(json, &view_map, &view, cai_cli_json_read, source,
                            &local) != LONEJSON_STATUS_OK)
    rc = cai_cli_lj_error(error, &local);
  cai_source_close(source);
  if (rc == CAI_OK && !view.ok)
    rc = cai_cli_error(error, CAI_ERR_INVALID, view.message);
  if (rc == CAI_OK) {
    const char *heading = list ? "## Kanban boards\n\n" : "## Kanban: ";
    if (write(context, heading, strlen(heading)) != 0 ||
        (!list && (cell(write, context, view.board_name) != 0 ||
                   write(context, "\n\n", 2U) != 0)))
      rc = CAI_ERR_TRANSPORT;
  }
  if (rc == CAI_OK && !list &&
      (*layout == NULL || strcmp(*layout, "board") == 0))
    rc = board_view(&view, write, context);
  else if (rc == CAI_OK) {
    const char *header =
        list ? "| Key | Name |\n| --- | --- |\n"
             : "| Item | Status | Title |\n| --- | --- | --- |\n";
    if (write(context, header, strlen(header)) != 0)
      rc = CAI_ERR_TRANSPORT;
    count = list ? view.boards.count : view.items.count;
    for (i = 0U; rc == CAI_OK && i < count; i++) {
      todo_entry *entry =
          (todo_entry *)(list ? view.boards.items : view.items.items) + i;
      if (write(context, "| ", 2U) != 0 ||
          cell(write, context, list ? entry->key : entry->id) != 0 ||
          write(context, " | ", 3U) != 0 ||
          cell(write, context, list ? entry->name : entry->status) != 0 ||
          (!list && (write(context, " | ", 3U) != 0 ||
                     cell(write, context, entry->title) != 0)) ||
          write(context, " |\n", 3U) != 0)
        rc = CAI_ERR_TRANSPORT;
    }
  }
  if (rc == CAI_OK && view.truncated) {
    const char *note = "\nResult limit reached; some entries are omitted.\n";
    if (write(context, note, strlen(note)) != 0)
      rc = CAI_ERR_TRANSPORT;
  }
  lonejson_cleanup(&view_map, &view);
  lonejson_free(json);
  if (rc == CAI_ERR_TRANSPORT && (error == NULL || error->code == CAI_OK))
    cai_cli_error(error, rc, "display kanban view");
  return rc;
}
