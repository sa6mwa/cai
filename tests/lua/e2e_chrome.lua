local cai = require("cai")
local socket, endpoint, workspace, finish = table.unpack(arg)
local client = assert(cai.open({api_key = "fixture", base_url = endpoint,
  logger_disabled = true}))
local chunks = {}
local runtime = assert(client:new_smith_runtime({
  workspace_directory = workspace,
  disable_default_session_store = true,
  disable_terminal = true,
  disable_review_subagent = true,
  chrome = {socket_path = socket, timeout_ms = 2000},
  event_callback = function(event)
    if event.type == cai.AGENT_EVENT_TEXT_DELTA then
      chunks[#chunks + 1] = event.data
    end
  end,
}))
local initial_ids = assert(runtime:ids())
assert(initial_ids.session_id == runtime:session_id() and initial_ids.response_id == nil)
assert(runtime:submit("Inspect the browser"))
local state
repeat
  state = assert(runtime:pump(50))
until state == "completed" or state == "failed" or state == "cancelled"
assert(state == "completed", state)
assert(table.concat(chunks) == "Chrome final response")
local ids = assert(runtime:ids())
assert(ids.session_id == initial_ids.session_id)
assert(ids.response_id:match("^resp_fixture_") and ids.conversation_id == nil)
if finish == "gc" then
  runtime = nil
  collectgarbage("collect")
else
  local _, err = runtime:close()
  assert(err == nil, err and err.message)
end
local _, err = client:close()
assert(err == nil, err and err.message)
print("Lua Chrome runtime passed: " .. finish)
