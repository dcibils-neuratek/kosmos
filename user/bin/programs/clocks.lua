-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- How fast each processor is really running.
--
--   clocks          one second, measured
--   clocks 5        five seconds
--
-- **Measured, not looked up** (`roadmap.md`, "The processor's speed"):
-- Diego asked on 11 October why the M700's older i7 renders faster than
-- the T14's newer i5, and the answer offered - that nothing in Kosmos asks
-- either processor for speed, so each runs where its firmware left it - is
-- an inference until each core says what it ran at. Each core's own two
-- counters, read on its ticks (`pulse.speeds`), over the time asked for.

local pulse = use("/Kosmos/Libraries/pulse.lua")

local seconds = tonumber((args or ""):match("%d+")) or 1
local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 0
local name = pulse.name()

local a, ia = sys.cpuload(), sys.info()
local started = sys.ticks()

while sys.ticks() - started < seconds * hz do sys.sleep(10) end

local b, ib = sys.cpuload(), sys.info()
local heat = pulse.heat(ia, ib, (sys.ticks() - started) / math.max(hz, 1), b)
local speeds = a and b and hz > 0 and pulse.speeds(a, b, hz)

print(name ~= "" and name or "This processor")

-- How hot it is and what it draws, before the speeds, because on a machine
-- that keeps them they are what says why the speed is what it is.
if heat then
  local hot = {}

  for c = 1, #b do hot[#hot + 1] = b[c].temp_c and tostring(b[c].temp_c) or "-" end

  print(("  %s C, slows at %s C; cores %s"):format(tostring(heat.temp_c or "?"),
        tostring(heat.limit_c or "?"), table.concat(hot, " ")))

  if heat.limit_w then
    print(("  drawing %s, may draw %.0f W sustained and %s a burst"):format(
          heat.watts and ("%.1f W"):format(heat.watts) or "an unknown power",
          heat.limit_w, heat.burst_w and ("%.0f W"):format(heat.burst_w) or "no more"))
  end

  print(heat.held == "hot" and "  held back now: too hot"
        or heat.held == "power" and "  held back now: at its power limit"
        or "  held back by neither its heat nor its power limit")
else
  print("  says nothing of its heat or its power to this kernel")
end

if not speeds then
  print("  does not say how fast it runs: it keeps no APERF and MPERF, or")
  print("  the machine it runs in does not pass them on.")
  return
end

for c = 1, #b do
  print(("  core %-2d  %s"):format(c, speeds[c] and ("%5.0f MHz"):format(speeds[c])
                                    or "    idle the whole time"))
end

print(("  over %d s, against a reference of %.0f MHz"):format(seconds, hz / 1e6))

-- Who chooses the speed: the processor itself (`opt/kosmos/speed`), or the
-- firmware's setting as it was left.
local leaning = b[1] and b[1].speed_leaning

print(leaning and ("  the processor chooses its own speed, leaning %d of 255 to saving power"):format(leaning)
      or "  the speed is the firmware's: opt/kosmos/speed is not set")
