#include "../src/cai_internal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct reader {
  const char *data;
  size_t offset;
} reader;
static size_t read_text(void *context, void *bytes, size_t size,
                        cai_error *error) {
  reader *r = context;
  size_t remaining = strlen(r->data) - r->offset;
  (void)error;
  if (size > remaining)
    size = remaining;
  memcpy(bytes, r->data + r->offset, size);
  r->offset += size;
  return size;
}
static int import(cai_session *session, const char *json, cai_error *error) {
  cai_source_callbacks callbacks;
  cai_source *source;
  reader r;
  int rc;
  memset(&callbacks, 0, sizeof(callbacks));
  r.data = json;
  r.offset = 0U;
  callbacks.context = &r;
  callbacks.read = read_text;
  assert(cai_source_from_callbacks(&callbacks, &source, error) == CAI_OK);
  rc = cai_session_import_state_source(session, source, error);
  cai_source_close(source);
  return rc;
}

int main(void) {
  cai_client_config config;
  cai_agent_config agent_config;
  cai_client *client;
  cai_agent *agent;
  cai_session *session;
  cai_source *source;
  cai_error error;
  char state[8192];
  size_t used;
  cai_error_init(&error);
  cai_client_config_init(&config);
  config.api_key = "fixture";
  config.base_url = "http://127.0.0.1:1";
  assert(cai_client_open(&config, &client, &error) == CAI_OK);
  CAI_CLIENT_IMPL(client)->catalog_policy_origin = CAI_CHATGPT_CODEX_BASE_URL;
  cai_agent_config_init(&agent_config);
  agent_config.model = CAI_MODEL_GPT_6_LUNA;
  agent_config.enable_local_history = 1;
  assert(cai_client_new_agent(client, &agent_config, &agent, &error) == CAI_OK);
  assert(cai_agent_new_session(agent, &session, &error) == CAI_OK);
  assert(cai_session_context_window_tokens(session) == 0LL);
  assert(
      import(
          session,
          "{\"version\":1,\"model\":\"gpt-6-luna\","
          "\"request_model\":\"gpt-6-luna\",\"context_window_tokens\":272000,"
          "\"history\":[]}",
          &error) == CAI_OK);
  assert(cai_session_context_window_tokens(session) == 272000LL);
  assert(cai_session_export_state_source(session, &source, &error) == CAI_OK);
  used = source->read(source, state, sizeof(state) - 1U, &error);
  state[used] = '\0';
  assert(strstr(state, "\"context_window_tokens\":272000") != NULL);
  cai_source_close(source);
  assert(import(session, "{\"version\":1,\"context_window_tokens\":-1}",
                &error) == CAI_ERR_INVALID);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  assert(cai_session_context_window_tokens(session) == 272000LL);
  assert(cai_model_catalog_import_provider_response(
             client,
             "{\"data\":[{\"id\":\"gpt-6-luna\",\"context_window_tokens\":"
             "1050000}]}",
             &error) == CAI_OK);
  assert(cai_session_context_window_tokens(session) == 1050000LL);
  cai_session_destroy(session);
  cai_agent_destroy(agent);
  cai_client_close(client);
  cai_error_cleanup(&error);
  return 0;
}
