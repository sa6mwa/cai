/** @file cai/agent_runtime.h
 *  Owner-thread event runtime for CAI agent presets.
 */
#ifndef CAI_AGENT_RUNTIME_H
#define CAI_AGENT_RUNTIME_H

#include <cai/cai.h>
#include <cai/session_store.h>
#include <cai/tools/terminal.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cai_agent_runtime cai_agent_runtime;

/** Observable execution state of an agent runtime. */
typedef enum cai_agent_run_state {
  CAI_AGENT_IDLE = 0,
  CAI_AGENT_SAMPLING = 1,
  CAI_AGENT_DISPATCHING_TOOL = 2,
  CAI_AGENT_COMPLETED = 3,
  CAI_AGENT_FAILED = 4,
  CAI_AGENT_CANCELLED = 5
} cai_agent_run_state;

/** Stable semantic outcome classification for a runtime tool event. */
typedef enum cai_agent_tool_action {
  CAI_AGENT_TOOL_ACTION_NONE = 0,
  CAI_AGENT_TOOL_ACTION_READ = 1,
  CAI_AGENT_TOOL_ACTION_LIST = 2,
  CAI_AGENT_TOOL_ACTION_VIEW = 3,
  CAI_AGENT_TOOL_ACTION_PATCH = 4,
  CAI_AGENT_TOOL_ACTION_EXECUTE = 5,
  CAI_AGENT_TOOL_ACTION_WRITE_STDIN = 6,
  CAI_AGENT_TOOL_ACTION_GET_GOAL = 7,
  CAI_AGENT_TOOL_ACTION_CREATE_GOAL = 8,
  CAI_AGENT_TOOL_ACTION_UPDATE_GOAL = 9,
  CAI_AGENT_TOOL_ACTION_CLEAR_GOAL = 10,
  CAI_AGENT_TOOL_ACTION_IMAGE_GENERATION = 11,
  CAI_AGENT_TOOL_ACTION_EXTERNAL = 12
} cai_agent_tool_action;

/** Event emitted by an agent runtime. */
typedef enum cai_agent_runtime_event_type {
  CAI_AGENT_EVENT_RUN_STARTED = 1,
  CAI_AGENT_EVENT_RUN_STATE_CHANGED = 2,
  CAI_AGENT_EVENT_TEXT_DELTA = 3,
  CAI_AGENT_EVENT_TOOL_CALL_STARTED = 4,
  CAI_AGENT_EVENT_TOOL_CALL_COMPLETED = 5,
  CAI_AGENT_EVENT_TOOL_CALL_FAILED = 6,
  CAI_AGENT_EVENT_STEERING_QUEUED = 7,
  CAI_AGENT_EVENT_STEERING_DELIVERED = 8,
  CAI_AGENT_EVENT_RUN_COMPLETED = 9,
  CAI_AGENT_EVENT_RUN_FAILED = 10,
  CAI_AGENT_EVENT_SESSION_CHECKPOINTED = 11,
  CAI_AGENT_EVENT_TERMINAL_COMMAND_STARTED = 12,
  CAI_AGENT_EVENT_TERMINAL_OUTPUT = 13,
  CAI_AGENT_EVENT_TERMINAL_WAITING = 14,
  CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED = 15,
  CAI_AGENT_EVENT_TERMINAL_COMMAND_CANCELLED = 16,
  /** A normal user turn was accepted to run after the active turn. */
  CAI_AGENT_EVENT_TURN_QUEUED = 17
} cai_agent_runtime_event_type;

/** A borrowed runtime event, valid only for the event callback duration. */
typedef struct cai_agent_runtime_event {
  /** CAI_AGENT_EVENT_* discriminator. */
  int type;
  /** State at the time the event was enqueued. */
  int state;
  /** Monotonic sequence within this runtime. */
  unsigned long long sequence;
  /** UTF-8 event data; not NUL-terminated. */
  const char *data;
  /** Number of bytes in data. */
  size_t data_length;
  /** Tool name for tool events, otherwise NULL. */
  const char *tool_name;
  /** CAI_AGENT_TOOL_ACTION_* derived from a known tool name. */
  int tool_action;
  /**
   * Best-effort primary target path supplied by a known local tool.
   * NULL when the invocation has no single target or CAI cannot derive one.
   */
  const char *tool_path;
  /** Number of file targets confirmed by an apply_patch completion event. */
  size_t tool_path_count;
  /**
   * Provider-issued stable invocation identifier for tool events and the
   * originating exec_command call for terminal events, otherwise NULL.
   */
  const char *tool_call_id;
  /** Stable terminal ID for terminal events, otherwise NULL. */
  const char *terminal_id;
  /** Monotonic command ID within terminal_id for terminal events. */
  unsigned long long terminal_command_id;
  /** Exit status supplied only by final terminal events. */
  int terminal_has_exit_code;
  long long terminal_exit_code;
  /** Signal status supplied only by final terminal events. */
  int terminal_has_signal;
  long long terminal_signal;
  /** Elapsed wall-clock time supplied by terminal lifecycle events. */
  unsigned long long terminal_duration_ms;
  /** Total observed bytes supplied by terminal lifecycle events. */
  unsigned long long terminal_total_output_bytes;
  /** Non-zero when CAI omitted bounded terminal output. */
  int terminal_output_truncated;
  /** Non-zero when detached descendants may remain after shell completion. */
  int terminal_detached_processes_possible;
} cai_agent_runtime_event;

