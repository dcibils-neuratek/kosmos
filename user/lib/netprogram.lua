-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- What a program that serves the network needs before it serves anything.
--
--   local netprogram = use("/Kosmos/Libraries/netprogram.lua")
--   local p = netprogram.open{ name = "telnetd", port = 23, wait = true,
--                              named = true, neighbours = true }
--   if not p then return end            -- it has said why
--
--   p.listener, p.port, p.info          what `fs.listen` and `/Network` said
--   p:note("10.0.2.2  connected")       a line in its log, and printed
--   p:publish{ sessions = list }        its state, for the Servers window
--   local conn, from = p:accept(1)      a connection, from a neighbour
--   p:disconnects(function(from) ... return how_many end)
--
-- `httpd`, `telnetd` and `vncd` each began the same way: is there a card,
-- listen, a folder in `/Temporary` holding a status and the last forty
-- lines of a log for the Servers window to read, a name in `/Running` for
-- its Disconnect, the subnet rule, and the Disconnect answered - with
-- `neighbour` byte for byte the same in two of them. Each of those is here
-- once, and each program keeps only what it serves.
--
-- **In `/Temporary` rather than printed, because a manager cannot read a
-- console** (`httpd` said it first): the desktop starts these as processes
-- of their own, and their output goes wherever that process's console goes,
-- which is not a window. `/Temporary` and not `/Home`, since a log about
-- what the machine did while it ran has the machine's lifetime.

local ipv4 = use("/Kosmos/Libraries/ipv4.lua")
local files = use("/Kosmos/Libraries/files.lua")

local netprogram = {}

-- The last forty lines of a log and no more: appending means reading the
-- whole back and writing it out, and forty is what fits in the window.
local LOG_LINES = 40

local methods = {}
methods.__index = methods

