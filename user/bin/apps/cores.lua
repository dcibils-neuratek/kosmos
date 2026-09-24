-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Pulse
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
-- **This header described a machine that no longer exists, and the version
-- it described is worth keeping one line of: all four processors used to
-- start, claim a `struct percpu`, and sit in `wfi` for ever.**
--
-- They do not. Since `docs/smp.md` step five each has its own runqueue,
-- idle thread and timer, and runs the same loop core zero runs. All four
-- take ticks, so all four have real `idle_ticks` and `busy_ticks` and every
-- row is measured rather than assumed.
--
-- What decides whether a row *moves* is placement, which is a policy and is
-- off by default: `make SMPWORK=4 qemu` spreads new threads over all four.
-- A dark chip still means a processor that never reached the kernel, which
-- is a different statement from one that is measured and idle - and on this
-- machine there are none.
--
-- So this is an instrument built before the experiment, which is what this
-- project does: `jitter` measured the noise floor before anybody optimised
-- against it, and `frames` measured where a pass went before anybody
-- rewrote the window manager. **You cannot see SMP arrive without something
-- that would show it.**
--
-- And it is honest rather than aspirational. With placement off it says one
-- core in words at the bottom, and two workers produce one full bar and a
-- machine exactly twice as slow. With placement on it currently shows the
-- open bug rather than a working machine: three bars carry work and one
-- will not rise, because only core zero preempts. `docs/smp.md` has the
-- measurement. Watching that is the point of having built this first.
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
local ONLINE     = info.cpus_online or SCHEDULING
local CORES      = info.cpus_present or ONLINE

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

--
-- Sized from what is in it: the header, the page's top margin, the panel,
-- two lines of note and the page's foot - so a machine with one core and
-- one with eight both get a window with the drawings' margins all round.
--
local L = ui.layout
local W = 520

