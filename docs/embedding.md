# Embedding the CAI application

`libcai_app` contains the complete CAI terminal application: softline/libmdf,
configuration, encrypted sessions/auth, tools, MCP, slash commands, review and
non-interactive work. Core `libcai` remains independently usable without the
application dependencies. The application supports Linux x86_64, aarch64 and armhf,
with both glibc and musl. The standalone `cai` binary ships for all three Linux
architectures as a static musl executable, built alongside its matching SDK.

Build with `CAI_BUILD_APP=ON`; `CAI_BUILD_CLI=ON` also selects it. The `cai`
executable is the small C embedding example in `cli/main.c`. Install the
`cai-app` component alongside its separately installed core/dependency SDKs.
Use `find_package(cai_app CONFIG REQUIRED)` and `cai::app_static` or
`cai::app_shared`, or pkg-config `cai-app`. The application shared library has
ABI 0; the core libcai ABI is 5.

```c
#include <cai/app.h>

int run_smith(int count, const char *const *arguments) {
  cai_app_config config;
  cai_app *app = NULL;
  cai_error error;
  int code = 1;
  int rc;
  cai_app_setting settings[] = {
    {"agent", "identity", "Vectis Agent Smith"},
    {"agent", "codex_agents_md", "true"}
  };
  cai_app_config_init(&config);
  config.program_name = "smith";
  config.invocation_name = "vectis -a smith --";
  config.banner = "Vectis Agent Smith\n\n";
  config.introduction_markdown = "**Enter** sends a task; `/help` lists commands.\n";
  config.introduction_margin_left = 0;
  config.introduction_margin_right = 0;
  config.settings = settings;
  config.setting_count = sizeof(settings) / sizeof(settings[0]);
  /* config.logger = host_logger; -- optional borrowed pslog logger */
  cai_error_init(&error);
  rc = cai_app_open(&config, &app, &error);
  if (rc == CAI_OK)
    rc = app->run(app, count, arguments, &code, &error);
  if (app != NULL)
    app->close(app);
  cai_error_cleanup(&error);
  return rc == CAI_OK ? code : 1;
}
```

Pass only CAI arguments, excluding the program name. A host can forward the
tail after `vectis -a smith --`, or a C main can pass `argc-1`, `argv+1`.
`run` is synchronous, never exits the host, and returns after `/quit`, EOF,
completed batch/review work, or failure. On CAI_OK its output exit code has the
normal CLI meaning (0/1/2). Invocation and restoration failures return a CAI
status and `cai_error` instead.

The initialized config selects CAI defaults, including automatic XDG paths and
ChatGPT/provider/model defaults. `config_file`, `config_directory`, `no_config`
select startup configuration. The `settings` array accepts the full INI schema,
including `[mcp.NAME]` and `[env]`, without duplicating its validation rules.
Values use INI's parsed value form: e.g. `true`, without surrounding quotes.

`program_name` is the application identity, default `"cai"`. Set it once before
`open`; the receiver copies it. It must be a lowercase identifier, at most 32
bytes, starting with a letter (remaining characters: letters, digits, `_`, `-`).
Use `invocation_name` for a multiword usage label such as `vectis -a smith --`.
With `program_name = "smith"`, automatic defaults are:

| Setting | Default |
| --- | --- |
| Environment prefix | `SMITH_` (`-` becomes `_`) |
| Configuration | `$XDG_CONFIG_HOME/smith/config.ini` |
| Instructions / skills / model overrides | `config_directory/AGENTS.md`, `skills`, `models.json` |
| State / cache / data | `$XDG_STATE_HOME/smith`, `$XDG_CACHE_HOME/smith`, `$XDG_DATA_HOME/smith` |
| Pouch / encryption key / prompt history | `state_directory/pouch`, `pouch.key`, `history` |
| Clipboard / interactive logs | `cache_directory/clipboard`, `cache_directory` |
| Exports | `data_directory/exports` |
| Chrome sockets | `$XDG_RUNTIME_DIR/smith/chrome` when set; otherwise `cache_directory/chrome` |
| Storage namespaces | `smith.sessions`, `smith.scopes`, `smith.auth`, `smith.todo`, `smith.messages` |
| Automatic mailbox / receipt prefix | `smith` |
| MCP client name / default agent identity | `smith` |

Unset XDG variables use the standard HOME fallbacks. The C and Lua open configs
both accept `environment_prefix`, `namespace_prefix`, `queue_prefix`,
`home_directory`, `runtime_directory`,
`config_file`, `config_directory`, `state_directory`, `cache_directory`,
`data_directory`, `history_directory`, `clipboard_directory`, `log_directory`,
and `pouch_key_file`. Roots are independent: moving the config file does not
implicitly move instructions or state. Child defaults follow their selected
root. For example, a host can supply its own resolved XDG paths:

