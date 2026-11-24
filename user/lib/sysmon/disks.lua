-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- System Monitor's Disks page: Disk Monitor's window, moved in whole -
-- the drives down the left, the chosen one's card, its minute, how long a
-- request takes, its cache, its totals and the programs using it
-- (`docs/sysmonitor.md` S5; inventory rows 61-74).
--
--   local page = use("/Kosmos/Libraries/sysmon/disks.lua").new{
--     S = S, pace = function() return pace end, bits = function() return bits end,
--     prefix = "sysmon: disks", on_choose = function(key) end }
--   page:measure()            -- page.h
--   page:draw(g)              -- in the page's own coordinates
--   page:mouse(action, x, y)  -- a drive chosen in the list
--   page:choose(key)          -- or from outside: the sidebar's row for it
--   page:rows()               -- the inventory's rows 61-74, for `--tell`
--
-- **One page, two windows until S9.** Disk Monitor (`diskmon.lua`) draws
-- this same page in a window of its own, asking only the drives; S9
-- retires that window and `diskmon` opens this page.
--
-- **It counts nothing itself**: each drive's server does - `.device`'s
-- counts and `.programs`' by the program that asked (`deviceproto.h`) -
-- and `meters.drive` and `meters.flows` take the differences, in
-- `sysmon/sample.lua`. The chosen drive's programs are asked only while
-- this page shows (`S:want("programs", "chosen")`).
--
-- **Reading and writing are the theme's `io_in` and `io_out`**, as in
-- every monitor: reading above the line and writing below it, on one scale
-- the graph says.

local ui = use("/Kosmos/Libraries/ui.lua")
local files = use("/Kosmos/Libraries/files.lua")
local cpu_page = use("/Kosmos/Libraries/sysmon/cpu.lua")
local theme = ui.theme
local L = ui.layout

local M = {}

M.WIDTH        = 1040
local LIST_W   = 300
local DEV_H    = 92
local LIST_TOP = 40
local CARD_H   = 90
local PIC      = 56
local GRAPH_H  = 120
local TOTALS_H = 58
local ROWS     = 6
local ROW_H    = 24
local GROUND   = cpu_page.GRAPH_GROUND
local FAINT    = 0xff8b949e

-- The right side's width at the page's own width: `measure` works it out
-- from `self.w`, so a window grown by its grip grows the graphs (S6).
local function inner_at(w) return w - LIST_W - 2 * L.page_side end

M.LIST_W, M.LIST_TOP, M.DEV_H = LIST_W, LIST_TOP, DEV_H

local KIND = { disk = "Disk", usb = "USB stick", share = "Share", mem = "Memory" }

