#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "pouch.h"
#include <cai/auth.h>

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const lonejson_field session_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(cai_cli_session, id, "id",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(cai_cli_session, workspace, "workspace",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(cai_cli_session, first_prompt, "first_prompt",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(cai_cli_session, checkpoint_name, "checkpoint",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_U64(cai_cli_session, checkpoint_ns, "checkpoint_ns"),
    LONEJSON_FIELD_U64(cai_cli_session, applied_sequence, "applied_sequence"),
    LONEJSON_FIELD_I64(cai_cli_session, published, "published")};
LONEJSON_MAP_DEFINE(session_map, cai_cli_session, session_fields);

typedef struct scope_predicate {
  char field[24];
  char value[PATH_MAX];
} scope_predicate;
static const lonejson_field scope_predicate_fields[] = {
    LONEJSON_FIELD_STRING_FIXED(scope_predicate, field, "field",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED(scope_predicate, value, "value",
                                LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(scope_predicate_map, scope_predicate,
                    scope_predicate_fields);
typedef struct scope_query {
  scope_predicate eq;
} scope_query;
static const lonejson_field scope_fields[] = {
    LONEJSON_FIELD_OBJECT(scope_query, eq, "eq", &scope_predicate_map)};
LONEJSON_MAP_DEFINE(scope_map, scope_query, scope_fields);

typedef struct cli_record {
  char record_type[24];
  lonejson_uint64 sequence;
  lonejson_uint64 applied_event_sequence;
  lonejson_uint64 checkpoint_created_at_ns;
  const char *type;
  const char *data;
  lonejson_json_value state;
} cli_record;
static const lonejson_field event_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(cli_record, record_type, "record_type",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_U64_REQ(cli_record, sequence, "sequence"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cli_record, type, "type"),
    LONEJSON_FIELD_STRING_ALLOC(cli_record, data, "data")};
LONEJSON_MAP_DEFINE(event_map, cli_record, event_fields);
static const lonejson_field checkpoint_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(cli_record, record_type, "record_type",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_U64_REQ(cli_record, checkpoint_created_at_ns,
                           "checkpoint_created_at_ns"),
    LONEJSON_FIELD_U64_REQ(cli_record, applied_event_sequence,
                           "applied_event_sequence"),
    LONEJSON_FIELD_JSON_VALUE_REQ(cli_record, state, "state")};
LONEJSON_MAP_DEFINE(checkpoint_map, cli_record, checkpoint_fields);

static const lonejson_field import_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(cli_record, record_type, "record_type",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_U64(cli_record, sequence, "sequence"),
    LONEJSON_FIELD_U64(cli_record, checkpoint_created_at_ns,
                       "checkpoint_created_at_ns"),
    LONEJSON_FIELD_U64(cli_record, applied_event_sequence,
                       "applied_event_sequence"),
    LONEJSON_FIELD_STRING_ALLOC(cli_record, type, "type"),
    LONEJSON_FIELD_STRING_ALLOC(cli_record, data, "data"),
    LONEJSON_FIELD_JSON_VALUE(cli_record, state, "state")};
LONEJSON_MAP_DEFINE(import_map, cli_record, import_fields);

typedef struct json_source {
  lonejson *json;
  lonejson_generator generator;
} json_source;

static size_t json_read(void *context, void *bytes, size_t count,
                        cai_error *error) {
  json_source *source;
  size_t n;
  int eof;
  source = (json_source *)context;
  n = 0U;
  if (lonejson_generator_read(&source->generator, bytes, count, &n, &eof) !=
      LONEJSON_STATUS_OK)
    cai_cli_lj_error(error, &source->generator.error);
  return n;
}

static void json_close(void *context) {
  json_source *source;
  source = (json_source *)context;
  lonejson_generator_cleanup(&source->generator);
  lonejson_free(source->json);
  free(source);
}

static int mapped_source(const lonejson_map *map, const void *doc,
                         cai_source **out, cai_error *error) {
  json_source *source;
  cai_source_callbacks callbacks;
  lonejson_error local;
  int rc;
  source = (json_source *)calloc(1U, sizeof(*source));
  if (source == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate JSON generator");
  source->json = lonejson_new(NULL, &local);
  if (source->json == NULL) {
    free(source);
    return cai_cli_lj_error(error, &local);
  }
  if (lonejson_generator_init(source->json, &source->generator, map, doc) !=
      LONEJSON_STATUS_OK) {
    rc = cai_cli_lj_error(error, &source->generator.error);
    lonejson_free(source->json);
    free(source);
    return rc;
  }
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.read = json_read;
  callbacks.close = json_close;
  callbacks.context = source;
  rc = cai_source_from_callbacks(&callbacks, out, error);
  if (rc != CAI_OK)
    json_close(source);
  return rc;
}

typedef struct download {
  lc_client *client;
  char *key;
  char *attachment;
} download;

static void download_close(void *context) {
  download *request;
  request = (download *)context;
  free(request->key);
  free(request->attachment);
  free(request);
}

static int download_write(void *context, cai_sink *sink, cai_error *error) {
  download *request;
  lc_sink *destination;
  lc_error local;
  lc_get_opts options;
  lc_get_res response;
  lc_attachment_get_op attachment;
  lc_attachment_get_res attached;
  int rc;
  request = (download *)context;
  destination = NULL;
  lc_error_init(&local);
  rc = cai_cli_lc_sink(sink, &destination, error);
  if (rc == CAI_OK) {
    if (request->attachment != NULL) {
      lc_attachment_get_op_init(&attachment);
      memset(&attached, 0, sizeof(attached));
      attachment.lease.ns = request->client->default_namespace;
      attachment.lease.key = request->key;
      attachment.selector.name = request->attachment;
      attachment.public_read = 1;
      rc = lc_get_attachment(request->client, &attachment, destination,
                             &attached, &local);
      lc_attachment_get_res_cleanup(&attached);
    } else {
      memset(&response, 0, sizeof(response));
      memset(&options, 0, sizeof(options));
      options.public_read = 1;
      rc = request->client->get(request->client, request->key, &options,
                                destination, &response, &local);
      lc_get_res_cleanup(&response);
    }
    if (rc != LC_OK)
      rc = cai_cli_lc_error(error, &local);
  }
  lc_sink_close(destination);
  lc_error_cleanup(&local);
  return rc;
}

static int download_source(lc_client *client, const char *key,
                           const char *attachment, cai_source **out,
                           cai_error *error) {
  download *request;
  int rc;
  request = (download *)calloc(1U, sizeof(*request));
  if (request == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate pouch download");
  request->client = client;
  request->key = strdup(key);
  request->attachment = attachment != NULL ? strdup(attachment) : NULL;
  if (request->key == NULL ||
      (attachment != NULL && request->attachment == NULL)) {
    download_close(request);
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate pouch download key");
  }
  rc = cai_cli_stream_source(download_write, request, download_close, out,
                             error);
  if (rc != CAI_OK)
    download_close(request);
  return rc;
}

typedef struct parse_input {
  cai_source *source;
  cai_error error;
} parse_input;

static lonejson_read_result parse_read(void *context, unsigned char *bytes,
                                       size_t capacity) {
  parse_input *input;
  lonejson_read_result result;
  input = (parse_input *)context;
  result = lonejson_default_read_result();
  result.bytes_read =
      cai_source_read(input->source, bytes, capacity, &input->error);
  result.eof = result.bytes_read == 0U && input->error.code == CAI_OK;
  result.error_code = input->error.code == CAI_OK ? 0 : EIO;
  return result;
}

static int parse_source(cai_source *source, const lonejson_map *map, void *doc,
                        cai_error *error) {
  lonejson *json;
  lonejson_error local;
  int rc;
  char message[512];
  parse_input input;
  json = lonejson_new(NULL, &local);
  if (json == NULL)
    return cai_cli_lj_error(error, &local);
  input.source = source;
  cai_error_init(&input.error);
  rc = lonejson_parse_reader(json, map, doc, parse_read, &input, &local) ==
               LONEJSON_STATUS_OK
           ? CAI_OK
           : cai_cli_lj_error(error, &local);
  if (rc != CAI_OK) {
    snprintf(message, sizeof(message), "parse stored %s: %s", map->name,
             input.error.message != NULL ? input.error.message : local.message);
    rc = cai_cli_error(error, rc, message);
    error->http_status = input.error.http_status;
  }
  cai_error_cleanup(&input.error);
  lonejson_free(json);
  return rc;
}

int cai_cli_pouch_session(cai_cli_pouch *pouch, const char *id,
                          cai_cli_session *out, cai_error *error) {
  cai_source *source;
  int rc;
  memset(out, 0, sizeof(*out));
  source = NULL;
  rc = download_source(pouch->sessions, id, NULL, &source, error);
  if (rc == CAI_OK)
    rc = parse_source(source, &session_map, out, error);
  if (rc != CAI_OK) {
    char message[640];
    long http_status;
    http_status = error->http_status;
    snprintf(message, sizeof(message), "load session %s: %s", id,
             error->message != NULL ? error->message : "read failed");
    rc = cai_cli_error(error, rc, message);
    error->http_status = http_status;
  }
  cai_source_close(source);
  return rc;
}

static int acquire(lc_client *client, const char *key, lc_lease **out,
                   cai_error *error) {
  lc_acquire_req request;
  lc_error local;
  int rc;
  lc_acquire_req_init(&request);
  lc_error_init(&local);
  request.key = key;
  request.owner = "cai";
  request.ttl_seconds = 3600;
  request.block_seconds = 5;
  rc = client->acquire(client, &request, out, &local);
  if (rc != LC_OK)
    rc = cai_cli_lc_error(error, &local);
  lc_error_cleanup(&local);
  return rc;
}

static int release(lc_lease *lease, int rc, cai_error *error) {
  lc_error local;
  int result;
  if (lease == NULL)
    return rc;
  lc_error_init(&local);
  result = lease->release(lease, NULL, &local);
  if (result != LC_OK) {
    lc_lease_close(lease);
    if (rc == CAI_OK)
      rc = cai_cli_lc_error(error, &local);
  }
  lc_error_cleanup(&local);
  return rc;
}

static int update(lc_lease *lease, cai_source *source, cai_error *error) {
  lc_source *input;
  lc_error local;
  int rc;
  input = NULL;
  lc_error_init(&local);
  rc = cai_cli_lc_source(source, &input, error);
  if (rc == CAI_OK) {
    rc = lease->update(lease, input, NULL, &local);
    if (rc != LC_OK)
      rc = cai_cli_lc_error(error, &local);
  }
  lc_source_close(input);
  lc_error_cleanup(&local);
  return rc;
}

static int update_session(lc_lease *lease, cai_cli_session *session,
                          cai_error *error) {
  cai_source *source;
  int rc;
  source = NULL;
  rc = mapped_source(&session_map, session, &source, error);
  if (rc == CAI_OK)
    rc = update(lease, source, error);
  cai_source_close(source);
  return rc;
}

static int attach(lc_lease *lease, const char *name, cai_source *source,
                  cai_error *error) {
  lc_source *input;
  lc_attach_req request;
  lc_attach_res response;
  lc_error local;
  int rc;
  input = NULL;
  lc_attach_req_init(&request);
  memset(&response, 0, sizeof(response));
  lc_error_init(&local);
  request.name = name;
  request.content_type = "application/json";
  request.prevent_overwrite = 1;
  rc = cai_cli_lc_source(source, &input, error);
  if (rc == CAI_OK) {
    rc = lc_lease_attach(lease, &request, input, &response, &local);
    if (rc != LC_OK)
      rc = cai_cli_lc_error(error, &local);
  }
  lc_source_close(input);
  lc_attach_res_cleanup(&response);
  lc_error_cleanup(&local);
  return rc;
}

static int get_session(lc_lease *lease, const char *scope, const char *id,
                       cai_cli_session *session, cai_error *error) {
  lc_error local;
  lc_get_res response;
  int rc;
  memset(session, 0, sizeof(*session));
  if (lease->state_etag != NULL && lease->state_etag[0] != '\0') {
    lc_error_init(&local);
    memset(&response, 0, sizeof(response));
    rc = lease->load(lease, &session_map, session, NULL, &response, &local);
    if (rc != LC_OK)
      rc = cai_cli_lc_error(error, &local);
    lc_get_res_cleanup(&response);
    lc_error_cleanup(&local);
    if (rc != CAI_OK)
      return rc;
    if (strcmp(session->workspace, scope) != 0)
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "session belongs to another directory");
  } else {
    memset(session, 0, sizeof(*session));
    if (strlen(scope) >= sizeof(session->workspace) ||
        strlen(id) >= sizeof(session->id))
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "session identifier or workspace too long");
    strcpy(session->workspace, scope);
    strcpy(session->id, id);
    session->published = 1;
  }
  return CAI_OK;
}

static int checkpoint(void *context, const char *scope, const char *id,
                      cai_source *state, unsigned long long sequence,
                      cai_error *error) {
  cai_cli_pouch *pouch;
  cai_cli_session session;
  lc_lease *lease;
  struct timespec now;
  int rc;
  pouch = (cai_cli_pouch *)context;
  lease = NULL;
  rc = acquire(pouch->sessions, id, &lease, error);
  if (rc == CAI_OK)
    rc = get_session(lease, scope, id, &session, error);
  if (rc == CAI_OK && clock_gettime(CLOCK_REALTIME, &now) != 0)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "read checkpoint timestamp");
  if (rc == CAI_OK) {
    session.checkpoint_ns = (lonejson_uint64)now.tv_sec * 1000000000ULL +
                            (lonejson_uint64)now.tv_nsec;
    session.applied_sequence = sequence;
    snprintf(session.checkpoint_name, sizeof(session.checkpoint_name),
             "checkpoint-%020llu-%020llu",
             (unsigned long long)session.checkpoint_ns, sequence);
    rc = attach(lease, session.checkpoint_name, state, error);
    if (rc == CAI_OK)
      rc = update_session(lease, &session, error);
  }
  return release(lease, rc, error);
}

static int load_id(void *context, const char *scope, const char *id,
                   cai_source **out, unsigned long long *sequence,
                   cai_error *error) {
  cai_cli_pouch *pouch;
  cai_cli_session session;
  int rc;
  pouch = (cai_cli_pouch *)context;
  *out = NULL;
  rc = cai_cli_pouch_session(pouch, id, &session, error);
  if (rc != CAI_OK)
    return rc;
  if (strcmp(scope, session.workspace) != 0)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "session belongs to another directory");
  if (session.checkpoint_name[0] == '\0')
    return CAI_OK;
  *sequence = session.applied_sequence;
  return download_source(pouch->sessions, id, session.checkpoint_name, out,
                         error);
}

static int session_compare(const void *left, const void *right) {
  const cai_cli_session *a;
  const cai_cli_session *b;
  a = (const cai_cli_session *)left;
  b = (const cai_cli_session *)right;
  if (a->checkpoint_ns != b->checkpoint_ns)
    return a->checkpoint_ns > b->checkpoint_ns ? -1 : 1;
  return strcmp(b->id, a->id);
}

typedef struct key_page {
  char keys[256][CAI_AGENT_SESSION_ID_MAX];
  size_t count;
  size_t length;
} key_page;
static int key_begin(void *context, lc_error *error) {
  key_page *page;
  (void)error;
  page = (key_page *)context;
  page->length = 0U;
  return page->count < 256U;
}
static int key_chunk(void *context, const char *bytes, size_t length,
                     lc_error *error) {
  key_page *page;
  (void)error;
  page = (key_page *)context;
  if (page->length + length >= CAI_AGENT_SESSION_ID_MAX)
    return 0;
  memcpy(page->keys[page->count] + page->length, bytes, length);
  page->length += length;
  return 1;
}
static int key_end(void *context, lc_error *error) {
  key_page *page;
  (void)error;
  page = (key_page *)context;
  page->keys[page->count++][page->length] = '\0';
  return 1;
}

int cai_cli_pouch_list(cai_cli_pouch *pouch, const char *scope,
                       cai_cli_session **out, size_t *count, cai_error *error) {
  lc_query_req query;
  lc_query_res result;
  lc_query_key_handler handler;
  lc_error local;
  key_page page;
  cai_cli_session session;
  cai_cli_session *grown;
  char *cursor;
  char *selector;
  lonejson *json;
  lonejson_error json_error;
  size_t i;
  int rc;
  *out = NULL;
  *count = 0U;
  cursor = NULL;
  selector = NULL;
  json = lonejson_new(NULL, &json_error);
  if (json == NULL)
    return cai_cli_lj_error(error, &json_error);
  /* LQL literal escaping is delegated to lonejson; selectors never interpolate
   * raw paths. */
  if (scope != NULL) {
    scope_query doc;
    memset(&doc, 0, sizeof(doc));
    strcpy(doc.eq.field, "/workspace");
    strcpy(doc.eq.value, scope);
    selector =
        lonejson_serialize_alloc(json, &scope_map, &doc, NULL, &json_error);
    if (selector == NULL) {
      lonejson_free(json);
      return cai_cli_lj_error(error, &json_error);
    }
  }
  lc_error_init(&local);
  lc_query_req_init(&query);
  memset(&handler, 0, sizeof(handler));
  handler.begin = key_begin;
  handler.chunk = key_chunk;
  handler.end = key_end;
  query.selector_json = selector;
  query.selector_lql = scope == NULL ? "eq{field=/published,value=1}" : NULL;
  query.engine = "index";
  query.refresh = "wait_for";
  query.limit = 256;
  rc = CAI_OK;
  do {
    memset(&page, 0, sizeof(page));
    memset(&result, 0, sizeof(result));
    query.cursor = cursor;
    if (lc_query_keys(pouch->sessions, &query, &handler, &page, &result,
                      &local) != LC_OK)
      rc = cai_cli_lc_error(error, &local);
    free(cursor);
    cursor = result.cursor != NULL ? strdup(result.cursor) : NULL;
    if (result.cursor != NULL && cursor == NULL)
      rc = cai_cli_error(error, CAI_ERR_NOMEM, "allocate query cursor");
    lc_query_res_cleanup(&result);
    for (i = 0U; rc == CAI_OK && i < page.count; i++) {
      rc = cai_cli_pouch_session(pouch, page.keys[i], &session, error);
      if (rc != CAI_OK)
        break;
      if (!session.published || session.checkpoint_name[0] == '\0' ||
          (scope != NULL && strcmp(scope, session.workspace) != 0))
        continue;
      grown = (cai_cli_session *)realloc(*out, (*count + 1U) * sizeof(**out));
      if (grown == NULL) {
        rc = cai_cli_error(error, CAI_ERR_NOMEM, "allocate session list");
        break;
      }
      *out = grown;
      (*out)[(*count)++] = session;
    }
  } while (rc == CAI_OK && cursor != NULL && cursor[0] != '\0');
  free(cursor);
  free(selector);
  lonejson_free(json);
  lc_error_cleanup(&local);
  if (rc != CAI_OK) {
    free(*out);
    *out = NULL;
    *count = 0U;
  } else if (*count > 1U) {
    qsort(*out, *count, sizeof(**out), session_compare);
  }
  return rc;
}

static int load_latest(void *context, const char *scope, char *id,
                       size_t capacity, cai_source **out,
                       unsigned long long *sequence, cai_error *error) {
  cai_cli_session *sessions;
  size_t count;
  int rc;
  *out = NULL;
  rc = cai_cli_pouch_list((cai_cli_pouch *)context, scope, &sessions, &count,
                          error);
  if (rc == CAI_OK && count > 0U) {
    if (strlen(sessions[0].id) >= capacity)
      rc = cai_cli_error(error, CAI_ERR_INVALID, "session ID output too small");
    else {
      strcpy(id, sessions[0].id);
      rc = load_id(context, scope, id, out, sequence, error);
    }
  }
  free(sessions);
  return rc;
}

static void preview(char *out, size_t capacity, const char *text) {
  size_t n;
  n = 0U;
  while (text != NULL && *text != '\0' && n + 1U < capacity) {
    out[n++] = (unsigned char)*text < 32U || *text == 127 ? ' ' : *text;
    text++;
  }
  /* Drop any incomplete UTF-8 sequence at the truncation boundary. */
  if (text != NULL && *text != '\0') {
    if (n > 3U)
      n -= 3U;
    while (n > 0U && ((unsigned char)out[n - 1U] & 0xc0U) == 0x80U)
      n--;
    if (n > 0U && (unsigned char)out[n - 1U] >= 0xc0U)
      n--;
    if (n + 4U <= capacity) {
      memcpy(out + n, "...", 3U);
      n += 3U;
    }
  }
  out[n] = '\0';
}

static int append_event(void *context, const char *scope, const char *id,
                        const cai_agent_session_event *event,
                        cai_error *error) {
  cai_cli_pouch *pouch;
  cai_cli_session session;
  cli_record record;
  lc_lease *lease;
  cai_source *source;
  char name[64];
  int rc;
  pouch = (cai_cli_pouch *)context;
  lease = NULL;
  source = NULL;
  memset(&record, 0, sizeof(record));
  strcpy(record.record_type, "event");
  record.sequence = event->sequence;
  record.type = event->type;
  record.data = event->data;
  snprintf(name, sizeof(name), "event-%020llu", event->sequence);
  rc = acquire(pouch->sessions, id, &lease, error);
  if (rc == CAI_OK)
    rc = get_session(lease, scope, id, &session, error);
  if (rc == CAI_OK &&
      (lease->state_etag == NULL || lease->state_etag[0] == '\0'))
    rc = update_session(lease, &session, error);
  if (rc == CAI_OK)
    rc = mapped_source(&event_map, &record, &source, error);
  if (rc == CAI_OK)
    rc = attach(lease, name, source, error);
  if (rc == CAI_OK && session.first_prompt[0] == '\0' && event->data != NULL &&
      (strcmp(event->type, "turn_submitted") == 0 ||
       strcmp(event->type, "turn_queued") == 0)) {
    preview(session.first_prompt, sizeof(session.first_prompt), event->data);
    rc = update_session(lease, &session, error);
  }
  cai_source_close(source);
  return release(lease, rc, error);
}

static int attachment_compare(const void *left, const void *right) {
  const lc_attachment_info *a;
  const lc_attachment_info *b;
  a = (const lc_attachment_info *)left;
  b = (const lc_attachment_info *)right;
  return strcmp(a->name, b->name);
}

static int event_attachments(cai_cli_pouch *pouch, const char *id,
                             lc_attachment_list *out, cai_error *error) {
  lc_attachment_list_req request;
  lc_error local;
  int rc;
  lc_attachment_list_req_init(&request);
  lc_error_init(&local);
  memset(out, 0, sizeof(*out));
  request.lease.ns = "cai.sessions";
  request.lease.key = id;
  request.public_read = 1;
  rc = lc_list_attachments(pouch->sessions, &request, out, &local);
  if (rc != LC_OK)
    rc = cai_cli_lc_error(error, &local);
  else if (out->count > 1U)
    qsort(out->items, out->count, sizeof(*out->items), attachment_compare);
  lc_error_cleanup(&local);
  return rc;
}

static int session_exists(cai_cli_pouch *pouch, const char *id, int *exists,
                          cai_error *error) {
  lc_describe_req request;
  lc_describe_res response;
  lc_error local;
  int rc;
  *exists = 0;
  lc_describe_req_init(&request);
  memset(&response, 0, sizeof(response));
  lc_error_init(&local);
  request.ns = "cai.sessions";
  request.key = id;
  rc = lc_describe(pouch->sessions, &request, &response, &local);
  if (rc != LC_OK && local.http_status != 404)
    rc = cai_cli_lc_error(error, &local);
  else {
    *exists = response.state_etag != NULL && response.state_etag[0] != '\0';
    rc = CAI_OK;
  }
  lc_describe_res_cleanup(&response);
  lc_error_cleanup(&local);
  return rc;
}

static int load_events(void *context, const char *scope, const char *id,
                       unsigned long long after,
                       cai_agent_session_event_fn visit, void *visitor,
                       cai_error *error) {
  cai_cli_pouch *pouch;
  cai_cli_session session;
  lc_attachment_list attachments;
  cli_record record;
  cai_agent_session_event event;
  cai_source *source;
  unsigned long long sequence;
  size_t i;
  int rc;
  int exists;
  pouch = (cai_cli_pouch *)context;
  memset(&attachments, 0, sizeof(attachments));
  rc = session_exists(pouch, id, &exists, error);
  if (rc != CAI_OK || !exists)
    return rc;
  rc = cai_cli_pouch_session(pouch, id, &session, error);
  if (rc == CAI_OK && strcmp(scope, session.workspace) != 0)
    rc = cai_cli_error(error, CAI_ERR_INVALID,
                       "session belongs to another directory");
  if (rc == CAI_OK)
    rc = event_attachments(pouch, id, &attachments, error);
  for (i = 0U; rc == CAI_OK && i < attachments.count; i++) {
    if (strncmp(attachments.items[i].name, "event-", 6U) != 0)
      continue;
    sequence = strtoull(attachments.items[i].name + 6U, NULL, 10);
    if (sequence <= after)
      continue;
    source = NULL;
    memset(&record, 0, sizeof(record));
    rc = download_source(pouch->sessions, id, attachments.items[i].name,
                         &source, error);
    if (rc == CAI_OK)
      rc = parse_source(source, &event_map, &record, error);
    cai_source_close(source);
    if (rc == CAI_OK && (record.sequence != sequence ||
                         strcmp(record.record_type, "event") != 0))
      rc = cai_cli_error(error, CAI_ERR_INVALID,
                         "invalid stored event identity");
    if (rc == CAI_OK) {
      event.sequence = record.sequence;
      event.type = record.type;
      event.data = record.data;
      rc = visit(visitor, &event, error);
    }
    lonejson_cleanup(&event_map, &record);
  }
  lc_attachment_list_cleanup(&attachments);
  return rc;
}

static int auth_load(void *context, const char *key, cai_source **out,
                     cai_error *error) {
  cai_cli_pouch *pouch;
  lc_describe_req request;
  lc_describe_res response;
  lc_error local;
  int rc;
  pouch = (cai_cli_pouch *)context;
  *out = NULL;
  lc_describe_req_init(&request);
  memset(&response, 0, sizeof(response));
  lc_error_init(&local);
  request.key = key;
  request.ns = "cai.auth";
  rc = lc_describe(pouch->auth, &request, &response, &local);
  if (rc != LC_OK && local.http_status != 404)
    rc = cai_cli_lc_error(error, &local);
  else {
    rc = response.state_etag != NULL && response.state_etag[0] != '\0'
             ? download_source(pouch->auth, key, NULL, out, error)
             : CAI_OK;
  }
  lc_describe_res_cleanup(&response);
  lc_error_cleanup(&local);
  return rc;
}

static int auth_replace(void *context, const char *key, cai_source *source,
                        cai_error *error) {
  cai_cli_pouch *pouch;
  lc_lease *lease;
  int rc;
  pouch = (cai_cli_pouch *)context;
  lease = NULL;
  rc = acquire(pouch->auth, key, &lease, error);
  if (rc == CAI_OK)
    rc = update(lease, source, error);
  return release(lease, rc, error);
}

static int private_directory(const char *path, cai_error *error) {
  struct stat status;
  if (mkdir(path, 0700) != 0 && errno != EEXIST)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "create cai state directory");
  if (lstat(path, &status) != 0 || !S_ISDIR(status.st_mode) ||
      status.st_uid != geteuid() || (status.st_mode & 077U) != 0U)
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "cai state directory must be owned by you with mode 0700");
  return CAI_OK;
}

static int ensure_parents(char *path, cai_error *error) {
  char *cursor;
  struct stat status;
  for (cursor = path + 1; *cursor != '\0'; cursor++) {
    if (*cursor != '/')
      continue;
    *cursor = '\0';
    if (mkdir(path, 0700) != 0 && errno != EEXIST) {
      *cursor = '/';
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "create state parent directory");
    }
    if (stat(path, &status) != 0 || !S_ISDIR(status.st_mode)) {
      *cursor = '/';
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "state parent is not a directory");
    }
    *cursor = '/';
  }
  return CAI_OK;
}

