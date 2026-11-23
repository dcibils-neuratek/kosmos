-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- System Monitor's Network page: Network Monitor's window, moved in whole
-- - the card, in and out now, the minute mirrored, the totals since the
-- machine started and the connections, busiest first (`docs/sysmonitor.md`
-- S5; inventory rows 75-86).
--
--   local page = use("/Kosmos/Libraries/sysmon/network.lua").new{
--     S = S, pace = function() return pace end, bits = function() return bits end }
--   page:measure()            -- page.h
--   page:draw(g)              -- in the page's own coordinates
--   page:rows()               -- the inventory's rows 75-86, for `--tell`
--
-- **One page, two windows until S9.** Network Monitor (`netmon.lua`) draws
-- this same page in a window of its own, asking only the network; S9
-- retires that window and `netmon` opens this page.
--
-- **It counts nothing itself.** The network server counts every frame and
-- every connection's bytes (`NET_OP_STATS`), `meters.network` takes the
-- differences, and `sysmon/sample.lua` keeps the minute and makes the
-- connections into rows while a page lists them (`S:want("connections")`).
-- In and out are the theme's `io_in` and `io_out`, as in every monitor.

local ui = use("/Kosmos/Libraries/ui.lua")
local files = use("/Kosmos/Libraries/files.lua")
local sample = use("/Kosmos/Libraries/sysmon/sample.lua")
local cpu_page = use("/Kosmos/Libraries/sysmon/cpu.lua")
local theme = ui.theme
local L = ui.layout

local M = {}

M.WIDTH        = 680
local CARD_H   = 90
local PORT     = 56                     -- the card's picture
local GRAPH_H  = 120
local TOTALS_H = 58
local ROWS     = 8                      -- connections shown, the busiest
local ROW_H    = 24
local GROUND   = cpu_page.GRAPH_GROUND
local FAINT    = 0xff8b949e

-- The columns of the list, as fractions of its width: Program, To, State,
-- In, Out - the last two right-aligned.
local COLS = { 0, 0.22, 0.58, 0.74, 0.87 }

--
-- **The card, said once**: its maker split off its model only from a
-- card's name - "No network card" is a sentence, and split it read "NO"
-- over "network card" - and where it is on the network.
--
function M.card(S)
  local name = S.card or "No network card"
  local maker, model = "", name

  if S.card then
    maker, model = name:match("^(%S+)%s+(.+)$")
    if not maker then maker, model = "", name end
  end

  local info = S.net_info or {}
  local facts = info.card and ("%s \u{b7} MTU %d"):format(sample.address(info.address), info.mtu or 0)
                or "not on a network"

  return maker, model, facts
end

