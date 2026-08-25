local cai = require("cai")
local pslog = require("pslog")

local reset = "\27[0m"
local gray = "\27[90m"
local green = "\27[32m"
local magenta = "\27[35m"
local red = "\27[31m"

local source = debug.getinfo(1, "S").source
local script_directory = source:sub(1, 1) == "@" and
  source:sub(2):match("^(.*[/\\])") or nil
if script_directory then
  package.path = script_directory .. "?.lua;" .. package.path
end
local smith_renderer = require("renderer")

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
local verbosity = 0
local i = 1
while i <= #arg do
  if arg[i] == "--chatgpt-auth" then
    use_chatgpt_auth = true
    i = i + 1
  elseif arg[i] == "-v" or arg[i] == "--verbose" then
    verbosity = verbosity + 1
    i = i + 1
  elseif arg[i] == "-vv" then
    verbosity = verbosity + 2
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
    io.stderr:write("usage: lua examples/lua-smith-terminal/main.lua [--chatgpt-auth] [--chatgpt-auth-json <path>] [--model <model>] [-v|--verbose] [-vv]\n\n" ..
      "Smith uses CAI's ChatGPT subscription auth by default. Run make chatgpt-login first.\n\n" ..
      "  -v, --verbose  Enable debug lifecycle logging on stderr.\n" ..
      "  -vv            Enable trace lifecycle logging on stderr.\n\n" ..
      "Logging defaults to disabled; LOG_LEVEL and other LOG_* pslog settings override these defaults.\n")
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

local logger = pslog.from_env("LOG_", {
  output = "stderr",
  mode = "console",
  color = "auto",
  min_level = verbosity >= 2 and "trace" or (verbosity == 1 and "debug" or "disabled"),
}):with("component", "smith-example")

local render = smith_renderer.new(cai, {
  write = io.write,
  flush = io.flush,
}, {
  reset = reset,
  gray = gray,
  green = green,
  magenta = magenta,
  red = red,
})

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
  ok(review:pump(0), nil, "review:pump final events")
  ok(parent:finish_review(review), nil, "runtime:finish_review")
  ok(parent:pump(0), nil, "runtime:pump review handoff")
  review:close()
  render.suppress_review_text = false
end

local client = ok(cai.open({
  chatgpt_auth = use_chatgpt_auth,
  chatgpt_auth_json = auth_json,
  logger = logger,
}), nil, "cai.open")
local runtime = ok(client:new_smith_runtime({
  workspace_directory = os.getenv("PWD") or ".",
  model = model,
  logger = logger,
  event_callback = render.event,
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
logger:close()
