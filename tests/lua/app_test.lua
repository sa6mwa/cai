local application = require("cai.app")
assert(not application.open(false))
assert(not application.open {on_event = true})
assert(not application.open {on_replay = "not a callback"})
assert(not application.open {introduction_markdown = true})
for _, field in ipairs {
  "program_name", "invocation_name", "environment_prefix", "namespace_prefix", "queue_prefix",
  "state_directory", "cache_directory", "data_directory", "history_directory", "clipboard_directory",
  "log_directory", "pouch_key_file", "banner", "config_file", "config_directory", "introduction_markdown",
} do
  assert(not application.open {[field] = true})
  assert(not application.open {[field] = "invalid\0text"})
end
for _, margin in ipairs {"introduction_margin_left", "introduction_margin_right"} do
  for _, invalid in ipairs {-1, 1.5, true, "2", 2147483648} do
    assert(not application.open {[margin] = invalid})
  end
end
assert(not application.open {unknown = true})
assert(not application.open {program_name = "bad\0name"})
assert(not application.open {settings = {agent = {model = "bad\0model"}}})
assert(not application.open {settings = {agent = {unknown_setting = "bad"}}})
local messaging_app = assert(application.open {
  no_config = true,
  settings = {messaging = {enabled = true, enable_auto_enqueue = false, ack_timeout_ms = 300000, queue_name = "lua-mailbox"}},
})
messaging_app:close()
assert(not application.open {settings = {messaging = {enable_auto_enqueue = "not-a-bool"}}})
local app = assert(application.open {
  invocation_name = "vectis -a smith --",
  banner = "HIDDEN_BANNER",
  introduction_markdown = "HIDDEN_INTRODUCTION",
  no_config = true,
})
local named = assert(application.open {
  program_name = 'smith', invocation_name = 'vectis -a smith --',
  environment_prefix = 'VECTIS_SMITH_', namespace_prefix = 'vectis.smith', queue_prefix = 'smith',
  config_file = '/unused/config.ini', config_directory = '/unused/config',
  state_directory = '/unused/state', cache_directory = '/unused/cache', data_directory = '/unused/data',
  history_directory = '/unused/history', clipboard_directory = '/unused/clipboard',
  log_directory = '/unused/log', pouch_key_file = '/unused/key',
})
assert(named:help():find('smith and libcai', 1, true))
named:close()
assert(not application.open {program_name = '../bad'})
assert(not application.open {environment_prefix = '1BAD_'})
local list_view = assert(application.open {no_config = true, settings = {ui = {todo_layout = 'list'}}})
list_view:close()
for _, limit in ipairs {-1, 0, 20} do
  local replay_view = assert(application.open {no_config = true, settings = {ui = {replay_turns = limit}}})
  replay_view:close()
end
assert(not application.open {no_config = true, settings = {ui = {replay_turns = "invalid"}}})
assert(not application.open {no_config = true, settings = {ui = {todo_layout = 'invalid'}}})
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
assert(not text:find("HIDDEN_INTRODUCTION", 1, true))
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
