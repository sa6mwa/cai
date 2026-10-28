/* Durable state edits. History is an escaped stream of canonical history
 * records, never a reconstructed array or a serialized copy of old context.
 * Collection entry atomicity publishes an edit together with its metadata. */
static const lonejson_field cai_journal_usage_fields[] = {
    LONEJSON_FIELD_I64(cai_usage_accounting, usage.input_tokens,
                       "input_tokens"),
    LONEJSON_FIELD_I64(cai_usage_accounting, usage.input_cached_tokens,
                       "cached_tokens"),
    LONEJSON_FIELD_I64(cai_usage_accounting, usage.output_tokens,
                       "output_tokens"),
    LONEJSON_FIELD_I64(cai_usage_accounting, usage.output_reasoning_tokens,
                       "reasoning_tokens"),
    LONEJSON_FIELD_I64(cai_usage_accounting, usage.total_tokens,
                       "total_tokens"),
    LONEJSON_FIELD_F64(cai_usage_accounting, estimated_spend_usd, "usd"),
    LONEJSON_FIELD_F64(cai_usage_accounting, estimated_spend_eur, "eur")};
LONEJSON_MAP_DEFINE(cai_journal_usage_map, cai_usage_accounting,
                    cai_journal_usage_fields);
typedef struct cai_journal_edit {
  lonejson_uint64 base_bytes, end_bytes;
  lonejson_uint64 blocks_keep, hidden_keep, owned_keep;
  lonejson_uint64 revision;
  cai_usage_accounting accounting;
  cai_session_state_doc metadata;
  lonejson_json_value history;
} cai_journal_edit;
static const lonejson_field cai_journal_edit_fields[] = {
    LONEJSON_FIELD_U64_REQ(cai_journal_edit, revision, "revision"),
    LONEJSON_FIELD_U64_REQ(cai_journal_edit, base_bytes, "base_bytes"),
    LONEJSON_FIELD_U64_REQ(cai_journal_edit, end_bytes, "end_bytes"),
    LONEJSON_FIELD_U64_REQ(cai_journal_edit, blocks_keep, "blocks_keep"),
    LONEJSON_FIELD_U64_REQ(cai_journal_edit, hidden_keep, "hidden_keep"),
    LONEJSON_FIELD_U64_REQ(cai_journal_edit, owned_keep, "owned_keep"),
    LONEJSON_FIELD_OBJECT(cai_journal_edit, accounting, "accounting",
                          &cai_journal_usage_map),
    LONEJSON_FIELD_OBJECT(cai_journal_edit, metadata, "metadata",
                          &cai_session_state_map),
    LONEJSON_FIELD_JSON_VALUE_REQ(cai_journal_edit, history, "history_append")};
LONEJSON_MAP_DEFINE(cai_journal_edit_map, cai_journal_edit,
                    cai_journal_edit_fields);

typedef struct cai_journal_source {
  cai_journal_edit edit;
  lonejson_generator generator;
  lonejson_spooled cursor;
  char board[CAI_TODO_BOARD_NAME_MAX + 1U];
  unsigned char input[4096], escaped[6];
  size_t begin, end, escape_begin, escape_end;
  int started, finished;
} cai_journal_source;
static int cai_journal_prepare(cai_session *session, cai_error *error);

static size_t cai_journal_common(const lonejson_object_array *a,
                                 const lonejson_object_array *b, size_t width) {
  size_t i = 0U;
  while (i < a->count && i < b->count &&
         memcmp((const char *)a->items + i * width,
                (const char *)b->items + i * width, width) == 0)
    i++;
  return i;
}
static void cai_journal_tail(lonejson_object_array *array, size_t count,
                             size_t width) {
  if (count != 0U)
    array->items = (char *)array->items + count * width;
  array->count -= count;
  array->capacity = array->count;
}

