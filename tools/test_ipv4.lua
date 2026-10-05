-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- An address between four bytes and four numbers (`user/lib/ipv4.lua`), and
-- what a program that serves the network begins with (`netprogram.lua`) -
-- `httpd`, `telnetd` and `vncd`'s, which each had its own - on this
-- computer, over a `/Network` and a `/Temporary` kept here.
--
--   build/host/lua tools/test_ipv4.lua

local checks, failed = 0, 0

local function check(ok, what)
  checks = checks + 1

  if not ok then
    failed = failed + 1
    print("not ok - " .. what)
  end
end

use = use or function(path) return dofile((path:gsub("^/Kosmos/Libraries/", "user/lib/"))) end

local ipv4 = dofile("user/lib/ipv4.lua")

--------------------------------------------------------------------------
-- Addresses.
--------------------------------------------------------------------------

check(ipv4.text("\10\0\2\15") == "10.0.2.15", "four bytes as four numbers")
check(ipv4.text(nil) == "?" and ipv4.text("abc") == "?", "what is not four bytes is ?")
check(ipv4.text(nil, "") == "" and ipv4.text("\1\2\3", "none") == "none",
      "or what the caller says stands in for one")

check(ipv4.bytes("10.0.2.15") == "\10\0\2\15", "four numbers as four bytes")
check(ipv4.bytes(" 192.168.1.40 ") == "\192\168\1\40", "spaces round a field's text allowed")
check(ipv4.bytes("256.1.1.1") == nil and ipv4.bytes("1.2.3") == nil and ipv4.bytes("a.b.c.d") == nil
      and ipv4.bytes(nil) == nil, "a number over 255, three numbers, letters and nothing refused")

check(ipv4.given("\10\0\2\15") and not ipv4.given("\0\0\0\0") and not ipv4.given(nil),
      "an address given, and 0.0.0.0 not one")

local net = { address = "\192\168\1\40", netmask = "\255\255\255\0", card = true }

check(ipv4.neighbour("\192\168\1\7", net), "the same /24 is a neighbour")
check(not ipv4.neighbour("\192\168\2\7", net), "the next /24 is not")
check(not ipv4.neighbour("\192\168\1\7", { address = "\192\168\1\40" }),
      "with no mask, nobody is")
check(not ipv4.neighbour(nil, net), "nor is nobody")

--------------------------------------------------------------------------
-- A network program's scaffolding.
--------------------------------------------------------------------------

local temporary, said, registered = {}, {}, {}
local refused_closed = 0
local waiting = {}                  -- requests on the program's name

local function connection(from)
  return { from = from, close = function() refused_closed = refused_closed + 1 end }
end

local pending = {}                  -- connections the listener holds

fs = {
  net_info = function() return net end,
  listen = function(_, port) return { port = port } end,
  accept = function()
    local c = table.remove(pending, 1)
    if not c then return nil end
    return c, c.from
  end,
  getattr = function(path) return temporary[path] and { kind = "directory" } or nil end,
  send = function(path, req, cap)
    if req.type == "mkdir" then temporary[path] = true return { ok = true } end
    if req.type == "register" then registered[req.name] = cap return { ok = true } end
    return nil, "not here"
  end,
  write = function(path, value) temporary[path] = value return true end,
  read = function(path)
    if path == "/Devices/cpu" then return { counter_hz = 1000 } end
    return nil
  end,
}

local ticks = 0
local replies = {}

sys = {
  info = function() return { tick_hz = 250 } end,
  sleep = function() end,
  ticks = function() ticks = ticks + 5000 return ticks end,
  endpoint = function() return 7 end,
  receive = function(_, _) local r = table.remove(waiting, 1) if r then return r, #replies + 1 end end,
  reply = function(who, t) replies[who] = t return true end,
}

