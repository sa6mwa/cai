# Chrome through the ChatGPT extension

CAI can control Chrome using the official **ChatGPT** extension
(`hehggadaopoacecdllhhajmbjkdcmajg`). Native-host setup is explicit; Smith's
browser tool is enabled by default and connects lazily. CAI supplies
its own native-messaging host in the same statically linked `cai` executable;
no OpenAI executable, Node runtime, Playwright, or remote debugging port is
required. Nothing from OpenAI's proprietary browser bundle is distributed.

## Protocol evidence

The CDP request contract was checked against the bundled OpenAI Chrome client
`26.928.21956` and the official extension `1.26.1020.11845`, downloaded from
Google's extension update service on 2026-10-27. In the extension's
`background.js`, the CDP bridge calls `chrome.debugger.sendCommand` with
`target`, `method`, and **`commandParams`**. The bundle uses that same field.
The outer JSON-RPC object still uses `params`; CAI's raw `cdp` tool argument
also remains `params`. CAI translates that argument into the extension's
`commandParams` field rather than nesting another `params` field.

```json
{"jsonrpc":"2.0","id":"example","method":"executeCdp","params":{"session_id":"example","turn_id":"1","session_context":"live","target":{"tabId":7},"method":"Runtime.evaluate","commandParams":{"expression":"document.title","returnByValue":true}}}
```

The inspected extension accepts `claimUserTab` and `attach` with `tabId` and
session/turn identity. Claiming owns a tab; attaching enables its debugger.
CAI claims or creates a tab before attaching it. The extension enforces tab
ownership before executing CDP commands. Void handlers may omit `result` from
their serialized reply; CAI handles that only for pending void-capable methods.

The native-host fixtures enforce the CDP argument key and check command values,
paired mouse/key events, empty argument objects, escaped text, errors, cleanup,
and screenshot delivery through C, Lua and the CLI. These checks do not replace
acceptance with an installed Chrome instance. An unexplained attach EOF still
requires the native-host error and installed extension version.

## Setup

Install the ChatGPT extension from the Chrome Web Store. Install the CAI
binary at its permanent location, then run:

```sh
cai --chrome-setup
# Open the ChatGPT extension in Chrome to establish its native connection.
cai --chrome
```

Setup writes a user-level `com.openai.codexextension.json` in
`${CHROME_CONFIG_HOME:-${XDG_CONFIG_HOME:-$HOME/.config}}/google-chrome/NativeMessagingHosts/`.
The manifest points to the current executable; Chrome appends the extension
origin when launching it, which selects CAI's native-host mode. Keep the
binary at that location. Run setup again after moving the binary, after
explicitly removing its old registration.

The extension fixes the host name to `com.openai.codexextension`. Setup
**never overwrites another registration**, including OpenAI's own host, an
unrecognized manifest, or a symlink. Removal (`cai --chrome-remove`) likewise
only removes a manifest verified to belong to this executable. Repeating
setup/removal is harmless. Registration is published atomically after writing
and syncing, and a competing installer cannot be overwritten.
Before publishing a new user registration, setup also refuses to shadow a
host in Chrome's or Chromium's standard system-wide registration directory.

If another host is registered, CAI reports its manifest path and the executable
it points to. Keep that registration, or deliberately remove it before
installing CAI's host. There is no force-overwrite option or automatic takeover.
The native host supports browser control; it does not replace OpenAI's Codex
side-panel/app-server functionality. A browser using this registration cannot
also use a different host under the same name in that configuration location.

For Chromium or a different native-host location, explicitly select the file:

```sh
cai --chrome-setup --chrome-manifest /absolute/browser/NativeMessagingHosts/com.openai.codexextension.json
cai --chrome-remove --chrome-manifest /absolute/browser/NativeMessagingHosts/com.openai.codexextension.json
```

Setup does not launch Chrome, modify the extension, open sessions, or read
provider credentials. Normal startup never installs/removes a registration.

## Connections and browser tools

Smith runtimes and CAI enable Chrome tools by default, connecting only on the
first browser request. Use `--no-chrome`, `CAI_CHROME=false`, or
`[chrome] enabled=false` to disable them. No registration or browser is needed
for an ordinary turn. Setup remains explicit. An unavailable host fails the
tool call with setup guidance; a later call may discover a newly connected
host. Sent operations are never automatically retried.

Each Chrome extension connection creates a private socket named `<pid>.sock`
in `$XDG_RUNTIME_DIR/cai/chrome`, falling back to
`${XDG_CACHE_HOME:-$HOME/.cache}/cai/chrome` when the runtime directory is unset.
Embedding applications replace `cai` with their `program_name`; their configured
cache directory supplies the fallback. `[chrome] socket_directory` overrides
both locations. Default CAI applications and default SDK clients share the same
discovery directory.
Directories and sockets are private to the current user. Discovery ignores
stale sockets and requires exactly one live endpoint; with multiple Chrome
instances use `cai --chrome-socket /absolute/path/to/<pid>.sock`. This also
enables Chrome tools. CAI verifies socket ownership, peer UID on Linux, and
that the connected backend identifies itself as a Chrome extension.

The native host accepts one CAI client at a time. It services extension
heartbeats even when the agent is idle or a client sends a fragmented frame.
Native messaging stdout is reserved for framed JSON; host diagnostics always
use stderr (`LOG_OUTPUT` is ignored in this dedicated host process).
The agent keeps the usual interactive log-file or non-interactive pslog policy.