```c
config.program_name = "smith";
config.invocation_name = "vectis -a smith --";
config.config_file = host_config_file;
config.config_directory = host_config_directory;
config.cache_directory = host_cache_directory;
config.state_directory = host_state_directory;
```

Set `home_directory` to derive all unset roots below one directory: `config`,
`state`, `cache`, `data` and `run`. Explicit individual fields, INI settings,
environment variables and flags take precedence over home-derived paths.
No application-name suffix is added. Both receivers copy the home string at
open, just like the other path fields:

```c
config.home_directory = "/srv/vectis/smith";
config.config_file = "/etc/vectis/smith.ini";
```

```lua
local app = assert(require("cai.app").open {
  program_name = "smith",
  home_directory = "/srv/vectis/smith",
  config_file = "/etc/vectis/smith.ini",
})
```

The equivalent invocation settings are `--home DIR`, `SMITH_HOME`, or
`[paths] home_dir`. The field, flag or environment variable can select
`home/config/config.ini` before startup configuration is read. An INI home
value changes derived defaults without reopening configuration elsewhere.
`-c FILE`/`--config FILE` remains authoritative. `--no-config` suppresses only
configuration creation/loading; it does not disable home-derived paths.
`runtime_directory`, `--runtime-dir`, `SMITH_RUNTIME_DIR`, or `[paths] runtime_dir`
can override the runtime root independently. These settings never change the
host process's `HOME` or relocate explicit external inputs or endpoints.

These fields seed defaults. Explicit INI values, environment, `settings`, and
argv take precedence in that order. Paths are also configurable through
`[paths]` (`home_dir`, `runtime_dir`, `config_dir`, `state_dir`, `cache_dir`, `data_dir`, `history_dir`,
`clipboard_dir`, `log_dir`, and the existing individual file
settings), and matching `--...-dir` / `--pouch-key-file` flags. Each namespace
has its own `[storage] ..._namespace` key and matching flag, alongside
`namespace_prefix`. `[messaging] queue_prefix` / `--queue-prefix` controls
automatic names; `queue_name` / `--message-queue` replaces the full public name.
`[chrome] socket_directory` and `[mcp.NAME] client_name` override those defaults.

Environment keys use the selected prefix plus uppercased section and key,
for example `SMITH_PATHS_STATE_DIR` and `SMITH_STORAGE_AUTH_NAMESPACE`.
Convenience aliases include `SMITH_MODEL`, `SMITH_PROVIDER`,
`SMITH_REASONING_EFFORT`, `SMITH_HOME`, `SMITH_RUNTIME_DIR`, `SMITH_STATE_DIR`, `SMITH_CACHE_DIR`, `SMITH_DATA_DIR`,
`SMITH_LOCKD`, `SMITH_NAMESPACE_PREFIX`, `SMITH_QUEUE_PREFIX`, and the existing
tool/messaging names with the new prefix. `SMITH_CONFIG` and `SMITH_CONFIG_DIR`
select the startup config file/root before loading INI. An explicitly supplied
`environment_prefix`, e.g. `"VECTIS_SMITH_"`, replaces `SMITH_` entirely; there
is no fallback to `CAI_`. Custom-provider API keys default to `SMITH_API_KEY`.
Provider conventions (`OPENAI_API_KEY`, `BERGET_API_KEY`, `CODEX_HOME`), pslog's
`LOG_`, extension host protocol names, and actual library/module names stay
independent. Explicit banner and introduction strings are used verbatim.

Names isolate storage even at the same endpoint. To share selected data, set
the corresponding namespaces explicitly; encrypted shared roots also require
the same `pouch_key_file`. The independent `cai_app_kanban_open` (Lua `application.kanban`) receiver accepts
`program_name`, `namespace_prefix`, `todo_namespace`, and `pouch_key_file` in
addition to its existing state/endpoint fields, so route handlers can open the
same board as a named application.

### Native custom tools

Set `cai_app_config.register_agent_tools` to register host tools through the same
`cai_agent_register_*` APIs used by the core runtime:

