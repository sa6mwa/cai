/* Faults at the public SDK boundary; actual encrypted collections underneath.
 */
static int journal_append_mode, journal_append_calls, identity_updates;
static int (*journal_real_append)(lc_collection *,
                                  const lc_collection_append_req *, lc_source *,
                                  lc_collection_append_res *, lc_error *);
static int (*journal_real_status)(lc_collection *, const char *,
                                  lc_collection_append_status_res *,
                                  lc_error *);
static int (*journal_real_acquire)(lc_client *, const lc_acquire_req *,
                                   lc_lease **, lc_error *);
static int (*journal_real_update)(lc_lease *, lc_source *,
                                  const lc_update_opts *, lc_error *);
static int journal_lost_ack(lc_collection *collection,
                            const lc_collection_append_req *request,
                            lc_source *source, lc_collection_append_res *result,
                            lc_error *error) {
  int rc = LC_OK;
  journal_append_calls++;
  if (journal_append_mode == 1)
    rc = journal_real_append(collection, request, source, result, error);
  if (rc == LC_OK) {
    error->code = LC_ERR_TRANSPORT;
    error->message = strdup("injected lost append acknowledgement");
    rc = LC_ERR_TRANSPORT;
  }
  return rc;
}
static int journal_status(lc_collection *collection, const char *id,
                          lc_collection_append_status_res *result,
                          lc_error *error) {
  if (journal_append_mode != 3)
    return journal_real_status(collection, id, result, error);
  error->code = LC_ERR_TRANSPORT;
  error->message = strdup("injected unavailable append status");
  return LC_ERR_TRANSPORT;
}
static int journal_identity_update(lc_lease *lease, lc_source *source,
                                   const lc_update_opts *options,
                                   lc_error *error) {
  if (++identity_updates == 2) {
    error->code = LC_ERR_TRANSPORT;
    error->message = strdup("injected post-append metadata failure");
    return LC_ERR_TRANSPORT;
  }
  return journal_real_update(lease, source, options, error);
}
static int journal_acquire(lc_client *client, const lc_acquire_req *request,
                           lc_lease **out, lc_error *error) {
  int rc = journal_real_acquire(client, request, out, error);
  if (rc == LC_OK && strcmp(request->key, "fault-identity") == 0) {
    journal_real_update = (*out)->update;
    (*out)->update = journal_identity_update;
  }
  return rc;
}
static size_t journal_identity_read(void *context, void *bytes, size_t count,
                                    cai_error *error) {
  static const char edit[] =
      "{\"revision\":1,\"base_bytes\":0,\"end_bytes\":0,"
      "\"blocks_keep\":0,\"hidden_keep\":0,\"owned_keep\":0,"
      "\"metadata\":{\"version\":1,\"previous_response_id\":\"resp_fault\"},"
      "\"history_append\":\"\"}";
  size_t *offset = context;
  (void)error;
  if (count > sizeof(edit) - 1U - *offset)
    count = sizeof(edit) - 1U - *offset;
  memcpy(bytes, edit + *offset, count);
  *offset += count;
  return count;
}
static void journal_faults(cai_cli_pouch *pouch, cai_error *error) {
  lc_collection *collection = NULL;
  lc_collection_info info;
  lc_error local;
  cai_source_callbacks callbacks;
  cai_source *source = NULL;
  cai_cli_pouch alias;
  cai_cli_session session;
  size_t offset;
  unsigned long long position = 0U;
  int mode, calls;
  lc_error_init(&local);
  journal_real_acquire = pouch->sessions->acquire;
  pouch->sessions->acquire = journal_acquire;
  identity_updates = 0;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.read = journal_identity_read;
  callbacks.context = &offset;
  offset = 0U;
  assert(cai_source_from_callbacks(&callbacks, &source, error) == CAI_OK);
  assert(pouch->store.commit(pouch, "/work/fault-identity", "fault-identity",
                             source, 1U, error) == CAI_OK);
  cai_source_close(source);
  source = NULL;
  pouch->sessions->acquire = journal_real_acquire;
  assert(identity_updates == 2);
  assert(cai_cli_pouch_session(pouch, "fault-identity", &session, error) ==
         CAI_OK);
  assert(session.commit_position == 0U); /* Derived publication failed. */
  newest(pouch, "/work/fault-identity", "fault-identity", error);
  assert(validate_commit(pouch, "fault-identity", "/work/fault-identity",
                         error) == CAI_OK);
  assert(history_collection(pouch, "append-faults", "events", 1, &collection,
                            error) == CAI_OK);
  journal_real_append = collection->append;
  journal_real_status = collection->append_status;
  collection->append = journal_lost_ack;
  collection->append_status = journal_status;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.read = read_state;
  callbacks.context = &offset;
  for (mode = 1; mode <= 3; mode++) {
    journal_append_mode = mode;
    offset = 0U;
    assert(cai_source_from_callbacks(&callbacks, &source, error) == CAI_OK);
    if (mode == 1) {
      assert(collection_append_source(pouch, collection, "lost-ack", source,
                                      NULL, &position, NULL, error) == CAI_OK);
      assert(position == 1U && error->code == CAI_OK);
    } else {
      assert(collection_append_source(pouch, collection, "uncommitted", source,
                                      NULL, NULL, NULL,
                                      error) == CAI_ERR_TRANSPORT);
      cai_error_cleanup(error);
    }
    cai_source_close(source);
    source = NULL;
    if (mode == 2) {
      /* A definite local failure with an absent append remains retryable. */
      collection->append = journal_real_append;
      offset = 0U;
      assert(cai_source_from_callbacks(&callbacks, &source, error) == CAI_OK);
      assert(collection_append_source(pouch, collection, "uncommitted", source,
                                      NULL, &position, NULL, error) == CAI_OK);
      cai_source_close(source);
      collection->append = journal_lost_ack;
      assert(position == 2U);
    }
  }
  assert(collection->describe(collection, &info, &local) == LC_OK);
  assert(info.live_count == 2U);
  assert(cai_cli_pouch_open(&alias, NULL, NULL, NULL, error) == CAI_OK);
  assert(alias.sessions == pouch->sessions);
  offset = 0U;
  assert(cai_source_from_callbacks(&callbacks, &source, error) == CAI_OK);
  calls = journal_append_calls;
  assert(collection_append_source(&alias, collection, "blocked", source, NULL,
                                  NULL, NULL, error) == CAI_ERR_TRANSPORT);
  assert(journal_append_calls == calls && offset == 0U);
  assert(strstr(error->message, "unresolved outcome") != NULL);
  cai_source_close(source);
  cai_error_cleanup(error);
  cai_cli_pouch_close(&alias);
  collection->close(collection);
  lc_error_cleanup(&local);
  cai_cli_pouch_close(pouch);
  assert(cai_cli_pouch_open(pouch, NULL, NULL, NULL, error) == CAI_OK);
  newest(pouch, "/work/fault-identity", "fault-identity", error);
  assert(validate_commit(pouch, "fault-identity", "/work/fault-identity",
                         error) == CAI_OK);
  save(pouch, "/work/after-fault", "after-fault", error);
}