Some extension operations return JavaScript `undefined`, so their serialized
success replies contain an ID but no `result` member. CAI accepts this for
known void-capable methods: `attach`, `detach`, `markTab`, `finalizeTabs`,
`nameSession`, `turnEnded` and `executeCdp`. Native and Lua callers receive JSON
`null`. Identity and tab-data methods still require their result member;
screenshots still require actual image data. Reply IDs, protocol versions,
notifications and error objects retain their validation.
After replacing the registered executable, reload the ChatGPT extension or
restart Chrome so its native-host process runs the new code.

The model receives a `chrome` function tool:

| Action | Parameters and behavior |
| --- | --- |
| `tabs` | List user tabs. |
| `open` | Create and attach a tab; optional HTTP(S) `url`. |
| `claim` | Claim and attach an existing `tab_id`. |
| `navigate` | Navigate owned `tab_id` to HTTP(S) `url`. |
| `snapshot` | Return URL, title, bounded visible text, and links. |
| `screenshot` | Attach a PNG screenshot of `tab_id` as typed image input. |
| `click` | Click viewport `x`, `y` on owned `tab_id`. |
| `type` | Insert `text` into the focused element. |
| `key` | Dispatch key down/up using a CDP key name in `text`. |
| `scroll` | Scroll at `x`, `y` using `delta_y`. |
| `evaluate` | Evaluate JavaScript in `text`; return its value. |
| `cdp` | Execute advanced CDP `method` with object `params`. |
| `keep` | Mark an owned tab deliverable so it survives cleanup. |
| `close` | Close an owned tab. |

Navigation is asynchronous: observe the page again before acting on it.
Existing tabs must be claimed before debugger operations. The extension
rejects tabs owned by another browser session. Browser actions use CAI's
normal tool execution behavior; enabling Chrome grants the agent access to
that browser session. CAI does not introduce per-origin flags or approval UI.

The runtime issues `turnEnded` after success, failure, or cancellation. The
native host first detaches the tabs successfully attached for that session,
then forwards `turnEnded` to release ownership. Agent-created tabs close unless
marked to keep; detaching a kept tab does not close it. A user-cancelled debugger
may already be detached: cleanup still attempts the remaining tabs and sends
`turnEnded`, logging any detach failures. The host uses the same sequence if a
client disconnects, waiting for an in-flight operation to return first.
The cleanup sequence has one five-second budget, not a separate budget per tab.
If the extension disappears, CAI reports
that failure; it cannot confirm browser-side cleanup after losing the extension.
Review runtimes reject browser access, and subagents do not inherit it.
Replaying a saved session never re-executes browser actions.

Screenshots use the existing typed image delivery path for both Responses and
Chat Completions. Base64 is not returned as ordinary tool text or retained in
durable history. A model/provider must support image input to inspect one.
Browser page contents are untrusted input.

## libcai and Lua

Include `<cai/chrome.h>`, open a `cai_chrome` using an explicitly selected socket
or private discovery directory, and pass it as `cai_agent_runtime_config.chrome`.
The runtime borrows it exclusively until close and handles turn identities and
cleanup. Close the runtime before closing the browser connection.
Low-level `cai_chrome_request` accepts complete extension parameters; callers
supply the required session/turn metadata and are responsible for `turnEnded`.
Use `cai_chrome_result_free` to release returned JSON.
Smith's runtime can instead own its default lazy connection; set
`disable_chrome` to opt out or `chrome_config` to customize discovery/limits.
`cai_chrome_open_lazy` also exposes this behavior directly; `cai_chrome_open`
continues to perform eager discovery and identity validation. Custom presets
must grant `CAI_AGENT_PRESET_TOOL_CHROME` to acquire or borrow a browser.
Children and isolated reviewers receive no default browser.

Lua Smith runtimes own their browser connection when configured with a table:

```lua
local runtime = assert(client:new_smith_runtime({
  workspace_directory = workspace,
  chrome = { socket_directory = private_chrome_socket_directory },
}))
-- Submit and pump as usual. runtime:close() also closes its browser connection.
```

Both transports use bounded, **buffered** native-messaging frames, not a
streaming JSON API. Requests are limited to 1 MiB. CAI's native host limits
responses to 16 MiB; the library client defaults to the same limit and accepts
up to 64 MiB when configured for another compatible host.
Requests default to a 30-second deadline, with a 60-second maximum.
Fragmented-frame/blocked-output deadlines in the host are
five seconds. Errors report their extension message and JSON-RPC code.
Transport/protocol failures invalidate the connection and commands are never
automatically retried, since they may already have executed.

## Compatibility and verification

This integration implements the extension's internal browser protocol,
researched from the official extension and a supplied browser bundle. It is
not a documented, stable OpenAI SDK. Extension updates may require adapting
CAI's implementation.
The research baseline is ChatGPT extension version `1.26.901.11451`.

Offline tests simulate the extension over the actual native-host stdio pipes,
then exercise the full CAI runtime and model transport. They cover registration
conflicts, protocol errors, fragmentation, heartbeats, cancellation, disconnect
cleanup, browser commands, and typed screenshot delivery. They do not launch
or control a live browser. A first live Chrome check remains necessary before
claiming compatibility with an installed extension version.
