local cai = require("cai")

local function assert_ok(value, err)
  if not value then
    error(type(err) == "table" and (err.message or err.status_string) or tostring(err), 2)
  end
  return value
end

local client = assert_ok(cai.open())
local conversation = assert_ok(client:create_conversation())
local params = assert_ok(cai.conversation_items_params())
assert_ok(params:add_text("user", "Remember that this conversation was created from Lua."))
local items = assert_ok(client:create_conversation_items(conversation, params))

print("conversation_id=" .. tostring(conversation:id()))
print("items=" .. tostring(items:summary().count))

items:close()
params:close()
conversation:close()
client:close()
