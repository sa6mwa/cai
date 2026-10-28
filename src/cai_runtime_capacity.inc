/* Capacity is a provider availability error, not a new conversation turn.
 * Keep all waiting on the worker; the owner continues pumping input/events. */
#ifdef CAI_TESTING
static int (*cai_capacity_wait_test)(cai_agent_runtime *, long, cai_error *);
void cai_runtime_test_set_capacity_wait_fn(int (*fn)(cai_agent_runtime *, long,
                                                     cai_error *)) {
  cai_capacity_wait_test = fn;
}
#endif

static int cai_runtime_capacity_wait(cai_agent_runtime *runtime, long delay_ms,
                                     cai_error *error) {
  struct timespec deadline;
  int rc = 0, cancelled;
#ifdef CAI_TESTING
  if (cai_capacity_wait_test != NULL)
    return cai_capacity_wait_test(runtime, delay_ms, error);
#endif
  if (clock_gettime(CLOCK_REALTIME, &deadline) != 0)
    return cai_set_error(error, CAI_ERR_TRANSPORT, "read capacity retry clock");
  deadline.tv_sec += delay_ms / 1000L;
  deadline.tv_nsec += (delay_ms % 1000L) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000L;
  }
  pthread_mutex_lock(&runtime->lock);
  while (!runtime->stopping && !runtime->turn_cancel_requested && rc == 0)
    rc = pthread_cond_timedwait(&runtime->condition, &runtime->lock, &deadline);
  cancelled = runtime->stopping || runtime->turn_cancel_requested;
  pthread_mutex_unlock(&runtime->lock);
  if (cancelled)
    return cai_set_error(error, CAI_ERR_CANCELLED,
                         "agent turn was cancelled during capacity backoff");
  return rc == ETIMEDOUT ? CAI_OK
                         : cai_set_error(error, CAI_ERR_TRANSPORT,
                                         "wait for model capacity retry");
}

static int cai_runtime_capacity_retry(void *context, const cai_error *failure,
                                      int *retry, cai_error *error) {
  cai_agent_runtime *runtime = context;
  unsigned int attempt;
  long delay_ms;
  char message[192];
  int rc, limited = 0;
  *retry = 0;
  if (runtime->disable_capacity_retry || failure->server_code == NULL ||
      strcmp(failure->server_code, "server_is_overloaded") != 0 ||
      runtime->capacity_retries >= 10U)
    return CAI_OK;
  if (cai_runtime_cancel_requested(runtime))
    return cai_set_error(error, CAI_ERR_CANCELLED, "agent turn was cancelled");
  rc = cai_runtime_account_goal(runtime, &limited, error);
  if (rc != CAI_OK || limited)
    return rc != CAI_OK
               ? rc
               : cai_set_error(
                     error, CAI_ERR_LIMIT,
                     "goal token budget exhausted before capacity retry");
  attempt = ++runtime->capacity_retries;
  delay_ms = 30000L * (1L << (attempt < 4U ? attempt - 1U : 3U));
  snprintf(message, sizeof(message),
           "Selected model is at capacity; retrying in %ld seconds (%u/10).",
           delay_ms / 1000L, attempt);
  if (runtime->logger != NULL && !runtime->logger_disabled &&
      runtime->logger->warn != NULL) {
    pslog_field fields[4];
    memset(fields, 0, sizeof(fields));
    fields[0].key = "attempt";
    fields[0].type = PSLOG_FIELD_UNSIGNED;
    fields[0].as.unsigned_value = attempt;
    fields[1].key = "max_retries";
    fields[1].type = PSLOG_FIELD_UNSIGNED;
    fields[1].as.unsigned_value = 10U;
    fields[2].key = "delay_ms";
    fields[2].type = PSLOG_FIELD_SIGNED;
    fields[2].as.signed_value = delay_ms;
    fields[3].key = "provider_code";
    fields[3].type = PSLOG_FIELD_STRING;
    fields[3].as.string_value = failure->server_code;
    fields[3].value_len = strlen(failure->server_code);
    fields[0].key_len = strlen(fields[0].key);
    fields[1].key_len = strlen(fields[1].key);
    fields[2].key_len = strlen(fields[2].key);
    fields[3].key_len = strlen(fields[3].key);
    runtime->logger->warn(runtime->logger, message, fields, 4U);
  }
  pthread_mutex_lock(&runtime->lock);
  cai_runtime_clear_review_report_locked(runtime);
  if (runtime->record_transcript && runtime->history_summary_started)
    rc = cai_runtime_append_journal_event_locked(runtime, "assistant_text_end",
                                                 NULL, NULL, error);
  else
    rc = CAI_OK;
  runtime->history_summary_started = 0;
  if (rc == CAI_OK)
    rc = cai_runtime_execution_error_locked(runtime, "request.failed", NULL,
                                            NULL, failure, error);
  if (rc == CAI_OK)
    rc = cai_runtime_enqueue_locked(runtime, CAI_AGENT_EVENT_REQUEST_RETRY,
                                    message, strlen(message), NULL, NULL,
                                    runtime->state, error);
  pthread_mutex_unlock(&runtime->lock);
  if (rc == CAI_OK)
    rc = cai_runtime_capacity_wait(runtime, delay_ms, error);
  if (rc == CAI_OK && cai_runtime_cancel_requested(runtime))
    rc = cai_set_error(error, CAI_ERR_CANCELLED, "agent turn was cancelled");
  if (rc == CAI_OK) {
    pthread_mutex_lock(&runtime->lock);
    rc = cai_runtime_enqueue_locked(runtime,
                                    CAI_AGENT_EVENT_REQUEST_RETRY_STARTED, NULL,
                                    0U, NULL, NULL, runtime->state, error);
    pthread_mutex_unlock(&runtime->lock);
  }
  if (rc == CAI_OK)
    *retry = 1;
  return rc;
}
