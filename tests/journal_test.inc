typedef struct journal_bytes {
  char *data;
  size_t used, offset, chunk;
  cai_session *observe;
  int streaming;
} journal_bytes;
static void journal_bytes_append(journal_bytes *b, const void *data,
                                 size_t count) {
  b->data = realloc(b->data, b->used + count + 1U);
  assert(b->data != NULL);
  memcpy(b->data + b->used, data, count);
  b->used += count;
  b->data[b->used] = '\0';
}
static size_t journal_bytes_read(void *context, void *data, size_t count,
                                 cai_error *error) {
  journal_bytes *b = context;
  (void)error;
  if (b->observe != NULL && b->offset < 32768U &&
      CAI_SESSION_IMPL(b->observe)
              ->history.size_fn(&CAI_SESSION_IMPL(b->observe)->history) > 0U)
    b->streaming = 1;
  if (count > b->used - b->offset)
    count = b->used - b->offset;
  if (b->chunk != 0U && count > b->chunk)
    count = b->chunk;
  memcpy(data, b->data + b->offset, count);
  b->offset += count;
  return count;
}
static void journal_bytes_capture(cai_source *source, journal_bytes *bytes,
                                  size_t chunk, cai_error *error) {
  char buffer[4096];
  size_t count;
  while ((count = cai_source_read(source, buffer, chunk, error)) != 0U)
    journal_bytes_append(bytes, buffer, count);
  assert(error->code == CAI_OK);
}
static int journal_bytes_restore(cai_session *session, journal_bytes *bytes,
                                 cai_error *error) {
  cai_source_callbacks callbacks;
  cai_source *source;
  int rc;
  bytes->offset = 0U;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.read = journal_bytes_read;
  callbacks.context = bytes;
  assert(cai_source_from_callbacks(&callbacks, &source, error) == CAI_OK);
  rc = session->restore_journal_source(session, source, error);
  cai_source_close(source);
  return rc;
}
static void journal_test_commit(cai_session *session, journal_bytes *journal,
                                cai_error *error) {
  cai_source *source;
  journal_bytes edit = {0};
  assert(cai_session_journal_source(session, &source, error) == CAI_OK);
  journal_bytes_capture(source, &edit, journal->used == 0U ? 4096U : 1U, error);
  cai_source_close(source);
  journal_bytes_append(journal, "{\"record_type\":\"commit\",\"state\":", 32U);
  journal_bytes_append(journal, edit.data, edit.used);
  journal_bytes_append(journal, "}\n", 2U);
  assert(cai_session_journal_accept(session, error) == CAI_OK);
  free(edit.data);
}
static void journal_test_equal(cai_session *a, cai_session *b,
                               cai_error *error) {
  cai_source *source;
  journal_bytes left = {0}, right = {0};
  cai_usage_accounting a_usage, b_usage;
  assert(a->export_state_source(a, &source, error) == CAI_OK);
  journal_bytes_capture(source, &left, 4096U, error);
  cai_source_close(source);
  assert(b->export_state_source(b, &source, error) == CAI_OK);
  journal_bytes_capture(source, &right, 4096U, error);
  cai_source_close(source);
  assert(left.used == right.used &&
         memcmp(left.data, right.data, left.used) == 0);
  assert(a->usage(a, &a_usage, error) == CAI_OK);
  assert(b->usage(b, &b_usage, error) == CAI_OK);
  assert(memcmp(&a_usage.usage, &b_usage.usage, sizeof(a_usage.usage)) == 0);
  assert(a_usage.estimated_spend_usd == b_usage.estimated_spend_usd);
  assert(a_usage.estimated_spend_eur == b_usage.estimated_spend_eur);
  free(left.data);
  free(right.data);
}
static void journal_test_invalid(cai_agent *agent, cai_error *error) {
  static const char valid[] =
      "{\"record_type\":\"commit\",\"state\":{\"revision\":1,"
      "\"base_bytes\":0,\"end_bytes\":7,\"blocks_keep\":0,"
      "\"hidden_keep\":0,\"owned_keep\":0,\"metadata\":{\"version\":1},"
      "\"history_append\":\"4\\n[{}]\\n\"}}\n";
  static const struct {
    const char *before, *after;
  } cases[] = {
      {"\"revision\":1", "\"revision\":2"},
      {"\"base_bytes\":0", "\"base_bytes\":1"},
      {"\"end_bytes\":7", "\"end_bytes\":8"},
      {"\"end_bytes\":7", "\"end_bytes\":6"},
      {"\"blocks_keep\":0", "\"blocks_keep\":1"},
      {"\"hidden_keep\":0", "\"hidden_keep\":1"},
      {"\"owned_keep\":0", "\"owned_keep\":1"},
      {"\"version\":1", "\"version\":99"},
      {"\"version\":1", "\"version\":1,\"history\":[]"},
      {"[{}]", "[xx]"},
      {"\"history_append\":\"4\\n[{}]\\n\"", "\"history_append\":null"},
      {"\"history_append\":\"4\\n[{}]\\n\"", "\"history_append\":{}"},
      {"\"history_append\"", "\"missing_history\""},
      {"\"metadata\":", "\"accounting\":{\"total_tokens\":-1},\"metadata\":"},
      {"\"commit\"", "\"unknown\""}};
  cai_session *session;
  journal_bytes bytes = {0};
  const char *found;
  size_t i;
  journal_bytes_append(&bytes, valid, strlen(valid));
  bytes.chunk = 1U;
  assert(cai_agent_new_session(agent, &session, error) == CAI_OK);
  assert(journal_bytes_restore(session, &bytes, error) == CAI_OK);
  session->close(session);
  free(bytes.data);
  for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); i++) {
    memset(&bytes, 0, sizeof(bytes));
    bytes.chunk = 1U;
    found = strstr(valid, cases[i].before);
    assert(found != NULL);
    journal_bytes_append(&bytes, valid, (size_t)(found - valid));
    journal_bytes_append(&bytes, cases[i].after, strlen(cases[i].after));
    found += strlen(cases[i].before);
    journal_bytes_append(&bytes, found, strlen(found));
    assert(cai_agent_new_session(agent, &session, error) == CAI_OK);
    assert(journal_bytes_restore(session, &bytes, error) != CAI_OK);
    session->close(session);
    cai_error_cleanup(error);
    free(bytes.data);
  }
}
static void test_journal_edits(void) {
  cai_client_config config;
  cai_agent_config agent_config;
  cai_client *client;
  cai_agent *agent;
  cai_session *session, *restored;
  cai_source *source;
  cai_error error;
  journal_bytes journal = {0}, edit = {0}, retry = {0}, compact = {0},
                bad = {0};
  char *large, prompt[80];
  lonejson_config json_config = lonejson_default_config();
  size_t i, before, large_size = json_config.max_dynamic_string_bytes + 65536U;
  cai_error_init(&error);
  cai_client_config_init(&config);
  config.api_key = "fixture";
  config.base_url = "http://127.0.0.1:1/v1";
  cai_agent_config_init(&agent_config);
  agent_config.enable_local_history = 1;
  agent_config.session_continuity = CAI_SESSION_CONTINUITY_CLIENT_HISTORY;
  agent_config.model = "gpt-6-luna";
  /* Exercise spill-backed cursors as well as tiny parser/source chunks. */
  agent_config.history_memory_limit = 128U;
  assert(cai_client_open(&config, &client, &error) == CAI_OK);
  assert(cai_client_new_agent(client, &agent_config, &agent, &error) == CAI_OK);
  journal_test_invalid(agent, &error);
  assert(cai_agent_new_session(agent, &session, &error) == CAI_OK);
  large = malloc(large_size + 1U);
  assert(large != NULL);
  memset(large, 'x', large_size);
  memcpy(large, "UNIQUE_LARGE_CONTEXT ", 21U);
  large[large_size] = '\0';
  assert(session->add_user_text(session, large, &error) == CAI_OK);
  assert(cai_session_commit_pending_inputs(session, &error) == CAI_OK);
  journal_test_commit(session, &journal, &error);
  free(large);
  for (i = 0U; i < 16U; i++) {
    snprintf(prompt, sizeof(prompt), "journal-turn-%lu: λ \\\"\n",
             (unsigned long)i);
    assert(cai_session_history_begin(session, CAI_AGENT_HISTORY_TURN, i + 1U,
                                     i + 1U, "unit turn", &error) == CAI_OK);
    assert(session->add_user_text(session, prompt, &error) == CAI_OK);
    assert(cai_session_commit_pending_inputs(session, &error) == CAI_OK);
    before = journal.used;
    journal_test_commit(session, &journal, &error);
    assert(journal.used - before < 4096U);
    assert(strstr(journal.data + before, "UNIQUE_LARGE_CONTEXT") == NULL);
  }
  CAI_SESSION_IMPL(session)->usage.usage.input_tokens = 30LL;
  CAI_SESSION_IMPL(session)->usage.usage.input_cached_tokens = 10LL;
  CAI_SESSION_IMPL(session)->usage.usage.output_tokens = 18LL;
  CAI_SESSION_IMPL(session)->usage.usage.output_reasoning_tokens = 5LL;
  CAI_SESSION_IMPL(session)->usage.usage.total_tokens = 48LL;
  CAI_SESSION_IMPL(session)->usage.estimated_spend_usd = 0.125;
  CAI_SESSION_IMPL(session)->usage.estimated_spend_eur = 0.25;
  assert(session->set_previous_response_id(
             session, "response-after-metadata-edit", &error) == CAI_OK);
  assert(cai_session_journal_source(session, &source, &error) == CAI_OK);
  journal_bytes_capture(source, &edit, 4096U, &error);
  cai_source_close(source);
  /* Reading or abandoning a producer never advances its durable cursor. */
  assert(cai_session_journal_source(session, &source, &error) == CAI_OK);
  journal_bytes_capture(source, &retry, 1U, &error);
  cai_source_close(source);
  assert(edit.used == retry.used &&
         memcmp(edit.data, retry.data, edit.used) == 0);
  assert(strstr(edit.data, "\"history_append\":\"\"") != NULL);
  journal_test_commit(session, &journal, &error);
  assert(cai_agent_new_session(agent, &restored, &error) == CAI_OK);
  journal.chunk = 3U;
  journal.observe = restored;
  assert(journal_bytes_restore(restored, &journal, &error) == CAI_OK);
  assert(journal.streaming);
  journal.observe = NULL;
  journal_test_equal(session, restored, &error);
  restored->close(restored);
  /* A trim is a prefix edit, with no retransmission of retained context. */
  assert(cai_session_history_trim(session, 2U, 100U, &error) == CAI_OK);
  before = journal.used;
  journal_test_commit(session, &journal, &error);
  assert(strstr(journal.data + before, "\"history_append\":\"\"") != NULL);
  assert(cai_agent_new_session(agent, &restored, &error) == CAI_OK);
  assert(journal_bytes_restore(restored, &journal, &error) == CAI_OK);
  journal_test_equal(session, restored, &error);
  restored->close(restored);
  /* Compaction replaces context once, retaining opaque provider items. */
  journal_bytes_append(
      &compact,
      "[{\"type\":\"compaction\",\"encrypted_content\":\"opaque-context\"}]",
      60U);
  {
    cai_source_callbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.read = journal_bytes_read;
    callbacks.context = &compact;
    assert(cai_source_from_callbacks(&callbacks, &source, &error) == CAI_OK);
    assert(session->import_history_source(session, source, &error) == CAI_OK);
    cai_source_close(source);
  }
  cai_session_history_compacted(session);
  before = journal.used;
  journal_test_commit(session, &journal, &error);
  assert(strstr(journal.data + before, "opaque-context") != NULL);
  assert(cai_agent_new_session(agent, &restored, &error) == CAI_OK);
  assert(journal_bytes_restore(restored, &journal, &error) == CAI_OK);
  journal_test_equal(session, restored, &error);
  restored->close(restored);
  /* Duplicate, reordered/missing and truncated records fail closed. */
  journal_bytes_append(&bad, journal.data, journal.used);
  journal_bytes_append(&bad, journal.data, journal.used);
  assert(cai_agent_new_session(agent, &restored, &error) == CAI_OK);
  assert(journal_bytes_restore(restored, &bad, &error) != CAI_OK);
  cai_error_cleanup(&error);
  restored->close(restored);
  bad.used = journal.used - 7U;
  assert(cai_agent_new_session(agent, &restored, &error) == CAI_OK);
  assert(journal_bytes_restore(restored, &bad, &error) != CAI_OK);
  cai_error_cleanup(&error);
  restored->close(restored);
  session->close(session);
  agent->close(agent);
  client->close(client);
  free(journal.data);
  free(edit.data);
  free(retry.data);
  free(compact.data);
  free(bad.data);
  cai_error_cleanup(&error);
}