static lonejson_read_result
cai_journal_history_read(void *context, unsigned char *bytes, size_t capacity) {
  cai_journal_source *source = context;
  lonejson_read_result result = lonejson_default_read_result();
  static const char hex[] = "0123456789abcdef";
  unsigned char ch;
  while (result.bytes_read < capacity) {
    if (source->escape_begin < source->escape_end) {
      bytes[result.bytes_read++] = source->escaped[source->escape_begin++];
      continue;
    }
    source->escape_begin = source->escape_end = 0U;
    if (!source->started) {
      source->started = 1;
      bytes[result.bytes_read++] = '"';
      continue;
    }
    if (source->finished) {
      result.eof = 1;
      break;
    }
    if (source->begin == source->end) {
      lonejson_read_result read = source->cursor.read(
          &source->cursor, source->input, sizeof(source->input));
      if (read.error_code != 0) {
        result.error_code = read.error_code;
        break;
      }
      source->begin = 0U;
      source->end = read.bytes_read;
      if (read.bytes_read == 0U) {
        source->finished = 1;
        bytes[result.bytes_read++] = '"';
        continue;
      }
    }
    ch = source->input[source->begin++];
    if (ch == '"' || ch == '\\') {
      source->escaped[0] = '\\';
      source->escaped[1] = ch;
      source->escape_end = 2U;
    } else if (ch < 32U) {
      memcpy(source->escaped, "\\u0000", 6U);
      source->escaped[4] = (unsigned char)hex[ch >> 4];
      source->escaped[5] = (unsigned char)hex[ch & 15U];
      source->escape_end = 6U;
    } else {
      bytes[result.bytes_read++] = ch;
    }
  }
  return result;
}
static size_t cai_journal_read(void *context, void *bytes, size_t count,
                               cai_error *error) {
  cai_journal_source *source = context;
  size_t written = 0U;
  int eof;
  if (lonejson_generator_read(&source->generator, bytes, count, &written,
                              &eof) != LONEJSON_STATUS_OK)
    cai_set_error_detail(error, CAI_ERR_TRANSPORT, "read session journal edit",
                         source->generator.error.message);
  return written;
}
static void cai_journal_close(void *context) {
  cai_journal_source *source = context;
  lonejson_generator_cleanup(&source->generator);
  CAI_LJ->json_value_cleanup(CAI_LJ, &source->edit.history);
  cai_free_mem(NULL, source);
}
static int cai_session_journal_source_mode(cai_session *session,
                                           cai_source **out, int seed,
                                           cai_error *error) {
  cai_session_impl *impl = CAI_SESSION_IMPL(session);
  cai_journal_source *source = cai_alloc(NULL, sizeof(*source));
  cai_source_callbacks callbacks;
  lonejson_error local;
  int rc;
  if (source == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate journal edit source");
  memset(source, 0, sizeof(*source));
  if (!seed && impl->journal_revision == ULLONG_MAX) {
    cai_free_mem(NULL, source);
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "session journal revision exhausted");
  }
  source->edit.revision = seed ? 1U : impl->journal_revision + 1U;
  source->edit.accounting = impl->usage;
  source->edit.end_bytes = impl->history.size_fn(&impl->history);
  source->edit.base_bytes =
      seed ? 0U
      : impl->journal_replace
          ? (impl->journal_keep_bytes < impl->journal_history_bytes
                 ? impl->journal_keep_bytes
                 : impl->journal_history_bytes)
          : impl->journal_history_bytes;
  if (source->edit.base_bytes > source->edit.end_bytes) {
    cai_free_mem(NULL, source);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "invalid journal history cursor");
  }
  rc = cai_session_state_fill(session, &source->edit.metadata, source->board,
                              error);
  if (rc == CAI_OK && !seed)
    rc = cai_journal_prepare(session, error);
  source->edit.blocks_keep =
      seed ? 0U
           : cai_journal_common(&impl->journal_blocks, &impl->history_blocks,
                                sizeof(cai_history_block));
  source->edit.hidden_keep =
      seed ? 0U
           : cai_journal_common(&impl->journal_hidden,
                                &impl->history_hidden_events,
                                sizeof(cai_history_hidden_range));
  source->edit.owned_keep =
      seed ? 0U
           : cai_journal_common(&impl->journal_owned,
                                &impl->history_owned_events,
                                sizeof(cai_history_owned_event));
  cai_journal_tail(&source->edit.metadata.history_blocks,
                   (size_t)source->edit.blocks_keep, sizeof(cai_history_block));
  cai_journal_tail(&source->edit.metadata.history_hidden_events,
                   (size_t)source->edit.hidden_keep,
                   sizeof(cai_history_hidden_range));
  cai_journal_tail(&source->edit.metadata.history_owned_events,
                   (size_t)source->edit.owned_keep,
                   sizeof(cai_history_owned_event));
  source->cursor = impl->history;
  source->cursor.read_offset = (size_t)source->edit.base_bytes;
  CAI_SESSION_AGENT_IMPL(session)->history_runtime->json_value_init(
      CAI_SESSION_AGENT_IMPL(session)->history_runtime, &source->edit.history);
  lonejson_error_init(&local);
  if (rc == CAI_OK && source->edit.history.methods->set_reader(
                          &source->edit.history, cai_journal_history_read,
                          source, &local) != LONEJSON_STATUS_OK)
    rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                              "prepare journal history stream", local.message);
  if (rc == CAI_OK &&
      lonejson_generator_init(CAI_SESSION_AGENT_IMPL(session)->history_runtime,
                              &source->generator, &cai_journal_edit_map,
                              &source->edit) != LONEJSON_STATUS_OK)
    rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT, "prepare journal edit",
                              source->generator.error.message);
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.read = cai_journal_read;
  callbacks.close = cai_journal_close;
  callbacks.context = source;
  if (rc == CAI_OK)
    rc = cai_source_from_callbacks(&callbacks, out, error);
  if (rc != CAI_OK)
    cai_journal_close(source);
  return rc;
}
int cai_session_journal_source(cai_session *session, cai_source **out,
                               cai_error *error) {
  return cai_session_journal_source_mode(session, out, 0, error);
}
int cai_session_journal_seed_source(cai_session *session, cai_source **out,
                                    cai_error *error) {
  return cai_session_journal_source_mode(session, out, 1, error);
}

