local cai = require("cai")

local function assert_eq(actual, expected, label)
  if actual ~= expected then
    error((label or "value") .. ": expected " .. tostring(expected) ..
      " got " .. tostring(actual), 2)
  end
end

local function assert_ok(value, err, label)
  if not value then
    error((label or "operation") .. " failed: " ..
      (type(err) == "table" and (err.message or err.status_string) or tostring(err)), 2)
  end
  return value
end

local function assert_not_ok(value, err, label)
  if value then
    error((label or "operation") .. ": expected failure", 2)
  end
  assert(err ~= nil, (label or "operation") .. ": expected error detail")
  return err
end

local function assert_throws(fn, label)
  local ok, err = pcall(fn)
  if ok then
    error((label or "operation") .. ": expected Lua error", 2)
  end
  assert(err ~= nil, (label or "operation") .. ": expected Lua error detail")
  return err
end

local function spool_text(text, chunk_size)
  return {
    pos = 1,
    rewind = function(self)
      self.pos = 1
      return true
    end,
    read = function(self, n)
      local size = chunk_size or n or 4096
      local first = self.pos
      local last
      if first > #text then
        return nil
      end
      last = first + size - 1
      self.pos = last + 1
      return text:sub(first, last)
    end,
  }
end

local function bad_spool_read_error()
  return {
    rewind = function()
      return true
    end,
    read = function()
      error("reader exploded")
    end,
  }
end

local function bad_spool_rewind_false()
  return {
    rewind = function()
      return nil, "no rewind"
    end,
    read = function()
      return "unreachable"
    end,
  }
end

local function bad_spool_empty_forever()
  return {
    rewind = function()
      return true
    end,
    read = function()
      return ""
    end,
  }
end

assert(type(cai.open) == "function")
assert(type(cai.tool_registry) == "function")
assert(type(cai.mcp_handler) == "function")
assert(type(cai.tool_schema) == "function")
assert(type(cai.load_dotenv_api_key) == "function")
assert_eq(cai.CONTINUITY_SERVER, 0, "server continuity")
assert_eq(cai.DEFAULT_DOTENV_PATH, ".env", "default dotenv path")
assert_eq(cai.OPENAI_API_KEY_ENV, "OPENAI_API_KEY", "OpenAI env name")
assert_eq(cai.OPENROUTER_API_KEY_ENV, "OPENROUTER_API_KEY", "OpenRouter env name")
assert_eq(cai.TOOL_CHOICE_AUTO, "auto", "tool choice auto")
assert_eq(cai.TOOL_CHOICE_NONE, "none", "tool choice none")
assert_eq(cai.TOOL_CHOICE_REQUIRED, "required", "tool choice required")
assert_eq(cai.REASONING_EFFORT_MINIMAL, "minimal", "reasoning effort minimal")
assert_eq(cai.REASONING_SUMMARY_AUTO, "auto", "reasoning summary auto")
assert(type(cai.MODEL_GPT_5_NANO) == "string")
assert_eq(cai.MODEL_DEFAULT_RESPONSES, cai.MODEL_GPT_5_NANO, "default model")
assert_eq(cai.MODEL_GPT_4O, "gpt-4o", "model constant")
assert(type(cai.MODEL_CAP_RESPONSES) == "number")
assert(type(cai.MODEL_META_PROVIDER_OPENROUTER) == "number")
assert(type(cai.OPENROUTER_MODEL_POOLSIDE_LAGUNA_XS_2_FREE) == "string")
local model = cai.model_info(cai.MODEL_GPT_5_NANO)
assert(type(model) == "table")
assert(model.context_window_tokens > 0)
assert(model.auto_compact_token_limit > 0)

local dummy_client = assert_ok(cai.open({ api_key = "test-key", timeout_ms = 1 }))
local dotenv_path = "/tmp/cai-lua-dotenv-test.env"
do
  local fp = assert(io.open(dotenv_path, "w"))
  fp:write("OPENAI_API_KEY=lua-dotenv-key\n")
  fp:close()
end
assert_eq(assert_ok(cai.load_dotenv_api_key(dotenv_path)), "lua-dotenv-key",
  "Lua dotenv helper")
local dotenv_value, dotenv_err = cai.load_dotenv_api_key("", nil)
assert_not_ok(dotenv_value, dotenv_err, "empty Lua dotenv path")
local dummy_agent = assert_ok(dummy_client:new_agent({
  model = cai.MODEL_GPT_5_NANO,
  instructions = "offline lua test",
  session_continuity = cai.CONTINUITY_CLIENT_HISTORY,
  history_memory_limit = 128,
}))
local dummy_session = assert_ok(dummy_agent:new_session())
assert_ok(dummy_session:set_previous_response_id("resp_lua_test"))
assert_ok(dummy_session:set_conversation_id("conv_lua_test"))
assert_ok(dummy_session:add_user_text("hello"))
assert_ok(dummy_session:add_user_text_spooled(spool_text("spooled hello", 3)))
assert_ok(dummy_session:add_user_file_data_spooled(
  "notes.txt", spool_text("spooled file data", 4)))
