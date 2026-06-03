#include <cai/cai.h>
#include <cai/tools/read.h>

#include <limits.h>
#include <lonejson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

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

static int fuzz_read_root(char *root, size_t root_size) {
  static char cached_root[PATH_MAX];
  static int initialized;
  char template_path[PATH_MAX];

  if (!initialized) {
    snprintf(template_path, sizeof(template_path),
             "/tmp/cai-read-fuzz-%ld-XXXXXX", (long)getpid());
    if (mkdtemp(template_path) == NULL) {
      return 0;
    }
    memcpy(cached_root, template_path, strlen(template_path) + 1U);
    initialized = 1;
  }
  if (strlen(cached_root) + 1U > root_size) {
    return 0;
  }
  memcpy(root, cached_root, strlen(cached_root) + 1U);
  return 1;
}

static int fuzz_write_read_fixture(const char *root, const unsigned char *data,
                                   size_t size) {
  char path[PATH_MAX];
  FILE *fp;
  int n;

  n = snprintf(path, sizeof(path), "%s/fuzz.bin", root);
  if (n < 0 || (size_t)n >= sizeof(path)) {
    return 0;
  }
  fp = fopen(path, "wb");
  if (fp == NULL) {
    return 0;
  }
  if (size != 0U && fwrite(data, 1U, size, fp) != size) {
    fclose(fp);
    return 0;
  }
  fclose(fp);
  return 1;
}

static void fuzz_run_read_tools(cai_tool_registry *registry, cai_sink *sink,
                                const unsigned char *data, size_t size,
                                cai_error *error) {
  static const char read_full[] = "{\"path\":\"fuzz.bin\"}";
  static const char read_one[] = "{\"path\":\"fuzz.bin\",\"max_bytes\":1}";
  static const char read_two[] = "{\"path\":\"fuzz.bin\",\"max_bytes\":2}";
  static const char read_range[] =
      "{\"path\":\"fuzz.bin\",\"start_line\":2,\"end_line\":3,"
      "\"max_bytes\":7}";
  static const char list_root[] =
      "{\"path\":\".\",\"recursive\":true,\"include_hidden\":true}";
  cai_read_tool_config config;
  char root[PATH_MAX];

  if (!fuzz_read_root(root, sizeof(root)) ||
      !fuzz_write_read_fixture(root, data, size)) {
    return;
  }

  memset(&config, 0, sizeof(config));
  config.root_path = root;
  config.default_workdir = root;
  config.content_memory_limit = 8U;
  config.content_max_bytes = 64U;

  if (cai_tool_registry_register_read_tool(registry, &config, error) !=
      CAI_OK) {
    return;
  }
  if (cai_tool_registry_register_list_files_tool(registry, &config, error) !=
      CAI_OK) {
    return;
  }

  (void)cai_tool_registry_run(registry, CAI_READ_DEFAULT_TOOL_NAME, read_full,
                              sink, error);
  cai_error_cleanup(error);
  cai_error_init(error);
  (void)cai_tool_registry_run(registry, CAI_READ_DEFAULT_TOOL_NAME, read_one,
                              sink, error);
  cai_error_cleanup(error);
  cai_error_init(error);
  (void)cai_tool_registry_run(registry, CAI_READ_DEFAULT_TOOL_NAME, read_two,
                              sink, error);
  cai_error_cleanup(error);
  cai_error_init(error);
  (void)cai_tool_registry_run(registry, CAI_READ_DEFAULT_TOOL_NAME, read_range,
                              sink, error);
  cai_error_cleanup(error);
  cai_error_init(error);
  (void)cai_tool_registry_run(registry, CAI_LIST_FILES_DEFAULT_TOOL_NAME,
                              list_root, sink, error);
  cai_error_cleanup(error);
  cai_error_init(error);
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
    cai_error_cleanup(&error);
    cai_error_init(&error);
    fuzz_run_read_tools(registry, sink, data, size, &error);
  }
  cai_sink_close(sink);
  cai_tool_registry_destroy(registry);
  cai_error_cleanup(&error);
  free(json);
  return 0;
}
