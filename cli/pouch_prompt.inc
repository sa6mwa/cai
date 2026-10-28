/* Prompt recall is independent of the conversation journal. Keep one receiver
 * open for the active session and use synchronous JSON generation/parsing on
 * the caller's thread. There is no file backend or per-entry stream bridge. */
typedef struct prompt_record {
  const char *prompt;
} prompt_record;
static const lonejson_field prompt_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(prompt_record, prompt, "prompt")};
LONEJSON_MAP_DEFINE(prompt_map, prompt_record, prompt_fields);

void cai_cli_pouch_prompt_close(cai_cli_pouch *pouch) {
  lc_collection_close(pouch->prompt_history);
  pouch->prompt_history = NULL;
}
int cai_cli_pouch_prompt_open(cai_cli_pouch *pouch, const char *id,
                              cai_error *error) {
  lc_collection *collection = NULL;
  int rc;
  if (!session_component(id) || strlen(id) >= CAI_AGENT_SESSION_ID_MAX)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "invalid prompt-history conversation ID");
  rc = open_history_collection(pouch, id, "prompts", 1, 0, &collection, error);
  if (rc == CAI_OK) {
    cai_cli_pouch_prompt_close(pouch);
    pouch->prompt_history = collection;
  }
  return rc;
}

int cai_cli_pouch_prompt_set_limit(cai_cli_pouch *pouch, unsigned int limit,
                                   cai_error *error) {
  lc_collection_info info;
  lc_collection_metadata_req request;
  lc_error local;
  int rc;
  if (pouch->prompt_history == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "prompt-history collection is not open");
  lc_error_init(&local);
  rc = pouch->prompt_history->describe(pouch->prompt_history, &info, &local);
  if (rc == LC_OK && info.max_entries != (uint64_t)limit) {
    lc_collection_metadata_req_init(&request);
    request.has_max_entries = 1;
    request.max_entries = limit;
    rc = pouch->prompt_history->metadata(pouch->prompt_history, &request, &info,
                                         &local);
  }
  if (rc != LC_OK)
    rc = cai_cli_lc_error(error, &local);
  lc_error_cleanup(&local);
  return rc;
}

int cai_cli_pouch_prompt_configure(cai_cli_pouch *pouch, const char *id,
                                   unsigned int limit, cai_error *error) {
  int rc = CAI_OK;
  if (!session_component(id) || strlen(id) >= CAI_AGENT_SESSION_ID_MAX)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "invalid prompt-history conversation ID");
  if (pouch->prompt_history == NULL) {
    if (limit != 0U)
      rc = cai_cli_pouch_prompt_open(pouch, id, error);
    else
      rc = open_history_collection(pouch, id, "prompts", 0, 1,
                                   &pouch->prompt_history, error);
  }
  if (rc == CAI_OK && pouch->prompt_history != NULL)
    rc = cai_cli_pouch_prompt_set_limit(pouch, limit, error);
  if (rc == CAI_OK && limit == 0U)
    cai_cli_pouch_prompt_close(pouch);
  return rc;
}

