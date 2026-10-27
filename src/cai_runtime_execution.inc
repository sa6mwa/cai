/* Included after the journal append primitive. All persistence uses the same
 * runtime sequence and store as conversation events. No logger is involved. */
typedef struct cai_runtime_execution_stream {
  cai_agent_runtime *runtime;
  cai_execution_record record;
  char bytes[CAI_EXECUTION_CHUNK_BYTES + 4U];
  size_t length;
  int next_line_start;
  const char *journal_type;
  int journal_only;
} cai_runtime_execution_stream;

static void cai_runtime_execution_init(cai_runtime_execution_stream *s,
                                       cai_agent_runtime *runtime,
                                       const char *kind, const char *tool,
                                       const char *call, const char *field) {
  struct timespec now;
  memset(s, 0, sizeof(*s));
  s->runtime = runtime;
  s->record.kind = kind;
  s->record.session_id = runtime->session_id;
  s->record.parent_session_id = runtime->execution_parent_session;
  s->record.parent_call_id = runtime->execution_parent_call;
  s->record.subagent = runtime->execution_subagent;
  s->record.tool_name = tool;
  s->record.call_id = call;
  s->record.field = field;
  if (runtime->agent != NULL)
    s->record.model = CAI_AGENT_IMPL(runtime->agent)->model;
  s->record.base_url = CAI_CLIENT_IMPL(runtime->client)->base_url;
  if (runtime->session != NULL)
    s->record.last_response_id =
        CAI_SESSION_IMPL(runtime->session)->previous_response_id;
  s->next_line_start = 1;
  if (clock_gettime(CLOCK_REALTIME, &now) == 0)
    s->record.timestamp_ns = (unsigned long long)now.tv_sec * 1000000000ULL +
                             (unsigned long long)now.tv_nsec;
}

static int cai_runtime_execution_emit_locked(cai_runtime_execution_stream *s,
                                             size_t count, int final,
                                             cai_error *error) {
  char *json = NULL;
  int rc;
  memcpy(s->record.data, s->bytes, count);
  s->record.data[count] = '\0';
  s->record.final = final;
  s->record.line_start = s->next_line_start;
  if (!s->journal_only)
    s->record.source_sequence = s->runtime->session_store != NULL
                                    ? s->runtime->next_event_sequence + 1U
                                    : 0U;
  rc = cai_execution_record_json(&s->record, &json, error);
  if (rc == CAI_OK)
    rc = cai_runtime_append_journal_event_locked(
        s->runtime, s->journal_type != NULL ? s->journal_type : "execution",
        json, NULL, error);
  if (rc == CAI_OK && !s->journal_only &&
      pthread_equal(s->runtime->owner_thread, pthread_self())) {
    cai_error notice;
    cai_error_init(&notice);
    /* Synchronous owner operations cannot wait for their own event pump.
     * The journal remains authoritative if a live notification cannot fit. */
    (void)cai_runtime_enqueue_nonblocking_locked(
        s->runtime, CAI_AGENT_EVENT_EXECUTION_RECORD, json, strlen(json),
        s->record.tool_name, s->record.call_id, s->runtime->state, &notice);
    cai_error_cleanup(&notice);
  } else if (rc == CAI_OK && !s->journal_only)
    rc = cai_runtime_enqueue_locked(
        s->runtime, CAI_AGENT_EVENT_EXECUTION_RECORD, json, strlen(json),
        s->record.tool_name, s->record.call_id, s->runtime->state, error);
  cai_free_mem(NULL, json);
  if (rc == CAI_OK) {
    if (count != 0U)
      s->next_line_start = s->bytes[count - 1U] == '\n';
    s->record.offset += count;
    s->length -= count;
    memmove(s->bytes, s->bytes + count, s->length);
  }
  return rc;
}

static int cai_runtime_execution_emit(cai_runtime_execution_stream *s,
                                      size_t count, int final,
                                      cai_error *error) {
  int rc;
  pthread_mutex_lock(&s->runtime->lock);
  rc = cai_runtime_execution_emit_locked(s, count, final, error);
  pthread_mutex_unlock(&s->runtime->lock);
  return rc;
}

static size_t cai_runtime_execution_utf8_prefix(const char *data, size_t n) {
  size_t start = n;
  unsigned char c;
  size_t need;
  while (start != 0U && ((unsigned char)data[start - 1U] & 0xc0U) == 0x80U)
    start--;
  if (start != 0U)
    start--;
  c = (unsigned char)data[start];
  need = c >= 0xf0U ? 4U : c >= 0xe0U ? 3U : c >= 0xc0U ? 2U : 1U;
  return start + need > n ? start : n;
}

