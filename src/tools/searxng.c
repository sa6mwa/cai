#include "../cai_internal.h"

#include <cai/tools/searxng.h>

#include <stdio.h>
#include <string.h>

typedef struct cai_searxng_context {
  char *base_url;
  char *search_path;
  char *engine;
  char *language;
  long timeout_ms;
  size_t response_memory_limit;
  size_t response_max_bytes;
  char *response_spool_dir;
} cai_searxng_context;

typedef struct cai_searxng_args {
  char *query;
} cai_searxng_args;

typedef struct cai_searxng_result {
  char *query;
  char *engine;
  char *title;
  char *url;
  char *snippet;
  char *source;
  long long result_count;
  long long infobox_count;
} cai_searxng_result;

typedef struct cai_searxng_item_doc {
  char *url;
  char *title;
  char *content;
  char *engine;
  char *infobox;
  char *id;
} cai_searxng_item_doc;

typedef struct cai_searxng_response_doc {
  char *query;
  long long number_of_results;
  int has_number_of_results;
  lonejson_mapped_array_stream results;
  lonejson_mapped_array_stream infoboxes;
} cai_searxng_response_doc;

typedef struct cai_searxng_spool_reader {
  lonejson_spooled cursor;
} cai_searxng_spool_reader;

typedef struct cai_searxng_parse_state {
  cai_searxng_item_doc result_item;
  cai_searxng_item_doc infobox_item;
  long long result_count;
  long long infobox_count;
  int has_result_item;
  int has_infobox_item;
} cai_searxng_parse_state;

static const lonejson_field cai_searxng_arg_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_searxng_args, query, "query")};
LONEJSON_MAP_DEFINE(cai_searxng_args_map, cai_searxng_args,
                    cai_searxng_arg_fields);

static const lonejson_field cai_searxng_result_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_searxng_result, query, "query"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_searxng_result, engine, "engine"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_searxng_result, title, "title"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_searxng_result, url, "url"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_searxng_result, snippet, "snippet"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_searxng_result, source, "source"),
    LONEJSON_FIELD_I64_REQ(cai_searxng_result, result_count, "result_count"),
    LONEJSON_FIELD_I64_REQ(cai_searxng_result, infobox_count, "infobox_count")};
LONEJSON_MAP_DEFINE(cai_searxng_result_map, cai_searxng_result,
                    cai_searxng_result_fields);

static const lonejson_field cai_searxng_item_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_searxng_item_doc, url, "url"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_searxng_item_doc, title, "title"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_searxng_item_doc, content,
                                          "content"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_searxng_item_doc, engine,
                                          "engine"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_searxng_item_doc, infobox,
                                          "infobox"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_searxng_item_doc, id, "id")};
LONEJSON_MAP_DEFINE(cai_searxng_item_map, cai_searxng_item_doc,
                    cai_searxng_item_fields);

static const lonejson_field cai_searxng_response_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_searxng_response_doc, query,
                                          "query"),
    LONEJSON_FIELD_I64_PRESENT(cai_searxng_response_doc, number_of_results,
                               has_number_of_results, "number_of_results"),
    LONEJSON_FIELD_MAPPED_ARRAY_STREAM(cai_searxng_response_doc, results,
                                       "results"),
    LONEJSON_FIELD_MAPPED_ARRAY_STREAM(cai_searxng_response_doc, infoboxes,
                                       "infoboxes")};
LONEJSON_MAP_DEFINE(cai_searxng_response_map, cai_searxng_response_doc,
                    cai_searxng_response_fields);

static void cai_searxng_context_cleanup(void *context) {
  cai_searxng_context *ctx;

  ctx = (cai_searxng_context *)context;
  if (ctx == NULL) {
    return;
  }
  cai_free_mem(NULL, ctx->base_url);
  cai_free_mem(NULL, ctx->search_path);
  cai_free_mem(NULL, ctx->engine);
  cai_free_mem(NULL, ctx->language);
  cai_free_mem(NULL, ctx->response_spool_dir);
  cai_free_mem(NULL, ctx);
}

