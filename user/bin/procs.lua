-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: section system
-- kosmos: needs processes
-- Every process, and what it is costing.
--
-- BeOS's ProcessController, which put a bar beside each team and let you
-- see at a glance which one was eating the machine. The same idea and the
-- same reason: a number in a column tells you a process used forty-one
-- ticks, and a bar tells you which one to look at.
--
-- The numbers come from `sys.processes()`, the same call `htop` and `ps`
-- use. Nothing here is privileged - the kernel will tell any process what
-- the table looks like, because a process table is not authority: knowing
-- that something exists is not being able to reach it.

local ui = use("/lib/ui.lua")
-- The *kit's* palette, not a copy of it.
--
-- `use` runs the chunk again and hands back a different table, and only the
-- one `ui.lua` holds is the one it mutates when the desktop changes theme.
-- An application that loaded its own kept the colours it started with while
-- every widget around it changed - which is exactly what Monitor, Processes,
-- Photo and the Terminal did.
local theme = ui.theme

--
-- The counter's rate, read once.
--
-- `sys.ticks()` counts at whatever this board runs its counter at - 62.5
-- MHz under TCG, 24 under hvf, a thousand on the other machine - and there
-- is no ratio to carry in your head, which is why every correct piece of
-- counter arithmetic in this system reads the rate three lines above the
-- sum. `architecture.md` §5 is the whole account.
--
local counter_hz = (fs.read("/dev/cpu") or {}).counter_hz or 62500000

local W, H = 790, 482

-- The menu bar's height, which everything below it is offset by. A menu bar
-- is an ordinary widget in this window rather than a band the window
-- manager reserves, so the offset is this program's business - see
-- `ui.menubar`.
local BAR_H = gfx.font.h + 8
local ROW = gfx.font.h + 4

local win, err = ui.window{ title = "Processes", w = W, h = H, x = 150, y = 90 }

if not win then
  print("procs: " .. tostring(err))
  return
end

--------------------------------------------------------------------------
-- What kind of thing each process is.
--
-- The glossary says a server is a process that owns something and a kit is
-- code you run; the question a monitor can actually answer is the first
-- one, because owning is a fact the system reports and everything else is
-- a label somebody wrote down.
--
-- **The file says what it is, in its own header.** `kosmos: application`
-- already meant a window, and `kosmos: server` now means one that owns
-- something; a file that says neither is a console program. The program
-- store parses the header and hands it over as a `kind` attribute, which
-- is how the Deskbar has always decided what to list. A runner names
-- itself after the program it runs, so the name in the process table is
-- the file name in `/bin`.
--
-- What is left over - anything with no file in `/bin` - is one of the
-- servers init starts and keeps, because nothing else survives without
-- being a program somebody launched.
--
-- **What this deliberately does not do is guess from what a process
-- holds.** That was the first version and it was wrong twice over: the
-- shell is handed the screen so it can pass it to the desktop, and until
-- the change that went with this, *every* launched program was handed it
-- too. A grant says what something may do, not what it is.
--
-- **Drivers are missing from this list, and that is the true answer.**
-- Every driver in Kosmos is inside the kernel, in `hal/`, so no process is
-- one. The day virtio-gpu arrives in userland is the day this needs a
-- fourth kind, and until then saying "driver" would be inventing a row.
--------------------------------------------------------------------------

-- Asked once. `/bin` does not change while this runs, and a round trip
-- per row per second for an answer that never moves would be a lot of
-- messages to learn the same thing.
local from_bin = {}

do
  for _, file in ipairs(fs.list("/bin") or {}) do
    local attrs = fs.getattr("/bin/" .. file)
    local kind  = attrs and attrs.kind

    from_bin[(file:gsub("%.lua$", ""))] =
      (kind == "application") and "app" or kind or "program"
  end
end

local function kind_of(p)
  --
  -- The shell is the one thing a name has to answer for, and it is the one
  -- thing that cannot be asked: it is the only program that lives inside
  -- init rather than in `/bin`, so there is no file to have declared it.
  --
  if p.name == "shell" then
    return "program"
  end

  return from_bin[p.name] or "server"
end

local rows = {}          -- { name, kind, id, pct, pages, caps, owns, exited }
local totals = { procs = 0, threads = 0 }
local last = {}          -- ticks per process, from the previous sample
local last_total         -- idle + busy from the previous sample: the machine
local last_idle, last_busy

