#include <cai/cai.h>

#include "cai_lj.h"

#include "../common.h"

#include <curl/curl.h>
#include <lonejson.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMHI_FORECAST_URL                                                      \
  "https://opendata-download-metfcst.smhi.se/api/category/snow1g/version/1/"  \
  "geotype/point/lon/%.6f/lat/%.6f/data.json?timeseries=1&parameters="        \
  "air_temperature,wind_from_direction,wind_speed,relative_humidity,"         \
  "cloud_area_fraction,precipitation_amount,"                                \
  "predominant_precipitation_type_at_surface,symbol_code"

#define OPEN_METEO_GEOCODING_URL                                               \
  "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&format=json"

typedef struct smhi_weather_args {
  char *location;
} smhi_weather_args;

typedef struct smhi_weather_result {
  char *source;
  char *location;
  char *valid_time_utc;
  char *reference_time_utc;
  double latitude;
  int has_latitude;
  double longitude;
  int has_longitude;
  double air_temperature_c;
  int has_air_temperature_c;
  double wind_speed_mps;
  int has_wind_speed_mps;
  double wind_from_direction_degrees;
  int has_wind_from_direction_degrees;
  double relative_humidity_percent;
  int has_relative_humidity_percent;
  double cloud_cover_percent;
  int has_cloud_cover_percent;
  double precipitation_amount_mm_per_hour;
  int has_precipitation_amount_mm_per_hour;
  double precipitation_type_code;
  int has_precipitation_type_code;
  double weather_symbol_code;
  int has_weather_symbol_code;
} smhi_weather_result;

typedef struct smhi_forecast_data_doc {
  double air_temperature;
  int has_air_temperature;
  double wind_from_direction;
  int has_wind_from_direction;
  double wind_speed;
  int has_wind_speed;
  double relative_humidity;
  int has_relative_humidity;
  double cloud_area_fraction;
  int has_cloud_area_fraction;
  double precipitation_amount;
  int has_precipitation_amount;
  double predominant_precipitation_type_at_surface;
  int has_predominant_precipitation_type_at_surface;
  double symbol_code;
  int has_symbol_code;
} smhi_forecast_data_doc;

typedef struct smhi_forecast_time_doc {
  char *time;
  smhi_forecast_data_doc data;
} smhi_forecast_time_doc;

typedef struct smhi_geometry_doc {
  lonejson_f64_array coordinates;
} smhi_geometry_doc;

typedef struct smhi_forecast_doc {
  char *reference_time;
  smhi_geometry_doc geometry;
  lonejson_mapped_array_stream time_series;
} smhi_forecast_doc;

typedef struct open_meteo_location_doc {
  double latitude;
  int has_latitude;
  double longitude;
  int has_longitude;
} open_meteo_location_doc;

typedef struct open_meteo_geocoding_doc {
  lonejson_mapped_array_stream results;
} open_meteo_geocoding_doc;

typedef struct smhi_spool_reader {
  lonejson_spooled cursor;
} smhi_spool_reader;

typedef struct smhi_forecast_parse_state {
  smhi_forecast_time_doc first;
  int has_first;
  long long count;
} smhi_forecast_parse_state;

typedef struct open_meteo_parse_state {
  open_meteo_location_doc first;
  int has_first;
  long long count;
} open_meteo_parse_state;

static const lonejson_field smhi_weather_arg_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(smhi_weather_args, location, "location")};
LONEJSON_MAP_DEFINE(smhi_weather_args_map, smhi_weather_args,
                    smhi_weather_arg_fields);

