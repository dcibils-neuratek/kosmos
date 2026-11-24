-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- System Monitor's Memory page: used, free and Nebula's part over the
-- history window, the four pools each of its ceiling, who holds the most,
-- and what the GPU's buffers and the drives' caches hold (`docs/sysmonitor.md`
-- S6; the inventory's rows 21-24, 34, 36, 48 and 71).
--
--   local page = use("/Kosmos/Libraries/sysmon/memory.lua").new{
--     S = S, w = 680, pace = function() return pace end }
--   page:measure()            -- page.h
--   page:draw(g)
--   page:rows()               -- its rows of the inventory, for `--tell`
--
-- **A page that is new, from readings that exist today.** Memory and the
-- pools are the devices server's (`/Devices/memory`, `/Devices/kernel`),
-- every process's pages are Nebula's (`sys.processes()`), the GPU's
-- buffers are its driver's (`GPU_OP_STATS`) and a drive's cache is its
-- server's (`.device`); `sysmon/sample.lua` asks them, `meters.lua` takes
-- the differences, and this draws. **The address spaces' pool is drawn for
-- the first time**: Process Viewer's header counted spaces and drew no bar.
--
-- **What is not measured says so.** What `/Temporary` holds is no reply's:
-- its server keeps it and nothing asks (`sysmonitor.md`, *Sensors*), so the
-- page says "not measured on this machine" in its place, never a nought.
--
-- **Nothing here asks anything.** The page draws what the sample last
-- heard, and the largest ten are the sample's (`S.largest`), sorted where
-- the busiest five are.

local ui = use("/Kosmos/Libraries/ui.lua")
local files = use("/Kosmos/Libraries/files.lua")
local proctable = use("/Kosmos/Libraries/proctable.lua")
local cpu_page = use("/Kosmos/Libraries/sysmon/cpu.lua")
local sample = use("/Kosmos/Libraries/sysmon/sample.lua")
local theme = ui.theme
local L = ui.layout

local M = {}

local UNMEASURED = cpu_page.UNMEASURED
local GROUND = cpu_page.GRAPH_GROUND
local MEM_COL = 0xff4fb3ff              -- the Overview's memory, the same in every look
local NEBULA_COL = 0xff2f7fc0           -- Nebula's part, under the rest

local GRAPH_H = 96
local POOL_H, POOL_GAP = 35, 14         -- a pool's line and its bar, as Processes' band
local ROW_H = 24                        -- a holder's row
local MOST = 10                         -- the largest shown, and Nebula

local function gb(mb) return ("%.1f GB"):format(mb / 1024) end

-- Each pool, by the devices server's names, and what the page calls it.
local POOLS = { { "threads", "Threads" }, { "processes", "Processes" },
                { "endpoints", "Endpoints" }, { "spaces", "Address spaces" } }

function M.new(spec)
  local S = spec.S
  local W = spec.w
  local pace = spec.pace

  local page = { name = "memory", title = "Memory", w = W }

  local LH, UH = gfx.height("label"), gfx.height("ui")

  --------------------------------------------------------------------------
  -- What it says, from what the sample last heard.
  --------------------------------------------------------------------------

  local function nebula_bytes()
    local k = S.kernel or {}

    return S.nebula_bytes or (k.memory_kb and k.memory_kb * 1024) or nil
  end

  -- "1.9 GB of 7.9 GB used · Nebula 64 MB": the heading, and the Overview's.
  function page:heading()
    local t = S.totals

    if not t.used_mb then return "memory not read yet" end

    local nb = nebula_bytes()

    return ("%d of %d MB used \u{b7} Nebula %s"):format(t.used_mb, t.total_mb,
                                                       nb and files.size(nb) or "-")
  end

  -- What the GPU's buffers hold: "19.0 MB in 3 buffers".
  local function gpu_words()
    local now = S.gpu_lib and S.gpu.now

    if not now then return "no GPU here" end

    return ("%.1f MB in %d buffers"):format((S.gpu.bytes or 0) / 1048576, #(now.buffers or {}))
  end

  -- Each drive's cache: "/Home 94%", or "-" before two readings.
  local function cache_words()
    local out = {}

    for _, d in ipairs(S.drives or {}) do
      local m = S.drive[d.key]

      out[#out + 1] = ("%s %s"):format(d.path, (m and m.cache) and (m.cache .. "%") or "-")
    end

    return #out > 0 and table.concat(out, ", ") or "no drives"
  end

  function page:spaces()
    local t = S.totals

    return ("%s of %s"):format(tostring(t.spaces or "-"), tostring(t.spaces_max or "-"))
  end

  --------------------------------------------------------------------------
  -- Where everything goes.
  --------------------------------------------------------------------------

  function page:measure()
    local m = { inner = self.w - 2 * L.page_side }
    local y = L.page_top

    m.used = y
    y = y + L.group + GRAPH_H + L.between

    m.pools = y
    y = y + L.group + 2 * POOL_H + POOL_GAP + L.between

    m.holders = y
    y = y + L.group + 4 + (MOST + 1) * ROW_H + 4 + L.between

    m.elsewhere = y
    y = y + L.group + 2 * (LH + UH + 8)

    m.foot = y + 10
    self.h = m.foot + gfx.height() + L.page_foot
    self.m = m
    return m
  end

  local function heading(g, x, y, w, title, right)
    g:text(x + 3, y + (L.group - gfx.height("heading")) // 2, title, theme.text, theme.window, "heading")

    if right then
      g:text(x + w - gfx.measure(right), y + (L.group - gfx.height()) // 2, right,
             theme.text_dim, theme.window, "ui")
    end
  end

  local function draw_used(g, x, y, w)
    local t = S.totals
    local right = t.used_mb and ("%s of %s used \u{b7} %s free"):format(gb(t.used_mb), gb(t.total_mb),
                                                                      gb(t.total_mb - t.used_mb)) or "-"

    heading(g, x, y, w, pace().heading, right)
    ui.history.draw(g, x, y + L.group, w, GRAPH_H, S.memory_h, {
      mode = "stacked", columns = S.columns, ground = GROUND, radius = 6,
      colours = { NEBULA_COL, MEM_COL } })

    -- What the two colours are, on the graph's ground.
    local ly = y + L.group + 8
    local lx = x + 10
    local nb = nebula_bytes()

    for _, part in ipairs({ { NEBULA_COL, "Nebula " .. (nb and files.size(nb) or "-") },
                            { MEM_COL, "the rest spoken for" } }) do
      g:fill_round(lx, ly + (LH - 9) // 2, 9, 9, part[1], 2)
      g:text(lx + 13, ly, part[2], theme.text_dim, GROUND, "label")
      lx = lx + 13 + gfx.measure(part[2], "label") + 14
    end
  end

  local function draw_pools(g, x, y, w)
    local t = S.totals
    local gap = 24
    local each = (w - gap) // 2

    heading(g, x, y, w, "The pools", "each of the most there can be")

    for i, p in ipairs(POOLS) do
      local col, row = (i - 1) % 2, (i - 1) // 2

      proctable.level(g, x + col * (each + gap), y + L.group + row * (POOL_H + POOL_GAP), each, POOL_H,
                      p[2], t[p[1]], t[p[1] .. "_max"])
    end
  end

  -- The largest ten and Nebula, each with a bar against the largest.
  local function draw_holders(g, x, y, w)
    local list = S.largest or {}
    local nb = nebula_bytes()
    local most = math.max(1, nb or 0, list[1] and list[1].bytes or 0)

    heading(g, x, y, w, "Who holds the most", ("the %d largest and Nebula"):format(math.min(MOST, #list)))

    local ty = y + L.group

    g:fill_round(x, ty, w, (MOST + 1) * ROW_H + 8, theme.raised, L.card_r)
    g:frame_round(x, ty, w, (MOST + 1) * ROW_H + 8, theme.line_soft, L.card_r)

    local rows = { { name = "nebula", id = 0, bytes = nb or 0 } }

    for i = 1, math.min(MOST, #list) do rows[#rows + 1] = list[i] end

    local size_w = gfx.measure("000.0 MB", "ui")
    local bar_w = w // 3
    local ry = ty + 4

    for _, r in ipairs(rows) do
      local size = files.size(r.bytes)
      local bx = x + w - L.row_in - size_w - 12 - bar_w
      local by = ry + (ROW_H - 6) // 2

      g:text(x + L.row_in, ry + (ROW_H - UH) // 2,
             ui.fitted(("%s \u{b7} %d"):format(r.name, r.id), bx - x - 2 * L.row_in, "ui"),
             r.id == 0 and theme.text_dim or theme.text, theme.raised, "ui")
      g:fill_round(bx, by, bar_w, 6, theme.track, 3)
      g:fill_round(bx, by, math.max(6, bar_w * r.bytes // most), 6, r.id == 0 and NEBULA_COL or MEM_COL, 3)
      g:text(x + w - L.row_in - gfx.measure(size, "ui"), ry + (ROW_H - UH) // 2, size,
             theme.text_dim, theme.raised, "ui")
      ry = ry + ROW_H
    end
  end

  -- What the GPU's buffers, `/Temporary` and the drives' caches hold.
  local function draw_elsewhere(g, x, y, w)
    heading(g, x, y, w, "Elsewhere")

    local cells = {
      { "THE GPU'S BUFFERS", gpu_words() },
      { "WHAT /TEMPORARY HOLDS", UNMEASURED },
      { "THE DRIVES' CACHES ANSWERED", cache_words() },
    }
    local cw = (w - 2 * 12) // #cells
    local cy = y + L.group

    for i, c in ipairs(cells) do
      local cx = x + (i - 1) * (cw + 12)

      g:text(cx + 3, cy, ui.fitted(c[1], cw - 6, "label"), theme.text_dim, theme.window, "label")
      g:text(cx + 3, cy + LH, ui.fitted(c[2], cw - 6, "ui"),
             c[2] == UNMEASURED and theme.text_dim or theme.text, theme.window, "ui")
    end

    -- The GPU's buffers against the GPU page's scale, where there is one.
    if S.gpu_lib then
      local bw = cw - 6
      local filled = math.min(bw, bw * (S.gpu.bytes or 0) // sample.GPU_MEMORY)

      g:fill_round(x + 3, cy + LH + UH + 4, bw, 6, theme.track, 3)
      if filled > 0 then g:fill_round(x + 3, cy + LH + UH + 4, math.max(6, filled), 6, MEM_COL, 3) end
    end
  end

  function page:draw(g)
    local m = self.m or self:measure()
    local x = L.page_side

    draw_used(g, x, m.used, m.inner)
    draw_pools(g, x, m.pools, m.inner)
    draw_holders(g, x, m.holders, m.inner)
    draw_elsewhere(g, x, m.elsewhere, m.inner)
    g:text(x + 3, m.foot, pace().foot, theme.text_dim, theme.window, "ui")
  end

  --------------------------------------------------------------------------
  -- Its rows of `docs/sysmonitor.md`'s inventory, for `--tell`.
  --------------------------------------------------------------------------

  function page:rows()
    local t = S.totals
    local nb = nebula_bytes()
    local holders = {}

    for i = 1, math.min(MOST, #(S.largest or {})) do
      local r = S.largest[i]

      holders[#holders + 1] = ("%s %s"):format(r.name, files.size(r.bytes))
    end

    local function pool(n, name)
      return { n, name, ("%s of %s"):format(tostring(t[name] or "-"), tostring(t[name .. "_max"] or "-")) }
    end

    return {
      { 21, "Used, free and Nebula's", t.used_mb and ("%d of %d MB, %d free, Nebula %s; %d columns"):format(
              t.used_mb, t.total_mb, t.total_mb - t.used_mb, nb and files.size(nb) or "-", #S.memory_h) or "-" },
      pool(22, "threads"), pool(23, "processes"), pool(24, "endpoints"),
      { 34, "Who holds the most", #holders > 0 and table.concat(holders, ", ") or "-" },
      { 36, "Nebula's part", nb and ("holds %s"):format(files.size(nb)) or "-" },
      { 48, "The GPU's buffers", gpu_words() },
      { 71, "The drives' caches answered", cache_words() },
    }
  end

  page:measure()
  return page
end

return M
