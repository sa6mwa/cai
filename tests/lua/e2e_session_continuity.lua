local cai = require("cai")
local chatgpt_e2e = dofile("tests/lua/chatgpt_e2e.lua")

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

local function stream_turn(session, text)
  local chunks = {}
  local tool_events = {}
  local ok, err

  ok, err = session:add_user_text(text)
  assert_ok(ok, err, "session:add_user_text")

  ok, err = session:stream({
    max_tool_rounds = 3,
    response = function(chunk)
      chunks[#chunks + 1] = chunk
      return true
    end,
    tool_event = function(event)
      tool_events[#tool_events + 1] = event.kind .. ":" .. (event.name or "")
      return true
    end,
  })
  assert_ok(ok, err, "session:stream")
  return table.concat(chunks), table.concat(tool_events, "\n")
end

local auth_path = chatgpt_e2e.require_live_auth(cai, "CAI_LUA_SESSION_E2E")

local secret = "lua-continuity-" .. tostring(os.time()) .. "-gothenburg"
local client, client_err = cai.open({
  timeout_ms = 60000,
  chatgpt_auth_json = auth_path,
})
client = assert_ok(client, client_err, "cai.open")
local agent, agent_err = client:new_agent({
  model = cai.MODEL_GPT_5_NANO,
  instructions = table.concat({
    "You are a deterministic Lua SDK integration-test agent.",
    "When asked to store the continuity secret, call lua_secret_lookup exactly once.",
    "The tool is the only source of the secret value.",
    "After the tool result, answer exactly: LUA_CONTINUITY_STORED <secret>.",
    "On later turns, answer from conversation/session context without calling the tool unless explicitly asked.",
    "When asked to recall it, answer exactly: LUA_CONTINUITY_RECALL <secret>.",
  }, " "),
  reasoning_effort = cai.REASONING_EFFORT_MINIMAL,
  tool_choice = cai.TOOL_CHOICE_AUTO,
  max_output_tokens = 512,
  session_continuity = cai.CONTINUITY_SERVER,
  prompt_cache_key = "cai:lua-session-continuity-e2e:v1",
})
agent = assert_ok(agent, agent_err, "client:new_agent")

local schema = [[
{"type":"object","properties":{"code":{"type":"string"}},"required":["code"],"additionalProperties":false}
]]
local tool_calls = 0
local ok, err = agent:register_raw_spooled_tool(
  "lua_secret_lookup",
  "Return the continuity test secret for the requested code.",
  schema,
  function(args)
    local raw = args:read_all()
    local done = false
    tool_calls = tool_calls + 1
    if not raw:match("gothenburg") then
      fail("tool arguments did not include gothenburg: " .. raw)
    end
    return function()
      if done then
        return nil
      end
      done = true
      return '{"code":"gothenburg","secret":"' .. secret .. '"}'
    end
  end,
  true
)
assert_ok(ok, err, "agent:register_raw_spooled_tool")

local session = assert_ok(agent:new_session(), nil, "agent:new_session")
local first, first_events = stream_turn(
  session,
  "Store the continuity secret. Use code gothenburg and the lookup tool."
)
if not first:find("LUA_CONTINUITY_STORED " .. secret, 1, true) then
  fail("first response did not store secret correctly:\n" .. first)
end
if not first_events:find("start:lua_secret_lookup", 1, true) then
  fail("first turn did not emit tool start event:\n" .. first_events)
end
if tool_calls ~= 1 then
  fail("expected exactly one tool call after first turn, got " .. tostring(tool_calls))
end

local ids = session:ids()
if not ids.previous_response_id or ids.previous_response_id == "" then
  fail("session did not retain previous_response_id after streamed first turn")
end

local second, second_events = stream_turn(
  session,
  "Recall the exact continuity secret from the previous turn. Do not call tools."
)
if not second:find("LUA_CONTINUITY_RECALL " .. secret, 1, true) then
  fail("second response did not recall secret from session context:\n" .. second)
end
if second_events:find("lua_secret_lookup", 1, true) then
  fail("second turn unexpectedly called tool:\n" .. second_events)
end
if tool_calls ~= 1 then
  fail("expected no additional tool calls, got " .. tostring(tool_calls))
end

local usage = session:last_usage()
if not usage or (usage.total_tokens or 0) <= 0 then
  fail("session:last_usage did not return token usage")
end
local context = session:context()
if not context or not context.context_window_tokens or context.context_window_tokens <= 0 then
  fail("session:context did not return model context metadata")
end

session:close()
agent:close()
client:close()

print("cai lua session continuity e2e passed")
