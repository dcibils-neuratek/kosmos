-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A tone, and the first thing this machine ever said out loud.
--
--   beep                 440 Hz for a third of a second
--   beep 880             that pitch
--   beep 880 1000        that pitch, that many milliseconds
--
-- Deliberately a *program* and not an application: it makes a noise and
-- exits, which is what the console is for. What it is really for is being
-- the smallest thing that exercises the whole path - a stream (`audio.lua`)
-- through its shared ring to the audio server, and the server to the
-- device - so that when something larger is silent, this says whether the
-- silence starts above or below it.
--
-- It also prints how long the sound took to hand over against how long it
-- lasts, which says whether the device was setting the pace.

local audio = use("/Kosmos/Libraries/audio.lua")

local fmt = audio.format()

if fmt.period == 0 then
  print("beep: this machine has no sound device")
  return
end

local RATE     = fmt.rate
local PERIOD   = fmt.period                 -- bytes

--
-- Through the audio server rather than straight at the device.
--
-- `beep` used to hold `SPAWN_AUDIO` itself, which worked and meant only one
-- thing could ever make a noise. Now it opens a stream like anything else,
-- appears in the Mixer with its own fader, and can be turned down while
-- something louder plays.
--
local out, why = audio.open("beep")

if not out then
  print("beep: " .. tostring(why))
  return
end

local hz = tonumber((args or ""):match("^%s*(%d+)")) or 440
local ms = tonumber((args or ""):match("^%s*%d+%s+(%d+)")) or 333

--
-- A seam-free loop, built once and repeated.
--
-- Three versions of this, and the differences are the whole lesson.
--
-- The first snapped the frequency so a whole number of cycles fitted one
-- *period*, to avoid a discontinuity at the seam. A 256-frame period has a
-- frequency resolution of 172 Hz, so 440 came out as 517.
--
-- The second built the entire tone up front, which is exact - and is a lot
-- of work: three seconds is 132,000 iterations of `sin` and `string.char`.
-- That was fine at the prompt and fatal under the desktop. A program doing
-- that much Lua before its first sound is at NORMAL priority competing with
-- a compositor that spins at DISPLAY, so it never finishes: the Mixer
-- showed a stream that had opened and never played a note, and the machine
-- was silent with everything apparently working. `docs/state.md` records
-- that starvation already; this is a second face of it.
--
-- This one builds a whole number of *cycles* - seam-free by construction,
-- because the buffer length is chosen to fit the pitch rather than the
-- pitch squeezed to fit the buffer - and repeats it. About eleven thousand
-- iterations however long the tone lasts.
--
if ms > 60000 then ms = 60000 end

local loop_frames = RATE // 4
local cycles = math.max(1, (hz * loop_frames) // RATE)

-- The buffer holds exactly that many cycles, so its end meets its start.
loop_frames = (cycles * RATE) // hz

local sample = {}

for i = 0, loop_frames - 1 do
  -- A quarter of full scale. Loud enough to hear and quiet enough that a
  -- mistake in the mixing later is not painful through headphones.
  local v = math.floor(math.sin(2 * math.pi * cycles * i / loop_frames) * 8000)

  if v < 0 then v = v + 65536 end

  local lo = v & 0xff
  local hi = (v >> 8) & 0xff

  sample[#sample + 1] = string.char(lo, hi, lo, hi)
end

local loop = table.concat(sample)

-- Enough copies to cover the duration, cut to length - four bytes a frame,
-- two channels of signed sixteen-bit, as the loop above builds them.
local want_bytes = ((RATE * ms) // 1000) * 4
local copies = math.max(1, (want_bytes // #loop) + 1)
local tone = string.rep(loop, copies):sub(1, want_bytes)

--
-- Out, a period at a time, into the stream's ring. A ring with no room is
-- the *good* case: the server has all it can hold, which means this loop is
-- ahead of the device, and `write` sleeps until there is room again.
--
local at, sent = 1, 0

--
-- Wall clock, because the periods sent say nothing about pace without it.
--
-- A device that plays in real time takes a second to play a second, and the
-- loop below spends that second waiting - which is when running dry means
-- something. QEMU's `wav` backend does not: it writes whatever arrives as
-- fast as it arrives, so the queue is empty almost always and a latency
-- measured there would be the backend's rather than the system's.
--
-- So the report says both. If the elapsed time is near the duration, the
-- device paced it; if it is far below, the measurement is of a test rig
-- and should be ignored. An instrument that cannot say whether it was
-- measuring anything is worse than none.
--
local counter_hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
local began = sys.ticks()

while at <= #tone do
  --
  -- `write` waits; `play` reports and returns.
  --
  -- This was a loop around `play` with `sys.yield()` on "full", under a
  -- comment reading "yield rather than spin" - which had the two words the
  -- wrong way round. Yielding *is* spinning: the thread goes to the back
  -- of its band and is immediately runnable again, so it was never not
  -- running. The meter said 63%.
  --
  local ok, why = out:write(tone:sub(at, at + PERIOD - 1))

  if not ok then
    -- Not "wait", but "that did not arrive". Spinning on this is how a
    -- program hangs with its stream still open and nothing to show for it.
    print("beep: the audio server would not take a period: " .. tostring(why))
    out:close()

    return
  end

  at = at + PERIOD
  sent = sent + 1
end

local elapsed = ((sys.ticks() - began) * 1000) // counter_hz
local paced = elapsed * 10 >= ms * 8      -- within a fifth of real time

--
-- Held open until the server has mixed what it was given.
--
-- Closing the moment the last period is handed over drops whatever has not
-- been mixed yet - which for a short beep is most of it, because the client
-- runs ahead of the device by design. This is the one place a client has to
-- know the pipe has depth: wait for the stream's own queue to drain before
-- taking it away.
--
for _ = 1, 200 do
  -- Its own ring, read directly. Asking the server would be a round trip
  -- for a number this process can see.
  if out:queued() == 0 then break end

  sys.sleep(1)
end

out:close()

print(("beep: %d Hz, %d ms of sound in %d ms, %d periods")
      :format(hz, ms, elapsed, sent))

if not paced then
  print("      the backend took it as fast as it was offered, so there is"
        .. " no latency here to measure")
end

