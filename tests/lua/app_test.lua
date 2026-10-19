local application = require("cai.app")
assert(not application.open(false))
assert(not application.open {unknown = true})
assert(not application.open {program_name = "bad\0name"})
assert(not application.open {settings = {agent = {model = "bad\0model"}}})
assert(not application.open {settings = {agent = {unknown_setting = "bad"}}})
local app = assert(application.open {
  program_name = "vectis -a smith --",
  banner = "HIDDEN_BANNER",
  no_config = true,
})
local options, model, resume = app:options()
assert(#options > 50)
for _, option in ipairs(options) do
  assert(option.long_flag:match("^%-%-") and option.description)
  if option.long_flag == "--model" then model = option end
  if option.long_flag == "--resume" then resume = option end
end
assert(model.short_flag == "m" and model.type == "string")
assert(resume.type == "optional_string")
local text = assert(app:help())
assert(text:find("Usage: vectis -a smith -- [options]", 1, true))
assert(not text:find("HIDDEN_BANNER", 1, true))
assert(app:run {"--help"} == 0)
assert(app:run {"--version"} == 0)
assert(app:run {"--unknown"} == 2)
assert(not app:run {false})
assert(not app:run {"--help\0junk"})
assert(not app:run(false))
app:close()
app:close()
assert(not app:run {})
assert(not app:help())
local pslog = require("pslog")
local lua_sink = pslog.new_json {output = function() error("must not run from a CAI worker") end}
assert(not application.open {logger = lua_sink})
lua_sink:close()
local logger = pslog.new_json {level = "warn", output = "stderr"}
local owned = assert(application.open {logger = logger, no_config = true})
assert(owned:run {"--unknown"} == 2)
owned:close()
logger:warn("HOST_LOGGER_STILL_OPEN")
logger:close()
assert(not application.open {logger = logger})
collectgarbage("collect")
