-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Where the time goes when a period is handed to the audio server.
--
-- An instrument rather than a test: it plays a hundred periods of silence
-- and reports how many of the writes came back at once and how long the
-- rest waited. The number that matters is the last one - if the mean wait
-- is a whole tick, the pipeline is running at the tick rate instead of the
-- device rate, which is exactly the fault that made a beep cost 63% of the
-- machine and still play at two thirds speed.
local audio = use("/lib/audio.lua")

local fmt = audio.format()
if fmt.period == 0 then print("audiolag: no sound device") return end

local hz = (fs.read("/dev/cpu") or {}).counter_hz or 62500000
local us = hz // 1000000

local chunk = string.rep("\0", fmt.period)
local out = assert(audio.open("audiolag"))

--
-- Deadlines, not throughput.
--
-- The old version of this counted how long the writes took and how much
-- audio came out, and both were the wrong question: a stream can deliver a
-- full second of sound every second and still have a hole in it, which
-- measures perfect and sounds broken. What matters is whether any period
-- reached the device after the device had already run out.
--
local N = 400                        -- about 2.3 seconds

local before = sys.info() or {}
local dry0 = before.audio_dry or 0

local chunk = string.rep("\0", fmt.period)
local out = assert(audio.open("audiolag"))

local hz = (fs.read("/dev/cpu") or {}).counter_hz or 62500000
local us = hz // 1000000

local worst, over = 0, 0
local began = sys.ticks()

for _ = 1, N do
  local t0 = sys.ticks()

  if not out:write(chunk) then break end

  local took = (sys.ticks() - t0) // us

  if took > worst then worst = took end

  -- A period is 5800 us of sound. A write that took longer than that is a
  -- write the device was waiting on.
  if took > 5800 then over = over + 1 end
end

local wall = (sys.ticks() - began) // us
out:close()

local after = sys.info() or {}
local audio_ms = N * (fmt.period // 4) * 1000 // fmt.rate

print(("audiolag: %d periods, %d ms of audio in %d ms wall")
      :format(N, audio_ms, wall // 1000))
print(("audiolag: UNDERRUNS %d   worst write %d us   writes over one period %d")
      :format((after.audio_dry or 0) - dry0, worst, over))
print(("audiolag: queue floor %d of %d periods")
      :format(after.audio_floor or 0, fmt.periods))
