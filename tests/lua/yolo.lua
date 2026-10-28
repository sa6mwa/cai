local cai = require("cai")
local endpoint, work = assert(arg[1]), assert(arg[2])
local registry = assert(cai.tool_registry())
assert(registry:register_exec_tool {root_path=work, yolo=true,
  bwrap_path="/no-such-bubblewrap"})
assert(registry:register_read_tool {root_path=work, yolo=true})
local chunks = {}
local function capture(chunk) chunks[#chunks+1] = chunk; return true end
assert(registry:run("exec_command", [[{"cmd":"printf LUA_YOLO_EXEC; cat ../outside"}]], capture))
assert(table.concat(chunks):find("LUA_YOLO_EXEC", 1, true))
assert(table.concat(chunks):find("LUA_YOLO_OUTSIDE", 1, true))
chunks = {}
assert(registry:run("read_file", [[{"path":"../outside"}]], capture))
assert(table.concat(chunks):find("LUA_YOLO_OUTSIDE", 1, true))
registry:close()
local client = assert(cai.open {api_key="fixture", base_url=endpoint,
  logger_disabled=true, http_2_disabled=1})
local runtime = assert(client:new_smith_runtime {workspace_directory=work,
  yolo=true, disable_default_session_store=true, disable_terminal=true,
  disable_chrome=true, disable_web_search=true, model="gpt-6-luna"})
local policy = assert(runtime:yolo())
assert(policy.enabled and policy.can_change)
assert(runtime:try_set_yolo(false))
assert(not runtime:yolo().enabled)
assert(runtime:try_set_yolo(true))
assert(runtime:yield_time_ms() == 10000)
assert(runtime:yield_time_ms(0) == 0)
assert(runtime:yield_time_ms(250) == 250)
assert(not pcall(function() runtime:yield_time_ms(-1) end))
assert(not pcall(function() runtime:yield_time_ms(30001) end))
assert(runtime:yield_time_ms() == 250)
assert(runtime:yield_time_ms(10000) == 10000)
assert(runtime:submit("Exercise Lua runtime execution policy"))
assert(not runtime:try_set_yolo(false))
assert(runtime:yolo().enabled and not runtime:yolo().can_change)
for _=1,100 do
  local state = assert(runtime:pump(100))
  assert(state ~= "failed", "YOLO runtime failed")
  if state == "completed" then break end
end
assert(runtime:state() == "completed")
assert(runtime:try_set_yolo(false))
assert(not runtime:yolo().enabled)
runtime:close(); client:close()
-- Constructor defaults and overrides share the same native runtime policy.
client = assert(cai.open {api_key="fixture", base_url=endpoint,
  logger_disabled=true, http_2_disabled=1})
for _, settings in ipairs({{yield_time_ms=0}, {yield_time_ms=25},
                           {foreground=true, yield_time_ms=25}}) do
  settings.workspace_directory=work
  settings.disable_default_session_store=true
  settings.disable_terminal=true
  settings.disable_chrome=true
  settings.disable_web_search=true
  settings.model="gpt-6-luna"
  runtime = assert(client:new_smith_runtime(settings))
  assert(runtime:yield_time_ms() ==
         ((settings.foreground or settings.yield_time_ms == 0) and 0 or 25))
  runtime:close()
end
client:close()
