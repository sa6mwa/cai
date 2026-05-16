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

- `POST /mcp` for MCP JSON-RPC requests.
- `GET /health` for a small JSON health response.
- `GET /mcp` for the same lightweight diagnostic response.

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
```

The MCP Inspector e2e test uses the official container image and is opt-in:

```sh
CAI_MCP_INSPECTOR_E2E=1 ctest --preset debug -R cai_mcp_inspector_e2e --output-on-failure
```
