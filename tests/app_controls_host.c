#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <cai/app.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct host {
  cai_app *app;
  pthread_t owner;
  pthread_mutex_t lock;
  pthread_cond_t condition;
  const char *mode;
  int ready, sent, finished;
  int events, replay, starts, text, acks, rejected, goal_changes;
} host;

static void send_control(host *h, cai_app_control_type type, const char *text) {
  cai_error error;
  unsigned long long id;
  cai_error_init(&error);
  assert(h->app->control(h->app, type, text, &id, &error) == CAI_OK);
  assert(id > 0U);
  cai_error_cleanup(&error);
}
static int startup_stop(void *context, cai_agent *agent, cai_error *error) {
  host *h = context;
  (void)agent;
  (void)error;
  send_control(h, CAI_APP_STOP, NULL);
  return CAI_OK;
}
static void *worker(void *context) {
  host *h = context;
  cai_app_status status;
  cai_error error;
  cai_app_status_init(&status);
  cai_error_init(&error);
  pthread_mutex_lock(&h->lock);
  while (!h->ready && !h->finished)
    pthread_cond_wait(&h->condition, &h->lock);
  if (h->ready) {
    assert(!pthread_equal(h->owner, pthread_self()));
    assert(h->app->get_status(h->app, &status, &error) == CAI_OK);
    assert(status.running && status.runtime_ready && status.session_id != NULL);
    assert(status.effective.model != NULL);
    if (strcmp(h->mode, "queue") == 0 || strcmp(h->mode, "interactive") == 0) {
      char prompt[] = "HOST NEXT PROMPT";
      send_control(h, CAI_APP_QUEUE_PROMPT, prompt);
      prompt[0] = 'X';
      if (strcmp(h->mode, "queue") == 0)
        send_control(h, CAI_APP_QUEUE_PROMPT, "HOST LAST PROMPT");
    } else if (strcmp(h->mode, "steer") == 0) {
      send_control(h, CAI_APP_STEER, "HOST STEERING");
    } else if (strcmp(h->mode, "reject") == 0) {
      send_control(h, CAI_APP_STEER, "TOO LATE");
      send_control(h, CAI_APP_PAUSE_GOAL, NULL);
    } else if (strcmp(h->mode, "goals") == 0) {
      send_control(h, CAI_APP_REPLACE_GOAL, "HOST GOAL");
    } else if (strcmp(h->mode, "cancel") == 0 ||
               strcmp(h->mode, "cancelqueued") == 0) {
      if (strcmp(h->mode, "cancelqueued") == 0)
        send_control(h, CAI_APP_QUEUE_PROMPT, "AFTER CANCEL");
      send_control(h, CAI_APP_CANCEL_TURN, NULL);
    } else if (strcmp(h->mode, "overflow") == 0) {
      unsigned long long id;
      int i;
      for (i = 0; i < 64; i++)
        send_control(h, CAI_APP_STEER, "queued steering");
      assert(h->app->control(h->app, CAI_APP_STOP, NULL, &id, &error) ==
             CAI_ERR_INVALID);
      assert(id == 0U);
    } else {
      if (strcmp(h->mode, "stopqueued") == 0)
        send_control(h, CAI_APP_QUEUE_PROMPT, "DISCARD QUEUED PROMPT");
      send_control(h, CAI_APP_STOP, NULL);
      /* Accepted commands behind STOP must receive a negative ack. */
      send_control(h, CAI_APP_QUEUE_PROMPT, "MUST NOT RUN");
    }
    h->sent = 1;
    pthread_cond_broadcast(&h->condition);
  }
  pthread_mutex_unlock(&h->lock);
  cai_app_status_cleanup(&status);
  cai_error_cleanup(&error);
  return NULL;
}
static int event(void *context, const cai_agent_runtime_event *e,
                 cai_error *error) {
  host *h = context;
  (void)error;
  assert(pthread_equal(h->owner, pthread_self()));
  h->events++;
  if (e->type == CAI_AGENT_EVENT_RUN_STARTED)
    h->starts++;
  if (e->type == CAI_AGENT_EVENT_TEXT_DELTA)
    h->text++;
  if (strcmp(h->mode, "observer_error") == 0 &&
      e->type == CAI_AGENT_EVENT_TEXT_DELTA) {
    send_control(h, CAI_APP_QUEUE_PROMPT, "ABANDON THIS PROMPT");
    error->code = CAI_ERR_PROTOCOL;
    error->http_status = 418;
    error->message = strdup("HOST OBSERVER FAILED");
    error->detail = strdup("observer detail");
    error->server_code = strdup("host_observer");
    error->request_id = strdup("host_request");
    return CAI_ERR_PROTOCOL;
  }
  if (strcmp(h->mode, "goals") == 0 &&
      e->type == CAI_AGENT_EVENT_GOAL_CHANGED) {
    h->goal_changes++;
    if (h->goal_changes == 1)
      send_control(h, CAI_APP_PAUSE_GOAL, NULL);
    else if (h->goal_changes == 2)
      send_control(h, CAI_APP_RESUME_GOAL, NULL);
    else if (h->goal_changes == 3)
      send_control(h, CAI_APP_CLEAR_GOAL, NULL);
    else if (h->goal_changes == 4)
      send_control(h, CAI_APP_STOP, NULL);
  }
  if (strcmp(h->mode, "replay") != 0 &&
      strcmp(h->mode, "observer_error") != 0 &&
      strcmp(h->mode, "replay_error") != 0 && strcmp(h->mode, "failure") != 0 &&
      ((strcmp(h->mode, "queue") == 0 &&
        e->type == CAI_AGENT_EVENT_RUN_STARTED) ||
       ((strcmp(h->mode, "interactive") == 0 ||
         strcmp(h->mode, "reject") == 0) &&
        e->type == CAI_AGENT_EVENT_RUN_COMPLETED) ||
       (strcmp(h->mode, "queue") != 0 && strcmp(h->mode, "interactive") != 0 &&
        strcmp(h->mode, "reject") != 0 &&
        e->type == CAI_AGENT_EVENT_TEXT_DELTA))) {
    pthread_mutex_lock(&h->lock);
    if (!h->sent) {
      h->ready = 1;
      pthread_cond_broadcast(&h->condition);
      while (!h->sent)
        pthread_cond_wait(&h->condition, &h->lock);
    }
    pthread_mutex_unlock(&h->lock);
  }
  return CAI_OK;
}
static int replay(void *context, const cai_agent_session_event *e,
                  cai_error *error) {
  host *h = context;
  assert(pthread_equal(h->owner, pthread_self()));
  assert(e->type != NULL);
  h->replay++;
  if (strcmp(h->mode, "replay_error") == 0) {
    error->code = CAI_ERR_PROTOCOL;
    error->message = strdup("HOST REPLAY FAILED");
    return CAI_ERR_PROTOCOL;
  }
  return CAI_OK;
}
static void ack(void *context, unsigned long long id, int status,
                const cai_error *error) {
  host *h = context;
  cai_app_status snapshot;
  cai_error local;
  const char *release = getenv("CAI_TEST_APP_RELEASE");
  assert(pthread_equal(h->owner, pthread_self()) && id > 0U);
  h->acks++;
  if (status != CAI_OK) {
    assert(error != NULL && error->message != NULL);
    h->rejected++;
  }
  cai_error_init(&local);
  cai_app_status_init(&snapshot);
  assert(h->app->get_status(h->app, &snapshot, &local) == CAI_OK);
  assert(snapshot.control_id == id && snapshot.control_status == status);
  cai_app_status_cleanup(&snapshot);
  cai_error_cleanup(&local);
  if (release != NULL) {
    FILE *file = fopen(release, "w");
    assert(file != NULL && fclose(file) == 0);
  }
}
int main(int argc, char **argv) {
  host h;
  cai_app_config config;
  cai_app_status snapshot, retained;
  cai_error error;
  pthread_t thread;
  unsigned long long id;
  int code, rc;
  memset(&h, 0, sizeof(h));
  h.owner = pthread_self();
  h.mode = getenv("CAI_TEST_APP_CONTROL");
  assert(h.mode != NULL);
  assert(pthread_mutex_init(&h.lock, NULL) == 0);
  assert(pthread_cond_init(&h.condition, NULL) == 0);
  cai_app_config_init(&config);
  config.event_callback = event;
  config.event_context = &h;
  config.replay_callback = replay;
  config.replay_context = &h;
  config.control_callback = ack;
  config.control_context = &h;
  if (strcmp(h.mode, "startup_stop") == 0) {
    config.register_agent_tools = startup_stop;
    config.register_agent_tools_context = &h;
  }
  cai_error_init(&error);
  cai_app_status_init(&snapshot);
  cai_app_status_init(&retained);
  assert(cai_app_open(&config, &h.app, &error) == CAI_OK);
  assert(h.app->get_status(h.app, &snapshot, &error) == CAI_OK);
  assert(snapshot.outcome == CAI_APP_NOT_RUN && !snapshot.running);
  assert(h.app->control(h.app, CAI_APP_STOP, NULL, &id, &error) ==
         CAI_ERR_INVALID);
  cai_error_cleanup(&error);
  assert(pthread_create(&thread, NULL, worker, &h) == 0);
  rc = h.app->run(h.app, argc - 1, (const char *const *)(argv + 1), &code,
                  &error);
  pthread_mutex_lock(&h.lock);
  h.finished = 1;
  pthread_cond_broadcast(&h.condition);
  pthread_mutex_unlock(&h.lock);
  assert(pthread_join(thread, NULL) == 0);
  assert(rc == CAI_OK);
  assert(h.app->get_status(h.app, &snapshot, &error) == CAI_OK);
  assert(!snapshot.running && !snapshot.runtime_ready &&
         snapshot.exit_code == code);
  assert(h.app->get_status(h.app, &retained, &error) == CAI_OK);
  if (strcmp(h.mode, "queue") == 0) {
    assert(code == 0 && h.starts == 3 && h.acks == 2 && h.rejected == 0);
    assert(snapshot.outcome == CAI_APP_COMPLETED &&
           snapshot.metrics.has_last_turn);
    assert(snapshot.metrics.session_usage.usage.total_tokens >= 60);
  } else if (strcmp(h.mode, "stop") == 0) {
    assert(snapshot.outcome == CAI_APP_STOPPED && h.acks == 2 &&
           h.rejected == 1);
  } else if (strcmp(h.mode, "stopqueued") == 0) {
    assert(snapshot.outcome == CAI_APP_STOPPED && h.acks == 3 &&
           h.rejected == 1 && h.starts == 1);
  } else if (strcmp(h.mode, "cancel") == 0) {
    assert(snapshot.outcome == CAI_APP_CANCELLED && h.acks == 1 &&
           h.rejected == 0);
  } else if (strcmp(h.mode, "startup_stop") == 0) {
    assert(code == 0 && snapshot.outcome == CAI_APP_STOPPED && h.acks == 1 &&
           h.starts == 0);
  } else if (strcmp(h.mode, "cancelqueued") == 0) {
    assert(code == 0 && snapshot.outcome == CAI_APP_COMPLETED &&
           h.starts == 2 && h.acks == 2 && h.rejected == 0);
  } else if (strcmp(h.mode, "observer_error") == 0) {
    assert(code == 1 && snapshot.outcome == CAI_APP_FAILED);
    assert(snapshot.error.http_status == 418);
    assert(strcmp(snapshot.error.detail, "observer detail") == 0);
    assert(strcmp(snapshot.error.server_code, "host_observer") == 0);
    assert(strcmp(snapshot.error.request_id, "host_request") == 0);
    assert(h.acks == 1 && h.rejected == 1);
  } else if (strcmp(h.mode, "goals") == 0) {
    assert(h.goal_changes == 4 && h.acks == 5 && h.rejected == 0);
    assert(!snapshot.goal.has_goal && snapshot.outcome == CAI_APP_STOPPED);
  } else if (strcmp(h.mode, "replay") == 0) {
    assert(code == 0 && h.replay > 0 && h.starts == 1);
  } else if (strcmp(h.mode, "replay_error") == 0 ||
             strcmp(h.mode, "failure") == 0) {
    assert(code == 1 && snapshot.outcome == CAI_APP_FAILED &&
           snapshot.error.message != NULL);
  } else if (strcmp(h.mode, "overflow") == 0) {
    assert(h.acks == 64);
  } else if (strcmp(h.mode, "steer") == 0) {
    assert(h.acks == 1 && h.rejected == 0 && h.starts == 1);
  } else if (strcmp(h.mode, "reject") == 0) {
    assert(code == 0 && h.acks == 2 && h.rejected == 2);
    assert(snapshot.control_status != CAI_OK &&
           snapshot.control_error.message != NULL);
  } else if (strcmp(h.mode, "interactive") == 0) {
    assert(code == 0 && h.starts == 2 && h.acks == 1 && h.rejected == 0);
  }
  h.app->close(h.app);
  /* No borrowed pointers into the app remain in either snapshot. */
  if (retained.session_id != NULL)
    assert(strcmp(retained.session_id, snapshot.session_id) == 0);
  cai_app_status_cleanup(&retained);
  cai_app_status_cleanup(&snapshot);
  cai_error_cleanup(&error);
  assert(pthread_cond_destroy(&h.condition) == 0);
  assert(pthread_mutex_destroy(&h.lock) == 0);
  return code;
}