static const char *cai_searxng_default_string(const char *value,
                                              const char *fallback) {
  return value != NULL && value[0] != '\0' ? value : fallback;
}

static int cai_searxng_context_copy_string(char **out, const char *value,
                                           cai_error *error) {
  *out = cai_strdup(NULL, value);
  if (*out == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate SearXNG tool config");
  }
  return CAI_OK;
}

static int cai_searxng_context_copy_optional_string(char **out,
                                                    const char *value,
                                                    cai_error *error) {
  if (value == NULL || value[0] == '\0') {
    *out = NULL;
    return CAI_OK;
  }
  return cai_searxng_context_copy_string(out, value, error);
}

static int cai_searxng_context_new(const cai_searxng_tool_config *config,
                                   cai_searxng_context **out,
                                   cai_error *error) {
  const char *base_url;
  const char *search_path;
  const char *language;
  cai_searxng_context *ctx;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "SearXNG context output pointer is required");
  }
  *out = NULL;
  base_url = cai_searxng_default_string(
      config != NULL ? config->base_url : NULL, CAI_SEARXNG_DEFAULT_BASE_URL);
  search_path =
      cai_searxng_default_string(config != NULL ? config->search_path : NULL,
                                 CAI_SEARXNG_DEFAULT_SEARCH_PATH);
  language = cai_searxng_default_string(
      config != NULL ? config->language : NULL, "en");
  ctx = (cai_searxng_context *)cai_alloc(NULL, sizeof(*ctx));
  if (ctx == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate SearXNG tool context");
  }
  memset(ctx, 0, sizeof(*ctx));
  rc = cai_searxng_context_copy_string(&ctx->base_url, base_url, error);
  if (rc == CAI_OK) {
    rc = cai_searxng_context_copy_string(&ctx->search_path, search_path, error);
  }
  if (rc == CAI_OK) {
    rc = cai_searxng_context_copy_optional_string(
        &ctx->engine, config != NULL ? config->engine : NULL, error);
  }
  if (rc == CAI_OK) {
    rc = cai_searxng_context_copy_string(&ctx->language, language, error);
  }
  if (rc == CAI_OK) {
    rc = cai_searxng_context_copy_optional_string(
        &ctx->response_spool_dir,
        config != NULL ? config->response_spool_dir : NULL, error);
  }
  if (rc != CAI_OK) {
    cai_searxng_context_cleanup(ctx);
    return rc;
  }
  ctx->timeout_ms =
      config != NULL && config->timeout_ms > 0L ? config->timeout_ms : 10000L;
  ctx->response_memory_limit =
      config != NULL && config->response_memory_limit != 0U
          ? config->response_memory_limit
          : 128U * 1024U;
  ctx->response_max_bytes = config != NULL && config->response_max_bytes != 0U
                                ? config->response_max_bytes
                                : 1024U * 1024U;
  *out = ctx;
  return CAI_OK;
}