int cai_cli_pouch_open(cai_cli_pouch *pouch, const char *selected_endpoint,
                       const char *client_pem, pslog_logger *logger,
                       cai_error *error) {
  const char *base;
  char path[PATH_MAX];
  char root[PATH_MAX];
  char init_path[PATH_MAX];
  char *endpoint;
  const char *endpoints[1];
  lc_client_config config;
  lc_pouch_settings settings;
  lc_error local;
  struct stat status;
  int lock_fd;
  int root_exists;
  int local_pouch;
  CURLU *url;
  char *decoded_root;
  int rc;
  memset(pouch, 0, sizeof(*pouch));
  pouch->logger = logger;
  base = getenv("XDG_STATE_HOME");
  if (base != NULL && base[0] != '\0')
    rc = snprintf(path, sizeof(path), "%s/cai", base);
  else {
    base = getenv("HOME");
    if (base == NULL || base[0] == '\0')
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "set HOME or XDG_STATE_HOME");
    rc = snprintf(path, sizeof(path), "%s/.local/state/cai", base);
  }
  if (rc < 0 || (size_t)rc >= sizeof(path) || path[0] != '/')
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "cai state path must be absolute and fit PATH_MAX");
  rc = ensure_parents(path, error);
  if (rc == CAI_OK)
    rc = private_directory(path, error);
  if (rc != CAI_OK)
    return rc;
  if (realpath(path, pouch->state_directory) == NULL)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "resolve cai state directory");
  if (snprintf(pouch->key_path, sizeof(pouch->key_path), "%s/pouch.key",
               pouch->state_directory) >= (int)sizeof(pouch->key_path) ||
      snprintf(root, sizeof(root), "%s/pouch", pouch->state_directory) >=
          (int)sizeof(root) ||
      snprintf(init_path, sizeof(init_path), "%s/pouch.init.lock",
               pouch->state_directory) >= (int)sizeof(init_path))
    return cai_cli_error(error, CAI_ERR_INVALID, "cai state path too long");
  local_pouch = selected_endpoint == NULL ||
                strncmp(selected_endpoint, "pouch://", 8U) == 0;
  if (selected_endpoint != NULL && local_pouch) {
    url = curl_url();
    decoded_root = NULL;
    if (url == NULL ||
        curl_url_set(url, CURLUPART_URL, selected_endpoint,
                     CURLU_NON_SUPPORT_SCHEME) != CURLUE_OK ||
        curl_url_get(url, CURLUPART_PATH, &decoded_root, CURLU_URLDECODE) !=
            CURLUE_OK ||
        decoded_root[0] != '/' || strlen(decoded_root) >= sizeof(root)) {
      curl_free(decoded_root);
      curl_url_cleanup(url);
      return cai_cli_error(error, CAI_ERR_INVALID, "invalid pouch root URL");
    }
    strcpy(root, decoded_root);
    curl_free(decoded_root);
    curl_url_cleanup(url);
  }
  lock_fd = open(init_path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (lock_fd < 0 || flock(lock_fd, LOCK_EX) != 0) {
    if (lock_fd >= 0)
      close(lock_fd);
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "lock pouch initialization");
  }
  root_exists = lstat(root, &status) == 0;
  if (local_pouch && lstat(pouch->key_path, &status) != 0 && root_exists) {
    close(lock_fd);
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "pouch.key is missing for the existing encrypted "
                         "store; restore the original key");
  }
  lc_error_init(&local);
  endpoint = NULL;
  if (selected_endpoint != NULL) {
    endpoint = strdup(selected_endpoint);
    rc = endpoint != NULL ? LC_OK : LC_ERR_NOMEM;
  } else {
    rc = lc_pouch_endpoint_build(root, NULL, 0U, &endpoint, &local);
  }
  lc_client_config_init(&config);
  config.logger = logger;
  lc_pouch_settings_init(&settings);
  settings.set_mask =
      LC_POUCH_SETTING_SINGLE_WRITER | LC_POUCH_SETTING_DURABLE_SYNC |
      LC_POUCH_SETTING_QUERY_INDEXING | LC_POUCH_SETTING_QUERY_ENGINE |
      LC_POUCH_SETTING_CRYPTO_KEY_FILE |
      LC_POUCH_SETTING_CRYPTO_GENERATE_KEY_FILE;
  settings.single_writer = 0;
  settings.durable_sync = 1;
  settings.query_indexing_enabled = 1;
  settings.query_engine = "index";
  settings.crypto_key_file = pouch->key_path;
  settings.crypto_generate_key_file = !root_exists;
  endpoints[0] = endpoint;
  config.endpoints = endpoints;
  config.endpoint_count = 1U;
  config.default_namespace = "cai.sessions";
  config.pouch_settings = local_pouch ? &settings : NULL;
  config.client_bundle_path = client_pem;
  config.disable_mtls = client_pem == NULL;
  if (rc == LC_OK)
    rc = lc_client_open(&config, &pouch->sessions, &local);
  if (rc == LC_OK) {
    config.default_namespace = "cai.auth";
    settings.crypto_generate_key_file = 0;
    rc = lc_client_open(&config, &pouch->auth, &local);
  }
  lc_pouch_endpoint_free(endpoint);
  if (rc != LC_OK)
    rc = cai_cli_lc_error(error, &local);
  lc_error_cleanup(&local);
  close(lock_fd);
  if (rc != CAI_OK) {
    cai_cli_pouch_close(pouch);
    return rc;
  }
  pouch->store.context = pouch;
  pouch->store.checkpoint = checkpoint;
  pouch->store.load_latest = load_latest;
  pouch->store.load_id = load_id;
  pouch->store.append_event = append_event;
  pouch->store.load_events_after = load_events;
  pouch->credentials.context = pouch;
  pouch->credentials.load = auth_load;
  pouch->credentials.replace = auth_replace;
  return CAI_OK;
}

