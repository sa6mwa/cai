-- Lua embedding example. Arguments begin at index 1, without a program name.
local application = require("cai.app")
local app = assert(application.open({program_name = "cai-lua"}))
local code, err = app:run(arg)
app:close()
assert(code, err)
os.exit(code)
