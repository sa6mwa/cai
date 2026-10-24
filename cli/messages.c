#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "messages.h"
#include "log.h"
#include "session_commands.h"
#include <errno.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#define MESSAGE_NS "cai.messages"
#define MESSAGE_BYTES 65536U
#define PAGE_KEYS 32U

typedef struct message_doc {
  char kind[16], id[LC_XID_STRING_SIZE], sender[129], recipient[129];
  char pending_for[129];
  char sender_session[129], instance[LC_XID_STRING_SIZE];
  char recipient_instance[LC_XID_STRING_SIZE], workspace[PATH_MAX];
  char *text;
  lonejson_int64 deadline_ms;
} message_doc;
static const lonejson_field message_fields[] = {
    LONEJSON_FIELD_STRING_FIXED(message_doc, kind, "kind",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(message_doc, id, "id", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(message_doc, sender, "sender",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(message_doc, recipient, "recipient",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(message_doc, pending_for, "pending_for",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(message_doc, sender_session, "sender_session",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(message_doc, instance, "instance",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(message_doc, recipient_instance,
                                "recipient_instance", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(message_doc, workspace, "workspace",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(message_doc, text, "text"),
    LONEJSON_FIELD_I64(message_doc, deadline_ms, "deadline_ms")};
LONEJSON_MAP_DEFINE(message_map, message_doc, message_fields);
typedef struct send_args {
  char operation[16], recipient[129];
  char *text;
} send_args;
static const lonejson_field argument_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(send_args, operation, "operation",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(send_args, recipient, "recipient",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(send_args, text, "text")};
LONEJSON_MAP_DEFINE(arguments_map, send_args, argument_fields);
typedef struct message_link {
  struct message_link *next;
  sl_prompt_id_t prompt_id;
  message_doc doc;
} message_link;
struct cai_cli_messages {
  cai_cli_pouch *pouch;
  lc_consumer_service *service, *receipts_service;
  pthread_mutex_t lock;
  pthread_cond_t condition;
  int fd, stopping, dirty, receipt, claimed, lost;
  unsigned long long cancel_sequence;
  long timeout_ms;
  char session[129], queue[129], instance[LC_XID_STRING_SIZE], receipts[160],
      workspace[PATH_MAX], waiting[LC_XID_STRING_SIZE], waiting_recipient[129];
  message_link *links;
};
static lonejson_int64 realtime_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_REALTIME, &now) != 0)
    return -1;
  return (lonejson_int64)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static int safe_id(const char *id) {
  size_t i, n = strlen(id);
  if (n == 0U || n > 128U)
    return 0;
  for (i = 0U; i < n; i++)
    if (strchr(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-",
            id[i]) == NULL)
      return 0;
  return 1;
}
static int queue_name(const char *input, char out[129]) {
  size_t i, length = strlen(input);
  if (length == 0U || length > 128U)
    return 0;
  for (i = 0U; i < length; i++) {
    char c = input[i];
    if (c >= 'A' && c <= 'Z')
      c = (char)(c - 'A' + 'a');
    if (strchr("abcdefghijklmnopqrstuvwxyz0123456789._-", c) == NULL)
      return 0;
    out[i] = c;
  }
  out[length] = '\0';
  return 1;
}
static int workspace_queue(const char *workspace, char out[129],
                           cai_error *error) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int count = 0U;
  char canonical[PATH_MAX], slug[118];
  const char *base;
  size_t used = 0U;
  if (realpath(workspace, canonical) == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "messaging workspace must be an existing directory");
  if (EVP_Digest(canonical, strlen(canonical), digest, &count, EVP_sha256(),
                 NULL) != 1 ||
      count < 3U)
    return cai_cli_error(error, CAI_ERR_PROTOCOL,
                         "hash messaging workspace path");
  base = strrchr(canonical, '/');
  base = base != NULL ? base + 1 : canonical;
  while (*base != '\0' && used < sizeof(slug) - 1U) {
    unsigned char c = (unsigned char)*base++;
    if (c >= 'A' && c <= 'Z')
      c = (unsigned char)(c - 'A' + 'a');
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
        c == '_')
      slug[used++] = (char)c;
    else if (used > 0U && slug[used - 1U] != '-')
      slug[used++] = '-';
  }
  while (used > 0U && slug[used - 1U] == '-')
    used--;
  slug[used] = '\0';
  if (used == 0U)
    strcpy(slug, "root");
  snprintf(out, 129U, "cai.%s.%02x%02x%02x", slug, (unsigned int)digest[0],
           (unsigned int)digest[1], (unsigned int)digest[2]);
  return CAI_OK;
}
static int valid_text(const char *text) {
  const unsigned char *p = (const unsigned char *)text;
  size_t count = 0U;
  if (p == NULL || *p == 0)
    return 0;
  while (*p != 0) {
    if ((*p < 32U && *p != '\n' && *p != '\t') || *p == 127U ||
        ++count > MESSAGE_BYTES)
      return 0;
    p++;
  }
  return 1;
}
static int fail_lc(cai_error *error, lc_error *local) {
  return cai_cli_lc_error(error, local);
}
static int lease_open(cai_cli_messages *m, const char *key, lc_lease **out,
                      lc_error *error) {
  lc_acquire_req req;
  lc_acquire_req_init(&req);
  req.ns = MESSAGE_NS;
  req.key = key;
  req.owner = m->instance;
  req.ttl_seconds = 30;
  req.block_seconds = 5;
  return lc_acquire(m->pouch->sessions, &req, out, error);
}
static int lease_finish(lc_lease *lease, int result, lc_error *error) {
  lc_release_req req;
  lc_error cleanup;
  int rc;
  lc_release_req_init(&req);
  req.rollback = result != LC_OK;
  lc_error_init(&cleanup);
  rc = lc_lease_release(lease, &req, &cleanup);
  if (rc != LC_OK) {
    lc_lease_close(lease);
    if (result == LC_OK)
      (void)lc_error_set(error, cleanup.code, cleanup.http_status,
                         cleanup.message, cleanup.detail, cleanup.server_code,
                         cleanup.correlation_id);
  }
  lc_error_cleanup(&cleanup);
  return result == LC_OK ? rc : result;
}
static void notify_owner(cai_cli_messages *m) {
  uint64_t one = 1;
  pthread_mutex_lock(&m->lock);
  m->dirty = 1;
  pthread_mutex_unlock(&m->lock);
  /* eventfd saturation already means a notification is pending. */
  while (write(m->fd, &one, sizeof(one)) < 0 && errno == EINTR) {
  }
}
static int mailbox_load(lc_lease *lease, message_doc *doc, lc_error *error) {
  lc_get_res result;
  int rc = LC_OK;
  memset(doc, 0, sizeof(*doc));
  memset(&result, 0, sizeof(result));
  if (lease->state_etag != NULL && lease->state_etag[0] != '\0')
    rc = lc_lease_load(lease, &message_map, doc, NULL, &result, error);
  lc_get_res_cleanup(&result);
  return rc;
}
static int mailbox_claim(cai_cli_messages *m, const char *name, int automatic,
                         cai_error *error) {
  lc_lease *lease = NULL;
  lc_error local;
  message_doc *doc;
  char key[160];
  int rc;
  doc = calloc(1U, sizeof(*doc));
  if (doc == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate mailbox binding");
  snprintf(key, sizeof(key), "mailbox.%s", name);
  lc_error_init(&local);
  rc = lease_open(m, key, &lease, &local);
  if (rc == LC_OK) {
    rc = mailbox_load(lease, doc, &local);
    if (rc == LC_OK && automatic && doc->workspace[0] != '\0' &&
        strcmp(doc->workspace, m->workspace) != 0)
      rc = lc_error_set(&local, LC_ERR_INVALID, 0,
                        "Generated mailbox name collides with another "
                        "directory; use --message-queue NAME",
                        name, NULL, NULL);
    if (rc == LC_OK) {
      strcpy(doc->kind, "mailbox");
      strcpy(doc->recipient, name);
      strcpy(doc->sender_session, m->session);
      strcpy(doc->instance, m->instance);
      strcpy(doc->workspace, m->workspace);
      rc = lc_lease_save(lease, &message_map, doc, &local);
    }
    rc = lease_finish(lease, rc, &local);
  }
  if (rc == LC_OK) {
    m->claimed = 1;
    if (m->pouch->logger != NULL) {
      pslog_field fields[3];
      fields[0] = pslog_str("queue", name);
      fields[1] = pslog_str("session_id", m->session);
      fields[2] = pslog_str("workspace", m->workspace);
      m->pouch->logger->info(m->pouch->logger, "Workspace mailbox claimed",
                             fields, 3U);
    }
  } else
    rc = fail_lc(error, &local);
  lc_error_cleanup(&local);
  lonejson_cleanup(&message_map, doc);
  free(doc);
  return rc;
}
/* Hold this binding lease through durable inbox acceptance. A takeover and an
 * acceptance are ordered by the same lease, so a superseded process can never
 * accept a new delivery into its old conversation. */
static int mailbox_guard(cai_cli_messages *m, lc_lease **out, lc_error *error) {
  message_doc *doc;
  char key[160];
  int rc;
  *out = NULL;
  doc = calloc(1U, sizeof(*doc));
  if (doc == NULL)
    return lc_error_set(error, LC_ERR_NOMEM, 0, "allocate mailbox owner check",
                        NULL, NULL, NULL);
  snprintf(key, sizeof(key), "mailbox.%s", m->queue);
  rc = lease_open(m, key, out, error);
  if (rc == LC_OK)
    rc = mailbox_load(*out, doc, error);
  if (rc == LC_OK && strcmp(doc->instance, m->instance) != 0) {
    int first;
    pthread_mutex_lock(&m->lock);
    first = !m->lost;
    m->lost = 1;
    pthread_mutex_unlock(&m->lock);
    if (first && m->pouch->logger != NULL) {
      pslog_field fields[2];
      fields[0] = pslog_str("queue", m->queue);
      fields[1] = pslog_str("new_session_id", doc->sender_session);
      m->pouch->logger->warn(
          m->pouch->logger,
          "Workspace mailbox taken over; new messages go to its new owner. "
          "Already delivered messages remain here.",
          fields, 2U);
    }
    notify_owner(m);
    rc =
        lc_error_set(error, LC_ERR_INVALID, 0,
                     "Workspace mailbox was taken over by another CAI instance",
                     m->queue, "mailbox_taken_over", NULL);
  }
  if (rc != LC_OK && *out != NULL) {
    rc = lease_finish(*out, rc, error);
    *out = NULL;
  }
  lonejson_cleanup(&message_map, doc);
  free(doc);
  return rc;
}
static void mailbox_release(cai_cli_messages *m, const char *name) {
  lc_error error;
  lc_lease *lease = NULL;
  message_doc *doc;
  char key[160];
  int rc;
  if (!m->claimed)
    return;
  doc = calloc(1U, sizeof(*doc));
  if (doc == NULL)
    return;
  snprintf(key, sizeof(key), "mailbox.%s", name);
  lc_error_init(&error);
  rc = lease_open(m, key, &lease, &error);
  if (rc == LC_OK) {
    rc = mailbox_load(lease, doc, &error);
    if (rc == LC_OK && strcmp(doc->instance, m->instance) == 0) {
      doc->instance[0] = '\0';
      rc = lc_lease_save(lease, &message_map, doc, &error);
    }
    rc = lease_finish(lease, rc, &error);
  }
  if (rc != LC_OK && m->pouch->logger != NULL) {
    pslog_field fields[2];
    fields[0] = pslog_str("queue", name);
    fields[1] = pslog_str("detail", error.message != NULL ? error.message
                                                          : "unknown error");
    m->pouch->logger->warn(m->pouch->logger,
                           "Cannot release workspace mailbox binding", fields,
                           2U);
  }
  lc_error_cleanup(&error);
  lonejson_cleanup(&message_map, doc);
  free(doc);
}
static void inbox_key(char key[300], const message_doc *doc) {
  snprintf(key, 300U, "inbox.%s.%s", doc->recipient, doc->id);
}
static int save_doc(cai_cli_messages *m, message_doc *doc, cai_error *error) {
  lc_lease *lease = NULL;
  lc_error local;
  char key[300];
  int rc;
  inbox_key(key, doc);
  lc_error_init(&local);
  rc = lease_open(m, key, &lease, &local);
  if (rc == LC_OK) {
    rc = lc_lease_save(lease, &message_map, doc, &local);
    rc = lease_finish(lease, rc, &local);
  }
  if (rc != LC_OK)
    rc = fail_lc(error, &local);
  lc_error_cleanup(&local);
  return rc;
}
static int enqueue(cai_cli_pouch *pouch, const message_doc *doc,
                   cai_error *error) {
  cai_source *source = NULL;
  lc_source *wire = NULL;
  lc_enqueue_req req;
  lc_enqueue_res res;
  lc_error local;
  char queue[160];
  int rc;
  if (strcmp(doc->kind, "receipt") == 0)
    snprintf(queue, sizeof(queue), "cai.receipts.%s", doc->recipient_instance);
  else
    snprintf(queue, sizeof(queue), "%s", doc->recipient);
  lc_error_init(&local);
  lc_enqueue_req_init(&req);
  memset(&res, 0, sizeof(res));
  req.ns = MESSAGE_NS;
  req.queue = queue;
  req.content_type = "application/json";
  req.visibility_timeout_seconds = 30;
  req.max_attempts = 3;
  if (strcmp(doc->kind, "message") != 0)
    req.ttl_seconds = 300;
  else if (doc->deadline_ms > 0) {
    req.ttl_seconds = (long)((doc->deadline_ms - realtime_ms() + 999) / 1000);
    if (req.ttl_seconds <= 0)
      req.ttl_seconds = 1;
  }
  rc = cai_cli_mapped_source(&message_map, doc, &source, error);
  if (rc == CAI_OK)
    rc = cai_cli_lc_source(source, &wire, error);
  if (rc == CAI_OK &&
      lc_enqueue(pouch->sessions, &req, wire, &res, &local) != LC_OK)
    rc = fail_lc(error, &local);
  lc_source_close(wire);
  cai_source_close(source);
  lc_enqueue_res_cleanup(&res);
  lc_error_cleanup(&local);
  return rc;
}
typedef struct message_reader {
  lc_source *source;
  lc_error error;
  size_t total;
} message_reader;
static lonejson_read_result read_message(void *context, unsigned char *bytes,
                                         size_t capacity) {
  message_reader *reader = context;
  lonejson_read_result out = lonejson_default_read_result();
  if (reader->total >= MESSAGE_BYTES * 6U + 4096U) {
    out.error_code = EOVERFLOW;
    return out;
  }
  if (capacity > MESSAGE_BYTES * 6U + 4096U - reader->total)
    capacity = MESSAGE_BYTES * 6U + 4096U - reader->total;
  out.bytes_read =
      reader->source->read(reader->source, bytes, capacity, &reader->error);
  reader->total += out.bytes_read;
  out.error_code = reader->error.code == LC_OK ? 0 : EIO;
  out.eof = out.bytes_read == 0U && !out.error_code;
  return out;
}
static int consume(void *context, lc_consumer_message *delivery,
                   lc_error *error) {
  cai_cli_messages *m = context;
  message_doc doc, receipt;
  message_reader reader;
  lc_lease *lease = NULL;
  lc_lease *binding = NULL;
  lonejson *json;
  lonejson_error parse_error;
  cai_error local;
  char key[300];
  int rc = LC_OK, fresh = 0;
  memset(&doc, 0, sizeof(doc));
  memset(&reader, 0, sizeof(reader));
  lc_error_init(&reader.error);
  cai_error_init(&local);
  reader.source = delivery->message->payload_reader(delivery->message);
  json = lonejson_new(NULL, &parse_error);
  if (json == NULL ||
      lonejson_parse_reader(json, &message_map, &doc, read_message, &reader,
                            &parse_error) != LONEJSON_STATUS_OK ||
      !lc_xid_is_valid(doc.id) || !safe_id(doc.sender)) {
    rc = lc_error_set(error, LC_ERR_PROTOCOL, 0,
                      "Invalid agent message envelope", NULL, NULL, NULL);
    goto done;
  }
  if (strcmp(doc.kind, "receipt") == 0) {
    if (strcmp(doc.recipient_instance, m->instance) != 0 ||
        !safe_id(doc.recipient)) {
      rc = lc_error_set(error, LC_ERR_PROTOCOL, 0,
                        "Receipt targets another application instance", NULL,
                        NULL, NULL);
      goto done;
    }
    pthread_mutex_lock(&m->lock);
    /* The private return address survives /queue; a delayed duplicate may
     * still name the former public mailbox. Only the active send matches. */
    if (strcmp(m->waiting, doc.id) == 0 &&
        strcmp(m->waiting_recipient, doc.sender) == 0) {
      m->receipt = 1;
      pthread_cond_broadcast(&m->condition);
    }
    pthread_mutex_unlock(&m->lock);
    goto done;
  }
  if (strcmp(doc.recipient, m->queue) != 0 ||
      strcmp(doc.kind, "message") != 0 || !valid_text(doc.text) ||
      !safe_id(doc.sender_session) || doc.deadline_ms < 0 ||
      (doc.instance[0] != '\0' &&
       (!lc_xid_is_valid(doc.instance) || doc.deadline_ms == 0))) {
    rc = lc_error_set(error, LC_ERR_PROTOCOL, 0,
                      "Invalid agent message content", NULL, NULL, NULL);
    goto done;
  }
  rc = mailbox_guard(m, &binding, error);
  if (rc != LC_OK) {
    if (error->server_code != NULL &&
        strcmp(error->server_code, "mailbox_taken_over") == 0) {
      lc_nack_req defer;
      lc_error deferred;
      lc_nack_req_init(&defer);
      lc_error_init(&deferred);
      defer.intent = LC_NACK_INTENT_DEFER;
      /* Intentional handoff must not consume the sender's failure budget. */
      if (lc_message_nack(delivery->message, &defer, &deferred) != LC_OK)
        rc = lc_error_set(error, deferred.code, deferred.http_status,
                          deferred.message, deferred.detail, NULL, NULL);
      else {
        /* stop() only requests shutdown; never join the callback's worker.
         * The explicitly deferred delivery is already terminal to the SDK. */
        (void)lc_consumer_service_stop(m->service);
        lc_error_cleanup(error);
        lc_error_init(error);
        rc = LC_OK;
      }
      lc_error_cleanup(&deferred);
    }
    goto done;
  }
  inbox_key(key, &doc);
  rc = lease_open(m, key, &lease, error);
  if (rc == LC_OK) {
    fresh = lease->state_etag == NULL || lease->state_etag[0] == '\0';
    if (fresh && (doc.deadline_ms == 0 || doc.deadline_ms > realtime_ms())) {
      strcpy(doc.kind, "pending");
      strcpy(doc.pending_for, m->session);
      rc = lc_lease_save(lease, &message_map, &doc, error);
    } else if (fresh) {
      rc = lease_finish(lease, rc, error);
      lease = NULL;
      goto done; /* Expired before acceptance: never execute or ACK as
                    delivered. */
    }
    rc = lease_finish(lease, rc, error);
    lease = NULL;
  }
  if (rc != LC_OK)
    goto done;
  rc = lease_finish(binding, rc, error);
  binding = NULL;
  if (rc != LC_OK)
    goto done;
  memset(&receipt, 0, sizeof(receipt));
  strcpy(receipt.kind, "receipt");
  strcpy(receipt.id, doc.id);
  strcpy(receipt.sender, m->queue);
  strcpy(receipt.recipient, doc.sender);
  strcpy(receipt.recipient_instance, doc.instance);
  /* Durable inbox acceptance is delivery. Approval and execution are separate.
   */
  /* An absent return address means enqueue-only delivery, with no receipt. */
  rc = doc.instance[0] != '\0' ? enqueue(m->pouch, &receipt, &local) : CAI_OK;
  if (rc != CAI_OK) {
    rc = lc_error_set(error, LC_ERR_TRANSPORT, 0,
                      "Cannot acknowledge agent delivery", local.message, NULL,
                      NULL);
    goto done;
  }
  notify_owner(m);
  if (fresh && m->pouch->logger != NULL) {
    pslog_field fields[3];
    fields[0] = pslog_str("sender_session_id", doc.sender_session);
    fields[1] = pslog_str("message_id", doc.id);
    fields[2] = pslog_str("sender_queue", doc.sender);
    cai_cli_log_message(m->pouch->logger, PSLOG_LEVEL_INFO,
                        "agent_message_received", "agent", doc.text,
                        strlen(doc.text), fields, 3U);
  }
done:
  if (binding != NULL)
    rc = lease_finish(binding, rc, error);
  lonejson_cleanup(&message_map, &doc);
  lonejson_free(json);
  lc_error_cleanup(&reader.error);
  cai_error_cleanup(&local);
  return rc;
}
static int service_error(void *context, const lc_consumer_error *event,
                         lc_error *error) {
  cai_cli_messages *m = context;
  pslog_field fields[2];
  char name[129];
  (void)error;
  pthread_mutex_lock(&m->lock);
  strcpy(name, m->queue);
  pthread_mutex_unlock(&m->lock);
  fields[0] = pslog_str("queue", name);
  fields[1] = pslog_str("detail", event->cause != NULL ? event->cause->message
                                                       : "unknown error");
  if (m->pouch->logger != NULL)
    m->pouch->logger->warn(m->pouch->logger, "Agent message consumer failed",
                           fields, 2U);
  return LC_OK;
}
int cai_cli_messages_new(cai_cli_pouch *pouch, long timeout,
                         cai_cli_messages **out, cai_error *error) {
  cai_cli_messages *m;
  *out = NULL;
  if (timeout <= 0 || timeout > 86400000L)
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "message_ack_timeout_ms must be between 1 and 86400000");
  m = calloc(1U, sizeof(*m));
  if (m == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate messaging service");
  m->fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (m->fd < 0) {
    free(m);
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "create messaging notification descriptor");
  }
  if (pthread_mutex_init(&m->lock, NULL) != 0) {
    close(m->fd);
    free(m);
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "initialize messaging lock");
  }
  if (pthread_cond_init(&m->condition, NULL) != 0) {
    pthread_mutex_destroy(&m->lock);
    close(m->fd);
    free(m);
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "initialize messaging condition");
  }
  m->pouch = pouch;
  m->timeout_ms = timeout;
  m->dirty = 1;
  *out = m;
  return CAI_OK;
}
static int new_consumer(cai_cli_messages *m, const char *name,
                        lc_consumer_service **out, lc_error *error) {
  lc_consumer_config consumer;
  lc_consumer_service_config config;
  lc_consumer_config_init(&consumer);
  lc_consumer_service_config_init(&config);
  consumer.name = name;
  consumer.request.ns = MESSAGE_NS;
  consumer.request.queue = name;
  consumer.request.owner = m->instance;
  consumer.request.visibility_timeout_seconds = 30;
  consumer.handle = consume;
  consumer.on_error = service_error;
  consumer.context = m;
  config.consumers = &consumer;
  config.consumer_count = 1U;
  return lc_client_new_consumer_service(m->pouch->sessions, &config, out,
                                        error);
}
int cai_cli_messages_start(cai_cli_messages *m, const char *id,
                           const char *workspace, const char *name,
                           cai_error *error) {
  lc_error local;
  int rc;
  if (!safe_id(id))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "conversation ID cannot be used as a messaging owner");
  strcpy(m->session, id);
  if (realpath(workspace, m->workspace) == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "resolve messaging workspace directory");
  if (name != NULL && name[0] != '\0') {
    if (!queue_name(name, m->queue))
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "message queue name must be 1..128 ASCII letters, "
                           "digits, '.', '_' or '-'");
  } else if (workspace_queue(m->workspace, m->queue, error) != CAI_OK)
    return error->code;
  lc_error_init(&local);
  rc = lc_xid_new(m->instance, &local);
  if (rc != LC_OK)
    rc = fail_lc(error, &local);
  lc_error_cleanup(&local);
  if (rc != LC_OK)
    return rc;
  snprintf(m->receipts, sizeof(m->receipts), "cai.receipts.%s", m->instance);
  rc = mailbox_claim(m, m->queue, name == NULL || name[0] == '\0', error);
  if (rc != CAI_OK)
    return rc;
  lc_error_init(&local);
  rc = new_consumer(m, m->queue, &m->service, &local);
  if (rc == LC_OK)
    rc = lc_consumer_service_start(m->service, &local);
  if (rc == LC_OK) {
    rc = new_consumer(m, m->receipts, &m->receipts_service, &local);
  }
  if (rc == LC_OK)
    rc = lc_consumer_service_start(m->receipts_service, &local);
  if (rc != LC_OK)
    rc = fail_lc(error, &local);
  lc_error_cleanup(&local);
  return rc;
}
const char *cai_cli_messages_name(const cai_cli_messages *m) {
  return m != NULL ? m->queue : NULL;
}
/* Owner thread, between runtime turns: no send_tool call may be in flight.
 * Keep the inbox links, eventfd and private receipt listener in place. */
int cai_cli_messages_set_queue(cai_cli_messages *m, const char *name,
                               cai_error *error) {
  char normalized[129];
  lc_consumer_service *next = NULL;
  lc_error local;
  int rc, sending;
  if (!queue_name(name, normalized))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "Usage: /queue NAME (1..128 ASCII letters, digits, "
                         "'.', '_' or '-')");
  pthread_mutex_lock(&m->lock);
  sending = m->waiting[0] != '\0' || m->stopping;
  pthread_mutex_unlock(&m->lock);
  if (sending)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "Change the message queue between turns.");
  lc_error_init(&local);
  rc = new_consumer(m, normalized, &next, &local);
  if (rc != LC_OK)
    rc = fail_lc(error, &local);
  else
    rc = mailbox_claim(m, normalized, 0, error);
  if (rc != CAI_OK) {
    lc_consumer_service_close(next);
    lc_error_cleanup(&local);
    return rc;
  }
  (void)lc_consumer_service_stop(m->service);
  rc = lc_consumer_service_wait(m->service, &local);
  if (rc != LC_OK && m->pouch->logger != NULL) {
    pslog_field field = pslog_str(
        "detail", local.message != NULL ? local.message : "unknown error");
    m->pouch->logger->warn(m->pouch->logger,
                           "Previous mailbox listener stopped with an error",
                           &field, 1U);
  }
  lc_consumer_service_close(m->service);
  if (strcmp(m->queue, normalized) != 0)
    mailbox_release(m, m->queue);
  pthread_mutex_lock(&m->lock);
  strcpy(m->queue, normalized);
  m->lost = 0;
  pthread_mutex_unlock(&m->lock);
  m->service = next;
  lc_error_cleanup(&local);
  lc_error_init(&local);
  rc = lc_consumer_service_start(m->service, &local);
  if (rc != LC_OK) {
    char message[512];
    snprintf(message, sizeof(message),
             "Queue '%s' selected, but its listener could not start: %s. "
             "Use /queue NAME to retry.",
             m->queue, local.message != NULL ? local.message : "unknown error");
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, message);
  }
  lc_error_cleanup(&local);
  notify_owner(m);
  return rc;
}
int cai_cli_messages_fd(const cai_cli_messages *m) {
  return m != NULL ? m->fd : -1;
}

