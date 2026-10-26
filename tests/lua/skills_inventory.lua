local cai = require("cai")
local store = require("cai_native_todo_store_test")
local client = assert(cai.open {api_key = "fixture", logger_disabled = true})
local runtime = assert(client:new_smith_runtime {
  workspace_directory = arg[2],
  session_store = assert(store.new_agent_session()),
  skills_directory = arg[1],
  disable_terminal = true,
  logger_disabled = true,
})
local skills = assert(runtime:skills())
assert(#skills == 3)
local names = {}
for _, skill in ipairs(skills) do
  names[skill.name] = skill.description
end
assert(names["alpha-skill"] == "First loaded description.")
assert(names["beta_skill"]:find("UTF-8 åäö.", 1, true))
assert(names["odd|_*[]"] == "Literal punctuation.")
skills[1].name = "changed by host"
for _, skill in ipairs(assert(runtime:skills())) do
  assert(skill.name ~= "changed by host")
end
runtime:close()
client:close()