local real_print = print
print = function(text) said[#said + 1] = text end

local netprogram = dofile("user/lib/netprogram.lua")
local p = netprogram.open{ name = "telnetd", port = 23, named = true, neighbours = true }

check(p and p.listener and p.listener.port == 23, "listening on its port")
check(temporary["/Temporary/telnetd"] == true, "its folder in /Temporary made")
check(registered.telnetd == 7, "a name in /Running, on its own endpoint")
check(p:address() == "192.168.1.40", "its address as a person writes it")

p:note("10.0.2.2  connected")
check(said[#said] == "telnetd: 10.0.2.2  connected", "a note printed with its name: " .. tostring(said[#said]))
check(type(temporary["/Temporary/telnetd/log"]) == "table"
      and temporary["/Temporary/telnetd/log"][1]:match("^%s*%d+s  10%.0%.2%.2  connected$"),
      "and kept in the log with its seconds: " .. tostring((temporary["/Temporary/telnetd/log"] or {})[1]))

for i = 1, 45 do p:note("line " .. i) end
check(#temporary["/Temporary/telnetd/log"] == 40 and temporary["/Temporary/telnetd/log"][40]:find("line 45"),
      "the log is the last forty lines")

p:publish{ sessions = { { from = "192.168.1.7" } } }
check(temporary["/Temporary/telnetd/status"].state == "running"
      and temporary["/Temporary/telnetd/status"].port == 23
      and temporary["/Temporary/telnetd/status"].sessions[1].from == "192.168.1.7",
      "its status: running, its port, and what it said")
p:publish{ state = "stopped" }
check(temporary["/Temporary/telnetd/status"].state == "stopped", "a state it says is kept")

pending = { connection("\8\8\8\8"), connection("\192\168\1\7") }

do
  local conn = p:accept(1)

  check(conn == nil and refused_closed == 1 and said[#said]:find("8.8.8.8  refused, not on this network", 1, true),
        "one from outside this network closed unanswered, and noted")

  local from

  conn, from = p:accept(1)
  check(conn and from == "\192\168\1\7", "one from this network let in")
end

waiting = { { type = "disconnect", from = "192.168.1.7" }, { type = "other" } }

local asked = {}

p:disconnects(function(from) asked[#asked + 1] = from return 2 end)

check(#asked == 1 and asked[1] == "192.168.1.7", "a Disconnect handed the address it names")
check(replies[1] and replies[1].ok == true and replies[1].ended == 2, "and answered with how many ended")
check(replies[2] and replies[2].ok == false and replies[2].ended == 0,
      "anything else asked ends nothing")

-- With no card, nothing to serve on, and said.
net = { card = false }
check(netprogram.open{ name = "httpd", port = 80 } == nil
      and said[#said] == "httpd: this machine has no network card", "no card is said, and nothing opened")

-- With no address yet, waited for when asked to - and said once.
net = { address = "\0\0\0\0", netmask = "\0\0\0\0", card = true }

local tries = 0

fs.listen = function(_, port)
  tries = tries + 1
  if tries < 3 then return nil, 3 end
  net = { address = "\10\0\2\15", netmask = "\255\255\255\0", card = true }
  return { port = port }
end

local before = #said
local waited = netprogram.open{ name = "telnetd", port = 23, wait = true }

check(waited and tries == 3 and #said == before + 1
      and said[#said] == "telnetd: waiting for an address to listen on port 23",
      "no address waited for, said once, and listened on once there is one")

tries = 0

check(netprogram.open{ name = "vncd", port = 5900 } == nil
      and said[#said] == "vncd: could not listen on port 5900: 3",
      "and not waited for when not asked: " .. tostring(said[#said]))

print = real_print

if failed == 0 then
  print(("PASS: %d checks on addresses and a network program's scaffolding (four "
         .. "bytes and four numbers both ways, a neighbour by the mask, a listener "
         .. "waited for, its folder, log and status, a name in /Running, outsiders "
         .. "turned away and a Disconnect answered)."):format(checks))
else
  print(("FAIL: %d of %d checks on ipv4.lua and netprogram.lua"):format(failed, checks))
  os.exit(1)
end
