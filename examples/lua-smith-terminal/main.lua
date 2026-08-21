local cai = require("cai")
local common = dofile("examples/lua-common.lua")

local reset = "\27[0m"
local gray = "\27[90m"

local function fail(operation, err)
  io.stderr:write(operation .. " failed: " ..
    (type(err) == "table" and (err.message or err.status_string) or tostring(err)) .. "\n")
  os.exit(1)
end

local function ok(value, err, operation)
  if not value then
    fail(operation, err)
  end
  return value
end

local model = nil
local auth_json = nil
local use_chatgpt_auth = false
local i = 1
while i <= #arg do
  if arg[i] == "--chatgpt-auth" then
    use_chatgpt_auth = true
    i = i + 1
  elseif arg[i] == "--chatgpt-auth-json" then
    auth_json = arg[i + 1]
    if not auth_json or auth_json == "" then
      io.stderr:write("--chatgpt-auth-json requires a path\n")
      os.exit(2)
    end
    use_chatgpt_auth = true
    i = i + 2
  elseif arg[i] == "--model" then
    model = arg[i + 1]
    if not model or model == "" then
      io.stderr:write("--model requires a model id\n")
      os.exit(2)
    end
    i = i + 2
  elseif arg[i] == "--help" or arg[i] == "-h" then
    io.stderr:write("usage: lua examples/lua-smith-terminal/main.lua [--chatgpt-auth] [--chatgpt-auth-json <path>] [--model <model>]\n")
    os.exit(0)
  else
    io.stderr:write("unknown argument: " .. tostring(arg[i]) .. "\n")
    os.exit(2)
  end
end

local render = { lines = 0, omitted = false, command = "" }

local function remembered_command(value)
  if #value <= 160 then
    return value
  end
  return value:sub(1, 157) .. "..."
end

local function render_terminal_output(event)
  local data = event.data or ""
  local start = 1
  while start <= #data do
    local newline = data:find("\n", start, true)
    local finish = newline and newline - 1 or #data
    if render.lines < 10 then
      io.write(gray, data:sub(start, finish))
      if newline then
        io.write("\n")
        render.lines = render.lines + 1
      end
      io.write(reset)
    elseif not render.omitted then
      io.write(gray, "… more output omitted\n", reset)
      render.omitted = true
    end
    if not newline then
      break
    end
    start = newline + 1
  end
end

local function render_event(event)
  if event.type == cai.AGENT_EVENT_TEXT_DELTA then
    io.write(event.data or "")
    io.flush()
  elseif event.type == cai.AGENT_EVENT_TERMINAL_COMMAND_STARTED then
    render.lines = 0
    render.omitted = false
    render.command = remembered_command(event.data or "")
    io.write("$ ", event.data or "", "\n")
  elseif event.type == cai.AGENT_EVENT_TERMINAL_OUTPUT then
    render_terminal_output(event)
  elseif event.type == cai.AGENT_EVENT_TERMINAL_WAITING then
    io.write(gray, "Waiting for terminal progress…\n", reset)
  elseif event.type == cai.AGENT_EVENT_TERMINAL_COMMAND_COMPLETED or
      event.type == cai.AGENT_EVENT_TERMINAL_COMMAND_CANCELLED then
    local verb = event.type == cai.AGENT_EVENT_TERMINAL_COMMAND_CANCELLED and
      "Cancelled" or "Ran"
    local status
    if event.terminal_exit_code ~= nil then
      status = "exit " .. tostring(event.terminal_exit_code)
    elseif event.terminal_signal ~= nil then
      status = "signal " .. tostring(event.terminal_signal)
    else
      status = "status unavailable"
    end
    io.write(string.format("%s %s (%s, %.1fs)\n", verb, render.command,
      status, (event.terminal_duration_ms or 0) / 1000))
  elseif event.type == cai.AGENT_EVENT_TURN_QUEUED then
    io.write(gray, "Queued next turn\n", reset)
  elseif event.type == cai.AGENT_EVENT_TOOL_CALL_COMPLETED and
      event.tool_name ~= "exec_command" and event.tool_name ~= "write_stdin" then
    local verbs = {
      [cai.AGENT_TOOL_ACTION_READ] = "Read",
      [cai.AGENT_TOOL_ACTION_LIST] = "Listed",
      [cai.AGENT_TOOL_ACTION_VIEW] = "Viewed",
      [cai.AGENT_TOOL_ACTION_PATCH] = "Patched",
    }
    local verb = verbs[event.tool_action]
    if verb and event.tool_path then
      io.write(gray, verb, " ", event.tool_path, "\n", reset)
    elseif verb and (event.tool_path_count or 0) > 1 then
      io.write(gray, verb, " ", tostring(event.tool_path_count), " files\n", reset)
    elseif verb then
      io.write(gray, verb, "\n", reset)
    else
      io.write(gray, "Completed ", event.tool_name or "tool", "\n", reset)
    end
  end
end

local client_config
if use_chatgpt_auth then
  client_config = { chatgpt_auth = true, chatgpt_auth_json = auth_json }
else
  client_config = common.client_config(cai)
end
local client = ok(cai.open(client_config), nil, "cai.open")
local runtime = ok(client:new_smith_runtime({
  workspace_directory = os.getenv("PWD") or ".",
  model = model,
  event_callback = render_event,
}), nil, "client:new_smith_runtime")

while true do
  io.write("smith> ")
  io.flush()
  local line = io.read("*l")
  if not line or line == "/exit" then
    break
  end
  if line == "/export" then
    local path, err = runtime:export_markdown_file("cai")
    if not path then
      io.stderr:write("export failed: " ..
        (type(err) == "table" and (err.message or err.status_string) or tostring(err)) .. "\n")
    else
      io.write(gray, "Exported ", path, "\n", reset)
    end
  elseif line ~= "" then
    if line:sub(1, 7) == "/queue " then
      ok(runtime:submit_queued(line:sub(8)), nil, "runtime:submit_queued")
    else
      local state = ok(runtime:state(), nil, "runtime:state")
      if state == "sampling" or state == "dispatching_tool" then
        ok(runtime:submit_steering(line), nil, "runtime:submit_steering")
      else
        ok(runtime:submit(line), nil, "runtime:submit")
      end
    end
    while true do
      local state, err = runtime:pump(100)
      if not state then
        fail("runtime:pump", err)
      end
      if state == "completed" or state == "failed" or state == "cancelled" then
        io.write("\n")
        break
      end
    end
  end
end

runtime:close()
client:close()
