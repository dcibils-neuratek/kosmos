-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon Device_Harddisk
-- kosmos: name Disk Monitor
-- kosmos: section system
-- What each drive is reading and writing, now and over the last minute, and
-- which programs are doing it.
--
--   wm diskmon
--
-- Diego, 12 October 2026: "we will need to do another app called Disk
-- Monitor to moniutor input ouptput like we do for network to show all
-- devices and its input output bandwtudh, procesees using io, etc". Drawn in
-- `docs/diskmonitor.html` and agreed; `docs/diskmonitor.md` is what it is
-- made of, and this is step D3.
--
-- **It counts nothing itself**: each drive's server does (D1, D2), and this
-- reads `.device` and `.programs` under each drive's path once a pace and
-- takes differences. The graphs and the card are the kit's, and reading and
-- writing are the theme's `io_in` and `io_out`, as in every monitor.

local ui = use("/Kosmos/Libraries/ui.lua")
local drivelist = use("/Kosmos/Libraries/drivelist.lua")
local netshares = use("/Kosmos/Libraries/netshares.lua")
local theme = ui.theme
local L = ui.layout

local counter_hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
local TELL = (tostring(args or "")):find("%-%-tell") ~= nil   -- a line a sample, for its suite

local PACES = {
  { half = 1, menu = "Update every 0.5 s", heading = "The last 30 seconds",
    foot = "A column every half second; the newest at the right." },
  { half = 2, menu = "Update every 1 s",   heading = "The last minute",
    foot = "A column a second; the newest at the right." },
  { half = 4, menu = "Update every 2 s",   heading = "The last 2 minutes",
    foot = "A column every two seconds; the newest at the right." },
}
local pace = PACES[2]
local bits = false

local W        = 1040
local LIST_W   = 300
local DEV_H    = 92
local CARD_H   = 90
local PIC      = 56
local GRAPH_H  = 120
local COLUMNS  = 60
local TOTALS_H = 58
local ROWS     = 6
local ROW_H    = 24
local GRAPH_GROUND = 0xff16181d

--------------------------------------------------------------------------
-- How numbers are said.
--------------------------------------------------------------------------

local function scaled(v, step, units)
  local i = 1

  while v >= step and i < #units do v, i = v / step, i + 1 end

  return (i == 1 or v >= 100) and ("%.0f %s"):format(v, units[i])
         or ("%.1f %s"):format(v, units[i])
end

local function rate(b)
  if bits then return scaled((b or 0) * 8, 1000, { "b/s", "Kb/s", "Mb/s", "Gb/s" }) end

  return scaled(b or 0, 1024, { "B/s", "KB/s", "MB/s", "GB/s" })
end

local function amount(b) return scaled(b or 0, 1024, { "B", "KB", "MB", "GB", "TB" }) end

--------------------------------------------------------------------------
-- The drives: `/Home`, the sticks' volumes, the shares connected, and
-- memory - each with where its counts are read and what to draw it as.
--------------------------------------------------------------------------

local function find_drives()
  local out = {}
  local home_on

  for _, d in ipairs(drivelist.drives()) do
    for _, v in ipairs(d.volumes or {}) do
      if drivelist.is_home(v) then
        home_on = d
      elseif drivelist.opens(v) then
        out[#out + 1] = { key = "/Drives/" .. v.name, path = "/Drives/" .. v.name,
                          name = d.name, kind = "usb",
                          sub = "USB stick \u{b7} /Drives/" .. v.name }
      end
    end
  end

  local disk = sys.disk and sys.disk()
  local home = { key = "/Home", path = "/Home", kind = "disk", name = "Home" }

  if home_on then
    home.kind, home.name, home.sub = "usb", home_on.name, "USB stick \u{b7} /Home"
  elseif type(disk) == "table" and (disk.bytes or 0) > 0 then
    home.name, home.sub = "Internal drive", "the machine's disk \u{b7} /Home"
  else
    home.kind, home.name, home.sub = "mem", "Home", "in memory \u{b7} /Home"
  end

  table.insert(out, 1, home)

  -- Shares connected now, each one's first: a share asked of a server not
  -- connected would start a connection, which looking must never do.
  local status = fs.share_status and fs.share_status() or {}
  local listing = fs.list(netshares.ROOT) or {}

  for _, s in ipairs(netshares.servers(status, listing, nil, {})) do
    if s.state == "connected" and s.label then
      for _, share in ipairs(fs.list(netshares.ROOT .. "/" .. s.label) or {}) do
        local where = netshares.ROOT .. "/" .. s.label .. "/" .. share

        out[#out + 1] = { key = where, path = where, kind = "share", name = share,
                          sub = "a share on " .. tostring(s.name) .. " \u{b7} SMB" }
      end
    end
  end

  out[#out + 1] = { key = "/Temporary", path = "/Temporary", kind = "mem",
                    name = "Memory", sub = "/Temporary" }
  return out
