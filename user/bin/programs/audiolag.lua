-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Where the time goes when a period is handed to the audio server.
--
-- An instrument rather than a test: it plays four hundred periods of silence
-- and reports how many of the writes came back at once and how long the
-- rest waited. The number that matters is the last one - if the mean wait
-- is a whole tick, the pipeline is running at the tick rate instead of the
-- device rate, which is exactly the fault that made a beep cost 63% of the
-- machine and still play at two thirds speed.
local audio = use("/lib/audio.lua")

local fmt = audio.format()
if fmt.period == 0 then print("audiolag: no sound device") return end

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

--
-- `audiolag [periods]`, and the argument is the whole point of having one.
--
-- The ring's depth is the one number that trades latency against
-- underruns, and until now it was a constant nobody could argue with. A
-- shallow ring is less to wait behind and less to survive a late turn; a
-- deep one is the other way round. Run this at 2, at 8 and at 32 and the
-- two figures below - worst latency and UNDERRUNS - move in opposite
-- directions, which is the curve worth knowing before choosing a default.
--
-- Omitted means the default, so the ordinary run is the ordinary run.
--
local want = tonumber(args)

local chunk = string.rep("\0", fmt.period)
local out = assert(audio.open("audiolag", want))

local hz = (fs.read("/dev/cpu") or {}).counter_hz or 62500000
local us = hz // 1000000

local worst, over, sent = 0, 0, 0
local worst_gap = 0                 -- between iterations, while playing
local worst_delay = 0               -- frames between writing and hearing
local began = sys.ticks()
local prev_iter = began

for _ = 1, N do
  local t0 = sys.ticks()

  local ok, why = out:write(chunk)

  if not ok then
    print("audiolag: stopped at period " .. tostring(sent) .. ": "
          .. tostring(why))
    break
  end

  sent = sent + 1

  --
  -- The latency itself, which this program has never been able to do
  -- anything but infer.
  --
  -- Every other number here is a *timing* - how long a write took, how long
  -- a turn took - and a timing is evidence about latency rather than the
  -- thing itself. This is the thing itself: the server publishes how many
  -- frames of this stream have left the speaker, so the distance between
  -- that and what has been handed over is the whole pipe, the ring and the
  -- device's own queue together.
  --
  -- No clock in it and nothing to convert, which is the point: it is a
  -- subtraction of two frame counts, and a frame is the only unit in this
  -- system that means the same thing on both sides of a boundary.
  --
  local delay = out:delay()

  if delay > worst_delay then worst_delay = delay end

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

  --
  -- And the gap between one turn round this loop and the next, measured
  -- *while sound is playing* rather than after it has stopped.
  --
  -- The first version of this probe ran after `close`, and reported 106 ms
  -- - which turned out to be the teardown, not the playing: unmapping a
  -- region and dropping a capability, once, at the end. Measuring the quiet
  -- moment after the interesting one is an easy mistake and it pointed at
  -- the scheduler for an hour.
  --
  local t_now = sys.ticks()
  local gap = (t_now - prev_iter) // us

  if gap > worst_gap then worst_gap = gap end

  prev_iter = t_now
end

local wall = (sys.ticks() - began) // us

--
-- Let it finish before asking how it went.
--
-- Closing drops whatever is still in the ring and the device then plays
-- itself empty, which counts as an underrun every time - so the figure was
-- the same three whatever the machine did, and no change to the ring, the
-- band or the scheduler ever moved it. Third instrument in this subsystem
-- to measure the shutdown instead of the run.
--
for _ = 1, 2000 do
  if out:queued() == 0 then break end

  sys.sleep(1)
end

local after_play = sys.info() or {}

