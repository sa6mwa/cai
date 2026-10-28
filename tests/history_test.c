#define _POSIX_C_SOURCE 200809L
#include "../src/cai_internal.h"
#include "session_journal_fixture.inc"
#include <assert.h>
#include <cai/agent_runtime.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct fixture {
  char *state;
  char *journal;
  char input[32768];
  size_t offset;
  unsigned long long watermark;
  int fail_commit;
  int expect_prefix_edit;
  size_t seen;
  size_t entries;
  int reject;
} fixture;

static size_t read_state(void *context, void *bytes, size_t count,
                         cai_error *error) {
  fixture *f = context;
  size_t left = strlen(f->input) - f->offset;
  (void)error;
  if (count > left)
    count = left;
  memcpy(bytes, f->input + f->offset, count);
  f->offset += count;
  return count;
}
static int load(void *context, const char *scope, char *id, size_t capacity,
                cai_source **out, unsigned long long *watermark,
                cai_error *error) {
  fixture *f = context;
  cai_source_callbacks callbacks;
  (void)scope;
  assert(capacity > 8U);
  strcpy(id, "history");
  *watermark = f->watermark;
  assert(test_journal_fixture(f->state, 0, f->watermark, f->input,
                              sizeof(f->input), error) == CAI_OK);
  free(f->journal);
  f->journal = strdup(f->input);
  f->offset = 0U;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.read = read_state;
  callbacks.context = f;
  return cai_source_from_callbacks(&callbacks, out, error);
}
static int commit(void *context, const char *scope, const char *id,
                  cai_source *source, unsigned long long watermark,
                  cai_error *error) {
  fixture *f = context;
  char bytes[4096];
  char *state = NULL;
  size_t used = 0U, n;
  (void)scope;
  (void)id;
  if (f->fail_commit)
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "injected history commit failure");
  while ((n = cai_source_read(source, bytes, sizeof(bytes), error)) != 0U) {
    state = realloc(state, used + n + 1U);
    assert(state != NULL);
    memcpy(state + used, bytes, n);
    used += n;
    state[used] = '\0';
  }
  assert(state != NULL);
  if (f->expect_prefix_edit) {
    assert(strstr(state, "\"history_append\":\"\"") != NULL);
    f->expect_prefix_edit = 0;
  }
  {
    size_t old = f->journal != NULL ? strlen(f->journal) : 0U;
    if (strstr(state, "\"revision\":1,") != NULL)
      old = 0U;
    f->journal = realloc(f->journal, old + used + 100U);
    assert(f->journal != NULL);
    snprintf(f->journal + old, used + 100U,
             "{\"record_type\":\"commit\",\"applied_event_sequence\":%llu,"
             "\"state\":%s}\n",
             watermark, state);
    f->state = realloc(f->state, 32768U);
    assert(f->state != NULL);
    assert(test_journal_fixture(f->journal, 1, 0U, f->state, 32768U, error) ==
           CAI_OK);
    free(state);
  }
  f->watermark = watermark;
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
                  unsigned long long after, cai_agent_session_event_fn callback,
                  void *visitor, cai_error *error) {
  static const char *types[] = {"input_journal_v2",
                                "turn_submitted",
                                "input_consumed",
                                "assistant_text_delta",
                                "turn_queued",
                                "assistant_text_delta",
                                "input_consumed",
                                "steering_queued",
                                "assistant_text_delta",
                                "input_consumed",
                                "user_shell_group",
                                "user_shell_started",
                                "user_shell_output_tail",
                                "user_shell_completed",
                                "user_shell_context_consumed",
                                "history_unused"};
  static const char *data[] = {
      NULL,
      "keep-prompt",
      "2",
      "keep-answer",
      "drop-prompt",
      "keep-footer",
      "5",
      "steer-prompt",
      "drop-answer",
      "8",
      "entry",
      "shell-command",
      "shell-output",
      "<user_shell_command>shell-output</user_shell_command>",
      "14",
      NULL};
  cai_agent_session_event event;
  size_t i;
  int rc;
  (void)context;
  (void)scope;
  (void)id;
  for (i = 0U; i < sizeof(types) / sizeof(types[0]); i++) {
    event.sequence = (unsigned long long)i + 1U;
    if (event.sequence <= after)
      continue;
    event.type = types[i];
    event.data = data[i];
    rc = callback(visitor, &event, error);
    if (rc != CAI_OK)
      return rc;
  }
  return CAI_OK;
}
static int observed(void *context, const cai_agent_session_event *event,
                    cai_error *error) {
  fixture *f = context;
  (void)error;
  if (event->sequence == 5U || event->sequence == 8U || event->sequence == 9U ||
      (event->sequence >= 12U && event->sequence <= 15U))
    f->seen++;
  return CAI_OK;
}
static int entry(void *context, const cai_agent_history_entry *row,
                 cai_error *error) {
  fixture *f = context;
  if (f->reject)
    return cai_set_error(error, CAI_ERR_TRANSPORT, "injected visitor failure");
  f->entries++;
  assert(row->remove_count == f->entries);
  assert(strchr(row->summary, '\n') == NULL);
  return CAI_OK;
}
static void clear(cai_error *error) {
  cai_error_cleanup(error);
  cai_error_init(error);
}
static size_t count(cai_agent_runtime *runtime, fixture *f, size_t limit,
                    cai_error *error) {
  f->entries = 0U;
  assert(cai_agent_runtime_visit_history(runtime, limit, entry, f, error) ==
         CAI_OK);
  return f->entries;
}

