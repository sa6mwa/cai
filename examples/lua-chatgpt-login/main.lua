local cai = require("cai")

local auth_json = os.getenv("CAI_CHATGPT_AUTH_JSON")
local issuer = nil
local requested_port = cai.CHATGPT_AUTH_DEFAULT_CALLBACK_PORT
local browser_command = nil
local open_browser = true

local function usage()
  io.stderr:write(
    "usage: lua examples/lua-chatgpt-login/main.lua [--auth-json <path>] " ..
    "[--port <port>] [--issuer <url>] [--browser-command <cmd>] " ..
    "[--no-open-browser]\n\n" ..
    "  --auth-json <path>    Codex-style auth.json path to write; default is cai's XDG auth path.\n" ..
    "  --port <port>         Local callback port, default 1455.\n" ..
    "  --issuer <url>        OAuth issuer, default https://auth.openai.com.\n" ..
    "  --browser-command <cmd>\n" ..
    "                         Browser opener command; default is platform selected.\n" ..
    "  --no-open-browser     Print the URL without launching a browser.\n\n" ..
    "CAI_CHATGPT_AUTH_JSON can override the default auth-json path.\n")
end

local function parse_port(text)
  local n = tonumber(text)
  if not n or n < 1 or n > 65535 or math.floor(n) ~= n then
    return nil
  end
  return n
end

local i = 1
while i <= #arg do
  if arg[i] == "--auth-json" then
    if not arg[i + 1] or arg[i + 1] == "" then
      io.stderr:write("--auth-json requires a path\n")
      os.exit(2)
    end
    auth_json = arg[i + 1]
    i = i + 2
  elseif arg[i] == "--issuer" then
    if not arg[i + 1] or arg[i + 1] == "" then
      io.stderr:write("--issuer requires a URL\n")
      os.exit(2)
    end
    issuer = arg[i + 1]
    i = i + 2
  elseif arg[i] == "--port" then
    local port = arg[i + 1] and parse_port(arg[i + 1])
    if not port then
      io.stderr:write("--port requires a valid TCP port\n")
      os.exit(2)
    end
    requested_port = port
    i = i + 2
  elseif arg[i] == "--browser-command" then
    if not arg[i + 1] or arg[i + 1] == "" then
      io.stderr:write("--browser-command requires a command\n")
      os.exit(2)
    end
    browser_command = arg[i + 1]
    i = i + 2
  elseif arg[i] == "--no-open-browser" then
    open_browser = false
    i = i + 1
  elseif arg[i] == "--help" or arg[i] == "-h" then
    usage()
    os.exit(0)
  else
    io.stderr:write("unknown argument: " .. tostring(arg[i]) .. "\n")
    usage()
    os.exit(2)
  end
end

local ok_socket, socket = pcall(require, "socket")
if not ok_socket then
  io.stderr:write("lua-chatgpt-login requires LuaSocket for the example HTTP listener\n")
  io.stderr:write("Install LuaSocket separately, or use the C chatgpt-login example.\n")
  os.exit(2)
end

local function bind_loopback(port)
  local server, err = socket.bind("127.0.0.1", port)
  if not server then
    return nil, err
  end
  server:settimeout(nil)
  local _, bound_port = server:getsockname()
  return server, bound_port
end

local server, port = bind_loopback(requested_port)
if not server and requested_port == cai.CHATGPT_AUTH_DEFAULT_CALLBACK_PORT then
  server, port = bind_loopback(cai.CHATGPT_AUTH_FALLBACK_CALLBACK_PORT)
end
if not server then
  io.stderr:write("listen failed on localhost: " .. tostring(port) .. "\n")
  os.exit(1)
end

local redirect_uri = "http://localhost:" .. tostring(port) ..
  cai.CHATGPT_AUTH_DEFAULT_CALLBACK_PATH
local login, authorize_or_err = cai.chatgpt_login({
  auth_json_path = auth_json,
  redirect_uri = redirect_uri,
  issuer = issuer,
})
if not login then
  io.stderr:write("cai.chatgpt_login failed: " ..
    (type(authorize_or_err) == "table" and
      (authorize_or_err.message or authorize_or_err.status_string) or
      tostring(authorize_or_err)) .. "\n")
  os.exit(1)
end
local authorize_url = authorize_or_err

io.stderr:write("Open this URL to authenticate:\n" .. authorize_url .. "\n\n")
io.stderr:write("Waiting for OAuth callback on " .. redirect_uri .. "\n")
if open_browser then
  local launched, launch_err = cai.chatgpt_login_open_browser(authorize_url, {
    command = browser_command,
  })
  if not launched then
    io.stderr:write("Could not launch browser; open the URL manually.\n")
    if type(launch_err) == "table" and launch_err.message then
      io.stderr:write("detail: " .. launch_err.message .. "\n")
    end
  end
end

local function read_request(client)
  client:settimeout(10)
  local line = client:receive("*l")
  if not line then
    return nil, nil
  end
  local method, target = line:match("^(%S+)%s+(%S+)%s+HTTP/%d%.%d\r?$")
  while true do
    local header = client:receive("*l")
    if not header or header == "" or header == "\r" then
      break
    end
  end
  return method, target
end

local function status_text(status)
  if status == 200 then return "OK" end
  if status == 400 then return "Bad Request" end
  if status == 404 then return "Not Found" end
  if status == 405 then return "Method Not Allowed" end
  return "Internal Server Error"
end

local function write_response(client, response)
  local body = response.body or ""
  local content_type = response.content_type or "text/plain; charset=utf-8"
  local status = response.status or 500
  client:send("HTTP/1.1 " .. tostring(status) .. " " .. status_text(status) .. "\r\n")
  client:send("Content-Type: " .. content_type .. "\r\n")
  client:send("Content-Length: " .. tostring(#body) .. "\r\n")
  client:send("Connection: close\r\n\r\n")
  client:send(body)
end

local exit_code = 1
while true do
  local client = server:accept()
  if client then
    local method, target = read_request(client)
    local response
    if not method or not target then
      response = {
        status = 400,
        content_type = "text/plain; charset=utf-8",
        body = "Bad Request\n",
        completed = false,
      }
    else
      local err
      response, err = login:handle_callback({ method = method, target = target })
      if not response then
        io.stderr:write("OAuth callback failed: " ..
          (type(err) == "table" and (err.message or err.status_string) or tostring(err)) ..
          "\n")
        response = {
          status = 500,
          content_type = "text/plain; charset=utf-8",
          body = "OAuth callback failed\n",
          completed = true,
        }
      end
    end
    write_response(client, response)
    client:close()
    if login:completed() then
      local path = auth_json
      if not path or path == "" then
        path = cai.chatgpt_auth_default_path()
      end
      io.stderr:write("ChatGPT auth saved to " .. tostring(path) .. "\n")
      exit_code = 0
      break
    end
    if response.completed then
      break
    end
  end
end

login:close()
server:close()
os.exit(exit_code)