typedef struct prompt_read {
  lonejson *json;
  lonejson_curl_parse parser;
  prompt_record record;
  int initialized, rc;
  int (*emit)(void *, const char *, cai_error *);
  void *context;
  cai_error *error;
} prompt_read;
static void prompt_read_cleanup(prompt_read *read) {
  lonejson_curl_parse_cleanup(&read->parser);
  lonejson_cleanup(&prompt_map, &read->record);
  memset(&read->parser, 0, sizeof(read->parser));
  memset(&read->record, 0, sizeof(read->record));
  read->initialized = 0;
}
static int prompt_read_write(lc_sink *sink, const void *bytes, size_t count,
                             lc_error *error) {
  prompt_read *read = sink->impl;
  const unsigned char *input = bytes;
  char chunk[4096];
  size_t amount;
  (void)error;
  if (!read->initialized) {
    read->initialized = 1;
    if (lonejson_curl_parse_init(&read->parser, read->json, &prompt_map,
                                 &read->record) != LONEJSON_STATUS_OK)
      read->rc = cai_cli_lj_error(read->error, &read->parser.error);
  }
  while (count != 0U && read->rc == CAI_OK) {
    amount = count < sizeof(chunk) ? count : sizeof(chunk);
    memcpy(chunk, input, amount);
    if (lonejson_curl_write_callback(chunk, 1U, amount, &read->parser) !=
        amount)
      read->rc = cai_cli_lj_error(read->error, &read->parser.error);
    input += amount;
    count -= amount;
  }
  return read->rc == CAI_OK;
}
static int prompt_read_boundary(void *context,
                                const lc_collection_read_metadata *metadata,
                                lc_error *error) {
  prompt_read *read = context;
  (void)error;
  if (metadata->kind != LC_COLLECTION_READ_METADATA_ENTRY)
    return LC_OK;
  if (lonejson_curl_parse_finish(&read->parser) != LONEJSON_STATUS_OK)
    read->rc = cai_cli_lj_error(read->error, &read->parser.error);
  if (read->rc == CAI_OK)
    read->rc = read->emit(read->context, read->record.prompt, read->error);
  prompt_read_cleanup(read);
  return read->rc == CAI_OK ? LC_OK : LC_ERR_INVALID;
}
int cai_cli_pouch_prompt_load(cai_cli_pouch *pouch, unsigned int limit,
                              int (*emit)(void *, const char *, cai_error *),
                              void *context, cai_error *error) {
  prompt_read read;
  lc_collection_info info;
  lc_collection_read_opts options;
  lc_collection_read_res result;
  lc_sink sink;
  lc_error local;
  lonejson_error json_error;
  int rc;
  if (pouch->prompt_history == NULL || emit == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "prompt-history collection is not open");
  memset(&read, 0, sizeof(read));
  memset(&sink, 0, sizeof(sink));
  lc_error_init(&local);
  lc_collection_read_opts_init(&options);
  lc_collection_read_res_init(&result);
  read.emit = emit;
  read.context = context;
  read.error = error;
  read.json = lonejson_new(NULL, &json_error);
  rc = read.json != NULL ? CAI_OK : cai_cli_lj_error(error, &json_error);
  if (rc == CAI_OK && pouch->prompt_history->describe(pouch->prompt_history,
                                                      &info, &local) != LC_OK)
    rc = cai_cli_lc_error(error, &local);
  if (rc == CAI_OK) {
    /* Count maintenance removes oldest positions without renumbering the
     * retained suffix. Read the bounded tail; never scan old prompt bodies. */
    options.has_after = options.has_through = 1;
    options.after =
        info.upper_position > limit ? info.upper_position - limit : 0U;
    options.through = info.upper_position;
    options.on_metadata = prompt_read_boundary;
    options.metadata_context = &read;
    sink.write = prompt_read_write;
    sink.impl = &read;
    if (pouch->prompt_history->read(pouch->prompt_history, &options, &sink,
                                    &result, &local) != LC_OK)
      rc = read.rc != CAI_OK ? read.rc : cai_cli_lc_error(error, &local);
  }
  prompt_read_cleanup(&read);
  lonejson_free(read.json);
  lc_collection_read_res_cleanup(&result);
  lc_error_cleanup(&local);
  return rc;
}
int cai_cli_pouch_prompt_append(cai_cli_pouch *pouch, const char *prompt,
                                cai_error *error) {
  unsigned char nonce[16];
  char id[33];
  size_t i;
  prompt_record record;
  cai_source *source = NULL;
  int rc;
  if (pouch->prompt_history == NULL || prompt == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "prompt-history collection is not open");
  if (RAND_bytes(nonce, sizeof(nonce)) != 1)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "generate prompt append identity");
  for (i = 0U; i < sizeof(nonce); i++)
    snprintf(id + i * 2U, 3U, "%02x", (unsigned int)nonce[i]);
  record.prompt = prompt;
  rc = cai_cli_mapped_source(&prompt_map, &record, &source, error);
  if (rc == CAI_OK)
    rc = collection_append_source(pouch, pouch->prompt_history, id, source,
                                  NULL, NULL, NULL, error);
  cai_source_close(source);
  return rc;
}
