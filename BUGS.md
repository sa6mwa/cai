# Bug tracker

This file tracks reported behavior, investigation evidence, and the verification
needed to close each issue. An upstream cause must be reproduced independently
before it is attributed to a dependency. Keep resolved entries with their fix
commit and verification evidence.

## BUG-057: Patch matching rejects Codex-compatible repeated context

- **Status:** fixed.
- **Evidence:** the three reported calls in session `dbgtfervs68i52ohr8gg`
  fail CAI's global-uniqueness check on repeated profile rows. Codex's actual
  executable accepts each call. The `Slh36j` patch includes context-only chunks
  advancing between two profiles, so the repeated row is intentionally ordered.
- **Resolution:** match logical lines against original source, using Codex's
  first ordered candidate and exact/whitespace/Unicode fallback precedence.
  Preserve context bytes and mixed endings; handle insertion-only chunks,
  unterminated final lines and trailing empty context. Build the result once,
  checking its byte bound before each append. Path confinement, identity checks
  and all-file validation/publication remain authoritative.
- **Verification:** fast native cases exercise both direct and spooled tool
  invocation; exact reported patches and compatibility cases are also compared
  against Codex's executable. Interactive tool rendering and Lua facade checks
  cover the shared implementation without live inference.

## BUG-056: Command boundary policies differ across the interactive surface

- **Status:** fixed.
- **Evidence:** several settings followed ordinary Q/S prompt intent rather
  than their runtime contract. Whole-turn-only operations had inconsistent
  mode labels; the audit and some tests still described obsolete behavior.
- **Resolution:** one complete command inventory classifies feed-local,
  response/tool-boundary and idle-only operations. Model, effort, tier,
  goal and theme changes use the next safe seam with either submission key.
  Queries/preferences stay feed-local; stable-runtime operations become Q.
  Unknown commands and explicit exit execute locally. Softline retains native
  admission, identity, approval, editing and stopped-entry ownership.
- **Verification:** focused Debug classification, full command-inventory
  editing, wire-level Enter settings/tier cycling, theme/steer ordering,
  cancellation/approval, idle-only operations and both provider protocols.
  The complete decision matrix is in `docs/interactive-command-audit.md`.

## BUG-055: Enter-submitted tier changes wait for the whole turn

- **Status:** fixed.
- **Evidence:** a paused tool-round reproduction showed `/fast` submitted with
  Enter staying Q while the following provider request used the original tier.
  The existing regression exercised only Alt-Enter and missed this path.
- **Resolution:** `/fast` and `/tier` routing changes declare independent native
  queue policies and become executable at the next runtime input boundary for
  either key. They remain editable/approval-controlled until consumed; the
  in-flight request and ordinary conversation queue semantics are unchanged.
- **Verification:** focused Debug classification and local-provider wire checks
  cover Enter toggles, explicit Enter selections, Alt-Enter toggles, unchanged
  in-flight settings, continuation routing and session resume on both transports.

## BUG-054: Whole-turn theme gating strands later steering messages

- **Status:** fixed.
- **Evidence:** session `dbgtfervs68i52ohr8gg` showed `S 1. /theme
  gruvbox:rose-pine-dawn` and a plain `S 2.` while multiple tool/input boundaries
  passed without admission. A local-provider reproduction confirmed that the
  next inference omitted the steer and the theme changed only after the turn.
  CAI classified themes as whole-turn operations; Softline correctly preserved
  the managed-command FIFO. No Softline change is required.
- **Resolution:** theme changes submitted as steers execute at the runtime's
  input boundary. Theme replacement retains the active tool display and updates
  its color setting rather than discarding command identity/decoder/preview
  state. Commands that cannot run at a steer seam are visibly reclassified Q;
  while busy mutating commands use the native ordinary-turn lane so valid steers
  retain Softline's existing seam priority. Idle commands still enter the
  editable native queue. Read-only command policies and approval/stopped state
  remain in Softline.
- **Idle drain:** eligible steering that missed the last active seam starts
  its own input at the idle boundary instead of waiting for a nonexistent
  worker boundary and blocking later commands. The original S identity/mode
  is consumed only after runtime admission. Approval, stopped backlog, editing,
  active drafts, history search and manual delivery remain authoritative.
- **Verification:** Debug classification coverage for whole-turn commands,
  exact theme/steer ordering against both wire protocols, invalid/ASCII themes,
  native and Lua color updates, and command state across palette changes.

## BUG-053: Patch context failures lack actionable diagnostics

- **Status:** fixed.
- **Evidence:** the rejected call `call_CEqj2jeVlnBsU1LSGTezRijO` in session
  `dbgtfervs68i52ohr8gg` changed internal spacing in its expected Go declarations.
  A journal read immediately after rejection confirms the mismatch. Codex's
  matcher also preserves internal spaces, so accepting this patch would require
  a different matching policy. The original diagnostic gave no file or hunk,
  and two log warnings repeated the already-rendered tool failure.
- **Resolution:** identify file/hunk and report bounded, escaped expected/actual
  lines from the first starting-line candidate. Distinguish missing anchors,
  mismatches, ambiguity and EOF failures. Tool guidance now requires copying
  context exactly and rereading after a mismatch. Matching/atomicity are
  unchanged. Attributed duplicate tool warnings stay in the JSON log and no
  longer produce additional interactive notices.
- **Verification:** fast Debug regressions cover the minimized journal case,
  later-hunk atomic failure, anchors, ambiguity, EOF, terminal-control escaping,
  successful correction, log retention and single-failure interactive rendering.

## BUG-052: Completed lease transactions make the next Pouch open slow

- **Status:** reproduced in liblockdc 0.23.0; upstream fix underway.
- **Evidence:** the reported startup spent roughly seven seconds recovering
  722 transaction decisions before instruction/context loading, with 2,888
  storage reads and 722 durable deletes. The root was only about 7.6 MiB.
- **Independent reproduction:** a public-SDK-only probe performs 128 successful
  lease-retention renewals and cleanly closes. First reopen takes 0.945 seconds,
  recovering 128 decisions with 512 reads and 128 fsyncs; the next takes 0.010
  seconds. Acquire/release without renewals and collection-retention renewals
  leave no recovery backlog. CAI currently renews session metadata per event.
- **Report:** `stash/liblockdc-0.23.0-startup-transaction-recovery.md` and its
  standalone C89 probe/runner contain the upstream reproduction and controls.
  These local investigation files are intentionally under ignored `stash/`.
- **Next:** upgrade when the upstream fix is released, rerun the reproducer
  and affected Pouch regressions, and rebuild static musl CAI. No recovery or
  durability workaround has been introduced in CAI.

## BUG-051: Prompt recall remains in plaintext Softline files

- **Status:** fixed.
- **Cause:** the conversation journal cutover did not replace CAI's call to
  Softline's native file backend. Per-conversation `.history` files remained
  outside encrypted Pouch storage.
- **Resolution:** custom Softline load/append hooks use a separate
  `prompts.<session-id>` collection on the existing session-store client and
  key. Writes append one prompt; startup reads a configurable tail (default 200).
  Session deletion drops the prompt collection, and temporary chats use their
  private session store. File-history paths and C/Lua configuration are removed;
  there is no migration or fallback backend.
- **Retention:** whole conversation collections and their metadata use SDK
  retention (default 30 days since update); `/retention` persists a per-session
  override. `/history` controls the 200-entry recall window, explicitly deferring
  stored-entry pruning until liblockdc provides `max_entries`.
- **Verification:** encrypted-store recall, bounded tail, client reuse,
  multiline/Unicode prompts, restart and session switching, draft persistence
  failure/retry, malformed records, and deletion with/without prompt records.

## BUG-050: SDK LQL collection selectors exceed default musl pthread stacks

- **Status:** CAI journal restoration fixed; upstream liblql issue remains.
- **Evidence:** liblockdc 0.23.0 / liblql 0.3.0 selector execution uses a
  178,808-byte scanner frame. A standalone public-SDK probe with one tiny
  document faults on a default musl pthread and passes with a 2 MiB stack.
- **Resolution:** restoration now reads the ordered collection directly. Its
  parser already handles commits and incrementally ignores observations;
  removing the redundant selector also removes a second parse.
- **Verification:** static musl journal restart/export/import round trip,
  including large context, compression and rejected imports. Other SDK callers
  using LQL on small stacks still need the upstream correction. See
  [the investigation](docs/liblockdc-lql-musl-stack.md).

## BUG-049: Review replay loses child output and obscures conversation text

