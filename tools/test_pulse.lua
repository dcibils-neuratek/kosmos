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

-- 3b. A busy core over a few seconds, as the M700 read it: ten billion
-- counts each way at 3.41 GHz, where counter_hz times the actual count is
-- past 2^63 - it read -172 MHz while the ratio was taken last.
do
  local a = core(4000000000000, 4000000000000)
  local b = core(4000000000000 + 10230000000, 4000000000000 + 10230000000)
  local s = pulse.speeds({ a }, { b }, 3410000000)

  check(s and math.abs(s[1] - 3410) < 0.001, "a busy core over three seconds at 3.41 GHz was "
        .. tostring(s and s[1]) .. " MHz")
end

-- 4. A processor that keeps no counters: both zero always, and no speeds at
-- all - not a table of nothing, which a window would draw as "idle".
do
  check(pulse.speeds({ core(0, 0), core(0, 0) }, { core(0, 0), core(0, 0) }, 1e9) == nil,
        "a processor with no counters was given speeds")
  check(pulse.speeds({ {}, {} }, { {}, {} }, 1e9) == nil,
        "a reading from a kernel before these fields was given speeds")
end

-- 5. The processor's name, split: the three machines Kosmos runs on, an
-- AMD, and a name from nobody it knows.
do
  local function said(name, cpu)
    local maker, model, rated = pulse.split_name(name, cpu)
    return maker .. "|" .. model .. "|" .. tostring(rated)
  end

  local cases = {
    { "Intel(R) Core(TM) i7-6700 CPU @ 3.40GHz", "Intel|Core i7-6700|3.40 GHz" },
    { "11th Gen Intel(R) Core(TM) i5-1145G7 @ 2.60GHz",
      "Intel \u{b7} 11th Gen|Core i5-1145G7|2.60 GHz" },
    { "AMD Ryzen 9 5950X 16-Core Processor", "AMD|Ryzen 9 5950X|nil" },
    { "QEMU Virtual CPU version 2.5+", "|QEMU Virtual CPU version 2.5+|nil" },
  }

  for _, c in ipairs(cases) do
    check(said(c[1]) == c[2], ("%q split as %q, not %q"):format(c[1], said(c[1]), c[2]))
  end

  check(said("", { implementer = "ARM", part = "Cortex-A72" }) == "ARM|Cortex-A72|nil",
        "an ARM core with no brand string: " .. said("", { implementer = "ARM", part = "Cortex-A72" }))
end

if fails == 0 then
  print(("PASS: %d checks on pulse.speeds and pulse.split_name (turbo, a core at its reference, an idle core "
         .. "beside a busy one, none where the processor keeps no counters; and the "
         .. "processor's name split, Intel's two shapes, AMD's, QEMU's and an ARM core's)"):format(checks))
else
  print(("FAIL: %d of %d checks on pulse.speeds"):format(fails, checks))
  os.exit(1)
end
