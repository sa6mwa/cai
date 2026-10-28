#include "../cli/pouch.c"
#include <assert.h>

unsigned int cai_test_storage_open_count(void);

static int dump(void *context, const char *prompt, cai_error *error) {
  prompt_record record;
  cai_source *source = NULL;
  char bytes[257];
  size_t count;
  int rc;
  (void)context;
  record.prompt = prompt;
  rc = cai_cli_mapped_source(&prompt_map, &record, &source, error);
  while (rc == CAI_OK &&
         (count = cai_source_read(source, bytes, sizeof(bytes), error)) != 0U)
    if (fwrite(bytes, 1U, count, stdout) != count)
      rc = CAI_ERR_TRANSPORT;
  if (rc == CAI_OK && error->code != CAI_OK)
    rc = error->code;
  cai_source_close(source);
  if (rc == CAI_OK && putchar('\n') == EOF)
    rc = CAI_ERR_TRANSPORT;
  return rc;
}
typedef struct seen {
  int count, first;
} seen;
static int visit(void *context, const char *prompt, cai_error *error) {
  seen *state = context;
  char expected[64];
  (void)error;
  snprintf(expected, sizeof(expected), "entry-%d", state->first + state->count);
  assert(strcmp(prompt, expected) == 0);
  state->count++;
  return CAI_OK;
}
static int reject(void *context, const char *prompt, cai_error *error) {
  (void)context;
  (void)prompt;
  return cai_cli_error(error, CAI_ERR_CANCELLED, "cancel prompt import");
}
static int exact(void *context, const char *prompt, cai_error *error) {
  (void)error;
  assert(strcmp(context, prompt) == 0);
  return CAI_OK;
}
static int fail_read(lc_collection *collection,
                     const lc_collection_read_opts *options, lc_sink *sink,
                     lc_collection_read_res *result, lc_error *error) {
  (void)collection;
  (void)options;
  (void)sink;
  (void)result;
  error->code = LC_ERR_TRANSPORT;
  return LC_ERR_TRANSPORT;
}
static int fail_append(lc_collection *collection,
                       const lc_collection_append_req *request,
                       lc_source *source, lc_collection_append_res *result,
                       lc_error *error) {
  (void)collection;
  (void)request;
  (void)source;
  (void)result;
  error->code = LC_ERR_INVALID;
  return LC_ERR_INVALID;
}
static int fail_describe(lc_collection *collection, lc_collection_info *info,
                         lc_error *error) {
  (void)collection;
  (void)info;
  error->code = LC_ERR_INVALID;
  return LC_ERR_INVALID;
}
static int metadata_calls;
static int (*real_metadata)(lc_collection *, const lc_collection_metadata_req *,
                            lc_collection_info *, lc_error *);
