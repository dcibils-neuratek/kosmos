-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The table of processes, written once: what each row says, how the table
-- sorts, chooses, scrolls and draws, the pools as levels, the header's
-- line and End. Process Viewer (`procs.lua`) and System Monitor's
-- Processes page (`sysmon/processes.lua`) both draw through it
-- (`docs/sysmonitor.md` S4).
--
--   local proctable = use("/Kosmos/Libraries/proctable.lua")
--   local t = proctable.new{ prefix = "procs:", x =, y =, w =, h =,
--                            window = function() return win end }
--   t:set(proctable.rows(share_rows, { kernel = k, video = v, declared = d, ... }))
--   win:add(t.view)
--
-- **One table, not two that look alike** (*Kits supply, applications
-- orchestrate*). The page began as Process Viewer's table moved into
-- System Monitor, which left the same three hundred lines in two files
-- until S9 - a second copy, which this project counts as a defect from the
-- day it is made, not a convenience until later. So the table is here,
-- and each window is what is around it: its header, its band, where its
-- numbers come from.
--
-- **The numbers come from elsewhere.** This builds rows from what a window
-- already read - `procshare`'s rows (through `meters.processes`), the
-- kernel's counters, the window manager's windows - and asks nobody
-- itself, save End's `sys.kill` and, when the kernel says no, the
-- desktop. A heading sorts the rows it has, at once.
--
-- **What it says**, for the suites, begins with `prefix`: `procs:` for
-- Process Viewer, as `tools/run_sysapps.py` reads, and `sysmon: processes`
-- for the page - `<prefix> headings ...`, `<prefix> rows: ...`, `<prefix>
-- sorted by id, ascending: ...`, the shapes `tools/run_sysmon.py`'s
-- `parse` and `sort_checks` read for both.

local ui = use("/Kosmos/Libraries/ui.lua")
local prefs = use("/Kosmos/Libraries/prefs.lua")
local files = use("/Kosmos/Libraries/files.lua")
local prockind = use("/Kosmos/Libraries/prockind.lua")
local duration = use("/Kosmos/Libraries/clock.lua").duration
local theme = ui.theme

local proctable = {}

local PAD = 14                          -- the table's cells, in from its edges

--
-- **Memory as Tracker writes a size** (Diego, 14 November 2026): 640 KB,
-- 4.3 MB, 607 MB - `files.size`, the one door to it.
--
function proctable.memory(r) return files.size((r.kb or 0) * 1024) end

local memory_of = proctable.memory

--------------------------------------------------------------------------
-- What each row is.
--------------------------------------------------------------------------

--
-- **The privilege a row runs at**, in the processor's own word: an
-- exception level on AArch64, a ring on x86-64. Every process is at the
-- least privileged and the kernel at what `/Devices/cpu` says - the
-- microkernel's shape in one column. `el` is in AArch64's units on both
-- machines, the kernel the larger (`arch/x86_64/cpu.h`), so ring 0 is
-- `el` 1. Returns the function and the kernel's level.
--
function proctable.privilege(cpu)
  cpu = cpu or {}

  local x86 = cpu.arch == "x86-64"

  return function(level)
    if x86 then return (level >= 1) and "ring 0" or "ring 3" end

    return "EL" .. level
  end, cpu.el or 1
end

--
-- **What each file declares**, for a row's kind (`prockind.lua`): the
-- kinds in `/Kosmos/Apps` and `/Kosmos/Programs`, and the applications
-- installed in `/Home/Apps`. Asked once, since what is installed does not
-- change while a window shows it - a round trip a row a second for an
-- answer that never moves would be a lot of messages for nothing.
--
function proctable.declared()
  local from_bin = {}

  for _, dir in ipairs({ "/Kosmos/Apps", "/Kosmos/Programs" }) do
    for _, file in ipairs(fs.list(dir) or {}) do
      local attrs = fs.getattr(dir .. "/" .. file)
      local kind = attrs and attrs.kind

      from_bin[(file:gsub("%.lua$", ""))] = (kind == "application") and "app" or kind or "program"
    end
  end

  for _, app in ipairs(use("/Kosmos/Libraries/filetypes.lua").installed()) do
    from_bin[app.name] = "app"
  end

  return from_bin
end

