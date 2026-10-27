/* Private goal prompt and empty-continuation policy. Called by the worker. */
static const unsigned char cai_goal_continuation_template[] = {
#include "cai_goal_continuation.inc"
};

static int cai_runtime_goal_prompt(cai_agent_runtime *runtime, char **out,
                                   cai_error *error) {
  cai_session_impl *goal = CAI_SESSION_IMPL(runtime->session);
  cai_buffer_builder result;
  const char *cursor = (const char *)cai_goal_continuation_template;
  const char *slot;
  char used[32], budget[32], remaining[32];
  int rc = CAI_OK;
  memset(&result, 0, sizeof(result));
  *out = NULL;
  snprintf(used, sizeof(used), "%lld", goal->goal_tokens_used);
  if (goal->goal_has_token_budget) {
    snprintf(budget, sizeof(budget), "%lld", goal->goal_token_budget);
    snprintf(remaining, sizeof(remaining), "%lld",
             goal->goal_tokens_used >= goal->goal_token_budget
                 ? 0LL
                 : goal->goal_token_budget - goal->goal_tokens_used);
  } else {
    strcpy(budget, "none");
    strcpy(remaining, "unbounded");
  }
  while (rc == CAI_OK && (slot = strstr(cursor, "{{ ")) != NULL) {
    rc = cai_buffer_append(&result, cursor, (size_t)(slot - cursor), error);
    if (rc != CAI_OK)
      break;
    if (strncmp(slot, "{{ objective }}", 15U) == 0) {
      const char *p = goal->goal_objective;
      for (; rc == CAI_OK && *p != '\0'; p++) {
        if (*p == '&')
          rc = cai_buffer_append_cstr(&result, "&amp;", error);
        else if (*p == '<')
          rc = cai_buffer_append_cstr(&result, "&lt;", error);
        else if (*p == '>')
          rc = cai_buffer_append_cstr(&result, "&gt;", error);
        else
          rc = cai_buffer_append(&result, p, 1U, error);
      }
      cursor = slot + 15U;
    } else if (strncmp(slot, "{{ tokens_used }}", 17U) == 0) {
      rc = cai_buffer_append_cstr(&result, used, error);
      cursor = slot + 17U;
    } else if (strncmp(slot, "{{ token_budget }}", 18U) == 0) {
      rc = cai_buffer_append_cstr(&result, budget, error);
      cursor = slot + 18U;
    } else if (strncmp(slot, "{{ remaining_tokens }}", 22U) == 0) {
      rc = cai_buffer_append_cstr(&result, remaining, error);
      cursor = slot + 22U;
    } else {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                         "unknown goal prompt placeholder");
    }
  }
  if (rc == CAI_OK)
    rc = cai_buffer_append_cstr(&result, cursor, error);
  if (rc == CAI_OK)
    *out = result.data;
  else
    cai_free_mem(NULL, result.data);
  return rc;
}

/* Unicode White_Space, as used by Codex's trim(). Keep decoder state across
 * chunks so a split UTF-8 whitespace character does not look like activity. */
static int cai_runtime_goal_space(unsigned int cp) {
  return cp == 0x20U || (cp >= 9U && cp <= 13U) || cp == 0x85U || cp == 0xa0U ||
         cp == 0x1680U || (cp >= 0x2000U && cp <= 0x200aU) || cp == 0x2028U ||
         cp == 0x2029U || cp == 0x202fU || cp == 0x205fU || cp == 0x3000U;
}

static void cai_runtime_goal_note_text(cai_agent_runtime *runtime,
                                       const void *bytes, size_t length) {
  const unsigned char *p = bytes;
  size_t i;
  for (i = 0U; !runtime->goal_turn_has_activity && i < length; i++) {
    unsigned int cp = p[i];
    if (runtime->goal_utf8_remaining > 0 && (cp & 0xc0U) == 0x80U) {
      runtime->goal_utf8_value = (runtime->goal_utf8_value << 6) | (cp & 0x3fU);
      if (--runtime->goal_utf8_remaining > 0)
        continue;
      cp = runtime->goal_utf8_value;
    } else if (cp >= 0xc2U && cp <= 0xf4U) {
      runtime->goal_utf8_remaining = cp < 0xe0U ? 1 : cp < 0xf0U ? 2 : 3;
      runtime->goal_utf8_value = cp & (cp < 0xe0U   ? 0x1fU
                                       : cp < 0xf0U ? 0xfU
                                                    : 7U);
      continue;
    } else if (runtime->goal_utf8_remaining > 0 || cp >= 0x80U) {
      runtime->goal_turn_has_activity = 1;
      break;
    }
    if (!cai_runtime_goal_space(cp))
      runtime->goal_turn_has_activity = 1;
  }
}

static int cai_runtime_goal_empty_finish(cai_agent_runtime *runtime,
                                         cai_error *error) {
  cai_session_impl *goal = CAI_SESSION_IMPL(runtime->session);
  static const char message[] =
      "Goal blocked after three consecutive empty automatic continuations. "
      "No text, reasoning, tool or compaction activity was produced.";
  int rc;
  if (!runtime->goal_turn_automatic || runtime->goal_turn_has_activity ||
      runtime->goal_utf8_remaining > 0 || goal->goal_status == NULL ||
      strcmp(goal->goal_status, "active") != 0) {
    runtime->goal_empty_turns = 0U;
    return CAI_OK;
  }
  runtime->goal_empty_turns++;
  if (runtime->goal_empty_turns < 3U)
    return CAI_OK;
  rc = cai_runtime_goal_replace_status(runtime->session, "blocked", error);
  if (rc != CAI_OK)
    return rc;
  goal->goal_updated_at = (long long)time(NULL);
  cai_session_goal_stop_elapsed(runtime->session, goal->goal_updated_at);
  rc = cai_runtime_goal_add_context(runtime, message, error);
  if (rc == CAI_OK)
    rc = cai_session_commit_pending_inputs(runtime->session, error);
  if (rc == CAI_OK)
    rc = cai_runtime_refresh_goal_projection(runtime, error);
  if (rc == CAI_OK) {
    pthread_mutex_lock(&runtime->lock);
    rc = cai_runtime_enqueue_locked(runtime, CAI_AGENT_EVENT_GOAL_CHANGED,
                                    message, sizeof(message) - 1U, NULL, NULL,
                                    runtime->state, error);
    pthread_mutex_unlock(&runtime->lock);
    if (!runtime->logger_disabled && runtime->logger != NULL &&
        runtime->logger->warnf != NULL)
      runtime->logger->warnf(runtime->logger, message, "session_id=%s",
                             runtime->session_id);
  }
  return rc;
}
