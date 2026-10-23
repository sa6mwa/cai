local cai = require("cai")
assert(cai.review_default_base(arg[1]) == "refs/heads/trunk")
assert(not cai.review_default_base(arg[1] .. "/missing"))
local client = assert(cai.open {
  api_key = "fixture", base_url = os.getenv("CAI_TEST_REVIEW_ENDPOINT"),
  logger_disabled = true,
})
local function review(request)
  local reports, completed = 0, false
  local runtime = assert(client:new_smith_review_runtime {
    workspace_directory = arg[1], disable_default_session_store = true,
    model = "gpt-5.6-luna", enable_image_generation = false,
    event_callback = function(event)
      if event.type == 18 then reports = reports + 1 end
      if event.type == 9 then completed = true end
      assert(event.type ~= 10, event.data)
    end,
  })
  if request then assert(runtime:submit_review(request))
  else assert(runtime:submit_review()) end
  while not completed do assert(runtime:pump(1000)) end
  assert(runtime:state() == "completed" and reports == 1)
  runtime:close()
end
review()
review {target = "uncommitted"}
review {base_branch = "HEAD"}
review {instructions = "Custom Lua review scope"}
local parent = assert(client:new_smith_runtime {
  workspace_directory = arg[1], disable_default_session_store = true,
  model = "gpt-5.6-luna", enable_image_generation = false,
})
local child = assert(parent:start_review())
while child:state() ~= "completed" do assert(child:pump(1000)) end
assert(parent:finish_review(child))
child:close()
local chunks = {}
assert(parent:export_markdown(function(chunk)
  chunks[#chunks + 1] = chunk
  return true
end))
assert(table.concat(chunks):find("Fix parser", 1, true))
parent:close()
client:close()
