local cai = require("cai")
local root, endpoint = assert(arg[1]), assert(arg[2])
local function ok(value, err)
  assert(value, err and err.message or "operation failed")
  return value
end
local client = ok(cai.open({api_key="fixture", base_url=endpoint,
  api_protocol="chat_completions", http_2_disabled=1, chat_max_attempts=1}))
local agent = ok(client:new_agent({model="fixture", disable_auto_compaction=1}))
local function session()
  local s = ok(agent:new_session())
  ok(s:add_user_text("hello"))
  return s
end
local s = session()
local count, retained = 0, nil
ok(s:stream({
  response_prefix="NOT_MESSAGE_TEXT", response_suffix="NOT_MESSAGE_TEXT",
  on_output_item_done=function(item)
    count = count + 1
    assert(item.type == "message" and item.output_index == 0)
    assert(item.id == "chat_text")
    retained = item.json_spooled
    return true
  end,
}))
assert(count == 1)
local f = assert(io.open(root.."/item.json", "w"))
assert(f:write(retained:read_all())); assert(f:close())
s:close()
s = session()
count = 0
local value, err = s:stream({on_output_item_done=function()
  count = count + 1
  return false
end})
assert(not value and err and count == 1)
s:close()
local params = ok(cai.response_params())
ok(params:set_model("fixture")); ok(params:add_text("user", "buffered"))
local response = ok(client:create_response(params))
assert(response:output_text() == "buffered")
assert(#response:output_items() == 1)
response:close(); params:close()
local calls = 0
ok(agent:register_raw_tool("echo", "Echo", '{"type":"object"}', function()
  calls = calls + 1
  return '{"ok":true}'
end))
s = session()
local types = {}
ok(s:stream({on_output_item_done=function(item)
  types[#types+1] = item.type
  return true
end}))
assert(table.concat(types, ",") == "message,function_call,message")
assert(calls == 1)
s:close(); agent:close(); client:close()
