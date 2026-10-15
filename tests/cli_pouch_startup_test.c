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
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.read = read_state;
  callbacks.context = &offset;
  assert(cai_source_from_callbacks(&callbacks, &source, error) == CAI_OK);
  assert(pouch->store.checkpoint(pouch, scope, id, source, 1U, error) ==
         CAI_OK);
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
  assert(acquire(pouch.sessions, "first", &lease, &error) == CAI_OK);
  assert(get_session(lease, "/work/a", "first", &session, &error) == CAI_OK);
  session.checkpoint_ns = index.latest.checkpoint_ns + 1U;
  assert(update_session(lease, &session, &error) == CAI_OK);
  assert(release(lease, CAI_OK, &error) == CAI_OK);
  lease = NULL;
  newest(&pouch, "/work/a", "first", &error);
  assert(query_calls > 0);
  query_calls = 0;
  newest(&pouch, "/work/a", "first", &error);
  assert(query_calls == 0);
  /* Tie-breaking matches the explicit session listing. */
  assert(acquire(pouch.sessions, "second", &lease, &error) == CAI_OK);
  assert(get_session(lease, "/work/a", "second", &session, &error) == CAI_OK);
  session.checkpoint_ns = index.latest.checkpoint_ns + 1U;
  assert(update_session(lease, &session, &error) == CAI_OK);
  assert(release(lease, CAI_OK, &error) == CAI_OK);
  assert(cai_cli_pouch_publish(&pouch, "second", 1, &error) == CAI_OK);
  newest(&pouch, "/work/a", "second", &error);
  /* If wall time moves backwards, replacing the selected session's newest
   * checkpoint can make a different session newest. Cache and listing agree. */
  lease = NULL;
  assert(latest_open(&pouch, "/work/a", &lease, &index, &error) == CAI_OK);
  index.latest.checkpoint_ns += 1000000000000ULL;
  {
    lc_lease *session_lease = NULL;
    assert(acquire(pouch.sessions, "second", &session_lease, &error) == CAI_OK);
    assert(get_session(session_lease, "/work/a", "second", &session, &error) ==
           CAI_OK);
    session.checkpoint_ns = index.latest.checkpoint_ns;
    index.latest = session;
    assert(update_session(session_lease, &session, &error) == CAI_OK);
    assert(release(session_lease, CAI_OK, &error) == CAI_OK);
    session_lease = NULL;
    assert(acquire(pouch.sessions, "first", &session_lease, &error) == CAI_OK);
    assert(get_session(session_lease, "/work/a", "first", &session, &error) ==
           CAI_OK);
    session.checkpoint_ns = index.latest.checkpoint_ns - 1U;
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
    assert(acquire(pouch.sessions, "unfinished-import", &lease, &error) ==
           CAI_OK);
    assert(get_session(lease, "/work/a", "unfinished-import", &session,
                       &error) == CAI_OK);
    session.published = 0;
    assert(update_session(lease, &session, &error) == CAI_OK);
    assert(release(lease, CAI_OK, &error) == CAI_OK);
    lease = NULL;
    assert(acquire(pouch.sessions, "unfinished-import", &lease, &error) ==
           CAI_OK);
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
  cai_cli_pouch_close(&pouch);
  cai_error_cleanup(&error);
  interrupted_idle_writer();
  /* Test runner removes the isolated encrypted root. */
  printf("Fixture: %s\n", directory);
  return 0;
}
