#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "pouch.h"
#include "identity.h"
#include "log.h"
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

/* Store contexts retain these handles through teardown. SDK clients and
 * their app-owned logging state close only when the final borrower exits. */
typedef struct cai_cli_pouch_client {
  struct cai_cli_pouch_client *next;
  lc_client *client;
  cai_cli_log *logging;
  size_t references;
  int local;
  dev_t device, key_device;
  ino_t inode, key_inode;
  char *endpoint, *client_pem;
} cai_cli_pouch_client;
static pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;
static cai_cli_pouch_client *clients;

static int same_pouch_options(const char *a, const char *b) {
  size_t na, nb;
  a += 8U + strcspn(a + 8U, "?#");
  b += 8U + strcspn(b + 8U, "?#");
  na = *a == '?' ? strcspn(++a, "#") : 0U;
  nb = *b == '?' ? strcspn(++b, "#") : 0U;
  return na == nb && memcmp(a, b, na) == 0;
}

static int shared_client_open(cai_cli_pouch *pouch,
                              const lc_client_config *config, const char *root,
                              int local, const char *client_pem,
                              cai_error *error) {
  cai_cli_pouch_client *entry;
  struct stat directory, key;
  lc_error detail;
  int exists, rc;
  pthread_mutex_lock(&clients_lock);
  exists = local && stat(root, &directory) == 0;
  for (entry = clients; entry != NULL; entry = entry->next) {
    if (entry->local != local ||
        (local ? !exists || entry->device != directory.st_dev ||
                     entry->inode != directory.st_ino
               : strcmp(entry->endpoint, config->endpoints[0]) != 0))
      continue;
    if ((local &&
         (stat(pouch->key_path, &key) != 0 || key.st_dev != entry->key_device ||
          key.st_ino != entry->key_inode ||
          !same_pouch_options(entry->endpoint, config->endpoints[0]))) ||
        (!local && strcmp(entry->client_pem,
                          client_pem != NULL ? client_pem : "") != 0)) {
      pthread_mutex_unlock(&clients_lock);
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "conflicting key or TLS configuration for an "
                           "already open store; select the same key file");
    }
    entry->references++;
    pouch->connection = entry;
    pouch->sessions = entry->client;
    pthread_mutex_unlock(&clients_lock);
    return CAI_OK;
  }
  if (local && exists && stat(pouch->key_path, &key) != 0) {
    pthread_mutex_unlock(&clients_lock);
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "pouch.key is missing for the existing encrypted "
                         "store; restore the original key");
  }
  entry = calloc(1U, sizeof(*entry));
  if (entry != NULL) {
    entry->endpoint = strdup(config->endpoints[0]);
    entry->client_pem = strdup(client_pem != NULL ? client_pem : "");
  }
  if (entry == NULL || entry->endpoint == NULL || entry->client_pem == NULL) {
    if (entry != NULL) {
      free(entry->endpoint);
      free(entry->client_pem);
      free(entry);
    }
    pthread_mutex_unlock(&clients_lock);
    return cai_cli_error(error, CAI_ERR_NOMEM, "retain store connection");
  }
  lc_error_init(&detail);
  rc = lc_client_open(config, &entry->client, &detail);
  rc = rc == LC_OK ? CAI_OK : cai_cli_lc_error(error, &detail);
  lc_error_cleanup(&detail);
  if (rc == CAI_OK && local &&
      (stat(root, &directory) != 0 || stat(pouch->key_path, &key) != 0)) {
    char message[1200];
    snprintf(message, sizeof(message),
             "identify opened pouch root %s and key %s: %s", root,
             pouch->key_path, strerror(errno));
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, message);
  }
  if (rc != CAI_OK) {
    if (entry->client != NULL)
      entry->client->close(entry->client);
    free(entry->endpoint);
    free(entry->client_pem);
    free(entry);
    pthread_mutex_unlock(&clients_lock);
    return rc;
  }
  entry->local = local;
  if (local) {
    entry->device = directory.st_dev;
    entry->inode = directory.st_ino;
    entry->key_device = key.st_dev;
    entry->key_inode = key.st_ino;
  }
  entry->references = 1U;
  entry->logging = cai_cli_log_retain(config->logger);
  entry->next = clients;
  clients = entry;
  pouch->connection = entry;
  pouch->sessions = entry->client;
  pthread_mutex_unlock(&clients_lock);
  return CAI_OK;
}

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

/* A derived directory index, kept separately from conversation documents.
 * Dirty is durable before a checkpoint/publication changes; an interrupted
 * writer therefore causes reconstruction from authoritative session metadata.
 */
typedef struct latest_index {
  char workspace[PATH_MAX];
  lonejson_int64 dirty;
  cai_cli_session latest;
} latest_index;
static const lonejson_field latest_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(latest_index, workspace, "workspace",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_I64_REQ(latest_index, dirty, "dirty"),
    LONEJSON_FIELD_OBJECT(latest_index, latest, "latest", &session_map)};
LONEJSON_MAP_DEFINE(latest_map, latest_index, latest_fields);
static int latest_open(cai_cli_pouch *, const char *, lc_lease **,
                       latest_index *, cai_error *);
static int latest_write(cai_cli_pouch *, lc_lease *, latest_index *,
                        cai_error *);
static int session_exists(cai_cli_pouch *, const char *, int *, cai_error *);
static int latest_rebuild(cai_cli_pouch *, latest_index *, cai_error *);
static int session_compare(const void *, const void *);

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

