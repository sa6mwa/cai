#ifndef CAI_CLI_REPLAY_TOOLS_H
#define CAI_CLI_REPLAY_TOOLS_H
#include <cai/display.h>
#include <cai/execution.h>

typedef struct cai_cli_replay_tools cai_cli_replay_tools;
/* Index only bounded labels/status, never tool output. Older selected turns
 * can be evicted while the normal recovery/index pass advances. */
int cai_cli_replay_tools_index(cai_cli_replay_tools **tools,
                               const cai_execution_record *record,
                               unsigned long long sequence,
                               unsigned long long first, cai_error *error);
/* Returns handled=0 for non-tool execution records. Saved bodies are decoded
 * incrementally on the owner thread and use the live display's preview limit.
 */
int cai_cli_replay_tools_render(cai_cli_replay_tools *tools,
                                const cai_execution_record *record,
                                cai_tool_display *display,
                                int (*markdown)(void *, const char *, size_t),
                                void *context, int *handled, cai_error *error);
/* Flush any command whose recorded tail was interrupted. */
int cai_cli_replay_tools_finish(cai_cli_replay_tools *tools, cai_error *error);
void cai_cli_replay_tools_close(cai_cli_replay_tools *tools);
#endif