static const lonejson_field smhi_weather_result_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(smhi_weather_result, source, "source"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(smhi_weather_result, location, "location"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(smhi_weather_result, valid_time_utc,
                                    "valid_time_utc"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(smhi_weather_result, reference_time_utc,
                                    "reference_time_utc"),
    LONEJSON_FIELD_F64_PRESENT(smhi_weather_result, latitude, has_latitude,
                               "latitude"),
    LONEJSON_FIELD_F64_PRESENT(smhi_weather_result, longitude, has_longitude,
                               "longitude"),
    LONEJSON_FIELD_F64_PRESENT(smhi_weather_result, air_temperature_c,
                               has_air_temperature_c, "air_temperature_c"),
    LONEJSON_FIELD_F64_PRESENT(smhi_weather_result, wind_speed_mps,
                               has_wind_speed_mps, "wind_speed_mps"),
    LONEJSON_FIELD_F64_PRESENT(smhi_weather_result,
                               wind_from_direction_degrees,
                               has_wind_from_direction_degrees,
                               "wind_from_direction_degrees"),
    LONEJSON_FIELD_F64_PRESENT(smhi_weather_result, relative_humidity_percent,
                               has_relative_humidity_percent,
                               "relative_humidity_percent"),
    LONEJSON_FIELD_F64_PRESENT(smhi_weather_result, cloud_cover_percent,
                               has_cloud_cover_percent,
                               "cloud_cover_percent"),
    LONEJSON_FIELD_F64_PRESENT(smhi_weather_result,
                               precipitation_amount_mm_per_hour,
                               has_precipitation_amount_mm_per_hour,
                               "precipitation_amount_mm_per_hour"),
    LONEJSON_FIELD_F64_PRESENT(smhi_weather_result, precipitation_type_code,
                               has_precipitation_type_code,
                               "precipitation_type_code"),
    LONEJSON_FIELD_F64_PRESENT(smhi_weather_result, weather_symbol_code,
                               has_weather_symbol_code,
                               "weather_symbol_code")};
LONEJSON_MAP_DEFINE(smhi_weather_result_map, smhi_weather_result,
                    smhi_weather_result_fields);

static const lonejson_field smhi_forecast_data_fields[] = {
    LONEJSON_FIELD_F64_PRESENT(smhi_forecast_data_doc, air_temperature,
                               has_air_temperature, "air_temperature"),
    LONEJSON_FIELD_F64_PRESENT(smhi_forecast_data_doc, wind_from_direction,
                               has_wind_from_direction, "wind_from_direction"),
    LONEJSON_FIELD_F64_PRESENT(smhi_forecast_data_doc, wind_speed,
                               has_wind_speed, "wind_speed"),
    LONEJSON_FIELD_F64_PRESENT(smhi_forecast_data_doc, relative_humidity,
                               has_relative_humidity, "relative_humidity"),
    LONEJSON_FIELD_F64_PRESENT(smhi_forecast_data_doc, cloud_area_fraction,
                               has_cloud_area_fraction, "cloud_area_fraction"),
    LONEJSON_FIELD_F64_PRESENT(smhi_forecast_data_doc, precipitation_amount,
                               has_precipitation_amount,
                               "precipitation_amount"),
    LONEJSON_FIELD_F64_PRESENT(
        smhi_forecast_data_doc, predominant_precipitation_type_at_surface,
        has_predominant_precipitation_type_at_surface,
        "predominant_precipitation_type_at_surface"),
    LONEJSON_FIELD_F64_PRESENT(smhi_forecast_data_doc, symbol_code,
                               has_symbol_code, "symbol_code")};
LONEJSON_MAP_DEFINE(smhi_forecast_data_map, smhi_forecast_data_doc,
                    smhi_forecast_data_fields);

static const lonejson_field smhi_forecast_time_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(smhi_forecast_time_doc, time, "time"),
    LONEJSON_FIELD_OBJECT(smhi_forecast_time_doc, data, "data",
                          &smhi_forecast_data_map)};
LONEJSON_MAP_DEFINE(smhi_forecast_time_map, smhi_forecast_time_doc,
                    smhi_forecast_time_fields);

static const lonejson_field smhi_geometry_fields[] = {LONEJSON_FIELD_F64_ARRAY(
    smhi_geometry_doc, coordinates, "coordinates", LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(smhi_geometry_map, smhi_geometry_doc, smhi_geometry_fields);

static const lonejson_field smhi_forecast_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(smhi_forecast_doc, reference_time,
                                    "referenceTime"),
    LONEJSON_FIELD_OBJECT(smhi_forecast_doc, geometry, "geometry",
                          &smhi_geometry_map),
    LONEJSON_FIELD_MAPPED_ARRAY_STREAM(smhi_forecast_doc, time_series,
                                       "timeSeries")};
LONEJSON_MAP_DEFINE(smhi_forecast_map, smhi_forecast_doc,
                    smhi_forecast_fields);

static const lonejson_field open_meteo_location_fields[] = {
    LONEJSON_FIELD_F64_PRESENT(open_meteo_location_doc, latitude, has_latitude,
                               "latitude"),
    LONEJSON_FIELD_F64_PRESENT(open_meteo_location_doc, longitude,
                               has_longitude, "longitude")};
LONEJSON_MAP_DEFINE(open_meteo_location_map, open_meteo_location_doc,
                    open_meteo_location_fields);

static const lonejson_field open_meteo_geocoding_fields[] = {
    LONEJSON_FIELD_MAPPED_ARRAY_STREAM(open_meteo_geocoding_doc, results,
                                       "results")};
LONEJSON_MAP_DEFINE(open_meteo_geocoding_map, open_meteo_geocoding_doc,
                    open_meteo_geocoding_fields);

static const char *example_model(void) {
  const char *model;

  model = getenv("CAI_EXAMPLE_MODEL");
  if (model == NULL || model[0] == '\0') {
    return CAI_MODEL_GPT_5_NANO;
  }
  return model;
}

static int print_error(const char *operation, int rc, const cai_error *error) {
  fprintf(stderr, "%s failed: %s\n", operation,
          error->message != NULL ? error->message : cai_status_string(rc));
  if (error->detail != NULL) {
    fprintf(stderr, "detail: %s\n", error->detail);
  }
  return 1;
}

static char *smhi_strdup(const char *value) {
  size_t len;
  char *copy;

  if (value == NULL) {
    value = "";
  }
  len = strlen(value);
  copy = (char *)malloc(len + 1U);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, value, len + 1U);
  return copy;
}

static int smhi_set_error(cai_error *error, int code, const char *message,
                          const char *detail) {
  if (error != NULL) {
    cai_error_cleanup(error);
    cai_error_init(error);
    error->code = code;
    error->message = smhi_strdup(message);
    if (detail != NULL) {
      error->detail = smhi_strdup(detail);
    }
    if (error->message == NULL || (detail != NULL && error->detail == NULL)) {
      return CAI_ERR_NOMEM;
    }
  }
  return code;
}

static size_t smhi_write_spool(char *ptr, size_t size, size_t nmemb,
                               void *userdata) {
  lonejson_spooled *spool;
  lonejson_error json_error;
  size_t len;

  spool = (lonejson_spooled *)userdata;
  len = size * nmemb;
  lonejson_error_init(&json_error);
  if (spool->append(spool, ptr, len, &json_error) !=
      LONEJSON_STATUS_OK) {
    return 0U;
  }
  return len;
}

static int smhi_fetch_url(const char *url, const char *label,
                          lonejson_spooled *out, cai_error *error) {
  CURL *curl;
  CURLcode code;
  long http_status;
  int rc;

  CAI_LJ->spooled_init(CAI_LJ, out);
  curl = curl_easy_init();
  if (curl == NULL) {
    out->cleanup(out);
    return smhi_set_error(error, CAI_ERR_TRANSPORT,
                          "failed to initialize curl", NULL);
  }
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, smhi_write_spool);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "cai-smhi-weather-example/1");
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  code = curl_easy_perform(curl);
  http_status = 0L;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
  curl_easy_cleanup(curl);
  if (code != CURLE_OK) {
    out->cleanup(out);
    return smhi_set_error(error, CAI_ERR_TRANSPORT, label,
                          curl_easy_strerror(code));
  }
  if (http_status < 200L || http_status >= 300L) {
    out->cleanup(out);
    return smhi_set_error(error, CAI_ERR_SERVER, label, NULL);
  }
  rc = CAI_OK;
  return rc;
}

