# Shared kanban

CAI's optional `todo_kanban` tool stores named boards in the same encrypted
Pouch backend as sessions, under namespace `cai.todo`, document `kanban`.
Independent CAI applications sharing the endpoint and encryption key see the
same cards. Each conversation has its own selected board. Tool registration
remains off by default: enable `--todo`, `CAI_TODO=true`, or `[agent] todo=true`.

## Selection

| Setting | Purpose |
| --- | --- |
| `--board NAME` | Select/create this board at startup, overriding the resumed selection once. Does not enable the tool. |
| `CAI_BOARD` | Initial board for new conversations; overrides INI. |
| `[todo] default_board` | Initial board for new conversations; defaults to `default`. |
| `/todo` | Show the selected board, including completed cards. |
| `/todo list` | List the boards shared on this backend. |
| `/todo see ID` | Read the complete task on the selected board, including done items. |
| `/todo use NAME` | Select an existing board between turns. |
| `/todo new NAME` | Create or select a board between turns. |
| `/todo help` | Show command help. |

The usual `[env]` precedence policy applies to `CAI_BOARD`. Explicit flags win.
The selected board is committed with the conversation. Resume restores it;
`/new` uses the configured initial board. Changing one conversation's selection
does not change another's. `/status` includes the selected board or `disabled`.
Names are case-sensitive UTF-8, 1–256 bytes, without ASCII control characters.
The board named `default` reserves key `DEF`; other boards derive distinct keys.

The agent uses `create_board`, `list_boards` and `select_board` to manage its
selection. `current_board` shows the current selection; `snapshot_board` includes
archived done items. Existing `add_item`, `move_item`, `complete_item`,
`set_wip_limit`, `current_work` and `list_board` operations retain their roles.
`add_item` accepts an optional Markdown `description`. `get_item` reads a task
by `item_id` on the selected or explicitly requested board, including archived
done items. It returns canonical `item_id`, full `title`, `status` and a
nonempty `description` as top-level fields. Empty descriptions may be omitted.
Detail lookup is independent of `max_result_items`; missing IDs return
`ok=false, code=item_not_found`. Missing `item_id` is an `invalid_request`.
Explicit board selectors target a board without selecting it. WIP denials and
missing boards are structured results (`ok=false`); transport/storage failures
return errors. Item IDs such as `DEF-001` are assigned by the store.

## Terminal view

In the terminal application, `/todo` defaults to a kanban table with TODO,
DOING (`in_process`) and DONE columns. Each card has a bold ID on its own row
and an italic title below it. Titles are limited to 28 Unicode characters,
including the ellipsis, with Markdown punctuation preserved literally. The
stored title remains complete. Libmdf wraps cells to fit the terminal.

`/todo layout list` switches to the item/status/full-title table;
`/todo layout board` restores the board view. `/todo layout` reports the current
setting. These settings affect future output in this application invocation,
including after `/new` or `/resume`; they do not rewrite past output or save a
conversation preference. Board selection still belongs to the conversation.
`/todo list` continues to list boards, independently of card layout.

`/todo see def-001` renders the canonical ID and lane in a heading, for example
`DEF-001 (TODO)`, the complete task title as a bold subtitle, and its Markdown
description as the body. Lanes display as TODO, DOING and DONE. Lists and board
layouts never include descriptions. Inspecting an item leaves board selection
and layout unchanged and makes no model request.

Startup defaults can be set with `[ui] todo_layout = board|list`,
`CAI_TODO_LAYOUT` (or the configured application prefix), and
`--todo-layout board|list`. Forwarded flags take precedence. Embedders use
the same setting through C `cai_app_setting` or Lua
`settings = {ui = {todo_layout = "list"}}`. Layout selection does not enable the
optional todo tool. Structured tool results and non-interactive logs keep
their existing JSON representation.

## C receiver and Pouch adapter

Core libcai exposes `cai_todo` in `<cai/tools/todo.h>`. `cai_todo_open` accepts
file storage or dependency-free transactional callbacks. Its receiver provides
`call`, `select`, `get_board`, `handle_http`, and `close`. `call` accepts a
`cai_source` of JSON arguments and writes JSON results to a `cai_sink`.

JSON parsing, serialization and store rewrites use incremental readers/writers.
Results materialize at most `max_result_items` entries (default 100); snapshots
set `truncated=true` when additional cards are omitted. The browser displays this
limitation. `item_count` counts active cards, including when the snapshot also
contains archived cards. Descriptions remain stored but the view shows titles.

A successful open takes ownership of custom store context; a failed open does
not. `cai_todo_retain` adds an owned reference. Final close destroys the store.
Registration with `cai_todo_tool_config.instance` retains the supplied receiver.
Receiver operations serialize internally; callbacks must not reenter it. Stores
must provide transactions across receivers/processes. A supplied pslog logger is
borrowed and must remain alive until the last reference closes.

For the application Pouch adapter:

