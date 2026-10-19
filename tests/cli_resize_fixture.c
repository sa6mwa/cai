#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <softline/softline.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Pause only after a complete physical feed write. The test waits until tmux
 * has displayed the marker, changes geometry, then releases the owner callback.
 * This exercises a resize inside CAI's real runtime/renderer dispatch. */
static int fixture_output(sl_t *sl, const char *bytes, size_t length) {
  static size_t total;
  static int paused;
  const char *ready = getenv("CAI_TEST_RESIZE_READY");
  const char *release = getenv("CAI_TEST_RESIZE_RELEASE");
  FILE *file;
  struct pollfd fd;
  char token;
  int result, rc = sl_output_stream_write(sl, bytes, length);
  total += length;
  if (rc != SL_OK || paused || total < 4096U || length == 0U ||
      bytes[length - 1U] != '\n' || ready == NULL || release == NULL)
    return rc;
  paused = 1;
  if (sl_output_stream_write(sl, "RESIZE SINK READY\n", 18U) != SL_OK)
    return SL_ERROR_IO;
  file = fopen(ready, "w");
  if (file == NULL)
    return SL_ERROR_IO;
  if (fputs("ready\n", file) == EOF || fclose(file) != 0)
    return SL_ERROR_IO;
  fd.fd = open(release, O_RDONLY | O_NONBLOCK);
  fd.events = POLLIN;
  if (fd.fd < 0)
    return SL_ERROR_IO;
  do {
    result = poll(&fd, 1U, 10000);
  } while (result < 0 && errno == EINTR);
  if (result <= 0 || read(fd.fd, &token, 1U) != 1 || token != 'P')
    rc = SL_ERROR_IO;
  close(fd.fd);
  return rc;
}
#define sl_output_stream_write fixture_output
#include "../cli/app_run.c"
#undef sl_output_stream_write
int main(int argc, char **argv) { return cai_cli_app_main(argc, argv, NULL); }