--
-- **And for whoever shows them** - the Servers window, and the Deskbar's
-- icons beside the bell (Diego, 8 October: "a screen sharing status symbol
-- near the notifications bell") - the two questions about one of these
-- programs, answered once:
--
--   netprogram.running("vncd")    its process's id, or nil when it is not
--   netprogram.status("vncd")     what it last published, or nil when it
--                                 is not running - a file left by one that
--                                 has stopped says nothing
--
function netprogram.running(program)
  for _, p in ipairs(sys.processes() or {}) do
    if p.name == program and not p.exited then return p.id end
  end

  return nil
end

function netprogram.status(program)
  if not netprogram.running(program) then return nil end

  local s = fs.read("/Temporary/" .. program .. "/status")

  return type(s) == "table" and s or {}
end

--
-- **Listening, and waiting for an address if asked to** (`testing.md`
-- 18.352). A stick and a network boot start `telnetd` before DHCP has
-- answered, and with no address the stack refuses to listen - so it said
-- "could not listen on port 23: 3" and was gone a second after it started.
-- With `wait`, a refusal while the machine has no address is waited out, a
-- second at a time, said once; any other refusal is said and ends it.
--
local function listen(name, port, info, wait)
  local tick_hz = (sys.info() or {}).tick_hz or 250
  local listener, why = fs.listen("/Network", port)
  local waited = false

  while not listener do
    info = fs.net_info("/Network") or info

    if not wait or ipv4.given(info.address) then
      print(("%s: could not listen on port %d: %s"):format(name, port, tostring(why)))
      return nil
    end

    if not waited then
      print(("%s: waiting for an address to listen on port %d"):format(name, port))
      waited = true
    end

    sys.sleep(tick_hz)
    listener, why = fs.listen("/Network", port)
  end

  return listener, fs.net_info("/Network") or info
end

--
-- `spec`: `name`, the program's, which its folder in `/Temporary` and its
-- name in `/Running` are; `port`; `wait`, for an address before listening;
-- `named`, for a name in `/Running` the Servers window's Disconnect asks;
-- and `neighbours`, to let in only this machine's own network. Nil when
-- there is nothing to serve on, having printed why.
--
function netprogram.open(spec)
  local name, port = spec.name, spec.port
  local info = fs.net_info("/Network")

  if not info or not info.card then
    print(name .. ": this machine has no network card")
    return nil
  end

  local listener

  listener, info = listen(name, port, info, spec.wait)

  if not listener then return nil end

  local dir = "/Temporary/" .. name

  files.make_folder(dir)

  local self = setmetatable({
    name = name, port = port, listener = listener, info = info,
    neighbours = spec.neighbours,
    status = dir .. "/status", log = dir .. "/log", lines = {}, heard = {},
    counter_hz = math.max(1, ((fs.read("/Devices/cpu") or {}).counter_hz or 1)),
  }, methods)

  --
  -- **A name in `/Running`, for the Servers window's Disconnect**: a
  -- request `{ type = "disconnect", from = "<address>" }` on it ends that
  -- address's connections. Tables, since this is a program's own name
  -- rather than a server's wire (`CLAUDE.md`, a declared shape).
  --
  if spec.named then
    self.control = sys.endpoint()

    if self.control then
      fs.send("/Running", { type = "register", name = name }, self.control)
    end
  end

  return self
end

-- This machine's address as a person writes it; DHCP's, so it may change,
-- and asked afresh.
function methods:address()
  return ipv4.text((fs.net_info("/Network") or self.info).address)
end

-- Whether `from` is on this machine's own network, as it is now.
function methods:neighbour(from)
  return ipv4.neighbour(from, fs.net_info("/Network") or self.info)
end

-- A line in the log, with the seconds since the machine started, and
-- printed with the program's name.
function methods:note(text)
  local lines = self.lines

  lines[#lines + 1] = ("%5ds  %s"):format(sys.ticks() // self.counter_hz, text)

  while #lines > LOG_LINES do table.remove(lines, 1) end

  fs.write(self.log, lines)
  print(self.name .. ": " .. text)
end

--
-- **Someone connected, said to the person at the machine** (Diego, 7
-- October: "vnc server should notify when a new connection is made from a
-- client so the user knows someone connected", and the same of telnetd): a
-- notification - `title`, `body`, an alert when `alert` - the first time an
-- address connects, and again only after it has been quiet for ten minutes.
-- Once a session rather than once a connection, because a script driving
-- this machine over Telnet connects for every command it sends, and a
-- banner each time would bury what it is for. True when it was said.
--
local QUIET_SECONDS = 600

function methods:tell_connected(from, title, body, alert)
  local key = ipv4.text(from)
  local now = sys.ticks()
  local last = self.heard[key]

  self.heard[key] = now

  if last and (now - last) < QUIET_SECONDS * self.counter_hz then return false end

  use("/Kosmos/Libraries/notify.lua").post{ title = title, body = body,
                                           alert = alert or nil }
  return true
end

-- What it is doing, written whole: `fields`, with its port, and its state
-- - "running" unless `fields` says otherwise.
function methods:publish(fields)
  local out = { state = "running", port = self.port }

  for k, v in pairs(fields or {}) do out[k] = v end

  fs.write(self.status, out)
end

--
-- **The next connection waiting**, within `wait_ticks`: conn and from, or
-- nil. One from outside this machine's own network, when only neighbours
-- are let in, is closed unanswered and noted - nothing beyond the router
-- can make one.
--
function methods:accept(wait_ticks)
  local conn, from = fs.accept("/Network", self.listener, wait_ticks)

  if conn and self.neighbours and not self:neighbour(from) then
    self:note(ipv4.text(from) .. "  refused, not on this network")
    conn:close()
    return nil
  end

  return conn, from
end

--
-- **Every Disconnect waiting**, each answered: `ended(from)` ends that
-- address's connections and says how many, and the Servers window is told
-- `{ ok, ended }`. Anything else asked of the name ends none.
--
function methods:disconnects(ended)
  while self.control do
    local req, who = sys.receive(self.control, true)

    if not req then return end

    local n = 0

    if type(req) == "table" and req.type == "disconnect" then
      n = ended(tostring(req.from)) or 0
    end

    pcall(sys.reply, who, { ok = n > 0, ended = n })
  end
end

return netprogram
