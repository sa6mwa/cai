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
  local input_tokens = usage.input_tokens or 0
  local input = input_tokens - (usage.input_cached_tokens or 0)
  local cached = usage.input_cached_tokens or 0
  local output = usage.output_tokens or 0
  local input_price = model_info.input_usd_per_million or 0
  local cached_price = model_info.cached_input_usd_per_million or 0
  local output_price = model_info.output_usd_per_million or 0
  local long_threshold = model_info.long_context_threshold_tokens or 0
  if long_threshold > 0 and input_tokens > long_threshold then
    input_price = model_info.long_input_usd_per_million or input_price
    cached_price = model_info.long_cached_input_usd_per_million or cached_price
    output_price = model_info.long_output_usd_per_million or output_price
  end
  return ((input * input_price) + (cached * cached_price) +
    (output * output_price)) / 1000000
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

local function tool_arguments_json(event)
  if event.arguments_json then
    return event.arguments_json
  end
  if event.arguments_spooled and event.arguments_spooled.read_all then
    local ok_read, value = pcall(function()
      return event.arguments_spooled:read_all()
    end)
    if ok_read and value then
      return value
    end
    return "<failed to read arguments>"
  end
  return "{}"
end

local model = os.getenv("CAI_TERMINAL_CHAT_MODEL") or os.getenv("CAI_EXAMPLE_MODEL")
local searxng_base_url = os.getenv("CAI_SEARXNG_BASE_URL") or "http://127.0.0.1:8888"
local todo_store = os.getenv("CAI_LUA_TODO_STORE")
local todo_lock = os.getenv("CAI_LUA_TODO_LOCK")
local exec_tool_dir = nil
local read_tool_dir = nil
local chatgpt_auth_json = nil
local chatgpt_auth = false
local usage_limits = {
  max_output_tokens = 1000000,
}

local function parse_nonnegative_integer(name, value)
  local parsed = tonumber(value)
  if not parsed or parsed < 0 or parsed ~= math.floor(parsed) then
    io.stderr:write(name .. " requires a non-negative integer\n")
    os.exit(2)
  end
  return parsed
end

local function parse_nonnegative_number(name, value)
  local parsed = tonumber(value)
  if not parsed or parsed < 0 then
    io.stderr:write(name .. " requires a non-negative number\n")
    os.exit(2)
  end
  return parsed
end

local i = 1
while i <= #arg do
  if arg[i] == "--exec-tool-dir" then
    if not arg[i + 1] or arg[i + 1] == "" then
      io.stderr:write("--exec-tool-dir requires a path\n")
      os.exit(2)
    end
    exec_tool_dir = arg[i + 1]
    i = i + 2
  elseif arg[i] == "--read-tool-dir" then
    if not arg[i + 1] or arg[i + 1] == "" then
      io.stderr:write("--read-tool-dir requires a path\n")
      os.exit(2)
    end
    read_tool_dir = arg[i + 1]
    i = i + 2
  elseif arg[i] == "--chatgpt-auth" then
    chatgpt_auth = true
    i = i + 1
  elseif arg[i] == "--chatgpt-auth-json" then
    if not arg[i + 1] or arg[i + 1] == "" then
      io.stderr:write("--chatgpt-auth-json requires a path\n")
      os.exit(2)
    end
    chatgpt_auth_json = arg[i + 1]
    chatgpt_auth = true
    i = i + 2
  elseif arg[i] == "--model" then
    if not arg[i + 1] or arg[i + 1] == "" then
      io.stderr:write("--model requires a model id\n")
      os.exit(2)
    end
    model = arg[i + 1]
    i = i + 2
  elseif arg[i] == "--max-input-tokens" then
    usage_limits.max_input_tokens =
      parse_nonnegative_integer("--max-input-tokens", arg[i + 1])
    i = i + 2
  elseif arg[i] == "--max-cached-input-tokens" then
    usage_limits.max_input_cached_tokens =
      parse_nonnegative_integer("--max-cached-input-tokens", arg[i + 1])
    i = i + 2
  elseif arg[i] == "--max-output-tokens" then
    usage_limits.max_output_tokens =
      parse_nonnegative_integer("--max-output-tokens", arg[i + 1])
    i = i + 2
  elseif arg[i] == "--max-reasoning-output-tokens" then
    usage_limits.max_output_reasoning_tokens =
      parse_nonnegative_integer("--max-reasoning-output-tokens", arg[i + 1])
    i = i + 2
  elseif arg[i] == "--max-total-tokens" then
    usage_limits.max_total_tokens =
      parse_nonnegative_integer("--max-total-tokens", arg[i + 1])
    i = i + 2
  elseif arg[i] == "--max-spend-usd" then
    usage_limits.max_spend_usd =
      parse_nonnegative_number("--max-spend-usd", arg[i + 1])
    i = i + 2
  elseif arg[i] == "--help" or arg[i] == "-h" then
    io.stderr:write("usage: lua examples/lua-terminal-chat/main.lua [--chatgpt-auth] [--chatgpt-auth-json <path>] [--model <model>] [--exec-tool-dir <path>] [--read-tool-dir <path>] [usage-limit flags]\n")
    io.stderr:write("usage-limit flags: --max-input-tokens <n> --max-cached-input-tokens <n> --max-output-tokens <n> --max-reasoning-output-tokens <n> --max-total-tokens <n> --max-spend-usd <n>\n")
    os.exit(0)
  else
    io.stderr:write("unknown argument: " .. tostring(arg[i]) .. "\n")
    os.exit(2)
  end
