#define _POSIX_C_SOURCE 200809L
#include "pouch_stream.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct cli_stream {
  pthread_mutex_t lock;
  pthread_cond_t ready;
  pthread_t thread;
  unsigned char bytes[16384];
  size_t begin;
  size_t end;
  int done;
  int stopped;
  cai_error error;
  cai_cli_producer produce;
  void *context;
  void (*destroy)(void *);
} cli_stream;

int cai_cli_error(cai_error *error, int code, const char *message) {
  char *copy;
  if (error != NULL) {
    copy = strdup(message != NULL ? message : "lockd operation failed");
    cai_error_cleanup(error);
    error->code = code;
    error->message = copy;
  }
  return code;
}

int cai_cli_lc_error(cai_error *error, const lc_error *source) {
  char message[2048];
  int rc;
  snprintf(message, sizeof(message), "%s%s%s%s%s",
           source->message != NULL ? source->message : "lockd operation failed",
           source->detail != NULL ? ": " : "",
           source->detail != NULL ? source->detail : "",
           source->correlation_id != NULL ? "; request " : "",
           source->correlation_id != NULL ? source->correlation_id : "");
  rc = cai_cli_error(error,
                     source->code == LC_ERR_NOMEM     ? CAI_ERR_NOMEM
                     : source->code == LC_ERR_INVALID ? CAI_ERR_INVALID
                                                      : CAI_ERR_TRANSPORT,
                     message);
  if (error != NULL) {
    error->http_status = source->http_status;
  }
  return rc;
}

int cai_cli_lj_error(cai_error *error, const lonejson_error *source) {
  return cai_cli_error(error, CAI_ERR_INVALID, source->message);
}

static int stream_write(void *context, const void *data, size_t count,
                        cai_error *error) {
  cli_stream *stream;
  const unsigned char *bytes;
  size_t amount;
  stream = (cli_stream *)context;
  bytes = (const unsigned char *)data;
  pthread_mutex_lock(&stream->lock);
  while (count > 0U && !stream->stopped) {
    while (stream->end != stream->begin && !stream->stopped)
      pthread_cond_wait(&stream->ready, &stream->lock);
    if (stream->stopped)
      break;
    amount = count < sizeof(stream->bytes) ? count : sizeof(stream->bytes);
    memcpy(stream->bytes, bytes, amount);
    stream->begin = 0U;
    stream->end = amount;
    bytes += amount;
    count -= amount;
    pthread_cond_broadcast(&stream->ready);
  }
  pthread_mutex_unlock(&stream->lock);
  return count == 0U ? CAI_OK
                     : cai_cli_error(error, CAI_ERR_TRANSPORT,
                                     "stream consumer closed");
}

static void *stream_produce(void *context) {
  cli_stream *stream;
  cai_sink_callbacks callbacks;
  cai_sink *sink;
  int rc;
  stream = (cli_stream *)context;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = stream_write;
  callbacks.context = stream;
  sink = NULL;
  rc = cai_sink_from_callbacks(&callbacks, &sink, &stream->error);
  if (rc == CAI_OK)
    rc = stream->produce(stream->context, sink, &stream->error);
  cai_sink_close(sink);
  pthread_mutex_lock(&stream->lock);
  if (rc != CAI_OK && stream->error.code == CAI_OK)
    cai_cli_error(&stream->error, rc, "stream producer failed");
  stream->done = 1;
  pthread_cond_broadcast(&stream->ready);
  pthread_mutex_unlock(&stream->lock);
  return NULL;
}

static size_t stream_read(void *context, void *bytes, size_t count,
                          cai_error *error) {
  cli_stream *stream;
  size_t available;
  stream = (cli_stream *)context;
  if (count == 0U)
    return 0U;
  pthread_mutex_lock(&stream->lock);
  while (stream->begin == stream->end && !stream->done)
    pthread_cond_wait(&stream->ready, &stream->lock);
  available = stream->end - stream->begin;
  if (available > count)
    available = count;
  if (available > 0U) {
    memcpy(bytes, stream->bytes + stream->begin, available);
    stream->begin += available;
    pthread_cond_broadcast(&stream->ready);
  } else if (stream->error.code != CAI_OK) {
    cai_cli_error(error, stream->error.code, stream->error.message);
    if (error != NULL)
      error->http_status = stream->error.http_status;
  }
  pthread_mutex_unlock(&stream->lock);
  return available;
}

