local cai = require("cai")
local function ok(value, err)
  assert(value, err and ((err.message or "operation failed") .. ": " .. (err.detail or "")) or "operation failed")
  return value
end
local client = ok(cai.open {api_key="fixture", base_url="http://127.0.0.1:1/v1", logger_disabled=true})
local agent = ok(client:new_agent {model="gpt-6-luna", enable_local_history=true,
  session_continuity=cai.CONTINUITY_CLIENT_HISTORY})
local session = ok(agent:new_session())
local input = assert(io.open(arg[1], "rb"))
ok(session:restore_journal(function() return input:read(7) end))
input:close()
local parts = {}
ok(session:export_history(function(chunk) parts[#parts+1]=chunk; return true end))
local history = table.concat(parts)
assert(history:find("FIRST%-CONTEXT%-λ"))
assert(history:find("PROMPT 3", 1, true) and history:find("REPLY 3", 1, true))
assert(ok(session:usage()).usage.total_tokens == 80)
session:close()
-- The low-level runtime exporter is available to Lua hosts without an app or
-- provider call, and produces a portable initialization commit.
local runtime = ok(client:new_smith_runtime {workspace_directory=assert(arg[2]),
  model="gpt-6-luna", disable_default_session_store=true,
  disable_terminal=true, disable_chrome=true, disable_web_search=true,
  disable_review_subagent=true, disable_skills=true})
parts = {}
ok(runtime:export_journal_state(function(chunk) parts[#parts+1]=chunk; return true end))
local data, offset = table.concat(parts), 1
session = ok(agent:new_session())
ok(session:restore_journal(function()
  if offset > #data then return nil end
  local chunk = data:sub(offset, offset)
  offset = offset+1
  return chunk
end))
session:close(); runtime:close(); agent:close(); client:close()
