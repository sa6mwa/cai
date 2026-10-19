local app = assert(require("cai.app").open {
  program_name = "vectis -a smith --",
  banner = os.getenv("CAI_TEST_APP_BANNER"),
  settings = {agent = {model = os.getenv("CAI_TEST_APP_MODEL")}},
})
local code, err = app:run(arg)
assert(code, err)
if os.getenv("CAI_TEST_APP_REPEAT") then code, err = app:run(arg) end
app:close()
assert(code, err)
os.exit(code)
