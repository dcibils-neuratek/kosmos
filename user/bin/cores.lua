-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: section system
-- kosmos: needs processes
-- What every processor in this machine is doing, and a way to give them
-- something to do.
--
-- A bar per core, live, and two buttons that add and remove a
-- compute-bound process. Put a worker on and watch a bar fill; put another
-- on and watch what happens to the second bar. **That second bar is the
-- whole point of this program**, and today it does not exist.
--
--------------------------------------------------------------------------
-- Why this exists before the thing it shows
--
-- Kosmos runs on one core. `docs/smp.md` is the plan for more and step one
-- of it is done - the state that belongs to a processor rather than to the
-- machine now lives in `struct percpu`, reached through `TPIDR_EL1`. There
-- is still exactly one.
--
-- So this is an instrument built before the experiment, which is what this
-- project does: `jitter` measured the noise floor before anybody optimised
-- against it, and `frames` measured where a pass went before anybody
-- rewrote the window manager. **You cannot see SMP arrive without something
-- that would show it.**
--
-- And it is honest in the meantime rather than aspirational. With one core
-- it says one core, it says so in words at the bottom, and two workers on
-- one core produce one full bar and a machine that is exactly twice as slow
-- - which is a real thing to have watched before there is a second core to
-- compare it against.
--
--------------------------------------------------------------------------
-- What a bar actually means
--
-- The difference between two readings, and never one of them. `sys.cpuload`
-- reports ticks charged to the idle thread and to everything else on each
-- core, since boot, and both only rise. A single reading answers "what
-- fraction of all time since this machine started was busy", which on a
-- machine that has been sitting at a prompt is a number that stopped
-- moving. Every meter in this system makes the same subtraction and
-- `sysmon` says so too.
--
local ui = use("/lib/ui.lua")
local theme = ui.theme

local CORES = (sys.info() or {}).cpus or 1

local ROW = 40
local W = 460
local H = 160 + CORES * ROW

local win, err = ui.window{ title = "Cores", w = W, h = H, x = 120, y = 100 }

if not win then
  print("cores: " .. tostring(err))
  return
end

--------------------------------------------------------------------------
-- The workers.
--
-- `/bin/spin.lua`, which exists for exactly this and says so in its own
-- first line: it burns a core and **deliberately does not yield**, because
-- a process that hands the core back politely is not what a workload looks
-- like. It is the thing the scheduler has to preempt.
--
-- Which makes this a demonstration of the priority bands as much as of the
-- cores. A spinner runs at NORMAL and this window runs at DISPLAY, so the
-- bars keep moving and the buttons keep answering while a core is pinned.
-- If they ever stop, that is not this program failing - it is the
-- responsiveness claim failing, and this is where you would see it.
--
-- Started detached and killed by id. `run` hands back nothing to hold, so
-- the ids come from the process list, and the ones this program started are
-- the ones named `spin`.
--------------------------------------------------------------------------

local workers = 0

local function spin_ids()
  local list = sys.processes and sys.processes() or nil
  local ids = {}

  if not list then return ids end

  for i = 1, #list do
    if list[i].name == "spin" then
      ids[#ids + 1] = list[i].id
    end
  end

  return ids
end

local function add_worker()
  -- A long spin, so it outlives a look. It is killed rather than waited
  -- out; `spin 600` is ten minutes and nobody watches for ten minutes.
  if run("/bin/spin.lua", "600", true) then
    workers = workers + 1
  end
end

local function remove_worker()
  local ids = spin_ids()

  if #ids > 0 and sys.kill(ids[#ids]) then
    workers = workers - 1

    if workers < 0 then workers = 0 end
  end
end

--------------------------------------------------------------------------
-- The bars.
--------------------------------------------------------------------------

local pct = {}
local last = {}

local function meter(index, y)
  local v = ui.view{ x = 14, y = y, w = W - 28, h = 26 }

  function v:draw(g)
    local value = pct[index] or 0
    local label = (CORES == 1) and "processor"
                               or ("core " .. tostring(index - 1))
    local right = ("%d%%"):format(value)

    g:text(0, 0, label, "text_dim")
    g:text(self.w - #right * gfx.font.w, 0, right, "text")

    local top = gfx.font.h + 4

    g:fill(0, top, self.w, 10, "sunken")
    g:frame(0, top, self.w, 10, "line")

    local filled = (self.w - 2) * value // 100

    if filled > 0 then
      g:fill(1, top + 1, filled, 8,
             (value > 80) and theme.bad or theme.good)
    end
  end

  return v
end

for c = 1, CORES do
  win:add(meter(c, 14 + (c - 1) * ROW))
end

local y = 14 + CORES * ROW + 8

local count = ui.label{ x = 14, y = y, text = "no workers" }
win:add(count)

win:add(ui.button{
  x = 14, y = y + 24, w = 110, text = "add a worker",
  on_click = add_worker,
})

win:add(ui.button{
  x = 134, y = y + 24, w = 110, text = "take one off",
  on_click = remove_worker,
})

--
-- Said plainly rather than left to be inferred from one bar.
--
-- Two labels and not one wrapped string: `ui.label` does not wrap, and a
-- line longer than the window is a line with its end cut off. `ui.text`
-- wraps and would be the widget for a paragraph; two lines are not a
-- paragraph.
--
local note = (CORES == 1)
  and { "One core, so a second worker makes this twice as slow",
        "rather than twice as fast. docs/smp.md is the plan." }
  or  { tostring(CORES) .. " cores. Add workers and watch them fill.", nil }

for i = 1, #note do
  win:add(ui.label{
    x = 14, y = y + 58 + (i - 1) * (gfx.font.h + 3),
    text = note[i], color = "text_dim",
  })
end

--------------------------------------------------------------------------

local sampler = ui.view{ x = 0, y = 0, w = 0, h = 0 }

function sampler:tick()
  local load = sys.cpuload()

  if load then
    for i = 1, #load do
      local was = last[i]

      if was then
        local di = load[i].idle - was.idle
        local db = load[i].busy - was.busy

        if di + db > 0 then
          pct[i] = (db * 100) // (di + db)
        end
      end

      last[i] = { idle = load[i].idle, busy = load[i].busy }
    end
  end

  -- Counted from the process list rather than from this program's own
  -- tally, so a worker that ended on its own is noticed.
  workers = #spin_ids()

  count.text = (workers == 0) and "no workers"
               or (tostring(workers) .. " worker"
                   .. ((workers == 1) and "" or "s") .. " running")
end

win:add(sampler)
win:run()
