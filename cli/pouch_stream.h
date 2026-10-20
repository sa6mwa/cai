#ifndef CAI_CLI_POUCH_STREAM_H
#define CAI_CLI_POUCH_STREAM_H

#include <cai/cai.h>
#include <lc/lc.h>
#include <lonejson.h>

typedef int (*cai_cli_producer)(void *context, cai_sink *sink,
                                cai_error *error);

/* Adapt a synchronous push producer to a pull source with bounded backpressure.
 * close cancels and joins the producer; context is released after the join. */
int cai_cli_stream_source(cai_cli_producer produce, void *context,
                          void (*destroy)(void *), cai_source **out,
                          cai_error *error);
typedef int (*cai_cli_consumer)(void *context, cai_source *source,
                                cai_error *error);
/* Bounded push-to-pull upload. Finish sends EOF and waits for the consumer's
 * result; close without finish aborts the source and joins the consumer. */
int cai_cli_stream_sink(cai_cli_consumer consume, void *context, cai_sink **out,
                        cai_error *error);
int cai_cli_stream_finish(cai_sink *sink, cai_error *error);
/* Establish the read snapshot before a related write starts. Waits for the
 * first bounded chunk or producer completion, never for the entire value. */
int cai_cli_stream_ready(cai_source *source, cai_error *error);
int cai_cli_error(cai_error *error, int code, const char *message);
int cai_cli_lc_error(cai_error *error, const lc_error *source);
int cai_cli_lj_error(cai_error *error, const lonejson_error *source);
lonejson_read_result cai_cli_json_read(void *context, unsigned char *bytes,
                                       size_t capacity);
lonejson_status cai_cli_json_write(void *context, const void *bytes,
                                   size_t count, lonejson_error *error);
int cai_cli_lc_source(cai_source *source, lc_source **out, cai_error *error);
int cai_cli_lc_sink(cai_sink *sink, lc_sink **out, cai_error *error);
/* Mapped JSON source owns its generator, borrows doc until source close. */
int cai_cli_mapped_source(const lonejson_map *map, const void *doc,
                          cai_source **out, cai_error *error);

#endif
