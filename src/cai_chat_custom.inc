/* The public push parser decodes the Chat function wrapper's input string as
 * fragments arrive. Only a bounded decoded chunk is exposed per callback;
 * completing the whole JSON document is not a prerequisite for a delta. */
typedef struct cai_chat_custom_delta_doc {
  lonejson_json_value input;
} cai_chat_custom_delta_doc;
static const lonejson_field cai_chat_custom_delta_fields[] = {
    LONEJSON_FIELD_JSON_VALUE_REQ(cai_chat_custom_delta_doc, input, "input")};
LONEJSON_MAP_DEFINE(cai_chat_custom_delta_map, cai_chat_custom_delta_doc,
                    cai_chat_custom_delta_fields);

struct cai_chat_custom_stream {
  lonejson *runtime;
  lonejson_curl_parse parser;
  lonejson_value_visitor visitor;
  cai_chat_custom_delta_doc doc;
  cai_chat_stream_state *state;
  const char *item_id;
  int output_index;
  int failure;
};

static lonejson_status cai_chat_custom_wrong_type(void *context,
                                                  lonejson_error *error) {
  cai_chat_custom_stream *custom = context;
  (void)error;
  custom->failure = cai_set_error(custom->state->error, CAI_ERR_PROTOCOL,
                                  "custom tool input must be a string");
  return LONEJSON_STATUS_CALLBACK_FAILED;
}
static lonejson_status cai_chat_custom_wrong_bool(void *context, int value,
                                                  lonejson_error *error) {
  (void)value;
  return cai_chat_custom_wrong_type(context, error);
}
static lonejson_status cai_chat_custom_chunk(void *context, const char *bytes,
                                             size_t count,
                                             lonejson_error *error) {
  cai_chat_custom_stream *custom = context;
  lonejson_spooled chunk;
  lonejson_status status;
  int rc;
  if (count == 0U)
    return LONEJSON_STATUS_OK;
  CAI_LJ->spooled_init(CAI_LJ, &chunk);
  status = chunk.append(&chunk, bytes, count, error);
  rc =
      status == LONEJSON_STATUS_OK
          ? custom->state->sinks->custom_tool_call_input_delta(
                custom->state->sinks->custom_tool_call_context, custom->item_id,
                custom->output_index, &chunk, custom->state->error)
          : cai_set_error_detail(custom->state->error, CAI_ERR_TRANSPORT,
                                 "cannot decode custom tool input chunk",
                                 error->message);
  chunk.cleanup(&chunk);
  if (rc != CAI_OK) {
    custom->failure = rc;
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return LONEJSON_STATUS_OK;
}

static void cai_chat_custom_close(cai_chat_custom_stream *custom) {
  if (custom != NULL) {
    lonejson_curl_parse_cleanup(&custom->parser);
    if (custom->runtime != NULL)
      custom->runtime->cleanup(custom->runtime, &cai_chat_custom_delta_map,
                               &custom->doc);
    lonejson_free(custom->runtime);
    cai_free_mem(NULL, custom);
  }
}
static int cai_chat_custom_error(cai_chat_custom_stream *custom) {
  return custom->failure != CAI_OK
             ? custom->failure
             : cai_set_error_detail(custom->state->error, CAI_ERR_PROTOCOL,
                                    "invalid custom tool arguments",
                                    custom->parser.error.message);
}
static int cai_chat_custom_delta(cai_chat_stream_state *state,
                                 cai_chat_tool_state *tool, int index,
                                 const lonejson_spooled *fragment) {
  cai_chat_custom_stream *custom = tool->custom;
  lonejson_spooled cursor;
  lonejson_read_result result;
  lonejson_error error;
  lonejson_config config;
  unsigned char bytes[8192];
  lonejson_error_init(&error);
  if (custom == NULL) {
    /* Heap ownership keeps parser destinations and visitor contexts stable
     * when another interleaved call grows the surrounding tools array. */
    custom = cai_alloc(NULL, sizeof(*custom));
    if (custom == NULL)
      return cai_set_error(state->error, CAI_ERR_NOMEM,
                           "allocate custom tool input decoder");
    memset(custom, 0, sizeof(*custom));
    tool->custom = custom;
    custom->state = state;
    custom->item_id = tool->id;
    custom->output_index = index;
    config = lonejson_default_config();
    /* The existing spooled string input has no 1 MiB visitor ceiling. Keep
     * that contract while decoding bounded chunks for the delta callback. */
    config.json_value_max_string_bytes = 0U;
    custom->runtime = lonejson_new(&config, &error);
    if (custom->runtime == NULL)
      return cai_set_error_detail(state->error, CAI_ERR_NOMEM,
                                  "allocate custom tool input parser",
                                  error.message);
    custom->runtime->init(custom->runtime, &cai_chat_custom_delta_map,
                          &custom->doc);
    if (lonejson_curl_parse_init(&custom->parser, custom->runtime,
                                 &cai_chat_custom_delta_map,
                                 &custom->doc) != LONEJSON_STATUS_OK)
      return cai_chat_custom_error(custom);
    custom->visitor = lonejson_default_value_visitor();
    custom->visitor.string_chunk = cai_chat_custom_chunk;
    custom->visitor.object_begin = custom->visitor.array_begin =
        custom->visitor.number_begin = custom->visitor.null_value =
            cai_chat_custom_wrong_type;
    custom->visitor.boolean_value = cai_chat_custom_wrong_bool;
    if (lonejson_json_value_set_parse_visitor(&custom->doc.input,
                                              &custom->visitor, custom,
                                              &error) != LONEJSON_STATUS_OK)
      return cai_set_error_detail(state->error, CAI_ERR_PROTOCOL,
                                  "configure custom tool input decoder",
                                  error.message);
  }
  cursor = *fragment;
  if (cursor.rewind(&cursor, &error) != LONEJSON_STATUS_OK)
    return cai_set_error(state->error, CAI_ERR_PROTOCOL,
                         "read custom tool input fragment");
  do {
    result = cursor.read(&cursor, bytes, sizeof(bytes));
    if (result.error_code != 0)
      return cai_set_error(state->error, CAI_ERR_PROTOCOL,
                           "read custom tool input fragment");
    if (result.bytes_read > 0U &&
        custom->parser.write_callback(&custom->parser, (char *)bytes, 1U,
                                      result.bytes_read) != result.bytes_read)
      return cai_chat_custom_error(custom);
  } while (!result.eof);
  return CAI_OK;
}
static int cai_chat_custom_finish(cai_chat_custom_stream *custom) {
  return custom->parser.finish(&custom->parser) == LONEJSON_STATUS_OK
             ? CAI_OK
             : cai_chat_custom_error(custom);
}
