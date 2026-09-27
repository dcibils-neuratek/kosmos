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

local ui = use("/Kosmos/Libraries/ui.lua")
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
local cpu = fs.read("/Devices/cpu") or {}
local counter_hz = cpu.counter_hz or 62500000

--
-- **The privilege each row runs at** (`roadmap.md` 6m): the processor's own
-- word for it, which is an exception level on AArch64 and a ring on
-- x86-64. Every process is at the least privileged - EL0, ring 3 - and the
-- kernel at what `/Devices/cpu` says it runs at, which is the whole of the
-- microkernel's shape in one column: the filesystem, the console and the
-- desktop all read EL0, and only the kernel's row does not.
--
-- `el` is reported in AArch64's units on both machines, the kernel the
-- larger number (`arch/x86_64/cpu.h`), so ring 0 is `el` 1.
--
local X86 = (cpu.arch == "x86-64")
local KERNEL_LEVEL = cpu.el or 1

local function privilege(level)
  if X86 then return (level >= 1) and "ring 0" or "ring 3" end

  return "EL" .. level
end

local W, H = 1080, 482

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
-- What kind of thing each process is: `/Kosmos/Libraries/prockind.lua` decides, from
-- device authority, who started it, and what its file in `/bin` declares -
-- and says why it is those three and not a name.
--------------------------------------------------------------------------

local prockind = use("/Kosmos/Libraries/prockind.lua")

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
  return prockind.of(p, from_bin)
end

local rows = {}          -- { name, kind, id, pct, pages, caps, owns, exited }
local totals = { procs = 0, threads = 0 }
local share = use("/Kosmos/Libraries/procshare.lua")
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
-- The list is sorted, busiest first unless somebody chose otherwise, and
-- that order changes every second, so a selected row number selects a
-- different process each time it is redrawn - you aim at one and end
-- another. Following the id means the highlight moves with the process as
-- the list reorders around it.
local selected_id = nil
local selected = 1

--
-- **Which column the list is sorted by, and which way** (`roadmap.md` 6m).
-- Diego, 26 September: "i want to be able to sort by any of the columns",
-- "right now is just by busiest and id" - which were two items in the
-- `...` menu. A heading is the control now: pressed, it sorts by its column,
-- and pressed again it turns the order round. Busiest first is where it
-- starts, which is what a list like this is opened for.
--
local sort_key, sort_down = "processor", true
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
  { key = "name",      text = "name",      w = 120 },
  --
  -- **The file it runs, whole** (`roadmap.md` 6m): Diego, "so i can tell if
  -- doom is running where is running from", "like the whole path and file
  -- name". What the process said as it named itself, kept by the kernel
  -- beside the name; a process built into the image runs no file and says
  -- so. The one column that takes what is left, since a path is the one
  -- thing here whose length is not known.
  --
  { key = "file",      text = "file",      w = 0 },
  { key = "kind",      text = "kind",      w = 70 },
  { key = "privilege", text = "privilege", w = 64 },
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
  --
  -- **Its threads** (`roadmap.md` 6m), the first among them: a program with
  -- workers - Cafesa3D rendering on four - reads 5 rather than being one
  -- line among many that look alike.
  --
  { key = "threads",   text = "threads",   w = 56, right = true },
  { key = "memory",    text = "memory",    w = 80, right = true },
  { key = "processor", text = "processor", w = 120 },
}

-- The sort arrow beside a heading: a line icon, and a pixel on each side.
local ARROW = 15

