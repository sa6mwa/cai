#ifndef CAI_CLI_TOOL_RESULT_H
#define CAI_CLI_TOOL_RESULT_H
#include <cai/display.h>

typedef struct cai_cli_tool_result cai_cli_tool_result;
/* Known receipts and the Markdown fallback for non-command results. NULL
 * selects a native heading/patch preview or command renderer. Event payloads
 * remain borrowed and model-visible results are never changed. */
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
typedef struct cai_cli_tool_text {
  int started, line_start, format;
} cai_cli_tool_text;
/* UTF-8-aligned payload chunks; bounded state only. Failures are literal
 * blockquotes; generic JSON uses indented code; other results are Markdown. */
int cai_cli_tool_text_write(cai_cli_tool_text *state, const char *text,
                            size_t length, int final, int failed,
                            int (*write)(void *, const char *, size_t),
                            void *context, cai_error *error);
#endif
