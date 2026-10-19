local logger, log_file
if os.getenv("CAI_TEST_APP_LOG_PATH") then
  log_file = assert(io.open(os.getenv("CAI_TEST_APP_LOG_PATH"), "a"))
  logger = require("pslog").new_json {output = log_file, level = "trace"}
end
local app = assert(require("cai.app").open {
  program_name = "vectis -a smith --",
  banner = os.getenv("CAI_TEST_APP_BANNER"),
  logger = logger,
  settings = {agent = {model = os.getenv("CAI_TEST_APP_MODEL")}},
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
