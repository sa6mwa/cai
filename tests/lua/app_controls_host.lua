local application = require("cai.app")
local mode, starts, replays = "queue", 0, 0
local app
local resume_cancelled = false
app = assert(application.open {
  no_config = true,
  on_event = function(event)
    assert(type(event.type) == "number" and type(event.sequence) == "number")
    local status = assert(app:get_status())
    assert(status.running and status.runtime_ready and status.effective.model)
    if event.type == 1 then -- RUN_STARTED
      starts = starts + 1
      assert(not app:run {"--help"}) -- cannot disturb the active run
      assert(not app:close())
      if mode == "queue" and starts == 1 then
        assert(app:control("queue_prompt", "LUA NEXT PROMPT"))
      end
    end
    if mode == "goal_resume" then
      local cai = require("cai")
      if event.type == cai.AGENT_EVENT_TEXT_DELTA and not resume_cancelled then
        resume_cancelled = true
        assert(app:control("cancel_turn"))
      elseif event.type == cai.AGENT_EVENT_RUN_CANCELLED then
        assert(app:control("resume_goal"))
      end
    end
    if event.data and event.data:find("Observer failure", 1, true) then
      assert(app:control("queue_prompt", "ABANDON LUA PROMPT"))
      error("LUA OBSERVER FAILED")
    end
    if mode == "stop" and event.data then
      assert(app:control("stop"))
      mode = "stopping"
    end
  end,
  on_replay = function(event)
    assert(type(event.type) == "string" and type(event.sequence) == "number")
    replays = replays + 1
    if mode == "replay_error" then error("LUA REPLAY FAILED") end
  end,
})
assert(app:get_status().outcome == "not_run")
assert(not app:control("unknown"))
assert(not app:control("queue_prompt", "bad\0text"))
assert(not app:control("queue_prompt", "not running"))
local function run(...)
  local args = {}
  for _, value in ipairs(arg) do args[#args + 1] = value end
  for _, value in ipairs {...} do args[#args + 1] = value end
  return assert(app:run(args))
end
if os.getenv("CAI_TEST_APP_LUA_INTERACTIVE_ERROR") then
  mode = "interactive_error"
  local code = assert(app:run(arg))
  local failed = assert(app:get_status())
  assert(code == 1 and failed.outcome == "failed")
  assert(failed.error.message:find("LUA OBSERVER FAILED", 1, true))
  app:close()
  os.exit(code)
end
if os.getenv("CAI_TEST_APP_LUA_GOAL_RESUME") then
  mode = "goal_resume"
  assert(run("-Nn", "--goal", "Resume immediately after host cancellation") == 0)
  local status = assert(app:get_status())
  assert(starts == 2 and resume_cancelled and status.outcome == "completed")
  assert(status.goal.status == "complete" and status.control_status == 0)
  app:close()
  os.exit(0)
end
assert(run("-Nni", "initial") == 0)
local retained = assert(app:get_status())
assert(starts == 2 and retained.outcome == "completed" and not retained.running)
assert(retained.metrics.has_last_turn and retained.metrics.session_usage.total_tokens >= 40)
assert(retained.control_id == 1 and retained.control_status == 0)
mode = "replay"
assert(run("-ni", "resume") == 0)
assert(replays > 0)
assert(app:get_status().control_id == 0)
mode = "error"
assert(run("-Nni", "observer failure") == 1)
local failed = app:get_status()
assert(failed.outcome == "failed" and failed.error.message:find("LUA OBSERVER FAILED", 1, true))
assert(failed.control_status ~= 0 and failed.control_error.message)
mode = "stop"
assert(run("-Nni", "stop") == 0)
assert(app:get_status().outcome == "stopped")
mode = "replay_error"
assert(run("-ni", "must not run") == 1)
assert(app:get_status().error.message:find("LUA REPLAY FAILED", 1, true))
app:close()
assert(retained.session_id and retained.effective.model)
assert(not app:get_status() and not app:control("stop"))
local weak = setmetatable({}, {__mode = "v"})
do
  local cycle
  cycle = assert(application.open {on_event = function() return cycle end})
  weak[1] = cycle
end
collectgarbage("collect")
collectgarbage("collect")
assert(weak[1] == nil)
