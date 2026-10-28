/* Test the actual encrypted backend, including interrupted directory indexing.
 */
#include "../cli/pouch.c"
#include <assert.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>

static int query_calls;
static int (*real_query)(lc_client *, const lc_query_req *,
                         const lc_query_key_handler *, void *, lc_query_res *,
                         lc_error *);
static int query_count(lc_client *client, const lc_query_req *req,
                       const lc_query_key_handler *handler, void *context,
                       lc_query_res *result, lc_error *error) {
  query_calls++;
  return real_query(client, req, handler, context, result, error);
}

static size_t read_state(void *context, void *bytes, size_t capacity,
                         cai_error *error) {
  size_t *offset = context;
  size_t length = 2U - *offset;
  (void)error;
  if (length > capacity)
    length = capacity;
  memcpy(bytes, "{}" + *offset, length);
  *offset += length;
  return length;
}

static void save(cai_cli_pouch *pouch, const char *scope, const char *id,
                 cai_error *error) {
  cai_source *source = NULL;
  size_t offset = 0U;
  cai_source_callbacks callbacks;
  int rc;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.read = read_state;
  callbacks.context = &offset;
  assert(cai_source_from_callbacks(&callbacks, &source, error) == CAI_OK);
  rc = pouch->store.commit(pouch, scope, id, source, 1U, error);
  if (rc != CAI_OK)
    fprintf(stderr, "save %s: %s\n", id, error->message);
  assert(rc == CAI_OK);
  cai_source_close(source);
}

static void newest(cai_cli_pouch *pouch, const char *scope,
                   const char *expected, cai_error *error) {
  char id[CAI_AGENT_SESSION_ID_MAX];
  cai_source *source = NULL;
  unsigned long long sequence = 0;
  assert(pouch->store.load_latest(pouch, scope, id, sizeof(id), &source,
                                  &sequence, error) == CAI_OK);
  assert((source != NULL) == (expected != NULL));
  if (expected != NULL) {
    assert(strcmp(id, expected) == 0);
    assert(sequence == 1U);
  }
  cai_source_close(source);
}

static int observed;
static int consumed;
static int begin_replay(void *context, const char *id, cai_error *error) {
  (void)context;
  (void)error;
  assert(strcmp(id, "second") == 0);
  assert(observed == 0 && consumed == 0);
  return CAI_OK;
}
static int recover(void *context, const cai_agent_session_event *event,
                   cai_error *error) {
  (void)context;
  (void)error;
  assert(event->sequence == (unsigned long long)++consumed);
  assert(consumed == observed + 1);
  return CAI_OK;
}
static int replay(void *context, const cai_agent_session_event *event,
                  cai_error *error) {
  (void)context;
  (void)error;
  assert(event->sequence == (unsigned long long)++observed);
  assert(observed == consumed);
  return CAI_OK;
}
static int reject(void *context, const cai_agent_session_event *event,
                  cai_error *error) {
  (void)context;
  (void)event;
  return cai_cli_error(error, CAI_ERR_TRANSPORT, "replay sink failed");
}

