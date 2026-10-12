-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- `/Kosmos/Libraries/pulse.lua`'s arithmetic, on the host: how fast each
-- processor ran between two readings of `sys.cpuload()` (`pulse.speeds`,
-- `roadmap.md` "The processor's speed"). QEMU's processor keeps no APERF or
-- MPERF, so no guest suite can see a speed; the readings are made up here
-- in the shape the kernel gives them, and the drawing half of the library
-- is stood in for.
--
-- Usage: lua tools/test_pulse.lua

local checks, fails = 0, 0

local function check(ok, what)
  checks = checks + 1

  if not ok then
    fails = fails + 1
    print("  " .. what)
  end
end

function use() return { theme = {} } end

local pulse = dofile("user/lib/pulse.lua")

local function core(actual, reference)
  return { clock_actual = actual, clock_reference = reference }
end

-- 1. Turbo: 37 cycles for 34 of a 3.4 GHz reference is 3.7 GHz.
do
  local s = pulse.speeds({ core(1000, 1000) }, { core(1037, 1034) }, 3400000000)

  check(s and math.abs(s[1] - 3700) < 0.001, "37 against 34 at 3.4 GHz was not 3700 MHz: "
        .. tostring(s and s[1]))
end

-- 2. Held at its base: the two move together, and the speed is the reference.
do
  local s = pulse.speeds({ core(5e9, 5e9) }, { core(5e9 + 2.6e9, 5e9 + 2.6e9) }, 2600000000)

  check(s and math.abs(s[1] - 2600) < 0.001, "a core at its reference was not 2600 MHz: "
        .. tostring(s and s[1]))
end

-- 3. A core asleep the whole time: neither counter moved, so no speed for it -
-- and the others still have theirs.
do
  local s = pulse.speeds({ core(10, 10), core(500, 500) }, { core(10, 10), core(1000, 900) }, 1e9)

  check(s and s[1] == nil and math.abs(s[2] - 1250) < 0.001,
        "an idle core beside a busy one: " .. tostring(s and s[1]) .. ", " .. tostring(s and s[2]))
end

-- 4. A processor that keeps no counters: both zero always, and no speeds at
-- all - not a table of nothing, which a window would draw as "idle".
do
  check(pulse.speeds({ core(0, 0), core(0, 0) }, { core(0, 0), core(0, 0) }, 1e9) == nil,
        "a processor with no counters was given speeds")
  check(pulse.speeds({ {}, {} }, { {}, {} }, 1e9) == nil,
        "a reading from a kernel before these fields was given speeds")
end

if fails == 0 then
  print(("PASS: %d checks on pulse.speeds (turbo, a core at its reference, an idle core "
         .. "beside a busy one, and none where the processor keeps no counters)"):format(checks))
else
  print(("FAIL: %d of %d checks on pulse.speeds"):format(fails, checks))
  os.exit(1)
end
