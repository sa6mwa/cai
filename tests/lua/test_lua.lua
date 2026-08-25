local cai = require("cai")
local pslog = require("pslog")
local native_store_test = require("cai_native_todo_store_test")

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
assert(type(cai.native_store) == "function")
assert(type(cai.native_backend) == "function")
assert(type(cai.mcp_handler) == "function")
assert(type(cai.mcp_client) == "function")
assert(type(cai.chatgpt_auth) == "function")
assert(type(cai.chatgpt_login) == "function")
assert(type(cai.chatgpt_login_browser_command) == "function")
assert(type(cai.chatgpt_login_open_browser) == "function")
assert(type(cai.chatgpt_auth_default_path) == "function")
assert(type(cai.chatgpt_login_browser_command()) == "string")
assert(cai.chatgpt_login_browser_command() ~= "")
assert(cai.MCP_DEFAULT_TOOL_OUTPUT_MAX_BYTES > 0)
assert_eq(cai.MCP_TOOL_OUTPUT_UNLIMITED, -1, "mcp unlimited sentinel")
assert_eq(cai.MCP_CLIENT_AUTH_NONE, 0, "mcp client no auth")
assert(type(cai.MCP_CLIENT_AUTH_API_KEY) == "number")
assert(type(cai.MCP_CLIENT_AUTH_OAUTH2_CLIENT_CREDENTIALS) == "number")
assert_eq(cai.MCP_CLIENT_TOOL_TASK_SUPPORT_UNSPECIFIED, 0,
  "mcp client task support unspecified")
assert(type(cai.MCP_CLIENT_TOOL_TASK_SUPPORT_FORBIDDEN) == "number")
assert(type(cai.MCP_CLIENT_TOOL_TASK_SUPPORT_OPTIONAL) == "number")
assert(type(cai.MCP_CLIENT_TOOL_TASK_SUPPORT_REQUIRED) == "number")
assert(type(cai.tool_schema) == "function")
assert(type(cai.load_dotenv_api_key) == "function")
assert_eq(cai.CONTINUITY_SERVER, 0, "server continuity")
assert(type(cai.AGENT_EVENT_TEXT_DELTA) == "number", "agent text event")
assert(type(cai.AGENT_EVENT_TERMINAL_COMMAND_STARTED) == "number",
  "agent terminal start event")
assert(type(cai.AGENT_EVENT_TERMINAL_OUTPUT) == "number", "agent terminal output event")
assert(type(cai.AGENT_EVENT_TERMINAL_WAITING) == "number", "agent terminal wait event")
assert(type(cai.AGENT_EVENT_TERMINAL_COMMAND_COMPLETED) == "number",
  "agent terminal completion event")
assert(type(cai.AGENT_EVENT_TERMINAL_COMMAND_CANCELLED) == "number",
  "agent terminal cancellation event")
assert(type(cai.AGENT_EVENT_TURN_QUEUED) == "number", "agent queued turn event")
assert(type(cai.AGENT_EVENT_REVIEW_REPORT) == "number", "agent review report event")
assert(type(cai.AGENT_EVENT_REVIEW_STARTED) == "number", "agent review started event")
assert(type(cai.AGENT_EVENT_REVIEW_HANDED_OFF) == "number", "agent review handoff event")
assert(type(cai.AGENT_EVENT_REASONING_SUMMARY) == "number", "agent reasoning summary event")
assert(type(cai.AGENT_EVENT_RESPONSE_COMPLETED) == "number", "agent response completed event")
assert(type(cai.AGENT_TOOL_ACTION_READ) == "number", "agent read action")
assert(type(cai.AGENT_TOOL_ACTION_PATCH) == "number", "agent patch action")
assert_eq(cai.DEFAULT_DOTENV_PATH, ".env", "default dotenv path")
assert_eq(cai.CHATGPT_AUTH_DEFAULT_ISSUER, "https://auth.openai.com",
  "ChatGPT auth issuer")
assert_eq(cai.CHATGPT_AUTH_DEFAULT_CALLBACK_PATH, "/auth/callback",
  "ChatGPT callback path")
assert_eq(cai.CHATGPT_AUTH_DEFAULT_CALLBACK_PORT, 1455,
  "ChatGPT callback port")
assert_eq(cai.CHATGPT_AUTH_FALLBACK_CALLBACK_PORT, 1457,
  "ChatGPT fallback callback port")
assert_eq(cai.OPENAI_API_KEY_ENV, "OPENAI_API_KEY", "OpenAI env name")
assert_eq(cai.OPENROUTER_API_KEY_ENV, "OPENROUTER_API_KEY", "OpenRouter env name")
assert_eq(cai.TEXT_VERBOSITY_LOW, "low", "text verbosity low")

local mcp_client_meta = debug.getregistry()["cai.mcp_client"]
assert(type(mcp_client_meta) == "table")
for _, method in ipairs({
  "initialize",
  "ping",
  "refresh_tools",
  "tool_count",
  "tool_at",
  "tools",
  "call_tool",
  "refresh_resources",
  "resource_count",
  "resource_at",
  "resources",
  "read_resource",
  "refresh_resource_templates",
  "resource_template_count",
  "resource_template_at",
  "resource_templates",
  "refresh_prompts",
  "prompt_count",
  "prompt_at",
  "prompts",
  "get_prompt",
  "complete",
  "send_request",
  "send_notification",
  "register_tools",
  "close",
}) do
  assert(type(mcp_client_meta.__index[method]) == "function",
    "missing mcp client method " .. method)
end

