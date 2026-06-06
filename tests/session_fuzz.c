#define CAI_FUZZ_COMMON_SOURCE
#define CAI_FUZZ_COMMON_SINK
#define CAI_FUZZ_COMMON_HEX
#include <cai/cai.h>

#include "fuzz_common.h"

#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);

static char *cai_fuzz_build_history_json(const char *payload_hex) {
  static const char prefix[] =
      "[{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":"
      "\"input_text\",\"text\":\"";
  static const char suffix[] =
      "\"}]},{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{"
      "\"type\":\"output_text\",\"text\":\"ok\"}]}]";
  char *json;
  size_t payload_len;
  size_t json_len;

  payload_len = strlen(payload_hex);
  json_len = strlen(prefix) + payload_len + strlen(suffix);
  json = (char *)malloc(json_len + 1U);
  if (json == NULL) {
    return NULL;
  }
  memcpy(json, prefix, strlen(prefix));
  memcpy(json + strlen(prefix), payload_hex, payload_len);
  memcpy(json + strlen(prefix) + payload_len, suffix, strlen(suffix) + 1U);
  return json;
}

static char *cai_fuzz_build_state_json(const char *payload_hex) {
  static const char prefix[] =
      "{\"version\":1,\"previous_response_id\":\"resp_fuzz_state\","
      "\"history\":[{\"type\":\"message\",\"role\":\"user\",\"content\":[{"
      "\"type\":\"input_text\",\"text\":\"";
  static const char suffix[] =
      "\"}]},{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{"
      "\"type\":\"output_text\",\"text\":\"done\"}]}]}";
  char *json;
  size_t payload_len;
  size_t json_len;

  payload_len = strlen(payload_hex);
  json_len = strlen(prefix) + payload_len + strlen(suffix);
  json = (char *)malloc(json_len + 1U);
  if (json == NULL) {
    return NULL;
  }
  memcpy(json, prefix, strlen(prefix));
  memcpy(json + strlen(prefix), payload_hex, payload_len);
  memcpy(json + strlen(prefix) + payload_len, suffix, strlen(suffix) + 1U);
  return json;
}

