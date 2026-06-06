local M = {}

function M.client_config(cai, env_name)
  local value, err = cai.load_dotenv_api_key(cai.DEFAULT_DOTENV_PATH, env_name)
  if value ~= nil and value ~= "" then
    return { api_key = value }
  end
  if type(err) == "table" and err.status_string == "cancelled" then
    return {}
  end
  if err ~= nil then
    error(err.message or err.status_string or tostring(err), 2)
  end
  return {}
end

return M
