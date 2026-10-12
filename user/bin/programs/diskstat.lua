-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- What each drive has read and written, and which programs did it.
--
--   diskstat              /Home, /Temporary and every volume in /Drives
--   diskstat /Drives/X    one drive
--
-- The servers' own counts (`docs/diskmonitor.md` D1 and D2): `.device` under
-- a drive's path - requests, bytes and the time they took, each way - and
-- `.programs`, a row for each program that read or wrote it, with the last
-- path it touched. Running totals since each server started; Disk Monitor
-- is what turns two readings into a rate.

local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
local names = {}

for _, p in ipairs(sys.processes() or {}) do names[p.id] = p.name end

local function ms(ticks, n)
  return n > 0 and ("%.2f ms"):format(ticks / hz * 1000 / n) or "-"
end

local function show(where)
  local d = fs.read(where .. "/.device")

  if type(d) ~= "table" then
    print(("diskstat: %s says nothing of its reading and writing"):format(where))
    return
  end

  print(("diskstat: %s read %d bytes in %d (%s each), wrote %d bytes in %d (%s each)")
        :format(where, d.read_bytes, d.reads, ms(d.read_counter_ticks, d.reads),
                d.write_bytes, d.writes, ms(d.write_counter_ticks, d.writes)))

  local rows = fs.read(where .. "/.programs")

  for _, r in ipairs(type(rows) == "table" and rows or {}) do
    print(("diskstat:   by %d %s, read %d in %d, wrote %d in %d, last %s")
          :format(r.process, names[r.process] or "(ended)", r.read_bytes, r.reads,
                  r.write_bytes, r.writes, r.last ~= "" and r.last or "-"))
  end
end

local asked = (args or ""):match("%S+")

if asked then
  show(asked)
  return
end

show("/Home")
show("/Temporary")

for _, v in ipairs(fs.list("/Drives") or {}) do show("/Drives/" .. v) end
