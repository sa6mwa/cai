/* Cancellation settles goal state before publishing the terminal event. */
typedef struct goal_cancel_test_state {
  runtime_session_store_state store;
  runtime_event_state events;
  cai_agent_runtime *runtime;
  int failure;
  pthread_mutex_t lock;
  pthread_cond_t ready;
  int checkpoint_armed, checkpoint_waiting, release_checkpoint;
} goal_cancel_test_state;

static int test_goal_cancel_checkpoint(void *context, const char *scope,
                                       const char *id, cai_source *source,
                                       unsigned long long sequence,
                                       cai_error *error) {
  goal_cancel_test_state *test = context;
  pthread_mutex_lock(&test->lock);
  if (test->checkpoint_armed && !test->release_checkpoint) {
    test->checkpoint_waiting = 1;
    pthread_cond_broadcast(&test->ready);
    while (!test->release_checkpoint)
      pthread_cond_wait(&test->ready, &test->lock);
  }
  pthread_mutex_unlock(&test->lock);
  return test_runtime_session_store_checkpoint(&test->store, scope, id, source,
                                               sequence, error);
}

static int test_goal_cancel_append(void *context, const char *scope,
                                   const char *id,
                                   const cai_agent_session_event *event,
                                   cai_error *error) {
  goal_cancel_test_state *test = context;
  if (test->failure == 1 && strcmp(event->type, "goal_cancel_pause") == 0)
    return cai_set_error(error, CAI_ERR_TRANSPORT, "pause journal failed");
  return test_runtime_session_store_append_event(&test->store, scope, id, event,
                                                 error);
}

static int test_goal_cancel_event(void *context,
                                  const cai_agent_runtime_event *event,
                                  cai_error *error) {
  goal_cancel_test_state *test = context;
  int rc = test_runtime_event(&test->events, event, error);
  if (rc != CAI_OK || event->type != CAI_AGENT_EVENT_INPUT_BOUNDARY)
    return rc;
  if (test->failure == 4) {
    pthread_mutex_lock(&test->lock);
    test->checkpoint_armed = 1;
    pthread_mutex_unlock(&test->lock);
    return CAI_OK;
  }
  if (test->failure == 2)
    cai_agent_runtime_test_set_fail_goal_status_replace(1);
  if (test->failure == 3) {
    test->store.fail_checkpoint_once = 1;
    snprintf(test->store.fail_checkpoint_session_id,
             sizeof(test->store.fail_checkpoint_session_id), "%s",
             cai_agent_runtime_session_id(test->runtime));
  }
  return cai_agent_runtime_cancel_turn(test->runtime, error);
}

