-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- htop: what this machine is doing, in layers.
--
-- A program, in /Kosmos/Programs, running in an address space of its own.
-- It reads /Devices through its own namespace - the same list/read protocol
-- the filesystem answers - and asks the kernel only for the process table.
--
-- What is worth seeing about Kosmos is not that there are five processes.
-- It is that there are two layers, that the lower one is twenty kilobytes
-- and knows nothing about files or windows, and that every process in the
-- upper one can reach exactly the things in its capability table and
-- nothing else. So the columns are CAPS and OWNS, and EL1 gets a box.

--
-- What kind of thing each process is - a driver, a server, an app or a
-- program - as Processes says it: `/Kosmos/Libraries/prockind.lua` decides,
-- from device authority, who started it, and what its file declares. This
-- kept a table of names of its own, which every new server had to be added
-- to and which called anything it did not know an app.
--
local prockind = use("/Kosmos/Libraries/prockind.lua")

-- What each file in the image's two folders declares, and the
-- applications installed in `/Home/Apps`, asked once: none of it changes
-- while this runs.
local declared = {}

for _, dir in ipairs({ "/Kosmos/Apps", "/Kosmos/Programs" }) do
  for _, file in ipairs(fs.list(dir) or {}) do
    local attrs = fs.getattr(dir .. "/" .. file)
    local kind  = attrs and attrs.kind

    declared[(file:gsub("%.lua$", ""))] =
      (kind == "application") and "app" or kind or "program"
  end
end

for _, app in ipairs(use("/Kosmos/Libraries/filetypes.lua").installed()) do
  declared[app.name] = "app"
end

local STATE = { [0] = "unused", "ready", "running", "blocked", "dead" }

--
-- The scheduling bands, by name. `sched.h` names six of eight and says
-- anything unnamed is NORMAL.
--
-- Shown because the band is the thing that decides who runs when the machine
-- is busy, and nothing on this screen said what it was: a process at 90% and
-- a process at 90% *in the display band* are different situations.
--
-- The kernel's names for its bands (`sys.scheduler().band_names`).
local BANDS = (sys.scheduler() or {}).band_names or {}

-- A share as a bar of characters, as `monitor` draws one (`text.lua`).
local meter = use("/Kosmos/Libraries/text.lua").meter

-- Two samples, because a percentage is the difference between them.
--
-- A single reading says what fraction of all time since boot was busy,
-- which on a machine that has been sitting at a prompt is a number that
-- never moves. Every run of this program is a fresh process with no memory
-- of the last one, so it takes both samples itself.
--
-- The differences are `meters.lua`'s, as the Deskbar's meters and
-- Processes take them: the machine's busy, and each process's share of
-- every tick that passed (`procshare.lua`'s rows), the process's own
-- table in each. A sample answers both: the busy, and the rows.
local meters = use("/Kosmos/Libraries/meters.lua")
local busy, procs = meters.busy(), meters.processes()

local function sample()
  local k = fs.read("/Devices/kernel")
  local list = sys.processes()

  return busy:take(k), procs:take(list, k)
end

-- Asleep for `ticks` scheduler ticks, rather than yielding until the
-- counter passes a number - which is a spin dressed as a wait. Returns false
-- if Control-C was pressed while waiting, so the caller can stop between
-- rounds rather than only between screens.
local function pause(ticks)
  sys.sleep(ticks)
  return not interrupted()
end

-- `pct` the machine's busy since the first sample (nil when no tick
-- passed), `rows` each process's share of it.
local function report(pct, rows)
  local cpu = fs.read("/Devices/cpu")
  local mem = fs.read("/Devices/memory")
  local k   = fs.read("/Devices/kernel")

  pct = pct or 0

  local used, total = meters.memory(mem)

  print("")
  print(("KOSMOS%sup %ds"):format((" "):rep(52), sys.ticks() // cpu.counter_hz))
  print("")
  print(("  CPU  %s %3d%%    %s %s x%d"):format(
        meter(pct, 22), pct, cpu.implementer, cpu.part, cpu.cores))
  print(("  MEM  %s %d / %d MB"):format(
        meter(meters.share(used, total) or 0, 22), used, total))
  print(("  POOLS  threads %d/%d  processes %d/%d  spaces %d/%d  endpoints %d/%d"):format(
        k.threads, k.threads_max, k.processes, k.processes_max,
        k.spaces, k.spaces_max, k.endpoints, k.endpoints_max))

  print("")
  -- "KERNEL" and "USER" rather than EL1 and EL0: those are AArch64's names
  -- for the two sides of this line, and there are two architectures now.
  print("  KERNEL  the kernel")
  print("          threads . address spaces . IPC . capabilities")
  print("          It does not know what a file is, what a window is, or")
  print("          what Lua is. Everything below asks it for those.")
  print("")
  print("  USER    every process, in an address space of its own")
  print("")
  print("   PID  NAME       KIND        BAND      CPU%  CAPS  OWNS            STATE")

  -- Every process's row; the kernel's own, which Processes shows, is the
  -- KERNEL box above rather than a line in this list.
  for _, row in ipairs(rows) do
    if not row.kernel then
      local p, share = row.process, row.pct

      local owns = {}
      if p.owns & 1 ~= 0 then owns[#owns + 1] = "console" end
      if p.owns & 2 ~= 0 then owns[#owns + 1] = "screen" end

      print(("  %4d  %-10s %-11s %-8s %3d%%  %4d  %-15s %s"):format(
            p.id, p.name, prockind.of(p, declared),
            BANDS[p.priority] or tostring(p.priority),
            share, p.caps,
            #owns > 0 and table.concat(owns, "+") or "-",
            p.exited and ("exited " .. p.exit_code) or (STATE[p.state] or "?")))
    end
  end
end

local rounds = tonumber(args) or 1

-- Half a second, in the scheduler's ticks - asked of the kernel, since the
-- rate is the board's and not this program's to assume.
local half = math.max(1, (fs.read("/Devices/kernel").tick_hz or 250) // 2)

for i = 1, rounds do
  sample()                    -- measured from here: what it says is since the last

  if not pause(half) then break end
  report(sample())

  if i < rounds and not pause(half) then break end
end