- **Status:** fixed.
- **Evidence:** the reported session retains 2,351 parent assistant text
  fragments. Isolated replay reproduces a long trailing sequence of generic
  tool headings from three unlabelled reviews. The old binary fails a focused
  assertion that reviewer progress was retained in the parent journal. Validated
  findings were also absent from its replayable presentation data.
- **Resolution:** copy child journal events into attributed, bounded `subagent`
  envelopes in the parent journal; mirror execution records once. Preserve
  progress, reasoning summaries and validated Markdown findings without feeding
  them into parent model context. Label review/subagent boundaries, remove
  duplicate successful-tool start headings and place startup policy notices
  before replay. Resumed conversations have an explicit resumed idle message.
- **Completion race:** terminal state could become visible while a child was
  still persisting its final error. Publish terminal state only after final
  records/observations, notify the owner and drain remaining observations before
  closing a manual reviewer.
- **Ownership:** steering pins the child while releasing the parent mutex;
  handoff waits for admitted users before closing it. Envelopes copy immutable
  identity, without reading the worker-owned model/response pointers.
- **Malformed records:** initialize validation diagnostics even when a UTF-8
  visitor rejects text without setting its own error. Decoder and replay-index
  memory checks have no Valgrind errors or leaks. The HTTP poll-only fixture
  passes natively; its 500 ms/1 s deadlines do not accommodate instrumentation.
- **Verification:** deterministic local-provider regressions cover both wire
  protocols, normal parent text, review progress, multi-chunk UTF-8 findings,
  invalid reports, cancellation, generic subagents, selected/unlimited replay,
  source identity and exclusion from model context. Native poll-only and Lua
  replay checks cover embedding without dependence on a terminal event pump.
- **Limit:** historical child output not previously copied into the parent
  journal is not backfilled or synthesized.

## BUG-048: Interactive command queue cutover is incomplete

- **Status:** fixed with Softline 0.14.0 native queue policies (2026-10-28).
- **Admission:** all typed slash and shell commands enter the native queue,
  including while idle. Enter, Alt-Enter and Tab retain Softline's behavior.
- **Delivery:** native execution holds and `eligible` inspection separate feed,
  model/tool and whole-turn boundaries without popping ineligible entries or
  replacing their identity. Independent queries pass blocked commands.
- **Cancellation:** fresh commands can run after Escape without resuming old
  stopped entries. Editing old stopped work retains its stop; explicit
  promotion releases only the selected managed entry. Stopped entries do not
  prevent a fresh `/goal resume` from starting pursuit.
- **Verification:** event-gated local-provider tests cover both wire protocols,
  idle/busy command editing, tool seams, cancellation/resumption, shell activity
  and message approval. No inference or simulated typing delays are required.
- **Full audit:** [command matrix and execution-path inventory](docs/interactive-command-audit.md).
  Synchronous execution after delivery is distinct from queue admission.

## BUG-047: Local commands bypass Softline's editable delivery queue

- **Evidence:** the Enter/Alt-Enter callback forced every slash and pipe command
  to submit immediately, even while sampling. `/goal pause` disappeared from
  Softline before the runtime could apply it. The queued-input consumer understood
  only model steering and whole-turn delivery, without a between-feed delivery
  policy for local inspectors.
- **Resolution:** one Softline queue supports three delivery boundaries. Local
  queries and display controls run between feed writes; agent changes retain
  Softline's ordinary-turn or steering intent until their
  delivery boundary. Commands taken at a model/tool seam use the normal local
  dispatcher. Queue edits retain their identity, position and mode. Goal/model
  changes remain editable until consumed, including when inference is pending.
- **Verification:** deterministic paused-provider regressions cover both wire
  protocols, inspection before first-token output with another command queued,
  Alt-E changes to both queries and agent controls, native turn and steering
  delivery, local validation errors, pipe commands, stopped-queue preservation,
  command exclusion from model input, and queued exit/status rendering.

## BUG-046: Goal becomes active on resume but no continuation starts

- **Evidence:** the reported log has `resumed` with runtime state 5 (cancelled),
  followed by no new turn. App/core continuation guards accepted only idle or
  successful outcomes, unlike normal input. Earlier cancellation tests sent an
  intervening ordinary turn and therefore missed immediate resume. Host resume
  also incorrectly rejected an already-active unfinished goal.
- **Resolution:** normal, queued and automatic inputs share terminal-state
  admission. The application distinguishes live work from the last outcome.
  Resume accepts any unfinished goal and preserves active accounting/timing;
  completed goals remain non-resumable. Codex's external-active goal path was
  checked through `continue_if_idle` into `start_turn_if_idle`.
- **Verification:** mock wire tests directly resume/replace cancelled and
  failed runtimes, asserting that another request runs and completes. TUI
  checks cover both protocols, no intervening prompt/restart, busy active resume
  without duplicate turns, and completed-goal rejection. C and Lua embedding
  controls resume directly from their cancellation event.

## BUG-045: Removing Chrome registration leaves its native hosts running

- **Evidence:** removal only unlinked the manifest. Chrome owns each native
  process through its extension connection; quitting the agent or unregistering
  did not close an existing connection.
- **Resolution:** unregister first, then use separate private control sockets
  to stop matching executable/user hosts in the configured directory. Hosts
  stop accepting browser work, detach tracked tabs and send `turnEnded` before
  acknowledging. Cleanup is bounded and errors reach the utility caller.
  Stale sockets and different executables are not stopped. Live hosts without
  a control endpoint report the one-time extension reload/restart requirement.
- **Verification:** real Unix sockets/native stdio fixtures cover multiple idle
  hosts, an occupied browser connection, different executables, unsupported
  controls, stale/missing endpoints, custom directories and cleanup rejection.
  Existing browser protocol and Lua checks pass. The rebuilt static musl
  executable passes the same shutdown scenarios. Real Chrome is not available
  in this environment.

## BUG-044: Cancelled conversations fail to reopen on repeated input acknowledgements

- **Evidence:** the supplied session log records cancellation followed by repeated
  `input-consumed journal event has no queued input` startup errors. A native
  regression reproduces the rejection for normal/queued prompts, steering and
  goal continuations with repeated checkpoint-covered acknowledgements.
- **Resolution:** covered acknowledgements are idempotent, matching shell-context
  acknowledgements. The checkpoint remains authoritative; duplicate markers do
  not inject input again or discard saved context. Malformed, zero and forward
  references still fail validation. No stored conversation rewrite is required.
- **Verification:** native checks retain and export the saved question/answer,
  without starting inference. The terminal regression cancels, quits with Alt-Q,
  reopens the Pouch-backed conversation and completes another mocked turn.

## BUG-043: Goal continuations omit Codex's objective and completion audits

- **Evidence:** the application sent one short continuation sentence. The
  model could not pause to ask a question, and successful empty turns could
  generate unlimited automatic requests.
- **Resolution:** libcai embeds the Codex continuation template, retaining its
  objective, budget, evidence, fidelity, completion and blocked audits. CAI
  permits model pauses for questions/approvals and model-decided resume of
  paused goals, respecting outstanding approval or stay-paused instructions.
  Completed goals are technically non-resumable. Three consecutive empty
  automatic continuations durably block the goal. Activity and user turns reset
  the streak. C/Lua hosts and libcai_app share continuation admission and policy.
- **Verification:** prompt-source and compiled-byte checks prevent loss of the
  upstream audits. Mock wire tests exercise usage, Unicode whitespace, text,
  reasoning, local/hosted tools, compaction, user input and completed-goal
  rejection. CLI protocols and the Lua facade cover pause/resume and stopping.

## BUG-042: Escape stops pursuit while leaving the goal active

- **Evidence:** turn cancellation left an active durable goal behind while the
  application stopped generating continuations. Explicitly paused goals also
  blocked ordinary user turns, preventing a model-requested resume.
- **Resolution:** settle cancellation by durably pausing an active goal before
  reporting the stop, including cancellation during the final checkpoint and
  an isolated review. Recover pending goal controls before exposing the startup
  snapshot. Paused goals permit user turns; `/goal resume` or a user-requested
  `update_goal(status="active")` resumes pursuit without resetting accounting.
- **Verification:** deterministic native checks cover journal/status/checkpoint
  failures, crash recovery and the final-checkpoint race. Mock Responses and
  Chat Completions terminal checks cover Escape/Ctrl-C, restart, ordinary paused
  turns and both resume paths. Parent goals pause when `/review` is cancelled.

## BUG-041: Command tools replace HOME and silently filter XDG configuration

- **Evidence:** Lingon notifications look under the workspace's `.lingon`:
  both command implementations substituted workspace HOME; Smith also removed
  all XDG settings. Its PTY runner is not an OS sandbox.
