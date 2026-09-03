-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A tone, and the first thing this machine ever said out loud.
-- kosmos: needs audio
--
--   beep                 440 Hz for a third of a second
--   beep 880             that pitch
--   beep 880 1000        that pitch, that many milliseconds
--
-- Deliberately a *program* and not an application: it makes a noise and
-- exits, which is what the console is for. What it is really for is being
-- the smallest thing that exercises the whole path - `sys.sound` to the
-- syscall to `hal_snd_write` to the virtqueue - so that when something
-- larger is silent, this says whether the silence starts above or below it.
--
-- It also prints how close it came to the deadline, which is the number
-- `roadmap.md` M11a promises instead of a bound.

local info = sys.info()

if not info or (info.audio_period or 0) == 0 then
  print("beep: this machine has no sound device")
  return
end

local RATE     = info.audio_rate
local CHANNELS = info.audio_channels
local PERIOD   = info.audio_period          -- bytes
local DEPTH    = info.audio_periods

-- Four bytes a frame: two channels of signed sixteen-bit.
local FRAME  = 2 * CHANNELS
local FRAMES = PERIOD // FRAME

local hz = tonumber((args or ""):match("^%s*(%d+)")) or 440
local ms = tonumber((args or ""):match("^%s*%d+%s+(%d+)")) or 333

--
-- The whole tone, built before any of it is played.
--
-- The first version built *one period* and repeated it, snapping the
-- frequency so a whole number of cycles fitted - otherwise the seam
-- between repeats is a discontinuity, and a discontinuity at the period
-- rate is a buzz louder than the tone. That works and the snapping is
-- useless: a 256-frame period at 44100 Hz has a frequency resolution of
-- 172 Hz, so 440 came out as 517 and there was nothing to be done about it
-- short of a longer period, which is latency.
--
-- Generating the lot up front costs memory - a second of stereo is 176 KB,
-- which is why this refuses to be asked for very long - and buys two
-- things. The pitch is exact, because the phase simply continues. And the
-- arithmetic happens *before* the first sample is due rather than between
-- one period and the next, which is the whole difficulty with audio: the
-- expensive part must not be on the path with the deadline.
--
local seconds = ms / 1000

if seconds > 5 then
  print("beep: five seconds is the limit; this builds it all up front")
  return
end

local total  = math.floor(RATE * seconds)
local step   = 2 * math.pi * hz / RATE
local sample = {}

for i = 0, total - 1 do
  -- A quarter of full scale. Loud enough to hear and quiet enough that a
  -- mistake in the mixing later is not painful through headphones.
  local v = math.floor(math.sin(i * step) * 8000)

  if v < 0 then v = v + 65536 end

  local lo = v & 0xff
  local hi = (v >> 8) & 0xff

  -- The same sample to both channels, little endian.
  sample[#sample + 1] = string.char(lo, hi, lo, hi)
end

local tone = table.concat(sample)

--
-- Out, a period at a time.
--
-- `sys.sound` returning false is the *good* case: the device has all it can
-- hold, which means this loop is ahead. What matters is the other end -
-- `sys.sound_queued()` reaching zero means nothing was in hand when the
-- device wanted more, and that is a click you can hear. Counting them is
-- the measurement `roadmap.md` M11a promises in place of a bound.
--
local at, sent, dry, lowest = 1, 0, 0, DEPTH

--
-- Wall clock, because the dry count is meaningless without it.
--
-- A device that plays in real time takes a second to play a second, and the
-- loop above spends that second waiting - which is when running dry means
-- something. QEMU's `wav` backend does not: it writes whatever arrives as
-- fast as it arrives, so the queue is empty almost always and "ran dry"
-- counts the backend rather than the system.
--
-- So the report says both. If the elapsed time is near the duration, the
-- device paced and the dry count is real; if it is far below, the
-- measurement is of a test rig and should be ignored. An instrument that
-- cannot say whether it was measuring anything is worse than none.
--
local counter_hz = (fs.read("/dev/cpu") or {}).counter_hz or 62500000
local began = sys.ticks()

while at <= #tone do
  local chunk = tone:sub(at, at + PERIOD - 1)
  local queued = sys.sound_queued() or 0

  if queued < lowest then lowest = queued end

  -- Only counted once the pipe has had a chance to fill, or the first few
  -- periods - when it is legitimately empty - would read as failures.
  if queued == 0 and sent > DEPTH then dry = dry + 1 end

  if sys.sound(chunk) then
    at = at + PERIOD
    sent = sent + 1
  else
    -- Full. Yield rather than spin: a busy wait here is a core taken away
    -- from whatever else is running, to wait for a device that will be
    -- ready in five milliseconds whatever this thread does.
    sys.yield()
  end
end

local elapsed = ((sys.ticks() - began) * 1000) // counter_hz
local paced = elapsed * 10 >= ms * 8      -- within a fifth of real time

print(("beep: %d Hz, %d ms of sound in %d ms, %d periods, %d deep at worst")
      :format(hz, ms, elapsed, sent, lowest))

if paced then
  print(("      the device paced it; ran dry %d times"):format(dry))
else
  print("      the backend took it as fast as it was offered, so there is"
        .. " no latency here to measure")
end