static int cai_searxng_append_url_part(cai_json_builder *url,
                                       const char *base_url,
                                       const char *search_path,
                                       cai_error *error) {
  size_t base_len;
  int rc;

  base_len = strlen(base_url);
  rc = cai_json_builder_lit(url, base_url, error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (base_len > 0U && base_url[base_len - 1U] == '/' &&
      search_path[0] == '/') {
    return cai_json_builder_lit(url, search_path + 1, error);
  }
  if ((base_len == 0U || base_url[base_len - 1U] != '/') &&
      search_path[0] != '/') {
    rc = cai_json_builder_lit(url, "/", error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  return cai_json_builder_lit(url, search_path, error);
}

static int cai_searxng_append_query_param(CURL *curl, cai_json_builder *url,
                                          const char *name, const char *value,
                                          int *need_amp, cai_error *error) {
  char *escaped;
  int rc;

  escaped = curl_easy_escape(curl, value != NULL ? value : "", 0);
  if (escaped == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to URL-encode SearXNG query parameter");
  }
  rc = cai_json_builder_lit(url, *need_amp ? "&" : "?", error);
  if (rc == CAI_OK) {
    rc = cai_json_builder_lit(url, name, error);
  }
  if (rc == CAI_OK) {
    rc = cai_json_builder_lit(url, "=", error);
  }
  if (rc == CAI_OK) {
    rc = cai_json_builder_lit(url, escaped, error);
  }
  curl_free(escaped);
  *need_amp = 1;
  return rc;
}

static int cai_searxng_build_url(CURL *curl, const cai_searxng_context *ctx,
                                 const char *query, char **out,
                                 cai_error *error) {
  cai_json_builder url;
  int need_amp;
  int rc;

  memset(&url, 0, sizeof(url));
  need_amp = 0;
  rc =
      cai_searxng_append_url_part(&url, ctx->base_url, ctx->search_path, error);
  if (rc == CAI_OK) {
    rc = cai_searxng_append_query_param(curl, &url, "q", query, &need_amp,
                                        error);
  }
  if (rc == CAI_OK) {
    rc = cai_searxng_append_query_param(curl, &url, "format", "json", &need_amp,
                                        error);
  }
  if (rc == CAI_OK && ctx->engine != NULL && ctx->engine[0] != '\0') {
    rc = cai_searxng_append_query_param(curl, &url, "engines", ctx->engine,
                                        &need_amp, error);
  }
  if (rc == CAI_OK) {
    rc = cai_searxng_append_query_param(curl, &url, "language", ctx->language,
                                        &need_amp, error);
  }
  if (rc == CAI_OK) {
    rc = cai_searxng_append_query_param(curl, &url, "safesearch", "0",
                                        &need_amp, error);
  }
  if (rc == CAI_OK) {
    *out = url.data;
    return CAI_OK;
  }
  cai_free_mem(NULL, url.data);
  return rc;
}

static size_t cai_searxng_write_spool(char *ptr, size_t size, size_t nmemb,
                                      void *userdata) {
  lonejson_spooled *spool;
  lonejson_error json_error;
  size_t len;

  spool = (lonejson_spooled *)userdata;
  len = size * nmemb;
  lonejson_error_init(&json_error);
  if (lonejson_spooled_append(spool, ptr, len, &json_error) !=
      LONEJSON_STATUS_OK) {
    return 0U;
  }
  return len;
}

static int cai_searxng_fetch(const cai_searxng_context *ctx, const char *query,
                             lonejson_spooled *out, cai_error *error) {
  CURL *curl;
  CURLcode code;
  long http_status;
  char *url;
  lonejson_spool_options spool_options;
  int rc;

  url = NULL;
  curl = curl_easy_init();
  if (curl == NULL) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to initialize curl for SearXNG");
  }
  rc = cai_searxng_build_url(curl, ctx, query, &url, error);
  if (rc != CAI_OK) {
    curl_easy_cleanup(curl);
    return rc;
  }
  spool_options = lonejson_default_spool_options();
  spool_options.memory_limit = ctx->response_memory_limit;
  spool_options.max_bytes = ctx->response_max_bytes;
  spool_options.temp_dir = ctx->response_spool_dir;
  lonejson_spooled_init(out, &spool_options);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cai_searxng_write_spool);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "cai-searxng-tool/1");
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, ctx->timeout_ms);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  code = curl_easy_perform(curl);
  http_status = 0L;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
  curl_easy_cleanup(curl);
  cai_free_mem(NULL, url);
  if (code != CURLE_OK) {
    lonejson_spooled_cleanup(out);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "SearXNG request failed",
                                curl_easy_strerror(code));
  }
  if (http_status < 200L || http_status >= 300L) {
    lonejson_spooled_cleanup(out);
    return cai_set_error_http(error, CAI_ERR_SERVER, http_status,
                              "SearXNG request failed", NULL, NULL, NULL);
  }
  return CAI_OK;
}

