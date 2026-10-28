/* Private whole-entry history operations. History remains a sequence of
 * length-prefixed, single-item JSON arrays; copying a prefix uses bounded
 * chunks and preserves provider items, including opaque reasoning. */
static void cai_history_summary(char *out, size_t capacity, const char *text) {
  size_t used = 0U;
  size_t lead, expected;
  unsigned char ch;
  int space = 0;
  while (*text != '\0' && used + 1U < capacity) {
    ch = (unsigned char)*text++;
    if (ch <= 32U || ch == 127U) {
      space = used != 0U;
      continue;
    }
    if (space && used + 2U < capacity)
      out[used++] = ' ';
    space = 0;
    out[used++] = (char)ch;
  }
  /* Never leave a partial final UTF-8 codepoint in a bounded summary. */
  if (used != 0U) {
    lead = used;
    while (lead != 0U && ((unsigned char)out[lead - 1U] & 0xc0U) == 0x80U)
      lead--;
    if (lead == 0U)
      used = 0U;
    else if ((unsigned char)out[lead - 1U] >= 0xc0U) {
      ch = (unsigned char)out[lead - 1U];
      expected = ch >= 0xf0U ? 4U : ch >= 0xe0U ? 3U : 2U;
      if (used - lead + 1U < expected)
        used = lead - 1U;
    }
  }
  out[used] = '\0';
}

static int cai_history_prefix(cai_session *session, size_t keep,
                              lonejson_spooled *out, size_t *total,
                              cai_error *error) {
  cai_spooled_record_reader reader;
  lonejson_error local;
  unsigned long length;
  unsigned long remaining;
  unsigned char bytes[4096];
  char header[32];
  size_t count = 0U, chunk, i;
  int have, rc;
  memset(&reader, 0, sizeof(reader));
  reader.cursor = CAI_SESSION_IMPL(session)->history;
  lonejson_error_init(&local);
  if (reader.cursor.rewind(&reader.cursor, &local) != LONEJSON_STATUS_OK)
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "cannot read conversation history",
                                local.message);
  for (;;) {
    rc = cai_history_read_record_length(&reader, &length, &have, error);
    if (rc != CAI_OK || !have)
      break;
    if (out != NULL && count < keep) {
      snprintf(header, sizeof(header), "%lu\n", length);
      rc = cai_history_append_bytes(out, header, strlen(header), error);
      if (rc != CAI_OK)
        break;
    }
    remaining = length;
    while (rc == CAI_OK && remaining != 0UL) {
      chunk = remaining > sizeof(bytes) ? sizeof(bytes) : (size_t)remaining;
      for (i = 0U; i < chunk; i++) {
        if (cai_spooled_record_reader_next(&reader, &bytes[i], error) <= 0) {
          rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                             "truncated conversation history");
          break;
        }
      }
      if (rc == CAI_OK && out != NULL && count < keep)
        rc = cai_history_append_bytes(out, bytes, chunk, error);
      remaining -= (unsigned long)chunk;
    }
    if (rc == CAI_OK && out != NULL && count < keep)
      rc = cai_history_append_bytes(out, "\n", 1U, error);
    if (rc != CAI_OK)
      break;
    count++;
  }
  if (total != NULL)
    *total = count;
  return rc;
}

