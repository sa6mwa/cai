/* Recover entry boundaries from this session's journal and current model
 * context. Journal input kinds distinguish steering from turn submissions.
 * Only fingerprints, offsets and bounded summaries are retained here. */
typedef struct cai_history_input_mark {
  unsigned char digest[32];
  unsigned long long sequence, completion, committed;
  size_t item;
  int kind, private_shell, merge, matched;
  char command[1024];
} cai_history_input_mark;
typedef struct cai_history_shell_mark {
  unsigned long long sequence, input;
} cai_history_shell_mark;
typedef struct cai_history_message_mark {
  unsigned char digest[32];
  size_t item, input;
  int kind;
  char text[1024];
} cai_history_message_mark;
struct cai_history_rebuild {
  cai_history_input_mark *inputs;
  size_t count, capacity;
  cai_history_shell_mark *shell_events;
  size_t shell_count, shell_capacity;
  unsigned long long *assistant_events;
  size_t assistant_count, assistant_capacity;
  cai_history_message_mark *messages;
  size_t message_count, message_capacity;
  size_t current_shell;
  int journal_v2, shell_group;
};

static int cai_history_grow(void **items, size_t *capacity, size_t count,
                            size_t size, cai_error *error) {
  size_t next;
  void *grown;
  if (count < *capacity)
    return CAI_OK;
  next = count < 16U ? 16U : count * 2U;
  if (next < count || next > (size_t)-1 / size)
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "conversation history index exhausted");
  grown = cai_realloc_mem(NULL, *items, next * size);
  if (grown == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "cannot index active conversation history");
  *items = grown;
  *capacity = next;
  return CAI_OK;
}
int cai_history_rebuild_open(cai_history_rebuild **out, cai_error *error) {
  *out = cai_alloc(NULL, sizeof(**out));
  if (*out == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "cannot index active history");
  memset(*out, 0, sizeof(**out));
  (*out)->current_shell = (size_t)-1;
  (*out)->shell_group = -1;
  return CAI_OK;
}
void cai_history_rebuild_close(cai_history_rebuild *r) {
  if (r == NULL)
    return;
  cai_free_mem(NULL, r->inputs);
  cai_free_mem(NULL, r->shell_events);
  cai_free_mem(NULL, r->assistant_events);
  cai_free_mem(NULL, r->messages);
  cai_free_mem(NULL, r);
}
int cai_history_rebuild_feed(cai_history_rebuild *r,
                             const cai_agent_session_event *event,
                             unsigned long long watermark, cai_error *error) {
  cai_history_input_mark *input;
  unsigned long long target;
  size_t i;
  int kind, rc;
  void *grown;
  const char *end;
  const char *type = event->type;
  if (r == NULL || event->sequence > watermark)
    return CAI_OK;
  if (strcmp(type, "input_journal_v2") == 0)
    r->journal_v2 = 1;
  if (strcmp(type, "user_shell_group") == 0) {
    r->shell_group = event->data != NULL && strcmp(event->data, "turn") == 0;
    return CAI_OK;
  }
  kind = strcmp(type, "turn_submitted") == 0 || strcmp(type, "turn_queued") == 0
             ? 1
         : strcmp(type, "steering_queued") == 0    ? 0
         : strcmp(type, "user_shell_started") == 0 ? 2
                                                   : -1;
  if (kind >= 0) {
    grown = r->inputs;
    rc = cai_history_grow(&grown, &r->capacity, r->count, sizeof(*r->inputs),
                          error);
    if (rc != CAI_OK)
      return rc;
    r->inputs = grown;
    input = &r->inputs[r->count];
    memset(input, 0, sizeof(*input));
    input->kind = kind;
    input->sequence = event->sequence;
    input->committed = r->journal_v2 ? 0U : event->sequence;
    input->item = (size_t)-1;
    input->merge = -1;
    if (kind == 2) {
      input->merge = r->shell_group;
      r->current_shell = r->count;
      r->shell_group = -1;
      cai_history_summary(input->command, sizeof(input->command),
                          event->data != NULL ? event->data
                                              : "Shell execution");
    } else if (event->data != NULL &&
               EVP_Digest(event->data, strlen(event->data), input->digest, NULL,
                          EVP_sha256(), NULL) != 1) {
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "cannot fingerprint history input");
    }
    r->count++;
  }
  if (r->current_shell != (size_t)-1 &&
      (strcmp(type, "user_shell_started") == 0 ||
       strcmp(type, "user_shell_output_tail") == 0 ||
       strcmp(type, "user_shell_output_hex") == 0 ||
       strcmp(type, "user_shell_completed") == 0 ||
       strcmp(type, "user_shell_unshared") == 0 ||
       strcmp(type, "replay_event_omitted") == 0)) {
    grown = r->shell_events;
    rc = cai_history_grow(&grown, &r->shell_capacity, r->shell_count,
                          sizeof(*r->shell_events), error);
    if (rc != CAI_OK)
      return rc;
    r->shell_events = grown;
    input = &r->inputs[r->current_shell];
    r->shell_events[r->shell_count].sequence = event->sequence;
    r->shell_events[r->shell_count++].input = input->sequence;
    if (strcmp(type, "user_shell_completed") == 0 ||
        strcmp(type, "user_shell_unshared") == 0 ||
        strcmp(type, "replay_event_omitted") == 0) {
      input->completion = event->sequence;
      input->private_shell = strcmp(type, "user_shell_unshared") == 0;
      if (input->private_shell || !r->journal_v2)
        input->committed = event->sequence;
      if (event->data != NULL &&
          EVP_Digest(event->data, strlen(event->data), input->digest, NULL,
                     EVP_sha256(), NULL) != 1)
        return cai_set_error(error, CAI_ERR_TRANSPORT,
                             "cannot fingerprint shell context");
      r->current_shell = (size_t)-1;
    }
  }
  if ((strcmp(type, "input_consumed") == 0 ||
       strcmp(type, "user_shell_context_consumed") == 0) &&
      event->data != NULL) {
    if (cai_parse_unsigned_decimal(event->data, &end, &target) != 0 ||
        *end != '\0' || target == 0U)
      return cai_set_error(
          error, CAI_ERR_INVALID,
          "history acknowledgement has an invalid input sequence");
    for (i = r->count; i != 0U; i--)
      if (r->inputs[i - 1U].sequence == target ||
          r->inputs[i - 1U].completion == target) {
        r->inputs[i - 1U].committed = event->sequence;
        break;
      }
  }
  if (strcmp(type, "assistant_text_delta") == 0 && event->data != NULL &&
      event->data[0] != '\0') {
    grown = r->assistant_events;
    rc = cai_history_grow(&grown, &r->assistant_capacity, r->assistant_count,
                          sizeof(*r->assistant_events), error);
    if (rc != CAI_OK)
      return rc;
    r->assistant_events = grown;
    r->assistant_events[r->assistant_count++] = event->sequence;
  }
  return CAI_OK;
}