- **Resolution:** retain actual HOME/XDG values in both modes. Explicit YOLO
  opts out of local confinement, environment filtering, terminal policy gates
  and the synchronous exec tool's OS sandbox. Startup and `/status` expose the
  effective mode; defaults remain restricted, inherited by child runtimes.
- **Verification:** native tool/environment probes and local mock-provider
  CLI/Lua checks cover filtered/full environments, outside-workspace access,
  reviewer inheritance, configuration precedence and unavailable Bubblewrap.

## BUG-040: Tool failures cannot be reproduced from conversation exports

- **Evidence:** runtime tool-start notifications omitted arguments, failures
  kept only the short message, and the Markdown exporter used model context
  without a durable execution timeline. Logs were required to inspect failures.
- **Resolution:** append structured execution records through the runtime
  journal: exact input/output, partial output and full errors, correlated with
  source/parent sessions and calls. Replay/export do not execute these records
  or add them to model context. Debug/application logs remain separate.
- **Verification:** native runtime probes exercise Unicode chunk boundaries,
  failed tools with partial output, poll-only recording and failed persistence
  before execution. Offline CLI/Lua checks cover provider protocols, export,
  import, replay without logs, provider errors and hosted-tool records.

## BUG-039: Steering disappears from Softline before its delivery boundary

- **Reported:** pending steering does not remain in the native editable queue;
  CAI overrides Softline behavior instead of using its conversation profile.
- **Evidence:** a paused-stream regression fails against the previous static
  executable: the pending `S` entry is gone before the response finishes. The
  earlier BUG-038 checks proved feed order but missed pending queue ownership.
- **Resolution:** use Softline's `QUEUED_TURNS` profile and automatic normal
  delivery. Enter queues the next turn while busy; Alt-Enter steers or promotes
  the newest entry; Alt-E edits it. The runtime synchronizes input collection
  with its owner at response/tool boundaries, including foreground children.
  Pending steering stays in Softline until admission succeeds. No competing UI
  queue or ordinary-input key handling remains.
- **Verification:** offline real-terminal checks cover editing, promotion,
  cancellation, queue saturation, review/worker routing and replay order. Core
  and embedded-host checks cover callback failure, close/cancel and STOP at
  root and child boundaries. See the task commit for the final verification.

## BUG-038: Submitted steering splits streamed assistant output

- **Reported:** a user steer appears between two fragments of the same streamed
  assistant paragraph, with the response continuing beneath it.
- **Evidence:** the application calls `cli_prompt()` before runtime admission;
  it finishes the renderer and inserts the prompt while sampling can continue.
- **Resolution:** native Softline queue admission no longer writes to the
  feed. `RUN_STARTED` and `STEERING_DELIVERED` place prompts at actual runtime
  boundaries. Replay retains pending prompt text until its consumed marker;
  assistant deltas continue directly through libmdf and Softline.
- **Verification:** paused Responses and Chat Completions streams verify that
  trailing text/tool results precede steering. Selected-tail and replay-all
  checks preserve that order. BUG-039 separately covers native pending queue
  ownership, editing and saturation. The native CLI, Lua application and x86_64 musl
  executable pass their applicable offline regressions.

## BUG-037: Parent request after review advertises unsupported image generation

- **Reported:** after `/review`, a ChatGPT WebSocket request fails with HTTP 400,
  `unsupported_parameter`: `image_generation` requires authorization/metering
  unavailable on the `rustponsesapi` route.
- **Pending:** inspect parent restoration and provider/model tool capabilities.
  Do not assume the review caused the capability mismatch without a request
  capture. Session: `dbgat2ip0pfreu3puj5g`.

## BUG-036: Delegated reviewer executes workflow notifications

- **Reported:** the reviewer ran `ping_engineer.sh` before reporting findings;
  that invoked Lingon and failed authentication. Review should hand its report
  back without running the parent workflow's notification commands.
- **Pending:** inspect the review role and inherited global instructions while
  preserving applicable project constraints.

## BUG-035: Reviewer steering is presented but routes to the parent

- **Evidence:** the application queues normal input to the parent while
  `review_runtime` is present; the core explicitly rejects steering an isolated
  review. Native Softline STEER entries are not drained by the application.
- **Decision:** enable steering the foreground reviewer/subagent by default,
  with an explicit disable setting that leaves the input queued for the parent.
  Apply and display steering only at safe conversation boundaries.
- **Resolution:** parent admission forwards steering to its live foreground
  child. `--no-subagent-steering`, `CAI_SUBAGENT_STEERING=false`, or
  `[agent] subagent_steering=false` queues parent work instead. C and Lua
  runtimes expose the corresponding `disable_subagent_steering` policy.
- **Verification:** request captures distinguish manual review, model-launched
  review, and ordinary worker steering from parent turns. Disabled delivery
  stays in Softline's editable queue, including Alt-E/requeue; CLI overrides
  inherited INI/environment policy. The native Softline queue profile and
  delivery policy are initialized for every interactive invocation, including
  the standalone test scaffold.

## BUG-034: Reported ambiguous patch requires its actual input

- **Reported:** `update patch context is ambiguous`, session
  `dbgat2ip0pfreu3puj5g`, call `call_WRPoqB6VLPXOwXHEMxhl95tv`.
- **Pending evidence:** the rejected patch and matching target-file snapshot.
  The provided `/status` identifies the session but contains neither; its log
  is absent from this machine's CAI cache.
- **Existing contract:** repeated unanchored context is rejected without
  modifying the file. The native patch regression covers this and successful
  disambiguation with a context anchor. Do not weaken that guard based only on
  the error message.

## BUG-033: CLI resume treats a listed number as a literal ID

- **Reported:** `cai --resume` lists numbered conversations, but `--resume 2`
  attempts to load ID `2` and reports an incomplete JSON document.
- **Resolution:** resolve positive numeric selections against the current
  directory's numbered listing, retaining exact numeric-ID precedence. Missing
  IDs report not-found rather than a parser failure.
- **Verification:** CLI checks cover both flag spellings, IDs, invalid numbers,
  empty directories and directory isolation. A native selection check covers
  numeric-ID precedence, overflow and storage-query failure without inference.

## BUG-032: Chrome CDP commands pass undefined command arguments

- **Reported:** snapshots and clicks fail with `Invalid parameters` and
  `CBOR: map start expected at position 0` after tab discovery/claim.
- **Cause:** CAI serialized the nested CDP arguments as `params`. Both the
  supplied OpenAI browser bundle and official extension 1.26.1020.11845 use
  `commandParams`; the extension forwards that field to Chrome's debugger API.
  Earlier fixtures accepted either shape without checking the arguments.
- **Resolution:** serialize `commandParams` on the extension wire while keeping
  the CAI tool's public `params` argument. Require the actual wire shape in the
  fixture and inspect values for each high-level action.
- **Evidence:** the stricter peer reproduced the reported CBOR error with the
  previous binary; the corrected native client passes it. Real browser
  acceptance remains to be confirmed. This does not establish the cause of
  the separately reported attach EOF (BUG-030).

## BUG-031: Browser control remains visible after turn completion

- **Status:** explicit detach cleanup implemented; real-browser confirmation
  pending.
- **Reported:** the extension's cursor remains visible after CAI finishes,
  including after the user cancels browser debugging.
- **Evidence:** the supplied OpenAI browser client calls detachAllTabs before
  turnEnded. CAI previously sent only turnEnded, including its orphan-client
  cleanup path. The fixture exposed retained tab attachments at turnEnded.
- **Resolution:** the native host tracks successful attaches, deduplicates tab
  IDs and detaches those tabs before forwarding turnEnded. Normal completion,
  failure/cancellation and disconnected clients share that cleanup path.
  Already-detached errors cannot skip other tabs or final ownership release.
  Only CAI's tracked tabs are targeted; keep/deliverable handling remains with
  the extension. The whole sequence has one five-second budget.
- **Verification:** fixture coverage includes several tabs, repeated attaches,
  explicit detachment, client abandonment and user-detach notifications with
  detach errors. Physical extension cursor behavior still requires confirmation
  on the user's Chrome installation.

## BUG-030: Chrome attachment disconnects on omitted void results

- **Status:** compatibility fix implemented; real-browser confirmation pending.
- **Reported:** tab listing/opening and the extension's control arrow work,
  but attachment fails with `Chrome native host disconnected during a frame`
  and `EOF`, preventing screenshot retrieval.
