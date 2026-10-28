# Interactive input and command audit

Historical audited baseline: `fce46cb`, including the corresponding static musl
`cai`. The original baseline excluded then-uncommitted `/yolo` changes.

## Current disposition

The fixes following `50de6d7` remove forced busy `!COMMAND` submission and account
for active shells in Softline's busy lifecycle. Typed commands entered while
busy, including `/shell`, use the editable native queue. Idle-only commands
retain that ownership through tool seams even after empty-editor Alt-Enter.
`/todo layout board|list` is now feed-local. Approval holds and queue edits are
preserved. Tests cover both Responses and Chat Completions with local fixtures.

Softline 0.14.0 completes uniform idle command admission. Its native classifier
queues every typed slash/shell command; CAI declares execution readiness by
identity and consumes only entries marked eligible. Independent feed-local
queries can pass blocked work. Whole-turn commands retain steering intent but
wait until idle. Enter/Alt-Enter/Tab are no longer intercepted by CAI.
Fresh managed commands can execute after Escape while the old backlog remains
stopped. Re-editing old work retains its stop until explicit promotion. There
is no global delivery-mode workaround, queue reinsert/rebinding, fake submission,
profile reset or second application queue. The completed upstream request is
`stash/softline-command-queue-policies-cr.md`.

Trusted host controls (`CAI_APP_STEER` and goal operations), startup instructions
and non-interactive invocations keep their documented API behavior. They are
separate from the human TUI command path. Slow synchronous work *after* command
delivery remains a separate concern; this change does not make it asynchronous.

The findings, matrix and probes below describe the historical baseline. In
particular, their forced pipe-submission, hidden pipe-backlog, busy-shell
rejection and popped-command findings have been addressed as described above.

The requested contract is that commands can enter Softline's visible, editable
queue on Enter, including while idle. The application consumes them when their
delivery boundary is eligible. Feed boundaries, model/tool boundaries and turn
boundaries are different. A command being queued does not establish that its
eventual execution is asynchronous. Synchronous execution after delivery is a
separate property, not evidence that admission bypassed Softline.

## Historical gaps in queue ownership

1. **Most slash commands still bypass queue admission while idle.**
   `cli_editor_key` explicitly enqueues only the feed-command allowlist. All
   other slash commands fall through to the queued-turns profile's normal idle
   Enter behavior: `sl_next_prompt` returns the submitted line directly, and
   `cli_dispatch_command` runs it. `/review` is in this group. This native
   behavior is legitimate Softline behavior; failing to route the requested
   application commands through its queue is CAI's omission.
2. **`!COMMAND` still has an explicit submission override.** Fresh Enter and
   Alt-Enter request `SL_KEY_ACTION_SUBMIT`, even while busy. Editing an existing
   queued entry is the exception. The child runs asynchronously, but that does
   not make its admission use the editable queue.
3. **Normal prompts can move into a hidden queue behind `!COMMAND`.**
   `cli_sync_status` derives busy from model/turn state, omitting the user shell
   process. While the model is idle and the pipe command is running, Enter can
   return a normal prompt directly. The owner loop then calls
   `cai_agent_runtime_submit_queued` because `state.shell.pid > 0`. Pending input
   is now in libcai rather than Softline and Alt-E cannot retrieve it.
4. **An ineligible command can be removed and rejected instead of retained.**
   The queue consumer takes an approved steering entry before dispatching its
   command. At a model/tool seam, operations requiring a fully idle runtime may
   then fail their own busy guards. The command is no longer editable. Affects
   `/review`, `/new`, `/resume NUMBER`, `/theme NAME`, `/queue on|off|NAME`,
   `/todo use|new NAME`, `/reasoning-tool` changes and `/trim COUNT`. During an
   active reviewer, `/review-and-fix` is another example. Existing regression
   tests explicitly expect some of these errors; that expectation does not
   prove correct retention until the appropriate boundary.
5. **Some local display changes wait for a turn unnecessarily.**
   `/todo layout` is feed-eligible, but `/todo layout board|list` is not in the
   feed allowlist. The setting change is therefore treated like an ordinary
   queued turn on busy Enter, or a steering command on Alt-Enter. The handler
   also fetches a board snapshot after changing the layout.
