/* Native Pouch delivery and failure regressions. Queue waits are event-driven;
 * deadlines use milliseconds and no inference or arbitrary sleeps are needed.
 */
#include "../cli/messages.c"
#include <assert.h>
#include <poll.h>

typedef struct capture {
  char text[4096];
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
static lc_source *test_payload;
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
  req.queue = "cai.session.cancel-target";
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
  assert(argc == 2);
  cai_error_init(&error);
  lc_error_init(&local);
  assert(cai_cli_pouch_open(&a, NULL, NULL, NULL, &error) == CAI_OK);
  assert(cai_cli_pouch_open(&b, NULL, NULL, NULL, &error) == CAI_OK);
  assert(cai_cli_messages_new(&a, 2000L, &sender, &error) == CAI_OK);
  assert(cai_cli_messages_new(&b, 2000L, &receiver, &error) == CAI_OK);
  assert(cai_cli_messages_start(sender, "sender", argv[1], &error) == CAI_OK);
  assert(cai_cli_messages_start(receiver, "receiver", argv[1], &error) ==
         CAI_OK);
  host_hook(sender, argv[1]);
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
  assert(cai_cli_messages_start(receiver, "receiver", argv[1], &error) ==
         CAI_OK);
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
  cai_cli_pouch_close(&b);
  cai_cli_pouch_close(&a);
  lc_error_cleanup(&local);
  cai_error_cleanup(&error);
  return 0;
}