-- Where each column starts for a row `room` wide. A column is never
-- narrower than its heading with the arrow beside it, whatever face the
-- look draws headings in, so choosing one never pushes its arrow into its
-- neighbour's word.
local function columns(room)
  local fixed = 0

  for _, c in ipairs(COLUMNS) do
    c.cw = (c.w > 0) and math.max(c.w, gfx.measure(c.text) + ARROW + 2) or 0
    fixed = fixed + c.cw
  end

  local x = PAD
  local rest = math.max(60, room - 2 * PAD - fixed - GAP * (#COLUMNS - 1))

  for _, c in ipairs(COLUMNS) do
    c.x = x
    if c.w == 0 then c.cw = rest end
    x = x + c.cw + GAP
  end
end

local function fitted(text, w)
  text = tostring(text or "")

  while #text > 1 and gfx.measure(text) > w do text = text:sub(1, -2) end

  return text
end

-- The same, cut from the front: the end of a path is the file's own name,
-- which is the part somebody is reading the column for.
local function fitted_tail(text, w)
  if gfx.measure(text) <= w then return text end

  while #text > 1 and gfx.measure("..." .. text) > w do text = text:sub(2) end

  return "..." .. text
end

--
-- What each column sorts by, and nil for a row with nothing there - an
-- application without a window has no `draws`, the kernel no memory of its
-- own - which goes after every row that has something, whichever way the
-- order runs: a column sorted is a question about the rows it has an answer
-- for. Ties go by id, so rows that are equal do not trade places every
-- second.
--
local SORT_BY = {
  id        = function(r) return r.id end,
  name      = function(r) return r.name:lower() end,
  file      = function(r) return r.from end,
  kind      = function(r) return r.kind end,
  privilege = function(r) return r.level end,
  draws     = function(r) return r.video end,
  priority  = function(r) return r.priority end,
  core      = function(r) return r.cpu end,
  threads   = function(r) return r.threads end,
  memory    = function(r) return (not r.synthetic) and r.kb or nil end,
  processor = function(r) return r.pct end,
}

-- Which way a column runs when it is first chosen. What costs something,
-- the most first, since the question is what is using the machine; the
-- highest band and the most privileged first, for the same reason; and
-- everything else as it reads, from the top of the alphabet or the count.
local DOWN_FIRST = { threads = true, memory = true, processor = true,
                     priority = true, privilege = true }

local function in_order(a, b)
  local get = SORT_BY[sort_key]
  local x, y = get(a), get(b)

  if x ~= y then
    if x == nil then return false end
    if y == nil then return true end
    if sort_down then return x > y end
    return x < y
  end

  return a.id < b.id
end

-- What the rows say, for whoever drives Processes from outside -
-- `tools/run_sysapps.py` reads them back after pressing a heading - as
-- `id:name:threads:privilege:file`, with `-` where there is nothing, and
-- `; ` between two, since x86-64's privilege is two words.
local function say_rows(what)
  local out = {}

  for _, r in ipairs(rows) do
    out[#out + 1] = ("%d:%s:%s:%s:%s"):format(r.id, r.name, r.threads or "-",
                                              privilege(r.level), r.from or "-")
  end

  print(("procs: %s: %s"):format(what, table.concat(out, "; ")))
end

-- The list in its order, and the selection found again in it. It may have
-- exited, in which case the row number is kept and whatever is there now
-- becomes the selection - which is the least surprising thing available.
local function sort_rows()
  table.sort(rows, in_order)

  if selected_id then
    for i, r in ipairs(rows) do
      if r.id == selected_id then selected = i break end
    end
  end

  if selected > #rows then selected = #rows > 0 and #rows or 1 end

  if not selected_id then
    selected_id = rows[selected] and rows[selected].id or nil
  end
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

  -- Where the window and its headings are, once there are rows to say:
  -- `tools/run_sysapps.py` presses where these say rather than holding pixel
  -- numbers of its own, so it follows the layout when the layout changes.
  if not self.told and #rows > 0 then
    local at = {}

    for _, c in ipairs(COLUMNS) do
      at[#at + 1] = ("%s %d,%d"):format(c.key, self.x + c.x + c.cw // 2,
                                        self.y + 1 + ROW // 2)
    end

    print(("procs: window at %d,%d"):format(win.origin_x or 0, win.origin_y or 0))
    print("procs: headings " .. table.concat(at, "; "))
    say_rows("rows")
    self.told = true
  end

  local ty = 1 + (ROW - gfx.height()) // 2

  --
  -- The column the list is sorted by has its heading in the text's colour
  -- and an arrow saying which way: an order you cannot see is an order you
  -- cannot trust. The arrow goes on the side the column's words start from,
  -- so a column of numbers keeps its heading over its last digit.
  --
  for _, c in ipairs(COLUMNS) do
    local on = (c.key == sort_key)
    local tw = gfx.measure(c.text)
    local x = c.right and (c.x + c.cw - tw) or c.x

    g:text(x, ty, c.text, on and theme.text or theme.text_dim, nil, "ui")

    if on then
      g:line_icon(c.right and (x - ARROW - 1) or (x + tw + 1),
                  1 + (ROW - ARROW) // 2,
                  sort_down and "descending" or "ascending", theme.text_dim)
    end
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

    -- Where it came from, or that it came with the image. The kernel's own
    -- row is neither and says nothing.
    if r.from then
      g:text(col.file.x, wy, fitted_tail(r.from, col.file.cw), dim, bg)
    elseif not r.synthetic then
      g:text(col.file.x, wy, "built in", dim, bg)
    end

    -- What the row *is*, dimmer than what it is called, since the name is
    -- what you are looking for; how it draws, and its band, the same.
    g:text(col.kind.x, wy, r.kind or "", dim, bg)

    -- The kernel's in the text's colour and every process's dimmer, so the
    -- one row that is not like the others reads as not like them.
    g:text(col.privilege.x, wy, privilege(r.level),
           (r.level > 0) and fg or dim, bg)
    g:text(col.draws.x, wy, r.video or "", dim, bg)
    g:text(col.priority.x, wy, r.band or "", fg, bg)

    --
    -- Its home processor. Blank rather than a number when the process has
    -- no thread to have one - an exited process that has not been reaped
    -- is not on core zero, it is nowhere, and printing 0 would say the
    -- first of those.
    --
    -- From one, as Monitor names them; `t->sched.cpu` counts from zero.
    g:text(col.core.x, wy, r.cpu and tostring(r.cpu + 1) or "", fg, bg)

    -- Its threads, and nothing for one that has ended: it has none, and a
    -- 0 down a column of ones reads as a fault rather than as gone.
    if r.threads and not r.exited then
      local n = tostring(r.threads)

      g:text(col.threads.x + col.threads.cw - gfx.measure(n), wy, n, fg, bg)
    end

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

  --
  -- A heading sorts by its column, and the one already sorting turns
  -- round. At once, from the rows already here, rather than at the next
  -- sample a second away - and the view goes to the selection, which has
  -- just moved somewhere else in the list.
  --
  if y < 1 + ROW then
    if action == "press" then
      for _, c in ipairs(COLUMNS) do
        if x >= c.x - GAP // 2 and x < c.x + c.cw + GAP // 2 then
          if sort_key == c.key then
            sort_down = not sort_down
          else
            sort_key, sort_down = c.key, DOWN_FIRST[c.key] or false
          end

          sort_rows()
          followed = nil
          say_rows(("sorted by %s, %s"):format(sort_key,
                   sort_down and "descending" or "ascending"))
          break
        end
      end
    end

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

-- The wheel moves the rows, three a notch; the selection stays with its
-- process, and `draw` keeps the view inside the list.
function table_view:wheel(n)
  top = top - n * ui.WHEEL_ROWS
  if top < 1 then top = 1 end
  followed = selected
  return true
end

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
-- **The order is chosen at the headings now**, and no longer here. This
-- menu held "Busiest first" and "By id", and before that a `View` menu held
-- the same two doing nothing at all; two orders out of eleven columns was
-- what Diego asked to be rid of (`roadmap.md` 6m).
--------------------------------------------------------------------------

local function more_menu()
  return {
    { text = "Refresh", on_choose = function() sampler:tick() end },
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
-- they happened to arrive in the same `/Devices/kernel` reply - and it has
-- become the processor monitor alone, which is one thing rather than six.
--
-- A total belongs beside the detail it is the total *of*. The table under
-- these says which process holds how much memory; the bar says how much
-- there is. "Seventeen of thirty-two processes" is a sentence about the row
-- count directly beneath it, and reading them apart, in two windows, was
-- always a small act of arithmetic nobody should have been doing.
--
-- Drawn as a continuous fill and not as the segments a processor gets, and
-- that difference is deliberate: `/Kosmos/Libraries/pulse.lua` says why. A processor is
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

    local ok, why = fs.send("/Running/wm", { type = "end_process", pid = r.id })

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
  local k = fs.read("/Devices/kernel")
  local list = sys.processes()

  -- Which processes have windows, and how those windows draw.
  video = {}

  local desktop = fs.send("/Running/wm", { type = "windows" })

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
  -- eating the machine. `/Kosmos/Libraries/procshare.lua` has why, and is tested on the
  -- Mac.
  --
  local fresh = {}

  for _, r in ipairs(share.rows(sampled, list, k)) do
    local p = r.process

    if r.kernel then
      fresh[#fresh + 1] = { id = 0, name = "kernel", kind = "threads",
                            band = "", kb = 0, pct = r.pct,
                            level = KERNEL_LEVEL, synthetic = true }
    else
      fresh[#fresh + 1] = {
        id = p.id, name = p.name, pages = p.pages, caps = p.caps,
        owns = p.owns, kind = kind_of(p), video = video[p.id],
        band = BANDS[p.priority or 2] or tostring(p.priority),
        kb = ((p.held or p.pages or 0) * 4096) // 1024,
        exited = p.exited,
        cpu    = p.cpu,
        pct = r.pct,
        priority = p.priority,
        threads  = p.threads,
        from     = p.from,
        level    = 0,
      }
    end
  end

  --
  -- **The kernel's own threads are what is left** when every process's are
  -- taken from the machine's: one idle thread a core and the workers it
  -- keeps. The header said "in the kernel" by taking one thread a process
  -- for granted, which a process with workers made wrong.
  --
  local theirs, kernel_row = 0, nil

  for _, r in ipairs(fresh) do
    if r.synthetic then
      kernel_row = r
    elseif not r.exited then
      theirs = theirs + (r.threads or 1)
    end
  end

  -- **The kernel's row from the first sample**, at nothing until there are
  -- two to take a share between: `procshare` has none to give on the first,
  -- and a row that arrived a second after the window opened moved every
  -- row under it while somebody was reading them.
  if not kernel_row then
    kernel_row = { id = 0, name = "kernel", kind = "threads", band = "",
                   kb = 0, pct = 0, level = KERNEL_LEVEL, synthetic = true }
    fresh[#fresh + 1] = kernel_row
  end

  totals.procs = #fresh - (kernel_row and 1 or 0)
  totals.threads = k and k.threads or 0
  totals.kernel = math.max(0, totals.threads - theirs)

  if kernel_row then kernel_row.threads = totals.kernel end

  rows = fresh
  sort_rows()

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
  -- `/Devices/memory` is a second read and it is worth it: this sampler already
  -- reads `/Devices/kernel` every tick for the idle and busy counters, and the
  -- memory node is the only other place the free page count lives.
  --
  local m = fs.read("/Devices/memory")

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
            :format(totals.procs, totals.threads, totals.kernel,
                    totals_state.spaces,
                    (totals_state.spaces == 1) and "" or "s",
                    up // 60, up % 60)

  if said and sys.ticks() - said_at > 4 * counter_hz then said = nil end

  header.sub = said or summary
end

win:add(sampler)
win:run()
