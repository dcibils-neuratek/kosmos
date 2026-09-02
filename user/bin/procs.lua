-- kosmos: application
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

local W, H = 520, 420
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
-- The *process* that is selected, not the row.
--
-- The list is sorted by processor share and that order changes every
-- second, so a selected row number selects a different process each time it
-- is redrawn - you aim at one and end another. Following the id means the
-- highlight moves with the process as the list reorders around it.
local selected_id = nil
local selected = 1
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
local table_view = ui.view{ x = 12, y = 70, w = W - 24, h = H - 116,
                            follow = { "left", "right", "top", "bottom" } }

function table_view:draw(g)
  g:fill(0, 0, self.w, self.h, "sunken")
  g:frame(0, 0, self.w, self.h, "line")

  local visible = (self.h - 6) // ROW

  --
  -- Scrolled to keep the selection in view rather than by its own handle.
  -- There is no scrollbar because there is nothing to drag it with that
  -- the arrow keys do not already do, and a list that follows what you
  -- selected never needs one - the selection *is* the scroll position.
  --
  if selected < top then
    top = selected
  elseif selected > top + visible - 1 then
    top = selected - visible + 1
  end

  if top > #rows - visible + 1 then top = #rows - visible + 1 end
  if top < 1 then top = 1 end

  for i = top, math.min(top + visible - 1, #rows) do
    local r = rows[i]
    local y = 3 + (i - top) * ROW
    local on = (i == selected)
    local bg = on and theme.accent or theme.sunken

    if on then
      g:fill(2, y, self.w - 4, ROW, bg)
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

    local bar_x = 262
    local bar_w = self.w - bar_x - 60

    g:fill(bar_x, y + 3, bar_w, ROW - 6, "window")

    local filled = bar_w * r.pct // 100

    if filled > 0 then
      g:fill(bar_x, y + 3, filled, ROW - 6,
             (r.pct > 60) and theme.bad or "good")
    end

    local right = r.exited and "gone" or ("%d%%"):format(r.pct)
    g:text(self.w - #right * gfx.font.w - 8, y + 2, right,
           on and theme.text_on or "text_dim", bg)
  end
end

--
-- Clicking picks a row, which is what the End button acts on.
--
function table_view:mouse(action, x, y)
  if action == "press" or action == "move" then
    local row = (y - 3) // ROW + top

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

win:add(table_view)

local heading = ui.label{ x = 12, y = 12, text = "", color = "text" }
win:add(heading)

local note = ui.label{ x = 12, y = 16, text = "", color = "text_dim" }

--------------------------------------------------------------------------
-- Ending one.
--
-- Only what this process started may be ended by it, which is nothing: the
-- kernel allows a kill from a parent and this is nobody's parent. So the
-- button says what it can and cannot do rather than failing quietly, which
-- is the honest version of a control that is present because it looks like
-- it should be.
--------------------------------------------------------------------------

-- In the bar at the top, with the path and the heading, rather than under
-- the list. `ui.md` 16.10: the verbs go next to the noun, and the bottom
-- edge is where a window's size is least certain.
win:add(ui.button{
  x = 12, y = 38, w = 60, h = 24, text = "End",
  on_click = function()
    local r = rows[selected]

    if not r then return end

    -- Some processes are the ground you are standing on.
    --
    -- The grant makes ending *anything* possible, which is what a task
    -- manager needs and is also enough rope to end the desktop you are
    -- clicking in, or the console every program prints to. Both take the
    -- whole session with them and neither is what anybody meant.
    --
    -- The check is what a process *holds*, not what it is called: the one
    -- with the screen is the desktop whatever it was named, and a list of
    -- trusted names would be wrong the first time somebody wrote another
    -- window manager.
    --
    -- init is refused by the kernel and does not need a check here.
    local OWNS_CONSOLE, OWNS_SCREEN = 1, 2

    if r.owns and (r.owns & (OWNS_CONSOLE | OWNS_SCREEN)) ~= 0 then
      note.text = r.name .. " holds the screen or the console; ending it "
                  .. "would take the session with it"
      return
    end

    -- Through the desktop, because it started the applications and only a
    -- parent may end a child. `sys.kill` is tried first for the case this
    -- program ever does have children of its own.
    if sys.kill(r.id) then
      note.text = "ended " .. r.name
      return
    end

    local ok, why = fs.send("/dev/wm", { type = "end_process", pid = r.id })

    if ok then
      note.text = "asked the desktop to end " .. r.name
    else
      note.text = r.name .. ": " .. tostring(why)
    end
  end,
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

local sampler = ui.view{ x = 0, y = 0, w = 0, h = 0 }

function sampler:tick()
  local k = fs.read("/dev/kernel")
  local list = sys.processes()

  if not list then return end

  local now = {}
  local moved = 0

  for _, p in ipairs(list) do
    now[p.id] = p.ticks
    moved = moved + (p.ticks - (last[p.id] or p.ticks))
  end

  local fresh = {}

  for _, p in ipairs(list) do
    local delta = p.ticks - (last[p.id] or p.ticks)

    fresh[#fresh + 1] = {
      id = p.id, name = p.name, pages = p.pages, caps = p.caps,
      owns = p.owns, kind = kind_of(p),
      exited = p.exited,
      pct = (moved > 0) and (delta * 100 // moved) or 0,
    }
  end

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
  -- **Every row is a process at EL0.** There is no such thing here as a
  -- process running in the kernel: Nebula has threads of its own - the idle
  -- thread among them - and they are not processes and do not appear.
  -- Saying so is more useful than a column that reads "user" on every line,
  -- and it is the microkernel's shape stated out loud: the filesystem, the
  -- console and the desktop are all in this list, and the kernel is not.
  -- Short enough to fit the window, which the first version was not.
  heading.text = ("%d at EL0, %d threads; %d in the kernel, drivers too")
                 :format(totals.procs, totals.threads,
                         math.max(0, totals.threads - totals.procs))
end

win:add(sampler)
win:run()
