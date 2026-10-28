local cai = require("cai")
local objective = assert(cai.review_and_fix_objective(".", {target="uncommitted"}))
assert(objective:find("User-authorized review/fix loop", 1, true))
assert(objective:find("target=uncommitted", 1, true))
objective = assert(cai.review_and_fix_objective(".", {base_branch="HEAD"}))
assert(objective:find("Review changes against base HEAD", 1, true))
assert(not cai.review_and_fix_objective(".", {target="base"}))
assert(not cai.review_and_fix_objective(".", {target="uncommitted", base_branch="HEAD"}))
assert(not cai.review_and_fix_objective(".", {commit="deadbeef"}))
assert(not pcall(cai.review_and_fix_objective, ".", {target="custom"}))
local client = assert(cai.open{api_key="fixture", base_url="http://127.0.0.1:1/v1"})
for _, policy in ipairs{"disable_goals", "disable_review_subagent"} do
  local config = {workspace_directory=".", enable_review_and_fix=true,
                  disable_default_session_store=true, disable_chrome=true}
  config[policy] = true
  local runtime, err = client:new_smith_runtime(config)
  assert(not runtime and err, "Lua must forward the opt-in and reject missing prerequisites")
end
client:close()