static int cai_journal_prepare(cai_session *session, cai_error *error) {
  cai_session_impl *impl = CAI_SESSION_IMPL(session);
  lonejson_object_array blocks = {0}, hidden = {0}, owned = {0};
  int rc = cai_history_metadata_copy(&impl->history_blocks,
                                     sizeof(cai_history_block), &blocks, error);
  if (rc == CAI_OK)
    rc = cai_history_metadata_copy(&impl->history_hidden_events,
                                   sizeof(cai_history_hidden_range), &hidden,
                                   error);
  if (rc == CAI_OK)
    rc = cai_history_metadata_copy(&impl->history_owned_events,
                                   sizeof(cai_history_owned_event), &owned,
                                   error);
  if (rc == CAI_OK) {
    cai_free_mem(NULL, impl->journal_next_blocks.items);
    cai_free_mem(NULL, impl->journal_next_hidden.items);
    cai_free_mem(NULL, impl->journal_next_owned.items);
    impl->journal_next_blocks = blocks;
    impl->journal_next_hidden = hidden;
    impl->journal_next_owned = owned;
    impl->journal_prepared = 1;
  } else {
    cai_free_mem(NULL, blocks.items);
    cai_free_mem(NULL, hidden.items);
    cai_free_mem(NULL, owned.items);
  }
  return rc;
}
int cai_session_journal_accept(cai_session *session, cai_error *error) {
  cai_session_impl *impl = CAI_SESSION_IMPL(session);
  if (!impl->journal_prepared)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "journal edit was not prepared");
  /* No allocation is permitted after the store acknowledged durable append. */
  cai_free_mem(NULL, impl->journal_blocks.items);
  cai_free_mem(NULL, impl->journal_hidden.items);
  cai_free_mem(NULL, impl->journal_owned.items);
  impl->journal_blocks = impl->journal_next_blocks;
  impl->journal_hidden = impl->journal_next_hidden;
  impl->journal_owned = impl->journal_next_owned;
  memset(&impl->journal_next_blocks, 0, sizeof(impl->journal_next_blocks));
  memset(&impl->journal_next_hidden, 0, sizeof(impl->journal_next_hidden));
  memset(&impl->journal_next_owned, 0, sizeof(impl->journal_next_owned));
  impl->journal_prepared = 0;
  impl->journal_history_bytes = impl->history.size_fn(&impl->history);
  impl->journal_replace = 0;
  impl->journal_revision++;
  return CAI_OK;
}