6. **Interactive host steering still bypasses Softline.**
   `CAI_APP_QUEUE_PROMPT` injects into Softline, but `CAI_APP_STEER` directly calls
   the runtime steering API. Host goal controls likewise call goal operations
   directly. These are documented trusted-host control paths, not a new
   regression; nevertheless, they are exceptions to a claim that every
   interactive injection remains editable in Softline. Core libcai without the
   application has no Softline dependency and is not expected to use its queue.

## Historical command admission matrix

Outside history search, with no queued draft currently being edited:

| Commands | Enter while idle | Enter while busy | Other delivery details |
| --- | --- | --- | --- |
| `/help`, `/status`, `/tools`, `/skills` | Explicit native enqueue | Explicit native enqueue | Eligible between feeds. |
| `/goal`, `/goal status`, `/goal help` | Explicit native enqueue | Explicit native enqueue | Queries/help, not model steering. |
| `/model`, `/reasoning` without arguments | Explicit native enqueue | Explicit native enqueue | Catalog operations can run synchronously after taking the entry. |
| `/tier list` | Explicit native enqueue | Explicit native enqueue | Other `/tier` forms are not in this class. |
| `/theme`, `/queue`, `/reasoning-tool` without arguments | Explicit native enqueue | Explicit native enqueue | Queries, not their setting-changing forms. |
| `/todo`, `/todo list`, `/todo help`, `/todo layout`, `/todo see ID` | Explicit native enqueue | Explicit native enqueue | Board reads can block during execution. |
| `/output [compact|full]`, `/autoqueue [on|off]`, `/shell-context [on|off]` | Explicit native enqueue | Explicit native enqueue | Includes invalid values, which are validated after delivery. |
| `/resume` without a number | Explicit native enqueue | Explicit native enqueue | Listing reads session storage synchronously. |
| `/review` with any scope | **Direct submission** | Native ordinary queue | Busy Alt-Enter makes a steering entry; no command-specific idle eligibility gate. |
| `/review-and-fix` with any scope | **Direct submission** | Native ordinary queue | Goal preparation may perform Git work. |
| `/model MODEL [EFFORT]`, `/model EFFORT`, `/reasoning EFFORT` | **Direct submission** | Native ordinary queue | Busy Alt-Enter uses the model/tool seam. |
| `/goal new TEXT`, `/goal pause`, `/goal resume`, `/goal clear` | **Direct submission** | Native ordinary queue | Busy Alt-Enter uses the model/tool seam. |
| `/tier`, `/tier NAME`, `/fast` | **Direct submission** | Native ordinary queue | Busy Alt-Enter uses the model/tool seam. |
| `/theme NAME`, `/queue on|off|NAME`, `/todo use|new NAME`, `/reasoning-tool` changes | **Direct submission** | Native ordinary queue | Busy Alt-Enter can consume the command before its idle guard succeeds. |
| `/todo layout board|list` | **Direct submission** | Native ordinary queue | Local preference is missing from the feed-command classification. |
| `/trim`, `/trim list [LIMIT]`, `/trim COUNT` | **Direct submission** | Native ordinary queue | History APIs require an idle runtime. |
| `/new`, `/resume NUMBER`, `/export [ID]` | **Direct submission** | Native ordinary queue | Storage/session work occurs in the command handler. |
| `/quit`, `/exit`, unknown slash commands | **Direct submission** | Native ordinary queue | Busy Alt-Enter uses the steering seam. |
| Fresh `!COMMAND` | **Forced direct submission** | **Forced direct submission** | Existing queue edits keep their native delivery intent. |
| `/shell`, `/shellshare`, `!sh`, `!sh+` | Guarded direct handoff | Rejected with draft retained | Explicitly implemented as non-queueable terminal handoffs. |

For the ordinary slash-command rows, busy Alt-Enter creates a native steering
entry. The consumer still needs a command-specific eligibility check; its Q/S
label alone cannot determine whether the operation can run.

History-search selection, approval and an active Alt-E edit retain Softline's
own handling. Alt-S intentionally suspends the editor for terminal handoff;
Alt-Q intentionally saves the draft and exits. Startup automation, batch mode
and `--review` are separate from the interactive `/review` command.

