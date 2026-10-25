local logger, log_file
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
  config_directory = os.getenv("CAI_TEST_APP_CONFIG_DIRECTORY"),
  state_directory = os.getenv("CAI_TEST_APP_STATE_DIRECTORY"),
  cache_directory = os.getenv("CAI_TEST_APP_CACHE_DIRECTORY"),
  data_directory = os.getenv("CAI_TEST_APP_DATA_DIRECTORY"),
  banner = os.getenv("CAI_TEST_APP_BANNER"),
  introduction_markdown = os.getenv("CAI_TEST_APP_INTRODUCTION"),
  introduction_margin_left = tonumber(os.getenv("CAI_TEST_APP_INTRODUCTION_LEFT")),
  introduction_margin_right = tonumber(os.getenv("CAI_TEST_APP_INTRODUCTION_RIGHT")),
  logger = logger,
  settings = {agent = {model = os.getenv("CAI_TEST_APP_MODEL")},
              ui = {theme = os.getenv("CAI_TEST_APP_THEME")}},
})
local code, err = app:run(arg)
assert(code, err)
if os.getenv("CAI_TEST_APP_REPEAT") then code, err = app:run(arg) end
app:close()
if logger then
  logger:info("HOST_LOGGER_STILL_OPEN")
  logger:close()
  log_file:flush()
end
assert(code, err)
os.exit(code)