end

--------------------------------------------------------------------------
-- A drive's picture, as the drawing has each kind: an SSD, a stick, a share,
-- memory. Drawn at `size`, at x, y.
--------------------------------------------------------------------------

local KIND = { disk = "Disk", usb = "USB stick", share = "Share", mem = "Memory" }

local function picture(kind, size)
  return function(g, x, y)
    local s = size

    g:fill_round(x, y, s, s, 0xff2b3240, s // 6)

    if kind == "usb" then
      g:fill_round(x + s * 30 // 100, y + s * 12 // 100, s * 40 // 100, s * 62 // 100, 0xff4a5468, 4)
      g:fill(x + s * 38 // 100, y + s * 74 // 100, s * 24 // 100, s * 16 // 100, 0xffaab4c4)
    elseif kind == "share" then
      g:fill_round(x + s // 6, y + s * 30 // 100, s * 2 // 3, s * 40 // 100, 0xff3b82f6, 4)
      g:fill(x + s * 45 // 100, y + s * 70 // 100, s // 10, s // 8, 0xffaab4c4)
      g:fill(x + s // 4, y + s * 82 // 100, s // 2, 2, 0xffaab4c4)
    elseif kind == "mem" then
      g:fill_round(x + s // 8, y + s * 34 // 100, s * 3 // 4, s * 30 // 100, 0xff2f8f55, 3)

      for i = 0, 3 do
        g:fill(x + s // 5 + i * s * 16 // 100, y + s * 40 // 100, s // 10, s // 6, 0xff16181d)
      end
    else
      g:fill_round(x + s // 8, y + s // 4, s * 3 // 4, s // 2, 0xff4a5468, 4)
      g:fill(x + s * 22 // 100, y + s * 40 // 100, s * 30 // 100, s // 5, 0xff16181d)
    end
  end
end

--------------------------------------------------------------------------
-- What each drive is doing, a sample at a time.
--------------------------------------------------------------------------

local drives, chosen = find_drives(), 1
local by_key = {}
local programs, prog_last, prog_open = {}, {}, 0
local samples = 0

local function state_of(d)
  local s = by_key[d.key]

  if not s then
    s = { history = {}, rin = 0, rout = 0 }
    by_key[d.key] = s
  end

  return s
end

local function sample()
  samples = samples + 1

  -- The drives again every few samples: a stick comes and goes.
  if samples % 5 == 0 then
    local keep = drives[chosen] and drives[chosen].key

    drives = find_drives()
    chosen = 1

    for i, d in ipairs(drives) do if d.key == keep then chosen = i end end
  end

  local now = sys.ticks()

  for _, d in ipairs(drives) do
    local s = state_of(d)
    local c = fs.read(d.path .. "/.device")

    if type(c) == "table" then
      if s.last and now > s.at then
        local seconds = (now - s.at) / counter_hz
        local dr, dw = c.reads - s.last.reads, c.writes - s.last.writes

        s.rin = (c.read_bytes - s.last.read_bytes) / seconds
        s.rout = (c.write_bytes - s.last.write_bytes) / seconds
        s.read_ms = dr > 0 and (c.read_counter_ticks - s.last.read_counter_ticks) / dr / counter_hz * 1000 or nil
        s.write_ms = dw > 0 and (c.write_counter_ticks - s.last.write_counter_ticks) / dw / counter_hz * 1000 or nil

        local hits = c.cache_hits - s.last.cache_hits
        local asks = hits + c.cache_misses - s.last.cache_misses

        s.cache = asks > 0 and hits * 100 // asks or s.cache
        ui.history.push(s.history, { s.rin, s.rout }, COLUMNS)
      end

      s.last, s.at, s.counts = c, now, c
    else
      s.silent = true
    end
  end

  -- The chosen drive's programs: their rates by the difference in each
  -- one's own totals, the busiest first.
  local d = drives[chosen]
  local rows = d and fs.read(d.path .. "/.programs")
  local names = {}

  for _, p in ipairs(sys.processes() or {}) do names[p.id] = p.name end

  local seen, list = {}, {}
  local seconds = prog_last.at and (now - prog_last.at) / counter_hz or 0

  for _, r in ipairs(type(rows) == "table" and rows or {}) do
    local was = prog_last.rows and prog_last.rows[r.process]
    local rin = was and seconds > 0 and (r.read_bytes - was.read_bytes) / seconds or 0
    local rout = was and seconds > 0 and (r.write_bytes - was.write_bytes) / seconds or 0

    seen[r.process] = r
    list[#list + 1] = { program = names[r.process] or ("process " .. r.process .. ", ended"),
                        rin = rin, rout = rout, last = r.last, alive = names[r.process] ~= nil }
  end

  table.sort(list, function(a, b)
    if a.rin + a.rout ~= b.rin + b.rout then return a.rin + a.rout > b.rin + b.rout end
    if a.alive ~= b.alive then return a.alive end
    return a.program < b.program
  end)

  programs, prog_last = list, { rows = seen, at = now, key = d and d.key }
  prog_open = 0

  for _, p in ipairs(list) do if p.rin + p.rout > 0 then prog_open = prog_open + 1 end end

  if TELL and d then
    local s = state_of(d)
    local top = programs[1]

    print(("diskmon: %s read %d B/s, wrote %d B/s; busiest %s %d %d")
          :format(d.key, math.floor(s.rin), math.floor(s.rout),
                  top and top.program or "-", top and math.floor(top.rin) or 0,
                  top and math.floor(top.rout) or 0))
  end
end

--------------------------------------------------------------------------
-- Where everything goes.
--------------------------------------------------------------------------

local RIGHT_W = W - LIST_W - L.page_side
local INNER = RIGHT_W - L.page_side

local function measure()
  local m = {}
  local y = L.page_top

  m.card = y
  y = y + CARD_H + L.between
  m.heading = y
  y = y + L.group + 8
  m.graph = y
  y = y + GRAPH_H + 14
  m.totals = y
  y = y + TOTALS_H + L.between
  m.progs_heading = y
  y = y + L.group + 8
  m.progs = y
  y = y + (ROWS + 1) * ROW_H
  m.foot = y + 10
  m.h = m.foot + gfx.height() + L.page_foot
  return m
end

local M = measure()

local win, err = ui.window{ title = "Disk Monitor", w = W, h = L.head + M.h,
                            x = 90, y = 90, header = true }

if not win then
  print("diskmon: " .. tostring(err))
  return
end

local legend = ui.view{ w = 2 * (10 + 6) + 14 + gfx.measure("Reading") + gfx.measure("Writing"), h = 26 }

function legend:draw(g)
  local y, ty, x = (self.h - 10) // 2, (self.h - gfx.height()) // 2, 0

  for _, part in ipairs({ { theme.io_in, "Reading" }, { theme.io_out, "Writing" } }) do
    g:fill_round(x, y, 10, 10, part[1], 3)
    x = x + 10 + 6
    g:text(x, ty, part[2], theme.text_dim, theme.raised, "ui")
    x = x + gfx.measure(part[2]) + 14
  end
end

local set_pace
local more = ui.iconbutton{ icon = "more" }

more.on_click = function()
  local items = {}

  for _, p in ipairs(PACES) do
    items[#items + 1] = { text = p.menu, mark = (p == pace), on_choose = function() set_pace(p) end }
  end

  items[#items + 1] = { separator = true }
  items[#items + 1] = { text = "Bytes a second", mark = not bits,
                        on_choose = function() bits = false print("diskmon: bytes") end }
  items[#items + 1] = { text = "Bits a second", mark = bits,
                        on_choose = function() bits = true print("diskmon: bits") end }

  local m = win:open_menu(win.origin_x + more.x, win.origin_y + L.head, items)

  if m then
    print(("diskmon: more menu at %d,%d, %d wide, rows of %d"):format(m.x, m.y, m.w, m.row))
  end
end

win:add(ui.header{ x = 0, y = 0, w = W, title = "Disk Monitor",
                   right = { legend, more }, title_bar = true })

--------------------------------------------------------------------------
-- The list of drives, down the left.
--------------------------------------------------------------------------

local list = ui.view{ x = 0, y = L.head, w = LIST_W, h = M.h }

function list:draw(g)
  g:fill(0, 0, self.w, self.h, theme.sunken)
  g:fill(self.w - 1, 0, 1, self.h, theme.line_soft)
  g:text(L.page_side, 14, "Drives", theme.text_dim, theme.sunken, "label")

  for i, d in ipairs(drives) do
    local y = 40 + (i - 1) * DEV_H
    local s = state_of(d)

    if i == chosen then g:fill_round(8, y, self.w - 16, DEV_H - 6, theme.raised, L.card_r) end

    picture(d.kind, 36)(g, 18, y + 10)

    local tx, room = 66, self.w - 66 - 16

    g:text(tx, y + 8, ui.fitted(d.name, room, "heading"), theme.text, theme.sunken, "heading")
    g:text(tx, y + 8 + gfx.height("heading"), ui.fitted(d.sub or "", room, "label"),
           theme.text_dim, theme.sunken, "label")

    local said = s.counts and ("%s read \u{b7} %s write"):format(rate(s.rin), rate(s.rout))
                 or "says nothing of its reading and writing"

    g:text(tx, y + 8 + gfx.height("heading") + gfx.height("label"), ui.fitted(said, room, "label"),
           theme.text_dim, theme.sunken, "label")

    -- On the list's own colour, not the big graph's dark ground: twenty
    -- pixels of near-black under every drive read as bars, not graphs.
    ui.history.draw(g, tx, y + DEV_H - 30, room, 20, s.history, {
      mode = "mirrored", columns = COLUMNS, ground = theme.raised, radius = 4,
      colours = { theme.io_in, theme.io_out },
    })
  end
end

function list:mouse(action, x, y)
  return ui.click(self, action, x, y, function(_, py)
    local i = (py - 40) // DEV_H + 1

    if drives[i] then
      return i, function()
        chosen = i
        prog_last = {}
        print(("diskmon: chose %s"):format(drives[i].key))
      end
    end
  end)
end

win:add(list)

--------------------------------------------------------------------------
-- The chosen drive, on the right.
--------------------------------------------------------------------------

local function heading(g, x, y, left, right)
  g:text(x + 3, y + (L.group - gfx.height("heading")) // 2, left, theme.text, theme.window, "heading")

  if right then
    g:text(x + INNER - gfx.measure(right), y + (L.group - gfx.height()) // 2, right,
           theme.text_dim, theme.window, "ui")
  end
end

local function draw_totals(g, x, y, s)
  local c = s.counts or {}
  local reads, writes = c.reads or 0, c.writes or 0
  local overall_read = reads > 0 and (c.read_counter_ticks or 0) / reads / counter_hz * 1000 or nil
  local overall_write = writes > 0 and (c.write_counter_ticks or 0) / writes / counter_hz * 1000 or nil
  local ms = function(v) return v and ("%.2f ms"):format(v) or "-" end
  local hits, misses = c.cache_hits or 0, c.cache_misses or 0
  local cache = s.cache or ((hits + misses) > 0 and hits * 100 // (hits + misses) or nil)
  local cells = {
    { "A read takes", ms(s.read_ms or overall_read) },
    { "A write takes", ms(s.write_ms or overall_write) },
    { "Cache answered", cache and (cache .. "%") or "-" },
    { "Read \u{b7} written", amount(c.read_bytes) .. " \u{b7} " .. amount(c.write_bytes) },
  }
  local cw = INNER // #cells

  g:fill_round(x, y, INNER, TOTALS_H, theme.raised, L.card_r)
  g:frame_round(x, y, INNER, TOTALS_H, theme.line_soft, L.card_r)

  for i, cell in ipairs(cells) do
    local cx = x + (i - 1) * cw + L.row_in
    local cy = y + (TOTALS_H - gfx.height("label") - gfx.height("heading") - 2) // 2

    g:text(cx, cy, cell[1], theme.text_dim, theme.raised, "label")
    g:text(cx, cy + gfx.height("label") + 2, ui.fitted(cell[2], cw - 2 * L.row_in, "heading"),
           theme.text, theme.raised, "heading")
  end
end

local COLS = { 0, 0.30, 0.45, 0.60 }

local function draw_programs(g, x, y)
  local w = INNER
  local function col(i) return x + 8 + math.floor((w - 16) * COLS[i]) end
  local hy = y + (ROW_H - gfx.height("label")) // 2

  g:text(col(1), hy, "Program", theme.text_dim, theme.window, "label")
  g:text(col(3) - 12 - gfx.measure("Reading", "label"), hy, "Reading", theme.text_dim, theme.window, "label")
  g:text(col(4) - 12 - gfx.measure("Writing", "label"), hy, "Writing", theme.text_dim, theme.window, "label")
  g:text(col(4), hy, "Last file", theme.text_dim, theme.window, "label")
  g:fill(x, y + ROW_H - 1, w, 1, theme.line_soft)

  for r = 1, math.min(ROWS, #programs) do
    local p = programs[r]
    local ty = y + r * ROW_H + (ROW_H - gfx.height()) // 2
    local rin, rout = rate(p.rin), rate(p.rout)

    g:text(col(1), ty, ui.fitted(p.program, col(2) - col(1) + 40, "ui"),
           p.alive and theme.text or theme.text_dim, theme.window, "ui")
    g:text(col(3) - 12 - gfx.measure(rin), ty, rin, theme.io_in, theme.window, "ui")
    g:text(col(4) - 12 - gfx.measure(rout), ty, rout, theme.io_out, theme.window, "ui")
    g:text(col(4), ty, ui.fitted(p.last or "", x + w - 8 - col(4), "ui", true),
           theme.text_dim, theme.window, "ui")
  end

  if #programs == 0 then
    g:text(col(1), y + ROW_H + (ROW_H - gfx.height()) // 2, "Nothing has read or written it yet.",
           theme.text_dim, theme.window, "ui")
  end
end

local body = ui.view{ x = LIST_W, y = L.head, w = RIGHT_W, h = M.h }

function body:draw(g)
  if not self.told and more.x then
    print(("diskmon: window at %d,%d; more at %d,%d; graph at %d,%d, %dx%d; list at %d,%d, rows of %d"):format(
          win.origin_x or 0, win.origin_y or 0, more.x + more.w // 2, more.y + more.h // 2,
          LIST_W + L.page_side, L.head + M.graph, INNER, GRAPH_H, 0, L.head + 40, DEV_H))
    self.told = true
  end

  local d = drives[chosen]

  if not d then return end

  local s = state_of(d)
  local x = L.page_side

  ui.meter_card(g, x, M.card, INNER, CARD_H, {
    picture = picture(d.kind, PIC), size = PIC, maker = KIND[d.kind] or "", model = d.name,
    facts = d.sub or d.path,
    rates = { { rate(s.rin), "reading, now", theme.io_in }, { rate(s.rout), "writing, now", theme.io_out } },
  })

  local most_in, most_out = 0, 0

  for _, h in ipairs(s.history) do
    most_in, most_out = math.max(most_in, h[1]), math.max(most_out, h[2])
  end

  heading(g, x, M.heading, pace.heading, ("peak %s reading, %s writing"):format(rate(most_in), rate(most_out)))

  local scale = ui.history.draw(g, x, M.graph, INNER, GRAPH_H, s.history, {
    mode = "mirrored", columns = COLUMNS, ground = GRAPH_GROUND,
    colours = { theme.io_in, theme.io_out },
  })

  if scale > 0 then
    g:text(x + 10, M.graph + 6, rate(scale) .. " reading", 0xff8b949e, GRAPH_GROUND, "label")
    g:text(x + 10, M.graph + GRAPH_H - 6 - gfx.height("label"), rate(scale) .. " writing",
           0xff8b949e, GRAPH_GROUND, "label")
  end

  draw_totals(g, x, M.totals, s)
  heading(g, x, M.progs_heading, "Programs using it",
          prog_open > 0 and ("%d now"):format(prog_open) or ("%d since start"):format(#programs))
  draw_programs(g, x, M.progs)
  g:text(x + 3, M.foot, pace.foot, theme.text_dim, theme.window, "ui")
end

win:add(body)

local sampler = ui.view{ x = 0, y = 0, w = 0, h = 0 }

function sampler:tick() sample() end

win:add(sampler)

set_pace = function(p)
  pace = p
  win.tick_every = counter_hz * p.half // 2
  win.last_tick = sys.ticks()

  for _, s in pairs(by_key) do s.history, s.last = {}, nil end

  sample()
  print(("diskmon: %s, %s"):format(p.menu:lower(), p.heading:lower()))
end

sample()
win.tick_every = counter_hz * pace.half // 2
win:run()
