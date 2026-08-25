/** @file cai/agent_runtime.h
 *  Owner-thread event runtime for CAI agent presets.
 */
#ifndef CAI_AGENT_RUNTIME_H
#define CAI_AGENT_RUNTIME_H

#include <cai/cai.h>
#include <cai/session_store.h>
#include <cai/skills.h>
#include <cai/smith.h>
#include <cai/tools/terminal.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cai_agent_runtime cai_agent_runtime;

/** Observable execution state of an agent runtime. */
typedef enum cai_agent_run_state {
  /** Ready to accept an immediate turn. */
  CAI_AGENT_IDLE = 0,
  /** A provider response or terminal completion is in progress. */
  CAI_AGENT_SAMPLING = 1,
  /** CAI is dispatching one model-requested tool call. */
  CAI_AGENT_DISPATCHING_TOOL = 2,
  /** The current turn completed successfully. */
  CAI_AGENT_COMPLETED = 3,
  /** The current turn failed. Inspect the terminal event/error for detail. */
  CAI_AGENT_FAILED = 4,
  /** The current turn was cancelled during runtime shutdown. */
  CAI_AGENT_CANCELLED = 5
} cai_agent_run_state;

/** Stable semantic outcome classification for a runtime tool event. */
typedef enum cai_agent_tool_action {
  /** The event has no classified local-tool action. */
  CAI_AGENT_TOOL_ACTION_NONE = 0,
  /** A local file was read. */
  CAI_AGENT_TOOL_ACTION_READ = 1,
  /** Local filesystem entries were listed. */
  CAI_AGENT_TOOL_ACTION_LIST = 2,
  /** A local image was prepared as model input. */
  CAI_AGENT_TOOL_ACTION_VIEW = 3,
  /** A native apply_patch operation changed workspace files. */
  CAI_AGENT_TOOL_ACTION_PATCH = 4,
  /** A terminal command was started or observed. */
  CAI_AGENT_TOOL_ACTION_EXECUTE = 5,
  /** Bytes were written to, polled from, or used to terminate the terminal. */
  CAI_AGENT_TOOL_ACTION_WRITE_STDIN = 6,
  /** The current goal was read. */
  CAI_AGENT_TOOL_ACTION_GET_GOAL = 7,
  /** A new goal was created. */
  CAI_AGENT_TOOL_ACTION_CREATE_GOAL = 8,
  /** An existing goal was updated. */
  CAI_AGENT_TOOL_ACTION_UPDATE_GOAL = 9,
  /** The current goal was cleared. */
  CAI_AGENT_TOOL_ACTION_CLEAR_GOAL = 10,
  /** A hosted or configured image-generation operation ran. */
  CAI_AGENT_TOOL_ACTION_IMAGE_GENERATION = 11,
  /** A configured MCP or other non-local tool ran. */
  CAI_AGENT_TOOL_ACTION_EXTERNAL = 12,
  /** A synchronous child agent was launched. */
  CAI_AGENT_TOOL_ACTION_SUBAGENT = 13
} cai_agent_tool_action;

