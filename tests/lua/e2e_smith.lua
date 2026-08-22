local cai = require("cai")

local function skip(message)
  io.stdout:write("SKIP: " .. message .. "\n")
  os.exit(77)
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

local function checked(label, callback)
  local value, err = callback()
  return assert_ok(value, err, label)
end

local function shell_quote(value)
  return "'" .. value:gsub("'", "'\\\"'\\\"'") .. "'"
end

local function run(command, label)
  local process = assert(io.popen(command .. " 2>&1", "r"))
  local output = process:read("*a")
  local ok, why, status = process:close()

  if not ok then
    fail((label or command) .. " failed (" .. tostring(why) .. " " ..
      tostring(status) .. "):\n" .. output)
  end
  return output
end

local function make_workspace()
  local process = assert(io.popen("mktemp -d /tmp/cai-lua-smith-e2e-XXXXXX", "r"))
  local workspace = process:read("*l")
  local ok, why, status = process:close()

  if not ok or workspace == nil or workspace == "" then
    fail("mktemp failed (" .. tostring(why) .. " " .. tostring(status) .. ")")
  end
  return workspace
end

local function pump_until_completed(runtime, timeout_seconds, label)
  local deadline = os.time() + timeout_seconds

  while true do
    local state = runtime:pump(1000)
    if state == "completed" then
      return
    end
    if state == "failed" or state == "cancelled" then
      fail((label or "agent runtime") .. " reached terminal state " .. state)
    end
    if os.time() >= deadline then
      fail((label or "agent runtime") .. " did not complete within " ..
        tostring(timeout_seconds) .. " seconds (state=" .. tostring(state) .. ")")
    end
  end
end

local function event_state()
  return {
    answer = {},
    reads = 0,
    patches = 0,
    executions = 0,
    terminal_started = 0,
    terminal_completed = 0,
    reasoning_summaries = {},
    review_reports = {},
    review_started = 0,
    review_handed_off = 0,
  }
end

