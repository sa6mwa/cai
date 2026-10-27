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
assert(runtime:submit("Exercise Lua runtime execution policy"))
for _=1,100 do
  local state = assert(runtime:pump(100))
  assert(state ~= "failed", "YOLO runtime failed")
  if state == "completed" then break end
end
assert(runtime:state() == "completed")
runtime:close(); client:close()