assert_not_ok(cai.mcp_client({}))
assert_not_ok(cai.mcp_client({
  url = "http://example.test/mcp",
  auth_mode = cai.MCP_CLIENT_AUTH_API_KEY,
  api_key = "secret",
}))
local inert_mcp_client = assert_ok(cai.mcp_client({
  url = "http://127.0.0.1:1/mcp",
  client_name = "cai-lua-test",
}))
assert_eq(inert_mcp_client:tool_count(), 0, "empty mcp client tool cache")
assert(inert_mcp_client:tool_at(1) == nil)
assert_eq(#inert_mcp_client:tools(), 0, "empty mcp client tools")
local function assert_mcp_rejects_source(label, fn)
  local value, failure = fn()
  local err = assert_not_ok(value, failure, label)
  assert(tostring(err.message or ""):find("buffered", 1, true),
    label .. " returned wrong error")
end
assert_mcp_rejects_source("MCP call_tool callback params", function()
  return inert_mcp_client:call_tool("echo", function()
    return "{}"
  end)
end)
assert_mcp_rejects_source("MCP call_tool reader params", function()
  return inert_mcp_client:call_tool("echo", spool_text("{}", 1))
end)
assert_mcp_rejects_source("MCP get_prompt callback arguments", function()
  return inert_mcp_client:get_prompt("demo", function()
    return "{}"
  end)
end)
assert_mcp_rejects_source("MCP complete callback context", function()
  return inert_mcp_client:complete("prompt", "demo", "name", "value", function()
    return "{}"
  end)
end)
assert_mcp_rejects_source("MCP send_request callback params", function()
  return inert_mcp_client:send_request("ping", function()
    return "{}"
  end)
end)
assert_mcp_rejects_source("MCP send_notification callback params", function()
  return inert_mcp_client:send_notification("notifications/initialized",
    function()
      return "{}"
    end)
end)
inert_mcp_client:close()
assert_throws(function()
  inert_mcp_client:tool_count()
end, "closed mcp client rejects method calls")
assert_eq(cai.RESPONSE_TRUNCATION_AUTO, "auto", "response truncation auto")
assert_eq(cai.SERVICE_TIER_FLEX, "flex", "service tier flex")
assert_eq(cai.HOSTED_TOOL_WEB_SEARCH, "web_search", "hosted web search")
assert_eq(cai.TOOL_CHOICE_AUTO, "auto", "tool choice auto")
assert_eq(cai.TOOL_CHOICE_NONE, "none", "tool choice none")
assert_eq(cai.TOOL_CHOICE_REQUIRED, "required", "tool choice required")
assert_eq(cai.REASONING_EFFORT_MINIMAL, "minimal", "reasoning effort minimal")
assert_eq(cai.REASONING_EFFORT_MAX, "max", "reasoning effort max")
assert_eq(cai.REASONING_SUMMARY_AUTO, "auto", "reasoning summary auto")
assert_eq(cai.REASONING_SUMMARY_NONE, "none", "reasoning summary none")
assert(type(cai.MODEL_GPT_5_NANO) == "string")
assert_eq(cai.MODEL_DEFAULT_RESPONSES, cai.MODEL_GPT_5_NANO, "default model")

do
  local registry = debug.getregistry()
  local client_methods = assert(registry["cai.client"].__index,
    "Lua client metatable")
  local agent_methods = assert(registry["cai.agent"].__index,
    "Lua agent metatable")
  local session_methods = assert(registry["cai.session"].__index,
    "Lua session metatable")
  local response_methods = assert(registry["cai.response"].__index,
    "Lua response metatable")
  local output_methods = assert(registry["cai.output"].__index,
    "Lua output metatable")
  assert(type(client_methods.set_usage_limits) == "function",
    "Lua client set_usage_limits method missing")
  assert(type(client_methods.usage) == "function",
    "Lua client usage method missing")
  assert(type(client_methods.open_response_text_source) == "function",
    "Lua client open_response_text_source method missing")
  assert(type(client_methods.new_smith_runtime) == "function",
    "Lua client new_smith_runtime method missing")
  assert(type(client_methods.new_agent_runtime) == "function",
    "Lua client new_agent_runtime method missing")
  assert(type(client_methods.new_smith_review_runtime) == "function",
    "Lua client new_smith_review_runtime method missing")
  assert(type(registry["cai.agent_runtime"].__index.submit_review) == "function",
    "Lua agent runtime submit_review method missing")
  assert(type(registry["cai.agent_runtime"].__index.start_review) == "function",
    "Lua agent runtime start_review method missing")
  assert(type(registry["cai.agent_runtime"].__index.finish_review) == "function",
    "Lua agent runtime finish_review method missing")
  assert(type(agent_methods.set_session_usage_limits) == "function",
    "Lua agent set_session_usage_limits method missing")
  assert(type(agent_methods.usage) == "function",
    "Lua agent usage method missing")
  assert(type(agent_methods.run_auto_output) == "function",
    "Lua agent run_auto_output method missing")
  assert(type(agent_methods.add_user_file_source) == "function",
    "Lua agent add_user_file_source method missing")
  assert(type(agent_methods.open_text_source) == "function",
    "Lua agent open_text_source method missing")
  assert(type(session_methods.set_usage_limits) == "function",
    "Lua session set_usage_limits method missing")
  assert(type(session_methods.usage) == "function",
    "Lua session usage method missing")
  assert(type(session_methods.close_with_usage) == "function",
    "Lua session close_with_usage method missing")
  assert(type(session_methods.add_user_file_source) == "function",
    "Lua session add_user_file_source method missing")
  assert(type(session_methods.open_text_source) == "function",
    "Lua session open_text_source method missing")
  assert(type(session_methods.compact_experimental) == "function",
    "Lua session compact_experimental method missing")
  for _, name in ipairs({
    "conversation_id",
    "created_at",
    "error_code",
    "error_message",
    "incomplete_reason",
    "input_tokens",
    "input_cached_tokens",
    "output_tokens",
    "output_reasoning_tokens",
    "total_tokens",
  }) do
    assert(type(response_methods[name]) == "function",
      "Lua response method missing: " .. name)
  end
  assert(type(output_methods.write_raw_json) == "function",
    "Lua output write_raw_json method missing")
end
assert_eq(cai.MODEL_GPT_4O, "gpt-4o", "model constant")
assert(type(cai.MODEL_CAP_RESPONSES) == "number")
assert(type(cai.MODEL_META_PROVIDER_OPENROUTER) == "number")
assert(type(cai.OPENROUTER_MODEL_POOLSIDE_LAGUNA_XS_2_FREE) == "string")
assert(type(cai.OPENROUTER_MODEL_POOLSIDE_LAGUNA_M_1_FREE) == "string")
assert(type(cai.OPENROUTER_MODEL_POOLSIDE_LAGUNA_S_2_1_FREE) == "string")
assert_eq(cai.OPENROUTER_MODEL_OPENAI_GPT_5_6_LUNA,
  "openai/gpt-5.6-luna", "OpenRouter GPT-5.6 Luna constant")
assert_eq(cai.OPENROUTER_MODEL_DEFAULT_RESPONSES,
  cai.OPENROUTER_MODEL_OPENAI_GPT_5_6_LUNA,
  "OpenRouter default model")
assert_eq(cai.MODEL_GPT_5_4_PRO, "gpt-5.4-pro", "GPT-5.4 pro constant")
assert_eq(cai.MODEL_GPT_5_6_SOL, "gpt-5.6-sol", "GPT-5.6 Sol constant")
assert_eq(cai.MODEL_GPT_5_6_LUNA, "gpt-5.6-luna", "GPT-5.6 Luna constant")
assert_eq(cai.MODEL_GPT_5_3_CODEX, "gpt-5.3-codex",
  "GPT-5.3-Codex constant")
assert_eq(cai.MODEL_CHAT_LATEST, "chat-latest", "Chat latest constant")
local model = cai.model_info(cai.MODEL_GPT_5_NANO)
assert(type(model) == "table")
assert(model.context_window_tokens > 0)
assert(model.auto_compact_token_limit > 0)
local latest_model = cai.model_info(cai.MODEL_GPT_5_5)
assert(type(latest_model) == "table")
assert_eq(latest_model.input_usd_per_million, 5.0, "gpt-5.5 input price")
assert_eq(latest_model.cached_input_usd_per_million, 0.5,
  "gpt-5.5 cached input price")
assert_eq(latest_model.output_usd_per_million, 30.0, "gpt-5.5 output price")
assert_eq(latest_model.long_context_threshold_tokens, 272000,
  "gpt-5.5 long context threshold")
assert_eq(latest_model.long_input_usd_per_million, 10.0,
  "gpt-5.5 long input price")
local gpt_5_6_luna = cai.model_info(cai.MODEL_GPT_5_6_LUNA)
assert(type(gpt_5_6_luna) == "table")
assert_eq(gpt_5_6_luna.input_usd_per_million, 0.2,
  "gpt-5.6 Luna input price")
assert_eq(gpt_5_6_luna.long_output_usd_per_million, 1.8,
  "gpt-5.6 Luna long output price")
assert((gpt_5_6_luna.capabilities & cai.MODEL_CAP_REASONING_PRO_MODE) ~= 0)
local o1_mini = cai.model_info(cai.MODEL_O1_MINI)
assert(type(o1_mini) == "table")
assert((o1_mini.capabilities & cai.MODEL_CAP_REASONING) ~= 0)
assert((o1_mini.capabilities & cai.MODEL_CAP_STREAMING) ~= 0)
assert((o1_mini.capabilities & cai.MODEL_CAP_FUNCTION_CALLING) == 0)
assert((o1_mini.capabilities & cai.MODEL_CAP_STRUCTURED_OUTPUTS) == 0)
assert_eq(o1_mini.input_usd_per_million, 1.1, "o1-mini input price")
assert_eq(o1_mini.cached_input_usd_per_million, 0.55,
  "o1-mini cached input price")
assert_eq(o1_mini.output_usd_per_million, 4.4, "o1-mini output price")
local o1_mini_snapshot = cai.model_info(cai.MODEL_O1_MINI_2024_09_12)
assert(type(o1_mini_snapshot) == "table")
assert((o1_mini_snapshot.capabilities & cai.MODEL_CAP_REASONING) ~= 0)
assert((o1_mini_snapshot.metadata_flags & cai.MODEL_META_DEPRECATED) ~= 0)
assert_eq(cai.model_can_estimate_usage_usd(cai.MODEL_GPT_5_NANO), true,
  "priced model can enforce spend")
assert_eq(cai.model_can_estimate_usage_usd(cai.MODEL_GPT_5_5), true,
  "latest model can enforce spend")
assert_eq(cai.model_can_estimate_usage_usd(cai.MODEL_GPT_5_5_PRO), true,
  "latest pro model can enforce spend")
assert_eq(cai.model_can_estimate_usage_usd(cai.MODEL_GPT_5_4_NANO), true,
  "priced GPT-5.4 nano can enforce spend")
assert_eq(cai.model_can_estimate_usage_usd(cai.MODEL_GPT_5_PRO), true,
  "GPT-5 pro can enforce spend")
assert_eq(cai.model_can_estimate_usage_usd(cai.MODEL_CODEX_MINI_LATEST), false,
  "incomplete model cannot enforce spend")
assert_eq(cai.model_can_estimate_usage_usd(cai.MODEL_GPT_4_TURBO), false,
  "supported model without price metadata cannot enforce spend")
assert_eq(cai.model_can_estimate_usage_usd("future-model"), false,
  "unknown model cannot enforce spend")
assert_eq(cai.model_can_estimate_usage_usd(
  cai.OPENROUTER_MODEL_POOLSIDE_LAGUNA_S_2_1_FREE), true,
  "verified free OpenRouter model can enforce spend")

assert_not_ok(cai.open({ api_key = "test-key", logger = {} }))
assert_not_ok(cai.chatgpt_auth({ logger = {} }))
assert_not_ok(cai.chatgpt_login({ logger = {} }))
assert_not_ok(cai.mcp_client({ url = "http://127.0.0.1:1/mcp", logger = {} }))

local logger_chunks = {}
local base_logger = pslog.new_json({
  output = function(chunk)
    logger_chunks[#logger_chunks + 1] = chunk
  end,
  no_color = true,
  disable_timestamp = true,
})
local derived_logger = base_logger:with("component", "lua-cai-test")
local logged_client = assert_ok(cai.open({
  api_key = "test-key",
  logger = derived_logger,
  timeout_ms = 1,
}))
logged_client:close()
local client_log_text = table.concat(logger_chunks)
assert(client_log_text:find("cai.client.opened", 1, true))
assert(client_log_text:find("lua-cai-test", 1, true))
logger_chunks = {}
local logged_mcp_client = assert_ok(cai.mcp_client({
  url = "http://127.0.0.1:1/mcp",
  logger = derived_logger,
  timeout_ms = 1,
}))
logged_mcp_client:close()
local mcp_log_text = table.concat(logger_chunks)
assert(mcp_log_text:find("cai.mcp.client.opened", 1, true))
assert(mcp_log_text:find("lua-cai-test", 1, true))
assert_throws(function()
  cai.open({
    api_key = "test-key",
    logger = derived_logger,
    usage_limits = "bad",
  })
end, "client logger with malformed usage limits")
assert_throws(function()
  cai.open({
    api_key = "test-key",
    logger = derived_logger,
    chatgpt_auth_http_timeout_ms = {},
  })
end, "client logger with malformed ChatGPT auth field")
assert_throws(function()
  cai.mcp_client({
    url = "http://127.0.0.1:1/mcp",
    logger = derived_logger,
    allocator = {},
  })
end, "mcp logger with unsupported allocator")
do
  local co_client
  local co = coroutine.create(function()
    return cai.open({
      api_key = "test-key",
      base_url = "http://127.0.0.1:1/v1",
      logger = derived_logger,
      timeout_ms = 1,
    })
  end)
  local resumed, value_or_err, err = coroutine.resume(co)
  assert(resumed, "logger coroutine client creation coroutine failed")
  co_client = assert_ok(value_or_err, err, "logger coroutine client creation")
  co = nil
  collectgarbage()
  collectgarbage()
  co_client:close()
end
derived_logger:close()

local reentrant_client
local reentrant_close_seen = false
local reentrant_logger = pslog.new_json({
  output = function()
    local ok
    local err
    if reentrant_client ~= nil and not reentrant_close_seen then
      ok, err = reentrant_client:close()
      assert_not_ok(ok, err, "active client close from logger")
      assert(tostring(err.message or ""):find("active operation", 1, true),
        "active client close returned wrong error")
      reentrant_close_seen = true
    end
  end,
  no_color = true,
  disable_timestamp = true,
})
reentrant_client = assert_ok(cai.open({
  api_key = "test-key",
  base_url = "http://127.0.0.1:1/v1",
  logger = reentrant_logger,
  timeout_ms = 1,
}))
assert_not_ok(reentrant_client:retrieve_response("resp_lua_reentrant_close"),
  "reentrant logger request must fail normally")
assert(reentrant_close_seen, "logger did not exercise reentrant client close")
reentrant_client:close()
reentrant_logger:close()

local child_close_client
local child_close_armed = false
local child_close_seen = false
local child_close_logger = pslog.new_json({
  output = function()
    local ok
    local err
    if child_close_client ~= nil and child_close_armed and not child_close_seen then
      ok, err = child_close_client:close()
      assert_not_ok(ok, err, "active parent client close from agent logger")
      assert(tostring(err.message or ""):find("active operation", 1, true),
        "active parent client close returned wrong error")
      child_close_seen = true
    end
  end,
  no_color = true,
  disable_timestamp = true,
})
child_close_client = assert_ok(cai.open({
  api_key = "test-key",
  base_url = "http://127.0.0.1:1/v1",
  logger = child_close_logger,
  timeout_ms = 1,
}))
local child_close_agent = assert_ok(child_close_client:new_agent({
  model = cai.MODEL_GPT_5_NANO,
  instructions = "offline lua test",
}))
assert_ok(child_close_agent:add_user_text("hello"))
child_close_armed = true
assert_not_ok(child_close_agent:run(),
  "agent logger parent close request must fail normally")
child_close_armed = false
assert(child_close_seen, "agent logger did not exercise parent client close")
child_close_agent:close()
child_close_client:close()
child_close_logger:close()

local active_agent_client
local active_agent_agent
local active_agent_armed = false
local active_agent_close_seen = false
local active_agent_logger = pslog.new_json({
  output = function()
    local ok
    local err
    if active_agent_agent ~= nil and active_agent_armed and
        not active_agent_close_seen then
      ok, err = active_agent_agent:close()
      assert_not_ok(ok, err, "active agent self close from logger")
      assert(tostring(err.message or ""):find("active operation", 1, true),
        "active agent self close returned wrong error")
      active_agent_close_seen = true
    end
  end,
  no_color = true,
  disable_timestamp = true,
})
active_agent_client = assert_ok(cai.open({
  api_key = "test-key",
  base_url = "http://127.0.0.1:1/v1",
  logger = active_agent_logger,
  timeout_ms = 1,
}))
active_agent_agent = assert_ok(active_agent_client:new_agent({
  model = cai.MODEL_GPT_5_NANO,
  instructions = "offline lua test",
}))
assert_ok(active_agent_agent:add_user_text("hello"))
active_agent_armed = true
assert_not_ok(active_agent_agent:run(),
  "agent logger self close request must fail normally")
active_agent_armed = false
assert(active_agent_close_seen, "agent logger did not exercise self close")
active_agent_agent:close()
active_agent_client:close()
active_agent_logger:close()

local session_close_client
local session_close_armed = false
local session_close_seen = false
local session_close_logger = pslog.new_json({
  output = function()
    local ok
    local err
    if session_close_client ~= nil and session_close_armed and
        not session_close_seen then
      ok, err = session_close_client:close()
      assert_not_ok(ok, err, "active parent client close from session logger")
      assert(tostring(err.message or ""):find("active operation", 1, true),
        "active session parent client close returned wrong error")
      session_close_seen = true
    end
  end,
  no_color = true,
  disable_timestamp = true,
})
session_close_client = assert_ok(cai.open({
  api_key = "test-key",
  base_url = "http://127.0.0.1:1/v1",
  logger = session_close_logger,
  timeout_ms = 1,
}))
local session_close_agent = assert_ok(session_close_client:new_agent({
  model = cai.MODEL_GPT_5_NANO,
  instructions = "offline lua test",
}))
local session_close_session = assert_ok(session_close_agent:new_session())
assert_ok(session_close_session:add_user_text("hello"))
session_close_armed = true
assert_not_ok(session_close_session:run(),
  "session logger parent close request must fail normally")
session_close_armed = false
assert(session_close_seen, "session logger did not exercise parent client close")
session_close_session:close()
session_close_agent:close()
session_close_client:close()
session_close_logger:close()

local active_session_client
local active_session_agent
local active_session_session
local active_session_armed = false
local active_session_close_seen = false
local active_session_logger = pslog.new_json({
  output = function()
    local ok
    local err
    if active_session_session ~= nil and active_session_armed and
        not active_session_close_seen then
      ok, err = active_session_session:close()
      assert_not_ok(ok, err, "active session self close from logger")
      assert(tostring(err.message or ""):find("active operation", 1, true),
        "active session self close returned wrong error")
      active_session_close_seen = true
    end
  end,
  no_color = true,
  disable_timestamp = true,
})
active_session_client = assert_ok(cai.open({
  api_key = "test-key",
  base_url = "http://127.0.0.1:1/v1",
  logger = active_session_logger,
  timeout_ms = 1,
}))
active_session_agent = assert_ok(active_session_client:new_agent({
  model = cai.MODEL_GPT_5_NANO,
  instructions = "offline lua test",
}))
active_session_session = assert_ok(active_session_agent:new_session())
assert_ok(active_session_session:add_user_text("hello"))
active_session_armed = true
assert_not_ok(active_session_session:run(),
  "session logger self close request must fail normally")
active_session_armed = false
assert(active_session_close_seen, "session logger did not exercise self close")
active_session_session:close()
active_session_agent:close()
active_session_client:close()
active_session_logger:close()

do
  local lifetime_client = assert_ok(cai.open({
    api_key = "test-key",
    timeout_ms = 1,
  }))
  local lifetime_agent = assert_ok(lifetime_client:new_agent({
    model = cai.MODEL_GPT_5_NANO,
    instructions = "offline lua test",
  }))
  local lifetime_session = assert_ok(lifetime_agent:new_session())
  assert_not_ok(lifetime_agent:close(),
    "agent close with a live session must fail")
  lifetime_client = nil
  lifetime_agent = nil
  collectgarbage()
  collectgarbage()
  assert_ok(lifetime_session:add_user_text("still alive"),
    nil, "session must retain parent client after agent close")
  lifetime_session:close()
end

do
  local close_usage_client = assert_ok(cai.open({
    api_key = "test-key",
    timeout_ms = 1,
  }))
  local close_usage_agent = assert_ok(close_usage_client:new_agent({
    model = cai.MODEL_GPT_5_NANO,
    instructions = "offline lua test",
  }))
  local close_usage_session = assert_ok(close_usage_agent:new_session())
  assert_ok(close_usage_session:close_with_usage(),
    nil, "session close_with_usage")
  local _, close_err = close_usage_agent:close()
  assert(close_err == nil,
    "close_with_usage must release parent session bookkeeping")
  close_usage_client:close()
end

local registration_registry
local registration_registry_client
local registration_registry_armed = false
local registration_registry_close_seen = false
local registration_registry_logger = pslog.new_json({
  output = function()
    local ok
    local err
    if registration_registry ~= nil and registration_registry_armed and
        not registration_registry_close_seen then
      ok, err = registration_registry:close()
      assert_not_ok(ok, err, "active registry close from registration logger")
      assert(tostring(err.message or ""):find("active operation", 1, true),
        "active registry close returned wrong error")
      registration_registry_close_seen = true
    end
  end,
  no_color = true,
  disable_timestamp = true,
})
registration_registry = assert_ok(cai.tool_registry())
registration_registry_client = assert_ok(cai.mcp_client({
  url = "http://127.0.0.1:1/mcp",
  logger = registration_registry_logger,
  timeout_ms = 1,
}))
registration_registry_armed = true
assert_not_ok(registration_registry:register_mcp_client_tools(
  registration_registry_client), "registry MCP registration must fail normally")
registration_registry_armed = false
assert(registration_registry_close_seen,
  "registry registration logger did not exercise close")
registration_registry:close()
registration_registry_client:close()
registration_registry_logger:close()

do
  local failed_registry = assert_ok(cai.tool_registry())
  local failed_mcp = assert_ok(cai.mcp_client({
    url = "http://127.0.0.1:1/mcp",
    timeout_ms = 1,
  }))
  assert_not_ok(failed_registry:register_mcp_client_tools(failed_mcp),
    "failed registry MCP registration")
  assert_ok(failed_mcp:close(),
    nil, "failed registry MCP registration must release MCP client")
  failed_registry:close()
end

local client_registry
local client_registry_client
local client_registry_armed = false
local client_registry_close_seen = false
local client_registry_logger = pslog.new_json({
  output = function()
    local ok
    local err
    if client_registry ~= nil and client_registry_armed and
        not client_registry_close_seen then
      ok, err = client_registry:close()
      assert_not_ok(ok, err, "active registry close from client registration")
      assert(tostring(err.message or ""):find("active operation", 1, true),
        "active client registration registry close returned wrong error")
      client_registry_close_seen = true
    end
  end,
  no_color = true,
  disable_timestamp = true,
})
client_registry = assert_ok(cai.tool_registry())
client_registry_client = assert_ok(cai.mcp_client({
  url = "http://127.0.0.1:1/mcp",
  logger = client_registry_logger,
  timeout_ms = 1,
}))
client_registry_armed = true
assert_not_ok(client_registry_client:register_tools(client_registry),
  "client registry registration must fail normally")
client_registry_armed = false
assert(client_registry_close_seen,
  "client registry logger did not exercise close")
client_registry:close()
client_registry_client:close()
client_registry_logger:close()

do
  local failed_registry = assert_ok(cai.tool_registry())
  local failed_mcp = assert_ok(cai.mcp_client({
    url = "http://127.0.0.1:1/mcp",
    timeout_ms = 1,
  }))
  assert_not_ok(failed_mcp:register_tools(failed_registry),
    "failed client registry MCP registration")
  assert_ok(failed_mcp:close(),
    nil, "failed client registry MCP registration must release MCP client")
  failed_registry:close()
end

local registration_agent_client
local registration_agent
local registration_agent_mcp
local registration_agent_armed = false
local registration_agent_close_seen = false
local registration_agent_logger = pslog.new_json({
  output = function()
    local ok
    local err
    if registration_agent ~= nil and registration_agent_armed and
        not registration_agent_close_seen then
      ok, err = registration_agent:close()
      assert_not_ok(ok, err, "active agent close from registration logger")
      assert(tostring(err.message or ""):find("active operation", 1, true),
        "active registration agent close returned wrong error")
      registration_agent_close_seen = true
    end
  end,
  no_color = true,
  disable_timestamp = true,
})
registration_agent_client = assert_ok(cai.open({
  api_key = "test-key",
  base_url = "http://127.0.0.1:1/v1",
  timeout_ms = 1,
}))
registration_agent = assert_ok(registration_agent_client:new_agent({
  model = cai.MODEL_GPT_5_NANO,
  instructions = "offline lua test",
}))
registration_agent_mcp = assert_ok(cai.mcp_client({
  url = "http://127.0.0.1:1/mcp",
  logger = registration_agent_logger,
  timeout_ms = 1,
}))
registration_agent_armed = true
assert_not_ok(registration_agent:register_mcp_client_tools(registration_agent_mcp),
  "agent MCP registration must fail normally")
registration_agent_armed = false
assert(registration_agent_close_seen,
  "agent registration logger did not exercise close")
registration_agent:close()
registration_agent_client:close()
registration_agent_mcp:close()
registration_agent_logger:close()

do
  local failed_client = assert_ok(cai.open({
    api_key = "test-key",
    base_url = "http://127.0.0.1:1/v1",
    timeout_ms = 1,
  }))
  local failed_agent = assert_ok(failed_client:new_agent({
    model = cai.MODEL_GPT_5_NANO,
    instructions = "offline lua test",
  }))
  local failed_mcp = assert_ok(cai.mcp_client({
    url = "http://127.0.0.1:1/mcp",
    timeout_ms = 1,
  }))
  assert_not_ok(failed_agent:register_mcp_client_tools(failed_mcp),
    "failed agent MCP registration")
  assert_ok(failed_mcp:close(),
    nil, "failed agent MCP registration must release MCP client")
  failed_agent:close()
  failed_client:close()
end

do
  local failed_client = assert_ok(cai.open({
    api_key = "test-key",
    base_url = "http://127.0.0.1:1/v1",
    timeout_ms = 1,
  }))
  local failed_agent = assert_ok(failed_client:new_agent({
    model = cai.MODEL_GPT_5_NANO,
    instructions = "offline lua test",
  }))
  local failed_mcp = assert_ok(cai.mcp_client({
    url = "http://127.0.0.1:1/mcp",
    timeout_ms = 1,
  }))
  assert_not_ok(failed_mcp:register_tools(failed_agent),
    "failed client agent MCP registration")
  assert_ok(failed_mcp:close(),
    nil, "failed client agent MCP registration must release MCP client")
  failed_agent:close()
  failed_client:close()
end

do
  local review_client = assert_ok(cai.open({
    api_key = "test-key",
    base_url = "http://127.0.0.1:1/v1",
    timeout_ms = 1,
  }))
  local parent = assert_ok(review_client:new_smith_runtime({
    workspace_directory = ".",
    review_model = cai.MODEL_GPT_5_6_LUNA,
    review_reasoning_effort = cai.REASONING_EFFORT_MEDIUM,
    disable_default_session_store = true,
  }))
  local review = assert_ok(parent:start_review({ target = "uncommitted" }))
  assert_not_ok(parent:submit("must wait for review"),
    "parent direct input must be paused during review")
  local review_state = "sampling"
  for _ = 1, 20 do
    review_state = assert_ok(review:pump(50))
    if review_state == "failed" or review_state == "cancelled" or review_state == "completed" then
      break
    end
  end
  assert(review_state == "failed" or review_state == "cancelled",
    "offline review child must reach a terminal failure state")
  assert_ok(parent:finish_review(review))
  local second_review = assert_ok(parent:start_review({ target = "uncommitted" }))
  review:close()
  assert_not_ok(parent:close(),
    "closing an earlier review must retain a later review callback receiver")
  review_state = "sampling"
  for _ = 1, 20 do
    review_state = assert_ok(second_review:pump(50))
    if review_state == "failed" or review_state == "cancelled" or review_state == "completed" then
      break
    end
  end
  assert(review_state == "failed" or review_state == "cancelled",
    "second offline review child must reach a terminal failure state")
  assert_ok(parent:finish_review(second_review))
  second_review:close()
  parent:close()
  review_client:close()
end

do
  local review_client = assert_ok(cai.open({
    api_key = "test-key",
    base_url = "http://127.0.0.1:1/v1",
    timeout_ms = 1,
  }))
  local parent = assert_ok(review_client:new_smith_runtime({
    workspace_directory = ".",
    disable_default_session_store = true,
  }))
  local review = assert_ok(parent:start_review({ target = "uncommitted" }))
  review:close()
  assert_throws(function()
    parent:submit("abandoned reviews cannot resume the parent")
  end, "abandoned review parent must reject further operations")
  parent:close()
  review_client:close()
end

local dummy_client = assert_ok(cai.open({ api_key = "test-key", timeout_ms = 1 }))
do
  local runtime_meta = debug.getregistry()["cai.agent_runtime"]
  assert(type(runtime_meta) == "table", "missing agent runtime metatable")
  for _, method in ipairs({ "submit", "submit_review", "start_review", "finish_review", "submit_steering", "submit_queued", "goal", "create_goal", "pause_goal", "resume_goal", "set_goal_objective", "set_goal_token_budget", "clear_goal_token_budget", "clear_goal", "pump", "state",
    "session_id", "export_markdown", "export_markdown_file", "wakeup_fd", "close" }) do
    assert(type(runtime_meta.__index[method]) == "function",
      "missing agent runtime method " .. method)
  end
  assert_throws(function()
    dummy_client:new_agent_runtime({
      workspace_directory = ".",
      event_callback = function() end,
    })
  end, "Lua custom runtimes must require a preset table before retaining callbacks")
  do
    local weak = setmetatable({}, { __mode = "v" })

    do
      local sentinel = {}
      weak.callback = sentinel
      assert_throws(function()
        dummy_client:new_smith_runtime({
          workspace_directory = ".",
          disable_default_session_store = true,
          event_callback = function()
            return sentinel
          end,
          subagents = { "not a profile" },
        })
      end, "Lua invalid subagent configuration must fail")
    end
    collectgarbage("collect")
    collectgarbage("collect")
    assert(weak.callback == nil,
      "rejected Lua runtime must release callback registry references")
  end
  native_store_test.reset_sessions()
  local prepare_backend = native_store_test.new_subagent_prepare_backend()
  local custom_runtime = assert_ok(dummy_client:new_agent_runtime({
    workspace_directory = ".",
    disable_default_session_store = true,
    preset = {
      name = "vectis-engineer",
      prompt_version = "vectis-engineer-1",
      default_identity = "Vectis Engineer",
      default_model = cai.MODEL_GPT_5_6_LUNA,
      default_reasoning_effort = cai.REASONING_EFFORT_LOW,
      default_reasoning_summary = cai.REASONING_SUMMARY_CONCISE,
      developer_instructions = "You are {{agent_identity}}, Vectis' coding agent.",
      tool_capabilities = cai.AGENT_PRESET_TOOL_READ_FILE + cai.AGENT_PRESET_TOOL_SUBAGENTS,
    },
    review_allowed_models = { cai.MODEL_GPT_5_6_LUNA },
    review_allowed_reasoning_efforts = { cai.REASONING_EFFORT_LOW },
    review_allowed_reasoning_summaries = { cai.REASONING_SUMMARY_CONCISE },
    subagents = {
      {
        name = "summarizer",
        description = "Return a compact implementation handover.",
        allowed_models = { cai.MODEL_GPT_5_6_LUNA },
        allowed_reasoning_efforts = { cai.REASONING_EFFORT_LOW },
        allowed_reasoning_summaries = { cai.REASONING_SUMMARY_CONCISE },
        parameters = {
          {
            name = "format",
            description = "Required output format.",
            type = "enum",
            required = true,
            enum_values = { "json", "markdown" },
          },
          {
            name = "max_items",
            description = "Optional output limit.",
            type = "integer",
          },
        },
        instruction_template = "Return {{format}} for {{instructions}}.",
        expose_instructions = true,
        prepare_backend = prepare_backend,
        preset = {
          name = "vectis-summarizer",
          prompt_version = "vectis-summarizer-1",
          default_identity = "Vectis Summarizer",
          default_model = cai.MODEL_GPT_5_6_LUNA,
          default_reasoning_effort = cai.REASONING_EFFORT_LOW,
          default_reasoning_summary = cai.REASONING_SUMMARY_CONCISE,
          developer_instructions = "You are {{agent_identity}}.",
          tool_capabilities = cai.AGENT_PRESET_TOOL_READ_FILE,
        },
      },
    },
  }))
  assert_eq(custom_runtime:state(), "idle", "Lua custom preset initial state")
  local custom_chunks = {}
  assert_ok(custom_runtime:export_markdown(function(chunk)
    custom_chunks[#custom_chunks + 1] = chunk
    return true
  end), nil, "Lua custom preset export")
  assert(table.concat(custom_chunks):find("vectis-engineer", 1, true),
    "Lua custom preset export metadata")
  do
    local ok, err = prepare_backend:close()
    assert_not_ok(ok, err, "Lua prepare backend must remain retained by runtime")
  end
  custom_runtime:close()
  assert(pcall(function()
    prepare_backend:close()
  end), "Lua prepare backend closes after runtime")
  do
    local poll_client = assert_ok(cai.open({
      api_key = "test-key",
      base_url = "http://127.0.0.1:1/v1",
      timeout_ms = 1,
    }))
    local poll_runtime = assert_ok(poll_client:new_smith_runtime({
      workspace_directory = ".",
      disable_default_session_store = true,
      event_queue_limit = 1,
    }))
    assert_ok(poll_runtime:submit("Lua poll-only initial turn"), nil,
      "Lua poll-only runtime accepts an initial turn")
    assert_ok(poll_runtime:submit_queued("Lua poll-only queued turn"), nil,
      "Lua poll-only runtime does not queue undrainable events")
    poll_runtime:close()
    poll_client:close()
  end
  do
    local callback_client = assert_ok(cai.open({
      api_key = "test-key",
      base_url = "http://127.0.0.1:1/v1",
      timeout_ms = 1,
    }))
    local callback_runtime
    local callback_calls = 0

    callback_runtime = assert_ok(callback_client:new_smith_runtime({
      workspace_directory = ".",
      disable_default_session_store = true,
      event_callback = function()
        callback_calls = callback_calls + 1
        callback_runtime:close()
      end,
    }))
    assert_ok(callback_runtime:submit("close from Lua event callback"), nil,
      "Lua callback-close runtime accepts its turn")
    assert_eq(callback_runtime:pump(100), "cancelled",
      "Lua callback-close pump reports cancellation")
    assert_eq(callback_calls, 1, "Lua callback-close called exactly once")
    assert_throws(function()
      callback_runtime:state()
    end, "Lua callback-close runtime is closed after pump")
    callback_client:close()
  end
  do
    local goal_runtime = assert_ok(dummy_client:new_smith_runtime({
      workspace_directory = ".",
      disable_default_session_store = true,
    }))
    assert_ok(goal_runtime:create_goal({ objective = "Exercise host goals", token_budget = 12 }))
    local goal
    for _ = 1, 10 do
      goal_runtime:pump(10)
      goal = goal_runtime:goal()
      if goal.has_goal then break end
    end
    assert(goal and goal.status == "active", "Lua host goal creation")
    assert(goal.remaining_tokens == 12, "Lua host goal remaining budget")
    assert_ok(goal_runtime:pause_goal())
    for _ = 1, 10 do
      goal_runtime:pump(10)
      goal = goal_runtime:goal()
      if goal.status == "paused" then break end
    end
    assert(goal.status == "paused", "Lua host goal pause")
    assert_ok(goal_runtime:resume_goal())
    assert_ok(goal_runtime:set_goal_objective("Retargeted host goal"))
    assert_ok(goal_runtime:clear_goal_token_budget())
    for _ = 1, 10 do goal_runtime:pump(10) end
    goal = goal_runtime:goal()
    assert(goal.status == "active" and goal.objective == "Retargeted host goal",
      "Lua host goal resume and retarget")
    assert(not goal.has_token_budget, "Lua host goal budget removal")
    assert_ok(goal_runtime:clear_goal())
    for _ = 1, 10 do
      goal_runtime:pump(10)
      if not goal_runtime:goal().has_goal then break end
    end
    assert(not goal_runtime:goal().has_goal, "Lua host goal clear")
    goal_runtime:close()
  end
  do
    local rejected_mcp = assert_ok(cai.mcp_client({
      url = "http://127.0.0.1:1/mcp",
      timeout_ms = 1,
    }))
    local rejected_runtime, rejected_error = dummy_client:new_agent_runtime({
      workspace_directory = ".",
      disable_default_session_store = true,
      preset = {
        name = "read-only-engineer",
        prompt_version = "read-only-engineer-1",
        default_identity = "Read-only Engineer",
        default_model = cai.MODEL_GPT_5_6_LUNA,
        default_reasoning_effort = cai.REASONING_EFFORT_LOW,
        developer_instructions = "You are {{agent_identity}}.",
        tool_capabilities = cai.AGENT_PRESET_TOOL_READ_FILE,
      },
      mcp_clients = { rejected_mcp },
    })
    local rejection = assert_not_ok(rejected_runtime, rejected_error,
      "custom preset without MCP capability")
    assert(tostring(rejection.message or ""):find("does not enable MCP", 1,
      true), "custom MCP capability rejection")
    assert_ok(rejected_mcp:close(), nil,
      "rejected runtime must release MCP client")
  end
  local native_session_store = assert(native_store_test.new_agent_session())
  local wrong_session_store = assert(native_store_test.new_mcp_session())
  local runtime_logger_chunks = {}
  local runtime_logger = pslog.new_json({
    output = function(chunk)
      runtime_logger_chunks[#runtime_logger_chunks + 1] = chunk
    end,
    no_color = true,
    disable_timestamp = true,
  })
  assert_not_ok(cai.native_store("agent_session", nil),
    "native agent session stores must require a C callback table")
  assert_throws(function()
    dummy_client:new_smith_runtime({
      workspace_directory = ".",
      session_store = wrong_session_store,
      disable_terminal = true,
    })
  end, "Smith runtime must reject an MCP session backend")
  wrong_session_store:close()
  local runtime = assert_ok(dummy_client:new_smith_runtime({
    workspace_directory = ".",
    session_store = native_session_store,
    disable_terminal = true,
    logger = runtime_logger,
    event_callback = function() end,
  }))
  assert(table.concat(runtime_logger_chunks):find("cai.agent.runtime.opened", 1, true),
    "Lua Smith runtime accepts and retains a native pslog logger")
  assert_eq(runtime:state(), "idle", "Lua Smith runtime initial state")
  assert(type(runtime:session_id()) == "string", "Lua Smith runtime session id")
  assert(type(runtime:wakeup_fd()) == "number", "Lua Smith runtime wakeup fd")
  do
    local chunks = {}
    assert_ok(runtime:export_markdown(function(chunk)
      chunks[#chunks + 1] = chunk
      return true
    end), nil, "Lua Smith runtime streaming export")
    local handover = table.concat(chunks)
    assert(handover:find("# CAI agent handover", 1, true),
      "Lua Smith runtime handover title")
    assert(handover:find("cai-agent-handover/1", 1, true),
      "Lua Smith runtime handover format")
    assert(handover:find("non-resumable handover document", 1, true),
      "Lua Smith runtime handover contract")
    assert(handover:find("## Runtime", 1, true),
      "Lua Smith runtime handover runtime metadata")
    assert(handover:find("## Active developer instructions", 1, true),
      "Lua Smith runtime handover developer instructions")
    local export_path = os.tmpname()
    os.remove(export_path)
    assert_eq(assert_ok(runtime:export_markdown_file("cai", export_path), nil,
      "Lua Smith runtime explicit export"), export_path,
      "Lua Smith runtime export path")
    local fp = assert(io.open(export_path, "rb"))
    assert_eq(fp:read("*a"), handover,
      "Lua Smith runtime export file content")
    fp:close()
    os.remove(export_path)
    export_path = os.tmpname()
    os.remove(export_path)
    assert_eq(assert_ok(runtime:export_markdown_file(nil, export_path), nil,
      "Lua Smith runtime explicit export without app name"), export_path,
      "Lua Smith runtime export path without app name")
    os.remove(export_path)
  end
  assert(native_store_test.checkpoint_count() > 0,
    "native agent session store must receive runtime checkpoints")
  assert_not_ok(native_session_store:close(),
    "native session store must remain pinned by a live runtime")
  do
    local value, err = dummy_client:close()
    assert_not_ok(value, err, "client close must reject a live Lua agent runtime")
  end
  runtime:close()
  runtime_logger:close()
  native_session_store:close()
  local review_runtime = assert_ok(dummy_client:new_smith_review_runtime({
    workspace_directory = ".",
    disable_default_session_store = true,
    event_callback = function() end,
  }))
  assert_eq(review_runtime:state(), "idle", "Lua Smith review runtime initial state")
  assert_not_ok(review_runtime:submit_review({ target = "custom" }),
    "Lua Smith review custom target requires instructions")
  assert_ok(review_runtime:submit_review({ target = "base", base_branch = "HEAD~2" }),
    nil, "Lua Smith review target accepts a safe revision expression")
  review_runtime:close()
end
assert_ok(dummy_client:set_usage_limits({ max_total_tokens = 100 }))
assert_not_ok(dummy_client:set_usage_limits({ max_total_tokens = -1 }),
  "negative Lua client usage limit must fail")
do
  local accounting = assert_ok(dummy_client:usage())
  assert_eq(accounting.usage.total_tokens, 0, "Lua client usage total")
  assert_eq(accounting.limit_exceeded, false, "Lua client limit flag")
end
assert_not_ok(dummy_client:new_agent({
  model = cai.MODEL_GPT_5_NANO,
  max_output_tokens = -1,
}), "negative Lua agent max output tokens must fail")
assert_not_ok(dummy_client:new_agent({
  model = cai.MODEL_GPT_5_NANO,
  max_tool_calls = -1,
}), "negative Lua agent max tool calls must fail")
assert_not_ok(dummy_client:new_agent({
  model = cai.MODEL_GPT_4_TURBO,
  session_usage_limits = { max_spend_usd = 1.0 },
}), "Lua agent spend cap with missing pricing must fail")
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
  session_usage_limits = { max_total_tokens = 100 },
}))
assert_ok(dummy_agent:set_session_usage_limits({ max_total_tokens = 120 }))
assert_not_ok(dummy_agent:set_session_usage_limits({ max_total_tokens = -1 }),
  "negative Lua agent session usage limit must fail")
do
  local accounting = assert_ok(dummy_agent:usage())
  assert_eq(accounting.usage.total_tokens, 0, "Lua agent usage total")
end
assert_ok(dummy_agent:add_simple_hosted_tool(cai.HOSTED_TOOL_WEB_SEARCH))
assert_ok(dummy_agent:add_hosted_mcp_tool({
  server_label = "dice",
  server_url = "https://example.test/mcp",
  allowed_tool_names = { "roll" },
}))
assert_not_ok(dummy_agent:add_hosted_mcp_tool({
  server_url = "https://example.test/mcp",
}), "Lua hosted MCP server_label is required")
assert_not_ok(dummy_agent:add_hosted_tool_json("[]"),
  "agent hosted tool JSON must be an object")
assert_not_ok(dummy_agent:add_user_text_spooled({ read = "not callable" }),
  "agent spooled reader with non-callable read must fail")
local dummy_session = assert_ok(dummy_agent:new_session())
do
  local value, err = dummy_session:run_auto({
    max_tool_calls_per_round = -1,
  })
  local rejection = assert_not_ok(value, err,
    "negative Lua per-round tool-call limit must fail")
  assert_eq(rejection.status_string, "invalid",
    "Lua per-round tool-call limit must reach native validation")
end
assert_ok(dummy_session:set_usage_limits({
  max_input_tokens = 10,
  max_input_cached_tokens = 10,
  max_output_tokens = 10,
  max_output_reasoning_tokens = 10,
  max_total_tokens = 100,
  max_spend_usd = 1.0,
}))
assert_not_ok(dummy_session:set_usage_limits({ max_spend_usd = -1 }),
  "negative Lua session spend limit must fail")
local unpriced_agent = assert_ok(dummy_client:new_agent({
  model = cai.MODEL_GPT_4_TURBO,
  instructions = "offline lua unpriced spend test",
}))
local unpriced_session = assert_ok(unpriced_agent:new_session())
assert_not_ok(unpriced_agent:set_session_usage_limits({ max_spend_usd = 1.0 }),
  "Lua agent spend cap setter with missing pricing must fail")
assert_not_ok(unpriced_session:set_usage_limits({ max_spend_usd = 1.0 }),
  "Lua session spend cap setter with missing pricing must fail")
unpriced_session:close()
unpriced_agent:close()
do
  local accounting = assert_ok(dummy_session:usage())
  assert_eq(accounting.usage.total_tokens, 0, "Lua session usage total")
  assert_eq(accounting.limit_exceeded, false, "Lua session limit flag")
end
assert_ok(dummy_session:set_previous_response_id("resp_lua_test"))
assert_ok(dummy_session:set_conversation_id("conv_lua_test"))
assert_not_ok(dummy_session:set_conversation_id(""),
  "empty Lua session conversation id must fail")
assert_ok(dummy_session:add_user_text("hello"))
assert_ok(dummy_session:add_user_text_spooled(spool_text("spooled hello", 3)))
assert_ok(dummy_session:add_user_file_data_spooled(
  "notes.txt", spool_text("spooled file data", 4)))
local file_source_part = 0
assert_ok(dummy_session:add_user_file_source("source.txt", function()
  file_source_part = file_source_part + 1
  return ({ "streamed ", "file", nil })[file_source_part]
end, "auto"))
assert_not_ok(dummy_session:add_user_file_source("bad-source.txt", function()
  error("file source exploded")
end), "Lua file source callback failure must fail")
assert_ok(dummy_agent:add_user_file_source("agent-source.txt", "agent file source"))
assert_not_ok(dummy_agent:add_user_file_source("agent-bad-source.txt", function()
  return {}
end), "Lua agent file source callback non-string result must fail")
local source_part = 0
assert_ok(dummy_session:add_user_text_source(function()
  source_part = source_part + 1
  return ({ "streamed ", "hello", nil })[source_part]
end))
assert_not_ok(dummy_session:add_user_text_source(function()
  error("source exploded")
end), "Lua source callback failure must fail")
assert_not_ok(dummy_session:add_user_text_source(function()
  return {}
end), "Lua source callback non-string result must fail")
assert_not_ok(dummy_session:add_user_text_source(function()
  return ""
end), "Lua source callback unbounded empty chunks must fail")
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

do
  local default_path = assert_ok(cai.chatgpt_auth_default_path(), nil,
    "Lua ChatGPT default auth path")
  assert(default_path:match("/cai/auth%.json$"),
    "Lua ChatGPT default auth path suffix")

  local auth_path = os.tmpname()
  local future_token = "eyJhbGciOiJub25lIn0.eyJleHAiOjQxMDI0NDQ4MDB9.lua"
  local fp = assert(io.open(auth_path, "w"))
  fp:write('{"auth_mode":"chatgpt","tokens":{"id_token":"' .. future_token ..
    '","access_token":"' .. future_token ..
    '","refresh_token":"refresh-lua","account_id":"acct_lua"},' ..
    '"last_refresh":"2026-01-01T00:00:00Z"}')
  fp:close()
	  local standalone_auth = assert_ok(cai.chatgpt_auth({
	    auth_json_path = auth_path,
	    issuer = "http://127.0.0.1:1",
	    refresh_window_seconds = 300,
	    http_timeout_ms = 250,
	    insecure_skip_verify = 1,
	    ca_bundle_path = "/tmp/cai-lua-ca.pem",
	    ca_path = "/tmp/cai-lua-ca-dir",
	    logger = base_logger,
	  }), nil, "Lua standalone ChatGPT auth open")
  assert_eq(assert_ok(standalone_auth:access_token(), nil,
    "Lua standalone ChatGPT access token"), future_token,
    "Lua standalone ChatGPT token")
  standalone_auth:close()
  assert_throws(function()
    standalone_auth:access_token()
  end, "Lua standalone ChatGPT closed auth")
	  local auth_client = assert_ok(cai.open({
	    chatgpt_auth_json = auth_path,
	    base_url = "http://127.0.0.1:1/v1",
    http_2_disabled = 1,
    ca_bundle_path = "/tmp/cai-lua-client-ca.pem",
    ca_path = "/tmp/cai-lua-client-ca-dir",
    timeout_ms = 1,
    chatgpt_auth_http_timeout_ms = 250,
	    chatgpt_auth_insecure_skip_verify = 1,
	    chatgpt_auth_ca_bundle_path = "/tmp/cai-lua-auth-ca.pem",
	    chatgpt_auth_ca_path = "/tmp/cai-lua-auth-ca-dir",
	    logger = base_logger,
	  }), nil, "Lua ChatGPT auth client open")
  auth_client:close()
  os.remove(auth_path)
  local missing_client, missing_err = cai.open({
    chatgpt_auth_json = auth_path,
  })
  assert_not_ok(missing_client, missing_err, "Lua ChatGPT auth missing file")
end

do
  local auth_path = os.tmpname()
  os.remove(auth_path)
	  local login, authorize_url_or_err = cai.chatgpt_login({
	    auth_json_path = auth_path,
	    redirect_uri = "http://localhost:1455/auth/callback",
    issuer = "https://auth.example.test/",
    state = "state-fixed",
    code_verifier = "test-verifier-abcdefghijklmnopqrstuvwxyz-0123456789",
    originator = "cai-lua-test",
    http_timeout_ms = 250,
	    insecure_skip_verify = 1,
	    ca_bundle_path = "/tmp/cai-lua-login-ca.pem",
	    ca_path = "/tmp/cai-lua-login-ca-dir",
	    logger = base_logger,
	  })
  local authorize_url = authorize_url_or_err
  assert_ok(login, authorize_url_or_err, "Lua ChatGPT login start")
  assert(authorize_url:match("^https://auth%.example%.test/oauth/authorize%?"),
    "Lua ChatGPT authorize URL base")
  assert(authorize_url:find("client_id=" .. cai.CHATGPT_AUTH_DEFAULT_CLIENT_ID,
    1, true), "Lua ChatGPT authorize URL client id")
  assert(authorize_url:find("redirect_uri=http%%3A%%2F%%2Flocalhost%%3A1455%%2Fauth%%2Fcallback"),
    "Lua ChatGPT authorize URL redirect")
  assert(authorize_url:find("code_challenge_method=S256", 1, true),
    "Lua ChatGPT authorize URL PKCE")
  assert(authorize_url:find("state=state-fixed", 1, true),
    "Lua ChatGPT authorize URL state")
  assert_eq(login:authorize_url(), authorize_url, "Lua ChatGPT authorize method")
  assert_eq(login:completed(), false, "Lua ChatGPT login starts incomplete")

  local bad_state = assert_ok(login:handle_callback({
    method = "GET",
    target = "/auth/callback?code=mock-code&state=wrong",
  }), nil, "Lua ChatGPT login state mismatch response")
  assert_eq(bad_state.status, 400, "Lua ChatGPT state mismatch status")
  assert_eq(bad_state.completed, true, "Lua ChatGPT state mismatch terminal")
  assert_eq(login:completed(), false, "Lua ChatGPT state mismatch not success")

  local bad_method = assert_ok(login:handle_callback("POST",
    "/auth/callback?code=mock-code&state=state-fixed"), nil,
    "Lua ChatGPT login method response")
  assert_eq(bad_method.status, 405, "Lua ChatGPT method status")
  assert_eq(bad_method.completed, false, "Lua ChatGPT method not terminal")

  local bad_path = assert_ok(login:handle_callback("GET",
    "/other?code=mock-code&state=state-fixed"), nil,
    "Lua ChatGPT login path response")
  assert_eq(bad_path.status, 404, "Lua ChatGPT path status")
  assert_eq(bad_path.completed, false, "Lua ChatGPT path not terminal")

  login:close()
  assert_throws(function()
    login:completed()
  end, "closed Lua ChatGPT login handle must fail")
end

do
  local chained_agent = assert_ok(cai.open({
    api_key = "test-key",
    timeout_ms = 1,
  }):new_agent({
    model = cai.MODEL_GPT_5_NANO,
    instructions = "offline lua chained parent lifetime test",
    session_continuity = cai.CONTINUITY_CLIENT_HISTORY,
  }))
  collectgarbage("collect")
  collectgarbage("collect")
  assert_ok(chained_agent:add_user_text("agent parent must still be alive"))
  chained_agent:close()

  local chained_session = assert_ok(cai.open({
    api_key = "test-key",
    timeout_ms = 1,
  }):new_agent({
    model = cai.MODEL_GPT_5_NANO,
    instructions = "offline lua chained session lifetime test",
    session_continuity = cai.CONTINUITY_CLIENT_HISTORY,
  }):new_session())
  collectgarbage("collect")
  collectgarbage("collect")
  assert_ok(chained_session:add_user_text("session parent must still be alive"))
  chained_session:close()
end
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
assert_ok(params:set_background(true))
assert_ok(params:set_store(false))
assert_ok(params:set_service_tier(cai.SERVICE_TIER_FLEX))
assert_ok(params:set_truncation(cai.RESPONSE_TRUNCATION_AUTO))
assert_ok(params:set_metadata_json('{"tenant":"lua"}'))
assert_ok(params:set_include_json('["reasoning.encrypted_content"]'))
assert_ok(params:set_prompt_json('{"id":"pmpt_lua","variables":{"topic":"cai"}}'))
assert_ok(params:set_tool_choice(cai.TOOL_CHOICE_AUTO))
assert_ok(params:set_tool_choice_json('{"type":"web_search"}'))
assert_not_ok(params:set_tool_choice_json('['),
  "invalid Lua raw tool choice JSON must fail")
assert_ok(params:set_tool_choice(cai.TOOL_CHOICE_AUTO))
assert_ok(params:set_max_output_tokens(128))
assert_ok(params:set_max_tool_calls(3))
assert_not_ok(params:set_max_tool_calls(-1),
  "negative Lua max tool calls must fail")
assert_not_ok(params:set_conversation_id(""),
  "empty Lua params conversation id must fail")
assert_not_ok(params:set_previous_response_id(""),
  "empty Lua params previous response id must fail")
assert_ok(params:set_parallel_tool_calls(true))
assert_ok(params:set_compact_threshold(320000))
assert_ok(params:set_reasoning("minimal", "auto"))
assert_ok(params:set_reasoning_mode(cai.REASONING_MODE_PRO))
assert_not_ok(params:set_reasoning_mode("ultra"),
  "invalid Lua reasoning mode must fail")
assert_ok(params:set_text_format_json_object())
assert_ok(params:set_text_format_json_schema("lua_test", "Lua schema test", '{"type":"object","properties":{"ok":{"type":"boolean"}},"additionalProperties":false}', true))
assert_ok(params:set_text_verbosity(cai.TEXT_VERBOSITY_LOW))
assert_ok(params:add_text("user", "hello"))
assert_ok(params:add_text_spooled("user", spool_text("params spooled text", 5)))
assert_ok(params:add_image_url("user", "https://example.com/image.png", "low"))
assert_ok(params:add_image_file_id("user", "file_lua_image", "low"))
assert_ok(params:add_file_id("user", "file_lua_doc"))
assert_ok(params:add_file_data_spooled(
  "user", "params.txt", spool_text("params file data", 6)))
assert_ok(params:add_file_url("user", "https://example.com/file.txt"))
assert_ok(params:add_function_tool("noop", "No-op test tool", '{"type":"object","properties":{},"additionalProperties":false}', true))
assert_ok(params:add_simple_hosted_tool(cai.HOSTED_TOOL_WEB_SEARCH))
assert_ok(params:add_hosted_tool_json('{"type":"code_interpreter","container":{"type":"auto"}}'))
assert_ok(params:add_hosted_mcp_tool({
  server_label = "dice",
  server_url = "https://example.test/mcp",
  server_description = "Lua dice tools",
  allowed_tool_names = { "roll", "status" },
  require_approval_json = '"never"',
}))
assert_not_ok(params:add_hosted_mcp_tool({
  server_label = "bad",
  server_url = "https://example.test/mcp",
  allowed_tools_json = "[",
}), "invalid Lua hosted MCP policy JSON must fail")
assert_not_ok(params:add_hosted_mcp_tool({
  server_label = "bad",
}), "Lua hosted MCP endpoint is required")
assert_not_ok(params:add_hosted_mcp_tool({
  server_label = "bad",
  server_url = "https://example.test/mcp",
  connector_id = "conn_lua",
}), "Lua hosted MCP endpoints are mutually exclusive")
assert_not_ok(params:add_hosted_mcp_tool({
  server_label = "bad",
  server_url = "https://example.test/mcp",
  allowed_tool_names = { "ask" },
  allowed_tools_json = '["ask"]',
}), "Lua hosted MCP allowed tool policies are mutually exclusive")
assert_not_ok(params:add_hosted_mcp_tool({
  server_label = "bad",
  server_url = "https://example.test/mcp",
  allowed_tools_json = '"ask"',
}), "Lua hosted MCP allowed_tools must be array or object")
assert_not_ok(params:add_hosted_mcp_tool({
  server_label = "bad",
  server_url = "https://example.test/mcp",
  headers_json = '[]',
}), "Lua hosted MCP headers must be object")
assert_not_ok(params:add_hosted_mcp_tool({
  server_label = "bad",
  server_url = "https://example.test/mcp",
  require_approval_json = 'true',
}), "Lua hosted MCP require_approval must be string or object")
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
do
  local run_close_seen = false
  local run_close_registry = assert_ok(cai.tool_registry())
  assert_ok(run_close_registry:register_raw_tool(
    "close_during_run", "Lua reentrant registry close test", weather_schema,
    function()
      local ok, err = run_close_registry:close()
      assert_not_ok(ok, err, "active registry close from run")
      assert(tostring(err.message or ""):find("active operation", 1, true),
        "active registry run close returned wrong error")
      run_close_seen = true
      return '{"ok":true}'
    end, true))
  assert_ok(run_close_registry:run("close_during_run",
    '{"city":"Gothenburg"}', function()
    end), nil, "registry run reentrant close")
  assert(run_close_seen, "registry run did not exercise reentrant close")
  run_close_registry:close()
end
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

local retained_spooled_args = nil
assert_ok(registry:register_raw_spooled_tool("lua_retained_spooled_weather", "Lua retained spooled argument test tool", weather_schema, function(args)
  retained_spooled_args = args
  return '{"ok":true}'
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
chunks = {}
assert_ok(registry:run("lua_retained_spooled_weather", '{"city":"Gothenburg"}', function(chunk)
  chunks[#chunks + 1] = chunk
  return true
end))
assert(retained_spooled_args ~= nil, "retained spooled callback arguments missing")
assert(retained_spooled_args:read_all():match("Gothenburg"),
  "retained spooled callback arguments must remain readable after callback")
assert_not_ok(registry:run("lua_spooled_weather", '{"city":"Gothenburg"}', function()
  return false
end), "registry run must propagate sink cancellation")
assert_not_ok(registry:run("lua_throwing_tool", '{"city":"Gothenburg"}', function()
  return true
end), "raw spooled tool must propagate callback failure")
assert_not_ok(registry:run("lua_spooled_weather", '{"city":', function()
  return true
end), "registry run must reject invalid arguments JSON")

local exec_root = "/tmp/cai-lua-exec-test"
os.execute("rm -rf " .. exec_root)
assert(os.execute("mkdir -p " .. exec_root .. "/sub"))
assert_ok(registry:register_exec_tool({
  root_path = exec_root,
  default_workdir = exec_root,
  timeout_ms = 1000,
  max_timeout_ms = 1000,
  output_memory_limit = 8,
  output_max_bytes = 4096,
  allow_pty = true,
}))
local chunks = {}
assert_ok(registry:run("exec_command", '{"cmd":"printf lua-out; printf lua-err >&2","tty":false}', function(chunk)
  chunks[#chunks + 1] = chunk
  return true
end))
local exec_json = table.concat(chunks)
assert(exec_json:match('"stdout":"lua%-out"'))
assert(exec_json:match('"stderr":"lua%-err'))
assert(exec_json:match('"exit_code":0'))
chunks = {}
assert_ok(registry:run("exec_command", '{"cmd":"cat","stdin":"lua-stdin-alpha\\nlua-stdin-beta\\n"}', function(chunk)
  chunks[#chunks + 1] = chunk
  return true
end))
assert(table.concat(chunks):match("lua%-stdin%-alpha"), "exec tool must pass stdin data")
assert(table.concat(chunks):match("lua%-stdin%-beta"), "exec tool must pass multiline stdin data")
chunks = {}
assert_ok(registry:run("exec_command", '{"cmd":"sh -s","stdin":"printf lua-script-ok:%s\\\\n \\"$PWD\\"\\n"}', function(chunk)
  chunks[#chunks + 1] = chunk
  return true
end))
assert(table.concat(chunks):match("lua%-script%-ok:"), "exec tool must run stdin scripts")
chunks = {}
assert_ok(registry:run("exec_command", '{"cmd":"pwd","workdir":"sub","tty":null}', function(chunk)
  chunks[#chunks + 1] = chunk
  return true
end))
assert(table.concat(chunks):match("/sub"))
assert_not_ok(registry:run("exec_command", '{"cmd":"pwd","workdir":"/tmp"}', function()
  return true
end), "exec tool must reject workdir outside root")
chunks = {}
assert_ok(registry:run("exec_command", '{"cmd":"cat /etc/passwd"}', function(chunk)
  chunks[#chunks + 1] = chunk
  return true
end))
local host_passwd_attempt = table.concat(chunks)
assert(not host_passwd_attempt:match("root:x:"), "exec tool must not expose host /etc/passwd")
local leak = io.open("/var/tmp/cai-lua-host-leak", "w")
if leak then
  leak:write("host")
  leak:close()
end
chunks = {}
assert_ok(registry:run("exec_command", '{"cmd":"if test -e /var/tmp/cai-lua-host-leak; then printf leak; else printf isolated; fi; printf ok >/var/tmp/sandbox-created"}', function(chunk)
  chunks[#chunks + 1] = chunk
  return true
end))
assert(table.concat(chunks):match("isolated"), "exec tool must isolate /var/tmp")
os.remove("/var/tmp/cai-lua-host-leak")
chunks = {}
assert_ok(registry:run("exec_command", '{"cmd":"printf env:${CAI_LUA_EXEC_SHOULD_NOT_LEAK-unset}:$HOME:$TMPDIR:$LANG"}', function(chunk)
  chunks[#chunks + 1] = chunk
  return true
end))
local env_output = table.concat(chunks)
assert(env_output:match("env:unset:"), "exec tool must clear host environment")
assert(env_output:match(":/tmp:"), "exec tool must set sandbox TMPDIR")
chunks = {}
assert_ok(registry:run("exec_command", '{"cmd":"if test -t 0; then printf in-tty; else printf in-notty; fi; read x && printf got:$x || printf read-eof","stdin":"pty-input\\n","tty":true}', function(chunk)
  chunks[#chunks + 1] = chunk
  return true
end))
local tty_output = table.concat(chunks)
assert(tty_output:match("in%-notty"), "exec PTY mode must not expose stdin as tty")
assert(tty_output:match("got:pty%-input"), "exec PTY mode must receive configured stdin")

local read_root = "/tmp/cai-lua-read-test"
os.execute("rm -rf " .. read_root)
assert(os.execute("mkdir -p " .. read_root .. "/sub"))
assert(os.execute("mkdir -p " .. read_root .. "/sub/nested"))
local read_file = assert(io.open(read_root .. "/sub/alpha.txt", "w"))
read_file:write("one\ntwo\nthree\n")
read_file:close()
local deep_file = assert(io.open(read_root .. "/sub/nested/deep.txt", "w"))
deep_file:write("deep\n")
deep_file:close()
local hidden_file = assert(io.open(read_root .. "/sub/.hidden", "w"))
hidden_file:write("hidden\n")
hidden_file:close()
local utf8_file = assert(io.open(read_root .. "/sub/utf8.txt", "wb"))
utf8_file:write("a", string.char(0xc3), string.char(0xa9), "\n")
utf8_file:close()
local binary_file = assert(io.open(read_root .. "/sub/binary.bin", "wb"))
binary_file:write("text", string.char(0), "x")
binary_file:close()
local control_file = assert(io.open(read_root .. "/sub/control.txt", "wb"))
control_file:write("text", string.char(0x1b), "x")
control_file:close()
local invalid_utf8_file = assert(io.open(read_root .. "/sub/invalid-utf8.txt", "wb"))
invalid_utf8_file:write("bad", string.char(0xc3), "(")
invalid_utf8_file:close()
assert_ok(registry:register_read_tool({
  root_path = read_root,
  default_workdir = read_root .. "/sub",
  content_memory_limit = 8,
  content_max_bytes = 64,
}))
assert_ok(registry:register_list_files_tool({
  root_path = read_root,
  default_workdir = read_root .. "/sub",
  content_memory_limit = 8,
  content_max_bytes = 64,
}))
chunks = {}
assert_ok(registry:run("read_file", '{"path":"alpha.txt","start_line":2,"end_line":2}', function(chunk)
  chunks[#chunks + 1] = chunk
  return true
end))
local read_json = table.concat(chunks)
assert(read_json:match('"content":"two\\n"'), "read_file must return selected line content")
assert(read_json:match('"truncated":false'), "read_file must report non-truncated reads")
chunks = {}
assert_ok(registry:run("read_file", '{"path":"alpha.txt","max_bytes":4}', function(chunk)
  chunks[#chunks + 1] = chunk
  return true
end))
assert(table.concat(chunks):match('"truncated":true'), "read_file must report max byte truncation")
chunks = {}
assert_ok(registry:run("read_file", '{"path":"utf8.txt","max_bytes":2}', function(chunk)
  chunks[#chunks + 1] = chunk
  return true
end))
local utf8_json = table.concat(chunks)
assert(utf8_json:match('"content":"a"'), "read_file must not split UTF-8 characters")
assert(utf8_json:match('"truncated":true'), "read_file must report UTF-8 boundary truncation")
assert_not_ok(registry:run("read_file", '{"path":"/etc/passwd"}', function()
  return true
end), "read_file must reject absolute escapes")
assert_not_ok(registry:run("read_file", '{"path":"../missing/../../etc/passwd"}', function()
  return true
end), "read_file must reject relative escapes")
assert_not_ok(registry:run("read_file", '{"path":"binary.bin"}', function()
  return true
end), "read_file must reject binary content")
assert_not_ok(registry:run("read_file", '{"path":"control.txt"}', function()
  return true
end), "read_file must reject control characters")
assert_not_ok(registry:run("read_file", '{"path":"invalid-utf8.txt"}', function()
  return true
end), "read_file must reject invalid UTF-8 content")
chunks = {}
assert_ok(registry:run("list_files", '{"path":"."}', function(chunk)
  chunks[#chunks + 1] = chunk
  return true
end))
local list_json = table.concat(chunks)
assert(list_json:match('"path":"sub/alpha%.txt"'), "list_files must list files")
assert(list_json:match('"text_candidate":true'), "list_files must report text candidates")
assert(list_json:match('"path":"sub/binary%.bin"'), "list_files must list binary files")
assert(list_json:match('"binary_candidate":true'), "list_files must report binary candidates")
assert(not list_json:match("%.hidden"), "list_files must hide dotfiles by default")
chunks = {}
assert_ok(registry:run("list_files", '{"path":".","recursive":true,"include_hidden":true}', function(chunk)
  chunks[#chunks + 1] = chunk
  return true
end))
local recursive_list_json = table.concat(chunks)
assert(recursive_list_json:match('"path":"sub/nested/deep%.txt"'), "list_files must recurse")
assert(recursive_list_json:match('"path":"sub/%.hidden"'), "list_files must include hidden when requested")
chunks = {}
assert_ok(registry:run("list_files", '{"path":".","recursive":true,"max_entries":1}', function(chunk)
  chunks[#chunks + 1] = chunk
  return true
end))
assert(table.concat(chunks):match('"truncated":true'), "list_files must report truncation")
assert_not_ok(registry:run("list_files", '{"path":"/etc"}', function()
  return true
end), "list_files must reject absolute escapes")

local native_todo = native_store_test
assert_not_ok(cai.native_store("todo", nil),
  "native todo stores must require a C callback table")
native_todo.reset()
local native_store = assert(native_todo.new())
local native_registry = cai.tool_registry()
assert_throws(function()
  cai.mcp_handler({
    name = "cai-lua-wrong-native-store",
    tools = native_registry,
    session = native_store,
  })
end, "MCP handler must reject a todo backend")
assert_ok(native_registry:register_todo_tool({
  store = native_store,
  default_board = "native",
}))
assert_not_ok(native_registry:register_todo_tool({store = native_store}),
  "a native todo store must only be registered once")
assert_not_ok(native_registry:run("todo_kanban",
  '{"operation":"list_boards"}', function()
    return true
  end), "native C callbacks must service todo operations")
assert(native_todo.begin_count() == 1,
  "native todo store must be called without a Lua callback")
native_registry:close()
assert(native_todo.destroy_count() == 1,
  "native todo store context must be destroyed exactly once")
native_store:close()
collectgarbage("collect")
assert(native_todo.destroy_count() == 1,
  "closing or collecting consumed native store must not double destroy")

os.remove("/tmp/cai-lua-test-todo.json")
os.remove("/tmp/cai-lua-test-todo.lock")
assert_ok(registry:register_todo_tool({
  store_path = "/tmp/cai-lua-test-todo.json",
  lock_path = "/tmp/cai-lua-test-todo.lock",
  default_board = "lua",
}))

chunks = {}
assert_ok(registry:run("todo_kanban", '{"operation":"help"}', function(chunk)
  chunks[#chunks + 1] = chunk
  return true
end))
local help_json = table.concat(chunks)
assert(help_json:match("todo_kanban"))
assert(help_json:match("operation"))

chunks = {}
assert_ok(registry:run(
  "todo_kanban",
  '{"operation":"list_boards","board_id":null,"board_name":null,"item_id":null,"title":null,"description":null,"status":null,"wip_limit":null}',
  function(chunk)
    chunks[#chunks + 1] = chunk
    return true
  end
))
local list_json = table.concat(chunks)
assert(list_json:match('"ok":true'))
assert(list_json:match("boards listed"))
assert(list_json:match('"boards"%s*:'))
assert(list_json:match('"name":"lua"'))
assert(list_json:match('"board_count":1'))
assert(not list_json:match('"items"%s*:'))

chunks = {}
assert_ok(registry:run(
  "todo_kanban",
  '{"operation":"add_item","title":"lua default task"}',
  function(chunk)
    chunks[#chunks + 1] = chunk
    return true
  end
))
local default_add_json = table.concat(chunks)
assert(default_add_json:match('"ok":true'))
assert(default_add_json:match('"board_name":"lua"'))
assert(default_add_json:match('"item_id"'))

chunks = {}
assert_ok(registry:run(
  "todo_kanban",
  '{"operation":"set_wip_limit","board_id":null,"board_name":null,"item_id":null,"title":null,"description":null,"status":null,"wip_limit":null}',
  function(chunk)
    chunks[#chunks + 1] = chunk
    return true
  end
))
local invalid_wip_json = table.concat(chunks)
assert(invalid_wip_json:match('"ok":false'))
assert(invalid_wip_json:match("invalid_request"))
assert(invalid_wip_json:match("wip_limit is required"))

local mcp = assert_ok(cai.mcp_handler({
  name = "cai-lua-test",
  version = "0.0.0",
  tools = registry,
  require_protocol_version = 1,
  tool_output_max_bytes = cai.MCP_TOOL_OUTPUT_UNLIMITED,
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

native_store_test.reset_sessions()
local native_mcp_store = assert(native_store_test.new_mcp_session())
assert_throws(function()
  cai.mcp_handler({
    name = "cai-lua-native-mcp-store-invalid",
    tools = {},
    session = native_mcp_store,
  })
end, "invalid native MCP session configuration must preserve the backend")
local native_mcp = assert_ok(cai.mcp_handler({
  name = "cai-lua-native-mcp-store",
  tools = registry,
  session = native_mcp_store,
}))
local native_init_chunks = {}
local native_init = assert_ok(native_mcp:handle_http({
  method = "POST",
  headers = {
    ["content-type"] = "application/json",
    ["mcp-protocol-version"] = cai.MCP_PROTOCOL_VERSION,
  },
  body = '{"jsonrpc":"2.0","id":"native-init","method":"initialize","params":{"protocolVersion":"' ..
      cai.MCP_PROTOCOL_VERSION ..
      '","clientInfo":{"name":"native-client","version":"1.0"}}}',
  write = function(chunk)
    native_init_chunks[#native_init_chunks + 1] = chunk
    return true
  end,
}))
assert_eq(native_init.status, 200, "native MCP initialize status")
assert_eq(native_init.headers["mcp-session-id"], "native-session",
  "native MCP session header")
assert_eq(native_store_test.mcp_create_count(), 1,
  "native MCP create callback count")
local native_ping = assert_ok(native_mcp:handle_http({
  method = "POST",
  headers = {
    ["content-type"] = "application/json",
    ["mcp-protocol-version"] = cai.MCP_PROTOCOL_VERSION,
    ["mcp-session-id"] = native_init.headers["mcp-session-id"],
  },
  body = '{"jsonrpc":"2.0","id":"native-ping","method":"ping"}',
  write = function()
    return true
  end,
}))
assert_eq(native_ping.status, 200, "native MCP ping status")
assert_eq(native_store_test.mcp_load_count(), 1,
  "native MCP load callback count")
assert_eq(native_store_test.mcp_save_count(), 1,
  "native MCP save callback count")
local native_delete = assert_ok(native_mcp:handle_http({
  method = "DELETE",
  headers = {
    ["mcp-protocol-version"] = cai.MCP_PROTOCOL_VERSION,
    ["mcp-session-id"] = native_init.headers["mcp-session-id"],
  },
  body = "{}",
  write = function()
    return true
  end,
}))
assert_eq(native_delete.status, 202, "native MCP delete status")
assert_eq(native_store_test.mcp_destroy_count(), 1,
  "native MCP destroy callback count")
native_mcp:close()
assert_eq(native_store_test.cleanup_count(), 1,
  "native MCP session cleanup must run exactly once")
native_mcp_store:close()
collectgarbage("collect")
assert_eq(native_store_test.cleanup_count(), 1,
  "consumed native MCP session store must not double cleanup")

local sessions = {}
local session_events = { creates = 0, loads = 0, saves = 0, destroys = 0 }
local stateful_mcp = assert_ok(cai.mcp_handler({
  name = "cai-lua-stateful-test",
  tools = registry,
  session = {
    create = function(state)
      session_events.creates = session_events.creates + 1
      assert(state.client_name == "lua-client")
      sessions["lua-session-1"] = state
      return "lua-session-1"
    end,
    load = function(id)
      session_events.loads = session_events.loads + 1
      return sessions[id]
    end,
    save = function(id, state)
      session_events.saves = session_events.saves + 1
      sessions[id] = state
      return true
    end,
    destroy = function(id)
      session_events.destroys = session_events.destroys + 1
      sessions[id] = nil
      return true
    end,
  },
}))
local stateful_chunks = {}
local init_response = assert_ok(stateful_mcp:handle_http({
  method = "POST",
  headers = {
    ["content-type"] = "application/json",
    ["mcp-protocol-version"] = cai.MCP_PROTOCOL_VERSION,
  },
  body = '{"jsonrpc":"2.0","id":"init","method":"initialize","params":{"protocolVersion":"' ..
      cai.MCP_PROTOCOL_VERSION ..
      '","clientInfo":{"name":"lua-client","version":"1.0"}}}',
  write = function(chunk)
    stateful_chunks[#stateful_chunks + 1] = chunk
    return true
  end,
}))
assert_eq(init_response.status, 200, "stateful mcp initialize status")
assert_eq(init_response.headers["mcp-session-id"], "lua-session-1",
  "stateful mcp session header")
assert_eq(session_events.creates, 1, "stateful mcp create count")

stateful_chunks = {}
local ping_response = assert_ok(stateful_mcp:handle_http({
  method = "POST",
  headers = {
    ["content-type"] = "application/json",
    ["mcp-protocol-version"] = cai.MCP_PROTOCOL_VERSION,
    ["mcp-session-id"] = init_response.headers["mcp-session-id"],
  },
  body = '{"jsonrpc":"2.0","id":"ping","method":"ping"}',
  write = function(chunk)
    stateful_chunks[#stateful_chunks + 1] = chunk
    return true
  end,
}))
assert_eq(ping_response.status, 200, "stateful mcp ping status")
assert_eq(session_events.loads, 1, "stateful mcp load count")
assert_eq(session_events.saves, 1, "stateful mcp save count")
assert(table.concat(stateful_chunks):match('"id":"ping"'))

stateful_chunks = {}
local missing_response = assert_ok(stateful_mcp:handle_http({
  method = "POST",
  headers = {
    ["content-type"] = "application/json",
    ["mcp-protocol-version"] = cai.MCP_PROTOCOL_VERSION,
  },
  body = '{"jsonrpc":"2.0","id":"missing","method":"ping"}',
  write = function(chunk)
    stateful_chunks[#stateful_chunks + 1] = chunk
    return true
  end,
}))
assert_eq(missing_response.status, 400, "stateful mcp missing session status")

local delete_response = assert_ok(stateful_mcp:handle_http({
  method = "DELETE",
  headers = {
    ["mcp-protocol-version"] = cai.MCP_PROTOCOL_VERSION,
    ["mcp-session-id"] = init_response.headers["mcp-session-id"],
  },
  body = "{}",
  write = function()
    return true
  end,
}))
assert_eq(delete_response.status, 202, "stateful mcp delete status")
assert_eq(session_events.destroys, 1, "stateful mcp destroy count")
stateful_mcp:close()

registry:close()
os.remove("/tmp/cai-lua-test-todo.json")
os.remove("/tmp/cai-lua-test-todo.lock")

dofile("tests/lua/smith_terminal_renderer_test.lua")

print("cai lua tests passed")
