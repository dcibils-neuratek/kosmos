-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_GLDirectMode
-- kosmos: name GPU Monitor
-- kosmos: section system
-- The graphics processor: how busy it is, how fast it runs and how long it
-- sleeps, its power, its memory and who holds it, and what the display and
-- the desktop take from it - now and over the last minute.
--
--   wm gpumon
--
-- Diego, 17 October 2026: "we need to vreate a gpu monitor app now to see
-- memory pressure, gpu usage, etc", with Windows' GPU page as the picture.
-- Drawn in `docs/gpumonitor.html` and agreed ("go ahead, build it as
-- drawn"); `docs/gpumonitor.md` is what it is made of, and this is G2.
--
-- **It measures nothing itself.** The GPU's driver reads its registers and
-- keeps its buffers (`GPU_OP_STATS`, `gpu.stats`), Nebula reads the GPU's
-- energy (`sys.info().gpu_energy_uj`), and this asks once a pace and takes
-- the difference between two readings, as the other monitors do. **No
-- engine is sent work yet**, so the busy graph says so rather than drawing
-- a 0% that looks like a measurement (`m700-2d.md` step 5 is the first).

local ui = use("/Kosmos/Libraries/ui.lua")
local gpu = use("/Kosmos/Libraries/gpu.lua")
local rendering = use("/Kosmos/Libraries/rendering.lua")
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

-- `--tell`: a line a sample, for a suite to read. Not without it.
local TELL = (tostring(args or "")):find("%-%-tell") ~= nil

local MAIN_W   = 640
local SIDE_W   = 240
local W        = MAIN_W + SIDE_W
local CARD_H   = 90
local CHIP     = 56
local GRAPH_H  = 96
local SMALL_H  = 72
local COLUMNS  = 60
local ROWS     = 6
local ROW_H    = 24
local GRAPH_GROUND = 0xff16181d         -- Monitor's: dark in every look
local GPU_COLOUR   = 0xffb98cff         -- the GPU's own, as the processor's are green and red
local MEM_COLOUR   = 0xff4fb3ff
local DISP_COLOUR  = 0xff5fd08a
local FAINT        = 0xff8b949e
local MEMORY_SCALE = 256 * 1048576      -- the memory graph's top: room for the screens and more

--------------------------------------------------------------------------
-- What the window shows, refreshed a sample at a time.
--------------------------------------------------------------------------

local now, rate = nil, nil              -- the latest snapshot, and rates from the one before
local last, last_info = nil, nil
local watts = nil                       -- the GPU's power, where it is known
local why_not = nil                     -- the driver's sentence when there is no GPU
local clock_h, mem_h, disp_h, busy_h = {}, {}, {}, {}
local names = {}
local mode = nil                        -- the rendering mode in use, the window manager's word

-- Who does a stage, as `docs/composing.png` colours it.
local STAGE_COLOUR = { cpu = 0xff4f7fd6, gpu = 0xff9d74e8, display = 0xff4fb878,
                       none = 0xff3a414f }

local function sample()
  local s, why = gpu.stats()
  local info = sys.info() or {}
  local r = fs.send("/Running/wm", { type = "rendering" })

  -- The rendering mode in use (`docs/rendering.md`; Diego: "also the gpu
  -- monitor needs to show the current rendering mode selected").
  mode = type(r) == "table" and r.ok and r.mode or mode

  if not s then
    why_not = why
    return
  end

  for _, p in ipairs(sys.processes() or {}) do names[p.id] = p.name end

  if last then
    rate = gpu.since(last, s, counter_hz)

    if rate then
      local bytes = 0

      for _, b in ipairs(s.buffers) do bytes = bytes + b.bytes end

      ui.history.push(busy_h, { math.floor(rate.blitter * 100 + 0.5) }, COLUMNS)
      ui.history.push(clock_h, { s.max_mhz > 0 and math.floor(100 * s.mhz / s.max_mhz) or 0 },
                      COLUMNS)
      ui.history.push(mem_h, { math.min(100, math.floor(100 * bytes / MEMORY_SCALE + 0.5)) },
                      COLUMNS)
      ui.history.push(disp_h, { rate.frames > 0 and math.min(100, math.floor(100 * rate.flips / rate.frames)) or 0 },
                      COLUMNS)

      if info.gpu_energy_uj and last_info and last_info.gpu_energy_uj then
        watts = ((info.gpu_energy_uj - last_info.gpu_energy_uj) & 0xffffffff) / 1e6 / rate.seconds
      end
    end
  end

  now, last, last_info = s, s, info

  if TELL and rate then
    print(("gpumon: %s, %d MHz, asleep %.0f%%, %.1f frames a second, %.1f flips a second, %d buffers")
          :format(s.model, s.mhz, rate.asleep * 100, rate.frames, rate.flips, #s.buffers))
  end
end

--------------------------------------------------------------------------
-- How numbers are said.
--------------------------------------------------------------------------

local function mb(bytes) return ("%.1f MB"):format(bytes / 1048576) end

local function clock_words(s)
  return s.mhz > 0 and ("%d MHz"):format(s.mhz) or "asleep"
end

local function holder(id)
  return id == 0 and "the firmware" or names[id] or ("process " .. id)
end

local function buffer_bytes(s)
  local bytes = 0

  for _, b in ipairs(s.buffers) do bytes = bytes + b.bytes end

  return bytes
end

--------------------------------------------------------------------------
-- Where everything goes.
--------------------------------------------------------------------------

local function measure()
  local m = { inner = MAIN_W - 2 * L.page_side }
  local y = L.page_top

  m.card = y
  y = y + CARD_H + L.between
  m.busy_heading = y
  y = y + L.group + 4
  m.engines = y
  y = y + 26
  m.busy = y
  y = y + SMALL_H + L.between
  m.clock_heading = y
  y = y + L.group + 4
  m.clock = y
  y = y + GRAPH_H + L.between
  m.mem_heading = y
  y = y + L.group + 4
  m.mem = y
  y = y + SMALL_H + L.between
  m.disp_heading = y
  y = y + L.group + 4
  m.disp = y
  y = y + SMALL_H + L.between
  m.holders_heading = y
  y = y + L.group + 4
  m.holders = y
  y = y + (ROWS + 1) * ROW_H
  m.foot = y + 10
  m.h = m.foot + gfx.height() + L.page_foot
  return m
end

local M = measure()

local win, err = ui.window{ title = "GPU Monitor", w = W, h = L.head + M.h,
                            x = 130, y = 90, header = true }

if not win then
  print("gpumon: " .. tostring(err))
  return
end

--------------------------------------------------------------------------
-- The header: the name and the dots.
--------------------------------------------------------------------------

local set_pace
local more = ui.iconbutton{ icon = "more" }

more.on_click = function()
  local items = {}

  for _, p in ipairs(PACES) do
    items[#items + 1] = { text = p.menu, mark = (p == pace),
                          on_choose = function() set_pace(p) end }
  end

  win:open_menu(win.origin_x + more.x, win.origin_y + L.head, items)
end

win:add(ui.header{ x = 0, y = 0, w = W, title = "GPU Monitor", right = { more }, title_bar = true })

--------------------------------------------------------------------------
-- The page.
--------------------------------------------------------------------------

local function heading(g, x, y, left, right, colour)
  g:text(x + 3, y + (L.group - gfx.height("heading")) // 2, left, theme.text, theme.window, "heading")

  if right then
    g:text(x + M.inner - gfx.measure(right), y + (L.group - gfx.height()) // 2, right,
           colour or theme.text_dim, theme.window, "ui")
  end
end

local function graph(g, x, y, h, list, colour, top)
  ui.history.draw(g, x, y, M.inner, h, list, { columns = COLUMNS, ground = GRAPH_GROUND,
                                               colours = { colour } })

  if top then
    g:text(x + 10, y + 6, top, FAINT, GRAPH_GROUND, "label")
  end
end

-- A sentence in the middle of an empty graph.
local function note(g, x, y, h, text)
  g:fill_round(x, y, M.inner, h, GRAPH_GROUND, 8)
  g:text(x + (M.inner - gfx.measure(text)) // 2, y + (h - gfx.height()) // 2, text,
         FAINT, GRAPH_GROUND, "ui")
end

local ENGINES = { "Render", "Blitter", "Video", "Video enhance" }

-- Each engine's chip: the blitter's share now (G3), the others a dash -
-- nothing sends them work yet.
local function engines(g, x, y)
  for _, name in ipairs(ENGINES) do
    local text = name == "Blitter" and rate and ("Blitter  %.1f%%"):format(rate.blitter * 100)
                 or (name .. "  -")
    local w = gfx.measure(text, "label") + 18

    g:frame_round(x, y, w, 22, theme.line_soft, 6)
    g:text(x + 9, y + (22 - gfx.height("label")) // 2, text, theme.text_dim, theme.window, "label")
    x = x + w + 8
  end
end

local HOLDER_COLS = { 0, 0.30, 0.52, 0.70 }

local function holders(g, x, y)
  local w = M.inner
  local function col(i) return x + 8 + math.floor((w - 16) * HOLDER_COLS[i]) end
  local hy = y + (ROW_H - gfx.height("label")) // 2

  for i, name in ipairs({ "Holder", "Size", "Memory", "For" }) do
    g:text(col(i), hy, name, theme.text_dim, theme.window, "label")
  end

  g:fill(x, y + ROW_H - 1, w, 1, theme.line_soft)

  for r, b in ipairs(now and now.buffers or {}) do
    if r > ROWS then break end

    local ty = y + r * ROW_H + (ROW_H - gfx.height()) // 2

    g:text(col(1), ty, ui.fitted(holder(b.holder), col(2) - col(1) - 8, "ui"), theme.text, theme.window, "ui")
    g:text(col(2), ty, ("%d x %d"):format(b.width, b.height), theme.text_dim, theme.window, "ui")
    g:text(col(3), ty, mb(b.bytes), theme.text_dim, theme.window, "ui")
    g:text(col(4), ty, b.kind, theme.text_dim, theme.window, "ui")
  end
end

-- The facts down the right, as Windows lists them.
local function side(g, x, y)
  local s = now
  local ground = theme.raised

  g:fill(x, 0, SIDE_W, M.h, ground)
  g:fill(x, 0, 1, M.h, theme.line_soft)
  x = x + 18
  y = y + 4

  local function fact(label, value, colour)
    g:text(x, y, label, theme.text_dim, ground, "label")
    y = y + gfx.height("label") + 2
    g:text(x, y, value, colour or theme.text, ground, "heading")
    y = y + gfx.height("heading") + 12
  end

  fact("Utilisation", rate and ("blitter %.1f%%"):format(rate.blitter * 100) or "-")
  fact("Clock", s and (clock_words(s) .. " / " .. s.max_mhz .. " MHz") or "-", GPU_COLOUR)
  fact("Asleep (RC6)", rate and ("%.0f%%"):format(rate.asleep * 100) or "-")
  fact("Power", watts and ("%.2f W"):format(watts) or "not known here")
  fact("Memory", s and mb(buffer_bytes(s)) .. " in " .. #s.buffers .. " buffers" or "-", MEM_COLOUR)
  fact("Display", s and s.width > 0 and ("%d x %d, %.0f Hz"):format(s.width, s.height, rate and rate.frames or 0) or "-",
       DISP_COLOUR)

  -- The rendering mode in use, and the six stages it gives whom.
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

  for _, kv in ipairs({ { "Vulkan", "not yet" }, { "OpenGL ES", "not yet" },
                        { "Device", s and ("8086:%04x"):format(s.device) or "-" } }) do
    g:text(x, y, kv[1], theme.text_dim, ground, "label")
    g:text(x + 84, y, kv[2], theme.text, ground, "label")
    y = y + gfx.height("label") + 6
  end
end

local body = ui.view{ x = 0, y = L.head, w = W, h = M.h }

function body:draw(g)
  local x = L.page_side
  local s = now

  if not s then
    ui.meter_card(g, x, M.card, M.inner, CARD_H, {
      picture = function(pg, px, py) ui.processor_chip(pg, px, py, CHIP, "", "GPU") end,
      size = CHIP, maker = "", model = "No GPU this driver knows",
      facts = why_not or "asking the GPU's driver",
    })
    note(g, x, M.busy, SMALL_H, "Under QEMU there is no Intel GPU to watch.")
    side(g, MAIN_W, M.card)
    return
  end

  ui.meter_card(g, x, M.card, M.inner, CARD_H, {
    picture = function(pg, px, py) ui.processor_chip(pg, px, py, CHIP, "intel", "GPU") end,
    size = CHIP, maker = "Intel", model = s.model,
    facts = ("8086:%04x \u{b7} %d to %d MHz"):format(s.device, s.min_mhz, s.max_mhz),
    rates = { { clock_words(s), "clock, now", GPU_COLOUR },
              { rate and ("%.0f%%"):format(rate.asleep * 100) or "-", "asleep" } },
  })

  heading(g, x, M.busy_heading, "Busy", pace.heading:lower())
  engines(g, x, M.engines)
  -- The blitter's busy share over the minute, by its own clock; render and
  -- video have no work yet, which their chips above say.
  graph(g, x, M.busy, SMALL_H, busy_h, GPU_COLOUR, "100%")

  heading(g, x, M.clock_heading, "Clock", rate and ("asleep %.0f%% of the time"):format(rate.asleep * 100),
          GPU_COLOUR)
  graph(g, x, M.clock, GRAPH_H, clock_h, GPU_COLOUR, s.max_mhz .. " MHz")

  heading(g, x, M.mem_heading, "Memory", ("%s in %d buffers"):format(mb(buffer_bytes(s)), #s.buffers),
          MEM_COLOUR)
  graph(g, x, M.mem, SMALL_H, mem_h, MEM_COLOUR, "256 MB")

  heading(g, x, M.disp_heading, "The display",
          rate and ("%.0f flips a second of %.0f frames%s"):format(rate.flips, rate.frames,
                                                                 s.underrun and ", an underrun seen" or ""),
          DISP_COLOUR)
  graph(g, x, M.disp, SMALL_H, disp_h, DISP_COLOUR, "every frame")

  heading(g, x, M.holders_heading, "Who holds its memory", ("%d buffers"):format(#s.buffers))
  holders(g, x, M.holders)

  g:text(x + 3, M.foot, pace.foot, theme.text_dim, theme.window, "ui")
  side(g, MAIN_W, M.card)
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

  -- Begun again: columns at one pace beside columns at another would be a
  -- time axis that lies.
  clock_h, mem_h, disp_h, busy_h, last = {}, {}, {}, {}, nil
  sample()
  print(("gpumon: %s"):format(p.menu:lower()))
end

sample()
print(now and ("gpumon: " .. now.model) or ("gpumon: no GPU: " .. tostring(why_not)))
win.tick_every = counter_hz * pace.half // 2
win:run()
