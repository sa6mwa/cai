#include "../cli/replay.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>

static void feed(cai_cli_replay *r, unsigned long long sequence,
                 const char *type, const char *data, cai_error *error) {
  cai_agent_session_event event;
  event.sequence = sequence;
  event.type = type;
  event.data = data;
  assert(cai_cli_replay_feed(r, &event, error) == CAI_OK);
}
int main(void) {
  cai_cli_replay *r = NULL;
  cai_error error;
  unsigned long long base;
  int i;
  cai_error_init(&error);
  assert(cai_cli_replay_open(0U, &r, &error) == CAI_ERR_INVALID && r == NULL);
  cai_error_cleanup(&error);
  assert(cai_cli_replay_open((size_t)LONG_MAX, &r, &error) == CAI_OK);
  assert(cai_cli_replay_finish(r, &error) == CAI_OK);
  assert(cai_cli_replay_first(r) == 0U);
  feed(r, 1U, "turn_submitted", "prompt", &error);
  feed(r, ~0ULL, "assistant_text_delta", "large sequence gap", &error);
  assert(cai_cli_replay_finish(r, &error) == CAI_OK);
  assert(cai_cli_replay_first(r) == 1U && cai_cli_replay_last(r) == ~0ULL);
  assert(cai_cli_replay_contains(r, 1U) && cai_cli_replay_contains(r, ~0ULL));
  cai_cli_replay_close(r);
  assert(cai_cli_replay_open(20U, &r, &error) == CAI_OK);
  for (i = 0; i < 25; i++) {
    base = (unsigned long long)i * 6U;
    feed(r, base + 1U, "turn_submitted", "prompt", &error);
    feed(r, base + 2U, "tool_call_completed", "tool", &error);
    feed(r, base + 3U, "steering_queued", "steer", &error);
    feed(r, base + 4U, "assistant_text_delta", "first", &error);
    feed(r, base + 5U, "assistant_text_delta", "second", &error);
    feed(r, base + 6U, "assistant_text_end", NULL, &error);
  }
  assert(cai_cli_replay_finish(r, &error) == CAI_OK);
  assert(cai_cli_replay_omitted(r) == 5U && cai_cli_replay_first(r) == 31U);
  for (base = 1U; base <= 150U; base++)
    assert(cai_cli_replay_contains(r, base) == (base >= 31U));
  cai_cli_replay_close(r);
  assert(cai_cli_replay_open(1U, &r, &error) == CAI_OK);
  feed(r, 1U, "input_journal_v2", NULL, &error);
  feed(r, 2U, "turn_submitted", "first", &error);
  feed(r, 3U, "input_consumed", "2", &error);
  feed(r, 4U, "assistant_text_delta", "old partial", &error);
  feed(r, 5U, "turn_queued", "next", &error);
  feed(r, 6U, "steering_queued", "next steer", &error);
  feed(r, 7U, "assistant_text_delta", "old ending", &error);
  feed(r, 8U, "assistant_text_end", NULL, &error);
  feed(r, 9U, "input_consumed", "5", &error);
  feed(r, 10U, "input_consumed", "6", &error);
  feed(r, 11U, "assistant_text_delta", "new answer", &error);
  feed(r, 12U, "assistant_text_end", NULL, &error);
  assert(cai_cli_replay_omitted(r) == 1U);
  assert(cai_cli_replay_first(r) == 5U);
  assert(cai_cli_replay_contains(r, 5U) && cai_cli_replay_contains(r, 6U));
  assert(!cai_cli_replay_contains(r, 7U) && !cai_cli_replay_contains(r, 8U));
  assert(cai_cli_replay_contains(r, 11U) && cai_cli_replay_contains(r, 12U));
  /* Shells inside a turn stay attached; standalone executions count once. */
  feed(r, 13U, "user_shell_group", "turn", &error);
  feed(r, 14U, "user_shell_started", "sh", &error);
  feed(r, 15U, "user_shell_output_tail", "text", &error);
  feed(r, 16U, "user_shell_completed", "result", &error);
  assert(cai_cli_replay_contains(r, 5U) && cai_cli_replay_contains(r, 16U));
  feed(r, 17U, "user_shell_group", "entry", &error);
  feed(r, 18U, "user_shell_started", "sh", &error);
  feed(r, 19U, "user_shell_output_tail", "text", &error);
  feed(r, 20U, "user_shell_completed", "result", &error);
  assert(!cai_cli_replay_contains(r, 5U) && cai_cli_replay_contains(r, 18U));
  feed(r, 21U, "turn_queued", "pending", &error);
  feed(r, 22U, "steering_queued", "pending steer", &error);
  assert(cai_cli_replay_finish(r, &error) == CAI_OK);
  assert(cai_cli_replay_omitted(r) == 3U);
  assert(!cai_cli_replay_contains(r, 18U));
  assert(cai_cli_replay_contains(r, 21U) && cai_cli_replay_contains(r, 22U));
  cai_cli_replay_close(r);
  /* Gaps from trimmed/oversized events must never reappear in the feed. */
  assert(cai_cli_replay_open(1U, &r, &error) == CAI_OK);
  feed(r, 1U, "turn_submitted", "prompt", &error);
  feed(r, 2U, "replay_event_omitted", NULL, &error);
  feed(r, 3U, "assistant_text_delta", "answer", &error);
  assert(!cai_cli_replay_contains(r, 2U));
  assert(cai_cli_replay_contains(r, 3U));
  cai_cli_replay_close(r);
  cai_error_cleanup(&error);
  return 0;
}
