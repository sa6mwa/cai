#include "../cli/app_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct status_test {
  cai_app_bridge *bridge;
  cai_agent_runtime *runtime;
  cai_app_status before;
  unsigned int commits;
  int armed;
} status_test;

static void *allocate(void *context, size_t size) {
  return *(int *)context ? NULL : malloc(size);
}
static void *resize(void *context, void *pointer, size_t size) {
  return *(int *)context ? NULL : realloc(pointer, size);
}
static void release(void *context, void *pointer) {
  (void)context;
  free(pointer);
}

static void same_snapshot(const cai_app_status *a, const cai_app_status *b) {
  assert(a->running == b->running && a->runtime_ready == b->runtime_ready);
  assert(a->outcome == b->outcome && a->state == b->state);
  assert(a->control_id == b->control_id);
  assert(a->error.code == b->error.code);
  assert(a->pending.present == b->pending.present);
  assert(strcmp(a->session_id, b->session_id) == 0);
  assert(strcmp(a->effective.model, b->effective.model) == 0);
  assert(strcmp(a->effective.reasoning_effort, b->effective.reasoning_effort) ==
         0);
}

static int commit(void *context, const char *scope, const char *id,
                  cai_source *source, unsigned long long sequence,
                  cai_error *error) {
  status_test *test = context;
  cai_app_status after;
  cai_agent_runtime_settings effective;
  cai_error local;
  char chunk[1024];
  long bytes;
  (void)scope;
  (void)id;
  (void)sequence;
  /* Consume the real commit source without storing a complete document. */
  while ((bytes = cai_source_read(source, chunk, sizeof(chunk), error)) > 0) {
  }
  if (bytes < 0)
    return error->code;
  if (!test->armed)
    return CAI_OK;
  test->commits++;
  cai_error_init(&local);
  cai_app_status_init(&after);
  /* Settings are deliberately unreadable while this durable change commits.
   * The callback runs on the owner thread, so publication can be tested at
   * exactly that boundary without timing or thread races. */
  assert(cai_agent_runtime_get_settings(test->runtime, &effective, NULL,
                                        &local) == CAI_ERR_INVALID);
  assert(strcmp(local.message, "runtime settings are being applied") == 0);
  cai_error_cleanup(&local);
  assert(cai_app_bridge_publish(test->bridge, test->runtime, &local) == CAI_OK);
  assert(local.code == CAI_OK);
  assert(cai_app_bridge_status(test->bridge, &after, &local) == CAI_OK);
  same_snapshot(&test->before, &after);
  cai_app_status_cleanup(&after);
  cai_error_cleanup(&local);
  return CAI_OK;
}

static int load(void *context, const char *scope, char *id, size_t capacity,
                cai_source **out, unsigned long long *sequence,
                cai_error *error) {
  (void)context;
  (void)scope;
  (void)id;
  (void)capacity;
  (void)error;
  *out = NULL;
  *sequence = 0U;
  return CAI_OK;
}
static int append(void *context, const char *scope, const char *id,
                  const cai_agent_session_event *event, cai_error *error) {
  (void)context;
  (void)scope;
  (void)id;
  (void)event;
  (void)error;
  return CAI_OK;
}
static int events(void *context, const char *scope, const char *id,
                  unsigned long long sequence,
                  cai_agent_session_event_fn callback, void *callback_context,
                  cai_error *error) {
  (void)context;
  (void)scope;
  (void)id;
  (void)sequence;
  (void)callback;
  (void)callback_context;
  (void)error;
  return CAI_OK;
}

static void *wrong_thread(void *context) {
  status_test *test = context;
  cai_error error;
  cai_error_init(&error);
  /* A genuine caller error must still propagate, rather than being deferred. */
  assert(cai_app_bridge_publish(test->bridge, test->runtime, &error) ==
         CAI_ERR_INVALID);
  assert(error.code == CAI_ERR_INVALID);
  cai_error_cleanup(&error);
  return NULL;
}