static int cai_history_validate_metadata(cai_session *session,
                                         lonejson_spooled *history,
                                         const cai_session_state_doc *doc,
                                         cai_error *error) {
  cai_session view = *session;
  cai_session_impl impl = *CAI_SESSION_IMPL(session);
  const cai_history_block *blocks = doc->history_blocks.items;
  const cai_history_hidden_range *ranges = doc->history_hidden_events.items;
  const cai_history_owned_event *owned = doc->history_owned_events.items;
  size_t count, i;
  int rc;
  impl.history = *history;
  view.impl = &impl;
  rc = cai_history_prefix(&view, 0U, NULL, &count, error);
  if (rc != CAI_OK)
    return rc;
  for (i = 0U; i < doc->history_blocks.count; i++) {
    if (blocks[i].first_item < 0LL ||
        (unsigned long long)blocks[i].first_item > count ||
        (i == 0U && blocks[i].first_item != 0LL) ||
        (i != 0U && blocks[i].first_item < blocks[i - 1U].first_item) ||
        blocks[i].event_start < 0LL || blocks[i].input_sequence < 0LL ||
        blocks[i].kind < CAI_AGENT_HISTORY_TURN ||
        blocks[i].kind > CAI_AGENT_HISTORY_COMPACTION)
      return cai_set_error(error, CAI_ERR_INVALID,
                           "invalid conversation history entry boundary");
  }
  for (i = 0U; i < doc->history_hidden_events.count; i++)
    if (ranges[i].first < 1LL || ranges[i].last < ranges[i].first)
      return cai_set_error(error, CAI_ERR_INVALID,
                           "invalid trimmed journal range");
  for (i = 0U; i < doc->history_owned_events.count; i++)
    if (owned[i].entry < 0LL ||
        (unsigned long long)owned[i].entry >= doc->history_blocks.count ||
        owned[i].sequence <= 0LL)
      return cai_set_error(error, CAI_ERR_INVALID,
                           "invalid history event ownership");
  return CAI_OK;
}

int cai_session_history_own_event(cai_session *session,
                                  unsigned long long sequence,
                                  cai_error *error) {
  cai_session_impl *impl = CAI_SESSION_IMPL(session);
  cai_history_owned_event *owned;
  size_t count = impl->history_owned_events.count;
  if (sequence == 0U || impl->history_blocks.count == 0U)
    return CAI_OK;
  if (sequence > (unsigned long long)LLONG_MAX ||
      count == (size_t)-1 / sizeof(*owned))
    return cai_set_error(error, CAI_ERR_LIMIT, "history event index exhausted");
  if (count == impl->history_owned_events.capacity) {
    size_t capacity = count < 16U ? 16U : count * 2U;
    if (capacity < count || capacity > (size_t)-1 / sizeof(*owned))
      return cai_set_error(error, CAI_ERR_LIMIT,
                           "history event index exhausted");
    owned = cai_realloc_mem(NULL, impl->history_owned_events.items,
                            capacity * sizeof(*owned));
    if (owned == NULL)
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "cannot index history event owner");
    impl->history_owned_events.items = owned;
    impl->history_owned_events.capacity = capacity;
  }
  owned = impl->history_owned_events.items;
  owned[count].entry = (long long)impl->history_blocks.count - 1LL;
  owned[count].sequence = (long long)sequence;
  impl->history_owned_events.count++;
  return CAI_OK;
}

void cai_session_history_summary_delta(cai_session *session, const char *bytes,
                                       size_t length, int first) {
  cai_session_impl *impl = CAI_SESSION_IMPL(session);
  cai_history_block *blocks = impl->history_blocks.items;
  char *out;
  size_t used, i;
  unsigned char ch;
  if (impl->history_blocks.count == 0U ||
      blocks[impl->history_blocks.count - 1U].kind != CAI_AGENT_HISTORY_TURN)
    return;
  out = blocks[impl->history_blocks.count - 1U].summary;
  if (first)
    out[0] = '\0';
  used = strlen(out);
  for (i = 0U; i < length && used + 1U < sizeof(blocks->summary); i++) {
    ch = (unsigned char)bytes[i];
    if (ch <= 32U || ch == 127U) {
      if (used != 0U && out[used - 1U] != ' ')
        out[used++] = ' ';
    } else {
      size_t needed = ch >= 0xf0U   ? 4U
                      : ch >= 0xe0U ? 3U
                      : ch >= 0xc0U ? 2U
                                    : 1U;
      if (needed > sizeof(blocks->summary) - used - 1U) {
        /* Fill the bounded prefix without cutting a UTF-8 character. The
         * full buffer also prevents later deltas appending its suffix. */
        memset(out + used, ' ', sizeof(blocks->summary) - used - 1U);
        used = sizeof(blocks->summary) - 1U;
        break;
      }
      out[used++] = (char)ch;
    }
  }
  out[used] = '\0';
}

