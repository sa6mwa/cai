#include <assert.h>
#include <cai/cai.h>
#ifdef CAI_SMOKE_WITH_APP
#include <cai/app.h>
#endif
#include <stdio.h>
#include <string.h>

int main(void) {
  cai_client_config config;
  cai_agent_config agent_config;
  cai_client *client;
  cai_agent *agent;
  cai_session *session;
  cai_session *restored;
  cai_source *source;
  cai_sink *sink;
  cai_error error;
  FILE *state;
  char bytes[4096];
  size_t count;
  size_t used;
#ifdef CAI_SMOKE_WITH_APP
  cai_app_config application;
  cai_app_config_init(&application);
  assert(application.program_name != NULL);
#endif

  cai_error_init(&error);
  cai_client_config_init(&config);
  config.api_key = "cross-smoke-not-a-real-key";
  config.base_url = "http://127.0.0.1:1";
  assert(cai_client_open(&config, &client, &error) == CAI_OK);
  cai_agent_config_init(&agent_config);
  agent_config.model = "cross-smoke";
  agent_config.disable_auto_compaction = 1;
  agent_config.enable_local_history = 1;
  assert(cai_client_new_agent(client, &agent_config, &agent, &error) == CAI_OK);
  assert(cai_agent_new_session(agent, &session, &error) == CAI_OK);
  state = tmpfile();
  assert(state != NULL);
  assert(fputs("[{\"role\":\"user\",\"content\":[{\"type\":\"input_text\","
               "\"text\":\"cross-runtime-smoke\"}]}]",
               state) >= 0);
  rewind(state);
  assert(cai_source_file(state, 0, &source, &error) == CAI_OK);
  assert(cai_session_import_history_source(session, source, &error) == CAI_OK);
  cai_source_close(source);
  assert(fclose(state) == 0);
  assert(cai_session_set_previous_response_id(session, "resp_cross_smoke",
                                              &error) == CAI_OK);

  state = tmpfile();
  assert(state != NULL);
  assert(cai_session_export_state_source(session, &source, &error) == CAI_OK);
  assert(cai_sink_file(state, 0, &sink, &error) == CAI_OK);
  assert(cai_source_copy_to_sink(source, sink, &error) == CAI_OK);
  cai_sink_close(sink);
  cai_source_close(source);
  rewind(state);
  assert(cai_source_file(state, 0, &source, &error) == CAI_OK);
  assert(cai_agent_new_session(agent, &restored, &error) == CAI_OK);
  assert(cai_session_import_state_source(restored, source, &error) == CAI_OK);
  cai_source_close(source);
  assert(strcmp(cai_session_previous_response_id(restored),
                "resp_cross_smoke") == 0);
  assert(cai_session_export_state_source(restored, &source, &error) == CAI_OK);
  used = 0U;
  while ((count = cai_source_read(source, bytes + used,
                                  sizeof(bytes) - used - 1U, &error)) != 0U) {
    used += count;
    assert(used < sizeof(bytes) - 1U);
  }
  assert(used > 0U);
  bytes[used] = '\0';
  assert(strstr(bytes, "cross-runtime-smoke") != NULL);
  cai_source_close(source);
  assert(fclose(state) == 0);
  cai_session_destroy(restored);
  cai_session_destroy(session);
  cai_agent_destroy(agent);
  cai_client_close(client);
  cai_error_cleanup(&error);
  puts("cross runtime smoke passed");
  return 0;
}
