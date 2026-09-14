-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- How fast a drive or a filesystem is, at the prompt.
--
--   diskbench                        what can be measured here
--   diskbench /home [seconds] [runs] a filesystem, through a test file
--   diskbench usb 0 [seconds] [runs] a USB stick's blocks, read only
--
-- The rows Disk Benchmark's window will draw, printed as each one finishes,
-- and the run saved in `/home/benchmarks` so a later one can be compared
-- with it. `/lib/diskbench.lua` has what each row measures and why some say
-- "not yet".

local diskbench = use("/lib/diskbench.lua")

local words = {}

for word in (args or ""):gmatch("%S+") do
  words[#words + 1] = word
end

local targets = diskbench.targets()

local function usage()
  print("usage: diskbench /home [seconds] [runs]")
  print("       diskbench usb UNIT [seconds] [runs]")
  print("")

  if #targets == 0 then
    print("nothing here can be measured")
  else
    print("what can be measured here:")

    for _, t in ipairs(targets) do
      print("  " .. t.name)
    end
  end
end

local target, rest

if words[1] == "/home" then
  for _, t in ipairs(targets) do
    if t.kind == "file" then target = t end
  end
  rest = 2
elseif words[1] == "usb" and tonumber(words[2]) then
  for _, t in ipairs(targets) do
    if t.kind == "drive" and t.unit == tonumber(words[2]) then target = t end
  end
  rest = 3
end

if not target then
  usage()
  return
end

local seconds = tonumber(words[rest]) or diskbench.SECONDS
local runs = math.tointeger(tonumber(words[rest + 1])) or diskbench.RUNS

local function mb(c)
  return ("%.1f MB/s"):format(c.bytes_per_second / (1024 * 1024))
end

local function side(c, random)
  if not c.bytes_per_second then return c.why end

  if random then
    return ("%s  %.0f IOPS"):format(mb(c), c.per_second)
  end

  return mb(c)
end

print(("Disk Benchmark %s: %s, best of %d, %s s each")
      :format(tostring((sys.build() or {}).version), target.name, runs,
              tostring(seconds)))
print("Under an emulator this measures the emulator: compare runs on the same footing.")
print("")
print(("%-20s %-34s %s"):format("", "read", "write"))

local co = diskbench.measure(target, seconds, runs)
local result

while true do
  local ok, got = coroutine.resume(co)

  if not ok then
    print("diskbench: " .. tostring(got))
    return
  end

  if coroutine.status(co) == "dead" then
    result = got
    break
  end

  if got and got.row then
    for _, row in ipairs(diskbench.ROWS) do
      if row.id == got.row then
        local random = row.id:sub(1, 3) == "rnd"

        print(("%-20s %-34s %s"):format(
          ("%s x%d"):format(row.name, row.queue),
          side(got.cell.read, random), side(got.cell.write, random)))
      end
    end
  end
end

if result.why then
  print("diskbench: " .. tostring(result.why))
  return
end

print("")
print(result.note)
print("where the time went: not measured yet")

local path, why = diskbench.save(result)

if path then
  print("saved " .. path)
else
  print("not saved: " .. tostring(why))
end
