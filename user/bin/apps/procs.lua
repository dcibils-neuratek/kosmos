-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon TeamIcon
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

local W, H = 850, 482

-- **As `docs/apps.html` draws it** (`roadmap.md` 5zp): the kit's header
-- with what the machine is doing beside the title and End and the dots at
-- its end; the four pools under it in a band 14 above and 10 below, 18 in;
-- and the table from there to the window's edges, a heading row and rows
-- the fixed layout's height. It was a row 33 tall, meters with a framed
-- well, a sunken box 12 in, and a status line along the bottom.
--
local L = ui.layout
local BAND_TOP, BAND_SIDE, BAND_FOOT = 14, 18, 10
local METER_H = 35                       -- a line, 6, and the bar's 8

local win, err = ui.window{ title = "Processes", w = W, h = H, x = 150, y = 90 }

-- After the window, so the faces are the look's: the band holds a line of
-- words over its bars.
local LIST_Y = L.head + BAND_TOP + METER_H + BAND_FOOT
local ROW = ui.metrics.row
local PAD = 14                           -- the table's cells, in from its edges

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
local share = use("/lib/procshare.lua")
local sampled = {}       -- what `share.rows` keeps from one sample to the next

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

-- Which order the list is in: "busy" or "id". The `...` menu sets it.
local order = "busy"
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
local table_view = ui.view{ x = 0, y = LIST_Y, w = W, h = H - LIST_Y,
                            follow = { "left", "right", "top", "bottom" } }

--
-- The columns: a name and a width each, and the name's column takes what is
-- left. One table, read by the heading and by the rows, because two
-- functions agreeing about geometry by coincidence is how a column ends up
-- labelled in one place and drawn in another. `right` is a column of
-- numbers, read down its last digit.
--
local GAP = 12

local COLUMNS = {
  { key = "id",        text = "id",        w = 36 },
  { key = "name",      text = "name",      w = 0 },
  { key = "kind",      text = "kind",      w = 70 },
  { key = "draws",     text = "draws",     w = 76 },
  { key = "priority",  text = "priority",  w = 70 },
  --
  -- **"core" is a fact, not a sample, and that is why it is worth a
  -- column.**
  --
  -- On a system that migrates threads this would be the core it happened to
  -- be on when the list was built, different on the next pass and useless
  -- to look at. Kosmos does not migrate: `t->sched.cpu` is set once when
  -- the thread is created and never changes, so this is where the process
  -- lives for its whole life. `docs/smp.md` has why the kernel is built
  -- that way and what it buys.
  --
  { key = "core",      text = "core",      w = 40 },
  { key = "memory",    text = "memory",    w = 80, right = true },
  { key = "processor", text = "processor", w = 120 },
}

