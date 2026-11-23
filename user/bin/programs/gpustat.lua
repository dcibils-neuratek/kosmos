-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- What the GPU is doing, and who holds its memory.
--
--   gpustat           over one second
--   gpustat 5         over five
--
-- The GPU's driver's own counts (`docs/gpumonitor.md` G1): its clock now
-- and its range, how much of the time it was asleep (RC6), the display's
-- flips and frames - so its refresh is measured - and every buffer the
-- driver made, with the process that holds it. And the GPU's power, where
-- the processor's RAPL has a graphics plane. GPU Monitor draws the same.

local gpu = use("/Kosmos/Libraries/gpu.lua")
local words = use("/Kosmos/Libraries/files.lua").words(args, 1)
local seconds = math.tointeger(tonumber(words[1] or "")) or 1
local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
local names = {}

for _, p in ipairs(sys.processes() or {}) do names[p.id] = p.name end

local a, why = gpu.stats()

if not a then
  print("gpustat: " .. why)
  return
end

-- The rates and the watts are `meters.lua`'s, as GPU Monitor takes them:
-- two snapshots, each beside `sys.info()` for the graphics plane's energy.
local measured = use("/Kosmos/Libraries/meters.lua").gpu(hz)
local e0 = sys.info()

measured:take(a, e0)
sys.sleep(math.max(1, seconds) * (e0.tick_hz or 250))   -- scheduler ticks

local b = gpu.stats() or a

measured:take(b, sys.info())

local r = measured.rate

print(("%s (8086:%04x)"):format(b.model, b.device))
-- A clock of nought is the GPU asleep: RPSTAT1 reads 0 in RC6.
print(("  clock      %s, of %d to %d MHz    (RPSTAT1 %08x)")
      :format(b.mhz > 0 and (b.mhz .. " MHz") or "asleep", b.min_mhz, b.max_mhz, b.rpstat))

if r then
  print(("  asleep     %.0f%% of %.1f s in RC6"):format(r.asleep * 100, r.seconds))
end

if measured.watts then
  print(("  power      %.2f W"):format(measured.watts))
else
  print("  power      not known on this processor")
end

print(r and ("  engines    render %.1f%% busy, blitter %.1f%%; video: no work sent yet")
                :format((r.render or 0) * 100, r.blitter * 100)
      or "  engines    render -, blitter -; video: no work sent yet")

if b.width > 0 then
  print(("  display    %dx%d, %.1f frames a second, %.1f flips a second%s")
        :format(b.width, b.height, r and r.frames or 0, r and r.flips or 0,
                b.underrun and ", an underrun seen" or ""))
end

print("  desktop    " .. gpu.desktop_words(b))
print("")
print(("  %-4s %-16s %-12s %10s  %s"):format("id", "holder", "size", "memory", "for"))

for _, buf in ipairs(b.buffers) do
  print(("  %-4s %-16s %-12s %8.1f MB  %s")
        :format(buf.id and tostring(buf.id) or "-",
                buf.holder == 0 and "the firmware" or (names[buf.holder] or ("process " .. buf.holder)),
                ("%dx%d"):format(buf.width, buf.height), buf.bytes / 1048576, buf.kind))
end