static int smhi_fetch_forecast(double latitude, double longitude,
                               lonejson_spooled *out, cai_error *error) {
  char url[512];

  if (latitude < -90.0 || latitude > 90.0 || longitude < -180.0 ||
      longitude > 180.0) {
    return smhi_set_error(error, CAI_ERR_INVALID,
                          "latitude or longitude is outside valid bounds",
                          NULL);
  }
  snprintf(url, sizeof(url), SMHI_FORECAST_URL, longitude, latitude);
  return smhi_fetch_url(url, "SMHI request failed", out, error);
}

static lonejson_read_result smhi_spool_read(void *user, unsigned char *buffer,
                                            size_t capacity) {
  smhi_spool_reader *reader;

  reader = (smhi_spool_reader *)user;
  return reader->cursor.read(&reader->cursor, buffer, capacity);
}

static void smhi_forecast_time_cleanup(smhi_forecast_time_doc *time_doc) {
  if (time_doc == NULL) {
    return;
  }
  free(time_doc->time);
  memset(time_doc, 0, sizeof(*time_doc));
}

static int smhi_forecast_time_copy(smhi_forecast_time_doc *dst,
                                   const smhi_forecast_time_doc *src) {
  memset(dst, 0, sizeof(*dst));
  dst->time = smhi_strdup(src->time);
  if (dst->time == NULL) {
    return CAI_ERR_NOMEM;
  }
  dst->data = src->data;
  return CAI_OK;
}