static void cai_fuzz_session_export_copy(cai_source *source) {
  cai_sink *sink;
  cai_fuzz_noop_sink_context sink_context;
  cai_error error;

  sink = NULL;
  memset(&sink_context, 0, sizeof(sink_context));
  cai_error_init(&error);
  if (source != NULL &&
      cai_fuzz_sink_new(&sink_context, &sink, &error) == CAI_OK) {
    (void)cai_source_copy_to_sink(source, sink, &error);
    cai_sink_close(sink);
  }
  cai_error_cleanup(&error);
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
  cai_client_config client_config;
  cai_agent_config agent_config;
  cai_client *client;
  cai_agent *agent;
  cai_session *session;
  cai_session *restored;
  cai_source *input_source;
  cai_source *history_source;
  cai_source *state_source;
  cai_source *fuzz_source;
  cai_error error;
  char *payload_hex;
  char *history_json;
  char *state_json;
  size_t max_chunk;

  cai_client_config_init(&client_config);
  cai_agent_config_init(&agent_config);
  client_config.api_key = "fuzz-key";
  agent_config.model = CAI_MODEL_GPT_5_NANO;
  agent_config.enable_local_history = 1;
  agent_config.history_memory_limit = 32U;
  client = NULL;
  agent = NULL;
  session = NULL;
  restored = NULL;
  input_source = NULL;
  history_source = NULL;
  state_source = NULL;
  fuzz_source = NULL;
  payload_hex = NULL;
  history_json = NULL;
  state_json = NULL;
  max_chunk = size == 0U ? 1U : (size_t)(1U + (data[0] % 29U));
  cai_error_init(&error);

  if (cai_client_open(&client_config, &client, &error) != CAI_OK ||
      cai_client_new_agent(client, &agent_config, &agent, &error) != CAI_OK ||
      cai_agent_new_session(agent, &session, &error) != CAI_OK ||
      cai_agent_new_session(agent, &restored, &error) != CAI_OK) {
    cai_source_close(input_source);
    cai_source_close(history_source);
    cai_source_close(state_source);
    cai_source_close(fuzz_source);
    cai_session_destroy(restored);
    cai_session_destroy(session);
    cai_agent_destroy(agent);
    cai_client_close(client);
    cai_error_cleanup(&error);
    free(payload_hex);
    free(history_json);
    free(state_json);
    return 0;
  }

  payload_hex = cai_fuzz_hex_string(data, size, 16U * 1024U);
  if (payload_hex != NULL &&
      cai_fuzz_source_new((const unsigned char *)payload_hex,
                          strlen(payload_hex), max_chunk, &input_source,
                          &error) == CAI_OK) {
    (void)cai_session_add_user_text_source(session, input_source, &error);
    cai_source_close(input_source);
    input_source = NULL;
    cai_error_cleanup(&error);
    cai_error_init(&error);
    (void)cai_session_add_function_call_output(session, "call_fuzz",
                                               payload_hex, &error);
    cai_error_cleanup(&error);
    cai_error_init(&error);
  }

  if (cai_session_export_history_source(session, &history_source, &error) ==
      CAI_OK) {
    cai_fuzz_session_export_copy(history_source);
    (void)cai_source_reset(history_source, &error);
    (void)cai_session_import_history_source(restored, history_source, &error);
    cai_source_close(history_source);
    history_source = NULL;
    cai_error_cleanup(&error);
    cai_error_init(&error);
  }
  if (cai_session_export_state_source(session, &state_source, &error) ==
      CAI_OK) {
    cai_fuzz_session_export_copy(state_source);
    (void)cai_source_reset(state_source, &error);
    (void)cai_session_import_state_source(restored, state_source, &error);
    cai_source_close(state_source);
    state_source = NULL;
    cai_error_cleanup(&error);
    cai_error_init(&error);
  }

  if (cai_fuzz_source_new(data, size, max_chunk, &fuzz_source, &error) ==
      CAI_OK) {
    (void)cai_session_import_history_source(restored, fuzz_source, &error);
    cai_error_cleanup(&error);
    cai_error_init(&error);
    if (cai_source_reset(fuzz_source, &error) == CAI_OK) {
      cai_error_cleanup(&error);
      cai_error_init(&error);
      (void)cai_session_import_state_source(restored, fuzz_source, &error);
      cai_error_cleanup(&error);
      cai_error_init(&error);
    }
    cai_source_close(fuzz_source);
    fuzz_source = NULL;
  }

  if (payload_hex != NULL) {
    history_json = cai_fuzz_build_history_json(payload_hex);
    state_json = cai_fuzz_build_state_json(payload_hex);
    if (history_json != NULL &&
        cai_fuzz_source_new((const unsigned char *)history_json,
                            strlen(history_json), max_chunk, &history_source,
                            &error) == CAI_OK) {
      (void)cai_session_import_history_source(restored, history_source, &error);
      cai_source_close(history_source);
      history_source = NULL;
      cai_error_cleanup(&error);
      cai_error_init(&error);
    }
    if (state_json != NULL &&
        cai_fuzz_source_new((const unsigned char *)state_json,
                            strlen(state_json), max_chunk, &state_source,
                            &error) == CAI_OK) {
      (void)cai_session_import_state_source(restored, state_source, &error);
      cai_source_close(state_source);
      state_source = NULL;
      cai_error_cleanup(&error);
      cai_error_init(&error);
    }
  }

  cai_source_close(input_source);
  cai_source_close(history_source);
  cai_source_close(state_source);
  cai_source_close(fuzz_source);
  cai_session_destroy(restored);
  cai_session_destroy(session);
  cai_agent_destroy(agent);
  cai_client_close(client);
  cai_error_cleanup(&error);
  free(payload_hex);
  free(history_json);
  free(state_json);
  return 0;
}
