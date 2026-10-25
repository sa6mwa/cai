-- Lua embedding example. Arguments begin at index 1, without a program name.
-- This identity derives CAI_LUA_ variables and cai-lua XDG paths/namespaces.
-- No banner/introduction is supplied; a CLI exit code of 0 is truthy in Lua.
local application = require("cai.app")
local app = assert(application.open({program_name = "cai-lua"}))
local code, err = app:run(arg)
app:close()
assert(code, err)
os.exit(code)
