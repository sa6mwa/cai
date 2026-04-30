#include "cai_internal.h"

#include <string.h>

struct cai_source {
  cai_source_callbacks callbacks;
};

struct cai_sink {
  cai_sink_callbacks callbacks;
};

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

int cai_source_from_lc(struct lc_source *source, cai_source **out,
                       cai_error *error) {
  (void)source;
  if (out != NULL) {
    *out = NULL;
  }
  return cai_set_error(error, CAI_ERR_INVALID,
                       "lc_source interop is not enabled in this build");
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
  (void)output;
  (void)map;
  (void)value;
  return cai_set_error(error, CAI_ERR_INVALID,
                       "cai_output is not implemented yet");
}

void cai_output_destroy(cai_output *output) {
  if (output == NULL) {
    return;
  }
  cai_response_destroy(output->response);
  cai_free_mem(NULL, output);
}
