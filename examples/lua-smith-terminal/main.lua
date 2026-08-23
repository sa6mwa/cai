local cai = require("cai")
local lonejson = require("lonejson")

local reset = "\27[0m"
local gray = "\27[90m"
local green = "\27[32m"
local magenta = "\27[35m"
local red = "\27[31m"

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
local auth_json = os.getenv("CAI_CHATGPT_AUTH_JSON")
local use_chatgpt_auth = true
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
    io.stderr:write("usage: lua examples/lua-smith-terminal/main.lua [--chatgpt-auth] [--chatgpt-auth-json <path>] [--model <model>]\n\n" ..
      "Smith uses CAI's ChatGPT subscription auth by default. Run make chatgpt-login first.\n")
    os.exit(0)
  else
    io.stderr:write("unknown argument: " .. tostring(arg[i]) .. "\n")
    os.exit(2)
  end
end

if not model or model == "" then
  model = os.getenv("CAI_SMITH_MODEL")
end
if not model or model == "" then
  model = os.getenv("CAI_TERMINAL_CHAT_MODEL")
end
if not model or model == "" then
  model = "gpt-5.6-luna"
end

local render = {
  lines = 0,
  omitted = false,
  command = "",
  text_open = false,
  reasoning_open = false,
  suppress_review_text = false,
  review_report_visible = false,
}

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

local function close_message()
  if render.text_open or render.reasoning_open then
    io.write(reset, "\n")
    render.text_open = false
    render.reasoning_open = false
  end
end