--
-- How each process gets its pixels, asked of the desktop.
--
-- Three answers, and the difference is the whole of `gfx.md` 19.4:
--
--   direct     it owns a region the compositor blits from. A whole surface
--              changes every frame and describing it would cost more than
--              copying it - a rendered scene, a video frame.
--   commands   it sends drawing commands and the compositor owns every
--              pixel. This is what lets a hung application keep a window.
--   (blank)    it has no window at all.
--
-- The kernel does not know this and should not: which of two ways an
-- application draws is a fact about the desktop, so the desktop is asked.
--
local video = {}

--
-- The scheduling bands, by name.
--
-- `sched.h` names five of eight and says anything unnamed is NORMAL. A
-- number in a column would be a number a person has to go and look up, and
-- the whole reason to show it is that the bands are the thing the scheduler
-- app changes and nothing showed what it had done.
--
local BANDS = { [0] = "idle", "low", "normal", "display", "input" }
-- The *process* that is selected, not the row.
--
-- The list is sorted by processor share and that order changes every
-- second, so a selected row number selects a different process each time it
-- is redrawn - you aim at one and end another. Following the id means the
-- highlight moves with the process as the list reorders around it.
local selected_id = nil
local selected = 1
local followed = nil     -- the selection the view last scrolled to
local top = 1            -- the first row drawn, for a list taller than the view

--------------------------------------------------------------------------
-- The table, drawn as one view.
--
-- One view rather than a widget per row, because the rows come and go with
-- the processes: building and destroying views every second to track a
-- list that changes would be a lot of work to look identical.
--------------------------------------------------------------------------

-- Follows all four edges, so it grows with the window. The first widget in
-- Kosmos to use a follow mode for real - see `ui.md` 16.4 for why that took
-- until something could be resized.
local table_view = ui.view{ x = 12, y = 112 + BAR_H, w = W - 24,
                            h = H - 158 - BAR_H,
                            follow = { "left", "right", "top", "bottom" } }

--
-- Where each column starts. One table, read by the heading and by the rows,
-- because two functions agreeing about geometry by coincidence is how a
-- column ends up labelled in one place and drawn in another.
--
local COLUMNS = {
  { x = 6,   text = "id  name" },
  { x = 190, text = "kind" },
  { x = 266, text = "draws" },
  { x = 356, text = "priority" },
  { x = 440, text = "memory" },
  { x = 516, text = "processor" },
}

