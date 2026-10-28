# cai

`cai` is a C89/POSIX SDK for building OpenAI Responses and compatible Chat
Completions agents in C and Lua. It gives applications a handle-oriented
client, agents, sessions, streaming, tool execution, ChatGPT subscription auth,
MCP serving and client
connectivity, model/cost accounting, and release-packaged C/Lua integration
without making embedders hand-roll HTTP, WebSocket, JSON, or tool
orchestration.

The design target is systems software and embedded application runtimes such
as Vectis: stream-first data flow, explicit ownership, zero-defaultable config
structs, predictable error handling, allocator hooks, and clean integration
points for existing event loops, HTTP servers, loggers, and dependency stacks.

## Features

- OpenAI Responses API client with response creation, streaming, input-token
  counting, hosted-tool passthrough, raw JSON request controls, structured text
  formats, reasoning controls, metadata, truncation, service tier, prompt
  templates, and server-side continuation.
- OpenAI-compatible Chat Completions transport with incremental text/reasoning,
  function and custom tools, image input, local history, and client-side
  compaction; Berget is a built-in provider preset.
- Conversations API helpers and higher-level agent/session facades for
  multi-turn workflows, including client-side history replay for stateless
  providers such as OpenRouter.
- True streaming over HTTP/SSE and Responses WebSocket. OpenAI API-key and
  ChatGPT subscription-auth streaming use Responses WebSocket when appropriate,
  with a default HTTPS streaming fallback for WebSocket failures that happen
  before any output reaches caller sinks; OpenRouter and local/mock providers
  use HTTP/SSE.
- Local tool callbacks with streamed tool-call argument handling, tool output
  reporting, retryable tool errors, and configurable tool-round limits.
- Built-in tool presets for SearXNG search, reverse geocoding, todo/kanban,
  sandboxed command execution, file reading, and file listing.
- Streamable HTTP MCP handler for serving cai tools through an embedding-owned
  HTTP server, plus a Streamable HTTP MCP client for consuming remote tools,
  resources, prompts, and completions.
- ChatGPT subscription authentication with CAI-owned, Codex-wire-compatible
  `auth.json`, browser OAuth login helper, token persistence, and transparent one-shot
  refresh retry on 401/403.
- Usage accounting with per-client and per-session limits for input tokens,
  output tokens, total tokens, requests, and currency-specific spend estimates.
- Lua 5.5 binding and LuaRock packaging covering the same main client,
  session, streaming, tool, MCP, and ChatGPT auth workflows as the C API.
- Release archives with installed headers, static/shared `libcai`, CMake
  package metadata, pkg-config metadata, docs, source tarball, checksums,
  sanitizer-free release builds, and relative runpaths.
- Hardening gates for local unit tests, live integration tests, Lua tests,
  deterministic examples, AFL++ fuzzing, ASan/UBSan and Valgrind memory
  checks, and release artifact validation.
- Static musl Linux x86_64, aarch64 and armhf `cai` terminal agent packages
  with a `cai(1)` manual, README, and LICENSE in the SDK's relocatable install layout.

## Agent Runtimes and Smith

`cai_agent_runtime` is CAI's host-neutral agent-mode loop: it owns the worker,
serial tool dispatch, durable commits, and an event queue, while the host
owns input, rendering, and its application event loop. An event callback is
delivered only when the owner thread calls `cai_agent_runtime_pump`; a
poll-only runtime deliberately suppresses presentation events and exposes state
only. Hosts can use the borrowed wakeup descriptor with `poll`/`select`, copy
streamed text or provider-issued reasoning summaries into any renderer, and
call the explicit threadsafe input methods from an input thread.

The built-in `smith` preset is CAI's Codex-inspired coding-agent profile. It
uses the native `apply_patch`, sandboxed file/image tools, optional MCP and
image generation, durable goals, and one managed pipe/PTY terminal slot with only
one active command. `submit_interactive` follows Codex's StartOrSteer policy:
it starts an idle turn or queues interactive direction for an active one at
the next safe model/tool boundary. `submit_queued` queues an ordinary FIFO
turn for after the active turn completes. Core `libcai` does not parse slash
commands or render a UI: downstream applications map their own controls to
those APIs, review lifecycle calls, and runtime Markdown export.
`libcai_app` supplies the terminal application, including its slash commands;
the standalone `cai` executable is its embedding example.

Managed terminal commands preserve the executable `PATH` inherited when their
tools are registered, so toolchains installed outside the system directories
remain available. An unset `PATH` uses the system default; an explicitly empty
value stays empty. `HOME` and every `XDG_*` variable are preserved exactly,
including empty and unset values. Other host variables are filtered by default.

`--yolo` disables local tool confinement and command environment filtering;
`--no-yolo` restores the default restricted mode. Set `CAI_YOLO=true` or
`[agent] yolo = true` in config.ini for the same behavior. Precedence is CLI,
environment, configuration, then the built-in default `false`. Embedded apps
use their configured environment prefix (for example, `SMITH_YOLO`).
In the TUI, `/yolo` reports the current policy and `/yolo on` or `/yolo off`
changes it once active turns, subagents and terminal commands finish. Pending
changes remain editable in Softline and appear in `/status`. This does not
write configuration or save the policy in conversation history.

YOLO commands inherit the host environment and ordinary user filesystem
permissions; managed commands still force `PAGER`, `GIT_PAGER`, and `GH_PAGER`
to `cat`. Local read/list/patch/image tools may access paths outside the
workspace, and reviewers/subagents inherit the selected mode. Explicitly
disabled tools remain disabled; output retention, timeout and cancellation
mechanisms still apply. The legacy synchronous exec preset bypasses Bubblewrap
or sandbox-exec in YOLO mode. Smith's managed command runner has no OS sandbox in
either mode: restricted mode scopes file tools and working directories and
filters command environments. Startup warns about the effective policy and
`/status` reports it. `!COMMAND` and `/shell` already use ordinary user access.

At an idle runtime boundary, hosts can select the next-turn model with
`cai_agent_runtime_set_model` (or Lua `runtime:set_model`). For ChatGPT
subscription clients, `cai_client_list_models` retrieves Codex's authoritative
account catalog, including the opaque compaction compatibility hash. CAI uses
that metadata for switches: equal hashes preserve context, differing declared
hashes compact retained local history before the selection changes, and missing
metadata does not cause CAI to invent a compatibility decision. The selected
model, reasoning effort, and reasoning-summary mode are committed together.
Resume restores them unless explicit runtime configuration overrides the
corresponding fields.

Context reporting uses the effective provider window, rather than a compiled
API model maximum for a signed-in ChatGPT account. Before discovery it is
unknown; a commit retains the last known window for the selected model so
resume can report it without blocking startup on the catalog. Current catalog
metadata supersedes that saved value. Runtime metrics and compaction use the
same window. Lua exposes these metrics through `runtime:metrics()`, including
context/usage and active/completed turn timings, and accepts `resume_session_id`
for exact resume. `runtime:ids()` exposes `session_id`, the last retained
provider `response_id`, and a provider `conversation_id` when configured.
The C snapshot API is `cai_agent_runtime_get_ids`; it remains safe during a
turn and makes no provider requests. Lua clients select `api_protocol = "responses"` or
`"chat_completions"` explicitly when endpoint presets are insufficient.