void cai_cli_pouch_close(cai_cli_pouch *pouch) {
  if (pouch->auth != NULL)
    pouch->auth->close(pouch->auth);
  if (pouch->sessions != NULL)
    pouch->sessions->close(pouch->sessions);
  pouch->auth = NULL;
  pouch->sessions = NULL;
}

typedef struct auth_origin {
  char path[PATH_MAX];
  char digest[65];
} auth_origin;
static const lonejson_field auth_origin_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(auth_origin, path, "path",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(auth_origin, digest, "digest",
                                    LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(auth_origin_map, auth_origin, auth_origin_fields);

int cai_cli_pouch_seed_auth(cai_cli_pouch *pouch, const char *path,
                            cai_error *error) {
  auth_origin current;
  auth_origin previous;
  unsigned char buffer[4096];
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_length;
  EVP_MD_CTX *hash;
  cai_source *source;
  cai_source *stored;
  FILE *file;
  cai_chatgpt_auth_config auth_config;
  cai_chatgpt_auth *auth;
  size_t count;
  size_t i;
  int rc;
  memset(&current, 0, sizeof(current));
  memset(&previous, 0, sizeof(previous));
  if (realpath(path, current.path) == NULL)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "cannot open authentication source file");
  /* Reject invalid credentials before changing the durable credential store.
   * Opening auth only validates the document; token access performs refresh. */
  auth = NULL;
  cai_chatgpt_auth_config_init(&auth_config);
  auth_config.logger = pouch->logger;
  auth_config.auth_json_path = current.path;
  rc = cai_chatgpt_auth_open(&auth_config, &auth, error);
  cai_chatgpt_auth_close(auth);
  if (rc != CAI_OK)
    return rc;
  file = fopen(current.path, "rb");
  if (file == NULL)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "cannot read authentication source file");
  source = NULL;
  stored = NULL;
  rc = cai_source_file(file, 1, &source, error);
  if (rc != CAI_OK) {
    fclose(file);
    return rc;
  }
  hash = EVP_MD_CTX_new();
  if (hash == NULL || EVP_DigestInit_ex(hash, EVP_sha256(), NULL) != 1)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                       "initialize auth source fingerprint");
  while (rc == CAI_OK) {
    count = cai_source_read(source, buffer, sizeof(buffer), error);
    if (count == 0U) {
      rc = error->code;
      break;
    }
    if (EVP_DigestUpdate(hash, buffer, count) != 1)
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "fingerprint auth source");
  }
  if (rc == CAI_OK && EVP_DigestFinal_ex(hash, digest, &digest_length) != 1)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                       "finish auth source fingerprint");
  EVP_MD_CTX_free(hash);
  OPENSSL_cleanse(buffer, sizeof(buffer));
  if (rc == CAI_OK) {
    for (i = 0U; i < digest_length; i++)
      snprintf(current.digest + i * 2U, sizeof(current.digest) - i * 2U, "%02x",
               (unsigned int)digest[i]);
    rc = auth_load(pouch, "source.json", &stored, error);
  }
  if (rc == CAI_OK && stored != NULL)
    rc = parse_source(stored, &auth_origin_map, &previous, error);
  cai_source_close(stored);
  stored = NULL;
  /* An unchanged Codex file must not replace refreshed tokens in the pouch. */
  if (rc == CAI_OK && (strcmp(current.path, previous.path) != 0 ||
                       strcmp(current.digest, previous.digest) != 0)) {
    rc = cai_source_reset(source, error);
    if (rc == CAI_OK)
      rc = auth_replace(pouch, "auth.json", source, error);
    if (rc == CAI_OK)
      rc = mapped_source(&auth_origin_map, &current, &stored, error);
    if (rc == CAI_OK)
      rc = auth_replace(pouch, "source.json", stored, error);
  }
  cai_source_close(stored);
  cai_source_close(source);
  return rc;
}