int cai_cli_mapped_source(const lonejson_map *map, const void *doc,
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
  lc_lease *lease;
  char *key;
  char *attachment;
  const char *ns;
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
    if (request->lease != NULL && request->attachment != NULL) {
      lc_attachment_get_req private_request;
      lc_attachment_get_req_init(&private_request);
      memset(&attached, 0, sizeof(attached));
      private_request.selector.name = request->attachment;
      rc = lc_lease_get_attachment(request->lease, &private_request,
                                   destination, &attached, &local);
      lc_attachment_get_res_cleanup(&attached);
    } else if (request->lease != NULL) {
      memset(&response, 0, sizeof(response));
      rc = lc_lease_get(request->lease, destination, NULL, &response, &local);
      lc_get_res_cleanup(&response);
    } else if (request->attachment != NULL) {
      lc_attachment_get_op_init(&attachment);
      memset(&attached, 0, sizeof(attached));
      attachment.lease.ns = request->ns;
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
      rc = lc_get_in_namespace(request->client, request->ns, request->key,
                               &options, destination, &response, &local);
      lc_get_res_cleanup(&response);
    }
    if (rc != LC_OK)
      rc = cai_cli_lc_error(error, &local);
  }
  lc_sink_close(destination);
  lc_error_cleanup(&local);
  return rc;
}

