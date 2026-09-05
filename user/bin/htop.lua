-- htop: what this machine is doing, in layers.
--
-- A program, in /bin, running in an address space of its own. It reads
-- /dev through its own namespace - the same list/read protocol the
-- filesystem answers - and asks the kernel only for the process table.
--
-- What is worth seeing about Kosmos is not that there are five processes.
-- It is that there are two layers, that the lower one is twenty kilobytes
-- and knows nothing about files or windows, and that every process in the
-- upper one can reach exactly the things in its capability table and
-- nothing else. So the columns are CAPS and OWNS, and EL1 gets a box.

--
-- The four that were missing said `app`, which is what an unknown name gets.
--
-- Not because anything classified them wrongly: they were spawned as C
-- servers and never called `sys.name`, so they were all called `init` and
-- there was nothing here to look up. Naming them showed the gap.
--
local LAYER = {
  init = "supervisor", console = "server", ramfs  = "server",
  binfs = "server",    devices = "server", libfs  = "server",
  appfs = "server",    diskfs  = "server", audio  = "server",
  shell = "shell",     burn    = "app",    run    = "runner",
}

local STATE = { [0] = "unused", "ready", "running", "blocked", "dead" }

--
-- The scheduling bands, by name. `sched.h` names five of eight and says
-- anything unnamed is NORMAL.
--
-- Shown because the band is the thing that decides who runs when the machine
-- is busy, and nothing on this screen said what it was: a process at 90% and
-- a process at 90% *in the display band* are different situations.
--
local BANDS = { [0] = "idle", "low", "normal", "display", "input" }

local function meter(pct, width)
  local filled = (pct * width) // 100
  if filled > width then filled = width end
  return "[" .. ("|"):rep(filled) .. ("."):rep(width - filled) .. "]"
end

-- Two samples, because a percentage is the difference between them.
--
-- A single reading says what fraction of all time since boot was busy,
-- which on a machine that has been sitting at a prompt is a number that
-- never moves. Every run of this program is a fresh process with no memory
-- of the last one, so it takes both samples itself.
local function sample()
  local k = fs.read("/dev/kernel")
  local by_pid = {}

  for _, p in ipairs(sys.processes()) do
    by_pid[p.id] = p.ticks
  end

  return { idle = k.idle_ticks, busy = k.busy_ticks, procs = by_pid }
end

-- Returns false if Control-C was pressed while waiting, so the caller can
-- stop between rounds rather than only between screens.
local function pause(ticks)
  local until_ = sys.ticks() + ticks
  while sys.ticks() < until_ do sys.yield() end
  return not interrupted()
end

local function report(before, after)
  local cpu = fs.read("/dev/cpu")
  local mem = fs.read("/dev/memory")
  local k   = fs.read("/dev/kernel")

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
  print("  EL1  the kernel")
  print("       threads . address spaces . IPC . capabilities")
  print("       It does not know what a file is, what a window is, or what")
  print("       Lua is. Everything below this line asks it for those.")
  print("")
  print("  EL0  every process, in an address space of its own")
  print("")
  print("   PID  NAME       LAYER       BAND      CPU%  CAPS  OWNS            STATE")

  for _, p in ipairs(sys.processes()) do
    local was = before.procs[p.id] or p.ticks
    local share = elapsed > 0 and ((p.ticks - was) * 100) // elapsed or 0

    local owns = {}
    if p.owns & 1 ~= 0 then owns[#owns + 1] = "console" end
    if p.owns & 2 ~= 0 then owns[#owns + 1] = "screen" end

    print(("  %4d  %-10s %-11s %-8s %3d%%  %4d  %-15s %s"):format(
          p.id, p.name, LAYER[p.name] or "app",
          BANDS[p.priority] or tostring(p.priority),
          share, p.caps,
          #owns > 0 and table.concat(owns, "+") or "-",
          p.exited and ("exited " .. p.exit_code) or (STATE[p.state] or "?")))
  end
end

local rounds = tonumber(args) or 1
local hz = fs.read("/dev/cpu").counter_hz

for i = 1, rounds do
  local before = sample()

  if not pause(hz // 2) then break end
  report(before, sample())

  if i < rounds and not pause(hz // 2) then break end
end
