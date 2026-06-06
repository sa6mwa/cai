#include "../cai_internal.h"

#include <cai/tools/revgeo.h>

#include <stdio.h>
#include <string.h>

typedef struct cai_revgeo_context {
  char *base_url;
  char *reverse_path;
  char *user_agent;
  char *language;
  int zoom;
  long timeout_ms;
  size_t response_memory_limit;
  size_t response_max_bytes;
  char *response_spool_dir;
} cai_revgeo_context;

typedef struct cai_revgeo_args {
  double latitude;
  double longitude;
} cai_revgeo_args;

typedef struct cai_revgeo_result {
  char *provider;
  char *label;
  char *city;
  char *municipality;
  char *region;
  char *country;
  char *country_code;
  double latitude;
  int has_latitude;
  double longitude;
  int has_longitude;
} cai_revgeo_result;

typedef struct cai_revgeo_address_doc {
  char *city;
  char *town;
  char *village;
  char *hamlet;
  char *municipality;
  char *county;
  char *state;
  char *region;
  char *country;
  char *country_code;
} cai_revgeo_address_doc;

typedef struct cai_revgeo_response_doc {
  char *display_name;
  cai_revgeo_address_doc address;
} cai_revgeo_response_doc;

typedef struct cai_revgeo_spool_reader {
  lonejson_spooled cursor;
} cai_revgeo_spool_reader;

static const lonejson_field cai_revgeo_arg_fields[] = {
    LONEJSON_FIELD_F64_REQ(cai_revgeo_args, latitude, "latitude"),
    LONEJSON_FIELD_F64_REQ(cai_revgeo_args, longitude, "longitude")};
LONEJSON_MAP_DEFINE(cai_revgeo_args_map, cai_revgeo_args,
                    cai_revgeo_arg_fields);

static const lonejson_field cai_revgeo_result_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_revgeo_result, provider, "provider"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_revgeo_result, label, "label"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_revgeo_result, city, "city"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_revgeo_result, municipality,
                                    "municipality"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_revgeo_result, region, "region"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_revgeo_result, country, "country"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_revgeo_result, country_code,
                                    "country_code"),
    LONEJSON_FIELD_F64_PRESENT(cai_revgeo_result, latitude, has_latitude,
                               "latitude"),
    LONEJSON_FIELD_F64_PRESENT(cai_revgeo_result, longitude, has_longitude,
                               "longitude")};
LONEJSON_MAP_DEFINE(cai_revgeo_result_map, cai_revgeo_result,
                    cai_revgeo_result_fields);

static const lonejson_field cai_revgeo_address_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_revgeo_address_doc, city, "city"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_revgeo_address_doc, town, "town"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_revgeo_address_doc, village,
                                          "village"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_revgeo_address_doc, hamlet,
                                          "hamlet"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_revgeo_address_doc, municipality,
                                          "municipality"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_revgeo_address_doc, county,
                                          "county"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_revgeo_address_doc, state,
                                          "state"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_revgeo_address_doc, region,
                                          "region"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_revgeo_address_doc, country,
                                          "country"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_revgeo_address_doc, country_code,
                                          "country_code")};
LONEJSON_MAP_DEFINE(cai_revgeo_address_map, cai_revgeo_address_doc,
                    cai_revgeo_address_fields);

static const lonejson_field cai_revgeo_response_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_revgeo_response_doc, display_name,
                                          "display_name"),
    LONEJSON_FIELD_OBJECT(cai_revgeo_response_doc, address, "address",
                          &cai_revgeo_address_map)};
LONEJSON_MAP_DEFINE(cai_revgeo_response_map, cai_revgeo_response_doc,
                    cai_revgeo_response_fields);

static const char *cai_revgeo_default_string(const char *value,
                                             const char *fallback) {
  return value != NULL && value[0] != '\0' ? value : fallback;
}

static int cai_revgeo_copy_string(char **out, const char *value,
                                  cai_error *error) {
  *out = cai_strdup(NULL, value);
  if (*out == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate reverse-geocoding tool config");
  }
  return CAI_OK;
}

static int cai_revgeo_copy_optional_string(char **out, const char *value,
                                           cai_error *error) {
  if (value == NULL || value[0] == '\0') {
    *out = NULL;
    return CAI_OK;
  }
  return cai_revgeo_copy_string(out, value, error);
}

static void cai_revgeo_context_cleanup(void *context) {
  cai_revgeo_context *ctx;

  ctx = (cai_revgeo_context *)context;
  if (ctx == NULL) {
    return;
  }
  cai_free_mem(NULL, ctx->base_url);
  cai_free_mem(NULL, ctx->reverse_path);
  cai_free_mem(NULL, ctx->user_agent);
  cai_free_mem(NULL, ctx->language);
  cai_free_mem(NULL, ctx->response_spool_dir);
  cai_free_mem(NULL, ctx);
}

