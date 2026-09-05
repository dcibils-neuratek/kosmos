-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: needs network
-- Serves files over HTTP.
--
--   httpd                  /home/www on port 80
--   httpd 8080             the same, on port 8080
--   httpd 8080 /home/site  and from there
--
-- **The thing this machine has been building towards without saying so.**
-- `roadmap.md` records an HTTP server as the goal that would settle an
-- argument rather than as a feature: it is the second consumer of the driver
-- model, and it is the first program here that answers a request instead of
-- making one. `accept` exists because of this program.
--
-- Static files only, and that is the whole of it. No CGI, no directory
-- listing that executes anything, no ranges, no keep-alive. What it does is
-- read a path, find a file, and send it - which is what serving a page is,
-- and everything else is a different program.
--
-- **HTTP/1.0 with `Connection: close`.** 1.1 would mean keeping the
-- connection open and then knowing when a request ended, which is
-- `Content-Length` on the way *in* and a state machine to go with it. Closing
-- after each answer is slower and is one thing to get right; `fetch` on this
-- machine speaks the same version for the same reason.

local words = {}

for w in tostring(args or ""):gmatch("%S+") do words[#words + 1] = w end

local port = tonumber(words[1]) or 80
local root = words[2] or "/home/www"

local function dotted(bytes)
  if type(bytes) ~= "string" or #bytes ~= 4 then return "?" end

  return ("%d.%d.%d.%d"):format(bytes:byte(1, 4))
end

local info = fs.net_info("/net")

if not info or not info.card then
  print("httpd: this machine has no network card")
  return
end

--------------------------------------------------------------------------
-- What this server is doing, where something else can read it.
--
-- **In `/data` rather than printed, because a manager cannot read a
-- console.** The desktop launches this as a process of its own and its
-- output goes wherever that process's console goes, which is not a window.
-- So the state and the log are *written*, and `webserver` reads them - the
-- same arrangement any service manager has with any service, and the reason
-- daemons have log files rather than shouting.
--
-- `/data` and not `/home`: ramfs is always there, a disk is not, and a log
-- that vanishes when the machine stops is the right lifetime for a log
-- about what the machine did while it was running.
--------------------------------------------------------------------------

local STATUS = "/data/httpd/status"
local LOG    = "/data/httpd/log"

--
-- The last forty lines and no more.
--
-- Bounded because appending means reading the whole thing back, adding to
-- it and writing it out - which is quadratic, and a server that has answered
-- ten thousand requests would spend its time rewriting its own diary. Forty
-- is what fits in a window.
--
local LOG_LINES = 40
local log = {}

local served, refused = 0, 0

local function note(line)
  log[#log + 1] = line

  while #log > LOG_LINES do table.remove(log, 1) end

  print(line)
  fs.write(LOG, log)
end

local function publish(state)
  fs.write(STATUS, {
    state   = state,
    port    = port,
    root    = root,
    address = dotted(info.address),
    served  = served,
    refused = refused,
  })
end

--
-- What a browser should be told a file is.
--
-- Short on purpose, and the same argument `filetypes.lua` makes: an entry
-- here is a claim about a format, and a wrong one is a page that renders as
-- text or an image that downloads. What is not here is `application/octet-
-- stream`, which is the honest answer for anything else.
--
local TYPES = {
  html = "text/html",
  htm  = "text/html",
  css  = "text/css",
  js   = "text/javascript",
  txt  = "text/plain",
  md   = "text/plain",
  png  = "image/png",
  jpg  = "image/jpeg",
  jpeg = "image/jpeg",
  gif  = "image/gif",
  ico  = "image/x-icon",
  pdf  = "application/pdf",
  wav  = "audio/wav",
  mp3  = "audio/mpeg",
}

local function content_type(path)
  local ext = path:match("%.([%w]+)$")

  return (ext and TYPES[ext:lower()]) or "application/octet-stream"
end

--
-- A path from a request, made safe.
--
-- **This is the security-relevant line in the file** and it is worth being
-- explicit about why. A request for `/../../home/.startup` is a request to
-- read outside the directory being served, and every static server that has
-- ever been written wrong has been written wrong here. So: the query string
-- is cut, `..` is refused outright rather than resolved, and the result must
-- still begin with the root.
--
-- Refusing rather than normalising is deliberate. A normaliser has to agree
-- with the filesystem about what a path means - about `//`, about `.`, about
-- a trailing slash - and disagreeing by one case is the bug. Nothing
-- legitimate asks for `..`.
--
local function resolve(target)
  local path = target:match("^([^?#]*)") or "/"

  if path == "" or path == "/" then path = "/index.html" end

  if path:find("%.%.") then return nil end
  if path:sub(1, 1) ~= "/" then return nil end

  local full = root .. path

  if full:sub(1, #root) ~= root then return nil end

  return full
end

local function status_line(code)
  local said = ({ [200] = "OK", [400] = "Bad Request", [403] = "Forbidden",
                  [404] = "Not Found",
                  [500] = "Internal Server Error" })[code] or "Error"

  return ("HTTP/1.0 %d %s"):format(code, said)
end

local function respond(conn, code, kind, body)
  local head = table.concat({
    status_line(code),
    "Server: Kosmos",
    "Content-Type: " .. kind,
    "Content-Length: " .. #body,
    "Connection: close",
    "", "",
  }, "\r\n")

  --
  -- Written in pieces the ring can take.
  --
  -- `conn:write` returns how many bytes fitted, which is the whole point of
  -- the ring being finite: a page larger than 16 KB does not fit at once and
  -- a server that ignored the answer would send the first 16 KB of every
  -- image. So this loops, and `conn:wait` is what blocks until the far end
  -- has acknowledged enough for more to fit.
  --
  local text = head .. body
  local at = 1

  while at <= #text do
    local wrote = conn:write(text:sub(at))

    if wrote > 0 then
      at = at + wrote
    elseif conn:closed() then
      return false
    else
      -- The ring is full and the far end has not caught up. Waiting is not
      -- optional here: a loop that retried immediately would be a spin.
      conn:wait(5)
    end
  end

  return true
end

--------------------------------------------------------------------------

local listener, why = fs.listen("/net", port)

if not listener then
  print("httpd: could not listen on port " .. port .. ": " .. tostring(why))
  return
end

note(("serving %s on %s port %d"):format(root, dotted(info.address), port))
print("Control-C to stop.")
publish("running")

while true do
  local conn, from = fs.accept("/net", listener)

  if not conn then
    print("httpd: accept: " .. tostring(from))
    break
  end

  --
  -- The request, up to the blank line that ends its headers.
  --
  -- Bounded, because a client that never sends one would otherwise hold
  -- this server for ever - which is the oldest denial of service there is
  -- and costs one counter to avoid. Nothing here is asynchronous: one
  -- request is served at a time, and that is a real limitation written down
  -- rather than hidden. What it costs is a slow client blocking a fast one;
  -- what it saves is a state machine per connection.
  --
  local request = ""
  local tries = 0

  while not request:find("\r\n\r\n") and tries < 200 do
    local piece = conn:read()

    if piece then
      request = request .. piece
    elseif conn:closed() then
      break
    else
      conn:wait(5)
      tries = tries + 1
    end
  end

  local method, target = request:match("^(%u+)%s+(%S+)")

  if not method then
    respond(conn, 400, "text/plain", "the request was not a request\n")
  elseif method ~= "GET" and method ~= "HEAD" then
    respond(conn, 400, "text/plain", method .. " is not served here\n")
  else
    local path = resolve(target)

    if not path then
      -- Said as a refusal rather than as a miss, because they are different
      -- facts and a log that conflates them hides the interesting one.
      respond(conn, 403, "text/plain", "that path is not allowed\n")
      refused = refused + 1
      note(("%s  %s %s -> 403"):format(dotted(from), method, target))
    else
      local attrs = fs.getattr(path)

      if not attrs or attrs.kind == "directory" then
        respond(conn, 404, "text/html",
                "<html><body><h1>404</h1><p>" .. target
                .. " is not here.</p></body></html>\n")
        refused = refused + 1
        note(("%s  %s %s -> 404"):format(dotted(from), method, target))
      else
        local body = fs.read(path)

        if type(body) ~= "string" then
          respond(conn, 500, "text/plain", "could not read it\n")
          refused = refused + 1
          note(("%s  %s %s -> 500"):format(dotted(from), method, target))
        else
          -- HEAD is the same answer without the body, which is what makes it
          -- HEAD rather than a different request.
          respond(conn, 200, content_type(path),
                  (method == "HEAD") and "" or body)
          served = served + 1
          note(("%s  %s %s -> 200, %d bytes")
               :format(dotted(from), method, target, #body))
        end
      end
    end
  end

  conn:close()
  publish("running")

  if fs.interrupted and fs.interrupted("/dev/console") then
    note("stopping")
    break
  end
end

publish("stopped")
