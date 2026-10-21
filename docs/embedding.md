# Embedding the CAI application

`libcai_app` contains the complete CAI terminal application: softline/libmdf,
configuration, encrypted sessions/auth, tools, MCP, slash commands, review and
non-interactive work. Core `libcai` remains independently usable without the
application dependencies. The application supports Linux x86_64, aarch64 and armhf,
with both glibc and musl. The standalone `cai` binary remains Linux x86_64 musl.

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
  config.banner = "Vectis Agent Smith";
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
Precedence is defaults, file/environment, explicit settings, then arguments.
Environment entries retain the normal `config_env_has_precedence` policy.
`[paths]`, `[storage]`, and `[env]` XDG variables provide host path overrides.
Configuration strings/settings are copied by open. Logger ownership stays with
the host until close; CAI neither closes nor reconfigures a supplied logger.
With a supplied logger CAI does not create its default log files or intercept
that logger's records for interactive notices. Use a terminal-compatible sink.

An interactive banner is printed once before softline starts. Help, version,
utilities, native messaging, reviews and non-interactive runs do not print it.
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

## Lua 5.5

With the application and Lua surfaces selected, `require("cai.app")` exposes
the same receiver and ownership boundary:

```lua
local app = assert(require("cai.app").open {
  program_name = "vectis -a smith --",
  banner = "Vectis Agent Smith",
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