static int cai_revgeo_context_new(const cai_revgeo_tool_config *config,
                                  cai_revgeo_context **out, cai_error *error) {
  const char *base_url;
  const char *reverse_path;
  const char *user_agent;
  const char *language;
  cai_revgeo_context *ctx;
  int rc;

  if (out == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "reverse-geocoding context output is required");
  }
  *out = NULL;
  base_url = cai_revgeo_default_string(config != NULL ? config->base_url : NULL,
                                       CAI_REVGEO_DEFAULT_BASE_URL);
  reverse_path =
      cai_revgeo_default_string(config != NULL ? config->reverse_path : NULL,
                                CAI_REVGEO_DEFAULT_REVERSE_PATH);
  user_agent = cai_revgeo_default_string(
      config != NULL ? config->user_agent : NULL,
      "cai-revgeo-tool/1 (https://github.com/sa6mwa/cai)");
  language =
      cai_revgeo_default_string(config != NULL ? config->language : NULL, "en");
  ctx = (cai_revgeo_context *)cai_alloc(NULL, sizeof(*ctx));
  if (ctx == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate reverse-geocoding context");
  }
  memset(ctx, 0, sizeof(*ctx));
  rc = cai_revgeo_copy_string(&ctx->base_url, base_url, error);
  if (rc == CAI_OK) {
    rc = cai_revgeo_copy_string(&ctx->reverse_path, reverse_path, error);
  }
  if (rc == CAI_OK) {
    rc = cai_revgeo_copy_string(&ctx->user_agent, user_agent, error);
  }
  if (rc == CAI_OK) {
    rc = cai_revgeo_copy_string(&ctx->language, language, error);
  }
  if (rc == CAI_OK) {
    rc = cai_revgeo_copy_optional_string(
        &ctx->response_spool_dir,
        config != NULL ? config->response_spool_dir : NULL, error);
  }
  if (rc != CAI_OK) {
    cai_revgeo_context_cleanup(ctx);
    return rc;
  }
  ctx->zoom = config != NULL && config->zoom > 0 ? config->zoom : 10;
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

static int cai_revgeo_append_url_part(cai_buffer_builder *url,
                                      const char *base_url, const char *path,
                                      cai_error *error) {
  size_t base_len;
  int rc;

  base_len = strlen(base_url);
  rc = cai_buffer_append_cstr(url, base_url, error);
  if (rc != CAI_OK) {
    return rc;
  }
  if (base_len > 0U && base_url[base_len - 1U] == '/' && path[0] == '/') {
    return cai_buffer_append_cstr(url, path + 1, error);
  }
  if ((base_len == 0U || base_url[base_len - 1U] != '/') && path[0] != '/') {
    rc = cai_buffer_append_cstr(url, "/", error);
    if (rc != CAI_OK) {
      return rc;
    }
  }
  return cai_buffer_append_cstr(url, path, error);
}

static int cai_revgeo_append_param(CURL *curl, cai_buffer_builder *url,
                                   const char *name, const char *value,
                                   int *need_amp, cai_error *error) {
  char *escaped;
  int rc;

  escaped = curl_easy_escape(curl, value != NULL ? value : "", 0);
  if (escaped == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to URL-encode reverse-geocoding parameter");
  }
  rc = cai_buffer_append_cstr(url, *need_amp ? "&" : "?", error);
  if (rc == CAI_OK) {
    rc = cai_buffer_append_cstr(url, name, error);
  }
  if (rc == CAI_OK) {
    rc = cai_buffer_append_cstr(url, "=", error);
  }
  if (rc == CAI_OK) {
    rc = cai_buffer_append_cstr(url, escaped, error);
  }
  curl_free(escaped);
  *need_amp = 1;
  return rc;
}

static void cai_revgeo_format_decimal(char *buffer, size_t capacity,
                                      double value) {
  char *cursor;

  if (capacity == 0U) {
    return;
  }
  snprintf(buffer, capacity, "%.8f", value);
  buffer[capacity - 1U] = '\0';
  for (cursor = buffer; *cursor != '\0'; cursor++) {
    if (*cursor == ',') {
      *cursor = '.';
      return;
    }
  }
}

