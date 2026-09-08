-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: section system
-- kosmos: needs processes
-- What every processor in this machine is doing, and a way to give them
-- something to do.
--
-- A bar per processor, live, and two buttons that add and remove a
-- compute-bound process. Put a worker on and watch a bar fill; put another
-- on and watch what happens to the second bar. **That second bar moving is
-- the whole point of this program**, and today it cannot.
--
--------------------------------------------------------------------------
-- Why this exists before the thing it shows
--
-- **Kosmos schedules on one processor and the machine has four.** The other
-- three are not in firmware: `docs/smp.md` step three starts them, each
-- claims its own `struct percpu` through its own `TPIDR_EL1`, and each then
-- sits in `wfi` for ever. They are in the kernel and they run no threads.
--
-- So all four get a row and only one of them gets a number. A parked
-- processor's `idle_ticks` and `busy_ticks` are zero because nothing ever
-- wrote them - the timer interrupt does not reach it - and a bar sitting at
-- 0% would claim that processor was measured and found idle. Its chip is
-- dark and its segments are unlit, which says the other thing.
--
-- Step four is the one that makes a second row move.
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
local pulse = use("/lib/pulse.lua")

--
-- Two numbers, and the rows are the larger one.
--
-- `cpus_present` is what the machine has and `cpus` is what this kernel
-- schedules on. **A row per processor that exists**, because the three that
-- are parked are the subject of this program rather than an omission from
-- it - the same call `sysmon` makes, and the reason the chip beside each
-- bar is lit or dark.
--
local info       = sys.info() or {}
local SCHEDULING = info.cpus or 1
local CORES      = info.cpus_present or SCHEDULING

--------------------------------------------------------------------------
-- The panel is `/lib/pulse.lua`, which `sysmon` draws too.
--
-- Identity box, one segmented bar per processor, a numbered chip on each.
-- That library says why the layout is BeOS's and why it is worth copying;
-- what this program adds is the two buttons under it, which are the whole
-- difference between the monitor you leave open and the one you open to
-- find something out.
--------------------------------------------------------------------------

local ident = pulse.identity()

local W = 470
local H = 48 + pulse.height(CORES, #ident) + 62

local win, err = ui.window{ title = "Cores", w = W, h = H, x = 120, y = 100 }

if not win then
  print("cores: " .. tostring(err))
  return
end

local pct  = {}
local last = {}

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

--
-- **A control that fails silently is a control that lies.**
--
-- This used to be `if run(...) then workers = workers + 1 end`, so a
-- refusal looked exactly like a click that never landed - and when one
-- actually started refusing, an afternoon went into the pointer, the
-- button's hit box and the window's coordinates before anybody asked the
-- one question the program could have answered by itself.
--
local why = nil

local function add_worker()
  -- A long spin, so it outlives a look. It is killed rather than waited
  -- out; `spin 600` is ten minutes and nobody watches for ten minutes.
  local ok, err = run("/bin/spin.lua", "600", true)

  if ok then
    workers = workers + 1
    why = nil
  else
    why = tostring(err or "run refused")
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

--
-- **The controls go above the panel, and that is a decision the test
-- forced.**
--
-- Underneath, their y depended on how many processors the machine has -
-- four rows on this board and one on the other - so the display harness,
-- which drives a real pointer at real coordinates, needed a different
-- number per board to click the same button. A control whose position
-- depends on the data above it is a control nothing can reliably aim at,
-- and that is true of a person on a strange machine as much as of a test.
--
-- Above, they are at a fixed offset from the window's own corner on every
-- machine, at any core count. A row of controls across the top is Tracker's
-- shape anyway.
--
win:add(ui.button{
  x = 14, y = 14, w = 110, h = 24, text = "add a worker",
  on_click = add_worker,
})

win:add(ui.button{
  x = 134, y = 14, w = 110, h = 24, text = "take one off",
  on_click = remove_worker,
})

local count = ui.label{ x = 258, y = 20, text = "no workers",
                        color = "text_dim" }
win:add(count)

win:add(pulse.panel{
  x = 14, y = 48, w = W - 28,
  cores = CORES, scheduling = SCHEDULING, ident = ident,
  read = function(c) return pct[c] end,
})

local y = 48 + pulse.height(CORES, #ident)

--
-- Said plainly rather than left to be inferred from a dark chip.
--
-- Two labels and not one wrapped string: `ui.label` does not wrap, and a
-- line longer than the window is a line with its end cut off. `ui.text`
-- wraps and would be the widget for a paragraph; two lines are not one.
--
local note

if CORES > SCHEDULING then
  -- Kept inside the window on purpose: `ui.label` does not wrap, and the
  -- first version of this line ran off the right edge and ended in "kern".
  note = { ("%d processors, %d scheduling. The other %d are parked")
           :format(CORES, SCHEDULING, CORES - SCHEDULING),
           "in wfi; docs/smp.md step 4 gives them threads." }
elseif CORES == 1 then
  note = { "One core, so a second worker makes this twice as slow",
           "rather than twice as fast. docs/smp.md is the plan." }
else
  note = { tostring(CORES) .. " cores. Add workers and watch them fill." }
end

for i = 1, #note do
  win:add(ui.label{
    x = 14, y = y + 10 + (i - 1) * (gfx.font.h + 3),
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

  count.text = why
               or ((workers == 0) and "no workers"
                   or (tostring(workers) .. " worker"
                       .. ((workers == 1) and "" or "s") .. " running"))
end

win:add(sampler)
win:run()