- **Reproduction:** the production native-host fixture produces the same EOF
  after successful tab listing when an attachment success reply omits `result`.
  The supplied OpenAI browser transport accepts such replies as JavaScript
  `undefined`; the prior fixture always emitted explicit `result: null`.
- **Resolution:** normalize omitted success results to JSON null only for
  pending void-capable methods, in both host and client. Preserve ID checking,
  result requirements for identity/tab data, and error validation. CDP replies
  may be void, but screenshots must still contain image data.
- **Verification:** exercise omitted results, failures and malformed replies,
  notifications and reverse heartbeats, plus the native-host/CLI screenshot
  flow and Lua runtime. The reported browser's actual reply has not been
  captured; this proves the reproduced failure mode rather than every possible
  cause of a native-host disconnect. A running host must restart to use the fix.

## BUG-029: UTF-8 C1 shell output aborts rendering and replay

- **Status:** fixed.
- **Cause:** the literal shell renderer rejected single-byte controls but
  passed decoded U+0080–U+009F through to softline, which rejects them. The
  same rendering path could then prevent a stored conversation from reopening.
- **Resolution:** escape C1 controls byte-for-byte after UTF-8 decoding,
  retaining split-scalar state and preserving non-control Unicode. Stored
  events and model context are unchanged; live output and replay share the fix.
- **Verification:** the native shell-runner regression covers every C1 scalar,
  raw C1 bytes, every two-chunk split, output-buffer flushes, neighboring Unicode
  and an incomplete final scalar. The TUI regression checks live output and
  automatic resume, plus imported raw-tail/hex records (including a split
  scalar) rendered during startup without executing the recorded commands.

## BUG-028: get_goal rejects result-shaped arguments tolerated by Codex

- **Status:** fixed.
- **Evidence:** the saved failing call supplied `{"has_goal":false}`. CAI's
  typed argument validator rejected that unknown field before the read handler.
  Codex advertises an empty-object schema but ignores argument contents.
- **Resolution:** ignore inputs only for `get_goal`, including for spooled
  dispatch, while returning actual goal state. Keep validation for mutating
  tools unchanged. No global relaxation or public API/ABI change is involved.
- **Verification:** native tests cover empty, extra, result-shaped, scalar,
  malformed and spooled arguments, state preservation and mutating-tool
  rejection. Mock Responses/Chat Completions and Lua application checks verify
  successful dispatch, unchanged schema and absent-goal results without errors.

## BUG-027: Command previews print terminal escapes as unreadable text

- **Status:** fixed.
- **Cause:** the tool renderer escaped every control byte, including the ESC
  introducing ordinary Git/ripgrep color sequences, displaying `\x1b[32m`.
- **Resolution:** incrementally filter command terminal controls, preserving
  recognized SGR colors/styles when enabled. Drop cursor operations and control
  strings. Isolate style and split UTF-8/control state per command, resetting
  styling before returning to the host. `NO_COLOR` produces plain previews;
  structured logs and model-facing tool results retain their original content.
- **Verification:** `cai_display` covers every two-chunk split, malformed and
  unfinished controls, hidden strings, UTF-8/C1 boundaries, interleaving, style
  resets, bounded state, previews and sink errors. `cai_cli_tool_display` checks
  colored/plain output through softline and original content in JSON logs.
  Related terminal/theme/reasoning and Lua application checks pass, as does
  Valgrind for the renderer.

## BUG-026: Routine Pouch compaction cleanup appears as failure warnings

- **Status:** investigated; upstream diagnostic improvement needed. No
  functional compaction failure was confirmed in this incident.
- **Evidence:** compaction completed at 13:09:02 with three obsolete artifacts
  awaiting deletion. The derived index was deliberately invalidated and rebuilt
  successfully on reopening at 13:11:39. The 15-minute deletion grace period
  does not promise cleanup at that deadline; background maintenance defaults
  to a 30-minute interval.
- **Verification:** an isolated copy of the reported store reopened and listed
  sessions without errors. Both existing CAI compaction/recovery checks passed,
  including byte-for-byte exports, credentials, keys, and resumed context.
- **Upstream report:** [compaction diagnostics](docs/liblockdc-compaction-diagnostics.md)
  identifies the duplicate warning sites and the misleading `records` field.
  No liblockdc source or original user storage was modified.

## BUG-024: apply_patch rejects delete/add replacements

- **Status:** fixed.
- **Evidence:** the rejected call deleted `internal/toll/calculator.go`, added
  unrelated files, then added its replacement at the original path. A private
  copy of the session store confirmed the submitted patch; the live store and
  workspace were not modified during diagnosis.
- **Resolution:** coalesce delete-then-add into one update before preflight,
  preserving permissions, size limits, path validation and atomic publication.
  Other repeated operations remain rejected. The duplicate-file-section
  diagnostic now names the path and suggests merging hunks.
- **Related defect:** rollback retained the temporary file's pre-exchange
  ctime, causing restoration to fail after a later write error. Retain the
  verified post-publication stat instead, while preserving concurrent-file
  checks.
- **Verification:** direct and spooled APIs cover non-adjacent replacements,
  normalized aliases, empty/no-final-newline sources, duplicate rejection,
  missing sources, size limits, permissions and later partial-write rollback
  for both replacement and ordinary update. A real CLI preview includes a
  replacement followed by a neighboring update.

## BUG-025: Managed terminal drops installed toolchains from PATH

- **Status:** fixed.
- **Cause:** the terminal's minimal environment replaced the inherited `PATH`
  with system directories, excluding `/usr/local/go/bin` and user toolchains.
- **Resolution:** capture an owned `PATH` at tool registration. Preserve an
  explicit empty value; use the system default only when unset. Other host
  environment variables, including credentials, remain filtered.
- **Verification:** native tests execute a fixture tool through a custom path,
  change the parent environment after registration, and check empty/unset
  paths. Existing credential-filtering checks remain active. The CLI executes
  a fixture executable outside the standard search directories.

## BUG-023: Model-created active goals stop after the first turn

- **Status:** fixed.
- **Cause:** the application enabled automatic pursuit only for goals started
  through commands or startup flags. A model's `create_goal` tool updated the
  durable goal but left the application's separate activation flag unset.
- **Resolution:** remove the separate activation flag and consult durable goal
  state at settled turn boundaries, including after replay. All creation paths
  share continuation scheduling. Failed/cancelled turns stop pursuit until a
  new user turn; paused or limited goals do not automatically continue.
  Approved queued prompts precede generated continuations.
- **Verification:** the original two-request stall was reproduced with a mock
  provider before the fix. Regressions exercise Responses and Chat Completions
  interactively and in batch, plus public C and Lua application receivers:
  model-created goals, multiple continuations, completion, repeated startup
  instructions, queue ordering, pause/reopen/resume, cancellation, provider
  failure, blocked-goal termination, startup replay and explicit session
  switching.
- **Presentation:** the status bar shows `goal=STATUS` and elapsed active time
  in compact days/hours/minutes (for example `goal=active 10d10h45m`); the
  objective and accounting remain available through `/goal`. Completed goals
  remain as `goal=achieved` with their final duration until replaced or cleared.

## BUG-021: Reasoning row markers and styling are applied after Markdown rendering

- **Status:** fixed.
- **Cause:** reasoning went through libmdf, then a separate output-sink state
  machine injected `?` markers into rendered rows. Wrapping and quote styling
  consequently belonged to different layers.
- **Resolution:** stream real Markdown blockquote source into the existing
  renderer. Preserve blank lines, normalize split CRLF/CR, retain Markdown body
  structure and let libmdf wrap every row. Only line-boundary state is retained.
  Headings remain plain text in softline's message line. Notice/command markers
  retain their existing renderer path.
- **Verification:** all chunk sizes, UTF-8 splits, blank lines, partial lines,
  sink failures, narrow wrapping, multiple provider sections and transitions
  between assistant/reasoning/tool output are covered without live inference.
- **Styling:** quote colors and body styles belong to libmdf's active theme.
  The default theme retains normal body text; CAI injects no colors around it.

## BUG-020: apply_patch rejects harmless dot components as unsafe

- **Status:** fixed.
- **Cause:** path validation rejected `./` and interior `.` components before
  resolving the actual destination. The missing-parent rejection was fixed
  separately in BUG-017; running applications need a restart to use that fix.
- **Resolution:** normalize dot components and repeated separators for add,
  update, move and delete before collision checks. Continue rejecting `..`
  traversal, directory-only targets and escaping symlink parents.
- **Verification:** native coverage includes relative/absolute aliases, all
  four operations, shared missing parents and normalized destination collisions.
  The mock-provider CLI creates `internal/toll` files with both path spellings.