/** Event emitted by an agent runtime. */
typedef enum cai_agent_runtime_event_type {
  /** A submitted turn started. */
  CAI_AGENT_EVENT_RUN_STARTED = 1,
  /** The observable run state changed. */
  CAI_AGENT_EVENT_RUN_STATE_CHANGED = 2,
  /** A normal assistant-text UTF-8 delta arrived. */
  CAI_AGENT_EVENT_TEXT_DELTA = 3,
  /** The model requested a tool call. */
  CAI_AGENT_EVENT_TOOL_CALL_STARTED = 4,
  /** A tool call completed successfully. */
  CAI_AGENT_EVENT_TOOL_CALL_COMPLETED = 5,
  /** A tool call failed. */
  CAI_AGENT_EVENT_TOOL_CALL_FAILED = 6,
  /** Steering was durably accepted for the next safe boundary. */
  CAI_AGENT_EVENT_STEERING_QUEUED = 7,
  /** Accepted steering was delivered to the model. */
  CAI_AGENT_EVENT_STEERING_DELIVERED = 8,
  /** The active turn completed successfully. */
  CAI_AGENT_EVENT_RUN_COMPLETED = 9,
  /** The active turn failed. */
  CAI_AGENT_EVENT_RUN_FAILED = 10,
  /** A durable session checkpoint completed. */
  CAI_AGENT_EVENT_SESSION_CHECKPOINTED = 11,
  /** The managed terminal began a command. */
  CAI_AGENT_EVENT_TERMINAL_COMMAND_STARTED = 12,
  /** The managed terminal emitted output. */
  CAI_AGENT_EVENT_TERMINAL_OUTPUT = 13,
  /** A terminal wait/poll remains active. */
  CAI_AGENT_EVENT_TERMINAL_WAITING = 14,
  /** The managed terminal command exited and PTY output was drained. */
  CAI_AGENT_EVENT_TERMINAL_COMMAND_COMPLETED = 15,
  /** The managed terminal command was cancelled. */
  CAI_AGENT_EVENT_TERMINAL_COMMAND_CANCELLED = 16,
  /** A normal user turn was accepted to run after the active turn. */
  CAI_AGENT_EVENT_TURN_QUEUED = 17,
  /** Final JSON report emitted by a completed smith-review run. */
  CAI_AGENT_EVENT_REVIEW_REPORT = 18,
  /** A parent Smith runtime has launched an isolated review child. */
  CAI_AGENT_EVENT_REVIEW_STARTED = 19,
  /** A completed or failed review was durably handed back to its parent. */
  CAI_AGENT_EVENT_REVIEW_HANDED_OFF = 20,
  /**
   * Provider-supplied reasoning-summary delta. This is not raw chain of
   * thought; it is the summary explicitly returned by the provider.
   */
  CAI_AGENT_EVENT_REASONING_SUMMARY = 21,
  /**
   * One model response finished. A single agent run can contain multiple
   * responses when steering is delivered after a response boundary.
   */
  CAI_AGENT_EVENT_RESPONSE_COMPLETED = 22,
  /** Durable goal state changed at a model/tool safe boundary. */
  CAI_AGENT_EVENT_GOAL_CHANGED = 23,
  /** A synchronous child agent started from a parent tool call. */
  CAI_AGENT_EVENT_SUBAGENT_STARTED = 24,
  /** A synchronous child agent handed durable context back to its parent. */
  CAI_AGENT_EVENT_SUBAGENT_HANDED_OFF = 25
} cai_agent_runtime_event_type;

/**
 * A borrowed runtime event, valid only for the event callback duration.
 * Every pointer, including data and the tool/terminal/session identifiers, is
 * owned by CAI and becomes invalid when the callback returns.
 */
typedef struct cai_agent_runtime_event {
  /** CAI_AGENT_EVENT_* discriminator. */
  int type;
  /** State at the time the event was enqueued. */
  int state;
  /** Monotonic sequence within this runtime. */
  unsigned long long sequence;
  /**
   * UTF-8 event payload; not NUL-terminated. Its exact meaning depends on
   * type: text/reasoning bytes, a tool receipt/error, terminal output, or a
   * final review report. For CAI_AGENT_EVENT_SUBAGENT_STARTED, this is the
   * bounded delegated instruction text selected by the parent agent. Hosts
   * should treat it as untrusted model output when rendering it.
   */
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
  /** Stable source runtime session ID, borrowed until the callback returns. */
  const char *runtime_session_id;
  /** Enabled subagent profile for subagent lifecycle events, otherwise NULL. */
  const char *subagent_name;
  /**
   * Parent subagent invocation ID for forwarded child events, otherwise
   * NULL. tool_call_id remains the child tool's own provider invocation ID.
   */
  const char *parent_tool_call_id;
} cai_agent_runtime_event;

/**
 * Owner-thread callback that receives queued runtime events from
 * cai_agent_runtime_pump. Return CAI_OK after consuming/copying the borrowed
 * event. A non-OK return stops that pump call and leaves later events queued.
 * The callback may close its runtime, but must not re-enter it otherwise.
 */
