local cai = require("cai")
local root, endpoint, protocol = assert(arg[1]), assert(arg[2]), assert(arg[3])
local function ok(value, err)
  assert(value, err and err.message or "operation failed")
  return value
end
local function write(path, value)
  local file = assert(io.open(path, "w")); assert(file:write(value)); file:close()
end
local config = ok(cai.config())
ok(config:load("[agent]\ncodex_agents_md = true\n[runtime]\nshell_context_max_lines = 2\nshell_context_max_bytes = 4096\n"))
local function client(window)
  return ok(cai.open({api_key="fixture", base_url=endpoint, api_protocol=protocol,
    metadata_provider="custom", http_2_disabled=1,
    model_metadata_json=window and ('{"models":[{"provider":"custom","id":"cai-context","context_window_tokens":'..window..'}]}') or nil}))
end
local function open(c, resume, codex)
  return ok(c:new_smith_runtime({workspace_directory=root.."/work",
    model=not resume and "cai-context" or nil,
    resume_session_id=resume, disable_terminal=true,
    disable_chrome=true, disable_web_search=true,
    codex_compat_agents_md=codex,
    shell_context_max_lines=config:get_integer("runtime", "shell_context_max_lines"),
    shell_context_max_bytes=config:get_integer("runtime", "shell_context_max_bytes"),
  }))
end
local function turn(r, prompt)
  ok(r:submit(prompt))
  for _=1,200 do
    local state = ok(r:pump(50))
    assert(state ~= "failed" and state ~= "cancelled", state)
    if state == "completed" then return end
  end
  error("runtime did not settle")
end
local first = client(272000)
local runtime = open(first, nil, config:get_bool("agent", "codex_agents_md"))
assert(ok(runtime:metrics()).context_window_tokens == 272000)
local id = runtime:session_id()
ok(runtime:user_shell_begin("lua-hosted-pty", true))
ok(runtime:user_shell_write("DROPPED_LUA_PREFIX"))
local raw_chunk = string.rep("old output", 512)
for _=1,40 do ok(runtime:user_shell_write(raw_chunk)) end
ok(runtime:user_shell_write("\n"))
for _,piece in ipairs({"DROPPED_LUA_LINE\nkept-lua-one\n\27[3", "1mkept-lua-two\27[0m\n"}) do
  ok(runtime:user_shell_write(piece))
end
ok(runtime:user_shell_finish(7, 1250))
assert(not runtime:metrics().turn_active)
turn(runtime, "LUA_CONTEXT_FIRST")
local metrics = ok(runtime:metrics())
assert(metrics.context_window_tokens == 272000 and metrics.has_context_usage)
assert(metrics.has_last_turn and not metrics.turn_active)
assert(metrics.session_usage.usage.total_tokens > 0)
runtime:close(); first:close()
write(root.."/.codex/AGENTS.md", "LUA_CODEX_UPDATED\n")
local second = client(nil)
runtime = open(second, id, true)
assert(runtime:session_id() == id)
assert(runtime:metrics().context_window_tokens == 272000,
  "resume must use saved effective metadata before any provider request")
turn(runtime, "LUA_CONTEXT_RESUMED")
runtime:close(); second:close()
local third = client(1050000)
runtime = open(third, id, false)
assert(runtime:metrics().context_window_tokens == 1050000,
  "current provider metadata supersedes saved window")
turn(runtime, "LUA_CONTEXT_DISABLED")
runtime:close(); third:close(); config:close()