local function event_callback(events)
  return function(event)
    if event.type == cai.AGENT_EVENT_TEXT_DELTA and event.data ~= nil then
      events.answer[#events.answer + 1] = event.data
    elseif event.type == cai.AGENT_EVENT_REASONING_SUMMARY and event.data ~= nil then
      events.reasoning_summaries[#events.reasoning_summaries + 1] = event.data
    elseif event.type == cai.AGENT_EVENT_TOOL_CALL_COMPLETED then
      if event.tool_action == cai.AGENT_TOOL_ACTION_READ then
        events.reads = events.reads + 1
      elseif event.tool_action == cai.AGENT_TOOL_ACTION_PATCH then
        events.patches = events.patches + 1
      elseif event.tool_action == cai.AGENT_TOOL_ACTION_EXECUTE then
        events.executions = events.executions + 1
      end
    elseif event.type == cai.AGENT_EVENT_TERMINAL_COMMAND_STARTED then
      events.terminal_started = events.terminal_started + 1
    elseif event.type == cai.AGENT_EVENT_TERMINAL_COMMAND_COMPLETED then
      events.terminal_completed = events.terminal_completed + 1
    elseif event.type == cai.AGENT_EVENT_REVIEW_REPORT then
      events.review_reports[#events.review_reports + 1] = event.data or ""
    elseif event.type == cai.AGENT_EVENT_REVIEW_STARTED then
      events.review_started = events.review_started + 1
    elseif event.type == cai.AGENT_EVENT_REVIEW_HANDED_OFF then
      events.review_handed_off = events.review_handed_off + 1
    end
  end
end

local function event_answer(events)
  return table.concat(events.answer)
end

local function assert_contains(label, haystack, needle)
  if haystack == nil or not haystack:find(needle, 1, true) then
    fail(label .. " missing " .. needle .. ":\n" .. tostring(haystack))
  end
end

local function readable_file(path)
  if path == nil or path == "" then
    return false
  end
  local file = io.open(path, "rb")
  if file == nil then
    return false
  end
  file:close()
  return true
end

local function chatgpt_auth_path()
  local configured = os.getenv("CAI_CHATGPT_AUTH_JSON")
  if configured ~= nil and configured ~= "" then
    if readable_file(configured) then
      return configured
    end
    skip("configured ChatGPT auth file is unavailable: " .. configured)
  end

  local home = os.getenv("HOME")
  local codex_path = home ~= nil and home ~= "" and
    home .. "/.codex/auth.json" or nil
  if readable_file(codex_path) then
    return codex_path
  end

  local default_path, default_path_error = cai.chatgpt_auth_default_path()
  if default_path == nil or default_path == "" then
    skip("ChatGPT auth path is unavailable: " .. tostring(default_path_error))
  end
  if readable_file(default_path) then
    return default_path
  end
  skip("ChatGPT auth file is unavailable: " .. default_path)
end

if os.getenv("CAI_LUA_SMITH_E2E") ~= "1" then
  skip("set CAI_LUA_SMITH_E2E=1 to run Lua Smith e2e")
end

local auth_path = chatgpt_auth_path()

local workspace = make_workspace()
local client = nil
local parent = nil
local review = nil
local success = false

local function cleanup()
  if review ~= nil then
    review:close()
    review = nil
  end
  if parent ~= nil then
    parent:close()
    parent = nil
  end
  if client ~= nil then
    client:close()
    client = nil
  end
  run("rm -rf " .. shell_quote(workspace), "temporary workspace cleanup")
end

local ok, err = xpcall(function()
  local git_workspace = shell_quote(workspace)
  local events = event_state()
  local build_prompt = table.concat({
    "You are in a newly initialized, otherwise empty Git repository.",
    "Create a small Hello World project in C.",
    "Use apply_patch to create every project file; do not use shell redirection",
    "or heredocs to write source files.",
    "The project must include CMakeLists.txt, a conventional Makefile, and",
    "a C source file. Both `cmake -S . -B build && cmake --build build` and",
    "`make hello` must build a runnable `hello` program whose exact standard",
    "output is `Hello, world!` followed by a newline.",
    "Add ignore rules for generated build artifacts.",
    "Use read_file to inspect the files you created, then use the terminal to",
    "build and run both build paths.",
    "Commit the completed work with the repository's configured local Git author.",
    "Do not ask questions. In your final response include the exact marker",
    "LUA_SMITH_PROJECT_COMMITTED.",
  }, " ")
  local follow_up_prompt = table.concat({
    "Read the developer review handoff now in your context.",
    "If it identifies any actionable defect, fix every valid finding and do not",
    "make unrelated changes. If it identifies no actionable defect, leave the",
    "implementation alone.",
    "In either case, verify both the CMake and Makefile build paths and their",
    "hello output. Commit any changes you make with the configured local Git",
    "author, leaving the worktree clean.",
    "In your final response include the exact marker LUA_SMITH_REVIEW_FOLLOWUP.",
  }, " ")

  run("git init --quiet " .. git_workspace, "git init")
  run("git -C " .. git_workspace .. " config user.name " ..
    shell_quote("Test Tester"), "git user.name")
  run("git -C " .. git_workspace .. " config user.email " ..
    shell_quote("info@c89.systems"), "git user.email")

  local opened_client, open_error = cai.open({
    chatgpt_auth_json = auth_path,
    timeout_ms = 180000,
  })
  client = assert_ok(opened_client, open_error, "cai.open")
  parent = checked("client:new_smith_runtime", function()
    return client:new_smith_runtime({
      workspace_directory = workspace,
      model = cai.MODEL_GPT_5_6_LUNA,
      reasoning_effort = cai.REASONING_EFFORT_LOW,
      reasoning_summary = cai.REASONING_SUMMARY_DETAILED,
      review_model = cai.MODEL_GPT_5_6_LUNA,
      review_reasoning_effort = cai.REASONING_EFFORT_MEDIUM,
      review_reasoning_summary = cai.REASONING_SUMMARY_DETAILED,
      disable_default_session_store = true,
      event_callback = event_callback(events),
    })
  end)

  checked("parent:submit build", function()
    return parent:submit(build_prompt)
  end)
  pump_until_completed(parent, 240, "Lua Smith build turn")
  assert_contains("Lua Smith build answer", event_answer(events),
    "LUA_SMITH_PROJECT_COMMITTED")
  if #events.reasoning_summaries == 0 then
    fail("Lua Smith build did not expose a provider reasoning summary")
  end
  if events.reads < 1 or events.patches < 1 or events.executions < 1 or
      events.terminal_started < 1 or events.terminal_completed < 1 then
    fail("Lua Smith build evidence is incomplete (reads=" .. tostring(events.reads) ..
      " patches=" .. tostring(events.patches) ..
      " executions=" .. tostring(events.executions) ..
      " terminal=" .. tostring(events.terminal_started) .. "/" ..
      tostring(events.terminal_completed) .. ")")
  end

  local identity = run("git -C " .. git_workspace .. " log -1 " ..
    shell_quote("--format=%an <%ae>"), "git identity")
  if identity ~= "Test Tester <info@c89.systems>\n" then
    fail("Lua Smith committed with unexpected identity: " .. identity)
  end
  if run("git -C " .. git_workspace .. " status --porcelain", "git status") ~= "" then
    fail("Lua Smith initial project left a dirty worktree")
  end
  local commit = run("git -C " .. git_workspace .. " rev-parse --verify HEAD",
    "git rev-parse"):gsub("%s+$", "")
  if commit == "" then
    fail("Lua Smith initial project has no commit")
  end

  review = checked("parent:start_review", function()
    return parent:start_review({
      target = "commit",
      commit = commit,
    })
  end)
  pump_until_completed(review, 180, "Lua Smith review turn")
  if #events.review_reports ~= 1 or events.review_reports[1] == "" then
    fail("Lua Smith review did not produce one JSON report")
  end
  checked("parent:finish_review", function()
    return parent:finish_review(review)
  end)
  review:close()
  review = nil
  checked("parent:pump review handoff", function()
    return parent:pump(0)
  end)
  if events.review_started ~= 1 or events.review_handed_off ~= 1 then
    fail("Lua Smith review handoff evidence is incomplete (started=" ..
      tostring(events.review_started) .. " handed_off=" ..
      tostring(events.review_handed_off) .. ")")
  end

  events.answer = {}
  checked("parent:submit follow-up", function()
    return parent:submit(follow_up_prompt)
  end)
  pump_until_completed(parent, 240, "Lua Smith review follow-up")
  assert_contains("Lua Smith follow-up answer", event_answer(events),
    "LUA_SMITH_REVIEW_FOLLOWUP")

  local handover = {}
  checked("parent:export_markdown", function()
    return parent:export_markdown(function(chunk)
      handover[#handover + 1] = chunk
      return true
    end)
  end)
  handover = table.concat(handover)
  assert_contains("Lua Smith handover", handover, "# CAI agent handover")
  assert_contains("Lua Smith handover", handover, "LUA_SMITH_REVIEW_FOLLOWUP")
  assert_contains("Lua Smith handover", handover,
    "<review_handoff source=\"smith-review\"")

  run("cmake -S " .. git_workspace .. " -B " ..
    shell_quote(workspace .. "/build"), "CMake configure")
  run("cmake --build " .. shell_quote(workspace .. "/build"), "CMake build")
  if run(shell_quote(workspace .. "/build/hello"), "CMake hello") ~= "Hello, world!\n" then
    fail("CMake hello did not emit the expected output")
  end
  run("make -C " .. git_workspace .. " hello", "Makefile build")
  if run(shell_quote(workspace .. "/hello"), "Makefile hello") ~= "Hello, world!\n" then
    fail("Makefile hello did not emit the expected output")
  end
  if run("git -C " .. git_workspace .. " status --porcelain", "final git status") ~= "" then
    fail("Lua Smith review follow-up left a dirty worktree")
  end

  io.stderr:write("[lua-smith-e2e] review_report=" .. events.review_reports[1] ..
    " terminal_commands=" .. tostring(events.terminal_completed) ..
    " reasoning_summaries=" .. tostring(#events.reasoning_summaries) .. "\n")
  success = true
end, debug.traceback)

local cleanup_ok, cleanup_error = pcall(cleanup)
if not cleanup_ok then
  fail("Lua Smith e2e cleanup failed: " .. tostring(cleanup_error))
end
if not ok then
  fail(err)
end
if not success then
  fail("Lua Smith e2e did not complete")
end

print("cai Lua Smith e2e passed")