local source_part = 0
assert_ok(dummy_session:add_user_text_source(function()
  source_part = source_part + 1
  return ({ "streamed ", "hello", nil })[source_part]
end))
assert_not_ok(dummy_session:add_user_text_spooled({ read = "not callable" }),
  "spooled reader with non-callable read must fail")
assert_not_ok(dummy_session:add_user_text_spooled(bad_spool_rewind_false()),
  "spooled reader with failing rewind must fail")
assert_not_ok(dummy_session:add_user_text_spooled(bad_spool_read_error()),
  "spooled reader with throwing read must fail")
assert_not_ok(dummy_session:add_user_text_spooled(bad_spool_empty_forever()),
  "spooled reader with unbounded empty chunks must fail")
local ids = dummy_session:ids()
assert_eq(ids.conversation_id, "conv_lua_test", "session conversation id")
local state_chunks = {}
assert_ok(dummy_session:export_state(function(chunk)
  state_chunks[#state_chunks + 1] = chunk
  return true
end))
assert(table.concat(state_chunks):match("conv_lua_test"))
dummy_session:close()
dummy_agent:close()
dummy_client:close()
local dummy_openrouter = assert_ok(cai.open({
  openrouter = true,
  api_key = "test-key",
  timeout_ms = 1,
}))
dummy_openrouter:close()

local params = assert_ok(cai.response_params())
assert_ok(params:set_model(cai.MODEL_GPT_5_NANO))
assert_ok(params:set_instructions("Lua low-level params test"))
assert_ok(params:set_prompt_cache_key("cai:lua:test"))
assert_ok(params:set_tool_choice(cai.TOOL_CHOICE_AUTO))
assert_ok(params:set_max_output_tokens(128))
assert_ok(params:set_parallel_tool_calls(true))
assert_ok(params:set_compact_threshold(320000))
assert_ok(params:set_reasoning("minimal", "auto"))
assert_ok(params:set_text_format_json_object())
assert_ok(params:set_text_format_json_schema("lua_test", "Lua schema test", '{"type":"object","properties":{"ok":{"type":"boolean"}},"additionalProperties":false}', true))
assert_ok(params:add_text("user", "hello"))
assert_ok(params:add_text_spooled("user", spool_text("params spooled text", 5)))
assert_ok(params:add_image_url("user", "https://example.com/image.png", "low"))
assert_ok(params:add_image_file_id("user", "file_lua_image", "low"))
assert_ok(params:add_file_id("user", "file_lua_doc"))
assert_ok(params:add_file_data_spooled(
  "user", "params.txt", spool_text("params file data", 6)))
assert_ok(params:add_file_url("user", "https://example.com/file.txt"))
assert_ok(params:add_function_tool("noop", "No-op test tool", '{"type":"object","properties":{},"additionalProperties":false}', true))
assert_ok(params:add_function_call_output("call_test", '{"ok":true}'))
assert_ok(params:add_function_call_output_text("call_text", "plain tool result"))
assert_ok(params:add_function_call_output_image_url("call_image", "https://example.com/out.png", "low"))
assert_ok(params:add_function_call_output_file_id("call_file", "file_lua_result"))
assert_ok(params:add_function_call_output_file_data_spooled(
  "call_file_data", "result.txt", spool_text("function result file", 7)))
assert_not_ok(params:add_text_spooled("user", bad_spool_read_error()),
  "response params spooled read error must fail")
params:close()

local conversation = assert_ok(cai.conversation_from_id("conv_lua_test"))
assert_eq(conversation:id(), "conv_lua_test", "conversation id")
conversation:close()

local conv_params = assert_ok(cai.conversation_items_params())
assert_ok(conv_params:add_text("user", "hello"))
assert_ok(conv_params:add_text_spooled("user", spool_text("conv spooled text", 2)))
local part = 0
assert_ok(conv_params:add_text_source("user", function()
  part = part + 1
  return ({ "large ", "text", nil })[part]
end))
assert_ok(conv_params:add_image_url("user", "https://example.com/i.png", "low"))
assert_ok(conv_params:add_image_file_id("user", "file_lua_conv_image", "low"))
assert_ok(conv_params:add_file_id("user", "file_lua_conv_doc"))
assert_ok(conv_params:add_file_data_spooled(
  "user", "conv.txt", spool_text("conv file data", 3)))
assert_ok(conv_params:add_file_url("user", "https://example.com/f.txt"))
assert_not_ok(conv_params:add_text_spooled("user", bad_spool_rewind_false()),
  "conversation params spooled rewind error must fail")
conv_params:close()

local schema = assert_ok(cai.tool_schema())
assert_ok(schema:set_strict(true))
assert_ok(schema:string("city", "City name", true))
assert_ok(schema:integer("days", "Forecast days", false))
assert_ok(schema:string_enum("unit", "Temperature unit", { "c", "f" }, false))
assert_ok(schema:describe("city", "City to check"))
assert_ok(schema:raw_property("extra", "Raw schema", '{"type":"object"}', false))
assert(schema:strict())
local schema_json = schema:json()
assert(schema_json:match('"city"'))
assert(schema_json:match('"required"'))
schema:close()

local registry = assert_ok(cai.tool_registry())
local weather_schema = [[
{"type":"object","properties":{"city":{"type":"string"}},"required":["city"],"additionalProperties":false}
]]
assert_ok(registry:register_raw_tool("lua_weather", "Lua weather test tool", weather_schema, function(args_json)
  assert(args_json:match("Gothenburg"))
  return '{"ok":true,"summary":"dry enough"}'
end, true))

assert_ok(registry:register_raw_spooled_tool("lua_spooled_weather", "Lua spooled weather test tool", weather_schema, function(args)
  local arguments = args:read_all()
  local out = { '{"ok":', 'true,"summary":"spooled ', 'dry enough"}', nil }
  local i = 0
  assert(arguments:match("Gothenburg"))
  assert(args:size() > 0)
  return function()
    i = i + 1
    return out[i]
  end
end, true))
assert_ok(registry:register_raw_spooled_tool("lua_throwing_tool", "Lua throwing callback test tool", weather_schema, function()
  error("tool exploded")
end, true))

local raw_chunks = {}
assert_ok(registry:run("lua_weather", '{"city":"Gothenburg"}', function(chunk)
  raw_chunks[#raw_chunks + 1] = chunk
  return true
end))
local raw_json = table.concat(raw_chunks)
assert(raw_json:match('"ok":true'))
assert(raw_json:match('"summary":"dry enough"'))

local spooled_chunks = {}
assert_ok(registry:run("lua_spooled_weather", '{"city":"Gothenburg"}', function(chunk)
  spooled_chunks[#spooled_chunks + 1] = chunk
  return true
end))
local spooled_json = table.concat(spooled_chunks)
assert(spooled_json:match('"ok":true'))
assert(spooled_json:match('"summary":"spooled dry enough"'))
assert_not_ok(registry:run("lua_spooled_weather", '{"city":"Gothenburg"}', function()
  return false
end), "registry run must propagate sink cancellation")
assert_not_ok(registry:run("lua_throwing_tool", '{"city":"Gothenburg"}', function()
  return true
end), "raw spooled tool must propagate callback failure")
assert_not_ok(registry:run("lua_spooled_weather", '{"city":', function()
  return true
end), "registry run must reject invalid arguments JSON")

os.remove("/tmp/cai-lua-test-todo.json")
os.remove("/tmp/cai-lua-test-todo.lock")
assert_ok(registry:register_todo_tool({
  store_path = "/tmp/cai-lua-test-todo.json",
  lock_path = "/tmp/cai-lua-test-todo.lock",
  default_board = "lua",
}))

local chunks = {}
assert_ok(registry:run("todo_kanban", '{"operation":"help"}', function(chunk)
  chunks[#chunks + 1] = chunk
  return true
end))
local help_json = table.concat(chunks)
assert(help_json:match("todo_kanban"))
assert(help_json:match("operation"))

local mcp = assert_ok(cai.mcp_handler({
  name = "cai-lua-test",
  version = "0.0.0",
  tools = registry,
  allow_legacy_no_version = 1,
}))

assert_throws(function()
  mcp:handle_http({
    method = "POST",
    headers = {
      ["content-type"] = "application/json",
      ["mcp-protocol-version"] = cai.MCP_PROTOCOL_VERSION,
    },
    body = "{}",
  })
end, "mcp handler without streaming writer must throw")

local body_parts = {
  '{"jsonrpc":"2.0","id":"1","method":"tools/list","params":',
  '{}',
  '}',
}
local body_index = 0
local response_chunks = {}
local response = assert_ok(mcp:handle_http({
  method = "POST",
  headers = {
    ["content-type"] = "application/json",
    ["mcp-protocol-version"] = cai.MCP_PROTOCOL_VERSION,
  },
  body = function()
    body_index = body_index + 1
    return body_parts[body_index]
  end,
  write = function(chunk)
    response_chunks[#response_chunks + 1] = chunk
    return true
  end,
}))

assert_eq(response.status, 200, "mcp status")
assert(type(response.headers) == "table")
local response_json = table.concat(response_chunks)
assert(response_json:match("todo_kanban"))
assert(response_json:match('"jsonrpc"'))
assert(body_index > 1, "request body should be consumed in chunks")

mcp:close()
registry:close()
os.remove("/tmp/cai-lua-test-todo.json")
os.remove("/tmp/cai-lua-test-todo.lock")

print("cai lua tests passed")
