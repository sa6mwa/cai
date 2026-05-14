# TODO

Additions to [PLAN.md](PLAN.md)

- [x] Support openrouter.ai, we have OPENROUTER_API_KEY for https://openrouter.ai/api/v1
- [x] Find the cheapest model supporting the responses API via openrouter.ai
- [x] Investigate if this model via openrouter supports the responses API: https://openrouter.ai/nvidia/nemotron-3-nano-omni-30b-a3b-reasoning:free
- [x] Switch the OpenRouter go-to e2e model to the free model that actually
  passes cai continuity and tool-calling regressions.

Implemented first slice:

- `cai_client_config_use_openrouter()` selects `OPENROUTER_API_KEY` and
  `https://openrouter.ai/api/v1`.
- `CAI_OPENROUTER_MODEL_DEFAULT_RESPONSES` is
  `poolside/laguna-xs.2:free`.
- OpenRouter registry data currently reports that model as zero-price for
  prompt/completion tokens, 131k context, and supporting Responses, reasoning,
  and tool calling. It is the default because it passes cai's Responses,
  tool-calling, and 20-turn client-history e2e regressions.
- `CAI_INTEGRATION_OPENROUTER_DOTENV=1` verifies the OpenRouter `.env` key
  loading path by clearing the inherited `OPENROUTER_API_KEY` process envvar
  before opening the client.
- `CAI_INTEGRATION_OPENROUTER_E2E=1` runs the same 20-turn continuity eval as
  the OpenAI e2e path, using OpenRouter's default free Responses model and
  cai's client-side history replay mode.
- `nvidia/nemotron-3-nano-omni-30b-a3b-reasoning:free` remains enumerated, but
  it is not the default because it failed the 20-turn continuity e2e by
  repeating the first response on the second turn.
- `poolside/laguna-xs.2:free` is the go-to OpenRouter model for e2e testing.

Current OpenRouter boundary:

- Do not claim OpenRouter parity for Conversations or server-side compaction
  semantics until those specific behaviors are documented or separately proven.
- OpenRouter Responses beta is stateless, so cai uses client-side history
  replay for multi-turn OpenRouter sessions. Server-side continuation remains
  the cai default for OpenAI.

## MCP HTTP Test Server And Tool Presets

Keep this sequence ordered. Refine each item into a tighter spec immediately
before implementing that slice.

- [x] Add a tiny HTTP test server for MCP Streamable HTTP.
  - Compile it only as a test/example utility; never link it into `libcai`.
  - Serve the existing `cai_mcp_handler` on `/mcp`.
  - Bind to `127.0.0.1`.
  - Use `18765` as the manual default port because common local ports such as
    `8080` are often occupied on this host.
  - E2E tests should bind port `0`, read/report the selected port, and avoid
    fixed-port conflicts.
  - Keep it plain HTTP with no authentication.
  - Keep MCP origin validation enabled.

- [x] Add MCP compliance/e2e coverage using the official MCP Inspector CLI.
  - Prefer the official `@modelcontextprotocol/inspector` CLI against the test
    server's Streamable HTTP endpoint.
  - Cover at minimum `initialize`, `tools/list`, and `tools/call`.
  - Use this to validate the HTTP route boundary, not just direct
    `cai_mcp_handler_handle_http()` calls.
  - If the Inspector CLI path is brittle in CI/local containers, evaluate the
    official MCP TypeScript SDK client as the fallback conformance client.

- [x] Add a production reverse-geolocation tool preset.
  - Public header: `include/cai/tools/revgeo.h`.
  - Implementation: `src/tools/revgeo.c`.
  - Register as a normal cai typed tool; MCP exposure remains just a registry
    concern.
  - First release should work out of the box with a free unauthenticated public
    JSON API, matching the current example-level reverse-geocoding approach.
  - API-key/authenticated provider variants can come later.
  - Tool should accept location/coordinate input shape decided in the slice
    spec and return structured location metadata suitable for downstream
    weather tools.

- [x] Add a production todo/kanban tool preset.
  - Public header: `include/cai/tools/todo.h`.
  - Implementation: `src/tools/todo.c`.
  - Register as a normal cai typed tool; MCP exposure remains just a registry
    concern.
  - Persist active boards/lists in one JSON file.
  - Persist the done archive in a separate JSON file.
  - Both paths are configurable in the tool constructor/config.
  - Support multiple kanban boards.
  - Active statuses are `todo` and `in_process`.
  - Done items move to the separate done archive.
  - Each board can have an optional WIP limit for the `in_process` lane.
  - Default WIP limit is unset.
  - Moving an item into `in_process` must be denied with a structured tool
    result when the board's WIP limit would be exceeded.
  - Tool-generated opaque IDs are the default. The agent discovers IDs by
    querying/listing boards, then uses those IDs for follow-up operations.
  - IDs should be unique across active boards and the done archive. Prefer a
    time-based, conflict-resistant ID shape such as xid/ULID-style text, while
    keeping the exact implementation C89/POSIX-friendly.
  - Initial operation surface to refine before implementation:
    create board, configure WIP limit, add item, list/query board, move item,
    archive/complete item, and query current work.
  - Implemented as an object-framed JSON record stream so lonejson can parse
    and rewrite one board/item record at a time without materializing the whole
    store. lonejson 0.8.0 provides selected-array read streaming; a future
    streaming array rewriter is still needed before cai can move this to a
    single-document JSON array format without losing the memory bound.

- [ ] Add an example MCP server.
  - Directory: `examples/mcp-server/`.
  - Reuse the tiny HTTP server substrate or a small example-specific wrapper;
    do not add an HTTP server dependency to `libcai`.
  - Serve these tools through MCP:
    reverse geolocation preset, todo preset, SMHI weather example tool, and
    Linux clipboard example tool.
  - SMHI weather can remain example-only and should use the reverse-geolocation
    preset where useful.
  - Clipboard tool is Linux/X11-only and example-only.
  - Enable the clipboard tool by default only when `xclip` is found in `PATH`.
  - Clipboard implementation must pipe the tool input to
    `xclip -selection clipboard` via `fork`/`exec` or `posix_spawn`, not via a
    shell.
  - Clipboard input must be size-limited and documented as local-machine side
    effect behavior.