static void test_agent_runtime_goal_cancel(test_state *state) {
  static const char response[] =
      "data: {\"type\":\"response.output_text.delta\",\"delta\":\"partial "
      "work\"}\n\n"
      "data: "
      "{\"type\":\"response.completed\",\"response\":{\"id\":\"resp_cancel\"}}"
      "\n\n";
  static const mock_http_expectation script[] = {
      {"POST /v1/responses HTTP/", NULL, 0U, NULL, 0U, 200, "OK",
       "text/event-stream", NULL, response}};
  cai_agent_session_store store;
  cai_agent_runtime_config config;
  cai_agent_goal_snapshot goal;
  goal_cancel_test_state test;
  http_mock_client mock;
  cai_error error;
  int failure, i;
  for (failure = 0; failure < 5; failure++) {
    if (http_mock_client_open_script(state, "goal_cancel", script, 1U, &mock) !=
        0)
      return;
    memset(&test, 0, sizeof(test));
    memset(&store, 0, sizeof(store));
    pthread_mutex_init(&test.lock, NULL);
    pthread_cond_init(&test.ready, NULL);
    test.failure = failure;
    test.events.owner = pthread_self();
    test.store.checkpoint_json = "{\"version\":1,\"model\":\"gpt-5-nano\","
                                 "\"goal_objective\":\"finish work\","
                                 "\"goal_status\":\"active\",\"history\":[]}";
    store.context = &test;
    /* The generic fixture state is the first member. */
    store.checkpoint = test_goal_cancel_checkpoint;
    store.load_latest = test_runtime_session_store_load;
    store.load_events_after = test_runtime_session_store_load_events_after;
    store.append_event = test_goal_cancel_append;
    cai_error_init(&error);
    cai_agent_runtime_config_init(&config);
    config.workspace_directory = "/tmp";
    config.session_store = &store;
    config.resume_latest = 1;
    config.disable_terminal = config.disable_chrome =
        config.disable_web_search = 1;
    config.synchronize_input_boundaries = 1;
    config.event_callback = test_goal_cancel_event;
    config.event_context = &test;
    expect_int(
        state, "goal_cancel_open",
        cai_agent_runtime_open(mock.client, &config, &test.runtime, &error),
        CAI_OK);
    if (test.runtime != NULL) {
      expect_int(state, "goal_cancel_submit",
                 cai_agent_runtime_submit(test.runtime, "do work", &error),
                 CAI_OK);
      for (i = 0; i < 100 && test.events.cancelled_count == 0 &&
                  test.events.failure_message[0] == '\0';
           i++) {
        expect_int(state, "goal_cancel_pump",
                   cai_agent_runtime_pump(test.runtime, 100L, &error), CAI_OK);
        if (failure == 4 && test.checkpoint_armed && !test.release_checkpoint) {
          struct timespec deadline;
          int ready;
          clock_gettime(CLOCK_REALTIME, &deadline);
          deadline.tv_sec += 2;
          pthread_mutex_lock(&test.lock);
          while (!test.checkpoint_waiting &&
                 pthread_cond_timedwait(&test.ready, &test.lock, &deadline) ==
                     0) {
          }
          ready = test.checkpoint_waiting;
          pthread_mutex_unlock(&test.lock);
          expect_int(state, "goal_cancel_final_checkpoint_waiting", ready, 1L);
          expect_int(state, "goal_cancel_final_checkpoint_request",
                     cai_agent_runtime_cancel_turn(test.runtime, &error),
                     CAI_OK);
          pthread_mutex_lock(&test.lock);
          test.release_checkpoint = 1;
          pthread_cond_broadcast(&test.ready);
          pthread_mutex_unlock(&test.lock);
        }
      }
      if (failure == 0 || failure == 4) {
        expect_int(state, "goal_cancel_confirmed", test.events.cancelled_count,
                   1L);
        expect_int(state, "goal_cancel_goal",
                   cai_agent_runtime_get_goal(test.runtime, &goal, &error),
                   CAI_OK);
        expect_str(state, "goal_cancel_paused_before_event", goal.status,
                   "paused");
        expect_substr(state, "goal_cancel_pause_checkpoint",
                      test.store.saved_checkpoint,
                      "\"goal_status\":\"paused\"");
      } else {
        expect_int(state, "goal_cancel_failure_not_success",
                   test.events.cancelled_count, 0L);
        expect_int(state, "goal_cancel_failure_reported",
                   test.events.failure_message[0] != '\0', 1L);
      }
      cai_agent_runtime_close(test.runtime);
      test.runtime = NULL;
    }
    cai_agent_runtime_test_set_fail_goal_status_replace(0);
    /* A durable cancellation journal must recover even when its checkpoint
     * failed. Hosts must see paused on open, before deciding to pursue. */
    if (failure != 1 && test.store.saved_checkpoint[0] != '\0') {
      test.store.checkpoint_json = test.store.saved_checkpoint;
      test.store.load_applied_event_sequence =
          test.store.saved_applied_event_sequence;
      test.failure = 0;
      expect_int(
          state, "goal_cancel_reopen",
          cai_agent_runtime_open(mock.client, &config, &test.runtime, &error),
          CAI_OK);
      if (test.runtime != NULL) {
        expect_int(state, "goal_cancel_reopen_goal",
                   cai_agent_runtime_get_goal(test.runtime, &goal, &error),
                   CAI_OK);
        expect_str(state, "goal_cancel_reopen_paused", goal.status, "paused");
        cai_agent_runtime_close(test.runtime);
      }
    }
    http_mock_client_close(state, "goal_cancel", &mock);
    cai_error_cleanup(&error);
    pthread_cond_destroy(&test.ready);
    pthread_mutex_destroy(&test.lock);
  }
}