/** Owner-thread callback that receives queued runtime events. */
typedef int (*cai_agent_runtime_event_fn)(void *context,
                                          const cai_agent_runtime_event *event,
                                          cai_error *error);

/** Configuration for a host-neutral CAI agent runtime. */
typedef struct cai_agent_runtime_config {
  /**
   * Preset name; NULL selects smith. Supported values are smith and
   * smith-review. The review preset is isolated and read-only.
   */
  const char *preset;
  /** Canonical workspace root. Required by smith file tools. */
  const char *workspace_directory;
  /** Optional identity overriding Cai Smith. */
  const char *agent_identity;
  /** Optional model override; Smith defaults to gpt-5.6-terra. */
  const char *model;
  /** Optional reasoning-effort override; Smith defaults to medium. */
  const char *reasoning_effort;
  /** Optional host developer-instruction extension. */
  const char *developer_instructions_extension;
  /** Host-owned local MCP clients whose discovered tools Smith registers. */
  cai_mcp_client *const *mcp_clients;
  /** Number of entries in mcp_clients. */
  size_t mcp_client_count;
  /** Optional common registration policy for the configured MCP clients. */
  const cai_mcp_tool_registration_config *mcp_tool_config;
  /** Enable OpenAI's hosted image_generation tool for this Smith runtime. */
  int enable_image_generation;
  /** Optional policy/limit override for Smith's one-slot terminal tools. */
  const cai_terminal_tool_config *terminal_tool_config;
  /** Non-zero disables Smith's one-slot terminal tools. */
  int disable_terminal;
  /**
   * Optional callback-backed session store. NULL selects CAI's local JSONL
   * store unless disable_default_session_store is non-zero.
   */
  const cai_agent_session_store *session_store;
  /** Optional storage scope; NULL uses the canonical workspace directory. */
  const char *session_scope;
  /** Optional new session identifier; NULL generates one. */
  const char *session_id;
  /** Resume the newest checkpoint for the configured session scope. */
  int resume_latest;
  /** Disable CAI's default local JSONL store when session_store is NULL. */
  int disable_default_session_store;
  /** Maximum queued runtime events; zero selects the bounded default. */
  size_t event_queue_limit;
  /** Maximum queued steering inputs; zero selects the bounded default. */
  size_t steering_queue_limit;
  /** Owner-thread event consumer; NULL permits polling state only. */
  cai_agent_runtime_event_fn event_callback;
  /** Context passed to event_callback. */
  void *event_context;
  /** Maximum queued normal turns; zero selects the bounded default. */
  size_t turn_queue_limit;
} cai_agent_runtime_config;

/** Initialize a zero-defaultable runtime configuration. */
void cai_agent_runtime_config_init(cai_agent_runtime_config *config);
/** Open an event runtime for the requested preset. */
int cai_agent_runtime_open(cai_client *client,
                           const cai_agent_runtime_config *config,
                           cai_agent_runtime **out, cai_error *error);
/** Submit a new user turn while the runtime is idle or completed. */
int cai_agent_runtime_submit(cai_agent_runtime *runtime, const char *text,
                             cai_error *error);
/** Queue steering for injection after the current model/tool cycle. */
int cai_agent_runtime_submit_steering(cai_agent_runtime *runtime,
                                      const char *text, cai_error *error);
/** Thread-safe variant of cai_agent_runtime_submit_steering. */
int cai_agent_runtime_submit_steering_threadsafe(cai_agent_runtime *runtime,
                                                 const char *text,
                                                 cai_error *error);
/**
 * Queue a normal user turn. It runs FIFO after the active turn reaches a
 * terminal state, or immediately when the runtime is idle.
 */
int cai_agent_runtime_submit_queued(cai_agent_runtime *runtime,
                                    const char *text, cai_error *error);
/** Thread-safe variant of cai_agent_runtime_submit_queued. */
int cai_agent_runtime_submit_queued_threadsafe(cai_agent_runtime *runtime,
                                               const char *text,
                                               cai_error *error);
/** Drain ready events on the owner thread; timeout is currently advisory. */
int cai_agent_runtime_pump(cai_agent_runtime *runtime, long timeout_ms,
                           cai_error *error);
/**
 * Return a borrowed pollable descriptor that becomes readable when CAI queues
 * host-visible runtime events. The descriptor remains owned by runtime and is
 * closed by cai_agent_runtime_close. Call pump after readiness to drain it.
 */
int cai_agent_runtime_wakeup_fd(const cai_agent_runtime *runtime, int *out_fd,
                                cai_error *error);
/** Read the currently observable runtime state. */
int cai_agent_runtime_state(cai_agent_runtime *runtime,
                            cai_agent_run_state *out, cai_error *error);
/** Return the runtime's stable session identifier, borrowed until close. */
const char *cai_agent_runtime_session_id(const cai_agent_runtime *runtime);
/**
 * Close the runtime after stopping and joining its worker. If invoked by its
 * owner-thread event callback, destruction is deferred until pump unwinds.
 * Calls from other threads wait until active event delivery has unwound before
 * returning, so callback-owned resources are never used after close returns.
 */
void cai_agent_runtime_close(cai_agent_runtime *runtime);

#ifdef __cplusplus
}
#endif

#endif