static int cai_revgeo_build_url(CURL *curl, const cai_revgeo_context *ctx,
                                const cai_revgeo_args *args, char **out,
                                cai_error *error) {
  cai_buffer_builder url;
  char number[64];
  int need_amp;
  int rc;

  memset(&url, 0, sizeof(url));
  need_amp = 0;
  rc =
      cai_revgeo_append_url_part(&url, ctx->base_url, ctx->reverse_path, error);
  if (rc == CAI_OK) {
    rc = cai_revgeo_append_param(curl, &url, "format", "jsonv2", &need_amp,
                                 error);
  }
  if (rc == CAI_OK) {
    cai_revgeo_format_decimal(number, sizeof(number), args->latitude);
    rc = cai_revgeo_append_param(curl, &url, "lat", number, &need_amp, error);
  }
  if (rc == CAI_OK) {
    cai_revgeo_format_decimal(number, sizeof(number), args->longitude);
    rc = cai_revgeo_append_param(curl, &url, "lon", number, &need_amp, error);
  }
  if (rc == CAI_OK) {
    rc = cai_revgeo_append_param(curl, &url, "addressdetails", "1", &need_amp,
                                 error);
  }
  if (rc == CAI_OK) {
    snprintf(number, sizeof(number), "%d", ctx->zoom);
    rc = cai_revgeo_append_param(curl, &url, "zoom", number, &need_amp, error);
  }
  if (rc == CAI_OK) {
    rc = cai_revgeo_append_param(curl, &url, "accept-language", ctx->language,
                                 &need_amp, error);
  }
  if (rc == CAI_OK) {
    *out = url.data;
    return CAI_OK;
  }
  cai_free_mem(NULL, url.data);
  return rc;
}

static size_t cai_revgeo_write_spool(char *ptr, size_t size, size_t nmemb,
                                     void *userdata) {
  lonejson_spooled *spool;
  lonejson_error json_error;
  size_t len;

  spool = (lonejson_spooled *)userdata;
  len = size * nmemb;
  lonejson_error_init(&json_error);
  if (spool->append(spool, ptr, len, &json_error) != LONEJSON_STATUS_OK) {
    return 0U;
  }
  return len;
}

static int cai_revgeo_fetch(const cai_revgeo_context *ctx,
                            const cai_revgeo_args *args, lonejson_spooled *out,
                            cai_error *error) {
  CURL *curl;
  CURLcode code;
  long http_status;
  char *url;
  lonejson *spool_runtime;
  lonejson_config spool_config;
  int rc;

  url = NULL;
  spool_runtime = NULL;
  curl = curl_easy_init();
  if (curl == NULL) {
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "failed to initialize curl for reverse geocoding");
  }
  rc = cai_revgeo_build_url(curl, ctx, args, &url, error);
  if (rc != CAI_OK) {
    curl_easy_cleanup(curl);
    return rc;
  }
  spool_config = lonejson_default_config();
  spool_config.spool_default.memory_limit = ctx->response_memory_limit;
  spool_config.spool_default.max_bytes = ctx->response_max_bytes;
  spool_config.spool_default.temp_dir = ctx->response_spool_dir;
  rc = cai_lonejson_runtime_open(&spool_config, &spool_runtime, error);
  if (rc != CAI_OK) {
    curl_easy_cleanup(curl);
    cai_free_mem(NULL, url);
    return rc;
  }
  spool_runtime->spooled_init(spool_runtime, out);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cai_revgeo_write_spool);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, ctx->user_agent);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, ctx->timeout_ms);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  code = curl_easy_perform(curl);
  http_status = 0L;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
  curl_easy_cleanup(curl);
  cai_free_mem(NULL, url);
  if (code != CURLE_OK) {
    out->cleanup(out);
    cai_lonejson_runtime_close(&spool_runtime);
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "reverse-geocoding request failed",
                                curl_easy_strerror(code));
  }
  if (http_status < 200L || http_status >= 300L) {
    out->cleanup(out);
    cai_lonejson_runtime_close(&spool_runtime);
    return cai_set_error_http(error, CAI_ERR_SERVER, http_status,
                              "reverse-geocoding request failed", NULL, NULL,
                              NULL);
  }
  cai_lonejson_runtime_close(&spool_runtime);
  return CAI_OK;
}

static lonejson_read_result
cai_revgeo_spool_read(void *user, unsigned char *buffer, size_t capacity) {
  cai_revgeo_spool_reader *reader;

  reader = (cai_revgeo_spool_reader *)user;
  return reader->cursor.read(&reader->cursor, buffer, capacity);
}

static int cai_revgeo_parse(lonejson_spooled *json,
                            cai_revgeo_response_doc *doc, cai_error *error) {
  cai_revgeo_spool_reader reader;
  lonejson_error json_error;

  memset(doc, 0, sizeof(*doc));
  CAI_LJ->init(CAI_LJ, &cai_revgeo_response_map, doc);
  reader.cursor = *json;
  lonejson_error_init(&json_error);
  if (reader.cursor.rewind(&reader.cursor, &json_error) != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_revgeo_response_map, doc);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to rewind reverse-geocoding response",
                                json_error.message);
  }
  lonejson_error_init(&json_error);
  if (CAI_LJ->parse_reader(CAI_LJ, &cai_revgeo_response_map, doc,
                           cai_revgeo_spool_read, &reader,
                           &json_error) != LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &cai_revgeo_response_map, doc);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "failed to parse reverse-geocoding response",
                                json_error.message);
  }
  return CAI_OK;
}

