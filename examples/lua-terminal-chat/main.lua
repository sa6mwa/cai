local cai = require("cai")
local common = dofile("examples/lua-common.lua")

local reset = "\27[0m"
local gray = "\27[90m"
local green = "\27[32m"
local cyan = "\27[96m"
local magenta = "\27[35m"
local bold_white = "\27[1;37m"

local function fail(operation, err)
  io.stderr:write(operation .. " failed: " ..
    (type(err) == "table" and (err.message or err.status_string) or tostring(err)) .. "\n")
  if type(err) == "table" and err.detail then
    io.stderr:write("detail: " .. err.detail .. "\n")
  end
  os.exit(1)
end

local function ok(value, err, operation)
  if not value then
    fail(operation, err)
  end
  return value
end

local function usage_cost(model_info, usage)
  if not model_info or not usage then
    return 0
  end
  local input = (usage.input_tokens or 0) - (usage.input_cached_tokens or 0)
  local cached = usage.input_cached_tokens or 0
  local output = usage.output_tokens or 0
  return ((input * (model_info.input_usd_per_million or 0)) +
    (cached * (model_info.cached_input_usd_per_million or 0)) +
    (output * (model_info.output_usd_per_million or 0))) / 1000000
end

local function print_usage(usage, context, total_cost)
  local context_text = "n/a"
  if context and context.percent then
    context_text = string.format("%.2f%%", context.percent)
  end
  io.stderr:write(string.format(
    "%s[%susage%s]%s input=%s%d%s cached=%s%d%s output=%s%d%s reasoning=%s%d%s total=%s%d%s context=%s%s%s estimated_cost=%s$%.8f%s\n",
    gray, cyan, gray, reset,
    bold_white, usage.input_tokens or 0, reset,
    bold_white, usage.input_cached_tokens or 0, reset,
    bold_white, usage.output_tokens or 0, reset,
    bold_white, usage.output_reasoning_tokens or 0, reset,
    bold_white, usage.total_tokens or 0, reset,
    bold_white, context_text, reset,
    bold_white, total_cost, reset))
end

local model = os.getenv("CAI_EXAMPLE_MODEL") or cai.MODEL_GPT_5_NANO
local model_info = cai.model_info(model)
local searxng_base_url = os.getenv("CAI_SEARXNG_BASE_URL") or "http://127.0.0.1:8888"
local todo_store = os.getenv("CAI_LUA_TODO_STORE")
local todo_lock = os.getenv("CAI_LUA_TODO_LOCK")

local client = ok(cai.open(common.client_config(cai)), nil, "cai.open")
local agent = ok(client:new_agent({
  model = model,
  instructions =
    "You are a concise terminal chat assistant. Answer plainly. " ..
    "You have access to searxng_search for web search and todo_kanban " ..
    "for managing a local kanban board. Use search for current, external, " ..
    "or source-backed information and cite URLs from tool results. Use " ..
    "todo_kanban when the user asks you to remember, plan, list, move, " ..
    "limit, or archive work.",
  reasoning_effort = cai.REASONING_EFFORT_LOW,
  reasoning_summary = cai.REASONING_SUMMARY_AUTO,
  prompt_cache_key = "cai:example:lua-terminal-chat:v1",
}), nil, "client:new_agent")

ok(agent:register_searxng_tool({
  base_url = searxng_base_url,
  engine = os.getenv("CAI_SEARXNG_ENGINE"),
}), nil, "agent:register_searxng_tool")

ok(agent:register_todo_tool({
  store_path = todo_store,
  lock_path = todo_lock,
  default_board = os.getenv("CAI_LUA_TODO_BOARD") or "default",
}), nil, "agent:register_todo_tool")

local session = ok(agent:new_session(), nil, "agent:new_session")
local total_cost = 0

while true do
  io.write("> ")
  io.flush()
  local line = io.read("*l")
  if line == nil then
    if io.type(io.stdin) == "file" then
      io.write("\n")
    end
    break
  end
  if line == "" then
    goto continue
  end
  if line == "/quit" or line == "/exit" or line == "quit" or line == "exit" then
    break
  end

  ok(session:add_user_text(line), nil, "session:add_user_text")
  local stream_ok, stream_err = session:stream({
    max_tool_rounds = 10,
    reasoning = function(chunk)
      io.write(chunk)
      io.flush()
      return true
    end,
    response = function(chunk)
      io.write(chunk)
      io.flush()
      return true
    end,
    reasoning_prefix = gray .. "[" .. magenta .. "reasoning" .. gray .. "] " .. gray,
    reasoning_suffix = reset .. "\n\n",
    response_prefix = gray .. "[" .. green .. "response" .. gray .. "]" .. reset .. " ",
    response_suffix = reset .. "\n",
    tool_event = function(event)
      if event.kind == "start" then
        io.write(string.format("%s[%stool%s]%s %s input=%s\n",
          gray, cyan, gray, reset, event.name or "(unknown)", event.arguments_json or "{}"))
      elseif event.kind == "output" then
        io.write(string.format("%s[%stool%s]%s %s output=",
          gray, cyan, gray, reset, event.name or "(unknown)"))
        if event.write_output then
          event.write_output(function(chunk)
            io.write(chunk)
            return true
          end)
        else
          io.write(string.format("<%d bytes>", event.output_size or 0))
        end
        io.write("\n")
      elseif event.kind == "error" then
        io.write(string.format("%s[%stool%s]%s %s failed\n",
          gray, cyan, gray, reset, event.name or "(unknown)"))
      end
      io.flush()
      return true
    end,
  })
  io.write("\n")
  io.flush()
  if not stream_ok then
    fail("session:stream", stream_err)
  end

  local usage = session:last_usage()
  if usage then
    total_cost = total_cost + usage_cost(model_info, usage)
    local context = session:context()
    print_usage(usage, context, total_cost)
  end

  ::continue::
end

session:close()
agent:close()
client:close()