int cai_session_history_prepare(cai_session *session, cai_error *error) {
  cai_session_impl *impl = CAI_SESSION_IMPL(session);
  int rc = CAI_OK;
  if (!impl->history_items_known) {
    rc = cai_history_prefix(session, 0U, NULL, &impl->history_items, error);
    if (rc != CAI_OK)
      return rc;
    impl->history_items_known = 1;
  }
  return rc;
}

int cai_session_history_begin(cai_session *session, int kind,
                              unsigned long long event_start,
                              unsigned long long input_sequence,
                              const char *summary, cai_error *error) {
  cai_session_impl *impl = CAI_SESSION_IMPL(session);
  cai_history_block *blocks;
  size_t count;
  int rc = cai_session_history_prepare(session, error);
  if (rc != CAI_OK)
    return rc;
  if (event_start > (unsigned long long)LLONG_MAX ||
      input_sequence > (unsigned long long)LLONG_MAX ||
      impl->history_items > (size_t)LLONG_MAX)
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "conversation history index exhausted");
  count = impl->history_blocks.count;
  if (count >= ((size_t)-1 / sizeof(*blocks)) - 1U)
    return cai_set_error(error, CAI_ERR_LIMIT, "too many conversation entries");
  blocks = cai_realloc_mem(NULL, impl->history_blocks.items,
                           (count + 1U) * sizeof(*blocks));
  if (blocks == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "cannot index conversation entry");
  impl->history_blocks.items = blocks;
  impl->history_blocks.count = count + 1U;
  impl->history_blocks.capacity = count + 1U;
  memset(&blocks[count], 0, sizeof(*blocks));
  blocks[count].first_item = count == 0U ? 0LL : (long long)impl->history_items;
  blocks[count].event_start = (long long)event_start;
  blocks[count].input_sequence = (long long)input_sequence;
  blocks[count].kind = kind;
  cai_history_summary(blocks[count].summary, sizeof(blocks[count].summary),
                      summary);
  return CAI_OK;
}

void cai_session_history_compacted(cai_session *session) {
  cai_session_impl *impl = CAI_SESSION_IMPL(session);
  cai_history_block *blocks = impl->history_blocks.items;
  if (impl->history_blocks.count == 0U)
    return;
  /* All earlier model context is now opaque, including retained tail items. */
  memset(blocks, 0, sizeof(*blocks));
  blocks->kind = CAI_AGENT_HISTORY_COMPACTION;
  strcpy(blocks->summary, "Compacted history");
  impl->history_blocks.count = 1U;
  impl->history_owned_events.count = 0U;
}

int cai_session_history_visible(const cai_session *session,
                                unsigned long long sequence) {
  const cai_session_impl *impl = CAI_SESSION_IMPL(session);
  const cai_history_hidden_range *ranges = impl->history_hidden_events.items;
  size_t i;
  for (i = 0U; i < impl->history_hidden_events.count; i++)
    if (sequence >= (unsigned long long)ranges[i].first &&
        sequence <= (unsigned long long)ranges[i].last)
      return 0;
  return 1;
}

int cai_session_history_visit(cai_session *session, size_t limit,
                              cai_agent_history_entry_fn visit, void *context,
                              cai_error *error) {
  cai_session_impl *impl = CAI_SESSION_IMPL(session);
  cai_history_block *blocks;
  cai_agent_history_entry entry;
  size_t i, count;
  int rc;
  if (visit == NULL)
    return cai_set_error(error, CAI_ERR_INVALID, "history visitor is required");
  rc = cai_session_history_prepare(session, error);
  if (rc != CAI_OK)
    return rc;
  count = impl->history_blocks.count;
  blocks = impl->history_blocks.items;
  if (limit == 0U)
    limit = 20U;
  for (i = 0U; i < count && i < limit; i++) {
    entry.remove_count = i + 1U;
    entry.kind = (cai_agent_history_kind)blocks[count - 1U - i].kind;
    entry.summary = blocks[count - 1U - i].summary;
    rc = visit(context, &entry, error);
    if (rc != CAI_OK)
      return rc;
  }
  return CAI_OK;
}