static void stream_close(void *context) {
  cli_stream *stream;
  stream = (cli_stream *)context;
  pthread_mutex_lock(&stream->lock);
  stream->stopped = 1;
  pthread_cond_broadcast(&stream->ready);
  pthread_mutex_unlock(&stream->lock);
  pthread_join(stream->thread, NULL);
  if (stream->destroy != NULL)
    stream->destroy(stream->context);
  cai_error_cleanup(&stream->error);
  pthread_cond_destroy(&stream->ready);
  pthread_mutex_destroy(&stream->lock);
  free(stream);
}

int cai_cli_stream_source(cai_cli_producer produce, void *context,
                          void (*destroy)(void *), cai_source **out,
                          cai_error *error) {
  cli_stream *stream;
  cai_source_callbacks callbacks;
  int rc;
  *out = NULL;
  stream = (cli_stream *)calloc(1U, sizeof(*stream));
  if (stream == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate streaming adapter");
  if (pthread_mutex_init(&stream->lock, NULL) != 0) {
    free(stream);
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "initialize stream lock");
  }
  if (pthread_cond_init(&stream->ready, NULL) != 0) {
    pthread_mutex_destroy(&stream->lock);
    free(stream);
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "initialize stream condition");
  }
  stream->produce = produce;
  stream->context = context;
  stream->destroy = destroy;
  cai_error_init(&stream->error);
  if (pthread_create(&stream->thread, NULL, stream_produce, stream) != 0) {
    pthread_cond_destroy(&stream->ready);
    pthread_mutex_destroy(&stream->lock);
    free(stream);
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "start stream producer");
  }
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.read = stream_read;
  callbacks.close = stream_close;
  callbacks.context = stream;
  rc = cai_source_from_callbacks(&callbacks, out, error);
  if (rc != CAI_OK) {
    stream->destroy = NULL;
    stream_close(stream);
  }
  return rc;
}

lonejson_read_result cai_cli_json_read(void *context, unsigned char *bytes,
                                       size_t capacity) {
  lonejson_read_result result;
  cai_error error;
  result = lonejson_default_read_result();
  cai_error_init(&error);
  result.bytes_read =
      cai_source_read((cai_source *)context, bytes, capacity, &error);
  result.eof = result.bytes_read == 0U && error.code == CAI_OK;
  result.error_code = error.code == CAI_OK ? 0 : EIO;
  cai_error_cleanup(&error);
  return result;
}

lonejson_status cai_cli_json_write(void *context, const void *bytes,
                                   size_t count, lonejson_error *error) {
  cai_error local;
  int rc;
  (void)error;
  cai_error_init(&local);
  rc = cai_sink_write((cai_sink *)context, bytes, count, &local);
  cai_error_cleanup(&local);
  return rc == CAI_OK ? LONEJSON_STATUS_OK : LONEJSON_STATUS_IO_ERROR;
}

static size_t lc_read(void *context, void *bytes, size_t count,
                      lc_error *error) {
  cai_error local;
  size_t n;
  cai_error_init(&local);
  n = cai_source_read((cai_source *)context, bytes, count, &local);
  if (local.code != CAI_OK) {
    error->code = LC_ERR_TRANSPORT;
    error->message =
        strdup(local.message != NULL ? local.message : "read failed");
  }
  cai_error_cleanup(&local);
  return n;
}

int cai_cli_lc_source(cai_source *source, lc_source **out, cai_error *error) {
  lc_error local;
  int rc;
  lc_error_init(&local);
  rc = lc_source_from_callbacks(lc_read, NULL, NULL, source, out, &local);
  if (rc != LC_OK)
    rc = cai_cli_lc_error(error, &local);
  lc_error_cleanup(&local);
  return rc;
}

static int lc_write(lc_sink *self, const void *bytes, size_t count,
                    lc_error *error) {
  cai_error local;
  int rc;
  cai_error_init(&local);
  rc = cai_sink_write((cai_sink *)self->impl, bytes, count, &local);
  if (rc != CAI_OK) {
    error->code = LC_ERR_TRANSPORT;
    error->message =
        strdup(local.message != NULL ? local.message : "write failed");
  }
  cai_error_cleanup(&local);
  return rc == CAI_OK;
}

static void lc_sink_destroy(lc_sink *self) { free(self); }

int cai_cli_lc_sink(cai_sink *sink, lc_sink **out, cai_error *error) {
  *out = (lc_sink *)calloc(1U, sizeof(**out));
  if (*out == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate sink adapter");
  (*out)->write = lc_write;
  (*out)->close = lc_sink_destroy;
  (*out)->impl = sink;
  return CAI_OK;
}
