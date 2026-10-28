#ifndef CAI_CLI_TOOL_RESULT_H
#define CAI_CLI_TOOL_RESULT_H
#include <cai/display.h>

typedef struct cai_cli_tool_result cai_cli_tool_result;
/* Prepare known built-in results without writing output. NULL means the
 * result is unknown/malformed and the generic tool renderer should handle it.
 * The event payload is borrowed until close. Raw results remain untouched. */
int cai_cli_tool_result_open(const cai_agent_runtime_event *event,
                             cai_cli_tool_result **out, cai_error *error);
int cai_cli_tool_result_heading(cai_cli_tool_result *result,
                                cai_tool_display *display,
                                const cai_agent_runtime_event *event,
                                cai_error *error);
/* Feed Markdown incrementally into the application's existing renderer. */
int cai_cli_tool_result_write(cai_cli_tool_result *result,
                              int (*write)(void *, const char *, size_t),
                              void *context, cai_error *error);
void cai_cli_tool_result_close(cai_cli_tool_result *result);
#endif