-- Where each column starts for a row `room` wide.
local function columns(room)
  local fixed = 0

  for _, c in ipairs(COLUMNS) do fixed = fixed + c.w end

  local x = PAD
  local name_w = math.max(60, room - 2 * PAD - fixed - GAP * (#COLUMNS - 1))

  for _, c in ipairs(COLUMNS) do
    c.x = x
    c.cw = (c.key == "name") and name_w or c.w
    x = x + c.cw + GAP
  end
end

local function fitted(text, w)
  text = tostring(text or "")

  while #text > 1 and gfx.measure(text) > w do text = text:sub(1, -2) end

  return text
end

function table_view:draw(g)
  g:fill(0, 0, self.w, self.h, theme.sunken)
  g:fill(0, 0, self.w, 1, theme.line_soft)

  --
  -- The headings, and the reason they are worth a row.
  --
  -- The band column has always been here - `idle`, `low`, `normal`,
  -- `display`, `input` - and nothing said what those words were. A column of
  -- unexplained adjectives is not information, and the scheduler app is the
  -- thing that changes them, so this is where you check that it worked.
  --
  -- One row shorter, because the heading takes one.
  local visible = math.max(1, (self.h - 1 - ROW) // ROW)

  --
  -- Scrolled to keep the selection in view, *and* draggable by its own
  -- handle. Follow the selection when it moves, not on every pass:
  -- unconditionally it makes the bar useless - select a row near the end,
  -- drag the bar up, and the next redraw pulls it straight back. Same bug
  -- `ui.list` had.
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

  -- The bar runs beside the rows and not the heading, and the rows stop
  -- where it starts, or the last column of every row would be drawn
  -- underneath it.
  local saved = g:push(0, 1 + ROW, self.w, self.h - 1 - ROW)
  self.bar = ui.scrollbar(g, self.w, self.h - 1 - ROW, #rows, visible, top)
  g:pop(saved)
  self.visible = visible

  local room = self.w - (self.bar and ui.SCROLL_W + 2 or 0)

  columns(room)

  local ty = 1 + (ROW - gfx.height()) // 2

  for _, c in ipairs(COLUMNS) do
    local x = c.right and (c.x + c.cw - gfx.measure(c.text)) or c.x

    g:text(x, ty, c.text, theme.text_dim, nil, "ui")
  end

  g:fill(0, ROW, self.w, 1, theme.line_soft)

  local col = {}

  for _, c in ipairs(COLUMNS) do col[c.key] = c end

  for i = top, math.min(top + visible - 1, #rows) do
    local r = rows[i]
    local y = 1 + ROW + (i - top) * ROW
    local on = (i == selected)

    --
    -- The chosen row: a pale band in a flat look, with the words in their
    -- own colours - the drawings' `.tr.on` - and the accent with white
    -- words where a look fills a chosen row with it.
    --
    local lit = on and not theme.flat
    local bg = on and (theme.flat and theme.line_soft or theme.accent)
               or theme.sunken

    if on then g:fill(0, y, room, ROW, bg) end

    local fg = lit and theme.text_on or theme.text
    local dim = lit and theme.text_on or theme.text_dim
    local wy = y + (ROW - gfx.height()) // 2

    g:text(col.id.x, wy, tostring(r.id), dim, bg)
    g:text(col.name.x, wy, fitted(r.name, col.name.cw),
           r.exited and theme.text_dim or fg, bg)

    -- What the row *is*, dimmer than what it is called, since the name is
    -- what you are looking for; how it draws, and its band, the same.
    g:text(col.kind.x, wy, r.kind or "", dim, bg)
    g:text(col.draws.x, wy, r.video or "", dim, bg)
    g:text(col.priority.x, wy, r.band or "", fg, bg)

    --
    -- Its home processor. Blank rather than a number when the process has
    -- no thread to have one - an exited process that has not been reaped
    -- is not on core zero, it is nowhere, and printing 0 would say the
    -- first of those.
    --
    g:text(col.core.x, wy, r.cpu and tostring(r.cpu) or "", fg, bg)

    -- What it holds: the image, the heap, the stacks and any surface it
    -- asked for.
    if not r.synthetic then
      local kb = ("%d KB"):format(r.kb or 0)

      g:text(col.memory.x + col.memory.cw - gfx.measure(kb), wy, kb, fg, bg)
    end

    --
    -- Its share of a processor: a bar and the number. The bar is the point
    -- - BeOS put one beside every team for the same reason - drawn as the
    -- drawings' level, 8 high and round-ended, in the accent and in the
    -- warning colour past sixty.
    --
    local right = r.exited and "gone" or ("%d%%"):format(r.pct)
    local num_w = 34
    local bar_x = col.processor.x
    local bar_w = col.processor.cw - num_w - 8
    local by = y + (ROW - 8) // 2

    g:fill_round(bar_x, by, bar_w, 8, theme.track, 4)

    local filled = bar_w * r.pct // 100

    if filled > 0 then
      g:fill_round(bar_x, by, math.max(8, filled), 8,
                   (r.pct > 60) and theme.bad or theme.accent, 4)
    end

    g:text(bar_x + col.processor.cw - gfx.measure(right), wy, right, dim, bg)
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

  local to = ui.scrollbar_mouse(self, action, x, y - 1 - ROW, self.w,
                                self.h - 1 - ROW, #rows, visible, top)

  if to then
    top = to

    return true
  end

  if action == "press" or action == "move" then
    -- Below the heading row, which is not a process. Without the
    -- subtraction every click selected the process one place further down
    -- than the one under the pointer.
    local row = (y - 1 - ROW) // ROW + top

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

-- Declared before the header refers to them and defined below. The header
-- is written where it is drawn, at the top of the window, and what its
-- controls do is written where it belongs - so the two have to be able to
-- name each other.
local end_selected
local sampler

--------------------------------------------------------------------------
-- The header.
--
-- **One row across the top instead of a menu bar**, the same shape Tracker
-- and Preferences took (`docs/desktop.html`, `roadmap.md` 5zj): what the
-- machine is doing on the left, and on the right the one action plus a
-- `...` for the rest.
--
-- End keeps a control of its own because it is why this window is open
-- when it is open. Everything else a person does here is *looking*, and
-- looking needs no control at all.
--
-- **The two order items used to be in a `View` menu and did nothing** -
-- `on_choose = function() end`, both of them, since the menu bar was
-- written. They sort now, and they carry a mark saying which is on, which
-- is the thing that makes an order worth offering: an order you cannot see
-- is an order you cannot trust.
--------------------------------------------------------------------------

-- Rebuilt on every press, because the marks are read at that moment.
local function more_menu()
  return {
    { text = "Refresh", on_choose = function() sampler:tick() end },
    { separator = true },
    { text = "Busiest first", mark = (order == "busy"),
      on_choose = function() order = "busy" sampler:tick() end },
    { text = "By id", mark = (order == "id"),
      on_choose = function() order = "id" sampler:tick() end },
  }
end

local more = ui.iconbutton{ icon = "more" }

more.on_click = function()
  win:open_menu(win.origin_x + more.x, win.origin_y + L.head, more_menu())
end

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
  local v = ui.view{ x = spec.x, y = spec.y, w = spec.w, h = METER_H }

  v.label = spec.label
  v.read  = spec.read

  function v:draw(g)
    local value, of, text = self.read()
    local frac = (of > 0) and (value / of) or 0

    if frac < 0 then frac = 0 end
    if frac > 1 then frac = 1 end

    g:text(0, 0, self.label, "text_dim", nil, "ui")

    --
    -- **Measured, not counted**, and this one was missed when the row above
    -- was fixed the same way - which is what "fix the class, not the
    -- instance" means in practice. `gfx.font.w` is the *widest* glyph of
    -- the widget face, so `#right` of them is wider than the string really
    -- is, and the value was pushed left until it sat on top of the label:
    -- `mem124 of 512 MB`, which is what Diego photographed on 20 September.
    --
    local right = text or (tostring(value) .. " of " .. tostring(of))
    g:text(self.w - gfx.measure(right), 0, right, "text", nil, "ui")

    --
    -- The drawings' level: 8 high and round-ended, `track` under the
    -- accent. It was a framed well 10 high, a box around a number.
    --
    local top = self.h - 8
    local filled = self.w * frac // 1

    g:fill_round(0, top, self.w, 8, theme.track, 4)

    if filled > 0 then
      g:fill_round(0, top, math.max(8, filled), 8, theme.accent, 4)
    end
  end

  return v
end

do
  local n    = 4
  local gap  = 14
  local each = (W - 2 * BAND_SIDE - gap * (n - 1)) // n
  local ty   = L.head + BAND_TOP

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
    win:add(meter{ x = BAND_SIDE + (i - 1) * (each + gap), y = ty, w = each,
                   label = rows[i][1]:gsub("^%l", string.upper),
                   read = rows[i][2] })
  end
end

--
-- What the machine is doing, beside the title - and what the last End did,
-- in its place for a few seconds, since a sentence that is overwritten on
-- the next sample is a sentence nobody reads.
--
local summary, said, said_at = "", nil, 0

local function say(text)
  said, said_at = text, sys.ticks()
end

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
      say("ended " .. r.name)
      return
    end

    local ok, why = fs.send("/app/wm", { type = "end_process", pid = r.id })

    if ok then
      say("asked the desktop to end " .. r.name)
    else
      say(r.name .. ": " .. tostring(why_not or why))
    end
end

-- End, in the header beside the dots: the one action this window is
-- opened to perform.
local header = ui.header{
  x = 0, y = 0, w = W, title = "Processes", sub = "",
  right = { ui.button{ text = "End", on_click = function() end_selected() end },
            more },
}

-- Last, so the focus starts in the table and Tab reaches End after it.
win:add(header)

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

  --
  -- Each process's share of every tick since the last sample, and the
  -- kernel's row beside them - and no idle row, which read as a process
  -- eating the machine. `/lib/procshare.lua` has why, and is tested on the
  -- Mac.
  --
  local fresh = {}

  for _, r in ipairs(share.rows(sampled, list, k)) do
    local p = r.process

    if r.kernel then
      fresh[#fresh + 1] = { id = 0, name = "kernel", kind = "threads",
                            band = "", video = "", kb = 0, pct = r.pct,
                            synthetic = true }
    else
      fresh[#fresh + 1] = {
        id = p.id, name = p.name, pages = p.pages, caps = p.caps,
        owns = p.owns, kind = kind_of(p), video = video[p.id],
        band = BANDS[p.priority or 2] or tostring(p.priority),
        kb = ((p.held or p.pages or 0) * 4096) // 1024,
        exited = p.exited,
        cpu    = p.cpu,
        pct = r.pct,
      }
    end
  end

  -- Busiest first by default, which is what a list like this is for; by id
  -- when somebody asked for it, which is the order a machine grew in and
  -- the one to read when you are looking for a particular process rather
  -- than for whatever is eating the processor.
  table.sort(fresh, function(a, b)
    if order == "busy" and a.pct ~= b.pct then return a.pct > b.pct end
    return a.id < b.id
  end)

  rows = fresh

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

  --
  -- One line of the same facts, in the header. The separator is a middle
  -- dot rather than a dash surrounded by spaces, which is what the rest of
  -- the new windows use (`docs/desktop.html`).
  --
  summary = ("%d processes · %d threads (%d in the kernel) · "
             .. "%d space%s · up %d:%02d")
            :format(totals.procs, totals.threads,
                    math.max(0, totals.threads - totals.procs),
                    totals_state.spaces,
                    (totals_state.spaces == 1) and "" or "s",
                    up // 60, up % 60)

  if said and sys.ticks() - said_at > 4 * counter_hz then said = nil end

  header.sub = said or summary
end

win:add(sampler)
win:run()
