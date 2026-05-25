#include "cai_internal.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAI_LJ_STREAM_LIMIT CAI_DEFAULT_SSE_EVENT_LIMIT

typedef struct cai_lj_global_runtime {
  pthread_once_t once;
  lonejson *runtime;
  lonejson_error error;
} cai_lj_global_runtime;

static cai_lj_global_runtime cai_lj_default_runtime = {PTHREAD_ONCE_INIT};
static cai_lj_global_runtime cai_lj_preserve_runtime = {PTHREAD_ONCE_INIT};
static cai_lj_global_runtime cai_lj_stream_runtime = {PTHREAD_ONCE_INIT};
static pthread_once_t cai_lj_cleanup_once = PTHREAD_ONCE_INIT;

static void cai_lj_cleanup_all(void) {
  cai_lonejson_runtime_close(&cai_lj_default_runtime.runtime);
  cai_lonejson_runtime_close(&cai_lj_preserve_runtime.runtime);
  cai_lonejson_runtime_close(&cai_lj_stream_runtime.runtime);
}

static void cai_lj_register_cleanup(void) {
  (void)atexit(cai_lj_cleanup_all);
}

static lonejson *cai_lj_require_runtime(cai_lj_global_runtime *state) {
  if (state->runtime == NULL) {
    fprintf(stderr, "cai: failed to initialize lonejson runtime: %s\n",
            state->error.message[0] != '\0' ? state->error.message
                                            : "unknown error");
    abort();
  }
  return state->runtime;
}

static void cai_lj_init_runtime_default(void) {
  lonejson_error_init(&cai_lj_default_runtime.error);
  cai_lj_default_runtime.runtime =
      lonejson_new(NULL, &cai_lj_default_runtime.error);
}

static void cai_lj_init_runtime_preserve(void) {
  lonejson_config config;

  config = lonejson_default_config();
  config.clear_destination_by_default = 0;
  lonejson_error_init(&cai_lj_preserve_runtime.error);
  cai_lj_preserve_runtime.runtime =
      lonejson_new(&config, &cai_lj_preserve_runtime.error);
}

static void cai_lj_init_runtime_stream(void) {
  lonejson_config config;

  config = lonejson_default_config();
  config.clear_destination_by_default = 0;
  config.max_alloc_bytes = CAI_LJ_STREAM_LIMIT;
  config.max_dynamic_string_bytes = CAI_LJ_STREAM_LIMIT;
  config.json_value_max_total_bytes = CAI_LJ_STREAM_LIMIT;
  config.json_value_max_string_bytes = CAI_LJ_STREAM_LIMIT;
  config.json_value_max_key_bytes = CAI_LJ_STREAM_LIMIT;
  config.json_value_max_number_bytes = CAI_LJ_STREAM_LIMIT;
  config.spool_default.memory_limit = 64U * 1024U;
  config.spool_default.max_bytes = CAI_LJ_STREAM_LIMIT;
  config.spool_blob.memory_limit = 64U * 1024U;
  config.spool_blob.max_bytes = CAI_LJ_STREAM_LIMIT;
  config.spool_large_text.memory_limit = 64U * 1024U;
  config.spool_large_text.max_bytes = CAI_LJ_STREAM_LIMIT;
  lonejson_error_init(&cai_lj_stream_runtime.error);
  cai_lj_stream_runtime.runtime =
      lonejson_new(&config, &cai_lj_stream_runtime.error);
}

lonejson *cai_lonejson_runtime(void) {
  pthread_once(&cai_lj_cleanup_once, cai_lj_register_cleanup);
  pthread_once(&cai_lj_default_runtime.once, cai_lj_init_runtime_default);
  return cai_lj_require_runtime(&cai_lj_default_runtime);
}

lonejson *cai_lonejson_runtime_preserve(void) {
  pthread_once(&cai_lj_cleanup_once, cai_lj_register_cleanup);
  pthread_once(&cai_lj_preserve_runtime.once, cai_lj_init_runtime_preserve);
  return cai_lj_require_runtime(&cai_lj_preserve_runtime);
}

lonejson *cai_lonejson_runtime_stream(void) {
  pthread_once(&cai_lj_cleanup_once, cai_lj_register_cleanup);
  pthread_once(&cai_lj_stream_runtime.once, cai_lj_init_runtime_stream);
  return cai_lj_require_runtime(&cai_lj_stream_runtime);
}

int cai_lonejson_runtime_open(const lonejson_config *config,
                              lonejson **out_runtime, cai_error *error) {
  lonejson_error json_error;

  if (out_runtime == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "lonejson runtime output pointer is required");
  }
  *out_runtime = NULL;
  lonejson_error_init(&json_error);
  *out_runtime = lonejson_new(config, &json_error);
  if (*out_runtime == NULL) {
    return cai_set_error_detail(error, CAI_ERR_NOMEM,
                                "failed to initialize lonejson runtime",
                                json_error.message);
  }
  return CAI_OK;
}

void cai_lonejson_runtime_close(lonejson **runtime) {
  if (runtime != NULL && *runtime != NULL) {
    (*runtime)->free(*runtime);
    *runtime = NULL;
  }
}
