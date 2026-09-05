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
--
-- **Several at a time, without threads.** This system has none - there is no
-- thread syscall, and a process has one `lua_State`, so two threads inside
-- it would want a lock around the whole interpreter and would take turns
-- anyway. What it has instead is what nginx has: **one process, an event
-- loop, and a coroutine per connection**, resumed when that connection has
-- something. `fs.poll` is what makes it possible and is the `select` this
-- system had wanted six separate times.
--
-- So a slow client no longer blocks a fast one. What bounds it now is
-- `NET_CONN_MAX` - sixteen slots, one of them the listener - rather than the
-- shape of the loop.

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

--
-- Bytes onto a connection, in pieces the ring can take.
--
-- `conn:write` returns how many bytes fitted, which is the whole point of
-- the ring being finite: a page larger than 16 KB does not fit at once and a
-- server that ignored the answer would send the first 16 KB of every image.
--
-- **Never more than a ring's worth at a time**, and that is about memory
-- rather than about the wire. The first version passed `text:sub(at)` - the
-- whole remainder - on every pass, so a hundred-kilobyte file allocated a
-- hundred kilobytes, then ninety, then seventy-four, per connection per
-- pass. Six at once ran the heap out and two conversations died with "not
-- enough memory" while the other four were served. `conn:write` was only
-- ever going to take what fitted, so everything past the first ring's worth
-- of each of those strings was built to be thrown away.
--
local CHUNK = 16384

local function send(conn, text)
  local at = 1

  while at <= #text do
    local wrote = conn:write(text:sub(at, at + CHUNK - 1))

    if wrote > 0 then
      at = at + wrote
    elseif conn:closed() then
      return false
    else
      --
      -- The ring is full and the far end has not caught up.
      --
      -- **A yield, not a wait.** `conn:wait` blocks this whole process,
      -- which was fine when it served one request at a time and is exactly
      -- wrong now: a large file being sent to a slow client would stop every
      -- other conversation until it drained. Yielding hands control back to
      -- the loop, which waits on all of them at once and comes back here
      -- when *this* connection has room.
      --
      -- And it says *which* room it is waiting for, because the loop has to
      -- pass that on: a connection watched for arriving bytes is never
      -- reported ready for a server that is only trying to write.
      --
      coroutine.yield("write")
    end
  end

  return true
end

local function head_for(code, kind, length)
  return table.concat({
    status_line(code),
    "Server: Kosmos",
    "Content-Type: " .. kind,
    "Content-Length: " .. length,
    "Connection: close",
    "", "",
  }, "\r\n")
end