function M.new(spec)
  local S = spec.S
  local pace = spec.pace
  local bits = spec.bits or function() return false end
  local W = spec.w or M.WIDTH

  local page = { name = "network", title = "Network", w = W }
  local MAKER, MODEL, FACTS = M.card(S)

  page.maker, page.model, page.facts = MAKER, MODEL, FACTS

  local function rate(b) return files.rate(b, bits()) end
  local amount = files.size

  local function port(g, x, y) ui.device_picture(g, x, y, PORT, "port") end

  local function up() return sys.ticks() // S.hz end

  function page:measure()
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
    self.h = m.foot + gfx.height() + L.page_foot
    self.m = m
    return self.h
  end

  -- Where the minute is, in the page's coordinates, for a suite to look at.
  function page:graph_box()
    local m = self.m or self:measure()

    return L.page_side, m.graph, m.inner, GRAPH_H
  end

  local function heading(g, x, y, left, right, colour)
    ui.group_heading(g, x, y, page.m.inner, L.group, left, right, colour)
  end

  local function totals()
    local s = S.net_s or {}

    return {
      { "Received", amount(s.bytes_in) },
      { "Sent", amount(s.bytes_out) },
      { "Packets in \u{b7} out", sample.count(s.frames_in or 0) .. " \u{b7} " .. sample.count(s.frames_out or 0) },
      { "Dropped", sample.count((s.dropped_in or 0) + (s.dropped_out or 0)) },
    }
  end

  local function draw_totals(g, x, y)
    local cells = totals()
    local inner = page.m.inner
    local cw = inner // #cells

    g:fill_round(x, y, inner, TOTALS_H, theme.raised, L.card_r)
    g:frame_round(x, y, inner, TOTALS_H, theme.line_soft, L.card_r)

    for i, c in ipairs(cells) do
      local cx = x + (i - 1) * cw + L.row_in
      local cy = y + (TOTALS_H - gfx.height("label") - gfx.height("heading") - 2) // 2

      g:text(cx, cy, c[1], theme.text_dim, theme.raised, "label")
      g:text(cx, cy + gfx.height("label") + 2, ui.fitted(c[2], cw - 2 * L.row_in, "heading"),
             theme.text, theme.raised, "heading")
    end
  end

  local function draw_conns(g, x, y)
    local rows = S.conns
    local w = page.m.inner
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
      local ty = y + r * ROW_H + (ROW_H - gfx.height()) // 2
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

  local function peaks()
    local most_in, most_out = 0, 0

    for _, s in ipairs(S.net_h) do
      most_in, most_out = math.max(most_in, s[1] or 0), math.max(most_out, s[2] or 0)
    end

    return most_in, most_out
  end

  local function conns_heading()
    local open = S.net_open or 0

    return open > ROWS and ("%d open, the busiest %d"):format(open, ROWS) or ("%d open"):format(open)
  end

  -- "Since the machine started 2 h 14 min ago" (row 85): the foot's, and
  -- the Overview's header's.
  function page:since()
    return "Since the machine started " .. sample.since(up()) .. " ago."
  end

  function page:draw(g)
    local m = self.m or self:measure()
    local x = L.page_side

    ui.meter_card(g, x, m.card, m.inner, CARD_H, {
      picture = port, size = PORT, maker = MAKER, model = MODEL,
      facts = S:slow("network") and "slow to answer" or FACTS,
      rates = { { rate(S.net.rin), "in, now", theme.io_in }, { rate(S.net.rout), "out, now", theme.io_out } },
    })

    local most_in, most_out = peaks()

    heading(g, x, m.heading, pace().heading, ("peak %s in, %s out"):format(rate(most_in), rate(most_out)))

    local scale = ui.history.draw(g, x, m.graph, m.inner, GRAPH_H, S.net_h, {
      mode = "mirrored", columns = S.columns, ground = GROUND,
      colours = { theme.io_in, theme.io_out },
    })

    -- The scale, said at the graph's left edge - the oldest end, so the
    -- words never sit on what is happening now.
    if scale > 0 then
      g:text(x + 10, m.graph + 6, rate(scale) .. " in", FAINT, GROUND, "label")
      g:text(x + 10, m.graph + GRAPH_H - 6 - gfx.height("label"), rate(scale) .. " out", FAINT, GROUND, "label")
    end

    draw_totals(g, x, m.totals)
    heading(g, x, m.conns_heading, "Connections", conns_heading())
    draw_conns(g, x, m.conns)

    g:text(x + 3, m.foot, pace().foot .. " " .. self:since(), theme.text_dim, theme.window, "ui")
  end

  --------------------------------------------------------------------------
  -- What it says.
  --------------------------------------------------------------------------

  function page:heading()
    return ("%s | %s"):format(MODEL, FACTS)
  end

  -- A line a sample, for a suite: Network Monitor's, as it was.
  function page:tell_line()
    local top = S.conns[1]

    return ("in %d B/s, out %d B/s; %d open; busiest %s %d %d")
           :format(math.floor(S.net.rin), math.floor(S.net.rout), S.net_open or 0,
                   top and top.program or "-", top and math.floor(top.rin) or 0,
                   top and math.floor(top.rout) or 0)
  end

  -- What this page shows, a row of the inventory each (rows 75-86).
  function page:rows()
    local cells = totals()
    local most_in, most_out = peaks()
    local conns = {}

    for i, c in ipairs(S.conns) do
      if i > ROWS then break end
      conns[#conns + 1] = ("%s to %s %s %s/%s"):format(c.program, c.to, c.state, rate(c.rin), rate(c.rout))
    end

    return {
      { 75, "The card's maker and model", ("%s | %s"):format(MAKER, MODEL) },
      { 76, "Its address and MTU", FACTS },
      { 77, "In now, out now", rate(S.net.rin) .. " | " .. rate(S.net.rout) },
      { 78, "The minute, in over out", ("%d columns, mirrored"):format(#S.net_h) },
      { 79, "The peak each way", ("peak %s in, %s out"):format(rate(most_in), rate(most_out)) },
      { 80, "Received", cells[1][2] },
      { 81, "Sent", cells[2][2] },
      { 82, "Packets in, out", cells[3][2] },
      { 83, "Dropped", cells[4][2] },
      { 84, "Connections", conns_heading() .. (#conns > 0 and ": " .. table.concat(conns, ", ") or "") },
      { 85, "Since the machine started", self:since() },
      { 86, "Bytes or bits a second", bits() and "bits, from the dots" or "bytes, from the dots" },
    }
  end

  page:measure()
  return page
end

return M
