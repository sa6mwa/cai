/* Test owner-side reconciliation and failure propagation without a terminal,
 * model request, timer or producer delay. Actual terminal cells are covered
 * by the separate resize regressions. */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <softline/softline.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>

static struct winsize geometry;
static int geometry_error, reconcile_error, reconciles, writes;
static int fixture_ioctl(int fd, unsigned long request, ...) {
  va_list arguments;
  struct winsize *out;
  (void)fd;
  (void)request;
  va_start(arguments, request);
  out = va_arg(arguments, struct winsize *);
  va_end(arguments);
  *out = geometry;
  return geometry_error;
}
static int fixture_reconcile(sl_t *sl) {
  (void)sl;
  reconciles++;
  return reconcile_error ? SL_ERROR_IO : SL_OK;
}
static int fixture_write(sl_t *sl, const char *bytes, size_t count) {
  (void)sl;
  (void)bytes;
  (void)count;
  writes++;
  return SL_OK;
}
static int fixture_quote(sl_t *sl, const char *text) {
  return fixture_write(sl, text, strlen(text));
}
#define ioctl fixture_ioctl
#define sl_output_stream_reconcile fixture_reconcile
#define sl_output_stream_write fixture_write
#define sl_output_stream_write_quoted_prompt fixture_quote
#include "../cli/app_run.c"

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "geometry assertion failed at line %d\n", __LINE__);     \
      return 1;                                                                \
    }                                                                          \
  } while (0)

int main(void) {
  cli_state state;
  memset(&state, 0, sizeof(state));
  geometry.ws_col = 120;
  geometry.ws_row = 30;
  CHECK(cli_sink(&state, "first", 5U) == 0 && reconciles == 1 && writes == 1);
  CHECK(cli_sink(&state, "next", 4U) == 0 && reconciles == 1 && writes == 2);
  geometry.ws_row = 18;
  CHECK(cli_sink(&state, "height", 6U) == 0 && reconciles == 2 && writes == 3);
  geometry.ws_col = 60;
  reconcile_error = 1;
  CHECK(cli_sink(&state, "blocked", 7U) == -1 && reconciles == 3 &&
        writes == 3);
  CHECK(state.output_columns == 120 && state.output_rows == 18);
  reconcile_error = 0;
  CHECK(cli_sink(&state, "retry", 5U) == 0 && reconciles == 4 && writes == 4);
  CHECK(state.output_columns == 60 && state.output_rows == 18);
  geometry.ws_col = 130;
  CHECK(cli_sink(&state, "", 0U) == 0 && reconciles == 4 && writes == 4);
  geometry_error = -1;
  CHECK(cli_sink(&state, "redirected", 10U) == 0 && reconciles == 4 &&
        writes == 5);
  geometry_error = 0;
  geometry.ws_row = 0;
  CHECK(cli_sink(&state, "unknown", 7U) == 0 && reconciles == 4 && writes == 6);
  geometry.ws_row = 36;
  CHECK(cli_sink(&state, "grown", 5U) == 0 && reconciles == 5 && writes == 7);
  state.batch = 1;
  geometry.ws_row = 40;
  CHECK(cli_reconcile_output(&state) == 0 && reconciles == 5);
  CHECK(cli_prompt(&state, "batch") == 0 && reconciles == 5 && writes == 7);
  state.batch = 0;
  reconcile_error = 1;
  CHECK(cli_prompt(&state, "blocked") == -1 && reconciles == 6 && writes == 7);
  reconcile_error = 0;
  CHECK(cli_prompt(&state, "prompt") == 0 && reconciles == 7 && writes == 8);
  return 0;
}