typedef int (*cai_agent_runtime_event_fn)(void *context,
                                          const cai_agent_runtime_event *event,
                                          cai_error *error);

/** Explicit target for an isolated preset review run. */
typedef enum cai_agent_review_target {
  /** Review staged, unstaged, and untracked workspace changes. */
  CAI_AGENT_REVIEW_UNCOMMITTED = 1,
  /** Review the merge diff against base_branch. */
  CAI_AGENT_REVIEW_BASE_BRANCH = 2,
  /** Review the changes introduced by commit. */
  CAI_AGENT_REVIEW_COMMIT = 3,
  /** Run a review using host-provided review instructions. */
  CAI_AGENT_REVIEW_CUSTOM = 4
} cai_agent_review_target;

/**
 * Request for cai_agent_runtime_submit_review. Strings are borrowed for the
 * call only. base_branch is required for CAI_AGENT_REVIEW_BASE_BRANCH;
 * commit is required for CAI_AGENT_REVIEW_COMMIT; instructions is required
 * for CAI_AGENT_REVIEW_CUSTOM. commit_title is optional metadata.
 */
typedef struct cai_agent_review_request {
  int target;
  const char *base_branch;
  const char *commit;
  const char *commit_title;
  const char *instructions;
} cai_agent_review_request;

/** Initialize a zero-defaultable review request. */
void cai_agent_review_request_init(cai_agent_review_request *request);

/** Supported model-facing parameter kinds for a subagent profile tool. */
typedef enum cai_agent_subagent_parameter_type {
  CAI_AGENT_SUBAGENT_PARAMETER_STRING = 0,
  CAI_AGENT_SUBAGENT_PARAMETER_INTEGER = 1,
  CAI_AGENT_SUBAGENT_PARAMETER_ENUM = 2
} cai_agent_subagent_parameter_type;

/**
 * One declared field for a generated run_<profile> tool. enum_values is
 * required and non-empty for CAI_AGENT_SUBAGENT_PARAMETER_ENUM, and ignored
 * otherwise. Pointers are borrowed only for cai_agent_runtime_open.
 */
typedef struct cai_agent_subagent_parameter {
  /** Lowercase ASCII field name, excluding CAI-reserved names. */
  const char *name;
  /** Short model-facing field description. */
  const char *description;
  /** CAI_AGENT_SUBAGENT_PARAMETER_* discriminator. */
  int type;
  /** Non-zero requires this field in the generated tool invocation. */
  int required;
  /** Exact string values accepted for an enum field. */
  const char *const *enum_values;
  size_t enum_value_count;
} cai_agent_subagent_parameter;

/**
 * Host-defined profile available to CAI's synchronous subagent lifecycle.
 * Every pointer is borrowed only for cai_agent_runtime_open; CAI snapshots
 * the descriptor and its allowlists. name is a lowercase ASCII identifier and
 * may not be "review", which is reserved for Smith's built-in reviewer.
 * preset selects the child's developer instructions and tool policy. CAI
 * structurally removes CAI_AGENT_PRESET_TOOL_SUBAGENTS from every child.
 *
 * Model and reasoning overrides requested by the parent model are accepted
 * only when the corresponding allowlist is non-empty and contains the exact
 * requested value. With no request, a child inherits the parent's active
 * model, reasoning effort, and reasoning-summary mode.
 */
