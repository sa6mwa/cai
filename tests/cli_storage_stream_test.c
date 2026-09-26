#define _POSIX_C_SOURCE 200809L
#include "../cli/pouch_stream.h"

#include <stdio.h>
#include <string.h>

typedef struct fixture {
  size_t total;
  size_t produced;
  int destroyed;
  int fail;
} fixture;

static int produce(void *context, cai_sink *sink, cai_error *error) {
  fixture *value;
  unsigned char bytes[4096];
  size_t i;
  size_t n;
  int rc;
  value = (fixture *)context;
  while (value->produced < value->total) {
    n = value->total - value->produced;
    if (n > sizeof(bytes))
      n = sizeof(bytes);
    for (i = 0U; i < n; i++)
      bytes[i] = (unsigned char)((value->produced + i) % 251U);
    rc = cai_sink_write(sink, bytes, n, error);
    if (rc != CAI_OK)
      return rc;
    value->produced += n;
  }
  return value->fail
             ? cai_cli_error(error, CAI_ERR_TRANSPORT, "fixture failure")
             : CAI_OK;
}

static void destroy(void *context) { ((fixture *)context)->destroyed++; }

int main(void) {
  fixture value;
  cai_source *source;
  cai_error error;
  unsigned char bytes[997];
  size_t total;
  size_t n;
  size_t i;
  int pass;
  cai_error_init(&error);
  for (pass = 0; pass < 3; pass++) {
    memset(&value, 0, sizeof(value));
    value.total = pass == 2 ? 4096U : 16U * 1024U * 1024U;
    value.fail = pass == 2;
    source = NULL;
    if (cai_cli_stream_source(produce, &value, destroy, &source, &error) !=
        CAI_OK)
      return 1;
    total = 0U;
    if (pass != 1) {
      while ((n = cai_source_read(source, bytes, sizeof(bytes), &error)) !=
             0U) {
        for (i = 0U; i < n; i++) {
          if (bytes[i] != (unsigned char)((total + i) % 251U))
            return 2;
        }
        total += n;
      }
      if (total != value.total || (pass == 0 && error.code != CAI_OK) ||
          (pass == 2 && (error.code != CAI_ERR_TRANSPORT ||
                         strcmp(error.message, "fixture failure") != 0)))
        return 3;
    }
    cai_source_close(source);
    if (value.destroyed != 1 || (pass == 1 && value.produced > 16384U))
      return 4;
    cai_error_cleanup(&error);
  }
  cai_cli_error(&error, CAI_ERR_INVALID, "alias test");
  cai_cli_error(&error, CAI_ERR_INVALID, error.message);
  if (strcmp(error.message, "alias test") != 0)
    return 5;
  cai_error_cleanup(&error);
  return 0;
}