int cai_cli_pouch_publish(cai_cli_pouch *pouch, const char *id, int published,
                          cai_error *error) {
  cai_cli_session session;
  lc_lease *lease;
  int rc;
  lease = NULL;
  rc = acquire(pouch->sessions, id, &lease, error);
  if (rc == CAI_OK)
    rc = cai_cli_pouch_session(pouch, id, &session, error);
  if (rc == CAI_OK) {
    session.published = published;
    rc = update_session(lease, &session, error);
  }
  return release(lease, rc, error);
}

int cai_cli_pouch_lock(cai_cli_pouch *pouch, const char *id, lc_lease **out,
                       cai_error *error) {
  return acquire(pouch->sessions, id, out, error);
}

int cai_cli_pouch_unlock(lc_lease *lease, int result, cai_error *error) {
  return release(lease, result, error);
}

int cai_cli_pouch_export(cai_cli_pouch *pouch, const char *id, cai_sink *sink,
                         cai_error *error) {
  cai_cli_session session;
  cli_record record;
  cai_source *state;
  cai_source *source;
  lc_attachment_list attachments;
  lonejson_error local;
  size_t i;
  int rc;
  state = NULL;
  source = NULL;
  memset(&record, 0, sizeof(record));
  memset(&attachments, 0, sizeof(attachments));
  rc = cai_cli_pouch_session(pouch, id, &session, error);
  if (rc == CAI_OK && session.checkpoint_name[0] == '\0')
    rc = cai_cli_error(error, CAI_ERR_INVALID,
                       "session has no complete checkpoint");
  if (rc == CAI_OK)
    rc = download_source(pouch->sessions, id, session.checkpoint_name, &state,
                         error);
  if (rc == CAI_OK) {
    strcpy(record.record_type, "checkpoint");
    record.checkpoint_created_at_ns = session.checkpoint_ns;
    record.applied_event_sequence = session.applied_sequence;
    lonejson_json_value_init(NULL, &record.state);
    if (lonejson_json_value_set_reader(&record.state, cai_cli_json_read, state,
                                       &local) != LONEJSON_STATUS_OK)
      rc = cai_cli_lj_error(error, &local);
    else
      rc = mapped_source(&checkpoint_map, &record, &source, error);
  }
  if (rc == CAI_OK)
    rc = cai_source_copy_to_sink(source, sink, error);
  cai_source_close(source);
  cai_source_close(state);
  lonejson_json_value_cleanup(&record.state);
  if (rc == CAI_OK)
    rc = cai_sink_write(sink, "\n", 1U, error);
  if (rc == CAI_OK)
    rc = event_attachments(pouch, id, &attachments, error);
  for (i = 0U; rc == CAI_OK && i < attachments.count; i++) {
    if (strncmp(attachments.items[i].name, "event-", 6U) != 0)
      continue;
    source = NULL;
    rc = download_source(pouch->sessions, id, attachments.items[i].name,
                         &source, error);
    if (rc == CAI_OK)
      rc = cai_source_copy_to_sink(source, sink, error);
    cai_source_close(source);
    if (rc == CAI_OK)
      rc = cai_sink_write(sink, "\n", 1U, error);
  }
  lc_attachment_list_cleanup(&attachments);
  return rc;
}

