/** @file cai/display.h
 * Sink-based terminal presentation of agent tool activity and submitted
 * patches. This module has no softline or libmdf dependency.
 */
#ifndef CAI_DISPLAY_H
#define CAI_DISPLAY_H
#include <cai/agent_runtime.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct cai_tool_display cai_tool_display;
typedef enum cai_tool_output_mode {
  CAI_TOOL_OUTPUT_COMPACT = 0,
  CAI_TOOL_OUTPUT_FULL = 1
} cai_tool_output_mode;
typedef struct cai_tool_display_config {
  /** Borrowed output sink. The renderer never closes it. */
  cai_sink *sink;
  /** Enable patch backgrounds, bright success/failure markers and bold cyan
   * activity headings. Heading color is independent of the result marker. */
  int color;
  /** Default captured by the next tool call; compact is the zero default. */
  int mode;
  /** Command preview line count; zero selects five. */
  size_t preview_lines;
} cai_tool_display_config;

/** Create an owner-thread renderer. No sink ownership is transferred.
 * Finish events release call state; close releases any remaining active calls.
 * The renderer does not synchronize concurrent calls.
 */
int cai_tool_display_new(const cai_tool_display_config *config,
                         cai_tool_display **out, cai_error *error);
/** Select the mode for future calls. Active calls keep their captured mode. */
int cai_tool_display_set_mode(cai_tool_display *display, int mode,
                              cai_error *error);
/** Consume a borrowed owner-thread runtime event. Payloads are written as they
 * arrive; command output is never retained for later expansion. Sink failures
 * propagate to the caller. Control bytes are escaped before terminal output.
 */
int cai_tool_display_event(cai_tool_display *display,
                           const cai_agent_runtime_event *event,
                           cai_error *error);
/** Release the renderer and retained state. NULL is accepted; the sink is
 * left open. Do not call from its sink callback.
 */
void cai_tool_display_close(cai_tool_display *display);
/** Render a submitted Codex apply_patch body directly from a source to a sink.
 * No files or VCS state are read. Full mode preserves all supplied context;
 * compact mode retains one surrounding line and collapses interior context
 * with a vertical ellipsis. A bounded line buffer is used; long changed lines
 * flow in chunks, while exceptionally long compact context is visibly clipped.
 * Sources and sinks remain caller-owned. Input need not be rewindable.
 */
int cai_patch_render(cai_source *patch, const cai_tool_display_config *config,
                     cai_error *error);
#ifdef __cplusplus
}
#endif
#endif