static int download_source_ns(lc_client *client, const char *ns,
                              const char *key, const char *attachment,
                              cai_source **out, cai_error *error) {
  download *request;
  int rc;
  request = (download *)calloc(1U, sizeof(*request));
  if (request == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate pouch download");
  request->client = client;
  request->ns = ns;
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

static int download_source(cai_cli_pouch *pouch, const char *key,
                           const char *attachment, cai_source **out,
                           cai_error *error) {
  return download_source_ns(pouch->sessions, pouch->sessions_namespace, key,
                            attachment, out, error);
}

static int download_private(lc_lease *lease, const char *attachment,
                            cai_source **out, cai_error *error) {
  download *request = calloc(1U, sizeof(*request));
  int rc;
  if (request == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate private download");
  request->lease = lease;
  request->attachment = attachment != NULL ? strdup(attachment) : NULL;
  if (attachment != NULL && request->attachment == NULL) {
    download_close(request);
    return cai_cli_error(error, CAI_ERR_NOMEM, "copy private attachment name");
  }
  rc = cai_cli_stream_source(download_write, request, download_close, out,
                             error);
  if (rc != CAI_OK) {
    download_close(request);
    return rc;
  }
  rc = cai_cli_stream_ready(*out, error);
  if (rc != CAI_OK) {
    cai_source_close(*out);
    *out = NULL;
  }
  return rc;
}

typedef struct parse_input {
  cai_source *source;
  cai_error error;
  size_t bytes_read;
} parse_input;

static lonejson_read_result parse_read(void *context, unsigned char *bytes,
                                       size_t capacity) {
  parse_input *input;
  lonejson_read_result result;
  input = (parse_input *)context;
  result = lonejson_default_read_result();
  result.bytes_read =
      cai_source_read(input->source, bytes, capacity, &input->error);
  input->bytes_read += result.bytes_read;
  result.eof = result.bytes_read == 0U && input->error.code == CAI_OK;
  result.error_code = input->error.code == CAI_OK ? 0 : EIO;
  return result;
}

static int parse_source_optional(cai_source *source, const lonejson_map *map,
                                 void *doc, int *has_body, int *string_limit,
                                 cai_error *error) {
  lonejson *json;
  lonejson_error local;
  int rc;
  lonejson_status status;
  char message[512];
  parse_input input;
  if (string_limit != NULL)
    *string_limit = 0;
  json = lonejson_new(NULL, &local);
  if (json == NULL)
    return cai_cli_lj_error(error, &local);
  input.source = source;
  input.bytes_read = 0U;
  cai_error_init(&input.error);
  status = lonejson_parse_reader(json, map, doc, parse_read, &input, &local);
  /* lonejson reports all size overflows with one status. Restrict recovery to
   * its dynamic-string ceiling; malformed JSON, fixed identity overflow and
   * source/transport errors must still fail normally. */
  if (string_limit != NULL && input.error.code == CAI_OK &&
      local.code == LONEJSON_STATUS_OVERFLOW &&
      strstr(local.message, "exceeds configured max dynamic string bytes") !=
          NULL)
    *string_limit = 1;
  if (has_body != NULL)
    *has_body = input.bytes_read != 0U;
  rc = status == LONEJSON_STATUS_OK ||
               (has_body != NULL && !*has_body && input.error.code == CAI_OK)
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

static int parse_source(cai_source *source, const lonejson_map *map, void *doc,
                        cai_error *error) {
  return parse_source_optional(source, map, doc, NULL, NULL, error);
}

static int session_read(cai_cli_pouch *pouch, const char *id,
                        cai_cli_session *out, int *has_body, cai_error *error) {
  cai_source *source;
  int rc;
  memset(out, 0, sizeof(*out));
  source = NULL;
  rc = download_source(pouch, id, NULL, &source, error);
  if (rc == CAI_OK)
    rc =
        parse_source_optional(source, &session_map, out, has_body, NULL, error);
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

int cai_cli_pouch_session(cai_cli_pouch *pouch, const char *id,
                          cai_cli_session *out, cai_error *error) {
  return session_read(pouch, id, out, NULL, error);
}

static int acquire_wait(cai_cli_pouch *pouch, const char *ns, const char *key,
                        long wait_seconds, lc_lease **out, cai_error *error) {
  lc_acquire_req request;
  lc_error local;
  int rc;
  lc_acquire_req_init(&request);
  lc_error_init(&local);
  request.ns = ns;
  request.key = key;
  request.owner = pouch->program_name;
  request.ttl_seconds = 3600;
  request.block_seconds = wait_seconds;
  rc = pouch->sessions->acquire(pouch->sessions, &request, out, &local);
  if (rc != LC_OK)
    rc = cai_cli_lc_error(error, &local);
  lc_error_cleanup(&local);
  return rc;
}

static int acquire_ns(cai_cli_pouch *pouch, const char *ns, const char *key,
                      lc_lease **out, cai_error *error) {
  return acquire_wait(pouch, ns, key, 5L, out, error);
}

static int acquire(cai_cli_pouch *pouch, const char *key, lc_lease **out,
                   cai_error *error) {
  return acquire_ns(pouch, NULL, key, out, error);
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
  rc = cai_cli_mapped_source(&session_map, session, &source, error);
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
  lc_lease *directory;
  latest_index index;
  struct timespec now;
  int rc;
  pouch = (cai_cli_pouch *)context;
  lease = NULL;
  directory = NULL;
  rc = latest_open(pouch, scope, &directory, &index, error);
  if (rc == CAI_OK) {
    index.dirty = 1;
    rc = latest_write(pouch, directory, &index, error);
  }
  if (rc == CAI_OK)
    rc = acquire(pouch, id, &lease, error);
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
  rc = release(lease, rc, error);
  if (rc == CAI_OK) {
    if (session.published && strcmp(session.id, index.latest.id) == 0 &&
        session.checkpoint_ns < index.latest.checkpoint_ns)
      rc = latest_rebuild(pouch, &index, error);
    else if (session.published &&
             (index.latest.id[0] == '\0' ||
              strcmp(session.id, index.latest.id) == 0 ||
              session_compare(&session, &index.latest) < 0))
      index.latest = session;
    if (rc == CAI_OK) {
      index.dirty = 0;
      rc = latest_write(pouch, directory, &index, error);
    }
  }
  return release(directory, rc, error);
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
  return download_source(pouch, id, session.checkpoint_name, out, error);
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
  key_page *page;
  cai_cli_session session;
  cai_cli_session *grown;
  char *cursor;
  char *selector;
  lonejson *json;
  lonejson_error json_error;
  size_t i;
  int rc;
  int exists;
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
  page = (key_page *)calloc(1U, sizeof(*page));
  if (page == NULL) {
    free(selector);
    lonejson_free(json);
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate session query page");
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
    memset(page, 0, sizeof(*page));
    memset(&result, 0, sizeof(result));
    query.cursor = cursor;
    if (lc_query_keys(pouch->sessions, &query, &handler, page, &result,
                      &local) != LC_OK)
      rc = cai_cli_lc_error(error, &local);
    free(cursor);
    cursor = result.cursor != NULL ? strdup(result.cursor) : NULL;
    if (result.cursor != NULL && cursor == NULL)
      rc = cai_cli_error(error, CAI_ERR_NOMEM, "allocate query cursor");
    lc_query_res_cleanup(&result);
    for (i = 0U; rc == CAI_OK && i < page->count; i++) {
      /* Removed or uncommitted imports can have indexed keys but no public
       * state body. Skip those; malformed nonempty metadata still fails. */
      rc = session_read(pouch, page->keys[i], &session, &exists, error);
      if (rc == CAI_OK && !exists)
        continue;
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
  free(page);
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

static int latest_write(cai_cli_pouch *pouch, lc_lease *guard,
                        latest_index *index, cai_error *error) {
  lc_lease *lease = NULL;
  cai_source *source = NULL;
  int rc = acquire_ns(pouch, pouch->scopes_namespace, guard->key + 5U, &lease,
                      error);
  if (rc == CAI_OK)
    rc = cai_cli_mapped_source(&latest_map, index, &source, error);
  if (rc == CAI_OK)
    rc = update(lease, source, error);
  cai_source_close(source);
  /* Lease updates are staged. Releasing this separate document lease commits
   * dirty before any session mutation, while guard still serializes writers. */
  return release(lease, rc, error);
}

static int latest_rebuild(cai_cli_pouch *pouch, latest_index *index,
                          cai_error *error) {
  cai_cli_session *sessions = NULL;
  size_t count = 0U;
  int rc =
      cai_cli_pouch_list(pouch, index->workspace, &sessions, &count, error);
  if (rc == CAI_OK) {
    memset(&index->latest, 0, sizeof(index->latest));
    if (count != 0U)
      index->latest = sessions[0];
    index->dirty = 0;
  }
  free(sessions);
  return rc;
}

static int latest_key(const char *scope, char key[65], cai_error *error) {
  unsigned char digest[32];
  unsigned int length;
  size_t i;
  if (strlen(scope) >= PATH_MAX)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "directory index scope too long");
  if (EVP_Digest(scope, strlen(scope), digest, &length, EVP_sha256(), NULL) !=
          1 ||
      length != sizeof(digest))
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "hash directory index key");
  for (i = 0U; i < sizeof(digest); i++)
    snprintf(key + i * 2U, 65U - i * 2U, "%02x", digest[i]);
  return CAI_OK;
}

static int latest_open_wait(cai_cli_pouch *pouch, const char *scope,
                            long wait_seconds, lc_lease **out,
                            latest_index *index, cai_error *error) {
  char key[70];
  lc_lease *head = NULL;
  lc_get_res response;
  lc_error local;
  int rc;
  *out = NULL;
  memset(index, 0, sizeof(*index));
  memcpy(key, "lock-", 5U);
  rc = latest_key(scope, key + 5U, error);
  if (rc != CAI_OK)
    return rc;
  rc = acquire_wait(pouch, pouch->scopes_namespace, key, wait_seconds, out,
                    error);
  if (rc != CAI_OK)
    return rc;
  rc = acquire_ns(pouch, pouch->scopes_namespace, key + 5U, &head, error);
  if (rc != CAI_OK)
    return rc;
  lc_error_init(&local);
  memset(&response, 0, sizeof(response));
  if (head->state_etag != NULL && head->state_etag[0] != '\0') {
    rc = head->load(head, &latest_map, index, NULL, &response, &local);
    if (rc != LC_OK)
      rc = cai_cli_lc_error(error, &local);
    else if (strcmp(scope, index->workspace) != 0)
      rc = cai_cli_error(error, CAI_ERR_INVALID,
                         "directory index scope mismatch");
  } else {
    strcpy(index->workspace, scope);
    index->dirty = 1;
  }
  lc_get_res_cleanup(&response);
  lc_error_cleanup(&local);
  rc = release(head, rc, error);
  if (rc == CAI_OK && index->dirty) {
    pslog_field field = pslog_str("workspace", scope);
    if (pouch->logger != NULL)
      pouch->logger->info(pouch->logger, "Rebuilding directory session index",
                          &field, 1U);
    rc = latest_rebuild(pouch, index, error);
    if (rc == CAI_OK)
      rc = latest_write(pouch, *out, index, error);
  }
  return rc;
}

static int latest_open(cai_cli_pouch *pouch, const char *scope, lc_lease **out,
                       latest_index *index, cai_error *error) {
  return latest_open_wait(pouch, scope, 5L, out, index, error);
}

static int load_latest(void *context, const char *scope, char *id,
                       size_t capacity, cai_source **out,
                       unsigned long long *sequence, cai_error *error) {
  cai_cli_pouch *pouch = context;
  lc_lease *directory = NULL;
  cai_source *source = NULL;
  latest_index index;
  char key[65];
  int has_body = 0;
  int rc;
  *out = NULL;
  memset(&index, 0, sizeof(index));
  rc = latest_key(scope, key, error);
  if (rc == CAI_OK)
    rc = download_source_ns(pouch->sessions, pouch->scopes_namespace, key, NULL,
                            &source, error);
  if (rc == CAI_OK)
    rc = parse_source_optional(source, &latest_map, &index, &has_body, NULL,
                               error);
  cai_source_close(source);
  if (rc != CAI_OK && error->http_status == 404) {
    cai_error_cleanup(error);
    cai_error_init(error);
    rc = CAI_OK;
    has_body = 0;
  }
  if (rc == CAI_OK && has_body && strcmp(scope, index.workspace) != 0)
    rc =
        cai_cli_error(error, CAI_ERR_INVALID, "directory index scope mismatch");
  if (rc == CAI_OK && (!has_body || index.dirty)) {
    cai_error optional;
    cai_error_init(&optional);
    rc = latest_open_wait(pouch, scope, 0L, &directory, &index, &optional);
    rc = release(directory, rc, &optional);
    /* A concurrent/interrupted writer must not delay prompt readiness. If its
     * guard is held, select authoritative committed metadata without changing
     * that writer's dirty index. The next writer repairs it under the guard. */
    if (rc != CAI_OK) {
      pslog_field fields[2];
      fields[0] = pslog_str("workspace", scope);
      fields[1] =
          pslog_str("reason", optional.message != NULL ? optional.message
                                                       : "index unavailable");
      if (pouch->logger != NULL)
        pouch->logger->debug(pouch->logger,
                             "Selecting newest session from metadata", fields,
                             2U);
      memset(&index, 0, sizeof(index));
      strcpy(index.workspace, scope);
      rc = latest_rebuild(pouch, &index, error);
    }
    cai_error_cleanup(&optional);
  }
  if (rc == CAI_OK && index.latest.id[0] != '\0') {
    if (strlen(index.latest.id) >= capacity)
      rc = cai_cli_error(error, CAI_ERR_INVALID, "session ID output too small");
    else {
      strcpy(id, index.latest.id);
      rc = load_id(context, scope, id, out, sequence, error);
    }
  }
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
  rc = acquire(pouch, id, &lease, error);
  if (rc == CAI_OK)
    rc = get_session(lease, scope, id, &session, error);
  if (rc == CAI_OK &&
      (lease->state_etag == NULL || lease->state_etag[0] == '\0'))
    rc = update_session(lease, &session, error);
  if (rc == CAI_OK)
    rc = cai_cli_mapped_source(&event_map, &record, &source, error);
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
  request.lease.ns = pouch->sessions_namespace;
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
  request.ns = pouch->sessions_namespace;
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
  int observe;
  int string_limit;
  pouch = (cai_cli_pouch *)context;
  observe = pthread_equal(pthread_self(), pouch->owner_thread) &&
            pouch->replay_scope != NULL &&
            strcmp(scope, pouch->replay_scope) == 0;
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
  if (rc == CAI_OK && observe && pouch->replay_begin != NULL)
    rc = pouch->replay_begin(pouch->replay_context, id, error);
  for (i = 0U; rc == CAI_OK && i < attachments.count; i++) {
    if (strncmp(attachments.items[i].name, "event-", 6U) != 0)
      continue;
    sequence = strtoull(attachments.items[i].name + 6U, NULL, 10);
    if (sequence <= after)
      continue;
    source = NULL;
    string_limit = 0;
    memset(&record, 0, sizeof(record));
    rc = download_source(pouch, id, attachments.items[i].name, &source, error);
    if (rc == CAI_OK)
      rc = parse_source_optional(source, &event_map, &record, NULL,
                                 &string_limit, error);
    cai_source_close(source);
    if (rc != CAI_OK && string_limit) {
      pslog_field fields[3];
      fields[0] = pslog_str("session_id", id);
      fields[1] = pslog_u64("sequence", sequence);
      fields[2] = pslog_str("reason", error->message);
      if (pouch->logger != NULL)
        pouch->logger->warn(
            pouch->logger,
            "Skipped oversized session event during replay; stored record "
            "preserved, continuing with the remaining conversation",
            fields, 3U);
      cai_error_cleanup(error);
      cai_error_init(error);
      /* Advance the runtime's journal watermark even when the final record
       * is omitted. Never reuse its sequence and overwrite stored history. */
      event.sequence = sequence;
      event.type = "replay_event_omitted";
      event.data = NULL;
      rc = visit(visitor, &event, error);
      lonejson_cleanup(&event_map, &record);
      continue;
    }
    if (rc == CAI_OK && (record.sequence != sequence ||
                         strcmp(record.record_type, "event") != 0))
      rc = cai_cli_error(error, CAI_ERR_INVALID,
                         "invalid stored event identity");
    if (rc == CAI_OK) {
      event.sequence = record.sequence;
      event.type = record.type;
      event.data = record.data;
      rc = visit(visitor, &event, error);
      if (rc == CAI_OK && observe && pouch->replay_event != NULL)
        rc = pouch->replay_event(pouch->replay_context, &event, error);
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
  request.ns = pouch->auth_namespace;
  rc = lc_describe(pouch->sessions, &request, &response, &local);
  if (rc != LC_OK && local.http_status != 404)
    rc = cai_cli_lc_error(error, &local);
  else {
    rc = response.state_etag != NULL && response.state_etag[0] != '\0'
             ? download_source_ns(pouch->sessions, pouch->auth_namespace, key,
                                  NULL, out, error)
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
  rc = acquire_ns(pouch, pouch->auth_namespace, key, &lease, error);
  if (rc == CAI_OK)
    rc = update(lease, source, error);
  return release(lease, rc, error);
}

typedef struct todo_transaction {
  cai_cli_pouch *pouch;
  lc_lease *lease;
  cai_sink *upload;
  unsigned int staged;
  char stage_name[16];
} todo_transaction;
static int todo_initial(void *context, cai_sink *sink, cai_error *error) {
  static const char empty[] =
      "{\"version\":1,\"boards\":[],\"items\":[],\"done\":[]}";
  (void)context;
  return cai_sink_write(sink, empty, sizeof(empty) - 1U, error);
}

static int todo_begin(void *context, void **out, cai_error *error) {
  todo_transaction *txn;
  cai_source *initial = NULL;
  int rc;
  *out = NULL;
  txn = calloc(1U, sizeof(*txn));
  if (txn == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate kanban transaction");
  txn->pouch = context;
  rc = acquire_ns(txn->pouch, txn->pouch->todo_namespace, "kanban", &txn->lease,
                  error);
  if (rc == CAI_OK &&
      (txn->lease->state_etag == NULL || txn->lease->state_etag[0] == '\0')) {
    rc = cai_cli_stream_source(todo_initial, NULL, NULL, &initial, error);
    if (rc == CAI_OK)
      rc = update(txn->lease, initial, error);
    cai_source_close(initial);
  }
  if (rc != CAI_OK) {
    release(txn->lease, rc, error);
    free(txn);
    return rc;
  }
  *out = txn;
  return CAI_OK;
}
static int todo_open_read(void *context, void *transaction,
                          lonejson_reader_fn *reader, void **reader_context,
                          cai_error *error) {
  todo_transaction *txn = transaction;
  cai_source *source = NULL;
  int rc;
  (void)context;
  rc = download_private(txn->lease, txn->staged ? txn->stage_name : NULL,
                        &source, error);
  *reader = cai_cli_json_read;
  *reader_context = source;
  return rc;
}
static void todo_close_read(void *context, void *transaction, void *reader) {
  (void)context;
  (void)transaction;
  cai_source_close(reader);
}
static int todo_upload(void *context, cai_source *source, cai_error *error) {
  todo_transaction *txn = context;
  lc_source *input = NULL;
  lc_attach_req request;
  lc_attach_res response;
  lc_error local;
  int rc;
  lc_attach_req_init(&request);
  memset(&response, 0, sizeof(response));
  lc_error_init(&local);
  request.name = txn->stage_name;
  request.content_type = "application/json";
  rc = cai_cli_lc_source(source, &input, error);
  if (rc == CAI_OK &&
      lc_lease_attach(txn->lease, &request, input, &response, &local) != LC_OK)
    rc = cai_cli_lc_error(error, &local);
  lc_source_close(input);
  lc_attach_res_cleanup(&response);
  lc_error_cleanup(&local);
  return rc;
}
static int todo_open_write(void *context, void *transaction,
                           lonejson_sink_fn *sink, void **sink_context,
                           cai_error *error) {
  todo_transaction *txn = transaction;
  int rc;
  (void)context;
  snprintf(txn->stage_name, sizeof(txn->stage_name), "rewrite-%u",
           txn->staged % 2U);
  rc = cai_cli_stream_sink(todo_upload, txn, &txn->upload, error);
  *sink = cai_cli_json_write;
  *sink_context = txn->upload;
  return rc;
}
static int todo_commit_write(void *context, void *transaction,
                             cai_error *error) {
  todo_transaction *txn = transaction;
  int rc;
  (void)context;
  rc = cai_cli_stream_finish(txn->upload, error);
  cai_sink_close(txn->upload);
  txn->upload = NULL;
  if (rc == CAI_OK)
    txn->staged++;
  return rc;
}
static void todo_cleanup(todo_transaction *txn) {
  lc_error error;
  int deleted;
  cai_sink_close(txn->upload);
  lc_error_init(&error);
  if (lc_lease_delete_all_attachments(txn->lease, &deleted, &error) != LC_OK &&
      txn->pouch->logger != NULL)
    txn->pouch->logger->warnf(
        txn->pouch->logger, "Kanban staging cleanup failed", "error=%s",
        error.message != NULL ? error.message : "lockd error");
  lc_error_cleanup(&error);
}
static int todo_commit(void *context, void *transaction, cai_error *error) {
  todo_transaction *txn = transaction;
  cai_source *source = NULL;
  int rc = CAI_OK;
  (void)context;
  if (txn->staged) {
    rc = download_private(txn->lease, txn->stage_name, &source, error);
    if (rc == CAI_OK)
      rc = update(txn->lease, source, error);
    cai_source_close(source);
  }
  todo_cleanup(txn);
  if (rc != CAI_OK) {
    lc_release_req request;
    lc_error local;
    lc_release_req_init(&request);
    lc_error_init(&local);
    request.rollback = 1;
    if (lc_lease_release(txn->lease, &request, &local) != LC_OK)
      lc_lease_close(txn->lease);
    lc_error_cleanup(&local);
  } else
    rc = release(txn->lease, rc, error);
  free(txn);
  return rc;
}
static void todo_rollback(void *context, void *transaction) {
  todo_transaction *txn = transaction;
  lc_error error;
  lc_release_req request;
  (void)context;
  lc_error_init(&error);
  cai_sink_close(txn->upload);
  lc_release_req_init(&request);
  request.rollback = 1;
  if (lc_lease_release(txn->lease, &request, &error) != LC_OK) {
    if (txn->pouch->logger != NULL)
      txn->pouch->logger->warnf(
          txn->pouch->logger, "Kanban rollback failed", "error=%s",
          error.message != NULL ? error.message : "lockd error");
    lc_lease_close(txn->lease);
  }
  lc_error_cleanup(&error);
  free(txn);
}
const cai_todo_store_callbacks *cai_cli_pouch_todo_callbacks(void) {
  static const cai_todo_store_callbacks callbacks = {
      todo_begin,        todo_open_read, todo_close_read, todo_open_write,
      todo_commit_write, todo_commit,    todo_rollback,   NULL};
  return &callbacks;
}

static int private_directory(const char *path, cai_error *error) {
  struct stat status;
  if (mkdir(path, 0700) != 0 && errno != EEXIST)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "create application state directory");
  if (lstat(path, &status) != 0 || !S_ISDIR(status.st_mode) ||
      status.st_uid != geteuid() || (status.st_mode & 077U) != 0U)
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "application state directory must be owned by you with mode 0700");
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

/* Auto-resume reads the directory cache and checkpoint directly, so it need
 * not execute a query that repairs a retired/corrupt search index. The public
 * wait flush keeps a healthy index's trusted fast path and repairs derived
 * state when needed. Failure must not prevent direct conversation replay. */
static void prepare_session_index(cai_cli_pouch *pouch) {
  lc_index_flush_req request;
  lc_index_flush_res result;
  lc_error local;
  lc_index_flush_req_init(&request);
  memset(&result, 0, sizeof(result));
  lc_error_init(&local);
  request.ns = pouch->sessions_namespace;
  request.mode = "wait";
  if (lc_flush_index(pouch->sessions, &request, &result, &local) != LC_OK &&
      pouch->logger != NULL)
    pouch->logger->warnf(
        pouch->logger,
        "Session search index recovery failed; session replay remains "
        "available",
        "ns=%s code=%d error=%s detail=%s", request.ns, local.code,
        local.message != NULL ? local.message : "index flush failed",
        local.detail != NULL ? local.detail : "");
  lc_index_flush_res_cleanup(&result);
  lc_error_cleanup(&local);
}

int cai_cli_pouch_open(cai_cli_pouch *pouch, const char *selected_endpoint,
                       const char *client_pem, pslog_logger *logger,
                       cai_error *error) {
  return cai_cli_pouch_open_at(pouch, NULL, selected_endpoint, client_pem,
                               logger, error);
}

int cai_cli_pouch_open_at(cai_cli_pouch *pouch, const char *state_directory,
                          const char *selected_endpoint, const char *client_pem,
                          pslog_logger *logger, cai_error *error) {
  cai_cli_options options;
  memset(&options, 0, sizeof(options));
  options.state_dir = state_directory;
  options.lockd = selected_endpoint;
  options.lockd_client_pem = client_pem;
  return cai_cli_pouch_open_options(pouch, &options, logger, error);
}

int cai_cli_pouch_open_options(cai_cli_pouch *pouch,
                               const cai_cli_options *options,
                               pslog_logger *logger, cai_error *error) {
  const char *selected_endpoint = options->lockd;
  const char *client_pem = options->lockd_client_pem;
  const char *name = options->program_name != NULL
                         ? options->program_name
                         : CAI_APP_DEFAULT_PROGRAM_NAME;
  const char *prefix =
      options->namespace_prefix != NULL ? options->namespace_prefix : name;
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
  char *decoded_root;
  int rc;
  memset(pouch, 0, sizeof(*pouch));
  pouch->logger = logger;
  if (options->config_dir != NULL) {
    if (strlen(options->config_dir) >= sizeof(pouch->config_directory))
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "configuration directory too long");
    strcpy(pouch->config_directory, options->config_dir);
  }
  if (!cai_cli_identity_valid(name))
    return cai_cli_error(error, CAI_ERR_INVALID, "invalid program_name");
  strcpy(pouch->program_name, name);
  name = options->queue_prefix != NULL ? options->queue_prefix : name;
  if (!cai_cli_identity_valid(name))
    return cai_cli_error(error, CAI_ERR_INVALID, "invalid queue_prefix");
  strcpy(pouch->queue_prefix, name);
#define NAMESPACE(kind)                                                        \
  rc =                                                                         \
      options->kind##_namespace != NULL                                        \
          ? snprintf(pouch->kind##_namespace, sizeof(pouch->kind##_namespace), \
                     "%s", options->kind##_namespace)                          \
          : snprintf(pouch->kind##_namespace, sizeof(pouch->kind##_namespace), \
                     "%s." #kind, prefix);                                     \
  if (rc <= 0 || (size_t)rc >= sizeof(pouch->kind##_namespace) ||              \
      strspn(pouch->kind##_namespace,                                          \
             "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKL"                          \
             "MNOPQRSTUVWXYZ0123456789._-") != (size_t)rc)                     \
  return cai_cli_error(error, CAI_ERR_INVALID, "invalid " #kind " namespace")
  NAMESPACE(sessions);
  NAMESPACE(auth);
  NAMESPACE(scopes);
  NAMESPACE(todo);
  NAMESPACE(messages);
#undef NAMESPACE
  if (options->state_dir != NULL) {
    rc = snprintf(path, sizeof(path), "%s", options->state_dir);
    if (rc <= 0 || (size_t)rc >= sizeof(path) || path[0] != '/')
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "state directory must be absolute and fit PATH_MAX");
  } else if (cai_cli_xdg_path(path, sizeof(path), "XDG_STATE_HOME",
                              ".local/state", pouch->program_name,
                              error) != CAI_OK)
    return error != NULL ? error->code : CAI_ERR_INVALID;
  rc = ensure_parents(path, error);
  if (rc == CAI_OK)
    rc = private_directory(path, error);
  if (rc != CAI_OK)
    return rc;
  if (realpath(path, pouch->state_directory) == NULL)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "resolve application state directory");
  rc = options->pouch_key_file != NULL
           ? snprintf(pouch->key_path, sizeof(pouch->key_path), "%s",
                      options->pouch_key_file)
           : snprintf(pouch->key_path, sizeof(pouch->key_path), "%s/pouch.key",
                      pouch->state_directory);
  if (rc <= 0 || (size_t)rc >= sizeof(pouch->key_path) ||
      pouch->key_path[0] != '/' ||
      ensure_parents(pouch->key_path, error) != CAI_OK)
    return cai_cli_error(error, CAI_ERR_INVALID, "invalid pouch key path");
  if (snprintf(root, sizeof(root), "%s/pouch", pouch->state_directory) >=
          (int)sizeof(root) ||
      snprintf(init_path, sizeof(init_path), "%s/pouch.init.lock",
               pouch->state_directory) >= (int)sizeof(init_path))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "application state path too long");
  local_pouch = selected_endpoint == NULL ||
                strncmp(selected_endpoint, "pouch://", 8U) == 0;
  if (selected_endpoint != NULL && local_pouch) {
    /* Unlike HTTP, pouch:// has no authority component. Match the SDK's
     * documented root syntax and use libcurl only for percent-decoding. */
    const char *encoded = selected_endpoint + 8U;
    size_t length = strcspn(encoded, "?#"), i;
    int decoded_length = 0;
    if (length == 0U || length > INT_MAX)
      return cai_cli_error(error, CAI_ERR_INVALID, "invalid pouch root URL");
    for (i = 0U; i < length; i++) {
      if (encoded[i] == '%') {
        if (length - i < 3U ||
            strchr("0123456789abcdefABCDEF", encoded[i + 1U]) == NULL ||
            strchr("0123456789abcdefABCDEF", encoded[i + 2U]) == NULL)
          return cai_cli_error(error, CAI_ERR_INVALID,
                               "invalid pouch root percent escape");
        i += 2U;
      }
    }
    decoded_root =
        curl_easy_unescape(NULL, encoded, (int)length, &decoded_length);
    if (decoded_root == NULL || decoded_length <= 0 || decoded_root[0] != '/' ||
        (size_t)decoded_length >= sizeof(root) ||
        memchr(decoded_root, '\0', (size_t)decoded_length) != NULL) {
      curl_free(decoded_root);
      return cai_cli_error(error, CAI_ERR_INVALID, "invalid pouch root URL");
    }
    strcpy(root, decoded_root);
    curl_free(decoded_root);
  }
  lock_fd = open(init_path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (lock_fd < 0 || flock(lock_fd, LOCK_EX) != 0) {
    if (lock_fd >= 0)
      close(lock_fd);
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "lock pouch initialization");
  }
  root_exists = lstat(root, &status) == 0;
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
      LC_POUCH_SETTING_CRYPTO_GENERATE_KEY_FILE | LC_POUCH_SETTING_QUEUE_WATCH;
  settings.single_writer = 0;
  settings.queue_watch = 1;
  settings.durable_sync = 1;
  settings.query_indexing_enabled = 1;
  settings.query_engine = "index";
  settings.crypto_key_file = pouch->key_path;
  settings.crypto_generate_key_file = !root_exists;
  endpoints[0] = endpoint;
  config.endpoints = endpoints;
  config.endpoint_count = 1U;
  config.default_namespace = pouch->sessions_namespace;
  config.pouch_settings = local_pouch ? &settings : NULL;
  config.client_bundle_path = client_pem;
  config.disable_mtls = client_pem == NULL;
  if (rc == LC_OK)
    rc = shared_client_open(pouch, &config, root, local_pouch, client_pem,
                            error);
  else
    rc = cai_cli_lc_error(error, &local);
  lc_pouch_endpoint_free(endpoint);
  lc_error_cleanup(&local);
  close(lock_fd);
  if (rc != CAI_OK) {
    cai_cli_pouch_close(pouch);
    return rc;
  }
  pouch->owner_thread = pthread_self();
  if (local_pouch)
    prepare_session_index(pouch);
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
  cai_cli_pouch_client **link, *entry;
  pthread_mutex_lock(&clients_lock);
  entry = pouch->connection;
  if (entry != NULL && --entry->references == 0U) {
    for (link = &clients; *link != entry; link = &(*link)->next)
      ;
    *link = entry->next;
    entry->client->close(entry->client);
    (void)cai_cli_log_release(entry->logging);
    free(entry->endpoint);
    free(entry->client_pem);
    free(entry);
  }
  pthread_mutex_unlock(&clients_lock);
  pouch->connection = NULL;
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
      rc = cai_cli_mapped_source(&auth_origin_map, &current, &stored, error);
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
  latest_index index;
  lc_lease *lease = NULL;
  lc_lease *directory = NULL;
  int rc = cai_cli_pouch_session(pouch, id, &session, error);
  if (rc == CAI_OK)
    rc = latest_open(pouch, session.workspace, &directory, &index, error);
  if (rc == CAI_OK) {
    index.dirty = 1;
    rc = latest_write(pouch, directory, &index, error);
  }
  if (rc == CAI_OK)
    rc = acquire(pouch, id, &lease, error);
  if (rc == CAI_OK)
    rc = get_session(lease, index.workspace, id, &session, error);
  if (rc == CAI_OK) {
    session.published = published;
    rc = update_session(lease, &session, error);
  }
  rc = release(lease, rc, error);
  if (rc == CAI_OK) {
    if (!published && strcmp(index.latest.id, id) == 0)
      rc = latest_rebuild(pouch, &index, error);
    else if (published && session.checkpoint_name[0] != '\0' &&
             (index.latest.id[0] == '\0' ||
              session_compare(&session, &index.latest) < 0))
      index.latest = session;
    if (rc == CAI_OK) {
      index.dirty = 0;
      rc = latest_write(pouch, directory, &index, error);
    }
  }
  return release(directory, rc, error);
}

int cai_cli_pouch_lock(cai_cli_pouch *pouch, const char *id, lc_lease **out,
                       cai_error *error) {
  return acquire(pouch, id, out, error);
}

int cai_cli_pouch_unlock(lc_lease *lease, int result, cai_error *error) {
  return release(lease, result, error);
}

int cai_cli_pouch_export(cai_cli_pouch *pouch, const char *id,
                         cai_agent_runtime *runtime, cai_sink *sink,
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
    rc = download_source(pouch, id, session.checkpoint_name, &state, error);
  if (rc == CAI_OK) {
    strcpy(record.record_type, "checkpoint");
    record.checkpoint_created_at_ns = session.checkpoint_ns;
    record.applied_event_sequence = session.applied_sequence;
    lonejson_json_value_init(NULL, &record.state);
    if (lonejson_json_value_set_reader(&record.state, cai_cli_json_read, state,
                                       &local) != LONEJSON_STATUS_OK)
      rc = cai_cli_lj_error(error, &local);
    else
      rc = cai_cli_mapped_source(&checkpoint_map, &record, &source, error);
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
    if (!cai_agent_runtime_history_event_visible(
            runtime, strtoull(attachments.items[i].name + 6U, NULL, 10)))
      continue;
    source = NULL;
    rc = download_source(pouch, id, attachments.items[i].name, &source, error);
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
    rc = acquire(pouch, id, &lease, error);
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
    rc = acquire(pouch, id, &lease, error);
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
    if (acquire(pouch, id, &lease, &ignored) == CAI_OK) {
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