static int cai_runtime_execution_write(void *context, const void *bytes,
                                       size_t count, cai_error *error) {
  cai_runtime_execution_stream *s = context;
  const char *input = bytes;
  size_t amount, prefix;
  int rc;
  if (count == 0U)
    return CAI_OK;
  if (memchr(bytes, '\0', count) != NULL)
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "execution payload contains a NUL byte");
  while (count != 0U) {
    amount = CAI_EXECUTION_CHUNK_BYTES - s->length;
    if (amount > count)
      amount = count;
    memcpy(s->bytes + s->length, input, amount);
    s->length += amount;
    input += amount;
    count -= amount;
    if (s->length == CAI_EXECUTION_CHUNK_BYTES) {
      prefix = cai_runtime_execution_utf8_prefix(s->bytes, s->length);
      if (prefix == 0U)
        return cai_set_error(error, CAI_ERR_PROTOCOL,
                             "invalid execution UTF-8 chunk");
      rc = cai_runtime_execution_emit(s, prefix, 0, error);
      if (rc != CAI_OK)
        return rc;
    }
  }
  return CAI_OK;
}

/* Errors may alias the callback's error output. Use an independent error so a
 * persistence failure cannot free strings while they are being recorded. */
static int
cai_runtime_execution_error_locked(cai_agent_runtime *runtime, const char *kind,
                                   const char *tool, const char *call,
                                   const cai_error *failure, cai_error *error) {
  cai_runtime_execution_stream s;
  const char *fields[4];
  const char *names[4] = {"message", "detail", "provider_code", "request_id"};
  size_t i, n, prefix, remaining;
  cai_error local;
  int rc;
  if (!runtime->record_transcript)
    return CAI_OK;
  fields[0] = failure != NULL ? failure->message : NULL;
  fields[1] = failure != NULL ? failure->detail : NULL;
  fields[2] = failure != NULL ? failure->server_code : NULL;
  fields[3] = failure != NULL ? failure->request_id : NULL;
  cai_error_init(&local);
  cai_runtime_execution_init(&s, runtime, kind, tool, call, NULL);
  s.record.code = failure != NULL ? failure->code : CAI_ERR_PROTOCOL;
  s.record.http_status = failure != NULL ? failure->http_status : 0L;
  rc = cai_runtime_execution_emit_locked(&s, 0U, 1, &local);
  for (i = 0U; rc == CAI_OK && i < 4U; i++) {
    const char *text = fields[i];
    if (text == NULL)
      continue;
    s.record.field = names[i];
    s.record.offset = 0U;
    s.next_line_start = 1;
    remaining = strlen(text);
    do {
      n = remaining;
      if (n > CAI_EXECUTION_CHUNK_BYTES)
        n = CAI_EXECUTION_CHUNK_BYTES;
      prefix = cai_runtime_execution_utf8_prefix(text, n);
      if (prefix == 0U && n != 0U) {
        rc = CAI_ERR_PROTOCOL;
        break;
      }
      memcpy(s.bytes, text, prefix);
      s.length = prefix;
      text += prefix;
      remaining -= prefix;
      rc = cai_runtime_execution_emit_locked(&s, prefix, *text == '\0', &local);
    } while (rc == CAI_OK && *text != '\0');
  }
  if (rc != CAI_OK)
    cai_set_error_detail(error, rc, "persist execution failure", local.message);
  cai_error_cleanup(&local);
  return rc;
}

