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

  --
  -- Writes that waited more than *two* periods.
  --
  -- Waiting one period is not a fault - it is this client being correctly
  -- throttled by a device that consumes at its own rate, and counting those
  -- measured nothing but "the ring is full", which it should be. Two is
  -- where waiting stops being pacing and starts being a stall.
  --
  if took > 11600 then over = over + 1 end
end

local wall = (sys.ticks() - began) // us
out:close()

local after = sys.info() or {}
local audio_ms = N * (fmt.period // 4) * 1000 // fmt.rate

print(("audiolag: %d periods, %d ms of audio in %d ms wall")
      :format(N, audio_ms, wall // 1000))
print(("audiolag: UNDERRUNS %d   worst write %d us   stalls over two periods %d")
      :format((after.audio_dry or 0) - dry0, worst, over))
print(("audiolag: queue floor %d of %d periods")
      :format(after.audio_floor or 0, fmt.periods))

--
-- How the device drains cannot be sampled from here.
--
-- It was tried: `sys.sound_queued` is guarded by `owns_audio`, which this
-- process does not have and should not - the device belongs to one server
-- and that is what makes per-application volume possible at all. A client
-- asking always reads zero, so the probe measured its own lack of
-- permission and reported "never".
--
-- The measurement is still the one worth having, and it belongs in the
-- audio server, reported out with `streams`. Written down here because the
-- next person to want it will reach for exactly the call that does not work.
--
