-- kosmos: needs screen
-- A status bar along the bottom of the screen, kept up to date.
--
--   monitor        ten minutes, or until Control-C
--   monitor 30     thirty seconds
--
-- Run it detached and it keeps drawing while you use the shell - which is
-- the whole point, and why this is a program rather than something the
-- shell does between commands. A shell can only redraw when you type; a
-- process can redraw on a clock.
--
-- It draws in the rows the kernel console reserves for its boot progress
-- bar and never scrolls text through. Two writers on one framebuffer with
-- no compositor between them, which is honest here only because those rows
-- are nobody else's by construction - and is exactly the arrangement a
-- compositor exists to stop needing.
--
-- Control-C stops it, and so does running out of seconds. The second one
-- matters because the first is cooperative: this program is stoppable
-- because it asks once a second whether it should stop, and a program that
-- never asks still cannot be stopped from outside.

local RESERVED_ROWS = 2       -- matches kernel/console.c

local seconds = tonumber(args) or 600
local screen = gfx.screen()

if not screen then
  print("monitor: this process was not given the screen")
  return
end

local hz = fs.read("/dev/cpu").counter_hz
local w, h = screen:size()
local top = h - RESERVED_ROWS * gfx.font.h

local function meter(pct, width)
  local filled = (pct * width) // 100
  if filled > width then filled = width end
  return "[" .. ("|"):rep(filled) .. ("."):rep(width - filled) .. "]"
end

-- Usage is the difference between two readings, never one: a single
-- reading says what fraction of all time since boot was busy, which after
-- a minute at a prompt is a number that never moves again.
local last_idle, last_busy

local until_ = sys.ticks() + hz * seconds

while sys.ticks() < until_ do
  local k = fs.read("/dev/kernel")
  local m = fs.read("/dev/memory")
  local c = fs.read("/dev/cpu")

  local pct = 0
  if last_idle then
    local di = k.idle_ticks - last_idle
    local db = k.busy_ticks - last_busy
    if di + db > 0 then pct = (db * 100) // (di + db) end
  end
  last_idle, last_busy = k.idle_ticks, k.busy_ticks

  local text = (" %s x%d  cpu %s %3d%%   %d/%d thr   %d/%d proc   %d/%d MB   up %ds"):
    format(c.part, c.cores, meter(pct, 10), pct,
           k.threads, k.threads_max, k.processes, k.processes_max,
           m.total_mb - m.free_mb, m.total_mb, sys.ticks() // hz)

  screen:fill(0, top, w, h - top, 0xff161b22)
  screen:fill(0, top, w, 1, 0xff30363d)
  screen:text(4, top + gfx.font.h // 2, text, 0xff7ee787, 0xff161b22)

  -- The wait is also where the interrupt is noticed. Asking once per pass
  -- rather than once per yield: the question is an IPC round trip to the
  -- console, and a hundred a second to answer "no" would cost more than the
  -- drawing does.
  local next_ = sys.ticks() + hz
  while sys.ticks() < next_ do sys.yield() end

  if interrupted() then
    -- Give the rows back the way they were found, or the bar stays on
    -- screen with numbers that stopped being true.
    screen:fill(0, top, w, h - top, 0xff0d1117)
    return
  end
end