int cai_session_history_trim(cai_session *session, size_t count,
                             unsigned long long through, cai_error *error) {
  cai_session_impl *impl = CAI_SESSION_IMPL(session);
  cai_history_block *blocks;
  cai_history_hidden_range *ranges;
  cai_history_owned_event *owned = impl->history_owned_events.items;
  lonejson_spooled next;
  size_t first, keep, i, n, capacity, retained_owned;
  unsigned long long start;
  int rc;
  if (count == 0U)
    return CAI_OK;
  if (!CAI_SESSION_AGENT_IMPL(session)->local_history_enabled ||
      CAI_SESSION_AGENT_IMPL(session)->session_continuity !=
          CAI_SESSION_CONTINUITY_CLIENT_HISTORY)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "trimming requires client-managed local history");
  rc = cai_session_history_prepare(session, error);
  if (rc != CAI_OK)
    return rc;
  if (count > impl->history_blocks.count)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "trim count exceeds retained history entries");
  if (through > (unsigned long long)LLONG_MAX)
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "conversation journal index exhausted");
  first = impl->history_blocks.count - count;
  blocks = impl->history_blocks.items;
  keep = (size_t)blocks[first].first_item;
  n = impl->history_hidden_events.count;
  if (impl->history_owned_events.count >
          ((size_t)-1 / sizeof(*ranges)) - count - 1U ||
      n > ((size_t)-1 / sizeof(*ranges)) - count -
              impl->history_owned_events.count - 1U)
    return cai_set_error(error, CAI_ERR_LIMIT, "too many history cuts");
  capacity = n + count + impl->history_owned_events.count + 1U;
  ranges = cai_alloc(NULL, capacity * sizeof(*ranges));
  if (ranges == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "cannot record history cut");
  if (n != 0U)
    memcpy(ranges, impl->history_hidden_events.items, n * sizeof(*ranges));
  start = blocks[first].event_start != 0LL
              ? (unsigned long long)blocks[first].event_start
              : 1U;
  if (start <= through) {
    ranges[n].first = (long long)start;
    ranges[n++].last = (long long)through;
  }
  for (i = first; i < impl->history_blocks.count; i++) {
    if (blocks[i].input_sequence > 0LL &&
        (unsigned long long)blocks[i].input_sequence < start) {
      ranges[n].first = blocks[i].input_sequence;
      ranges[n++].last = blocks[i].input_sequence;
    }
  }
  for (i = 0U; i < impl->history_owned_events.count; i++)
    if ((unsigned long long)owned[i].entry >= first) {
      ranges[n].first = owned[i].sequence;
      ranges[n++].last = owned[i].sequence;
    }
  cai_history_init_spooled(session, &next);
  rc = cai_history_prefix(session, keep, &next, NULL, error);
  if (rc == CAI_OK) {
    cai_history_replace(session, &next);
    impl->journal_keep_bytes = impl->history.size_fn(&impl->history);
    impl->history_items = keep;
    impl->history_items_known = 1;
    impl->history_blocks.count = first;
    cai_free_mem(NULL, impl->history_hidden_events.items);
    impl->history_hidden_events.items = ranges;
    impl->history_hidden_events.count = n;
    impl->history_hidden_events.capacity = capacity;
    retained_owned = 0U;
    for (i = 0U; i < impl->history_owned_events.count; i++)
      if ((unsigned long long)owned[i].entry < first)
        owned[retained_owned++] = owned[i];
    impl->history_owned_events.count = retained_owned;
    ranges = NULL;
    cai_free_mem(&CAI_SESSION_CLIENT_IMPL(session)->allocator,
                 impl->previous_response_id);
    cai_free_mem(&CAI_SESSION_CLIENT_IMPL(session)->allocator,
                 impl->conversation_id);
    impl->previous_response_id = NULL;
    impl->conversation_id = NULL;
    cai_session_forget_context_usage(session);
  } else {
    next.cleanup(&next);
  }
  cai_free_mem(NULL, ranges);
  return rc;
}