--
-- **A drive's picture**, as the drawing has each kind: an SSD, a stick, a
-- share, memory. A disk and a stick as the kit draws them for every card
-- that is one (`ui.device_picture`, This Machine's too); a share and
-- memory are this page's own.
--
function M.picture(kind, size)
  return function(g, x, y)
    local s = size

    if kind == "usb" or kind == "disk" then
      ui.device_picture(g, x, y, s, kind == "usb" and "stick" or "ssd")
      return
    end

    g:fill_round(x, y, s, s, 0xff2b3240, s // 6)

    if kind == "share" then
      g:fill_round(x + s // 6, y + s * 30 // 100, s * 2 // 3, s * 40 // 100, 0xff3b82f6, 4)
      g:fill(x + s * 45 // 100, y + s * 70 // 100, s // 10, s // 8, 0xffaab4c4)
      g:fill(x + s // 4, y + s * 82 // 100, s // 2, 2, 0xffaab4c4)
    elseif kind == "mem" then
      g:fill_round(x + s // 8, y + s * 34 // 100, s * 3 // 4, s * 30 // 100, 0xff2f8f55, 3)

      for i = 0, 3 do
        g:fill(x + s // 5 + i * s * 16 // 100, y + s * 40 // 100, s // 10, s // 6, 0xff16181d)
      end
    end
  end
end

function M.new(spec)
  local S = spec.S
  local pace = spec.pace
  local bits = spec.bits or function() return false end
  local prefix = spec.prefix or "sysmon: disks"
  local counter_hz = S.hz

  local page = { name = "disks", title = "Disks", w = M.WIDTH }
  local INNER = inner_at(M.WIDTH)          -- the right side's, from the last `measure`

  local function rate(b) return files.rate(b, bits()) end
  local amount = files.size

  -- The drive chosen: by its key, which outlives a stick coming and going;
  -- the first drive when the chosen one went.
  local function chosen()
    for i, d in ipairs(S.drives) do
      if d.key == S.chosen_drive then return d, i end
    end

    local d = S.drives[1]

    S.chosen_drive = d and d.key or nil
    return d, 1
  end

  local function meter(d) return d and S.drive[d.key] or nil end

  -- The chosen drive's programs, as the sample last heard them.
  local function programs()
    local d = chosen()
    local p = d and S.programs[d.key]

    return p and p.list or {}, p and p.open or 0
  end

  function page:choose(key)
    if S.chosen_drive == key then return end

    S.chosen_drive = key
    print(("%s chose %s"):format(prefix, tostring(key)))
    if spec.on_choose then spec.on_choose(key) end
  end

  --------------------------------------------------------------------------
  -- Where everything goes.
  --------------------------------------------------------------------------

  function page:measure()
    local m = { inner = inner_at(self.w) }

    INNER = m.inner
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

    -- And at least as tall as the list of drives.
    local list_h = LIST_TOP + #S.drives * DEV_H + L.page_foot

    self.h = math.max(m.foot + gfx.height() + L.page_foot, list_h)
    self.m = m
    self.drawn_drives = #S.drives
    return self.h
  end

  -- Where the minute is, in the page's coordinates, for a suite to look at.
  function page:graph_box()
    local m = self.m or self:measure()

    return LIST_W + L.page_side, m.graph, INNER, GRAPH_H
  end

  -- Whether a drive came or went since the page was measured.
  function page:stale() return self.drawn_drives ~= #S.drives end

  --------------------------------------------------------------------------
  -- The list of drives, down the left.
  --------------------------------------------------------------------------

  local function draw_list(g, h)
    local here = chosen()

    g:fill(0, 0, LIST_W, h, theme.sunken)
    g:fill(LIST_W - 1, 0, 1, h, theme.line_soft)
    g:text(L.page_side, 14, "Drives", theme.text_dim, theme.sunken, "label")

    for i, d in ipairs(S.drives) do
      local y = LIST_TOP + (i - 1) * DEV_H
      local s = meter(d) or {}

      if d == here then g:fill_round(8, y, LIST_W - 16, DEV_H - 6, theme.raised, L.card_r) end

      M.picture(d.kind, 36)(g, 18, y + 10)

      local tx, room = 66, LIST_W - 66 - 16

      g:text(tx, y + 8, ui.fitted(d.name, room, "heading"), theme.text, theme.sunken, "heading")
      g:text(tx, y + 8 + gfx.height("heading"), ui.fitted(d.sub or "", room, "label"),
             theme.text_dim, theme.sunken, "label")

      local said = S:slow(d.key) and "slow to answer"
                   or s.counts and ("%s read \u{b7} %s write"):format(rate(s.rin), rate(s.rout))
                   or "says nothing of its reading and writing"

      g:text(tx, y + 8 + gfx.height("heading") + gfx.height("label"), ui.fitted(said, room, "label"),
             theme.text_dim, theme.sunken, "label")

      -- On the list's own colour, not the big graph's dark ground: twenty
      -- pixels of near-black under every drive read as bars, not graphs.
      ui.history.draw(g, tx, y + DEV_H - 30, room, 20, s.history or {}, {
        mode = "mirrored", columns = S.columns, ground = theme.raised, radius = 4,
        colours = { theme.io_in, theme.io_out },
      })
    end
  end

  function page:mouse(action, x, y)
    return ui.click(self, action, x, y, function(px, py)
      if px >= LIST_W then return nil end

      local i = (py - LIST_TOP) // DEV_H + 1
      local d = py >= LIST_TOP and S.drives[i]

      if d then return i, function() page:choose(d.key) end end
    end)
  end

  --------------------------------------------------------------------------
  -- The chosen drive, on the right.
  --------------------------------------------------------------------------

  local function heading(g, x, y, left, right, colour)
    ui.group_heading(g, x, y, INNER, L.group, left, right, colour)
  end

  -- How long a read and a write take, the cache's share and the totals:
  -- over the pace where there were any, since the start where not.
  local function totals_of(s)
    local c = s.counts or {}
    local reads, writes = c.reads or 0, c.writes or 0
    local overall_read = reads > 0 and (c.read_counter_ticks or 0) / reads / counter_hz * 1000 or nil
    local overall_write = writes > 0 and (c.write_counter_ticks or 0) / writes / counter_hz * 1000 or nil
    local ms = function(v) return v and ("%.2f ms"):format(v) or "-" end
    local hits, misses = c.cache_hits or 0, c.cache_misses or 0
    local cache = s.cache or ((hits + misses) > 0 and hits * 100 // (hits + misses) or nil)

    return {
      { "A read takes", ms(s.read_ms or overall_read) },
      { "A write takes", ms(s.write_ms or overall_write) },
      { "Cache answered", cache and (cache .. "%") or "-" },
      { "Read \u{b7} written", amount(c.read_bytes) .. " \u{b7} " .. amount(c.write_bytes) },
    }
  end

  local function draw_totals(g, x, y, s)
    local cells = totals_of(s)
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
    local list = programs()
    local w = INNER
    local function col(i) return x + 8 + math.floor((w - 16) * COLS[i]) end
    local hy = y + (ROW_H - gfx.height("label")) // 2

    g:text(col(1), hy, "Program", theme.text_dim, theme.window, "label")
    g:text(col(3) - 12 - gfx.measure("Reading", "label"), hy, "Reading", theme.text_dim, theme.window, "label")
    g:text(col(4) - 12 - gfx.measure("Writing", "label"), hy, "Writing", theme.text_dim, theme.window, "label")
    g:text(col(4), hy, "Last file", theme.text_dim, theme.window, "label")
    g:fill(x, y + ROW_H - 1, w, 1, theme.line_soft)

    for r = 1, math.min(ROWS, #list) do
      local p = list[r]
      local ty = y + r * ROW_H + (ROW_H - gfx.height()) // 2
      local rin, rout = rate(p.rin), rate(p.rout)

      g:text(col(1), ty, ui.fitted(p.program, col(2) - col(1) + 40, "ui"),
             p.alive and theme.text or theme.text_dim, theme.window, "ui")
      g:text(col(3) - 12 - gfx.measure(rin), ty, rin, theme.io_in, theme.window, "ui")
      g:text(col(4) - 12 - gfx.measure(rout), ty, rout, theme.io_out, theme.window, "ui")
      g:text(col(4), ty, ui.fitted(p.last or "", x + w - 8 - col(4), "ui", true),
             theme.text_dim, theme.window, "ui")
    end

    if #list == 0 then
      g:text(col(1), y + ROW_H + (ROW_H - gfx.height()) // 2, "Nothing has read or written it yet.",
             theme.text_dim, theme.window, "ui")
    end
  end

  local function peaks(s)
    local most_in, most_out = 0, 0

    for _, h in ipairs(s.history or {}) do
      most_in, most_out = math.max(most_in, h[1] or 0), math.max(most_out, h[2] or 0)
    end

    return most_in, most_out
  end

  local function programs_heading()
    local list, open = programs()

    return open > 0 and ("%d now"):format(open) or ("%d since start"):format(#list)
  end

  function page:draw(g)
    local m = self.m or self:measure()

    draw_list(g, self.h)

    local d = chosen()

    if not d then return end

    local s = meter(d) or {}
    local x = LIST_W + L.page_side

    ui.meter_card(g, x, m.card, INNER, CARD_H, {
      picture = M.picture(d.kind, PIC), size = PIC, maker = KIND[d.kind] or "", model = d.name,
      facts = d.sub or d.path,
      rates = { { rate(s.rin or 0), "reading, now", theme.io_in }, { rate(s.rout or 0), "writing, now", theme.io_out } },
    })

    local most_in, most_out = peaks(s)

    heading(g, x, m.heading, pace().heading, ("peak %s reading, %s writing"):format(rate(most_in), rate(most_out)))

    local scale = ui.history.draw(g, x, m.graph, INNER, GRAPH_H, s.history or {}, {
      mode = "mirrored", columns = S.columns, ground = GROUND,
      colours = { theme.io_in, theme.io_out },
    })

    if scale > 0 then
      g:text(x + 10, m.graph + 6, rate(scale) .. " reading", FAINT, GROUND, "label")
      g:text(x + 10, m.graph + GRAPH_H - 6 - gfx.height("label"), rate(scale) .. " writing",
             FAINT, GROUND, "label")
    end

    draw_totals(g, x, m.totals, s)
    heading(g, x, m.progs_heading, "Programs using it", programs_heading())
    draw_programs(g, x, m.progs)
    g:text(x + 3, m.foot, pace().foot, theme.text_dim, theme.window, "ui")
  end

  --------------------------------------------------------------------------
  -- What it says.
  --------------------------------------------------------------------------

  function page:heading()
    local d = chosen()

    return d and ("%s, %s"):format(d.name, d.path) or "no drives"
  end

  -- A line a sample, for a suite: Disk Monitor's, as it was.
  function page:tell_line()
    local d = chosen()

    if not d then return nil end

    local s = meter(d) or {}
    local top = programs()[1]

    return ("%s read %d B/s, wrote %d B/s; busiest %s %d %d")
           :format(d.key, math.floor(s.rin or 0), math.floor(s.rout or 0),
                   top and top.program or "-", top and math.floor(top.rin) or 0,
                   top and math.floor(top.rout) or 0)
  end

  -- What this page shows, a row of the inventory each (rows 61-74).
  function page:rows()
    local d = chosen()
    local s = meter(d) or {}
    local names, pictures, said = {}, {}, {}

    for _, dd in ipairs(S.drives) do
      local ms = meter(dd) or {}

      names[#names + 1] = dd.name .. " " .. dd.path
      pictures[#pictures + 1] = dd.path .. " " .. tostring(dd.kind)
      said[#said + 1] = dd.path .. " " .. (ms.counts and (rate(ms.rin) .. "/" .. rate(ms.rout))
                                         or "says nothing of its reading and writing")
    end

    local silent = {}

    for _, dd in ipairs(S.drives) do
      if (meter(dd) or {}).silent then silent[#silent + 1] = dd.path end
    end

    local cells = totals_of(s)
    local most_in, most_out = peaks(s)
    local list = programs()
    local progs = {}

    for i, p in ipairs(list) do
      if i > ROWS then break end
      progs[#progs + 1] = ("%s %s/%s %s"):format(p.program, rate(p.rin), rate(p.rout), p.last or "-")
    end

    local none = "no drives"

    return {
      { 61, "The drives", #names > 0 and table.concat(names, ", ") or none },
      { 62, "Each drive's picture", #pictures > 0 and table.concat(pictures, ", ") or none },
      { 63, "Each drive's name and where", #S.drives > 0 and (function()
          local out = {}
          for _, dd in ipairs(S.drives) do out[#out + 1] = dd.name .. " (" .. (dd.sub or dd.path) .. ")" end
          return table.concat(out, ", ") end)() or none },
      { 64, "Each drive's reading and writing", #said > 0 and table.concat(said, ", ") or none },
      { 65, "A drive that does not answer", #silent > 0 and (table.concat(silent, ", ")
                                               .. " says nothing of its reading and writing") or "every drive answers" },
      { 66, "The chosen drive's card", d and ("%s | %s | %s | %s reading | %s writing"):format(
              KIND[d.kind] or "", d.name, d.sub or d.path, rate(s.rin or 0), rate(s.rout or 0)) or none },
      { 67, "The minute, reading over writing", d and ("%d columns, mirrored"):format(#(s.history or {})) or none },
      { 68, "The peak each way", d and ("peak %s reading, %s writing"):format(rate(most_in), rate(most_out)) or none },
      { 69, "A read takes", cells[1][2] },
      { 70, "A write takes", cells[2][2] },
      { 71, "Cache answered", cells[3][2] },
      { 72, "Read, written", cells[4][2] },
      { 73, "Programs using it", d and (programs_heading() .. (#progs > 0 and ": " .. table.concat(progs, ", ") or ""))
                                 or none },
      { 74, "Bytes or bits a second", bits() and "bits, from the dots" or "bytes, from the dots" },
    }
  end

  page:measure()
  return page
end

return M
