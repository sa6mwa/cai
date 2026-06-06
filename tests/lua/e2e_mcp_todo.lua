local cai = require("cai")

local function fail(message)
  error(message, 0)
end

local function assert_ok(value, err, label)
  if not value then
    local detail = tostring(err)
    if type(err) == "table" then
      detail = err.detail or err.message or err.status_string or detail
    end
    fail((label or "operation") .. " failed: " .. detail)
  end
  return value
end

local function assert_contains(label, haystack, needle)
  if not haystack or not haystack:find(needle, 1, true) then
    fail(label .. " missing " .. needle .. "\nactual:\n" .. tostring(haystack))
  end
end

local function assert_status(label, response, status)
  if response.status ~= status then
    fail(label .. ": expected status " .. tostring(status) ..
      " got " .. tostring(response.status))
  end
end

local function temp_path(name)
  local base = os.getenv("TMPDIR") or "/tmp"
  return string.format("%s/cai-lua-mcp-todo-%d-%s", base, os.time(), name)
end

local function chunked_source(text, chunk_size)
  local pos = 1
  return function()
    if pos > #text then
      return nil
    end
    local last = pos + chunk_size - 1
    local chunk = text:sub(pos, last)
    pos = last + 1
    return chunk
  end
end

local function mcp_request(handler, body)
  local chunks = {}
  local response = assert_ok(handler:handle_http({
    method = "POST",
    headers = {
      ["content-type"] = "application/json",
      ["mcp-protocol-version"] = cai.MCP_PROTOCOL_VERSION,
    },
    body = chunked_source(body, 11),
    write = function(chunk)
      chunks[#chunks + 1] = chunk
      return true
    end,
  }), nil, "mcp:handle_http")
  return response, table.concat(chunks)
end

local store_path = temp_path("store.json")
local lock_path = temp_path("store.lock")
os.remove(store_path)
os.remove(lock_path)

local registry = assert_ok(cai.tool_registry())
assert_ok(registry:register_todo_tool({
  store_path = store_path,
  lock_path = lock_path,
  default_board = "main",
}), nil, "registry:register_todo_tool")

local handler = assert_ok(cai.mcp_handler({
  name = "cai-lua-mcp-todo-test",
  version = "0.0.0",
  tools = registry,
  require_protocol_version = 1,
}), nil, "cai.mcp_handler")

local response, body = mcp_request(handler,
  '{"jsonrpc":"2.0","id":"list","method":"tools/list","params":{}}')
assert_status("tools/list", response, 200)
assert_contains("tools/list", body, "todo_kanban")
assert_contains("tools/list", body, "inputSchema")
assert_contains("tools/list", body, "Use help first when unsure")

response, body = mcp_request(handler,
  '{"jsonrpc":"2.0","id":"help","method":"tools/call","params":{"name":"todo_kanban","arguments":{"operation":"help"}}}')
assert_status("help", response, 200)
assert_contains("help", body, "current_work")
assert_contains("help", body, "wip_limit_exceeded")
assert_contains("help", body, "Refs accept DEF-001")

response, body = mcp_request(handler,
  '{"jsonrpc":"2.0","id":"board","method":"tools/call","params":{"name":"todo_kanban","arguments":{"operation":"create_board","board_name":"main","wip_limit":1}}}')
assert_status("create_board", response, 200)
assert_contains("create_board", body, '"ok":true')
assert_contains("create_board", body, '"board_id"')

response, body = mcp_request(handler,
  '{"jsonrpc":"2.0","id":"add1","method":"tools/call","params":{"name":"todo_kanban","arguments":{"operation":"add_item","board_name":"main","title":"first task","status":"in_process"}}}')
assert_status("add_item", response, 200)
assert_contains("add_item", body, '"ok":true')
assert_contains("add_item", body, '"item_id"')

local item_id = body:match('"item_id":"([^"]+)"')
if not item_id then
  fail("failed to capture item_id from add_item response:\n" .. body)
end

response, body = mcp_request(handler,
  '{"jsonrpc":"2.0","id":"wip","method":"tools/call","params":{"name":"todo_kanban","arguments":{"operation":"add_item","board_name":"main","title":"second task","status":"in_process"}}}')
assert_status("wip denial", response, 200)
assert_contains("wip denial", body, '"ok":false')
assert_contains("wip denial", body, "wip_limit_exceeded")

response, body = mcp_request(handler,
  '{"jsonrpc":"2.0","id":"list-board","method":"tools/call","params":{"name":"todo_kanban","arguments":{"operation":"list_board","board_name":"main"}}}')
assert_status("list_board", response, 200)
assert_contains("list_board", body, "first task")
assert_contains("list_board", body, "in_process")

response, body = mcp_request(handler,
  '{"jsonrpc":"2.0","id":"complete","method":"tools/call","params":{"name":"todo_kanban","arguments":{"operation":"complete_item","board_name":"main","item_id":"' .. item_id .. '"}}}')
assert_status("complete_item", response, 200)
assert_contains("complete_item", body, '"ok":true')
assert_contains("complete_item", body, "item completed")

response, body = mcp_request(handler,
  '{"jsonrpc":"2.0","id":"add2","method":"tools/call","params":{"name":"todo_kanban","arguments":{"operation":"add_item","board_name":"main","title":"second task","status":"in_process"}}}')
assert_status("add after complete", response, 200)
assert_contains("add after complete", body, '"ok":true')

response, body = mcp_request(handler,
  '{"jsonrpc":"2.0","id":"bad","method":"tools/call","params":{"name":"todo_kanban","arguments":{"operation":"no_such_operation"}}}')
assert_status("unknown operation", response, 200)
assert_contains("unknown operation", body, '"ok":false')
assert_contains("unknown operation", body, "unknown_operation")
assert_contains("unknown operation", body, '"isError":false')

handler:close()
registry:close()
os.remove(store_path)
os.remove(lock_path)

print("cai lua MCP todo e2e passed")