- **Expanded regression coverage:** direct and spooled APIs check 31 failure
  cases each: path aliases, source/destination collisions, missing sources,
  directory/file mismatches, hardlinks, symlinks, traversal and absolute paths
  into a similarly named sibling workspace. Each verifies source/outside
  preservation, rollback of the preceding add and absence of success receipts.
  Additional cases cover exact/exceeded patch and file byte limits, spaces,
  UTF-8, hidden filenames, long normalized spellings, actual overlong paths,
  injected new-parent synchronization failures, partial OS writes under a
  child-local file-size limit and rollback across new nested directories after
  a later publication conflict. Synchronization uses process
  barriers, without timed sleeps, polling or inference.

## BUG-019: libmdf drops escaped punctuation in headings and plain paragraphs

- **Status:** upstream issue; reproduced independently of CAI's feed.
- **Reproduction:** libmdf 0.15.0, ANSI format, width 60, ANSI off; set a plain
  output sink, feed `### Full \*literal\* title\n` one byte at a time, then
  finish the document. Output is `### Full *literal title`, losing the second
  literal asterisk. The plain-paragraph variant also loses it.
- **Scope:** CAI generates valid escaped Markdown. A bold subtitle preserves
  both literal asterisks, and is used for the todo detail view. No upstream
  source or dependency pin was modified.

## BUG-018: Reasoning summaries run against surrounding assistant text

- **Status:** fixed.
- **Cause:** switching between answer and reasoning documents finished the
  renderer document without separating the next visible block. The `?` marker
  already identified provider reasoning summaries, but both transitions lacked
  a blank row.
- **Resolution:** apply the existing feed separator to completed answer and
  reasoning blocks as well as tool output. Preserve one continuous turn/feed,
  immediate streaming and uninterrupted chunks within each block.
- **Verification:** renderer assertions cover both transitions; a paused
  mock-provider response emits commentary, a fragmented multi-paragraph summary
  and more answer text, proving the turn remains active and uses one request.

## BUG-017: Nested patch additions fail when parent directories are missing

- **Status:** fixed.
- **Cause:** new-file resolution required an existing immediate parent and
  reported every resolution failure as a workspace escape. Parent descriptor
  traversal also lacked directory creation.
- **Resolution:** validate the nearest existing ancestor, pin existing parents,
  and create missing add/move parents only after the complete plan validates.
  Descriptor traversal rejects symlink components and unsafe paths; new
  directories respect the process umask.
- **Verification:** native regression covers the reported four-file Go layout,
  shared missing parents, moves, permissions, traversal, escaping/dangling
  symlinks, non-directory parents and failure without preflight side effects.
  The mock-provider CLI regression covers nested freeform tool additions.

## BUG-022: Extra blank row before the first quoted user prompt

- **Status:** fixed by softline 0.12.0.
- **Required behavior:** one empty row between the first quoted user prompt
  and preceding terminal content, including the launching shell or host banner.
- **Cause:** a fresh stream initializes its trailing-newline count to zero.
  `sl_output_stream_write_quoted_prompt_impl()` therefore inserts two LFs even
  when the startup cursor report already places output at column zero.
- **Evidence (2026-10-19):** a standalone public-softline C probe in a private
  tmux terminal shows exactly two blank rows after both a shell marker and a
  host banner. No CAI runtime, Markdown renderer or provider is involved.
- **Repair boundary:** account for the existing line boundary when acquiring
  a fresh native stream with a confirmed column-zero cursor. Preserve later
  quote spacing, unfinished-line handling and known spacing across reopen.
  CAI should continue using softline's quoted-prompt API.
- **Verification (2026-10-20):** `cai_cli_startup_spacing` checks actual tmux
  terminal rows after replay, both following a shell marker and after an
  embedded host banner. Exactly one blank row precedes the first quoted prompt.

## BUG-013: Sparse patches omit surrounding unchanged lines

- **Reported:** isolated signature edits show only removed/added rows, with no
  neighboring code to orient the reader.
- **Resolution:** retain one adjacent unchanged line from the current edit's
  snapshot when the supplied hunk omits context. Stream those preview lines
  alongside the submitted changes; use no VCS comparison. Adjacent edits and
  file edges must not fabricate unchanged context. Compact previews mark gaps
  between hunks with `⋮` and retain the existing diff colors without syntax
  highlighting.
- **Verification:** cover sparse/separate and adjacent hunks, deletion, file
  edges, native Responses and Chat Completions tools, and bounded event chunks.
  Real CLI previews, native unit checks and Lua facade checks pass.

## BUG-014: Reasoning status concatenates separate summary headings

- **Reported:** `Planning full ANSI Tetris implementationConfirming build and
  source setup` appears as one thinking title (`stash/thinking_bug.png`).
- **Resolution:** preserve provider summary-section boundaries in the runtime,
  expose the boundary event in C and Lua, and replace the status title when a
  new section starts. Preserve body rendering and the current turn's timing.
- **Verification:** stream two fragmented summary sections in one response and
  assert that the latest title replaces the earlier title without becoming
  reasoning-body output.
  The real CLI multi-part regression and Lua event-constant check pass.

## BUG-001: Terminal resize corrupts the transcript and prompt position

- **Priority:** critical.
- **Status:** the controlled retained-frame corruption is fixed with softline
  0.11.0 and owner-thread reconciliation. All eight terminal resize regressions
  pass. The original intermittent XFCE incident's exact sequence is not traced.
- **Reported:** after resizing CAI, old command/source text remains inside patch
  rows or below the input prompt. The status bar and prompt can appear in the
  middle of output. Subsequent output can overwrite existing text.
  Environment: xfce4-terminal directly on X on local hardware, without SSH,
  tmux, or screen. The user cannot currently reproduce it consistently.
- **Required behavior:** transcript rows remain intact, feed output stays above
  the status/editor area, and resize updates both softline and libmdf geometry.
- **Evidence:** normal interactive assistant, reasoning, tool, patch, notice,
  and command output uses the softline output stream. `/shell` deliberately
  releases terminal ownership. CAI does not replace `SIGWINCH`; softline
  checks terminal dimensions with `TIOCGWINSZ` in its editor and feed paths.
- **Terminal coverage:** `tests/cli_terminal_resize_test.sh` uses a private tmux
  terminal to inspect actual displayed cells and scrollback after long command
  output, shrink/grow, a large patch and the final assistant response. It covers
  both a paused provider and a resize inside the runtime/renderer callback.
  Six softline-only cases cover watch, yielded and retained owner contexts.
- **Why byte-only coverage is insufficient:** the existing PTY peer records
  output bytes and a bounded input-row snapshot; it does not implement terminal
  reflow or verify the complete displayed transcript. It cannot prove that old
  source cells were cleared or that every row is positioned correctly.
- **Original reproduction (2026-10-16):** a softline-only C program on 0.10.0
  deterministically appended old status-message cells to an edit heading after a retained
  prompt, unfinished ASCII line, and settled 120x30 -> 60x18 resize. The observed
  line is `* Edited snake.cize reproduction`, instead of `* Edited snake.c`.
  This needs no libcai, libmdf, provider, tools or concurrent physical write.
  The report and exact reproduction steps are in
  [the upstream diagnostic](docs/softline-retained-resize-report.md).
- **Fix (2026-10-19):** softline 0.11.0 supplies
  `sl_output_stream_reconcile()`. CAI calls it on the UI owner thread before
  writing after changed terminal dimensions, including synchronous output after
  `sl_next_prompt` returns. Both feed fragments and quoted user prompts use this
  boundary. Failed reconciliation prevents the next write and is not cached as
  success. Ordinary fragments at unchanged dimensions do not reconcile again.
- **Coverage:** the previously disabled retained/shrink test now gates and
  passes without weakening its screen assertions. All eight terminal scenarios
  verify output and prompt placement; a fast unit test covers width/height
  changes, unchanged dimensions, redirected output, failure/retry and quoted
  prompts. No artificial feed delay, transcript buffering or replay was added.
- **Remaining boundary:** physical resizes can race a terminal write, and
  softline still documents Unicode/tab reflow limitations. These tests prove
  the controlled settled-resize repair, not every possible XFCE resize sequence.
- **Dependency update (2026-10-26):** CAI now pins softline 0.13.0, including
  its additional resize tracking and retained-cursor fixes. Alt-S uses native
  draft suspension across the existing local terminal handoff.

## BUG-002: Missing blank row before some tool activity blocks

