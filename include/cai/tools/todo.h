#ifndef CAI_TOOLS_TODO_H
#define CAI_TOOLS_TODO_H

#include <cai/cai.h>
#include <lonejson.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAI_TODO_DEFAULT_STORE_FILE "todo.json"
#define CAI_TODO_DEFAULT_LOCK_FILE "todo.lock"
#define CAI_TODO_DEFAULT_TOOL_NAME "todo_kanban"

typedef struct cai_todo_store_callbacks {
  int (*begin)(void *context, void **transaction, cai_error *error);
  int (*open_read)(void *context, void *transaction,
                   lonejson_reader_fn *reader, void **reader_context,
                   cai_error *error);
  void (*close_read)(void *context, void *transaction, void *reader_context);
  int (*open_write)(void *context, void *transaction, lonejson_sink_fn *sink,
                    void **sink_context, cai_error *error);
  int (*commit_write)(void *context, void *transaction, cai_error *error);
  int (*commit)(void *context, void *transaction, cai_error *error);
  void (*rollback)(void *context, void *transaction);
  void (*destroy)(void *context);
} cai_todo_store_callbacks;

typedef struct cai_todo_tool_config {
  const char *name;
  const char *description;
  const cai_todo_store_callbacks *store;
  void *store_context;
  const char *store_path;
  const char *lock_path;
  const char *default_board;
  size_t max_title_bytes;
  size_t max_description_bytes;
  size_t max_result_items;
} cai_todo_tool_config;

int cai_tool_registry_register_todo_tool(cai_tool_registry *registry,
                                         const cai_todo_tool_config *config,
                                         cai_error *error);
int cai_agent_register_todo_tool(cai_agent *agent,
                                 const cai_todo_tool_config *config,
                                 cai_error *error);

#ifdef __cplusplus
}
#endif

#endif
