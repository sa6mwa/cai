#define _POSIX_C_SOURCE 200809L
#include "../src/cai_internal.h"
#include <assert.h>
#include <cai/agent_runtime.h>
#include <cai/session_store.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(expr)                                                            \
  do {                                                                         \
    if (!(expr)) {                                                             \
      fprintf(stderr, "shell check failed at %d: %s\n", __LINE__, #expr);      \
      abort();                                                                 \
    }                                                                          \
  } while (0)

typedef struct buffer {
  char *data;
  size_t length;
  size_t capacity;
  size_t offset;
} buffer;
typedef struct record {
  unsigned long long sequence;
  char *type;
  char *data;
} record;
typedef struct fixture {
  pthread_mutex_t lock;
  buffer checkpoint;
  unsigned long long watermark;
  record records[256];
  size_t count;
  const char *fail_type;
  int fail_checkpoint;
  int fail_output_at;
  size_t output_bytes;
  int turns;
  int steering;
  int failed;
} fixture;

static int capture(void *context, const void *bytes, size_t count,
                   cai_error *error) {
  buffer *b = context;
  (void)error;
  CHECK(count < 1048576U && b->length + count < 1048576U);
  if (b->length + count + 1U > b->capacity) {
    b->capacity = (b->length + count + 1U) * 2U;
    b->data = realloc(b->data, b->capacity);
    CHECK(b->data != NULL);
  }
  memcpy(b->data + b->length, bytes, count);
  b->length += count;
  b->data[b->length] = '\0';
  return CAI_OK;
}
static size_t source_read(void *context, void *bytes, size_t count,
                          cai_error *error) {
  buffer *b = context;
  (void)error;
  if (count > b->length - b->offset)
    count = b->length - b->offset;
  memcpy(bytes, b->data + b->offset, count);
  b->offset += count;
  return count;
}
static int checkpoint(void *context, const char *scope, const char *id,
                      cai_source *source, unsigned long long watermark,
                      cai_error *error) {
  fixture *f = context;
  buffer next;
  char bytes[4096];
  size_t count;
  (void)scope;
  (void)id;
  memset(&next, 0, sizeof(next));
  pthread_mutex_lock(&f->lock);
  if (f->fail_checkpoint) {
    pthread_mutex_unlock(&f->lock);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "injected shell checkpoint failure");
  }
  while ((count = cai_source_read(source, bytes, sizeof(bytes), error)) != 0U)
    CHECK(capture(&next, bytes, count, error) == CAI_OK);
  free(f->checkpoint.data);
  f->checkpoint = next;
  f->watermark = watermark;
  pthread_mutex_unlock(&f->lock);
  return error->code;
}
static int load(void *context, const char *scope, char *id, size_t capacity,
                cai_source **out, unsigned long long *watermark,
                cai_error *error) {
  fixture *f = context;
  cai_source_callbacks callbacks;
  (void)scope;
  CHECK(capacity > strlen("shell-test"));
  strcpy(id, "shell-test");
  *watermark = f->watermark;
  f->checkpoint.offset = 0U;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.read = source_read;
  callbacks.context = &f->checkpoint;
  return cai_source_from_callbacks(&callbacks, out, error);
}
static int append(void *context, const char *scope, const char *id,
                  const cai_agent_session_event *event, cai_error *error) {
  fixture *f = context;
  record *r;
  (void)scope;
  (void)id;
  pthread_mutex_lock(&f->lock);
  if ((f->fail_type != NULL && strcmp(event->type, f->fail_type) == 0) ||
      (strcmp(event->type, "user_shell_output_hex") == 0 &&
       f->fail_output_at > 0 && --f->fail_output_at == 0)) {
    f->fail_type = NULL;
    pthread_mutex_unlock(&f->lock);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "injected shell journal failure");
  }
  CHECK(f->count < 256U);
  r = &f->records[f->count++];
  r->sequence = event->sequence;
  r->type = strdup(event->type);
  r->data = event->data != NULL ? strdup(event->data) : NULL;
  CHECK(r->type != NULL && (event->data == NULL || r->data != NULL));
  if (strcmp(event->type, "user_shell_output_hex") == 0) {
    CHECK(strlen(r->data) <= 8192U && strlen(r->data) % 2U == 0U);
    f->output_bytes += strlen(r->data) / 2U;
  }
  pthread_mutex_unlock(&f->lock);
  return CAI_OK;
}
static int replay(void *context, const char *scope, const char *id,
                  unsigned long long after, cai_agent_session_event_fn callback,
                  void *callback_context, cai_error *error) {
  fixture *f = context;
  size_t i, count = f->count;
  cai_agent_session_event event;
  int rc;
  (void)scope;
  (void)id;
  for (i = 0U; i < count; i++) {
    if (f->records[i].sequence <= after)
      continue;
    event.sequence = f->records[i].sequence;
    event.type = f->records[i].type;
    event.data = f->records[i].data;
    rc = callback(callback_context, &event, error);
    if (rc != CAI_OK)
      return rc;
  }
  return CAI_OK;
}
static int event(void *context, const cai_agent_runtime_event *event,
                 cai_error *error) {
  fixture *f = context;
  (void)error;
  if (event->type == CAI_AGENT_EVENT_RUN_STARTED)
    f->turns++;
  if (event->type == CAI_AGENT_EVENT_STEERING_DELIVERED ||
      event->type == CAI_AGENT_EVENT_STEERING_QUEUED)
    f->steering++;
  if (event->type == CAI_AGENT_EVENT_RUN_FAILED)
    f->failed++;
  return CAI_OK;
}
static void clear_error(cai_error *error) {
  cai_error_cleanup(error);
  cai_error_init(error);
}
static void wait_export(cai_agent_runtime *runtime, buffer *out,
                        cai_error *error) {
  cai_sink_callbacks callbacks;
  cai_sink *sink = NULL;
  struct timespec delay;
  int i, rc;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = capture;
  callbacks.context = out;
  CHECK(cai_sink_from_callbacks(&callbacks, &sink, error) == CAI_OK);
  delay.tv_sec = 0;
  delay.tv_nsec = 1000000L;
  for (i = 0; i < 2000; i++) {
    clear_error(error);
    CHECK(cai_agent_runtime_pump(runtime, 0L, error) == CAI_OK);
    out->length = 0U;
    rc = cai_agent_runtime_export_markdown(runtime, sink, error);
    if (rc == CAI_OK)
      break;
    CHECK(rc == CAI_ERR_INVALID);
    nanosleep(&delay, NULL);
  }
  CHECK(i < 2000);
  cai_sink_close(sink);
}
static size_t occurrences(const char *text, const char *needle) {
  size_t n = 0U;
  while ((text = strstr(text, needle)) != NULL) {
    n++;
    text += strlen(needle);
  }
  return n;
}
static void failed_append(fixture *f, const char *type) {
  pthread_mutex_lock(&f->lock);
  f->fail_type = type;
  pthread_mutex_unlock(&f->lock);
}