- **Priority:** high.
- **Status:** fixed; renderer and interactive feed checks pass.
- **Reported:** `* Running`, `* Ran`, and similar tool headings sometimes run
  directly against the preceding assistant paragraph or command output.
  Other transitions already have the desired spacing.
- **Required behavior:** one empty row above each tool activity block, without
  changing literal command output or inserting blank rows inside patches.
- **Verification:** cover assistant-to-tool, command-output-to-completion,
  consecutive tools, empty output, partial final lines, failures, and notices.
  Assert spacing in the feed and the displayed terminal.
- **Fix:** activity headings ask the feed owner to establish a blank row.
  Standalone library rendering separates its own blocks. Literal payloads and
  patch rows retain their existing spacing; partial final lines are terminated
  before the separator. `cai_display` and `cai_cli_tool_display` cover wrapped
  patches, preambles, partial/empty command output, failures and consecutive calls.

## BUG-003: Slash-command transcript colors do not communicate outcome

- **Priority:** normal.
- **Status:** fixed; native and static musl command checks passed.
- **Reported:** `$ /command` uses pale text and an undifferentiated `$`.
- **Required behavior:** green `$` for success, red `$` for failure. Command
  text should be code-like, non-italic, and brownish orange (the latest color
  selection supersedes the earlier prompt-colored/green suggestions). Keep
  terminal input distinct from the submitted transcript.
- **Verification:** successful and invalid slash commands, refused operations,
  wrapped commands, and `NO_COLOR`. Preserve input-before-result ordering and
  keep non-interactive structured logging intact.
- **Fix:** defer the command echo until validation or the first result, keeping
  it before all result output without buffering results. Use brownish-orange
  literal text, with green/red outcome markers and explicit color resets.
  Invalid resume selections, unknown commands, and busy session switches now
  report rejection through the normal diagnostic path.
- **Evidence:** `cai_cli_commands` covers successful and invalid settings,
  invalid resume selections, unknown commands, literal Markdown punctuation,
  wrapped output and `NO_COLOR`. Existing settings, shell, handoff, session,
  logging, and spinner checks pass with the change.

## BUG-004: Patch colors are too bright and lack distinct change markers

- **Priority:** normal.
- **Status:** fixed; C, Lua and interactive feed checks pass.
- **Reported:** the current ANSI red/green patch backgrounds are too bright;
  patch text and `-`/`+` markers do not have the intended contrast.
- **Required behavior:** use the palette from `../../github/codex/`: darker red
  and green backgrounds, brighter white text, bright red `-`, bright green `+`,
  and no background on unchanged context. Retain line numbers and compact/full
  patch behavior.
- **Verification:** inspect Codex's actual color constants; assert emitted
  colors and resets, unchanged context, wrapped rows, and color-free output.
- **Fix:** Codex's dark palette is `#213A2B` for additions and `#4A221D`
  for removals. Changed rows use bright white text, distinct bright green/red
  signs, and a reset after each row. Context has no background. Numbered,
  unnumbered, chunked and wrapped patch paths use the same palette; plain output
  remains free of ANSI sequences.

## BUG-005: Context window differs between startup and session replay

- **Priority:** high.
- **Status:** fixed; native, CLI, Lua and static-musl feature checks pass.
- **Reported:** ChatGPT `gpt-6-luna` with medium reasoning initially reports
  approximately one million context tokens, but reports 272000 after reload.
  A supplied `/status` example reports 14733 / 272000 tokens, or 5%.
- **Required behavior:** resolve the effective provider/session context limit
  after replay before presenting a context percentage. Fresh and resumed
  sessions should agree when their effective settings agree; a catalog fallback
  must not silently replace an authoritative session/provider limit.
- **Verification:** fresh versus resumed sessions, saved model/effort settings,
  live metadata versus fallback metadata, long-context settings, unavailable
  metadata, and consistent `/status` and status-bar percentages.
- **Cause/fix:** startup exposed the compiled API model maximum before account
  catalog discovery. Signed-in ChatGPT now reports an unknown window until
  authoritative metadata is available. Portable state retains the last effective
  window for its selected request model; resume can use it without a startup
  catalog call. Current metadata takes precedence, and metrics/compaction share
  the same window. Refreshing active limits preserves retained-history hash
  provenance until compatibility compaction succeeds, including retry failures.
- **Evidence:** native state round-trip/invalid-state/live-update tests;
  interactive fresh/offline-resume/provider-update capture; Lua Responses and
  Chat Completions exact-resume tests; existing model-switch/compaction failure
  tests. Lua now exposes runtime metrics and explicit client wire protocols.

## BUG-006: Spinner restarts during an active turn

- **Priority:** normal.
- **Status:** fixed; targeted native and static musl verification passed.
- **Fix commit:** `88e3eb5`.
- **Cause:** CAI reapplied softline's busy and spinner setters on every status
  refresh, including provider/tool wakeups and the elapsed-time timer. Softline
  0.10.0 resets the animation clock and frame in these setters even when their
  arguments are unchanged. This is CAI's integration error.
- **Fix:** apply busy/spinner changes only at busy-state transitions, keeping
  the spinner active while the runtime reports an active turn.
- **Verification:** `cai_cli_spinner` traces calls reaching the real softline
  setters using controlled runtime snapshots across repeated elapsed-time
  updates, tool boundaries, completion, provider failure, cancellation and
  subsequent turns. This directly exercises the production status projection
  without an inference run or timer sleeps.
  The existing Responses and Chat Completions settings tests pass, including
  queued settings changes during a turn. Static musl terminal failure/recovery
  coverage also passes.

## BUG-007: Codex global instructions appear missing after session replay

- **Priority:** high.
- **Status:** reopened after another report on an older conversation. Request
  capture tests include current files after resume, but the reported omission
  has not been reproduced. `/status` now reports CAI global, Codex global and
  repository files actually captured in the current agent instructions. This
  evidence is read from libcai, not inferred from configuration or file existence.
- **Reported:** enabling `[agent] codex_agents_md = true` in `config.ini` does
  not appear to load the global Codex `AGENTS.md` when resuming a session.
  The user confirms restarting CAI after editing the configuration.
- **Required behavior:** current startup flags, environment, and `config.ini`
  determine configuration; there must be no pouch-backed CAI configuration
  overriding them. Load CAI's global instructions, then
  `${CODEX_HOME:-$HOME/.codex}/AGENTS.md` when enabled, then ancestor/workspace
  instructions. Resuming conversation state must not suppress current global
  instructions or restore a stale instruction snapshot over them.
- **Verification:** capture actual provider requests for fresh and resumed
  sessions, enabling/disabling compatibility between invocations, changed
  instruction contents, default versus explicit `CODEX_HOME`, and configured
  CAI global instructions. Inspect pouch document kinds and replay to establish
  whether configuration or old instructions are overriding current settings.
- **Current evidence:** static musl request-capture tests pass for Responses
  and Chat Completions: enabling compatibility on an existing encrypted
  conversation, edited instructions, explicit CODEX_HOME, and disabling it
  again all affect the actual outbound instructions. The reported real-session
  case was still unaccounted for at that stage; those passing probes alone
  did not close the report.
  The CLI pouch adapter stores sessions, scope coordination, and credentials;
  it does not supply CAI startup configuration.
- **Added evidence and diagnostics:** current instructions also survive saved
  model selection and configuration-driven `CODEX_HOME`. `CAI_CODEX_AGENTS_MD`
  now follows CLI > environment > INI precedence, including an explicit
  `--no-codex-agents-md` override. The log records each global instruction
  source, resolved path and loaded byte count, or that the file is absent.
  File-open errors include the path and OS reason. No replay suppression or
  configuration stored in pouch was found. At that stage the reported
  real-session discrepancy remained open pending the follow-up below.
- **Follow-up evidence (2026-10-16):** a local provider fixture captured fresh
  and resumed requests using the actual user `config.ini` and Codex global file,
  with isolated state/cache paths. Both requests contained the complete current
  global file, and logs confirmed its resolved path and loaded bytes. The last
  available real-session startup was at 13:43:44 on October 15; the configuration
  file was modified at 13:43:58, after that startup. This log therefore cannot
  demonstrate instruction loss after enabling the setting. No live provider
  request or user-store mutation was needed for the probe.
- **Coverage/diagnostics:** C CLI capture now also covers interactive `/resume`,
  re-reading changed instruction files while retaining startup settings. Lua
  covers exact resume and enabling/disabling Codex instructions for both wire
  protocols. Logs explicitly identify disabled Codex compatibility; an existing
  empty global file is distinguished from an absent file. Configuration edits
  require a restart. The original subjective model report was not reproduced;
  current outbound requests are verified rather than inferring behavior from
  whether an assistant says it read an instruction.
