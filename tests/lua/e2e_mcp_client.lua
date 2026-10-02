local cai = require("cai")

local function fail(message)
  io.stderr:write(message .. "\n")
  os.exit(1)
end

local function assert_ok(value, err, label)
  if not value then
    local detail = tostring(err)
    if type(err) == "table" then
      detail = err.message or err.detail or err.status_string or detail
    end
    fail((label or "operation") .. " failed: " .. detail)
  end
  return value
end

local function assert_not_ok(value, err, label)
  if value then
    fail((label or "operation") .. ": expected failure")
  end
  if err == nil then
    fail((label or "operation") .. ": expected error detail")
  end
  return err
end

local function contains(name, text, needle)
  if type(text) ~= "string" or not text:find(needle, 1, true) then
    fail(name .. " missing expected fragment: " .. needle)
  end
end

local function assert_throws(fn, label)
  local ok = pcall(fn)
  if ok then
    fail((label or "operation") .. ": expected Lua error")
  end
end

local url = arg[1]
if type(url) ~= "string" or url == "" then
  fail("usage: e2e_mcp_client.lua MCP_STREAMABLE_HTTP_URL")
end
local mode = arg[2] or "full"

local client = assert_ok(cai.mcp_client({
  url = url,
  client_name = "cai-lua-mcp-client-e2e",
  timeout_ms = 5000,
}))

assert_ok(client:initialize(), nil, "initialize")
assert_ok(client:ping(), nil, "ping")
assert_ok(client:refresh_tools(), nil, "refresh_tools")

