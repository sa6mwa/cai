#define CAI_FUZZ_COMMON_HEX
#include "cai_internal.h"
#include "fuzz_common.h"

#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);

static int cai_fuzz_append(char **buffer, size_t *length, size_t *capacity,
                           const char *text, size_t text_len) {
  char *grown;
  size_t needed;

  needed = *length + text_len + 1U;
  if (needed > *capacity) {
    size_t next_capacity;

    next_capacity = *capacity == 0U ? 256U : *capacity;
    while (next_capacity < needed) {
      next_capacity *= 2U;
    }
    grown = (char *)realloc(*buffer, next_capacity);
    if (grown == NULL) {
      return 0;
    }
    *buffer = grown;
    *capacity = next_capacity;
  }
  memcpy(*buffer + *length, text, text_len);
  *length += text_len;
  (*buffer)[*length] = '\0';
  return 1;
}

static char *cai_fuzz_build_stream_sse(const unsigned char *data, size_t size) {
  static const char prefix_text[] =
      "event: response.output_text.delta\n"
      "data: {\"type\":\"response.output_text.delta\",\"item_id\":"
      "\"msg_fuzz_1\",\"output_index\":0,\"delta\":\"";
  static const char middle_text[] =
      "\"}\n\n"
      "event: response.function_call_arguments.done\n"
      "data: {\"type\":\"response.function_call_arguments.done\","
      "\"item_id\":\"fc_fuzz_1\",\"output_index\":1,\"call_id\":"
      "\"call_fuzz_1\",\"name\":\"fuzz_tool\",\"arguments\":\"{\\\"payload\\\":"
      "\\\"";
  static const char item_text[] =
      "\\\"}\"}\n\n"
      "event: response.output_item.done\n"
      "data: {\"type\":\"response.output_item.done\",\"output_index\":1,"
      "\"item\":{\"id\":\"fc_fuzz_1\",\"type\":\"function_call\","
      "\"call_id\":\"call_fuzz_1\",\"name\":\"fuzz_tool\",\"arguments\":"
      "\"{\\\"payload\\\":\\\"";
  static const char completed_text[] =
      "\\\"}\"}}\n\n"
      "event: response.completed\n"
      "data: {\"type\":\"response.completed\",\"response\":{\"id\":"
      "\"resp_fuzz_stream\",\"usage\":{\"input_tokens\":1,\"output_tokens\":1,"
      "\"total_tokens\":2}}}\n\n"
      "data: [DONE]\n\n";
  char *hex;
  char *sse;
  size_t length;
  size_t capacity;

  hex = cai_fuzz_hex_string(data, size, 4096U);
  if (hex == NULL) {
    return NULL;
  }
  sse = NULL;
  length = 0U;
  capacity = 0U;
  if (!cai_fuzz_append(&sse, &length, &capacity, prefix_text,
                       strlen(prefix_text)) ||
      !cai_fuzz_append(&sse, &length, &capacity, hex, strlen(hex)) ||
      !cai_fuzz_append(&sse, &length, &capacity, middle_text,
                       strlen(middle_text)) ||
      !cai_fuzz_append(&sse, &length, &capacity, hex, strlen(hex)) ||
      !cai_fuzz_append(&sse, &length, &capacity, item_text,
                       strlen(item_text)) ||
      !cai_fuzz_append(&sse, &length, &capacity, hex, strlen(hex)) ||
      !cai_fuzz_append(&sse, &length, &capacity, completed_text,
                       strlen(completed_text))) {
    free(hex);
    free(sse);
    return NULL;
  }
  free(hex);
  return sse;
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
  char *sse;

  (void)cai_stream_fuzz_sse(data, size);
  sse = cai_fuzz_build_stream_sse(data, size);
  if (sse != NULL) {
    (void)cai_stream_fuzz_sse((const unsigned char *)sse, strlen(sse));
    free(sse);
  }
  return 0;
}