typedef struct cai_agent_subagent_profile {
  /** Stable lowercase profile identifier, excluding the reserved review. */
  const char *name;
  /** Short operator/model-facing description of the delegated role. */
  const char *description;
  /** Required host-defined child preset. */
  const cai_agent_preset *preset;
  /** Exact allowed model overrides; NULL/zero rejects model overrides. */
  const char *const *allowed_models;
  size_t allowed_model_count;
  /** Exact allowed reasoning-effort overrides; NULL/zero rejects overrides. */
  const char *const *allowed_reasoning_efforts;
  size_t allowed_reasoning_effort_count;
  /** Exact allowed reasoning-summary overrides; NULL/zero rejects overrides. */
  const char *const *allowed_reasoning_summaries;
  size_t allowed_reasoning_summary_count;
  /**
   * Optional declared parameters for the generated run_<name> tool. CAI
   * validates their types and enum values before rendering the child input.
   */
  const cai_agent_subagent_parameter *parameters;
  size_t parameter_count;
  /**
   * Optional child-input template. {{field_name}} expands a declared value and
   * {{instructions}} expands the optional free-form field. NULL selects a
   * bounded structured argument rendering.
   */
  const char *instruction_template;
  /**
   * Non-zero exposes an optional instructions string on run_<name>. When it is
   * omitted, CAI forwards the current user turn into {{instructions}}.
   */
  int expose_instructions;
} cai_agent_subagent_profile;

/** Borrowed immutable projection of a runtime goal. */
typedef struct cai_agent_goal_snapshot {
  /** Non-zero when a goal exists. */
  int has_goal;
  /** Objective and status, borrowed until the next goal snapshot or close. */
  const char *objective;
  const char *status;
  /** Non-zero when token_budget and remaining_tokens are meaningful. */
  int has_token_budget;
  long long token_budget;
  long long tokens_used;
  /** Never negative; zero means the budget is exhausted. */
  long long remaining_tokens;
  /** Active wall time, excluding paused and terminal intervals. */
  long long elapsed_seconds;
  long long created_at;
  long long updated_at;
} cai_agent_goal_snapshot;

/** Request for a host-created goal. objective is required and borrowed. */
typedef struct cai_agent_goal_request {
  const char *objective;
  int has_token_budget;
  long long token_budget;
} cai_agent_goal_request;

void cai_agent_goal_request_init(cai_agent_goal_request *request);