end

if not model or model == "" then
  if chatgpt_auth then
    model = cai.MODEL_GPT_5_4_MINI
  else
    model = cai.MODEL_GPT_5_NANO
  end
end
local model_info = cai.model_info(model)
local reasoning_effort = cai.REASONING_EFFORT_LOW
if chatgpt_auth then
  reasoning_effort = cai.REASONING_EFFORT_MEDIUM
end

if chatgpt_auth_json then
  io.stderr:write("ChatGPT subscription auth enabled with: " .. chatgpt_auth_json .. "\n")
elseif chatgpt_auth then
  io.stderr:write("ChatGPT subscription auth enabled with default auth path\n")
end
if exec_tool_dir then
  io.stderr:write("exec_command enabled with root: " .. exec_tool_dir .. "\n")
else
  io.stderr:write("hint: pass --exec-tool-dir <path> to enable exec_command rooted to that path\n")
end
if read_tool_dir then
  io.stderr:write("read_file enabled with root: " .. read_tool_dir .. "\n")
else
  io.stderr:write("hint: pass --read-tool-dir <path> to enable list_files/read_file rooted to that path\n")
  io.stderr:write("      list_files reports text/binary hints; read_file is UTF-8 text-only\n")
end

local instructions =
  "You are a concise terminal chat assistant. Answer plainly. " ..
  "You have access to searxng_search for web search and todo_kanban " ..
  "for managing a local kanban board. Use search for current, external, " ..
  "or source-backed information and cite URLs from tool results. Use " ..
  "todo_kanban when the user asks you to remember, plan, list, move, " ..
  "limit, or archive work. todo_kanban has a default board; omit " ..
  "board_id, board_key, and board_name for ordinary single-board usage."

if exec_tool_dir or read_tool_dir then
  instructions =
    "You are a concise terminal chat assistant. Answer plainly. " ..
    "You have access to searxng_search for web search, todo_kanban for " ..
    "managing a local kanban board, list_files and read_file for inspecting " ..
    "files rooted to the configured read directory, and optionally exec_command for " ..
    "sandboxed command execution rooted to the configured sandbox " ..
    "directory. Use " ..
    "search for current, external, or source-backed information and cite " ..
    "URLs from tool results. Use todo_kanban when the user asks you to " ..
    "remember, plan, list, move, limit, or archive work. todo_kanban has " ..
    "a default board; omit board_id, board_key, and board_name for ordinary " ..
    "single-board usage. Use list_files before read_file when discovering " ..
    "paths. Prefer read_file over exec_command when the user " ..
    "asks to inspect file contents. Use exec_command only when the user explicitly " ..
    "asks you to inspect or run commands, always set workdir when a " ..
    "specific directory matters, and do not assume network access."
end

local client_config
if chatgpt_auth then
  client_config = { chatgpt_auth = true, chatgpt_auth_json = chatgpt_auth_json }
else
  client_config = common.client_config(cai)
end
local client = ok(cai.open(client_config), nil, "cai.open")
local agent = ok(client:new_agent({
  model = model,
  instructions = instructions,
  reasoning_effort = reasoning_effort,
  reasoning_summary = cai.REASONING_SUMMARY_AUTO,
  prompt_cache_key = "cai:example:lua-terminal-chat:v1",
  disable_parallel_tool_calls = 1,
  session_usage_limits = usage_limits,
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

if exec_tool_dir then
  ok(agent:register_exec_tool({
    root_path = exec_tool_dir,
    default_workdir = exec_tool_dir,
    timeout_ms = 10000,
    max_timeout_ms = 60000,
    output_memory_limit = 128 * 1024,
    output_max_bytes = 3 * 1024 * 1024,
    allow_pty = true,
  }), nil, "agent:register_exec_tool")
end

if read_tool_dir then
  ok(agent:register_list_files_tool({
    root_path = read_tool_dir,
    default_workdir = read_tool_dir,
    content_memory_limit = 128 * 1024,
    content_max_bytes = 1024 * 1024,
  }), nil, "agent:register_list_files_tool")
  ok(agent:register_read_tool({
    root_path = read_tool_dir,
    default_workdir = read_tool_dir,
    content_memory_limit = 128 * 1024,
    content_max_bytes = 1024 * 1024,
  }), nil, "agent:register_read_tool")
end

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
          gray, cyan, gray, reset, event.name or "(unknown)", tool_arguments_json(event)))
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
        local message = event.error and (event.error.message or event.error.status_string)
        if message then
          io.write(string.format("%s[%stool%s]%s %s failed: %s\n",
            gray, cyan, gray, reset, event.name or "(unknown)", message))
        else
          io.write(string.format("%s[%stool%s]%s %s failed\n",
            gray, cyan, gray, reset, event.name or "(unknown)"))
        end
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