static void smhi_forecast_parse_state_cleanup(
    smhi_forecast_parse_state *state) {
  if (state == NULL) {
    return;
  }
  smhi_forecast_time_cleanup(&state->first);
}

static lonejson_status smhi_forecast_time_cb(void *user, void *item,
                                             lonejson_error *error) {
  smhi_forecast_parse_state *state;
  int rc;

  (void)error;
  state = (smhi_forecast_parse_state *)user;
  state->count++;
  if (!state->has_first) {
    rc = smhi_forecast_time_copy(&state->first,
                                 (const smhi_forecast_time_doc *)item);
    if (rc != CAI_OK) {
      return LONEJSON_STATUS_ALLOCATION_FAILED;
    }
    state->has_first = 1;
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status open_meteo_location_cb(void *user, void *item,
                                              lonejson_error *error) {
  open_meteo_parse_state *state;

  (void)error;
  state = (open_meteo_parse_state *)user;
  state->count++;
  if (!state->has_first) {
    state->first = *(const open_meteo_location_doc *)item;
    state->has_first = 1;
  }
  return LONEJSON_STATUS_OK;
}

static int smhi_parse_forecast(lonejson_spooled *json, smhi_forecast_doc *doc,
                               smhi_forecast_parse_state *state,
                               cai_error *error) {
  smhi_spool_reader reader;
  smhi_forecast_time_doc item;
  lonejson_mapped_array_stream_handler handler;
  lonejson_error json_error;

  memset(doc, 0, sizeof(*doc));
  memset(state, 0, sizeof(*state));
  memset(&item, 0, sizeof(item));
  memset(&handler, 0, sizeof(handler));
  CAI_LJ->init(CAI_LJ, &smhi_forecast_map, doc);
  CAI_LJ->init(CAI_LJ, &smhi_forecast_time_map, &item);
  lonejson_mapped_array_stream_init(&doc->time_series);
  handler.item_map = &smhi_forecast_time_map;
  handler.item_dst = &item;
  handler.item = smhi_forecast_time_cb;
  handler.user = state;
  lonejson_error_init(&json_error);
  if (doc->time_series.set_handler(&doc->time_series, &handler, &json_error) !=
      LONEJSON_STATUS_OK) {
    lonejson_cleanup(&smhi_forecast_time_map, &item);
    lonejson_cleanup(&smhi_forecast_map, doc);
    return smhi_set_error(error, CAI_ERR_PROTOCOL,
                          "failed to configure SMHI timeSeries stream",
                          json_error.message);
  }
  reader.cursor = *json;
  lonejson_error_init(&json_error);
  if (reader.cursor.rewind(&reader.cursor, &json_error) !=
      LONEJSON_STATUS_OK) {
    lonejson_cleanup(&smhi_forecast_time_map, &item);
    lonejson_cleanup(&smhi_forecast_map, doc);
    return smhi_set_error(error, CAI_ERR_PROTOCOL,
                          "failed to rewind SMHI response", json_error.message);
  }
  lonejson_error_init(&json_error);
  if (CAI_LJ->parse_reader(CAI_LJ, &smhi_forecast_map, doc, smhi_spool_read, &reader, &json_error) != LONEJSON_STATUS_OK) {
    lonejson_cleanup(&smhi_forecast_time_map, &item);
    lonejson_cleanup(&smhi_forecast_map, doc);
    smhi_forecast_parse_state_cleanup(state);
    return smhi_set_error(error, CAI_ERR_PROTOCOL,
                          "failed to parse SMHI response JSON",
                          json_error.message);
  }
  lonejson_cleanup(&smhi_forecast_time_map, &item);
  return CAI_OK;
}

static int smhi_geocode_location(const char *location, double *latitude,
                                 double *longitude, cai_error *error) {
  CURL *curl;
  char *escaped;
  char url[1024];
  lonejson_spooled body;
  open_meteo_geocoding_doc doc;
  open_meteo_location_doc item;
  open_meteo_parse_state state;
  lonejson_mapped_array_stream_handler handler;
  smhi_spool_reader reader;
  lonejson_error json_error;
  int rc;

  if (location == NULL || location[0] == '\0') {
    return smhi_set_error(error, CAI_ERR_INVALID, "location is required",
                          NULL);
  }
  curl = curl_easy_init();
  if (curl == NULL) {
    return smhi_set_error(error, CAI_ERR_TRANSPORT,
                          "failed to initialize curl", NULL);
  }
  escaped = curl_easy_escape(curl, location, 0);
  if (escaped == NULL) {
    curl_easy_cleanup(curl);
    return smhi_set_error(error, CAI_ERR_NOMEM,
                          "failed to encode location", NULL);
  }
  snprintf(url, sizeof(url), OPEN_METEO_GEOCODING_URL, escaped);
  curl_free(escaped);
  curl_easy_cleanup(curl);
  rc = smhi_fetch_url(url, "Open-Meteo geocoding request failed", &body,
                      error);
  if (rc != CAI_OK) {
    return rc;
  }

  memset(&doc, 0, sizeof(doc));
  memset(&item, 0, sizeof(item));
  memset(&state, 0, sizeof(state));
  memset(&handler, 0, sizeof(handler));
  CAI_LJ->init(CAI_LJ, &open_meteo_geocoding_map, &doc);
  CAI_LJ->init(CAI_LJ, &open_meteo_location_map, &item);
  lonejson_mapped_array_stream_init(&doc.results);
  handler.item_map = &open_meteo_location_map;
  handler.item_dst = &item;
  handler.item = open_meteo_location_cb;
  handler.user = &state;
  lonejson_error_init(&json_error);
  if (doc.results.set_handler(&doc.results, &handler, &json_error) !=
      LONEJSON_STATUS_OK) {
    lonejson_cleanup(&open_meteo_location_map, &item);
    lonejson_cleanup(&open_meteo_geocoding_map, &doc);
    body.cleanup(&body);
    return smhi_set_error(error, CAI_ERR_PROTOCOL,
                          "failed to configure Open-Meteo result stream",
                          json_error.message);
  }
  reader.cursor = body;
  lonejson_error_init(&json_error);
  if (reader.cursor.rewind(&reader.cursor, &json_error) !=
      LONEJSON_STATUS_OK) {
    lonejson_cleanup(&open_meteo_location_map, &item);
    lonejson_cleanup(&open_meteo_geocoding_map, &doc);
    body.cleanup(&body);
    return smhi_set_error(error, CAI_ERR_PROTOCOL,
                          "failed to rewind Open-Meteo geocoding response",
                          json_error.message);
  }
  lonejson_error_init(&json_error);
  if (CAI_LJ->parse_reader(CAI_LJ, &open_meteo_geocoding_map, &doc, smhi_spool_read, &reader, &json_error) !=
      LONEJSON_STATUS_OK) {
    lonejson_cleanup(&open_meteo_location_map, &item);
    lonejson_cleanup(&open_meteo_geocoding_map, &doc);
    body.cleanup(&body);
    return smhi_set_error(error, CAI_ERR_PROTOCOL,
                          "failed to parse Open-Meteo geocoding response JSON",
                          json_error.message);
  }
  lonejson_cleanup(&open_meteo_location_map, &item);
  body.cleanup(&body);
  if (!state.has_first) {
    lonejson_cleanup(&open_meteo_geocoding_map, &doc);
    return smhi_set_error(error, CAI_ERR_PROTOCOL,
                          "Open-Meteo did not resolve the location", NULL);
  }
  if (!state.first.has_latitude || !state.first.has_longitude) {
    lonejson_cleanup(&open_meteo_geocoding_map, &doc);
    return smhi_set_error(error, CAI_ERR_PROTOCOL,
                          "Open-Meteo result did not contain coordinates",
                          NULL);
  }
  *latitude = state.first.latitude;
  *longitude = state.first.longitude;
  lonejson_cleanup(&open_meteo_geocoding_map, &doc);
  return CAI_OK;
}

static int smhi_copy_result_string(char **target, const char *value,
                                   cai_error *error) {
  *target = cai_tool_result_strdup(value != NULL ? value : "", error);
  return *target == NULL ? CAI_ERR_NOMEM : CAI_OK;
}

static int smhi_weather_tool(void *context, const void *params, void *result,
                             cai_error *error) {
  const smhi_weather_args *args;
  smhi_weather_result *out;
  smhi_forecast_doc forecast;
  smhi_forecast_parse_state forecast_state;
  const smhi_forecast_time_doc *first;
  lonejson_spooled body;
  double latitude;
  double longitude;
  int rc;

  (void)context;
  args = (const smhi_weather_args *)params;
  out = (smhi_weather_result *)result;
  latitude = 0.0;
  longitude = 0.0;
  rc = smhi_geocode_location(args->location, &latitude, &longitude, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = smhi_fetch_forecast(latitude, longitude, &body, error);
  if (rc != CAI_OK) {
    return rc;
  }
  rc = smhi_parse_forecast(&body, &forecast, &forecast_state, error);
  body.cleanup(&body);
  if (rc != CAI_OK) {
    return rc;
  }
  if (!forecast_state.has_first) {
    smhi_forecast_parse_state_cleanup(&forecast_state);
    lonejson_cleanup(&smhi_forecast_map, &forecast);
    return smhi_set_error(
        error, CAI_ERR_PROTOCOL,
        "SMHI response did not contain a forecast timeSeries", NULL);
  }
  first = &forecast_state.first;
  rc = smhi_copy_result_string(
      &out->source,
      "SMHI Open Data snow1g point forecast; location resolved by Open-Meteo "
      "Geocoding API",
      error);
  if (rc == CAI_OK) {
    rc = smhi_copy_result_string(&out->location, args->location, error);
  }
  if (rc == CAI_OK) {
    rc = smhi_copy_result_string(&out->valid_time_utc, first->time, error);
  }
  if (rc == CAI_OK) {
    rc = smhi_copy_result_string(&out->reference_time_utc,
                                 forecast.reference_time, error);
  }
  if (rc != CAI_OK) {
    smhi_forecast_parse_state_cleanup(&forecast_state);
    lonejson_cleanup(&smhi_forecast_map, &forecast);
    return rc;
  }
  if (forecast.geometry.coordinates.count >= 2U) {
    out->longitude = forecast.geometry.coordinates.items[0];
    out->has_longitude = 1;
    out->latitude = forecast.geometry.coordinates.items[1];
    out->has_latitude = 1;
  }
  if (first->data.has_air_temperature) {
    out->air_temperature_c = first->data.air_temperature;
    out->has_air_temperature_c = 1;
  }
  if (first->data.has_wind_speed) {
    out->wind_speed_mps = first->data.wind_speed;
    out->has_wind_speed_mps = 1;
  }
  if (first->data.has_wind_from_direction) {
    out->wind_from_direction_degrees = first->data.wind_from_direction;
    out->has_wind_from_direction_degrees = 1;
  }
  if (first->data.has_relative_humidity) {
    out->relative_humidity_percent = first->data.relative_humidity;
    out->has_relative_humidity_percent = 1;
  }
  if (first->data.has_cloud_area_fraction) {
    out->cloud_cover_percent = first->data.cloud_area_fraction;
    out->has_cloud_cover_percent = 1;
  }
  if (first->data.has_precipitation_amount) {
    out->precipitation_amount_mm_per_hour = first->data.precipitation_amount;
    out->has_precipitation_amount_mm_per_hour = 1;
  }
  if (first->data.has_predominant_precipitation_type_at_surface) {
    out->precipitation_type_code =
        first->data.predominant_precipitation_type_at_surface;
    out->has_precipitation_type_code = 1;
  }
  if (first->data.has_symbol_code) {
    out->weather_symbol_code = first->data.symbol_code;
    out->has_weather_symbol_code = 1;
  }
  smhi_forecast_parse_state_cleanup(&forecast_state);
  lonejson_cleanup(&smhi_forecast_map, &forecast);
  return CAI_OK;
}

static void build_prompt(char *buffer, size_t capacity, const char *location) {
  snprintf(buffer, capacity,
           "Get the current SMHI forecast for %s. Mention that the weather "
           "data source is SMHI.",
           location);
}

int main(int argc, char **argv) {
  const char *location;
  char prompt[512];
  cai_client_config client_config;
  cai_agent_config agent_config;
  cai_run_options run_options;
  cai_client *client;
  cai_agent *agent;
  cai_output *output;
  cai_error error;
  char *dotenv_api_key;
  int rc;
  int exit_code;

  location = argc > 1 ? argv[1] : "Gothenburg";
  build_prompt(prompt, sizeof(prompt), location);

  cai_error_init(&error);
  cai_client_config_init(&client_config);
  cai_agent_config_init(&agent_config);
  cai_run_options_init(&run_options);
  agent_config.model = example_model();
  agent_config.developer_instructions =
      "You are a concise weather assistant. For live weather, call "
      "smhi_weather. Use only the tool result for live weather facts.";
  agent_config.max_output_tokens = 512;
  agent_config.reasoning_effort = CAI_REASONING_EFFORT_MINIMAL;
  agent_config.prompt_cache_key = "cai:example:smhi-weather:v1";
  run_options.max_tool_rounds = 2;
  client = NULL;
  agent = NULL;
  output = NULL;
  dotenv_api_key = NULL;
  exit_code = 1;

  rc = curl_global_init(CURL_GLOBAL_DEFAULT);
  if (rc != CURLE_OK) {
    fprintf(stderr, "curl_global_init failed\n");
    goto done;
  }
  rc = cai_example_load_dotenv_api_key(&client_config, &dotenv_api_key, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_load_dotenv_api_key", rc, &error);
    goto done;
  }
  rc = cai_client_open(&client_config, &client, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_client_open", rc, &error);
    goto done;
  }
  rc = client->new_agent(client, &agent_config, &agent, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("cai_client_new_agent", rc, &error);
    goto done;
  }
  rc = agent->register_tool(
      agent, "smhi_weather",
      "Fetch the nearest-gridpoint weather forecast from SMHI Open Data for a "
      "location name. The tool resolves the location to coordinates before "
      "calling SMHI.",
      &smhi_weather_args_map, &smhi_weather_result_map, smhi_weather_tool, NULL,
      &error);
  if (rc != CAI_OK) {
    exit_code = print_error("agent register smhi_weather", rc, &error);
    goto done;
  }
  rc = agent->add_user_text(agent, prompt, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("agent add prompt", rc, &error);
    goto done;
  }
  rc = agent->run_auto_output(agent, &run_options, &output, &error);
  if (rc != CAI_OK) {
    exit_code = print_error("agent run_auto_output", rc, &error);
    goto done;
  }
  printf("%s\n", cai_output_text(output) != NULL ? cai_output_text(output) : "");
  exit_code = 0;

done:
  cai_output_destroy(output);
  if (agent != NULL) {
    agent->close(agent);
  }
  if (client != NULL) {
    client->close(client);
  }
  cai_string_destroy(dotenv_api_key);
  cai_error_cleanup(&error);
  curl_global_cleanup();
  return exit_code;
}