/** Configuration for a host-neutral CAI agent runtime. */
typedef struct cai_agent_runtime_config {
  /**
   * Built-in preset name; NULL selects smith. Custom profiles use
   * preset_descriptor instead. Supported built-in values are smith and
   * smith-review.
   */
  const char *preset;
  /**
   * Optional host-defined descriptor. CAI snapshots it during open, so the
   * caller may use stack or Lua-table-backed input. It takes precedence over
   * the built-in policy. Combine it with preset = "smith-review" only to open
   * that descriptor as an isolated review runtime; otherwise leave preset
   * NULL. Combining it with preset = "smith" is invalid.
   */
  const cai_agent_preset *preset_descriptor;
  /** Canonical workspace root. Required by smith file tools. */
  const char *workspace_directory;
  /** Optional CAI agent configuration directory for the global AGENTS.md. */
  const char *agent_config_directory;
  /** Optional exact global AGENTS.md path overriding the config directory. */
  const char *global_agents_md_path;
  /** Optional callback-backed global AGENTS.md source. Borrowed until close. */
  const cai_blob_store *global_instruction_store;
  /** Optional global skill configuration. NULL selects CAI's default root. */
  const cai_skill_config *skills;
  /** Enable explicit Codex-compatible ancestor AGENTS.md discovery. */
  int codex_compat_agents_md;
  /** Optional identity overriding Cai Smith. */
  const char *agent_identity;
  /** Optional model override; Smith defaults to gpt-5.6-terra. */
  const char *model;
  /** Optional reasoning-effort override; Smith defaults to medium. */
  const char *reasoning_effort;
  /**
   * Optional provider reasoning-summary mode for this runtime. Accepted values
   * are CAI_REASONING_SUMMARY_NONE, _AUTO, _CONCISE, and _DETAILED. NULL
   * selects the provider-directed CAI_REASONING_SUMMARY_AUTO default.
   */
  const char *reasoning_summary;
  /**
   * Optional model for review children launched from this Smith runtime. NULL
   * inherits model. Ignored by a directly opened smith-review runtime.
   */
  const char *review_model;
  /**
   * Optional reasoning effort for review children. NULL inherits
   * reasoning_effort. Ignored by a directly opened smith-review runtime.
   */
  const char *review_reasoning_effort;
  /**
   * Optional reasoning-summary mode for review children. NULL inherits
   * reasoning_summary. Ignored by a directly opened smith-review runtime.
   */
  const char *review_reasoning_summary;
  /**
   * Exact model overrides the built-in review subagent may accept. NULL/zero
   * rejects model overrides; no request inherits this runtime's active model.
   */
  const char *const *review_allowed_models;
  size_t review_allowed_model_count;
  /** Exact reasoning-effort overrides the built-in reviewer may accept. */
  const char *const *review_allowed_reasoning_efforts;
  size_t review_allowed_reasoning_effort_count;
  /** Exact reasoning-summary overrides the built-in reviewer may accept. */
  const char *const *review_allowed_reasoning_summaries;
  size_t review_allowed_reasoning_summary_count;
  /**
   * Optional host-defined profiles for generated run_<name> tools. CAI
   * snapshots these descriptors at open; children cannot recursively launch
   * subagents.
   */
  const cai_agent_subagent_profile *subagents;
  size_t subagent_count;
  /** Disable Smith's otherwise enabled built-in review subagent. */
  int disable_review_subagent;
  /** Optional host developer-instruction extension. */
  const char *developer_instructions_extension;
  /**
   * Host-owned local MCP clients whose discovered tools CAI registers. Every
   * client must remain open and valid until the runtime closes.
   */
  cai_mcp_client *const *mcp_clients;
  /** Number of entries in mcp_clients. */
  size_t mcp_client_count;
  /**
   * Optional common registration policy for the configured MCP clients. Its
   * callback/context lifetime must cover the runtime.
   */
  const cai_mcp_tool_registration_config *mcp_tool_config;
  /** Enable OpenAI's hosted image_generation tool for this Smith runtime. */
  int enable_image_generation;
  /**
   * Optional policy/limit override for Smith's one-slot terminal tools. CAI
   * copies scalar/path settings at open; policy and event callback contexts
   * remain borrowed for the runtime lifetime.
   */
  const cai_terminal_tool_config *terminal_tool_config;
  /** Non-zero disables Smith's one-slot terminal tools. */
  int disable_terminal;
  /**
   * Optional callback-backed session store. NULL selects CAI's local JSONL
   * store unless disable_default_session_store is non-zero.
   */
  const cai_agent_session_store *session_store;
  /**
   * Optional opaque storage namespace; NULL uses the canonical workspace
   * directory. The local store hashes this value and does not interpret it as
   * a filesystem path. smith-review derives a reserved private namespace from
   * this value so its checkpoints are not eligible for normal Smith resume;
   * normal Smith rejects the reserved smith-review: prefix.
   */
  const char *session_scope;
  /**
   * Optional new session identifier; NULL generates one. smith-review rejects
   * this so every review run starts in a fresh, unaddressable session.
   */
  const char *session_id;
  /**
   * Resume the newest checkpoint for the configured session scope. smith-review
   * rejects this to preserve review-session isolation.
   */
  int resume_latest;
  /** Disable CAI's default local JSONL store when session_store is NULL. */
  int disable_default_session_store;
  /** Maximum queued runtime events; zero selects the bounded default. */
  size_t event_queue_limit;
  /** Maximum queued steering inputs; zero selects the bounded default. */
  size_t steering_queue_limit;
  /**
   * Owner-thread event consumer. NULL selects poll-only operation: CAI does
   * not queue runtime events, while cai_agent_runtime_state remains usable.
   */
  cai_agent_runtime_event_fn event_callback;
  /** Context passed to event_callback. */
  void *event_context;
  /** Maximum queued normal turns; zero selects the bounded default. */
  size_t turn_queue_limit;
  /**
   * Optional observer for review children launched by this runtime. NULL uses
   * event_callback. The child is still pumped independently by the host.
   */
  cai_agent_runtime_event_fn review_event_callback;
  /** Context passed to review_event_callback; NULL uses event_context. */
  void *review_event_context;
} cai_agent_runtime_config;

