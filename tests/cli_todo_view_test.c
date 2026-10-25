/* Pure presentation regressions: no database, model, terminal or waits. */
#include "../cli/todo.c"
#include <assert.h>
#include <libmdf/mdf.h>

typedef struct captured {
  char bytes[32768];
  size_t used;
  int fail;
} captured;
static int collect(void *context, const char *data, size_t count) {
  captured *out = context;
  if (out->fail)
    return -1;
  assert(out->used + count < sizeof(out->bytes));
  assert(memchr(data, '\0', count) == NULL);
  memcpy(out->bytes + out->used, data, count);
  out->used += count;
  out->bytes[out->used] = '\0';
  return 0;
}
static void render(const captured *markdown, int width) {
  captured plain;
  mdf_options options;
  mdf_sink sink;
  mdf *renderer = NULL;
  size_t i;
  memset(&plain, 0, sizeof(plain));
  mdf_options_init(&options);
  options.width = width;
  options.ansi_mode = MDF_ANSI_OFF;
  sink.userdata = &plain;
  sink.write = collect;
  assert(mdf_create(MDF_FORMAT_ANSI, &options, &renderer) == MDF_OK);
  assert(renderer->set_sink(renderer, &sink) == MDF_OK);
  for (i = 0U; i < markdown->used; i++)
    assert(renderer->feed(renderer, markdown->bytes + i, 1U) == MDF_OK);
  assert(renderer->finish_document(renderer) == MDF_OK);
  assert(strstr(plain.bytes, "TODO") != NULL &&
         strstr(plain.bytes, "DOING") != NULL &&
         strstr(plain.bytes, "DONE") != NULL);
  assert(strstr(plain.bytes, "DEF-001") != NULL &&
         strstr(plain.bytes, "DEF-002") != NULL);
  assert(strstr(plain.bytes, "DEF-003") != NULL &&
         strstr(plain.bytes, "DEF-004") != NULL);
  assert(strstr(plain.bytes, "**") == NULL &&
         strchr(plain.bytes, '\033') == NULL);
  if (strstr(markdown->bytes, "\342\200\246") != NULL)
    assert(strstr(plain.bytes, "\342\200\246") != NULL);
  assert(strstr(plain.bytes, "\357\277\275") == NULL);
  renderer->destroy(renderer);
}
int main(void) {
  todo_entry items[4];
  todo_view view;
  captured output;
  char title[CARD_TITLE_CHARACTERS * 4U + 1U];
  char unicode[100];
  char ids[4][9] = {"DEF-001", "DEF-002", "DEF-003", "DEF-004"};
  char titles[4][32] = {"FIRST TODO", "SECOND TODO", "IN FLIGHT", "FINISHED"};
  char statuses[4][16] = {"todo", "todo", "in_process", "done"};
  char literal[] = "literal * _ | ` [x]", unknown[] = "unknown";
  const char *layout = "board";
  cai_error error;
  size_t i;
  memset(items, 0, sizeof(items));
  memset(&view, 0, sizeof(view));
  memset(&output, 0, sizeof(output));
  assert(board_view(&view, collect, &output) == CAI_OK);
  assert(strstr(output.bytes, "TODO | DOING | DONE") != NULL &&
         strstr(output.bytes, "No cards.") != NULL);
  for (i = 0U; i < 4U; i++) {
    items[i].id = ids[i];
    items[i].title = titles[i];
    items[i].status = statuses[i];
  }
  view.items.items = items;
  view.items.count = 4U;
  memset(&output, 0, sizeof(output));
  assert(board_view(&view, collect, &output) == CAI_OK);
  assert(strstr(output.bytes,
                "| **DEF-001** | **DEF-003** | **DEF-004** |\n| "
                "*FIRST TODO* | *IN FLIGHT* | *FINISHED* |") != NULL);
  assert(strstr(output.bytes,
                "| **DEF-002** |  |  |\n| *SECOND TODO* |  |  |") != NULL);
  render(&output, 120);
  render(&output, 60);
  card_title(title, "1234567890123456789012345678");
  assert(strcmp(title, "1234567890123456789012345678") == 0);
  card_title(title, "12345678901234567890123456789");
  assert(strcmp(title, "123456789012345678901234567\342\200\246") == 0);
  for (i = 0U; i < 29U; i++)
    memcpy(unicode + i * 3U, "\344\270\255", 3U);
  unicode[87] = '\0';
  card_title(title, unicode);
  assert(strlen(title) == 84U && memcmp(title, unicode, 81U) == 0 &&
         strcmp(title + 81U, "\342\200\246") == 0);
  items[0].title = unicode;
  memset(&output, 0, sizeof(output));
  assert(board_view(&view, collect, &output) == CAI_OK);
  render(&output, 120);
  card_title(title, "A\nB\rC\tD\033E\302\233F");
  assert(strcmp(title, "A B C D E F") == 0);
  card_title(title, "A\303B");
  assert(strcmp(title, "A?B") == 0);
  card_title(title, NULL);
  assert(strcmp(title, "Untitled") == 0);
  items[0].title = literal;
  items[3].status = unknown;
  memset(&output, 0, sizeof(output));
  assert(board_view(&view, collect, &output) == CAI_OK);
  assert(strstr(output.bytes, "OTHER") != NULL &&
         strstr(output.bytes, "**DEF-004**") != NULL);
  assert(strstr(output.bytes, "*literal \\* \\_ \\| \\` \\[x\\]*") != NULL);
  output.fail = 1;
  assert(board_view(&view, collect, &output) == CAI_ERR_TRANSPORT);
  output.fail = 0;
  output.used = 0U;
  cai_error_init(&error);
  assert(cai_cli_todo_command(NULL, "layout list", &layout, collect, &output,
                              &error) == CAI_OK);
  assert(strcmp(layout, "list") == 0);
  assert(cai_cli_todo_command(NULL, "layout invalid", &layout, collect, &output,
                              &error) == CAI_ERR_INVALID);
  assert(strcmp(layout, "list") == 0);
  cai_error_cleanup(&error);
  assert(cai_cli_todo_command(NULL, "layout\tboard  ", &layout, collect,
                              &output, &error) == CAI_OK);
  assert(strcmp(layout, "board") == 0);
  assert(cai_cli_todo_command(NULL, "layout", &layout, collect, &output,
                              &error) == CAI_OK);
  assert(strstr(output.bytes, "`board`") != NULL);
  output.fail = 1;
  assert(cai_cli_todo_command(NULL, "help", &layout, collect, &output,
                              &error) == CAI_ERR_TRANSPORT);
  cai_error_cleanup(&error);
  return 0;
}