static lonejson_read_result
cai_searxng_spool_read(void *user, unsigned char *buffer, size_t capacity) {
  cai_searxng_spool_reader *reader;

  reader = (cai_searxng_spool_reader *)user;
  return lonejson_spooled_read(&reader->cursor, buffer, capacity);
}

static void cai_searxng_item_cleanup(cai_searxng_item_doc *item) {
  if (item == NULL) {
    return;
  }
  cai_free_mem(NULL, item->url);
  cai_free_mem(NULL, item->title);
  cai_free_mem(NULL, item->content);
  cai_free_mem(NULL, item->engine);
  cai_free_mem(NULL, item->infobox);
  cai_free_mem(NULL, item->id);
  memset(item, 0, sizeof(*item));
}

static int cai_searxng_item_copy_field(char **dst, const char *src) {
  if (src == NULL) {
    *dst = NULL;
    return CAI_OK;
  }
  *dst = cai_strdup(NULL, src);
  return *dst != NULL ? CAI_OK : CAI_ERR_NOMEM;
}

static int cai_searxng_item_copy(cai_searxng_item_doc *dst,
                                 const cai_searxng_item_doc *src) {
  int rc;

  memset(dst, 0, sizeof(*dst));
  rc = cai_searxng_item_copy_field(&dst->url, src->url);
  if (rc == CAI_OK) {
    rc = cai_searxng_item_copy_field(&dst->title, src->title);
  }
  if (rc == CAI_OK) {
    rc = cai_searxng_item_copy_field(&dst->content, src->content);
  }
  if (rc == CAI_OK) {
    rc = cai_searxng_item_copy_field(&dst->engine, src->engine);
  }
  if (rc == CAI_OK) {
    rc = cai_searxng_item_copy_field(&dst->infobox, src->infobox);
  }
  if (rc == CAI_OK) {
    rc = cai_searxng_item_copy_field(&dst->id, src->id);
  }
  if (rc != CAI_OK) {
    cai_searxng_item_cleanup(dst);
  }
  return rc;
}

static void cai_searxng_parse_state_cleanup(cai_searxng_parse_state *state) {
  if (state == NULL) {
    return;
  }
  cai_searxng_item_cleanup(&state->result_item);
  cai_searxng_item_cleanup(&state->infobox_item);
}

