#define _POSIX_C_SOURCE 200809L
#include "replay.h"
#include "pouch_stream.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct replay_range {
  unsigned long long first, last;
} replay_range;
typedef struct replay_entry {
  struct replay_entry *next;
  replay_range *ranges;
  size_t count;
} replay_entry;
typedef struct replay_input {
  struct replay_input *next;
  unsigned long long sequence;
  int turn;
} replay_input;
struct cai_cli_replay {
  replay_entry *first, *last;
  replay_input *inputs, *inputs_last;
  size_t limit, count, omitted;
  unsigned long long through;
  int journal_v2, merge_shell;
};

static void entry_free(replay_entry *entry) {
  free(entry->ranges);
  free(entry);
}
static int entry_begin(cai_cli_replay *r, cai_error *error) {
  replay_entry *entry = calloc(1U, sizeof(*entry));
  if (entry == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "index console replay entry");
  if (r->last != NULL)
    r->last->next = entry;
  else
    r->first = entry;
  r->last = entry;
  if (r->count == r->limit) {
    entry = r->first;
    r->first = entry->next;
    entry_free(entry);
    r->omitted++;
  } else
    r->count++;
  return CAI_OK;
}
static int entry_add(cai_cli_replay *r, unsigned long long sequence,
                     cai_error *error) {
  replay_entry *entry = r->last;
  replay_range *ranges;
  if (entry == NULL)
    return CAI_OK;
  if (entry->count != 0U && sequence > entry->ranges[entry->count - 1U].last &&
      sequence - entry->ranges[entry->count - 1U].last == 1U) {
    entry->ranges[entry->count - 1U].last = sequence;
    return CAI_OK;
  }
  if (entry->count >= (size_t)-1 / sizeof(*ranges))
    return cai_cli_error(error, CAI_ERR_LIMIT,
                         "console replay index too large");
  ranges = realloc(entry->ranges, (entry->count + 1U) * sizeof(*ranges));
  if (ranges == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "index console replay sequence");
  entry->ranges = ranges;
  ranges[entry->count].first = ranges[entry->count].last = sequence;
  entry->count++;
  return CAI_OK;
}
int cai_cli_replay_open(size_t limit, cai_cli_replay **out, cai_error *error) {
  *out = NULL;
  if (limit == 0U)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "positive replay limit required");
  *out = calloc(1U, sizeof(**out));
  if (*out == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "create console replay index");
  (*out)->limit = limit;
  return CAI_OK;
}
int cai_cli_replay_feed(cai_cli_replay *r, const cai_agent_session_event *event,
                        cai_error *error) {
  const char *type = event->type;
  replay_input *input, **link;
  int turn =
      strcmp(type, "turn_submitted") == 0 || strcmp(type, "turn_queued") == 0;
  int steering = strcmp(type, "steering_queued") == 0;
  int rc = CAI_OK;
  r->through = event->sequence;
  if (strcmp(type, "replay_event_omitted") == 0)
    return CAI_OK;
  if (strcmp(type, "input_journal_v2") == 0)
    r->journal_v2 = 1;
  if (strcmp(type, "user_shell_group") == 0) {
    r->merge_shell = event->data != NULL && strcmp(event->data, "turn") == 0;
    return CAI_OK;
  }
  if (turn || steering) {
    if (!r->journal_v2) {
      if (turn || r->last == NULL)
        rc = entry_begin(r, error);
      return rc == CAI_OK ? entry_add(r, event->sequence, error) : rc;
    }
    input = calloc(1U, sizeof(*input));
    if (input == NULL)
      return cai_cli_error(error, CAI_ERR_NOMEM, "index pending replay input");
    input->sequence = event->sequence;
    input->turn = turn;
    if (r->inputs_last != NULL)
      r->inputs_last->next = input;
    else
      r->inputs = input;
    r->inputs_last = input;
    return CAI_OK;
  }
  if (strcmp(type, "input_consumed") == 0 && event->data != NULL) {
    char *end;
    unsigned long long sequence;
    errno = 0;
    sequence = strtoull(event->data, &end, 10);
    if (errno == 0 && end != event->data && *end == '\0') {
      for (link = &r->inputs; *link != NULL; link = &(*link)->next) {
        input = *link;
        if (input->sequence != sequence)
          continue;
        if (input->turn || r->last == NULL)
          rc = entry_begin(r, error);
        if (rc == CAI_OK)
          rc = entry_add(r, input->sequence, error);
        *link = input->next;
        if (r->inputs_last == input) {
          r->inputs_last = r->inputs;
          while (r->inputs_last != NULL && r->inputs_last->next != NULL)
            r->inputs_last = r->inputs_last->next;
        }
        free(input);
        break;
      }
    }
  }
  if (rc == CAI_OK && strcmp(type, "user_shell_started") == 0) {
    if (!r->merge_shell || r->last == NULL)
      rc = entry_begin(r, error);
    r->merge_shell = 0;
  }
  if (rc == CAI_OK && r->last == NULL &&
      (strcmp(type, "assistant_text_delta") == 0 ||
       strcmp(type, "policy_diagnostic") == 0))
    rc = entry_begin(r, error);
  return rc == CAI_OK ? entry_add(r, event->sequence, error) : rc;
}
int cai_cli_replay_finish(cai_cli_replay *r, cai_error *error) {
  replay_input *input;
  int rc = CAI_OK;
  while (rc == CAI_OK && (input = r->inputs) != NULL) {
    if (input->turn || r->last == NULL)
      rc = entry_begin(r, error);
    if (rc == CAI_OK)
      rc = entry_add(r, input->sequence, error);
    r->inputs = input->next;
    free(input);
  }
  r->inputs_last = NULL;
  return rc;
}
int cai_cli_replay_contains(void *context, unsigned long long sequence) {
  const cai_cli_replay *r = context;
  const replay_entry *entry;
  size_t i;
  for (entry = r->first; entry != NULL; entry = entry->next)
    for (i = 0U; i < entry->count; i++)
      if (sequence >= entry->ranges[i].first &&
          sequence <= entry->ranges[i].last)
        return 1;
  return 0;
}
unsigned long long cai_cli_replay_first(const cai_cli_replay *r) {
  const replay_entry *entry;
  unsigned long long first = 0U;
  size_t i;
  for (entry = r->first; entry != NULL; entry = entry->next)
    for (i = 0U; i < entry->count; i++)
      if (first == 0U || entry->ranges[i].first < first)
        first = entry->ranges[i].first;
  return first;
}
unsigned long long cai_cli_replay_last(const cai_cli_replay *r) {
  return r->through;
}
size_t cai_cli_replay_omitted(const cai_cli_replay *r) { return r->omitted; }
void cai_cli_replay_close(cai_cli_replay *r) {
  replay_entry *entry;
  replay_input *input;
  if (r == NULL)
    return;
  while ((entry = r->first) != NULL) {
    r->first = entry->next;
    entry_free(entry);
  }
  while ((input = r->inputs) != NULL) {
    r->inputs = input->next;
    free(input);
  }
  free(r);
}