typedef struct import_context {
  lonejson *json;
  lonejson_stream *stream;
  cli_record record;
  size_t state_bytes;
  cai_sink *sink;
} import_context;

static lonejson_status import_state_write(void *context, const void *bytes,
                                          size_t count, lonejson_error *error) {
  import_context *input;
  input = (import_context *)context;
  input->state_bytes += count;
  return cai_cli_json_write(input->sink, bytes, count, error);
}

static int import_checkpoint(void *context, cai_sink *sink, cai_error *error) {
  import_context *input;
  lonejson_error local;
  input = (import_context *)context;
  input->sink = sink;
  if (lonejson_json_value_set_parse_sink(&input->record.state,
                                         import_state_write, input,
                                         &local) != LONEJSON_STATUS_OK)
    return cai_cli_lj_error(error, &local);
  if (lonejson_stream_next(input->stream, &input->record, &local) !=
      LONEJSON_STREAM_OBJECT)
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "JSONL import must start with a complete checkpoint record");
  if (strcmp(input->record.record_type, "checkpoint") != 0 ||
      input->state_bytes == 0U)
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "JSONL import must start with a checkpoint containing state");
  return CAI_OK;
}

static int validate_checkpoint(cai_cli_pouch *pouch, const char *id,
                               const char *workspace, cai_error *error) {
  cai_client_config client_config;
  cai_agent_config agent_config;
  cai_client *client;
  cai_agent *agent;
  cai_session *session;
  cai_source *state;
  unsigned long long sequence;
  int rc;
  client = NULL;
  agent = NULL;
  session = NULL;
  state = NULL;
  cai_client_config_init(&client_config);
  client_config.logger = pouch->logger;
  client_config.base_url = "http://127.0.0.1:1/v1";
  client_config.api_key = "offline-state-validation";
  cai_agent_config_init(&agent_config);
  agent_config.model = "gpt-6-luna";
  agent_config.enable_local_history = 1;
  rc = cai_client_open(&client_config, &client, error);
  if (rc == CAI_OK)
    rc = cai_client_new_agent(client, &agent_config, &agent, error);
  if (rc == CAI_OK)
    rc = cai_agent_new_session(agent, &session, error);
  if (rc == CAI_OK)
    rc = load_id(pouch, workspace, id, &state, &sequence, error);
  if (rc == CAI_OK)
    rc = cai_session_import_state_source(session, state, error);
  cai_source_close(state);
  if (session != NULL)
    session->close(session);
  if (agent != NULL)
    agent->close(agent);
  if (client != NULL)
    client->close(client);
  return rc;
}

