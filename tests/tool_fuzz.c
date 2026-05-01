#include <cai/cai.h>

#include <lonejson.h>
#include <stdlib.h>
#include <string.h>

typedef struct fuzz_point_args {
  double latitude;
  double longitude;
} fuzz_point_args;

typedef struct fuzz_weather_args {
  char *city;
  long long days;
  fuzz_point_args point;
} fuzz_weather_args;

typedef struct fuzz_result {
  char *summary;
} fuzz_result;

static const lonejson_field fuzz_point_fields[] = {
    LONEJSON_FIELD_F64_REQ(fuzz_point_args, latitude, "latitude"),
    LONEJSON_FIELD_F64_REQ(fuzz_point_args, longitude, "longitude")};
LONEJSON_MAP_DEFINE(fuzz_point_map, fuzz_point_args, fuzz_point_fields);

static const lonejson_field fuzz_weather_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(fuzz_weather_args, city, "city"),
    LONEJSON_FIELD_I64(fuzz_weather_args, days, "days"),
    LONEJSON_FIELD_OBJECT(fuzz_weather_args, point, "point", &fuzz_point_map)};
LONEJSON_MAP_DEFINE(fuzz_weather_map, fuzz_weather_args, fuzz_weather_fields);

static const lonejson_field fuzz_result_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(fuzz_result, summary, "summary")};
LONEJSON_MAP_DEFINE(fuzz_result_map, fuzz_result, fuzz_result_fields);

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);

static int fuzz_typed_tool(void *context, const void *params, void *result,
                           cai_error *error) {
  fuzz_result *out;

  (void)context;
  (void)params;
  (void)error;
  out = (fuzz_result *)result;
  out->summary = cai_tool_result_strdup("ok", NULL);
  return out->summary != NULL ? CAI_OK : CAI_ERR_NOMEM;
}

static int fuzz_raw_tool(void *context, const char *arguments_json,
                         cai_sink *output, cai_error *error) {
  (void)context;
  (void)arguments_json;
  return cai_sink_write(output, "{\"ok\":true}", strlen("{\"ok\":true}"),
                        error);
}

static int fuzz_sink_write(void *context, const void *bytes, size_t count,
                           cai_error *error) {
  (void)context;
  (void)bytes;
  (void)count;
  (void)error;
  return CAI_OK;
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
  static const char raw_schema[] = "{\"type\":\"object\",\"properties\":{},"
                                   "\"additionalProperties\":true}";
  cai_tool_registry *registry;
  cai_sink_callbacks callbacks;
  cai_sink *sink;
  cai_error error;
  char *json;

  json = (char *)malloc(size + 1U);
  if (json == NULL) {
    return 0;
  }
  if (size > 0U) {
    memcpy(json, data, size);
  }
  json[size] = '\0';

  cai_error_init(&error);
  registry = NULL;
  sink = NULL;
  callbacks.write = fuzz_sink_write;
  callbacks.close = NULL;
  callbacks.context = NULL;
  if (cai_tool_registry_new(&registry, &error) == CAI_OK &&
      cai_sink_from_callbacks(&callbacks, &sink, &error) == CAI_OK &&
      cai_tool_registry_register_lonejson(
          registry, "weather", "Fuzz typed weather arguments",
          &fuzz_weather_map, &fuzz_result_map, fuzz_typed_tool, NULL,
          &error) == CAI_OK &&
      cai_tool_registry_register_raw(registry, "raw", "Fuzz raw arguments",
                                     raw_schema, 0, fuzz_raw_tool, NULL,
                                     &error) == CAI_OK) {
    (void)cai_tool_registry_run(registry, "weather", json, sink, &error);
    cai_error_cleanup(&error);
    cai_error_init(&error);
    (void)cai_tool_registry_run(registry, "raw", json, sink, &error);
  }
  cai_sink_close(sink);
  cai_tool_registry_destroy(registry);
  cai_error_cleanup(&error);
  free(json);
  return 0;
}