local function height()
  return L.head + L.page_top + pulse.height(CORES, #ident)
         + 10 + 2 * (gfx.height() + 3) + L.page_foot
end

local H = height()

local win, err = ui.window{ title = "Cores", w = W, h = H, x = 120, y = 100 }

if not win then
  print("cores: " .. tostring(err))
  return
end

--
-- **Measured again once the window exists**, because the look arrives with
-- it: whether the panel is the flat look's card of rows or the Pulse recess
-- is the look's to say, and a window sized before it knew was a card
-- running off its own bottom edge.
--
if height() ~= H then
  H = height()
  win:resize(W, H)
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

--
-- **`exited` is the whole of this function, and leaving it out broke the
-- button in a way that looked like the button.**
--
-- `sys.processes()` lists a process until it is reaped, and a killed one is
-- reaped some time later - so a dead `spin` keeps its row, its name and its
-- id. Counting by name alone therefore counted the dead, and two things
-- followed, neither of which looks like a counting bug from the outside:
--
--   - the label never came down. Five workers ended and it still said five,
--     because `tick` recounts from this list every pass.
--   - and worse, `remove_worker` takes `ids[#ids]` - the last - which after
--     the first kill is a process that has already exited. `sys.kill`
--     answers *true* for one of those (the kernel's "already gone; nothing
--     to do"), so the button reported success, decremented nothing that
--     stayed decremented, and left every live worker running.
--
-- So "take one off" appeared to do nothing at all, while doing exactly what
-- it was told to a process that was already dead.
--
--
-- **And `dying` is the other half, because `exited` arrives late.**
--
-- A kill is a *mark*: `process_kill` sets a flag and the process dies on its
-- own next entry into the kernel, which is a syscall or the next timer tick.
-- Until it does, its row still says `exited=false` - so a second click a
-- moment later finds the same process at the end of the list and kills it
-- again. `sys.kill` says true, nothing new stops, and clicking faster makes
-- it worse rather than better.
--
-- Which is the same failure as counting the dead, one window earlier, and it
-- is the one a person actually meets: nobody waits five seconds between
-- clicks on a button called "take one off".
--
-- So an id this program has already asked to stop is not a worker any more,
-- whatever the process table still says. Entries are dropped when the row
-- goes, which is when the parent reaps it.
--
local dying = {}

local function spin_ids()
  local list = sys.processes and sys.processes() or nil
  local ids = {}
  local seen = {}

  if not list then return ids end

  for i = 1, #list do
    if list[i].name == "spin" then
      seen[list[i].id] = true

      if not list[i].exited and not dying[list[i].id] then
        ids[#ids + 1] = list[i].id
      end
    end
  end

  -- Whatever is no longer listed at all has been reaped; stop remembering
  -- it, so an id the kernel later hands to a new process is not skipped.
  for id in pairs(dying) do
    if not seen[id] then dying[id] = nil end
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

  if #ids == 0 then
    why = "no workers to take off"
    return
  end

  local id = ids[#ids]
  local ok, err = sys.kill(id)

  if ok then
    dying[id] = true
    why = nil
    workers = workers - 1

    if workers < 0 then workers = 0 end
  else
    -- The same rule `add_worker` learned: a control that fails silently is
    -- a control that lies.
    why = tostring(err or "kill refused")
  end
end

--------------------------------------------------------------------------

--
-- **The controls are the header's** (`docs/apps.html`, `roadmap.md` 5zp):
-- Add a worker as the verb that starts something, Take one off beside it,
-- and how many are running where the window's subject goes.
--
-- They were a row above the panel, and the reason they were above rather
-- than under still holds: under the panel, their position depended on how
-- many processors the machine has, and nothing can aim at a control that
-- moves with the data. A header is at the window's own top on every
-- machine - and the display harness now drives them with the keyboard,
-- which aims at nothing at all.
--
-- The part a person is looking for: "Cortex-A72 r0p3", not the vendor alone.
local processor = table.concat({ ident[2] or "", ident[3] or "" }, " ")
                  :match("^%s*(.-)%s*$")

local header = ui.header{
  x = 0, y = 0, w = W, title = "Cores", sub = "no workers",
  right = { ui.button{ text = "Add a worker", go = true,
                       on_click = add_worker },
            ui.button{ text = "Take one off", on_click = remove_worker } },
}

win:add(header)

win:add(pulse.panel{
  x = L.page_side, y = L.head + L.page_top, w = W - 2 * L.page_side,
  cores = CORES, online = ONLINE, scheduling = SCHEDULING,
  ident = ident,
  read = function(c) return pct[c] end,
})

local y = L.head + L.page_top + pulse.height(CORES, #ident)

--
-- Said plainly rather than left to be inferred from a dark chip - as the
-- page's note, 10 under what it is about, in the dim `ui` face.
--
-- Two labels and not one wrapped string: `ui.label` does not wrap, and a
-- line longer than the window is a line with its end cut off.
--
local note

if ONLINE > SCHEDULING then
  note = { ("%d processors, all ticking. %d given work;")
           :format(ONLINE, SCHEDULING),
           "run with SMPWORK=4 to place threads on all of them." }
elseif CORES > ONLINE then
  note = { ("%d processors, %d in the kernel. The other %d are still")
           :format(CORES, ONLINE, CORES - ONLINE),
           "in firmware; docs/smp.md is what it takes to start them." }
elseif CORES == 1 then
  note = { "One core, so a second worker makes this twice as slow",
           "rather than twice as fast. docs/smp.md is the plan." }
else
  note = { tostring(CORES) .. " cores. Add workers and watch them fill." }
end

for i = 1, #note do
  win:add(ui.label{
    x = L.page_side + 3, y = y + 10 + (i - 1) * (gfx.height() + 3),
    w = W - 2 * L.page_side - 3,
    text = note[i], color = "text_dim", role = "ui",
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

  --
  -- What processor this is and how busy it has been told to be, beside the
  -- title - the drawing's header, and where the flat look's card leaves the
  -- processor's name, since it draws no identity box.
  --
  local busy = why or ((workers == 0) and "no workers"
                       or (tostring(workers) .. " worker"
                           .. ((workers == 1) and "" or "s") .. " running"))

  header.sub = (processor ~= "" and (processor .. " · ") or "") .. busy
end

win:add(sampler)
win:run()
