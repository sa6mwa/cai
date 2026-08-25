local root = (... and ... ~= "") and ... or "."
package.path = root .. "/examples/lua-smith-terminal/?.lua;" .. package.path

local renderer = require("renderer")

local function assert_contains(text, needle, label)
  if not text:find(needle, 1, true) then
    error((label or "output") .. ": missing [" .. needle .. "] in [" .. text .. "]")
  end
end

local function assert_not_contains(text, needle, label)
  if text:find(needle, 1, true) then
    error((label or "output") .. ": unexpectedly found [" .. needle .. "] in [" .. text .. "]")
  end
end

local cai = {
  AGENT_EVENT_TEXT_DELTA = 1,
  AGENT_EVENT_TERMINAL_COMMAND_STARTED = 12,
  AGENT_EVENT_TERMINAL_OUTPUT = 13,
  AGENT_EVENT_TERMINAL_WAITING = 14,
  AGENT_EVENT_TERMINAL_COMMAND_COMPLETED = 15,
  AGENT_EVENT_TERMINAL_COMMAND_CANCELLED = 16,
  AGENT_EVENT_TURN_QUEUED = 17,
  AGENT_EVENT_REVIEW_REPORT = 18,
  AGENT_EVENT_REVIEW_HANDED_OFF = 20,
  AGENT_EVENT_REASONING_SUMMARY = 21,
  AGENT_EVENT_RESPONSE_COMPLETED = 22,
  AGENT_EVENT_SUBAGENT_STARTED = 24,
  AGENT_EVENT_TOOL_CALL_COMPLETED = 5,
  AGENT_EVENT_RUN_FAILED = 10,
  AGENT_EVENT_RUN_COMPLETED = 9,
  AGENT_TOOL_ACTION_READ = 1,
  AGENT_TOOL_ACTION_LIST = 2,
  AGENT_TOOL_ACTION_VIEW = 3,
  AGENT_TOOL_ACTION_PATCH = 4,
}

local chunks = {}
local output = {
  write = function(...)
    for i = 1, select("#", ...) do
      chunks[#chunks + 1] = tostring(select(i, ...))
    end
  end,
  flush = function()
  end,
}
local colors = {
  reset = "<reset>",
  gray = "<gray>",
  green = "<green>",
  magenta = "<magenta>",
  red = "<red>",
}
local render = renderer.new(cai, output, colors)
local report = [[{"findings":[{"title":"Use stable state","body":"Avoid raw JSON.","confidence_score":0.9,"priority":2,"code_location":{"absolute_file_path":"/tmp/project/a.c","line_range":{"start":7,"end":7}}}],"overall_correctness":"incorrect","overall_explanation":"One actionable issue.","overall_confidence_score":0.9}]]

render.event({ type = cai.AGENT_EVENT_REASONING_SUMMARY, data = "**Planning " })
render.event({ type = cai.AGENT_EVENT_REASONING_SUMMARY,
  data = "files**\nInspecting the workspace." })
render.event({ type = cai.AGENT_EVENT_REASONING_SUMMARY, data = "**Planning files**" })
render.event({ type = cai.AGENT_EVENT_TERMINAL_COMMAND_STARTED, data = "pwd" })
render.event({ type = cai.AGENT_EVENT_TERMINAL_OUTPUT, data = "/tmp/project\n" })
render.event({ type = cai.AGENT_EVENT_TERMINAL_COMMAND_COMPLETED,
  terminal_exit_code = 0, terminal_duration_ms = 0 })
render.event({ type = cai.AGENT_EVENT_TOOL_CALL_COMPLETED, tool_name = "read_file",
  tool_action = cai.AGENT_TOOL_ACTION_READ, tool_path = "/tmp/project/a.c" })
render.event({ type = cai.AGENT_EVENT_SUBAGENT_STARTED, subagent_name = "review",
  data = "Review changes against trunk.\27[31m" })
render.event({ type = cai.AGENT_EVENT_REVIEW_REPORT, data = report })
render.event({ type = cai.AGENT_EVENT_REVIEW_HANDED_OFF, data = report })

local rendered = table.concat(chunks)
assert_contains(rendered, "<magenta>Thinking: <reset>Planning files\n", "heading")
assert_contains(rendered, "  Inspecting the workspace.", "reasoning body")
assert_not_contains(rendered, "**Planning files**", "raw heading")
assert_contains(rendered, "$ pwd\n", "terminal start")
assert_contains(rendered, "Ran pwd (exit 0, 0.0s)\n", "terminal completion")
assert_contains(rendered, "Read /tmp/project/a.c\n", "semantic tool receipt")
assert_contains(rendered, "Starting <reset>review<gray> subagent<reset><gray> — task: " ..
  "<reset>Review changes against trunk.\\x1B[31m\n", "subagent task")
assert_contains(rendered, "One actionable issue.\n", "review explanation")
assert_contains(rendered, "- Use stable state — /tmp/project/a.c:7-7\n", "review finding")
assert_contains(rendered, "  Avoid raw JSON.\n", "review body")
assert_not_contains(rendered, "\"overall_correctness\"", "review raw JSON")

print("lua Smith terminal renderer fixture passed")
