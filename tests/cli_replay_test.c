#include "../cli/replay.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct ordered_output {
  char text[256];
  int fail;
} ordered_output;
static int ordered_emit(void *context, const cai_agent_session_event *event,
                        cai_error *error) {
  ordered_output *out = context;
  (void)error;
  if (out->fail)
    return CAI_ERR_TRANSPORT;
  if (event->data != NULL && strcmp(event->type, "input_consumed") != 0) {
    assert(strlen(out->text) + strlen(event->data) + 2U < sizeof(out->text));
    strcat(out->text, event->data);
    strcat(out->text, "|");
  }
  return CAI_OK;
}
static void test_order(cai_error *error) {
  cai_cli_replay_order *order = NULL;
  cai_agent_session_event event;
  ordered_output out;
  memset(&out, 0, sizeof(out));
  assert(cai_cli_replay_order_open(1, &order, error) == CAI_OK);
  event.sequence = 2U;
  event.type = "turn_submitted";
  event.data = "prompt";
  assert(cai_cli_replay_order_feed(order, &event, ordered_emit, &out, error) ==
         CAI_OK);
  assert(out.text[0] == '\0');
  event.sequence = 3U;
  event.type = "input_consumed";
  event.data = "2";
  assert(cai_cli_replay_order_feed(order, &event, ordered_emit, &out, error) ==
         CAI_OK);
  event.sequence = 4U;
  event.type = "assistant_text_delta";
  event.data = "first";
  assert(cai_cli_replay_order_feed(order, &event, ordered_emit, &out, error) ==
         CAI_OK);
  event.sequence = 5U;
  event.type = "steering_queued";
  event.data = "steer";
  assert(cai_cli_replay_order_feed(order, &event, ordered_emit, &out, error) ==
         CAI_OK);
  event.sequence = 6U;
  event.type = "assistant_text_delta";
  event.data = "last";
  assert(cai_cli_replay_order_feed(order, &event, ordered_emit, &out, error) ==
         CAI_OK);
  assert(strcmp(out.text, "prompt|first|last|") == 0);
  event.sequence = 7U;
  event.type = "input_consumed";
  event.data = "5";
  out.fail = 1;
  assert(cai_cli_replay_order_feed(order, &event, ordered_emit, &out, error) ==
         CAI_ERR_TRANSPORT);
  out.fail = 0;
  assert(cai_cli_replay_order_feed(order, &event, ordered_emit, &out, error) ==
         CAI_OK);
  assert(strcmp(out.text, "prompt|first|last|steer|") == 0);
  /* Duplicate/unmatched/malformed receipts never duplicate a prompt. */
  assert(cai_cli_replay_order_feed(order, &event, ordered_emit, &out, error) ==
         CAI_OK);
  event.data = "999";
  assert(cai_cli_replay_order_feed(order, &event, ordered_emit, &out, error) ==
         CAI_OK);
  event.data = "not a number";
  assert(cai_cli_replay_order_feed(order, &event, ordered_emit, &out, error) ==
         CAI_OK);
  assert(strcmp(out.text, "prompt|first|last|steer|") == 0);
  event.sequence = 8U;
  event.type = "turn_queued";
  event.data = "not yet delivered";
  assert(cai_cli_replay_order_feed(order, &event, ordered_emit, &out, error) ==
         CAI_OK);
  cai_cli_replay_order_close(order);
  assert(strcmp(out.text, "prompt|first|last|steer|") == 0);
}

