local cai = require("cai")
local function ok(value, err)
  assert(value, err and err.message or "operation failed")
  return value
end
local client = ok(cai.open {api_key="fixture", base_url=assert(arg[1]),
  logger_disabled=true, http_2_disabled=1})
local events, compact_started, compact_done = {}, 0, 0
local function settle(runtime, expected)
  for _=1,200 do
    local state = ok(runtime:pump(50))
    if state == "completed" or state == "failed" or state == "cancelled" then
      assert(state == expected, state)
      return
    end
  end
  error("runtime did not settle")
end
local function open(callback)
  return ok(client:new_smith_runtime {workspace_directory=assert(arg[2]),
    disable_default_session_store=true, disable_terminal=true,
    disable_chrome=true, disable_web_search=true, model="gpt-5-nano",
    event_callback=callback})
end
local runtime = open(function(event)
  events[#events+1] = event
  if event.type == cai.AGENT_EVENT_COMPACTION_STARTED then compact_started=compact_started+1 end
  if event.type == cai.AGENT_EVENT_COMPACTION_COMPLETED then compact_done=compact_done+1 end
end)
assert(runtime:can_compact() == true)
ok(runtime:compact()) -- empty context fails asynchronously, without inference
assert(runtime:can_compact() == false)
assert(runtime:compact() == nil, "duplicate compaction must be rejected")
settle(runtime, "failed")
ok(runtime:submit("LUA_MANUAL_SOURCE"))
settle(runtime, "completed")
assert(runtime:can_compact() == true)
local before = #events
ok(runtime:compact())
assert(runtime:can_compact() == false)
assert(runtime:submit_steering("MUST_NOT_STEER_COMPACTION") == nil)
settle(runtime, "completed")
local saw_start, saw_finish = false, false
for i=before+1,#events do
  local event = events[i]
  if event.type == cai.AGENT_EVENT_RUN_STARTED then
    assert(event.data == nil, "manual compaction must not fabricate a prompt")
    saw_start=true
  elseif event.type == cai.AGENT_EVENT_COMPACTION_COMPLETED then
    saw_finish=true
  end
  assert(event.type ~= cai.AGENT_EVENT_TEXT_DELTA, "compactor must not emit an answer")
  assert(event.type ~= cai.AGENT_EVENT_TOOL_CALL_STARTED, "compactor must not execute tools")
end
assert(saw_start and saw_finish)
assert(compact_started >= 2 and compact_done == 1)
local history = ok(runtime:history())
assert(#history == 1 and history[1].kind == "compaction")
ok(runtime:submit("LUA_CONTINUE_AFTER_MANUAL"))
settle(runtime, "completed")
runtime:close()

-- Hosts that do not subscribe to events still receive correct pollable states.
runtime = open(nil)
ok(runtime:submit("POLL_ONLY_PROMPT"))
settle(runtime, "completed")
ok(runtime:compact())
settle(runtime, "completed")
assert(runtime:can_compact() == true)
assert(ok(runtime:history())[1].kind == "compaction")
runtime:close(); client:close()