```c
#include <cai/app.h>

/* During host startup: retain board until all routes have drained. */
cai_app_kanban_config config;
cai_todo *board = NULL;
cai_error error;
cai_app_kanban_config_init(&config);
/* config.state_directory = ...; config.lockd = ...; config.logger = ...; */
cai_error_init(&error);
if (cai_app_kanban_open(&config, &board, &error) != CAI_OK) {
  /* Report error.message, then clean up error. */
}
```

The helper reads no config.ini and changes neither cwd nor environment. Pass
resolved overrides from the host. NULL paths use CAI's XDG state directory,
`pouch.key` and local Pouch endpoint. To share a non-default store, pass the same
endpoint and state directory/key as the terminal application. Missing loggers
use pslog `LOG_` configuration with a warn default; supplied loggers bypass
logger initialization from the environment.

Runtime owners use `cai_agent_runtime_select_board` between turns for an immediate
commit. `cai_agent_runtime_todo` borrows the live receiver for inspection;
mutation should go through the runtime API or the registered tool. Borrowed
receivers expire when the runtime closes. Lua runtimes expose `select_board`
and `todo_board` for these controls.

## HTTP host integration

`cai_todo_handle_http` is a synchronous, framework-neutral read-only route
handler. Kore/Vectis owns the listener, request lifetime, permissions, routing,
headers and network backpressure. Keep one receiver open for the host lifetime.
Map a route prefix such as `/kanban/` to these relative resources:

| Resource | Content |
| --- | --- |
| empty or `index.html` | Browser page |
| `app.js` | Refresh/render logic |
| `style.css` | Responsive board styling |
| `board.json` | Current board snapshot |

Redirect `/kanban` to `/kanban/` so relative asset URLs resolve correctly. The
host URL-decodes the optional `board` query parameter and passes it as
`request.board`; the handler does not parse URLs or serve filesystem paths.
Open `/kanban/?board=NAME` for a specific board. A missing query uses the
receiver's selection. The page polls every two seconds after completion of the
previous fetch, only while visible, with a five-second fetch deadline. Titles
and names are inserted as text, never HTML. There are no browser write routes.

For each request fill `cai_todo_http_request` and `cai_todo_http_response`:

```c
request.method = method;
request.resource = relative_resource;
request.board = decoded_board_name; /* NULL selects this receiver's board */
response.begin = host_begin; /* status and Content-Type, before body bytes */
response.context = host_request;
response.body = host_sink;  /* incremental writes; host owns the sink */
rc = board->handle_http(board, &request, &response, &error);
```

GET/HEAD are supported; other methods return 405. Unknown resources/boards return
404; invalid names return 400; unavailable storage returns 503. HTTP errors may
have a complete response even when the function returns a CAI error. Once
`begin` succeeds, the host must not write a second response on a later failure.
A sink failure aborts output. HEAD sends headers only. Set `Cache-Control:
no-store` on JSON and `Allow: GET, HEAD` on 405. Host access control protects the
same sensitive information as session storage. The handler creates no listener.

## Lua

`require('cai').todo {store_path=..., lock_path=..., default_board=...}` opens
the standalone core engine. Native `store` callbacks and the usual title,
description and result bounds are also supported. `require('cai.app').kanban`
opens the application's Pouch adapter without a terminal:

```lua
local board = assert(require('cai.app').kanban {
  -- state_directory = ..., lockd = ..., logger = host_logger,
  default_board = 'default', max_result_items = 100,
})
assert(board:select('team', true)) -- create if absent
assert(board:get_board() == 'team')
assert(board:call('{"operation":"list_boards"}', function(bytes)
  host_write(bytes)
end))
local response = assert(board:handle_http {
  method = 'GET', resource = 'board.json', board = 'team',
  begin = function(status, content_type) host_begin(status, content_type) end,
  write = function(bytes) host_write(bytes) end,
})
assert(response.status == 200)
board:close()
```

`call` also accepts an incremental chunk-reader function instead of a string.
Return 1–65536 bytes per read, then nil at EOF. Both `call` and `handle_http`
require a writer and do not collect the complete serialized result. Writers and
`begin` may return false to reject output or raise an error; failures return
`nil, error`. Callback reentry/close is rejected while the receiver is busy.
`handle_http` returns `{status, content_type}` on success. `close` is idempotent;
Lua GC also closes it. Supplied native-sink pslog loggers remain pinned until
receiver close; Lua-callback loggers are unsuitable for Pouch worker threads.

## Verification

The focused CTest checks are `cai_app_kanban`, `cai_cli_kanban` and
`cai_app_lua_kanban`; core todo regressions remain in `cai_tests.todo_*`.
They use real encrypted Pouch transactions and mock inference. Storage fixtures
remove durable group-commit waits because these tests assert board operations,
not power-loss durability. Cross-process visibility and checkpoint/reopen remain
covered. Browser polling/render behavior can also be checked without a browser,
packages or real timers: `node tests/kanban_view_test.js`.