static int cai_runtime_execution_tool(cai_agent_runtime *runtime,
                                      const cai_tool_event *event,
                                      cai_error *error) {
  cai_runtime_execution_stream s;
  cai_sink_callbacks callbacks;
  cai_sink *sink = NULL;
  cai_error local;
  int rc;
  if (!runtime->record_transcript)
    return CAI_OK;
  if (event->type == CAI_TOOL_EVENT_ERROR) {
    pthread_mutex_lock(&runtime->lock);
    rc = cai_runtime_execution_error_locked(runtime, "tool.failed", event->name,
                                            event->call_id, event->tool_error,
                                            error);
    pthread_mutex_unlock(&runtime->lock);
    if (rc != CAI_OK || event->output_json == NULL)
      return rc;
  }
  cai_runtime_execution_init(
      &s, runtime,
      event->type == CAI_TOOL_EVENT_START ? "tool.started" : "tool.completed",
      event->name, event->call_id, NULL);
  rc = event->type == CAI_TOOL_EVENT_ERROR
           ? CAI_OK
           : cai_runtime_execution_emit(&s, 0U, 1, error);
  if (rc != CAI_OK)
    return rc;
  s.record.kind = event->type == CAI_TOOL_EVENT_START   ? "tool.arguments"
                  : event->type == CAI_TOOL_EVENT_ERROR ? "tool.partial_output"
                                                        : "tool.output";
  s.record.field = "data";
  s.record.offset = 0U;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = cai_runtime_execution_write;
  callbacks.context = &s;
  cai_error_init(&local);
  rc = cai_sink_from_callbacks(&callbacks, &sink, &local);
  if (rc == CAI_OK)
    rc = event->type == CAI_TOOL_EVENT_START
             ? cai_tool_event_write_arguments(event, sink, &local)
             : cai_tool_event_write_output(event, sink, &local);
  cai_sink_close(sink);
  if (rc == CAI_OK)
    rc = cai_runtime_execution_emit(&s, s.length, 1, &local);
  if (rc != CAI_OK)
    cai_set_error_detail(error, rc, "persist tool execution", local.message);
  cai_error_cleanup(&local);
  return rc;
}

static int cai_runtime_execution_lifecycle_locked(cai_agent_runtime *runtime,
                                                  int type, const char *text,
                                                  cai_error *error) {
  cai_runtime_execution_stream s;
  const char *kind =
      type == CAI_AGENT_EVENT_RUN_STARTED            ? "turn.started"
      : type == CAI_AGENT_EVENT_RUN_COMPLETED        ? "turn.completed"
      : type == CAI_AGENT_EVENT_RUN_CANCELLED        ? "turn.cancelled"
      : type == CAI_AGENT_EVENT_COMPACTION_STARTED   ? "compaction.started"
      : type == CAI_AGENT_EVENT_COMPACTION_COMPLETED ? "compaction.completed"
      : type == CAI_AGENT_EVENT_RESPONSE_COMPLETED   ? "response.completed"
      : type == CAI_AGENT_EVENT_GOAL_CHANGED         ? "goal.changed"
                                                     : NULL;
  if (!runtime->record_transcript || kind == NULL)
    return CAI_OK;
  cai_runtime_execution_init(&s, runtime, kind, NULL, NULL, NULL);
  if (type == CAI_AGENT_EVENT_GOAL_CHANGED && text != NULL) {
    size_t n = strlen(text);
    s.record.field = "change";
    if (n > CAI_EXECUTION_CHUNK_BYTES)
      return cai_set_error(error, CAI_ERR_LIMIT,
                           "goal status exceeds execution chunk");
    memcpy(s.bytes, text, n);
    s.length = n;
    return cai_runtime_execution_emit_locked(&s, n, 1, error);
  }
  return cai_runtime_execution_emit_locked(&s, 0U, 1, error);
}

static int cai_runtime_execution_child_input_locked(cai_agent_runtime *runtime,
                                                    cai_error *error) {
  cai_runtime_execution_stream s;
  const char *text = runtime->active_user_turn;
  size_t remaining, n;
  int rc = CAI_OK;
  if (!runtime->record_transcript || runtime->execution_parent_call == NULL ||
      text == NULL)
    return CAI_OK;
  cai_runtime_execution_init(&s, runtime, "subagent.input", NULL, NULL,
                             "instructions");
  remaining = strlen(text);
  do {
    n = remaining < CAI_EXECUTION_CHUNK_BYTES ? remaining
                                              : CAI_EXECUTION_CHUNK_BYTES;
    n = cai_runtime_execution_utf8_prefix(text, n);
    if (n == 0U && remaining != 0U)
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "invalid subagent input UTF-8");
    memcpy(s.bytes, text, n);
    s.length = n;
    text += n;
    remaining -= n;
    rc = cai_runtime_execution_emit_locked(&s, n, remaining == 0U, error);
  } while (rc == CAI_OK && remaining != 0U);
  return rc;
}

