-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- System Monitor's Heat and power page: every temperature and every watt
-- the machine measures, on one page (`docs/sysmonitor.md` S6; the
-- inventory's rows 7-9, 54 and 102).
--
--   local page = use("/Kosmos/Libraries/sysmon/heat.lua").new{
--     S = S, w = 680, pace = function() return pace end }
--   page:measure()            -- page.h
--   page:draw(g)
--   page:rows()               -- its rows of the inventory, for `--tell`
--   page:limits()             -- TjMax, PL1, its window and PL2, in words
--
-- **TjMax, PL1 and PL2 drawn for the first time.** Nebula has read them
-- since the processor's heat arrived (`MSR_TEMPERATURE_TARGET`,
-- `MSR_PKG_POWER_LIMIT`), `pulse.heat` has carried them, and `clocks`
-- printed them at the prompt; no window drew them. Here the package's
-- temperature is a graph against TjMax and its power a graph against PL1
-- and PL2, with "held" marked under it where the package was held - the
-- same pair the CPU page draws (`cpu.lua`'s `package_graphs`), with the
-- limits beside them in words.
--
-- **Each core's temperature as a row of numbers** (`IA32_THERM_STATUS`,
-- read on each core's tick); **the GPU's power** (RAPL's graphics plane)
-- now and over the history window; **the battery's charge** and whether
-- it charges, where the board reads one.
--
-- **Each reading this machine does not give says so, by name**, with
-- "not measured on this machine" in its place and never a nought: on
-- QEMU's processor every one of them, and on every machine today the
-- GPU's own temperature, the cores' and the memory's power planes, the
-- fans, a disk's temperature and the battery's draw (`sysmonitor.md`,
-- *Sensors*, and decision 5, which orders them).
--
-- **Nothing here asks anything.** The page draws what the sample last
-- heard: `sys.info()` and `sys.cpuload()` through `meters.heat`, the GPU's
-- through `meters.gpu`, and `/Devices/battery`.

local ui = use("/Kosmos/Libraries/ui.lua")
local cpu_page = use("/Kosmos/Libraries/sysmon/cpu.lua")
local theme = ui.theme
local L = ui.layout

local M = {}

local UNMEASURED = cpu_page.UNMEASURED
local GROUND = cpu_page.GRAPH_GROUND

local PACKAGE_H = 96                    -- each of the package's two graphs
local GPU_H = 56                        -- the GPU's power over the window
local CELL_W = 96                       -- a core's temperature
local KV_H = 26                         -- a reading's row

-- What no machine gives today, each by its name (`sysmonitor.md`,
-- *Sensors*: what each would cost).
local NOT_READ = {
  "The GPU's own temperature",
  "The cores' power (RAPL PP0)",
  "The memory's power (RAPL DRAM)",
  "The fans",
  "A disk's temperature (SMART)",
  "The battery's draw and time left",
}

function M.new(spec)
  local S = spec.S
  local W = spec.w
  local pace = spec.pace

  local page = { name = "heat", title = "Heat and power", w = W }

  local LH, UH = gfx.height("label"), gfx.height("ui")

  --------------------------------------------------------------------------
  -- Each reading in words, from what the sample last heard.
  --------------------------------------------------------------------------

  local function heat() return S.hot.heat end

  local function gpu_watts()
    local w = S.gpu_lib and S.gpu.watts

    return w and ("%.2f W"):format(w) or UNMEASURED
  end

  local function battery_words()
    local b = S.battery

    if not b then return "none on this machine" end

    return ("%d%%, %s"):format(b.percent, tostring(b.state or "-"))
  end

  -- The limits, each read once by Nebula and kept.
  local function limit(v, fmt)
    return v and fmt:format(v) or UNMEASURED
  end

  function page:limits()
    local h = heat() or {}

    return ("TjMax %s; PL1 %s; its window %s; PL2 %s"):format(
           limit(h.limit_c, "%d °C"), limit(h.limit_w, "%.0f W"),
           limit(h.window_s, "%.1f s"), limit(h.burst_w, "%.0f W"))
  end

  -- The first line, when the page is shown: the package's now.
  function page:heading()
    local h = heat()

    if not (h and h.temp_c) then return "the package: " .. UNMEASURED end

    return ("the package: %d °C%s"):format(h.temp_c, h.watts and (" \u{b7} %.1f W"):format(h.watts) or "")
  end

  -- Every reading as a row: its name, its value now, and whether it is one.
  local function readings()
    local h = heat() or {}
    local list = {
      { "The package's temperature", h.temp_c and ("%d °C"):format(h.temp_c) or UNMEASURED },
      { "Where it slows to cool (TjMax)", limit(h.limit_c, "%d °C") },
      { "The package's power", h.watts and ("%.1f W"):format(h.watts) or UNMEASURED },
      { "What it may draw sustained (PL1)", limit(h.limit_w, "%.0f W") },
      { "PL1 is averaged over", limit(h.window_s, "%.1f s") },
      { "What it may draw a burst (PL2)", limit(h.burst_w, "%.0f W") },
      { "Held", cpu_page.held_words(S) },
      { "The hottest core", S:hottest() and ("%d °C"):format(S:hottest()) or UNMEASURED },
      { "The GPU's power", gpu_watts() },
      { "The GPU's temperature", h.temp_c and ("%d °C, the package's"):format(h.temp_c) or UNMEASURED },
      { "The battery", battery_words() },
    }

    for _, name in ipairs(NOT_READ) do list[#list + 1] = { name, UNMEASURED } end

    return list
  end

  --------------------------------------------------------------------------
  -- Where everything goes.
  --------------------------------------------------------------------------

  function page:measure()
    local m = { inner = self.w - 2 * L.page_side }
    local y = L.page_top

    m.package = y
    y = y + L.group + cpu_page.PACKAGE_TOP + PACKAGE_H + 3 + cpu_page.HELD_H + L.between

    m.cores = y
    local across = math.max(1, m.inner // CELL_W)
    -- A row of cells a few cores where the processor keeps its heat - the
    -- limit read when the window opened says so before the first sample
    -- has - and one line saying it does not where it keeps none.
    local kept = (S.info and S.info.limit_c) or next(S.temps) ~= nil
    local down = kept and (S.cores + across - 1) // across or 1

    m.across = across
    y = y + L.group + down * (LH + UH + 8) + L.between

    if S.gpu_lib then
      m.gpu = y
      y = y + L.group + GPU_H + L.between
    end

    m.readings = y
    m.kv_rows = (#readings() + 1) // 2
    y = y + L.group + m.kv_rows * KV_H + 8

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

  local function draw_cores(g, x, y, w, m)
    local any = false

    for c = 1, S.cores do
      if S.temps[c] then any = true end
    end

    heading(g, x, y, w, "Each core", any and "its own sensor, on its tick" or nil)

    local cy = y + L.group

    if not any then
      g:text(x + 3, cy, UNMEASURED, theme.text_dim, theme.window, "ui")
      return
    end

    for c = 1, S.cores do
      local col, row = (c - 1) % m.across, (c - 1) // m.across
      local cx, ry = x + col * CELL_W, cy + row * (LH + UH + 8)
      local t = S.temps[c]

      g:text(cx + 3, ry, "CORE " .. c, theme.text_dim, theme.window, "label")
      g:text(cx + 3, ry + LH, t and ("%d °C"):format(t) or "-",
             t and cpu_page.HEAT_COL or theme.text_dim, theme.window, "ui")
    end
  end

  local function draw_gpu(g, x, y, w)
    heading(g, x, y, w, "The GPU's power", gpu_watts())
    ui.history.draw(g, x, y + L.group, w, GPU_H, S.gpu_watts_h, {
      mode = "line", columns = S.columns, ground = GROUND, radius = 6,
      most = math.max(5, math.ceil(ui.history.most(S.gpu_watts_h) / 5) * 5),
      colours = { cpu_page.WATT_COL } })
  end

  local function draw_readings(g, x, y, w, m)
    heading(g, x, y, w, "Every reading", "what this machine measures, and what it does not")

    local list = readings()
    local ty = y + L.group
    local gap = 16
    local cw = (w - gap) // 2

    g:fill_round(x, ty, w, m.kv_rows * KV_H + 8, theme.raised, L.card_r)
    g:frame_round(x, ty, w, m.kv_rows * KV_H + 8, theme.line_soft, L.card_r)

    for i, r in ipairs(list) do
      local col, row = (i - 1) // m.kv_rows, (i - 1) % m.kv_rows
      local rx = x + col * (cw + gap)
      local ry = ty + 4 + row * KV_H + (KV_H - UH) // 2
      local value = r[2]
      local room = cw - 2 * L.row_in

      g:text(rx + L.row_in, ry, ui.fitted(r[1], room * 3 // 5, "ui"), theme.text_dim, theme.raised, "ui")
      value = ui.fitted(value, room * 2 // 5, "ui")
      g:text(rx + cw - L.row_in - gfx.measure(value, "ui"), ry, value,
             r[2] == UNMEASURED and theme.text_dim or theme.text, theme.raised, "ui")
    end
  end

  function page:draw(g)
    local m = self.m or self:measure()
    local x = L.page_side

    heading(g, x, m.package, m.inner, "The package", pace().heading)
    cpu_page.package_graphs(g, x, m.package + L.group, m.inner, PACKAGE_H, S)
    draw_cores(g, x, m.cores, m.inner, m)
    if m.gpu then draw_gpu(g, x, m.gpu, m.inner) end
    draw_readings(g, x, m.readings, m.inner, m)
    g:text(x + 3, m.foot, pace().foot, theme.text_dim, theme.window, "ui")
  end

  --------------------------------------------------------------------------
  -- Its rows of `docs/sysmonitor.md`'s inventory, for `--tell`.
  --------------------------------------------------------------------------

  function page:rows()
    return {
      { 7, "The package's temperature, against TjMax", cpu_page.temperature_words(S) },
      { 8, "The package's power, against PL1 and PL2", cpu_page.power_words(S) },
      { 9, "Where it was held", cpu_page.held_words(S) },
      { 54, "The GPU's power", gpu_watts() },
      { 102, "The battery", battery_words() },
    }
  end

  page:measure()
  return page
end

return M