typedef struct cai_history_item_scan {
  EVP_MD_CTX *hash;
  size_t depth, key_used, role_used, type_used, text_used;
  char key[32], role[16], type[32], text[1024];
  int capture, failed;
} cai_history_item_scan;
static lonejson_status cai_history_scan_begin(void *context,
                                              lonejson_error *error) {
  cai_history_item_scan *scan = context;
  (void)error;
  scan->depth++;
  return LONEJSON_STATUS_OK;
}
static lonejson_status cai_history_scan_end(void *context,
                                            lonejson_error *error) {
  cai_history_item_scan *scan = context;
  (void)error;
  if (scan->depth != 0U)
    scan->depth--;
  return LONEJSON_STATUS_OK;
}
static lonejson_status cai_history_scan_key_begin(void *context,
                                                  lonejson_error *error) {
  cai_history_item_scan *scan = context;
  (void)error;
  scan->key_used = 0U;
  scan->key[0] = '\0';
  return LONEJSON_STATUS_OK;
}
static lonejson_status cai_history_scan_key_chunk(void *context,
                                                  const char *bytes,
                                                  size_t size,
                                                  lonejson_error *error) {
  cai_history_item_scan *scan = context;
  (void)error;
  if (size > sizeof(scan->key) - scan->key_used - 1U)
    size = sizeof(scan->key) - scan->key_used - 1U;
  memcpy(scan->key + scan->key_used, bytes, size);
  scan->key_used += size;
  scan->key[scan->key_used] = '\0';
  return LONEJSON_STATUS_OK;
}
static lonejson_status cai_history_scan_string_begin(void *context,
                                                     lonejson_error *error) {
  cai_history_item_scan *scan = context;
  (void)error;
  scan->capture = scan->depth == 2U && strcmp(scan->key, "role") == 0   ? 1
                  : scan->depth == 2U && strcmp(scan->key, "type") == 0 ? 2
                  : (scan->depth == 2U && strcmp(scan->key, "content") == 0) ||
                          (scan->depth == 4U && strcmp(scan->key, "text") == 0)
                      ? 3
                      : 0;
  return LONEJSON_STATUS_OK;
}
static lonejson_status cai_history_scan_string_chunk(void *context,
                                                     const char *bytes,
                                                     size_t size,
                                                     lonejson_error *error) {
  cai_history_item_scan *scan = context;
  char *destination;
  size_t *used, capacity;
  (void)error;
  if (scan->capture == 0)
    return LONEJSON_STATUS_OK;
  if (scan->capture == 3 && EVP_DigestUpdate(scan->hash, bytes, size) != 1) {
    scan->failed = 1;
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  destination = scan->capture == 1   ? scan->role
                : scan->capture == 2 ? scan->type
                                     : scan->text;
  used = scan->capture == 1   ? &scan->role_used
         : scan->capture == 2 ? &scan->type_used
                              : &scan->text_used;
  capacity = scan->capture == 1   ? sizeof(scan->role)
             : scan->capture == 2 ? sizeof(scan->type)
                                  : sizeof(scan->text);
  if (size > capacity - *used - 1U)
    size = capacity - *used - 1U;
  memcpy(destination + *used, bytes, size);
  *used += size;
  destination[*used] = '\0';
  return LONEJSON_STATUS_OK;
}
static int cai_history_scan_messages(cai_history_rebuild *r,
                                     cai_session *session, cai_error *error) {
  cai_spooled_record_reader reader;
  cai_history_record_json_reader record;
  cai_history_item_scan scan;
  cai_history_message_mark *message;
  lonejson_value_visitor visitor;
  lonejson_error local;
  unsigned long length;
  size_t item = 0U;
  int have, kind, rc = CAI_OK;
  void *grown;
  memset(&reader, 0, sizeof(reader));
  reader.cursor = CAI_SESSION_IMPL(session)->history;
  lonejson_error_init(&local);
  if (reader.cursor.rewind(&reader.cursor, &local) != LONEJSON_STATUS_OK)
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "cannot rewind active conversation history");
  visitor = lonejson_default_value_visitor();
  visitor.object_begin = visitor.array_begin = cai_history_scan_begin;
  visitor.object_end = visitor.array_end = cai_history_scan_end;
  visitor.object_key_begin = cai_history_scan_key_begin;
  visitor.object_key_chunk = cai_history_scan_key_chunk;
  visitor.string_begin = cai_history_scan_string_begin;
  visitor.string_chunk = cai_history_scan_string_chunk;
  for (;;) {
    rc = cai_history_read_record_length(&reader, &length, &have, error);
    if (rc != CAI_OK || !have)
      break;
    memset(&scan, 0, sizeof(scan));
    scan.hash = EVP_MD_CTX_new();
    if (scan.hash == NULL ||
        EVP_DigestInit_ex(scan.hash, EVP_sha256(), NULL) != 1) {
      EVP_MD_CTX_free(scan.hash);
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "cannot fingerprint active history item");
    }
    record.records = &reader;
    record.remaining = length;
    record.error = error;
    if (CAI_LJ->visit_value_reader(CAI_LJ, cai_history_record_json_read,
                                   &record, &visitor, &scan,
                                   &local) != LONEJSON_STATUS_OK ||
        record.remaining != 0UL)
      rc = cai_set_error_detail(error, CAI_ERR_INVALID,
                                "cannot inspect active history item",
                                local.message);
    kind = strcmp(scan.type, "compaction") == 0                          ? 3
           : strcmp(scan.role, "user") == 0 && scan.text_used != 0U      ? 1
           : strcmp(scan.role, "assistant") == 0 && scan.text_used != 0U ? 4
                                                                         : 0;
    if (rc == CAI_OK && kind != 0) {
      grown = r->messages;
      rc = cai_history_grow(&grown, &r->message_capacity, r->message_count,
                            sizeof(*r->messages), error);
      if (rc == CAI_OK) {
        r->messages = grown;
        message = &r->messages[r->message_count++];
        memset(message, 0, sizeof(*message));
        message->kind = kind;
        message->item = item;
        message->input = (size_t)-1;
        cai_history_summary(message->text, sizeof(message->text), scan.text);
        if (EVP_DigestFinal_ex(scan.hash, message->digest, NULL) != 1)
          rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                             "cannot fingerprint active history item");
      }
    }
    EVP_MD_CTX_free(scan.hash);
    if (rc != CAI_OK)
      break;
    item++;
  }
  CAI_SESSION_IMPL(session)->history_items = item;
  CAI_SESSION_IMPL(session)->history_items_known = rc == CAI_OK;
  return rc;
}
static int cai_history_input_order(const void *a, const void *b) {
  const cai_history_input_mark *left = a, *right = b;
  if (left->committed != right->committed)
    return left->committed < right->committed ? -1 : 1;
  return left->sequence < right->sequence ? -1
                                          : left->sequence != right->sequence;
}
static int cai_history_message_order(const void *a, const void *b) {
  const cai_history_message_mark *left = a, *right = b;
  if (left->item != right->item)
    return left->item < right->item ? -1 : 1;
  /* Synthetic private shell records precede the next real input. */
  if (left->kind == 2 && right->kind != 2)
    return -1;
  if (right->kind == 2 && left->kind != 2)
    return 1;
  return left->input < right->input ? -1 : left->input != right->input;
}

