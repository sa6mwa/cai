local cai = require("cai")

local function assert_eq(actual, expected, label)
  if actual ~= expected then
    error((label or "value") .. ": expected " .. tostring(expected) ..
      " got " .. tostring(actual), 2)
  end
end

local function assert_ok(value, err, label)
  if not value then
    error((label or "operation") .. " failed: " ..
      (type(err) == "table" and (err.message or err.status_string) or tostring(err)), 2)
  end
  return value
end

assert(type(cai.open) == "function")
assert(type(cai.tool_registry) == "function")
assert(type(cai.mcp_handler) == "function")
assert(type(cai.tool_schema) == "function")
assert_eq(cai.CONTINUITY_SERVER, 0, "server continuity")
assert(type(cai.MODEL_GPT_5_NANO) == "string")

local schema = assert_ok(cai.tool_schema())
assert_ok(schema:string("city", "City name", true))
assert_ok(schema:integer("days", "Forecast days", false))
local schema_json = schema:json()
assert(schema_json:match('"city"'))
assert(schema_json:match('"required"'))
schema:close()

local registry = assert_ok(cai.tool_registry())
local weather_schema = [[
{"type":"object","properties":{"city":{"type":"string"}},"required":["city"],"additionalProperties":false}
]]
assert_ok(registry:register_raw_tool("lua_weather", "Lua weather test tool", weather_schema, function(args_json)
  assert(args_json:match("Gothenburg"))
  return { ok = true, summary = "dry enough" }
end, true))

local raw_chunks = {}
assert_ok(registry:run("lua_weather", '{"city":"Gothenburg"}', function(chunk)
  raw_chunks[#raw_chunks + 1] = chunk
  return true
end))
local raw_json = table.concat(raw_chunks)
assert(raw_json:match('"ok":true'))
assert(raw_json:match('"summary":"dry enough"'))

assert_ok(registry:register_todo_tool({
  store_path = "/tmp/cai-lua-test-todo.json",
  lock_path = "/tmp/cai-lua-test-todo.lock",
  default_board = "lua",
}))

local chunks = {}
assert_ok(registry:run("todo_kanban", '{"operation":"help"}', function(chunk)
  chunks[#chunks + 1] = chunk
  return true
end))
local help_json = table.concat(chunks)
assert(help_json:match("todo_kanban"))
assert(help_json:match("operation"))

local mcp = assert_ok(cai.mcp_handler({
  name = "cai-lua-test",
  version = "0.0.0",
  tools = registry,
  allow_legacy_no_version = 1,
}))

local body_parts = {
  '{"jsonrpc":"2.0","id":"1","method":"tools/list","params":',
  '{}',
  '}',
}
local body_index = 0
local response_chunks = {}
local response = assert_ok(mcp:handle_http({
  method = "POST",
  headers = {
    ["content-type"] = "application/json",
    ["mcp-protocol-version"] = cai.MCP_PROTOCOL_VERSION,
  },
  body = function()
    body_index = body_index + 1
    return body_parts[body_index]
  end,
  write = function(chunk)
    response_chunks[#response_chunks + 1] = chunk
    return true
  end,
}))

assert_eq(response.status, 200, "mcp status")
assert(type(response.headers) == "table")
local response_json = table.concat(response_chunks)
assert(response_json:match("todo_kanban"))
assert(response_json:match('"jsonrpc"'))
assert(body_index > 1, "request body should be consumed in chunks")

mcp:close()
registry:close()

print("cai lua tests passed")
