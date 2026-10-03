#include "../cli/chrome.h"
#include <stdio.h>

/* The production host reactor, without the musl-only CLI dependencies, so
 * its framing, queues and lifecycle can also run under AddressSanitizer. */
int main(int argc, char **argv) {
  cai_error error;
  int rc;
  if (argc != 2)
    return 2;
  cai_error_init(&error);
  rc = cai_cli_chrome_host(argv[1], NULL, &error);
  if (rc != CAI_OK)
    fprintf(stderr, "%s: %s\n",
            error.message != NULL ? error.message : "failed",
            error.detail != NULL ? error.detail : "");
  cai_error_cleanup(&error);
  return rc == CAI_OK ? 0 : 1;
}
