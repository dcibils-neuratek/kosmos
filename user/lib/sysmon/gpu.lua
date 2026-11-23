-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- System Monitor's GPU page: GPU Monitor's window, moved in whole, with
-- the power over the history window as a graph and the temperature beside
-- it (`docs/sysmonitor.md` S5; inventory rows 42-60; drawn in
-- `docs/sysmonitor.html`'s GPU page).
--
--   local page = use("/Kosmos/Libraries/sysmon/gpu.lua").new{
--     S = S, pace = function() return pace end, prefix = "sysmon: gpu" }
--   page:measure()            -- page.h
--   page:draw(g)              -- in the page's own coordinates
--   page:rows()               -- the inventory's rows 42-60, for `--tell`
--
-- **One page, two windows until S9.** GPU Monitor (`gpumon.lua`) draws
-- this same page in a window of its own, asking only the GPU, so there is
-- one body and not two that look alike (*Kits supply, applications
-- orchestrate*); S9 retires that window and `gpumon` opens this page.
--
-- **It measures nothing itself.** The GPU's driver reads its registers and
-- keeps its buffers (`GPU_OP_STATS`), Nebula reads the graphics plane's
-- energy (`sys.info().gpu_energy_uj`), `meters.gpu` takes the differences
-- and `sysmon/sample.lua` keeps the minutes. **Busy is each engine by its
-- own clock**: the blitter's copies and the render engine's runs.
--
-- **Added: the power as a graph** - RAPL's graphics plane over the history
-- window, not only the number now - and **the temperature**. An Intel GPU
-- on the die has no sensor of its own, so the package's stands in and the
-- page says it is the package's. Where the machine measures neither -
-- QEMU's processor, the T14's Tiger Lake for the plane - the page says
-- "not measured on this machine" in its place, never a nought.
--
-- **No GPU** (row 59): the card says "No GPU this driver knows" with the
-- driver's own sentence, and the page "Under QEMU there is no Intel GPU to
-- watch.", as GPU Monitor did. The page is there for `sysmon gpu` and for
-- the suites; the sidebar and the Overview have no row and no tile for a
-- part the machine does not have.

local ui = use("/Kosmos/Libraries/ui.lua")
local rendering = use("/Kosmos/Libraries/rendering.lua")
local cpu_page = use("/Kosmos/Libraries/sysmon/cpu.lua")
local theme = ui.theme
local L = ui.layout

local M = {}

local MAIN_W   = 640
local SIDE_W   = 240
M.WIDTH        = MAIN_W + SIDE_W
M.MAIN_W       = MAIN_W

local CARD_H   = 90
local CHIP     = 56
local BUSY_H   = 72
local CELL_H   = 84                     -- the four graphs two by two, as the drawing has them
local DISP_H   = 56
local ROWS     = 6
local ROW_H    = 24
local GAP      = 12

local GROUND      = cpu_page.GRAPH_GROUND
local GPU_COLOUR  = 0xffb98cff          -- the GPU's own, as the processor's are green and red
local MEM_COLOUR  = 0xff4fb3ff
local DISP_COLOUR = 0xff5fd08a
local WATT_COLOUR = 0xffffc04d          -- the drawing's `--watt`
local HEAT_COLOUR = 0xffff8a5c          -- the Overview's heat
local FAINT       = 0xff8b949e
local UNMEASURED  = cpu_page.UNMEASURED

-- Who does a stage, as `docs/composing.png` colours it.
local STAGE_COLOUR = { cpu = 0xff4f7fd6, gpu = 0xff9d74e8, display = 0xff4fb878,
                       none = 0xff3a414f }

local ENGINES = { "Render", "Blitter", "Video", "Video enhance" }
local HOLDER_COLS = { 0, 0.30, 0.52, 0.70 }

local NO_GPU = "No GPU this driver knows"
local NO_GPU_HERE = "Under QEMU there is no Intel GPU to watch."

local function mb(bytes) return ("%.1f MB"):format(bytes / 1048576) end

local function clock_words(s)
  return s.mhz > 0 and ("%d MHz"):format(s.mhz) or "asleep"
end

local function buffer_bytes(s)
  local bytes = 0

  for _, b in ipairs(s.buffers) do bytes = bytes + b.bytes end

  return bytes
end

-- A watts graph's top: the next five watts above the most it holds.
local function watts_top(list)
  local most = ui.history.most(list)

  return math.max(5, math.ceil(most / 5) * 5)
end

function M.new(spec)
  local S = spec.S
  local pace = spec.pace
  local prefix = spec.prefix or "sysmon: gpu"

  local page = { name = "gpu", title = "GPU", w = M.WIDTH }

  -- The GPU's snapshot, its rates and its power, as the sample last heard.
  local function now() return S.gpu_lib and S.gpu.now or nil end
  local function rate() return S.gpu_lib and S.gpu.rate or nil end
  local function vendor(device) return S.gpu_lib.vendor(device) end

  local function watts_words()
    local w = S.gpu_lib and S.gpu.watts

    return w and ("%.2f W"):format(w) or UNMEASURED
  end

  -- The GPU's temperature: the package's, said to be (`sysmonitor.md`,
  -- *Sensors*: an Intel GPU on the die has no sensor of its own).
  local function temperature_words()
    local heat = S.hot.heat

    return heat and heat.temp_c and ("%d °C, the package's"):format(heat.temp_c) or UNMEASURED
  end

  --------------------------------------------------------------------------
  -- Where everything goes.
  --------------------------------------------------------------------------

  function page:measure()
    local m = { inner = MAIN_W - 2 * L.page_side }
    local y = L.page_top

    m.card = y
    y = y + CARD_H + L.between
    m.busy_heading = y
    y = y + L.group + 4
    m.engines = y
    y = y + 26
    m.busy = y
    y = y + BUSY_H + L.between
    m.cell_w = (m.inner - GAP) // 2
    m.grid = y                          -- two rows of a heading and a graph
    y = y + 2 * (L.group + 4 + CELL_H) + GAP + L.between
    m.disp_heading = y
    y = y + L.group + 4
    m.disp = y
    y = y + DISP_H + L.between
    m.holders_heading = y
    y = y + L.group + 4
    m.holders = y
    y = y + (ROWS + 1) * ROW_H
    m.foot = y + 10
    self.h = m.foot + gfx.height() + L.page_foot
    self.m = m
    return self.h
  end

  --------------------------------------------------------------------------
  -- The parts.
  --------------------------------------------------------------------------

  local function heading(g, x, y, w, left, right, colour)
    ui.group_heading(g, x, y, w, L.group, left, right, colour)
  end

  local function graph(g, x, y, w, h, list, colour, top, spec_)
    spec_ = spec_ or {}
    spec_.columns, spec_.ground, spec_.colours = S.columns, GROUND, { colour }
    ui.history.draw(g, x, y, w, h, list, spec_)

    if top then g:text(x + 10, y + 6, top, FAINT, GROUND, "label") end
  end

  -- A sentence in the middle of an empty graph.
  local function note(g, x, y, w, h, text)
    g:fill_round(x, y, w, h, GROUND, 8)
    g:text(x + (w - gfx.measure(text)) // 2, y + (h - gfx.height()) // 2, text, FAINT, GROUND, "ui")
  end

  -- Each engine's chip: the render engine's share now and the blitter's;
  -- the video engines a dash, nothing sends them work yet.
  local function engine_words(name)
    local r = rate()

    if r and name == "Blitter" then return ("Blitter  %.1f%%"):format(r.blitter * 100) end
    if r and name == "Render" then return ("Render  %.1f%%"):format((r.render or 0) * 100) end

    return name .. "  -"
  end

  local function engines(g, x, y)
    for _, name in ipairs(ENGINES) do
      local text = engine_words(name)
      local w = gfx.measure(text, "label") + 18

      g:frame_round(x, y, w, 22, theme.line_soft, 6)
      g:text(x + 9, y + (22 - gfx.height("label")) // 2, text, theme.text_dim, theme.window, "label")
      x = x + w + 8
    end
  end

  local function holders(g, x, y, w)
    local s = now()
    local function col(i) return x + 8 + math.floor((w - 16) * HOLDER_COLS[i]) end
    local hy = y + (ROW_H - gfx.height("label")) // 2

    for i, name in ipairs({ "Holder", "Size", "Memory", "For" }) do
      g:text(col(i), hy, name, theme.text_dim, theme.window, "label")
    end

    g:fill(x, y + ROW_H - 1, w, 1, theme.line_soft)

    for r, b in ipairs(s and s.buffers or {}) do
      if r > ROWS then break end

      local ty = y + r * ROW_H + (ROW_H - gfx.height()) // 2

      g:text(col(1), ty, ui.fitted(S:holder(b.holder), col(2) - col(1) - 8, "ui"), theme.text, theme.window, "ui")
      g:text(col(2), ty, ("%d x %d"):format(b.width, b.height), theme.text_dim, theme.window, "ui")
      g:text(col(3), ty, mb(b.bytes), theme.text_dim, theme.window, "ui")
      g:text(col(4), ty, b.kind, theme.text_dim, theme.window, "ui")
    end
  end

  -- The facts down the right, as Windows lists them.
  local function side(g, x, h)
    local s, r = now(), rate()
    local ground = theme.raised
    local y = L.page_top + 4

    g:fill(x, 0, SIDE_W, h, ground)
    g:fill(x, 0, 1, h, theme.line_soft)
    x = x + 18

    local function fact(label, value, colour, why)
      g:text(x, y, label, theme.text_dim, ground, "label")
      y = y + gfx.height("label") + 2
      g:text(x, y, ui.fitted(value, SIDE_W - 36, "heading"), colour or theme.text, ground, "heading")
      y = y + gfx.height("heading") + (why and 2 or 12)

      if why then
        g:text(x, y, ui.fitted(why, SIDE_W - 36, "label"), theme.text_dim, ground, "label")
        y = y + gfx.height("label") + 10
      end
    end

    fact("Utilisation", r and ("render %.1f%% \u{b7} blitter %.1f%%"):format((r.render or 0) * 100,
                                                                         r.blitter * 100) or "-")
    fact("Clock", s and (clock_words(s) .. " / " .. s.max_mhz .. " MHz") or "-", GPU_COLOUR)
    fact("Asleep (RC6)", r and ("%.0f%%"):format(r.asleep * 100) or "-")
    fact("Power", watts_words(), S.gpu_lib and S.gpu.watts and WATT_COLOUR or nil,
         S.gpu_lib and S.gpu.watts and "RAPL's graphics plane, PP1" or nil)
    fact("Temperature", temperature_words(), nil,
         S.hot.heat and S.hot.heat.temp_c and "an Intel GPU on the die has no sensor of its own" or nil)
    fact("Memory", s and mb(buffer_bytes(s)) .. " in " .. #s.buffers .. " buffers" or "-", MEM_COLOUR)
    fact("Display", s and s.width > 0 and ("%d x %d, %.0f Hz"):format(s.width, s.height, r and r.frames or 0)
         or "-", DISP_COLOUR)

    -- The rendering mode in use, and the six stages it gives whom.
    local mode = S.render_mode

    g:text(x, y, "Rendering mode", theme.text_dim, ground, "label")
    y = y + gfx.height("label") + 2
    g:text(x, y, mode and rendering.name(mode) or "-", theme.text, ground, "heading")
    y = y + gfx.height("heading") + 6

    if mode and rendering.MODES[mode] then
      local sw = (SIDE_W - 36 - 5 * 2) // 6

      for i, who in ipairs(rendering.MODES[mode].stages) do
        local sx = x + (i - 1) * (sw + 2)

        if who == "gpu+cpu" then
          g:fill_round(sx, y, sw, 14, STAGE_COLOUR.cpu, 3)
          g:fill_round(sx, y, sw * 55 // 100, 14, STAGE_COLOUR.gpu, 3)
        else
          g:fill_round(sx, y, sw, 14, STAGE_COLOUR[who] or STAGE_COLOUR.none, 3)
        end
      end

      y = y + 20
      g:text(x, y, ui.fitted("Needs " .. rendering.MODES[mode].needs, SIDE_W - 36, "label"),
             theme.text_dim, ground, "label")
      y = y + gfx.height("label")
    end

    y = y + 14

    -- Vulkan is Mesa's `anv` in an application's own image, on every GPU
    -- this driver serves (`docs/gpu3d.md` G7). GL ES is G8's.
    for _, kv in ipairs({ { "Vulkan", s and "1.4, Mesa anv" or "-" }, { "OpenGL ES", "not yet" },
                          { "Device", s and ("%04x:%04x"):format(vendor(s.device), s.device) or "-" } }) do
      g:text(x, y, kv[1], theme.text_dim, ground, "label")
      g:text(x + 84, y, kv[2], theme.text, ground, "label")
      y = y + gfx.height("label") + 6
    end
  end

  -- The four graphs two by two: the clock, the power, the temperature and
  -- the memory, each with its heading.
  local function grid(g, x, m, s, r)
    local cw = m.cell_w
    local heat = S.hot.heat
    local cells = {
      { "Clock", r and ("%s \u{b7} asleep %.0f%%"):format(clock_words(s), r.asleep * 100), GPU_COLOUR,
        function(cx, cy) graph(g, cx, cy, cw, CELL_H, S.gpu_clock_h, GPU_COLOUR, s.max_mhz .. " MHz") end },
      { "Power", S.gpu.watts and ("%.1f W now"):format(S.gpu.watts) or nil, WATT_COLOUR,
        function(cx, cy)
          if S.gpu.watts then
            local top = watts_top(S.gpu_watts_h)

            graph(g, cx, cy, cw, CELL_H, S.gpu_watts_h, WATT_COLOUR, top .. " W", { mode = "line", most = top })
          else
            note(g, cx, cy, cw, CELL_H, UNMEASURED)
          end
        end },
      { "Temperature", heat and heat.temp_c and ("%d °C, the package's"):format(heat.temp_c) or nil, HEAT_COLOUR,
        function(cx, cy)
          if heat and heat.temp_c then
            local top = heat.limit_c or 100

            graph(g, cx, cy, cw, CELL_H, S.heat_h, HEAT_COLOUR, top .. " °C", { mode = "line", most = top })
          else
            note(g, cx, cy, cw, CELL_H, UNMEASURED)
          end
        end },
      { "Memory", ("%s in %d buffers"):format(mb(buffer_bytes(s)), #s.buffers), MEM_COLOUR,
        function(cx, cy) graph(g, cx, cy, cw, CELL_H, S.gpu_mem_h, MEM_COLOUR, "256 MB") end },
    }

    for i, c in ipairs(cells) do
      local cx = x + ((i - 1) % 2) * (cw + GAP)
      local cy = m.grid + ((i - 1) // 2) * (L.group + 4 + CELL_H + GAP)

      heading(g, cx, cy, cw, c[1], c[2] and ui.fitted(c[2], cw - 80, "ui") or nil, c[3])
      c[4](cx, cy + L.group + 4)
    end
  end

  function page:draw(g)
    local m = self.m or self:measure()
    local x = L.page_side
    local s, r = now(), rate()

    if not s then
      ui.meter_card(g, x, m.card, m.inner, CARD_H, {
        picture = function(pg, px, py) ui.processor_chip(pg, px, py, CHIP, "", "GPU") end,
        size = CHIP, maker = "", model = NO_GPU,
        facts = S.gpu_why or "asking the GPU's driver",
      })
      note(g, x, m.busy, m.inner, BUSY_H, NO_GPU_HERE)
      side(g, MAIN_W, self.h)
      return
    end

    local rates = { { ("%d%%"):format(math.floor((S.gpu.busy or 0) * 100 + 0.5)), "busy, now", GPU_COLOUR },
                    { clock_words(s), "clock, now" },
                    { r and ("%.0f%%"):format(r.asleep * 100) or "-", "asleep" } }

    if S.gpu.watts then rates[#rates + 1] = { ("%.1f W"):format(S.gpu.watts), "now", WATT_COLOUR } end

    ui.meter_card(g, x, m.card, m.inner, CARD_H, {
      picture = function(pg, px, py) ui.processor_chip(pg, px, py, CHIP, "intel", "GPU") end,
      size = CHIP, maker = "Intel", model = s.model,
      facts = ("%04x:%04x \u{b7} %d to %d MHz"):format(vendor(s.device), s.device, s.min_mhz, s.max_mhz),
      rates = rates,
    })

    heading(g, x, m.busy_heading, m.inner, "Busy", "each engine by its own clock")
    engines(g, x, m.engines)
    graph(g, x, m.busy, m.inner, BUSY_H, S.gpu_h, GPU_COLOUR, "100%")

    grid(g, x, m, s, r)

    heading(g, x, m.disp_heading, m.inner, "The display",
            r and ("%.0f flips a second of %.0f frames%s"):format(r.flips, r.frames,
                                                                 s.underrun and ", an underrun seen" or ""),
            DISP_COLOUR)
    graph(g, x, m.disp, m.inner, DISP_H, S.gpu_disp_h, DISP_COLOUR, "every frame")

    heading(g, x, m.holders_heading, m.inner, "Who holds its memory", ("%d buffers"):format(#s.buffers))
    holders(g, x, m.holders, m.inner)

    g:text(x + 3, m.foot, pace().foot, theme.text_dim, theme.window, "ui")
    side(g, MAIN_W, self.h)
  end

  --------------------------------------------------------------------------
  -- What it says.
  --------------------------------------------------------------------------

  -- The page's first line, when it is shown: the GPU, or why there is none.
  function page:heading()
    local s = now()

    return s and s.model or ("no GPU: " .. tostring(S.gpu_why))
  end

  -- A line a sample, for a suite (row 60): GPU Monitor's, as it was.
  function page:tell_line()
    local s, r = now(), rate()

    if not (s and r) then return nil end

    return ("%s, %d MHz, asleep %.0f%%, %.1f frames a second, %.1f flips a second, %d buffers")
           :format(s.model, s.mhz, r.asleep * 100, r.frames, r.flips, #s.buffers)
  end

  --
  -- **What this page shows, a row of the inventory each** (rows 42-60), for
  -- `--tell`: under QEMU each says there is no GPU in its own place, so a
  -- row taken off the page is still missed.
  --
  function page:rows()
    local s, r = now(), rate()
    local why = tostring(S.gpu_why or "asking the GPU's driver")
    local none = "no GPU: " .. why
    local holders_ = {}

    for i, b in ipairs(s and s.buffers or {}) do
      if i > ROWS then break end
      holders_[#holders_ + 1] = ("%s %dx%d %s %s"):format(S:holder(b.holder), b.width, b.height, mb(b.bytes), b.kind)
    end

    local function pct(v) return v and ("%.1f%%"):format(v * 100) or "-" end

    return {
      { 42, "The card", s and ("Intel | %s | %04x:%04x | %d to %d MHz"):format(s.model, vendor(s.device), s.device,
                                                                           s.min_mhz, s.max_mhz)
                          or (NO_GPU .. " | " .. why) },
      { 43, "The card's rates", s and ("clock %s, asleep %s"):format(clock_words(s), r and pct(r.asleep) or "-") or none },
      { 44, "Each engine", s and table.concat({ engine_words("Render"), engine_words("Blitter"),
                                                engine_words("Video"), engine_words("Video enhance") }, " | ")
                           or none },
      { 45, "Busy over the minute", s and ("%d columns, now %d%%"):format(#S.gpu_h,
                                       math.floor((S.gpu.busy or 0) * 100 + 0.5)) or none },
      { 46, "The clock over the minute", s and ("%d columns, of %d MHz"):format(#S.gpu_clock_h, s.max_mhz) or none },
      { 47, "Asleep (RC6), in the clock's heading", s and (r and pct(r.asleep) or "-") or none },
      { 48, "Memory over the minute", s and ("%d columns, against 256 MB"):format(#S.gpu_mem_h) or none },
      { 49, "The display", s and (r and ("%.0f flips of %.0f frames%s"):format(r.flips, r.frames,
                                         s.underrun and ", an underrun seen" or "") or "-") or none },
      { 50, "Who holds its memory", s and (#holders_ > 0 and table.concat(holders_, ", ") or "no buffers") or none },
      { 51, "Side: Utilisation", s and ("render %s, blitter %s"):format(r and pct(r.render or 0) or "-",
                                                                        r and pct(r.blitter) or "-") or none },
      { 52, "Side: Clock", s and (clock_words(s) .. " / " .. s.max_mhz .. " MHz") or none },
      { 53, "Side: Asleep (RC6)", s and (r and pct(r.asleep) or "-") or none },
      { 54, "Side: Power", watts_words() .. (S.gpu_lib and S.gpu.watts
                                               and (", %d columns"):format(#S.gpu_watts_h) or "") },
      { 55, "Side: Memory", s and (mb(buffer_bytes(s)) .. " in " .. #s.buffers .. " buffers") or none },
      { 56, "Side: Display", s and (s.width > 0 and ("%d x %d, %.0f Hz"):format(s.width, s.height,
                                                                              r and r.frames or 0) or "-") or none },
      { 57, "Side: the rendering mode", S.render_mode and rendering.name(S.render_mode) or "-" },
      { 58, "Side: Vulkan, OpenGL ES, the device", s and ("1.4, Mesa anv | not yet | %04x:%04x"):format(
                                                       vendor(s.device), s.device) or "- | not yet | -" },
      { 59, "No GPU", s and "a GPU is here" or (NO_GPU .. "; " .. why .. "; " .. NO_GPU_HERE) },
      { 60, "--tell", "a line a sample while the page shows: " .. prefix .. " ..." },
    }
  end

  -- The GPU's temperature as the page says it, for `--tell` beside the rows.
  function page:temperature()
    return temperature_words()
  end

  page:measure()
  return page
end

return M
