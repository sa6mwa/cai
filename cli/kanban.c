#include "log.h"
#include "pouch.h"
#include <cai/app.h>
#include <stdlib.h>
#include <string.h>

typedef struct app_kanban_store {
  cai_cli_pouch pouch;
  cai_cli_log *log;
} app_kanban_store;

static void kanban_store_close(void *context) {
  app_kanban_store *store = context;
  cai_cli_pouch_close(&store->pouch);
  cai_cli_log_release(store->log);
  free(store);
}

void cai_app_kanban_config_init(cai_app_kanban_config *config) {
  if (config == NULL)
    return;
  memset(config, 0, sizeof(*config));
  config->program_name = CAI_APP_DEFAULT_PROGRAM_NAME;
  config->default_board = CAI_TODO_DEFAULT_BOARD;
}

int cai_app_kanban_open(const cai_app_kanban_config *config, cai_todo **out,
                        cai_error *error) {
  cai_app_kanban_config defaults;
  cai_todo_tool_config todo;
  cai_cli_options options;
  cai_todo_store_callbacks callbacks;
  app_kanban_store *store;
  int rc;
  if (out == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID, "kanban output required");
  *out = NULL;
  cai_app_kanban_config_init(&defaults);
  if (config == NULL)
    config = &defaults;
  store = calloc(1U, sizeof(*store));
  if (store == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate kanban store");

  rc = cai_cli_log_new(&store->log, PSLOG_LEVEL_WARN, 0, config->logger, error);
  memset(&options, 0, sizeof(options));
  options.program_name = config->program_name;
  options.namespace_prefix = config->namespace_prefix;
  options.todo_namespace = config->todo_namespace;
  options.pouch_key_file = config->pouch_key_file;
  options.state_dir = config->state_directory;
  options.lockd = config->lockd;
  options.lockd_client_pem = config->lockd_client_pem;
  if (rc == CAI_OK)
    rc = cai_cli_pouch_open_options(&store->pouch, &options, store->log->logger,
                                    error);
  if (rc == CAI_OK) {
    memset(&todo, 0, sizeof(todo));
    callbacks = *cai_cli_pouch_todo_callbacks();
    callbacks.destroy = kanban_store_close;
    todo.store = &callbacks;
    todo.store_context = store;
    todo.default_board = config->default_board;
    todo.max_result_items = config->max_result_items;
    todo.logger = store->log->logger;
    rc = cai_todo_open(&todo, out, error);
  }
  if (rc != CAI_OK)
    kanban_store_close(store);
  return rc;
}
