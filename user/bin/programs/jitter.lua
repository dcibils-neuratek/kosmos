-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- How long this machine can take the processor away without being asked.
--
--   jitter
--   jitter 5            for five seconds instead of one
--
-- A tight loop reading the counter and recording the biggest step between
-- two consecutive reads. Nothing is allocated, nothing is called, nothing
-- sleeps: every microsecond this loop does not account for is a microsecond
-- something else had the machine.
--
-- **This is the number every real-time claim has to be read against.** A
-- 5.8 ms audio period cannot be met on a machine whose noise floor is tens
-- of milliseconds, and no amount of shared memory, interrupt wiring or
-- priority band changes that - so measuring the floor first is what stops a
-- day being spent optimising against it.
--
-- It cannot tell apart being preempted, the whole guest being descheduled
-- by a host, and the emulator stopping to do its own work. All three are
-- "time passed and this did not run". What it settles is whether a missed
-- deadline is worth looking for in the design at all.

local hz = (fs.read("/dev/cpu") or {}).counter_hz or 62500000
local us = hz // 1000000
local seconds = tonumber((args or ""):match("%d+")) or 1

local worst, over_1ms, over_10ms, samples = 0, 0, 0, 0
local prev = sys.ticks()
local stop = prev + hz * seconds

while prev < stop do
  local now = sys.ticks()
  local gap = (now - prev) // us

  if gap > worst then worst = gap end
  if gap > 1000 then over_1ms = over_1ms + 1 end
  if gap > 10000 then over_10ms = over_10ms + 1 end

  samples = samples + 1
  prev = now
end

print(("jitter: %d samples over %d s"):format(samples, seconds))
print(("jitter: worst gap %d us   over 1 ms: %d   over 10 ms: %d")
      :format(worst, over_1ms, over_10ms))

if worst > 5800 then
  print("jitter: this machine cannot hold a 5.8 ms audio period. The floor "
        .. "is the platform, not the audio path.")
end