static lonejson_status cai_searxng_result_item_cb(void *user, void *item,
                                                  lonejson_error *error) {
  cai_searxng_parse_state *state;
  int rc;

  (void)error;
  state = (cai_searxng_parse_state *)user;
  state->result_count++;
  if (!state->has_result_item) {
    rc = cai_searxng_item_copy(&state->result_item,
                               (const cai_searxng_item_doc *)item);
    if (rc != CAI_OK) {
      return LONEJSON_STATUS_ALLOCATION_FAILED;
    }
    state->has_result_item = 1;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status cai_searxng_infobox_item_cb(void *user, void *item,
                                                   lonejson_error *error) {
  cai_searxng_parse_state *state;
  int rc;

  (void)error;
  state = (cai_searxng_parse_state *)user;
  state->infobox_count++;
  if (!state->has_infobox_item) {
    rc = cai_searxng_item_copy(&state->infobox_item,
                               (const cai_searxng_item_doc *)item);
    if (rc != CAI_OK) {
      return LONEJSON_STATUS_ALLOCATION_FAILED;
    }
    state->has_infobox_item = 1;
  }
  return LONEJSON_STATUS_OK;
}

static int cai_searxng_parse(lonejson_spooled *json,
                             cai_searxng_response_doc *doc,
                             cai_searxng_parse_state *state,
                             cai_error *error) {
  cai_searxng_spool_reader reader;
  cai_searxng_item_doc result_item;
  cai_searxng_item_doc infobox_item;
  lonejson_mapped_array_stream_handler result_handler;
  lonejson_mapped_array_stream_handler infobox_handler;
  lonejson_error json_error;

  memset(doc, 0, sizeof(*doc));
  memset(state, 0, sizeof(*state));
  memset(&result_item, 0, sizeof(result_item));
  memset(&infobox_item, 0, sizeof(infobox_item));
  memset(&result_handler, 0, sizeof(result_handler));
  memset(&infobox_handler, 0, sizeof(infobox_handler));
  lonejson_init(&cai_searxng_response_map, doc);
  lonejson_init(&cai_searxng_item_map, &result_item);
  lonejson_init(&cai_searxng_item_map, &infobox_item);
  lonejson_mapped_array_stream_init(&doc->results);
  lonejson_mapped_array_stream_init(&doc->infoboxes);
  result_handler.item_map = &cai_searxng_item_map;
  result_handler.item_dst = &result_item;
  result_handler.item = cai_searxng_result_item_cb;
  result_handler.user = state;
  infobox_handler.item_map = &cai_searxng_item_map;
  infobox_handler.item_dst = &infobox_item;
  infobox_handler.item = cai_searxng_infobox_item_cb;
  infobox_handler.user = state;
  lonejson_error_init(&json_error);
  if (lonejson_mapped_array_stream_set_handler(&doc->results, &result_handler,
                                               &json_error) !=
      LONEJSON_STATUS_OK) {
    lonejson_cleanup(&cai_searxng_item_map, &result_item);
    lonejson_cleanup(&cai_searxng_item_map, &infobox_item);
    lonejson_cleanup(&cai_searxng_response_map, doc);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to configure SearXNG result stream",
                                json_error.message);
  }
  lonejson_error_init(&json_error);
  if (lonejson_mapped_array_stream_set_handler(&doc->infoboxes,
                                               &infobox_handler,
                                               &json_error) !=
      LONEJSON_STATUS_OK) {
    lonejson_cleanup(&cai_searxng_item_map, &result_item);
    lonejson_cleanup(&cai_searxng_item_map, &infobox_item);
    lonejson_cleanup(&cai_searxng_response_map, doc);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to configure SearXNG infobox stream",
                                json_error.message);
  }
  reader.cursor = *json;
  lonejson_error_init(&json_error);
  if (lonejson_spooled_rewind(&reader.cursor, &json_error) !=
      LONEJSON_STATUS_OK) {
    lonejson_cleanup(&cai_searxng_item_map, &result_item);
    lonejson_cleanup(&cai_searxng_item_map, &infobox_item);
    lonejson_cleanup(&cai_searxng_response_map, doc);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to rewind SearXNG response",
                                json_error.message);
  }
  lonejson_error_init(&json_error);
  if (lonejson_parse_reader(&cai_searxng_response_map, doc,
                            cai_searxng_spool_read, &reader, NULL,
                            &json_error) != LONEJSON_STATUS_OK) {
    lonejson_cleanup(&cai_searxng_item_map, &result_item);
    lonejson_cleanup(&cai_searxng_item_map, &infobox_item);
    lonejson_cleanup(&cai_searxng_response_map, doc);
    cai_searxng_parse_state_cleanup(state);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to parse SearXNG response JSON",
                                json_error.message);
  }
  lonejson_cleanup(&cai_searxng_item_map, &result_item);
  lonejson_cleanup(&cai_searxng_item_map, &infobox_item);
  return CAI_OK;
}

static int cai_searxng_result_copy(char **target, const char *value,
                                   cai_error *error) {
  *target = cai_tool_result_strdup(value != NULL ? value : "", error);
  return *target != NULL ? CAI_OK : CAI_ERR_NOMEM;
}