- **Behavioral reproduction and resolution (2026-10-17):** the user's check
  was asking "Who am I?", expecting the production-executive role stated in
  the global file. Live `gpt-6-luna` with medium reasoning recognized that role
  in a fresh CAI conversation, but answered generically after resuming a
  conversation originally created with Codex instructions disabled. Logs still
  showed the complete 13,961-byte file loaded. Codex 0.162.0-alpha.18 recognized
  the role after adding the same file and resuming an equivalent conversation.
- **Fix and evidence:** libcai now identifies each loaded source as AGENTS.md,
  frames its complete contents in an `INSTRUCTIONS` block, and explicitly
  identifies it as the current user's instructions and working context,
  including stated role/preferences. File order and instruction authority are
  unchanged; model switches retain the framing. The live CAI disabled-to-enabled
  resume case now answers "You're the production executive for this workspace".
  These observations establish the reported behavioral improvement, rather
  than proving the previous generic answer meant that loading failed.
  Native prompt/review/model-switch tests, full multi-chunk file capture over
  both wire protocols, interactive resume, and Lua resume checks pass.
  Local live evidence is in `build/cai/agents-live-resume-probe.log`,
  `build/cai/codex-agents-live-probe.log`, and
  `build/cai/agents-framing-live-probe.log`. Auth copies and fixture state were
  isolated; no user conversation or original credential file was changed.
- **Compaction hypothesis (2026-10-19):** not reproduced. A native wire-level
  regression loads real CAI global, Codex global and workspace AGENTS.md files,
  a skill catalog and a developer extension. Both Responses and Chat Completions
  retain them in ordinary requests after rejected and successful compaction.
  Exporting compacted state and importing it into a newly constructed Smith
  agent sends the updated global file, without restoring its old contents.
  Runtime request assertions also preserve the active instruction extension
  during automatic compaction at a tool boundary and immediately after resume.
  Compaction replaces conversation history, not the separately retained active
  instructions; it does not reload edited files from disk. These request checks
  establish delivery, not whether a model follows every supplied instruction.
  The latest user-reported behavioral discrepancy remains unaccounted for.

## BUG-008: Enable the existing todo/kanban tool in CAI

- **Priority:** normal; feature backlog.
- **Status:** implemented; optional and disabled by default.
- **Original evidence:** libcai implemented `todo_kanban` and exposed
  `cai_agent_register_todo_tool`, but neither the CLI nor the Smith/runtime
  startup path registered it.
- **Required work:** expose the existing tool to the normal CAI agent, provide
  appropriate persistent storage, and include its capabilities in the agent's
  instructions. Cover fresh sessions, replay, persistence, failed operations,
  and the actual tool list sent to the provider. Keep isolated review behavior
  deliberate.
- **Implementation/evidence:** Smith runtime and CAI register `todo_kanban`
  when enabled. CAI uses an encrypted `cai.todo` namespace with streamed private
  lease reads and bounded upload backpressure; no JSON state file is created.
  Temporary rewrite attachments remain transactional. Establishing the read
  snapshot before upload avoids storage-lock inversion. Tests cover actual
  provider declarations, add/list across invocations, 256 KiB rewrites, multiple
  rewrites in one transaction, rollback, injected commit failure and an aborted
  partial upload. Lua retains borrowed native stores through runtime close and
  releases them correctly on failed startup.

## BUG-009: Expose optional tools and consistent tool configuration

- **Priority:** normal; feature backlog.
- **Status:** implemented; native protocol/CLI/Lua checks pass.
- **Previously missing tools:** todo/kanban (BUG-008), SearXNG search, and
  reverse geocoding. These extras are now available and disabled by default.
- **Smith defaults:** hosted Responses web search and Chrome extension control
  are enabled by default. Chrome connects only when called; ordinary startup
  needs no installed/connected browser. Chat Completions omits hosted search;
  optional SearXNG provides function-based search there. Isolated reviewers
  receive no default browser, hosted search or optional service/state tools.
- **Required configuration:** explicit enable/disable controls through CLI
  flags, environment variables, and `config.ini`, with documented precedence
  and a commented default template. Include provider/service settings where
  required, and individual controls for existing tools that lack them.
- **Executor distinction:** libcai also has a separate sandboxed exec preset
  using the same `exec_command` name as the registered terminal pair. Expose
  any alternative through explicit backend selection; do not register two
  implementations under the same name.
- **Verification:** inspect outbound provider tool declarations for defaults,
  each enable/disable path, precedence, resumed sessions, unavailable services,
  actual dispatch, and isolated review capabilities. Settings are startup
  configuration, not pouch-backed configuration.
- **Evidence:** request capture covers defaults, explicit flags, environment,
  INI, `[env]` precedence, resumed sessions, missing browser hosts, successful
  service dispatch and HTTP 503 recovery. Local file/goal/skill capability
  restrictions also narrow custom subagents. README, man page, public C fields,
  Lua runtime tables and generated INI schema describe the controls.
  Default web/browser registration is gated by explicit preset capability bits;
  a restricted custom descriptor named Smith cannot acquire extra tools.
- **Primary artifact:** the rebuilt static musl binary passes both protocol
  policy/service tests, both global-instruction replay tests and the interactive
  patch/spacing test. Browser protocol/setup coverage uses local fixtures;
  an installed Chrome extension was not exercised on this machine.

## BUG-010: /model catalog request times out and prints duplicate diagnostics

- **Priority:** high.
- **Status:** fixed; focused regression and affected CLI checks passed.
- **Reported:** `/model` cannot list the signed-in ChatGPT catalog. The GET
  `models?client_version=0.160.0` times out after about five seconds; CAI prints
  both `HTTP transport failed` and `Model catalog unavailable`, after a generic
  "Model list unavailable" response.
- **Required behavior:** use a suitable deadline for explicit model discovery,
  preserve available catalog data, and surface one actionable catalog failure
  with useful transport details. A catalog error must leave model selection,
  reasoning effort, session state, and ordinary turns usable.
- **Verification:** delayed success, genuine timeout, offline/provider failure,
  cached-catalog fallback, subsequent successful retry, and actual interactive
  output. Identify the failing deadline before increasing any timeout.
- **Cause and fix:** CAI unconditionally replaced the model-discovery client's
  configured timeout with 1500 ms. Discovery now inherits the configured client
  deadline. Explicit listings bypass the failure cooldown, and an unsuccessful
  refresh can display a previous catalog. Scoped catalog logging retains all
  transport records in the file while suppressing duplicate lower-level
  notices in the default JSON logging mode.
- **Evidence:** `cai_cli_model_catalog` passes a response delayed beyond the old
  deadline, HTTP 503 with cached fallback, a configured 100 ms timeout, a single
  actionable interactive warning, and immediate successful retry.

## BUG-011: Idle core dump followed by an unreadable pouch index warning

- **Priority:** critical.
- **Status:** fixed by upgrading to liblockdc 0.20.0; static-musl background
  compaction and storage recovery regressions pass.
- **Reported:** CAI dumped core while idle. On the next startup it printed
  `cache.load.index.error: pouch query-index manifest is not readable`
  for namespace `cai.sessions`, code 1.
- **Required behavior:** idle CAI must not crash. After abnormal termination,
  authoritative session documents and credentials must remain usable. Damaged
  derived query indexes should recover through liblockdc's supported recovery
  mechanism, with a clear diagnostic explaining recovery or required action.
- **Investigation:** locate the crash record and matching executable, inspect
  index loading and rebuild semantics, reproduce against an isolated test store,
  and distinguish a missing/corrupt cache from authoritative storage damage.
  Do not delete, rebuild, or otherwise modify the user's existing store during
  diagnosis. A missing index and a corrupt index need separate coverage.
- **Recovery evidence:** the actual CLI recovers missing, empty and malformed
  query-index manifests in an isolated encrypted store. Listings are unchanged,
  export and resume preserve the conversation, and the encryption key remains
  unchanged. A subsequent open no longer emits the warning. The diagnostic now
  explains that indexed queries will attempt recovery from stored records.