-- Reviewer strings are model-controlled. Keep decoded control code points
-- visible rather than allowing JSON escapes to become terminal commands.
local function safe_terminal_text(value)
  local output = {}
  local index = 1
  while index <= #value do
    local byte = value:byte(index)
    local next_byte = value:byte(index + 1)
    if byte == 0xc2 and next_byte and next_byte >= 0x80 and next_byte <= 0x9f then
      output[#output + 1] = string.format("\\u00%02X", next_byte)
      index = index + 2
    elseif byte < 0x20 or (byte >= 0x7f and byte <= 0x9f) then
      output[#output + 1] = string.format("\\x%02X", byte)
      index = index + 1
    else
      output[#output + 1] = string.char(byte)
      index = index + 1
    end
  end
  return table.concat(output)
end

local function render_review_body(body)
  local start = 1
  while start <= #body do
    local newline = body:find("\n", start, true)
    local finish = newline and newline - 1 or #body
    io.write("  ", safe_terminal_text(body:sub(start, finish)), "\n")
    if not newline then
      return
    end
    start = newline + 1
  end
end

-- The runtime has already validated the report. Its event payload stays JSON,
-- while the example renders the same operator-facing form as Codex review.
local function render_review_report(data)
  local decoded, report = pcall(lonejson.decode_json, data)
  if not decoded or type(report) ~= "table" then
    io.write(red, "Reviewer report could not be displayed\n", reset)
    return
  end
  local explanation = tostring(report.overall_explanation or "")
  local findings = type(report.findings) == "table" and report.findings or {}
  if explanation ~= "" then
    io.write(safe_terminal_text(explanation), "\n")
  end
  if #findings > 0 then
    if explanation ~= "" then
      io.write("\n")
    end
    io.write(#findings > 1 and "Full review comments:\n" or "Review comment:\n")
  elseif explanation == "" then
    -- A contract-valid reviewer can return only its verdict. This is still a
    -- completed review, rather than a missing reviewer response.
    io.write("Review completed: ",
      safe_terminal_text(tostring(report.overall_correctness or "unknown")), ".\n")
  end
  for _, finding in ipairs(findings) do
    local location = type(finding.code_location) == "table" and
      finding.code_location or {}
    local range = type(location.line_range) == "table" and location.line_range or {}
    io.write("\n- ", safe_terminal_text(tostring(finding.title or "Untitled finding")), " — ",
      safe_terminal_text(tostring(location.absolute_file_path or "unknown")), ":",
      tostring(range.start or "?"), "-", tostring(range["end"] or "?"), "\n")
    render_review_body(tostring(finding.body or ""))
  end
end

local function render_event(event)
  if event.type == cai.AGENT_EVENT_REASONING_SUMMARY then
    if not render.reasoning_open then
      close_message()
      io.write(magenta, "Thinking: ", reset)
      render.reasoning_open = true
    end
    io.write(event.data or "")
    io.flush()
  elseif event.type == cai.AGENT_EVENT_TEXT_DELTA then
    if render.suppress_review_text then
      return
    end
    if not render.text_open then
      close_message()
      io.write(green, "Smith: ", reset)
      render.text_open = true
    end
    io.write(event.data or "")
    io.flush()
  elseif event.type == cai.AGENT_EVENT_RESPONSE_COMPLETED then
    -- A steering follow-up remains in the same run but starts a new model
    -- response, so the next text/reasoning delta receives its own label.
    close_message()
  elseif event.type == cai.AGENT_EVENT_TERMINAL_COMMAND_STARTED then
    close_message()
    render.lines = 0
    render.omitted = false
    render.command = remembered_command(event.data or "")
    io.write("$ ", event.data or "", "\n")
  elseif event.type == cai.AGENT_EVENT_TERMINAL_OUTPUT then
    close_message()
    render_terminal_output(event)
  elseif event.type == cai.AGENT_EVENT_TERMINAL_WAITING then
    close_message()
    io.write(gray, "Waiting for terminal progress…\n", reset)
  elseif event.type == cai.AGENT_EVENT_TERMINAL_COMMAND_COMPLETED or
      event.type == cai.AGENT_EVENT_TERMINAL_COMMAND_CANCELLED then
    close_message()
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
    close_message()
    io.write(gray, "Queued next turn\n", reset)
  elseif event.type == cai.AGENT_EVENT_REVIEW_REPORT then
    close_message()
    render_review_report(event.data or "{}")
    render.review_report_visible = true
  elseif event.type == cai.AGENT_EVENT_REVIEW_HANDED_OFF then
    close_message()
    -- The durable parent handoff repeats the final review payload as a
    -- fallback. Display the result itself, never an opaque receipt.
    if not render.review_report_visible and event.data and #event.data > 0 then
      render_review_report(event.data)
    end
    render.review_report_visible = false
  elseif event.type == cai.AGENT_EVENT_TOOL_CALL_COMPLETED and
      event.tool_name ~= "exec_command" and event.tool_name ~= "write_stdin" then
    close_message()
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
  elseif event.type == cai.AGENT_EVENT_RUN_FAILED then
    close_message()
    io.write(red, "Smith failed: ", event.data or "agent run failed", "\n", reset)
  elseif event.type == cai.AGENT_EVENT_RUN_COMPLETED then
    close_message()
  end
end

local function review_request(command)
  local argument = command:sub(8):match("^%s*(.-)%s*$")
  if argument == "" or argument == "uncommitted" then
    return { target = "uncommitted" }
  end
  local base = argument:match("^base%s+(.+)$")
  if base then
    return { target = "base", base_branch = base }
  end
  local commit = argument:match("^commit%s+(.+)$")
  if commit then
    return { target = "commit", commit = commit }
  end
  -- Preserve Codex-like free-form review scope verbatim.
  return { target = "custom", instructions = argument }
end

local function run_review(parent, command)
  local review, err = parent:start_review(review_request(command))
  if not review then
    fail("runtime:start_review", err)
  end
  if err then
    io.stderr:write("review start checkpoint failed: " ..
      (err.message or err.status_string or tostring(err)) .. "\n")
  end
  io.write(gray, "Started isolated review\n", reset)
  render.suppress_review_text = true
  while true do
    local state, pump_err = review:pump(100)
    if not state then
      review:close()
      fail("review:pump", pump_err)
    end
    if state == "completed" or state == "failed" or state == "cancelled" then
      break
    end
  end
  -- A terminal state can become observable before the owner has dispatched
  -- every final event. Drain once more so the report is never skipped.
  ok(review:pump(0), nil, "review:pump final events")
  ok(parent:finish_review(review), nil, "runtime:finish_review")
  -- Flush the durable parent handoff before another operator action.
  ok(parent:pump(0), nil, "runtime:pump review handoff")
  review:close()
  render.suppress_review_text = false
end

local client_config = { chatgpt_auth = use_chatgpt_auth, chatgpt_auth_json = auth_json }
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
  elseif line:sub(1, 7) == "/review" and
      (#line == 7 or line:sub(8, 8):match("%s")) then
    run_review(runtime, line)
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
