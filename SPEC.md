# Loose spec

`cai` is to be a full-featured OpenAI responses API client supporting both regular http/https use as well as full WebSocket use. Both text and images should be supported for inference to the extent supported by the OpenAI responses API.

The stack requirements and constraints:

- C89 (-std=c89) with POSIX support (enabled via POSIX macros)
- No strcpy or sprintf usage, use snprintf, etc even if that is not standard C89 (if used at all)
- For dependencies, tie those to openssl, curl, nghttp2 from `liblockdc` (they contain .a and .so libs as well as the headers for these):
  * https://github.com/sa6mwa/liblockdc/releases/download/v0.3.0/liblockdc-0.3.0-x86_64-linux-gnu.tar.gz
  * https://github.com/sa6mwa/liblockdc/releases/download/v0.3.0/liblockdc-0.3.0-x86_64-linux-musl.tar.gz
  * https://github.com/sa6mwa/liblockdc/releases/download/v0.3.0/liblockdc-0.3.0-armhf-linux-musl.tar.gz
  * https://github.com/sa6mwa/liblockdc/releases/download/v0.3.0/liblockdc-0.3.0-armhf-linux-gnu.tar.gz
  * https://github.com/sa6mwa/liblockdc/releases/download/v0.3.0/liblockdc-0.3.0-arm64-apple-darwin.tar.gz
  * https://github.com/sa6mwa/liblockdc/releases/download/v0.3.0/liblockdc-0.3.0-aarch64-linux-musl.tar.gz
  * https://github.com/sa6mwa/liblockdc/releases/download/v0.3.0/liblockdc-0.3.0-aarch64-linux-gnu.tar.gz
  * https://github.com/sa6mwa/liblockdc/releases/download/v0.3.0/liblockdc-0.3.0-CHECKSUMS
- Use `lonejson` that is distributed with `liblockdc` for all JSON-related work regarding the API. lonejson has callbacks to `libcurl` making it a perfect match for serious streamed and memory-efficient JSON API work
- It should be possible to build against the OS/distro's openssl and curl libraries, but we develop against the liblockdc SDK distribution in `cai`
- Build and release for exactly the same architecture and OSes that liblockdc ship for: linux x86_64/armhf/aarch64 and gnu/musl as well as darwin arm64
- tarballs should be produced by `make release` and packaged under `dist/` with tar --owner=0 --group=0 and if the HEAD git commit has a v-tag on it, e.g v0.1.0 it should be the version of the tarballs and any version macros inside the header files, e.g: dist/cai-0.1.0-x86_64-linux-gnu.tar.gz
- Lua bindings for liblockdc and the downstream luas are available here, but I think :
  * https://github.com/sa6mwa/liblockdc/releases/download/v0.3.0/lockdc-0.3.0-1.rockspec
  * https://github.com/sa6mwa/liblockdc/releases/download/v0.3.0/lockdc-0.3.0-1.src.rock
  * https://github.com/sa6mwa/lonejson/releases/download/v0.4.1/lonejson-0.4.1-1.src.rock
  * https://github.com/sa6mwa/lonejson/releases/download/v0.4.1/lonejson-0.4.1-1.rockspec
  * https://github.com/sa6mwa/lonejson/releases/download/v0.4.1/lonejson-0.4.1-CHECKSUMS
  * https://github.com/sa6mwa/libpslog/releases/download/v0.3.1/lua-pslog-0.3.1-1.rockspec
  * https://github.com/sa6mwa/libpslog/releases/download/v0.3.1/lua-pslog-0.3.1-1.src.rock
- libpslog is shipped with liblockdc, there should (as with liblockdc) be a client logger functionality that (if enabled) log through pslog at various levels regarding what is happening inside `cai`. Levels are trace, debug, info, warn and error. Logging from inside cai should be granular to over all of those levels. Error and Trace are probably easiest to classify (error is pretty self-explanatory and trace is essentially every possible relevant metric), while debug, info, and warn are where judgment have to decide what the right level is. Info should not be Debug, debug should aid developers while info should hint operations while warn should obviously involve everything that is non-fatal and not a clear error, could also be warnings propagated from the openai api e.g credit limits, rate limits, etc.
- lockdc lua does not expose a possibility to provide a custom pslog logger, so there is no example of that in the Lua binding for lockdc. What I want is instead something of a C-only factory facade or something that sets up a default context for the module where you can specify a logger (this is obviously not called from Lua, but from C and Lua never sees the logger). Essentially, the host will own the pslog instance. That is perhaps a good preparation for exporting the lua module to where it will endup in the next phase when `cai` is done - into vectis, see ../vectis/. Vectis will bundle several lua sources and then it will be possible to send a pslog logger in C only throughout all of the lua modules that Vectis ships with its framework platform (and then we don't need to go up/down Lua/C for pslog as that logger gets shared across several lua facades from the back by the C layer that sits above lockd client, pslog, lonejson, and cai).

## OpenAI API

I don't know if there is an OpenAPI Spec for the responses API, but the documentation can be found here: <https://developers.openai.com/api/reference/responses/overview>

## CAI API Surface

The SDK API should **not** be a transport API, it should be a proper facade with excellent Developer Experience (DX) usage. The original preference was for handler-based usage with attached function pointers, but the implementation has settled on opaque handles with free functions, for example `cai_client_new_agent(client, ...)` and `cai_session_send_text(session, ...)`. This keeps state scoped to handles while making the C API easier to wrap from Vectis and Lua.

It should be simple to setup system prompt, etc and perhaps add an *agent* layer making this more steered towards agentic work where we can register tools with the agent.

## MCP support

We are to add MCP support to `cai` eventually, but that is out-of-scope for the initial implementation. The first implementation must however support the standard tools flow with callback handlers into C for the actual tool execution.

## Lua binding

As with `liblockdc`, `lonejson`, etc, `cai` should have a Lua binding for the entire C SDK API and should be distributed as a LuaRock (put Lua under `lua/` and the luarock spec and src rock should be built under `dist/` as with the SDK tarballs).
