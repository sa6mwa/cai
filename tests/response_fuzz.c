#define CAI_FUZZ_COMMON_SINK
#define CAI_FUZZ_COMMON_DUP
#define CAI_FUZZ_COMMON_HEX
#include "cai_internal.h"
#include "fuzz_common.h"

#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);

static char *cai_fuzz_build_response_json(const char *payload_hex) {
  static const char prefix[] =
      "{\"id\":\"resp_fuzz\",\"status\":\"completed\",\"model\":"
      "\"gpt-5-nano\",\"conversation_id\":\"conv_fuzz\",\"created_at\":1,"
      "\"instructions\":\"";
  static const char middle_one[] =
      "\",\"output\":[{\"id\":\"msg_fuzz_1\",\"type\":\"message\","
      "\"status\":\"completed\",\"role\":\"assistant\",\"content\":[{"
      "\"type\":\"output_text\",\"text\":\"";
  static const char middle_two[] =
      "\"}]},{\"id\":\"fc_fuzz_1\",\"type\":\"function_call\","
      "\"status\":\"completed\",\"call_id\":\"call_fuzz_1\","
      "\"name\":\"fuzz_tool\",\"arguments\":\"{\\\"payload\\\":\\\"";
  static const char suffix[] =
      "\\\"}\"}],\"usage\":{\"input_tokens\":1,"
      "\"input_cached_tokens\":0,\"output_tokens\":1,"
      "\"output_tokens_details\":{\"reasoning_tokens\":0},"
      "\"total_tokens\":2}}";
  char *json;
  size_t payload_len;
  size_t json_len;

  payload_len = strlen(payload_hex);
  json_len = strlen(prefix) + payload_len + strlen(middle_one) + payload_len +
             strlen(middle_two) + payload_len + strlen(suffix);
  json = (char *)malloc(json_len + 1U);
  if (json == NULL) {
    return NULL;
  }
  memcpy(json, prefix, strlen(prefix));
  memcpy(json + strlen(prefix), payload_hex, payload_len);
  memcpy(json + strlen(prefix) + payload_len, middle_one, strlen(middle_one));
  memcpy(json + strlen(prefix) + payload_len + strlen(middle_one), payload_hex,
         payload_len);
  memcpy(json + strlen(prefix) + payload_len + strlen(middle_one) + payload_len,
         middle_two, strlen(middle_two));
  memcpy(json + strlen(prefix) + payload_len + strlen(middle_one) +
             payload_len + strlen(middle_two),
         payload_hex, payload_len);
  memcpy(json + strlen(prefix) + payload_len + strlen(middle_one) +
             payload_len + strlen(middle_two) + payload_len,
         suffix, strlen(suffix) + 1U);
  return json;
}

static void cai_fuzz_run_response_parse(const char *json) {
  cai_response *response;
  cai_output *output;
  cai_sink *sink;
  cai_fuzz_noop_sink_context sink_context;
  lonejson_spooled output_items;
  const lonejson_spooled *arguments;
  char *output_items_json;
  cai_error error;
  size_t output_items_len;

  response = NULL;
  output = NULL;
  sink = NULL;
  output_items_json = NULL;
  output_items_len = 0U;
  memset(&sink_context, 0, sizeof(sink_context));
  memset(&output_items, 0, sizeof(output_items));
  cai_error_init(&error);
  if (cai_response_parse_json(json, &response, &error) == CAI_OK) {
    (void)cai_fuzz_sink_new(&sink_context, &sink, &error);
    if (sink != NULL) {
      (void)cai_response_write_output_text(response, sink, &error);
      cai_error_cleanup(&error);
      cai_error_init(&error);
      (void)cai_response_write_refusal(response, sink, &error);
      cai_sink_close(sink);
      sink = NULL;
    }
    (void)cai_response_output_items_json(response, &output_items_json, &error);
    free(output_items_json);
    output_items_json = NULL;
    cai_error_cleanup(&error);
    cai_error_init(&error);
    if (cai_response_output_items_spool(response, &output_items,
                                        &output_items_len, &error) == CAI_OK) {
      output_items.cleanup(&output_items);
      memset(&output_items, 0, sizeof(output_items));
    }
    arguments = cai_response_tool_call_arguments_spooled(response, 0U);
    if (arguments != NULL) {
      lonejson_spooled cursor;
      volatile size_t size_seen;

      cursor = *arguments;
      size_seen = cursor.size_fn(&cursor);
      (void)size_seen;
    }
    cai_error_cleanup(&error);
    cai_error_init(&error);
    if (cai_output_from_response(response, &output, &error) == CAI_OK) {
      (void)cai_fuzz_sink_new(&sink_context, &sink, &error);
      if (sink != NULL) {
        (void)cai_output_write_text(output, sink, &error);
        cai_error_cleanup(&error);
        cai_error_init(&error);
        (void)cai_output_write_raw_json(output, sink, &error);
        cai_sink_close(sink);
        sink = NULL;
      }
      cai_output_destroy(output);
      output = NULL;
      response = NULL;
    }
  }
  cai_output_destroy(output);
  cai_sink_close(sink);
  cai_response_destroy(response);
  if (output_items.append != NULL) {
    output_items.cleanup(&output_items);
  }
  free(output_items_json);
  cai_error_cleanup(&error);
}