/* Query keys one bounded page at a time; load documents only after the query
 * callback has returned, avoiding nested operations under backend callbacks. */
typedef struct message_page {
  char keys[PAGE_KEYS][300];
  size_t count, used;
} message_page;
static int key_begin(void *context, lc_error *error) {
  message_page *page = context;
  (void)error;
  page->used = 0;
  return page->count < PAGE_KEYS;
}
static int key_chunk(void *context, const char *bytes, size_t count,
                     lc_error *error) {
  message_page *page = context;
  if (count >= sizeof(page->keys[0]) - page->used)
    return lc_error_set(error, LC_ERR_PROTOCOL, 0,
                        "Agent message key is too long", NULL, NULL,
                        NULL) == LC_OK;
  memcpy(page->keys[page->count] + page->used, bytes, count);
  page->used += count;
  page->keys[page->count][page->used] = '\0';
  return 1;
}
static int key_end(void *context, lc_error *error) {
  message_page *page = context;
  (void)error;
  page->count++;
  return 1;
}
static message_link *find_doc(cai_cli_messages *m, const char *id) {
  message_link *link;
  for (link = m->links; link != NULL; link = link->next)
    if (strcmp(link->doc.id, id) == 0)
      return link;
  return NULL;
}
static int load_doc(cai_cli_messages *m, const char *key, message_doc *doc,
                    cai_error *error) {
  lc_error local;
  lc_lease *lease = NULL;
  lc_get_res result;
  int rc;
  memset(doc, 0, sizeof(*doc));
  memset(&result, 0, sizeof(result));
  lc_error_init(&local);
  rc = lease_open(m, key, &lease, &local);
  if (rc == LC_OK) {
    rc = lc_lease_load(lease, &message_map, doc, NULL, &result, &local);
    rc = lease_finish(lease, rc, &local);
  }
  lc_get_res_cleanup(&result);
  if (rc != LC_OK)
    rc = fail_lc(error, &local);
  lc_error_cleanup(&local);
  return rc;
}
static int list_mailboxes(cai_cli_messages *m, cai_sink *output,
                          cai_error *error) {
  lc_query_req query;
  lc_query_res result;
  lc_query_key_handler handler;
  lc_error local;
  message_page page;
  message_doc *doc;
  char *cursor = NULL;
  size_t i;
  int rc = CAI_OK;
  doc = calloc(1U, sizeof(*doc));
  if (doc == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate mailbox listing");
  lc_error_init(&local);
  {
    static const char heading[] = "This application's mailbox: ";
    static const char columns[] =
        "\nKnown destinations (queue, directory, last-bound conversation; not "
        "an online guarantee):\n";
    rc = cai_sink_write(output, heading, sizeof(heading) - 1U, error);
    if (rc == CAI_OK)
      rc = cai_sink_write(output, m->queue, strlen(m->queue), error);
    if (rc == CAI_OK)
      rc = cai_sink_write(output, columns, sizeof(columns) - 1U, error);
    if (rc != CAI_OK) {
      free(doc);
      lc_error_cleanup(&local);
      return rc;
    }
  }
  lc_query_req_init(&query);
  memset(&handler, 0, sizeof(handler));
  handler.begin = key_begin;
  handler.chunk = key_chunk;
  handler.end = key_end;
  query.ns = MESSAGE_NS;
  query.engine = "index";
  query.refresh = "wait_for";
  query.selector_lql = "eq{field=/kind,value=\"mailbox\"}";
  query.limit = PAGE_KEYS;
  do {
    memset(&page, 0, sizeof(page));
    memset(&result, 0, sizeof(result));
    query.cursor = cursor;
    if (lc_query_keys(m->pouch->sessions, &query, &handler, &page, &result,
                      &local) != LC_OK)
      rc = fail_lc(error, &local);
    free(cursor);
    cursor = result.cursor != NULL ? strdup(result.cursor) : NULL;
    if (result.cursor != NULL && cursor == NULL)
      rc = cai_cli_error(error, CAI_ERR_NOMEM, "copy mailbox listing cursor");
    lc_query_res_cleanup(&result);
    for (i = 0U; rc == CAI_OK && i < page.count; i++) {
      rc = load_doc(m, page.keys[i], doc, error);
      if (rc == CAI_OK) {
        const char *parts[6];
        size_t part;
        parts[0] = doc->recipient;
        parts[1] = "  ";
        parts[2] = doc->workspace;
        parts[3] = "  ";
        parts[4] = doc->sender_session;
        parts[5] = "\n";
        for (part = 0U; rc == CAI_OK && part < 6U; part++)
          rc = cai_sink_write(output, parts[part], strlen(parts[part]), error);
      }
      lonejson_cleanup(&message_map, doc);
    }
  } while (rc == CAI_OK && cursor != NULL);
  free(cursor);
  free(doc);
  lc_error_cleanup(&local);
  return rc;
}
int cai_cli_messages_is_message(const cai_cli_messages *m, sl_prompt_id_t id) {
  message_link *link;
  for (link = m != NULL ? m->links : NULL; link != NULL; link = link->next)
    if (link->prompt_id == id)
      return 1;
  return 0;
}
void cai_cli_messages_rebind(cai_cli_messages *m, sl_prompt_id_t old_id,
                             sl_prompt_id_t new_id) {
  message_link *link;
  for (link = m != NULL ? m->links : NULL; link != NULL; link = link->next)
    if (link->prompt_id == old_id)
      link->prompt_id = new_id;
}
void cai_cli_messages_detach(cai_cli_messages *m, sl_t *sl) {
  sl_prompt_queue_entry_info_t info;
  size_t i = 0U;
  while (i < sl_prompt_queue_count(sl)) {
    if (sl_prompt_queue_info(sl, i, &info) != SL_OK)
      break;
    if (cai_cli_messages_is_message(m, info.id) &&
        sl_prompt_queue_reject(sl, info.id) == SL_OK)
      continue;
    i++;
  }
}
static int finish_message(cai_cli_messages *m, sl_prompt_id_t id,
                          const char *kind, cai_error *error) {
  message_link **link, *found;
  message_doc tombstone;
  if (m == NULL)
    return CAI_OK;
  for (link = &m->links; *link != NULL; link = &(*link)->next) {
    if ((*link)->prompt_id != id)
      continue;
    found = *link;
    tombstone = found->doc;
    strcpy(tombstone.kind, kind);
    tombstone.pending_for[0] = '\0';
    tombstone.text = NULL;
    if (save_doc(m, &tombstone, error) != CAI_OK)
      return error->code;
    *link = found->next;
    lonejson_cleanup(&message_map, &found->doc);
    free(found);
    return CAI_OK;
  }
  return CAI_OK;
}
int cai_cli_messages_consumed(cai_cli_messages *m, sl_prompt_id_t id,
                              cai_error *error) {
  return finish_message(m, id, "consumed", error);
}
int cai_cli_messages_sync(cai_cli_messages *m, sl_t *sl, cai_error *error) {
  lc_query_req query;
  lc_query_res result;
  lc_query_key_handler handler;
  lc_error local;
  message_page page;
  message_doc doc;
  message_link *link;
  sl_prompt_queue_entry_info_t info, draft;
  char *cursor = NULL, *text, selector[256];
  size_t i, j, capacity;
  uint64_t notification;
  int dirty, present, rc = CAI_OK;
  if (m == NULL || sl == NULL)
    return CAI_OK;
  while (read(m->fd, &notification, sizeof(notification)) > 0) {
  }
  /* A native rejection removes the ID; editing retains it in the draft. */
  if (sl_prompt_queue_draft_info(sl, &draft) != SL_OK)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "inspect queued message edit");
  link = m->links;
  while (link != NULL) {
    sl_prompt_id_t id = link->prompt_id;
    message_link *next = link->next;
    present = draft.id == id;
    for (j = 0U; !present && j < sl_prompt_queue_count(sl); j++)
      present = sl_prompt_queue_info(sl, j, &info) == SL_OK && info.id == id;
    if (!present && sl_last_prompt_id(sl) != id) {
      if (finish_message(m, id, "rejected", error) != CAI_OK)
        return error->code;
    }
    link = next;
  }
  if (sl_prompt_queue_count(sl) + (draft.id != 0 ? 1U : 0U) >=
      sl_prompt_queue_capacity(sl))
    return CAI_OK;
  pthread_mutex_lock(&m->lock);
  dirty = m->dirty;
  m->dirty = 0;
  pthread_mutex_unlock(&m->lock);
  if (!dirty)
    return CAI_OK;
  lc_query_req_init(&query);
  lc_error_init(&local);
  memset(&handler, 0, sizeof(handler));
  handler.begin = key_begin;
  handler.chunk = key_chunk;
  handler.end = key_end;
  query.ns = MESSAGE_NS;
  query.engine = "index";
  query.refresh = "wait_for";
  /* Session IDs are restricted to safe_id's alphabet, not arbitrary LQL. */
  snprintf(selector, sizeof(selector), "eq{field=/pending_for,value=\"%s\"}",
           m->session);
  query.selector_lql = selector;
  query.limit = PAGE_KEYS;
  do {
    memset(&page, 0, sizeof(page));
    memset(&result, 0, sizeof(result));
    query.cursor = cursor;
    if (lc_query_keys(m->pouch->sessions, &query, &handler, &page, &result,
                      &local) != LC_OK) {
      rc = fail_lc(error, &local);
      lc_query_res_cleanup(&result);
      break;
    }
    free(cursor);
    cursor = result.cursor != NULL ? strdup(result.cursor) : NULL;
    if (result.cursor != NULL && cursor == NULL)
      rc = cai_cli_error(error, CAI_ERR_NOMEM, "copy message query cursor");
    lc_query_res_cleanup(&result);
    for (i = 0U; rc == CAI_OK && i < page.count; i++) {
      if (sl_prompt_queue_count(sl) + (draft.id != 0 ? 1U : 0U) >=
          sl_prompt_queue_capacity(sl)) {
        pthread_mutex_lock(&m->lock);
        m->dirty = 1;
        pthread_mutex_unlock(&m->lock);
        break;
      }
      rc = load_doc(m, page.keys[i], &doc, error);
      if (rc != CAI_OK) {
        lonejson_cleanup(&message_map, &doc);
        break;
      }
      if (strcmp(doc.kind, "pending") != 0 ||
          strcmp(doc.pending_for, m->session) != 0 ||
          find_doc(m, doc.id) != NULL) {
        lonejson_cleanup(&message_map, &doc);
        continue;
      }
      capacity = strlen(doc.text != NULL ? doc.text : "") + 384U;
      text = malloc(capacity);
      link = calloc(1U, sizeof(*link));
      if (text == NULL || link == NULL) {
        free(text);
        free(link);
        lonejson_cleanup(&message_map, &doc);
        rc =
            cai_cli_error(error, CAI_ERR_NOMEM, "queue incoming agent message");
        break;
      }
      snprintf(text, capacity,
               strcmp(doc.sender_session, "cli") == 0
                   ? "[CLI message %s from %s (%s)]\n\n%s"
                   : "[Agent message %s from %s (conversation %s)]\n\n%s",
               doc.id, doc.sender, doc.sender_session,
               doc.text != NULL ? doc.text : "");
      if (sl_inject_queued_prompt(sl, text, &link->prompt_id) != SL_OK) {
        free(text);
        free(link);
        lonejson_cleanup(&message_map, &doc);
        rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "inject incoming queued message");
        break;
      }
      free(text);
      link->doc = doc;
      link->next = m->links;
      m->links = link;
    }
  } while (rc == CAI_OK && cursor != NULL && i == page.count);
  free(cursor);
  lc_error_cleanup(&local);
  return rc;
}
static int send_cancelled(cai_cli_messages *m, unsigned long long sequence) {
  int cancelled;
  pthread_mutex_lock(&m->lock);
  cancelled = m->stopping || m->cancel_sequence != sequence;
  pthread_mutex_unlock(&m->lock);
  return cancelled;
}
static int send_tool(void *context, const char *arguments, cai_sink *output,
                     cai_error *error) {
  cai_cli_messages *m = context;
  send_args args;
  message_doc doc;
  lonejson *json;
  lonejson_error parse_error;
  lc_error local;
  lc_lease *binding = NULL;
  struct timespec deadline;
  char response[256];
  unsigned long long sequence;
  int rc, wait_result = 0;
  /* Snapshot before parsing or acquiring leases. Cancellation advances this
   * sequence even during startup; later sends take their own fresh snapshot. */
  pthread_mutex_lock(&m->lock);
  sequence = m->cancel_sequence;
  pthread_mutex_unlock(&m->lock);
  memset(&args, 0, sizeof(args));
  memset(&doc, 0, sizeof(doc));
  lc_error_init(&local);
  json = lonejson_new(NULL, &parse_error);
  if (json == NULL) {
    rc = cai_cli_lj_error(error, &parse_error);
    goto done;
  }
  if (lonejson_parse_buffer(json, &arguments_map, &args, arguments,
                            strlen(arguments),
                            &parse_error) != LONEJSON_STATUS_OK) {
    rc = cai_cli_lj_error(error, &parse_error);
    goto done;
  }
  if (strcmp(args.operation, "list") == 0) {
    rc = list_mailboxes(m, output, error);
    goto done;
  }
  if (strcmp(args.operation, "send") != 0 ||
      !queue_name(args.recipient, doc.recipient) || !valid_text(args.text)) {
    rc = cai_cli_error(
        error, CAI_ERR_INVALID,
        "agent_message requires operation=send, a recipient "
        "queue name, and 1..65536 text bytes (no terminal controls)");
    goto done;
  }
  if (send_cancelled(m, sequence)) {
    rc = cai_cli_error(error, CAI_ERR_CANCELLED,
                       "agent message delivery cancelled");
    goto done;
  }
  rc = mailbox_guard(m, &binding, &local);
  if (send_cancelled(m, sequence)) {
    rc = cai_cli_error(error, CAI_ERR_CANCELLED,
                       "agent message delivery cancelled");
    goto done;
  }
  if (rc == LC_OK)
    rc = lc_xid_new(doc.id, &local);
  if (rc != LC_OK) {
    rc = fail_lc(error, &local);
    goto done;
  }
  strcpy(doc.kind, "message");
  strcpy(doc.sender, m->queue);
  strcpy(doc.sender_session, m->session);
  strcpy(doc.instance, m->instance);
  doc.text = args.text;
  doc.deadline_ms = realtime_ms() + m->timeout_ms;
  deadline.tv_sec = (time_t)(doc.deadline_ms / 1000);
  deadline.tv_nsec = (long)(doc.deadline_ms % 1000) * 1000000L;
  pthread_mutex_lock(&m->lock);
  if (m->stopping || m->cancel_sequence != sequence) {
    pthread_mutex_unlock(&m->lock);
    rc = cai_cli_error(error, CAI_ERR_CANCELLED,
                       "agent message delivery cancelled");
    goto done;
  }
  if (m->waiting[0] != '\0') {
    pthread_mutex_unlock(&m->lock);
    rc = cai_cli_error(error, CAI_ERR_INVALID,
                       "message sender is already waiting for a delivery");
    goto done;
  }
  strcpy(m->waiting, doc.id);
  strcpy(m->waiting_recipient, doc.recipient);
  m->receipt = 0;
  pthread_mutex_unlock(&m->lock);
  rc = enqueue(m->pouch, &doc, error);
  if (binding != NULL) {
    int released = lease_finish(binding, LC_OK, &local);
    binding = NULL;
    if (released != LC_OK && rc == CAI_OK)
      rc = fail_lc(error, &local);
  }
  pthread_mutex_lock(&m->lock);
  while (rc == CAI_OK && !m->receipt && !m->stopping &&
         m->cancel_sequence == sequence && wait_result == 0)
    wait_result = pthread_cond_timedwait(&m->condition, &m->lock, &deadline);
  if (rc == CAI_OK && !m->receipt)
    rc = cai_cli_error(error,
                       m->cancel_sequence != sequence || m->stopping
                           ? CAI_ERR_CANCELLED
                           : CAI_ERR_TRANSPORT,
                       m->stopping || m->cancel_sequence != sequence
                           ? "agent message delivery cancelled"
                           : "agent message was not acknowledged "
                             "before the delivery deadline");
  m->waiting[0] = '\0';
  pthread_mutex_unlock(&m->lock);
  if (rc == CAI_OK) {
    snprintf(
        response, sizeof(response),
        "{\"message_id\":\"%s\",\"status\":\"delivered\",\"recipient\":\"%s\"}",
        doc.id, doc.recipient);
    rc = cai_sink_write(output, response, strlen(response), error);
  }
done:
  if (binding != NULL)
    (void)lease_finish(binding, LC_OK, &local);
  lc_error_cleanup(&local);
  lonejson_cleanup(&arguments_map, &args);
  lonejson_free(json);
  return rc;
}
int cai_cli_messages_send_once(cai_cli_pouch *pouch, const char *recipient,
                               const char *text, cai_sink *result,
                               cai_error *error) {
  message_doc doc;
  lc_error local;
  char response[256];
  int rc;
  memset(&doc, 0, sizeof(doc));
  if (recipient == NULL || text == NULL ||
      !queue_name(recipient, doc.recipient) || !valid_text(text))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "--message requires a valid queue name and 1..65536 "
                         "text bytes (no terminal controls)");
  lc_error_init(&local);
  rc = lc_xid_new(doc.id, &local);
  if (rc == LC_OK) {
    strcpy(doc.kind, "message");
    strcpy(doc.sender_session, "cli");
    snprintf(doc.sender, sizeof(doc.sender), "cai.cli.%s", doc.id);
    /* No return address or delivery deadline: the receiver may be offline. */
    doc.text = strdup(text);
    rc = doc.text != NULL
             ? enqueue(pouch, &doc, error)
             : cai_cli_error(error, CAI_ERR_NOMEM, "copy standalone message");
  } else {
    rc = fail_lc(error, &local);
  }
  if (rc == CAI_OK) {
    snprintf(
        response, sizeof(response),
        "{\"message_id\":\"%s\",\"status\":\"queued\",\"recipient\":\"%s\"}",
        doc.id, doc.recipient);
    rc = cai_sink_write(result, response, strlen(response), error);
  }
  free(doc.text);
  lc_error_cleanup(&local);
  return rc;
}