static const char *cai_revgeo_first_nonempty(const char *a, const char *b,
                                             const char *c, const char *d) {
  if (a != NULL && a[0] != '\0') {
    return a;
  }
  if (b != NULL && b[0] != '\0') {
    return b;
  }
  if (c != NULL && c[0] != '\0') {
    return c;
  }
  if (d != NULL && d[0] != '\0') {
    return d;
  }
  return "";
}

static int cai_revgeo_result_copy(char **target, const char *value,
                                  cai_error *error) {
  *target = cai_tool_result_strdup(value != NULL ? value : "", error);
  return *target != NULL ? CAI_OK : CAI_ERR_NOMEM;
}

static int cai_revgeo_fill_result(const cai_revgeo_args *args,
                                  const cai_revgeo_response_doc *doc,
                                  cai_revgeo_result *out, cai_error *error) {
  const cai_revgeo_address_doc *address;
  const char *city;
  const char *municipality;
  const char *region;
  int rc;

  address = &doc->address;
  city = cai_revgeo_first_nonempty(address->city, address->town,
                                   address->village, address->hamlet);
  municipality =
      cai_revgeo_first_nonempty(address->municipality, address->county, "", "");
  region = cai_revgeo_first_nonempty(address->state, address->region, "", "");
  rc = cai_revgeo_result_copy(&out->provider, "nominatim", error);
  if (rc == CAI_OK) {
    rc = cai_revgeo_result_copy(&out->label, doc->display_name, error);
  }
  if (rc == CAI_OK) {
    rc = cai_revgeo_result_copy(&out->city, city, error);
  }
  if (rc == CAI_OK) {
    rc = cai_revgeo_result_copy(&out->municipality, municipality, error);
  }
  if (rc == CAI_OK) {
    rc = cai_revgeo_result_copy(&out->region, region, error);
  }
  if (rc == CAI_OK) {
    rc = cai_revgeo_result_copy(&out->country, address->country, error);
  }
  if (rc == CAI_OK) {
    rc = cai_revgeo_result_copy(&out->country_code, address->country_code,
                                error);
  }
  out->latitude = args->latitude;
  out->has_latitude = 1;
  out->longitude = args->longitude;
  out->has_longitude = 1;
  return rc;
}

static int cai_revgeo_tool_callback(void *context, const void *params,
                                    void *result, cai_error *error) {
  const cai_revgeo_context *ctx;
  const cai_revgeo_args *args;
  cai_revgeo_result *out;
  cai_revgeo_response_doc doc;
  lonejson_spooled body;
  int rc;

  ctx = (const cai_revgeo_context *)context;
  args = (const cai_revgeo_args *)params;
  out = (cai_revgeo_result *)result;
  if (ctx == NULL || args == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "latitude and longitude are required");
  }
  if (args->latitude < -90.0 || args->latitude > 90.0 ||
      args->longitude < -180.0 || args->longitude > 180.0) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "latitude or longitude is out of range");
  }
  rc = cai_revgeo_fetch(ctx, args, &body, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_revgeo_parse(&body, &doc, error);
  body.cleanup(&body);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_revgeo_fill_result(args, &doc, out, error);
  CAI_LJ->cleanup(CAI_LJ, &cai_revgeo_response_map, &doc);
  return rc;
}

int cai_tool_registry_register_revgeo_tool(cai_tool_registry *registry,
                                           const cai_revgeo_tool_config *config,
                                           cai_error *error) {
  const char *name;
  const char *description;
  cai_revgeo_context *ctx;
  int rc;

  if (registry == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "tool registry is required");
  }
  name = cai_revgeo_default_string(config != NULL ? config->name : NULL,
                                   "reverse_geocode");
  description = cai_revgeo_default_string(
      config != NULL ? config->description : NULL,
      "Resolve latitude and longitude to structured location metadata.");
  rc = cai_revgeo_context_new(config, &ctx, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = cai_tool_registry_register_lonejson_owned(
      registry, name, description, &cai_revgeo_args_map, &cai_revgeo_result_map,
      cai_revgeo_tool_callback, ctx, cai_revgeo_context_cleanup, error);
  if (rc != CAI_OK) {
    cai_revgeo_context_cleanup(ctx);
  }
  return rc;
}

int cai_agent_register_revgeo_tool(cai_agent *agent,
                                   const cai_revgeo_tool_config *config,
                                   cai_error *error) {
  cai_agent_impl *impl;

  if (agent == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is required");
  }
  impl = CAI_AGENT_IMPL(agent);
  if (impl == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "agent is closed");
  }
  return cai_tool_registry_register_revgeo_tool(impl->tools, config, error);
}
