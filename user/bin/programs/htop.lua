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
local BANDS = { [0] = "idle", "low", "normal", "display", "audio", "input" }

-- A share as a bar of characters, as `monitor` draws one (`text.lua`).
local meter = use("/Kosmos/Libraries/text.lua").meter

-- Two samples, because a percentage is the difference between them.
--
-- A single reading says what fraction of all time since boot was busy,
-- which on a machine that has been sitting at a prompt is a number that
-- never moves. Every run of this program is a fresh process with no memory
-- of the last one, so it takes both samples itself.
local function sample()
  local k = fs.read("/Devices/kernel")
  local by_pid = {}

  for _, p in ipairs(sys.processes()) do
    by_pid[p.id] = p.ticks
  end

  return { idle = k.idle_ticks, busy = k.busy_ticks, procs = by_pid }
end

-- Asleep for `ticks` scheduler ticks, rather than yielding until the
-- counter passes a number - which is a spin dressed as a wait. Returns false
-- if Control-C was pressed while waiting, so the caller can stop between
-- rounds rather than only between screens.
local function pause(ticks)
  sys.sleep(ticks)
  return not interrupted()
end

local function report(before, after)
  local cpu = fs.read("/Devices/cpu")
  local mem = fs.read("/Devices/memory")
  local k   = fs.read("/Devices/kernel")

  local elapsed = (after.idle + after.busy) - (before.idle + before.busy)
  local busy = after.busy - before.busy
  local pct = elapsed > 0 and (busy * 100) // elapsed or 0

  local used = mem.total_mb - mem.free_mb

  print("")
  print(("KOSMOS%sup %ds"):format((" "):rep(52), sys.ticks() // cpu.counter_hz))
  print("")
  print(("  CPU  %s %3d%%    %s %s x%d"):format(
        meter(pct, 22), pct, cpu.implementer, cpu.part, cpu.cores))
  print(("  MEM  %s %d / %d MB"):format(
        meter((used * 100) // mem.total_mb, 22), used, mem.total_mb))
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

  for _, p in ipairs(sys.processes()) do
    local was = before.procs[p.id] or p.ticks
    local share = elapsed > 0 and ((p.ticks - was) * 100) // elapsed or 0

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

local rounds = tonumber(args) or 1

-- Half a second, in the scheduler's ticks - asked of the kernel, since the
-- rate is the board's and not this program's to assume.
local half = math.max(1, (fs.read("/Devices/kernel").tick_hz or 250) // 2)

for i = 1, rounds do
  local before = sample()

  if not pause(half) then break end
  report(before, sample())

  if i < rounds and not pause(half) then break end
end
