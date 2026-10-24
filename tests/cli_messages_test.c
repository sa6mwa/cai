/* Native Pouch delivery and failure regressions. Queue waits are event-driven;
 * deadlines use milliseconds and no inference or arbitrary sleeps are needed.
 */
#include "../cli/messages.c"
#include <assert.h>
#include <poll.h>
#include <sys/stat.h>

typedef struct capture {
  char text[32768];
  size_t used;
} capture;
static int capture_write(void *context, const void *bytes, size_t count,
                         cai_error *error) {
  capture *out = context;
  (void)error;
  assert(count < sizeof(out->text) - out->used);
  memcpy(out->text + out->used, bytes, count);
  out->used += count;
  out->text[out->used] = '\0';
  return CAI_OK;
}
static cai_sink *new_capture(capture *out) {
  cai_sink_callbacks callbacks;
  cai_sink *sink = NULL;
  cai_error error;
  memset(out, 0, sizeof(*out));
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = capture_write;
  callbacks.context = out;
  cai_error_init(&error);
  assert(cai_sink_from_callbacks(&callbacks, &sink, &error) == CAI_OK);
  cai_error_cleanup(&error);
  return sink;
}
static int run_send(cai_cli_messages *m, const char *args, capture *out,
                    cai_error *error) {
  cai_sink *sink = new_capture(out);
  int rc = send_tool(m, args, sink, error);
  cai_sink_close(sink);
  return rc;
}
static int reject_enqueue(lc_client *client, const lc_enqueue_req *request,
                          lc_source *source, lc_enqueue_res *result,
                          lc_error *error) {
  (void)client;
  (void)source;
  (void)result;
  assert(strcmp(request->ns, MESSAGE_NS) == 0);
  assert(request->ttl_seconds == 0);
  return lc_error_set(error, LC_ERR_TRANSPORT, 0,
                      "injected standalone enqueue failure", NULL, NULL, NULL);
}
static void standalone_failure(cai_cli_pouch *pouch) {
  cai_cli_pouch selected = *pouch;
  lc_client client = *pouch->sessions;
  capture output;
  cai_sink *sink = new_capture(&output);
  cai_error error;
  cai_error_init(&error);
  client.enqueue = reject_enqueue;
  selected.sessions = &client;
  assert(cai_cli_messages_send_once(&selected, "offline", "hello", sink,
                                    &error) == CAI_ERR_TRANSPORT);
  assert(strstr(error.message, "injected standalone enqueue failure") != NULL);
  assert(output.used == 0U);
  cai_sink_close(sink);
  cai_error_cleanup(&error);
}
static lc_source *test_payload;
static int deferred_count;
static int defer_test_message(lc_message *message, const lc_nack_req *req,
                              lc_error *error) {
  (void)message;
  (void)error;
  assert(req->intent == LC_NACK_INTENT_DEFER);
  deferred_count++;
  return LC_OK;
}
static lc_source *get_test_payload(lc_message *message) {
  (void)message;
  return test_payload;
}
static int deliver_direct(cai_cli_messages *receiver, message_doc *doc) {
  lc_message message;
  lc_consumer_message delivery;
  lc_error local;
  cai_error error;
  cai_source *source = NULL;
  int rc;
  memset(&message, 0, sizeof(message));
  memset(&delivery, 0, sizeof(delivery));
  lc_error_init(&local);
  cai_error_init(&error);
  assert(cai_cli_mapped_source(&message_map, doc, &source, &error) == CAI_OK);
  assert(cai_cli_lc_source(source, &test_payload, &error) == CAI_OK);
  message.payload_reader = get_test_payload;
  message.nack = defer_test_message;
  delivery.message = &message;
  rc = consume(receiver, &delivery, &local);
  lc_source_close(test_payload);
  test_payload = NULL;
  cai_source_close(source);
  lc_error_cleanup(&local);
  cai_error_cleanup(&error);
  return rc;
}
static int register_calls;
static int register_failure(void *context, cai_agent *agent, cai_error *error) {
  (void)context;
  (void)agent;
  register_calls++;
  return cai_cli_error(error, CAI_ERR_INVALID, "host registration failed");
}
static int register_host(void *context, cai_agent *agent, cai_error *error) {
  register_calls++;
  return cai_cli_messages_register(context, agent, error);
}
static int has_host_tool(void *context, const cai_agent_tool_info *info,
                         cai_error *error) {
  int *found = context;
  (void)error;
  if (strcmp(info->name, "agent_message") == 0)
    *found = 1;
  return CAI_OK;
}
static void host_hook(cai_cli_messages *m, const char *workspace) {
  cai_agent_runtime_config config;
  cai_client_config client_config;
  cai_agent_runtime *runtime = NULL;
  cai_client *client = NULL;
  cai_error error;
  int found = 0;
  cai_error_init(&error);
  cai_client_config_init(&client_config);
  client_config.api_key = "fixture";
  client_config.base_url = "http://127.0.0.1:1/v1";
  assert(cai_client_open(&client_config, &client, &error) == CAI_OK);
  cai_agent_runtime_config_init(&config);
  config.workspace_directory = workspace;
  config.disable_default_session_store = 1;
  config.disable_chrome = 1;
  config.register_agent_tools = register_host;
  config.register_agent_tools_context = m;
  assert(cai_agent_runtime_open(client, &config, &runtime, &error) == CAI_OK);
  assert(register_calls == 1);
  assert(cai_agent_runtime_visit_tools(runtime, has_host_tool, &found,
                                       &error) == CAI_OK);
  assert(found);
  cai_agent_runtime_close(runtime);
  runtime = NULL;
  config.register_agent_tools = register_failure;
  assert(cai_agent_runtime_open(client, &config, &runtime, &error) ==
         CAI_ERR_INVALID);
  assert(runtime == NULL && register_calls == 2);
  assert(strstr(error.message, "host registration failed") != NULL);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  config.preset = CAI_SMITH_REVIEW_PRESET;
  config.enable_image_generation = 0;
  assert(cai_agent_runtime_open(client, &config, &runtime, &error) ==
         CAI_ERR_INVALID);
  assert(runtime == NULL && register_calls == 2);
  assert(strstr(error.message, "host tools") != NULL);
  cai_client_close(client);
  cai_error_cleanup(&error);
}
static int fail_acquire(lc_client *self, const lc_acquire_req *req,
                        lc_lease **out, lc_error *error) {
  (void)self;
  (void)req;
  *out = NULL;
  return lc_error_set(error, LC_ERR_TRANSPORT, 0, "storage unavailable", NULL,
                      NULL, NULL);
}
static void persistence_failure(cai_cli_messages *receiver, message_doc *doc) {
  cai_cli_pouch pouch;
  cai_cli_messages fake;
  lc_client client;
  memset(&client, 0, sizeof(client));
  memset(&pouch, 0, sizeof(pouch));
  memset(&fake, 0, sizeof(fake));
  client.acquire = fail_acquire;
  pouch.sessions = &client;
  fake.pouch = &pouch;
  strcpy(fake.session, receiver->session);
  strcpy(fake.queue, receiver->queue);
  /* Failed durable acceptance returns an error to the managed consumer,
   * which NACKs delivery. It must not claim delivery or touch the UI. */
  assert(deliver_direct(&fake, doc) == LC_ERR_TRANSPORT);
}
typedef struct waiting_send {
  cai_cli_messages *sender;
  int result;
} waiting_send;
static void *wait_to_send(void *context) {
  waiting_send *waiting = context;
  cai_error error;
  capture output;
  cai_error_init(&error);
  waiting->result = run_send(waiting->sender,
                             "{\"operation\":\"send\",\"recipient\":\"cancel-"
                             "target\",\"text\":\"cancel me\"}",
                             &output, &error);
  cai_error_cleanup(&error);
  return NULL;
}
static void cancel_wait(cai_cli_messages *sender) {
  waiting_send waiting;
  pthread_t thread;
  lc_dequeue_req req;
  lc_message *message = NULL;
  lc_error error;
  lc_error_init(&error);
  waiting.sender = sender;
  waiting.result = CAI_OK;
  sender->timeout_ms = 2000L;
  assert(pthread_create(&thread, NULL, wait_to_send, &waiting) == 0);
  lc_dequeue_req_init(&req);
  req.ns = MESSAGE_NS;
  req.queue = "cancel-target";
  req.owner = "fixture";
  req.wait_seconds = 2;
  /* Receiving the queued envelope is the barrier: sender is now waiting. */
  assert(lc_dequeue(sender->pouch->sessions, &req, &message, &error) == LC_OK);
  assert(message != NULL);
  cai_cli_messages_cancel_send(sender);
  assert(pthread_join(thread, NULL) == 0);
  assert(waiting.result == CAI_ERR_CANCELLED);
  lc_message_close(message);
  lc_error_cleanup(&error);
}
static sl_t *message_editor(void) {
  sl_t *sl = sl_create();
  assert(sl != NULL);
  assert(sl_set_prompt_queue(sl, 1, 8, 3) == SL_OK);
  return sl;
}
static void send_named(cai_cli_messages *sender, const char *queue,
                       const char *text) {
  char args[512];
  capture output;
  cai_error error;
  cai_error_init(&error);
  snprintf(args, sizeof(args),
           "{\"operation\":\"send\",\"recipient\":\"%s\",\"text\":\"%s\"}",
           queue, text);
  assert(run_send(sender, args, &output, &error) == CAI_OK);
  assert(strstr(output.text, "\"delivered\"") != NULL);
  cai_error_cleanup(&error);
}

