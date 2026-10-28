static int cai_runtime_set_terminal_yield(cai_agent_runtime *runtime,
                                          long milliseconds, cai_error *error) {
  cai_agent_runtime *child;
  int rc = CAI_OK;
  pthread_mutex_lock(&runtime->lock);
  child = runtime->active_subagent != NULL ? runtime->active_subagent
                                           : runtime->active_review;
  if (child != NULL)
    pthread_mutex_lock(&child->lock);
  if (runtime->stopping || runtime->model_switching ||
      runtime->settings_applying ||
      (child != NULL &&
       (child->stopping || child->model_switching || child->settings_applying)))
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "runtime is changing execution settings");
  if (rc == CAI_OK) {
    cai_agent_runtime *target = runtime;
    for (;;) {
      rc = cai_terminal_tools_set_yield_internal(
          CAI_AGENT_IMPL(target->agent)->tools, milliseconds, error);
      if (rc != CAI_OK)
        break;
      target->smith_has_terminal_config = 1;
      target->smith_terminal_config.foreground = milliseconds == 0L;
      if (milliseconds > 0L)
        target->smith_terminal_config.default_yield_time_ms = milliseconds;
      if (target == child || child == NULL)
        break;
      target = child;
    }
  }
  if (child != NULL)
    pthread_mutex_unlock(&child->lock);
  pthread_mutex_unlock(&runtime->lock);
  return rc;
}

int cai_agent_runtime_get_terminal_yield_time(cai_agent_runtime *runtime,
                                              long *milliseconds,
                                              cai_error *error) {
  int rc = cai_runtime_owner(runtime, error);
  if (rc != CAI_OK)
    return rc;
  if (milliseconds == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "yield interval output is required");
  pthread_mutex_lock(&runtime->lock);
  *milliseconds = runtime->smith_terminal_config.foreground ? 0L
                  : runtime->smith_terminal_config.default_yield_time_ms > 0L
                      ? runtime->smith_terminal_config.default_yield_time_ms
                      : CAI_TERMINAL_DEFAULT_YIELD_TIME_MS;
  pthread_mutex_unlock(&runtime->lock);
  return CAI_OK;
}

int cai_agent_runtime_set_terminal_yield_time(cai_agent_runtime *runtime,
                                              long milliseconds,
                                              cai_error *error) {
  int rc = cai_runtime_owner(runtime, error);
  if (rc != CAI_OK)
    return rc;
  if (milliseconds < 0L || milliseconds > 30000L)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "yield interval must be 0..30000 ms");
  return cai_runtime_set_terminal_yield(runtime, milliseconds, error);
}
