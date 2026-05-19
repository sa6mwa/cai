local cai = require("cai")

local function skip(message)
  io.stdout:write("SKIP: " .. message .. "\n")
  os.exit(0)
end

local function fail(message)
  error(message, 0)
end

local function assert_ok(value, err, label)
  if not value then
    local detail = tostring(err)
    if type(err) == "table" then
      detail = err.detail or err.message or err.status_string or detail
      local parts = {}
      for k, v in pairs(err) do
        parts[#parts + 1] = tostring(k) .. "=" .. tostring(v)
      end
      if #parts > 0 then
        detail = detail .. " {" .. table.concat(parts, ",") .. "}"
      end
    end
    fail((label or "operation") .. " failed: " .. detail)
  end
  return value
end

local function api_key_config()
  if os.getenv("OPENAI_API_KEY") ~= nil and os.getenv("OPENAI_API_KEY") ~= "" then
    return nil
  end
  local key = cai.load_dotenv_api_key(cai.DEFAULT_DOTENV_PATH, cai.OPENAI_API_KEY_ENV)
  if key ~= nil and key ~= "" then
    return key
  end
  return nil
end

if os.getenv("CAI_LUA_TOOL_STREAM_E2E") ~= "1" then
  skip("set CAI_LUA_TOOL_STREAM_E2E=1 to run Lua streamed tool e2e")
end
local api_key = api_key_config()
if (os.getenv("OPENAI_API_KEY") == nil or os.getenv("OPENAI_API_KEY") == "") and
    api_key == nil then
  skip("OPENAI_API_KEY is not set")
end

local secret = "secret-" .. tostring(os.time()) .. "-gothenburg"
local client, client_err = cai.open({ timeout_ms = 60000, api_key = api_key })
client = assert_ok(client, client_err, "cai.open")
local agent, agent_err = client:new_agent({
  model = cai.MODEL_GPT_5_NANO,
  instructions = table.concat({
    "You are a deterministic integration-test agent.",
    "When the user asks for the Lua stream tool test, you must call ",
    "lua_stream_fact exactly once with code=gothenburg.",
    "The tool is the only source of the secret value. Never guess it.",
    "After the tool result, answer with exactly:",
    "LUA_STREAM_TOOL_OK gothenburg <secret>",
    "where <secret> is copied from the tool result secret field.",
  }, " "),
  reasoning_effort = cai.REASONING_EFFORT_MINIMAL,
  tool_choice = cai.TOOL_CHOICE_AUTO,
  max_output_tokens = 1024,
  session_continuity = cai.CONTINUITY_SERVER,
  prompt_cache_key = "cai:lua-tool-stream-e2e:v1",
})
agent = assert_ok(agent, agent_err, "client:new_agent")

local schema = [[
{"type":"object","properties":{"code":{"type":"string"}},"required":["code"],"additionalProperties":false}
]]

local tool_calls = 0
local ok, err = agent:register_raw_spooled_tool(
  "lua_stream_fact",
  "Return the known test fact for the requested code.",
  schema,
  function(args)
    local raw = args:read_all()
    local chunks
    local i

    tool_calls = tool_calls + 1
    if not raw:match("gothenburg") then
      fail("tool arguments did not include gothenburg: " .. raw)
    end
    chunks = {
      '{"code":"gothenburg",',
      '"secret":"' .. secret .. '"}',
      nil,
    }
    i = 0
    return function()
      i = i + 1
      return chunks[i]
    end
  end,
  true
)
assert_ok(ok, err, "agent:register_raw_spooled_tool")

local session, session_err = agent:new_session()
session = assert_ok(session, session_err, "agent:new_session")
ok, err = session:add_user_text(
  "Run the Lua stream tool test now. Use code gothenburg."
)
assert_ok(ok, err, "session:add_user_text")

local response_chunks = {}
local response_delta_chunks = {}
local reasoning_chunks = {}
local function_deltas = {}
local function_done = {}
local tool_events = {}
local tool_output = {}

ok, err = session:stream({
  max_tool_rounds = 3,
  response = function(chunk)
    response_chunks[#response_chunks + 1] = chunk
    return true
  end,
  on_response_delta = function(chunk)
    response_delta_chunks[#response_delta_chunks + 1] = chunk
    return true
  end,
  reasoning = function(chunk)
    reasoning_chunks[#reasoning_chunks + 1] = chunk
    return true
  end,
  on_function_call_delta = function(item_id, output_index, delta)
    function_deltas[#function_deltas + 1] = {
      item_id = item_id,
      output_index = output_index,
      delta = delta,
    }
    return true
  end,
  on_function_call_done = function(item_id, output_index, call_id, name, arguments)
    function_done[#function_done + 1] = {
      item_id = item_id,
      output_index = output_index,
      call_id = call_id,
      name = name,
      arguments = arguments,
    }
    return true
  end,
  tool_event = function(event)
    tool_events[#tool_events + 1] = event.kind .. ":" .. (event.name or "")
    if event.arguments_spooled ~= nil then
      local args = event.arguments_spooled:read_all()
      if not args:match("gothenburg") then
        fail("streamed tool event arguments lost gothenburg: " .. args)
      end
    end
    if event.write_output ~= nil then
      local before = #tool_output
      local ok, err = event.write_output(function(chunk)
        tool_output[#tool_output + 1] = chunk
        return true
      end)
      assert_ok(ok, err, "event.write_output")
      if #tool_output == before then
        fail("tool output event did not stream any output chunks")
      end
    end
    return true
  end,
})
assert_ok(ok, err, "session:stream")

local response = table.concat(response_chunks)
local response_delta = table.concat(response_delta_chunks)
local output = table.concat(tool_output)

if tool_calls ~= 1 then
  fail("expected exactly one tool callback, got " .. tostring(tool_calls) ..
    "; response=" .. response ..
    "; function_done=" .. tostring(#function_done) ..
    "; tool_events=" .. table.concat(tool_events, ","))
end
if #function_done < 1 then
  fail("expected function-call done callback")
end
if not function_done[#function_done].arguments:match("gothenburg") then
  fail("function-call done arguments did not include gothenburg")
end
if not table.concat(tool_events, "\n"):match("start:lua_stream_fact") then
  fail("missing tool start event")
end
if not table.concat(tool_events, "\n"):match("output:lua_stream_fact") then
  fail("missing tool output event")
end
if not output:find(secret, 1, true) then
  fail("streamed tool output did not include secret; output=" .. output)
end
if not response:find("LUA_STREAM_TOOL_OK gothenburg " .. secret, 1, true) then
  fail("unexpected final response: " .. response)
end
if not response_delta:find("LUA_STREAM_TOOL_OK gothenburg " .. secret, 1, true) then
  fail("response delta callback did not receive final response; delta=" ..
    response_delta)
end
if response ~= response_delta then
  fail("response sink and delta callback diverged; response=" .. response ..
    "; delta=" .. response_delta)
end

session:close()
agent:close()
client:close()

print("cai lua streamed tool e2e passed")
