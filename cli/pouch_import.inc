/* Native JSONL import feeds each physical record directly to collection append.
 * The header parser observes the same bounded chunks, discarding state bodies.
 * No whole-file, whole-record spool or per-record thread is involved. */
typedef struct journal_import_line {
  FILE *file;
  lonejson_curl_parse parser;
  int ended;
  unsigned char prefix[512];
  size_t prefix_length, prefix_offset;
  char preview[516];
  size_t preview_length;
} journal_import_line;
static lonejson_status journal_import_text(void *context, const char *bytes,
                                           size_t count,
                                           lonejson_error *error) {
  journal_import_line *line = context;
  size_t room = sizeof(line->preview) - line->preview_length - 1U;
  (void)error;
  if (count > room)
    count = room;
  memcpy(line->preview + line->preview_length, bytes, count);
  line->preview_length += count;
  line->preview[line->preview_length] = '\0';
  return LONEJSON_STATUS_OK;
}
static lonejson_status journal_import_bad_data(void *context,
                                               lonejson_error *error) {
  (void)context;
  (void)error;
  return LONEJSON_STATUS_CALLBACK_FAILED;
}
static lonejson_status journal_import_bad_bool(void *context, int value,
                                               lonejson_error *error) {
  (void)value;
  return journal_import_bad_data(context, error);
}
static size_t journal_import_read(void *context, void *bytes, size_t capacity,
                                  cai_error *error) {
  journal_import_line *line = context;
  unsigned char *out = bytes;
  size_t count = 0U;
  int ch;
  if (line->prefix_offset < line->prefix_length) {
    count = line->prefix_length - line->prefix_offset;
    if (count > capacity)
      count = capacity;
    memcpy(out, line->prefix + line->prefix_offset, count);
    line->prefix_offset += count;
    return count;
  }
  if (line->ended)
    return 0U;
  while (count < capacity) {
    ch = fgetc(line->file);
    if (ch == EOF || ch == '\n') {
      line->ended = 1;
      break;
    }
    out[count++] = (unsigned char)ch;
  }
  if (ferror(line->file)) {
    cai_cli_error(error, CAI_ERR_TRANSPORT, "read imported journal");
    return 0U;
  }
  if (count != 0U &&
      lonejson_curl_write_callback(bytes, 1U, count, &line->parser) != count) {
    cai_cli_lj_error(error, &line->parser.error);
    return 0U;
  }
  return count;
}
int cai_cli_pouch_import(cai_cli_pouch *pouch, const char *path,
                         const char *workspace, char *id, size_t capacity,
                         cai_error *error) {
  FILE *file;
  cai_cli_session session;
  lc_lease *lease = NULL;
  lc_collection *collection = NULL;
  lonejson *json;
  lonejson_error local;
  lc_error detail;
  lonejson_config config = lonejson_default_config();
  unsigned char random[16];
  unsigned long long previous = 0U, line_number = 0U, position = 0U;
  struct timespec now;
  size_t i;
  int ch, created = 0, rc = CAI_OK;
  if (capacity < 33U || strlen(workspace) >= sizeof(session.workspace))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "import output or workspace too long");
  file = fopen(path, "rb");
  if (file == NULL)
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "open journal import file");
  config.clear_destination_by_default = 0;
  config.json_value_max_total_bytes = 0U;
  config.json_value_max_string_bytes = 0U;
  json = lonejson_new(&config, &local);
  if (json == NULL || RAND_bytes(random, sizeof(random)) != 1) {
    fclose(file);
    lonejson_free(json);
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "initialize journal import");
  }
  for (i = 0U; i < sizeof(random); i++)
    snprintf(id + i * 2U, capacity - i * 2U, "%02x", (unsigned int)random[i]);
  memset(&session, 0, sizeof(session));
  strcpy(session.id, id);
  strcpy(session.workspace, workspace);
  strcpy(session.history_backend, "collections");
  rc = acquire(pouch, id, &lease, error);
  if (rc == CAI_OK && lease->state_etag != NULL && lease->state_etag[0] != '\0')
    rc = cai_cli_error(error, CAI_ERR_INVALID,
                       "generated session ID already exists");
  if (rc == CAI_OK) {
    rc = update_session(lease, &session, error);
    created = rc == CAI_OK;
  }
  rc = release(lease, rc, error);
  lease = NULL;
  if (rc == CAI_OK)
    rc = history_collection(pouch, id, "events", 1, &collection, error);
  while (rc == CAI_OK) {
    journal_import_line line;
    cli_record record;
    cai_source *source = NULL;
    cai_source_callbacks callbacks;
    lonejson_value_visitor visitor = lonejson_default_value_visitor();
    char append_id[64];
    ch = fgetc(file);
    if (ch == EOF)
      break;
    if (ch == '\n' || ch == '\r' || ch == ' ' || ch == '\t')
      continue;
    ungetc(ch, file);
    memset(&line, 0, sizeof(line));
    memset(&record, 0, sizeof(record));
    line.file = file;
    visitor.string_chunk = journal_import_text;
    visitor.object_begin = journal_import_bad_data;
    visitor.array_begin = journal_import_bad_data;
    visitor.number_begin = journal_import_bad_data;
    visitor.boolean_value = journal_import_bad_bool;
    lonejson_json_value_init(json, &record.data_value);
    lonejson_json_value_set_parse_visitor(&record.data_value, &visitor, &line,
                                          &local);
    lonejson_json_value_init(json, &record.state);
    lonejson_json_value_set_parse_sink(&record.state, discard_json, NULL,
                                       &local);
    if (lonejson_curl_parse_init(&line.parser, json, &import_map, &record) !=
        LONEJSON_STATUS_OK)
      rc = cai_cli_lj_error(error, &line.parser.error);
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.read = journal_import_read;
    callbacks.context = &line;
    if (rc == CAI_OK)
      rc = cai_source_from_callbacks(&callbacks, &source, error);
    if (rc == CAI_OK) {
      line.prefix_length =
          journal_import_read(&line, line.prefix, sizeof(line.prefix), error);
      if (error->code != CAI_OK)
        rc = error->code;
    }
    ++line_number;
    if (rc == CAI_OK &&
        ((strcmp(record.record_type, "event") != 0 &&
          strcmp(record.record_type, "commit") != 0) ||
         (strcmp(record.record_type, "event") == 0 && record.sequence == 0U)))
      rc = cai_cli_error(
          error, CAI_ERR_INVALID,
          "native journal identity headers must precede payloads");
    if (strcmp(record.record_type, "event") == 0)
      snprintf(append_id, sizeof(append_id), "event-%020llu", record.sequence);
    else
      snprintf(append_id, sizeof(append_id), "commit-import-%020llu",
               line_number);
    if (rc == CAI_OK)
      rc = journal_append_source(pouch, lease, &session, collection, append_id,
                                 source, &position, error);
    cai_source_close(source);
    if (rc == CAI_OK &&
        lonejson_curl_parse_finish(&line.parser) != LONEJSON_STATUS_OK)
      rc = cai_cli_lj_error(error, &line.parser.error);
    if (rc == CAI_OK && strcmp(record.record_type, "commit") == 0) {
      session.applied_sequence = record.applied_event_sequence;
      session.commit_position = position;
      session.commit_ns = record.commit_created_at_ns;
    } else if (rc == CAI_OK && strcmp(record.record_type, "event") == 0) {
      if (record.sequence <= previous || record.type == NULL ||
          record.type[0] == '\0')
        rc = cai_cli_error(error, CAI_ERR_INVALID,
                           "imported journal events must have strictly "
                           "increasing positive sequences");
      previous = record.sequence;
      if (record.type != NULL &&
          (strcmp(record.type, "turn_submitted") == 0 ||
           strcmp(record.type, "turn_queued") == 0) &&
          session.first_prompt[0] == '\0')
        preview(session.first_prompt, sizeof(session.first_prompt),
                line.preview);
    } else if (rc == CAI_OK)
      rc = cai_cli_error(error, CAI_ERR_INVALID,
                         "unknown imported journal record");
    lonejson_curl_parse_cleanup(&line.parser);
    lonejson_cleanup(&import_map, &record);
  }
  if (ferror(file) && rc == CAI_OK)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "read imported journal");
  fclose(file);
  lonejson_free(json);
  if (rc == CAI_OK && session.commit_position == 0U)
    rc = cai_cli_error(error, CAI_ERR_INVALID,
                       "imported journal has no committed state");
  /* An import is a new conversation now, even if its original journal is old.
   * Keep the original record timestamps intact for inspection. */
  if (rc == CAI_OK && clock_gettime(CLOCK_REALTIME, &now) != 0)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "read import timestamp");
  if (rc == CAI_OK)
    session.commit_ns = (lonejson_uint64)now.tv_sec * 1000000000ULL +
                        (lonejson_uint64)now.tv_nsec;
  if (rc == CAI_OK)
    rc = acquire(pouch, id, &lease, error);
  if (rc == CAI_OK)
    rc = update_session(lease, &session, error);
  if (rc == CAI_OK) {
    lc_error retention;
    lc_error_init(&retention);
    if (session_retain(lease, pouch->conversation_retention_seconds,
                       &retention) != LC_OK)
      rc = cai_cli_lc_error(error, &retention);
    lc_error_cleanup(&retention);
  }
  rc = release(lease, rc, error);
  lease = NULL;
  if (rc == CAI_OK)
    rc = validate_commit(pouch, id, workspace, error);
  if (rc == CAI_OK)
    rc = cai_cli_pouch_publish(pouch, id, 1, error);
  if (rc != CAI_OK && created) {
    cai_error ignored;
    cai_error_init(&ignored);
    lc_error_init(&detail);
    if (collection != NULL)
      (void)collection->drop(collection, &detail);
    if (acquire(pouch, id, &lease, &ignored) == CAI_OK) {
      (void)lease->remove(lease, NULL, &detail);
      (void)release(lease, CAI_OK, &ignored);
    }
    lc_error_cleanup(&detail);
    cai_error_cleanup(&ignored);
  }
  if (collection != NULL)
    collection->close(collection);
  return rc;
}
