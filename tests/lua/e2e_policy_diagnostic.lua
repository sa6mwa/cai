local cai = require("cai")
local endpoint, protocol = assert(arg[1]), assert(arg[2])
local client = assert(cai.open({api_key="fixture", base_url=endpoint,
  api_protocol=protocol, http_2_disabled=1}))
local started, failed, text = 0, 0, ""
local runtime = assert(client:new_smith_runtime({workspace_directory=assert(arg[3]),
  model="gpt-5.6-luna", reasoning_effort="low", disable_terminal=true,
  disable_chrome=true, disable_web_search=true, disable_default_session_store=true,
  event_callback=function(event)
    if event.type == cai.AGENT_EVENT_POLICY_DIAGNOSTIC_STARTED then
      started = started + 1
      assert(event.data:find("explanation only", 1, true))
    elseif event.type == cai.AGENT_EVENT_TEXT_DELTA then text = text .. event.data
    elseif event.type == cai.AGENT_EVENT_RUN_FAILED then
      failed = failed + 1
      assert(event.data:find("cyber_policy", 1, true))
    end
  end}))
assert(runtime:submit("LUA_POLICY_ORIGINAL_TASK"))
for _=1,200 do
  local state = assert(runtime:pump(50))
  if state == "failed" then break end
end
assert(runtime:state() == "failed")
assert(started == 1 and failed == 1, "one diagnostic and original failure required")
assert(text == "LUA_DIAGNOSTIC_EXPLANATION")
runtime:close(); client:close()
