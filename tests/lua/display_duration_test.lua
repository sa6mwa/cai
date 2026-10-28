local cai = require("cai")
local out = {}
local display = assert(cai.tool_display({color=true}, function(bytes) out[#out+1]=bytes end))
assert(display:event{type=cai.AGENT_EVENT_TERMINAL_COMMAND_STARTED,
  tool_call_id="live",runtime_session_id="s",data="command"})
assert(display:event{type=cai.AGENT_EVENT_TERMINAL_COMMAND_COMPLETED,
  tool_call_id="live",runtime_session_id="s",terminal_duration_ms=5025000,
  terminal_has_exit_code=true,terminal_exit_code=0})
assert(table.concat(out):find("Ran for 1h23m45s",1,true))
assert(table.concat(out):find("\27[0;36mRan for 1h23m45s \27[1;96mcommand",1,true))
out={}
assert(display:replay_begin({type=cai.AGENT_EVENT_TOOL_CALL_COMPLETED,
  tool_call_id="saved",runtime_session_id="s",terminal_duration_ms=63000},"Ran","saved command",true))
assert(table.concat(out):find("Ran for 1m3s",1,true))
assert(not pcall(display.event,display,{type=cai.AGENT_EVENT_TOOL_CALL_COMPLETED,terminal_duration_ms=-1}))
out={}
assert(display:event{type=cai.AGENT_EVENT_REASONING_TOOL_RESULT,
  tool_call_id="reason",runtime_session_id="s",
  data="requested high (high): medium -> high"})
assert(table.concat(out):find("requested high (high): medium -> high",1,true))
display:close()
