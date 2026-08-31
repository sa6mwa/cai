local function readable_file(path)
  local file

  if path == nil or path == "" then
    return false
  end
  file = io.open(path, "rb")
  if file == nil then
    return false
  end
  file:close()
  return true
end

local function chatgpt_auth_path(cai)
  local configured = os.getenv("CAI_CHATGPT_AUTH_JSON")
  local default_path
  local err

  if configured ~= nil and configured ~= "" then
    if readable_file(configured) then
      return configured
    end
    error("configured ChatGPT auth file is unavailable: " .. configured, 0)
  end

  default_path, err = cai.chatgpt_auth_default_path()
  if default_path == nil or default_path == "" then
    error("ChatGPT auth path is unavailable: " .. tostring(err), 0)
  end
  if readable_file(default_path) then
    return default_path
  end
  error("CAI ChatGPT auth file is unavailable: " .. default_path ..
    "; run `make chatgpt-login` or set CAI_CHATGPT_AUTH_JSON", 0)
end

return {
  require_live_auth = function(cai, enable_env)
    if os.getenv(enable_env) ~= "1" then
      error("set " .. enable_env .. "=1 to run this Lua ChatGPT e2e", 0)
    end
    return chatgpt_auth_path(cai)
  end,
}
