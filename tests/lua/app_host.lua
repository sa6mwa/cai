local logger, log_file
local replay_file
local execution_file
if os.getenv("CAI_TEST_APP_EXECUTION_PATH") then
  execution_file = assert(io.open(os.getenv("CAI_TEST_APP_EXECUTION_PATH"), "a"))
end
if os.getenv("CAI_TEST_APP_REPLAY_PATH") then
  replay_file = assert(io.open(os.getenv("CAI_TEST_APP_REPLAY_PATH"), "w"))
end
if os.getenv("CAI_TEST_APP_LOG_PATH") then
  log_file = assert(io.open(os.getenv("CAI_TEST_APP_LOG_PATH"), "a"))
  logger = require("pslog").new_json {output = log_file, level = "trace"}
end
local app = assert(require("cai.app").open {
  invocation_name = "vectis -a smith --",
  program_name = os.getenv("CAI_TEST_APP_NAME"),
  environment_prefix = os.getenv("CAI_TEST_APP_ENVIRONMENT_PREFIX"),
  namespace_prefix = os.getenv("CAI_TEST_APP_NAMESPACE_PREFIX"),
  queue_prefix = os.getenv("CAI_TEST_APP_QUEUE_PREFIX"),
  config_file = os.getenv("CAI_TEST_APP_CONFIG_FILE"),
  home_directory = os.getenv("CAI_TEST_APP_HOME_DIRECTORY"),
  runtime_directory = os.getenv("CAI_TEST_APP_RUNTIME_DIRECTORY"),
  config_directory = os.getenv("CAI_TEST_APP_CONFIG_DIRECTORY"),
  state_directory = os.getenv("CAI_TEST_APP_STATE_DIRECTORY"),
  cache_directory = os.getenv("CAI_TEST_APP_CACHE_DIRECTORY"),
  data_directory = os.getenv("CAI_TEST_APP_DATA_DIRECTORY"),
  banner = os.getenv("CAI_TEST_APP_BANNER"),
  introduction_markdown = os.getenv("CAI_TEST_APP_INTRODUCTION"),
  introduction_margin_left = tonumber(os.getenv("CAI_TEST_APP_INTRODUCTION_LEFT")),
  introduction_margin_right = tonumber(os.getenv("CAI_TEST_APP_INTRODUCTION_RIGHT")),
  logger = logger,
  on_event = execution_file and function(event)
    local cai = require("cai")
    if event.type == cai.AGENT_EVENT_EXECUTION_RECORD then
      local record = assert(cai.execution_record(event.data))
      assert(record.session_id and record.model and record.base_url)
      execution_file:write(event.data, "\n")
    end
  end or nil,
  on_replay = replay_file and function(event)
    replay_file:write(event.sequence, " ", event.type, "\n")
  end or nil,
  settings = {agent = {model = os.getenv("CAI_TEST_APP_MODEL")},
              ui = {theme = os.getenv("CAI_TEST_APP_THEME"),
                    temporary = os.getenv("CAI_TEST_APP_TEMPORARY"),
                    replay_turns = tonumber(os.getenv("CAI_TEST_APP_REPLAY_TURNS"))}},
})
local code, err = app:run(arg)
assert(code, err)
if os.getenv("CAI_TEST_APP_REPEAT") then code, err = app:run(arg) end
app:close()
if replay_file then replay_file:close() end
if execution_file then execution_file:close() end
if logger then
  logger:info("HOST_LOGGER_STILL_OPEN")
  logger:close()
  log_file:flush()
end
assert(code, err)
os.exit(code)