int cai_history_rebuild_finish(cai_history_rebuild *r, cai_session *session,
                               cai_error *error) {
  cai_session_impl *impl = CAI_SESSION_IMPL(session);
  cai_history_message_mark *message;
  cai_history_input_mark *input;
  cai_history_block *blocks;
  size_t i, j, next, owner, cursor, last_compaction = 0U;
  unsigned long long next_turn;
  void *grown;
  int rc = cai_history_scan_messages(r, session, error);
  if (rc != CAI_OK)
    return rc;
  qsort(r->inputs, r->count, sizeof(*r->inputs), cai_history_input_order);
  cursor = r->count;
  for (i = r->message_count; i != 0U; i--) {
    message = &r->messages[i - 1U];
    if (message->kind == 3) {
      if (message->item + 1U > last_compaction)
        last_compaction = message->item + 1U;
      continue;
    }
    if (message->kind != 1)
      continue;
    for (j = cursor; j != 0U; j--) {
      input = &r->inputs[j - 1U];
      if (!input->private_shell && input->committed != 0U &&
          memcmp(input->digest, message->digest, 32U) == 0) {
        message->input = j - 1U;
        message->kind = input->kind;
        input->matched = 1;
        input->item = message->item;
        cursor = j - 1U;
        break;
      }
    }
    /* An unmatched leading user item is a local compaction summary. Steering
     * is identified from its journal record, never from role=user alone. */
    if (message->input == (size_t)-1 && message->item < last_compaction)
      message->kind = 0;
    else if (message->input == (size_t)-1 && r->count != 0U) {
      message->kind = 3;
      if (message->item + 1U > last_compaction)
        last_compaction = message->item + 1U;
    }
  }
  /* Add recorded private shell executions, which have no model input item. */
  for (i = 0U; i < r->count; i++) {
    input = &r->inputs[i];
    if (input->kind != 2 || !input->private_shell || input->committed == 0U)
      continue;
    next = impl->history_items;
    next_turn = ULLONG_MAX;
    for (j = i + 1U; j < r->count; j++)
      if (r->inputs[j].kind == 1 && r->inputs[j].matched) {
        next = r->inputs[j].item;
        next_turn = r->inputs[j].committed;
        break;
      }
    if (input->merge < 0) {
      input->merge = 0;
      for (j = 0U; j < r->assistant_count; j++)
        if (r->assistant_events[j] > input->completion &&
            r->assistant_events[j] < next_turn) {
          input->merge = 1;
          break;
        }
    }
    if (input->merge)
      continue;
    grown = r->messages;
    rc = cai_history_grow(&grown, &r->message_capacity, r->message_count,
                          sizeof(*r->messages), error);
    if (rc != CAI_OK)
      return rc;
    r->messages = grown;
    message = &r->messages[r->message_count++];
    memset(message, 0, sizeof(*message));
    message->kind = 2;
    message->input = i;
    message->item = next;
    strcpy(message->text, input->command);
    input->item = next;
  }
  qsort(r->messages, r->message_count, sizeof(*r->messages),
        cai_history_message_order);
  impl->history_blocks.count = 0U;
  for (i = 0U; i < r->message_count; i++) {
    message = &r->messages[i];
    input = message->input != (size_t)-1 ? &r->inputs[message->input] : NULL;
    if (message->item < last_compaction)
      continue;
    if (message->kind == 4) {
      cai_session_history_summary_delta(session, message->text,
                                        strlen(message->text), 1);
      continue;
    }
    if (message->kind == 0)
      continue;
    if (message->kind == 2 && input != NULL) {
      if (input->merge < 0) {
        input->merge = 0;
        for (j = i + 1U; j < r->message_count && r->messages[j].kind != 1 &&
                         r->messages[j].kind != 3;
             j++)
          if (r->messages[j].kind == 4)
            input->merge = 1;
      }
      if (input->merge)
        continue;
    }
    if (impl->history_blocks.count == 0U && message->item != 0U &&
        last_compaction != 0U) {
      rc = cai_session_history_begin(session, 3, 0U, 0U, "Compacted history",
                                     error);
      if (rc != CAI_OK)
        return rc;
      blocks = impl->history_blocks.items;
      blocks[0].first_item = 0LL;
    }
    rc = cai_session_history_begin(
        session, message->kind, input != NULL ? input->committed : 0U,
        input != NULL ? input->sequence : 0U,
        message->kind == 3                    ? "Compacted history"
        : message->kind == 2 && input != NULL ? input->command
                                              : message->text,
        error);
    if (rc != CAI_OK)
      return rc;
    blocks = impl->history_blocks.items;
    blocks[impl->history_blocks.count - 1U].first_item =
        impl->history_blocks.count == 1U && last_compaction == 0U
            ? 0LL
            : (long long)message->item;
  }
  if (impl->history_items != 0U && impl->history_blocks.count == 0U) {
    rc = cai_session_history_begin(session, 3, 0U, 0U, "Compacted history",
                                   error);
    if (rc != CAI_OK)
      return rc;
    blocks = impl->history_blocks.items;
    blocks[0].first_item = 0LL;
  }
  blocks = impl->history_blocks.items;
  /* Attach input and shell records that can precede the actual turn start,
   * such as queued prompts and steering accepted during a catalog request. */
  for (i = 0U; i < r->count && impl->history_blocks.count != 0U; i++) {
    input = &r->inputs[i];
    if (input->committed == 0U)
      continue;
    owner = 0U;
    for (j = 1U; j < impl->history_blocks.count; j++)
      if (blocks[j].event_start <= (long long)input->committed)
        owner = j;
    next = impl->history_blocks.count;
    impl->history_blocks.count = owner + 1U;
    rc = cai_session_history_own_event(session, input->sequence, error);
    for (j = 0U; rc == CAI_OK && j < r->shell_count; j++)
      if (r->shell_events[j].input == input->sequence)
        rc = cai_session_history_own_event(session, r->shell_events[j].sequence,
                                           error);
    impl->history_blocks.count = next;
    if (rc != CAI_OK)
      return rc;
  }
  return cai_session_history_prepare(session, error);
}
