-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- What is on the network, by name (`docs/sharing.md` N11c).
--
--   mdns browse _smb._tcp           who shares files here, after three seconds
--   mdns browse _smb._tcp 10        after ten
--   mdns resolve studio.local       a .local name's address
--
-- Through `/Discovery`, `mdnsd`, which asks the network and keeps what it
-- says (`user/lib/mdns.lua`). `browse` asks, waits, and lists what has come
-- - each instance with its host, port and address once those have come too.
--

local mdns = use("/Kosmos/Libraries/mdns.lua")
local ipv4 = use("/Kosmos/Libraries/ipv4.lua")

local words = {}

for w in tostring(args or ""):gmatch("%S+") do words[#words + 1] = w end

local verb, what = words[1], words[2]

if verb == "browse" and what then
  local seconds = tonumber(words[3]) or 3
  local count, why = mdns.browse(what)

  if not count then
    print("mdns: " .. tostring(why))
    return
  end

  -- Asked again as it waits, so the browse keeps going and what has come
  -- since is counted; done early once every instance has its address.
  local deadline = sys.ticks() + seconds * (sys.info().counter_hz or 62500000)
  local list = {}

  repeat
    sys.sleep(25)
    mdns.browse(what)
    list = mdns.instances(what)

    local complete = #list > 0

    for _, i in ipairs(list) do complete = complete and i.address ~= nil end

    if complete and sys.ticks() > deadline - (seconds - 1) * (sys.info().counter_hz or 62500000) then
      break
    end
  until sys.ticks() >= deadline

  print(("mdns: %d of %s"):format(#list, what))

  for _, i in ipairs(list) do
    print(("mdns: %s  %s:%s  %s"):format(i.name, i.host or "?", tostring(i.port or "?"),
                                         i.address and ipv4.text(i.address) or "?"))
  end

  return
end

if verb == "resolve" and what then
  local address, why = mdns.resolve(what, 750)

  print(address and ("mdns: %s is %s"):format(what, ipv4.text(address))
        or ("mdns: %s: %s"):format(what, tostring(why)))
  return
end

print("mdns: browse TYPE [SECONDS] | resolve NAME.local")