int main(int argc, char **argv) {
  fixture f;
  buffer exported;
  cai_client *client = NULL;
  cai_agent_runtime *runtime = NULL;
  cai_client_config client_config;
  cai_agent_runtime_config config;
  cai_agent_session_store store;
  cai_agent_runtime_metrics metrics;
  cai_agent_run_state state;
  cai_error error;
  struct timespec delay;
  unsigned char raw[70000];
  size_t i;
  int protocol, rc;
  CHECK(argc == 2);
  for (protocol = 0; protocol < 2; protocol++) {
    memset(&f, 0, sizeof(f));
    memset(&exported, 0, sizeof(exported));
    CHECK(pthread_mutex_init(&f.lock, NULL) == 0);
    memset(&store, 0, sizeof(store));
    store.checkpoint = checkpoint;
    store.load_latest = load;
    store.append_event = append;
    store.load_events_after = replay;
    store.context = &f;
    cai_error_init(&error);
    cai_client_config_init(&client_config);
    client_config.api_key = "fixture";
    client_config.base_url = "http://127.0.0.1:1/v1";
    client_config.api_protocol = protocol ? CAI_API_PROTOCOL_CHAT_COMPLETIONS
                                          : CAI_API_PROTOCOL_RESPONSES;
    CHECK(cai_client_open(&client_config, &client, &error) == CAI_OK);
    cai_agent_runtime_config_init(&config);
    config.workspace_directory = argv[1];
    config.model = CAI_MODEL_GPT_5_NANO;
    config.session_id = "shell-test";
    config.session_store = &store;
    config.event_callback = event;
    config.event_context = &f;
    CHECK(cai_agent_runtime_open(client, &config, &runtime, &error) == CAI_OK);
    CHECK(cai_agent_runtime_user_shell_write(runtime, "x", 1U, &error) ==
          CAI_ERR_INVALID);
    clear_error(&error);
    CHECK(cai_agent_runtime_user_shell_finish(runtime, 0, 0U, &error) ==
          CAI_ERR_INVALID);
    clear_error(&error);
    CHECK(cai_agent_runtime_user_shell_begin(runtime, "", 1, &error) ==
          CAI_ERR_INVALID);
    clear_error(&error);
    failed_append(&f, "user_shell_started");
    CHECK(cai_agent_runtime_user_shell_begin(runtime, "will-not-start", 1,
                                             &error) == CAI_ERR_TRANSPORT);
    clear_error(&error);
    CHECK(cai_agent_runtime_user_shell_begin(runtime, "streamed-user-command",
                                             1, &error) == CAI_OK);
    CHECK(cai_agent_runtime_user_shell_begin(runtime, "second", 1, &error) ==
          CAI_ERR_INVALID);
    clear_error(&error);
    CHECK(cai_agent_runtime_user_shell_write(runtime, NULL, 1U, &error) ==
          CAI_ERR_INVALID);
    clear_error(&error);
    memset(raw, 'L', sizeof(raw));
    raw[0] = 0U;
    raw[1] = 27U;
    raw[2] = 255U;
    raw[3] = 0xe2U;
    raw[4] = 0x82U;
    raw[5] = 0xacU;
    memcpy(raw + sizeof(raw) - 9U, "RAW-TAIL!", 9U);
    CHECK(cai_agent_runtime_user_shell_write(runtime, raw, 4U, &error) ==
          CAI_OK);
    CHECK(cai_agent_runtime_user_shell_write(
              runtime, raw + 4U, sizeof(raw) - 4U, &error) == CAI_OK);
    CHECK(f.output_bytes == sizeof(raw));
    failed_append(&f, "user_shell_completed");
    CHECK(cai_agent_runtime_user_shell_finish(runtime, 7, 1234U, &error) ==
          CAI_ERR_TRANSPORT);
    clear_error(&error);
    CHECK(cai_agent_runtime_user_shell_finish(runtime, 7, 1234U, &error) ==
          CAI_OK);
    wait_export(runtime, &exported, &error);
    CHECK(strstr(exported.data, "\\x00\\x1b\\xff€") != NULL);
    CHECK(strstr(exported.data, "Exit code: 7") != NULL &&
          strstr(exported.data, "1.234 seconds") != NULL);
    CHECK(strstr(exported.data, "70000 raw bytes total") != NULL);
    CHECK(strstr(exported.data, "RAW-TAIL!") == NULL);
    CHECK(occurrences(exported.data, "<user_shell_command>") == 1U);
    CHECK(cai_agent_runtime_get_metrics(runtime, &metrics, &error) == CAI_OK);
    CHECK(!metrics.turn_active && !metrics.has_last_turn);
    CHECK(f.turns == 0 && f.steering == 0 && f.failed == 0);
    CHECK(cai_agent_runtime_user_shell_begin(runtime, "private-command", 0,
                                             &error) == CAI_OK);
    CHECK(cai_agent_runtime_user_shell_write(runtime, "private-result", 14U,
                                             &error) == CAI_OK);
    CHECK(cai_agent_runtime_user_shell_finish(runtime, 0, 0U, &error) ==
          CAI_OK);
    wait_export(runtime, &exported, &error);
    CHECK(strstr(exported.data, "private-result") == NULL);

    CHECK(cai_agent_runtime_user_shell_begin(runtime, "partial-failed-write", 0,
                                             &error) == CAI_OK);
    pthread_mutex_lock(&f.lock);
    f.fail_output_at = 2;
    i = f.output_bytes;
    pthread_mutex_unlock(&f.lock);
    CHECK(cai_agent_runtime_user_shell_write(runtime, raw, 9000U, &error) ==
          CAI_ERR_TRANSPORT);
    clear_error(&error);
    CHECK(f.output_bytes == i + 4096U);
    CHECK(cai_agent_runtime_user_shell_write(runtime, "new bytes", 9U,
                                             &error) == CAI_OK);
    CHECK(cai_agent_runtime_user_shell_finish(runtime, 1, 100U, &error) ==
          CAI_OK);

    /* A completed record survives a failed checkpoint. A consumption marker
     * beyond the saved watermark must not suppress recovery or start a turn. */
    pthread_mutex_lock(&f.lock);
    f.fail_checkpoint = 1;
    pthread_mutex_unlock(&f.lock);
    CHECK(cai_agent_runtime_user_shell_begin(runtime, "recover-command", 1,
                                             &error) == CAI_OK);
    CHECK(cai_agent_runtime_user_shell_write(runtime, "recover-result", 14U,
                                             &error) == CAI_OK);
    CHECK(cai_agent_runtime_user_shell_finish(runtime, 0, 500U, &error) ==
          CAI_OK);
    delay.tv_sec = 0;
    delay.tv_nsec = 1000000L;
    for (i = 0U; i < 2000U; i++) {
      CHECK(cai_agent_runtime_pump(runtime, 0L, &error) == CAI_OK);
      CHECK(cai_agent_runtime_state(runtime, &state, &error) == CAI_OK);
      if (state == CAI_AGENT_FAILED)
        break;
      nanosleep(&delay, NULL);
    }
    CHECK(i < 2000U && f.failed == 1);
    cai_agent_runtime_close(runtime);
    runtime = NULL;
    f.fail_checkpoint = 0;
    config.resume_latest = 1;
    clear_error(&error);
    rc = cai_agent_runtime_open(client, &config, &runtime, &error);
    if (rc != CAI_OK)
      fprintf(stderr, "recovery error: %d %s %s\n", rc,
              error.message != NULL ? error.message : "",
              error.detail != NULL ? error.detail : "");
    CHECK(rc == CAI_OK);
    wait_export(runtime, &exported, &error);
    CHECK(occurrences(exported.data, "<user_shell_command>") == 2U);
    CHECK(occurrences(exported.data, "recover-result") == 1U);
    CHECK(strstr(exported.data, "private-result") == NULL);
    CHECK(f.turns == 0 && f.steering == 0);
    CHECK(cai_agent_runtime_user_shell_begin(runtime, "unfinished-command", 1,
                                             &error) == CAI_OK);
    CHECK(cai_agent_runtime_user_shell_write(runtime, "unfinished-result", 17U,
                                             &error) == CAI_OK);
    /* Reopening cannot inject completed context twice or execute an unfinished
     * command. */
    cai_agent_runtime_close(runtime);
    runtime = NULL;
    CHECK(cai_agent_runtime_open(client, &config, &runtime, &error) == CAI_OK);
    wait_export(runtime, &exported, &error);
    CHECK(occurrences(exported.data, "recover-result") == 1U);
    CHECK(strstr(exported.data, "unfinished-result") == NULL);
    CHECK(cai_agent_runtime_user_shell_begin(
              runtime, "after unfinished recording", 0, &error) == CAI_OK);
    CHECK(cai_agent_runtime_user_shell_finish(runtime, 0, 0U, &error) ==
          CAI_OK);
    cai_agent_runtime_close(runtime);
    runtime = NULL;
    cai_client_close(client);
    client = NULL;
    cai_error_cleanup(&error);
    free(exported.data);
    free(f.checkpoint.data);
    for (i = 0U; i < f.count; i++) {
      free(f.records[i].type);
      free(f.records[i].data);
    }
    pthread_mutex_destroy(&f.lock);
  }
  return 0;
}
