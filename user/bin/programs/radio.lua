-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A radio station played at the prompt (`docs/flight.md` F9).
--
--   radio http://10.0.2.2:8000/station.mp3          for a minute
--   radio https://example.org/stream.mp3 30         for thirty seconds
--
-- **The Radio library's measure** (`/Kosmos/Libraries/radio.lua`): every
-- line of what plays a station is the library's, and this only says where
-- and for how long, and prints what the station is doing once a second -
-- the step, the bytes come, the frames heard and the times the player went
-- dry - which is what a suite holds it to.

local radio = use("/Kosmos/Libraries/radio.lua")
local words = use("/Kosmos/Libraries/files.lua").words(args)
local address, seconds = words[1], tonumber(words[2]) or 60

if not address then
  print("radio <address> [seconds]")
  print("  an MP3 station over HTTP or HTTPS, for instance: radio http://10.0.2.2:8000/station.mp3 20")
  return
end

local r, why = radio.open(address, { name = "radio" })
if not r then
  print("radio: " .. tostring(why))
  return
end

local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
local tick_hz = (fs.read("/Devices/kernel") or {}).tick_hz or 250
local began = sys.ticks()
local said = began
print("radio: tuned to " .. address)

while true do
  r:tick()
  local now = sys.ticks()
  if now - said >= hz then
    said = now
    local s = r:state()
    print(("radio: %s status=%d got=%d frames=%d dry=%d refused=%d connects=%d%s")
          :format(s.step, s.status, s.got, s.frames, s.dry, s.refused, s.connects,
                  s.why and ("  " .. s.why) or ""))
  end
  if now - began >= seconds * hz then break end
  sys.sleep(math.max(1, tick_hz // 100))
end

local s = r:state()
r:close()
print(("radio: done after %d s, %d bytes, %d frames heard, %d dry")
      :format((sys.ticks() - began) // hz, s.got, s.frames, s.dry))
