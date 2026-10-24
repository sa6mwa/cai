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
`cai::app_shared`, or pkg-config `cai-app`. The new shared library has ABI 0;
it does not change the core libcai ABI.

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
  config.program_name = "vectis -a smith --";
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

`app->control(app, type, text, &id, &error)` is safe from other host threads
while the app remains alive. It copies input into a FIFO of at most 64 waiting
commands and wakes the app loop. Calls with no ready runtime, invalid arguments,
or a full queue fail immediately, with `id` set to zero. STOP is also accepted
during startup and is handled when the app reaches its control boundary.

| Type | Text | Behavior |
| --- | --- | --- |
| `CAI_APP_QUEUE_PROMPT` | Required | Interactive: inject into softline's editable approval queue, using the existing autoqueue setting. Batch: enqueue the next turn. |
| `CAI_APP_STEER` | Required | Submit steering to the active turn at the next runtime safe boundary. Reject if no turn can accept it. |
| `CAI_APP_CANCEL_TURN` | None | Cooperatively cancel the current turn. This leaves an interactive app open. |
| `CAI_APP_STOP` | None | Stop the application, discard unstarted work, and wait for worker shutdown before returning. The host process stays alive. |
| `CAI_APP_REPLACE_GOAL` | Required | Replace the goal with an unbudgeted objective and pursue it, as `/goal new TEXT` does. |
| `CAI_APP_PAUSE_GOAL` | None | Pause an active goal at a safe boundary. |
| `CAI_APP_RESUME_GOAL` | None | Resume a resumable goal and pursue it. |
| `CAI_APP_CLEAR_GOAL` | None | Clear goal tracking without undoing completed work. |

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
application surface remains ABI 0: it has no preceding released app ABI.

## Lua 5.5

With the application and Lua surfaces selected, `require("cai.app")` exposes
the same receiver and ownership boundary:

```lua
local app = assert(require("cai.app").open {
  program_name = "vectis -a smith --",
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

The optional native module installs with component `cai-app-lua` under
`lib/lua/5.5/cai/app.so`. Add that prefix's `lib/lua/5.5/?.so` to `LUA_CPATH`,
or compile `lua/app_lua.c` against the installed public `cai-app` SDK and Lua
5.5/lua-pslog headers. `lua/CMakeLists.txt` builds this module when pkg-config
finds `cai-app`; its install target places the module in the same Lua layout.
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
a stable basename plus canonical-path hash. The newest app start owns the
named mailbox; old delivered messages stay in their original conversation.
`/queue NAME` overrides the full name for this app between turns; `/queue`
shows it. The override survives `/new` and `/resume`, without changing config.ini.
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
