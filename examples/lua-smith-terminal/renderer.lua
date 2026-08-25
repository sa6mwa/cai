local M = {}

function M.new(cai, output, colors)
  local reset = colors.reset
  local gray = colors.gray
  local green = colors.green
  local magenta = colors.magenta
  local red = colors.red
  local render = {
    lines = 0,
    omitted = false,
    command = "",
    text_open = false,
    reasoning_open = false,
    suppress_review_text = false,
    review_report_visible = false,
    reasoning_heading_seen = false,
    reasoning_probe = "",
    last_reasoning_heading = "",
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
        output.write(gray, data:sub(start, finish))
        if newline then
          output.write("\n")
          render.lines = render.lines + 1
        end
        output.write(reset)
      elseif not render.omitted then
        output.write(gray, "… more output omitted\n", reset)
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
      output.write(reset, "\n")
      render.text_open = false
      render.reasoning_open = false
    end
  end

  local function render_reasoning_raw(data)
    if data == "" then
      return
    end
    if not render.reasoning_open then
      close_message()
      output.write(magenta, "Thinking: ", reset)
      render.reasoning_open = true
    end
    output.write(data)
    output.flush()
  end

  local function render_reasoning_body(data)
    data = data:gsub("^[\r\n]+", "")
    if data == "" then
      return
    end
    if not render.reasoning_open then
      output.write("  ")
      render.reasoning_open = true
    end
    output.write(data)
    output.flush()
  end

  local function render_reasoning_heading(data)
    if data == "*" then
      return false
    end
    if data:sub(1, 2) ~= "**" then
      return nil
    end
    local finish = data:find("**", 3, true)
    if not finish then
      return false
    end
    local heading = data:sub(3, finish - 1):match("^%s*(.-)%s*$")
    if heading == "" or #heading > 512 then
      return nil
    end
    if render.last_reasoning_heading ~= heading then
      close_message()
      output.write(magenta, "Thinking: ", reset, heading, "\n")
      output.flush()
      render.last_reasoning_heading = heading
    end
    render.reasoning_heading_seen = true
    return data:sub(finish + 2)
  end

  local function render_reasoning_delta(data)
    data = render.reasoning_probe .. (data or "")
    render.reasoning_probe = ""
    if data:sub(1, 1) == "*" then
      if #data == 1 or data:sub(2, 2) == "*" then
        local rest = render_reasoning_heading(data)
        if rest == false then
          if #data <= 512 then
            render.reasoning_probe = data
            return
          end
          render_reasoning_raw(data)
          return
        end
        if rest ~= nil then
          render_reasoning_body(rest)
          return
        end
      end
    end
    if render.reasoning_heading_seen then
      render_reasoning_body(data)
    else
      render_reasoning_raw(data)
    end
  end

  local function reset_reasoning_summary()
    if render.reasoning_probe ~= "" then
      render_reasoning_raw(render.reasoning_probe)
    end
    render.reasoning_probe = ""
    render.reasoning_heading_seen = false
    render.last_reasoning_heading = ""
  end

  -- Reviewer strings are model-controlled. Keep decoded control code points
  -- visible rather than allowing JSON escapes to become terminal commands.
  local function safe_terminal_text(value)
    local rendered = {}
    local index = 1
    while index <= #value do
      local byte = value:byte(index)
      local next_byte = value:byte(index + 1)
      if byte == 0xc2 and next_byte and next_byte >= 0x80 and next_byte <= 0x9f then
        rendered[#rendered + 1] = string.format("\\u00%02X", next_byte)
        index = index + 2
      elseif byte < 0x20 or (byte >= 0x7f and byte <= 0x9f) then
        rendered[#rendered + 1] = string.format("\\x%02X", byte)
        index = index + 1
      else
        rendered[#rendered + 1] = string.char(byte)
        index = index + 1
      end
    end
    return table.concat(rendered)
  end

  local function abbreviated_task(value)
    if #value <= 240 then
      return value
    end
    return value:sub(1, 240) .. "…"
  end

  local function render_review_body(body)
    local start = 1
    while start <= #body do
      local newline = body:find("\n", start, true)
      local finish = newline and newline - 1 or #body
      output.write("  ", safe_terminal_text(body:sub(start, finish)), "\n")
      if not newline then
        return
      end
      start = newline + 1
    end
  end

  local function render_review_report(data)
    local ok, report = pcall(require("lonejson").decode_json, data)
    if not ok or type(report) ~= "table" then
      output.write(red, "Reviewer report could not be displayed\n", reset)
      return
    end
    local explanation = tostring(report.overall_explanation or "")
    local findings = type(report.findings) == "table" and report.findings or {}
    if explanation ~= "" then
      output.write(safe_terminal_text(explanation), "\n")
    end
    if #findings > 0 then
      if explanation ~= "" then
        output.write("\n")
      end
      output.write(#findings > 1 and "Full review comments:\n" or "Review comment:\n")
    elseif explanation == "" then
      output.write("Review completed: ",
        safe_terminal_text(tostring(report.overall_correctness or "unknown")), ".\n")
    end
    for _, finding in ipairs(findings) do
      local location = type(finding.code_location) == "table" and finding.code_location or {}
      local range = type(location.line_range) == "table" and location.line_range or {}
      output.write("\n- ", safe_terminal_text(tostring(finding.title or "Untitled finding")), " — ",
        safe_terminal_text(tostring(location.absolute_file_path or "unknown")), ":",
        tostring(range.start or "?"), "-", tostring(range["end"] or "?"), "\n")
      render_review_body(tostring(finding.body or ""))
    end
  end

  function render.event(event)
    if event.type ~= cai.AGENT_EVENT_REASONING_SUMMARY then
      reset_reasoning_summary()
    end
    if event.type == cai.AGENT_EVENT_REASONING_SUMMARY then
      render_reasoning_delta(event.data)
    elseif event.type == cai.AGENT_EVENT_TEXT_DELTA then
      if render.suppress_review_text then
        return
      end
      if not render.text_open then
        close_message()
        output.write(green, "Smith: ", reset)
        render.text_open = true
      end
      output.write(event.data or "")
      output.flush()
    elseif event.type == cai.AGENT_EVENT_RESPONSE_COMPLETED then
      close_message()
    elseif event.type == cai.AGENT_EVENT_TERMINAL_COMMAND_STARTED then
      close_message()
      render.lines = 0
      render.omitted = false
      render.command = remembered_command(event.data or "")
      output.write("$ ", event.data or "", "\n")
    elseif event.type == cai.AGENT_EVENT_TERMINAL_OUTPUT then
      close_message()
      render_terminal_output(event)
    elseif event.type == cai.AGENT_EVENT_TERMINAL_WAITING then
      close_message()
      output.write(gray, "Waiting for terminal progress…\n", reset)
    elseif event.type == cai.AGENT_EVENT_TERMINAL_COMMAND_COMPLETED or
        event.type == cai.AGENT_EVENT_TERMINAL_COMMAND_CANCELLED then
      close_message()
      local verb = event.type == cai.AGENT_EVENT_TERMINAL_COMMAND_CANCELLED and "Cancelled" or "Ran"
      local status
      if event.terminal_exit_code ~= nil then
        status = "exit " .. tostring(event.terminal_exit_code)
      elseif event.terminal_signal ~= nil then
        status = "signal " .. tostring(event.terminal_signal)
      else
        status = "status unavailable"
      end
      output.write(string.format("%s %s (%s, %.1fs)\n", verb, render.command,
        status, (event.terminal_duration_ms or 0) / 1000))
    elseif event.type == cai.AGENT_EVENT_TURN_QUEUED then
      close_message()
      output.write(gray, "Queued next turn\n", reset)
    elseif event.type == cai.AGENT_EVENT_REVIEW_REPORT then
      close_message()
      render_review_report(event.data or "{}")
      render.review_report_visible = true
    elseif event.type == cai.AGENT_EVENT_REVIEW_HANDED_OFF then
      close_message()
      if not render.review_report_visible and event.data and #event.data > 0 then
        output.write("Review handoff:\n", event.data)
        if event.data:sub(-1) ~= "\n" then
          output.write("\n")
        end
      end
      render.review_report_visible = false
    elseif event.type == cai.AGENT_EVENT_SUBAGENT_STARTED then
      close_message()
      output.write(gray, "Starting ", reset,
        safe_terminal_text(event.subagent_name or "delegated"), gray,
        " subagent", reset)
      if event.data and #event.data > 0 then
        output.write(gray, " — task: ", reset,
          safe_terminal_text(abbreviated_task(event.data)))
      end
      output.write("\n")
    elseif event.type == cai.AGENT_EVENT_SUBAGENT_HANDED_OFF then
      close_message()
      output.write(event.subagent_name or "Subagent", " handoff:\n", event.data or "")
      if not event.data or event.data:sub(-1) ~= "\n" then
        output.write("\n")
      end
    elseif event.type == cai.AGENT_EVENT_TOOL_CALL_COMPLETED and
        event.tool_name ~= "exec_command" and event.tool_name ~= "write_stdin" and
        event.tool_action ~= cai.AGENT_TOOL_ACTION_SUBAGENT then
      close_message()
      local verbs = {
        [cai.AGENT_TOOL_ACTION_READ] = "Read",
        [cai.AGENT_TOOL_ACTION_LIST] = "Listed",
        [cai.AGENT_TOOL_ACTION_VIEW] = "Viewed",
        [cai.AGENT_TOOL_ACTION_PATCH] = "Patched",
      }
      local verb = verbs[event.tool_action]
      if verb and event.tool_path then
        output.write(gray, verb, " ", event.tool_path, "\n", reset)
      elseif verb and (event.tool_path_count or 0) > 1 then
        output.write(gray, verb, " ", tostring(event.tool_path_count), " files\n", reset)
      elseif verb then
        output.write(gray, verb, "\n", reset)
      else
        output.write(gray, "Completed ", event.tool_name or "tool", "\n", reset)
      end
    elseif event.type == cai.AGENT_EVENT_RUN_FAILED then
      close_message()
      output.write(red, "Smith failed: ", event.data or "agent run failed", "\n", reset)
    elseif event.type == cai.AGENT_EVENT_RUN_COMPLETED then
      close_message()
    end
  end

  return render
end

return M
