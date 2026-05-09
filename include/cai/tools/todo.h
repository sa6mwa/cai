#ifndef CAI_TOOLS_TODO_H
#define CAI_TOOLS_TODO_H

#include <cai/cai.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAI_TODO_DEFAULT_ACTIVE_FILE "todo-active.json"
#define CAI_TODO_DEFAULT_DONE_FILE "todo-done.json"
#define CAI_TODO_DEFAULT_LOCK_FILE "todo.lock"
#define CAI_TODO_DEFAULT_TOOL_NAME "todo_kanban"

typedef struct cai_todo_tool_config {
  const char *name;
  const char *description;
  const char *active_path;
  const char *done_path;
  const char *lock_path;
  const char *default_board;
  int create_default_board;
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