int main(int argc, char **argv) {
  status_test test;
  cai_app_config app;
  cai_agent_runtime_config runtime;
  cai_agent_session_store store;
  cai_agent_runtime_settings change;
  cai_client_config config;
  cai_client *client = NULL;
  cai_error error;
  cai_app_status after;
  pthread_t thread;
  unsigned int mode;
  int deny_allocations = 0;
  assert(argc == 2);
  memset(&test, 0, sizeof(test));
  memset(&store, 0, sizeof(store));
  store.commit = commit;
  store.load_latest = load;
  store.append_event = append;
  store.load_events_after = events;
  store.context = &test;
  cai_error_init(&error);
  cai_app_config_init(&app);
  assert(cai_app_bridge_open(&test.bridge, &app, &error) == CAI_OK);
  cai_app_bridge_begin(test.bridge);
  cai_client_config_init(&config);
  config.allocator.malloc_fn = allocate;
  config.allocator.realloc_fn = resize;
  config.allocator.free_fn = release;
  config.allocator.context = &deny_allocations;
  config.api_key = "fixture";
  config.base_url = "http://127.0.0.1:1/v1";
  config.metadata_provider = "custom";
  config.model_metadata_json =
      "{\"models\":[{\"provider\":\"custom\",\"id\":\"status-a\","
      "\"reasoning_efforts\":[\"medium\",\"high\"],\"comp_hash\":\"same\"},"
      "{\"provider\":\"custom\",\"id\":\"status-b\","
      "\"reasoning_efforts\":[\"medium\",\"high\"],\"comp_hash\":\"same\"}]}";
  assert(cai_client_open(&config, &client, &error) == CAI_OK);
  cai_agent_runtime_config_init(&runtime);
  runtime.workspace_directory = argv[1];
  runtime.model = "status-a";
  runtime.disable_terminal = runtime.disable_chrome = runtime.disable_skills =
      1;
  runtime.session_store = &store;
  assert(cai_agent_runtime_open(client, &runtime, &test.runtime, &error) ==
         CAI_OK);
  assert(cai_app_bridge_publish(test.bridge, test.runtime, &error) == CAI_OK);
  for (mode = 0U; mode < 2U; mode++) {
    assert(cai_app_bridge_status(test.bridge, &test.before, &error) == CAI_OK);
    memset(&change, 0, sizeof(change));
    if (mode == 0U) {
      change.present = CAI_AGENT_RUNTIME_SETTING_REASONING_EFFORT;
      change.reasoning_effort = "high";
    } else {
      change.present = CAI_AGENT_RUNTIME_SETTING_MODEL;
      change.model = "status-b";
    }
    test.armed = 1;
    assert(cai_agent_runtime_update_settings(test.runtime, &change, NULL,
                                             &error) == CAI_OK);
    test.armed = 0;
    assert(cai_app_bridge_publish(test.bridge, test.runtime, &error) == CAI_OK);
    assert(cai_app_bridge_status(test.bridge, &test.before, &error) == CAI_OK);
    assert(strcmp(test.before.effective.reasoning_effort, "high") == 0);
    assert(strcmp(test.before.effective.model,
                  mode == 0U ? "status-a" : "status-b") == 0);
  }
  assert(test.commits == 2U);
  assert(pthread_create(&thread, NULL, wrong_thread, &test) == 0);
  assert(pthread_join(thread, NULL) == 0);
  deny_allocations = 1;
  assert(cai_app_bridge_publish(test.bridge, test.runtime, &error) ==
         CAI_ERR_NOMEM);
  assert(error.code == CAI_ERR_NOMEM);
  deny_allocations = 0;
  cai_error_cleanup(&error);
  cai_app_status_init(&after);
  assert(cai_app_bridge_status(test.bridge, &after, &error) == CAI_OK);
  same_snapshot(&test.before, &after);
  cai_app_status_cleanup(&after);
  cai_agent_runtime_close(test.runtime);
  cai_client_close(client);
  cai_app_status_cleanup(&test.before);
  cai_app_bridge_close(test.bridge);
  cai_error_cleanup(&error);
  return 0;
}
