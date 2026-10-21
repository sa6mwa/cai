#define _POSIX_C_SOURCE 200809L
#include "todo.h"
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
                         cai_cli_todo_write write, void *context,
                         cai_error *error) {
  todo_call call;
  todo_view view;
  cai_source *source = NULL;
  lonejson *json;
  lonejson_error local;
  const char *name;
  size_t i, count;
  int list = 0, rc = CAI_OK;
  memset(&call, 0, sizeof(call));
  memset(&view, 0, sizeof(view));
  while (*arguments == ' ' || *arguments == '\t')
    arguments++;
  if (strcmp(arguments, "help") == 0) {
    const char *help =
        "## Kanban\n\n| Command | Action |\n| --- | --- |\n"
        "| `/todo` | Show this conversation's selected board. |\n"
        "| `/todo list` | List boards shared on this backend. |\n"
        "| `/todo use NAME` | Select an existing board between turns. |\n"
        "| `/todo new NAME` | Create or select a named board between turns. |\n"
        "\nSelection is saved with this conversation; other conversations keep "
        "their selection.\n";
    return write(context, help, strlen(help)) == 0 ? CAI_OK : CAI_ERR_TRANSPORT;
  }
  call.todo = cai_agent_runtime_todo(runtime);
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
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "Usage: /todo [list|use NAME|new NAME|help]");
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
  if (rc == CAI_OK) {
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
    if (rc == CAI_OK && view.truncated) {
      const char *note = "\nResult limit reached; some entries are omitted.\n";
      if (write(context, note, strlen(note)) != 0)
        rc = CAI_ERR_TRANSPORT;
    }
  }
  lonejson_cleanup(&view_map, &view);
  lonejson_free(json);
  if (rc == CAI_ERR_TRANSPORT && error->code == CAI_OK)
    cai_cli_error(error, rc, "display kanban view");
  return rc;
}
