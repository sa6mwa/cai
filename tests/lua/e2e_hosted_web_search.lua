local cai = require("cai")

local function skip(message)
  io.stdout:write("SKIP: " .. message .. "\n")
  os.exit(0)
end

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

local function api_key_config()
  if os.getenv("OPENAI_API_KEY") ~= nil and os.getenv("OPENAI_API_KEY") ~= "" then
    return nil
  end
  local key = cai.load_dotenv_api_key(cai.DEFAULT_DOTENV_PATH, cai.OPENAI_API_KEY_ENV)
  if key ~= nil and key ~= "" then
    return key
  end
  return nil
end

if os.getenv("CAI_LUA_HOSTED_WEB_SEARCH_E2E") ~= "1" then
  skip("set CAI_LUA_HOSTED_WEB_SEARCH_E2E=1 to run Lua hosted web_search e2e")
end

local api_key = api_key_config()
if (os.getenv("OPENAI_API_KEY") == nil or os.getenv("OPENAI_API_KEY") == "") and
    api_key == nil then
  skip("OPENAI_API_KEY is not set")
end

local model = os.getenv("CAI_TEST_MODEL")
if model == nil or model == "" then
  model = cai.MODEL_GPT_5_NANO
end

io.stderr:write("[lua-hosted-web-search] model=" .. model .. "\n")

local client = assert_ok(cai.open({
  timeout_ms = 60000,
  api_key = api_key,
}), nil, "cai.open")

local params = assert_ok(cai.response_params(), nil, "cai.response_params")
assert_ok(params:set_model(model), nil, "params:set_model")
assert_ok(params:set_reasoning(cai.REASONING_EFFORT_LOW), nil,
  "params:set_reasoning")
assert_ok(params:set_tool_choice(cai.TOOL_CHOICE_REQUIRED), nil,
  "params:set_tool_choice")
assert_ok(params:set_max_output_tokens(512), nil,
  "params:set_max_output_tokens")
assert_ok(params:add_hosted_tool_json(
  '{"type":"web_search","search_context_size":"low"}'
), nil, "params:add_hosted_tool_json")
assert_ok(params:add_text(
  "user",
  "Use web search and answer in one sentence: what is the latest OpenAI " ..
    "model family mentioned in OpenAI docs?"
), nil, "params:add_text")

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