static void feed(cai_cli_replay *r, unsigned long long sequence,
                 const char *type, const char *data, cai_error *error) {
  cai_agent_session_event event;
  event.sequence = sequence;
  event.type = type;
  event.data = data;
  assert(cai_cli_replay_feed(r, &event, error) == CAI_OK);
}
static void child_event(cai_cli_replay *r, unsigned long long sequence,
                        const char *type, const char *kind, const char *id,
                        unsigned long long source, cai_error *error) {
  char json[1024];
  snprintf(json, sizeof(json),
           "{\"kind\":\"%s\",\"session_id\":\"%s\",\"subagent\":\"review\","
           "\"timestamp_ns\":1,\"source_sequence\":%llu,\"offset\":0,"
           "\"final\":true,\"line_start\":true,\"data\":\"x\"}",
           kind, id, source);
  feed(r, sequence, type, json, error);
}
static void test_review_ranges(cai_error *error) {
  cai_cli_replay *r = NULL;
  assert(cai_cli_replay_open(20U, &r, error) == CAI_OK);
  child_event(r, 1U, "subagent", "assistant_text_delta", "review", 10U, error);
  child_event(r, 2U, "execution", "tool.started", "review", 11U, error);
  child_event(r, 3U, "subagent", "assistant_text_delta", "review", 12U, error);
  child_event(r, 4U, "subagent", "review_markdown_delta", "review", 14U, error);
  child_event(r, 5U, "subagent", "assistant_text_delta", "invalid", 12U, error);
  child_event(r, 6U, "subagent", "assistant_text_delta", "steered", 20U, error);
  child_event(r, 7U, "subagent", "input_consumed", "steered", 21U, error);
  child_event(r, 8U, "subagent", "assistant_text_delta", "steered", 22U, error);
  child_event(r, 9U, "subagent", "review_markdown_delta", "steered", 24U,
              error);
  /* A projection interrupted before its end marker must not hide the raw
   * report that is still available for inspection. */
  assert(cai_cli_replay_child_text_visible(r, "review", 12U));
  assert(cai_cli_replay_child_text_visible(r, "steered", 22U));
  child_event(r, 10U, "subagent", "review_markdown_end", "review", 15U, error);
  child_event(r, 11U, "subagent", "review_markdown_end", "steered", 25U, error);
  assert(cai_cli_replay_child_text_visible(r, "review", 10U));
  assert(!cai_cli_replay_child_text_visible(r, "review", 12U));
  assert(cai_cli_replay_child_text_visible(r, "review", 14U));
  assert(cai_cli_replay_child_text_visible(r, "invalid", 12U));
  assert(cai_cli_replay_child_text_visible(r, "unknown", 12U));
  assert(cai_cli_replay_child_text_visible(r, "steered", 20U));
  assert(!cai_cli_replay_child_text_visible(r, "steered", 22U));
  cai_cli_replay_close(r);
}
int main(void) {
  cai_cli_replay *r = NULL;
  cai_error error;
  unsigned long long base;
  int i;
  cai_error_init(&error);
  test_order(&error);
  test_review_ranges(&error);
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
  /* A conversation containing only a manual review still has an inspectable
   * activity entry; individual tool records never become separate turns. */
  assert(cai_cli_replay_open(1U, &r, &error) == CAI_OK);
  feed(r, 1U, "input_journal_v2", NULL, &error);
  feed(r, 2U, "execution", "first tool record", &error);
  feed(r, 3U, "execution", "failure record", &error);
  assert(cai_cli_replay_finish(r, &error) == CAI_OK);
  assert(cai_cli_replay_omitted(r) == 0U);
  assert(cai_cli_replay_contains(r, 2U) && cai_cli_replay_contains(r, 3U));
  cai_cli_replay_close(r);
  /* A child's turn/input markers are presentation inside its parent entry,
   * not independent parent inputs that can evict the parent's reply. */
  assert(cai_cli_replay_open(1U, &r, &error) == CAI_OK);
  feed(r, 1U, "input_journal_v2", NULL, &error);
  feed(r, 2U, "turn_submitted", "parent", &error);
  feed(r, 3U, "input_consumed", "2", &error);
  feed(r, 4U, "assistant_text_delta", "parent answer", &error);
  feed(r, 5U, "subagent", "child turn_submitted", &error);
  feed(r, 6U, "subagent", "child input_consumed", &error);
  feed(r, 7U, "subagent", "child assistant text", &error);
  assert(cai_cli_replay_finish(r, &error) == CAI_OK);
  assert(cai_cli_replay_omitted(r) == 0U);
  assert(cai_cli_replay_contains(r, 2U) && cai_cli_replay_contains(r, 4U));
  assert(cai_cli_replay_contains(r, 5U) && cai_cli_replay_contains(r, 7U));
  cai_cli_replay_close(r);
  cai_error_cleanup(&error);
  return 0;
}
