local cai = require("cai")
local client = assert(cai.open {api_key="fixture", base_url=assert(arg[1]),
  logger_disabled=true, http_2_disabled=1})
local runtime = assert(client:new_smith_runtime {workspace_directory=assert(arg[2]),
  disable_default_session_store=true, disable_terminal=true, disable_chrome=true,
  disable_web_search=true, model="gpt-5-nano"})
assert(runtime:continue_goal() == false, "no goal must not start inference")
assert(runtime:replace_goal("Exercise Lua continuation policy"))
for _=1,100 do
  runtime:pump(100)
  if runtime:goal().has_goal then break end
end
for turn=1,3 do
  assert(runtime:continue_goal() == true)
  for _=1,100 do
    local state = assert(runtime:pump(100))
    assert(state ~= "failed")
    if state == "completed" then break end
  end
  assert(runtime:state() == "completed")
  assert(runtime:goal().status == (turn == 3 and "blocked" or "active"))
end
assert(runtime:continue_goal() == false, "blocked goal must stop inference")
runtime:close(); client:close()