static int cai_runtime_execution_hosted(void *context, const char *id,
                                        int index, const char *type,
                                        const lonejson_spooled *item,
                                        cai_error *error) {
  cai_agent_runtime *runtime = context;
  cai_runtime_execution_stream s;
  cai_tool_event source;
  cai_sink_callbacks callbacks;
  cai_sink *sink = NULL;
  int rc;
  size_t length;
  (void)index;
  if (type == NULL)
    return CAI_OK;
  length = strlen(type);
  if (length < 5U || strcmp(type + length - 5U, "_call") != 0 ||
      strcmp(type, "function_call") == 0 ||
      strcmp(type, "custom_tool_call") == 0)
    return CAI_OK;
  pthread_mutex_lock(&runtime->lock);
  runtime->goal_turn_has_activity = 1;
  pthread_mutex_unlock(&runtime->lock);
  if (!runtime->record_transcript)
    return CAI_OK;
  cai_runtime_execution_init(&s, runtime, "hosted.tool", type, id, "item");
  memset(&source, 0, sizeof(source));
  source.output_json = item;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = cai_runtime_execution_write;
  callbacks.context = &s;
  rc = cai_sink_from_callbacks(&callbacks, &sink, error);
  if (rc == CAI_OK)
    rc = cai_tool_event_write_output(&source, sink, error);
  cai_sink_close(sink);
  if (rc == CAI_OK)
    rc = cai_runtime_execution_emit(&s, s.length, 1, error);
  return rc;
}

static int cai_runtime_execution_parent(cai_agent_runtime *child,
                                        cai_agent_runtime *parent,
                                        const char *call, const char *profile,
                                        cai_error *error) {
  child->execution_parent_session = cai_strdup(NULL, parent->session_id);
  child->execution_parent_call = cai_strdup(NULL, call);
  child->execution_subagent = cai_strdup(NULL, profile);
  if (child->execution_parent_session != NULL &&
      child->execution_parent_call != NULL &&
      child->execution_subagent != NULL) {
    if (parent->record_transcript)
      child->journal_parent = parent;
    return CAI_OK;
  }
  return cai_set_error(error, CAI_ERR_NOMEM,
                       "retain execution parent identity");
}

/* Copy the child's durable journal, rather than whichever live observations
 * a host happens to pump. Parent sequence numbers order delivery; the bounded
 * envelope retains the child sequence and delegation identity. Each forwarder
 * retains only a bounded source chunk; no additional worker or whole journal
 * is staged. Source and destination locks are never held together. */
static int
cai_runtime_mirror_journal_locked(cai_agent_runtime *child,
                                  const cai_agent_session_event *event,
                                  cai_error *error) {
  cai_agent_runtime *parent = child->journal_parent;
  cai_runtime_execution_stream s;
  struct timespec now;
  int rc;
  /* Admission can run concurrently with inference. Envelope identity is
   * immutable; unlike worker-generated execution records it must not inspect
   * the child's model or last-response pointers while its session changes. */
  memset(&s, 0, sizeof(s));
  s.record.kind = event->type;
  s.record.session_id = child->session_id;
  s.record.parent_session_id = child->execution_parent_session;
  s.record.parent_call_id = child->execution_parent_call;
  s.record.subagent = child->execution_subagent;
  s.next_line_start = 1;
  if (clock_gettime(CLOCK_REALTIME, &now) == 0)
    s.record.timestamp_ns = (unsigned long long)now.tv_sec * 1000000000ULL +
                            (unsigned long long)now.tv_nsec;
  s.runtime = parent;
  s.record.source_sequence = event->sequence;
  s.journal_type = "subagent";
  s.journal_only = 1;
  pthread_mutex_unlock(&child->lock);
  if (strcmp(event->type, "execution") == 0 ||
      strcmp(event->type, "subagent") == 0) {
    pthread_mutex_lock(&parent->lock);
    rc = cai_runtime_append_journal_event_locked(parent, event->type,
                                                 event->data, NULL, error);
    pthread_mutex_unlock(&parent->lock);
  } else {
    rc = event->data != NULL ? cai_runtime_execution_write(
                                   &s, event->data, strlen(event->data), error)
                             : CAI_OK;
    if (rc == CAI_OK)
      rc = cai_runtime_execution_emit(&s, s.length, 1, error);
  }
  pthread_mutex_lock(&child->lock);
  return rc;
}

/* A delivered prompt has the same full-string presentation contract as a
 * normal user prompt. Preserve that projection at its actual seam, including
 * poll-only/manual reviews. Attributed source queue/receipt events above
 * remain the complete inspection record; this projection is never model input.
 */
