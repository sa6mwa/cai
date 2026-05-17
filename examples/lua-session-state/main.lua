local cai = require("cai")

local function assert_ok(value, err)
  if not value then
    error(type(err) == "table" and (err.message or err.status_string) or tostring(err), 2)
  end
  return value
end

local path = arg[1] or "/tmp/cai-lua-session-state.json"
local client = assert_ok(cai.open())
local agent = assert_ok(client:new_agent({
  model = cai.MODEL_GPT_5_NANO,
  instructions = "You are a terse Lua session-state example assistant.",
  reasoning_effort = cai.REASONING_EFFORT_MINIMAL,
}))

local session = assert_ok(agent:new_session())
assert_ok(session:add_user_text("Say 'state ready' in exactly two words."))
local output = assert_ok(session:run_output())
print(output:text() or "")
assert_ok(session:save_state_path(path))

local restored = assert_ok(agent:new_session())
assert_ok(restored:load_state_path(path))
assert_ok(restored:add_user_text("Continue with exactly two more words."))
local restored_output = assert_ok(restored:run_output())
print(restored_output:text() or "")

restored_output:close()
restored:close()
output:close()
session:close()
agent:close()
client:close()
