#define _POSIX_C_SOURCE 200809L
#include <cai/chrome.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>

/* The production host reactor, without the musl-only CLI dependencies, so
 * its framing, queues and lifecycle can also run under AddressSanitizer. */
int main(int argc, char **argv) {
  cai_error error;
  int rc, input_flags, output_flags;
  struct sigaction before, after;
  if (argc != 2)
    return 2;
  input_flags = fcntl(0, F_GETFL);
  output_flags = fcntl(1, F_GETFL);
  if (sigaction(SIGPIPE, NULL, &before) != 0)
    return 2;
  cai_error_init(&error);
  rc = cai_chrome_native_host_run(argv[1], NULL, &error);
  if (rc != CAI_OK)
    fprintf(stderr, "%s: %s\n",
            error.message != NULL ? error.message : "failed",
            error.detail != NULL ? error.detail : "");
  cai_error_cleanup(&error);
  if (fcntl(0, F_GETFL) != input_flags || fcntl(1, F_GETFL) != output_flags ||
      sigaction(SIGPIPE, NULL, &after) != 0 ||
      before.sa_handler != after.sa_handler) {
    fprintf(stderr, "Chrome host did not restore process state\n");
    return 2;
  }
  return rc == CAI_OK ? 0 : 1;
}
