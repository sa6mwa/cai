#include "cai_internal.h"

#include <string.h>

struct cai_source {
  cai_source_callbacks callbacks;
};

struct cai_sink {
  cai_sink_callbacks callbacks;
};

typedef struct cai_spooled_source_context {
  lonejson_spooled spool;
} cai_spooled_source_context;

typedef struct cai_file_source_context {
  FILE *fp;
  int close_file;
} cai_file_source_context;

typedef struct cai_file_sink_context {
  FILE *fp;
  int close_file;
} cai_file_sink_context;

static size_t cai_spooled_source_read(void *context, void *buffer,
                                      size_t count, cai_error *error) {
  cai_spooled_source_context *source_context;
  lonejson_read_result result;

  source_context = (cai_spooled_source_context *)context;
  if (source_context == NULL || buffer == NULL || count == 0U) {
    return 0U;
  }
  result = lonejson_spooled_read(&source_context->spool,
                                 (unsigned char *)buffer, count);
  if (result.error_code != 0) {
    cai_set_error(error, CAI_ERR_TRANSPORT, "failed to read spooled source");
    return 0U;
  }
  return result.bytes_read;
}

static int cai_spooled_source_reset(void *context, cai_error *error) {
  cai_spooled_source_context *source_context;
  lonejson_error json_error;

  source_context = (cai_spooled_source_context *)context;
  if (source_context == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "spooled source is required");
  }
  lonejson_error_init(&json_error);
  if (lonejson_spooled_rewind(&source_context->spool, &json_error) ==
      LONEJSON_STATUS_OK) {
    return CAI_OK;
  }
  return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                              "failed to rewind spooled source",
                              json_error.message);
}

static void cai_spooled_source_close(void *context) {
  cai_spooled_source_context *source_context;

  source_context = (cai_spooled_source_context *)context;
  if (source_context == NULL) {
    return;
  }
  lonejson_spooled_cleanup(&source_context->spool);
  cai_free_mem(NULL, source_context);
}

static size_t cai_file_source_read(void *context, void *buffer, size_t count,
                                   cai_error *error) {
  cai_file_source_context *source_context;

  source_context = (cai_file_source_context *)context;
  if (source_context == NULL || source_context->fp == NULL) {
    cai_set_error(error, CAI_ERR_INVALID, "file source is closed");
    return 0U;
  }
  if (buffer == NULL || count == 0U) {
    return 0U;
  }
  count = fread(buffer, 1U, count, source_context->fp);
  if (count == 0U && ferror(source_context->fp)) {
    cai_set_error(error, CAI_ERR_TRANSPORT, "failed to read file source");
  }
  return count;
}

static int cai_file_source_reset(void *context, cai_error *error) {
  cai_file_source_context *source_context;

  source_context = (cai_file_source_context *)context;
  if (source_context == NULL || source_context->fp == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "file source is closed");
  }
  clearerr(source_context->fp);
  if (fseek(source_context->fp, 0L, SEEK_SET) != 0) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to rewind file source");
  }
  return CAI_OK;
}

static void cai_file_source_close(void *context) {
  cai_file_source_context *source_context;

  source_context = (cai_file_source_context *)context;
  if (source_context == NULL) {
    return;
  }
  if (source_context->close_file && source_context->fp != NULL) {
    fclose(source_context->fp);
  }
  source_context->fp = NULL;
  cai_free_mem(NULL, source_context);
}

static int cai_file_sink_write(void *context, const void *bytes, size_t count,
                               cai_error *error) {
  cai_file_sink_context *sink_context;

  sink_context = (cai_file_sink_context *)context;
  if (sink_context == NULL || sink_context->fp == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "file sink is closed");
  }
  if (count == 0U) {
    return CAI_OK;
  }
  if (fwrite(bytes, 1U, count, sink_context->fp) != count) {
    return cai_set_error(error, CAI_ERR_TRANSPORT, "failed to write file sink");
  }
  if (fflush(sink_context->fp) != 0) {
    return cai_set_error(error, CAI_ERR_TRANSPORT, "failed to flush file sink");
  }
  return CAI_OK;
}

static void cai_file_sink_close(void *context) {
  cai_file_sink_context *sink_context;

  sink_context = (cai_file_sink_context *)context;
  if (sink_context == NULL) {
    return;
  }
  if (sink_context->close_file && sink_context->fp != NULL) {
    fclose(sink_context->fp);
  }
  sink_context->fp = NULL;
  cai_free_mem(NULL, sink_context);
}

