# MCP Server Example

This example runs a foreground MCP-over-HTTP server on loopback. It is test and
integration infrastructure, not a daemon.

Build and run:

```sh
cmake --build --preset debug --target cai_example_mcp_server
./build/debug/cai_example_mcp_server --port 18766
```

Useful flags:

```sh
./build/debug/cai_example_mcp_server --port 0 --print-port
./build/debug/cai_example_mcp_server --port 18766 --requests 10
```

The server exposes:

- `POST /mcp` for MCP JSON-RPC requests. When the client asks only for
  `text/event-stream`, successful JSON-RPC responses are returned as a single
  SSE event.
- `GET /health` for a small JSON health response.
- `GET /mcp` for a lightweight SSE stream heartbeat.

Logs are written with libpslog to `stderr`. `--print-port` writes only the
selected port to `stdout`, which keeps scripts and tests simple.

The server registers:

- `reverse_geocode`
- `todo_kanban`
- `copy_to_clipboard`, only when `xclip` is available in `PATH`

For isolated todo storage:

```sh
CAI_MCP_EXAMPLE_TODO_STORE=/tmp/cai-todo.json \
CAI_MCP_EXAMPLE_TODO_LOCK=/tmp/cai-todo.lock \
./build/debug/cai_example_mcp_server --port 18766
```

Minimal checks:

```sh
curl -s http://127.0.0.1:18766/health

curl -s http://127.0.0.1:18766/mcp \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2025-11-25' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'

curl -s http://127.0.0.1:18766/mcp \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2025-11-25' \
  -d '{"jsonrpc":"2.0","id":"help","method":"tools/call","params":{"name":"todo_kanban","arguments":{"operation":"help"}}}'

curl -s http://127.0.0.1:18766/mcp \
  -H 'Accept: text/event-stream' \
  -H 'MCP-Protocol-Version: 2025-11-25'

curl -s http://127.0.0.1:18766/mcp \
  -H 'Content-Type: application/json' \
  -H 'Accept: text/event-stream' \
  -H 'MCP-Protocol-Version: 2025-11-25' \
  -d '{"jsonrpc":"2.0","id":"sse","method":"ping"}'
```

The MCP Inspector e2e test uses the official container image and is opt-in:

```sh
CAI_MCP_INSPECTOR_E2E=1 ctest --preset debug -R cai_mcp_inspector_e2e --output-on-failure
```

## Testing From Agent Clients

The example server is a Streamable HTTP MCP server. Run it in one terminal with
isolated storage:

```sh
cmake --build --preset debug --target cai_example_mcp_server
tmpdir=$(mktemp -d)
CAI_MCP_EXAMPLE_TODO_STORE="$tmpdir/todo.json" \
CAI_MCP_EXAMPLE_TODO_LOCK="$tmpdir/todo.lock" \
./build/debug/cai_example_mcp_server --port 18766
```

Then register `http://127.0.0.1:18766/mcp` in the MCP client.

Codex CLI:

```sh
codex mcp add caiTodo --url http://127.0.0.1:18766/mcp
codex mcp list
```

Equivalent Codex TOML:

```toml
[mcp_servers.caiTodo]
url = "http://127.0.0.1:18766/mcp"
```

Claude Code:

```sh
claude mcp add --transport http caiTodo http://127.0.0.1:18766/mcp
claude mcp get caiTodo
```

Equivalent project-scoped `.mcp.json`:

```json
{
  "mcpServers": {
    "caiTodo": {
      "type": "http",
      "url": "http://127.0.0.1:18766/mcp"
    }
  }
}
```

Useful agent prompt for the todo preset:

```text
Use the caiTodo MCP server. Call todo_kanban with operation=help first,
then create a board named "agent-test", set its WIP limit to 1, add two
items, move the first item to in_process, verify moving the second item to
in_process is denied by the WIP limit, complete the first item, then move
the second item to in_process and summarize the board state.
```

That sequence exercises tool discovery, help text, create/list/update
operations, the WIP-limit invariant, and the done archive without touching the
default user todo store.
