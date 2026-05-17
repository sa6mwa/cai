local cai = require("cai")

local client, err = cai.open()
if not client then
  error(err.message or err.status_string)
end

local agent = assert(client:new_agent({
  model = cai.MODEL_GPT_5_NANO,
  instructions = "You are a concise Lua example assistant.",
  reasoning_effort = "minimal",
}))

agent:add_user_text("Say hello from cai Lua in one short sentence.")

local ok, stream_err = agent:stream({
  response = function(chunk)
    io.write(chunk)
    io.flush()
    return true
  end,
  response_suffix = "\n",
})

if not ok then
  error(stream_err.message or stream_err.status_string)
end

local usage = agent:last_usage()
if usage then
  io.stderr:write(string.format(
    "[usage] input=%d cached=%d output=%d reasoning=%d total=%d\n",
    usage.input_tokens,
    usage.input_cached_tokens,
    usage.output_tokens,
    usage.output_reasoning_tokens,
    usage.total_tokens))
end

agent:close()
client:close()
