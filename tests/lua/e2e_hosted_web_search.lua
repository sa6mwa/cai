local cai = require("cai")
local chatgpt_e2e = dofile("tests/lua/chatgpt_e2e.lua")

local function fail(message)
  error(message, 0)
end

local function assert_ok(value, err, label)
  if not value then
    local detail = tostring(err)
    if type(err) == "table" then
      detail = err.detail or err.message or err.status_string or detail
    end
    fail((label or "operation") .. " failed: " .. detail)
  end
  return value
end

local auth_path = chatgpt_e2e.require_live_auth(cai,
  "CAI_LUA_HOSTED_WEB_SEARCH_E2E")
local model = cai.MODEL_GPT_5_NANO

io.stderr:write("[lua-hosted-web-search] model=" .. model .. "\n")

local client, client_err = cai.open({
  timeout_ms = 60000,
  chatgpt_auth_json = auth_path,
})
client = assert_ok(client, client_err, "cai.open")

local params = assert_ok(cai.response_params(), nil, "cai.response_params")
assert_ok(params:set_model(model), nil, "params:set_model")
assert_ok(params:set_reasoning(cai.REASONING_EFFORT_LOW), nil,
  "params:set_reasoning")
assert_ok(params:set_tool_choice_json('{"type":"web_search"}'), nil,
  "params:set_tool_choice_json")
assert_ok(params:set_max_output_tokens(512), nil,
  "params:set_max_output_tokens")
assert_ok(params:set_max_tool_calls(1), nil, "params:set_max_tool_calls")
assert_ok(params:add_hosted_tool_json(
  '{"type":"web_search","search_context_size":"low"}'
), nil, "params:add_hosted_tool_json")
assert_ok(params:add_text(
  "user",
  "Use web search and answer in one sentence: what is the latest OpenAI " ..
    "model family mentioned in OpenAI docs?"
), nil, "params:add_text")

local token_count = assert_ok(client:count_response_input_tokens(params), nil,
  "client:count_response_input_tokens")
if (token_count.input_tokens or 0) <= 0 then
  fail("hosted web search input token count was empty")
end

local response = assert_ok(client:create_response(params), nil,
  "client:create_response")
local items_json = assert_ok(response:output_items_json(), nil,
  "response:output_items_json")
local text = response:output_text()
local usage = assert_ok(response:usage(), nil, "response:usage")

if not items_json:find('"web_search_call"', 1, true) then
  fail("hosted web search did not produce web_search_call: " .. items_json)
end
if text == nil or text == "" then
  fail("hosted web search response had no output text")
end
if (usage.total_tokens or 0) <= 0 then
  fail("hosted web search response had no token usage")
end

response:close()
params:close()
client:close()

print("cai lua hosted web_search e2e passed")
