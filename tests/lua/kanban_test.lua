local cai = require('cai')
local application = require('cai.app')
local root = assert(arg[1])
local settings = {state_directory = root, lockd = 'pouch://' .. root .. '/pouch'}
assert(not application.kanban(false))
assert(not application.kanban {unknown = true})
assert(not application.kanban {default_board = 'bad\0board'})
assert(not application.kanban {max_result_items = -1})
local a = assert(application.kanban(settings))
local b = assert(application.kanban(settings))
assert(a:get_board() == 'default' and b:get_board() == 'default')
local function call(board, json)
  local chunks = {}
  assert(board:call(json, function(bytes) chunks[#chunks+1] = bytes end))
  return table.concat(chunks)
end
assert(call(a, '{"operation":"add_item","title":"SHARED FROM LUA"}'):find('DEF-001', 1, true))
assert(call(b, '{"operation":"snapshot_board"}'):find('SHARED FROM LUA', 1, true))
assert(a:select('lua-selected', true))
assert(a:get_board() == 'lua-selected' and b:get_board() == 'default')
assert(not a:select('missing'))
assert(a:get_board() == 'lua-selected')
local header, chunks = false, {}
local response = assert(b:handle_http {
  resource = 'board.json',
  begin = function(status, content_type)
    assert(status == 200 and content_type:find('application/json', 1, true))
    header = true
    assert(not b:close()) -- callback reentry cannot destroy live state
  end,
  write = function(bytes)
    assert(header)
    chunks[#chunks+1] = bytes
  end,
})
assert(response.status == 200 and #chunks > 1)
assert(table.concat(chunks):find('SHARED FROM LUA', 1, true))
assert(not b:handle_http {write = function() error('sink failure') end})
assert(b:get_board() == 'default')
assert(b:handle_http {method='HEAD', resource='board.json', write=function() error('HEAD body') end}.status == 200)
assert(b:handle_http {resource='missing', write=function() end}.status == 404)
assert(b:handle_http {method='POST', write=function() end}.status == 405)
assert(not b:call('{broken', function() end))
assert(not b:call('{"operation":"help"}', function() return false end))
local segments = {'{"operation":', '"list_board"}', false}
assert(b:call(function() return table.remove(segments, 1) or nil end, function() end))
assert(not b:call(function() return 123 end, function() end))
a:close(); a:close(); b:close()
assert(not a:get_board())
local core = assert(cai.todo {store_path=root..'/core.json', lock_path=root..'/core.lock'})
assert(core:get_board() == 'default')
assert(call(core, '{"operation":"add_item","title":"CORE"}'):find('DEF-001', 1, true))
core:close()
local pslog = require('pslog')
local logger = pslog.new_json {level='warn', output='stderr'}
settings.logger = logger
local logged = assert(application.kanban(settings))
logged:close(); collectgarbage('collect')
logger:warn('HOST LOGGER STILL AVAILABLE'); logger:close()
assert(not application.kanban(settings))
print('kanban Lua facade passed')
-- Runtime controls use the same public selection mechanism without inference.
local client = assert(cai.open {api_key='fixture'})
local runtime = assert(client:new_smith_runtime {
  workspace_directory=root, disable_default_session_store=true,
  disable_chrome=true, disable_web_search=true,
  enable_todo=true, todo={store_path=root..'/runtime.json', lock_path=root..'/runtime.lock'},
})
assert(runtime:todo_board() == 'default')
assert(runtime:select_board('runtime-board', true))
assert(runtime:todo_board() == 'runtime-board')
assert(not runtime:select_board('does-not-exist'))
assert(runtime:todo_board() == 'runtime-board')
runtime:close(); client:close()
