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

if os.getenv("CAI_LUA_USAGE_LIMITS_E2E") ~= "1" then
  skip("set CAI_LUA_USAGE_LIMITS_E2E=1 to run Lua usage limits e2e")
end
local api_key = api_key_config()
if (os.getenv("OPENAI_API_KEY") == nil or os.getenv("OPENAI_API_KEY") == "") and
    api_key == nil then
  skip("OPENAI_API_KEY is not set")
end

local client = assert_ok(cai.open({ timeout_ms = 60000, api_key = api_key }), nil, "cai.open")
local agent = assert_ok(client:new_agent({
  model = cai.MODEL_GPT_5_NANO,
  instructions = "You are a Lua usage-limit integration test. Obey the user exactly.",
  reasoning_effort = cai.REASONING_EFFORT_MINIMAL,
  max_output_tokens = 64,
  session_usage_limits = {
    max_output_tokens = 1,
  },
}), nil, "client:new_agent")

local session = assert_ok(agent:new_session(), nil, "agent:new_session")
assert_ok(session:add_user_text(
  "Reply with exactly this sentence: alpha beta gamma delta epsilon."
), nil, "session:add_user_text")

local ok_stream, err = session:stream({
  response = function()
    return true
  end,
})
if ok_stream then
  fail("session:stream unexpectedly succeeded with max_output_tokens=1")
end
if type(err) ~= "table" or err.message ~= "configured usage or spend limit exceeded" then
  fail("unexpected usage-limit error: " .. tostring(type(err) == "table" and err.message or err))
end

local usage = assert_ok(session:usage(), nil, "session:usage")
if usage.limit_exceeded ~= true and usage.limit_exceeded ~= 1 then
  fail("session usage was not marked limit_exceeded")
end
if not usage.usage or (usage.usage.output_tokens or 0) <= 1 then
  fail("provider output usage did not exceed the configured cap")
end

assert_ok(session:add_user_text("This should fail before transport."), nil,
  "session:add_user_text second")
local ok_again, err_again = session:stream({
  response = function()
    return true
  end,
})
if ok_again then
  fail("session:stream unexpectedly succeeded after usage limit was exceeded")
end
if type(err_again) ~= "table" or
    err_again.message ~= "configured usage or spend limit exceeded" then
  fail("unexpected preflight usage-limit error: " ..
    tostring(type(err_again) == "table" and err_again.message or err_again))
end

session:close()
agent:close()
client:close()
