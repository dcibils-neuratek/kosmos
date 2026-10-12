-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon Prefs_Network
-- kosmos: name Network Monitor
-- kosmos: section system
-- kosmos: needs network
-- What goes in and out of this machine, now and over the last minute.
--
--   wm netmon
--
-- Diego, 12 October 2026: "i realizsed we need network monitor app as well
-- to see network input and output". Drawn in `docs/netmonitor.html` and
-- agreed ("mockup is perfect! go ahead and build it"); `docs/netmonitor.md`
-- is what it is made of, and this is step N3.
--
-- **It counts nothing itself.** The network server counts every frame and
-- every connection's bytes (`NET_OP_STATS`, `fs.net_stats`), and this asks
-- once a pace and takes the difference between two readings, as Monitor
-- does with a processor's time. The graph and the card are the kit's
-- (`ui.history`, `ui.meter_card`), and in and out are the theme's `io_in`
-- and `io_out`, blue and orange, as in every monitor.

local ui = use("/Kosmos/Libraries/ui.lua")
local hardware = use("/Kosmos/Libraries/hardware.lua")
local theme = ui.theme
local L = ui.layout

local counter_hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000

-- Monitor's three paces, and what sixty columns of each come to.
local PACES = {
  { half = 1, menu = "Update every 0.5 s", heading = "The last 30 seconds",
    foot = "A column every half second; the newest at the right." },
  { half = 2, menu = "Update every 1 s",   heading = "The last minute",
    foot = "A column a second; the newest at the right." },
  { half = 4, menu = "Update every 2 s",   heading = "The last 2 minutes",
    foot = "A column every two seconds; the newest at the right." },
}
local pace = PACES[2]

-- `--tell`: a line a sample, for `tools/run_netmon.py` to read the rates
-- off. Not without it: a line a second for as long as the window is open
-- filled the M700's log on 12 October.
local TELL = (tostring(args or "")):find("%-%-tell") ~= nil
local bits = false                      -- rates in bits a second, from the menu

local W        = 660
local CARD_H   = 90
local PORT     = 56                     -- the card's picture
local GRAPH_H  = 120
local COLUMNS  = 60
local TOTALS_H = 58
local ROWS     = 8                      -- connections shown, the busiest
local ROW_H    = 24
local GRAPH_GROUND = 0xff16181d         -- Monitor's: dark in every look

--------------------------------------------------------------------------
-- How numbers are said.
--------------------------------------------------------------------------

local function scaled(v, step, units)
  local i = 1

  while v >= step and i < #units do v, i = v / step, i + 1 end

  return (i == 1 or v >= 100) and ("%.0f %s"):format(v, units[i])
         or ("%.1f %s"):format(v, units[i])
end

-- A rate, in bytes a second or - a link is sold that way - in bits.
local function rate(bytes_a_second)
  if bits then
    return scaled((bytes_a_second or 0) * 8, 1000, { "b/s", "Kb/s", "Mb/s", "Gb/s" })
  end

  return scaled(bytes_a_second or 0, 1024, { "B/s", "KB/s", "MB/s", "GB/s" })
end

local function amount(bytes)
  return scaled(bytes or 0, 1024, { "B", "KB", "MB", "GB", "TB" })
end

local function count(n)
  return n < 1000 and tostring(n) or scaled(n, 1000, { "", "K", "M", "G" })
end

local function address(a)
  return #(a or "") == 4 and ("%d.%d.%d.%d"):format(a:byte(1, 4)) or "?"
end

