#include <string.h>
/* Translate existing explicit-state fixtures into native journal input. This
 * is test-data construction, never a production compatibility reader. */
typedef struct test_journal_reader {
  const char *text;
  size_t offset;
} test_journal_reader;
static size_t test_journal_fixture_read(void *context, void *bytes,
                                        size_t count, cai_error *error) {
  test_journal_reader *reader = context;
  size_t left = strlen(reader->text) - reader->offset;
  (void)error;
  if (count > left)
    count = left;
  memcpy(bytes, reader->text + reader->offset, count);
  reader->offset += count;
  return count;
}
static int test_journal_fixture(const char *input, int journal_input,
                                unsigned long long watermark, char *output,
                                size_t capacity, cai_error *error) {
  cai_client_config client_config;
  cai_agent_config agent_config;
  cai_client *client = NULL;
  cai_agent *agent = NULL;
  cai_session *session = NULL;
  cai_source *source = NULL, *encoded = NULL;
  cai_source_callbacks callbacks;
  cai_agent_impl *settings = NULL;
  char *model = NULL, *effort = NULL, *summary = NULL, *tier = NULL;
  test_journal_reader reader;
  size_t used = 0U, count;
  cai_error operation_error;
  cai_error *caller_error = error;
  int rc;
  error = &operation_error;
  cai_error_init(error);
  memset(&reader, 0, sizeof(reader));
  reader.text = input;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.read = test_journal_fixture_read;
  callbacks.context = &reader;
  cai_client_config_init(&client_config);
  client_config.api_key = "fixture";
  client_config.base_url = "http://127.0.0.1:1/v1";
  cai_agent_config_init(&agent_config);
  agent_config.model = "gpt-6-luna";
  agent_config.enable_local_history = 1;
  rc = cai_client_open(&client_config, &client, error);
  if (rc == CAI_OK)
    rc = cai_client_new_agent(client, &agent_config, &agent, error);
  if (rc == CAI_OK)
    rc = cai_agent_new_session(agent, &session, error);
  if (rc == CAI_OK)
    rc = cai_source_from_callbacks(&callbacks, &source, error);
  if (rc == CAI_OK)
    rc = journal_input
             ? cai_session_restore_journal_source(session, source, error)
             : cai_session_import_state_source(session, source, error);
  if (rc == CAI_OK) {
    cai_session_impl *saved = CAI_SESSION_IMPL(session);
    settings = CAI_AGENT_IMPL(agent);
    model = settings->model;
    effort = settings->reasoning_effort;
    summary = settings->reasoning_summary;
    tier = settings->service_tier;
    /* Mirror the runtime's settings restoration before inspecting a low-level
     * session through explicit snapshot APIs. These strings stay borrowed. */
    settings->model = saved->state_request_model != NULL
                          ? saved->state_request_model
                      : saved->state_model != NULL ? saved->state_model
                                                   : model;
    settings->reasoning_effort = saved->state_reasoning_effort;
    settings->reasoning_summary = saved->state_reasoning_summary;
    settings->service_tier = saved->state_service_tier;
  }
  if (rc == CAI_OK)
    rc = journal_input
             ? cai_session_export_state_source(session, &encoded, error)
             : cai_session_journal_source(session, &encoded, error);
  if (rc == CAI_OK && !journal_input) {
    used = (size_t)snprintf(output, capacity,
                            "{\"record_type\":\"commit\",\"applied_event_"
                            "sequence\":%llu,\"state\":",
                            watermark);
  }
  while (rc == CAI_OK && used + 3U < capacity) {
    count =
        cai_source_read(encoded, output + used, capacity - used - 3U, error);
    if (count == 0U) {
      rc = error->code;
      break;
    }
    used += count;
  }
  if (rc == CAI_OK && used + 3U >= capacity)
    rc =
        cai_set_error(error, CAI_ERR_LIMIT, "journal fixture buffer exhausted");
  if (rc == CAI_OK && !journal_input) {
    output[used++] = '}';
    output[used++] = '\n';
  }
  output[used] = '\0';
  cai_source_close(encoded);
  cai_source_close(source);
  if (settings != NULL) {
    settings->model = model;
    settings->reasoning_effort = effort;
    settings->reasoning_summary = summary;
    settings->service_tier = tier;
  }
  if (session != NULL)
    session->close(session);
  if (agent != NULL)
    agent->close(agent);
  if (client != NULL)
    client->close(client);
  /* Successful persistence must not erase the inference failure that caused
   * the runtime to commit its final state. Keep adapter errors separate. */
  if (rc != CAI_OK) {
    cai_error_cleanup(caller_error);
    *caller_error = operation_error;
    memset(&operation_error, 0, sizeof(operation_error));
  }
  cai_error_cleanup(&operation_error);
  return rc;
}
