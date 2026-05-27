#ifndef CAI_TESTS_FUZZ_COMMON_H
#define CAI_TESTS_FUZZ_COMMON_H

#include "cai_internal.h"

#include <stdlib.h>
#include <string.h>

#if defined(CAI_FUZZ_COMMON_SOURCE)
typedef struct cai_fuzz_mem_source_context {
  const unsigned char *data;
  size_t size;
  size_t offset;
  size_t max_chunk;
} cai_fuzz_mem_source_context;

static size_t cai_fuzz_source_read(void *context, void *buffer, size_t count,
                                   cai_error *error) {
  cai_fuzz_mem_source_context *source;
  size_t remaining;
  size_t chunk;

  (void)error;
  source = (cai_fuzz_mem_source_context *)context;
  if (source == NULL || source->offset >= source->size || count == 0U) {
    return 0U;
  }
  remaining = source->size - source->offset;
  chunk = count;
  if (source->max_chunk != 0U && chunk > source->max_chunk) {
    chunk = source->max_chunk;
  }
  if (chunk > remaining) {
    chunk = remaining;
  }
  memcpy(buffer, source->data + source->offset, chunk);
  source->offset += chunk;
  return chunk;
}

static int cai_fuzz_source_reset(void *context, cai_error *error) {
  cai_fuzz_mem_source_context *source;

  (void)error;
  source = (cai_fuzz_mem_source_context *)context;
  if (source != NULL) {
    source->offset = 0U;
  }
  return CAI_OK;
}

static void cai_fuzz_source_close(void *context) { free(context); }

static int cai_fuzz_source_new(const unsigned char *data, size_t size,
                               size_t max_chunk, cai_source **out,
                               cai_error *error) {
  cai_source_callbacks callbacks;
  cai_fuzz_mem_source_context *source;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "fuzz source output is required");
  }
  *out = NULL;
  source = (cai_fuzz_mem_source_context *)calloc(1U, sizeof(*source));
  if (source == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate fuzz source context");
  }
  source->data = data;
  source->size = size;
  source->offset = 0U;
  source->max_chunk = max_chunk;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.read = cai_fuzz_source_read;
  callbacks.reset = cai_fuzz_source_reset;
  callbacks.close = cai_fuzz_source_close;
  callbacks.context = source;
  if (cai_source_from_callbacks(&callbacks, out, error) != CAI_OK) {
    free(source);
    return error != NULL ? error->code : CAI_ERR_NOMEM;
  }
  return CAI_OK;
}
#endif

#if defined(CAI_FUZZ_COMMON_SINK)
typedef struct cai_fuzz_noop_sink_context {
  size_t bytes_seen;
} cai_fuzz_noop_sink_context;

static int cai_fuzz_noop_sink_write(void *context, const void *bytes,
                                    size_t count, cai_error *error) {
  cai_fuzz_noop_sink_context *sink;

  (void)bytes;
  (void)error;
  sink = (cai_fuzz_noop_sink_context *)context;
  if (sink != NULL) {
    sink->bytes_seen += count;
  }
  return CAI_OK;
}

static int cai_fuzz_sink_new(cai_fuzz_noop_sink_context *context,
                             cai_sink **out, cai_error *error) {
  cai_sink_callbacks callbacks;

  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = cai_fuzz_noop_sink_write;
  callbacks.context = context;
  return cai_sink_from_callbacks(&callbacks, out, error);
}
#endif

#if defined(CAI_FUZZ_COMMON_DUP)
static char *cai_fuzz_dup_cstr(const unsigned char *data, size_t size) {
  char *copy;

  copy = (char *)malloc(size + 1U);
  if (copy == NULL) {
    return NULL;
  }
  if (size != 0U) {
    memcpy(copy, data, size);
  }
  copy[size] = '\0';
  return copy;
}
#endif

#if defined(CAI_FUZZ_COMMON_HEX)
static char *cai_fuzz_hex_string(const unsigned char *data, size_t size,
                                 size_t max_input_bytes) {
  static const char hex_digits[] = "0123456789abcdef";
  char *copy;
  size_t i;
  size_t used;

  used = size;
  if (used > max_input_bytes) {
    used = max_input_bytes;
  }
  copy = (char *)malloc(used * 2U + 1U);
  if (copy == NULL) {
    return NULL;
  }
  for (i = 0U; i < used; i++) {
    copy[i * 2U] = hex_digits[(data[i] >> 4) & 0x0FU];
    copy[i * 2U + 1U] = hex_digits[data[i] & 0x0FU];
  }
  copy[used * 2U] = '\0';
  return copy;
}
#endif

#endif
