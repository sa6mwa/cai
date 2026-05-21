#ifndef CAI_TOOLS_TODO_H
#define CAI_TOOLS_TODO_H

#include <cai/cai.h>
#include <lonejson.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Default todo storage filename under the cai config directory. */
#define CAI_TODO_DEFAULT_STORE_FILE "todo.json"
/** Default lock filename used by the file-backed todo store. */
#define CAI_TODO_DEFAULT_LOCK_FILE "todo.lock"
/** Default function tool name for the kanban todo preset. */
#define CAI_TODO_DEFAULT_TOOL_NAME "todo_kanban"

/** Streaming storage callbacks for the kanban todo preset. */
typedef struct cai_todo_store_callbacks {
  /** Begin a storage transaction and return an opaque transaction. */
  int (*begin)(void *context, void **transaction, cai_error *error);
  /** Open the current JSON document as a lonejson reader. */
  int (*open_read)(void *context, void *transaction,
                   lonejson_reader_fn *reader, void **reader_context,
                   cai_error *error);
  /** Close a reader opened by open_read. */
  void (*close_read)(void *context, void *transaction, void *reader_context);
  /** Open a replacement JSON document as a lonejson sink. */
  int (*open_write)(void *context, void *transaction, lonejson_sink_fn *sink,
                    void **sink_context, cai_error *error);
  /** Commit bytes written through the active write sink. */
  int (*commit_write)(void *context, void *transaction, cai_error *error);
  /** Commit the whole transaction. */
  int (*commit)(void *context, void *transaction, cai_error *error);
  /** Roll back a transaction after failure or cancellation. */
  void (*rollback)(void *context, void *transaction);
  /** Destroy callback context owned by the store implementation. */
  void (*destroy)(void *context);
} cai_todo_store_callbacks;

/** Configuration for registering the kanban todo preset tool. */
typedef struct cai_todo_tool_config {
  /** Tool name exposed to the model, or NULL for the default name. */
  const char *name;
  /** Tool description exposed to the model, or NULL for the default. */
  const char *description;
  /** Optional custom streaming store implementation. */
  const cai_todo_store_callbacks *store;
  /** Context passed to the custom store implementation. */
  void *store_context;
  /** File-backed store path, or NULL for the cai config default. */
  const char *store_path;
  /** File-backed lock path, or NULL for the cai config default. */
  const char *lock_path;
  /** Optional default board name for frictionless add/list operations.
   *
   * The default board uses the public key `DEF`; item references are assigned
   * as board-key sequence values such as `DEF-001`. Sequence numbers are owned
   * by cai and cannot be supplied by callers.
   */
  const char *default_board;
  /** Maximum title size in bytes; zero uses the preset default. */
  size_t max_title_bytes;
  /** Maximum description size in bytes; zero uses the preset default. */
  size_t max_description_bytes;
  /** Maximum listed items per tool result; zero uses the preset default. */
  size_t max_result_items;
} cai_todo_tool_config;

/** Register the kanban todo preset in a standalone tool registry. */
int cai_tool_registry_register_todo_tool(cai_tool_registry *registry,
                                         const cai_todo_tool_config *config,
                                         cai_error *error);
/** Register the kanban todo preset directly on an agent. */
int cai_agent_register_todo_tool(cai_agent *agent,
                                 const cai_todo_tool_config *config,
                                 cai_error *error);

#ifdef __cplusplus
}
#endif

#endif