#include "journal_test.inc"

int main(void) {
  const char *const initial[] = {
      "{\"version\":1,\"model\":\"gpt-6-luna\",\"request_model\":\"gpt-6-"
      "luna\","
      "\"reasoning_effort\":\"low\",\"previous_response_id\":\"old-response\","
      "\"history\":[{\"role\":\"user\",\"content\":\"keep-prompt\"},"
      "{\"type\":\"compaction\",\"encrypted_content\":\"opaque-old\"},"
      "{\"role\":\"assistant\",\"content\":\"keep-answer\"},"
      "{\"role\":\"user\",\"content\":\"drop-prompt\"},"
      "{\"type\":\"function_call\",\"call_id\":\"tool\",\"name\":\"read_file\","
      "\"arguments\":\"{}\"},"
      "{\"type\":\"function_call_output\",\"call_id\":\"tool\",\"output\":"
      "\"tool-result\"},",
      "{\"type\":\"reasoning\",\"encrypted_content\":\"opaque-reasoning\"},"
      "{\"role\":\"user\",\"content\":\"steer-prompt\"},"
      "{\"role\":\"assistant\",\"content\":\"drop-answer\"},"
      "{\"role\":\"user\",\"content\":\"<user_shell_command>shell-output</"
      "user_shell_command>\"}],",
      "\"history_blocks\":[{\"first_item\":0,\"event_start\":0,\"input_"
      "sequence\":0,\"kind\":3,\"summary\":\"Compacted history\"},"
      "{\"first_item\":3,\"event_start\":7,\"input_sequence\":5,\"kind\":1,"
      "\"summary\":\"drop-answer\"},"
      "{\"first_item\":9,\"event_start\":12,\"input_sequence\":14,\"kind\":2,"
      "\"summary\":\"shell-command\"}]}"};
  fixture f;
  cai_client_config client_config;
  cai_agent_runtime_config config;
  cai_agent_session_store store;
  cai_client *client;
  cai_agent_runtime *runtime;
  cai_error error;
  char *before;
  int protocol, i, rc;
  struct timespec delay;
  test_journal_edits();
  for (protocol = 0; protocol < 2; protocol++) {
    memset(&f, 0, sizeof(f));
    f.state = cai_alloc(NULL, 2048U);
    assert(f.state != NULL);
    f.state[0] = '\0';
    for (i = 0; i < (int)(sizeof(initial) / sizeof(initial[0])); i++)
      strcat(f.state, initial[i]);
    f.watermark = 16U;
    memset(&store, 0, sizeof(store));
    store.context = &f;
    store.commit = commit;
    store.load_latest = load;
    store.append_event = append;
    store.load_events_after = events;
    cai_error_init(&error);
    cai_client_config_init(&client_config);
    client_config.api_key = "fixture";
    client_config.base_url = "http://127.0.0.1:1/v1";
    client_config.api_protocol = (protocol & 1)
                                     ? CAI_API_PROTOCOL_CHAT_COMPLETIONS
                                     : CAI_API_PROTOCOL_RESPONSES;
    assert(cai_client_open(&client_config, &client, &error) == CAI_OK);
    cai_agent_runtime_config_init(&config);
    config.workspace_directory = ".";
    config.session_store = &store;
    config.resume_latest = 1;
    config.disable_terminal = 1;
    config.disable_review_subagent = 1;
    config.disable_web_search = 1;
    config.disable_chrome = 1;
    config.disable_skills = 1;
    config.logger_disabled = 1;
    config.replay_callback = observed;
    config.replay_context = &f;
    assert(cai_agent_runtime_open(client, &config, &runtime, &error) == CAI_OK);
    assert(count(runtime, &f, 0U, &error) == 3U);
    assert(count(runtime, &f, 1U, &error) == 1U);
    before = cai_strdup(NULL, f.state);
    assert(cai_agent_runtime_trim_history(runtime, 0U, &error) == CAI_OK);
    assert(strcmp(before, f.state) == 0);
    free(before);
    f.reject = 1;
    assert(cai_agent_runtime_visit_history(runtime, 0U, entry, &f, &error) ==
           CAI_ERR_TRANSPORT);
    clear(&error);
    f.reject = 0;
    assert(cai_agent_runtime_trim_history(runtime, 4U, &error) ==
           CAI_ERR_INVALID);
    clear(&error);
    f.fail_commit = 1;
    assert(cai_agent_runtime_trim_history(runtime, 1U, &error) ==
           CAI_ERR_TRANSPORT);
    clear(&error);
    assert(count(runtime, &f, 0U, &error) == 3U);
    f.fail_commit = 0;
    f.expect_prefix_edit = 1;
    assert(cai_agent_runtime_trim_history(runtime, 1U, &error) == CAI_OK);
    assert(count(runtime, &f, 0U, &error) == 2U);
    assert(strstr(f.state, "shell-output") == NULL);
    assert(strstr(f.state, "tool-result") != NULL);
    assert(strstr(f.state, "previous_response_id") == NULL);
    assert(cai_agent_runtime_trim_history(runtime, 1U, &error) == CAI_OK);
    assert(count(runtime, &f, 0U, &error) == 1U);
    assert(strstr(f.state, "tool-result") == NULL);
    assert(strstr(f.state, "opaque-reasoning") == NULL);
    assert(strstr(f.state, "drop-prompt") == NULL);
    assert(strstr(f.state, "keep-answer") != NULL);
    assert(cai_agent_runtime_history_event_visible(runtime, 6U));
    assert(!cai_agent_runtime_history_event_visible(runtime, 5U));
    assert(!cai_agent_runtime_history_event_visible(runtime, 9U));
    cai_agent_runtime_close(runtime);
    f.seen = 0U;
    assert(cai_agent_runtime_open(client, &config, &runtime, &error) == CAI_OK);
    assert(f.seen == 0U);
    assert(count(runtime, &f, 0U, &error) == 1U);
    assert(cai_agent_runtime_trim_history(runtime, 1U, &error) == CAI_OK);
    assert(count(runtime, &f, 0U, &error) == 0U);
    assert(strstr(f.state, "keep-answer") == NULL);
    cai_agent_runtime_close(runtime);
    f.seen = 0U;
    assert(cai_agent_runtime_open(client, &config, &runtime, &error) == CAI_OK);
    assert(f.seen == 0U && count(runtime, &f, 0U, &error) == 0U);
    cai_agent_runtime_close(runtime);
    config.resume_latest = 0;
    config.replay_callback = NULL;
    assert(cai_agent_runtime_open(client, &config, &runtime, &error) == CAI_OK);
    delay.tv_sec = 0;
    delay.tv_nsec = 1000000L;
    for (i = 0; i < 25; i++) {
      assert(cai_agent_runtime_user_shell_begin(runtime, "local-only", 0,
                                                &error) == CAI_OK);
      assert(cai_agent_runtime_trim_history(runtime, 1U, &error) ==
             CAI_ERR_INVALID);
      clear(&error);
      assert(cai_agent_runtime_trim_history(runtime, 0U, &error) == CAI_OK);
      assert(cai_agent_runtime_user_shell_write(runtime, "local result", 12U,
                                                &error) == CAI_OK);
      assert(cai_agent_runtime_user_shell_finish(runtime, 0, 0U, &error) ==
             CAI_OK);
      do {
        f.entries = 0U;
        rc = cai_agent_runtime_visit_history(runtime, 100U, entry, &f, &error);
        if (rc != CAI_OK) {
          assert(rc == CAI_ERR_INVALID);
          clear(&error);
          nanosleep(&delay, NULL);
        }
      } while (rc != CAI_OK);
      assert(f.entries == (size_t)i + 1U);
    }
    assert(count(runtime, &f, 0U, &error) == 20U);
    assert(count(runtime, &f, 100U, &error) == 25U);
    assert(strstr(f.state, "local result") == NULL);
    assert(cai_agent_runtime_trim_history(runtime, 25U, &error) == CAI_OK);
    assert(count(runtime, &f, 0U, &error) == 0U);
    cai_agent_runtime_close(runtime);
    cai_client_close(client);
    free(f.state);
    free(f.journal);
    cai_error_cleanup(&error);
  }
  return 0;
}