static cai_cli_messages *startup_sender;
static lc_client *startup_client;
static int startup_phase, startup_enqueues;
static int cancel_startup_acquire(lc_client *self, const lc_acquire_req *req,
                                  lc_lease **out, lc_error *error) {
  int phase = startup_phase, rc;
  (void)self;
  if (phase != 0 && strcmp(req->key, "mailbox.startup-sender") == 0) {
    startup_phase = 0;
    if (phase == 1)
      cai_cli_messages_cancel_send(startup_sender);
  } else
    phase = 0;
  rc = startup_client->acquire(startup_client, req, out, error);
  if (phase == 2)
    cai_cli_messages_cancel_send(startup_sender);
  return rc;
}
static int count_startup_enqueue(lc_client *self, const lc_enqueue_req *req,
                                 lc_source *source, lc_enqueue_res *out,
                                 lc_error *error) {
  (void)self;
  startup_enqueues++;
  return startup_client->enqueue(startup_client, req, source, out, error);
}
static int startup_consumer(lc_client *self,
                            const lc_consumer_service_config *config,
                            lc_consumer_service **out, lc_error *error) {
  (void)self;
  return startup_client->new_consumer_service(startup_client, config, out,
                                              error);
}
static void cancel_startup(cai_cli_pouch *pouch, const char *workspace) {
  cai_cli_pouch selected = *pouch;
  lc_client proxy;
  cai_error error;
  capture output;
  int phase;
  startup_client = pouch->sessions;
  memset(&proxy, 0, sizeof(proxy));
  proxy.acquire = cancel_startup_acquire;
  proxy.enqueue = count_startup_enqueue;
  proxy.new_consumer_service = startup_consumer;
  selected.sessions = &proxy;
  cai_error_init(&error);
  assert(cai_cli_messages_new(&selected, 5L, &startup_sender, &error) ==
         CAI_OK);
  assert(cai_cli_messages_start(startup_sender, "startup-sender", workspace,
                                "startup-sender", &error) == CAI_OK);
  for (phase = 1; phase <= 2; phase++) {
    /* Synchronous acquisition callbacks are deterministic barriers. Cancel
     * before/after the real lease acquisition; no sleeps or timeout waits. */
    startup_enqueues = 0;
    startup_phase = phase;
    assert(run_send(startup_sender,
                    "{\"operation\":\"send\",\"recipient\":\"absent-startup-"
                    "recipient\",\"text\":\"Do not enqueue this\"}",
                    &output, &error) == CAI_ERR_CANCELLED);
    assert(startup_phase == 0 && startup_enqueues == 0 && output.used == 0U);
    cai_error_cleanup(&error);
    cai_error_init(&error);
  }
  /* Cancellation of an idle sender must not poison a later successful send. */
  cai_cli_messages_cancel_send(startup_sender);
  startup_sender->timeout_ms = 2000L;
  send_named(startup_sender, "receiver", "A fresh send after cancellation");
  assert(startup_enqueues == 1);
  cai_cli_messages_close(startup_sender);
  startup_sender = NULL;
  startup_client = NULL;
  cai_error_cleanup(&error);
}
static void naming_rules(void) {
  cai_cli_message_name self = {"node-a", "machine-a", "/home/mike/g/lab", "",
                               1};
  cai_cli_message_name known[3] = {
      {"node-a", "machine-a", "/home/alice/g/lab", "cai.lab", 1},
      {"node-a", "machine-a", "/x/common/lab", "cai.x.lab", 1},
      {"node-a", "machine-a", "/y/d/b/lab", "cai.y.lab", 1}};
  cai_error error;
  char name[129], deep[4096], deeper[4096], large[160];
  size_t i;
  cai_error_init(&error);
  assert(cai_cli_message_name_select(&self, NULL, 0U, name, &error) == CAI_OK);
  assert(strcmp(name, "cai.lab") == 0);
  assert(cai_cli_message_name_select(&self, known, 1U, name, &error) == CAI_OK);
  assert(strcmp(name, "cai.mike.lab") == 0);
  self.workspace = "/home/alice/g/lab";
  known[0].workspace = "/home/mike/g/lab";
  assert(cai_cli_message_name_select(&self, known, 1U, name, &error) == CAI_OK);
  assert(strcmp(name, "cai.alice.lab") == 0);
  self.workspace = "/home/mike/g/lab";
  known[0].workspace = "/lab";
  assert(cai_cli_message_name_select(&self, known, 1U, name, &error) == CAI_OK);
  assert(strcmp(name, "cai.g.lab") == 0);
  self.workspace = "/a/b/lab";
  known[0].workspace = "/b/a/lab";
  assert(cai_cli_message_name_select(&self, known, 1U, name, &error) == CAI_OK);
  assert(strcmp(name, "cai.a.b.lab") == 0);
  /* Component count wins even when a two-component choice is shorter text. */
  self.workspace = "/long-descriptive-real-directory/a/b/lab";
  known[0].workspace = "/x/a/c/lab";
  assert(cai_cli_message_name_select(&self, known, 3U, name, &error) == CAI_OK);
  assert(strcmp(name, "cai.long-descriptive-real-directory.lab") == 0);
  self.workspace = "/home/mike/g/lab";
  self.hostname = "node-b";
  self.host_id = "machine-b";
  known[0].workspace = self.workspace;
  assert(cai_cli_message_name_select(&self, known, 1U, name, &error) == CAI_OK);
  assert(strcmp(name, "cai.node-b.lab") == 0);
  known[0].hostname = "node-b";
  assert(cai_cli_message_name_select(&self, known, 1U, name, &error) ==
         CAI_ERR_INVALID);
  assert(strstr(error.message, "same hostname") != NULL);
  known[0].host_id = "machine-b";
  known[0].name = "cai.saved.lab";
  assert(cai_cli_message_name_select(&self, known, 1U, name, &error) == CAI_OK);
  assert(strcmp(name, "cai.saved.lab") == 0);
  known[1] = known[0];
  known[1].host_id = "duplicate-hostname-machine";
  known[1].name = "explicit-other-host";
  known[1].automatic = 0;
  assert(cai_cli_message_name_select(&self, known, 2U, name, &error) == CAI_OK);
  assert(strcmp(name, "cai.saved.lab") == 0);
  known[0].name = "Invalid saved name";
  assert(cai_cli_message_name_select(&self, known, 1U, name, &error) ==
         CAI_ERR_PROTOCOL);
  known[0].name = "cai.lab";
  known[0].workspace = "/g/lab";
  self.workspace = "/lab";
  assert(cai_cli_message_name_select(&self, known, 1U, name, &error) ==
         CAI_ERR_INVALID);
  assert(strstr(error.message, "--message-queue NAME") != NULL);
  self.workspace = "/Uppercase";
  assert(cai_cli_message_name_select(&self, NULL, 0U, name, &error) ==
         CAI_ERR_INVALID);
  self.workspace = "/space name";
  assert(cai_cli_message_name_select(&self, NULL, 0U, name, &error) ==
         CAI_ERR_INVALID);
  self.workspace = "/";
  assert(cai_cli_message_name_select(&self, NULL, 0U, name, &error) ==
         CAI_ERR_INVALID);
  memset(large, 'a', sizeof(large));
  large[0] = '/';
  large[sizeof(large) - 1U] = '\0';
  self.workspace = large;
  assert(cai_cli_message_name_select(&self, NULL, 0U, name, &error) ==
         CAI_ERR_INVALID);
  /* Identical common ancestors must not cause an exponential search. */
  for (i = 0U; i < 1000U; i++) {
    deep[i * 2U] = '/';
    deep[i * 2U + 1U] = 'g';
  }
  strcpy(deep + 2000U, "/lab");
  strcpy(deeper, "/extra");
  strcat(deeper, deep);
  self.workspace = deep;
  known[0].workspace = deeper;
  assert(cai_cli_message_name_select(&self, known, 1U, name, &error) ==
         CAI_ERR_INVALID);
  /* Repeated components also stay cheap when the full paths do differ,
   * but the distinguishing repetition cannot fit the queue-name limit. */
  known[0].workspace = deep + 2U;
  assert(cai_cli_message_name_select(&self, known, 1U, name, &error) ==
         CAI_ERR_INVALID);
  cai_error_cleanup(&error);
}