local function since(seconds)
  local m = seconds // 60

  if m < 1 then return ("%d s"):format(seconds) end
  if m < 60 then return ("%d min"):format(m) end

  return ("%d h %d min"):format(m // 60, m % 60)
end

--------------------------------------------------------------------------
-- The card, said once.
--------------------------------------------------------------------------

local driven = hardware.network(sys.bus())
local CARD = driven[1] and driven[1].name or "No network card"
local MAKER, MODEL = CARD:match("^(%S+)%s+(.+)$")

if not MAKER then MAKER, MODEL = "", CARD end

local info = fs.net_info("/Network") or {}
local FACTS = info.card and ("%s \u{b7} MTU %d"):format(address(info.address), info.mtu or 0)
              or "not on a network"

print(("netmon: card %s | %s | %s"):format(MAKER, MODEL, FACTS))

-- An Ethernet port, as the drawing has it: a socket and its eight pins.
local function port(g, x, y)
  g:fill_round(x, y, PORT, PORT, 0xff2b3240, 10)
  g:fill_round(x + 10, y + 14, PORT - 20, PORT - 22, 0xff0d1117, 4)
  g:fill(x + 20, y + PORT - 10, PORT - 40, 4, 0xff0d1117)

  for i = 0, 7 do g:fill(x + 14 + i * 4, y + 16, 2, 7, 0xffd4a72c) end
end

--------------------------------------------------------------------------
-- What the window shows, refreshed a sample at a time.
--------------------------------------------------------------------------

local now_in, now_out = 0, 0            -- bytes a second, now
local history = {}                      -- { in, out } a column
local totals = nil                      -- the server's, as last read
local rows, open = {}, 0                -- the connections, busiest first
local last, last_at = nil, nil          -- the reading before
local last_conn = {}                    -- handle -> { in, out } then

local function sample()
  local s = fs.net_stats("/Network")
  local at = sys.ticks()

  if not s then return end

  local seconds = last_at and (at - last_at) / counter_hz or 0
  local names = {}

  for _, p in ipairs(sys.processes() or {}) do names[p.id] = p.name end

  if last and seconds > 0 then
    now_in = (s.bytes_in - last.bytes_in) / seconds
    now_out = (s.bytes_out - last.bytes_out) / seconds
    ui.history.push(history, { now_in, now_out }, COLUMNS)
  end

  local seen, list = {}, {}

  for _, c in ipairs(s.connections) do
    -- A connection not there at the last reading opened since, so every
    -- byte it has is new: a short one - a request, an answer, closed - would
    -- otherwise never be seen twice and never have a rate.
    local was = last_conn[c.handle] or (last and { 0, 0 })
    local rin = was and seconds > 0 and (c.bytes_in - was[1]) / seconds or 0
    local rout = was and seconds > 0 and (c.bytes_out - was[2]) / seconds or 0
    local to = c.kind == "listener" and ("listening on %d"):format(c.local_port)
               or c.kind == "udp" and ("port %d, datagrams"):format(c.local_port)
               or ("%s:%d"):format(address(c.remote), c.remote_port)

    seen[c.handle] = { c.bytes_in, c.bytes_out }
    list[#list + 1] = {
      program = names[c.process] or (c.process > 0 and ("process " .. c.process) or "the stack"),
      to = to,
      state = c.kind == "tcp" and c.state or (c.kind == "udp" and "datagrams" or "listening"),
      rin = rin, rout = rout,
    }
  end

  table.sort(list, function(a, b)
    if a.rin + a.rout ~= b.rin + b.rout then return a.rin + a.rout > b.rin + b.rout end
    return a.program < b.program
  end)

  rows, open, totals = list, s.there, s
  last, last_at, last_conn = s, at, seen

  local top = rows[1]

  if not TELL then return end

  print(("netmon: in %d B/s, out %d B/s; %d open; busiest %s %d %d")
        :format(math.floor(now_in), math.floor(now_out), open,
                top and top.program or "-", top and math.floor(top.rin) or 0,
                top and math.floor(top.rout) or 0))
end

--------------------------------------------------------------------------
-- Where everything goes.
--------------------------------------------------------------------------

local function measure()
  local m = { inner = W - 2 * L.page_side }
  local y = L.page_top

  m.card = y
  y = y + CARD_H + L.between
  m.heading = y
  y = y + L.group + 8
  m.graph = y
  y = y + GRAPH_H + 14
  m.totals = y
  y = y + TOTALS_H + L.between
  m.conns_heading = y
  y = y + L.group + 8
  m.conns = y
  y = y + (ROWS + 1) * ROW_H
  m.foot = y + 10
  m.h = m.foot + gfx.height() + L.page_foot
  return m
end

local M = measure()

local win, err = ui.window{ title = "Network Monitor", w = W, h = L.head + M.h,
                            x = 110, y = 110, header = true }

if not win then
  print("netmon: " .. tostring(err))
  return
end

--------------------------------------------------------------------------
-- The header: the name, the two colours, the dots.
--------------------------------------------------------------------------

local legend = ui.view{ w = 2 * (10 + 6) + 14 + gfx.measure("In") + gfx.measure("Out"), h = 26 }

function legend:draw(g)
  local y, ty, x = (self.h - 10) // 2, (self.h - gfx.height()) // 2, 0

  for _, part in ipairs({ { theme.io_in, "In" }, { theme.io_out, "Out" } }) do
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
    items[#items + 1] = { text = p.menu, mark = (p == pace),
                          on_choose = function() set_pace(p) end }
  end

  items[#items + 1] = { separator = true }
  items[#items + 1] = { text = "Bytes a second", mark = not bits,
                        on_choose = function() bits = false print("netmon: bytes") end }
  items[#items + 1] = { text = "Bits a second", mark = bits,
                        on_choose = function() bits = true print("netmon: bits") end }

  local m = win:open_menu(win.origin_x + more.x, win.origin_y + L.head, items)

  if m then
    print(("netmon: more menu at %d,%d, %d wide, rows of %d"):format(m.x, m.y, m.w, m.row))
  end
end

win:add(ui.header{ x = 0, y = 0, w = W, title = "Network Monitor",
                   right = { legend, more }, title_bar = true })

--------------------------------------------------------------------------
-- The page.
--------------------------------------------------------------------------

local function heading(g, x, y, left, right)
  g:text(x + 3, y + (L.group - gfx.height("heading")) // 2, left, theme.text, theme.window, "heading")

  if right then
    g:text(x + M.inner - gfx.measure(right), y + (L.group - gfx.height()) // 2, right,
           theme.text_dim, theme.window, "ui")
  end
end

local function draw_totals(g, x, y)
  local s = totals or {}
  local cells = {
    { "Received", amount(s.bytes_in) },
    { "Sent", amount(s.bytes_out) },
    { "Packets in \u{b7} out", count(s.frames_in or 0) .. " \u{b7} " .. count(s.frames_out or 0) },
    { "Dropped", count((s.dropped_in or 0) + (s.dropped_out or 0)) },
  }
  local cw = M.inner // #cells

  g:fill_round(x, y, M.inner, TOTALS_H, theme.raised, L.card_r)
  g:frame_round(x, y, M.inner, TOTALS_H, theme.line_soft, L.card_r)

  for i, c in ipairs(cells) do
    local cx = x + (i - 1) * cw + L.row_in
    local cy = y + (TOTALS_H - gfx.height("label") - gfx.height("heading") - 2) // 2

    g:text(cx, cy, c[1], theme.text_dim, theme.raised, "label")
    g:text(cx, cy + gfx.height("label") + 2, ui.fitted(c[2], cw - 2 * L.row_in, "heading"),
           theme.text, theme.raised, "heading")
  end
end

-- The columns of the list, as fractions of its width: Program, To, State,
-- In, Out - the last two right-aligned.
local COLS = { 0, 0.22, 0.58, 0.74, 0.87 }

local function draw_conns(g, x, y)
  local w = M.inner
  local function col(i) return x + 8 + math.floor((w - 16) * COLS[i]) end
  local hy = y + (ROW_H - gfx.height("label")) // 2

  for i, name in ipairs({ "Program", "To", "State" }) do
    g:text(col(i), hy, name, theme.text_dim, theme.window, "label")
  end

  g:text(col(5) - 12 - gfx.measure("In", "label"), hy, "In", theme.text_dim, theme.window, "label")
  g:text(x + w - 8 - gfx.measure("Out", "label"), hy, "Out", theme.text_dim, theme.window, "label")
  g:fill(x, y + ROW_H - 1, w, 1, theme.line_soft)

  for r = 1, math.min(ROWS, #rows) do
    local c = rows[r]
    local ry = y + r * ROW_H
    local ty = ry + (ROW_H - gfx.height()) // 2
    local rin, rout = rate(c.rin), rate(c.rout)

    g:text(col(1), ty, ui.fitted(c.program, col(2) - col(1) - 8, "ui"), theme.text, theme.window, "ui")
    g:text(col(2), ty, ui.fitted(c.to, col(3) - col(2) - 8, "ui"), theme.text, theme.window, "ui")
    g:text(col(3), ty, c.state, theme.text_dim, theme.window, "ui")
    g:text(col(5) - 12 - gfx.measure(rin), ty, rin, theme.io_in, theme.window, "ui")
    g:text(x + w - 8 - gfx.measure(rout), ty, rout, theme.io_out, theme.window, "ui")
  end

  if #rows == 0 then
    g:text(col(1), y + ROW_H + (ROW_H - gfx.height()) // 2, "No connections open.",
           theme.text_dim, theme.window, "ui")
  end
end

local body = ui.view{ x = 0, y = L.head, w = W, h = M.h }

function body:draw(g)
  if not self.told and more.x then
    print(("netmon: window at %d,%d; more at %d,%d; graph at %d,%d, %dx%d"):format(
          win.origin_x or 0, win.origin_y or 0, more.x + more.w // 2, more.y + more.h // 2,
          L.page_side, L.head + M.graph, M.inner, GRAPH_H))
    self.told = true
  end

  local x = L.page_side

  ui.meter_card(g, x, M.card, M.inner, CARD_H, {
    picture = port, size = PORT, maker = MAKER, model = MODEL, facts = FACTS,
    rates = { { rate(now_in), "in, now", theme.io_in }, { rate(now_out), "out, now", theme.io_out } },
  })

  local most_in, most_out = 0, 0

  for _, s in ipairs(history) do
    most_in, most_out = math.max(most_in, s[1]), math.max(most_out, s[2])
  end

  heading(g, x, M.heading, pace.heading,
          ("peak %s in, %s out"):format(rate(most_in), rate(most_out)))

  local scale = ui.history.draw(g, x, M.graph, M.inner, GRAPH_H, history, {
    mode = "mirrored", columns = COLUMNS, ground = GRAPH_GROUND,
    colours = { theme.io_in, theme.io_out },
  })

  -- The scale, said at the graph's left edge - the oldest end, so the
  -- words never sit on what is happening now: what the top and the bottom
  -- mean.
  if scale > 0 then
    g:text(x + 10, M.graph + 6, rate(scale) .. " in", 0xff8b949e, GRAPH_GROUND, "label")
    g:text(x + 10, M.graph + GRAPH_H - 6 - gfx.height("label"), rate(scale) .. " out",
           0xff8b949e, GRAPH_GROUND, "label")
  end

  draw_totals(g, x, M.totals)
  heading(g, x, M.conns_heading, "Connections",
          open > ROWS and ("%d open, the busiest %d"):format(open, ROWS) or ("%d open"):format(open))
  draw_conns(g, x, M.conns)

  local up = sys.ticks() // counter_hz

  g:text(x + 3, M.foot, pace.foot .. " Since the machine started " .. since(up) .. " ago.",
         theme.text_dim, theme.window, "ui")
end

win:add(body)

--------------------------------------------------------------------------
-- The sampling, on the window kit's clock set to the pace.
--------------------------------------------------------------------------

local sampler = ui.view{ x = 0, y = 0, w = 0, h = 0 }

function sampler:tick() sample() end

win:add(sampler)

set_pace = function(p)
  pace = p
  win.tick_every = counter_hz * p.half // 2
  win.last_tick = sys.ticks()

  -- Begun again, from a reading taken now: columns at one pace beside
  -- columns at another would be a time axis that lies.
  history, last = {}, nil
  sample()
  print(("netmon: %s, %s"):format(p.menu:lower(), p.heading:lower()))
end

sample()
win.tick_every = counter_hz * pace.half // 2
win:run()