int cai_cli_messages_register(void *context, cai_agent *agent,
                              cai_error *error) {
  return cai_agent_register_raw_tool(
      agent, "agent_message",
      "Use operation=list to discover named workspace mailboxes on this "
      "backend (queue name, directory, last-bound conversation; a listing is "
      "not an online guarantee). Use operation=send with recipient queue name "
      "and text to "
      "send a message. Waits for durable delivery acknowledgement, not human "
      "approval or completion; a missing ACK fails after the configured "
      "deadline (default five minutes). Messages become ordinary next-turn "
      "input after approval. Never use this to send steering instructions.",
      "{\"type\":\"object\",\"properties\":{\"operation\":{\"type\":\"string\","
      "\"enum\":[\"send\",\"list\"]},\"recipient\":{\"type\":\"string\"},"
      "\"text\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":65536}},"
      "\"required\":[\"operation\"],\"additionalProperties\":false}",
      0, send_tool, context, error);
}
void cai_cli_messages_stop(cai_cli_messages *m) {
  if (m == NULL)
    return;
  pthread_mutex_lock(&m->lock);
  m->stopping = 1;
  pthread_cond_broadcast(&m->condition);
  pthread_mutex_unlock(&m->lock);
  if (m->service != NULL) {
    lc_error local;
    lc_error_init(&local);
    (void)lc_consumer_service_stop(m->service);
    (void)lc_consumer_service_wait(m->service, &local);
    lc_error_cleanup(&local);
  }
  if (m->receipts_service != NULL) {
    lc_error local;
    lc_error_init(&local);
    (void)lc_consumer_service_stop(m->receipts_service);
    (void)lc_consumer_service_wait(m->receipts_service, &local);
    lc_error_cleanup(&local);
  }
  mailbox_release(m, m->queue);
  m->claimed = 0;
}
void cai_cli_messages_cancel_send(cai_cli_messages *m) {
  if (m == NULL)
    return;
  pthread_mutex_lock(&m->lock);
  m->cancel_sequence++;
  pthread_cond_broadcast(&m->condition);
  pthread_mutex_unlock(&m->lock);
}
void cai_cli_messages_close(cai_cli_messages *m) {
  message_link *link, *next;
  if (m == NULL)
    return;
  cai_cli_messages_stop(m);
  lc_consumer_service_close(m->service);
  lc_consumer_service_close(m->receipts_service);
  for (link = m->links; link != NULL; link = next) {
    next = link->next;
    lonejson_cleanup(&message_map, &link->doc);
    free(link);
  }
  close(m->fd);
  pthread_cond_destroy(&m->condition);
  pthread_mutex_destroy(&m->lock);
  free(m);
}
