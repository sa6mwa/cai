/* Public-softline-only diagnostic: resize between writes in one watch callback.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <softline/softline.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int control_fd, yield_mode, phase, partial;
static int emit(sl_t *sl, const char *text) {
  return sl_output_stream_write(sl, text, strlen(text));
}
static int producer(sl_t *sl, const sl_watch_event_t *event, void *context) {
  struct pollfd descriptor;
  char row[256], command;
  int i, ready;
  (void)event;
  (void)context;
  if (read(control_fd, &command, 1U) != 1 || command != (phase ? 'P' : 'F'))
    return SL_ERROR_IO;
  if (!phase) {
    for (i = 0; i < 120; i++) {
      snprintf(row, sizeof(row),
               "SOURCE_%03d abcdefghijklmnopqrstuvwxyz "
               "ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789\n",
               i);
      if (emit(sl, row) != SL_OK)
        return SL_ERROR_IO;
    }
    if (emit(sl, partial ? "RESIZE READY " : "RESIZE READY\n") != SL_OK)
      return SL_ERROR_IO;
    if (partial) {
      for (i = 0; i < 500; i++)
        if (emit(sl, "x") != SL_OK)
          return SL_ERROR_IO;
    }
    phase = 1;
    if (yield_mode)
      return SL_OK;
    /* A stationary resize, not a resize racing a physical write. The shell
     * changes the PTY geometry before releasing this next producer chunk. */
    descriptor.fd = control_fd;
    descriptor.events = POLLIN;
    do {
      ready = poll(&descriptor, 1U, 10000);
    } while (ready < 0 && errno == EINTR);
    if (ready <= 0 || read(control_fd, &command, 1U) != 1 || command != 'P')
      return SL_ERROR_IO;
  }
  if (partial && emit(sl, "\n") != SL_OK)
    return SL_ERROR_IO;
  if (emit(sl, "* Edited snake.c\n") != SL_OK)
    return SL_ERROR_IO;
  for (i = 0; i < 6; i++) {
    snprintf(row, sizeof(row), "\033[42m%4d +PATCH_ROW_%03d\033[0m\n", i + 1,
             i);
    if (emit(sl, row) != SL_OK)
      return SL_ERROR_IO;
  }
  if (emit(sl, "FINAL ANSWER\n\n") != SL_OK ||
      sl_set_status_busy(sl, 0) != SL_OK ||
      sl_set_status_spinner(sl, 0) != SL_OK ||
      sl_set_status_message(sl, "Worked for 0m 1s") != SL_OK)
    return SL_ERROR_IO;
  return SL_OK;
}
int main(int argc, char **argv) {
  sl_config_t config;
  sl_t *sl;
  sl_watch_id_t watch;
  char *line;
  int result = 0, retained;
  if (argc < 2 || argc > 4)
    return 2;
  retained = argc >= 3 && strcmp(argv[2], "retained") == 0;
  yield_mode = argc >= 3 && strcmp(argv[2], "yield") == 0;
  partial = argc == 4 && strcmp(argv[3], "partial") == 0;
  control_fd = open(argv[1], O_RDONLY | O_NONBLOCK);
  if (control_fd < 0)
    return 2;
  sl_config_init(&config);
  sl = sl_create_with_config(&config);
  if (sl == NULL)
    return 2;
  if (sl_set_statusline(sl, 1, 0) != SL_OK ||
      sl_set_status_busy(sl, 1) != SL_OK ||
      sl_set_status_spinner(sl, 1) != SL_OK ||
      sl_set_status_message(sl, "Welcome to resize reproduction") != SL_OK ||
      sl_set_status_element(
          sl, 0U,
          "gpt-6-luna medium : ctx 5% : ~/workspace : w 99% : crd 58816") !=
          SL_OK ||
      sl_output_stream_begin(sl) != SL_OK ||
      (!retained && sl_watch_add(sl, control_fd, SL_WATCH_READ, producer, NULL,
                                 &watch) != SL_OK)) {
    sl_destroy(sl);
    close(control_fd);
    return 2;
  }
  while ((line = sl_next_prompt(sl, "> ", NULL)) != NULL) {
    int quit = strcmp(line, "/quit") == 0;
    sl_free_string(sl, line);
    if (quit)
      break;
    if (retained) {
      struct pollfd descriptor;
      descriptor.fd = control_fd;
      descriptor.events = POLLIN;
      if (poll(&descriptor, 1U, 10000) <= 0 ||
          producer(sl, NULL, NULL) != SL_OK) {
        result = 1;
        break;
      }
    }
  }
  if (sl_last_readline_status(sl) == SL_READLINE_ERROR)
    result = 1;
  if (!retained)
    (void)sl_watch_remove(sl, watch);
  if (sl_output_stream_end(sl) != SL_OK)
    result = 1;
  sl_destroy(sl);
  close(control_fd);
  return result;
}