int cai_cli_pouch_import(cai_cli_pouch *pouch, const char *path,
                         const char *workspace, char *id, size_t capacity,
                         cai_error *error) {
  import_context input;
  lonejson_config config;
  lonejson_error local;
  lonejson_stream_result result;
  cai_cli_session session;
  cai_agent_session_event event;
  lc_lease *lease;
  cai_source *source;
  unsigned char random[16];
  unsigned long long previous;
  size_t i;
  int rc;
  int created;
  if (capacity < 33U || strlen(workspace) >= sizeof(session.workspace))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "import output or workspace too long");
  if (RAND_bytes(random, sizeof(random)) != 1)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "generate imported session ID");
  for (i = 0U; i < sizeof(random); i++)
    snprintf(id + i * 2U, capacity - i * 2U, "%02x", (unsigned int)random[i]);
  memset(&input, 0, sizeof(input));
  memset(&session, 0, sizeof(session));
  config = lonejson_default_config();
  config.clear_destination_by_default = 0;
  input.json = lonejson_new(&config, &local);
  if (input.json == NULL)
    return cai_cli_lj_error(error, &local);
  lonejson_init(input.json, &import_map, &input.record);
  input.stream =
      lonejson_stream_open_path(input.json, &import_map, path, &local);
  rc = input.stream != NULL ? CAI_OK : cai_cli_lj_error(error, &local);
  lease = NULL;
  source = NULL;
  created = 0;
  if (rc == CAI_OK)
    rc = acquire(pouch->sessions, id, &lease, error);
  if (rc == CAI_OK) {
    if ((lease->state_etag != NULL && lease->state_etag[0] != '\0'))
      rc = cai_cli_error(error, CAI_ERR_INVALID,
                         "generated session ID already exists");
    else {
      strcpy(session.id, id);
      strcpy(session.workspace, workspace);
      session.published = 0;
      rc = update_session(lease, &session, error);
      created = rc == CAI_OK;
    }
  }
  rc = release(lease, rc, error);
  if (rc == CAI_OK)
    rc = cai_cli_stream_source(import_checkpoint, &input, NULL, &source, error);
  if (rc == CAI_OK)
    rc = checkpoint(pouch, workspace, id, source, 0U, error);
  /* close joins the JSON parser before its mapped checkpoint metadata is read.
   */
  cai_source_close(source);
  if (rc == CAI_OK) {
    lease = NULL;
    rc = acquire(pouch->sessions, id, &lease, error);
    if (rc == CAI_OK)
      rc = get_session(lease, workspace, id, &session, error);
    if (rc == CAI_OK) {
      session.applied_sequence = input.record.applied_event_sequence;
      rc = update_session(lease, &session, error);
    }
    rc = release(lease, rc, error);
  }
  previous = 0U;
  while (rc == CAI_OK) {
    lonejson_cleanup(&import_map, &input.record);
    lonejson_init(input.json, &import_map, &input.record);
    result = lonejson_stream_next(input.stream, &input.record, &local);
    if (result == LONEJSON_STREAM_EOF)
      break;
    if (result != LONEJSON_STREAM_OBJECT) {
      rc = cai_cli_lj_error(error, &local);
      break;
    }
    if (strcmp(input.record.record_type, "event") != 0 ||
        input.record.type == NULL || input.record.type[0] == '\0' ||
        input.record.sequence <= previous) {
      rc = cai_cli_error(error, CAI_ERR_INVALID,
                         "imported journal events must have nonempty types and "
                         "strictly increasing positive sequences");
      break;
    }
    previous = input.record.sequence;
    event.sequence = input.record.sequence;
    event.type = input.record.type;
    event.data = input.record.data;
    rc = append_event(pouch, workspace, id, &event, error);
  }
  if (rc == CAI_OK)
    rc = validate_checkpoint(pouch, id, workspace, error);
  if (rc == CAI_OK)
    rc = cai_cli_pouch_publish(pouch, id, 1, error);
  if (rc != CAI_OK && created) {
    cai_error ignored;
    lc_error ignored_lc;
    int deleted;
    cai_error_init(&ignored);
    lc_error_init(&ignored_lc);
    lease = NULL;
    if (acquire(pouch->sessions, id, &lease, &ignored) == CAI_OK) {
      (void)lc_lease_delete_all_attachments(lease, &deleted, &ignored_lc);
      (void)lease->remove(lease, NULL, &ignored_lc);
      (void)release(lease, CAI_OK, &ignored);
    }
    lc_error_cleanup(&ignored_lc);
    cai_error_cleanup(&ignored);
  }
  lonejson_cleanup(&import_map, &input.record);
  lonejson_stream_close(input.stream);
  lonejson_free(input.json);
  return rc;
}