static int cai_output_write_string(const char *value, cai_sink *sink,
                                   cai_error *error) {
  size_t length;

  if (sink == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "sink is required");
  }
  if (value == NULL) {
    return CAI_OK;
  }
  length = strlen(value);
  if (length == 0U) {
    return CAI_OK;
  }
  return cai_sink_write(sink, value, length, error);
}

int cai_source_from_callbacks(const cai_source_callbacks *callbacks,
                              cai_source **out, cai_error *error) {
  cai_source *source;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "source output pointer is required");
  }
  *out = NULL;
  if (callbacks == NULL || callbacks->read == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "source read callback is required");
  }
  source = (cai_source *)cai_alloc(NULL, sizeof(*source));
  if (source == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate source");
  }
  source->callbacks = *callbacks;
  *out = source;
  return CAI_OK;
}

int cai_source_from_spooled(lonejson_spooled *spool, cai_source **out,
                            cai_error *error) {
  cai_spooled_source_context *context;
  cai_source_callbacks callbacks;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "source output pointer is required");
  }
  *out = NULL;
  if (spool == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "spool is required");
  }
  context = (cai_spooled_source_context *)cai_alloc(NULL, sizeof(*context));
  if (context == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate spooled source");
  }
  context->spool = *spool;
  memset(spool, 0, sizeof(*spool));
  if (cai_spooled_source_reset(context, error) != CAI_OK) {
    cai_spooled_source_close(context);
    return error != NULL ? error->code : CAI_ERR_TRANSPORT;
  }
  callbacks.read = cai_spooled_source_read;
  callbacks.reset = cai_spooled_source_reset;
  callbacks.close = cai_spooled_source_close;
  callbacks.context = context;
  rc = cai_source_from_callbacks(&callbacks, out, error);
  if (rc != CAI_OK) {
    cai_spooled_source_close(context);
  }
  return rc;
}

int cai_source_from_lc(struct lc_source *source, cai_source **out,
                       cai_error *error) {
  (void)source;
  if (out != NULL) {
    *out = NULL;
  }
  return cai_set_error(error, CAI_ERR_INVALID,
                       "lc_source interop is not enabled in this build");
}

int cai_source_file(FILE *fp, int close_file, cai_source **out,
                    cai_error *error) {
  cai_file_source_context *context;
  cai_source_callbacks callbacks;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "source output pointer is required");
  }
  *out = NULL;
  if (fp == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "file pointer is required");
  }
  context = (cai_file_source_context *)cai_alloc(NULL, sizeof(*context));
  if (context == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate file source");
  }
  context->fp = fp;
  context->close_file = close_file ? 1 : 0;
  callbacks.read = cai_file_source_read;
  callbacks.reset = cai_file_source_reset;
  callbacks.close = cai_file_source_close;
  callbacks.context = context;
  rc = cai_source_from_callbacks(&callbacks, out, error);
  if (rc != CAI_OK) {
    cai_file_source_close(context);
  }
  return rc;
}

size_t cai_source_read(cai_source *source, void *buffer, size_t count,
                       cai_error *error) {
  if (source == NULL || source->callbacks.read == NULL) {
    cai_set_error(error, CAI_ERR_INVALID, "source is not readable");
    return 0U;
  }
  return source->callbacks.read(source->callbacks.context, buffer, count,
                                error);
}

int cai_source_reset(cai_source *source, cai_error *error) {
  if (source == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "source is required");
  }
  if (source->callbacks.reset == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "source is not rewindable");
  }
  return source->callbacks.reset(source->callbacks.context, error);
}

void cai_source_close(cai_source *source) {
  if (source == NULL) {
    return;
  }
  if (source->callbacks.close != NULL) {
    source->callbacks.close(source->callbacks.context);
  }
  cai_free_mem(NULL, source);
}

int cai_source_copy_to_sink(cai_source *source, cai_sink *sink,
                            cai_error *error) {
  char buffer[4096];
  size_t nread;
  int rc;

  if (source == NULL || sink == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "source and sink are required");
  }
  for (;;) {
    nread = cai_source_read(source, buffer, sizeof(buffer), error);
    if (nread == 0U) {
      return CAI_OK;
    }
    rc = cai_sink_write(sink, buffer, nread, error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
}

int cai_sink_from_callbacks(const cai_sink_callbacks *callbacks, cai_sink **out,
                            cai_error *error) {
  cai_sink *sink;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "sink output pointer is required");
  }
  *out = NULL;
  if (callbacks == NULL || callbacks->write == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "sink write callback is required");
  }
  sink = (cai_sink *)cai_alloc(NULL, sizeof(*sink));
  if (sink == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate sink");
  }
  sink->callbacks = *callbacks;
  *out = sink;
  return CAI_OK;
}

