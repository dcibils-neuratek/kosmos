-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The processor: what it is called, and how fast each of its cores ran.
--
--   local pulse = use("/Kosmos/Libraries/pulse.lua")
--   pulse.name()                     the firmware's brand string
--   pulse.processor()                maker, model, rated clock
--   pulse.speeds(before, after, hz)  each core's MHz between two readings
--   pulse.heat(before, after, seconds, load)  how hot, how much power, why held
--
-- Monitor and `clocks` use it; About and neofetch can say the name the same
-- way. It was also the panel BeOS's Pulse drew - a bar a processor - for
-- the Processor Cores app, which went on 12 October 2026 (Diego: "remove the
-- processor cores app which is useless now") once Monitor showed every core
-- better; the panel, and `ui.leds` it drew with, went with it.

local pulse = {}

--
-- What processor this is, in the words `/Devices/cpu` uses.
--
-- **The rate is labelled "counter" and that is not pedantry.** Pulse said
-- "450 MHz" and meant the core clock; `/Devices/cpu` has no such number on
-- either of this system's boards. What it has is the frequency of the
-- counter the kernel measures time with - 62.5 MHz under TCG, 24 under hvf,
-- a thousand on the other machine - and printing that where a person reads
-- a clock speed would be the same mistake this system has already made with
-- these units twice, one board apart. So it says which clock it is.
-- `architecture.md` §5 is why there are two.
--
--
-- The processor's name for a person: its own when it gives one ("Intel(R)
-- Core(TM) i7-6700 CPU @ 3.40GHz", `/Devices/cpu`'s `name`), else its
-- maker and part. Diego, 8 October: "why ... the cpu monitor app does not
-- show the real name of the cpu".
--

function pulse.name()
  local cpu = fs.read("/Devices/cpu") or {}

  if type(cpu.name) == "string" and cpu.name ~= "" then return cpu.name end

  return (("%s %s"):format(cpu.implementer or "", cpu.part or "")):match("^%s*(.-)%s*$")
end

--
-- **The processor's name, split to be shown** (`docs/monitor.html`): the
-- firmware's brand string with the trademarks taken off, as a maker, a
-- model and the clock it is rated at -
--
--   "Intel(R) Core(TM) i7-6700 CPU @ 3.40GHz"
--       -> "Intel", "Core i7-6700", "3.40 GHz"
--   "11th Gen Intel(R) Core(TM) i5-1145G7 @ 2.60GHz"
--       -> "Intel \u{b7} 11th Gen", "Core i5-1145G7", "2.60 GHz"
--   "AMD Ryzen 9 5950X 16-Core Processor" -> "AMD", "Ryzen 9 5950X", nil
--
-- A name from a maker it does not know is the model whole, with no maker;
-- an ARM core, with no brand string, is its implementer and its part. Here
-- rather than in Monitor because About and neofetch say the same thing.
--
local MAKERS = { Intel = true, AMD = true, VIA = true, Zhaoxin = true, Hygon = true }

function pulse.split_name(name, cpu)
  name = tostring(name or "")
  cpu = cpu or {}

  if name == "" then
    return tostring(cpu.implementer or ""), tostring(cpu.part or "processor"), nil
  end

  local s = name:gsub("%((%a+)%)", function(mark)
    return (mark == "R" or mark:upper() == "TM") and "" or nil
  end)

  local rated = s:match("@%s*([%d%.]+)%s*GHz")

  s = s:gsub("%s*@.*$", "")
  s = s:gsub("%s+[Pp]rocessor%s*$", ""):gsub("%s+%d+%-[Cc]ore%s*$", "")
  s = s:gsub("%s+CPU%s*$", "")
  s = s:gsub("%s+", " "):match("^%s*(.-)%s*$")

  local gen, rest = s:match("^(%d+%a%a Gen) (.+)$")

  if gen then s = rest end

  local maker, model = s:match("^(%S+) (.+)$")

  if not maker or not MAKERS[maker] then
    maker, model = "", s
  end

  if gen then maker = maker .. " \u{b7} " .. gen end

  return maker, model, rated and (rated .. " GHz") or nil
end

-- This machine's, from `/Devices/cpu`.
function pulse.processor()
  local cpu = fs.read("/Devices/cpu") or {}

  return pulse.split_name(cpu.name, cpu)
end

--
-- **How fast each processor ran between two readings of `sys.cpuload()`**,
-- in MHz: its own two counters - one at the speed it ran, one at a fixed
-- reference rate, which on x86 is the counter's, `counter_hz` - read by it
-- on its ticks (`kernel/percpu.h`). An array a core, nil for a core that
-- did not run in between, and nil altogether where the processor keeps no
-- such counters - an ARM core, or a hypervisor that does not pass them on.
--
function pulse.speeds(a, b, counter_hz)
  local out, any = {}, false

  for c = 1, math.max(#(a or {}), #(b or {})) do
    local x, y = a[c], b[c]
    local da = x and y and (y.clock_actual or 0) - (x.clock_actual or 0) or 0
    local dr = x and y and (y.clock_reference or 0) - (x.clock_reference or 0) or 0

    if x and y and ((y.clock_reference or 0) ~= 0) then any = true end
    -- The ratio first, in floating point: `counter_hz * da` is past 2^63
    -- after a few seconds of a busy core - 3.4 GHz times ten billion - and
    -- integers wrap; the M700 read -172 MHz on every loaded core.
    out[c] = (dr > 0 and da > 0) and (counter_hz * (da / dr) / 1e6) or nil
  end

  return any and out or nil
end

--
-- **How hot the processor is and what it draws** between two readings of
-- `sys.info()` taken `seconds` apart, with `sys.cpuload()`'s latest beside
-- them for each core's own sensor (`roadmap.md`, "The processor's heat and
-- power"). nil where nothing is known - every processor the kernel does
-- not know to keep these, and QEMU's - otherwise a table:
--
--   temp_c      the package's temperature, or the hottest core's
--   limit_c     where it slows to cool (TjMax)
--   watts       what the package drew between the readings, or nil
--   limit_w     what it may draw sustained (PL1); burst_w a burst (PL2)
--   held        "hot" or "power" when it is held below its speed now
--
function pulse.heat(a, b, seconds, load)
  local hottest

  for _, c in ipairs(load or {}) do
    if c.temp_c and (not hottest or c.temp_c > hottest) then hottest = c.temp_c end
  end

  if not (b and b.limit_c) and not hottest then return nil end

  local out = { temp_c = b and b.package_c or hottest, limit_c = b and b.limit_c }

  if b and b.limit_c then
    local da = a and a.energy_uj and b.energy_uj and b.energy_uj - a.energy_uj

    out.watts = da and da >= 0 and seconds and seconds > 0 and da / 1e6 / seconds or nil
    out.limit_w = (b.power_limit_mw or 0) > 0 and b.power_limit_mw / 1000 or nil
    out.burst_w = (b.power_burst_mw or 0) > 0 and b.power_burst_mw / 1000 or nil
    out.held = (b.held_hot == 1 and "hot") or (b.held_power == 1 and "power") or nil
  end

  return out
end

return pulse