static void workspace_mailboxes(cai_cli_pouch *pouch, const char *root) {
  cai_cli_messages *sender = NULL, *old = NULL, *new_owner = NULL,
                   *other = NULL, *collision = NULL;
  cai_error error;
  lc_error local;
  lc_lease *guard = NULL;
  sl_t *old_sl, *new_sl;
  message_doc doc;
  capture output;
  char parent[PATH_MAX], one[PATH_MAX], two[PATH_MAX], link[PATH_MAX];
  char name[129], alias[129], key[160];
  int rc;
  cai_error_init(&error);
  lc_error_init(&local);
  snprintf(parent, sizeof(parent), "%s/one", root);
  assert(mkdir(parent, 0700) == 0);
  snprintf(one, sizeof(one), "%s/one/project", root);
  assert(mkdir(one, 0700) == 0);
  snprintf(parent, sizeof(parent), "%s/two", root);
  assert(mkdir(parent, 0700) == 0);
  snprintf(two, sizeof(two), "%s/two/project", root);
  assert(mkdir(two, 0700) == 0);
  strcpy(name, "cai.project");
  snprintf(link, sizeof(link), "%s/project-link", root);
  assert(symlink(one, link) == 0);
  assert(queue_name("Friendly-Name", alias) &&
         strcmp(alias, "friendly-name") == 0);
  assert(!queue_name("../bad", alias) && !queue_name("", alias));
  assert(cai_cli_messages_new(pouch, 2000L, &sender, &error) == CAI_OK);
  assert(cai_cli_messages_start(sender, "naming-sender", root, "test-sender",
                                &error) == CAI_OK);
  assert(cai_cli_messages_new(pouch, 2000L, &old, &error) == CAI_OK);
  assert(cai_cli_messages_start(old, "old-conversation", link, NULL, &error) ==
         CAI_OK);
  assert(strcmp(cai_cli_messages_name(old), name) == 0);
  old_sl = message_editor();
  send_named(sender, name, "Held in old conversation");
  assert(cai_cli_messages_sync(old, old_sl, &error) == CAI_OK);
  assert(sl_prompt_queue_count(old_sl) == 1U);
  assert(cai_cli_messages_new(pouch, 2000L, &new_owner, &error) == CAI_OK);
  assert(cai_cli_messages_start(new_owner, "new-conversation", one, NULL,
                                &error) == CAI_OK);
  assert(strcmp(cai_cli_messages_name(new_owner), name) == 0);
  assert(strcmp(old->instance, new_owner->instance) != 0);
  new_sl = message_editor();
  assert(cai_cli_messages_sync(new_owner, new_sl, &error) == CAI_OK);
  assert(sl_prompt_queue_count(new_sl) == 0U);
  /* Force a stale consumer's delivery callback, independently of which
   * process wins the next dequeue. It must defer, never ACK or accept. */
  memset(&doc, 0, sizeof(doc));
  assert(lc_xid_new(doc.id, &local) == LC_OK);
  strcpy(doc.kind, "message");
  strcpy(doc.sender, sender->queue);
  strcpy(doc.sender_session, sender->session);
  strcpy(doc.instance, sender->instance);
  strcpy(doc.recipient, name);
  doc.text = output.text;
  strcpy(output.text, "Stale consumer must defer");
  doc.deadline_ms = realtime_ms() + 2000L;
  rc = deferred_count;
  assert(deliver_direct(old, &doc) == LC_OK);
  assert(deferred_count == rc + 1 && old->lost);
  assert(lc_consumer_service_wait(old->service, &local) == LC_OK);
  send_named(sender, name, "Only the new conversation receives this");
  assert(cai_cli_messages_sync(new_owner, new_sl, &error) == CAI_OK);
  assert(sl_prompt_queue_count(new_sl) == 1U);
  assert(strcmp(new_owner->links->doc.pending_for, "new-conversation") == 0);
  assert(cai_cli_messages_sync(old, old_sl, &error) == CAI_OK);
  assert(sl_prompt_queue_count(old_sl) == 1U);
  assert(strcmp(old->links->doc.pending_for, "old-conversation") == 0);
  cai_cli_messages_close(old);
  old = NULL;
  sl_destroy(old_sl);
  /* A displaced process exiting cannot clear the current binding. */
  assert(mailbox_guard(new_owner, &guard, &local) == LC_OK);
  assert(lease_finish(guard, LC_OK, &local) == LC_OK);
  guard = NULL;
  send_named(sender, name, "New owner survives the old owner exiting");
  assert(cai_cli_messages_new(pouch, 2000L, &other, &error) == CAI_OK);
  assert(cai_cli_messages_start(other, "other-project", two, NULL, &error) ==
         CAI_OK);
  assert(strcmp(other->queue, "cai.two.project") == 0);
  assert(cai_cli_messages_set_queue(other, "cai.two.project", &error) ==
         CAI_OK);
  assert(load_doc(sender, "mailbox.cai.two.project", &doc, &error) == CAI_OK);
  assert(doc.automatic);
  lonejson_cleanup(&message_map, &doc);
  assert(run_send(sender, "{\"operation\":\"list\"}", &output, &error) ==
         CAI_OK);
  assert(strstr(output.text, "This application's mailbox: test-sender") !=
         NULL);
  assert(strstr(output.text, name) != NULL &&
         strstr(output.text, other->queue) != NULL);
  cai_cli_messages_close(new_owner);
  new_owner = NULL;
  sl_destroy(new_sl);
  /* Resuming the original conversation restores its own undecided message. */
  assert(cai_cli_messages_new(pouch, 2000L, &old, &error) == CAI_OK);
  assert(cai_cli_messages_start(old, "old-conversation", one, NULL, &error) ==
         CAI_OK);
  old_sl = message_editor();
  assert(cai_cli_messages_sync(old, old_sl, &error) == CAI_OK);
  assert(sl_prompt_queue_count(old_sl) == 1U);
  assert(strcmp(old->links->doc.text, "Held in old conversation") == 0);
  cai_cli_messages_close(old);
  sl_destroy(old_sl);
  cai_cli_messages_close(other);
  other = NULL;
  /* An explicit takeover reserves the former automatic name. Reallocation
   * must use real distinguishing components, without taking it back. */
  assert(cai_cli_messages_new(pouch, 2000L, &other, &error) == CAI_OK);
  assert(cai_cli_messages_start(other, "explicit-owner", two, name, &error) ==
         CAI_OK);
  assert(cai_cli_messages_new(pouch, 2000L, &collision, &error) == CAI_OK);
  assert(cai_cli_messages_start(collision, "collision", one, NULL, &error) ==
         CAI_OK);
  assert(strcmp(collision->queue, "cai.one.project") == 0);
  cai_cli_messages_close(collision);
  assert(mailbox_guard(other, &guard, &local) == LC_OK);
  assert(lease_finish(guard, LC_OK, &local) == LC_OK);
  snprintf(key, sizeof(key), "mailbox.%s", name);
  assert(load_doc(sender, key, &doc, &error) == CAI_OK);
  assert(strcmp(doc.sender_session, "explicit-owner") == 0);
  lonejson_cleanup(&message_map, &doc);
  cai_cli_messages_close(other);
  cai_cli_messages_close(sender);
  lc_error_cleanup(&local);
  cai_error_cleanup(&error);
}

