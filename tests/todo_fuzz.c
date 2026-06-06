#define CAI_FUZZ_COMMON_SINK
#define CAI_FUZZ_COMMON_DUP
#define CAI_FUZZ_COMMON_HEX
#include <cai/cai.h>
#include <cai/tools/todo.h>

#include "cai_internal.h"
#include "fuzz_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);

static int cai_fuzz_todo_root(char *root, size_t root_size) {
  static char cached_root[PATH_MAX];
  static int initialized;
  char template_path[PATH_MAX];

  if (!initialized) {
    snprintf(template_path, sizeof(template_path),
             "/tmp/cai-todo-fuzz-%ld-XXXXXX", (long)getpid());
    if (mkdtemp(template_path) == NULL) {
      return 0;
    }
    memcpy(cached_root, template_path, strlen(template_path) + 1U);
    initialized = 1;
  }
  if (strlen(cached_root) + 1U > root_size) {
    return 0;
  }
  memcpy(root, cached_root, strlen(cached_root) + 1U);
  return 1;
}

static char cai_fuzz_todo_key_char(const char *value, size_t index,
                                   char fallback) {
  size_t len;

  if (value == NULL) {
    return fallback;
  }
  len = strlen(value);
  if (index >= len) {
    return fallback;
  }
  return value[index];
}

static void cai_fuzz_todo_run(cai_tool_registry *registry, cai_sink *sink,
                              const char *json, cai_error *error) {
  lonejson_spooled arguments;
  lonejson_error json_error;

  (void)cai_tool_registry_run(registry, CAI_TODO_DEFAULT_TOOL_NAME, json, sink,
                              error);
  cai_error_cleanup(error);
  cai_error_init(error);
  memset(&arguments, 0, sizeof(arguments));
  CAI_LJ->spooled_init(CAI_LJ, &arguments);
  lonejson_error_init(&json_error);
  if (arguments.append(&arguments, json, strlen(json), &json_error) ==
      LONEJSON_STATUS_OK) {
    (void)cai_tool_registry_run_spooled(registry, CAI_TODO_DEFAULT_TOOL_NAME,
                                        &arguments, sink, error);
    cai_error_cleanup(error);
    cai_error_init(error);
  }
  arguments.cleanup(&arguments);
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
  cai_tool_registry *registry;
  cai_todo_tool_config config;
  cai_fuzz_noop_sink_context sink_context;
  cai_sink *sink;
  cai_error error;
  char root[PATH_MAX];
  char store_path[PATH_MAX];
  char lock_path[PATH_MAX];
  char *json;
  char *payload_hex;
  char board_name[32];
  char board_key[8];
  char command[8192];

  registry = NULL;
  sink = NULL;
  json = NULL;
  payload_hex = NULL;
  memset(&config, 0, sizeof(config));
  memset(&sink_context, 0, sizeof(sink_context));
  cai_error_init(&error);
  if (!cai_fuzz_todo_root(root, sizeof(root))) {
    cai_error_cleanup(&error);
    return 0;
  }
  snprintf(store_path, sizeof(store_path), "%s/todo.json", root);
  snprintf(lock_path, sizeof(lock_path), "%s/todo.lock", root);
  unlink(store_path);
  unlink(lock_path);
  if (cai_tool_registry_new(&registry, &error) != CAI_OK) {
    cai_error_cleanup(&error);
    return 0;
  }
  config.store_path = store_path;
  config.lock_path = lock_path;
  config.default_board = "default";
  config.max_title_bytes = 4096U;
  config.max_description_bytes = 16U * 1024U;
  config.max_result_items = 32U;
  if (cai_tool_registry_register_todo_tool(registry, &config, &error) !=
          CAI_OK ||
      cai_fuzz_sink_new(&sink_context, &sink, &error) != CAI_OK) {
    cai_sink_close(sink);
    cai_tool_registry_destroy(registry);
    cai_error_cleanup(&error);
    return 0;
  }

  json = cai_fuzz_dup_cstr(data, size);
  if (json != NULL) {
    cai_fuzz_todo_run(registry, sink, json, &error);
  }
  payload_hex = cai_fuzz_hex_string(data, size, 1024U);
  if (payload_hex != NULL) {
    snprintf(board_name, sizeof(board_name), "board-%c%c%c",
             cai_fuzz_todo_key_char(payload_hex, 0U, 'a'),
             cai_fuzz_todo_key_char(payload_hex, 1U, 'b'),
             cai_fuzz_todo_key_char(payload_hex, 2U, 'c'));
    snprintf(board_key, sizeof(board_key), "B%c%c",
             cai_fuzz_todo_key_char(payload_hex, 0U, '0'),
             cai_fuzz_todo_key_char(payload_hex, 1U, '1'));

    cai_fuzz_todo_run(registry, sink, "{\"operation\":\"help\"}", &error);
    snprintf(command, sizeof(command),
             "{\"operation\":\"create_board\",\"board_name\":\"%s\","
             "\"board_key\":\"%s\",\"wip_limit\":1}",
             board_name, board_key);
    cai_fuzz_todo_run(registry, sink, command, &error);
    snprintf(command, sizeof(command),
             "{\"operation\":\"add_item\",\"board_name\":\"%s\","
             "\"title\":\"%s\",\"description\":\"%s\",\"status\":\"todo\"}",
             board_name, payload_hex, payload_hex);
    cai_fuzz_todo_run(registry, sink, command, &error);
    snprintf(command, sizeof(command),
             "{\"operation\":\"list_board\",\"board_name\":\"%s\"}",
             board_name);
    cai_fuzz_todo_run(registry, sink, command, &error);
    snprintf(command, sizeof(command),
             "{\"operation\":\"move_item\",\"board_name\":\"%s\","
             "\"item_id\":\"%s-001\",\"status\":\"in_process\"}",
             board_name, board_key);
    cai_fuzz_todo_run(registry, sink, command, &error);
    snprintf(command, sizeof(command),
             "{\"operation\":\"set_wip_limit\",\"board_name\":\"%s\","
             "\"wip_limit\":1}",
             board_name);
    cai_fuzz_todo_run(registry, sink, command, &error);
    snprintf(command, sizeof(command),
             "{\"operation\":\"complete_item\",\"board_name\":\"%s\","
             "\"item_id\":\"%s-001\"}",
             board_name, board_key);
    cai_fuzz_todo_run(registry, sink, command, &error);
  }

  free(payload_hex);
  free(json);
  cai_sink_close(sink);
  cai_tool_registry_destroy(registry);
  cai_error_cleanup(&error);
  return 0;
}