```c
static int host_fact(void *context, const char *arguments, cai_sink *output,
                     cai_error *error) {
  static const char result[] = "{\"host_fact\":\"ready\"}";
  (void)context;
  (void)arguments;
  return cai_sink_write(output, result, sizeof(result) - 1U, error);
}

static int register_host_tools(void *context, cai_agent *agent,
                               cai_error *error) {
  return cai_agent_register_raw_tool(agent, "host_fact", "Read a host fact",
                                     "{\"type\":\"object\"}", 0,
                                     host_fact, context, error);
}

/* Before cai_app_open: */
config.register_agent_tools = register_host_tools;
config.register_agent_tools_context = host_context;
```

Typed, spooled and custom/freeform tools can be registered in the same hook.
Registration runs on the thread calling `app->run`, after built-in and messaging
tools are installed and before workers start. It runs again whenever a new
runtime opens, including session switches. The agent is borrowed for the call.
Host contexts remain borrowed and must outlive each run; tool handlers execute
on worker threads and must be thread-safe. Lua and other VM callbacks require
their own owner-thread dispatch rather than calling the VM from a tool worker.
Registration errors abort startup. Help and utility commands do not call the
hook. It is not inherited by child agents and is rejected in isolated review,
matching the core runtime's extension policy.
Precedence is defaults, file/environment, explicit settings, then arguments.
Environment entries retain the normal `config_env_has_precedence` policy.
Prompt and Markdown themes use the application setting `ui.theme` (default
`default`). For example, pass `{"ui", "theme", "synthwave"}` in the C
`config.settings` array, or `settings = {ui = {theme = "synthwave"}}` in Lua.
Aliases select paired softline/libmdf themes; `riced:iosvkem` selects them
independently. The same setting accepts `--theme` / `-t`, `CAI_THEME` (rebased
to the host's environment prefix), and `[ui] theme` in INI. `--list-themes`
lists both libraries' themes before configuration, authentication or storage
startup. See the [pairing table](../README.md) for aliases. `plain:ascii`
disables Markdown ANSI styling while preserving UTF-8. `NO_COLOR` overrides
the selection with the plain prompt and escape-free Markdown. Themes apply
to the Markdown introduction and conversation feed; raw banners stay verbatim
and logger configuration remains independent.
The same TUI provides `/theme` to show the active pair and both libraries'
themes, and `/theme NAME` or `/theme SOFTLINE:LIBMDF` to switch between turns.
This selection applies to future output in the current app invocation,
including after `/new` or `/resume`, without changing startup settings.
`[paths]`, `[storage]`, and `[env]` XDG variables provide host path overrides.
Configuration strings/settings are copied by open. Logger ownership stays with
the host until close; CAI neither closes nor reconfigures a supplied logger.
With a supplied logger CAI does not create its default log files or intercept
that logger's records for interactive notices. Use a terminal-compatible sink.

`banner` and `introduction_markdown` default to NULL in C and are unset by
default in Lua. The host supplies both; CAI's own name, attribution and usage
text are configured only by `cli/main.c`. The optional raw banner is printed
verbatim before softline/libmdf starts: all newlines belong in the host's banner
string. CAI's scaffold begins its banner on the first output line and ends it
with `\n\n` to separate the introduction. The optional Markdown introduction
has independent `introduction_margin_left` and `introduction_margin_right`
fields in both C and Lua, passed directly to its own libmdf renderer. Both
default to zero and must be nonnegative, leaving at least three content columns
at the current terminal width. Conversation text retains its normal margin
and a blank line before content starts. The introduction goes through
libmdf's output feed before the first session opens or replays; it is
not added to model context or session history. Help, version, utilities, native
messaging, reviews and non-interactive runs print neither. The CAI scaffold
colors C/I bright white and A bright blue on a terminal, resets before Smith
and the attribution, and honors NO_COLOR and TERM=dumb for its banner.
`-h` uses the configured program name. `app->option_count/option_at` expose an
immutable array of long flags, short flags, argument types, descriptions,
repeatability and negative aliases. `app->write_help` writes CAI's formatter to
a caller-owned sink. Neither method loads configuration, logs in or starts UI.

The host must run on its main thread and serialize process-global state use.
CAI temporarily owns stdin/stdout and may change cwd, environment, locale and
signal handlers. These are restored on return, including invalid options and
runtime failures. No concurrent or recursive application runs are permitted.
The receiver may run again after returning. Close it only while idle. Worker
threads finish before return; they never call the Lua facade.

### Observing and controlling a run

`event_callback` and `replay_callback` in `cai_app_config` observe the same
runtime and journal events as the application, alongside its normal rendering
and logging. They run on the thread calling `run`. Events and their pointers
are borrowed for the callback only; copy anything retained. Replay does not run
tools or submit prompts. Return `CAI_OK`, or a CAI error with `cai_error` detail
to stop the run. Observers may use the receiver's `control` and `get_status`
methods, but must not reenter `run`, close the app, or operate on its runtime.

Console replay defaults to 20 whole turns or standalone shell entries. Set
`ui.replay_turns` through `cai_app_setting` or Lua
`settings = {ui = {replay_turns = 20}}`; `--replay-turns` and the application
environment prefix plus `REPLAY_TURNS` also apply. Zero hides replay and -1
shows all. This affects console output only: C `replay_callback` and Lua
`on_replay` still receive each retained journal event once, including omitted
console turns. Logs, exports, and the full restored agent context are unchanged.

Local execution policy is configured with `[agent] yolo`, `CAI_YOLO`, or
`--yolo`/`--no-yolo`, defaulting to false. An embedded application supplies
`{ "agent", "yolo", "true" }` as an application setting; its program-specific
environment prefix applies normally. C runtime/preset configurations and Lua
runtime/Smith tables expose `yolo`. Standalone C tool configurations and Lua's
exec/read/list registration tables expose the same opt-out. It is inherited by
reviewers and other child runtimes. `HOME` and every `XDG_*` value are preserved
in both modes; YOLO inherits all remaining environment variables too. Ordinary
user permissions, disabled tools, output retention, timeouts and cancellation
still apply.

`/yolo` inspects the live policy; `/yolo on` and `/yolo off` change it without
reopening the conversation or writing config.ini. `/status` shows the effective
mode and any queued change. Changes remain editable in Softline until the
runtime is idle: the active turn, child runtimes, shell capture and managed
terminal processes must finish first. Esc preserves Softline's stopped queue.
The selected mode applies to later tools and new children; it is not stored in
the conversation checkpoint. Restarting the application uses startup settings.

C hosts use the owner-thread `cai_agent_runtime_get_yolo(runtime, &enabled,
&can_change, error)` and `cai_agent_runtime_try_set_yolo(runtime, enabled,
&applied, error)` APIs. Busy changes return `CAI_OK` with `applied == 0`; the host
retains the request and retries at an idle boundary. A no-op succeeds immediately.
An error preserves the complete previous policy. Tool registrations, terminal
handles/output, callbacks, goals and session identity are retained. Lua exposes
`runtime:yolo()` returning `{enabled=boolean, can_change=boolean}` and
`runtime:try_set_yolo(boolean)` returning whether the request applied. A false
result means busy; an error returns `nil, error`.

Execution history is enabled by the application and is independent of pslog
levels or host logger configuration. `event_callback` / Lua `on_event` receives
`CAI_AGENT_EVENT_EXECUTION_RECORD` / `cai.AGENT_EVENT_EXECUTION_RECORD` with a
bounded JSON record in `data`; `replay_callback` / `on_replay` receives those
records with the journal type `execution`. The normal console replay limit
still applies only to presentation. No execution record is model input.

C hosts include `<cai/execution.h>`, initialize a `cai_execution_record`, call
`cai_execution_record_parse`, inspect the fields and call
`cai_execution_record_cleanup`, including on error. The optional
`cai_execution_record_write_markdown` consumes records in order and joins their
bounded payload chunks without inserting breaks inside a field. Rendering
escapes terminal controls while leaving the stored payload unchanged.

`cai_agent_runtime_export_execution_markdown(runtime, sink, error)` streams the
stored timeline at an idle boundary; Lua exposes
`runtime:export_execution_markdown(sink)`. The application combines this with
the existing conversation exporter in its human-facing `/export` and
`--export` Markdown file. Core `export_markdown` and `export_markdown_file`
remain agent-handover projections and exclude execution records.

```lua
local cai = require("cai")
-- Inside on_event, or on_replay with event.type == "execution":
if event.type == cai.AGENT_EVENT_EXECUTION_RECORD or event.type == "execution" then
  local record = assert(cai.execution_record(event.data))
  -- record.kind, tool_name, call_id, data, offset, final, code, http_status...
end
```

Kinds include `tool.started`, `tool.arguments`, `tool.completed`, `tool.output`,
`tool.failed`, `tool.partial_output`, `hosted.tool`, `subagent.input`,
`request.failed`, `response.completed`, `turn.started`, `turn.completed`,
`turn.cancelled`, `turn.failed`, `goal.changed`, and compaction
started/completed/failed. Error fields are separate ordered chunks; arguments
are exact JSON text or native custom-tool input. `offset` counts original
UTF-8 bytes and `final` ends that field, including an empty field. The source
session/sequence and parent session/call identify delegated activity. When
correlating with a parent export, use its outer journal sequence for display
order. A started call without a terminal record is incomplete, not a replay
request to execute it.

The journal durably acknowledges each record before its live notification.
Worker-side notifications use the bounded event queue; synchronous owner
operations never wait for their own pump and retain the journal if the live
queue is full. Recording failures prevent starting a tool whose arguments
could not be recorded. Deeper transport/storage implementation logs remain
with the host's pslog sink; no log-severity filter feeds execution history.

`app->control(app, type, text, &id, &error)` is safe from other host threads
while the app remains alive. It copies input into a FIFO of at most 64 waiting
commands and wakes the app loop. Calls with no ready runtime, invalid arguments,
or a full queue fail immediately, with `id` set to zero. STOP is also accepted
during startup and utility runs, without a ready runtime, and is handled when
the app reaches its control boundary.

| Type | Text | Behavior |
| --- | --- | --- |
| `CAI_APP_QUEUE_PROMPT` | Required | Interactive: inject into softline's editable approval queue, using the existing autoqueue setting. Batch: enqueue the next turn. |
| `CAI_APP_STEER` | Required | Submit steering to the active turn at the next runtime safe boundary. Reject if no turn can accept it. |
| `CAI_APP_CANCEL_TURN` | None | Cooperatively cancel the current turn and durably pause an active goal. This leaves an interactive app open; ordinary user turns work while paused. |
| `CAI_APP_STOP` | None | Stop the application, discard unstarted work, and wait for worker shutdown before returning. The host process stays alive. |
| `CAI_APP_REPLACE_GOAL` | Required | Replace the goal with an unbudgeted objective and pursue it, as `/goal new TEXT` does. |
| `CAI_APP_PAUSE_GOAL` | None | Pause an active goal at a safe boundary. |
| `CAI_APP_RESUME_GOAL` | None | Resume a resumable goal and pursue it. |
| `CAI_APP_CLEAR_GOAL` | None | Clear goal tracking without undoing completed work. |

The application pursues the runtime's active durable goal regardless of whether
it was created by a model tool, a slash command, startup options or host control.
Successful turns generate further goal continuations until the goal completes
or stops; approved queued prompts run first. Resuming an active goal from storage
also starts pursuit. Cancellation durably pauses the goal, including across
restart; provider failure stops automatic pursuit for the current invocation.
The model may pause for a user answer/approval and decide to resume a paused
goal in a later turn, respecting outstanding approvals and instructions to
remain paused. Completed goals cannot resume. Three consecutive empty automatic
turns without text, reasoning, tool or compaction activity block the goal.
This behavior is shared by the C receiver, Lua facade and CAI executable.

Goal status and the last turn's outcome are independent. Host resume/replacement
controls can start pursuit immediately after cancellation or failure; no ordinary
prompt or process restart is required. Resume also accepts an already-active
unfinished goal without resetting its timer or accounting. Completed goals
remain non-resumable. An in-flight turn continues through its safe boundary;
resume does not create a duplicate turn. Existing usage/token caps still apply.

Hosts using libcai directly can call
`cai_agent_runtime_continue_goal(runtime, &started, &error)` or Lua
`runtime:continue_goal()`. These admit an idle active goal and render the compiled
Codex-derived objective/budget/audit prompt. They report whether work started;
pending user input or goal controls take priority. Ordinary user submissions
are separate from automatic continuations and reset the empty-turn streak.

Acceptance into the app FIFO is distinct from acceptance by the runtime or
softline. `control_callback(context, id, status, error)` acknowledges each
accepted command on the run thread, outside the queue lock. This includes
commands rejected during application or abandoned at shutdown. Successful
prompt injection acknowledges queue insertion, not human approval or completion;
successful goal/steering controls acknowledge the runtime's safe-boundary request,
not its eventual effect. IDs increase within each run and restart at one on the
next run. A callback may enqueue another command. Further commands behind STOP
are rejected. A stopped run need not deliver events still pending at shutdown.
STOP also ends isolated review without emitting unfinished findings. It does
not deliver approved prompts after shutdown has been requested. An observer's
failure detail is retained even if the terminal loop reports a later I/O error.
During `/shell` or `!sh`, the child uses the terminal directly. Only the shared
variants (`/shellshare`, `!sh+`) use a PTY relay to capture output. STOP interrupts
either handoff, terminates the shell and restores the terminal before command
acknowledgement. Other host controls remain queued until handoff ends.
STOP also interrupts login's callback accept and partial-request waits. Login
and standalone sends acknowledge STOP without opening an agent runtime. Standalone
`--queue NAME --message TEXT` returns after liblockdc accepts the enqueue; a
STOP arriving during that operation is acknowledged afterward and does not
recall the committed message.
Acknowledgement callbacks must not close or recursively run the app, including
final callbacks delivered after process restoration. Close after `run` returns.

The optional model-controlled `set_reasoning` tool accepts just
`{"mode":"normal"}` and `{"mode":"high"}`. Enable it with
`[agent] reasoning_tool = true`, `CAI_REASONING_TOOL=true`, or
`--reasoning-tool`. Independent `normal_reasoning_effort` and
`high_reasoning_effort` settings default to `medium` and `high`; the
corresponding `CAI_NORMAL_REASONING_EFFORT` / `CAI_HIGH_REASONING_EFFORT`
variables and `--normal-reasoning-effort` / `--high-reasoning-effort` flags
override them. The generated INI template includes these settings.
The model is instructed to raise effort for complex tasks and only return to
normal when explicitly told. A high request never lowers a higher current
effort. Changes take effect after the durable tool round, before another model
request, and use the existing persisted request settings on resume.

Every valid tool request returns the same `ok` acknowledgement, whether it
changes effort or leaves settings unchanged. The tool does not expose the
effective effort or confirm that a change took effect. Unsupported or unavailable
advertised effort lists produce a host/user warning only; no recovery prompt or
provider request is generated for an unsupported change. Supply metadata through
the existing provider catalog when choices are unavailable.
Mappings and tool enablement come from configuration, not conversation storage;
manual `-r` and `/reasoning` remain independent of the normal mapping.
`/reasoning-tool [on|off|normal LEVEL|high LEVEL]` configures the current app
between turns, including subsequent `/new` or `/resume`, without editing INI.
Review children retain their own reasoning settings and do not inherit the tool.

C hosts initialize `cai_agent_runtime_config.reasoning_tool` with
`cai_reasoning_tool_config_init`, then set `enabled`, `normal_effort`, and
`high_effort`. The owner-thread `cai_agent_runtime_get_reasoning_tool` and
`cai_agent_runtime_set_reasoning_tool` APIs inspect or reconfigure it between
turns; setters copy strings. Lua hosts pass
`reasoning_tool={enabled=true, normal_effort="medium", high_effort="high"}`
to `client:new_smith_runtime` and use `runtime:reasoning_tool()` or
`runtime:reasoning_tool{enabled=false}` to inspect or update it.

```c
cai_app_status status;
unsigned long long id;
cai_app_status_init(&status);
/* From a host thread while the run has a ready runtime: */
rc = app->control(app, CAI_APP_QUEUE_PROMPT, "Check the result", &id, &error);
rc = app->get_status(app, &status, &error);
/* status.session_id, status.effective, status.pending, status.goal,
 * status.metrics, status.control_id/control_error, status.outcome/error */
cai_app_status_cleanup(&status);
```

Status getters copy an owned snapshot under a mutex; they never call into the
runtime from a host thread. Initialize the output before first use. A successful
query replaces it; a failed query leaves it unchanged. All its strings and error
fields survive later queries, runs and application close, until snapshot cleanup.
Do not shallow-copy a snapshot or modify its owned pointers.

Snapshots describe the most recent owner-thread safe boundary; an observer may
see the previous boundary while its event is being processed. `running` and
`runtime_ready` distinguish startup/utilities, an open runtime, and a finished
invocation. The final snapshot retains the session ID, effective/pending model,
reasoning and tier settings, goal projection, cumulative token/cost accounting,
context usage, turn timing, outcome and CLI exit status. Paused, blocked and
other goal outcomes are in `goal.status`. Failures retain `cai_error` fields
where available; runtime failure events provide their diagnostic text. The last
control's status/error is separate from run failure. Assistant output stays
incremental through the event observer rather than being accumulated in status.
`error` can also retain a recovered interactive diagnostic; use outcome and exit
status to decide whether the invocation failed.
After STOP or an observer error, runtime fields retain the last available
boundary before shutdown; `outcome` and `running` describe the completed app
invocation even if that interrupted runtime snapshot still shows an active turn.

A later accepted run resets status and command IDs; an invocation rejected
before acquiring application ownership leaves the previous result intact.
Keep the receiver alive until host threads have stopped using controls/status;
`close` itself is idle-only and must not race other receiver calls. The new
application surface remains ABI 0, independently of the core library's ABI 5.

## Lua 5.5

With the application and Lua surfaces selected, `require("cai.app")` exposes
the same receiver and ownership boundary:

```lua
local app = assert(require("cai.app").open {
  program_name = "smith",
  invocation_name = "vectis -a smith --",
  banner = "Vectis Agent Smith\n\n",
  introduction_markdown = "**Enter** sends a task; `/help` lists commands.\n",
  introduction_margin_left = 0,
  introduction_margin_right = 0,
  settings = {agent = {identity = "Vectis Agent Smith"}},
  -- logger = initialized_pslog_logger,
})
local code, err = app:run {"-Nni", "Explain this workspace"}
app:close()
assert(code, err)
```

`app:options()` returns metadata entries, `app:help()` returns formatted text,
and `app:close()` is idempotent. Invalid operations return `nil, error`.
The logger is retained through its public pslog Lua interop until close.
Only loggers advertising pslog's native-sink capability are accepted, because
the application and its dependencies log from native worker threads. Lua
function sinks are rejected before starting work.
The complete executable example is `examples/app.lua`.

Set `on_event = function(event) ... end` and `on_replay = function(event) ... end`
in the open configuration to observe runtime and saved journal events. Lua
receives copied tables on its owning run thread. Callback errors stop that run
and appear in `app:get_status().error`, after process state is restored.
Callbacks remain collectable even when they capture the application itself.

`app:control(name, text)` returns a command ID or `nil, error`. Names are
`queue_prompt`, `steer`, `cancel_turn`, `stop`, `replace_goal`, `pause_goal`,
`resume_goal`, and `clear_goal`; text requirements match the C table above.
`app:get_status()` returns a copied table with the C snapshot's fields. Outcome
strings are `not_run`, `running`, `completed`, `stopped`, `cancelled`, and
`failed`. The last command acknowledgement is in `control_id`, `control_status`
and `control_error`. Lua callbacks may queue controls or query status; they
cannot close or recursively run the active application. Native custom-tool
functions remain a C-only hook because their handlers execute on workers.
`state` is the numeric C `cai_agent_run_state` value (see `<cai/agent_runtime.h>`).
In Lua, `metrics.session_usage` contains token counters directly alongside
`estimated_spend_usd`, `estimated_spend_eur`, and `limit_exceeded`; it does not
have the C accounting struct's nested `usage` member. `app:options()` returns
a 1-based array of copied option records; absent short flags/argument labels
are `nil`. Neither query starts a run or reads configuration files.

The optional native module installs with component `cai-app-lua` under
`lib/lua/5.5/cai/app.so`. Add that prefix's `lib/lua/5.5/?.so` to `LUA_CPATH`,
or compile `lua/app_lua.c` against the installed public `cai-app` SDK and Lua
5.5/lua-pslog headers. `lua/CMakeLists.txt` builds this module when pkg-config
finds `cai-app`; its install target places the module in the same Lua layout.
Both Lua modules use installed public SDK headers and shared libraries;
neither requires CAI's private application or runtime headers.
The ordinary core `cai` LuaRock stays usable without libcai_app. Its source
archive includes the optional application facade for native embedders.
`make lua-app-test` builds this optional surface against the native application
and runs its ownership, error, export and mock-provider embedding checks.

The six Linux x86_64/aarch64/armhf GNU/musl SDK presets each include `libcai_app.a`,
`libcai_app.so` (ABI 0), the public header and CMake/pkg-config metadata. They
ship inside the corresponding `cai-<version>-<target>.tar.gz` SDK archive:

| Architecture | glibc preset | musl preset |
| --- | --- | --- |
| x86_64 | `x86_64-linux-gnu-release` | `x86_64-linux-musl-release` |
| aarch64 | `aarch64-linux-gnu-release` | `aarch64-linux-musl-release` |
| armhf (ARMv7 hard float) | `armhf-linux-gnu-release` | `armhf-linux-musl-release` |

Other SDK targets retain core libcai only. Application dependency pins are recorded
in `share/cai_app/dependencies.json`; core dependency metadata remains in
`share/cai/dependencies.json`. The standalone binary archive includes the C/Lua
embedding examples and this guide alongside `bin/cai` and `cai(1)`.

Chrome native-host registration records the current executable path. An
embedding executable that offers `--chrome-setup` must also dispatch Chrome's
extension-origin invocation to CAI's run entrypoint, or directly to the public
`cai_chrome_native_host_run` API, rather than its normal host option parser.

## Conversation messaging

The application includes optional shared-Pouch messaging (default off) with user approval through
softline's normal prompt hook. Configure it through C app settings or Lua
`settings = {messaging = {enabled = true, enable_auto_enqueue = false,
ack_timeout_ms = 300000, queue_name = "smith"}}`. Omit `queue_name` to derive
a stable name allocated from the basename and the fewest distinguishing
ancestor components, with the actual hostname as a leading component.
The newest app start owns the
named mailbox; old delivered messages stay in their original conversation.
`/queue on` and `/queue off` enable or disable messaging between turns in the
same conversation. `/queue NAME` overrides the full name; `/queue` shows it.
`/status` includes the queue and approval mode. These live settings survive
`/new` and `/resume`, without changing config.ini.
`storage.messaging_store` selects a named `store.NAME` backend independently
of `storage.sessions_store` and `storage.auth_store` (all default to `default`).
Each store defines `endpoint`, pouch `key_file`, and optional HTTP `client_pem`.
In C these are ordinary `cai_app_setting` section/key/value entries. In Lua use,
for example, `settings = {["store.team"] = {endpoint = "pouch:///srv/agents/pouch",
key_file = "/srv/agents/pouch.key"}, storage = {messaging_store = "team"}}`.
Default-store key configuration is `[store.default] key_file`; the C
`pouch_key_file` field still seeds that default.

Clients live for the app run and its nested contexts, not individual turns or
reopened sessions. The process shares one client per physical pouch root, and
the final owning context closes it. App-owned logging state is retained with
the client. Externally supplied loggers and outputs remain borrowed and must
outlive the store contexts using them. SDK diagnostics retain the opening
context's sink until that client closes; supply the same host logger when
independent contexts need a common destination. Liblockdc owns reconnect behavior.

`--temp` (or `settings = {ui = {temporary = true}}` in Lua) creates a private
cache workspace and conversation store without rewriting the normal config.
Relative forwarded input paths resolve against the original selected workspace
before entering the temporary workspace; INI paths remain relative to the
configuration file.
Auth and messaging retain their selected stores. Teardown closes workers,
clients and softline, restores the preceding working directory, and deletes
private history, prompt history and managed cache data. The workspace deletion
question defaults to No. Non-interactive runs, EOF, errors, and host STOP
preserve the workspace unless `--yes` / `--force` is passed. STOP also interrupts
the final question. See the [temporary workspace and store configuration](../README.md).

See [messaging](agent-messaging.md) for delivery,
editing, ACK deadlines, and core C/Lua next-turn integration APIs.

## Darwin port assessment

Core libcai already has a Darwin arm64 SDK. All four application dependencies
also publish Darwin arm64 SDKs at the pinned versions. The application remains disabled
for Darwin until these boundaries are implemented and verified:

| Boundary | Current implementation | Port work |
| --- | --- | --- |
| Worker/log notifications | Linux `eventfd` | Use a nonblocking pipe or Darwin notification backend; keep softline's descriptor-watch interface |
| Turn/status and shell deadlines | Linux `timerfd` | Provide monotonic timers with idle disarming and cancellation; preserve existing deadlines |
| Shell handoff | `pipe2`, `ptsname_r`, Linux PTY setup | Use Darwin PTY and descriptor setup while preserving terminal restoration, child cleanup and bounded capture |
| Host environment restoration | GNU `clearenv`, process `environ` | Use Darwin `_NSGetEnviron()` and portable environment clearing/restoration without changing the host ownership contract |
| Agent command sandbox | Core exec already selects experimental `sandbox-exec` on Darwin | Verify Smith's tool defaults, permissions and failure behavior on a native Mac; Linux cgroups remain unavailable |
| Chrome native host | Linux host/setup code; Darwin currently returns unsupported | Implement executable/manifest discovery, socket flags and peer credentials, or explicitly keep this capability unavailable |
| Shared/module packaging | ELF `.so`, GNU linker flags | Add Mach-O `.dylib` identities, export rules and the appropriate Lua module linking rules |

Recommended implementation: keep one application runtime and put notification,
timer, PTY and environment differences behind small private platform helpers.
Local osxcross SDK probes confirm the missing `sys/eventfd.h`, `sys/timerfd.h`,
`clearenv` and `pipe2` interfaces. The port requires source changes.
Use the existing osxcross lane for build/link/export/relocation checks, then a
native macOS host for real softline input, resize, shell handoff, cancellation
and repeated embedded-run checks. Cross compilation cannot establish that
terminal behavior. See the [roadmap](../ROADMAP.md) for the deferred port.

## Shared kanban and HTTP routes

Use `cai_app_kanban_open` to open CAI's encrypted shared board engine independently
of the terminal. It returns the core `cai_todo` receiver; `handle_http` serves an
embedded read-only page, assets and live snapshots. The equivalent Lua helper is
`require('cai.app').kanban`. See [shared kanban](kanban.md) for C/Lua ownership,
routing, streaming, configuration and per-conversation selection.