static cai_cli_messages *name_claimant(cai_cli_pouch *pouch,
                                       const char *workspace,
                                       const char *hostname, const char *id) {
  cai_cli_messages *m = NULL;
  cai_error error;
  lc_error local;
  cai_error_init(&error);
  lc_error_init(&local);
  assert(cai_cli_messages_new(pouch, 2000L, &m, &error) == CAI_OK);
  strcpy(m->workspace, workspace);
  strcpy(m->hostname, hostname);
  strcpy(m->host_id, id);
  strcpy(m->session, "naming-fixture");
  assert(lc_xid_new(m->instance, &local) == LC_OK);
  lc_error_cleanup(&local);
  cai_error_cleanup(&error);
  return m;
}

typedef struct name_race {
  cai_cli_messages *messages;
  pthread_barrier_t *barrier;
  int result;
} name_race;

static void *claim_concurrently(void *context) {
  name_race *race = context;
  cai_error error;
  int rc;
  cai_error_init(&error);
  rc = pthread_barrier_wait(race->barrier);
  assert(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD);
  race->result = mailbox_claim(race->messages, NULL, 1, &error);
  cai_error_cleanup(&error);
  return NULL;
}

static void shared_naming(cai_cli_pouch *pouch, const char *root) {
  const char *id_a = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  const char *id_b = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  const char *id_c = "cccccccccccccccccccccccccccccccc";
  cai_cli_messages *a, *b, *m;
  cai_error error;
  char workspace[PATH_MAX], name[129], left[PATH_MAX], right[PATH_MAX];
  pthread_barrier_t barrier;
  pthread_t threads[2];
  name_race race[2];
  size_t i;
  cai_error_init(&error);
  /* More than one catalog page; offline bindings must still reserve names. */
  for (i = 0U; i < PAGE_KEYS + 3U; i++) {
    m = name_claimant(pouch, root, "node-a", id_a);
    snprintf(name, sizeof(name), "offline-%lu", (unsigned long)i);
    assert(mailbox_claim(m, name, 0, &error) == CAI_OK);
    strcpy(m->queue, name);
    cai_cli_messages_close(m);
  }
  snprintf(workspace, sizeof(workspace), "%s/shared-lab", root);
  assert(mkdir(workspace, 0700) == 0);
  a = name_claimant(pouch, workspace, "node-a", id_a);
  b = name_claimant(pouch, workspace, "node-b", id_b);
  assert(mailbox_claim(a, NULL, 1, &error) == CAI_OK);
  assert(strcmp(a->queue, "cai.shared-lab") == 0);
  assert(mailbox_claim(b, NULL, 1, &error) == CAI_OK);
  assert(strcmp(b->queue, "cai.node-b.shared-lab") == 0);
  cai_cli_messages_close(a);
  a = name_claimant(pouch, workspace, "node-a", id_a);
  assert(mailbox_claim(a, NULL, 1, &error) == CAI_OK);
  assert(strcmp(a->queue, "cai.shared-lab") == 0);
  m = name_claimant(pouch, workspace, "node-a", id_b);
  assert(mailbox_claim(m, NULL, 1, &error) == CAI_OK);
  assert(strcmp(m->queue, "cai.node-b.shared-lab") == 0);
  cai_cli_messages_close(m);
  m = name_claimant(pouch, workspace, "node-a", id_c);
  assert(mailbox_claim(m, NULL, 1, &error) == CAI_ERR_INVALID);
  assert(strstr(error.message, "same hostname") != NULL);
  assert(mailbox_claim(m, "explicit-other-host", 0, &error) == CAI_OK);
  strcpy(m->queue, "explicit-other-host");
  assert(mailbox_claim(a, NULL, 1, &error) == CAI_OK);
  assert(strcmp(a->queue, "cai.shared-lab") == 0);
  cai_cli_messages_close(m);
  cai_cli_messages_close(b);
  cai_cli_messages_close(a);
  snprintf(left, sizeof(left), "%s/left", root);
  snprintf(right, sizeof(right), "%s/right", root);
  assert(mkdir(left, 0700) == 0 && mkdir(right, 0700) == 0);
  strcat(left, "/concurrent");
  strcat(right, "/concurrent");
  assert(mkdir(left, 0700) == 0 && mkdir(right, 0700) == 0);
  race[0].messages = name_claimant(pouch, left, "node-a", id_a);
  race[1].messages = name_claimant(pouch, right, "node-a", id_a);
  assert(pthread_barrier_init(&barrier, NULL, 2U) == 0);
  for (i = 0U; i < 2U; i++) {
    race[i].barrier = &barrier;
    race[i].result = CAI_ERR_INVALID;
    assert(pthread_create(&threads[i], NULL, claim_concurrently, &race[i]) ==
           0);
  }
  for (i = 0U; i < 2U; i++) {
    assert(pthread_join(threads[i], NULL) == 0);
    assert(race[i].result == CAI_OK);
  }
  assert(strcmp(race[0].messages->queue, race[1].messages->queue) != 0);
  assert(strcmp(race[0].messages->queue, "cai.concurrent") == 0 ||
         strcmp(race[1].messages->queue, "cai.concurrent") == 0);
  assert(strcmp(race[0].messages->queue, "cai.concurrent") == 0 ||
         strcmp(race[0].messages->queue, "cai.left.concurrent") == 0);
  assert(strcmp(race[1].messages->queue, "cai.concurrent") == 0 ||
         strcmp(race[1].messages->queue, "cai.right.concurrent") == 0);
  for (i = 0U; i < 2U; i++)
    cai_cli_messages_close(race[i].messages);
  assert(pthread_barrier_destroy(&barrier) == 0);
  cai_error_cleanup(&error);
}
static int fail_consumer(lc_client *client,
                         const lc_consumer_service_config *config,
                         lc_consumer_service **out, lc_error *error) {
  (void)client;
  (void)config;
  *out = NULL;
  return lc_error_set(error, LC_ERR_TRANSPORT, 0,
                      "listener preparation unavailable", NULL, NULL, NULL);
}
static void queue_overrides(cai_cli_pouch *pouch, const char *workspace) {
  cai_cli_messages *sender = NULL, *receiver = NULL, *other = NULL,
                   *failed = NULL;
  cai_cli_pouch fake_pouch;
  lc_client fake_client;
  cai_error error;
  capture output;
  message_doc doc;
  sl_t *sl = message_editor();
  sl_prompt_queue_entry_info_t held;
  char too_long[130];
  cai_error_init(&error);
  assert(cai_cli_messages_new(pouch, 2000L, &sender, &error) == CAI_OK);
  assert(cai_cli_messages_start(sender, "override-sender", workspace,
                                "override-sender", &error) == CAI_OK);
  assert(cai_cli_messages_new(pouch, 2000L, &receiver, &error) == CAI_OK);
  assert(cai_cli_messages_start(receiver, "override-receiver", workspace,
                                "before-rename", &error) == CAI_OK);
  send_named(sender, "before-rename", "Keep this pending approval");
  assert(cai_cli_messages_sync(receiver, sl, &error) == CAI_OK);
  assert(sl_prompt_queue_info(sl, 0U, &held) == SL_OK && held.held);
  assert(cai_cli_messages_set_queue(receiver, "bad/name", &error) != CAI_OK);
  memset(too_long, 'a', sizeof(too_long) - 1U);
  too_long[sizeof(too_long) - 1U] = '\0';
  assert(cai_cli_messages_set_queue(receiver, too_long, &error) != CAI_OK);
  assert(cai_cli_messages_set_queue(receiver, "", &error) != CAI_OK);
  assert(strcmp(receiver->queue, "before-rename") == 0);
  pthread_mutex_lock(&receiver->lock);
  strcpy(receiver->waiting, "busy");
  pthread_mutex_unlock(&receiver->lock);
  assert(cai_cli_messages_set_queue(receiver, "busy-rename", &error) != CAI_OK);
  pthread_mutex_lock(&receiver->lock);
  receiver->waiting[0] = '\0';
  pthread_mutex_unlock(&receiver->lock);
  assert(cai_cli_messages_set_queue(receiver, "Full-Name", &error) == CAI_OK);
  assert(strcmp(receiver->queue, "full-name") == 0);
  memset(&doc, 0, sizeof(doc));
  strcpy(doc.kind, "receipt");
  strcpy(doc.id, receiver->links->doc.id);
  strcpy(doc.sender, "override-sender");
  strcpy(doc.recipient, "before-rename");
  strcpy(doc.recipient_instance, receiver->instance);
  assert(deliver_direct(receiver, &doc) == LC_OK);
  assert(!receiver->receipt);
  strcpy(doc.recipient_instance, sender->instance);
  assert(deliver_direct(receiver, &doc) != LC_OK);
  assert(load_doc(sender, "mailbox.before-rename", &doc, &error) == CAI_OK);
  assert(doc.instance[0] == '\0');
  lonejson_cleanup(&message_map, &doc);
  assert(cai_cli_messages_is_message(receiver, held.id));
  assert(cai_cli_messages_sync(receiver, sl, &error) == CAI_OK);
  assert(sl_prompt_queue_count(sl) == 1U);
  send_named(sender, "full-name", "Delivered to the new full name");
  send_named(receiver, "override-sender", "Reply uses the new sender name");
  assert(cai_cli_messages_sync(receiver, sl, &error) == CAI_OK);
  assert(sl_prompt_queue_count(sl) == 2U);
  assert(run_send(receiver, "{\"operation\":\"list\"}", &output, &error) ==
         CAI_OK);
  assert(strstr(output.text, "This application's mailbox: full-name") != NULL);
  assert(cai_cli_messages_new(pouch, 2000L, &other, &error) == CAI_OK);
  assert(cai_cli_messages_start(other, "takes-over", workspace, "full-name",
                                &error) == CAI_OK);
  /* Selecting the same name explicitly reclaims it after a takeover. */
  assert(cai_cli_messages_set_queue(receiver, "full-name", &error) == CAI_OK);
  send_named(sender, "full-name", "Reclaimed listener receives again");
  assert(cai_cli_messages_sync(receiver, sl, &error) == CAI_OK);
  assert(sl_prompt_queue_count(sl) == 3U);
  memset(&fake_client, 0, sizeof(fake_client));
  fake_pouch = *pouch;
  fake_pouch.sessions = &fake_client;
  fake_client.new_consumer_service = fail_consumer;
  assert(cai_cli_messages_new(&fake_pouch, 2000L, &failed, &error) == CAI_OK);
  strcpy(failed->queue, "unchanged-on-error");
  assert(cai_cli_messages_set_queue(failed, "new-name", &error) != CAI_OK);
  assert(strstr(error.message, "listener preparation unavailable") != NULL);
  assert(strcmp(failed->queue, "unchanged-on-error") == 0);
  cai_cli_messages_close(failed);
  cai_cli_messages_close(other);
  cai_cli_messages_close(receiver);
  cai_cli_messages_close(sender);
  sl_destroy(sl);
  cai_error_cleanup(&error);
}
int main(int argc, char **argv) {
  cai_cli_pouch a, b;
  cai_cli_messages *sender = NULL, *receiver = NULL;
  cai_error error;
  lc_error local;
  capture output;
  sl_t *sl;
  sl_prompt_queue_entry_info_t first, second;
  message_doc duplicate, expired, loaded;
  char key[300];
  char expired_text[] = "expired";
  char controls[] = "illegal\033[31m terminal control";
  assert(argc == 2 || (argc == 3 && strcmp(argv[2], "startup") == 0));
  naming_rules();
  cai_error_init(&error);
  lc_error_init(&local);
  assert(cai_cli_pouch_open(&a, NULL, NULL, NULL, &error) == CAI_OK);
  assert(cai_cli_pouch_open(&b, NULL, NULL, NULL, &error) == CAI_OK);
  assert(cai_cli_messages_new(&a, 2000L, &sender, &error) == CAI_OK);
  assert(cai_cli_messages_new(&b, 2000L, &receiver, &error) == CAI_OK);
  assert(cai_cli_messages_start(sender, "sender", argv[1], "sender", &error) ==
         CAI_OK);
  assert(cai_cli_messages_start(receiver, "receiver", argv[1], "receiver",
                                &error) == CAI_OK);
  if (argc == 3) {
    cancel_startup(&a, argv[1]);
    cai_cli_messages_close(receiver);
    cai_cli_messages_close(sender);
    cai_cli_pouch_close(&b);
    cai_cli_pouch_close(&a);
    lc_error_cleanup(&local);
    cai_error_cleanup(&error);
    return 0;
  }
  host_hook(sender, argv[1]);
  standalone_failure(&a);
  /* No UI approval or model turn is required for durable delivery ACK. */
  assert(run_send(sender,
                  "{\"operation\":\"send\",\"recipient\":\"receiver\",\"text\":"
                  "\"First inbound message\"}",
                  &output, &error) == CAI_OK);
  assert(strstr(output.text, "\"status\":\"delivered\"") != NULL);
  sl = sl_create();
  assert(sl != NULL);
  assert(sl_set_prompt_queue(sl, 1, 8, 3) == SL_OK);
  assert(cai_cli_messages_sync(receiver, sl, &error) == CAI_OK);
  assert(sl_prompt_queue_count(sl) == 1U);
  assert(sl_prompt_queue_info(sl, 0U, &first) == SL_OK && first.held &&
         first.mode == SL_PROMPT_QUEUE_MODE_QUEUED);
  assert(receiver->links != NULL);
  duplicate = receiver->links->doc;
  strcpy(duplicate.kind, "message");
  assert(deliver_direct(receiver, &duplicate) == LC_OK);
  assert(cai_cli_messages_sync(receiver, sl, &error) == CAI_OK);
  assert(sl_prompt_queue_count(sl) == 1U);
  assert(sl_prompt_queue_info(sl, 0U, &second) == SL_OK &&
         second.id == first.id);
  assert(run_send(sender,
                  "{\"operation\":\"send\",\"recipient\":\"receiver\",\"text\":"
                  "\"Second inbound message\"}",
                  &output, &error) == CAI_OK);
  assert(cai_cli_messages_sync(receiver, sl, &error) == CAI_OK);
  assert(sl_prompt_queue_count(sl) == 2U);
  assert(sl_prompt_queue_approve(sl, first.id) == SL_OK);
  assert(sl_prompt_queue_info(sl, 0U, &first) == SL_OK && !first.held &&
         first.mode == SL_PROMPT_QUEUE_MODE_QUEUED);
  assert(sl_set_auto_approval(sl, 1) == SL_OK);
  assert(sl_prompt_queue_info(sl, 1U, &second) == SL_OK && !second.held);
  assert(sl_set_auto_approval(sl, 0) == SL_OK);
  assert(sl_prompt_queue_reject(sl, first.id) == SL_OK);
  assert(cai_cli_messages_sync(receiver, sl, &error) == CAI_OK);
  inbox_key(key, &duplicate);
  assert(load_doc(receiver, key, &loaded, &error) == CAI_OK);
  assert(strcmp(loaded.kind, "rejected") == 0);
  assert(loaded.text == NULL && loaded.pending_for[0] == '\0');
  lonejson_cleanup(&message_map, &loaded);
  /* Undecided messages survive close/reopen in the encrypted inbox. */
  cai_cli_messages_detach(receiver, sl);
  sl_destroy(sl);
  cai_cli_messages_close(receiver);
  receiver = NULL;
  assert(cai_cli_messages_new(&b, 2000L, &receiver, &error) == CAI_OK);
  assert(cai_cli_messages_start(receiver, "receiver", argv[1], "receiver",
                                &error) == CAI_OK);
  sl = sl_create();
  assert(sl != NULL);
  assert(sl_set_prompt_queue(sl, 1, 8, 3) == SL_OK);
  assert(cai_cli_messages_sync(receiver, sl, &error) == CAI_OK);
  assert(sl_prompt_queue_count(sl) == 1U);
  /* A full editor queue retains new durable messages until capacity is free. */
  assert(sl_set_prompt_queue(sl, 1, 1, 1) == SL_OK);
  assert(run_send(sender,
                  "{\"operation\":\"send\",\"recipient\":\"receiver\",\"text\":"
                  "\"Overflow inbox\"}",
                  &output, &error) == CAI_OK);
  assert(cai_cli_messages_sync(receiver, sl, &error) == CAI_OK);
  assert(sl_prompt_queue_count(sl) == 1U);
  assert(sl_prompt_queue_info(sl, 0U, &first) == SL_OK);
  assert(sl_prompt_queue_reject(sl, first.id) == SL_OK);
  assert(cai_cli_messages_sync(receiver, sl, &error) == CAI_OK);
  assert(sl_prompt_queue_count(sl) == 1U);
  /* Acknowledged messages use the normal hook even when auto-approved. */
  assert(sl_set_auto_approval(sl, 1) == SL_OK);
  assert(sl_prompt_queue_info(sl, 0U, &first) == SL_OK && !first.held &&
         first.mode == SL_PROMPT_QUEUE_MODE_QUEUED);
  {
    char *text = NULL;
    assert(sl_prompt_queue_take(sl, 0U, &text) == SL_OK);
    assert(strstr(text, "Overflow inbox") != NULL);
    sl_free_string(sl, text);
    assert(cai_cli_messages_consumed(receiver, first.id, &error) == CAI_OK);
  }
  /* An offline target fails promptly at its configured ACK deadline. */
  sender->timeout_ms = 10L;
  assert(run_send(sender,
                  "{\"operation\":\"send\",\"recipient\":\"offline\",\"text\":"
                  "\"Expires before acceptance\"}",
                  &output, &error) != CAI_OK);
  assert(error.message != NULL && strstr(error.message, "deadline") != NULL);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  assert(run_send(
             sender,
             "{\"operation\":\"send\",\"recipient\":\"../bad\",\"text\":\"x\"}",
             &output, &error) == CAI_ERR_INVALID);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  memset(&expired, 0, sizeof(expired));
  strcpy(expired.kind, "message");
  strcpy(expired.sender, "sender");
  strcpy(expired.sender_session, "sender");
  strcpy(expired.instance, sender->instance);
  strcpy(expired.recipient, "receiver");
  assert(lc_xid_new(expired.id, &local) == LC_OK);
  expired.text = expired_text;
  expired.deadline_ms = 1;
  assert(deliver_direct(receiver, &expired) == LC_OK);
  assert(cai_cli_messages_sync(receiver, sl, &error) == CAI_OK);
  assert(sl_prompt_queue_count(sl) == 0U);
  expired.deadline_ms = realtime_ms() + 2000L;
  persistence_failure(receiver, &expired);
  expired.text = controls;
  assert(deliver_direct(receiver, &expired) == LC_ERR_PROTOCOL);
  {
    char *large = malloc(MESSAGE_BYTES + 2U);
    assert(large != NULL);
    memset(large, 'x', MESSAGE_BYTES + 1U);
    large[MESSAGE_BYTES + 1U] = '\0';
    expired.text = large;
    assert(deliver_direct(receiver, &expired) == LC_ERR_PROTOCOL);
    /* A maximum-size escaped payload is parsed incrementally and accepted. */
    memset(large, '\t', MESSAGE_BYTES);
    large[MESSAGE_BYTES] = '\0';
    assert(deliver_direct(receiver, &expired) == LC_OK);
    assert(cai_cli_messages_sync(receiver, sl, &error) == CAI_OK);
    assert(sl_prompt_queue_count(sl) == 1U);
    free(large);
  }
  expired.text = expired_text;
  strcpy(expired.kind, "invalid");
  assert(deliver_direct(receiver, &expired) != LC_OK);
  strcpy(expired.kind, "message");
  strcpy(expired.recipient, "someone-else");
  assert(deliver_direct(receiver, &expired) != LC_OK);
  cancel_wait(sender);
  cai_cli_messages_stop(sender);
  assert(run_send(sender,
                  "{\"operation\":\"send\",\"recipient\":\"receiver\",\"text\":"
                  "\"Cannot send after stop\"}",
                  &output, &error) != CAI_OK);
  sl_destroy(sl);
  cai_cli_messages_close(receiver);
  cai_cli_messages_close(sender);
  workspace_mailboxes(&a, argv[1]);
  shared_naming(&a, argv[1]);
  queue_overrides(&a, argv[1]);
  cai_cli_pouch_close(&b);
  cai_cli_pouch_close(&a);
  lc_error_cleanup(&local);
  cai_error_cleanup(&error);
  return 0;
}