static int cai_runtime_child_input_projection_locked(cai_agent_runtime *child,
                                                     const char *text,
                                                     cai_error *error) {
  cai_agent_runtime *parent = child->journal_parent;
  int rc;
  if (parent == NULL)
    return CAI_OK;
  pthread_mutex_unlock(&child->lock);
  pthread_mutex_lock(&parent->lock);
  rc = cai_runtime_append_journal_event_locked(parent, "subagent_input", text,
                                               NULL, error);
  pthread_mutex_unlock(&parent->lock);
  pthread_mutex_lock(&child->lock);
  return rc;
}

/* Validation already owns a bounded complete report. Persist its readable
 * projection in bounded UTF-8 chunks, so parent replay does not depend on
 * reconstructing a child runtime or accepting raw JSON as findings. */
static int cai_runtime_review_markdown_locked(cai_agent_runtime *runtime,
                                              const char *markdown,
                                              cai_error *error) {
  char chunk[CAI_EXECUTION_CHUNK_BYTES + 1U];
  size_t remaining = strlen(markdown), n;
  int rc = CAI_OK;
  while (remaining != 0U && rc == CAI_OK) {
    n = remaining < CAI_EXECUTION_CHUNK_BYTES ? remaining
                                              : CAI_EXECUTION_CHUNK_BYTES;
    n = cai_runtime_execution_utf8_prefix(markdown, n);
    if (n == 0U)
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "invalid review Markdown UTF-8");
    memcpy(chunk, markdown, n);
    chunk[n] = '\0';
    rc = cai_runtime_append_journal_event_locked(
        runtime, "review_markdown_delta", chunk, NULL, error);
    markdown += n;
    remaining -= n;
  }
  if (rc == CAI_OK)
    rc = cai_runtime_append_journal_event_locked(runtime, "review_markdown_end",
                                                 NULL, NULL, error);
  return rc;
}

/* Standalone reviews are pumped by the UI, rather than the parent worker. */
static int cai_runtime_manual_review_event(void *context,
                                           const cai_agent_runtime_event *event,
                                           cai_error *error) {
  cai_agent_runtime *parent = context;
  return parent->review_event_callback != NULL
             ? parent->review_event_callback(parent->review_event_context,
                                             event, error)
             : CAI_OK;
}

typedef struct cai_runtime_execution_export {
  cai_agent_runtime *runtime;
  cai_sink *sink;
  int started;
} cai_runtime_execution_export;

static int cai_runtime_execution_export_event(
    void *context, const cai_agent_session_event *event, cai_error *error) {
  cai_runtime_execution_export *out = context;
  cai_execution_record record;
  int rc;
  if ((strcmp(event->type, "execution") != 0 &&
       strcmp(event->type, "subagent") != 0) ||
      !cai_session_history_visible(out->runtime->session, event->sequence))
    return CAI_OK;
  if (!out->started) {
    static const char heading[] = "\n\n## Execution history\n\n*Recorded "
                                  "activity; never replayed as model input.*\n";
    rc = cai_sink_write(out->sink, heading, sizeof(heading) - 1U, error);
    if (rc != CAI_OK)
      return rc;
    out->started = 1;
  }
  cai_execution_record_init(&record);
  rc = cai_execution_record_parse(event->data, &record, error);
  if (rc != CAI_OK) {
    char notice[192];
    int count = snprintf(notice, sizeof(notice),
                         "\n\n> Execution record %llu could not be decoded; "
                         "its original data remains in the JSONL export.\n",
                         event->sequence);
    cai_execution_record_cleanup(&record);
    cai_error_cleanup(error);
    cai_error_init(error);
    return cai_sink_write(out->sink, notice, (size_t)count, error);
  }
  if (rc == CAI_OK)
    rc = cai_execution_record_write_markdown(&record, 0, out->sink, error);
  cai_execution_record_cleanup(&record);
  return rc;
}

static int cai_runtime_export_execution_markdown(cai_agent_runtime *runtime,
                                                 cai_sink *sink,
                                                 cai_error *error) {
  cai_runtime_execution_export out;
  if (runtime->session_store == NULL)
    return CAI_OK;
  memset(&out, 0, sizeof(out));
  out.runtime = runtime;
  out.sink = sink;
  return runtime->session_store->load_events_after(
      runtime->session_store->context, runtime->session_scope,
      runtime->session_id, 0U, cai_runtime_execution_export_event, &out, error);
}