--
-- The position, read from the far end before the stream goes away.
--
-- The loop above waits until the ring is empty, so everything handed over
-- is either heard or still inside the device. `heard` is the server's
-- count, arrived at by watching the device drain; `handed` is this
-- program's, arrived at by counting a loop. Two ends of the same pipe that
-- never consulted each other.
--
-- They should differ by no more than what the device is holding right now.
-- A larger gap means the position is *drifting*, and that is the number to
-- watch on real hardware: a position that drifts is a video player that
-- desynchronises slowly enough to look like something else's fault.
--
local heard = out:position()
local handed = sent * fmt.frames

-- Read while the stream is still open, so the report below does not depend
-- on what a closed stream happens to leave in its table.
local bound = out:bound()
local ring_periods = out.ring_periods

out:close()

local after = after_play
local audio_ms = N * (fmt.period // 4) * 1000 // fmt.rate

print(("audiolag: %d periods, %d ms of audio in %d ms wall")
      :format(N, audio_ms, wall // 1000))
print(("audiolag: UNDERRUNS %d   worst write %d us   stalls over two periods %d")
      :format((after.audio_dry or 0) - dry0, worst, over))
print(("audiolag: worst gap between turns while playing %d us")
      :format(worst_gap))

--
-- And the bound it should never have crossed.
--
-- The ring holds `fmt.periods` and the device holds its own, so the pipe
-- cannot be longer than the two of them added up. A figure at the bound is
-- a client that keeps the ring full, which is what this one does on
-- purpose; a figure *past* it means the position and the indices disagree,
-- and `tools/test_audioring.c` is where to go and find out why.
--
print(("audiolag: worst latency %d frames, %d us "
       .. "(bound %d frames: ring %d + device %d periods)")
      :format(worst_delay, worst_delay * 1000000 // fmt.rate,
              bound, ring_periods, fmt.periods))
print(("audiolag: position %d frames, handed %d, behind by %d "
       .. "(the device holds %d)")
      :format(heard, handed, handed - heard, fmt.periods * fmt.frames))

--
-- And the same question asked of the server, which is the only process that
-- can see both ends.
--
-- `starved` is passes where the device had room and no ring had a period -
-- the clients are behind. `late` is the worst gap between two turns of the
-- server's own loop - the server is behind. An underrun is one or the
-- other, and until now there was no way to tell which.
--
local srv = audio.stats() or {}

print(("audiolag: server starved %d times, worst turn %d us")
      :format(srv.starved or -1, srv.late or -1))
print(("audiolag: queue floor %d of %d periods")
      :format(after.audio_floor or 0, fmt.periods))

--
-- Did the device actually say anything?
--
-- The distinction that matters when a deadline is missed: an interrupt that
-- never arrives and an interrupt that arrives while something else is slow
-- are the same symptom and completely different faults. One period is 5.8
-- ms, so a second of sound should raise this about 172 times.
--
print(("audiolag: device raised its line %d times for %d periods")
      :format((after.audio_wakes or 0) - (before.audio_wakes or 0), N))

--
-- How long this thread can be away from the processor without asking to be.
--
-- **The measurement that says whether a number here means anything.** A
-- tight loop reading the clock should see gaps of microseconds; a gap of
-- tens of milliseconds means something took the machine away, and until
-- that is known no audio figure can be attributed to the audio path.
--
-- Three things it cannot tell apart, and it is worth being honest that it
-- cannot: this thread preempted by another, the whole guest descheduled by
-- the host, and QEMU stopping to do its own work. All three look like time
-- that passed without this loop running. What it *does* settle is whether
-- the fault is inside the audio design at all - `CLAUDE.md` warns that QEMU
-- is for spotting regressions rather than for knowing whether something is
-- fast, and this is what that warning looks like as a number.
--
local worst_gap, gaps = 0, 0
local prev = sys.ticks()
local stop = prev + hz                  -- one second

while prev < stop do
  local now = sys.ticks()
  local gap = (now - prev) // us

  if gap > worst_gap then worst_gap = gap end
  if gap > 5800 then gaps = gaps + 1 end

  prev = now
end

print(("audiolag: worst gap in a tight loop %d us, %d over one period")
      :format(worst_gap, gaps))

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
