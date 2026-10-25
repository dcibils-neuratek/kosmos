-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: needs bluetooth
-- Bluetooth, at the prompt (`bluetooth.md` B4).
--
--   bt                    the adapter, and every device heard so far
--   bt scan [seconds]     look for five seconds, or as many as said, and
--                         say each device as it is first heard
--   bt stop               stop looking
--
-- The smallest thing that uses `/Devices/bluetooth` end to end, through
-- `bluetooth.lua`, as `midi` is for MIDI. It is `bt` rather than
-- `bluetooth` so that the name typed at the prompt opens the app.

local bt = use("/Kosmos/Libraries/bluetooth.lua")

local words = {}

for _, w in ipairs(use("/Kosmos/Libraries/files.lua").words(args)) do words[#words + 1] = w end

local tick_hz = (sys.info() or {}).tick_hz or 250

local function say_state(s)
  if not s.present then
    print("bt: no adapter ready")
    return
  end

  local can = s.classic and s.le and "Classic and Low Energy"
              or s.le and "Low Energy only" or "Classic only"

  print(("bt: %s adapter, Bluetooth %s from %s, %s: %s%s; %s"):format(
    s.bus == "usb" and ("USB port " .. s.port) or s.bus, s.version, s.maker,
    s.address, can, s.ssp and ", SSP" or "",
    s.scanning and "looking" or "not looking"))
end

local function say_device(d)
  print(("bt:   %-24s %-18s %-7s %s  %s"):format(
    d.name ~= "" and d.name or "(no name yet)", d.kind, d.transport,
    d.address, d.rssi and (d.rssi .. " dBm") or "no RSSI"))
end

local s, why = bt.state()

if not s then
  print("bt: " .. tostring(why))
  return
end

if words[1] == nil then
  say_state(s)

  local all = bt.devices()

  print(("bt: %d %s heard"):format(#all, #all == 1 and "device" or "devices"))

  for _, d in ipairs(all) do say_device(d) end
elseif words[1] == "scan" then
  local seconds = tonumber(words[2] or "5") or 5
  local said = {}

  s, why = bt.scan(math.floor(seconds * 1000))

  if not s then
    print("bt: " .. tostring(why))
    return
  end

  say_state(s)

  -- Asked again four times a second; a device said when first heard, and
  -- again when the name it did not have at first arrives.
  for _ = 1, math.max(1, math.floor(seconds * 4)) do
    sys.sleep(math.max(1, tick_hz // 4))

    for _, d in ipairs(bt.devices()) do
      local key = d.address .. d.transport

      if said[key] == nil or (said[key] == "" and d.name ~= "") then
        said[key] = d.name
        say_device(d)
      end
    end
  end

  bt.stop()

  local all = bt.devices()

  print(("bt: done, %d %s heard"):format(#all, #all == 1 and "device" or "devices"))
elseif words[1] == "stop" then
  s, why = bt.stop()
  print(s and "bt: stopped" or ("bt: " .. tostring(why)))
else
  print("bt: say `bt`, `bt scan [seconds]` or `bt stop`")
end
