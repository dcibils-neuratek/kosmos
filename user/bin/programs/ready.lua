-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: needs network
--
-- ready: say to another machine that this one has booted and is ready.
--
--   ready 192.168.1.38:47700           one datagram there, once up
--   ready 192.168.1.38:47700 desktop   and only once the desktop is too
--
-- Started by init from the command line's `opt/kosmos/ready=ADDRESS:PORT`
-- (Diego, 10 October 2026: "a small script that when kosmos boots it send a
-- small message to the mac over udp or tcp to let it know the kosmos is
-- booted and ready"). A machine on network boot comes back with whatever
-- the Mac serves, and the Mac had to keep asking over Telnet whether it was
-- back yet - and could not tell a machine that had restarted from one that
-- never went away. This says so, the moment it is true, with the build:
--
--   kosmos ready 0.11.136 build 3f1c2a0 up 14.2 s desktop
--
-- **Ready** is the network up - this datagram leaving is the proof - and,
-- on a boot to the desktop, the window manager answering with a window
-- open. Sent three times a second apart, since a datagram may be lost and
-- the listener counts it once (`tools/kosmos_telnet.py`). Through the
-- namespace's `fs.udp`, as `udp` sends, so this holds no capability of the
-- network's own.
--

local ipv4 = use("/Kosmos/Libraries/ipv4.lua")

local words = {}

for w in tostring(args or ""):gmatch("%S+") do words[#words + 1] = w end

local host, port = tostring(words[1] or ""):match("^([%d%.]+):(%d+)$")
local to = host and ipv4.bytes(host)

port = tonumber(port)

if not to or not port or port < 1 or port > 65535 then
  print("ready: ADDRESS:PORT [desktop] - an address of four numbers and a port")
  return
end

local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
local tick_hz = (sys.info() or {}).tick_hz or 250

-- The desktop, when this boot is for one: the window manager answering
-- with a window - the Deskbar's, nearly always - open. Asked every half
-- second, for two minutes at most; then said without it, since a machine
-- whose desktop did not come is still worth hearing from.
local desktop = "no desktop"

if words[2] == "desktop" then
  local wm = use("/Kosmos/Libraries/wmproto.lua")
  local deadline = sys.ticks() + 120 * hz

  desktop = "desktop not up"

  while sys.ticks() < deadline do
    local list = wm.windows()

    if list and list.windows and #list.windows > 0 then
      desktop = "desktop"
      break
    end

    sys.sleep(tick_hz // 2)
  end
end

--
-- **And the network with an address**, which on a real machine comes later
-- than the desktop: the M700 asked its router and had none yet at 1.9 s -
-- "no route to it: this machine has no address" - where a test's machine
-- is given one on its command line (10 October). Tried once a second until
-- a datagram leaves, for two minutes, saying why once if it cannot.
--
local b = sys.build()
local sock, said_why = nil, nil
local deadline = sys.ticks() + 120 * hz
local text

while true do
  local code, why

  sock, code, why = fs.udp("/Network", 0)

  if sock then
    text = ("kosmos ready %s build %s up %.1f s %s"):format(
      b.version or "?", b.build or "?", sys.ticks() / hz, desktop)

    if sock:send(to, port, text) then break end

    why = "it would not leave"
    sock:close()
    sock = nil
  end

  why = tostring(why or code)

  if why ~= said_why then
    print("ready: not yet - " .. why)
    said_why = why
  end

  if sys.ticks() >= deadline then
    print("ready: gave up after two minutes")
    return
  end

  sys.sleep(tick_hz)
end

for _ = 2, 3 do
  sys.sleep(tick_hz)
  sock:send(to, port, text)
end

print(("ready: said to %s:%d - %s"):format(host, port, text))

-- A moment for the last to leave before the port is let go with it waiting.
sys.sleep(tick_hz // 10)
sock:close()