--
-- **How each process gets its pixels**, from the window manager's list of
-- windows (`wmproto.windows()`), into `into` by process id:
--
--   kit        the UI kit draws it, in the application's own process
--   pixels     it draws every pixel itself - a map, a page, a game
--   (none)     it has no window - a server, a driver, the kernel.
--
-- The window server, which composes them all onto the screen, is
-- "screen" by its name (`proctable.rows`). The kernel does not know this
-- and should not: how an application draws is the desktop's fact.
--
function proctable.video(desktop, into)
  into = into or {}

  for k in pairs(into) do into[k] = nil end

  for _, w in ipairs(desktop and desktop.windows or {}) do
    if w.pid then
      -- A process with two windows draws its pixels if either does.
      if (w.direct and not w.kit) or into[w.pid] == "pixels" then
        into[w.pid] = "pixels"
      else
        into[w.pid] = "kit"
      end
    end
  end

  return into
end

--
-- **The rows, from what a window already read**: `share` is
-- `procshare.rows` (through `meters.processes`), and `o` holds
--
--   kernel          `/Devices/kernel`'s reading, for Nebula's memory
--   video           `proctable.video`'s answer
--   declared        `proctable.declared`'s, or nil until it is read
--   bands           `sys.scheduler().band_names`
--   kernel_level    the kernel's privilege (`proctable.privilege`)
--   kernel_threads  Nebula's own threads (`meters.processes`)
--
-- **Nebula's row from the first reading**, at nothing until there are two
-- to take a share between: a row arriving a second after the window
-- opened moved every row under it while somebody was reading them.
--
function proctable.rows(share, o)
  local k = o.kernel or {}
  local video, declared, bands = o.video or {}, o.declared or {}, o.bands or {}
  local fresh, kernel_row = {}, nil

  for _, r in ipairs(share or {}) do
    local p = r.process

    if r.kernel then
      kernel_row = { id = 0, name = "nebula", kind = "kernel", band = "", kb = k.memory_kb or 0,
                     pct = r.pct, level = o.kernel_level or 1, synthetic = true }
      fresh[#fresh + 1] = kernel_row
    else
      fresh[#fresh + 1] = {
        id = p.id, name = p.name, pages = p.pages, caps = p.caps, owns = p.owns,
        kind = prockind.of(p, declared),
        video = video[p.id] or (p.name == "windowserver" and "screen" or nil),
        band = bands[p.priority or 2] or tostring(p.priority),
        kb = ((p.held or p.pages or 0) * 4096) // 1024,
        exited = p.exited, cpu = p.cpu, pct = r.pct, priority = p.priority,
        threads = p.threads, from = p.from, level = 0,
      }
    end
  end

  if not kernel_row then
    kernel_row = { id = 0, name = "nebula", kind = "kernel", band = "", kb = k.memory_kb or 0,
                   pct = 0, level = o.kernel_level or 1, synthetic = true }
    fresh[#fresh + 1] = kernel_row
  end

  kernel_row.threads = o.kernel_threads

  return fresh
end

--
-- **The header's line**: so many processes, threads (so many in the
-- kernel), spaces, and how long the machine has been up. A middle dot
-- between two, as the new windows have it (`docs/desktop.html`).
--
function proctable.summary(processes, threads, kernel_threads, spaces, up_seconds)
  return ("%d processes \u{b7} %d threads (%d in the kernel) \u{b7} %d space%s \u{b7} up %s")
         :format(processes or 0, threads or 0, kernel_threads or 0, spaces or 0,
                 (spaces == 1) and "" or "s", duration(up_seconds or 0))
end

--
-- **A pool, as a level**, `h` tall at `x`, `y`: its name, how much of how
-- much measured to the right - not counted, which pushed the value onto
-- its label (`mem124 of 512 MB`, 20 September) - and the drawings' level
-- under them, 8 high and round-ended. A continuous fill and not a
-- processor's segments: a pool is read and wants to show a level
-- (`pulse.lua`).
--
function proctable.level(g, x, y, w, h, label, value, of, words)
  value, of = value or 0, of or 0

  local frac = (of > 0) and (value / of) or 0

  if frac < 0 then frac = 0 end
  if frac > 1 then frac = 1 end

  g:text(x, y, label, "text_dim", nil, "ui")

  local right = words or (tostring(value) .. " of " .. tostring(of))

  g:text(x + w - gfx.measure(right), y, right, "text", nil, "ui")

  local filled = math.floor(w * frac)

  g:fill_round(x, y + h - 8, w, 8, theme.track, 4)

  if filled > 0 then g:fill_round(x, y + h - 8, math.max(8, filled), 8, theme.accent, 4) end
end

--
-- **Ending one**: `sys.kill`, which `needs processes` lets reach any
-- process - an administrator's tool that ends what it is pointed at; the
-- kernel refuses init, which has no parent - and the desktop asked only
-- when the kernel says no, for a process it started and this one somehow
-- cannot name. Answers the sentence to say and whether it ended or was
-- asked to.
--
-- (There used to be a refusal in Process Viewer that would end nothing
-- holding the screen, and `init.lua` hands the screen to every program, so
-- it refused `spin`. A guard that fires on everything protects nothing.)
--
function proctable.finish(r)
  local killed, why_not = sys.kill(r.id)

  if killed then return "ended " .. r.name, true end

  local ok, why = fs.send("/Running/wm", { type = "end_process", pid = r.id })

  if ok then return "asked the desktop to end " .. r.name, true end

  return r.name .. ": " .. tostring(why_not or why), false
end

--------------------------------------------------------------------------
-- The table, as one view: rows come and go with the processes, and
-- building a widget a row every second to track them would be a lot of
-- work to look identical.
--------------------------------------------------------------------------

--
-- `spec`:
--   prefix      what its lines begin with ("procs:", "sysmon: processes")
--   x y w h     the view; `follow` as `ui.view`'s
--   window      a function answering the window, for the headings' menu
--   cpu         `/Devices/cpu`, for the privilege column
--   prefs       where the columns' arrangement is kept ("procs")
--   framed      a frame round it, on a page of cards; Process Viewer's
--               runs to its window's edges with a rule along the top
--   say_window  say where the window is with the headings (Process Viewer)
--   on_choose   function(id, said): the chosen process, after every change
--               and on every press; `said` when a line said it
--
function proctable.new(spec)
  local prefix = spec.prefix
  local window = spec.window
  local ROW = ui.metrics.row
  local privilege = proctable.privilege(spec.cpu)
  local kept_in = spec.prefs or "procs"

  local t = { list = {}, sort_key = "processor", sort_down = true,
              selected = 1, selected_id = nil, on_choose = spec.on_choose }

  local view = ui.view{ x = spec.x or 0, y = spec.y or 0, w = spec.w or 0, h = spec.h or 0,
                        follow = spec.follow }

  t.view = view

  local function say(fmt, ...) print(prefix .. " " .. fmt:format(...)) end

  ------------------------------------------------------------------------
  -- **The columns, the kit's** (`ui.columns`, `docs/tracker-list.html`):
  -- each a key, a heading and a width, read by the heading and by the
  -- rows. They drag wider by their headings' edges, fit by a double-click
  -- there, and are shown or hidden from the headings' menu.
  ------------------------------------------------------------------------

  local widest_of

  local cols = ui.columns{
    role = "ui",
    columns = {
      { key = "id",        title = "id",        w = 36 },
      { key = "name",      title = "name",      w = 120, min = 80 },
      --
      -- **The file it runs, whole** (`roadmap.md` 6m): "so i can tell if
      -- doom is running where is running from". The one column that takes
      -- what is left, since a path's length is the one not known.
      --
      { key = "file",      title = "file",      fill = true, min = 120 },
      { key = "kind",      title = "kind",      w = 70 },
      { key = "privilege", title = "privilege", w = 64 },
      { key = "draws",     title = "draws",     w = 76 },
      { key = "priority",  title = "priority",  w = 70 },
      --
      -- **"core" is a fact, not a sample**: a thread's home is set when it
      -- is made and changes only when asked (`docs/smp.md`), so this is
      -- where the process lives.
      --
      { key = "core",      title = "core",      w = 40 },
      { key = "threads",   title = "threads",   w = 56, align = "right" },
      { key = "memory",    title = "memory",    w = 80, align = "right" },
      { key = "processor", title = "processor", w = 120 },
    },
    widest = function(c) return widest_of(c.key) end,
    on_change = function(c)
      local kept = prefs.read(kept_in) or {}

      kept.columns = c:text()
      prefs.write(kept_in, kept)
      say("columns %s", kept.columns)
    end,
  }

  cols:set_text((prefs.read(kept_in) or {}).columns)
  t.columns = cols

  -- Each column by its key: the kit's own tables, which reordering keeps.
  local COL = {}

  for _, c in ipairs(cols.all) do COL[c.key] = c end

  t.column = COL

  -- A cell's options, made once and set each pass: nothing allocated a row.
  local CELL, PATH_CELL = { right = 0 }, { right = 0, keep_end = true }

  -- What a row says in column `key`.
  local function text_of(r, key)
    if key == "id" then return tostring(r.id) end
    if key == "name" then return r.name end
    if key == "file" then return r.from or (r.synthetic and "" or "built in") end
    if key == "kind" then return r.kind or "" end
    if key == "privilege" then return privilege(r.level) end
    if key == "draws" then return r.video or "" end
    if key == "priority" then return r.band or "" end
    if key == "core" then return r.cpu and tostring(r.cpu + 1) or "" end
    if key == "threads" then return tostring(r.threads or "") end
    if key == "memory" then return memory_of(r) end
    return r.exited and "gone" or ("%d%%"):format(r.pct or 0)
  end

  t.text_of, t.privilege = text_of, privilege

  function widest_of(key)
    if key == "processor" then return 120 end    -- a bar and its number

    local most = 0

    for _, r in ipairs(t.list) do most = math.max(most, gfx.measure(text_of(r, key))) end

    return most
  end

  --
  -- What each column sorts by, and nil for a row with nothing there, which
  -- goes after every row that has something whichever way the order runs.
  -- Ties go by id, so equal rows do not trade places every second.
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

  -- Which way a column runs when first chosen: what costs something the
  -- most first, the highest band and the most privileged first, and the
  -- rest as they read.
  local DOWN_FIRST = { threads = true, memory = true, processor = true,
                       priority = true, privilege = true }

  local function in_order(a, b)
    local get = SORT_BY[t.sort_key]
    local x, y = get(a), get(b)

    if x ~= y then
      if x == nil then return false end
      if y == nil then return true end
      if t.sort_down then return x > y end
      return x < y
    end

    return a.id < b.id
  end

  -- The rows as a suite reads them back: `id:name:threads:privilege:file`,
  -- `-` where there is nothing, `; ` between two; and each one's kind, how
  -- it draws and its memory on a line of its own, `name:kind:draws:memory`.
  function t:say_rows(what)
    local out, how = {}, {}

    for _, r in ipairs(self.list) do
      out[#out + 1] = ("%d:%s:%s:%s:%s"):format(r.id, r.name, r.threads or "-",
                                                privilege(r.level), r.from or "-")
      how[#how + 1] = ("%s:%s:%s:%s"):format(r.name, r.kind or "-", r.video or "-",
                                            r.synthetic and "-" or memory_of(r))
    end

    say("%s: %s", what, table.concat(out, "; "))
    say("drawn: %s", table.concat(how, "; "))
  end

  --
  -- The list in its order, and the selection found again in it: the
  -- *process* selected, not the row, since the order changes every second.
  -- One that has gone leaves its row number, and whatever is there now
  -- becomes the selection - its id too, so End and a strip both mean the
  -- row that is lit.
  --
  local function sort_rows()
    local rows, found = t.list, false

    table.sort(rows, in_order)

    if t.selected_id then
      for i, r in ipairs(rows) do
        if r.id == t.selected_id then
          t.selected, found = i, true
          break
        end
      end
    end

    if t.selected > #rows then t.selected = #rows > 0 and #rows or 1 end

    if not found then t.selected_id = rows[t.selected] and rows[t.selected].id or nil end

    if t.on_choose then t.on_choose(t.selected_id, false) end
  end

  -- A new reading's rows, sorted, the selection kept.
  function t:set(rows)
    self.list = rows
    sort_rows()
  end

  -- The row chosen, or nil.
  function t:chosen()
    return self.list[self.selected]
  end

  ------------------------------------------------------------------------
  -- Drawing.
  ------------------------------------------------------------------------

  local followed, top = nil, 1

  local function visible_rows()
    return math.max(1, (view.h - 1 - ROW) // ROW)
  end

  --
  -- **Scrolled to keep the selection in view when it moves** - not on
  -- every pass, or the bar could never be dragged away from it (`ui.list`
  -- had the same bug) - and kept inside the list. Decided here rather than
  -- in the paint, so a sort can say where the view starts.
  --
  local function keep_in_view()
    local rows, visible, selected = t.list, visible_rows(), t.selected

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
  end

  function view:draw(g)
    local rows = t.list

    g:fill(0, 0, self.w, self.h, theme.sunken)

    if spec.framed then
      g:frame(0, 0, self.w, self.h, theme.line_soft)
    else
      g:fill(0, 0, self.w, 1, theme.line_soft)
    end

    local visible = visible_rows()
    local selected = t.selected

    keep_in_view()

    -- The bar beside the rows and not the heading, and the rows stop where
    -- it starts.
    local saved = g:push(0, 1 + ROW, self.w, self.h - 1 - ROW)
    self.bar = ui.scrollbar(g, self.w, self.h - 1 - ROW, #rows, visible, top)
    g:pop(saved)
    self.visible = visible

    local room = self.w - (self.bar and ui.SCROLL_W + 2 or 0)

    cols:shown(PAD, room - 2 * PAD)

    -- Where the window and the headings are, once there are rows to say:
    -- the suites press where these say rather than holding pixel numbers.
    if not self.told and #rows > 0 then
      local ax, ay = self:in_window()
      local at, edges = {}, {}

      for _, c in ipairs(cols:shown(PAD)) do
        at[#at + 1] = ("%s %d,%d"):format(c.key, ax + c.x + c.w // 2, ay + 1 + ROW // 2)
        edges[#edges + 1] = ("%s@%d"):format(c.key, ax + c.x + c.w - 4)
      end

      if spec.say_window then
        local win = window()

        say("window at %d,%d", win.origin_x or 0, win.origin_y or 0)
      end

      say("headings %s", table.concat(at, "; "))
      say("edges %s", table.concat(edges, " "))
      say("table at %d,%d, rows of %d, %d shown", ax, ay + 1 + ROW, ROW, visible)
      t:say_rows("rows")
      self.told = true
    end

    -- The headings: the kit's, the one sorted by in the text's colour with
    -- an arrow saying which way.
    cols:headings(g, PAD, 1, ROW, room - PAD, theme.sunken, t.sort_key, t.sort_down)
    g:fill(0, ROW, self.w, 1, theme.line_soft)

    CELL.right, PATH_CELL.right = room - PAD, room - PAD

    for i = top, math.min(top + visible - 1, #rows) do
      local r = rows[i]
      local y = 1 + ROW + (i - top) * ROW
      local on = (i == selected)

      -- The chosen row: a pale band in a flat look, the words in their own
      -- colours; the accent with white words where a look fills it.
      local lit = on and not theme.flat
      local bg = on and (theme.flat and theme.line_soft or theme.accent) or theme.sunken

      if on then g:fill(spec.framed and 1 or 0, y, room - (spec.framed and 1 or 0), ROW, bg) end

      local fg = lit and theme.text_on or theme.text
      local dim = lit and theme.text_on or theme.text_dim
      local wy = y + (ROW - gfx.height()) // 2

      cols:cell(g, "id", wy, tostring(r.id), dim, bg, CELL)
      cols:cell(g, "name", wy, r.name, r.exited and theme.text_dim or fg, bg, CELL)

      -- Where it came from, cut from the front (the end is the file's own
      -- name), or that it came with the image; the kernel's says nothing.
      if r.from then
        cols:cell(g, "file", wy, r.from, dim, bg, PATH_CELL)
      elseif not r.synthetic then
        cols:cell(g, "file", wy, "built in", dim, bg, CELL)
      end

      cols:cell(g, "kind", wy, r.kind or "", dim, bg, CELL)

      -- The kernel's in the text's colour and every process's dimmer.
      cols:cell(g, "privilege", wy, privilege(r.level), (r.level > 0) and fg or dim, bg, CELL)
      cols:cell(g, "draws", wy, r.video or "", dim, bg, CELL)
      cols:cell(g, "priority", wy, r.band or "", fg, bg, CELL)

      -- Its home, from one as Monitor names them; blank with no thread.
      cols:cell(g, "core", wy, r.cpu and tostring(r.cpu + 1) or "", fg, bg, CELL)

      -- Its threads, and nothing for one that has ended.
      if r.threads and not r.exited then
        cols:cell(g, "threads", wy, tostring(r.threads), fg, bg, CELL)
      end

      if not r.synthetic then cols:cell(g, "memory", wy, memory_of(r), fg, bg, CELL) end

      --
      -- Its share of a processor: a bar and the number - BeOS put one
      -- beside every team for the same reason - 8 high, round-ended, in
      -- the accent and the warning colour past sixty. Not when its column
      -- is hidden, scrolled past, or past the edge.
      --
      local pc = COL.processor

      if pc.x and pc.x + pc.w <= room - PAD then
        local right = r.exited and "gone" or ("%d%%"):format(r.pct)
        local inner = pc.w - 8                -- the kit's gap after a column
        local bar_w = inner - 34 - 8
        local by = y + (ROW - 8) // 2

        g:fill_round(pc.x, by, bar_w, 8, theme.track, 4)

        local filled = bar_w * r.pct // 100

        if filled > 0 then
          g:fill_round(pc.x, by, math.max(8, filled), 8, (r.pct > 60) and theme.bad or theme.accent, 4)
        end

        g:text(pc.x + inner - gfx.measure(right), wy, right, dim, bg)
      end
    end
  end

  ------------------------------------------------------------------------
  -- The pointer, the wheel and the keys.
  ------------------------------------------------------------------------

  -- A row chosen; said when it changes and on every press.
  local function choose(row, pressed)
    local rows = t.list

    if row < 1 or row > #rows then return end

    local was = t.selected_id

    t.selected = row
    t.selected_id = rows[row].id

    local told = pressed or t.selected_id ~= was

    if told then say("selected %d %s", t.selected_id, rows[row].name) end
    if t.on_choose then t.on_choose(t.selected_id, told) end
  end

  t.choose = choose

  function view:mouse(action, x, y)
    local rows = t.list
    local visible = self.visible or 1

    -- The bar first, which sits over the right-hand end of every row.
    local to = ui.scrollbar_mouse(self, action, x, y - 1 - ROW, self.w, self.h - 1 - ROW,
                                  #rows, visible, top)

    if to then
      top = to
      return true
    end

    --
    -- A heading sorts by its column, and the one already sorting turns
    -- round - at once, from the rows already here - on the release over
    -- the heading it was pressed on (`ui.click`: a click is a press and a
    -- release, "in all kosmos").
    --
    local function heading_at(px, py)
      if py < 0 or py >= 1 + ROW then return nil end

      local c = cols:at(px)

      if c then
        return "column:" .. c.key, function()
          if t.sort_key == c.key then
            t.sort_down = not t.sort_down
          else
            t.sort_key, t.sort_down = c.key, DOWN_FIRST[c.key] or false
          end

          sort_rows()
          followed = nil
          keep_in_view()
          t:say_rows(("sorted by %s, %s"):format(t.sort_key, t.sort_down and "descending" or "ascending"))
          say("row %d chosen of %d, the view from row %d", t.selected, #t.list, top)
        end
      end

      return nil
    end

    -- An edge of the headings, held and dragged or fitted by a
    -- double-click, before a heading's own click: it belongs to neither.
    if cols.drag then
      if action == "move" then cols:move(x) end

      if action == "release" then
        local c = cols.drag.col

        cols:release()
        say("column %s now %d", c.key, c.w)
      end

      return true
    end

    if action == "press" and y < 1 + ROW and self.click_key == nil then
      if cols:arrows_press(x) then return true end

      if cols:edge_at(x) then
        local _, c = cols:press(x)

        if not cols.drag then say("column %s fitted to %d", c.key, c.w) end

        return true
      end
    end

    if self.click_key ~= nil or y < 1 + ROW then
      ui.click(self, action, x, y, heading_at)
      return true
    end

    -- Below the heading row, which is not a process.
    if action == "press" or action == "move" then choose((y - 1 - ROW) // ROW + top, action == "press") end

    return true
  end

  view.focusable = true

  -- A right click on the headings: which columns, and Fit and Reset.
  function view:on_context(x, y)
    if y >= 1 + ROW then return false end

    local win = window()
    local ax, ay = self:in_window()

    win:open_menu((win.origin_x or 0) + ax + x, (win.origin_y or 0) + ay + y, cols:menu())
    say("the headings' menu")
    return true
  end

  -- The wheel moves the rows, three a notch; the selection stays with its
  -- process, and the paint keeps the view inside the list.
  function view:wheel(n)
    top = top - n * ui.WHEEL_ROWS
    if top < 1 then top = 1 end
    followed = t.selected
    return true
  end

  function view:key(c)
    if c == -1 then choose(math.max(1, t.selected - 1)) return true end
    if c == -2 then choose(math.min(#t.list, t.selected + 1)) return true end
    return false
  end

  return t
end

return proctable