## Synchronous work after delivery

These are execution properties, separate from the admission defects above.
They explain why a consumed command can stop input processing. They have not
all been reproduced with slow backends.

| Commands or callbacks | Work on the UI owner thread |
| --- | --- |
| `/review` startup, all scopes | Child runtime creation, instruction/skill loading, request preparation, durable parent pause/checkpoint. Default/base scopes also run Git capture with blocking `read`/`waitpid`. The reviewer watch is added afterward. |
| Reviewer completion callback | Durable report handoff/checkpoint and child close before returning to Softline. |
| `/review-and-fix` without explicit scope | Git default-branch resolution before goal admission. |
| `/model`/`/reasoning` queries | Online model-catalog call where the endpoint policy supports live discovery. OpenRouter's suppressed listing and providers without a live catalog are exceptions. |
| `/model MODEL [EFFORT]`, `/reasoning EFFORT` | Metadata lookup/validation; idle setting application/checkpoint. A model switch may also compact history synchronously. |
| `/tier [list|NAME]`, `/fast` | Catalog lookup if uncached; idle setting application/checkpoint. |
| `/status` with ChatGPT quota enabled | `cli_refresh_quota(state, 1)` passes `wait=1` to the quota worker and waits on its condition variable. Having a worker does not make this caller nonblocking. |
| `/resume`, `/new`, `/resume NUMBER` | Stored-session listing, close/open, replay and associated initialization. |
| `/export [ID]` | Document lease acquisition, session reads and both exports. |
| `/queue on|off` | Close/reopen the runtime, stop/start message services and restore session state. |
| `/queue NAME` | Mailbox acquisition, old consumer stop/wait and replacement consumer start. |
| `/todo` board/list/detail/layout views | The producer uses a thread, but the UI thread parses its reader synchronously and waits for data/completion. Board selection also checkpoints. |
| `/trim` listing/removal | Stable history traversal or mutation/checkpoint. |
| Goal-changing commands | Durable journal append during runtime admission, followed by worker-side application. |
| Automatic status refresh | Git branch capture uses synchronous pipe read and `waitpid` at startup and busy-to-idle transitions. This can block without a slash command. |
| Incoming-message callbacks | Dirty-inbox queries/document loads and consumed/rejected tombstone writes occur on the owner thread. Background queue consumption does not eliminate these storage waits. |

`/tools` formats the captured inventory; it does not itself perform a new remote
MCP discovery. `/skills` visits the captured catalog. `/goal` queries and the
pure display settings have no identified long operation of their own, apart
from locks and rendering. Initial MCP discovery can still occur during runtime
construction, with its configured per-server timeout.

## Reproduction and verification limits

Two event-gated probes used the last committed static musl binary and the
existing terminal/HTTP fixtures, without live inference:

1. Hold the Git `symbolic-ref --quiet refs/remotes/origin/HEAD` operation on a
   FIFO while starting idle `/review`. CAI remains inside preparation, before
   any model POST and before the command feed echo. The native terminal remains
   owned, but subsequent input is not serviced. Releasing the gate allows the
   reviewer to start and finish. This proves a CAI owner-thread wait, not a
   Softline fault.
2. Hold an explicitly run pipe command on a FIFO, submit a normal prompt, then
   press Alt-E. The runtime logs `turn_queued`, no model request starts until
   the shell finishes, and Softline cannot restore the prompt. Releasing the
   shell gate allows that hidden input to run.

The disposable controller is `build/debug/softline-owner-audit.sh`. Model-catalog
HTTP blocking is identified by its synchronous call path; exploratory loopback
endpoint probes did not exercise live catalog policy and are not claimed as
latency evidence.

The earlier CLI tests covered queue editing and steering after streams or child
reviews had already started. They did not assert uniform admission while idle,
retention of ineligible command entries, responsiveness during preparation, or
Softline ownership of normal input behind a user-run pipe command. Passing those
tests did not establish a complete cutover.

The next implementation should make admission and boundary eligibility common
to command variants. It must preserve native queue identities, edits, approval
holds and cancellation, and distinguish slow execution after delivery from
whether the command was queued in the first place. `/yolo` should use that same
path rather than becoming another isolated exception.
