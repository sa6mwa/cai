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
#define CAI_TODO_DEFAULT_BOARD "default"
/** Board names are UTF-8 text without ASCII control bytes. */
#define CAI_TODO_BOARD_NAME_MAX 256U

typedef struct cai_todo cai_todo;

/** Framework-neutral, read-only browser route. resource is the relative asset
 * path (empty, board.json, app.js or style.css); board is a decoded board name,
 * or NULL for this receiver's selection. The host owns routing and access. */
typedef struct cai_todo_http_request {
  const char *method;
  const char *resource;
  const char *board;
} cai_todo_http_request;
typedef struct cai_todo_http_response {
  int status;
  const char *content_type;
  /** Called before any body bytes. May set host HTTP headers/status. */
  int (*begin)(void *context, int status, const char *content_type,
               cai_error *error);
  void *context;
  cai_sink *body;
} cai_todo_http_response;

/** Streaming storage callbacks for the kanban todo preset. */
typedef struct cai_todo_store_callbacks {
  /** Begin a storage transaction and return an opaque transaction. */
  int (*begin)(void *context, void **transaction, cai_error *error);
  /** Open the current JSON document as a lonejson reader. */
  int (*open_read)(void *context, void *transaction, lonejson_reader_fn *reader,
                   void **reader_context, cai_error *error);
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
   * The board named "default" uses the public key `DEF`; other names derive
   * their own keys. Item references are assigned
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
  /** Optional shared receiver. Registration retains it. Other store settings
   * are unused; NULL opens a receiver from the settings above. */
  cai_todo *instance;
  /** Borrowed thread-safe logger, valid until the receiver's final close. */
  struct pslog_logger *logger;
} cai_todo_tool_config;

/** Thread-safe kanban receiver. Operations serialize on this receiver; the
 * store supplies cross-receiver/process transactions. Tool results contain at
 * most max_result_items materialized entries; JSON input/output and document
 * rewrites use incremental readers/writers. Callbacks must not reenter it.
 * Each caller holds a reference until its operations have finished. */
struct cai_todo {
  int (*call)(cai_todo *self, cai_source *arguments, cai_sink *result,
              cai_error *error);
  int (*select)(cai_todo *self, const char *name, int create, cai_error *error);
  int (*get_board)(cai_todo *self, char *name, size_t capacity,
                   cai_error *error);
  int (*handle_http)(cai_todo *self, const cai_todo_http_request *request,
                     cai_todo_http_response *response, cai_error *error);
  void (*close)(cai_todo *self);
  void *impl;
};

/** NULL config uses the standalone file store and board "default". A custom
 * store's destroy/context ownership transfers only on successful open. */
int cai_todo_open(const cai_todo_tool_config *config, cai_todo **out,
                  cai_error *error);
/** Retain one owned reference. The caller must keep self alive during retain.
 */
cai_todo *cai_todo_retain(cai_todo *self);
/** Read JSON arguments and write the bounded result incrementally. Streams
 * remain caller-owned. error must be an initialized, non-NULL cai_error. */
int cai_todo_call(cai_todo *self, cai_source *arguments, cai_sink *result,
                  cai_error *error);
/** Select an existing named board, creating it first when create is nonzero.
 * Failure leaves the selection unchanged; a created board remains stored. */
int cai_todo_select(cai_todo *self, const char *name, int create,
                    cai_error *error);
/** Copy the selected name, including NUL; insufficient capacity returns LIMIT.
 */
int cai_todo_get_board(cai_todo *self, char *name, size_t capacity,
                       cai_error *error);
/** Serve a read-only route. begin precedes body writes; the caller owns body.
 * A storage error can return a nonzero CAI status after sending a 503 response.
 * error is initialized and non-NULL. No second response may follow begin. */
int cai_todo_handle_http(cai_todo *self, const cai_todo_http_request *request,
                         cai_todo_http_response *response, cai_error *error);
/** Release a reference; NULL is accepted. Final close destroys the store. */
void cai_todo_close(cai_todo *self);

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