static void cai_fuzz_run_request_serialize(const char *payload_hex) {
  cai_response_create_params *params;
  lonejson_spooled text_spool;
  lonejson_spooled json_spool;
  cai_error error;
  lonejson_error json_error;
  char *json;
  size_t json_len;
  int has_text_spool;
  int has_json_spool;

  params = NULL;
  json = NULL;
  json_len = 0U;
  memset(&text_spool, 0, sizeof(text_spool));
  memset(&json_spool, 0, sizeof(json_spool));
  has_text_spool = 0;
  has_json_spool = 0;
  cai_error_init(&error);
  if (cai_response_create_params_new(&params, &error) != CAI_OK) {
    cai_error_cleanup(&error);
    return;
  }
  (void)cai_response_create_params_set_model(params, CAI_MODEL_GPT_5_NANO,
                                             &error);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  (void)cai_response_create_params_set_instructions(params, payload_hex,
                                                    &error);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  CAI_LJ->spooled_init(CAI_LJ, &text_spool);
  has_text_spool = 1;
  lonejson_error_init(&json_error);
  if (text_spool.append(&text_spool, payload_hex, strlen(payload_hex),
                        &json_error) == LONEJSON_STATUS_OK) {
    (void)cai_response_create_params_add_text_spooled(params, "user",
                                                      &text_spool, &error);
    has_text_spool = text_spool.cleanup != NULL;
    cai_error_cleanup(&error);
    cai_error_init(&error);
  }
  (void)cai_response_create_params_add_function_tool(
      params, "fuzz_tool", "Fuzz tool",
      "{\"type\":\"object\",\"properties\":{},\"required\":[],"
      "\"additionalProperties\":true}",
      0, &error);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  (void)cai_response_create_params_serialize_json(params, &json, &json_len,
                                                  &error);
  free(json);
  json = NULL;
  cai_error_cleanup(&error);
  cai_error_init(&error);
  if (cai_response_create_params_spool_json(params, 0, &json_spool, &json_len,
                                            &error) == CAI_OK) {
    has_json_spool = 1;
    json_spool.cleanup(&json_spool);
    memset(&json_spool, 0, sizeof(json_spool));
    has_json_spool = 0;
  }
  if (has_json_spool) {
    json_spool.cleanup(&json_spool);
  }
  if (has_text_spool) {
    text_spool.cleanup(&text_spool);
  }
  cai_response_create_params_destroy(params);
  cai_error_cleanup(&error);
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
  static const char conversation_json[] =
      "{\"id\":\"conv_fuzz\",\"object\":\"conversation\"}";
  char *json;
  char *payload_hex;
  char *response_json;
  cai_input_item_list *list;
  cai_conversation *conversation;
  cai_conversation_item *item;
  cai_error error;

  json = cai_fuzz_dup_cstr(data, size);
  if (json == NULL) {
    return 0;
  }
  cai_error_init(&error);
  list = NULL;
  conversation = NULL;
  item = NULL;
  (void)cai_input_item_list_parse_json(json, &list, &error);
  cai_input_item_list_destroy(list);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  (void)cai_conversation_item_parse_json(json, &item, &error);
  cai_conversation_item_destroy(item);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  (void)cai_conversation_parse_json(json, &conversation, &error);
  cai_conversation_destroy(conversation);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  (void)cai_conversation_parse_json(conversation_json, &conversation, &error);
  cai_conversation_destroy(conversation);
  cai_error_cleanup(&error);

  cai_fuzz_run_response_parse(json);
  payload_hex = cai_fuzz_hex_string(data, size, 16U * 1024U);
  if (payload_hex != NULL) {
    response_json = cai_fuzz_build_response_json(payload_hex);
    if (response_json != NULL) {
      cai_fuzz_run_response_parse(response_json);
      free(response_json);
    }
    cai_fuzz_run_request_serialize(payload_hex);
    free(payload_hex);
  }

  free(json);
  return 0;
}
