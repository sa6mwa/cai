local cai = require("cai")
local function ok(value, err)
  assert(value, err and err.message or "operation failed")
  return value
end
local client = ok(cai.open {
  api_key="fixture", base_url="http://127.0.0.1:1/v1", metadata_provider="custom",
  model_metadata_json='{"models":[{"provider":"custom","id":"fixture","reasoning_efforts":["low","medium","high","xhigh"]}]}',
})
local runtime = ok(client:new_smith_runtime {
  workspace_directory=assert(arg[1]), model="fixture", disable_default_session_store=true,
  disable_terminal=true, disable_chrome=true,
  reasoning_tool={enabled=true, normal_effort="low", high_effort="xhigh"},
})
local config = ok(runtime:reasoning_tool())
assert(config.enabled and config.normal_effort == "low" and config.high_effort == "xhigh")
config = ok(runtime:reasoning_tool {enabled=false, normal_effort="medium"})
assert(not config.enabled and config.normal_effort == "medium" and config.high_effort == "xhigh")
config = ok(runtime:reasoning_tool {enabled=true, high_effort="high"})
assert(config.enabled and config.high_effort == "high")
local value, err = runtime:reasoning_tool {high_effort="invalid"}
assert(not value and err)
config = ok(runtime:reasoning_tool())
assert(config.high_effort == "high")
runtime:close(); client:close()
