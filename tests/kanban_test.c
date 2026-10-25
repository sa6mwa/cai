#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <cai/app.h>
#include <pslog.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

unsigned int cai_test_storage_open_count(void);

typedef struct bytes {
  char text[131072];
  size_t used, position;
  int began, writes, fail, headers_fail, status;
} bytes;
static char logged_board[257];
static void record_log(pslog_logger *logger, const char *message,
                       const char *fields, ...) {
  va_list arguments;
  (void)logger;
  (void)message;
  if (strcmp(fields, "operation=%s board=%s") != 0)
    return;
  va_start(arguments, fields);
  (void)va_arg(arguments, const char *);
  snprintf(logged_board, sizeof(logged_board), "%s",
           va_arg(arguments, const char *));
  va_end(arguments);
}
static size_t read_bytes(void *context, void *out, size_t size,
                         cai_error *error) {
  bytes *b = context;
  size_t n = strlen(b->text) - b->position;
  (void)error;
  if (n > size)
    n = size;
  if (n > 7U)
    n = 7U;
  memcpy(out, b->text + b->position, n);
  b->position += n;
  return n;
}
static int write_bytes(void *context, const void *input, size_t size,
                       cai_error *error) {
  bytes *b = context;
  (void)error;
  assert(b->began);
  b->writes++;
  if (b->fail)
    return CAI_ERR_TRANSPORT;
  assert(b->used + size < sizeof(b->text));
  memcpy(b->text + b->used, input, size);
  b->used += size;
  b->text[b->used] = '\0';
  return CAI_OK;
}
static int begin(void *context, int status, const char *type,
                 cai_error *error) {
  bytes *b = context;
  (void)error;
  assert(!b->began && status >= 200 && type != NULL);
  b->began = 1;
  b->status = status;
  return b->headers_fail ? CAI_ERR_TRANSPORT : CAI_OK;
}
static int request(cai_todo *todo, const char *json, bytes *out,
                   cai_error *error) {
  bytes input;
  cai_source_callbacks r;
  cai_sink_callbacks w;
  cai_source *source = NULL;
  cai_sink *sink = NULL;
  int rc;
  memset(&input, 0, sizeof(input));
  strcpy(input.text, json);
  memset(out, 0, sizeof(*out));
  out->began = 1;
  memset(&r, 0, sizeof(r));
  r.context = &input;
  r.read = read_bytes;
  memset(&w, 0, sizeof(w));
  w.context = out;
  w.write = write_bytes;
  assert(cai_source_from_callbacks(&r, &source, error) == CAI_OK);
  assert(cai_sink_from_callbacks(&w, &sink, error) == CAI_OK);
  rc = todo->call(todo, source, sink, error);
  cai_source_close(source);
  cai_sink_close(sink);
  return rc;
}
static int http(cai_todo *todo, const char *method, const char *path,
                const char *board, bytes *out, cai_error *error) {
  cai_todo_http_request r;
  cai_todo_http_response response;
  cai_sink_callbacks w;
  int rc;
  memset(&response, 0, sizeof(response));
  r.method = method;
  r.resource = path;
  r.board = board;
  memset(&w, 0, sizeof(w));
  w.context = out;
  w.write = write_bytes;
  assert(cai_sink_from_callbacks(&w, &response.body, error) == CAI_OK);
  response.begin = begin;
  response.context = out;
  rc = todo->handle_http(todo, &r, &response, error);
  cai_sink_close(response.body);
  if (rc != CAI_OK)
    return -rc;
  return response.status;
}
static void reset(cai_error *error) {
  cai_error_cleanup(error);
  cai_error_init(error);
}
int main(int argc, char **argv) {
  cai_app_kanban_config config;
  cai_todo_tool_config core;
  cai_todo *a = NULL, *b = NULL, *file = NULL;
  pslog_logger logger;
  cai_error error;
  bytes out;
  char endpoint[4096], path[4096], lock[4096], board[257];
  int status;
  pid_t child;
  assert(argc == 2 || argc == 3);
  cai_error_init(&error);
  snprintf(endpoint, sizeof(endpoint), "pouch://%s/pouch", argv[1]);
  cai_app_kanban_config_init(&config);
  assert(strcmp(config.default_board, "default") == 0);
  config.program_name = "smith";
  config.namespace_prefix = "test";
  config.todo_namespace = "test.smith.todo";
  config.state_directory = argv[1];
  config.lockd = endpoint;
  if (argc == 3) {
    assert(cai_app_kanban_open(&config, &a, &error) == CAI_OK);
    assert(request(a, "{\"operation\":\"add_item\",\"title\":\"CHILD UPDATE\"}",
                   &out, &error) == CAI_OK);
    a->close(a);
    cai_error_cleanup(&error);
    return 0;
  }
  if (cai_app_kanban_open(&config, &a, &error) != CAI_OK) {
    fprintf(stderr, "open shared kanban: %s (%s)\n", error.message,
            error.detail != NULL ? error.detail : "");
    abort();
  }
  config.default_board = "other";
  assert(cai_app_kanban_open(&config, &b, &error) == CAI_OK);
  assert(cai_test_storage_open_count() == 1U);
  assert(request(a,
                 "{\"operation\":\"add_item\",\"title\":\"FIRST SHARED CARD\"}",
                 &out, &error) == CAI_OK);
  assert(strstr(out.text, "DEF-001") != NULL);
  assert(request(b,
                 "{\"operation\":\"add_item\",\"title\":\"OTHER BOARD CARD\"}",
                 &out, &error) == CAI_OK);
  assert(strstr(out.text, "OTH-001") != NULL);
  assert(b->select(b, "default", 0, &error) == CAI_OK);
  assert(request(b, "{\"operation\":\"snapshot_board\"}", &out, &error) ==
         CAI_OK);
  assert(strstr(out.text, "FIRST SHARED CARD") &&
         !strstr(out.text, "OTHER BOARD CARD"));
  assert(b->select(b, "team <&>", 1, &error) == CAI_OK);
  assert(a->get_board(a, board, sizeof(board), &error) == CAI_OK &&
         !strcmp(board, "default"));
  assert(b->select(b, "missing", 0, &error) == CAI_ERR_INVALID);
  reset(&error);
  assert(b->get_board(b, board, sizeof(board), &error) == CAI_OK &&
         !strcmp(board, "team <&>"));
  assert(b->select(b, "bad\nname", 1, &error) == CAI_ERR_INVALID);
  reset(&error);
  assert(b->get_board(b, board, 1U, &error) == CAI_ERR_LIMIT);
  reset(&error);
  /* An independently opened process mutates the same board while the reader
   * remains open, exactly as two embedded applications would. */
  child = fork();
  assert(child >= 0);
  if (child == 0) {
    execl(argv[0], argv[0], argv[1], "--child", (char *)NULL);
    _exit(127);
  }
  assert(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
         WEXITSTATUS(status) == 0);
  memset(&out, 0, sizeof(out));
  assert(http(a, "GET", "board.json", NULL, &out, &error) == 200);
  assert(strstr(out.text, "CHILD UPDATE") && out.writes > 1);
  assert(request(a, "{\"operation\":\"complete_item\",\"item_id\":\"DEF-001\"}",
                 &out, &error) == CAI_OK);
  memset(&out, 0, sizeof(out));
  assert(http(a, "GET", "board.json", NULL, &out, &error) == 200);
  assert(strstr(out.text, "FIRST SHARED CARD") &&
         strstr(out.text, "\"status\":\"done\""));
  memset(&out, 0, sizeof(out));
  assert(http(a, "HEAD", "board.json", NULL, &out, &error) == 200 &&
         out.used == 0U);
  memset(&out, 0, sizeof(out));
  assert(http(a, "GET", "board.json", "missing", &out, &error) == 404);
  memset(&out, 0, sizeof(out));
  assert(http(a, "GET", "board.json", "bad\nname", &out, &error) < 0 &&
         out.status == 400);
  reset(&error);
  memset(&out, 0, sizeof(out));
  assert(http(a, "GET", "", NULL, &out, &error) == 200 &&
         strstr(out.text, "app.js"));
  memset(&out, 0, sizeof(out));
  assert(http(a, "GET", "app.js", NULL, &out, &error) == 200 &&
         strstr(out.text, "document.hidden"));
  memset(&out, 0, sizeof(out));
  assert(http(a, "GET", "style.css", NULL, &out, &error) == 200 &&
         strstr(out.text, "grid"));
  memset(&out, 0, sizeof(out));
  assert(http(a, "POST", "board.json", NULL, &out, &error) == 405);
  memset(&out, 0, sizeof(out));
  assert(http(a, "GET", "../private", NULL, &out, &error) == 404);
  memset(&out, 0, sizeof(out));
  out.fail = 1;
  assert(http(a, "GET", "", NULL, &out, &error) < 0);
  reset(&error);
  memset(&out, 0, sizeof(out));
  out.headers_fail = 1;
  assert(http(a, "GET", "", NULL, &out, &error) < 0 && out.writes == 0);
  reset(&error);
  assert(request(a, "{broken", &out, &error) != CAI_OK);
  reset(&error);
  a->close(a);
  /* The first store and its app-owned logger may close before another view
   * finishes using the shared SDK client. */
  assert(request(b, "{\"operation\":\"snapshot_board\"}", &out, &error) ==
         CAI_OK);
  b->close(b);
  /* Standalone core receiver, bounded results, shared registry ownership. */
  memset(&core, 0, sizeof(core));
  snprintf(path, sizeof(path), "%s/todo.json", argv[1]);
  snprintf(lock, sizeof(lock), "%s/todo.lock", argv[1]);
  core.store_path = path;
  core.lock_path = lock;
  core.max_result_items = 1U;
  memset(&logger, 0, sizeof(logger));
  logger.infof = record_log;
  logger.warnf = record_log;
  core.logger = &logger;
  assert(cai_todo_open(&core, &file, &error) == CAI_OK);
  assert(request(file,
                 "{\"operation\":\"create_board\",\"board_name\":\"visible\"}",
                 &out, &error) == CAI_OK);
  assert(strcmp(logged_board, "visible") == 0);
  assert(request(file,
                 "{\"operation\":\"select_board\",\"board_name\":\"missing\"}",
                 &out, &error) == CAI_OK);
  assert(strcmp(logged_board, "visible") == 0);
  assert(request(file, "{\"operation\":\"help\"}", &out, &error) == CAI_OK);
  assert(strstr(out.text, "select_board") &&
         strstr(out.text, "snapshot_board"));
  assert(request(file, "{\"operation\":\"add_item\",\"title\":\"one\"}", &out,
                 &error) == CAI_OK);
  assert(request(file, "{\"operation\":\"add_item\",\"title\":\"two\"}", &out,
                 &error) == CAI_OK);
  assert(request(file, "{\"operation\":\"snapshot_board\"}", &out, &error) ==
             CAI_OK &&
         strstr(out.text, "\"truncated\":true"));
  assert(file->select(file, "bad\300\200utf8", 1, &error) == CAI_ERR_INVALID);
  reset(&error);
  {
    FILE *corrupt = fopen(path, "w");
    assert(corrupt != NULL);
    assert(fputs("{broken", corrupt) >= 0 && fclose(corrupt) == 0);
    memset(&out, 0, sizeof(out));
    assert(http(file, "GET", "board.json", NULL, &out, &error) < 0);
    assert(out.status == 503 && strstr(out.text, "snapshot unavailable"));
    reset(&error);
  }
  {
    cai_tool_registry *registry = NULL;
    assert(cai_tool_registry_new(&registry, &error) == CAI_OK);
    core.instance = file;
    assert(cai_tool_registry_register_todo_tool(registry, &core, &error) ==
           CAI_OK);
    cai_todo_close(file);
    file = NULL;
    cai_tool_registry_destroy(registry);
  }
  core.instance = NULL;
  core.default_board = "bad\nboard";
  assert(cai_todo_open(&core, &file, &error) == CAI_ERR_INVALID &&
         file == NULL);
  cai_error_cleanup(&error);
  return 0;
}