if mode == "raw" then
  local function raw_failure(label, value, err)
    return assert_not_ok(value, err, label)
  end
  local tool = client:tool_at(1)
  if tool.raw_input.field ~= "input" or tool.raw_input.framing ~= "text" then fail("native raw metadata missing") end
  local payload = string.rep('line "quote" \\ å🚀\n', 16000) .. "tail\n"
  local result = assert_ok(client:call_tool_raw("echo_message", tool.raw_input, payload), nil, "large raw upload")
  local json = result:read_all()
  if json:find('"isError":true', 1, true) then fail("large raw tool error: " .. json:sub(1, 1000)) end
  local quoted = '"' .. payload:gsub('\\', '\\\\'):gsub('"', '\\"'):gsub('\n', '\\n') .. '"'
  contains("large raw exact bytes", json, quoted)
  local registry = assert_ok(cai.tool_registry())
  raw_failure("invalid raw mapping", client:register_tools(registry, {
    raw_input_overrides = {
      { name = "echo_message", mapping = { field = "missing" } },
    },
  }))
  -- A successful retry with the same name proves failure left no registration.
  assert_ok(client:register_tools(registry), nil, "server raw advertisement")
  local chunks = {}
  assert_ok(registry:run("echo_message", payload, function(chunk) chunks[#chunks + 1] = chunk; return true end), nil, "native raw import round trip")
  contains("native raw import exact bytes", table.concat(chunks), quoted)
  raw_failure("raw registry retains client", client:close())
  for _, input in ipairs({ "[]\nbody", '{"input":"collision"}\nbody',
                           '{"input":null}\nbody', '{}',
                           string.rep(" ", 16384) .. "\nbody" }) do
    raw_failure("invalid raw header", client:call_tool_raw("echo_message",
      { field = "input", framing = "json-header" }, input))
  end
  raw_failure("invalid raw UTF-8", client:call_tool_raw("echo_message", tool.raw_input,
    "\255"))
  local reader_calls = 0
  raw_failure("raw reader failure", client:call_tool_raw("echo_message", tool.raw_input,
    function()
      reader_calls = reader_calls + 1
      if reader_calls == 1 then return "partial" end
      error("raw source failed deliberately")
    end))
  local writer_calls = 0
  local writer_error = raw_failure("raw writer failure", client:call_tool_raw("echo_message", tool.raw_input,
    "sink failure", function()
      writer_calls = writer_calls + 1
      return false, "raw sink failed deliberately"
    end))
  if writer_calls == 0 then
    fail("raw writer was not reached: " .. tostring(writer_error.message))
  end
  local reentry = false
  assert_ok(client:call_tool_raw("echo_message", tool.raw_input, "healthy",
    function()
      collectgarbage("collect")
      raw_failure("raw active client close", client:close())
      reentry = true
      return true
    end), nil, "raw call recovers after failures")
  if not reentry then fail("raw recovery sink not reached") end
  local binary = assert_ok(client:call_tool_raw("echo_message", tool.raw_input,
    "a\0b\n"), nil, "embedded NUL raw upload")
  contains("embedded NUL preserved", binary:read_all(), '"a\\u0000b\\n"')
  registry:close()
  assert_ok(client:close())
  print("Lua MCP native raw e2e passed at " .. url)
  return
end

if mode == "empty" then
  if client:tool_count() ~= 0 then
    fail("empty MCP test server advertised tools")
  end
  local empty_registry = assert_ok(cai.tool_registry(),
    nil, "empty tool registry")
  assert_ok(client:register_tools(empty_registry),
    nil, "register empty remote tools")
  assert_ok(client:close(),
    nil, "empty remote registration must not retain client")
  empty_registry:close()
  print("Lua MCP client empty e2e passed at " .. url)
  return
end

if client:tool_count() ~= 1 then
  fail("CAI MCP test server tool count changed")
end

local tool = client:tool_at(1)
if type(tool) ~= "table" or tool.name ~= "echo_message" then
  fail("MCP server did not advertise echo_message")
end
if tool.title ~= "echo_message" or
    tool.description ~= "Echo a message through the MCP test server" then
  fail("echo_message metadata was incomplete")
end
if type(tool.input_schema) ~= "table" then
  fail("echo_message input schema missing")
end
local found_message = false
for _, name in ipairs(tool.input_schema.properties) do
  if name == "message" then
    found_message = true
  end
end
if not found_message then
  fail("echo_message input schema missing message property")
end
if client:tools()[1].name ~= "echo_message" then
  fail("tools() did not return cached echo_message metadata")
end

assert_not_ok(client:send_request("", nil))
assert_not_ok(client:send_notification("", nil))
assert_not_ok(client:call_tool("", nil))
assert_not_ok(client:call_tool("echo_message", "[]"))
assert_not_ok(client:read_resource(""))
assert_not_ok(client:get_prompt(""))
assert_not_ok(client:complete("ref/unknown", "value", "argument", "", nil))
assert_not_ok(client:complete("ref/prompt", "completable-prompt", "name", "",
  "[]"))
assert_throws(function()
  client:call_tool({}, nil, function()
    return true
  end)
end, "call_tool invalid name with writer")
assert_throws(function()
  client:read_resource({}, function()
    return true
  end)
end, "read_resource invalid uri with writer")
assert_throws(function()
  client:get_prompt({}, nil, function()
    return true
  end)
end, "get_prompt invalid name with writer")
assert_throws(function()
  client:complete({}, "name", "argument", "value", nil, function()
    return true
  end)
end, "complete invalid ref type with writer")
assert_throws(function()
  client:send_request({}, nil, function()
    return true
  end)
end, "send_request invalid method with writer")
assert_throws(function()
  client:send_notification({}, nil)
end, "send_notification invalid method")
assert_throws(function()
  client:call_tool("echo_message", '{"message":"bad-writer"}', {})
end, "call_tool invalid writer")
assert_throws(function()
  client:get_prompt("missing", "{}", {})
end, "get_prompt invalid writer")
assert_throws(function()
  client:complete("ref/prompt", "completable-prompt", "name", "", "{}", {})
end, "complete invalid writer")
assert_throws(function()
  client:send_request("ping", "{}", {})
end, "send_request invalid writer")

local ping_result = assert_ok(client:send_request("ping", nil),
  nil, "generic ping")
contains("generic ping result", ping_result:read_all(), "{}")

assert_not_ok(client:send_request("unknown/method", nil))
assert_not_ok(client:send_request("tools/call", "{}"))
assert_not_ok(client:refresh_resources())
assert_not_ok(client:read_resource("demo://missing"))

assert_ok(client:send_notification("notifications/initialized", nil),
  nil, "initialized notification")

local echo = assert_ok(client:call_tool("echo_message",
  '{"message":"cai-lua-mcp-client-e2e-ok"}'), nil, "call_tool reader")
local echo_json = echo:read_all()
contains("echo_message output", echo_json, "cai-lua-mcp-client-e2e-ok")
contains("echo_message output", echo_json, "\"structuredContent\"")

local chunks = {}
assert_ok(client:call_tool("echo_message",
  '{"message":"cai-lua-mcp-client-writer-ok"}',
  function(chunk)
    chunks[#chunks + 1] = chunk
    return true
  end), nil, "call_tool writer")
contains("echo_message writer output", table.concat(chunks),
  "cai-lua-mcp-client-writer-ok")
local reentrant_close_seen = false
chunks = {}
assert_ok(client:call_tool("echo_message",
  '{"message":"cai-lua-mcp-client-reentrant-close-ok"}',
  function(chunk)
    local ok
    local err
    chunks[#chunks + 1] = chunk
    if not reentrant_close_seen then
      ok, err = client:close()
      assert_not_ok(ok, err, "active MCP client close")
      contains("active MCP client close error", tostring(err.message or ""),
        "active operation")
      reentrant_close_seen = true
    end
    return true
  end), nil, "call_tool reentrant close writer")
if not reentrant_close_seen then
  fail("call_tool writer did not exercise reentrant close")
end
contains("echo_message reentrant close output", table.concat(chunks),
  "cai-lua-mcp-client-reentrant-close-ok")

local missing = assert_ok(client:call_tool("missing_tool", "{}"),
  nil, "missing tool call")
contains("missing tool output", missing:read_all(), "\"isError\":true")

-- Explicit mappings work with ordinary servers that advertise no raw hint.
local raw_payload = string.rep('line "quote" \\ å🚀\n', 500) .. "tail\n"
local raw_result = assert_ok(client:call_tool_raw("echo_message", { field = "message" }, raw_payload), nil, "raw upload")
local raw_json = raw_result:read_all()
if raw_json:find('"isError":true', 1, true) then fail("raw tool error: " .. raw_json:sub(1, 1000)) end
contains("raw upload tail", raw_json, "tail\\n")
local quoted_payload = '"' .. raw_payload:gsub('\\', '\\\\'):gsub('"', '\\"'):gsub('\n', '\\n') .. '"'
contains("large raw exact bytes", raw_json, quoted_payload)
local raw_header_result = assert_ok(client:call_tool_raw("echo_message",
  { field = "message", framing = "json-header" }, '{}\nraw header body\n'), nil, "raw header upload")
contains("raw header output", raw_header_result:read_all(), "raw header body\\n")
local raw_registry = assert_ok(cai.tool_registry())
assert_ok(client:register_tools(raw_registry, {
  name_prefix = "raw_", raw_input_policy = "custom",
  raw_input_overrides = {{ name = "echo_message", mapping = { field = "message" } }},
}), nil, "raw override registration")
local raw_chunks = {}
assert_ok(raw_registry:run("raw_echo_message", 'unescaped "input"\n', function(chunk)
  raw_chunks[#raw_chunks + 1] = chunk
  return true
end), nil, "raw override execution")
contains("raw override output", table.concat(raw_chunks), 'unescaped \\"input\\"\\n')
raw_registry:close()

local registry = assert_ok(cai.tool_registry(), nil, "tool registry")
assert_ok(client:register_tools(registry, { name_prefix = "remote_" }),
  nil, "register remote tools")
chunks = {}
assert_ok(registry:run("remote_echo_message",
  '{"message":"cai-lua-mcp-client-registry-ok"}',
  function(chunk)
    chunks[#chunks + 1] = chunk
    return true
  end), nil, "registered remote tool")
contains("registered remote tool output", table.concat(chunks),
  "cai-lua-mcp-client-registry-ok")
local close_ok, close_err = client:close()
close_err = assert_not_ok(close_ok, close_err,
  "registered remote tool client close")
if type(close_err) ~= "table" or
    not tostring(close_err.message or ""):find("registered tools", 1, true) then
  fail("registered remote tool close returned wrong error")
end
registry:close()

local collision_registry = assert_ok(cai.tool_registry(), nil,
  "collision tool registry")
assert_ok(collision_registry:register_raw_tool("collision_echo_message",
  "Existing collision tool",
  '{"type":"object","properties":{},"additionalProperties":false}',
  function()
    return '{"ok":true}'
  end, true), nil, "collision local tool")
local register_ok, register_err = client:register_tools(collision_registry,
  { name_prefix = "collision_" })
assert_not_ok(register_ok, register_err, "colliding remote tool registration")
collision_registry:close()

local runtime_client = assert_ok(cai.open({
  api_key = "test-key",
  base_url = "http://127.0.0.1:1/v1",
  timeout_ms = 1,
}), nil, "runtime host client")
local runtime = assert_ok(runtime_client:new_smith_runtime({
  workspace_directory = ".",
  disable_terminal = true,
  disable_default_session_store = true,
  mcp_clients = { client },
  mcp_tool_config = { name_prefix = "runtime_" },
}), nil, "runtime MCP attachment")
close_ok, close_err = client:close()
close_err = assert_not_ok(close_ok, close_err,
  "runtime-attached MCP client close")
contains("runtime-attached MCP client close error",
  tostring(close_err.message or ""), "registered tools")
runtime:close()
assert_ok(client:close(), nil, "runtime MCP client release")
runtime_client:close()

print("Lua MCP client e2e passed at " .. url)
