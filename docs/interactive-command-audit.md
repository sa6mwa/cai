# Interactive command policies

CAI uses Softline 0.14.0's native queue policies. The complete inventory is
`cli_command_definitions` in `cli/app_run.c`; argument-specific query forms are
resolved by `cli_command_delivery`. Admission, readiness, mode normalization and
owner-thread consumption use that same classification. This replaces the earlier
audit's historical classifications.

## Boundaries

| Boundary | Behavior with Enter or Alt-Enter |
| --- | --- |
| Feed | Execute between owner-thread feed writes. No provider input is submitted. |
| Request | Execute at the next runtime `INPUT_BOUNDARY`, after the response/tool round and before the next request, or while idle. The in-flight request keeps its original settings. |
| Turn | Wait until the turn, shell and foreground review finish. Normalize steering intent to visible `Q`; the original identity remains editable. |
| Conversation prompt | Enter queues a new turn; Alt-Enter steers at a runtime input boundary. Native ordering and approval rules remain authoritative. |

Feed and request commands are independent native managed entries: unrelated
queued conversation input cannot delay them, and an unready command does not
block a later conversation steer. They remain approval-controlled, editable,
cancellable and stopped when appropriate. Request readiness is false during
streaming; submission never inserts command output into that response.

Turn-only commands submitted while busy use Softline's ordinary admission lane,
with readiness held until idle. They become `Q`, so native steering can pass
them. Idle commands use managed admission, retaining same-burst Enter/Alt-E
editing before dispatch. There is no second UI queue, synthetic key submission,
global delivery-mode workaround or queue removal/reinsertion.

## Complete command inventory

| Command | Boundary | Reason |
| --- | --- | --- |
| `/help` | Feed | Local help. |
| `/status` | Feed | Runtime/status snapshots and tool inventory. |
| `/tools` | Feed | Tool/MCP inventory. |
| `/skills` | Feed | Skill catalog. |
| `/model` | Feed | Model and reasoning choices. |
| `/model MODEL [EFFORT]`, `/model EFFORT` | Request | Settings for subsequent inference. |
| `/reasoning` | Feed | Current model's advertised efforts. |
| `/reasoning EFFORT` | Request | Effort for subsequent inference. |
| `/tier list` | Feed | Current/pending routing and choices. |
| `/tier`, `/tier NAME`, `/fast` | Request | Cycle, select or toggle routing within the current turn. |
| `/goal`, `/goal status`, `/goal help` | Feed | Inspect the goal or its controls. |
| `/goal new TEXT`, `/goal pause`, `/goal resume`, `/goal clear` | Request | Explicit parent goal controls, never model steering text. |
| `/theme` | Feed | Inspect themes. |
| `/theme NAME[:NAME]` | Request | Replace the renderer after its current response, retaining tool display state. |
| `/output [compact\|full]` | Feed | Set the display preference for future tool calls. |
| `/history [COUNT]` | Feed | Inspect/configure prompt recall. |
| `/shell-context [on\|off]` | Feed | Set sharing for future shell calls. |
| `/autoqueue [on\|off]` | Feed | Change native incoming-message approval policy; bare form toggles it. |
| `/yolo` | Feed | Inspect effective/pending confinement. |
| `/yolo on`, `/yolo off` | Turn | Reconfigure built-in tool contexts only when runtime/tools are stable. |
| `/reasoning-tool` | Feed | Inspect optional tool configuration. |
| `/reasoning-tool on\|off\|normal LEVEL\|high LEVEL` | Turn | Reconfigure tool registration/mappings. |
| `/queue` | Feed | Inspect the mailbox or show how to enable messaging. |
| `/queue on\|off\|NAME` | Turn | Reopen integration or change consumer ownership. |
| `/todo`, `/todo list`, `/todo help`, `/todo see ID` | Feed | Read boards or a card. |
| `/todo layout [board\|list]` | Feed | Inspect/change local layout and display the board. |
| `/todo use NAME`, `/todo new NAME` | Turn | Commit the runtime's selected board. |
| `/retention` | Feed | Inspect conversation retention. |
| `/retention DAYS\|off` | Turn | Update collection/session leases without racing runtime commits. |
| `/trim`, `/trim list [LIMIT]`, `/trim COUNT` | Turn | History APIs require a stable runtime. |
| `/export [BASENAME]` | Turn | Export consistent retained history. |
| `/resume` | Feed | List conversations. |
| `/resume NUMBER` | Turn | Switch the active runtime. |
| `/new` | Turn | Create and switch conversations. |
| `/review [SCOPE]` | Turn | Start an isolated foreground review. |
| `/review-and-fix [SCOPE]` | Turn | Start goal-driven review/fix work. |
| `/compact` | Turn | Start only when `can_compact` permits it. |
| `/shell`, `/shellshare`, `!sh`, `!sh+` | Turn | Transfer terminal ownership; only shared forms record output. |
| `!COMMAND` | Turn | Execute a user shell command. |
| `/quit`, `/exit` | Feed | Shut down without waiting for an active turn to complete. |
| Unknown slash commands | Feed | Reject locally; never forward as model steering. |

Horizontal whitespace separating a slash command and its arguments, and trailing
horizontal whitespace, mean the same during classification and dispatch. The
original input is echoed/recorded; argument interiors and shell text are preserved.

## Foreground reviews and subagents

Conversation steers reach the active child when subagent steering is enabled;
otherwise they become `Q`. Parent settings and goal controls wait for a parent
input boundary; they do not retarget a review's configuration or goal. Themes
are UI-local and may change at a child's completed response/tool seam. Queries
remain available. Turn-only operations wait for the child to finish.

## Cancellation, editing and delivery

Readiness follows runtime, shell and owner-loop events through
`sl_prompt_queue_set_executable`. Consumers take only natively eligible entries.
Execution readiness never overrides approval or stopped state. Escape stops
retained work. Fresh managed actions can execute without resuming that backlog;
passive inspection and edits do not resume it. Alt-E edits the admitted identity.

Conversation steering that misses the final active seam can start at idle only
when native delivery, approval, draft/search and stopped state permit it.
Alt-S's draft-retaining shell handoff and Alt-Q's draft-saving exit are explicit
editor actions. Trusted host controls, startup automation, batch input and the
standalone `--review` option retain their separate documented APIs.

## Synchronous work and verification

Admission and execution are distinct. Safely admitted `/model`, `/tools`,
`/status` and `/resume` can still synchronously load catalogs, discover MCP tools
or read storage. This audit does not make those operations asynchronous.

Focused Debug checks cover every command class and query/mutation split with
both modes, same-burst editing across the command inventory, Enter-based model
and effort changes, tier cycling/toggling, theme/steer ordering, turn-only
reclassification, stopped work, approval and idle delivery. Local fixtures
capture actual Responses and Chat Completions requests; no live inference or
cross-target testing is required for these regressions.