int cai_sink_from_lc(struct lc_sink *sink, cai_sink **out, cai_error *error) {
  (void)sink;
  if (out != NULL) {
    *out = NULL;
  }
  return cai_set_error(error, CAI_ERR_INVALID,
                       "lc_sink interop is not enabled in this build");
}

int cai_sink_file(FILE *fp, int close_file, cai_sink **out, cai_error *error) {
  cai_file_sink_context *context;
  cai_sink_callbacks callbacks;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "sink output pointer is required");
  }
  *out = NULL;
  if (fp == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "file pointer is required");
  }
  context = (cai_file_sink_context *)cai_alloc(NULL, sizeof(*context));
  if (context == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate file sink");
  }
  context->fp = fp;
  context->close_file = close_file ? 1 : 0;
  callbacks.write = cai_file_sink_write;
  callbacks.close = cai_file_sink_close;
  callbacks.context = context;
  rc = cai_sink_from_callbacks(&callbacks, out, error);
  if (rc != CAI_OK) {
    cai_file_sink_close(context);
  }
  return rc;
}

int cai_sink_stdout(cai_sink **out, cai_error *error) {
  return cai_sink_file(stdout, 0, out, error);
}

int cai_sink_stderr(cai_sink **out, cai_error *error) {
  return cai_sink_file(stderr, 0, out, error);
}

int cai_sink_write(cai_sink *sink, const void *bytes, size_t count,
                   cai_error *error) {
  if (sink == NULL || sink->callbacks.write == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "sink is not writable");
  }
  return sink->callbacks.write(sink->callbacks.context, bytes, count, error);
}

void cai_sink_close(cai_sink *sink) {
  if (sink == NULL) {
    return;
  }
  if (sink->callbacks.close != NULL) {
    sink->callbacks.close(sink->callbacks.context);
  }
  cai_free_mem(NULL, sink);
}

void cai_stream_sinks_init(cai_stream_sinks *sinks) {
  if (sinks == NULL) {
    return;
  }
  memset(sinks, 0, sizeof(*sinks));
}

int cai_output_as_lc_source(cai_output *output, struct lc_source **out,
                            cai_error *error) {
  (void)output;
  if (out != NULL) {
    *out = NULL;
  }
  return cai_set_error(error, CAI_ERR_INVALID,
                       "lc_source output interop is not enabled in this build");
}

int cai_output_from_response(cai_response *response, cai_output **out,
                             cai_error *error) {
  cai_output *output;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "output pointer is required");
  }
  *out = NULL;
  if (response == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "response is required");
  }
  output = (cai_output *)cai_alloc(NULL, sizeof(*output));
  if (output == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to allocate output");
  }
  output->response = response;
  *out = output;
  return CAI_OK;
}

const cai_response *cai_output_response(const cai_output *output) {
  return output != NULL ? output->response : NULL;
}

const char *cai_output_text(const cai_output *output) {
  return output != NULL ? cai_response_output_text(output->response) : NULL;
}

const char *cai_output_refusal(const cai_output *output) {
  return output != NULL ? cai_response_refusal(output->response) : NULL;
}

const char *cai_output_raw_json(const cai_output *output) {
  return output != NULL ? cai_response_raw_json(output->response) : NULL;
}

int cai_output_write_text(const cai_output *output, cai_sink *sink,
                          cai_error *error) {
  if (output == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "output is required");
  }
  return cai_response_write_output_text(output->response, sink, error);
}

int cai_output_write_refusal(const cai_output *output, cai_sink *sink,
                             cai_error *error) {
  if (output == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "output is required");
  }
  return cai_response_write_refusal(output->response, sink, error);
}

int cai_output_write_raw_json(const cai_output *output, cai_sink *sink,
                              cai_error *error) {
  if (output == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "output is required");
  }
  return cai_output_write_string(cai_response_raw_json(output->response), sink,
                                 error);
}

int cai_output_write_json(cai_output *output, const struct lonejson_map *map,
                          void *value, cai_error *error) {
  const char *text;
  lonejson_error json_error;

  if (output == NULL || map == NULL || value == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "output, lonejson map, and value are required");
  }
  text = cai_output_text(output);
  if (text == NULL || text[0] == '\0') {
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "output text does not contain JSON");
  }
  lonejson_error_init(&json_error);
  if (lonejson_parse_cstr(map, value, text, NULL, &json_error) !=
      LONEJSON_STATUS_OK) {
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to parse output JSON",
                                json_error.message);
  }
  return CAI_OK;
}

void cai_output_destroy(cai_output *output) {
  if (output == NULL) {
    return;
  }
  cai_response_destroy(output->response);
  cai_free_mem(NULL, output);
}