static int count_metadata(lc_collection *collection,
                          const lc_collection_metadata_req *request,
                          lc_collection_info *info, lc_error *error) {
  metadata_calls++;
  assert(request->has_max_entries && !request->has_retention_seconds);
  return real_metadata(collection, request, info, error);
}
static int reject_metadata(lc_collection *collection,
                           const lc_collection_metadata_req *request,
                           lc_collection_info *info, lc_error *error) {
  (void)collection;
  (void)request;
  (void)info;
  metadata_calls++;
  error->code = LC_ERR_INVALID;
  return LC_ERR_INVALID;
}
static size_t empty_state(void *context, void *bytes, size_t capacity,
                          cai_error *error) {
  size_t *offset = context, count = 2U - *offset;
  (void)error;
  if (count > capacity)
    count = capacity;
  memcpy(bytes, "{}" + *offset, count);
  *offset += count;
  return count;
}
static void save_session(cai_cli_pouch *pouch, const char *id,
                         cai_error *error) {
  cai_source_callbacks callbacks;
  cai_source *source = NULL;
  size_t offset = 0U;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.read = empty_state;
  callbacks.context = &offset;
  assert(cai_source_from_callbacks(&callbacks, &source, error) == CAI_OK);
  assert(pouch->store.commit(pouch, "/workspace", id, source, 1U, error) ==
         CAI_OK);
  cai_source_close(source);
}
static int no_entry_expiry(void *context,
                           const lc_collection_read_metadata *metadata,
                           lc_error *error) {
  (void)context;
  (void)error;
  if (metadata->kind == LC_COLLECTION_READ_METADATA_ENTRY) {
    assert(metadata->has_retention_expires_at_unix);
    assert(metadata->retention_expires_at_unix == 0);
  }
  return LC_OK;
}
static int discard_entry(lc_sink *sink, const void *bytes, size_t count,
                         lc_error *error) {
  (void)sink;
  (void)bytes;
  (void)count;
  (void)error;
  return 1;
}
static void session_deadline(cai_cli_pouch *pouch, const char *id, long seconds,
                             time_t before) {
  lc_describe_req request;
  lc_describe_res result;
  lc_error error;
  lc_describe_req_init(&request);
  memset(&result, 0, sizeof(result));
  lc_error_init(&error);
  request.ns = pouch->sessions_namespace;
  request.key = id;
  assert(lc_describe(pouch->sessions, &request, &result, &error) == LC_OK);
  assert(result.has_retention_expires_at_unix);
  if (seconds == 0L)
    assert(result.retention_expires_at_unix == 0);
  else
    assert(result.retention_expires_at_unix >= before + seconds &&
           result.retention_expires_at_unix <= time(NULL) + seconds);
  lc_describe_res_cleanup(&result);
  lc_error_cleanup(&error);
}
static void retention_test(cai_cli_pouch *pouch, cai_error *error) {
  lc_collection *collection = NULL;
  lc_lease *removed = NULL;
  lc_collection_info info;
  lc_collection_read_opts options;
  lc_collection_read_res result;
  lc_sink sink;
  lc_error local;
  cai_agent_session_event event;
  cai_cli_session *sessions = NULL;
  cai_source *source = NULL;
  char id[CAI_AGENT_SESSION_ID_MAX];
  unsigned long long sequence = 0U;
  size_t count;
  long days;
  cli_record imported;
  cai_cli_session metadata;
  time_t before = time(NULL);
  lc_error_init(&local);
  save_session(pouch, "retained", error);
  session_deadline(pouch, "retained", 30L * 86400L, before);
  assert(open_history_collection(pouch, "retained", "events", 0, 0, &collection,
                                 error) == CAI_OK);
  assert(collection->describe(collection, &info, &local) == LC_OK);
  assert(info.has_retention_expires_at_unix);
  assert(info.retention_expires_at_unix >= before + 30L * 86400L);
  assert(info.retention_expires_at_unix <= time(NULL) + 30L * 86400L);
  /* Move the deadline closer, then prove an append renews the whole owner. */
  assert(collection_retain(collection, 1L, &local) == LC_OK);
  memset(&event, 0, sizeof(event));
  event.sequence = 2U;
  event.type = "text";
  event.data = "retention regression";
  assert(pouch->store.append_event(pouch, "/workspace", "retained", &event,
                                   error) == CAI_OK);
  assert(collection->describe(collection, &info, &local) == LC_OK);
  assert(info.retention_expires_at_unix >= before + 30L * 86400L);
  /* Identical retries preserve both retention deadlines. */
  assert(collection_retain(collection, 1L, &local) == LC_OK);
  assert(collection->describe(collection, &info, &local) == LC_OK);
  {
    lc_unix_seconds deadline = info.retention_expires_at_unix;
    assert(acquire(pouch, "retained", &removed, error) == CAI_OK);
    assert(session_retain(removed, 1L, &local) == LC_OK);
    assert(release(removed, CAI_OK, error) == CAI_OK);
    removed = NULL;
    assert(pouch->store.append_event(pouch, "/workspace", "retained", &event,
                                     error) == CAI_OK);
    assert(collection->describe(collection, &info, &local) == LC_OK);
    assert(info.retention_expires_at_unix == deadline);
    session_deadline(pouch, "retained", 1L, before);
  }
  lc_collection_read_opts_init(&options);
  lc_collection_read_res_init(&result);
  memset(&sink, 0, sizeof(sink));
  sink.write = discard_entry;
  options.on_metadata = no_entry_expiry;
  assert(collection->read(collection, &options, &sink, &result, &local) ==
         LC_OK);
  lc_collection_read_res_cleanup(&result);
  assert(cai_cli_pouch_set_retention(pouch, "retained", 0L, error) == CAI_OK);
  event.sequence++;
  assert(pouch->store.append_event(pouch, "/workspace", "retained", &event,
                                   error) == CAI_OK);
  assert(cai_cli_pouch_retention(pouch, "retained", &days, error) == CAI_OK &&
         days == 0L);
  assert(collection->describe(collection, &info, &local) == LC_OK &&
         info.retention_expires_at_unix == 0);
  session_deadline(pouch, "retained", 0L, before);
  assert(cai_cli_pouch_set_retention(pouch, "retained", 7L, error) == CAI_OK);
  event.sequence++;
  assert(pouch->store.append_event(pouch, "/workspace", "retained", &event,
                                   error) == CAI_OK);
  assert(collection->describe(collection, &info, &local) == LC_OK);
  assert(info.retention_expires_at_unix >= before + 7L * 86400L &&
         info.retention_expires_at_unix <= time(NULL) + 7L * 86400L);
  /* Imports stream with no metadata lease; renew that at final publication. */
  memset(&imported, 0, sizeof(imported));
  strcpy(imported.record_type, "event");
  imported.sequence = 5U;
  imported.type = "text";
  imported.data = "unleased import append";
  assert(cai_cli_pouch_session(pouch, "retained", &metadata, error) == CAI_OK);
  assert(cai_cli_mapped_source(&event_map, &imported, &source, error) ==
         CAI_OK);
  real_metadata = collection->metadata;
  metadata_calls = 0;
  collection->metadata = reject_metadata;
  assert(journal_append_source(pouch, NULL, &metadata, collection,
                               "event-00000000000000000005", source, NULL,
                               error) == CAI_OK);
  assert(metadata_calls ==
         0); /* Renewal is part of append, not a second write. */
  collection->metadata = real_metadata;
  assert(collection->describe(collection, &info, &local) == LC_OK);
  assert(info.retention_expires_at_unix >= before + 7L * 86400L);
  cai_source_close(source);
  source = NULL;
  lc_collection_close(collection);
  /* Corrupt identities must fail validation, not masquerade as expired data. */
  assert(acquire(pouch, "retained", &removed, error) == CAI_OK);
  assert(get_session(removed, "/workspace", "retained", &metadata, error) ==
         CAI_OK);
  strcpy(metadata.id, "no-such-journal");
  assert(update_session(removed, &metadata, error) == CAI_OK);
  assert(release(removed, CAI_OK, error) == CAI_OK);
  assert(cai_cli_pouch_list(pouch, "/workspace", &sessions, &count, error) ==
         CAI_ERR_INVALID);
  assert(strstr(error->message, "identity") != NULL);
  free(sessions);
  sessions = NULL;
  cai_error_cleanup(error);
  cai_error_init(error);
  assert(acquire(pouch, "retained", &removed, error) == CAI_OK);
  strcpy(metadata.id, "retained");
  assert(update_session(removed, &metadata, error) == CAI_OK);
  assert(release(removed, CAI_OK, error) == CAI_OK);
  /* SDK cleanup is asynchronous; simulate its completed drop without a wait. */
  save_session(pouch, "expired", error);
  assert(open_history_collection(pouch, "expired", "events", 0, 0, &collection,
                                 error) == CAI_OK);
  assert(collection->drop(collection, &local) == LC_OK);
  lc_collection_close(collection);
  assert(cai_cli_pouch_list(pouch, "/workspace", &sessions, &count, error) ==
         CAI_OK);
  assert(count == 1U && strcmp(sessions[0].id, "retained") == 0);
  free(sessions);
  assert(pouch->store.load_latest(pouch, "/workspace", id, sizeof(id), &source,
                                  &sequence, error) == CAI_OK);
  assert(source != NULL && strcmp(id, "retained") == 0);
  cai_source_close(source);
  /* Maintenance may remove metadata before its collection, too. */
  save_session(pouch, "metadata-removed", error);
  assert(acquire(pouch, "metadata-removed", &removed, error) == CAI_OK);
  assert(removed->remove(removed, NULL, &local) == LC_OK);
  assert(release(removed, CAI_OK, error) == CAI_OK);
  source = NULL;
  assert(pouch->store.load_latest(pouch, "/workspace", id, sizeof(id), &source,
                                  &sequence, error) == CAI_OK);
  assert(source != NULL && strcmp(id, "retained") == 0);
  cai_source_close(source);
  assert(pouch->store.load_id(pouch, "/workspace", "expired", &source,
                              &sequence, error) != CAI_OK);
  cai_error_cleanup(error);
  cai_error_init(error);
  assert(open_history_collection(pouch, "retained", "events", 0, 0, &collection,
                                 error) == CAI_OK);
  assert(collection->drop(collection, &local) == LC_OK);
  lc_collection_close(collection);
  source = NULL;
  assert(pouch->store.load_latest(pouch, "/workspace", id, sizeof(id), &source,
                                  &sequence, error) == CAI_OK);
  assert(source == NULL);
  lc_error_cleanup(&local);
}
static void unit(const char *directory, cai_error *error) {
  cai_cli_pouch pouch, shared;
  lc_collection *receiver;
  lc_error local;
  lc_collection_append_req request;
  lc_collection_append_res result;
  lc_collection_info info;
  cai_source *source = NULL;
  cai_cli_session wrong;
  lc_source *input = NULL;
  seen state;
  char root[PATH_MAX], prompt[64], *name;
  int i, parent;
  int (*append)(lc_collection *, const lc_collection_append_req *, lc_source *,
                lc_collection_append_res *, lc_error *);
  int (*describe)(lc_collection *, lc_collection_info *, lc_error *);
  int (*read)(lc_collection *, const lc_collection_read_opts *, lc_sink *,
              lc_collection_read_res *, lc_error *);
  char large[20001];
  assert(snprintf(root, sizeof(root), "%s/prompts.XXXXXX", directory) <
         (int)sizeof(root));
  assert(mkdtemp(root) != NULL);
  assert(cai_cli_pouch_open_at(&pouch, root, NULL, NULL, NULL, error) ==
         CAI_OK);
  assert(cai_test_storage_open_count() == 1U);
  lc_error_init(&local);
  retention_test(&pouch, error);
  assert(cai_cli_pouch_prompt_configure(&pouch, "disabled", 0U, error) ==
         CAI_OK);
  assert(pouch.prompt_history == NULL);
  assert(open_history_collection(&pouch, "disabled", "prompts", 0, 1, &receiver,
                                 error) == CAI_OK);
  assert(receiver == NULL); /* Disabling recall does not create storage. */
  assert(cai_cli_pouch_prompt_open(&pouch, "one", error) == CAI_OK);
  receiver = pouch.prompt_history;
  real_metadata = receiver->metadata;
  receiver->metadata = count_metadata;
  metadata_calls = 0;
  assert(cai_cli_pouch_prompt_set_limit(&pouch, 12U, error) == CAI_OK);
  assert(metadata_calls == 1);
  assert(cai_cli_pouch_prompt_set_limit(&pouch, 12U, error) == CAI_OK);
  assert(metadata_calls == 1); /* No redundant metadata writes. */
  receiver->metadata = reject_metadata;
  assert(cai_cli_pouch_prompt_set_limit(&pouch, 13U, error) != CAI_OK);
  cai_error_cleanup(error);
  cai_error_init(error);
  assert(cai_cli_pouch_prompt_configure(&pouch, "one", 0U, error) != CAI_OK);
  assert(pouch.prompt_history ==
         receiver); /* Failed disable keeps recall usable. */
  cai_error_cleanup(error);
  cai_error_init(error);
  assert(cai_cli_pouch_prompt_configure(&pouch, "../unsafe", 0U, error) ==
         CAI_ERR_INVALID);
  assert(pouch.prompt_history == receiver);
  cai_error_cleanup(error);
  cai_error_init(error);
  receiver->metadata = real_metadata;
  assert(receiver->describe(receiver, &info, &local) == LC_OK);
  assert(info.max_entries == 12U);
  memset(&state, 0, sizeof(state));
  assert(cai_cli_pouch_prompt_load(&pouch, 12U, visit, &state, error) ==
         CAI_OK);
  assert(state.count == 0);
  for (i = 0; i < 17; i++) {
    snprintf(prompt, sizeof(prompt), "entry-%d", i);
    assert(cai_cli_pouch_prompt_append(&pouch, prompt, error) == CAI_OK);
    assert(pouch.prompt_history == receiver);
  }
  assert(receiver->describe(receiver, &info, &local) == LC_OK);
  assert(info.max_entries == 12U && info.live_count == 17U);
  state.first = 5;
  assert(cai_cli_pouch_prompt_load(&pouch, 12U, visit, &state, error) ==
         CAI_OK);
  assert(state.count == 12 && cai_test_storage_open_count() == 1U);
  state.count = 0;
  state.first = 14;
  assert(cai_cli_pouch_prompt_load(&pouch, 3U, visit, &state, error) == CAI_OK);
  assert(state.count == 3);
  state.count = 0;
  assert(cai_cli_pouch_prompt_load(&pouch, 0U, visit, &state, error) == CAI_OK);
  assert(state.count == 0);
  state.first = 5;
  assert(cai_cli_pouch_open_at(&shared, root, NULL, NULL, NULL, error) ==
         CAI_OK);
  assert(shared.sessions == pouch.sessions &&
         cai_test_storage_open_count() == 1U);
  assert(cai_cli_pouch_prompt_open(&shared, "one", error) == CAI_OK);
  state.count = 0;
  assert(cai_cli_pouch_prompt_load(&shared, 12U, visit, &state, error) ==
         CAI_OK);
  assert(state.count == 12);
  cai_cli_pouch_close(&shared);
  append = receiver->append;
  receiver->append = fail_append;
  assert(cai_cli_pouch_prompt_append(&pouch, "unsaved", error) != CAI_OK);
  receiver->append = append;
  cai_error_cleanup(error);
  cai_error_init(error);
  assert(cai_cli_pouch_prompt_load(&pouch, 1000U, reject, NULL, error) ==
         CAI_ERR_CANCELLED);
  cai_error_cleanup(error);
  cai_error_init(error);
  describe = receiver->describe;
  receiver->describe = fail_describe;
  assert(cai_cli_pouch_prompt_load(&pouch, 1000U, visit, &state, error) !=
         CAI_OK);
  receiver->describe = describe;
  cai_error_cleanup(error);
  cai_error_init(error);
  read = receiver->read;
  receiver->read = fail_read;
  assert(cai_cli_pouch_prompt_load(&pouch, 1000U, visit, &state, error) !=
         CAI_OK);
  receiver->read = read;
  cai_error_cleanup(error);
  cai_error_init(error);
  assert(cai_cli_pouch_prompt_open(&pouch, "../unsafe", error) ==
         CAI_ERR_INVALID);
  assert(pouch.prompt_history == receiver);
  cai_error_cleanup(error);
  cai_error_init(error);
  assert(cai_cli_pouch_prompt_open(&pouch, "large", error) == CAI_OK);
  for (i = 0; i < 20000; i += 5)
    memcpy(large + i, "\316\273\n\"\\", 5U);
  large[20000] = '\0';
  assert(cai_cli_pouch_prompt_append(&pouch, large, error) == CAI_OK);
  assert(cai_cli_pouch_prompt_load(&pouch, 1000U, exact, large, error) ==
         CAI_OK);
  assert(cai_cli_pouch_prompt_open(&pouch, "other", error) == CAI_OK);
  state.count = 0;
  assert(cai_cli_pouch_prompt_load(&pouch, 1000U, visit, &state, error) ==
         CAI_OK);
  assert(state.count == 0);
  /* Corrupt data is reported; it is never interpreted as model input. */
  lc_error_cleanup(&local);
  lc_error_init(&local);
  lc_collection_append_req_init(&request);
  request.append_id = "corrupt";
  memset(&wrong, 0, sizeof(wrong));
  assert(cai_cli_mapped_source(&session_map, &wrong, &source, error) == CAI_OK);
  assert(cai_cli_lc_source(source, &input, error) == CAI_OK);
  assert(pouch.prompt_history->append(pouch.prompt_history, &request, input,
                                      &result, &local) == LC_OK);
  lc_source_close(input);
  cai_source_close(source);
  assert(cai_cli_pouch_prompt_load(&pouch, 1000U, visit, &state, error) !=
         CAI_OK);
  cai_error_cleanup(error);
  cai_error_init(error);
  cai_cli_pouch_close(&pouch);
  assert(cai_cli_pouch_open_at(&pouch, root, NULL, NULL, NULL, error) ==
         CAI_OK);
  assert(cai_cli_pouch_prompt_open(&pouch, "one", error) == CAI_OK);
  state.count = 0;
  assert(cai_cli_pouch_prompt_load(&pouch, 12U, visit, &state, error) ==
         CAI_OK);
  assert(state.count == 12);
  assert(pouch.prompt_history->describe(pouch.prompt_history, &info, &local) ==
         LC_OK);
  assert(info.max_entries == 12U); /* SDK policy survives close/reopen. */
  assert(cai_cli_pouch_prompt_configure(&pouch, "one", 0U, error) == CAI_OK);
  assert(pouch.prompt_history == NULL);
  assert(cai_cli_pouch_prompt_open(&pouch, "one", error) == CAI_OK);
  assert(pouch.prompt_history->describe(pouch.prompt_history, &info, &local) ==
         LC_OK);
  assert(info.max_entries == 0U && info.live_count == 17U);
  lc_error_cleanup(&local);
  cai_cli_pouch_close(&pouch);
  parent = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  assert(parent >= 0);
  name = strrchr(root, '/');
  assert(cai_cli_remove_at(parent, name + 1) == 0);
  close(parent);
}
int main(int argc, char **argv) {
  cai_cli_pouch pouch;
  cai_error error;
  int rc;
  cai_error_init(&error);
  if (argc == 2) {
    unit(argv[1], &error);
    cai_error_cleanup(&error);
    return 0;
  }
  assert(argc == 4);
  rc = cai_cli_pouch_open_at(&pouch, argv[2], NULL, NULL, NULL, &error);
  if (rc == CAI_OK && strcmp(argv[1], "policy") == 0) {
    long days;
    rc = cai_cli_pouch_retention(&pouch, argv[3], &days, &error);
    if (rc == CAI_OK)
      printf("%ld\n", days);
  } else if (rc == CAI_OK && strcmp(argv[1], "drop") == 0) {
    lc_collection *collection = NULL;
    lc_error local;
    lc_error_init(&local);
    rc = open_history_collection(&pouch, argv[3], "events", 0, 0, &collection,
                                 &error);
    if (rc == CAI_OK && collection->drop(collection, &local) != LC_OK)
      rc = cai_cli_lc_error(&error, &local);
    lc_collection_close(collection);
    lc_error_cleanup(&local);
  } else if (rc == CAI_OK && strcmp(argv[1], "limit") == 0) {
    lc_collection_info info;
    lc_error local;
    lc_error_init(&local);
    rc = cai_cli_pouch_prompt_open(&pouch, argv[3], &error);
    if (rc == CAI_OK && pouch.prompt_history->describe(pouch.prompt_history,
                                                       &info, &local) != LC_OK)
      rc = cai_cli_lc_error(&error, &local);
    if (rc == CAI_OK)
      printf("%llu\n", (unsigned long long)info.max_entries);
    lc_error_cleanup(&local);
  } else if (rc == CAI_OK) {
    assert(strcmp(argv[1], "dump") == 0);
    rc = cai_cli_pouch_prompt_open(&pouch, argv[3], &error);
    if (rc == CAI_OK)
      rc = cai_cli_pouch_prompt_load(&pouch, 1000U, dump, NULL, &error);
  }
  if (rc != CAI_OK)
    fprintf(stderr, "prompt history: %s\n", error.message);
  cai_cli_pouch_close(&pouch);
  cai_error_cleanup(&error);
  return rc != CAI_OK;
}
