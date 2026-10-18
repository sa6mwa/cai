# Open bugs

This file tracks reported behavior, investigation evidence, and the verification
needed to close each issue. An upstream cause must be reproduced independently
before it is attributed to a dependency. Keep resolved entries with their fix
commit and verification evidence.

## BUG-001: Terminal resize corrupts the transcript and prompt position

- **Priority:** critical; investigate first.
- **Status:** a matching form of corruption is reproduced in softline 0.10.0;
  upstream repair remains open. The original XFCE incident is not traced.
- **Reported:** after resizing CAI, old command/source text remains inside patch
  rows or below the input prompt. The status bar and prompt can appear in the
  middle of output. Subsequent output can overwrite existing text.
  Environment: xfce4-terminal directly on X on local hardware, without SSH,
  tmux, or screen. The user cannot currently reproduce it consistently.
- **Required behavior:** transcript rows remain intact, feed output stays above
  the status/editor area, and resize updates both softline and libmdf geometry.
- **Evidence:** normal interactive assistant, reasoning, tool, patch, notice,
  and command output uses the softline output stream. `/shell` deliberately
  releases terminal ownership. CAI does not replace `SIGWINCH`; softline 0.10.0
  checks terminal dimensions with `TIOCGWINSZ` in its editor and feed paths.
- **Test in progress:** `tests/cli_terminal_resize_test.sh` uses a private tmux
  terminal to inspect actual displayed cells and scrollback. Its first case
  passes: long command output, shrink/grow while the provider is paused, then
  a large patch and final assistant response. This does **not** reproduce the
  reported corruption yet. Resizing during a producer callback and retained
  prompt transitions still need investigation.
- **Why existing coverage is insufficient:** the existing PTY peer records
  output bytes and a bounded input-row snapshot; it does not implement terminal
  reflow or verify the complete displayed transcript. It cannot prove that old
  source cells were cleared or that every row is positioned correctly.
- **Closure:** reproduce the failing sequence; identify CAI versus softline
  ownership; add a regression asserting actual screen cells, output ordering,
  and prompt placement. If upstream, provide a minimal softline-only reproducer,
  exact build/run steps, expected/actual captures, and the coverage gap. Do not
  patch another repository as part of this task.
- **Follow-up (2026-10-16):** a softline-only C reproducer now deterministically
  appends old status-message cells to a subsequent edit heading after a retained
  prompt, unfinished ASCII line, and settled 120x30 -> 60x18 resize. The observed
  line is `* Edited snake.cize reproduction`, instead of `* Edited snake.c`.
  This needs no libcai, libmdf, provider, tools or concurrent physical write.
  The report and exact reproduction steps are in
  [the upstream diagnostic](docs/softline-retained-resize-report.md).
- **Boundary:** softline documents approximate positioning after unobserved
  retained-frame resizes as an accepted limitation. CAI has synchronous output
  after `sl_next_prompt` returns, including command results and `/resume`
  replay. The dependency's current public API has no explicit input-owned
  resize-reconciliation call for that boundary. No hardcoded clear, artificial
  feed delay or transcript replay was added to conceal the corruption.
- **Coverage:** the existing actual-cell CAI test is now registered with CTest,
  alongside a real-runtime/renderer callback resize fixture and six softline-only
  boundary scenarios. Seven passing cases cover producer/input-owned resizes,
  width/height shrink/growth, partial lines and drafts. The reproducible failing
  retained-frame shrink case is registered but explicitly disabled pending
  upstream repair; running its documented script still fails and preserves
  screenshots. This is an open defect, not a passing resize fix.

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
  setters across timer ticks, mid-turn `/status`, successful and failed tool
  calls, provider failure, cancellation, and a subsequent recovery turn.
  The existing Responses and Chat Completions settings tests pass, including
  queued settings changes during a turn. Static musl terminal failure/recovery
  coverage also passes.

## BUG-007: Codex global instructions appear missing after session replay

- **Priority:** high.
- **Status:** resolved with explicit current-user AGENTS.md framing; C, CLI,
  Lua and live ChatGPT resume checks pass.
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
- **Status:** implemented; native/CLI/Lua and static-musl feature checks pass.
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