Smith defaults to `gpt-5.6-terra` with medium reasoning and provider-directed
`auto` reasoning summaries. The C and Lua Smith terminal examples intentionally
use `gpt-5.6-luna` and CAI-owned ChatGPT subscription authentication to keep
interactive use and opt-in live acceptance economical. Run `make chatgpt-login`
once; CAI reads its own XDG state file (or `CAI_CHATGPT_AUTH_JSON`) and never
reads or refreshes Codex's auth state in those examples. The `cai` CLI has its
own auth default described below. See
[the Smith specification](docs/agent-smith-spec.md) and
[the examples guide](examples/README.md#smith-terminal) for the complete
runtime, review, storage, and presentation contracts.

## Cai terminal agent

The musl SDK presets build the terminal coding agent as a statically linked
Linux x86_64, aarch64 or armhf executable. The `cai` convenience preset selects
the same x86_64 musl producer. It adds pinned softline 0.15.0 for the prompt and
libmdf 0.17.0 for streamed Markdown rendering, and liblockdc 0.23.0 with
liblql 0.4.0 for durable storage. It uses libpslog 0.12.0 for logging.
The native dependency SDK is cpkt 0.6.1 from
[c89systems/cpkt](https://github.com/c89systems/cpkt). CAI consumes liblockdc's
core archive; its separate PostgreSQL provider is excluded. cpktmisc 0.2.0
supplies libyaml; cpktdb is not required. Dependency prefixes remain separate,
and imported CMake targets supply the complete static link dependencies.
`libcai` itself has no dependency on the CLI prompt, rendering, or storage libraries.
Softline and libmdf run on the UI thread. The runtime
worker executes model requests and tools, then wakes the UI thread to render
events.

The UI keeps one softline output session open and forwards each libmdf sink
fragment directly to it, with no simulated typing delay or timed coalescing.
Softline owns prompt geometry and terminal scrollback. Before further output
after a width or height change, the UI owner calls softline's explicit resize
reconciliation API, including after prompt submission and during session replay.
A required cursor reply may wait up to 100 ms at that boundary; feed writes
themselves never wait for cursor replies. The UI also updates libmdf's width
before feeding further Markdown after a resize.
Ctrl-V pastes X11 clipboard PNG/JPEG images as file paths in the editor.
Images are saved as private files under `$XDG_CACHE_HOME/cai/clipboard/`,
falling back to `~/.cache/cai/clipboard/`, and remain after exit. They can be
removed when no longer needed. This requires `DISPLAY`, including forwarded X11
displays; ordinary bracketed text paste continues to work separately.
`view_image` can read images in both the workspace and this clipboard
directory; other cache files remain outside its access. Symlink escapes and
hardlinked image files are rejected.

Slash commands echo as plain `$ /command` input rows. The reasoning title is
rendered through libmdf with ANSI disabled; Softline owns the gray `! ` line.

`/m`, `/r` and `/t` alias `/model`, `/reasoning` and `/theme`, including their
arguments and delivery boundaries. `/help` lists the enabled Softline shortcuts:
cut/yank and yank rotation, draft stashing/discard, undo, word movement, multiline paste, history search,
queue editing/approval and draft-preserving shell handoff. Ctrl-Y yanks from the
in-process kill ring; use the terminal's paste shortcut for clipboard text and
Ctrl-V for X11 clipboard images.

**Alt-K** stashes the whole draft and clears it. **Alt-J** appends that saved
draft after all current text, regardless of the cursor position, retaining the
saved copy for reuse. A new nonempty stash replaces the previous one; stashing
an empty draft leaves it unchanged. This run-local stash is separate from the
kill ring and survives prompt submissions and draft-preserving shell handoff.
It is not persisted to prompt history merely by stashing.

**Alt-N** clears an ordinary draft or permanently discards the queued/approval
entry being edited. Undo recovers its text as a fresh draft, without restoring
queue identity or approval. Submitting an empty queued edit also deletes the
entry. Escape cancels editing and restores the original entry instead. An
approval can take focus only at an empty ordinary prompt: stash a draft with
Alt-K first, then use Alt-A and Alt-N to reject the message. With a nonempty
ordinary draft, Alt-N clears that draft and leaves pending approvals alone.

Softline's queued-turns profile handles conversation input: Enter starts an
idle turn or queues the next turn while busy. Alt-Enter queues steering, or
marks the newest pending entry as steering when the editor is empty. Alt-E
edits pending input. Steering entries remain in Softline until the runtime's next
response/tool boundary. The application does not keep a second UI queue.
Read-only local commands such as `/goal`, `/status`, `/model` and `/tier list`
use the same Softline queue but can run between feed writes, even while
inference is pending. Approval and active edits remain under Softline's control;
inspecting state does not restart FIFO delivery stopped by Escape. Model,
reasoning, tier, goal and theme changes remain editable until the next safe
response/tool boundary with either Enter or Alt-Enter. Commands execute locally
and never become model steering text. `/quit` and `/exit` use the native queue
but can shut down an active turn; Alt-Q saves the draft and exits directly.
All typed slash and `!COMMAND` inputs enter Softline's visible, editable queue,
including while idle. Its native policy keeps approval, execution readiness and
Escape's stopped backlog separate. A fresh `/goal resume` can run after Escape
without releasing old stopped entries. Editing an old stopped entry retains
its stop; Alt-Enter explicitly promotes that entry. Whole-turn commands such as
`/review`, `/compact`, `/shell` and `/yolo on|off` become `Q` and wait until idle,
even when entered as steering. Local `!COMMAND` results enter model context at
the next safe boundary. The complete command/boundary inventory is in
[the command policy audit](docs/interactive-command-audit.md).
A steering prompt appears in the
conversation feed only when the runtime inserts it after the current response
or tool round. It never interrupts streamed assistant text. Resume uses those
same delivery boundaries. Foreground reviewers and other subagents receive
steering by default. `--no-subagent-steering` leaves it queued for the parent
instead; `CAI_SUBAGENT_STEERING` and `[agent] subagent_steering` configure the
same policy. The C runtime field is `disable_subagent_steering`, also accepted
by Lua runtime constructors. SDK hosts that collect input on the owner thread
can set `synchronize_input_boundaries` and handle `INPUT_BOUNDARY` in their
event callback; the worker resumes after that callback returns. The setting is
inherited by children and requires an event callback. Lua exports the same
setting and `AGENT_EVENT_INPUT_BOUNDARY` constant.

Interactive prompt recall uses Softline's custom backend hooks and encrypted
`prompts.<session-id>` collections in the configured sessions namespace, beside
the conversation journal. It shares the session store's Pouch client and key;
there are no plaintext `.history` files or separate history-directory settings.
Recall loads only the newest 200 entries by default, oldest first, through a
bounded collection range. Configure the recall window with
`--prompt-history-limit`, `CAI_PROMPT_HISTORY_LIMIT`, or `[ui] prompt_history_limit`
(also accepted through C/Lua application settings). Zero disables recall and
recording. This window does not delete stored entries or set their age expiry.
`/history` shows the current window; `/history COUNT` changes it for this app
invocation. Stored-entry retention is deferred until liblockdc supports native
collection `max_entries`; the command explicitly reports that boundary.
Accepted prompts and local commands append immediately, including steering; replay and Ctrl-R
selection do not append. `/new` starts fresh recall; `/resume` restores the
selected conversation's recall. Each entry appends synchronously without
rewriting earlier entries, using the store's configured durability and
compression. Recall is separate from model context and conversation export;
deleting a conversation drops its prompt collection too. Temporary chats use
their private session store for recall. Existing file-backed recall is not
imported. Ctrl-R Enter selects a match for editing; Enter again sends.
**Alt-Q** saves the active draft as the newest prompt-history entry and exits
without submitting it. After restarting in the same workspace, press Up to
recall it. Empty drafts add nothing; an identical last entry is not duplicated.
If saving fails, CAI stays open with the draft intact and reports the error.

Conversation journals have a default **30-day inactivity retention**, enforced
by liblockdc on the whole `events.<session-id>` collection and its session
metadata. Every successful append renews the deadline; individual journal
entries have no separate expiry. Configure the default through
`--conversation-retention-days`, `CAI_CONVERSATION_RETENTION_DAYS`, or
`[storage] conversation_retention_days` (also accepted through C/Lua settings).
Zero disables expiry. `/retention` shows the active conversation's policy;
`/retention DAYS` persists its override, and `/retention 0` or `/retention off`
keeps it forever. Changing it waits for a whole-turn boundary through Softline.
Expiry is eligibility for asynchronous SDK maintenance, not exact-time
deletion. Reads do not renew it. After SDK cleanup, lists omit unavailable
conversations and auto-resume selects an available conversation or starts fresh.
Explicitly resuming a removed journal reports that it is unavailable.
Prompt-history aging/pruning remains deferred; conversation retention does not
silently expire individual prompt records, exports, or logs.

`-D` / `--delete` lists numbered conversations eligible for deletion in the
current directory. `-D ID` or `-D NUMBER` deletes that conversation, its child
and review collections, metadata, managed exports, prompt history, session
logs and delivered inbox messages. Numbers refer to the deletion list; exact
numeric IDs take precedence.
Open conversations are omitted and cannot be deleted until closed. Filesystem
locks on the shared Pouch root release automatically on process exit/crash.
Shared authentication, boards, mailboxes and workspace files remain separate.
Deletion is logical; Pouch reclaims dropped collection storage during maintenance.

Exact `!sh` and `/shell` commands hand the real terminal to `$SHELL -i`
(`/bin/sh -i` when unset or empty) between turns. They require terminal input
and output. While busy, typed shell commands remain editable in Softline's
queue until a whole-turn boundary; Alt-Enter cannot interrupt a turn to hand
off the terminal. The shell runs with normal user permissions in the workspace.
On exit, CAI restores terminal ownership, prompt settings and current geometry,
including after a screen clear or resize. The shell uses the terminal directly,
with native window changes and foreground-job control. Terminal output stays
local: it is neither journaled, logged nor sent to the model, including with
`--shell-context` enabled. Logs retain only shell start/exit metadata.
A failed launch or nonzero shell exit reports details and returns to CAI.

**Alt-S** opens that same local shell between turns while retaining the current
draft, cursor and native editing state, including undo. Exit the shell to resume
editing; the shortcut does not submit the draft or add it to prompt history.
The command forms return to an empty prompt. **Ctrl-L** refreshes terminal layout
without discarding the draft, queues or transcript.

`/shellshare` and `!sh+` explicitly share the shell's newest sanitized output
when it exits. These shared variants use a PTY relay for capture. This applies
to that invocation even if ordinary bang-command sharing is disabled.
The same rolling-tail storage described below applies;
older output is discarded as new text arrives. A shell that has not finished
does not save its captured output.

Other `!COMMAND` inputs run captured commands as described below.

Enter `!COMMAND` to run your shell (`$SHELL`, falling back to `/bin/sh`) with
normal user permissions in the workspace, outside the agent sandbox. Each
invocation uses `-c`, pipes and closed stdin, without a PTY or login profiles.
Output appears literally, without Markdown parsing or preview truncation.
UTF-8, newlines and tabs are preserved; other controls and invalid bytes are
shown as `\xNN`. Stdout and stderr share one pipe. Ctrl-C/Escape cancels the
shell first; quit also cancels it. Commands have a one-hour timeout, and their
background descendants are terminated at completion. Ordinary prompts entered
while a shell runs stay in Softline's visible, editable queue until its result
has been recorded; they are not moved into libcai's internal turn queue.

Commands are journaled at start; only the newest sanitized output is saved at
completion. Replay displays that tail without executing commands. By default,
a completed result becomes tagged
user-role context, including exit code and elapsed time, at the next safe
model/tool boundary. It does not start inference, steer the agent or advance a
goal turn. A rolling buffer retains up to 5,000 logical lines and the configured
byte ceiling (4 MiB by default), further limited by the replay parser's
dynamic-string bound (currently 128 KiB), including completion metadata and a
truncation notice. The newest text that fits is kept; earlier text falls out
of the buffer. The saved tail, logged output and model copy use that same text.
`--shell-context-max-lines N` and `--shell-context-max-bytes N` override
`CAI_SHELL_CONTEXT_MAX_LINES` / `CAI_SHELL_CONTEXT_MAX_BYTES`, which override
`[runtime] shell_context_max_lines` / `shell_context_max_bytes` in `config.ini`.
Zero in SDK/INI selects the default; CLI values must be positive (bytes >=4).
An oversized line keeps its newest UTF-8-aligned suffix.
ANSI/VT colors, CSI and OSC/DCS/control strings are removed before retention;
CR, backspace and horizontal cursor/erase controls update the current logical
line. Incomplete controls are dropped, invalid UTF-8 is replaced, and split
controls/UTF-8 are handled across reads. This is a text transcript, not a full
screen or alternate-screen emulator. JSONL contains only the retained tail.
Markdown export uses
the existing libcai exporter of retained model context.

`--no-shell-context`, `CAI_SHELL_CONTEXT=false`, `[ui] shell_context=false`,
or `/shell-context off` keeps future `!COMMAND` results out of
model context while still executing, displaying and saving them.
`/shell-context on` enables sharing; `/shell-context` shows the setting.
These controls never enable recording or sharing for `/shell` or `!sh`;
`/shellshare` and `!sh+` explicitly opt in for one invocation.
The setting is captured when each command starts. Earlier shared results stay
in context. Unshared results replay locally and remain in JSONL exports.

SDK hosts provide execution and call the owner-thread APIs
`cai_agent_runtime_user_shell_begin(runtime, command, share, &error)`,
`cai_agent_runtime_user_shell_write(runtime, bytes, length, &error)` for each
chunk, and `cai_agent_runtime_user_shell_finish(runtime, exit_code, duration_ms,
&error)`. Writes update the bounded rolling capture; they do not journal or log
raw chunks. Finish saves the sanitized tail, then its completion. A failed
finish can be retried; once the tail is saved, further writes are rejected.
Newly submitted turns wait
for finish; an existing model turn continues. Closing an unfinished recording
discards any unsaved capture without scheduling execution or shared context on
recovery. A tail already saved by an interrupted finish remains replayable.
Lua exposes `runtime:user_shell_begin(command, share)` (sharing
defaults to true), `runtime:user_shell_write(bytes)` (binary strings accepted),
and `runtime:user_shell_finish(exit_code, duration_ms)`. libcai never executes
these explicitly user-run commands itself.
Both C runtime configuration and Lua runtime option tables accept
`shell_context_max_lines` and `shell_context_max_bytes` with the defaults above.
The shared INI parser exposes them under `[runtime]` to C and Lua hosts.

### Trim conversation history

`/trim` lists the newest 20 complete history entries, newest first. Row `0`
means keep the current history; row `1` identifies the newest entry, row `2`
the preceding one. `/trim N` removes exactly the newest N entries. `/trim 0`
does nothing. `/trim list N` changes the listing length for that command.
Summaries use the assistant response, falling back to the prompt or shell
command, collapse whitespace, and fit the terminal width through libmdf.

A turn includes its reasoning, steering, tool calls and tool results. A
standalone recorded shell execution is one entry; an execution started during
a model turn belongs to that turn. Compacted context is one indivisible entry,
including any tail retained by the compactor. It is never expanded into the
original turns. Boundaries are persisted as incremental journal metadata;
steering records remain part of their turn.

Listing and removal require an idle runtime with no queued work, open shell,
pending settings, or review handoff. Removal commits the retained context
and journal visibility together. A commit failure restores the live history.
Restart, subsequent provider requests, and conversation exports exclude trimmed
entries. Older journal records and diagnostic logs are not erased. Changes
to files, tool stores, selected settings, goals, and incurred usage are not undone.
CAI clears provider continuation handles so discarded context cannot return via
an earlier response or conversation ID.

The C API exposes `cai_agent_runtime_visit_history(runtime, limit, visitor,
context, &error)` and `cai_agent_runtime_trim_history(runtime, count, &error)`.
Both are owner-thread operations. Visitor views are borrowed only during the
callback; callbacks must not mutate the runtime. A zero listing limit means 20;
an excessive trim count fails without removing entries. Hosts can use the
runtime configuration's `replay_callback` for retained journal replay, and
`cai_agent_runtime_history_event_visible` when exporting a stable journal.

Lua exposes the same behavior:

```lua
local entries = assert(runtime:history()) -- optional listing limit
for _, entry in ipairs(entries) do
  print(entry.remove_count, entry.kind, entry.summary)
end
assert(runtime:trim_history(1)) -- remove the newest whole entry
```

`kind` is `turn`, `shell`, or `compaction`. History and trimming do not issue
provider requests or execute tools.

### Manual context compaction

`/compact` starts a standalone compaction task between turns. While busy it
stays in Softline's native editable queue. Alt-Enter becomes `Q` and cannot
waive the whole-turn hold or steer the model. Escape cancels the task. Progress
appears in the gray status line and completion in the Markdown feed.
Automatic worker compaction also displays `Compacting context` in that status
line until completion, failure or cancellation.

The worker uses the same compactor as automatic compaction: streamed Responses
V2 `compaction_trigger` for Responses providers, matching Codex's manual task,
or client-side summarization for Chat Completions. It does not submit a user
prompt, execute tools or generate a follow-up answer. A successful replacement
is committed before completion, retains instruction files for subsequent
requests, and appears as one indivisible history entry. Failure or cancellation
before the replacement commits preserves the prior history. An unfinished
manual request is not automatically retried after a crash.

Native hosts call `cai_agent_runtime_can_compact(runtime, &ready, &error)`
and `cai_agent_runtime_compact(runtime, &error)` on the owner thread. The latter
starts asynchronous worker work; pump the usual run and compaction lifecycle
events until completion, failure or cancellation. `RUN_STARTED` has no prompt
payload for this task. Lua exposes `runtime:can_compact()` and
`runtime:compact()` with the same behavior. These additions use the opaque
runtime; no receiver layout or ABI changes are needed.

### Service tiers

`/tier` cycles available tiers; `/tier list` displays choices and the selected
and pending tier. `/tier NAME` selects one. `/fast` toggles Fast (`priority`, or
the provider's advertised `fast` ID) and explicit normal routing (`default`).
Changes apply at the next safe model/tool boundary and persist with the session.
Enter and Alt-Enter both use this next-request boundary for tier changes, even
during an active turn. The command remains editable in Softline until applied;
the request already in flight keeps its original tier. Routing commands are
independent of queued conversation prompts and do not submit model input.
`--service-tier NAME`, `CAI_SERVICE_TIER`, and `[agent] service_tier`
provide startup overrides. An unset value restores saved routing, or leaves
new-session routing to the provider. CAI never opts into a paid tier from a
catalog default automatically.
Explicit normal routing omits the tier on ChatGPT requests, matching Codex;
the public OpenAI API receives `"service_tier":"default"`.

ChatGPT's signed-in model catalog supplies `service_tiers` and
`default_service_tier`. OpenAI's published API request options are shown when
model metadata is absent; the provider still validates account/model access.
For other providers, explicit tier IDs can be sent when metadata is missing;
cycling and enabling Fast need known choices. A fallback models.json entry may
declare `"service_tiers":["priority","flex"]` and an optional
`"default_service_tier":"priority"`. Live metadata takes precedence.

The C runtime settings API accepts `CAI_AGENT_RUNTIME_SETTING_SERVICE_TIER` and
`settings.service_tier`; the same string is available in Lua configuration,
`runtime:settings()`, and `runtime:update_settings({service_tier="priority"})`.
Low-level agent configuration also accepts `service_tier`, serialized by both
Responses and Chat Completions, including tool continuations. Review and other
child agents inherit the selected tier.

### Build CAI

```sh
scripts/cpkt-toolchains.sh ensure x86_64-linux-gnu
scripts/cpkt-toolchains.sh ensure x86_64-linux-musl
cmake --preset libcai
cmake --build --preset libcai
cmake --preset cai
cmake --build --preset cai
cmake --build --preset cai --target cai_package_cli
build/x86_64-linux-musl-release/cai --help
build/x86_64-linux-musl-release/cai -N -C /path/to/project
```

For native development, use `make build-cli` and `make test-cli` (the
`debug-cai` preset). The `cai` convenience preset uses the exact configuration
and build directory of `x86_64-linux-musl-release`, retaining both static/shared
SDK libraries. For ARM, use `scripts/build.sh aarch64-linux-musl-release` or
`scripts/build.sh armhf-linux-musl-release`; each writes `cai` beside its SDK
libraries in `build/<target>-release/`. No separate `build/cai/` producer is used.
`ctest --preset cai` checks terminal feeds without cursor replies, failed-output
propagation, recovery after provider errors, truncated and malformed streams,
Ctrl-C/Escape cancellation, and clipboard failures. Cancellation keeps the
interactive session open so another turn can start. A confirmed stop produces
a warning notice (`Turn stopped.`, or `Review stopped.` for `/review`);
Escape at an idle prompt does not claim that a turn stopped. Responses stream
errors preserve the provider message, code, and request ID in both libcai streaming
APIs and CAI diagnostics; diagnostic messages are bounded to 1023 bytes.
Responses provider failures, including cybersecurity policy errors, produce an error-level
`cai.response.provider_error` log with the provider code, message, HTTP status,
and request ID when supplied. Interactive sessions write this to the session
log and show an error notice; non-interactive sessions log to stderr.
For `cyber_policy` or `cyber_notice` error codes, the runtime sends one automatic
explanation-only prompt in the same conversation, using the selected model.
Only the enabled workspace-scoped `read_file` and
`list_files` tools are available, with at most eight sequential tool rounds;
execution, edits, MCP, hosted tools and goal actions remain unavailable.
It asks for a bug-report summary of the preceding work, possible triggers,
and a tentative assessment
of the visible security context, separating facts from hypotheses. The model
cannot know the provider's internal trigger or certify a false positive.
The prompt and answer are recorded normally and can be exported. A failed
diagnostic is logged without another diagnostic attempt. The original error
still fails the run, including non-interactive invocations, and no original
task or goal is automatically resumed. Goal state remains unchanged, and an
explicit next user prompt runs normally with the user's configured tools.
The diagnostic's restrictions do not carry into that turn. The public runtime
event
`CAI_AGENT_EVENT_POLICY_DIAGNOSTIC_STARTED`
(`cai.AGENT_EVENT_POLICY_DIAGNOSTIC_STARTED` in Lua) identifies the generated
prompt before its response streams.

The default `chatgpt` provider imports `~/.codex/auth.json` first when present,
otherwise it loads credentials from cai's lockd store. `--auth-json` selects
another import source. Unchanged source files preserve tokens refreshed in
cai's store; refresh never writes back to the source file. If neither source
nor stored credentials exist, `cai --login` opens a browser login, stores auth
in the `cai.auth` namespace, and exits. `-l` means `--list`, not login. The
`openai` provider uses `OPENAI_API_KEY`, `openrouter` uses
`OPENROUTER_API_KEY`, `berget` uses `BERGET_API_KEY`, and `custom` requires
`--endpoint` and `--model` with `CAI_API_KEY` by default or a variable
selected by `--api-key-env`.
`--provider`/`-p` selects a provider. ChatGPT and OpenAI start with
`gpt-6-luna`; OpenRouter starts with `openai/gpt-5.6-luna`. Berget starts with
`zai-org/GLM-5.3-Flash`. It supports vision, streaming, and function calls for
cai's local and MCP tools. Berget does not offer provider-hosted image
generation. All providers start with medium reasoning;
`-m`/`--model` and `-r`/`--reasoning-effort` override those defaults.
`cai` loads optional model metadata from `$XDG_CONFIG_HOME/cai/models.json`
(or `~/.config/cai/models.json`); `--model-metadata FILE` overrides it.
libcai accepts the same JSON through `cai_client_config.model_metadata_json`.
Berget's live model catalog includes every model it advertises, with model
type, capabilities, and EUR prices. libcai includes a maintained table of
model-card context limits for Berget models because its API omits them;
fallback JSON can override those limits. See
[model metadata](docs/model-metadata.md) for the schema and precedence.
`--help` lists the other
user-facing model, review, instructions, skills, image, terminal, and session
options. Interactive startup shows `Welcome to cai. Send /help for usage.` in
the status line, including after replaying a resumed conversation. `/help`
renders Markdown tables of slash commands and prompt keys through libmdf;
it is handled locally and does not send a model request. Cai requests concise
provider reasoning summaries by default; the
`--reasoning-summary` flag overrides that choice. Provider-issued summaries
put their first nonblank line in Softline's `!` message bar with the active
turn's elapsed time. Libmdf renders that Markdown heading as plain text because
Softline themes the message and accepts no ANSI styling. Single-line summaries
do not enter the feed. Any remaining body streams through libmdf as Markdown
blockquotes, without a separate reasoning label or injected `?` row prefixes.
Quote markers and body styling come from the active libmdf theme. Before a
summary arrives, the bar says
`Working` with elapsed time. At completion it shows `Worked for 13m 32s -
252243`, where the final six digits are the local day, hour, and minute.
The live timer advances while the turn is active and stops when it settles.

Theme selection belongs to `libcai_app`, including its C and Lua embedders.
`cai --list-themes` lists softline themes, their default libmdf pairings, and
all available libmdf themes without opening a session. `--theme` / `-t` accepts
an alias or an explicit `SOFTLINE:LIBMDF` pair:

| Alias | Pair |
| --- | --- |
| `default` | `default:default` |
| `synthwave` | `synthwave:synthwave-84` |
| `plain` | `plain:ascii` |
| `accent` | `accent:night-owl` |
| `dracula` | `dracula:dracula` |
| `gruvbox` | `gruvbox:gruvbox` |
| `monochrome` | `monochrome:horizon` |
| `monogreen` | `monogreen:everforest` |
| `outrun` | `outrun:outrun-electric` |
| `riced` | `riced:catppuccin-mocha` |

For example, `cai -t synthwave` selects its pairing; `cai -t riced:iosvkem`
mixes the two libraries' themes. Names are case-insensitive. The startup default
is `default:default`. Set `CAI_THEME` or `[ui] theme = "synthwave"` in
`config.ini`; argv overrides both. Embedders use the same `ui.theme` setting,
and their configured environment prefix replaces `CAI_`.
In the TUI, `/theme` shows the current pair, all softline pairings, and all
libmdf themes. `/theme synthwave` or `/theme riced:iosvkem` changes future
output at the next safe response/tool boundary with either submission key.
The selection survives `/new` and `/resume` within
the app invocation; it does not rewrite `config.ini` or session history.
`ascii` is a libmdf output mode, not a theme: it disables Markdown ANSI styling
while retaining UTF-8 text. `NO_COLOR` selects the plain prompt and disables
Markdown styling regardless of the selected theme. The gray reasoning message
remains plain text styled by softline, and pslog retains its separate `LOG_`
configuration. Host-supplied raw banners are printed verbatim.

Review calls print a start receipt before preparation, then a completion or
failure receipt. Both model-invoked reviews and `/review` show standalone
**Review started** and **Review finished** Markdown titles, including when
colors are disabled. Review output uses the next libmdf theme by default and restores
the parent theme when it ends. These offsets affect Markdown only:

| Context | INI setting | Flag and TUI command | Environment | Default |
| --- | --- | --- | --- | --- |
| Parent | `[ui] theme_offset` | `--theme-offset N`, `/theme-offset [N]` | `CAI_THEME_OFFSET` | 0 |
| Ordinary child | `[subagent] theme_offset` | `--subagent-theme-offset N`, `/subagent-theme-offset [N]` | `CAI_SUBAGENT_THEME_OFFSET` | 0 |
| Reviewer | `[review] theme_offset` | `--review-theme-offset N`, `/review-theme-offset [N]` | `CAI_REVIEW_THEME_OFFSET` | 1 |

Child offsets are relative to the effective parent theme. Signed offsets wrap
through libmdf's listed themes, excluding `boring`; `-300` and `1234` are valid.
Zero keeps the selected theme. `/theme` also shows offsets and the active
libmdf theme. Offset changes stay editable in Softline until the next safe
response/tool boundary. Softline's theme, ASCII output and `NO_COLOR` are
preserved, including during replay. Settings and slash-command changes apply
to this invocation without rewriting history or configuration.

`cai --temp` starts a new conversation in a unique
`cache_dir/temp/chat-XXXXXX/workspace` directory. Its sibling `private/`
contains the encrypted conversation pouch and key, prompt history, clipboard,
exports, and app-owned logs. The normal global or explicit `--config` file is
loaded unchanged; path isolation is applied only in memory. Relative CLI input
paths (instructions, skills, model metadata, authentication, client PEM, and
Chrome socket) resolve against the original workspace selected by `-C` before
entering the temporary directory. INI paths remain relative to their config
file. On normal teardown, workers and softline close first, and private
history/state is deleted. The
plain terminal question `Remove temporary workspace …? [y/N]` controls whether
the workspace files are also deleted. No, empty input, EOF, interruption, host
STOP, and non-interactive runs preserve the workspace and report its location.
`--yes` / `--force` are equivalent and delete it without asking, including on
failure. Crashes can leave the cache directory behind. `-T` still selects review
output type. `[ui] temporary` / `CAI_TEMP` also select this mode; `--no-temp`
overrides them. A supplied logger or explicit `LOG_OUTPUT` keeps its own sink.

Named stores separate conversation, authentication, and messaging backends:

```ini
[store.default]
## Empty endpoint derives state_dir/pouch; key_file defaults to state_dir/pouch.key.
endpoint = ""

[store.team]
endpoint = "pouch:///srv/agents/pouch"
key_file = "/srv/agents/pouch.key"

[storage]
sessions_store = default
auth_store = default
messaging_store = team
```

All role selections default to `default`. The matching flags are
`--sessions-store`, `--auth-store`, and `--messaging-store`; environment aliases
are `CAI_SESSIONS_STORE`, `CAI_AUTH_STORE`, and `CAI_MESSAGING_STORE`.
`--lockd` / `CAI_LOCKD` and `--pouch-key-file` / `CAI_POUCH_KEY_FILE` override
the default store. Each named pouch store requires its own explicit `key_file`;
new roots generate it, while existing roots require their existing key.
HTTP(S) stores accept `client_pem` and do not use a local pouch key.
INI endpoint/key/TLS settings live under `store.NAME` (`endpoint`, `key_file`,
`client_pem`), replacing `storage.lockd`, `paths.pouch_key_file`, and
`paths.lockd_client_pem`. Existing configuration files are never rewritten.

Store handles remain open for the app run, including turns, `/new`, `/resume`,
messaging toggles, and nested runtime contexts. Pouch roots are identified by
their filesystem identity: aliases share one process client, references keep
it alive, and the final owning context closes it. Conflicting keys or endpoint
options for the same open root are rejected. Reconnection remains liblockdc's
responsibility. `--temp` substitutes only the conversation store; configured
auth and messaging stores remain available, including communication with normal
runs. Its kanban data stays with the private conversation store.

Conversation storage uses an [ordered incremental journal](docs/journal.md),
with zlib compression enabled by default for Pouch. Set
`[storage] disable_compression = true`, `CAI_DISABLE_COMPRESSION=true`, or
`--disable-compression` to disable it. The mode must match an existing root.
Normal durable boundaries append changes rather than full conversation copies.

Berget uses its OpenAI-compatible Chat Completions endpoint. The CLI streams
text, image URLs, and function tools, including `apply_patch` and MCP tools,
through libcai's normal agent and session runtime. It resumes conversations
by sending local history as chat messages. The default Berget model accepts
image input. Hosted image generation is disabled because Berget does not
provide that API; file IDs and arbitrary file attachments return an explicit
error. Berget API keys and session content are kept in the same
locations as other `cai` providers; the key is read from `BERGET_API_KEY`.
Chat Completions sends tool-returned images in a labelled user message after
all tool results from the assistant reply. The tool messages carry text
acknowledgements; image URLs and detail settings remain attached to the images.
GLM may emit reasoning separately or inside a tagged `<think>` section; cai
keeps tagged thinking out of the final answer and streams plain answer text.
Berget does not provide a concise reasoning summary for that model. A live
Berget compaction test runs a real function tool, forces client-side compaction,
then checks that the next turn recalls the tool's result without executing it
again. A separate vision test asks the live model to identify a deterministic
red PNG:

```sh
CAI_RUN_BERGET_LIVE_TEST=1 BERGET_API_KEY=... \
  ctest --test-dir build/debug -R 'cai_tests.berget_live_' --output-on-failure
```

Chat Completions refusal deltas use the visible output sink and delta callback,
and remain typed refusal content in completed message items and saved history.
Buffered responses expose that text through `response->refusal(response)`.

Deterministic local Chat Completions tests cover HTTP authentication, quota,
and server failures; errors inside a successful HTTP stream; malformed and
truncated streams; invalid completion order; callback failures; connection
refusal, disconnect, timeout, and cancellation. They also verify overflow
variants, bounded recovery, intact history after failed tool streams, and
parallel function/custom tools across session export and restore. Run them with
`ctest --test-dir build/debug -R 'cai_tests.chat_' --output-on-failure`. The
fixtures exercise both the Berget preset with a local endpoint and generic
Chat Completions clients. A separate opt-in canary discovers every currently
advertised streaming text model and tests streamed text, JSON/schema output,
function tools followed by forced compaction and continuation, and image input,
according to each model's advertised capabilities:

```sh
CAI_RUN_BERGET_LIVE_CANARIES=1 BERGET_API_KEY=... \
  ctest --test-dir build/debug -R '^cai_tests.berget_live_canaries$' --output-on-failure
```

Set `CAI_BERGET_CANARY_MODEL` to a model ID to restrict this gate to one model.
Live tests require credentials and spend tokens; they make no requests unless
explicitly enabled.

Smith runtimes automatically retry the exact provider error
`server_is_overloaded` ("Selected model is at capacity"). The first retry waits
30 seconds, then 60, 120, and 240 seconds; subsequent waits stay at 240 seconds.
There are at most ten retries per turn, in addition to the original attempt.
The active turn remains responsive to Escape, host cancellation and shutdown.
The status message and logs say when the next retry will run; exhaustion reports
the provider error and stops the turn. Completed tool calls are not repeated.
The pending request is reused without adding a user prompt such as "Proceed."
Incomplete attempt captures are discarded; any partial text already displayed
can appear again when the model resamples. Quota, rate-limit, policy and other
errors do not trigger this capacity policy.

Disable it with `--no-capacity-retry`, `CAI_CAPACITY_RETRY=false`, or
`[agent] capacity_retry = false`. C/Lua runtime config accepts
`disable_capacity_retry = true`; libcai_app accepts the normal agent setting.
The same policy applies to child runtimes and both wire protocols. Low-level
standalone client requests and explicit compaction endpoints do not use it.
With transcript recording, failed attempts and retry notices are execution
records, never model input. A new turn resets the retry allowance.

Separately, Chat Completions requests retry transient HTTP 429, 502, and 503 failures and
connection failures before response headers, with at most three attempts.
This includes failures while a large request is still uploading; stopping its
producer does not suppress a transport retry. Input serialization failures
remain errors and are not retried.
Retries stop once model activity or usage is received. Authentication, invalid
requests, permanent quota errors, capacity errors (handled by the runtime above), malformed streams, callback failures, and
partial responses are not retried. A retry after a completed tool round resends
the tool results without executing the tools again. Backoff starts at 500 ms,
adds jitter, and doubles up to 30 seconds. Valid `Retry-After` seconds or HTTP
dates set a minimum wait; values above the cap return the original error.
Cancellation interrupts the wait promptly. `timeout_ms` applies to each attempt.
The C client config and Lua `cai.open` accept `chat_max_attempts` (1 disables
retries, maximum 16), `chat_retry_initial_delay_ms`, and
`chat_retry_max_delay_ms` (maximum 60000). Zero selects each field's default;
the effective initial delay must not exceed the maximum delay.

Configure the status order with `--status-fields FIELDS`, `CAI_STATUS_FIELDS`,
or `[ui] status_fields` in `config.ini` (also available through libcai_app's
C/Lua settings). The default is `model:ctx:dir:branch:weekly:crd:yolo:goal`.
The Git branch appears after the directory and is omitted when unavailable,
including outside Git repositories or with detached HEAD. Discovery reads Git
metadata in-process, including ancestor repositories and linked worktrees;
it does not launch Git. Labels are literal, safely truncated UTF-8 with terminal
and direction/format controls rejected or escaped. Discovery runs at startup,
then at tool/turn or user-shell boundaries with a 60-second debounce. There is
no idle polling; explicit `/status` refreshes immediately. `default` expands that list:
`--status-fields provider:default` prepends the provider. Aliases are `context` for
`ctx`, `directory`/`folder`/`workspace` for `dir`, `w` for `weekly`, and
`credits` for `crd`. Names are lowercase; duplicates keep their first position.
Unavailable values are omitted. `weekly` includes any short quota window,
or the API cost estimate for API-key providers. `YOLO` appears only while the
effective mode is enabled, after credits in the default order.

`cai --dump-config [FILE]` writes the same documented, commented defaults
template generated at startup. Omit FILE to write to stdout. An existing
output file is overwritten; its parent directory must exist. This utility
does not load the selected config, copy secrets or explicit settings, create
the normal config, or open storage. Application identity/path defaults still
apply, including `--home` and embedding path overrides.

The softline prompt is `> `. Its status bar shows model and reasoning effort,
the last measured context percentage, a shortened workspace path, available
ChatGPT subscription quota remaining and credits, `YOLO` when enabled,
and goal status with elapsed active time, such as `goal=active 45m` or
`goal=active 10d10h45m`, without the objective. Completed goals show
`goal=achieved` with their final elapsed active time until replaced or cleared;
`/goal` shows the objective and accounting. Quota appears as `w 72%` when only
a weekly window is available, or `w 72% 1h 48%` when the backend also reports an
hourly window. Windows are
identified by their returned duration. Available credits follow the quota as
`crd 43` or `crd unlimited`, rounded to whole numbers; `/status` retains
fractional balances. Missing credit values are hidden. `libcai` exposes windows
and available credit balance through
`cai_client_chatgpt_quota` in `<cai/quota.h>`. The status
bar requests quota in the background after session startup and after completed
turns, at most once every five minutes automatically. Quota retrieval does not
block prompt readiness, and idle sessions make no periodic quota requests.
Quota requests have a 10-second default deadline. An explicitly configured
`[client] timeout_ms` overrides it, independently of the five-minute debounce.

`/goal` or `/goal status` shows the current durable goal and accounting.
Prefer asking the agent to create and manage goals. For explicit control,
`/goal new TEXT` replaces any current goal without confirmation, resets its
accounting and token budget, and starts pursuit. `/goal pause` pauses at the
next safe model/tool boundary; `/goal resume` resumes pursuit. `/goal clear`
removes goal tracking while allowing the current user turn to finish.
`/goal help` renders the command table. Unknown commands and missing objectives
are errors; they never become goal instructions. Commands remain responsive
during a turn, and goal state survives session replay. Goals created by model
tools, slash commands, startup options or host controls use the same pursuit
loop: an active goal continues after each completed turn and when resumed from
storage. Approved queued prompts take priority over generated continuations.
Escape/Ctrl-C cancels the active turn and durably pauses its goal, including
across restart. `/goal resume` restarts pursuit immediately, including when the
last turn was cancelled or failed. An already-active unfinished goal can also
be resumed without resetting accounting; an achieved goal cannot. Ordinary user
turns still work while paused. The model may pause with `update_goal(status="paused")` when
requested or when it needs a user answer or approval, then ask and finish the
turn. In a later turn it may decide to resume with `update_goal(status="active")`
without an explicit resume request, respecting outstanding approvals and
instructions to remain paused. Completed goals cannot be resumed.
Provider failure stops automatic pursuit for the current invocation
until another user turn or `/goal resume`, leaving goal state unchanged. Completed or stopped
pursuit leaves the interactive prompt open.

Automatic continuations use the embedded Codex goal prompt, including the current
objective, budget, progress checks, and completion/blocked audits. CAI omits
unavailable planning-tool guidance and adapts pause/resume policy. Three consecutive
empty automatic turns with no text, reasoning, tool or compaction activity block
the goal; user turns and activity reset the streak. `/goal resume` can restart a
blocked goal. No runtime template file is required. C hosts use
`cai_agent_runtime_continue_goal(runtime, &started, &error)`; Lua hosts use
`runtime:continue_goal()`, returning whether a continuation started. Pending user
work or goal controls take priority, and completed goals never start continuations.

`/model [MODEL [EFFORT]]` shows the current and pending settings and the
provider catalog, or changes model and optionally reasoning effort.
`/model high` changes only effort; `/model MODEL` keeps the current effort.
`/reasoning [EFFORT]` changes effort or lists the selected model’s advertised
choices and provider default. If model-specific choices are unavailable, it
says so rather than claiming support. Unsupported advertised choices are
rejected. ChatGPT lists every model returned by the signed-in account catalog,
including new IDs absent from CAI's compiled metadata; no local visibility,
API-support, or minimum-version filtering is applied. OpenRouter skips catalog
listing; other providers show at most 50 models, and any model ID can still be
selected explicitly.

Idle changes apply immediately; active requests and tools finish before the
change applies at the next durable tool-round or turn boundary. Subsequent
changes merge into pending settings. Applied settings are committed
atomically and restored on automatic or explicit resume; explicit `-m`, `-r`,
and `--reasoning-summary` independently override saved values. `/new` uses the
startup defaults. Catalog discovery uses a separate client and a five-minute
cache; it makes no additional startup request. Explicit `/model` and
`/reasoning` listings refresh discovery using the configured client request
timeout. A failed refresh retains the last catalog when available; repeat the
command to retry immediately. Accepted commands have a green `$`, rejected
commands a red `$`, and literal, non-italic command text is brownish orange.
Interactive warnings and errors
use a red `*` with `WARN:` or `ERROR:` and readable details, with one blank
line between feed blocks. Complete diagnostic records stay in the session log.

`/status` fetches quota on every invocation, updates the
bar, and renders a Markdown table with model, effort, available context
and usage, quota, and credits. API key providers show `cost ?$` in the status
bar until a USD cost estimate is available. Berget shows `cost ?€` and EUR
estimates without a USD conversion. ChatGPT's cost estimate, when
available, uses API prices as an equivalent estimate, not a subscription
charge. The
context percentage is unknown until response usage is available.
Provider model discovery runs on the agent worker when a turn needs it;
Berget startup does not wait for its models endpoint.
Each musl preset's `cai_package_cli` target writes
`dist/cai-cli-<version>-<arch>-linux-musl.tar.gz`, where `<arch>` is `x86_64`,
`aarch64` or `armhf`. The SDK archive retains its library payload; the executable
and its documentation are staged separately through the `cai-cli` install component.
Its relocatable layout includes `bin/cai`, `share/man/man1/cai.1`, and
`share/doc/libcai/{README.md,LICENSE}` plus the SDK's model metadata guide.
Install under `/usr` to place the man page at `/usr/share/man/man1/cai.1`.

By default, the cai binary stores sessions and ChatGPT credentials in one
encrypted liblockdc Pouch root at `$XDG_STATE_HOME/cai/pouch`, or
`~/.local/state/cai/pouch`. Session metadata is indexed in `cai.sessions`;
incremental state commits and observation events share one ordered collection
per conversation, streamed through the native collections API. Ordinary commits
append new context only; trim retains a prefix and compaction writes its new
replacement context once. There are no recurring full conversation snapshots.
See [session journals](docs/journal.md) for the C/Lua store contract and exports.
Session history requires Pouch; remote lockd collections are not available yet.
Old attachment/snapshot development stores are not migrated; use a fresh root.
No migration or compatibility backend is included.
Credentials are stored separately in `cai.auth`.
Pouch's query indexes are derived caches. Local startup prepares the session and workspace
indexes through liblockdc's wait flush, repairing missing or invalid manifests
without requiring a prompt or a session listing. A repair failure reports a
warning while direct session replay remains available. Indexed queries such
as `--list` or `/resume` can retry recovery. Index-cache recovery does not
replace the encryption key or discard conversation history. An unreadable
index warning alone does not identify the cause of a preceding process crash.
One lockd client per selected root serves its namespaces. A derived `cai.scopes`
index records the newest published session per directory, so normal auto-resume loads one session
without enumerating the directory's conversations. Missing or dirty directory
indexes are rebuilt from session identities and committed journal headers.
Resume falls back to
metadata when a writer holds the directory guard, rather than waiting for it.
Writers durably mark an index dirty before changing a commit or
publication. Runtime recovery consumes the complete journal in sequence order.
Console replay defaults to the newest 10 whole turns (standalone shell entries
also count); steering and tools stay with their turn. `--replay-turns N`,
`CAI_REPLAY_TURNS`, or `[ui] replay_turns` selects the limit. Use `0` to hide
console replay and `-1` to display all history. Older omitted turns remain in
the restored agent context, exports, logs, and host replay callbacks.
Replay indexes sequence ranges and validated-review source ranges, then streams
the selected journal records to libmdf without buffering conversation text or
repeating tool effects. The same two-pass path handles unlimited replay, so a
validated Markdown report can replace its raw JSON without guessing from text.
Auto-resume, explicit
`--resume ID`, and `/resume NUMBER` use the same limit. An omission notice is
shown only when the positive limit actually excludes earlier entries.
The root uses shared writer mode so cai can run in multiple directories.
The generated plaintext key is `$XDG_STATE_HOME/cai/pouch.key` (mode 0600).
**Back up the Pouch root and its original key together.** Losing the key
makes encrypted data unrecoverable; cai refuses to generate a replacement
key for an existing root. The state directory must be owned by you with mode
0700. libcai keeps its existing session store APIs and JSONL backend; these
CLI dependencies are excluded from the `libcai` preset.

`--lockd ENDPOINT` selects another absolute `pouch:///path/to/root` or a
remote `http://` / `https://` lockd server. Pouch roots selected this way use
cai's key and its durable, indexed, shared writer settings. Remote storage
uses the same namespaces; its encryption and access policies belong to the
server. HTTPS verifies server certificates. `--lockd-client-pem FILE` enables
mTLS using a combined client certificate, private key, and CA PEM bundle.
No local Pouch key is created for a remote endpoint.

Starting `cai` resumes the newest session for the canonical current directory.
`-N`/`--new` starts fresh; `/new` does the same inside the prompt and confirms
the new session ID and workspace in a Markdown summary after opening it
successfully. When messaging is enabled, the summary includes its active queue.
`-l`/`--list` shows all conversations across directories. `-R`/`--resume` without
an ID and `/resume` show a numbered list for the current directory only;
`/resume <number>` loads an entry. Rows show ID, directory, and a first-prompt
preview, clipped to the terminal width or 80 columns without a terminal.
`-R ID`/`--resume ID` addresses a saved session in the current directory directly.
`-R NUMBER`/`--resume NUMBER` selects the corresponding row from that directory's
numbered list, with the same ordering as `/resume`. An exact numeric ID, if
present in the directory, takes precedence over a row number.
Saved prompts, assistant text and concise execution summaries replay without
repeating tool side effects.

`--export ID` and `/export` write paired `ID.jsonl` and `ID.md` files
under `$XDG_DATA_HOME/cai/exports/ID` by default (falling back to
`~/.local/share/cai/exports/ID`). `/export` selects the active
session. `--export-dir DIR` overrides the export root. `/export BASENAME`
exports the active conversation to the named path; `--export ID --export-out
BASENAME` does the same for an explicit ID. An exact `.md` or `.jsonl` suffix
selects only that format. Otherwise both suffixes are appended, so
`/export output/conversation` creates `output/conversation.md` and
`output/conversation.jsonl`; `something.else` creates `something.else.md` and
`something.else.jsonl`. Relative paths use the workspace; missing parent
directories are created without changing existing directory permissions.
Existing files are replaced after the selected streams have been written.
Omitting the basename (or leaving `--export-out` empty) retains the default
export location. JSONL contains the
complete resumable commit and journal; Markdown uses libcai's existing
Markdown exporter, including its instructions, conversation, runtime, and
goal sections, followed by a structured execution timeline. Export performs no
model requests or tool execution. These
exports are plaintext private files (mode 0600); exported state contains
conversation and execution context, never the credential namespace.
`--import FILE` validates a JSONL export, assigns a new session ID in the
current directory, and exits. Failed imports never appear in the list.
Existing cai JSONL files are not automatically migrated.

Execution history records agent/harness activity directly; it does not copy
pslog output. It retains tool names and call IDs, exact arguments or raw custom
input, returned output, partial output on failure, and complete error fields
(code, HTTP status, message, detail, provider code and request ID). It also
records response/turn outcomes, request failures exposed to the agent loop,
compaction outcomes, goal controls and provider-supplied hosted-tool call items.
Delegated records retain their child session and originating parent call IDs;
manual review includes the child's actual instruction input. Model, endpoint
and last committed response IDs provide reproduction context without recording
the HTTP client's authentication headers.

These `execution` journal records share the session's collection and append
order. Logical payloads use ordered UTF-8 chunks of at most 4096 bytes, with
byte offsets and a final marker. Their source journal sequence is retained
when a record is forwarded to a parent. This adds no per-record threads,
attachments or full-payload staging. The tool API's existing captured output
is read incrementally into the journal when it becomes available.

Subagents also copy their durable journal events into the parent journal,
independently of live event callbacks. `subagent` events contain bounded
`cai_execution_record` envelopes: `kind` names the original event, and
`session_id`, `subagent`, `parent_session_id`, `parent_call_id` and
`source_sequence` identify its origin. Outer sequence numbers order the parent
timeline; `offset` and `final` describe chunks of one child event. Existing
child `execution` records retain their type and attribution, without duplicate
copies from the UI pump. This adds no worker threads or attachments.

Replay labels delegated activity, preserves assistant progress and reasoning
summaries, and shows validated review findings as Markdown. Raw review JSON
remains in the inspection timeline, not the normal findings display. Failed
review prose and failure details remain available without a logfile. Successful
tool calls use the live tool presentation with one completion heading. Commands
show `* Ran for 1m3s COMMAND` followed by their saved output, without a `Running` row;
`write_stdin` output stays attached to the original command and shares its
preview limit. `--tool-output compact|full` controls replay just as it controls
new calls. Saved patch previews retain confirmed line numbers and neighboring
context; older records containing only submitted patches cannot recover missing
context from files that have since changed. Startup policy notices precede
replay; the idle message distinguishes a resumed conversation from a new
conversation. Previously unrecorded child
output cannot be recreated by this change.

Live command completions use the same elapsed-time heading, with seconds,
minutes or hours as needed. The activity and duration use normal cyan; the
command remains bright cyan. Fast commands may omit the live completion row,
but still have a saved completion and output on replay.

Execution records and subagent envelopes never enter model context, recovery actions or compaction
prompts. Context compaction preserves them; explicit history trimming excludes
the corresponding entries from replay/export. JSONL preserves the complete
records, while console replay uses tool receipts, bounded output and failures.
Invalid side-band payloads produce a historical warning rather than blocking context
recovery; the original record remains in JSONL. Internal debug/trace logs and
ordinary application warnings remain in the pslog file.

Core SDK hosts enable this together with `record_transcript` and can inspect
live `CAI_AGENT_EVENT_EXECUTION_RECORD` events or replay events of type
`execution` or `subagent`. `<cai/execution.h>` provides decoding, cleanup and Markdown
rendering. `libcai_app` enables recording by default. Lua exposes the same
event constant and `cai.execution_record(json)` decoder.
Lua `on_replay` receives `subagent` envelopes through the same callback; decode
`event.data` with `cai.execution_record` to inspect child identity and content.
SDK hosts can stream a human-inspection timeline using
`cai_agent_runtime_export_execution_markdown` or Lua's
`runtime:export_execution_markdown(sink)`. Core agent-handover exporters exclude
these records; the application's Markdown export combines both projections.

For example, inspect tool failures without the log file:

```sh
jq 'select(.type == "execution") | .data | fromjson | select(.kind == "tool.failed")' ID.jsonl
```

### Embedding the terminal application

`libcai_app` provides the complete CAI application as a synchronous C receiver
and optional `cai.app` Lua 5.5 module. The `cai` executable is a thin C embedding
example. Hosts can supply settings, forwarded arguments, a raw startup banner,
a Markdown introduction, custom help naming and an initialized pslog logger.
Both startup strings default to empty in the library; CAI's scaffold supplies
its own interactive attribution and usage text before replay. See
[Embedding CAI](docs/embedding.md) for ownership, process-state restoration,
installation and C/Lua examples. Core libcai remains independent of the
application's softline, libmdf and liblockdc dependencies.
The x86_64, aarch64 and armhf Linux SDK archives include both static and shared
application libraries for glibc and musl.

### Configured MCP tools

`/status` includes the local session ID, last retained provider response ID,
provider conversation ID when available, instruction-file loading,
local/hosted/MCP tool counts, and
MCP server availability in its main metrics table. `/tools` shows
their names, descriptions, input formats and server ownership, plus disabled
or unavailable servers and discovery errors. Both render through libmdf and
remain available during a turn.

Add a named Streamable HTTP server to `config.ini`:

```ini
[mcp.example]
enabled = true
endpoint = https://example.com/mcp
api_key_env = EXAMPLE_MCP_API_KEY
discovery_timeout_ms = 3000
timeout_ms = 30000
```

The endpoint is required for enabled servers. `api_key_env` is optional; when
set, its environment variable supplies a Bearer token. `[env]` can supply that
variable under the normal configuration precedence rules. Server names accept
ASCII letters, digits, underscores and hyphens. Tools are exposed as
`mcp_7_example_TOOL`: the number is the server name's byte length, making the
server/tool boundary unambiguous even with underscores in either name. The
complete alias must fit 64 bytes and use the same characters. The existing
raw-input bridge honors server metadata for Responses
and supplies ordinary JSON function arguments for Chat Completions.

Discovery runs once when the first runtime opens. Each server's default
three-second budget covers initialization and all tools-list pages together;
normal tool calls use the separate `timeout_ms`. Transport, authentication or
catalog failures warn and leave that server unavailable while other tools
remain usable. Catalogs are cached for this invocation, including across
session switches; restart to rediscover. Utility commands and isolated
`--review` runs do not connect MCP servers. `[agent] mcp = false`, `CAI_MCP=false`
or `--no-mcp` disables configured servers; `--mcp` explicitly enables them.
The generated template contains a commented `[mcp.example]` section to copy.

CAI creates `${XDG_CONFIG_HOME:-$HOME/.config}/cai/config.ini` on first
startup (help/version do not create it). The private file is the configuration
reference: each supported setting includes its description and commented
default. Explanatory comments start with `##`; setting assignments start with
`#` so removing one marker selects the value explicitly. Automatic global
instruction, skills and export paths are shown as resolved at template creation.
Leaving them commented retains automatic lookup on subsequent starts; an empty
value has the field-specific discovery, inheritance or unset meaning documented
beside that setting. Existing files are
never rewritten. `-c FILE`/`--config FILE` selects another file; `--no-config` skips
creation and loading. `--config-dir DIR` also selects `DIR/config.ini`.
Startup configuration is read from these files and CLI/environment inputs,
not from pouch storage. Restart CAI after editing the configuration. Global
instruction files are loaded again when a conversation runtime opens, including
on replay; saved conversation state does not replace those files.
Precedence is built-in defaults, configuration, then CLI options. Resuming
restores the saved model and reasoning settings unless explicitly overridden
by startup flags. Paths accept `~/` and are relative to the configuration file.
The `[env]` section accepts literal environment assignments. Existing process
variables, including empty ones, win unless the global
`config_env_has_precedence = true` setting is uncommented. Environment values
are applied before authentication, logging and worker startup; templates do
not copy credentials or environment assignments. Path defaults may reflect the
current XDG roots, HOME or explicit `--config-dir`.

`--home DIR` or `CAI_HOME=DIR` places the application's default directories
under one root: `DIR/config`, `DIR/state`, `DIR/cache`, `DIR/data` and `DIR/run`.
The default config becomes `DIR/config/config.ini`; Pouch and its key live
under `DIR/state`, logs and clipboard files under `DIR/cache`, exports under
`DIR/data/exports`, and Chrome sockets under `DIR/run/chrome`.
Explicit individual paths always win over these derived defaults, even when
the home is supplied on the command line. For example:

```sh
cai --home ./agent-home
cai --home ./agent-home --cache-dir /fast/cai-cache
cai --home ./agent-home -c ./policies/cai.ini
```

`[paths] home_dir` selects the same root from an INI file. CLI/environment/home
embedding fields can select the root before config discovery; a home value
read from INI affects derived paths without moving, rewriting or loading
another config file. `-c FILE` and `CAI_CONFIG` keep their explicit selection.
`--no-config` still honors `--home` while skipping config creation/loading.
`--runtime-dir`, `CAI_RUNTIME_DIR` or `[paths] runtime_dir` overrides the runtime
root. Without a unified home, existing XDG defaults apply. Relative CLI home
paths are anchored to the workspace selected by `-C`; INI paths remain relative
to their config file. The process `HOME`, workspace, external Codex auth and
explicit lockd endpoints are unchanged. An embedding app named `smith` uses
`SMITH_HOME` and the same directory layout, without an extra `smith` suffix.

The shared `<cai/config.h>` receiver provides incremental INI loading,
schema validation, typed access, explicit environment/path handling, and
`write_template`/`create_template_file`. Apply explicit fields to initialized
client, low-level agent, runtime and terminal configuration records; keep the
receiver alive while its borrowed strings are used. Hosts select their own
schema with `cai_config_open_schema`, or use `cai_config_open` for the complete
SDK schema plus extensions. CAI selects only settings its application can
honor, retaining its directory-scoped encrypted session storage policy.
Numeric settings use a dot decimal separator regardless of the application's
locale. Schema extensions retain native field types and constraints, and
templates place globals before named sections. An explicit `--config-dir`
remains anchored to its original location when a configured workspace changes
the working directory.
`section_count`/`section_at` enumerate distinct explicitly configured sections
in first-setting order. Schema sections ending in `.*` accept named instances;
the SDK includes the `mcp.*` settings, which embedding hosts can enumerate to
construct clients explicitly. Applying runtime settings does not open transports.
Lua exposes `cai.config()` with `load(text_or_chunk_reader, origin)`,
`load_file`, `sections()`, typed getters, `set`, `resolve_path`, `apply_environment`,
`write_template(chunk_writer)` and `create_template_file`. Lua configuration
strings reject embedded NUL bytes instead of silently truncating them.

### Skills

Smith runtimes discover skill packages under
`${XDG_CONFIG_HOME:-$HOME/.config}/cai/skills`, or `--config-dir DIR`'s
`skills` directory. `--skills-dir DIR` and `[paths] skills_dir` override that
root. The catalog's names and descriptions enter developer instructions;
the model calls `read_skill` to load a selected `SKILL.md` and its referenced
text resources. Skill selection happens when requested by name or when the
task matches the description. Global skill files do not become workspace files.

`/skills` shows the active runtime's loaded skill names and descriptions in a
Markdown table. Name one in your prompt, for example `Use the my-skill skill
for this task.` The command also works during a turn and does not rescan files
or make model requests. It reports disabled or empty catalogs explicitly.
SDK hosts can visit the same catalog with `cai_agent_runtime_visit_skills`;
Lua exposes copied `{name, description}` entries through `runtime:skills()`.

```sh
cai --install-skill ./my-skill
cai --install-skill ./my-skill --force
cai --skills-dir ./skills --install-skill ./my-skill
```

Installation validates the YAML front matter and copies the complete package,
including nested/hidden regular files and executable scripts, without running
them. The installed directory is named from skill metadata. Existing skills
require explicit `--force` or `--yes`; publication is atomic and a failed copy
preserves the previous package. Links, devices and FIFOs are rejected. Examples containing
their own `SKILL.md` inside a package remain artifacts, not additional catalog
entries. Skill changes are discovered when the runtime is opened again.

YAML names/descriptions support quoting, folded/literal scalars and aliases.
Unknown metadata is ignored; catalog descriptions collapse whitespace.
`cai_skill_metadata_open` and its receiver getters provide the same validation
to SDK hosts. Front matter is buffered up to 1 MiB; the Markdown body is not
loaded during catalog discovery. `read_skill` limits each text resource to 1 MiB.
Lua uses `cai.skill_metadata(text_or_chunk_reader, fallback_name)` and
`metadata:name()`, `metadata:description()`, `metadata:close()`.

Libyaml 0.2.5 comes from the checksum-pinned cpktmisc 0.2.0 SDK through
`CpktYaml::Yaml`. Only its YAML component is linked, statically; cpktdb is not
required. The public API remains C89. SDKs and static CAI binaries include the
libyaml license. Static SDK consumers need the separate cpktmisc and compatible
cpkt core prefixes: CMake resolves `CpktYaml`, and pkg-config resolves `yaml-0.1`.
Host dependency mode requires these packages to be discoverable as well.
Core and misc SDKs must remain in separate prefixes; their package metadata
validates core compatibility and target identity. Archives use the shared
dependency cache (`CPKT_DEPENDENCY_CACHE`, or
`${XDG_CACHE_HOME:-$HOME/.cache}/cpkt/deps`), with extracted SDKs under the
checkout's `CAI_DEPS_DIR`. CAI no longer builds a private YAML producer;
hardening instruments CAI's parser integration, not the prebuilt dependency.

Smith enables hosted `web_search` on Responses providers and the ChatGPT Chrome
extension tool by default. Browser discovery is lazy: ordinary turns work
without Chrome, and a browser call reports missing setup or connection.
`cai --chrome-setup` registers cai's own native host. Setup refuses conflicting registrations;
`--chrome-remove` removes only cai's verified registration. No OpenAI native
executable is required. See [Chrome setup, API, and compatibility](docs/chrome.md).

Tool policy uses CLI > environment > `config.ini` > built-in defaults.
`[env]` is applied first with its documented environment precedence rule.
All boolean environment settings accept the INI boolean syntax. The following
controls apply at startup, including when resuming a conversation:

| Tool | CLI (also `--no-NAME`) | Environment | INI | Default |
| --- | --- | --- | --- | --- |
| Hosted search (Responses only) | `--web-search` | `CAI_WEB_SEARCH` | `[agent] web_search` | on |
| Chrome extension | `--chrome` | `CAI_CHROME` | `[chrome] enabled` | on |
| Kanban | `--todo` | `CAI_TODO` | `[agent] todo` | off |
| SearXNG function search | `--searxng` | `CAI_SEARXNG` | `[agent] searxng` | off |
| Reverse geocoding | `--revgeo` | `CAI_REVGEO` | `[agent] revgeo` | off |
| File reading | `--read-file` | `CAI_READ_FILE` | `[agent] read_file` | on |
| File listing | `--list-files` | `CAI_LIST_FILES` | `[agent] list_files` | on |
| Workspace patching | `--apply-patch` | `CAI_APPLY_PATCH` | `[agent] apply_patch` | on |
| Local images | `--view-image` | `CAI_VIEW_IMAGE` | `[agent] view_image` | model dependent |
| Goal tools | `--goals` | `CAI_GOALS` | `[agent] goals` | on |
| Skills | `--skills` | `CAI_SKILLS` | `[agent] skills` | on when installed |
| Managed commands | `--terminal` | `CAI_TERMINAL` | `[agent] terminal` | on |
| Image generation | `--image-generation` | `CAI_IMAGE_GENERATION` | `[agent] image_generation` | on |
| Review subagent | `--review-subagent` | `CAI_REVIEW_SUBAGENT` | `[agent] review_subagent` | on |
| Review/fix loop tool | `--review-and-fix-tool` | `CAI_REVIEW_AND_FIX_TOOL` | `[agent] review_and_fix_tool` | off |
| Foreground child steering | `--subagent-steering` | `CAI_SUBAGENT_STEERING` | `[agent] subagent_steering` | on |
| Codex global/ancestor instructions | `--codex-agents-md` | `CAI_CODEX_AGENTS_MD` | `[agent] codex_agents_md` | off |

Startup flags, environment and `config.ini` determine instruction settings;
editing the INI requires a restart. `/resume` keeps those startup settings and
reads the instruction files again. Session storage never supplies configuration
or an old global instruction snapshot. Logs identify each global file's resolved
path and loaded byte count, or explicitly report disabled Codex compatibility.

`--searxng-url`/`CAI_SEARXNG_URL` override `[searxng] base_url`;
`--revgeo-url`/`CAI_REVGEO_URL` override `[revgeo] base_url`.
`--chrome-socket`/`CAI_CHROME_SOCKET` select `[chrome] socket`.
The generated template documents service timeouts, result limits and language
settings. SearXNG is available to Chat Completions providers without hosted
search. CAI's optional kanban uses one shared encrypted `cai.todo` document,
with bounded streaming reads/writes and transaction commit/rollback; it creates
no `todo.json` file. Isolated reviews omit these optional tools and browser
access. Ordinary subagents inherit narrowed local capabilities and hosted
search policy, but receive no Chrome connection or optional kanban/service tools.
The separate sandboxed exec preset remains an SDK alternative to the managed
terminal pair; registering both under `exec_command` is not supported.

`--board NAME`, `CAI_BOARD`, or `[todo] default_board` select the initial kanban
board (`default` when unset). `/todo`, `/todo list`, `/todo use NAME` and
`/todo new NAME` inspect or switch it; selection is remembered per conversation.
`/todo see DEF-001` displays a task on the selected board, including completed
tasks: ID and status in the heading, full title beneath it, and its Markdown
description as the body. Descriptions stay out of list and board layouts.
The C/Lua receiver and read-only browser route share that same board engine.
See [shared kanban](docs/kanban.md) for precedence and embedding APIs.

The equivalent libcai runtime fields are `disable_web_search`, `disable_chrome`,
`chrome_config`, `enable_todo`/`todo`, `enable_searxng`/`searxng`,
`enable_revgeo`/`revgeo`, and the individual `disable_read_file`,
`disable_list_files`, `disable_apply_patch`, `disable_view_image`,
`disable_goals`, `disable_skills` policies. The standard INI parser projects
them through `apply_runtime`. SDK hosts enabling kanban can supply its streaming
store callbacks; without them the SDK's existing file store applies.
Preset capability masks also gate search and browser access:
`CAI_AGENT_PRESET_TOOL_WEB_SEARCH` and `CAI_AGENT_PRESET_TOOL_CHROME` are set
by Smith; custom descriptors grant them explicitly. Runtime disable policies
can narrow these grants.
Global instruction load records include the source, resolved path and byte count
in the session log, including after replay. Conversation state never supplies
CAI startup configuration.

`-C DIR`/`--directory DIR` changes directory before the agent opens its
session. Repeat `-i TEXT`/`--instruction TEXT` to run prompts in order, after
the saved conversation has replayed. `-g TEXT`/`--goal TEXT` starts or replaces
a durable goal; the agent keeps working until the goal completes or stops.
`-n`/`--non-interactive` runs the supplied turns or goal and exits without
reading terminal input. For example, `cai -Nni 'write a snake game in Lua'
-i 'rewrite it in Go'` starts fresh, completes both turns, then exits.

Cai uses `pslog_new_from_env("LOG_", ...)` for all diagnostics and runtime
activity, including libcai, liblockdc, tools, and subagents. JSON is the default
format. Ordinary prompts, assistant text, and provider reasoning summaries
are `info` records. Conversation records include `role` and `prompt_kind`
(`normal`, `steering`, `queued`, `developer_instructions`, or
`reasoning_summary`), with event and invocation identifiers where available.
There is no added `app` field. Long text is logged as ordered bounded chunks
with an `offset` field; activity reaches the sink as it arrives.

Interactive warnings, errors, and local command confirmations wrap through
libmdf as literal text. Diagnostic stars remain red, continuation rows align
under the message, and paths and punctuation retain their original spelling.
Notices keep one blank row before and after; `NO_COLOR` disables star colors.

Interactive tool activity uses bright green/red `*` receipts for success/failure,
with normal cyan activity verbs, bold bright cyan command/path details, and
separate MCP and Chrome receipts.
The heading color is independent of the green/red success/failure star.
Non-command result bodies use the themed Markdown feed. Tool failures are
wrapped literal blockquotes; known tools have concise receipts, MCP text content
is rendered as Markdown, and unfamiliar JSON-looking results use code blocks.
These presentation rules apply to replay too; model-visible results and journal
payloads are unchanged. Command runners alone use raw terminal previews.
Commands show their
first five logical output lines by default. `/output` shows the current setting;
`/output compact|full` changes it, and `--tool-output compact|full` selects the
startup setting. Each tool call captures its mode when it starts: active calls
and already displayed output stay unchanged. Full mode retains supplied
command output and all patch context. Non-command Markdown follows its tool's
response limits. Any tool-level output limit is
still reported explicitly.
Command previews preserve recognized ANSI SGR colors/styles and discard other
terminal controls, including cursor movement and title/clipboard strings.
`NO_COLOR` and plain output strip command styling too. Styles are isolated per
command and reset before returning to the host, including during streaming.
CRLF becomes a newline; standalone carriage returns separate progress lines.
Split control sequences and UTF-8 are handled incrementally; no terminal screen
emulation or whole-output buffering is used. Malformed UTF-8, non-control
binary bytes, and control bytes in other tool labels/results/patches are escaped
in the UI.
Terminal tool results and JSON logs preserve valid UTF-8 and replace malformed
bytes with `?`; their content is unaffected by preview filtering.

The native `apply_patch` matcher follows Codex's ordered line matching, including
repeated context, whitespace fallback and Unicode punctuation normalization.
It preserves unchanged context and validates the full plan before publication.
Failed patch diagnostics go through libmdf as quoted prose with literal
expected/actual values; source spacing and raw journal/log data are preserved.
See [patch tool behavior](docs/patch-tool.md) for the matching and safety contract.

Successful `apply_patch` calls display the **agent's submitted changes**, with
one neighboring unchanged line from the current edit's snapshot when the patch
omits context. No VCS comparison is used. A gutter shows actual file line numbers:
removed rows use the original file, while added and context rows use the result.
Matched hunk starts come from the native patch application, including adjustments
for earlier hunks. Added rows have a green background and removed rows a red
background across the entire terminal row, including the gutter and aligned
wrapped continuations. Context rows have no background. Tabs expand to spaces;
UTF-8 glyphs wrap by terminal cell width. Terminals narrower than 32 columns use
the unnumbered format. Compact mode keeps
one context row on either side of changes and uses `⋮` between separated change
blocks; outer context is trimmed without an ellipsis. Full mode retains all
supplied context. `NO_COLOR` disables these colors. Non-interactive invocations
log the full patch preview and tool activity as pslog records on stderr,
regardless of the preview setting; stdout still contains only the final reply.

Rejected update context identifies the file and hunk, with bounded escaped
expected/actual line excerpts when a starting-line candidate exists. Internal
spacing must match; read the current file before regenerating a rejected hunk.
These preflight failures leave files unchanged. The interactive feed shows the
tool failure once; attributed warning records remain in the JSON session log.

A `Delete File` followed by `Add File` for the same path replaces that file
atomically, including when other file sections separate the two. It preserves
the original file permissions and participates in whole-patch rollback. Other
repeated operations on a path are rejected; combine update hunks into one
`Update File` section or use separate calls.

SDK hosts can use `<cai/display.h>` without softline or libmdf. Create a
`cai_tool_display` with a borrowed sink and zero-initialized
`cai_tool_display_config` (compact, five preview lines, no colors), feed it
owner-thread runtime events with `cai_tool_display_event`, then close it with
`cai_tool_display_close`. `cai_tool_display_set_mode` affects future calls.
`cai_tool_display_set_color` (Lua `display:set_color`) changes future styling
between events without losing active command identity, preview limits or
split UTF-8/control decoding state.
Hosts with structured tool presenters can use `cai_tool_display_summary` instead
of the generic completion event, then feed details to their Markdown renderer.
It emits a colored success/failure marker with escaped plain-text labels and
releases the originating display call. Lua exposes
`display:summary(event, verb, label, success)` with the same behavior.
CAI's interactive presenter uses this for reasoning requests, messaging,
kanban, local search and reverse geocoding. It renders details through libmdf;
the original tool results remain in model context, structured logs and exports.

For streaming saved results, `cai_tool_display_replay_begin` (Lua
`display:replay_begin(event, verb, label, success)`) emits that heading but
retains the call's preview and control-decoding state. Supply body chunks as
`TERMINAL_OUTPUT` events, then an empty `TERMINAL_COMMAND_COMPLETED` event
carrying the saved exit status to flush and release the call without another
heading. This renders saved data; it never executes commands.
`cai_tool_display_set_width(display, columns, &error)` enables the numbered,
wrapped layout (32..4096 columns; zero restores unwrapped output). Hosts set
`LC_CTYPE` to their UTF-8 locale for glyph widths. Width changes apply to the
next logical patch row, including within an active call.
Successful native patch calls emit `CAI_AGENT_EVENT_TOOL_PATCH_LOCATIONS` first:
bounded chunks of ASCII `old new\n` rows, one per update hunk in submitted order,
then an empty chunk. These are one-based matched original/result file positions.
The positions include any added leading preview context. Added files start at
one. Next come bounded `CAI_AGENT_EVENT_TOOL_PATCH` chunks
containing submitted changes and neighboring preview context, followed by an
empty chunk. Rejected patches emit neither locations nor patch bytes.

`CAI_AGENT_EVENT_REASONING_SUMMARY_STARTED` precedes each provider summary
section, so a status renderer can replace its heading instead of concatenating
unrelated titles. Lua exposes the same event constant.

`cai_patch_render(source, &config, &error)` also renders a patch directly from
a source to a sink, with bounded line buffers and no rewind or full-body
materialization. `cai_patch_render_width(source, &config, columns, &error)`
also accepts explicit columns. Direct sources have no matched file metadata:
additions start at one; updates may specify `@@ -OLD +NEW @@`, otherwise the
gutter stays blank. No file is opened to guess missing positions.
Changed rows flow in chunks; exceptionally long compact
context rows are visibly clipped. Sources and sinks remain caller-owned.

Lua exposes the same renderer:

```lua
local display = assert(cai.tool_display({mode = "compact", color = true, width = 80},
  function(chunk) io.write(chunk); return true end))
-- Forward each runtime event from its owner-thread callback:
-- assert(display:event(event))
assert(display:set_mode("full")) -- future calls only
assert(display:set_width(100)) -- next logical patch row
assert(display:close())
-- Direct rendering accepts a patch string or a source callback returning chunks.
local patch = "*** Begin Patch\n*** Add File: example.txt\n+hello\n*** End Patch\n"
assert(cai.patch_render(patch, {mode = "full", width = 80},
  function(chunk) io.write(chunk); return true end))
```

Chat Completions emit `cai.chat.request.metrics` at debug level for each HTTP
attempt: model, request ID, attempt number, elapsed milliseconds, and first-token
milliseconds when a token or tool delta arrives. Available usage adds input,
cached input, output, reasoning, and total token counts. Known token prices add
`estimated_cost_usd` or `estimated_cost_eur`; unavailable estimates are omitted,
and no currency conversion is performed. Attempt timing excludes retry waits.
Retry decisions emit `cai.chat.retry` at info level with the attempt, HTTP status,
and wait in milliseconds.

In prompt mode, logs append to `$XDG_CACHE_HOME/cai/<sessionid>.log`, falling
back to `~/.cache/cai/<sessionid>.log`. Files are private (0600), with a private
cai cache directory (0700). Startup records are transferred to the selected
session's log; failed startup records remain in a `startup-*` file. The UI
surfaces readable warning and error notices, including their explanations and
available request context (HTTP status, endpoint, provider code, and request
ID). Full pslog records remain in the file; truncated notices direct you to
the session log. Normal conversation rendering continues through softline
and libmdf.

With `-n`, `--review`, or `--review-and-fix`, the default log sink is stderr.
Stdout contains only the latest top-level assistant response of the entire
invocation, including when a later turn fails without producing a response.
Earlier replies, replay, reasoning, tools, and child reviews stay in the log.
Activity logging is immediate; final stdout selection uses a private,
unlinked file-backed spool with bounded reads, then renders through libmdf.
Markdown rendering detects stdout: terminals allow ANSI styling, while pipes
and files receive escape-free UTF-8 rendered at 80 columns.
Help, version, listing, export, and import keep their normal stdout results.
Utility commands (`--list`/`-l`, `--resume` without an ID, `--export`,
`--import`, and `--login`) default to warning-level diagnostics on stderr.
Agent sessions and review modes retain all levels by default. Login URLs,
callback guidance, and successful login results are normal stdout output.
`LOG_LEVEL=info`, `debug`, or `trace` enables more utility diagnostics through
pslog's normal environment overrides.

Color defaults to automatic terminal detection for stdout/stderr logs;
redirected output and interactive log files stay uncolored.
Native pslog environment settings apply, including `LOG_MODE`, `LOG_LEVEL`,
`LOG_NO_COLOR`, `LOG_FORCE_COLOR`, `LOG_PALETTE`, `LOG_VERBOSE_FIELDS`,
`LOG_DISABLE_TIMESTAMP`, `LOG_TIME_FORMAT`, `LOG_UTC`, `LOG_OUTPUT`, and
`LOG_OUTPUT_FILE_MODE`. `LOG_OUTPUT` explicitly overrides the default sink;
`default+PATH` adds a second file while retaining the default sink and its UI
warning notices. `LOG_LEVEL=warn` filters out ordinary conversation records.

`cai --review` runs one isolated review against the Git default branch and exits.
`cai --review uncommitted` explicitly reviews staged, unstaged, and untracked
changes. Resolution uses local `origin/HEAD`, then requires exactly one local
`main`, `master`, or `trunk`; it never fetches. A missing or ambiguous default
requires `--base REF` or another explicit scope.
`--base REF` selects a base reference, or one `-i TEXT` supplies custom review
instructions. Activity, assistant output, and reasoning are pslog records on stderr;
the validated Markdown findings report appears on stdout. `-o FILE` writes
only the findings to that file and leaves stdout empty. `-T json` selects the
validated structured report instead of Markdown. Failure returns a nonzero
exit status. `cai --review-and-fix` starts a new non-interactive Smith goal
that invokes the built-in review subagent, fixes actionable issues, verifies,
and repeats until a clean review; stopped or failed goals return nonzero.
It has no default iteration limit. Review model and reasoning flags apply to
review subagents in both workflows. Review-and-fix uses the same default base;
`cai --review-and-fix uncommitted` selects uncommitted changes instead.
The default `run_review` tool performs one isolated review. A request to review,
or a later request to fix its findings, does not authorize repeated reviews or
a review/fix goal. The optional `run_review_and_fix` tool requires both explicit
opt-in and a user request or permission to run a loop. Enabling the tool alone
does not grant that permission. It replaces the current conversation's goal and
queues the same review/fix/verify workflow as the explicit command; it does not
launch an isolated fixing agent. It requires goal tools and the reviewer and
is never inherited by subagents. Its optional `target` is `base` or
`uncommitted`, with `base` required for `target=base`; no arguments use the Git
default branch. The tool acknowledges the queued goal, rather than waiting for
the loop to finish. No token budget is imposed.
In the TUI, `/review` starts an isolated reviewer between turns and preserves
the current conversation. `/review uncommitted`, `/review base REF`,
`/review commit SHA`, and `/review TEXT` select an explicit scope. Findings are
rendered as Markdown and become context for subsequent prompts. Escape/Ctrl-C
cancels the review; prompts sent while it runs queue for the parent conversation.
`/review-and-fix` starts the same review/fix/verify goal in the active
conversation, replacing its current goal without opening a new session. It
compares against the Git default branch; `/review-and-fix uncommitted` or
`/review-and-fix base REF` selects another scope. It has no default iteration
limit and leaves the TUI open when pursuit completes or stops. Goal and review
tools must be enabled; `/goal pause` and `/goal clear` control pursuit.
The C API's zero-initialized review request and Lua's `submit_review()` or
`start_review()` use the same default. `cai_agent_review_default_base` and Lua
`cai.review_default_base(workspace)` expose local base discovery to hosts.

Smith loads cai's global `AGENTS.md` from
`${XDG_CONFIG_HOME:-$HOME/.config}/cai/AGENTS.md` when it exists;
`--agents-md PATH` overrides that file. `--codex-agents-md` additionally
loads `${CODEX_HOME:-$HOME/.codex}/AGENTS.md` after the cai global file and
before repository ancestor `AGENTS.md` files. `-I TEXT` or
`--developer-instructions TEXT` appends developer instructions.
Loaded AGENTS.md sources are explicitly framed as the current user's instructions
and working context, including any stated role or preferences. Their complete
contents and loading order are preserved on resume and model changes.

### Messages between conversations

Enable messaging with `--messaging` or `/queue on` in the TUI (default off).
`/queue off` disables it between turns. CAI apps on a shared Pouch
backend can then use `agent_message` to discover named
workspace mailboxes and send by queue name. The default starts with
`cai.<basename>`; collisions add the fewest distinguishing real ancestor
components, skipping shared directories. The actual hostname is a leading
component for distinguishing hosts. Names persist in the shared backend and
stay stable across restarts. Override the full name with `--message-queue NAME`,
`CAI_MESSAGE_QUEUE`, or `[messaging] queue_name`. `/status` and `/queue` show
the name; `/status` also shows the incoming-message approval mode.
`/queue NAME` replaces the full name between turns for this app,
including `/new` and `/resume`, without changing `config.ini`.
The newest app start owns new deliveries for that name; already delivered
messages stay in their original conversation. Incoming messages use softline's normal
approval queue: Alt+A focuses approval, Alt+Y approves, Alt+N rejects, and
Alt+E edits. Approved messages start a new turn after current work completes.
`/autoqueue [on|off]` controls automatic approval (default off). Delivery is
acknowledged before approval, with a default five-minute ACK deadline.
See [conversation messaging](docs/agent-messaging.md) for configuration, the
dependency-free core integration hook, and recovery semantics.

## Release Scope

The current release line includes core `libcai`, the embeddable `libcai_app`,
the static musl `cai` executable, and the Lua 5.5 facades. Supported workflows
include Responses and Chat Completions agents, Conversations, HTTP/SSE
streaming, Responses WebSocket, ChatGPT subscription auth, agent/session DX,
local tools, MCP tool serving and
client connectivity, examples, LuaRock packaging, and release packaging.

Realtime WebSocket is intentionally not part of the current release line. OpenAI's
Realtime WebSocket API is a separate low-latency text/audio surface from
Responses WebSocket, and it is tracked separately in [ROADMAP.md](ROADMAP.md).

## Release Readiness

cai is pre-1.0 and continues to harden its public C and Lua API surfaces.
Compatibility-sensitive changes are made deliberately and verified through the
release SDK, Lua, and downstream-consumer gates.

Linux compiler collections are pinned to Bootlin stable 2026.08-1 (GCC 15.3.0)
for all six Linux targets. The AFL++ GCC plugin uses that same collection.
Local tests, examples, fuzzers, and e2e helpers use the selected x86_64 GNU
collection's loader and runtime directly; no shell-level library-path override
is needed. cpkt 0.6.1 shared dependencies require glibc 2.43 or
newer on Linux. Deployments on older glibc must upgrade their runtime or use a
fully static musl application and validate it.
CLI test builds require `tmux`, alongside the existing Bash/Python fixture and
`jq` tools, for actual terminal-cell and scrollback assertions. Each test owns
and cleans a private tmux socket/server. The resolved controlled retained-frame
resize sequence and its manual reproducer are documented in
[the softline resize report](docs/softline-retained-resize-report.md).
Fuzz harnesses disable their own core dumps so Apport and other host crash
collectors cannot delay crash reporting; no host sysctl changes are required.
The fuzz gate retains crashing or hanging inputs and fails when they are found.

The verification tiers are split intentionally:

Make/build entrypoints prepare their selected compiler collection. Direct CMake
configuration and AFL++ `discover`/`env` only inspect prepared tools; missing tools
fail with an explicit preparation command. Prepare fuzz tooling with
`scripts/cpkt-aflpp.sh ensure` before configuring its preset directly.

- Before committing any implementation slice, run `make format` and the
  relevant tests. `make finalize-slice` is the default local pre-commit path
  for ordinary C changes: it runs clang-format and native unit checks.
  `CTEST_FLAGS='-R pattern --output-on-failure'` selects a smaller slice.
  `make format` and `make format-check` do not configure or compile anything.
  Python is restricted to small wire/data fixtures under `tests/fixtures`;
  Bash, CMake and compiled C89 tests own execution and assertions.
- `make clangd-check` validates every native debug translation unit with the
  host `clangd` against `build/debug/compile_commands.json`. It is an editor
  gate only and is not part of cross-target or release verification. Each
  clangd process has a 30-second timeout; a stalled check fails the gate.
- `make test` runs native Debug unit checks once, excluding packaging and
  live checks. `make test-all` includes the complete native suite, including
  example and packaging checks, once. `make test-cli` checks the CLI on the
  native GNU runtime.
- With cpkt 0.5.0 and cpktmisc 0.2.0, the two host/auto dependency-mode
  configuration tests are temporarily disabled: repeated upstream SDK validation
  exceeds their time budget. Either dependency upgrade restores them automatically.
  Installed-SDK consumer, runtime, ABI and artifact checks remain enabled.
- CLI provider checks use a loopback OpenAI Responses/Chat Completions server
  with scripted responses; they make no real inference calls. Presentation
  and wire tests use the real runtime and tools. Stateless checks omit session
  storage; replay/export checks retain encryption and session formats, using
  a test client with per-event durable sync disabled. Pouch startup,
  recovery, compaction and idle-publication checks still use the production
  storage settings and remain in the same CLI suite. Spinner regressions use
  controlled runtime snapshots; timeout configuration is checked at client
  construction, with short budgets for actual timeout failures.
- `make test-all` adds native CLI, Valgrind, fuzz, Lua and local service checks.
  It never builds the cross matrix or reruns the core suite in Release.
  Fuzz builds only its harnesses; Valgrind builds only its checked executable.
  `CTEST_FILTER='-LE live'` also selects all deterministic tests directly
  with `make test-debug`.
- `make prerelease` is incremental preparation: `test-all` development and
  hardening checks followed by the target and artifact coverage below. Host-only
  fixtures already passed in Debug are omitted from its Release selection.
- `make release-matrix` builds the selected SDK targets, runs native Release
  tests (including host fixtures), ARM QEMU smoke checks and the musl CLI tests, packages SDK, CLI,
  source and Lua artifacts, and verifies the complete checksum inventory and
  extracted consumers. It does not run the additional Debug, memory, fuzz, Lua
  runtime or service scopes owned by `prerelease`, or reconstruct the source
  archive. Both preparation commands accept an explicit `CAI_VERSION_OVERRIDE`
  candidate and never clean or change release refs.
- `make release` is the final local delivery gate.
  Squash the verified development branch onto the local default branch before
  requesting amendment and signing. Keep remote refs unchanged. Signing, tagging
  and the final gate use that default-branch commit, without an intermediate
  release branch. Before cleaning or provisioning, HEAD must be attached to the
  resolved default branch and have one exact lightweight release
  tag, the final amended commit's signature must verify, the worktree must be
  clean, and no version override may be set. The gate removes the entire
  checkout-local `build/`, `dist/`, and `.cache/`, including hidden entries and
  local tools. Only global verified caches and installed prerequisites survive.
  Other checkouts' generated state cannot be borrowed to bypass cleanup.
  Archive-cache overrides may use this checkout's `build/` or `.cache/`; those
  local caches are disposable. The default global cpkt cache survives cleanup.
- The shipped source archive is reconstructed into the native Release lane and
  runs its complete non-live CTest inventory. That exact output supplies the GNU
  SDK package; the main checkout does not build or test a second equivalent lane.
  Reconstruction uses its own extracted helpers, build directory and dependency
  roots. Only checksum-verified global archives and toolchains are shared.
  Other target SDKs receive build/link and extracted-package proof. The shipped
  x86_64 musl CLI receives its actual runtime checks. The aarch64 and armhf
  GNU/musl presets also register a small `cross` CTest inventory: static/shared
  libcai and libcai_app initialization, local session state round trips, and
  musl CLI help/version/invalid-option checks. These run through `qemu-aarch64`
  or `qemu-arm` using the pinned target sysroot. Required runners and sysroots
  must exist; missing prerequisites or failed checks stop the matrix.
  This small inventory is registered even with `CAI_BUILD_TESTS=OFF`; the full
  regression suites remain native.
- `make cross-test` builds the selected matrix incrementally and runs those
  four ARM CTest selections, without native regression suites or packaging.
  For already built output, use `ctest --test-dir build/<target>-release
  --no-tests=error --output-on-failure -L cross`. Cross library checks have
  ten-second timeouts; the combined CLI smoke has a five-second timeout and
  performs no inference. Native `make test`, `test-cli` and `test-all` never
  initiate cross builds or QEMU execution. Darwin execution remains deferred.
- Native Valgrind, AFL++ corpus smoke, Lua, offline integration and local Podman
  e2e retain their distinct scopes. Source-only host fixtures run once; hardening
  and facade selections do not replay them. Every required selection rejects an
  empty inventory and stops at its first failure. All lifecycle commands serialize.
  Native debug library and CLI presets share `build/debug`, so enabling the CLI
  reuses the library producer. Linux configuration never falls back to host GCC.
  Linux markerless-workspace tests use `bwrap` to hide the surrounding Git
  repository while keeping their scratch files under the owned build directory.
  Native Darwin uses `sandbox-exec` to deny inspection of the host Git marker.
- `make package` uses prepared target build trees and their package dependency
  graphs, without an unconditional matrix build. Run `make build-release` first
  when preparation is missing. Its binary-only manifest is under `build/`.
  `make package-verify` verifies an existing complete final artifact set without
  rebuilding producers. The final CHECKSUMS file explicitly selects all shipped
  SDKs, source, CLI and Lua artifacts; a missing member cannot silently shrink it.
  Unlisted release artifacts are rejected. Warm preparation can retain another
  release's files when that release's own CHECKSUMS inventory identifies them.
- `CAI_ENABLE_INTEGRATION_TESTS=1 make prerelease-live` selects live-provider
  coverage explicitly. It creates no reusable credentialed success receipt.
  `CAI_ENABLE_INTEGRATION_TESTS=1 make prerelease-hardening` runs preparation
  with live coverage once, then extended fuzzing without repeating fuzz smoke.
  Live and inspector opt-in checks happen before their build prerequisites.
- Release failures stop; repair requires a separate iteration. Local delivery
  never retags, pushes or publishes. Publication requires explicit engineer
  direction and uses the checksum inventory, not a `dist/` wildcard.

## Design Bias

- C89 with POSIX features.
- CMake/Ninja/Make based build.
- Installed builds export CMake package targets under `lib/cmake/cai` and a
  relocatable `cai.pc` pkg-config file.
- Public C headers use clangd-friendly documentation comments on exported
  functions, structs, callbacks, macros, and method-table fields. New public
  symbols should get concise `/** ... */` comments in the same change.
- `make release` builds/tests the release matrix and writes binary SDK
  archives as `dist/cai-<version>-<target>.tar.gz`, a source archive as
  `dist/cai-<version>.tar.gz`, and `dist/cai-<version>-CHECKSUMS`.
  Release verification checks archive roots, installed docs, pkg-config/CMake
  metadata, dependency-header exclusion, and host-free relative runpaths
  (`$ORIGIN` on ELF targets, `@loader_path` on Darwin).
- The deterministic local example smoke gate is intentionally narrow. It
  covers the examples Makefile and the MCP server example automatically. Live
  example execution is opt-in and limited to a curated non-interactive subset.
- Release versions are detected from an exact lightweight `vX.Y.Z` git tag.
  Local release candidates can use `CAI_VERSION_OVERRIDE=X.Y.Z-rc.N cmake ...`
  or the same override through a custom CMake preset; untagged builds
  intentionally fall back to `0.0.0`.
- Binary SDK archives contain installed headers, CMake/pkg-config metadata,
  `libcai.a`, and the versioned shared library with compatibility symlinks for
  the target platform. They also include `README.md` and `LICENSE` under
  `share/doc/libcai/`, plus supplemental docs under `share/doc/libcai/docs/`.
- Dependency mode defaults to `cpkt`: the build uses the official
  `github.com/c89systems/cpkt` release tarball for the selected target to
  provide the native dependency stack. cai currently consumes curl, OpenSSL,
  nghttp2, libssh2, and zlib from that archive. It also uses official
  `lonejson` and `libpslog` release artifacts for the JSON library and logger
  header. Those dependency archives ship consumer CMake and pkg-config
  metadata, and cai consumes that metadata when available. Tarball URLs and
  SHA-256 values are pinned in CMake; sibling checkout artifacts are not
  dependency inputs.
- `CAI_DEPENDENCY_MODE=host` uses already-installed host dependencies instead:
  libcurl 7.86.0 or newer, OpenSSL crypto, `lonejson.h` plus `liblonejson`,
  `pslog.h`, and cpktmisc's `CpktYaml` package with a compatible cpkt core SDK.
  The libcurl minimum is required for Responses WebSocket
  support. The discovered `liblonejson` must match cai's required ABI
  generation (`liblonejson.so.26` on Linux, `liblonejson.26.dylib` on Darwin).
  `auto` chooses host only when all required host pieces are discoverable and
  the lonejson ABI matches, otherwise it falls back to `cpkt`.
- Installed CMake and pkg-config metadata preserve that dependency mode.
  `cpkt` mode records the official `cpkt` and `cpktmisc` dependency URLs and checksums;
  `host` mode records the resolved host include/library paths. `cai` archives
  do not vendor dependency headers.

Lua verification uses the local `cai_lua_runner`, linked to Lua 5.5.1 and the
selected Bootlin runtime on Linux. Build it with the `debug-lua` preset or
`make lua-test`. After evaluating `make lua-env`, run Lua examples with
`"$CAI_LUA_EXECUTABLE" example.lua`; dependency paths are private to the runner
and are not exported to host subprocesses.

Use the `debug-host-deps` configure/build/test preset to verify compatible
installed dependencies in a separate `build/debug-host-deps` directory. Configure
with `cmake --preset debug-host-deps -DCMAKE_PREFIX_PATH="/prefix/one;/prefix/two"`,
then run `cmake --build --preset debug-host-deps` and
`ctest --preset debug-host-deps`. Linux still uses the pinned Bootlin compiler
and libc; dependency locations do not select the toolchain. To test automatic
dependency selection, configure the same preset in another build directory with
`-B build/debug-auto-deps -DCAI_DEPENDENCY_MODE=auto`. Local runtime checks verify
dependency resolution and Bootlin libc selection in either mode.
- `cai_client_open` resolves API keys only from explicit `config.api_key` or
  `getenv(config.api_key_env)`. If `api_key_env` is NULL, cai uses
  `OPENAI_API_KEY`. It does not implicitly load dotenv files.
- ChatGPT subscription auth can be supplied explicitly through
  `cai_chatgpt_auth` from `cai/auth.h`. Open a CAI-owned, Codex-wire-compatible
  `auth.json` path with `cai_chatgpt_auth_open`, set `config.chatgpt_auth`, and
  keep the auth handle alive for the client lifetime. If `auth_json_path` is
  NULL/empty, the auth library uses `$XDG_STATE_HOME/cai/auth.json`, or
  `$HOME/.local/state/cai/auth.json` when `XDG_STATE_HOME` is unset or
  relative. The C SDK's implicit auth default never selects Codex's auth
  storage; the `cai` CLI explicitly selects it by default. Run
  `make chatgpt-login` to create CAI's default auth file through browser OAuth.
  cai reads `id_token`, `access_token`, `refresh_token`, and `account_id` from
  the file, refreshes access tokens through the configured OAuth issuer before
  expiry, persists returned token fields, and retries one 401/403 response
  after a forced refresh. Before using a refresh token, cai re-reads the auth
  file so another process can refresh or replace same-account tokens without
  racing the stale in-memory handle; a different account is rejected. The auth
  file is never loaded implicitly by `cai_client_open`; callers must explicitly
  open auth and pass `config.chatgpt_auth`.
- Interactive ChatGPT login is exposed as a server-agnostic OAuth callback
  handler, not as a built-in webserver. Call `cai_chatgpt_login_start` with an
  auth file path or the library default and a redirect URI, open the returned
  authorization URL yourself or with `cai_chatgpt_login_open_browser`,
  pass the embedding server's callback request target into
  `cai_chatgpt_login_handle_callback`, then write the returned status,
  content-type, and body. The example `cai_example_chatgpt_login` shows this
  with a tiny local test server. The browser helper uses `open` on Darwin and
  `xdg-open` elsewhere by default, accepts a configured opener command, and
  never invokes a shell. Lua exposes the same flow with `cai.chatgpt_login`,
  `login:handle_callback`, `cai.chatgpt_login_open_browser`, and
  `cai.chatgpt_auth_default_path`; the Lua example uses LuaSocket only for its
  example listener.
- `CAI_DEFAULT_DOTENV_PATH` is `.env` for callers that explicitly want cai's
  dotenv parser. Call `cai_load_dotenv_api_key(path, env_name, &key, &error)`,
  pass the returned key to `config.api_key`, then release it with
  `cai_string_destroy` after `cai_client_open` has copied it.
  Dotenv contents are treated as untrusted input: cai does not execute or
  expand dotenv values, and rejects invalid variable names, overlong lines,
  unterminated quoted values, empty keys, and control characters in key values.
- OpenRouter can be selected with `cai_client_config_use_openrouter()`, which
  uses `OPENROUTER_API_KEY` and `https://openrouter.ai/api/v1`.
- TLS verification is configurable on the surfaces that perform outbound HTTP:
  `cai_client_config`, ChatGPT auth/login configs, and
  `cai_mcp_streamable_http_client_config` expose `insecure_skip_verify`,
  `ca_bundle_path`, and `ca_path`. Leave the CA paths unset to let
  libcurl/OpenSSL use their default trust store.
- Public config structs are zero-defaultable. A memset-zero config means "use
  cai defaults" for omitted knobs; fields are named so nonzero values opt into
  behavior or override a limit/path.
- Session continuity defaults to OpenAI-style server-side continuation.
  Client-side history replay is available for stateless compatible providers
  such as OpenRouter.
- The low-level Responses params surface exposes current request controls used
  by modern Responses workflows: background mode, store, service tier,
  truncation, metadata, include, prompt-template JSON, text verbosity,
  reasoning, structured text format, server-side compaction, local function
  tools, raw hosted tool objects, raw JSON `tool_choice`, and `max_tool_calls`.
  `cai_client_count_response_input_tokens` wraps
  `/responses/input_tokens` and sends a counting-safe request clone, because
  the live endpoint accepts input-shaping fields such as tools and
  `tool_choice` but rejects generation/execution limits such as
  `max_output_tokens` and `max_tool_calls`.
- `lonejson` is the JSON layer and is linked as an external library, not
  compiled into cai. That keeps cai compatible with host applications such as
  Vectis that already provide their own lonejson instance.
- `pslog` is used as an external public header dependency for optional
  host-owned logging.
- Large inputs and outputs should stream through lonejson spooling/source/sink
  paths rather than being materialized in memory.

## Lua Binding

The Lua binding targets Lua 5.5 and is exposed as `require("cai")`. It wraps
the public C workflow facade: clients, agents, sessions, responses/outputs,
streaming sinks, tool registries, tool presets, ChatGPT auth/login helpers, and
the MCP Streamable HTTP handler and client. Lua Smith runtimes also accept
`chrome = { socket_path = path }` or `chrome = { socket_directory = dir }`,
with optional timeout/frame limits. They own a lazy connection until runtime close;
see [Chrome integration](docs/chrome.md).
Lua runtime tables also accept all `disable_*` policies and `enable_todo`,
`enable_searxng`, `enable_revgeo` with their corresponding tool configuration
tables. `todo.store` may be a native store; the runtime borrows it until close.
`runtime:tools()` returns the installed inventory as rows with `name`,
`description`, `input_format`, and `source` (`local`, `hosted` or `mcp`). MCP
rows also identify their client with a one-based `mcp_client_index`. This wraps
the owner-thread C API `cai_agent_runtime_visit_tools`; inspection performs no
network discovery or tool execution and is available while a turn runs.

`runtime:instruction_status()` and `agent:instruction_status()` report
`global_agents_md_loaded`, `codex_agents_md_enabled`, `codex_agents_md_loaded`
and `repository_agents_md_count`. The C equivalents are
`cai_agent_runtime_get_instruction_status` and `cai_agent_get_instruction_status`.
These describe nonempty files actually captured in the developer context,
not filesystem existence. `/status` shows this evidence too. Opening a resumed
session reads the current startup configuration and instruction files again;
editing files while CAI runs requires restarting or reopening the runtime.

Build and run the local LuaRock test:

```sh
make lua-test
```

The LuaRock depends on the `lonejson` Lua rock and links against an installed
`libcai` discovered through `pkg-config cai`; the local test target installs
the current debug build into `build/luarocks/cai-prefix` first. `lonejson`
0.45.0 is not assumed to exist on LuaRocks.org: `make lua-rock` installs it
from the official release source rock at
`https://github.com/sa6mwa/lonejson/releases/download/v0.45.0/lonejson-0.45.0-1.src.rock`
when needed. Lua projects can use the rock as a facade over the C library,
while Vectis can still call the C API directly where that fits its performance
and integration needs better.

The released rockspec uses the public GitHub release URL for its source archive.
The source rock embeds that same archive, so unpacking it needs no source download.

For manual LuaRock installation, install the matching lonejson source rock
first because this version is served from the GitHub release, not LuaRocks.org:

```sh
luarocks install https://github.com/sa6mwa/lonejson/releases/download/v0.45.0/lonejson-0.45.0-1.src.rock
luarocks install cai-<version>-1.src.rock
```

Lua APIs that accept large text or file payloads have `*_spooled` variants.
Those accept strings, chunk-producing callbacks, or lonejson-style spooled
readers with `rewind()` and `read(n)`. Lua raw spooled tool callbacks receive
that same reader shape for arguments, and may return a chunk-producing callback
or spooled reader to stream JSON output without building the full value first.

The Lua facade intentionally does not expose C-only embedding surfaces such as
custom allocators, `FILE *`, `lc_source` / `lc_sink`, raw `cai_source` /
`cai_sink` constructors, or lonejson C maps. A Lua host written in C can expose
any current CAI persistent backend through one constructor:

```lua
local backend = assert(cai.native_store(kind, callbacks, context))
```

`kind` is `"todo"` for a `cai_todo_store_callbacks` pointer plus its opaque
context, `"mcp_session"` for a `cai_mcp_session_callbacks` pointer plus its
opaque context, `"skills"` for a complete `cai_skill_provider` pointer, or
`"agent_session"` for a complete
`cai_agent_session_store` pointer (which already contains its context; pass no
third argument). `"blob"` accepts a `cai_blob_store` pointer (which already
contains its context; pass no third argument) and is used as `storage` for
ChatGPT auth/login or as `global_instruction_store` for Smith. The embedding
host supplies those pointers as lightuserdata;
CAI copies the callback table before returning the Lua userdata. Pass a skills
handle as `skill_provider` (or set `skills_directory`) on a Smith/custom agent
configuration; it supplies the global catalog and constrained `read_skill`
tool without exposing the host's config directory as a workspace file root.
The host must keep the opaque context valid for the handle's documented
lifetime; the lightuserdata itself is only an embedding boundary and must not
be supplied by untrusted Lua code.

Pass the handle as `store` to `register_todo_tool`, as `session` to
`cai.mcp_handler`, or as `session_store` to `new_smith_runtime`. Handles are
typed and cannot be used on the wrong surface. Todo and MCP handles transfer to
their registered owner and call the native `destroy` / `cleanup` hook exactly
once. Agent-session stores remain owned by the native host; CAI pins their Lua
handle until the Smith runtime has closed. No native-store callback enters a
`lua_State`. Pure Lua continues to use `store_path` / `lock_path` for todo and
may use the existing Lua callback table for MCP sessions.

The Lua examples include a basic streaming agent, a terminal chatbot with
SearXNG and todo/kanban tools, streamed tool output, low-level conversation
handling, and session state save/restore. See
[examples/README.md](examples/README.md).
- Non-streamed JSON responses are capped by `json_response_limit_bytes`
  (default 1 MiB). Buffered Chat Completions calls apply this limit during
  capture and to the complete constructed JSON, including escaping and usage
  fields; exceeding it returns `CAI_ERR_LIMIT`. Streaming calls are not capped
  by this setting. Tool output can spill to disk with
  `tool_output_memory_limit` and can be hard-capped per auto-run with
  `tool_output_max_bytes`.
- Clients and sessions can enforce cumulative token and estimated USD spend
  budgets with `cai_usage_limits`. Zero leaves a limit unset. Client limits are
  shared across all agents/sessions on that client; session limits are per
  session and can be set directly or inherited from
  `cai_agent_config.session_usage_limits`. If `max_spend_usd` is positive, the
  selected model must have CAI pricing metadata; verified free OpenRouter models
  are accepted as zero-cost, but unknown or incomplete model metadata fails
  closed instead of silently estimating `$0.00`. CAI estimates spend with
  bundled Standard API token prices and applies known long-context token prices
  for models whose pricing changes above a documented input-token threshold. A
  request that crosses a budget is charged, returns `CAI_ERR_LIMIT`, and later
  requests are rejected before transport until the limit is raised or disabled.
  Use `client->usage`, `session->usage`, or `session->close_with_usage` to
  report cumulative token usage and estimated USD spend. The matching
  `cai_client_usage`, `cai_session_usage`, and
  `cai_session_close_with_usage` free functions remain available:

  ```c
  cai_usage_limits limits;
  cai_usage_accounting spent;

  cai_usage_limits_init(&limits);
  limits.max_total_tokens = 100000;
  limits.max_spend_usd = 5.00;
  config.usage_limits = limits;

  /* ... run one or more sessions ... */
  session->usage(session, &spent, &error);
  ```

  Lua exposes the same surface with `usage_limits` on `cai.open`,
  `session_usage_limits` on `client:new_agent`, and `usage()` /
  `set_usage_limits()` methods on clients and sessions.
- The C and Lua terminal-chat examples set a default session
  `max_output_tokens` budget of `1,000,000` cumulative output tokens. Override
  any budget lane with `--max-input-tokens`, `--max-cached-input-tokens`,
  `--max-output-tokens`, `--max-reasoning-output-tokens`,
  `--max-total-tokens`, or `--max-spend-usd`; pass `0` to disable that lane.
- Production SDK calls should choose a model explicitly.
- Ordinary sessions use Responses context management. The agent runtime keeps
  local client history and compacts it before a known context limit. Responses
  uses the V2 `compaction_trigger` protocol. Chat Completions providers such as
  Berget use bounded client-side summaries, retaining recent turns; provider
  context-overflow errors also trigger one compaction and retry. A streamed
  `finish_reason=length` near the known context limit triggers compaction and
  continuation; an output-only cutoff continues without compaction.
  Reported cutoff usage is counted even when only reasoning or incomplete tool
  arguments were emitted. Session/client limits and goal token budgets apply
  before continuation; incomplete calls are never executed or added to history.
  Chat compaction summaries keep the selected service tier. Failed summaries
  still count reported usage while retaining the original history and error.
- Local spooled session history is opt-in with `enable_local_history`; default
  sessions rely on OpenAI server-side context through `previous_response_id` or
  Conversations. When enabled, `cai_session_export_history_source` streams the
  captured history as a JSON array for `cai_session_compact`, export, or
  offline inspection, and `cai_session_import_history_source` can load that
  JSON array back into a local-history-enabled session.
- Examples and integration development tests default to `gpt-5-nano`.
- OpenRouter development can use
  `CAI_OPENROUTER_MODEL_DEFAULT_RESPONSES`, currently
  `openai/gpt-5.6-luna`, because it passes cai's generic Responses,
  tool-calling, read-tool, streaming, and client-side continuity integration
  regressions through OpenRouter. cai's bundled metadata gives this OpenRouter
  route the same GPT-5.6 Luna context and pricing as the direct OpenAI model.
- OpenRouter tool-calling integration uses
  `CAI_OPENROUTER_MODEL_DEFAULT_RESPONSES` by default. Free OpenRouter models
  and `openrouter/free` may still be useful for ad hoc development, but they are
  not the release-pinned compatibility model.

## Consuming cai

Installed cai archives include both CMake package metadata and pkg-config
metadata. They are meant to keep consumer build files small and avoid manual
include/library path plumbing.

With pkg-config:

```sh
cc app.c $(pkg-config --cflags --libs cai)
```

With CMake:

```cmake
find_package(cai CONFIG REQUIRED)
add_executable(app app.c)
target_link_libraries(app PRIVATE cai::cai_shared)
```

The metadata records the dependency mode used to build cai. In the default
`cpkt` mode it points at the matching official `cpkt` release URL and
checksum for the native curl/OpenSSL stack and the matching official lonejson
release URL/checksum. The CMake package models lonejson as an external
dependency: `find_package(cai)` requires `liblonejson` and its `curl`, `oidc`,
and `openssl` component package surfaces to be discoverable and
adds lonejson headers when they are available, but cai release archives do not
vendor lonejson or curl runtime libraries. The discovered host `liblonejson`
must also expose cai's expected ABI generation. Shared-only consumers that only
use `<cai/cai.h>` can configure without lonejson development headers; consumers
that include lonejson-backed cai headers or use static linking still need the
lonejson development package. The pkg-config file exposes lonejson as a public
requirement and libcurl 7.86.0 or newer as a private link requirement.
`pslog.h`/`libpslog` is only needed by source builds, logging integrations
that construct pslog loggers directly, and the logging-enabled MCP and Smith
terminal examples. cai
release archives do not compile in single-header dependency variants.

## Logging

`cai_client_config.logger` and `cai_agent_runtime_config.logger` accept a
borrowed `pslog_logger *`. CAI never owns or destroys either logger. A runtime
inherits its client logger when no runtime logger is supplied; set the relevant
`logger_disabled` field to nonzero to suppress logging.

Current cai-owned log events cover client setup, runtime open/input/tool/
subagent lifecycle, OpenRouter server-continuity
warnings, HTTP request start/completion, API error status responses, transport
failures, and configured HTTP response-size limit failures. API keys and
normal request/response payloads are not logged; runtime logs likewise exclude
user prompts, tool arguments, delegated instructions, and streamed model output.
HTTP request starts are trace-level,
successful completions are debug-level, 4xx API responses are warn-level, and
5xx/transport failures are error-level. Chat Completions also logs the final
stream result (including HTTP status, model, error code, provider error detail,
and request ID), so HTTP 200 inference errors, invalid streams, and failed
callbacks remain visible. Cancellation is debug-level; output cutoffs and
compaction/continuation attempts are info-level. Exhausted recovery attempts
and failed compactions are warn-level. Runtime compaction completion is logged
after commit/rollback handling. Model catalog source/cache selection is
debug-level; failed refreshes and fallback to configured metadata are warn-level.
These diagnostics exclude normal conversation content; cai's CLI separately
logs conversation events as described above.

## Agent Instructions

Smith bundles pinned model-specific Codex developer instructions. GPT-5.6
Sol/Terra/Luna share a template; GPT-6 Luna, GPT-6 Sol, GPT-6.1 Sol and GPT-6
Astra each select their own. The `openai/` provider prefix is accepted for these
identifiers; other model identifiers use the GPT-5.6 fallback. Custom preset
instructions and the isolated review rubric remain authoritative.

Model changes replace the model-owned instruction prefix at the existing safe
settings boundary, preserving the captured tool policy, repository instructions,
skills and developer extension. Resume selects the saved model's instructions;
a failed commit restores both the model and its prior prompt. CAI adapts
Codex's channels, serial tool dispatch, questions, skills and connector assumptions
through its capability overlay. Builds use committed assets only. The import
helper `bash scripts/import_smith_prompts.sh CODEX_CHECKOUT` updates sources and
their provenance manifest for a deliberate prompt refresh.

The preferred high-level API is method-style handles:

```c
cai_client_open(&client_config, &client, &error);
agent_config.prompt_cache_key = "my-app:assistant:v1";
client->new_agent(client, &agent_config, &agent, &error);

agent->add_user_text(agent, "Explain epoll in one paragraph.", &error);
agent->stream_text(agent, sink, &error);

agent->close(agent);
client->close(client);
```

The receiver form is the primary style for instantiated cai handles that expose
method tables. `cai_client` includes the full client-owned Responses,
streaming, input-token counting, response lifecycle, conversation, agent, and
usage operations. `cai_agent`, `cai_session`, and `cai_tool_schema` follow the
same convention. The matching `cai_*` free functions remain part of the public
API for compatibility, callback wiring, and places where a value is still an
opaque builder or result handle.

Agent-level calls lazily create a default session and reuse it for follow-up
turns, so simple chat and workflow drivers get Responses context continuity
without manually carrying a `cai_session`. Explicit sessions remain available
for multi-session, conversation-handle, and advanced workflows.

Large user text can use `add_user_text_source` or `add_user_text_spooled`
instead of building a temporary C string. Source inputs are consumed when added
and stored as spooled pending input; request retries then clone that spool, so
retry behavior is the same as for small string input.

User file input is available at the same facade level. Use
`add_user_file_path` for a local path, `add_user_file_source` for a `cai_source`,
or `add_user_file_data_spooled` when the caller already owns a
`lonejson_spooled` value. Source/path file inputs are consumed when added and
stored as spooled pending input; request retries then clone that spool, so a
failed request does not consume the queued file input.

Process restart/resume has two separate pieces by design. To continue
inference against OpenAI-held Responses context, persist either
`session->previous_response_id(session)` or `session->conversation_id(session)`
and restore it into a new session with `session->set_previous_response_id` or
`session->set_conversation_id`.
That is the actual server-side continuation handle. Opt-in local history
export/import is for offline inspection, host-owned compaction, and replay
state; by itself it is not a substitute for the OpenAI continuation id.

For applications that want one file/object to persist, use
`cai_session_export_state_source` and `cai_session_import_state_source`. The
state envelope is versioned JSON and contains the active continuation handle,
the last completed response usage, plus local history when `enable_local_history`
is on. Import restores the continuation handle and last usage in all sessions
and restores local history only for
local-history-enabled sessions. `cai_session_save_state_path` and
`cai_session_load_state_path` are the file-backed convenience helpers around
the same source/sink state envelope; the primary handle style is
`session->save_state_path` and `session->load_state_path`.

Set `prompt_cache_key` on `cai_agent_config` when an agent has a stable prompt
prefix, large developer instructions, or stable tool schemas. cai sends it on
every Responses request created by that agent so OpenAI can bucket similar
requests for prompt caching. The default is unset; cai does not generate random
cache keys because that would fragment the cache.

The high-level agent facade uses `developer_instructions`, matching the current
Responses API guidance around application-provided behavior. OpenAI documents
the top-level `instructions` field as high-level instructions that take
priority over `input`, and shows it as roughly equivalent to a `developer`
message. cai therefore treats these as developer instructions in its DX surface.

The agent/session facade does not expose a normal `system` prompt layer. Root
instructions are OpenAI/model policy and are not API-settable. The Responses
wire format still has low-level message roles, but normal cai users should set
`developer_instructions` on the agent and add user input with
`session->add_user_text` or `session->add_user_image_url`.

`assistant` is not an instruction surface. In the Responses API it represents
model-generated messages, and it is mainly useful when manually reconstructing
conversation history. cai's default sessions use `previous_response_id`, and
conversation sessions use OpenAI Conversations, so assistant turns are preserved
through those handles instead of being manually appended by application code.

Structured output text can be decoded into a caller-owned lonejson struct with
`cai_output_write_json`. The function parses `cai_output_text(output)` as JSON;
it does not inspect arbitrary response metadata or raw response JSON.
For non-text output items such as hosted-tool, image, code, or future item
types, use `cai_response_output_items_json` for a materialized JSON array or
`cai_response_write_output_items_json` to stream the preserved output item array
into a sink. The per-item accessors expose common metadata fields, while the
preserved JSON is the compatibility path for item variants cai does not type
yet.

Tools are agent capabilities. The default `register_tool` path is a typed
lonejson callback; `register_raw_tool` is the full-string JSON escape hatch;
`register_raw_spooled_tool` is the raw JSON path for large arguments. The typed
path uses a lonejson map for parameters and a second lonejson map for the result.
JSON Schema is derived from the parameter map automatically, and `_REQ` fields
become required schema fields:

```c
typedef struct lookup_customer_params {
  char *customer_id;
  long long limit;
} lookup_customer_params;

typedef struct lookup_customer_result {
  char *summary;
} lookup_customer_result;

int lookup_customer(void *ctx, const void *params, void *result,
                    cai_error *error) {
  const lookup_customer_params *in = params;
  lookup_customer_result *out = result;

  out->summary = cai_tool_result_strdup("customer found", error);
  return out->summary != NULL ? CAI_OK : CAI_ERR_NOMEM;
}

agent->register_tool(agent, "lookup_customer", "Look up a customer.",
                     &lookup_customer_params_map,
                     &lookup_customer_result_map, lookup_customer, ctx,
                     &error);
```

OpenAI-hosted tools are different from local callback tools: OpenAI executes
them, so cai should not register a local callback for them. Use
`cai_response_create_params_add_simple_hosted_tool` or
`cai_response_create_params_add_hosted_tool_json` on low-level Responses
params, or `agent->add_simple_hosted_tool` / `agent->add_hosted_tool_json` on
the facade. The raw JSON hosted-tool path is intentionally the primary surface
for `mcp`, `file_search`, `code_interpreter`, `computer_use_preview`,
`image_generation`, `tool_search`, and future hosted tools, because their
schemas evolve faster than a C SDK should hard-code. cai validates the supplied
tool JSON with lonejson and streams it into the request body as a JSON value.
For remote MCP hosted tools, `cai_hosted_mcp_tool_config` is only a request
helper for server identity and exposure policy. Omit `allowed_tools` to expose
all remote tools from that MCP server, or provide `allowed_tool_names` /
`allowed_tools_json` to expose only named tools. `require_approval_json`,
`headers_json`, and the raw hosted-tool JSON path remain pass-through so cai
does not mirror or freeze remote tool schemas.

For large typed tool results, callbacks can put source-backed or spooled fields
into their result struct instead of building a raw JSON string. Use
`cai_tool_result_set_source_path` for lonejson source fields and
`cai_tool_result_set_spooled` for lonejson spooled string/base64 fields; cai
streams the resulting JSON into the tool output sink and then cleans up those
handles.

`cai_run_options.tool_output_max_bytes` is a total output cap for tool
auto-runs. Leave it at zero for unlimited output, or set it when tool output is
controlled by remote model decisions and should fail closed above a known size.

`cai_tool_schema_from_map` can be used when callers want to inspect or enrich
the generated schema. Metadata helpers such as `describe` update existing
properties; they do not decide requiredness. Requiredness belongs in the
lonejson field map so the C decoder and OpenAI schema cannot drift.

```c
cai_tool_schema_from_map(&lookup_customer_params_map, &schema, &error);
schema->describe(schema, "customer_id", "Customer id", &error);
```

Tool arguments and tool results are hostile boundaries. The typed tool path
validates argument JSON before callbacks run, rejects unknown fields against the
lonejson map, relies on lonejson for required/type/duplicate-key failures, and
serializes callback results as JSON values instead of raw text concatenation.
This prevents common prompt-injection payloads from smuggling extra fields into
typed callbacks or escaping JSON string boundaries on the way back to the
model. `register_raw_tool` deliberately bypasses the typed result API and
passes arguments as a full C string, so it is a convenience/materialized escape
hatch. Use `register_raw_spooled_tool` or
`cai_tool_registry_register_raw_spooled` when raw JSON arguments may be large;
cai validates the spooled value as JSON and passes the spool through without
rebuilding a whole argument string.

For streamed auto-run tool calls, `cai_tool_event.arguments_json` remains a
compatibility string for logging and simple handlers, while
`arguments_json_spooled` exposes the same validated arguments as a
`lonejson_spooled` handle. cai uses that spooled path for tool execution when
function-call arguments arrive through the streaming capture path.

## MCP Handler

`<cai/mcp.h>` exposes a transport-neutral MCP Streamable HTTP route handler for
serving a `cai_tool_registry` through MCP without making cai an HTTP server.
The host framework, for example Vectis/Kore, owns routing, TLS, auth,
timeouts, and connection lifecycle. cai receives a streaming request body,
reads logical headers through a callback, writes logical response headers
through a callback, and streams the JSON-RPC response into a caller-provided
sink.

First implemented scope:

- POST JSON-RPC requests with JSON replies by default.
- POST JSON-RPC requests can also reply as a single SSE event when the client
  asks for `Accept: text/event-stream` without `application/json`.
- GET `/mcp` returns an SSE stream endpoint for server-to-client transport
  compatibility. cai currently emits a lightweight SSE comment heartbeat and
  closes; server-initiated JSON-RPC notifications/requests are not implemented
  yet.
- `initialize`
- `notifications/initialized`
- `ping`
- `tools/list`
- `tools/call`
- optional stateful `Mcp-Session-Id` lifecycle through host-owned callbacks

The request body is a `cai_source` and the response body is a `cai_sink`.
The JSON-RPC envelope is parsed with lonejson while `id`, `params`, and
`params.arguments` are validated into spooled JSON values. Typed tool
arguments are parsed from that spool. Successful `tools/call` results are
bounded by `tool_output_max_bytes`, defaulting to
`CAI_MCP_DEFAULT_TOOL_OUTPUT_MAX_BYTES`, and cai intentionally uses bounded
spooling for that tool-call output so it can fail closed before committing a
partial JSON-RPC response. Set `tool_output_max_bytes` to
`CAI_MCP_TOOL_OUTPUT_UNLIMITED` only when the embedding route deliberately
accepts direct unbounded tool-output streaming. `GET`/SSE transport is
implemented for compatibility, but cai does not yet provide a host callback
surface for queued server-initiated MCP notifications or requests in the
handler. The handler currently serves tools; resources, prompts, completions,
generic client requests, and server-to-client notification observation belong
to the MCP client surface. Roots, sampling, elicitation, and resource
subscriptions are not implemented.

MCP session persistence is storage-neutral. The zero/default MCP config is
stateless. Set `config.enable_sessions = 1` and provide
`cai_mcp_session_callbacks` to let the host create, load, save, and destroy
protocol sessions. cai returns `Mcp-Session-Id` on `initialize`, loads and
saves session state for later requests carrying that header, marks the session
initialized on `notifications/initialized`, and calls `destroy` for `DELETE`
requests with a session id. Tool state remains tool-owned; MCP session state
only tracks protocol lifecycle and client metadata.

Minimal route adapter shape:

```c
cai_mcp_http_request req;
cai_mcp_http_response res;

req.method = "POST";
req.body = request_body_source;
req.header = my_header_get;
req.header_context = route;

res.body = response_body_sink;
res.set_header = my_header_set;
res.header_context = route;

cai_mcp_handler_handle_http(handler, &req, &res, &error);
```

The handler asks for headers by lowercase logical names such as
`content-type`, `accept`, `origin`, `mcp-protocol-version`, and
`mcp-session-id`; adapters should normalize however their HTTP stack stores
header names. cai sets response headers such as `content-type`,
`mcp-protocol-version`, `cache-control`, and `x-content-type-options`
through the supplied callback.

By default, requests without an `Origin` header are accepted and requests with
a non-empty `Origin` header are rejected unless the origin is present in
`cai_mcp_handler_config.allowed_origins`. Set `disable_origin_validation` only
when the embedding server handles equivalent browser-origin protection itself.

The test build also provides `cai_mcp_http_server`, a tiny plain HTTP utility
for serving the MCP handler on `/mcp`. It is test/example infrastructure only
and is not linked into `libcai`. It binds `127.0.0.1`, defaults to port
`18765` for manual runs, and supports `--port 0 --print-port` for conflict-free
tests. Headers are parsed in memory, while request bodies remain socket-backed
`cai_source` streams pulled by the MCP handler.

An opt-in MCP Inspector e2e test validates the same server through the official
Inspector container image:

```sh
CAI_MCP_INSPECTOR_E2E=1 make mcp-inspector-e2e
```

The test uses `ghcr.io/modelcontextprotocol/inspector:1.0.1` through
rootless Podman Kube and covers `tools/list` and `tools/call` over
Streamable HTTP. It is skipped by default so normal local test runs do not
depend on container image availability. Override the image with
`CAI_MCP_INSPECTOR_IMAGE`. Its container is declared in `devenv.yaml.in` and
rendered only for the explicit opt-in. Pasta forwards only the local test
fixture port into the pod; host networking is not used.

The example MCP server can also be attached to an agent client for manual
end-to-end testing of the production tool presets. Start it with an isolated
todo store:

```sh
cmake --build --preset debug --target cai_example_mcp_server
tmpdir=$(mktemp -d)
CAI_MCP_EXAMPLE_TODO_STORE="$tmpdir/todo.json" \
CAI_MCP_EXAMPLE_TODO_LOCK="$tmpdir/todo.lock" \
./build/debug/cai_example_mcp_server --port 18766
```

Then add the Streamable HTTP endpoint to Codex or Claude Code:

```sh
codex mcp add caiTodo --url http://127.0.0.1:18766/mcp
claude mcp add --transport http caiTodo http://127.0.0.1:18766/mcp
```

See [examples/mcp-server/README.md](examples/mcp-server/README.md) for the
equivalent config-file snippets and a todo-kanban workflow prompt that checks
tool discovery, help text, WIP-limit denial, completion, and board state.

## MCP Client

`<cai/mcp.h>` also exposes a transport-independent `cai_mcp_client` interface
that cai consumes through receiver-style function pointers. The built-in
Streamable HTTP implementation can initialize sessions, ping, cache and call
tools, cache and read resources, cache resource templates, cache and get
prompts, complete prompt/resource arguments, send generic JSON-RPC
requests/notifications, and observe server logging, resource-updated, progress,
cancellation, and list-changed notifications on response streams.

The HTTP client config owns transport details: endpoint URL, client identity,
protocol version, timeout, TLS verification behavior, optional CA bundle/path,
receiver callbacks, and allocator. When CA bundle/path are unset,
libcurl/OpenSSL resolve their default trust store.
`discovery_timeout_ms` optionally bounds a complete tools refresh, including
initialization and pagination. Zero preserves per-request timeouts. A positive
budget does not change subsequent tool-call timeouts. Lua `cai.mcp_client`
accepts the same field.

Remote MCP tools can be registered into a local `cai_tool_registry` with
`cai_mcp_client_register_tools`. The registry keeps a non-owning client pointer,
so callers must keep the MCP client alive for as long as those registered tools
can run.

### Raw input for MCP tools

libcai bridges model-facing Responses `custom` tools to ordinary MCP JSON
arguments. A server advertises the string field under the vendor metadata key
`systems.c89.cai/raw-input`; this is a libcai extension, not a standard MCP
custom-tool type. Other clients continue to use the original object schema.

For an ordinary JSON tool, attach a copied descriptor after registration:

```c
cai_tool_raw_input raw;
memset(&raw, 0, sizeof(raw));
raw.field = "patch";
cai_tool_registry_set_raw_input(registry, "apply_patch", &raw, &error);
```

The MCP handler adds this hint to `tools/list`:

```json
{
  "_meta": {
    "systems.c89.cai/raw-input": { "field": "patch", "framing": "text" }
  }
}
```

Native custom tools automatically export an ordinary object schema with a
required string `input` property and the corresponding hint. The server
unwraps that argument before invoking the raw callback. Custom registration
and `cai_response_create_params_add_custom_tool` accept a NULL format for
unconstrained text; a supplied grammar must specify `grammar`, `lark` or
`regex`, and a nonempty definition. The same format is included in the hint.
Use spooled callbacks when input can contain embedded NULs.

`cai_mcp_tool_registration_config.raw_input_policy` defaults to
`CAI_MCP_RAW_INPUT_AUTO`: use server hints when the MCP client implements
`call_tool_raw`. `CAI_MCP_RAW_INPUT_JSON` keeps the original function schema.
`CAI_MCP_RAW_INPUT_CUSTOM` requires raw-upload support for mapped tools.
`cai_agent_register_mcp_client_tools` automatically selects JSON for Chat
Completions agents; explicitly requesting CUSTOM there returns an error.
When registering directly into a standalone registry for a Chat Completions
provider, select JSON yourself.

Per-tool `raw_input_overrides` take precedence over server hints. Each entry
names the remote tool before `name_prefix` and supplies a descriptor; a NULL
mapping disables the hint. Unknown or duplicate override names are errors.
Mappings must select a declared top-level string property. Registration copies
configuration strings and the selected mapping before refreshing discovery,
so borrowed cached metadata can be used as overrides. It preserves pre-existing
tools on failure and rolls back all additions from the failed registration.
Custom MCP client implementations
can supply `cai_mcp_client_tool.input_schema_json` for full schema validation.

Plain `text` framing sends the entire input as the selected field. If other
arguments are required, use `CAI_TOOL_RAW_INPUT_JSON_HEADER` (`json-header`
in metadata): one JSON object on the first line, LF, then exact raw content.
The header must omit the raw field, contain the other required arguments,
have unique keys, and fit within 16 KiB including the LF. For example, mapping
`content` with this framing accepts:

```text
{"path":"src/example.c"}
#include <stdio.h>
int main(void) { puts("hello"); }
```

Header/body instructions and the ordinary argument schema are added to the
model-facing tool description. A supplied grammar constrains the entire
framed input. The remote tool retains responsibility for its normal argument
schema constraints; libcai validates the mapping and header structure.

The HTTP adapter uses a pull generator and chunked requests to escape input
incrementally, preserving quotes, backslashes, Unicode, embedded NULs, and
trailing newlines without staging a complete serialized request. The embedding
HTTP server must support chunked request bodies. Completed model input and
server callback dispatch retain the existing **spooled** lifecycle: this does
not execute a tool while the model is still producing its input. HTTP session
recovery rewinds and re-encodes the same completed input.

Malformed input is rejected with `CAI_ERR_INVALID`; source read/rewind failures
and dropped connections use `CAI_ERR_TRANSPORT`. Raw calls log the tool name,
status, and error details through pslog, without logging raw payloads. A failed
upload or response does not imply the remote tool had no side effects. Only
HTTP 404 for an expired session triggers one automatic replay; other failures
are returned to the caller. Turn cancellation reaches the HTTP worker,
returns `CAI_ERR_CANCELLED`, and never replays the cancelled request. Cancellation
is logged at debug level and does not undo remote side effects.

Deterministic tests cover buffered and streamed agent-to-MCP round trips,
JSON-header framing, model continuation after tool errors, Chat Completions'
JSON fallback, metadata and policy validation, atomic registration rollback,
byte-split Unicode and embedded NULs, expired-session recovery, JSON/SSE remote
errors, response ID mismatches, malformed responses, disconnects, HTTP errors,
output-sink failures followed by a successful call, borrowed metadata across
discovery refresh, oversized reader results, and cancellation before and during
HTTP work with successful client reuse. Run the C cases with
`ctest --test-dir build/debug --output-on-failure -j1 -R mcp_raw_input`.
`make lua-test` also exercises the real libcai HTTP server with large raw
uploads, malformed headers and UTF-8, failing Lua readers/writers, retained
client lifetime, configuration mutation and collection inside input callbacks,
and recovery after failures.

The equivalent Lua interfaces are:

```lua
registry:set_raw_input("apply_patch", { field = "patch" })
client:register_tools(registry, {
  raw_input_policy = "auto", -- "json" or "custom"
  raw_input_overrides = {
    { name = "write_file", mapping = { field = "content", framing = "json-header" } },
    { name = "another_tool", mapping = false },
  },
})
local result = client:call_tool_raw("apply_patch", { field = "patch" }, raw_patch)
```

Lua `call_tool_raw` accepts text or the existing spooled-input interfaces and
an optional fifth output sink argument. Discovered tool tables expose
`raw_input` and `input_schema_json`. Lua input is collected into the explicit
input spool; HTTP serialization does not collect a second complete request.

Result-producing MCP client methods stream the JSON-RPC `result` value to the
caller sink as it is parsed from JSON or SSE response chunks once the response
id has been validated. Server notifications and requests that arrive before
the final response are handled before result streaming starts. Invalid
pre-result envelopes and server errors do not write partial result JSON;
invalid data after streamed result bytes can still make the call fail after
the sink has observed those bytes.

Security fuzzing is available as opt-in pinned AFL++ instrumentation over the
native Bootlin GCC collection:

```sh
make fuzz
make fuzz-smoke
make fuzz-long
```

Dedicated fuzzers cover the most exposed cai surfaces:

- `cai_tool_fuzz`: typed/raw tool argument handling plus hostile
  `exec_command`, `list_files`, and `read_file` inputs
- `cai_stream_fuzz`: Responses SSE framing, chunk-boundary handling, tool-call
  stream events, and completion metadata
- `cai_response_fuzz`: response/conversation/item parsing plus request JSON
  serialization
- `cai_mcp_fuzz`: MCP JSON-RPC request parsing, session persistence, JSON vs
  SSE replies, and tool dispatch
- `cai_session_fuzz`: large local-history/state export/import paths with spill
  to disk
- `cai_todo_fuzz`: todo/kanban rewrite, persistence, and structured operation
  churn
- `cai_patch_fuzz`: native `apply_patch` parsing and application through both
  the direct API and the model-facing spooled custom-tool path, including
  short raw-read boundaries and isolated workspace fixtures

The fuzz build also registers corpus-replay smoke tests in CTest so every
harness is built and executed in the standard gate for the `fuzz` preset.
Checked-in corpora under `tests/fuzz-corpus/` seed the harnesses with realistic
Responses SSE transcripts, response JSON, MCP JSON-RPC envelopes, session
history/state documents, todo tool operations, and Codex-style patches.
`make fuzz-long` runs a
longer bounded AFL++ campaign, controlled by `CAI_FUZZ_LONG_SECONDS` per
target (120 seconds by default).

Streaming callers that need function-call arguments can attach callbacks to
`cai_stream_sinks.function_call_arguments_delta` and
`cai_stream_sinks.function_call_arguments_done`. Callers that need raw streamed
output-item events for hosted tools, image/code outputs, and future item types
can attach `cai_stream_sinks.output_item_done`; cai passes common metadata plus
the raw item JSON as a `lonejson_spooled` value. Callers that need raw streamed
assistant text deltas before any terminal/UI affixes can attach
`cai_stream_sinks.output_text_delta`. `stream_text` remains text-only; the
callback surface is for callers that want to observe streamed tool-call
argument deltas and output items directly and decide their own orchestration
policy.

For OpenAI API-key clients and ChatGPT subscription-auth clients, the streaming
path uses the Responses WebSocket transport when the base URL is OpenAI's API
or the ChatGPT Codex backend. OpenRouter and local/mock HTTP providers continue
to use HTTP/SSE streaming. The WebSocket implementation uses libcurl's
connect-only WebSocket support, sends `response.create` messages, requests the
OpenAI Responses WebSocket beta, reuses the connection while healthy, retries
one transient upgrade or stale-connection failure before events are observed,
falls back to HTTPS streaming by default when a WebSocket stream fails before
any output reaches caller sinks, and transparently refreshes ChatGPT OAuth
bearer tokens once on 401/403. After text, reasoning, tool-call, or output-item
events have been delivered to the caller, WebSocket transport failures are
returned as errors instead of retrying and risking duplicated output. Set
`cai_client_config.responses_websocket_fallback_disabled` to nonzero when
callers prefer the WebSocket error instead of the HTTPS retry.

Offline WebSocket tests cover handshake/header validation, normal streaming,
large frames, pre-stream transient retry, midstream disconnect failure,
multi-turn server-side continuation, stale keepalive reconnect, usage capture,
and response id propagation. Live integration tests exercise the same public
streaming APIs against OpenAI and ChatGPT subscription auth, including
multi-turn sessions and streaming tool calls. The dedicated live ChatGPT
WebSocket selector is `CAI_INTEGRATION_CHATGPT_RESPONSES_WEBSOCKET_E2E=1`; simulated
disconnect/reconnect fault injection is covered by the mock WebSocket server
instead of trying to break the real OpenAI service connection.

The pinned Bootlin ASan preset is an optional developer check:

```sh
make asan
```

The required native memory gate is Valgrind, run against a Bootlin-built debug
subset. It is intentionally separate from normal debug testing; it does not
claim to provide MemorySanitizer coverage.

```sh
make valgrind
```

One historical ASAN failure looked like a lonejson crash in
`lonejson__owned_malloc`, but the root cause was cai's custom zeroing allocator
returning storage that was only aligned after a `size_t` header. lonejson wraps
custom allocator results in its own owned-allocation header, which needs
maximal C object alignment. cai's allocator header is now explicitly
over-aligned with pointer, integer, floating, and `long double` members. See
`docs/lonejson-change-requests/lonejson-custom-allocator-alignment-cr.md` for
the matching lonejson contract/documentation request.

## OpenAI API Caveats

This SDK has to compensate for gaps in the OpenAI API contract. These are not
minor aesthetic problems; they directly affect whether a serious low-level SDK
can be correct, efficient, and pleasant to use.

The OpenAI Responses API is too untyped for a systems-language SDK. The OpenAPI
schema contains many broad unions, polymorphic items, open-ended maps, and raw
JSON escape hatches. That shape may be convenient for fast API evolution and
dynamic-language clients, but it is a poor contract for C. A C SDK needs stable
discriminators, bounded variant sets where possible, explicit ownership rules,
and mechanically reliable request/response shapes. Where the API does not
provide that, `cai` has to add typed wrappers for the common path and keep raw
JSON escape hatches only where typing would be dishonest or wasteful.

Model metadata is the larger design failure. Useful client behavior depends on
model facts:

- context window size,
- maximum output tokens,
- endpoint compatibility,
- streaming support,
- tool and structured-output support,
- image/audio support,
- deprecation state,
- safe auto-compaction thresholds.

The OpenAI Models API does not expose that as a real model registry. It exposes
basic model objects such as `id`, `object`, `created`, and `owned_by`, which can
answer whether a model exists for a key but not what the SDK needs to know to
run well. The richer data lives in documentation and model comparison pages,
not in a proper machine-readable registry.

That is bad API design. Context windows and capability flags are not marketing
copy; they are operational metadata. OpenAI does support server-side
compaction through `context_management` and `compact_threshold`, but the
threshold is an absolute token count, not a percentage. Without model context
metadata, a client cannot choose a principled default threshold such as 80% of
the context window, cannot report context-window usage as a percentage, cannot
validate whether a requested feature is supported, and cannot produce good
local diagnostics before an API call fails remotely.

As a result, `cai` will treat model metadata as curated SDK data:

- Model constants are strings, not a restrictive enum.
- Unknown model strings are allowed by default so new OpenAI models are usable
  before `cai` is updated.
- Known models get a compiled metadata table with capability flags and context
  limits plus pricing metadata where pricing has been explicitly entered.
- Metadata rows expose flags for verified, incomplete, inferred, deprecated,
  and provider-specific data.
- Runtime `/v1/models/{model}` checks may be used for availability, but not for
  context windows or feature discovery because the API does not return those
  facts.
- Auto-compaction must require known context metadata or an explicit caller
  token threshold. It should not guess.

The repeatable maintenance process for this curated table is documented in
[docs/model-metadata.md](docs/model-metadata.md).

In practice this means `cai` will likely need a reviewed generated metadata file
built from official OpenAI documentation/model comparison data. That is
unfortunate, but it is the only way to make default server-side
auto-compaction, context usage reporting, and feature validation useful until
OpenAI ships a proper model registry.

Relevant OpenAI documentation:

- <https://platform.openai.com/docs/api-reference/models/list>
- <https://platform.openai.com/docs/models/compare>
- <https://platform.openai.com/docs/api-reference/responses>

## Local development services

Local service e2e uses rootless Podman and `podman kube play`. Install Podman
with working user namespaces and Pasta networking; run these commands as your
ordinary user. CMake renders the tracked `devenv.yaml.in` template. There is
one orchestration entry point, `scripts/devenv.sh`.

```sh
make dev-up
make dev-ps
make dev-logs
make test-e2e
make dev-down
make dev-reset
```

Each checkout has distinct pod names and deterministic high host ports derived
from its canonical path. Published ports bind to `127.0.0.1`. `dev-up` prints
the effective endpoints and polls HTTP/JSON-RPC readiness. Override ports with
`CAI_SEARXNG_PORT` and `CAI_MCP_EVERYTHING_PORT`; stop a running pod before
changing its port or image. A conflicting port fails with Podman's diagnostic.

Generated manifests, credentials, bind-mounted state, temporary files, and
captured failure logs live under `build/devenv/`. Manifests and generated local
credentials are private files. User namespace mappings make all service writes
host-owned; startup verifies this with representative state and temporary files.
`dev-reset` stops pods using their original manifests before removing this tree
as the same unprivileged user. Tracked configuration stays in `devenv/`.
Reset always covers all services; `scripts/devenv.sh down SERVICE` stops only
one. Teardown fails if a live pod's original manifest is missing, preserving
state for diagnosis.
`make clean` also tears down these pods before deleting their manifests.

`test-e2e` builds the native fixture and runs the deterministic MCP Everything
matrix. It preserves a pre-existing pod and tears down only the pod it started,
on success or failure. `CAI_E2E_KEEP_DEVSERVICES=1` retains owned pods for
inspection. Failures print the endpoint, failed command, pod state, and recent
logs; captured service logs are in `build/devenv/logs/`. An explicit
`CAI_MCP_EVERYTHING_BASE_URL` uses an external service without invoking Podman.
Readiness waits default to 60 seconds; `CAI_DEV_READY_TIMEOUT` overrides the
bound (greater than zero, at most 600 seconds).

## Local SearXNG

The SearXNG pod supports development and testing of the web-search tool preset.

```sh
make searxng-up
make searxng-wait
make searxng-test
make searxng-down
```

The service uses a checkout-specific loopback port printed by `dev-up`. Override
the host port with `CAI_SEARXNG_PORT` and query a different running instance with
`CAI_SEARXNG_BASE_URL`. The local config enables SearXNG JSON output so cai can
consume `/search?format=json`.

Automated smoke tests intentionally use an explicit upstream engine instead of
SearXNG's default fanout. The default `make searxng-test` engine is
`wikipedia`, which keeps the local test away from commercial search UI scraping
paths such as Google/Startpage. Manual local experimentation can still override
the engine:

```sh
CAI_SEARXNG_TEST_ENGINE=brave make searxng-test
```

Future cai web-search tool tests should keep this rule: use explicit engines,
and prefer documented API-backed engines such as SearXNG's `braveapi`,
Mojeek, or Marginalia when credentials are configured.

## Local MCP Everything

cai includes a local MCP Everything reference server Podman pod for
developing and testing MCP client behavior against a broad, spec-oriented
server. The image is built locally from the pinned
`@modelcontextprotocol/server-everything` npm package and runs the Streamable
HTTP transport. `CAI_MCP_EVERYTHING_VERSION` selects an exact npm package
version for the local image; `CAI_MCP_EVERYTHING_IMAGE` selects a prebuilt
pinned image. `CAI_SEARXNG_IMAGE` similarly overrides the SearXNG image.

```sh
make mcp-everything-up
make mcp-everything-wait
make mcp-everything-test
make mcp-everything-down
```

`make mcp-everything-test` runs the MCP client against the pinned Everything
server and asserts the expected tools, resources, resource templates, prompts,
completions, and representative execution results. The checkout-specific
loopback endpoint is printed by `dev-up`. Override the host port with
`CAI_MCP_EVERYTHING_PORT` and query a different running instance with
`CAI_MCP_EVERYTHING_BASE_URL`. The live MCP client-tool integration test used
by `make test-integration`, `make prerelease-live`, and
`make mcp-everything-live-test` starts the containerized Everything service
automatically, waits for JSON-RPC readiness, and cleans up the service it
started unless `CAI_E2E_KEEP_DEVSERVICES=1` is set.

Agents or registries can register the opt-in SearXNG search preset by including
`<cai/tools/searxng.h>` and calling `cai_agent_register_searxng_tool` or
`cai_tool_registry_register_searxng_tool`. The
preset registers a typed lonejson tool named `searxng_search` by default. Its
input is `query`; its output is a compact typed result containing `query`,
`engine`, `title`, `url`, `snippet`, `source`, `result_count`, and
`infobox_count`. The callback fetches SearXNG JSON into bounded/spillable
`lonejson_spooled` storage, then streams the `results` and `infoboxes` arrays
with lonejson mapped-array streams. It counts every returned item but keeps only
the first usable result or infobox in the compact tool output. When
`config.engine` is unset, cai does not send `engines=...`; the SearXNG instance
uses its configured defaults.

Agents or registries can register the reverse-geocoding preset by including
`<cai/tools/revgeo.h>` and calling `cai_agent_register_revgeo_tool` or
`cai_tool_registry_register_revgeo_tool`. The default tool name is
`reverse_geocode`. Its required input is `latitude` and `longitude`; its typed
output contains `provider`, `label`, `city`, `municipality`, `region`,
`country`, `country_code`, `latitude`, and `longitude`. The default provider is
Nominatim at `https://nominatim.openstreetmap.org/reverse`, with a configurable
base URL, path, language, zoom, user agent, timeout, and bounded/spillable
response storage. Public Nominatim usage requires polite request volume and a
descriptive user agent; production deployments that need higher volume should
configure their own provider endpoint.

Agents or registries can register the persisted todo/kanban preset by
including `<cai/tools/todo.h>` and calling `cai_agent_register_todo_tool` or
`cai_tool_registry_register_todo_tool`. The default tool name is
`todo_kanban`. It exposes a single domain tool with an `operation` argument:
`help`, `create_board`, `list_boards`, `set_wip_limit`, `add_item`,
`list_board`, `current_work`, `get_item`, `move_item`, `complete_item`,
`select_board`, `current_board`, and `snapshot_board`. The registered
tool description and JSON schema include agent-facing usage guidance, operation
enums, field descriptions, and WIP-limit semantics. Agents can call
`operation=help` when they need an in-band reminder of the workflow.
When `board_id`, `board_key`, and `board_name` are omitted, the tool uses the
configured default board and lazily creates it if needed. The default board key
is `DEF`; other board keys are derived from board names unless `create_board`
is called with an explicit unique `board_key`. Calling `create_board` for an
existing board with a new unique `board_key` updates that board key. Item
sequence numbers are never caller-settable. `add_item` allocates a readable
public item reference from the board key and next sequence, for example
`DEF-001`, `DEF-087`, or `OPS-194`, while cai keeps its opaque internal ID in
storage. Tool input is lenient for item references: `DEF-001`, `DEF#1`,
`DEF001`, and `DEF1` all resolve to the same item, and results still return
the canonical dashed form. `list_boards` returns board records in a `boards`
array with `board_count`; item-listing operations return work records in an
`items` array with `item_count`.
`add_item` accepts an optional Markdown `description` in addition to its title
and status. `get_item` takes an `item_id` and returns its canonical ID, full
title, status and nonempty description as top-level fields, including for done
items. It uses the same board selectors and is independent of list-size limits.
List and snapshot operations return compact records without descriptions.

By default the store is `$XDG_CONFIG_HOME/cai/todo.json`, falling back to
`$HOME/.config/cai/todo.json`, with `todo.lock` as the transaction lockfile.
Callers can override both paths in `cai_todo_tool_config`. Missing parent
directories and files are created at registration and operation time.
For non-file backends, set `cai_todo_tool_config.store` and
`store_context`. The callback store API is transaction-oriented: cai opens a
transaction, streams the current JSON document through a `lonejson_reader_fn`,
streams each rewritten document through a `lonejson_sink_fn`, calls
`commit_write` to promote each rewrite pass inside the transaction, then calls
`commit` once to publish the final document. `rollback` must discard staged
writes. This is the intended boundary for lockd-backed or application-owned
todo storage.

The todo store is one canonical JSON document:
`{ "version": 1, "boards": [], "items": [], "done": [] }`. cai reads selected
arrays one item at a time with lonejson array cursors and mutates selected
arrays with `lonejson_array_rewrite_*` under an advisory `fcntl` lock plus
temp-file/rename transaction boundary in the default file store. The complete
document and selected arrays are not materialized for storage operations. Tool
results are bounded by `max_result_items`; when a result is truncated it
includes `truncated=true`.
Moving an item into `in_process` respects the board's WIP limit and reports
`ok=false` with `code="wip_limit_exceeded"` as a normal structured tool result,
not as a transport failure.

Agents or registries can register the opt-in command execution preset by
including `<cai/tools/exec.h>` and calling `cai_agent_register_exec_tool` or
`cai_tool_registry_register_exec_tool`. The default tool name is
`exec_command`, intentionally matching the Codex-compatible model-facing
shape. Its required input is `cmd`; optional inputs are `command` as a
Codex-compatible alias, `stdin`, `workdir`, `shell`, `tty`, `login`,
`timeout_ms`, and `max_output_tokens`. This preset returns after the process
exits. `stdin` is written to child stdin and then closed, which lets agents run
stdin-capable interpreters such as `python3 -`, `sh -s`, `bash -s`, or `lua -`
without fragile shell heredocs. `timeout_ms` defaults to 120 seconds and is
capped by `cai_exec_tool_config.max_timeout_ms`, which defaults to 3 hours.
`max_output_tokens` is treated as a per-call output budget cap against the
byte-oriented capture limit configured in `cai_exec_tool_config`. When
`tty=true` is allowed, cai uses the PTY only for stdout/stderr capture; child
stdin is still not a terminal and is closed after configured input is written.

Command execution is never registered by default. Callers must provide a
`root_path`; every requested `workdir` is resolved under that root before
execution. On Linux the sandbox backend is `bwrap`; when `bwrap` is not
available, the tool fails closed. On Darwin/macOS, cai uses experimental
`sandbox-exec` support with a generated seatbelt profile that allows command
execution, system binary/library reads, file access under `root_path`, and
optional network access. Darwin support is intentionally weaker than the Linux
`bwrap` path and should be treated as experimental until validated on the
target macOS release. There is no non-sandboxed execution fallback.
The bwrap profile keeps the caller UID/GID, clears the inherited environment,
sets only `PATH`, `HOME`, `TMPDIR`, and `LANG`, isolates `/tmp` and
`/var/tmp`, unshares PID/IPC/UTS/network namespaces by default, and starts a
new session while binding only the configured root writable plus read-only
system paths needed to run normal commands. Set `allow_network` only when the
tool should have network access; in that mode cai also binds common resolver
and trust-store files such as `/etc/resolv.conf`, `/etc/hosts`, `/etc/ssl`,
and `/etc/pki` when present.
Linux cgroup v2 pids/memory limits are available with
`enable_cgroup_limits`; zero `pids_max` and `memory_max_bytes` leave those
controllers unset. If both are zero, cai skips cgroup setup entirely. If
cgroup setup cannot be applied, the tool fails closed
instead of running unbounded. `cgroup_parent_path` can point at a host-owned
writable cgroup subtree; otherwise cai uses `/sys/fs/cgroup`.
Output is captured through bounded `lonejson_spooled` fields and serialized as
structured JSON with `stdout`, `stderr`, combined `output`, exit/signal
metadata, timeout state, per-stream and combined-output truncation flags,
effective cwd, and the sandbox backend used. `output_max_bytes` is a total
retained-byte budget across those three text fields; cai prioritizes combined
`output` when the budget is tight, so `stdout` and `stderr` may truncate
earlier than `output`.

Agents or registries can register the opt-in file inspection presets by
including `<cai/tools/read.h>`. `cai_agent_register_read_tool` or
`cai_tool_registry_register_read_tool` registers `read_file`; its required
input is `path`, with optional `start_line`, `end_line`, and `max_bytes`.
`cai_agent_register_list_files_tool` or
`cai_tool_registry_register_list_files_tool` registers `list_files`; its inputs
are optional `path`, `recursive`, `include_hidden`, and `max_entries`.
Both tools require a `root_path`. Relative paths are resolved from
`default_workdir` or the root, absolute paths must already be inside the root,
and symlink/path traversal escapes are rejected after canonicalization.
`read_file` also rejects regular files with multiple hard links so a writable
root cannot smuggle in host files through a hard-link alias.
`list_files` returns bounded file metadata entries and does not follow symlink
directories. Regular files include `text_candidate` and `binary_candidate`
hints from a bounded prefix scan so an agent can avoid reading obvious binary
or control-character payloads. `read_file` rejects directories and streams file
content into a bounded `lonejson_spooled` result field with `path`,
`resolved_path`, line range, `byte_count`, `file_size`, and `truncated`
metadata. Prefer these tools over `exec_command` when an agent only needs to
inspect files. `read_file` is text-only: NUL bytes, invalid UTF-8, and control
characters other than tab/newline/carriage return/form feed are rejected instead
of being serialized back through the model context.

## Integration Tests

The default test suite is offline. Integration tests intentionally spend API
tokens and must be run explicitly through the lifecycle opt-in gate:

```sh
CAI_ENABLE_INTEGRATION_TESTS=1 make test-integration
CAI_ENABLE_INTEGRATION_TESTS=1 make prerelease-live
# Run the Lua-owned Smith coding/review scenario through the installed LuaRock.
# This target builds and installs that rock before starting its live check.
CAI_ENABLE_INTEGRATION_TESTS=1 make test-lua-smith-e2e
```

Once integration tests are enabled, CTest enumerates the real API e2e cases.
The environment variables below are internal selectors for running one
scenario directly from an already-built integration preset, not opt-in gates
for the integration suite. ChatGPT subscription scenarios use
`CAI_CHATGPT_AUTH_JSON` when set, otherwise CAI's normal
`cai_chatgpt_auth_default_path`. If no CAI auth file is available, run
`make chatgpt-login`.

```sh
bash ./scripts/build.sh integration

CAI_INTEGRATION_TODO_WORKFLOW=1 build/integration/cai_integration_tests
build/integration/cai_integration_tests
CAI_INTEGRATION_CHATGPT_E2E=1 build/integration/cai_integration_tests
CAI_INTEGRATION_CHATGPT_ASTRA_SMOKE=1 build/integration/cai_integration_tests
CAI_INTEGRATION_CHATGPT_USAGE_LIMITS=1 build/integration/cai_integration_tests
CAI_INTEGRATION_CHATGPT_SUBSCRIPTION_E2E=1 build/integration/cai_integration_tests
CAI_INTEGRATION_CHATGPT_SMITH_E2E=1 build/integration/cai_integration_tests
CAI_INTEGRATION_CHATGPT_SMITH_REVIEW_E2E=1 build/integration/cai_integration_tests
CAI_INTEGRATION_CHATGPT_STATE_RESTORE=1 build/integration/cai_integration_tests
CAI_INTEGRATION_CHATGPT_COMPACTION=1 build/integration/cai_integration_tests
CAI_INTEGRATION_CHATGPT_TOOL_SECURITY=1 build/integration/cai_integration_tests
CAI_INTEGRATION_CHATGPT_HOSTED_WEB_SEARCH=1 build/integration/cai_integration_tests
CAI_INTEGRATION_CHATGPT_RESPONSES_WEBSOCKET_E2E=1 build/integration/cai_integration_tests
CAI_INTEGRATION_CHATGPT_EXEC_TOOL=1 build/integration/cai_integration_tests
CAI_INTEGRATION_CHATGPT_READ_TOOL=1 build/integration/cai_integration_tests
CAI_INTEGRATION_OPENROUTER_DOTENV=1 build/integration/cai_integration_tests
CAI_INTEGRATION_OPENROUTER=1 build/integration/cai_integration_tests
CAI_INTEGRATION_OPENROUTER_SESSION=1 build/integration/cai_integration_tests
CAI_INTEGRATION_OPENROUTER_TOOL=1 build/integration/cai_integration_tests
CAI_INTEGRATION_OPENROUTER_STREAM_TOOL=1 build/integration/cai_integration_tests
CAI_INTEGRATION_OPENROUTER_STREAM_HISTORY=1 build/integration/cai_integration_tests
CAI_INTEGRATION_OPENROUTER_TOOL_SECURITY=1 build/integration/cai_integration_tests
CAI_INTEGRATION_OPENROUTER_READ_TOOL=1 build/integration/cai_integration_tests
CAI_INTEGRATION_OPENROUTER_E2E=1 build/integration/cai_integration_tests
ctest --preset integration -R cai_lua_chatgpt_hosted_web_search_e2e --output-on-failure
ctest --preset integration -R cai_lua_chatgpt_tool_stream_e2e --output-on-failure
ctest --preset integration -R cai_lua_chatgpt_session_continuity_e2e --output-on-failure
```

`CAI_INTEGRATION_OPENROUTER_DOTENV=1` clears any inherited
`OPENROUTER_API_KEY` process environment variable before opening the client,
then runs the OpenRouter Responses compatibility check. This verifies the cai
`.env` loader path with `.env` containing `OPENROUTER_API_KEY=...`.

`CAI_INTEGRATION_OPENROUTER=1` runs a basic Responses compatibility check
against OpenRouter using `OPENROUTER_API_KEY`, `https://openrouter.ai/api/v1`,
and `CAI_OPENROUTER_MODEL_DEFAULT_RESPONSES` unless
`CAI_OPENROUTER_TEST_MODEL` overrides the model.

`CAI_INTEGRATION_OPENROUTER_SESSION=1` runs a two-turn OpenRouter session
continuity check using cai's client-side history replay mode. It does not test
OpenAI Conversations or server-side compaction on OpenRouter.

Every OpenAI-hosted probe in the routine live integration matrix uses ChatGPT
subscription authentication; it never resolves `OPENAI_API_KEY` or loads an
OpenAI dotenv key. Subscription probes use only GPT-5.6 Luna or GPT-5 Nano.
If the configured auth file cannot be read or refreshed, the selected test
fails rather than falling back to an API key. The separate API-key example
smoke remains available only through the explicit
`CAI_REGISTER_API_KEY_EXAMPLE_SMOKE` CMake option and is excluded from normal
and release live gates. OpenRouter tests retain their independent API-key
configuration and remain part of the normal live matrix.

`CAI_INTEGRATION_OPENROUTER_TOOL=1` runs a typed lonejson tool-calling
regression against OpenRouter using client-side history replay and
`CAI_OPENROUTER_MODEL_DEFAULT_RESPONSES` unless
`CAI_OPENROUTER_TOOL_TEST_MODEL` overrides the model. It verifies that the model
calls the registered tool, the callback result reaches the assistant answer,
and the next turn can recall that tool result through local history.

`CAI_INTEGRATION_OPENROUTER_STREAM_TOOL=1` runs the same style of OpenRouter
tool-calling regression through `cai_session_stream_auto`, validating streamed
tool execution, tool event callbacks, and client-side history replay after the
streaming tool turn.

`CAI_INTEGRATION_OPENROUTER_STREAM_HISTORY=1` runs a no-tool OpenRouter
streaming regression that verifies streamed assistant text is captured into
cai's client-side history and can be recalled on the next non-streaming turn.

`CAI_INTEGRATION_OPENROUTER_TOOL_SECURITY=1` runs the same hostile tool-output
regression as `CAI_INTEGRATION_CHATGPT_TOOL_SECURITY=1`, but against OpenRouter with
client-side history replay and the known working free tool-call model.

`CAI_INTEGRATION_OPENROUTER_READ_TOOL=1` runs `list_files`/`read_file` against
OpenRouter through the real Responses-compatible API. It verifies list hints,
text reads, client-side continuation, and binary-file denial using the known
working free tool-call model.

`CAI_INTEGRATION_OPENROUTER_E2E=1` runs a multi-turn continuity eval against
OpenRouter using
`CAI_OPENROUTER_MODEL_DEFAULT_RESPONSES` and cai's client-side history replay
mode. The test paces requests by `CAI_OPENROUTER_E2E_DELAY_SEC`, defaulting to
4 seconds, because OpenRouter routes can have provider-specific rate limits.
The ChatGPT-subscription E2E remains the longer 20-turn eval.

`CAI_INTEGRATION_CHATGPT_SEARXNG_TOOL=1` runs a ChatGPT-subscription
tool-calling regression
against a local SearXNG endpoint, defaulting to `http://127.0.0.1:8888` and the
explicit `wikipedia` engine. Start it with `make searxng-up` before running
the test, or set `CAI_SEARXNG_BASE_URL` to another SearXNG instance.

`CAI_INTEGRATION_CHATGPT_SEARXNG_STREAM_TOOL=1` runs the same local SearXNG
tool through the streaming auto-run path against the ChatGPT subscription
backend.

`CAI_INTEGRATION_CHATGPT_HOSTED_WEB_SEARCH=1` runs a ChatGPT-hosted `web_search`
regression. It uses the generic hosted-tool JSON path, requires a hosted tool
call, validates structured raw JSON `tool_choice`, verifies
`max_tool_calls`, checks `/responses/input_tokens`, and fails unless the
response output items include `web_search_call`.

`CAI_INTEGRATION_CHATGPT_RESPONSES_WEBSOCKET_E2E=1` forces the Responses
WebSocket transport against the ChatGPT subscription backend. It runs a multi-turn streamed session,
alternates `stream`, `stream_text`, and `stream_auto`, forces a streamed local
tool call, validates streamed tool events and arguments, checks state
continuity, and verifies token usage on every turn.

`cai_lua_chatgpt_hosted_web_search_e2e` runs the same hosted-tool path from Lua using
low-level `response_params` and fails unless the Lua response wrapper exposes a
`web_search_call` output item, `/responses/input_tokens` returns a positive
count, and token usage is present in the real API response.

`CAI_INTEGRATION_REVGEO_PROVIDER=1` runs the reverse-geocoding preset directly
against the default provider and asserts known Gothenburg coordinates resolve
to Sweden/SE with a recognizable Gothenburg label or city. This does not spend
OpenAI tokens, but it does call the public geocoding service.

`CAI_INTEGRATION_CHATGPT_TOOL_SECURITY=1` runs a ChatGPT-subscription
tool-output injection
regression. The registered tool returns text that looks like JSON role/system
messages and direct instructions to override the developer prompt. The test
passes only if the model treats that output as untrusted tool data and returns
the expected safe marker.

`CAI_INTEGRATION_CHATGPT_EXEC_TOOL=1` runs a ChatGPT-subscription streaming tool regression for
the `exec_command` preset. The model must call the command tool against a
temporary sandbox root, run `ls`, `uname`, `cat`, `grep`, and `tar`, and then
attempt `cat /etc/passwd | head -n 1`. The test verifies tool events, final
assistant markers, `sandbox="bwrap"`, successful normal command output, and
that the host `/etc/passwd` escape is denied.

`CAI_INTEGRATION_CHATGPT_READ_TOOL=1` runs a ChatGPT-subscription streaming tool regression for
the `list_files` and `read_file` presets. The model must inspect a sandboxed
test tree, read bounded UTF-8 text, avoid binary reads when list hints identify
a binary candidate, and observe a forced denial when explicitly asked to read a
binary file.

`CAI_INTEGRATION_CHATGPT_SUBSCRIPTION_E2E=1` runs an 11-turn ChatGPT
subscription-auth session against the ChatGPT Codex backend. It alternates
stream APIs, forces tool calls on selected turns, verifies first/current/
previous turn recall, and exercises transparent token refresh behavior through
the same Responses WebSocket transport used by ChatGPT-auth streaming sessions.

`CAI_INTEGRATION_CHATGPT_SMITH_E2E=1` is the live Luna-low coding acceptance
test for `cai_agent_runtime`, using a disposable workspace and local JSONL
store.
It proves Smith's runtime event loop, read/patch/read-back flow, single-process
terminal start/wait/completion lifecycle, steering delivery after a tool round,
durable commits, and `resume_latest` continuation. All live Smith tests
use ChatGPT subscription authentication and GPT-5.6 Luna; they never consume
an OpenAI API-key budget.

`CAI_INTEGRATION_CHATGPT_SMITH_REVIEW_E2E=1` is the live Luna Smith
review-handoff regression: its coding turn uses low reasoning effort and its
isolated reviewer uses medium effort. Its harness creates only an empty,
locally authored Git fixture; Smith creates, builds, runs, and commits a small
C/CMake/Makefile project. CAI then launches the isolated interactive review of
that commit, hands its report to the original Smith runtime, and Smith fixes
any actionable finding before both independent build paths and clean Git state
are checked.
It is registered with the integration label, so
`CAI_ENABLE_INTEGRATION_TESTS=1 make test-integration` and
`CAI_ENABLE_INTEGRATION_TESTS=1 make prerelease-live` include it.

`CAI_INTEGRATION_CHATGPT_SMITH_SUBAGENTS_E2E=1` is the live Luna acceptance
test for model-requested synchronous subagents. A parent uses a configured
checker profile that returns a constrained JSON handover and the built-in
reviewer against a disposable uncommitted Git fixture. It fails unless both
handovers return, the reviewer is presented as Markdown, and raw reviewer JSON
stays out of the parent event stream. The separate review E2E covers the
configured medium-effort reviewer policy.

`CAI_INTEGRATION_CHATGPT_E2E=1` runs a 20-turn session regression against the
ChatGPT subscription backend using GPT-5 Nano. It checks every turn for the current secret,
the first-turn secret, and the previous-turn secret so the test fails if
session continuity breaks.

`CAI_INTEGRATION_CHATGPT_ASTRA_SMOKE=1` sends one bounded, low-reasoning
Responses request to GPT-6 Astra through the ChatGPT subscription backend. It
proves that the model remains available through CAI's authenticated transport;
the integration CTest is included only when the explicit integration gate is
enabled.

`CAI_INTEGRATION_CHATGPT_STATE_RESTORE=1` runs a shorter save/restore regression: it
teaches one exact key, saves the session state to disk, creates a fresh
session, loads that state, and verifies the next API turn can recall the key
through the restored continuation handle.

The e2e path enforces a local estimated spend cap using actual token usage and
compiled model pricing metadata. The default cap is `$0.02`; override it with:

```sh
CAI_INTEGRATION_SPEND_LIMIT_USD=0.05 CAI_INTEGRATION_CHATGPT_E2E=1 build/integration/cai_integration_tests
```

The generic cost estimator remains useful for the separate OpenRouter matrix.
Subscription probes report token usage but do not spend an OpenAI API-key budget.
