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
  /** Enable patch backgrounds and bright success/failure markers. Activity
   * verbs use normal cyan; command/path details use bold bright cyan. Command
   * output retains recognized ANSI SGR colors/styles only when enabled. */
  int color;
  /** Default captured by the next tool call; compact is the zero default. */
  int mode;
  /** Command preview line count; zero selects five. */
  size_t preview_lines;
  /** Optional host callback before each activity heading. Ensure a blank row
   * after any preceding feed output; return a CAI status. Context is borrowed.
   * Without it the renderer separates its own activity blocks. */
  int (*begin_block)(void *context, cai_error *error);
  void *begin_block_context;
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
/** Enable/disable colors between events without replacing active calls.
 * Future output uses the new setting; command identity, preview limits and
 * split UTF-8/control decoding state are retained. enabled must be 0 or 1. */
int cai_tool_display_set_color(cai_tool_display *display, int enabled,
                               cai_error *error);
/** Set terminal columns for numbered, wrapped patch rows (32..4096).
 * Zero selects the default unwrapped patch format. Colored changed rows fill
 * the specified width; continuations align below the content. Width changes
 * apply at the next logical patch row, including during an active tool call.
 * Uses the host's LC_CTYPE for Unicode cell widths. No terminal I/O is queried.
 * Native runtime location events provide real file numbers; unavailable
 * positions leave a blank gutter. Sources/sinks remain borrowed.
 */
int cai_tool_display_set_width(cai_tool_display *display, size_t width,
                               cai_error *error);
/** Consume a borrowed owner-thread runtime event. Payloads are written as they
 * arrive; command output is never retained for later expansion. Sink failures
 * propagate to the caller. Command previews filter terminal controls, retain
 * recognized SGR colors/styles when color is enabled, and reset styling before
 * returning. Split controls/UTF-8 and styles are tracked per command. CRLF is
 * normalized; standalone CR starts a new transcript line. Incomplete controls
 * are discarded. Malformed UTF-8 and non-control binary bytes are escaped;
 * other tool labels, results and patch contents continue to escape control
 * bytes literally. Completed command headings include terminal_duration_ms as
 * whole elapsed seconds (e.g. "Ran for 1m3s"); the duration shares the activity
 * verb's color. REASONING_TOOL_RESULT renders the user-facing outcome without
 * altering the model's tool result.
 */
int cai_tool_display_event(cai_tool_display *display,
                           const cai_agent_runtime_event *event,
                           cai_error *error);
/** Present a host-formatted tool completion instead of its raw result.
 * event must be a completed/failed tool event. verb and label are plain text,
 * escaped by the renderer; success selects the marker color. Releases the
 * originating display call just like normal completion. Hosts can then render
 * structured details through their own Markdown renderer. The original event
 * and model-visible tool result are never modified. */
int cai_tool_display_summary(cai_tool_display *display,
                             const cai_agent_runtime_event *event,
                             const char *verb, const char *label, int success,
                             cai_error *error);
/** Begin a saved completion with its final heading before streaming its body.
 * Unlike summary(), retains per-call preview/control state. Follow with
 * TERMINAL_OUTPUT events and a TERMINAL_COMMAND_COMPLETED event (empty data)
 * to flush and release it, without another heading. The completion's terminal
 * status is used by that final event for command failures. No output is saved
 * or executed. A "Ran" verb includes event->terminal_duration_ms in the
 * heading. Labels and success follow summary() semantics. */
int cai_tool_display_replay_begin(cai_tool_display *display,
                                  const cai_agent_runtime_event *event,
                                  const char *verb, const char *label,
                                  int success, cai_error *error);
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
/** Direct patch rendering with explicit terminal columns; zero is unwrapped.
 * Add-file rows start at one. Updates can use numeric "@@ -OLD +NEW @@"
 * headers; bare/context-anchor headers leave the gutter blank because the
 * source alone cannot identify matched file positions. No files are read.
 */
int cai_patch_render_width(cai_source *patch,
                           const cai_tool_display_config *config, size_t width,
                           cai_error *error);
#ifdef __cplusplus
}
#endif
#endif