static void interrupted_idle_writer(void) {
  cai_cli_pouch pouch;
  cai_error error;
  int ready[2], status, received;
  pid_t child;
  struct pollfd descriptor;
  char ready_byte;
  assert(pipe(ready) == 0);
  child = fork();
  assert(child >= 0);
  if (child == 0) {
    close(ready[0]);
    cai_error_init(&error);
    assert(cai_cli_pouch_open(&pouch, NULL, NULL, NULL, &error) == CAI_OK);
    save(&pouch, "/work/idle-crash", "idle-crash", &error);
    assert(write(ready[1], "R", 1U) == 1);
    close(ready[1]);
    /* Leave the handle and background worker open, with no foreground work.
     * SIGKILL models an unclean exit without generating a system core file. */
    for (;;)
      pause();
  }
  close(ready[1]);
  descriptor.fd = ready[0];
  descriptor.events = POLLIN;
  descriptor.revents = 0;
  received = poll(&descriptor, 1U, 10000) == 1 &&
             read(ready[0], &ready_byte, 1U) == 1 && ready_byte == 'R';
  close(ready[0]);
  (void)kill(child, SIGKILL);
  assert(waitpid(child, &status, 0) == child);
  assert(received && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
  cai_error_init(&error);
  assert(cai_cli_pouch_open(&pouch, NULL, NULL, NULL, &error) == CAI_OK);
  newest(&pouch, "/work/idle-crash", "idle-crash", &error);
  {
    cai_cli_session *listed = NULL;
    size_t count = 0U;
    assert(cai_cli_pouch_list(&pouch, "/work/idle-crash", &listed, &count,
                              &error) == CAI_OK);
    assert(count == 1U && strcmp(listed[0].id, "idle-crash") == 0);
    free(listed);
  }
  cai_cli_pouch_close(&pouch);
  cai_error_cleanup(&error);
}

static void todo_write_document(cai_cli_pouch *pouch, void *txn,
                                const char *label, size_t chunks,
                                cai_error *error) {
  const cai_todo_store_callbacks *store = cai_cli_pouch_todo_callbacks();
  lonejson_sink_fn sink;
  void *context;
  lonejson_error json_error;
  char bytes[4096];
  size_t i;
  memset(bytes, 'x', sizeof(bytes));
  lonejson_error_init(&json_error);
  assert(store->open_write(pouch, txn, &sink, &context, error) == CAI_OK);
  assert(sink(context, "{\"label\":\"", 10U, &json_error) ==
         LONEJSON_STATUS_OK);
  assert(sink(context, label, strlen(label), &json_error) ==
         LONEJSON_STATUS_OK);
  assert(sink(context, "\",\"padding\":\"", 13U, &json_error) ==
         LONEJSON_STATUS_OK);
  for (i = 0U; i < chunks; i++)
    assert(sink(context, bytes, sizeof(bytes), &json_error) ==
           LONEJSON_STATUS_OK);
  assert(sink(context, "\"}", 2U, &json_error) == LONEJSON_STATUS_OK);
  assert(store->commit_write(pouch, txn, error) == CAI_OK);
}
static void todo_read_document(cai_cli_pouch *pouch, void *txn,
                               const char *label, size_t expected,
                               cai_error *error) {
  const cai_todo_store_callbacks *store = cai_cli_pouch_todo_callbacks();
  lonejson_reader_fn reader;
  lonejson_read_result result;
  void *context;
  unsigned char bytes[4096];
  size_t total = 0U;
  int found = 0;
  assert(store->open_read(pouch, txn, &reader, &context, error) == CAI_OK);
  do {
    result = reader(context, bytes, sizeof(bytes) - 1U);
    assert(!result.error_code);
    bytes[result.bytes_read] = '\0';
    if (strstr((char *)bytes, label) != NULL)
      found = 1;
    total += result.bytes_read;
  } while (!result.eof);
  store->close_read(pouch, txn, context);
  assert(found && total == expected);
}
static int reject_todo_update(lc_lease *lease, lc_source *source,
                              const lc_update_opts *options, lc_error *error) {
  (void)lease;
  (void)source;
  (void)options;
  error->code = LC_ERR_TRANSPORT;
  error->message = strdup("injected kanban commit failure");
  return LC_ERR_TRANSPORT;
}
static void todo_transactions(cai_cli_pouch *pouch, cai_error *error) {
  const cai_todo_store_callbacks *store = cai_cli_pouch_todo_callbacks();
  void *txn = NULL;
  size_t length = 10U + strlen("committed") + 13U + 64U * 4096U + 2U;
  assert(store->begin(pouch, &txn, error) == CAI_OK);
  todo_write_document(pouch, txn, "committed", 64U, error);
  todo_read_document(pouch, txn, "committed", length, error);
  /* Repeated rewrites read the most recent staged bytes in the same txn. */
  todo_write_document(pouch, txn, "committed", 64U, error);
  assert(store->commit(pouch, txn, error) == CAI_OK);
  assert(store->begin(pouch, &txn, error) == CAI_OK);
  todo_read_document(pouch, txn, "committed", length, error);
  todo_write_document(pouch, txn, "rolled back", 40U, error);
  store->rollback(pouch, txn);
  assert(store->begin(pouch, &txn, error) == CAI_OK);
  todo_write_document(pouch, txn, "failed commit", 2U, error);
  ((todo_transaction *)txn)->lease->update = reject_todo_update;
  assert(store->commit(pouch, txn, error) == CAI_ERR_TRANSPORT);
  assert(strstr(error->message, "injected kanban commit failure") != NULL);
  cai_error_cleanup(error);
  cai_error_init(error);
  assert(store->begin(pouch, &txn, error) == CAI_OK);
  todo_read_document(pouch, txn, "committed", length, error);
  {
    lonejson_sink_fn sink;
    void *context;
    lonejson_error local;
    lonejson_error_init(&local);
    assert(store->open_write(pouch, txn, &sink, &context, error) == CAI_OK);
    assert(sink(context, "{\"partial\":", 11U, &local) == LONEJSON_STATUS_OK);
    /* Closing before EOF must abort, rather than publish truncated JSON. */
    store->rollback(pouch, txn);
  }
  assert(store->begin(pouch, &txn, error) == CAI_OK);
  todo_read_document(pouch, txn, "committed", length, error);
  assert(store->commit(pouch, txn, error) == CAI_OK);
}

static void oversized_events(cai_cli_pouch *pouch, const char *scope,
                             const char *id, cai_error *error) {
  cai_agent_session_event event;
  lonejson_config config = lonejson_default_config();
  char *data = malloc(config.max_dynamic_string_bytes + 2U);
  assert(data != NULL);
  memset(data, 'Z', config.max_dynamic_string_bytes + 1U);
  data[config.max_dynamic_string_bytes + 1U] = '\0';
  event.type = "assistant_text_delta";
  event.sequence = 1000000U;
  event.data = "Before omitted terminal transcript";
  assert(append_event(pouch, scope, id, &event, error) == CAI_OK);
  event.sequence++;
  event.type = "user_shell_completed";
  event.data = data;
  assert(append_event(pouch, scope, id, &event, error) == CAI_OK);
  event.sequence++;
  event.type = "assistant_text_delta";
  event.data = "After omitted terminal transcript";
  assert(append_event(pouch, scope, id, &event, error) == CAI_OK);
  event.sequence++;
  event.type = "turn_submitted";
  event.data = data;
  assert(append_event(pouch, scope, id, &event, error) == CAI_OK);
  free(data);
}

static int recovered_event(void *context, const cai_agent_session_event *event,
                           cai_error *error) {
  unsigned long long *last = context;
  (void)error;
  assert(event->sequence == *last + 1U);
  *last = event->sequence;
  if (*last == 1000001U || *last == 1000003U) {
    assert(strcmp(event->type, "replay_event_omitted") == 0);
    assert(event->data == NULL);
  } else {
    assert(strcmp(event->type, "assistant_text_delta") == 0);
    assert(event->data != NULL);
  }
  return CAI_OK;
}

static void replay_limits(cai_cli_pouch *pouch, cai_error *error) {
  lonejson_config config = lonejson_default_config();
  cli_record original, parsed;
  cai_source *source;
  unsigned long long last;
  size_t length;
  int limited, rc;
  char *data = malloc(config.max_dynamic_string_bytes + 2U);
  assert(data != NULL);
  for (length = config.max_dynamic_string_bytes - 1U;
       length <= config.max_dynamic_string_bytes + 1U; length++) {
    memset(data, 'x', length);
    data[length] = '\0';
    memset(&original, 0, sizeof(original));
    strcpy(original.record_type, "event");
    original.sequence = 1U;
    original.type = "assistant_text_delta";
    original.data = data;
    memset(&parsed, 0, sizeof(parsed));
    assert(cai_cli_mapped_source(&event_map, &original, &source, error) ==
           CAI_OK);
    rc = parse_source_optional(source, &event_map, &parsed, NULL, &limited,
                               error);
    assert(limited == (length > config.max_dynamic_string_bytes));
    assert((rc == CAI_OK) == !limited);
    if (!limited)
      assert(strlen(parsed.data) == length);
    cai_source_close(source);
    lonejson_cleanup(&event_map, &parsed);
    cai_error_cleanup(error);
    cai_error_init(error);
  }
  free(data);
  save(pouch, "/work/oversized", "oversized", error);
  oversized_events(pouch, "/work/oversized", "oversized", error);
  last = 999999U;
  assert(load_events(pouch, "/work/oversized", "oversized", last,
                     recovered_event, &last, error) == CAI_OK);
  assert(last == 1000003U && error->code == CAI_OK);
  /* Invalid identities are not size-limit recovery cases. */
  memset(&original, 0, sizeof(original));
  strcpy(original.record_type, "not-an-event");
  original.sequence = 1000004U;
  original.type = "assistant_text_delta";
  original.data = "invalid identity";
  assert(cai_cli_mapped_source(&event_map, &original, &source, error) ==
         CAI_OK);
  {
    lc_collection *collection = NULL;
    assert(history_collection(pouch, "oversized", "events", 0, &collection,
                              error) == CAI_OK);
    rc = collection_append_source(pouch, collection,
                                  "event-00000000000001000004", source, NULL,
                                  NULL, NULL, error);
    collection->close(collection);
    assert(rc == CAI_OK);
  }
  cai_source_close(source);
  assert(load_events(pouch, "/work/oversized", "oversized", last,
                     recovered_event, &last, error) == CAI_ERR_INVALID);
  assert(strstr(error->message, "invalid stored event identity") != NULL);
  cai_error_cleanup(error);
  cai_error_init(error);
}

static int collection_range_event(void *context,
                                  const cai_agent_session_event *event,
                                  cai_error *error) {
  unsigned long long *last = context;
  (void)error;
  assert(event->sequence > *last);
  assert(event->sequence == 30U || event->sequence == 90U);
  assert(strcmp(event->data, "collection event") == 0);
  *last = event->sequence;
  return CAI_OK;
}
static size_t broken_commit(void *context, void *bytes, size_t capacity,
                            cai_error *error) {
  (void)context;
  (void)bytes;
  (void)capacity;
  cai_cli_error(error, CAI_ERR_TRANSPORT, "commit source failed");
  return 0U;
}
static int unavailable_collections(lc_client *client,
                                   lc_collection_capabilities *caps,
                                   lc_error *error) {
  (void)client;
  (void)error;
  memset(caps, 0, sizeof(*caps));
  return LC_OK;
}
static void collection_regressions(cai_cli_pouch *pouch, cai_error *error) {
  cai_agent_session_event event;
  cai_cli_session before, after;
  cai_source_callbacks callbacks;
  cai_source *source = NULL;
  lc_attachment_list_req request;
  lc_attachment_list attachments;
  lc_collection *collection = NULL;
  lc_collection_info info;
  lc_error local;
  lc_lease *lease = NULL;
  unsigned long long last, sequence;
  unsigned long long numbers[3];
  size_t i, used, count;
  char bytes[1024] = {0};
  int (*capabilities)(lc_client *, lc_collection_capabilities *, lc_error *);
  save(pouch, "/work/collections", "collection-test", error);
  assert(cai_cli_pouch_session(pouch, "collection-test", &before, error) ==
         CAI_OK);
  numbers[0] = 10U;
  numbers[1] = 30U;
  numbers[2] = 90U;
  event.type = "assistant_text_delta";
  event.data = "collection event";
  for (i = 0U; i < 3U; i++) {
    event.sequence = numbers[i];
    assert(append_event(pouch, "/work/collections", "collection-test", &event,
                        error) == CAI_OK);
    /* Exact retries commit only once, even when positions differ from event
     * sequence numbers. */
    assert(append_event(pouch, "/work/collections", "collection-test", &event,
                        error) == CAI_OK);
  }
  event.data = "conflicting retry";
  assert(append_event(pouch, "/work/collections", "collection-test", &event,
                      error) != CAI_OK);
  cai_error_cleanup(error);
  cai_error_init(error);
  last = 10U;
  assert(cai_cli_pouch_replay(pouch, "/work/collections", "collection-test",
                              10U, 30U, NULL, collection_range_event, &last,
                              error) == CAI_OK);
  assert(last == 30U);
  assert(load_events(pouch, "/work/collections", "collection-test", last,
                     collection_range_event, &last, error) == CAI_OK);
  assert(last == 90U);
  assert(load_events(pouch, "/wrong-directory", "collection-test", 0U, reject,
                     NULL, error) == CAI_ERR_INVALID);
  cai_error_cleanup(error);
  cai_error_init(error);
  lc_error_init(&local);
  assert(history_collection(pouch, "collection-test", "events", 0, &collection,
                            error) == CAI_OK);
  assert(collection->describe(collection, &info, &local) == LC_OK);
  assert(info.live_count == 4U);
  collection->close(collection);
  lc_attachment_list_req_init(&request);
  memset(&attachments, 0, sizeof(attachments));
  request.lease.ns = pouch->sessions_namespace;
  request.lease.key = "collection-test";
  request.public_read = 1;
  assert(lc_list_attachments(pouch->sessions, &request, &attachments, &local) ==
         LC_OK);
  assert(attachments.count == 0U);
  lc_attachment_list_cleanup(&attachments);
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.read = broken_commit;
  assert(cai_source_from_callbacks(&callbacks, &source, error) == CAI_OK);
  assert(commit(pouch, "/work/collections", "collection-test", source, 90U,
                error) != CAI_OK);
  cai_source_close(source);
  source = NULL;
  cai_error_cleanup(error);
  cai_error_init(error);
  assert(cai_cli_pouch_session(pouch, "collection-test", &after, error) ==
         CAI_OK);
  assert(before.commit_position == after.commit_position);
  assert(before.applied_sequence == after.applied_sequence);
  assert(load_id(pouch, "/work/collections", "collection-test", &source,
                 &sequence, error) == CAI_OK);
  used = 0U;
  while ((count = cai_source_read(source, bytes + used, sizeof(bytes) - used,
                                  error)) != 0U)
    used += count;
  assert(used > 3U && used < sizeof(bytes) && error->code == CAI_OK);
  bytes[used] = '\0';
  assert(strstr(bytes, "\"record_type\":\"commit\"") != NULL);
  assert(strstr(bytes, "\"state\":{}") != NULL);
  cai_source_close(source);
  /* A retired metadata record is rejected, never silently treated as a
   * usable commit or converted during resume. */
  assert(acquire(pouch, "retired-session", &lease, error) == CAI_OK);
  memset(&after, 0, sizeof(after));
  strcpy(after.id, "retired-session");
  strcpy(after.workspace, "/work/retired");
  assert(update_session(lease, &after, error) == CAI_OK);
  assert(release(lease, CAI_OK, error) == CAI_OK);
  source = NULL;
  assert(load_id(pouch, "/work/retired", "retired-session", &source, &sequence,
                 error) == CAI_ERR_INVALID);
  assert(strstr(error->message, "retired attachment-backed") != NULL);
  assert(source == NULL);
  cai_error_cleanup(error);
  cai_error_init(error);
  /* Remote history rejects locally, before attempting leases/network I/O.
   * Other store operations retain their own capabilities. */
  capabilities = pouch->sessions->collection_capabilities;
  pouch->sessions->collection_capabilities = unavailable_collections;
  assert(append_event(pouch, "/work/collections", "collection-test", &event,
                      error) == CAI_ERR_INVALID);
  assert(strstr(error->message, "requires a Pouch endpoint") != NULL);
  cai_error_cleanup(error);
  cai_error_init(error);
  source = NULL;
  assert(load_id(pouch, "/work/collections", "collection-test", &source,
                 &sequence, error) == CAI_ERR_INVALID);
  assert(source == NULL);
  cai_error_cleanup(error);
  cai_error_init(error);
  pouch->sessions->collection_capabilities = capabilities;
  /* A missing collection is corruption, not an empty recovered journal. */
  assert(history_collection(pouch, "collection-test", "events", 0, &collection,
                            error) == CAI_OK);
  assert(collection->drop(collection, &local) == LC_OK);
  collection->close(collection);
  assert(load_events(pouch, "/work/collections", "collection-test", 0U, reject,
                     NULL, error) != CAI_OK);
  cai_error_cleanup(error);
  cai_error_init(error);
  lc_error_cleanup(&local);
}

#include "pouch_journal_fault_test.inc"

int main(int argc, char **argv) {
  char directory[PATH_MAX];
  cai_cli_pouch pouch;
  cai_error error;
  lc_lease *lease = NULL;
  latest_index index;
  cai_cli_session session;
  cai_agent_session_event event;
  cai_source *source = NULL;
  unsigned long long sequence;
  char id[CAI_AGENT_SESSION_ID_MAX];
  int i;
  if (argc == 4 && strcmp(argv[1], "--oversized-replay-event") == 0) {
    cai_error_init(&error);
    assert(cai_cli_pouch_open(&pouch, NULL, NULL, NULL, &error) == CAI_OK);
    oversized_events(&pouch, argv[3], argv[2], &error);
    cai_cli_pouch_close(&pouch);
    cai_error_cleanup(&error);
    return 0;
  }
  assert(argc == 2);
  assert(snprintf(directory, sizeof(directory), "%s/pouch-startup-XXXXXX",
                  argv[1]) > 0);
  assert(mkdtemp(directory) != NULL);
  assert(setenv("XDG_STATE_HOME", directory, 1) == 0);
  cai_error_init(&error);
  assert(cai_cli_pouch_open(&pouch, NULL, NULL, NULL, &error) == CAI_OK);
  newest(&pouch, "/work/empty", NULL, &error);
  save(&pouch, "/work/a", "first", &error);
  save(&pouch, "/work/b", "elsewhere", &error);
  save(&pouch, "/work/a", "second", &error);
  newest(&pouch, "/work/a", "second", &error);
  newest(&pouch, "/work/b", "elsewhere", &error);
  /* A persisted clean index avoids queries after reopening, even with many
   * conversations in the same directory. */
  for (i = 0; i < 20; i++) {
    snprintf(id, sizeof(id), "history-%02d", i);
    save(&pouch, "/work/a", id, &error);
  }
  save(&pouch, "/work/a", "second", &error);
  cai_cli_pouch_close(&pouch);
  assert(cai_cli_pouch_open(&pouch, NULL, NULL, NULL, &error) == CAI_OK);
  real_query = pouch.sessions->query_keys;
  pouch.sessions->query_keys = query_count;
  query_calls = 0;
  newest(&pouch, "/work/a", "second", &error);
  assert(query_calls == 0);
  /* Hidden imports cannot replace the newest published conversation. */
  assert(cai_cli_pouch_publish(&pouch, "second", 0, &error) == CAI_OK);
  newest(&pouch, "/work/a", "history-19", &error);
  assert(cai_cli_pouch_publish(&pouch, "second", 1, &error) == CAI_OK);
  newest(&pouch, "/work/a", "second", &error);
  /* Model a crash after dirty was persisted and session metadata changed,
   * before the final directory-index write. Reopen must reconstruct it. */
  assert(latest_open(&pouch, "/work/a", &lease, &index, &error) == CAI_OK);
  index.dirty = 1;
  assert(latest_write(&pouch, lease, &index, &error) == CAI_OK);
  {
    latest_index committed;
    lc_get_opts options;
    lc_get_res response;
    lc_error local;
    memset(&options, 0, sizeof(options));
    memset(&response, 0, sizeof(response));
    memset(&committed, 0, sizeof(committed));
    options.public_read = 1;
    lc_error_init(&local);
    assert(lc_load_in_namespace(pouch.sessions, "cai.scopes", lease->key + 5U,
                                &latest_map, &committed, &options, &response,
                                &local) == LC_OK);
    /* The crash marker must already be public while the writer guard is held.
     */
    assert(committed.dirty == 1);
    /* A held writer guard cannot block resume; it selects committed session
     * metadata and leaves the active writer's crash marker alone. */
    newest(&pouch, "/work/a", "second", &error);
    lc_get_res_cleanup(&response);
    memset(&response, 0, sizeof(response));
    assert(lc_load_in_namespace(pouch.sessions, "cai.scopes", lease->key + 5U,
                                &latest_map, &committed, &options, &response,
                                &local) == LC_OK);
    assert(committed.dirty == 1);
    lc_get_res_cleanup(&response);
    lc_error_cleanup(&local);
  }

  assert(release(lease, CAI_OK, &error) == CAI_OK);
  lease = NULL;
  assert(acquire(&pouch, "first", &lease, &error) == CAI_OK);
  assert(get_session(lease, "/work/a", "first", &session, &error) == CAI_OK);
  session.commit_ns = index.latest.commit_ns + 1U;
  assert(update_session(lease, &session, &error) == CAI_OK);
  assert(release(lease, CAI_OK, &error) == CAI_OK);
  lease = NULL;
  newest(&pouch, "/work/a", "first", &error);
  assert(query_calls > 0);
  query_calls = 0;
  newest(&pouch, "/work/a", "first", &error);
  assert(query_calls == 0);
  /* Tie-breaking matches the explicit session listing. */
  assert(acquire(&pouch, "second", &lease, &error) == CAI_OK);
  assert(get_session(lease, "/work/a", "second", &session, &error) == CAI_OK);
  session.commit_ns = index.latest.commit_ns + 1U;
  assert(update_session(lease, &session, &error) == CAI_OK);
  assert(release(lease, CAI_OK, &error) == CAI_OK);
  assert(cai_cli_pouch_publish(&pouch, "second", 1, &error) == CAI_OK);
  newest(&pouch, "/work/a", "second", &error);
  /* If wall time moves backwards, replacing the selected session's newest
   * commit can make a different session newest. Cache and listing agree. */
  lease = NULL;
  assert(latest_open(&pouch, "/work/a", &lease, &index, &error) == CAI_OK);
  index.latest.commit_ns += 1000000000000ULL;
  {
    lc_lease *session_lease = NULL;
    assert(acquire(&pouch, "second", &session_lease, &error) == CAI_OK);
    assert(get_session(session_lease, "/work/a", "second", &session, &error) ==
           CAI_OK);
    session.commit_ns = index.latest.commit_ns;
    index.latest = session;
    assert(update_session(session_lease, &session, &error) == CAI_OK);
    assert(release(session_lease, CAI_OK, &error) == CAI_OK);
    session_lease = NULL;
    assert(acquire(&pouch, "first", &session_lease, &error) == CAI_OK);
    assert(get_session(session_lease, "/work/a", "first", &session, &error) ==
           CAI_OK);
    session.commit_ns = index.latest.commit_ns - 1U;
    assert(update_session(session_lease, &session, &error) == CAI_OK);
    assert(release(session_lease, CAI_OK, &error) == CAI_OK);
  }
  assert(latest_write(&pouch, lease, &index, &error) == CAI_OK);
  assert(release(lease, CAI_OK, &error) == CAI_OK);
  save(&pouch, "/work/a", "second", &error);
  newest(&pouch, "/work/a", "first", &error);
  assert(pouch.store.load_latest(&pouch, "/work/a", id, 2U, &source, &sequence,
                                 &error) == CAI_ERR_INVALID);
  assert(source == NULL);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  {
    cai_cli_session *listed = NULL;
    size_t count = 0U;
    /* A failed import leaves no public body; its indexed workspace key must
     * not poison listing or directory-index repair. */
    lease = NULL;
    assert(acquire(&pouch, "unfinished-import", &lease, &error) == CAI_OK);
    assert(get_session(lease, "/work/a", "unfinished-import", &session,
                       &error) == CAI_OK);
    session.published = 0;
    assert(update_session(lease, &session, &error) == CAI_OK);
    assert(release(lease, CAI_OK, &error) == CAI_OK);
    lease = NULL;
    assert(acquire(&pouch, "unfinished-import", &lease, &error) == CAI_OK);
    {
      lc_error local;
      lc_error_init(&local);
      assert(lease->remove(lease, NULL, &local) == LC_OK);
      lc_error_cleanup(&local);
    }
    assert(release(lease, CAI_OK, &error) == CAI_OK);
    assert(cai_cli_pouch_list(&pouch, "/work/a", &listed, &count, &error) ==
           CAI_OK);
    assert(count == 22U && error.code == CAI_OK);
    free(listed);
  }
  /* Each event feeds recovery and UI once, in order, without buffering. */
  for (i = 1; i <= 3; i++) {
    event.sequence = (unsigned long long)i;
    event.type = "assistant_text_delta";
    event.data = "replayed text";
    assert(pouch.store.append_event(&pouch, "/work/a", "second", &event,
                                    &error) == CAI_OK);
  }
  pouch.replay_scope = "/work/a";
  pouch.replay_begin = begin_replay;
  pouch.replay_event = replay;
  assert(pouch.store.load_events_after(&pouch, "/work/a", "second", 0U, recover,
                                       NULL, &error) == CAI_OK);
  assert(observed == 3 && consumed == 3);
  pouch.replay_begin = NULL;
  pouch.replay_event = reject;
  observed = consumed = 0;
  assert(pouch.store.load_events_after(&pouch, "/work/a", "second", 0U, recover,
                                       NULL, &error) == CAI_ERR_TRANSPORT);
  assert(consumed == 1);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  pouch.replay_event = NULL;
  observed = consumed = 0;
  assert(pouch.store.load_events_after(&pouch, "/work/a", "second", 0U, reject,
                                       NULL, &error) == CAI_ERR_TRANSPORT);
  assert(observed == 0);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  collection_regressions(&pouch, &error);
  todo_transactions(&pouch, &error);
  replay_limits(&pouch, &error);
  journal_faults(&pouch, &error);
  cai_cli_pouch_close(&pouch);
  cai_error_cleanup(&error);
  interrupted_idle_writer();
  /* Test runner removes the isolated encrypted root. */
  printf("Fixture: %s\n", directory);
  return 0;
}