/** Initialize a zero-defaultable runtime configuration. */
void cai_agent_runtime_config_init(cai_agent_runtime_config *config);
/**
 * Open an owner-thread event runtime for the requested preset. The calling
 * thread becomes its owner and must use the non-threadsafe control, state,
 * pump, and export functions. CAI snapshots string/profile inputs at open;
 * it borrows the client, MCP clients, session store, and callback contexts
 * until close. On success *out is owned by the caller and must be closed.
 */
int cai_agent_runtime_open(cai_client *client,
                           const cai_agent_runtime_config *config,
                           cai_agent_runtime **out, cai_error *error);
/**
 * Submit an immediate user turn while an ordinary runtime is idle or
 * completed. With durable storage the accepted input is journaled before this
 * call succeeds. Owner-thread-only; use submit_queued_threadsafe for input
 * arriving on another thread.
 */
int cai_agent_runtime_submit(cai_agent_runtime *runtime, const char *text,
                             cai_error *error);
/**
 * Submit an explicit Codex-style review target to an idle isolated review
 * runtime. The request is rendered as the review turn's user instruction.
 */
int cai_agent_runtime_submit_review(cai_agent_runtime *runtime,
                                    const cai_agent_review_request *request,
                                    cai_error *error);
/**
 * Launch a fresh, isolated reviewer derived from a quiescent ordinary preset
 * runtime and submit its one review request. A recovered durable review
 * pause is the sole exception: it may launch a replacement review while its
 * held queued turns remain paused. The returned child has its own session and
 * wakeup descriptor; the host pumps and renders it just like any other
 * runtime. While a review child is active, immediate and steering parent input
 * is rejected, while normal queued turns remain durable and wait for
 * cai_agent_runtime_finish_review. CAI checkpoints that pause before this
 * function returns, so recovery keeps queued turns held until a replacement
 * review is started and handed off. The caller owns the child and must finish
 * its review handoff, then close the child before closing its parent.
 * If the pause journal record was accepted but its checkpoint fails, this
 * function returns that error with out_review populated and keeps the parent
 * paused; the caller must retain and finish that child, or close both child
 * and parent before reopening the durable parent for a replacement review.
 */
int cai_agent_runtime_start_review(cai_agent_runtime *parent,
                                   const cai_agent_review_request *request,
                                   cai_agent_runtime **out_review,
                                   cai_error *error);
/**
 * Persist a completed review report, or a reviewer failure marker, as trusted
 * handoff context in parent before releasing its queued normal turns. review
 * must be the active child returned by cai_agent_runtime_start_review and be
 * in a terminal state. A failed durable checkpoint leaves the parent paused
 * so the caller may retry this function without duplicating the handoff. The
 * resolution marker and developer-role handoff are checkpointed together.
 */
int cai_agent_runtime_finish_review(cai_agent_runtime *parent,
                                    cai_agent_runtime *review,
                                    cai_error *error);
/**
 * Queue steering for injection at the next safe model/tool boundary. This is
 * the default interactive-input path while a turn is active; it is rejected
 * while idle, stopping, or paused for review. Owner-thread-only.
 */
int cai_agent_runtime_submit_steering(cai_agent_runtime *runtime,
                                      const char *text, cai_error *error);
/** Thread-safe variant of cai_agent_runtime_submit_steering. */
int cai_agent_runtime_submit_steering_threadsafe(cai_agent_runtime *runtime,
                                                 const char *text,
                                                 cai_error *error);
/**
 * Queue a normal user turn. It runs FIFO after the active turn reaches a
 * terminal state, or immediately when the runtime is idle. Owner-thread-only.
 */
int cai_agent_runtime_submit_queued(cai_agent_runtime *runtime,
                                    const char *text, cai_error *error);
/** Thread-safe variant of cai_agent_runtime_submit_queued. */
int cai_agent_runtime_submit_queued_threadsafe(cai_agent_runtime *runtime,
                                               const char *text,
                                               cai_error *error);
/** Return the current durable goal projection on the owner thread. */
int cai_agent_runtime_get_goal(cai_agent_runtime *runtime,
                               cai_agent_goal_snapshot *out, cai_error *error);