function table_view:draw(g)
  g:fill(0, 0, self.w, self.h, "sunken")
  g:frame(0, 0, self.w, self.h, "line")

  --
  -- The headings, and the reason they are worth a row.
  --
  -- The band column has always been here - `idle`, `low`, `normal`,
  -- `display`, `input` - and nothing said what those words were. A column of
  -- unexplained adjectives is not information, and the scheduler app is the
  -- thing that changes them, so this is where you check that it worked.
  --
  for _, c in ipairs(COLUMNS) do
    g:text(c.x, 3, c.text, theme.text_dim, theme.sunken)
  end

  g:fill(2, 3 + ROW - 2, self.w - 4, 1, theme.line)

  -- One row shorter, because the heading took one.
  local visible = (self.h - 6) // ROW - 1

  --
  -- Scrolled to keep the selection in view, *and* draggable by its own
  -- handle. This said there was no scrollbar, on the grounds that a list
  -- following the selection never needs one - and that was true when it
  -- was written and false three lines later, once a pointer arrived and
  -- `ui.scrollbar` went in below. A list you can drag needs somewhere to
  -- drag it, and a list of thirty processes is longer than the window.
  --
  --
  -- Follow the selection when it moves, not on every pass. Unconditionally
  -- it makes the bar useless: select a row near the end, drag the bar up,
  -- and the next redraw pulls it straight back. Same bug `ui.list` had.
  --
  if selected ~= followed then
    if selected < top then
      top = selected
    elseif selected > top + visible - 1 then
      top = selected - visible + 1
    end

    followed = selected
  end

  if top > #rows - visible + 1 then top = #rows - visible + 1 end
  if top < 1 then top = 1 end

  -- The bar, and the rows stop where it starts.
  self.bar = ui.scrollbar(g, self.w, self.h, #rows, visible, top)
  self.visible = visible

  local room = self.w - 4 - (self.bar and ui.SCROLL_W + 2 or 0)

  -- Where a row actually ends. Rows start at x = 2 and are `room` wide, so
  -- this is the last column the scrollbar does not own.
  --
  -- Everything at the right-hand end used to be placed against `self.w`
  -- instead, which is the window - so the load bar ran under the scrollbar
  -- and the percentage was drawn on top of it. Unreadable, and worse than
  -- unreadable: the bar could not be clicked, because the thing you were
  -- aiming at had a number painted over it.
  local edge = 2 + room

  for i = top, math.min(top + visible - 1, #rows) do
    local r = rows[i]
    local y = 3 + (i - top + 1) * ROW
    local on = (i == selected)
    local bg = on and theme.accent or theme.sunken

    if on then
      g:fill(2, y, room, ROW, bg)
    end

    -- Name, then a bar, then the share. The bar is the point: BeOS put one
    -- beside every team for the same reason.
    local fg = on and theme.text_on or theme.text
    g:text(6, y + 2, ("%-3d %s"):format(r.id, r.name), r.exited
                                                       and theme.text_dim
                                                       or fg, bg)

    -- Dimmer than the name, because it is what the row *is* rather than
    -- what it is called, and the name is what you are looking for.
    g:text(190, y + 2, r.kind or "",
           on and theme.text_on or "text_dim", bg)

    -- How it draws, dimmer still: it is a property of the row rather than
    -- something you are looking for.
    g:text(266, y + 2, r.video or "",
           on and theme.text_on or "text_dim", bg)

    -- And the band it is scheduled in.
    g:text(356, y + 2, r.band or "",
           on and theme.text_on or "text_dim", bg)

    -- What it holds: the image, the heap, the stacks and any surface it
    -- asked for. Right-aligned, because a column of numbers is read down
    -- its last digit.
    if not r.synthetic then
      local kb = ("%d KB"):format(r.kb or 0)

      g:text(500 - gfx.measure(kb), y + 2, kb,
             on and theme.text_on or "text_dim", bg)
    end

    local bar_x = 516
    local bar_w = edge - bar_x - 60

    g:fill(bar_x, y + 3, bar_w, ROW - 6, "window")

    local filled = bar_w * r.pct // 100

    if filled > 0 then
      g:fill(bar_x, y + 3, filled, ROW - 6,
             (r.pct > 60) and theme.bad or "good")
    end

    -- Right-aligned against the row's end, and measured rather than
    -- counted: `#right * gfx.font.w` is the width this string would have
    -- in the terminal face, and the rows are not drawn in it.
    local right = r.exited and "gone" or ("%d%%"):format(r.pct)

    g:text(edge - gfx.measure(right) - 8, y + 2, right,
           on and theme.text_on or "text_dim", bg)
  end
end

--
-- Clicking picks a row, which is what the End button acts on.
--
function table_view:mouse(action, x, y)
  --
  -- The bar first, because it sits over the right-hand end of every row.
  -- Shared with `ui.list` rather than written again here: three scrollbars
  -- in one system would be three that drift apart.
  --
  local visible = self.visible or 1

  local to = ui.scrollbar_mouse(self, action, x, y, self.w, self.h,
                                #rows, visible, top)

  if to then
    top = to

    return true
  end

  if action == "press" or action == "move" then
    -- Less one, for the heading row the list now starts below. Without it
    -- every click selected the process one place further down than the one
    -- under the pointer.
    local row = (y - 3) // ROW + top - 1

    if row >= 1 and row <= #rows then
      selected = row
      selected_id = rows[row] and rows[row].id or nil
    end
  end

  return true
end

table_view.focusable = true

function table_view:key(c)
  if c == -1 then
    selected = math.max(1, selected - 1)
    selected_id = rows[selected] and rows[selected].id or nil
    return true
  end

  if c == -2 then
    selected = math.min(#rows, selected + 1)
    selected_id = rows[selected] and rows[selected].id or nil
    return true
  end
  return false
end

-- Declared before the menu bar refers to them and defined below: the menu
-- and the button do the same thing, and the same thing should be one
-- function rather than two that drift.
local end_selected
local sampler

--
-- A menu bar, and the first in Kosmos.
--
-- `Process > End` does what the button does, which is the point rather than
-- a duplication: a menu that only holds things with no other way to reach
-- them is a menu nobody learns. Every desktop puts its common actions in
-- both places.
--
win:add(ui.menubar{
  x = 0, y = 0, w = W,
  menus = {
    { title = "Process",
      items = {
        { text = "End",     on_choose = function() end_selected() end },
        { separator = true },
        { text = "Refresh", on_choose = function() sampler:tick() end },
      } },
    { title = "View",
      items = {
        { text = "Busiest first", on_choose = function() end },
        { text = "By id",         on_choose = function() end },
      } },
  },
})

win:add(table_view)

--------------------------------------------------------------------------
-- What the machine has left, above the list of what is using it.
--
-- **These came from `sysmon`, and they are in the right place now.** That
-- window was five unrelated numbers keeping one another company because
-- they happened to arrive in the same `/dev/kernel` reply - and it has
-- become the processor monitor alone, which is one thing rather than six.
--
-- A total belongs beside the detail it is the total *of*. The table under
-- these says which process holds how much memory; the bar says how much
-- there is. "Seventeen of thirty-two processes" is a sentence about the row
-- count directly beneath it, and reading them apart, in two windows, was
-- always a small act of arithmetic nobody should have been doing.
--
-- Drawn as a continuous fill and not as the segments a processor gets, and
-- that difference is deliberate: `/lib/pulse.lua` says why. A processor is
-- *watched* and wants to show change; a pool is *read* and wants to show a
-- level.
--------------------------------------------------------------------------

local totals_state = {
  used_mb = 0, total_mb = 1,
  threads = 0, threads_max = 1,
  processes = 0, processes_max = 1,
  endpoints = 0, endpoints_max = 1,
  spaces = 0, spaces_max = 1,
}

local function meter(spec)
  local v = ui.view{ x = spec.x, y = spec.y, w = spec.w, h = 34 }

  v.label = spec.label
  v.read  = spec.read

  function v:draw(g)
    local value, of, text = self.read()
    local frac = (of > 0) and (value / of) or 0

    if frac < 0 then frac = 0 end
    if frac > 1 then frac = 1 end

    g:text(0, 0, self.label, "text_dim")

    local right = text or (tostring(value) .. " of " .. tostring(of))
    g:text(self.w - #right * gfx.font.w, 0, right, "text")

    local top = gfx.font.h + 4

    g:fill(0, top, self.w, 10, "sunken")
    g:frame(0, top, self.w, 10, "line")

    local filled = (self.w - 2) * frac // 1

    if filled > 0 then
      g:fill(1, top + 1, filled, 8, "accent")
    end
  end

  return v
end

do
  local n    = 4
  local gap  = 14
  local each = (W - 24 - gap * (n - 1)) // n
  local ty   = 70 + BAR_H

  local rows = {
    { "memory", function()
        return totals_state.used_mb, totals_state.total_mb,
               ("%d of %d MB"):format(totals_state.used_mb,
                                      totals_state.total_mb)
      end },
    { "threads", function()
        return totals_state.threads, totals_state.threads_max
      end },
    { "processes", function()
        return totals_state.processes, totals_state.processes_max
      end },
    { "endpoints", function()
        return totals_state.endpoints, totals_state.endpoints_max
      end },
  }

  for i = 1, n do
    win:add(meter{ x = 12 + (i - 1) * (each + gap), y = ty, w = each,
                   label = rows[i][1], read = rows[i][2] })
  end
end

local heading = ui.label{ x = 12, y = 12 + BAR_H, text = "", color = "text" }
win:add(heading)

local note = ui.label{ x = 12, y = 16, text = "", color = "text_dim" }

--------------------------------------------------------------------------
-- Ending one.
--
-- **This program may end anything, and that is the point of it.**
--
-- `-- kosmos: needs processes` in the header becomes `SPAWN_PROCCTL`, which
-- becomes `owns_procctl`, which makes `SYS_KILL` take the
-- `process_kill_any` branch instead of the parent-only one. So the button
-- works on a process this program did not start, which is every process on
-- the machine and is what a task manager is for.
--
-- The comment here used to say the opposite - "only what this process
-- started may be ended by it, which is nothing" - and it was describing the
-- program before the grant existed.
--------------------------------------------------------------------------

-- In the bar at the top, with the path and the heading, rather than under
-- the list. `ui.md` 16.10: the verbs go next to the noun, and the bottom
-- edge is where a window's size is least certain.
function end_selected()
    local r = rows[selected]

    if not r then return end

    --
    -- **There used to be a refusal here and it refused everything.**
    --
    -- It declined to end any process holding `OWNS_CONSOLE` or
    -- `OWNS_SCREEN`, on the reasoning that the one with the screen is the
    -- desktop and ending it takes the session down. The reasoning was
    -- sound; the premise was not. `init.lua` hands `SPAWN_SCREEN` to
    -- *every* program it launches - its own comment calls that "the screen
    -- to everything, which is wrong and is staying for now" - so
    -- `OWNS_SCREEN` is set on all of them, and a guard meant for the window
    -- manager rejected `spin`.
    --
    -- What it looked like from the outside was an End button that never did
    -- anything, on the one program in the system whose entire job is ending
    -- things, with a message explaining that a compute worker holds the
    -- screen.
    --
    -- **So it is gone, and this is an administrator's tool.** It ends what
    -- it is pointed at. The two real protections are elsewhere and are
    -- enough: the kernel refuses a process with no parent, which is init;
    -- and ending the desktop is a thing a person can mean, on a machine
    -- with a serial console and a reset button, in a system whose whole
    -- argument is that you are allowed to look at how it works.
    --
    -- A guard that fires on everything protects nothing and teaches the
    -- user that the button is broken.
    --
    -- `sys.kill` first, which with the grant above reaches any process; the
    -- desktop is asked only if the kernel says no, for a process it started
    -- and this one somehow cannot name.
    local killed, why_not = sys.kill(r.id)

    if killed then
      note.text = "ended " .. r.name
      return
    end

    local ok, why = fs.send("/app/wm", { type = "end_process", pid = r.id })

    if ok then
      note.text = "asked the desktop to end " .. r.name
    else
      note.text = r.name .. ": " .. tostring(why_not or why)
    end
end

-- The button, which is the same action under a different control.
win:add(ui.button{
  x = 12, y = 38 + BAR_H, w = 60, h = 24, text = "End",
  on_click = end_selected,
})

note.x = 130
note.y = H - 30
win:add(note)

--------------------------------------------------------------------------
-- Sampling.
--
-- A share of the processor is the difference between two readings, the same
-- as everywhere else here: a single number says what fraction of all time
-- since boot a process was running, which stops moving after a minute.
--------------------------------------------------------------------------

sampler = ui.view{ x = 0, y = 0, w = 0, h = 0 }

function sampler:tick()
  local k = fs.read("/dev/kernel")
  local list = sys.processes()

  -- Which processes have windows, and how those windows draw.
  video = {}

  local desktop = fs.send("/app/wm", { type = "windows" })

  for _, w in ipairs(desktop and desktop.windows or {}) do
    if w.pid then
      -- A process with two windows counts as direct if either of them is:
      -- what the column answers is "does this map the framebuffer", and one
      -- direct window is enough for that to be true.
      if w.direct or video[w.pid] == "direct" then
        video[w.pid] = "direct"
      else
        video[w.pid] = "commands"
      end
    end
  end

  if not list then return end

  local now = {}

  for _, p in ipairs(list) do now[p.id] = p.ticks end

  --
  -- A share of the *machine*, not a share of the work that happened.
  --
  -- This used to divide each process's ticks by the sum of every process's
  -- ticks, which makes the column always add up to a hundred whatever the
  -- machine is doing. On an idle desktop that reads "100%" beside whichever
  -- process did the small amount of work there was - while `sysmon`, two
  -- windows away, correctly said the processor was one per cent busy. Both
  -- numbers were right and one of them was a lie, because the column is
  -- headed with a percentage and a person reads that as "of the processor".
  --
  -- The denominator is now every tick that passed, idle ones included,
  -- which is exactly what `sysmon` divides by. The two agree now, and a
  -- process at 100% here is a process actually eating the machine.
  --
  -- Elapsed ticks come from the kernel rather than from a clock read here,
  -- so a slow pass does not turn into a spike: the numerator and the
  -- denominator are counted by the same interrupt.
  --
  local elapsed = 0

  if k and last_total then
    elapsed = (k.idle_ticks + k.busy_ticks) - last_total
  end

  if k then last_total = k.idle_ticks + k.busy_ticks end

  local fresh = {}
  local charged = 0

  for _, p in ipairs(list) do
    local delta = p.ticks - (last[p.id] or p.ticks)

    charged = charged + delta

    fresh[#fresh + 1] = {
      id = p.id, name = p.name, pages = p.pages, caps = p.caps,
      owns = p.owns, kind = kind_of(p), video = video[p.id],
      band = BANDS[p.priority or 2] or tostring(p.priority),
      kb = ((p.held or p.pages or 0) * 4096) // 1024,
      exited = p.exited,
      pct = (elapsed > 0) and math.min(100, delta * 100 // elapsed) or 0,
    }
  end

  --
  -- Where the rest of the machine went.
  --
  -- Two rows that are not processes, and saying so is the point rather than
  -- a caveat. Everything above them runs at EL0; these two are the time the
  -- machine spent somewhere a process cannot be:
  --
  --   kernel   threads Nebula owns - not the idle one. The busy ticks the
  --            kernel counted, less every tick charged to a process.
  --   idle     nothing wanted the processor.
  --
  -- No kernel change was needed for this. The kernel already counts idle
  -- and busy, and the difference between busy and the sum of the processes
  -- is exactly what ran in the kernel and was not idle. It also makes the
  -- column add up: if these two and the processes do not come to a hundred,
  -- one of the three is wrong, and that is worth being able to see.
  --
  if k and elapsed > 0 then
    local idle_delta = (last_idle and (k.idle_ticks - last_idle)) or 0
    local busy_delta = (last_busy and (k.busy_ticks - last_busy)) or 0
    local in_kernel = busy_delta - charged

    if in_kernel < 0 then in_kernel = 0 end

    fresh[#fresh + 1] = { id = 0, name = "kernel", kind = "threads",
                          band = "", video = "", kb = 0,
                          pct = in_kernel * 100 // elapsed, synthetic = true }

    fresh[#fresh + 1] = { id = 0, name = "idle", kind = "", band = "",
                          video = "", kb = 0,
                          pct = idle_delta * 100 // elapsed, synthetic = true }
  end

  if k then last_idle, last_busy = k.idle_ticks, k.busy_ticks end

  -- Busiest first, which is what a list like this is for.
  table.sort(fresh, function(a, b)
    if a.pct ~= b.pct then return a.pct > b.pct end
    return a.id < b.id
  end)

  rows = fresh
  last = now

  -- Find where the selected process ended up in the new order. It may have
  -- exited, in which case the row number is kept and whatever is there now
  -- becomes the selection - which is the least surprising thing available.
  if selected_id then
    for i, r in ipairs(rows) do
      if r.id == selected_id then selected = i break end
    end
  end

  if selected > #rows then selected = #rows > 0 and #rows or 1 end

  if not selected_id then
    selected_id = rows[selected] and rows[selected].id or nil
  end

  totals.procs = #rows
  totals.threads = k and k.threads or 0

  -- What is in this list and what is not.
  --
  -- **Every row is a process at user level.** There is no such thing here
  -- as a process running in the kernel: Nebula has threads of its own - the
  -- idle thread among them - and they are not processes and do not appear.
  -- Saying so is more useful than a column that reads "user" on every line,
  -- and it is the microkernel's shape stated out loud: the filesystem, the
  -- console and the desktop are all in this list, and the kernel is not.
  -- Short enough to fit the window, which the first version was not.
  --
  -- It said "at EL0", which is an AArch64 exception level and means ring 3
  -- on the machine this was read on. The distinction is real and worth
  -- drawing; the name for it was one architecture's.
  --
  -- The machine's own totals, which used to be a second window.
  --
  -- `/dev/memory` is a second read and it is worth it: this sampler already
  -- reads `/dev/kernel` every tick for the idle and busy counters, and the
  -- memory node is the only other place the free page count lives.
  --
  local m = fs.read("/dev/memory")

  if k then
    totals_state.threads,   totals_state.threads_max   = k.threads, k.threads_max
    totals_state.processes, totals_state.processes_max = k.processes, k.processes_max
    totals_state.endpoints, totals_state.endpoints_max = k.endpoints, k.endpoints_max
    totals_state.spaces,    totals_state.spaces_max    = k.spaces, k.spaces_max
  end

  if m then
    totals_state.used_mb  = m.total_mb - m.free_mb
    totals_state.total_mb = m.total_mb
  end

  local up = sys.ticks() // counter_hz

  heading.text = ("%d processes, %d threads; %d in the kernel, drivers too"
                  .. "   -   %d address space%s, up %d:%02d")
                 :format(totals.procs, totals.threads,
                         math.max(0, totals.threads - totals.procs),
                         totals_state.spaces,
                         (totals_state.spaces == 1) and "" or "s",
                         up // 60, up % 60)
end

win:add(sampler)
win:run()
