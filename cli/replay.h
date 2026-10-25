#ifndef CAI_CLI_REPLAY_H
#define CAI_CLI_REPLAY_H
#include <cai/session_store.h>

/* Console-only tail index. Retains sequence ranges and pending input IDs,
 * never conversation text. Feed only events visible to the runtime. */
typedef struct cai_cli_replay cai_cli_replay;
int cai_cli_replay_open(size_t limit, cai_cli_replay **out, cai_error *error);
int cai_cli_replay_feed(cai_cli_replay *replay,
                        const cai_agent_session_event *event, cai_error *error);
int cai_cli_replay_finish(cai_cli_replay *replay, cai_error *error);
int cai_cli_replay_contains(void *replay, unsigned long long sequence);
/* Zero when no console events were selected. */
unsigned long long cai_cli_replay_first(const cai_cli_replay *replay);
unsigned long long cai_cli_replay_last(const cai_cli_replay *replay);
size_t cai_cli_replay_omitted(const cai_cli_replay *replay);
void cai_cli_replay_close(cai_cli_replay *replay);
#endif