static int cai_searxng_fill_result(const cai_searxng_context *ctx,
                                   const cai_searxng_args *args,
                                   const cai_searxng_response_doc *doc,
                                   const cai_searxng_parse_state *state,
                                   cai_searxng_result *out, cai_error *error) {
  const cai_searxng_item_doc *item;
  const char *title;
  const char *url;
  const char *snippet;
  const char *source;
  const char *engine;
  int rc;

  item = NULL;
  engine = ctx->engine != NULL ? ctx->engine : "";
  if (state->has_result_item) {
    item = &state->result_item;
    title = item->title;
    url = item->url;
    snippet = item->content;
    source = "result";
    if (engine[0] == '\0' && item->engine != NULL) {
      engine = item->engine;
    }
  } else if (state->has_infobox_item) {
    item = &state->infobox_item;
    title = item->infobox != NULL ? item->infobox : item->title;
    url = item->id != NULL ? item->id : item->url;
    snippet = item->content;
    source = "infobox";
    if (engine[0] == '\0' && item->engine != NULL) {
      engine = item->engine;
    }
  } else {
    title = "";
    url = "";
    snippet = "";
    source = "none";
  }
  rc = cai_searxng_result_copy(
      &out->query, doc->query != NULL ? doc->query : args->query, error);
  if (rc == CAI_OK) {
    rc = cai_searxng_result_copy(&out->engine, engine, error);
  }
  if (rc == CAI_OK) {
    rc = cai_searxng_result_copy(&out->title, title, error);
  }
  if (rc == CAI_OK) {
    rc = cai_searxng_result_copy(&out->url, url, error);
  }
  if (rc == CAI_OK) {
    rc = cai_searxng_result_copy(&out->snippet, snippet, error);
  }
  if (rc == CAI_OK) {
    rc = cai_searxng_result_copy(&out->source, source, error);
  }
  out->result_count = state->result_count;
  out->infobox_count = state->infobox_count;
  return rc;
}

static int cai_searxng_tool_callback(void *context, const void *params,
                                     void *result, cai_error *error) {
  const cai_searxng_context *ctx;
  const cai_searxng_args *args;
  cai_searxng_result *out;
  cai_searxng_response_doc doc;
  cai_searxng_parse_state parse_state;
  lonejson_spooled body;
  int rc;

  ctx = (const cai_searxng_context *)context;
  args = (const cai_searxng_args *)params;
  out = (cai_searxng_result *)result;
  if (ctx == NULL || args == NULL || args->query == NULL ||
      args->query[0] == '\0') {
    return cai_set_error(error, CAI_ERR_INVALID, "SearXNG query is required");
  }
  rc = cai_searxng_fetch(ctx, args->query, &body, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_searxng_parse(&body, &doc, &parse_state, error);
  lonejson_spooled_cleanup(&body);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_searxng_fill_result(ctx, args, &doc, &parse_state, out, error);
  cai_searxng_parse_state_cleanup(&parse_state);
  lonejson_cleanup(&cai_searxng_response_map, &doc);
  return rc;
}

int cai_agent_register_searxng_tool(cai_agent *agent,
                                    const cai_searxng_tool_config *config,
                                    cai_error *error) {
  const char *name;
  const char *description;
  cai_searxng_context *ctx;
  cai_agent_impl *impl;
  cai_tool_schema *schema;
  int rc;

  if (agent == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  impl = CAI_AGENT_IMPL(agent);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is closed");
  }
  name = cai_searxng_default_string(config != NULL ? config->name : NULL,
                                    "searxng_search");
  description = cai_searxng_default_string(
      config != NULL ? config->description : NULL,
      "Search through the configured SearXNG endpoint.");
  rc = cai_searxng_context_new(config, &ctx, error);
  if (rc != CAI_OK) {
    return rc;
  }
  schema = NULL;
  rc = cai_tool_schema_from_map(&cai_searxng_args_map, &schema, error);
  if (rc == CAI_OK) {
    rc = cai_tool_schema_describe(schema, "query", "Search query", error);
  }
  if (rc == CAI_OK) {
    rc = cai_tool_registry_register_lonejson_owned(
        impl->tools, name, description, &cai_searxng_args_map,
        &cai_searxng_result_map, cai_searxng_tool_callback, ctx,
        cai_searxng_context_cleanup, error);
  }
  cai_tool_schema_destroy(schema);
  if (rc != CAI_OK) {
    cai_searxng_context_cleanup(ctx);
  }
  return rc;
}