- **Follow-up (2026-10-18):** with liblockdc 0.21.0, auto-resume followed by
  `/quit` repeatedly reported the invalid index. A prompted turn repaired it.
  Auto-resume can use the directory cache and direct reads without an indexed
  query, so the previous listing-driven recovery test missed this path.
  The regression reproduced the warning on the second auto-resume/quit cycle.
  Local Pouch startup now calls liblockdc's public wait flush, retaining the
  healthy-index fast path and repairing derived state before replay. Repair
  errors remain nonfatal and emit a diagnostic. Remote lockd startup is
  unchanged. Regression coverage includes missing, empty and malformed indexes,
  repeated auto-resume/quit cycles with no model requests, and an unusable
  index path that must not prevent direct replay.
  The expanded recovery regression and ten related startup, storage, auth,
  idle, endpoint and package checks pass with liblockdc 0.21.0.
  The collections cutover with liblockdc 0.22.0 also prepares the `cai.scopes`
  index at open. The recovery regression now corrupts that workspace index and
  verifies repeated resume/quit without inference or a recurring warning.
- **Crash coverage:** an idle writer killed without closing its pouch can be
  reopened and resumed. The static musl CAI also survives two real ten-second
  background index-publication cycles while interactive and idle. Neither test
  reproduced the reported core dump; no matching CAI core was available during
  those checks. The subsequently supplied core is analyzed below.
- **Lease hypothesis:** the user observed the warning disappear and suggested
  lease expiration. The logged code is `LC_ERR_INVALID` (1), not
  `LC_ERR_TIMEOUT` (6); it comes from parsing an invalid index manifest during
  cache warming, which does not acquire a document lease. CAI's normal document
  leases have a one-hour TTL and a separate acquisition wait. Automatic index
  rebuilding explains the same disappearance without waiting for lease expiry;
  the user's actual recovery timing remains unverified.
- **Core evidence (2026-10-15):** the supplied core and matching `~/cai`
  executable identify a stack overflow in liblockdc's background compaction
  worker. `lc_pouch_state_copy_file_span_to_fd_crc` reserves a 128 KiB local
  buffer; its first stack write faults in the musl thread's guard page.
  The then-current 0.19.0 static build contained the same allocation. The prior
  index-recovery checks did not exercise this compaction path. See
  [the upstream report](docs/liblockdc-compaction-stack-overflow.md) for the
  verified backtrace, instruction/register evidence and required regression
  coverage. The index warning's relationship to this interrupted compaction
  has not been established. No upstream files or user storage were modified.
- **Resolution (2026-10-17):** liblockdc 0.20.0 moves the compaction copy
  buffer onto the heap. The packaged 0.19.0 archive reproduces the exact
  `lc_pouch_state_copy_file_span_to_fd_crc` crash under GDB; 0.20.0 passes
  `cai_cli_pouch_compaction` on static musl. This regression crosses the real
  compaction byte threshold, observes a published encrypted session snapshot,
  checks an 8 MiB live payload and credential readback after reopening, compares
  JSONL/Markdown exports byte for byte, and resumes the original conversation.
  Only the test executable shortens the worker's 30-minute wait; the real
  default-stack worker and packaged storage code run unchanged. Existing
  interrupted-writer, missing/empty/malformed-index and session tests also pass.
  The historical index warning's precise causal relationship remains unproven.

## BUG-012: Capture PTY shell output for conversation context

- **Priority:** normal; feature backlog.
- **Status:** superseded by BUG-015: terminal handoff output stays local.
  The original requirements and implementation record below are historical.
- **Required behavior:** `!sh` and `/shell` must run with a real PTY and retain
  the last 5,000 lines of terminal output by default. Discard older lines and
  make the retained-line limit configurable through CLI flags, environment and
  `config.ini`, with the usual precedence and documented defaults.
- **Shared controls:** use the existing shell-context controls for both ordinary
  `!COMMAND` and PTY shells: `--no-shell-context`, `[ui] shell_context`, and
  `/shell-context [on|off]`. After shell exit, share retained output through the
  existing shell-result context mechanism, without treating it as a user prompt
  or automatically starting an inference turn. Persist and replay that context
  without executing the shell again.
- **Capture boundary:** relay terminal output normally to the user while
  independently scrubbing the capture before it enters the retained buffer.
  Use internal code, not external tools. Remove ANSI/VT control sequences,
  colors and DEC controls while preserving real text. The parser must retain
  state across chunks, including split escape sequences and UTF-8 characters;
  cover CSI, OSC/DCS strings and their BEL/ST terminators, and C0/C1 controls
  where applicable. Define handling of carriage returns, backspaces, erasure
  and cursor redraws so removing escape bytes does not leak parameters or
  destroy readable text. Do not claim complete terminal emulation without
  evidence.
- **Bounds:** keep capture bounded as output arrives, without a hidden full
  transcript or temporary spool. A line limit alone does not bound an enormous
  unterminated line; specify a byte bound and truncation behavior as part of
  implementation. Clearly identify omitted output in the shared shell result.
- **Verification:** exercise real PTYs, output above the retained-line limit,
  configurable limits and precedence, final unterminated lines, oversized
  lines, Unicode, chunk-split control sequences, color/string controls and
  terminal redraws. Cover shell exit, failure, cancellation, terminal restore,
  context enabled/disabled, persistence/replay and ordinary `!COMMAND`
  regressions. Capture the subsequent model request to prove that retained
  text is shell context and contains no terminal escape sequences.
- **Implementation:** CAI internally relays a real PTY; libcai incrementally
  sanitizes a bounded logical-line tail for both pipe and PTY shell results.
  Defaults are 5,000 lines and 4 MiB of retained UTF-8, configurable through
  CLI/environment/INI and C/Lua runtime fields. It handles split UTF-8 and
  control sequences, CSI, OSC/DCS/SOS/PM/APC strings, C0/C1 controls, CR,
  backspace, and horizontal movement/erasure. Vertical/alternate-screen state
  is not emulated. Oversized lines discard UTF-8-aligned prefix chunks.
  Raw journal chunks remain independent of the sanitized model tail, as with
  ordinary bang commands; there is no complete in-memory raw transcript.
- **Evidence:** native sanitizer chunk-boundary/limit/Unicode tests and runtime
  journal/finish-failure/replay tests; actual CLI PTY input, Ctrl-C, resize
  forwarding, exit 137 versus SIGKILL, failed launch, private/shared results,
  configuration precedence, a 5,000-line tail beyond the old 32 KiB limit,
  encrypted reopen and subsequent model-request capture. A background job
  retaining the slave cannot hang return after shell exit. Lua exercises the
  same retention, context delivery and exact-resume APIs on both protocols.

## BUG-015: Terminal handoff transcripts prevent session replay

- **Priority:** high.
- **Status:** fixed; seven native/CLI checks pass, including real PTY and
  encrypted replay/continuation/export coverage.
- **Report:** playing Tetris inside `/shell` recorded terminal frames; the next
  startup failed with `string field 'data' exceeds configured max dynamic string
  bytes` when replaying a stored event.
- **Decision:** `/shell` and `!sh` are local terminal handoffs. Their terminal
  output must never enter the journal, model context or logs. This supersedes
  the PTY propagation requirements in BUG-012. Ordinary `!COMMAND` sharing and
  configurable retention remain available.
- **Recovery:** omit journal events that exceed lonejson's dynamic-string
  ceiling, warn with session ID and event sequence, and continue replay. Advance
  the journal sequence even if the final event is omitted, without modifying
  stored records. JSONL export still contains the original records. Other
  malformed records, identity errors and storage/transport failures remain
  errors. This does not remove oversized content already inside a checkpoint.
- **Verification:** boundary tests at and around the parser ceiling; encrypted
  journal recovery with oversized input and shell-result events; repeated idle
  resume, continuation and reopen; lossless JSONL export and sequence safety;
  real PTY output above the ceiling excluded from requests, exports and logs;
  ordinary pipe commands still shared; terminal handoff/job-control regressions.

## BUG-016: Opt-in terminal sharing must retain only the newest replayable tail

- **Status:** fixed; native/CLI/Lua checks pass. Both native capture checks
  also pass Valgrind with zero errors and zero leaked bytes.
- `/shellshare` and `!sh+` explicitly share an interactive shell's output for
  that invocation. `/shell` and `!sh` remain local.
- Both shared terminal handoffs and ordinary `!COMMAND` recordings consume
  output into a bounded rolling buffer. Older text is discarded as output
  arrives, including prefixes of long lines and partial oldest rows.
- Only the newest sanitized tail is saved and logged at finish. Completion
  metadata and the truncation notice count toward the default replay parser's
  dynamic-string ceiling. No complete raw transcript is journaled.
- Tests cover exact byte suffixes, UTF-8/control sequence boundaries, long
  commands and extreme exit/time metadata, failed tail/completion writes and
  retries, real PTYs, ordinary pipe output, exports, logs, replay and the Lua
  facade on both provider protocols.