-- An answer whose body is a sentence: the errors, and nothing else.
local function respond(conn, code, kind, body)
  return send(conn, head_for(code, kind, #body)) and send(conn, body)
end

--
-- And an answer whose body is a file, which is never held.
--
-- `fs.chunks` walks it a message at a time, so what this process holds is
-- one piece rather than the whole thing - which is what lets six of these
-- run at once on a two-megabyte heap. `fs.read` would fetch it in exactly
-- the same pieces and then concatenate them, which is the one line that
-- undoes the streaming; `init.lua` says so where `chunks` is defined.
--
-- The length comes from `getattr` rather than from counting, because the
-- header has to go out before the body is read.
--
local function respond_file(conn, path, kind, length, with_body)
  if not send(conn, head_for(200, kind, length)) then
    return false
  end

  if not with_body then
    return true
  end

  for piece in fs.chunks(path) do
    if not send(conn, piece) then
      return false
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

--------------------------------------------------------------------------
-- One conversation, as a coroutine.
--
-- Written as though it had the connection to itself - read the request,
-- find the file, write the answer - and every place it would have waited is
-- a `yield` instead. The loop below resumes it when its connection has
-- something, so the straight-line shape survives and nothing here is a state
-- machine.
--
-- That is the whole argument for coroutines over callbacks, and it is the
-- same one `roadmap.md` makes about SSH's channels: the code that reads like
-- what it does is the code that can be checked against what it should do.
--------------------------------------------------------------------------

local function serve(conn, from)
  --
  -- The request, up to the blank line that ends its headers.
  --
  -- Bounded, because a client that never sends one would otherwise hold a
  -- slot for ever - the oldest denial of service there is, and one counter
  -- to avoid. It holds a *slot* now rather than the whole server, which is
  -- what the coroutines bought.
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
      tries = tries + 1
      coroutine.yield("read")
    end
  end

  local method, target = request:match("^(%u+)%s+(%S+)")

  if not method then
    respond(conn, 400, "text/plain", "the request was not a request\n")
    return
  end

  if method ~= "GET" and method ~= "HEAD" then
    respond(conn, 400, "text/plain", method .. " is not served here\n")
    return
  end

  local path = resolve(target)

  if not path then
    -- Said as a refusal rather than as a miss, because they are different
    -- facts and a log that conflates them hides the interesting one.
    refused = refused + 1
    respond(conn, 403, "text/plain", "that path is not allowed\n")
    note(("%s  %s %s -> 403"):format(dotted(from), method, target))
    return
  end

  local attrs = fs.getattr(path)

  if not attrs or attrs.kind == "directory" then
    refused = refused + 1
    respond(conn, 404, "text/html",
            "<html><body><h1>404</h1><p>" .. target
            .. " is not here.</p></body></html>\n")
    note(("%s  %s %s -> 404"):format(dotted(from), method, target))
    return
  end

  -- HEAD is the same answer without the body, which is what makes it HEAD
  -- rather than a different request.
  served = served + 1
  respond_file(conn, path, content_type(path), attrs.size or 0,
               method ~= "HEAD")
  note(("%s  %s %s -> 200, %d bytes"):format(dotted(from), method, target,
                                             attrs.size or 0))
end

--------------------------------------------------------------------------
-- The loop.
--------------------------------------------------------------------------

-- What is in flight: the connection, the coroutine serving it, who it is
-- from, and which of the two things that coroutine is waiting for.
local live = {}

while true do
  --
  -- One wait for everything.
  --
  -- The listener and every live connection, in one call - so this process is
  -- blocked rather than running between events, and wakes for whichever of
  -- them has something. A loop that polled each in turn would be a spin with
  -- as many steps as there are connections.
  --
  -- Sorted into the two lists by what each coroutine last asked for. That is
  -- the whole of why a coroutine yields a word: the loop cannot see where
  -- inside a conversation it stopped, so the conversation says.
  --
  local reading, writing = {}, {}

  for i = 1, #live do
    local entry = live[i]

    if entry.want == "write" then
      writing[#writing + 1] = entry.conn
    else
      reading[#reading + 1] = entry.conn
    end
  end

  local ready, arrived = fs.poll("/net", reading, writing, listener, 25)

  if not ready then
    note("httpd: poll: " .. tostring(arrived))
    break
  end

  --
  -- Somebody new. Accepted *before* the ready connections are served,
  -- because a connection waiting to be accepted is a client that has already
  -- been waiting longer than one being read from.
  --
  if arrived and #live < 12 then
    -- With a deadline, because `poll` saying somebody arrived and this
    -- message reaching the stack are two moments, and a reset in between
    -- would otherwise park this loop for good.
    local conn, from = fs.accept("/net", listener, 25)

    if conn then
      -- Waiting to read, because that is where `serve` begins: on a
      -- request nobody has sent yet.
      live[#live + 1] = {
        conn = conn,
        from = from,
        want = "read",
        run  = coroutine.create(function() serve(conn, from) end),
      }
    end
  end

  --
  -- And whichever have something. Resumed rather than called: each picks up
  -- where it yielded, which is inside whatever `read` or `write` was waiting.
  --
  for _, c in ipairs(ready) do
    for at = #live, 1, -1 do
      local entry = live[at]

      if entry.conn == c then
        -- What comes back is either what it yielded - "read" or "write" -
        -- or, when it raised, why.
        local ok, word = coroutine.resume(entry.run)

        if ok then
          entry.want = word
        else
          -- A conversation that raised takes itself down and nothing else.
          -- That is the point of one coroutine each: this server has no
          -- shared state a broken request could leave wrong.
          note(("%s  error: %s"):format(dotted(entry.from), tostring(word)))
        end

        if not ok or coroutine.status(entry.run) == "dead" then
          entry.conn:close()
          table.remove(live, at)
          publish("running")
        end

        break
      end
    end
  end

  if fs.interrupted and fs.interrupted("/dev/console") then
    note("stopping")
    break
  end
end

for _, entry in ipairs(live) do entry.conn:close() end

publish("stopped")