typedef struct cai_journal_record {
  char record_type[24];
  lonejson_uint64 applied_event_sequence;
  cai_journal_edit state;
  lonejson_json_value data;
} cai_journal_record;
static const lonejson_field cai_journal_record_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(cai_journal_record, record_type,
                                    "record_type", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_U64(cai_journal_record, applied_event_sequence,
                       "applied_event_sequence"),
    LONEJSON_FIELD_OBJECT(cai_journal_record, state, "state",
                          &cai_journal_edit_map),
    LONEJSON_FIELD_JSON_VALUE(cai_journal_record, data, "data")};
LONEJSON_MAP_DEFINE(cai_journal_record_map, cai_journal_record,
                    cai_journal_record_fields);

typedef struct cai_journal_restore {
  cai_session *session;
  cai_journal_record *record;
  cai_error *error;
  int seen, complete;
} cai_journal_restore;
static lonejson_status cai_journal_string_begin(void *context,
                                                lonejson_error *error) {
  cai_journal_restore *restore = context;
  cai_session_impl *impl = CAI_SESSION_IMPL(restore->session);
  size_t keep = (size_t)restore->record->state.base_bytes;
  lonejson_spooled next, cursor;
  unsigned char bytes[4096];
  int rc = CAI_OK;
  (void)error;
  if (strcmp(restore->record->record_type, "commit") != 0 || restore->seen ||
      impl->journal_revision == ULLONG_MAX ||
      restore->record->state.revision != impl->journal_revision + 1U ||
      restore->record->state.base_bytes >
          impl->history.size_fn(&impl->history) ||
      restore->record->state.end_bytes < restore->record->state.base_bytes) {
    cai_set_error(restore->error, CAI_ERR_INVALID,
                  "journal history prefix is missing or invalid");
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  restore->seen = 1;
  if (keep == impl->history.size_fn(&impl->history))
    return LONEJSON_STATUS_OK;
  cai_history_init_spooled(restore->session, &next);
  cursor = impl->history;
  cursor.read_offset = 0U;
  while (keep != 0U && rc == CAI_OK) {
    size_t amount = keep < sizeof(bytes) ? keep : sizeof(bytes);
    lonejson_read_result read = cursor.read(&cursor, bytes, amount);
    if (read.bytes_read == 0U || read.error_code != 0) {
      rc = cai_set_error(restore->error, CAI_ERR_INVALID,
                         "truncated journal prefix");
      break;
    }
    rc =
        cai_history_append_bytes(&next, bytes, read.bytes_read, restore->error);
    keep -= read.bytes_read;
  }
  if (rc == CAI_OK)
    cai_history_replace(restore->session, &next);
  else
    next.cleanup(&next);
  return rc == CAI_OK ? LONEJSON_STATUS_OK : LONEJSON_STATUS_CALLBACK_FAILED;
}
static lonejson_status cai_journal_string_chunk(void *context,
                                                const char *bytes, size_t count,
                                                lonejson_error *error) {
  cai_journal_restore *restore = context;
  cai_session_impl *impl = CAI_SESSION_IMPL(restore->session);
  (void)error;
  if (!restore->seen || restore->complete ||
      count > restore->record->state.end_bytes -
                  impl->history.size_fn(&impl->history)) {
    cai_set_error(restore->error, CAI_ERR_INVALID,
                  "journal history length mismatch");
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return cai_history_append_bytes(&impl->history, bytes, count,
                                  restore->error) == CAI_OK
             ? LONEJSON_STATUS_OK
             : LONEJSON_STATUS_CALLBACK_FAILED;
}
static lonejson_status cai_journal_string_end(void *context,
                                              lonejson_error *error) {
  cai_journal_restore *restore = context;
  (void)error;
  restore->complete = 1;
  return LONEJSON_STATUS_OK;
}
static lonejson_status cai_journal_bad_value(void *context,
                                             lonejson_error *error) {
  cai_journal_restore *restore = context;
  (void)error;
  cai_set_error(restore->error, CAI_ERR_INVALID,
                "journal history_append must be a string");
  return LONEJSON_STATUS_CALLBACK_FAILED;
}
static lonejson_status cai_journal_bad_bool(void *context, int value,
                                            lonejson_error *error) {
  (void)value;
  return cai_journal_bad_value(context, error);
}
static int cai_journal_join(lonejson_object_array *before, size_t keep,
                            const lonejson_object_array *tail, size_t width,
                            cai_error *error) {
  size_t count, capacity;
  void *items;
  if (keep > before->count || tail->count > ((size_t)-1 / width) - keep)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "journal metadata prefix is missing");
  count = keep + tail->count;
  if (count > before->capacity) {
    capacity = before->capacity <= ((size_t)-1 / width) / 2U
                   ? before->capacity * 2U
                   : count;
    if (capacity < count)
      capacity = count;
    items = cai_realloc_mem(NULL, before->items, capacity * width);
    if (items == NULL)
      return cai_set_error(error, CAI_ERR_NOMEM, "restore journal metadata");
    before->items = items;
    before->capacity = capacity;
  }
  if (tail->count != 0U)
    memcpy((char *)before->items + keep * width, tail->items,
           tail->count * width);
  before->count = count;
  return CAI_OK;
}

static lonejson_status cai_journal_discard(void *context, const void *bytes,
                                           size_t count,
                                           lonejson_error *error) {
  (void)context;
  (void)bytes;
  (void)count;
  (void)error;
  return LONEJSON_STATUS_OK;
}
static lonejson_status cai_journal_reject_snapshot(void *context,
                                                   const void *bytes,
                                                   size_t count,
                                                   lonejson_error *error) {
  cai_journal_restore *restore = context;
  (void)bytes;
  (void)count;
  (void)error;
  cai_set_error(restore->error, CAI_ERR_INVALID,
                "journal metadata must not contain a history snapshot");
  return LONEJSON_STATUS_CALLBACK_FAILED;
}

/* Validate retained provider arrays without constructing a combined JSON value.
 * Length framing alone cannot detect a corrupt/imported provider item. */
static int cai_journal_validate_context(cai_session *session,
                                        cai_error *error) {
  cai_spooled_record_reader records;
  cai_history_record_json_reader item;
  lonejson_writer writer;
  lonejson_error local;
  lonejson_status status;
  unsigned long length;
  int have, rc = CAI_OK;
  memset(&records, 0, sizeof(records));
  records.cursor = CAI_SESSION_IMPL(session)->history;
  records.cursor.read_offset = 0U;
  lonejson_error_init(&local);
  status = lonejson_writer_init_sink(
      CAI_SESSION_AGENT_IMPL(session)->history_runtime, &writer,
      cai_journal_discard, NULL, &local);
  if (status == LONEJSON_STATUS_OK)
    status = writer.begin_array(&writer, &local);
  while (status == LONEJSON_STATUS_OK) {
    rc = cai_history_read_record_length(&records, &length, &have, error);
    if (rc != CAI_OK || !have)
      break;
    memset(&item, 0, sizeof(item));
    item.records = &records;
    item.remaining = length;
    item.error = error;
    status = writer.array_items_reader(
        &writer, "", cai_history_record_json_read, &item, &local);
    if (status == LONEJSON_STATUS_OK && item.remaining != 0UL)
      rc = cai_set_error(error, CAI_ERR_INVALID,
                         "incomplete journal context record");
    if (rc != CAI_OK)
      break;
  }
  if (status == LONEJSON_STATUS_OK && rc == CAI_OK)
    status = writer.end_array(&writer, &local);
  if (status == LONEJSON_STATUS_OK && rc == CAI_OK)
    status = writer.finish(&writer, &local);
  writer.cleanup(&writer);
  if (rc == CAI_OK && status != LONEJSON_STATUS_OK)
    rc = cai_set_error_detail(error, CAI_ERR_INVALID,
                              "invalid journal context record", local.message);
  return rc;
}

int cai_session_restore_journal_source(cai_session *session, cai_source *source,
                                       cai_error *error) {
  lonejson_stream *stream;
  lonejson_config config = lonejson_default_config();
  lonejson *json;
  lonejson_error local;
  cai_source_reader_context reader;
  cai_journal_record record;
  cai_journal_restore restore;
  lonejson_value_visitor visitor = lonejson_default_value_visitor();
  int rc = CAI_OK, commits = 0;
  if (session == NULL || source == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session and journal source are required");
  config.clear_destination_by_default = 0;
  config.json_value_max_total_bytes = 0U;
  config.json_value_max_string_bytes = 0U;
  json = lonejson_new(&config, &local);
  if (json == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "create journal parser");
  memset(&reader, 0, sizeof(reader));
  reader.source = source;
  reader.error = error;
  stream = lonejson_stream_open_reader(
      json, &cai_journal_record_map, cai_state_source_reader, &reader, &local);
  if (stream == NULL)
    rc = cai_set_error_detail(error, CAI_ERR_INVALID, "open journal stream",
                              local.message);
  visitor.string_begin = cai_journal_string_begin;
  visitor.string_chunk = cai_journal_string_chunk;
  visitor.string_end = cai_journal_string_end;
  visitor.object_begin = cai_journal_bad_value;
  visitor.array_begin = cai_journal_bad_value;
  visitor.number_begin = cai_journal_bad_value;
  visitor.boolean_value = cai_journal_bad_bool;
  visitor.null_value = cai_journal_bad_value;
  while (rc == CAI_OK) {
    lonejson_stream_result result;
    cai_session_impl *impl = CAI_SESSION_IMPL(session);
    cai_session_state_doc merged;
    memset(&record, 0, sizeof(record));
    memset(&restore, 0, sizeof(restore));
    restore.session = session;
    restore.record = &record;
    restore.error = error;
    lonejson_json_value_init(json, &record.state.metadata.history);
    lonejson_json_value_set_parse_sink(&record.state.metadata.history,
                                       cai_journal_reject_snapshot, &restore,
                                       &local);
    lonejson_json_value_init(json, &record.data);
    lonejson_json_value_set_parse_sink(&record.data, cai_journal_discard, NULL,
                                       &local);
    lonejson_json_value_init(json, &record.state.history);
    record.state.history.methods->set_parse_visitor(&record.state.history,
                                                    &visitor, &restore, &local);
    result = lonejson_stream_next(stream, &record, &local);
    if (result == LONEJSON_STREAM_EOF) {
      lonejson_cleanup(&cai_journal_record_map, &record);
      break;
    }
    if (result != LONEJSON_STREAM_OBJECT)
      rc = error != NULL && error->code != CAI_OK
               ? error->code
               : cai_set_error_detail(error, CAI_ERR_INVALID,
                                      "parse session journal", local.message);
    else if (strcmp(record.record_type, "commit") == 0) {
      if (record.state.blocks_keep > (size_t)-1 ||
          record.state.hidden_keep > (size_t)-1 ||
          record.state.owned_keep > (size_t)-1)
        rc = cai_set_error(error, CAI_ERR_LIMIT,
                           "journal metadata cursor exceeds addressable size");
      if (!restore.seen || !restore.complete ||
          impl->history.size_fn(&impl->history) != record.state.end_bytes)
        rc = cai_set_error(error, CAI_ERR_INVALID,
                           "incomplete session journal edit");
      merged = record.state.metadata;
      if (rc == CAI_OK)
        rc = cai_journal_join(
            &impl->history_blocks, (size_t)record.state.blocks_keep,
            &merged.history_blocks, sizeof(cai_history_block), error);
      if (rc == CAI_OK)
        rc = cai_journal_join(&impl->history_hidden_events,
                              (size_t)record.state.hidden_keep,
                              &merged.history_hidden_events,
                              sizeof(cai_history_hidden_range), error);
      if (rc == CAI_OK)
        rc = cai_journal_join(&impl->history_owned_events,
                              (size_t)record.state.owned_keep,
                              &merged.history_owned_events,
                              sizeof(cai_history_owned_event), error);
      if (rc == CAI_OK)
        rc = cai_session_import_state(session, NULL, &merged, error);
      if (rc == CAI_OK &&
          record.applied_event_sequence < impl->journal_applied_sequence)
        rc = cai_set_error(error, CAI_ERR_INVALID,
                           "session journal watermark moved backwards");
      if (rc == CAI_OK &&
          (record.state.accounting.usage.input_tokens < 0LL ||
           record.state.accounting.usage.input_cached_tokens < 0LL ||
           record.state.accounting.usage.output_tokens < 0LL ||
           record.state.accounting.usage.output_reasoning_tokens < 0LL ||
           record.state.accounting.usage.total_tokens < 0LL ||
           !(record.state.accounting.estimated_spend_usd >= 0.0 &&
             record.state.accounting.estimated_spend_usd <= DBL_MAX) ||
           !(record.state.accounting.estimated_spend_eur >= 0.0 &&
             record.state.accounting.estimated_spend_eur <= DBL_MAX)))
        rc = cai_set_error(error, CAI_ERR_INVALID,
                           "invalid journal usage accounting");
      if (rc == CAI_OK) {
        impl->usage = record.state.accounting;
        impl->usage.limit_exceeded =
            cai_usage_accounting_exceeds(&impl->usage, &impl->usage_limits);
        commits++;
        impl->journal_revision = record.state.revision;
        impl->journal_applied_sequence = record.applied_event_sequence;
      }
    } else if (strcmp(record.record_type, "event") != 0)
      rc = cai_set_error(error, CAI_ERR_INVALID,
                         "unknown session journal record");
    lonejson_cleanup(&cai_journal_record_map, &record);
  }
  if (stream != NULL)
    lonejson_stream_close(stream);
  lonejson_free(json);
  if (rc == CAI_OK && commits == 0)
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "session journal has no committed state");
  if (rc == CAI_OK)
    rc = cai_journal_validate_context(session, error);
  if (rc == CAI_OK) {
    cai_session_state_doc metadata;
    memset(&metadata, 0, sizeof(metadata));
    metadata.history_blocks = CAI_SESSION_IMPL(session)->history_blocks;
    metadata.history_hidden_events =
        CAI_SESSION_IMPL(session)->history_hidden_events;
    metadata.history_owned_events =
        CAI_SESSION_IMPL(session)->history_owned_events;
    rc = cai_history_validate_metadata(
        session, &CAI_SESSION_IMPL(session)->history, &metadata, error);
  }
  if (rc == CAI_OK) {
    unsigned long long revision = CAI_SESSION_IMPL(session)->journal_revision;
    rc = cai_journal_prepare(session, error);
    if (rc == CAI_OK)
      rc = cai_session_journal_accept(session, error);
    CAI_SESSION_IMPL(session)->journal_revision = revision;
    CAI_SESSION_IMPL(session)->history_items_known = 0;
  }
  return rc;
}
