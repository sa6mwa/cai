#define _POSIX_C_SOURCE 200809L
#include "messages.h"
#include "log.h"
#include "session_commands.h"
#include <errno.h>
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
  lc_consumer_service *service;
  pthread_mutex_t lock;
  pthread_cond_t condition;
  int fd, stopping, dirty, receipt, cancelled;
  long timeout_ms;
  char session[129], queue[160], waiting[LC_XID_STRING_SIZE],
      waiting_recipient[129];
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
  req.owner = m->queue;
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
static int enqueue(cai_cli_messages *m, const message_doc *doc,
                   cai_error *error) {
  cai_source *source = NULL;
  lc_source *wire = NULL;
  lc_enqueue_req req;
  lc_enqueue_res res;
  lc_error local;
  char queue[160];
  int rc;
  snprintf(queue, sizeof(queue), "cai.session.%s", doc->recipient);
  lc_error_init(&local);
  lc_enqueue_req_init(&req);
  memset(&res, 0, sizeof(res));
  req.ns = MESSAGE_NS;
  req.queue = queue;
  req.content_type = "application/json";
  req.visibility_timeout_seconds = 30;
  req.max_attempts = 3;
  req.ttl_seconds =
      strcmp(doc->kind, "message") == 0
          ? (long)((doc->deadline_ms - realtime_ms() + 999) / 1000)
          : 300;
  if (req.ttl_seconds <= 0)
    req.ttl_seconds = 1;
  rc = cai_cli_mapped_source(&message_map, doc, &source, error);
  if (rc == CAI_OK)
    rc = cai_cli_lc_source(source, &wire, error);
  if (rc == CAI_OK &&
      lc_enqueue(m->pouch->sessions, &req, wire, &res, &local) != LC_OK)
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
  lonejson *json;
  lonejson_error parse_error;
  cai_error local;
  char key[300];
  uint64_t one = 1;
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
      !lc_xid_is_valid(doc.id) || !safe_id(doc.sender) ||
      strcmp(doc.recipient, m->session) != 0) {
    rc = lc_error_set(error, LC_ERR_PROTOCOL, 0,
                      "Invalid agent message envelope", NULL, NULL, NULL);
    goto done;
  }
  if (strcmp(doc.kind, "receipt") == 0) {
    pthread_mutex_lock(&m->lock);
    if (strcmp(m->waiting, doc.id) == 0 &&
        strcmp(m->waiting_recipient, doc.sender) == 0) {
      m->receipt = 1;
      pthread_cond_broadcast(&m->condition);
    }
    pthread_mutex_unlock(&m->lock);
    goto done;
  }
  if (strcmp(doc.kind, "message") != 0 || !valid_text(doc.text) ||
      doc.deadline_ms <= 0) {
    rc = lc_error_set(error, LC_ERR_PROTOCOL, 0,
                      "Invalid agent message content", NULL, NULL, NULL);
    goto done;
  }
  inbox_key(key, &doc);
  rc = lease_open(m, key, &lease, error);
  if (rc == LC_OK) {
    fresh = lease->state_etag == NULL || lease->state_etag[0] == '\0';
    if (fresh && doc.deadline_ms > realtime_ms()) {
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
  memset(&receipt, 0, sizeof(receipt));
  strcpy(receipt.kind, "receipt");
  strcpy(receipt.id, doc.id);
  strcpy(receipt.sender, m->session);
  strcpy(receipt.recipient, doc.sender);
  /* Durable inbox acceptance is delivery. Approval and execution are separate.
   */
  rc = enqueue(m, &receipt, &local);
  if (rc != CAI_OK) {
    rc = lc_error_set(error, LC_ERR_TRANSPORT, 0,
                      "Cannot acknowledge agent delivery", local.message, NULL,
                      NULL);
    goto done;
  }
  pthread_mutex_lock(&m->lock);
  m->dirty = 1;
  pthread_mutex_unlock(&m->lock);
  if (write(m->fd, &one, sizeof(one)) < 0 && errno != EAGAIN)
    rc = lc_error_set(error, LC_ERR_TRANSPORT, 0,
                      "Cannot notify agent message owner", strerror(errno),
                      NULL, NULL);
  if (fresh && m->pouch->logger != NULL) {
    pslog_field fields[2];
    fields[0] = pslog_str("sender_session_id", doc.sender);
    fields[1] = pslog_str("message_id", doc.id);
    cai_cli_log_message(m->pouch->logger, PSLOG_LEVEL_INFO,
                        "agent_message_received", "agent", doc.text,
                        strlen(doc.text), fields, 2U);
  }
done:
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
  (void)error;
  fields[0] = pslog_str("queue", m->queue);
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
int cai_cli_messages_start(cai_cli_messages *m, const char *id,
                           const char *workspace, cai_error *error) {
  lc_consumer_config consumer;
  lc_consumer_service_config config;
  lc_error local;
  int rc;
  (void)workspace;
  if (!safe_id(id))
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "conversation ID cannot be used as a messaging address");
  strcpy(m->session, id);
  snprintf(m->queue, sizeof(m->queue), "cai.session.%s", id);
  lc_consumer_config_init(&consumer);
  lc_consumer_service_config_init(&config);
  consumer.name = m->queue;
  consumer.request.ns = MESSAGE_NS;
  consumer.request.queue = m->queue;
  consumer.request.owner = m->queue;
  consumer.request.visibility_timeout_seconds = 30;
  consumer.handle = consume;
  consumer.on_error = service_error;
  consumer.context = m;
  config.consumers = &consumer;
  config.consumer_count = 1U;
  lc_error_init(&local);
  rc = lc_client_new_consumer_service(m->pouch->sessions, &config, &m->service,
                                      &local);
  if (rc == LC_OK)
    rc = lc_consumer_service_start(m->service, &local);
  if (rc != LC_OK)
    rc = fail_lc(error, &local);
  lc_error_cleanup(&local);
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
          strcmp(doc.recipient, m->session) != 0 ||
          find_doc(m, doc.id) != NULL) {
        lonejson_cleanup(&message_map, &doc);
        continue;
      }
      capacity = strlen(doc.text != NULL ? doc.text : "") + 256U;
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
      snprintf(text, capacity, "[Agent message %s from conversation %s]\n\n%s",
               doc.id, doc.sender, doc.text != NULL ? doc.text : "");
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
static int send_tool(void *context, const char *arguments, cai_sink *output,
                     cai_error *error) {
  cai_cli_messages *m = context;
  send_args args;
  message_doc doc;
  lonejson *json;
  lonejson_error parse_error;
  lc_error local;
  struct timespec deadline;
  char response[256];
  int rc, wait_result = 0;
  memset(&args, 0, sizeof(args));
  memset(&doc, 0, sizeof(doc));
  json = lonejson_new(NULL, &parse_error);
  if (json == NULL)
    return cai_cli_lj_error(error, &parse_error);
  if (lonejson_parse_buffer(json, &arguments_map, &args, arguments,
                            strlen(arguments),
                            &parse_error) != LONEJSON_STATUS_OK) {
    rc = cai_cli_lj_error(error, &parse_error);
    goto done;
  }
  if (strcmp(args.operation, "list") == 0) {
    cai_cli_session *sessions = NULL;
    size_t count = 0U;
    rc = cai_cli_pouch_list(m->pouch, NULL, &sessions, &count, error);
    if (rc == CAI_OK)
      rc = cai_cli_sessions_print(output, sessions, count, 0, 120, error);
    free(sessions);
    goto done;
  }
  if (strcmp(args.operation, "send") != 0 || !safe_id(args.recipient) ||
      !valid_text(args.text)) {
    rc = cai_cli_error(
        error, CAI_ERR_INVALID,
        "agent_message requires operation=send, a recipient "
        "conversation ID, and 1..65536 text bytes (no terminal controls)");
    goto done;
  }
  lc_error_init(&local);
  rc = lc_xid_new(doc.id, &local);
  if (rc != LC_OK) {
    rc = fail_lc(error, &local);
    lc_error_cleanup(&local);
    goto done;
  }
  lc_error_cleanup(&local);
  strcpy(doc.kind, "message");
  strcpy(doc.sender, m->session);
  strcpy(doc.recipient, args.recipient);
  doc.text = args.text;
  doc.deadline_ms = realtime_ms() + m->timeout_ms;
  deadline.tv_sec = (time_t)(doc.deadline_ms / 1000);
  deadline.tv_nsec = (long)(doc.deadline_ms % 1000) * 1000000L;
  pthread_mutex_lock(&m->lock);
  if (m->stopping || m->waiting[0] != '\0') {
    pthread_mutex_unlock(&m->lock);
    rc = cai_cli_error(
        error, CAI_ERR_INVALID,
        "message sender is stopping or already waiting for a delivery");
    goto done;
  }
  strcpy(m->waiting, doc.id);
  strcpy(m->waiting_recipient, doc.recipient);
  m->receipt = 0;
  m->cancelled = 0;
  pthread_mutex_unlock(&m->lock);
  rc = enqueue(m, &doc, error);
  pthread_mutex_lock(&m->lock);
  while (rc == CAI_OK && !m->receipt && !m->stopping && !m->cancelled &&
         wait_result == 0)
    wait_result = pthread_cond_timedwait(&m->condition, &m->lock, &deadline);
  if (rc == CAI_OK && !m->receipt)
    rc = cai_cli_error(
        error,
        m->cancelled || m->stopping ? CAI_ERR_CANCELLED : CAI_ERR_TRANSPORT,
        m->stopping || m->cancelled ? "agent message delivery cancelled"
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
  lonejson_cleanup(&arguments_map, &args);
  lonejson_free(json);
  return rc;
}
int cai_cli_messages_register(void *context, cai_agent *agent,
                              cai_error *error) {
  return cai_agent_register_raw_tool(
      agent, "agent_message",
      "Use operation=list to discover stored CAI conversations on this "
      "backend. Use operation=send with recipient conversation ID and text to "
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
}
void cai_cli_messages_cancel_send(cai_cli_messages *m) {
  if (m == NULL)
    return;
  pthread_mutex_lock(&m->lock);
  if (m->waiting[0] != '\0') {
    m->cancelled = 1;
    pthread_cond_broadcast(&m->condition);
  }
  pthread_mutex_unlock(&m->lock);
}
void cai_cli_messages_close(cai_cli_messages *m) {
  message_link *link, *next;
  if (m == NULL)
    return;
  cai_cli_messages_stop(m);
  lc_consumer_service_close(m->service);
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