/**
 * Queue a host goal control. Controls are journaled before acceptance and
 * apply at the next model/tool safe boundary; when idle they apply promptly.
 * These functions are owner-thread-only. They may be called while sampling.
 */
int cai_agent_runtime_create_goal(cai_agent_runtime *runtime,
                                  const cai_agent_goal_request *request,
                                  cai_error *error);
int cai_agent_runtime_pause_goal(cai_agent_runtime *runtime, cai_error *error);
int cai_agent_runtime_resume_goal(cai_agent_runtime *runtime, cai_error *error);
int cai_agent_runtime_set_goal_objective(cai_agent_runtime *runtime,
                                         const char *objective,
                                         cai_error *error);
int cai_agent_runtime_set_goal_token_budget(cai_agent_runtime *runtime,
                                            long long token_budget,
                                            cai_error *error);
int cai_agent_runtime_clear_goal_token_budget(cai_agent_runtime *runtime,
                                              cai_error *error);
int cai_agent_runtime_clear_goal(cai_agent_runtime *runtime, cai_error *error);
/**
 * Drain queued events on the owner thread. When no event is ready, wait up to
 * timeout_ms (zero is non-blocking); the timeout is advisory and this function
 * may return earlier after a wakeup. Poll-only runtimes have no callback and
 * therefore no events to drain, but may still use this as a synchronization
 * point before reading state.
 */
int cai_agent_runtime_pump(cai_agent_runtime *runtime, long timeout_ms,
                           cai_error *error);
/**
 * Return a borrowed pollable descriptor that becomes readable when CAI queues
 * host-visible runtime events. It is useful only when event_callback is
 * non-NULL; poll-only runtimes intentionally do not queue observations. The
 * descriptor remains owned by runtime and is closed by cai_agent_runtime_close.
 * Call pump after readiness to drain it.
 */
int cai_agent_runtime_wakeup_fd(const cai_agent_runtime *runtime, int *out_fd,
                                cai_error *error);
/** Read the currently observable runtime state on the owner thread. */
int cai_agent_runtime_state(cai_agent_runtime *runtime,
                            cai_agent_run_state *out, cai_error *error);
/** Return the runtime's stable session identifier, borrowed until close. */
const char *cai_agent_runtime_session_id(const cai_agent_runtime *runtime);
/**
 * Stream the current agent handover projection as Markdown to sink.
 *
 * The versioned, non-resumable document includes durable conversation context,
 * active developer instructions, and current session/goal/runtime metadata.
 * The caller retains sink ownership. Export is owner-thread-only and is
 * available only at a stable runtime boundary; it returns CAI_ERR_INVALID
 * while a model/tool turn or queued host input is active. The sink must not
 * re-enter this runtime while export is in progress.
 */
int cai_agent_runtime_export_markdown(cai_agent_runtime *runtime,
                                      cai_sink *sink, cai_error *error);
/**
 * Stream the current agent handover projection to a newly created Markdown
 * file. The path is created without replacing an existing file.
 *
 * When path is NULL or empty, CAI creates
 * <workspace>/<app_name>-session-<xid>.md. Otherwise path selects the output
 * file. app_name is required only for the default path and may contain ASCII
 * letters, digits, '-' and '_'. If out_path is supplied, it receives the
 * chosen path including its terminating NUL on success.
 */
int cai_agent_runtime_export_markdown_file(cai_agent_runtime *runtime,
                                           const char *app_name,
                                           const char *path, char *out_path,
                                           size_t out_path_capacity,
                                           cai_error *error);
/**
 * Close the runtime after stopping and joining its worker. If invoked by its
 * owner-thread event callback, destruction is deferred until pump unwinds. If
 * invoked by a worker-thread terminal lifecycle callback, it likewise requests
 * shutdown and defers destruction until the owner next pumps or closes after
 * the callback returns. Calls from other threads wait until active event
 * delivery has unwound before returning, so callback-owned resources are never
 * used after close returns.
 */
void cai_agent_runtime_close(cai_agent_runtime *runtime);

#ifdef __cplusplus
}
#endif

#endif
