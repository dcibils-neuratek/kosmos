-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: needs network
--
-- netspeed: how fast bytes cross the network, measured.
--
--   netspeed get 192.168.1.38 47701         read until the far end closes
--   netspeed send 192.168.1.38 47702 100    write 100 MB, then close
--
-- The far end is `tools/netspeed_peer.py` on the Mac, which serves bytes on
-- 47701 and swallows them on 47702, and says its own time. Nothing is kept
-- here: what is read is let go, so the number is the network and the stack
-- rather than a disk (Diego, 10 October 2026: "can you measure how much it
-- takes to send a 100 mb to kosmos from the mac?"). The roadmap's network
-- card is measured with it.
--
-- Through the namespace's `fs.connect`, as `fetch` connects.
--

local ipv4 = use("/Kosmos/Libraries/ipv4.lua")

local words = {}

for w in tostring(args or ""):gmatch("%S+") do words[#words + 1] = w end

local verb, host, port, mb = words[1], words[2], tonumber(words[3]), tonumber(words[4])
local to = host and ipv4.bytes(host)

if not (verb == "get" or (verb == "send" and mb)) or not to or not port then
  print("netspeed: get ADDRESS PORT | send ADDRESS PORT MB")
  return
end

local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
local conn, why, said = fs.connect("/Network", to, port)

if not conn then
  print("netspeed: " .. tostring(said or why))
  return
end

local started = sys.ticks()
local bytes = 0

if verb == "get" then
  while true do
    local piece = conn:read()

    if piece and #piece > 0 then
      bytes = bytes + #piece
    elseif conn:closed() then
      break
    else
      conn:wait(25)
    end
  end
else
  local chunk = string.rep("kosmos-netspeed-", 4096)        -- 64 KB
  local want = math.floor(mb * 1024 * 1024)

  while bytes < want do
    local wrote = conn:write(chunk:sub(1, math.min(#chunk, want - bytes)))

    if wrote and wrote > 0 then
      bytes = bytes + wrote
    elseif conn:closed() then
      break
    else
      conn:wait(25)
    end
  end
end

local took = (sys.ticks() - started) / hz

conn:close()

print(("netspeed: %s %.1f MB in %.2f s, %.1f MB/s"):format(
      verb == "get" and "received" or "sent", bytes / 1048576, took,
      took > 0 and bytes / 1048576 / took or 0))
